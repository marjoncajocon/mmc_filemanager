/* fnetstream.c -- an HTTP(S) resource read like a file (see fnetstream.h).
**
** Design decisions:
**   - One worker per open stream runs net_get_stream into a 256 KB ring; the
**     reader blocks on a condition until data, the end or an error arrives.
**     The worker blocks while the ring is full, so memory stays at the ring.
**   - Seeking is lazy: ns_seek only records the position, and the next read
**     restarts the request with "Range: bytes=N-". Decoders probe (dr_mp3
**     seeks to the end for ID3v1/APE tags and straight back), so eager
**     seeks cost a round trip each. A hop forward inside the ring is a skip.
**     A server that ignores the range (200 from byte 0) is handled by
**     dropping bytes up to the target.
**   - Files are fetched in bounded chunks ("Range: bytes=a-b", 8 MB) chained
**     into the same ring: googlevideo (YouTube) throttles open-ended range
**     requests to about playback speed (measured: 256 KB took 3 s), which is
**     also why yt-dlp downloads YouTube in chunks. The total size comes from
**     Content-Range. Live streams ignore ranges (200, no length): one request.
**   - ICY metadata (radio): with icy-metaint=K the server inserts, after
**     every K audio bytes, one length byte L and L*16 bytes of text such as
**     "StreamTitle='Artist - Title';". It is cut out here, so decoders only
**     ever see audio, and the title is kept for the player.
**   - HLS (an .m3u8 URL, an mpegurl content type, or a body that starts with
**     #EXTM3U): the media playlist's segments are fetched one after the
**     other into the same ring, so readers see ONE continuous file. That is
**     valid for MPEG-TS (concatenated TS is TS) and for fMP4 (the #EXT-X-MAP
**     init section, then the fragments, is a fragmented MP4); the content
**     type is sniffed from the first bytes (video/mp2t, video/mp4,
**     audio/aac) so Media Foundation picks the right source.
**     * A master playlist picks a variant (hls_pick_variant, up to 1080p);
**       resolvers that want a given size pass the variant's URL instead.
**     * Seeking (VOD): byte offsets are exact. Each segment's start offset is
**       recorded when it is reached, so a seek back restarts at the segment
**       holding the target and drops the bytes before it; a seek past what
**       was reached restarts at the furthest known segment and drops bytes
**       up to the target (downloads the gap; the BANDWIDTH x time estimate
**       was rejected: a guessed offset hands decoders the wrong bytes). The
**       size is unknown (-1) until the last segment is in. For time seeks,
**       open "<url>#t=SECONDS": the stream then starts at the segment holding
**       that time (ns_hls_start says when byte 0 plays), so a player can
**       reopen at a time instead of decoding its way there.
**     * Live (no #EXT-X-ENDLIST): starts three segments from the live edge,
**       reloads the playlist every half target duration and appends the
**       new segments by media sequence number; only forward seeks.
**     * AES-128 segments are decrypted as they stream (fhls.c, fcrypt AES);
**       SAMPLE-AES (DRM) and a changing fMP4 init section fail at open with
**       a clear message.
**     * A failed segment is fetched again (3 tries); the bytes the first try
**       already delivered are dropped, so the stream stays continuous.
**     * The next segment's request starts while the current one streams (a
**       second thread; its data callback waits for its turn, so what
**       arrives early sits in the socket buffer, not in our memory). That
**       hides connect + TLS + first byte, measured 0.6-1.0 s per request to
**       Dailymotion's CDN from here, a third of a 3-second segment.
*/
#include "fnetstream.h"
#include "fnet.h"
#include "fhls.h"
#include "fsdl.h"

#define RING (256u * 1024u)          /* the ring to start with (and for live radio) */
/* Files and VOD playlists grow it while the network is ahead of the reader:
** 256 KB is ~1 s of 720p, so every hiccup on a phone showed "Buffering"
** (measured: every 3-5 s on a 15 Mbit/s link). 4 MB is ~15 s of 720p. Live
** streams keep the small ring: their data is not worth hoarding. */
#define RING_MAX (4u * 1024u * 1024u)
#define CHUNK (8ll * 1024 * 1024)

/* HLS state (see the top comment); only the worker touches the cursor while
** it runs, the open/seek side only while it is stopped. */
typedef struct Hls {
  char *media;                 /* the media playlist URL (after a master pick) */
  FmHlsMedia pl;               /* VOD: the whole list; live: the latest reload */
  bool live;
  double t0;                   /* the time byte 0 plays at (#t=) */
  int first;                   /* VOD: the segment that follows the init section at byte 0 */
  i64 *off;                    /* VOD: stream offset where segment k starts, -1 = not reached */
  i64 init_len;                /* bytes of the fMP4 init section, -1 = not fetched yet, 0 = none */
  /* the worker's cursor */
  int item;                    /* VOD: next segment, -1 = the init section */
  i64 next_seq;                /* live: next media sequence number, -1 = near the live edge */
  i64 wpos;                    /* stream offset of the next byte the worker produces */
  i64 seg_pos;                 /* stream offset where the running segment started */
  i64 got;                     /* bytes of the running segment produced so far */
  struct HlsJob *turn;         /* the request whose bytes go into the ring now */
  bool sniffed;
  char *key_uri;               /* the AES-128 key in `key` (fetched once per URI) */
  u8 key[16];
} Hls;

struct FmNetStream {
  char *url;
  char headers[512];           /* caller's extra headers */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  volatile int cancel;
  /* ring, guarded by mx */
  u8 *ring;
  size_t cap;                  /* its size: RING, growing to cap_max (see grow_ring) */
  size_t cap_max;              /* RING_MAX unless ns_limit_ring said less */
  size_t rd, wr, fill;         /* read and write index, bytes buffered */
  bool eof, failed, head_in;
  volatile int aborted;        /* ns_abort: reads return 0 at once, nothing restarts */
  int status;
  char error[160];
  char ctype[96];
  i64 size;                    /* whole resource, -1 unknown */
  i64 pos;                     /* byte offset of the next byte ns_read returns */
  i64 req_start;               /* where the running request started */
  i64 pending;                 /* lazy seek target, -1 = none */
  i64 discard;                 /* bytes to drop: the server ignored our Range */
  int resumes;                 /* reconnects since the last good read */
  bool ranges;
  /* ICY */
  int metaint;
  int meta_left;               /* audio bytes until the next metadata block */
  int meta_len;                /* metadata bytes still to read (block in progress) */
  int meta_got;
  char meta[4096];
  char station[96];
  char title[256];
  Hls *hls;                    /* an HLS playlist, NULL = a plain resource */
};

/* ---- worker -------------------------------------------------------------------- */

static void on_head(void *u, const FmNetResp *r) {
  FmNetStream *s = (FmNetStream *)u;
  SDL_LockMutex(s->mx);
  s->status = r->status;
  fm_strlcpy(s->ctype, r->type, sizeof s->ctype);
  if (r->status == 206 && r->total > 0) {
    s->size = r->total;
    s->ranges = true;
  } else if (r->status == 206 || (r->status == 200 && s->req_start == 0)) {
    if (r->length >= 0 && r->status == 200) s->size = r->length;
    s->ranges = r->ranges || r->status == 206;
  } else if (r->status == 200 && s->req_start > 0) {
    s->discard = s->req_start;              /* whole body again: skip to where we wanted */
    if (r->length >= 0) s->size = r->length;
  }
  s->metaint = r->icy_metaint > 0 ? r->icy_metaint : 0;
  s->meta_left = s->metaint;
  if (r->icy_name[0]) fm_strlcpy(s->station, r->icy_name, sizeof s->station);
  s->head_in = true;
  SDL_CondBroadcast(s->cv);
  SDL_UnlockMutex(s->mx);
}

/* "StreamTitle='Artist - Title';StreamUrl='';" -> the title */
static void parse_meta(FmNetStream *s) {
  s->meta[FM_MIN(s->meta_got, (int)sizeof s->meta - 1)] = 0;
  const char *p = strstr(s->meta, "StreamTitle='");
  if (!p) return;
  p += 13;
  const char *e = strstr(p, "';");
  if (!e) e = p + strlen(p);
  size_t n = (size_t)(e - p);
  if (n >= sizeof s->title) n = sizeof s->title - 1;
  memcpy(s->title, p, n);
  s->title[n] = 0;
}

/* A full ring doubles (unwrapped into the new block) while the stream is a
** file or a VOD playlist; false = keep waiting for the reader. mx held. */
static bool grow_ring(FmNetStream *s) {
  bool live = s->hls ? s->hls->live : s->size < 0 && !s->ranges;
  if (live || s->cap >= s->cap_max) return false;
  size_t nc = s->cap * 2;
  u8 *nr = (u8 *)fm_alloc(nc);
  if (!nr) return false;
  size_t first = FM_MIN(s->fill, s->cap - s->rd);
  memcpy(nr, s->ring + s->rd, first);
  memcpy(nr + first, s->ring, s->fill - first);
  fm_free(s->ring);
  s->ring = nr;
  s->cap = nc;
  s->rd = 0;
  s->wr = s->fill;
  return true;
}

/* Appends audio bytes to the ring, waiting while it is full. */
static bool put_audio(FmNetStream *s, const u8 *p, size_t n) {
  while (n > 0) {
    while (s->fill == s->cap && !s->cancel && !grow_ring(s)) SDL_CondWaitTimeout(s->cv, s->mx, 500);
    if (s->cancel) return false;
    size_t room = s->cap - s->fill;
    size_t run = FM_MIN(n, FM_MIN(room, s->cap - s->wr));
    memcpy(s->ring + s->wr, p, run);
    s->wr = (s->wr + run) % s->cap;
    s->fill += run;
    p += run;
    n -= run;
    SDL_CondBroadcast(s->cv);
  }
  return true;
}

static bool on_data(void *u, const u8 *p, size_t n) {
  FmNetStream *s = (FmNetStream *)u;
  SDL_LockMutex(s->mx);
  bool ok = true;
  if (s->status < 200 || s->status >= 300) ok = false;      /* an error page is not audio */
  if (ok && s->discard > 0) {
    size_t d = (size_t)FM_MIN((i64)n, s->discard);
    s->discard -= (i64)d;
    p += d;
    n -= d;
  }
  while (ok && n > 0) {
    if (!s->metaint) { ok = put_audio(s, p, n); break; }
    if (s->meta_len > 0) {                                  /* inside a metadata block */
      size_t take = FM_MIN(n, (size_t)s->meta_len);
      size_t room = sizeof s->meta - 1 - (size_t)s->meta_got;
      memcpy(s->meta + s->meta_got, p, FM_MIN(take, room));
      s->meta_got += (int)FM_MIN(take, room);
      s->meta_len -= (int)take;
      p += take;
      n -= take;
      if (!s->meta_len) { parse_meta(s); s->meta_left = s->metaint; }
    } else if (s->meta_left == 0) {                         /* the length byte */
      s->meta_len = p[0] * 16;
      s->meta_got = 0;
      p++;
      n--;
      if (!s->meta_len) s->meta_left = s->metaint;
    } else {
      size_t take = FM_MIN(n, (size_t)s->meta_left);
      ok = put_audio(s, p, take);
      s->meta_left -= (int)take;
      p += take;
      n -= take;
    }
  }
  SDL_UnlockMutex(s->mx);
  return ok;
}

static int worker(void *u) {
  FmNetStream *s = (FmNetStream *)u;
  char hdr[700];
  SDL_LockMutex(s->mx);
  i64 start = s->req_start;
  SDL_UnlockMutex(s->mx);
  FmNetResp r;
  FmErr e;
  for (;;) {
    i64 last = start + CHUNK - 1;
    fm_snprintf(hdr, sizeof hdr, "%sIcy-MetaData: 1\r\nRange: bytes=%lld-%lld\r\n", s->headers, (long long)start,
                (long long)last);
    memset(&r, 0, sizeof r);
    e = net_get_stream(s->url, hdr, on_head, on_data, s, &r, &s->cancel);
    SDL_LockMutex(s->mx);
    /* a full chunk of a bigger file: chain the next one into the same ring */
    bool more = e == FM_OK && !s->cancel && r.status == 206 && s->size > 0 && last + 1 < s->size;
    if (more) {
      start = last + 1;
      s->req_start = start;
    }
    SDL_UnlockMutex(s->mx);
    if (!more) break;
  }
  SDL_LockMutex(s->mx);
  if (!s->head_in) {                      /* never got headers: tell the opener */
    s->head_in = true;
    s->status = r.status;
  }
  if (e != FM_OK && e != FM_ERR_CANCEL) {
    s->failed = true;
    fm_strlcpy(s->error, r.error[0] ? r.error : fm_err_str(e), sizeof s->error);
  } else if (e == FM_ERR_CANCEL && (s->status < 200 || s->status >= 300) && !s->cancel) {
    s->failed = true;                     /* stopped by on_data: an HTTP error */
  }
  s->eof = true;
  SDL_CondBroadcast(s->cv);
  SDL_UnlockMutex(s->mx);
  return 0;
}

/* ---- HLS worker ---------------------------------------------------------------------- */

/* One segment request: the running one, or the next one already connecting
** (its bytes wait for their turn in the data callback; the socket buffer
** holds what arrives meanwhile, not our memory). */
typedef struct HlsJob {
  FmNetStream *s;
  bool init;                   /* the fMP4 init section, not a segment */
  int item;                    /* VOD: the segment index */
  i64 seq;                     /* its media sequence number */
  char *url;
  char hdr[700];
  bool enc;                    /* AES-128: key and IV, the decryptor restarts from them on a retry */
  u8 key[16], iv[16];
  FmHlsDec dec;
  int status;                  /* HTTP status of the latest try, 0 = none */
  i64 seen;                    /* bytes the latest try delivered (a retry drops the first h->got) */
  FmErr e;
  char error[160];
  bool tried;
  SDL_Thread *thr;             /* the try running ahead, NULL = none */
  bool running;                /* thr has not finished its try */
  volatile int stop;           /* ends the try running ahead */
} HlsJob;

/* The first bytes of the stream: the container, for decoders that pick a
** parser from the content type (Media Foundation). */
static void hls_sniff_type(FmNetStream *s, const u8 *p, size_t n) {
  Hls *h = s->hls;
  const char *t = "video/mp2t";
  if (h->pl.map_uri || (n >= 8 && (!memcmp(p + 4, "ftyp", 4) || !memcmp(p + 4, "styp", 4) ||
                                   !memcmp(p + 4, "moof", 4))))
    t = "video/mp4";
  else if (n >= 1 && p[0] == 0x47)
    t = "video/mp2t";
  else if (n >= 3 && !memcmp(p, "ID3", 3))             /* packed audio, timestamped with an ID3 tag */
    t = h->pl.n && fm_stristr(h->pl.seg[0].uri, ".mp3") ? "audio/mpeg" : "audio/aac";
  else if (n >= 2 && p[0] == 0xFF && (p[1] & 0xF0) == 0xF0)
    t = (p[1] & 6) ? "audio/mpeg" : "audio/aac";       /* layer bits: 0 = ADTS */
  fm_strlcpy(s->ctype, t, sizeof s->ctype);
  h->sniffed = true;
}

/* Plain segment bytes of the job holding the turn into the stream (mx held). */
static bool hls_put(void *u, const u8 *p, size_t n) {
  HlsJob *j = (HlsJob *)u;
  FmNetStream *s = j->s;
  Hls *h = s->hls;
  if (j->seen < h->got) {                              /* a retry: already delivered */
    size_t d = (size_t)FM_MIN((i64)n, h->got - j->seen);
    j->seen += (i64)d;
    p += d;
    n -= d;
  }
  if (!n) return true;
  if (!s->head_in) {                                   /* the first bytes since a (re)start */
    if (!h->sniffed) hls_sniff_type(s, p, n);
    s->status = 200;
    s->head_in = true;
    SDL_CondBroadcast(s->cv);
  }
  j->seen += (i64)n;
  h->got += (i64)n;
  h->wpos += (i64)n;
  if (s->discard > 0) {                                /* a seek target further on */
    size_t d = (size_t)FM_MIN((i64)n, s->discard);
    s->discard -= (i64)d;
    p += d;
    n -= d;
  }
  return !n || put_audio(s, p, n);
}

static void hls_on_head(void *u, const FmNetResp *r) {
  HlsJob *j = (HlsJob *)u;
  SDL_LockMutex(j->s->mx);
  j->status = r->status;
  SDL_UnlockMutex(j->s->mx);
}

static bool hls_on_data(void *u, const u8 *p, size_t n) {
  HlsJob *j = (HlsJob *)u;
  FmNetStream *s = j->s;
  SDL_LockMutex(s->mx);
  while (s->hls->turn != j && !s->cancel && !j->stop) SDL_CondWaitTimeout(s->cv, s->mx, 250);   /* ahead */
  bool ok = !s->cancel && !j->stop && j->status >= 200 && j->status < 300;
  if (ok) ok = j->enc ? hls_dec_feed(&j->dec, p, n, hls_put, j) : hls_put(j, p, n);
  SDL_UnlockMutex(s->mx);
  return ok;
}

/* Waits ms (or until closed); false when the stream is closing. */
static bool hls_nap(FmNetStream *s, u32 ms) {
  SDL_LockMutex(s->mx);
  u64 until = SDL_GetTicks64() + ms;
  while (!s->cancel && SDL_GetTicks64() < until) SDL_CondWaitTimeout(s->cv, s->mx, 100);
  bool go = !s->cancel;
  SDL_UnlockMutex(s->mx);
  return go;
}

/* GET a playlist (or key) into r; err says why not. */
static FmErr hls_get(FmNetStream *s, const char *url, size_t max, FmNetResp *r, char *err, size_t errcap) {
  char *u = fm_strndup(url, strcspn(url, "#"));        /* fragments are ours (#t=) or the site's */
  FmErr e = net_get(u, s->headers[0] ? s->headers : NULL, max, r, &s->cancel);
  fm_free(u);
  if (e == FM_OK && (r->status < 200 || r->status >= 300)) {
    if (err) fm_snprintf(err, errcap, "The server answered %d", r->status);
    net_resp_free(r);
    return FM_ERR_IO;
  }
  if (e != FM_OK && err) fm_strlcpy(err, r->error[0] ? r->error : fm_err_str(e), errcap);
  if (e != FM_OK) net_resp_free(r);
  return e;
}

/* Live: a fresh copy of the playlist (only the worker reads h->pl). */
static bool hls_reload(FmNetStream *s) {
  Hls *h = s->hls;
  FmNetResp r;
  if (hls_get(s, h->media, HLS_MAX_TEXT, &r, NULL, 0) != FM_OK) return false;
  FmHlsMedia pl;
  FmErr e = hls_parse_media((const char *)r.data, r.len, h->media, &pl, NULL, 0);
  net_resp_free(&r);
  if (e != FM_OK) return false;
  hls_media_free(&h->pl);
  h->pl = pl;
  return true;
}

static void job_free(HlsJob *j) {
  if (!j) return;
  if (j->thr) {
    SDL_LockMutex(j->s->mx);
    j->stop = 1;
    SDL_CondBroadcast(j->s->cv);
    SDL_UnlockMutex(j->s->mx);
    SDL_WaitThread(j->thr, NULL);
  }
  fm_free(j->url);
  fm_free(j);
}

/* A job for segment g (NULL: the init section); fetches its key when it is
** not the one already held. NULL with err set when it cannot play. */
static HlsJob *job_new(FmNetStream *s, const FmHlsSeg *g, int item, char *err, size_t errcap) {
  Hls *h = s->hls;
  HlsJob *j = (HlsJob *)fm_calloc(1, sizeof *j);
  j->s = s;
  j->init = !g;
  j->item = item;
  j->seq = g ? g->seq : -1;
  j->url = (char *)fm_alloc(HLS_URL_MAX);
  hls_resolve(h->pl.base, g ? g->uri : h->pl.map_uri, j->url, HLS_URL_MAX);
  i64 off = g ? g->br_off : h->pl.map_off, len = g ? g->br_len : h->pl.map_len;
  if (len >= 0)
    fm_snprintf(j->hdr, sizeof j->hdr, "%sRange: bytes=%lld-%lld\r\n", s->headers, (long long)off,
                (long long)(off + len - 1));
  else
    fm_strlcpy(j->hdr, s->headers, sizeof j->hdr);
  if (!j->url[0]) {
    fm_strlcpy(err, "A segment address is too long", errcap);
    job_free(j);
    return NULL;
  }
  if (g && g->key >= 0 && g->key < h->pl.nkey) {
    const FmHlsKey *k = &h->pl.key[g->key];
    if (k->method != HLS_KEY_AES128) {
      fm_strlcpy(err, "The video is encrypted with DRM (SAMPLE-AES), which this player cannot decrypt", errcap);
      job_free(j);
      return NULL;
    }
    if (!h->key_uri || strcmp(h->key_uri, k->uri)) {
      FmNetResp r;
      if (hls_get(s, k->uri, 4096, &r, err, errcap) != FM_OK) { job_free(j); return NULL; }
      bool ok = r.len == 16;
      if (ok) memcpy(h->key, r.data, 16);
      net_resp_free(&r);
      if (!ok) {
        fm_strlcpy(err, "The video's decryption key is not 16 bytes", errcap);
        job_free(j);
        return NULL;
      }
      fm_free(h->key_uri);
      h->key_uri = fm_strdup(k->uri);
    }
    memcpy(j->key, h->key, 16);
    if (k->has_iv) memcpy(j->iv, k->iv, 16);
    else hls_seq_iv(g->seq, j->iv);
    j->enc = true;
  }
  return j;
}

/* One try of the request; cancel is the stream's (here) or the job's own
** (running ahead on its thread). */
static void job_try(HlsJob *j, volatile int *cancel) {
  FmNetStream *s = j->s;
  SDL_LockMutex(s->mx);
  j->status = 0;
  j->seen = 0;
  if (j->enc) hls_dec_init(&j->dec, j->key, j->iv);
  SDL_UnlockMutex(s->mx);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get_stream(j->url, j->hdr[0] ? j->hdr : NULL, hls_on_head, hls_on_data, j, &r, cancel);
  SDL_LockMutex(s->mx);
  j->e = e;
  if (!j->status) j->status = r.status;
  fm_strlcpy(j->error, r.error, sizeof j->error);
  j->tried = true;
  SDL_UnlockMutex(s->mx);
}

static int job_thread(void *u) {
  HlsJob *j = (HlsJob *)u;
  job_try(j, &j->stop);
  SDL_LockMutex(j->s->mx);
  j->running = false;
  SDL_CondBroadcast(j->s->cv);
  SDL_UnlockMutex(j->s->mx);
  return 0;
}

/* Finishes the job holding the turn, with retries. 0 done, 1 skip it (live:
** gone), -1 failed (err), -2 closing. */
static int job_finish(FmNetStream *s, HlsJob *j, char *err, size_t errcap) {
  Hls *h = s->hls;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (j->thr) {                                      /* it ran ahead: let it finish */
      SDL_LockMutex(s->mx);
      while (j->running && !s->cancel) SDL_CondWaitTimeout(s->cv, s->mx, 250);
      if (s->cancel) j->stop = 1;
      SDL_UnlockMutex(s->mx);
      SDL_WaitThread(j->thr, NULL);
      j->thr = NULL;
    } else if (attempt > 0 || !j->tried) {
      job_try(j, &s->cancel);
    }
    if (s->cancel) return -2;
    SDL_LockMutex(s->mx);
    int st = j->status;
    bool ok = j->e == FM_OK && st >= 200 && st < 300 && j->seen >= h->got;
    bool bad = ok && j->enc && !hls_dec_end(&j->dec, hls_put, j);
    SDL_UnlockMutex(s->mx);
    if (bad) { fm_strlcpy(err, "A segment did not decrypt (wrong key?)", errcap); return -1; }
    if (ok) return 0;
    if ((st == 404 || st == 410) && h->live) return 1; /* rolled off the live window */
    if (st >= 400 && st < 500) {
      fm_snprintf(err, errcap, "The server answered %d", st);
      SDL_LockMutex(s->mx);
      if (!s->head_in) s->status = st;                 /* wait_head reports it */
      SDL_UnlockMutex(s->mx);
      return -1;
    }
    fm_strlcpy(err, j->error[0] ? j->error : st ? "The server failed" : fm_err_str(j->e), errcap);
    if (!hls_nap(s, 400u << attempt)) return -2;
    j->tried = false;
  }
  return -1;
}

/* What the cursor points at: 1 = *g (NULL: the init section) / *item, 0 = the
** end of the list, -1 = nothing yet (live: reload). */
static int hls_cursor(Hls *h, const FmHlsSeg **g, int *item) {
  *g = NULL;
  *item = -1;
  if (!h->live) {
    if (h->item >= h->pl.n) return 0;
    if (h->item >= 0) *g = &h->pl.seg[h->item];
    *item = h->item;
    return 1;
  }
  if (h->pl.map_uri && h->init_len < 0) return 1;      /* live fMP4: the init section first */
  if (h->next_seq < 0 && h->pl.n) h->next_seq = h->pl.seg[FM_MAX(0, h->pl.n - 3)].seq;
  if (h->pl.n && h->next_seq < h->pl.first_seq) h->next_seq = h->pl.first_seq;     /* fell behind */
  i64 k = h->next_seq - h->pl.first_seq;
  if (h->pl.n && h->next_seq >= 0 && k >= 0 && k < h->pl.n) {
    *g = &h->pl.seg[k];
    return 1;
  }
  return h->pl.endlist ? 0 : -1;
}

/* The segment after job j, when the playlist already lists it. */
static const FmHlsSeg *hls_after(Hls *h, const HlsJob *j, int *item) {
  *item = -1;
  if (!h->live) {
    int k = j->init ? h->first : j->item + 1;
    if (k >= h->pl.n) return NULL;
    *item = k;
    return &h->pl.seg[k];
  }
  if (j->init) return NULL;                            /* the cursor decides after the init section */
  i64 k = j->seq + 1 - h->pl.first_seq;
  return k >= 0 && k < h->pl.n ? &h->pl.seg[k] : NULL;
}

static int hls_worker(void *u) {
  FmNetStream *s = (FmNetStream *)u;
  Hls *h = s->hls;
  char err[160] = "";
  bool done = false;                                   /* reached the end of the list */
  int reloads_failed = 0;
  HlsJob *cur = NULL, *next = NULL;
  while (!s->cancel) {
    if (!cur) {
      const FmHlsSeg *g;
      int item, rc = hls_cursor(h, &g, &item);
      if (rc == 0) { done = true; break; }
      if (rc < 0) {                                    /* live, nothing new yet: wait, reload */
        double t = h->pl.target > 0 ? h->pl.target : 4;
        if (!hls_nap(s, (u32)FM_CLAMP(t * 500, 1000, 10000))) break;
        if (hls_reload(s)) reloads_failed = 0;
        else if (++reloads_failed >= 5) { fm_strlcpy(err, "The live playlist stopped answering", sizeof err); break; }
        continue;
      }
      if (!(cur = job_new(s, g, item, err, sizeof err))) break;
    }
    SDL_LockMutex(s->mx);                              /* cur's turn */
    h->turn = cur;
    h->got = 0;
    h->seg_pos = h->wpos;
    if (!cur->init && !h->live && h->off[cur->item] < 0) h->off[cur->item] = h->wpos;
    SDL_CondBroadcast(s->cv);
    SDL_UnlockMutex(s->mx);
    if (!next) {                                       /* the one after starts connecting now */
      int item;
      const FmHlsSeg *g = hls_after(h, cur, &item);
      char e2[160];
      if (g && (next = job_new(s, g, item, e2, sizeof e2)) != NULL) {
        next->running = true;
        next->thr = fm_thread_create(job_thread, "netstream-hls", next);
        if (!next->thr) { job_free(next); next = NULL; }
      }
    }
    int rc = job_finish(s, cur, err, sizeof err);
    SDL_LockMutex(s->mx);
    h->turn = NULL;
    if (rc >= 0) {
      if (cur->init) {
        h->init_len = h->wpos;                         /* the init section: all there is before */
        if (!h->live) h->item = h->first;
      } else if (!h->live) {
        h->item = cur->item + 1;
      } else {
        h->next_seq = cur->seq + 1;
      }
    }
    SDL_UnlockMutex(s->mx);
    if (rc < 0) break;
    err[0] = 0;
    job_free(cur);
    cur = next;                                        /* already on its way */
    next = NULL;
    if (cur && h->live && cur->seq != h->next_seq) {   /* the cursor moved on (fell behind) */
      job_free(cur);
      cur = NULL;
    }
  }
  bool failed = err[0] && !s->cancel;
  job_free(next);                                      /* stops what runs ahead */
  job_free(cur);
  SDL_LockMutex(s->mx);
  if (failed) {
    s->failed = true;
    fm_strlcpy(s->error, err, sizeof s->error);
  }
  s->head_in = true;                                   /* tell the opener (status 0 or the HTTP error) */
  if (done && !s->failed) s->size = h->wpos;           /* now the length is known */
  s->eof = true;
  SDL_CondBroadcast(s->cv);
  SDL_UnlockMutex(s->mx);
  return 0;
}

/* Sets the worker's cursor so its first byte out is stream offset `at`. */
static void hls_locate(FmNetStream *s, i64 at) {
  Hls *h = s->hls;
  h->got = 0;
  h->turn = NULL;
  if (h->live) {                                       /* only forward: from the running segment */
    h->wpos = h->seg_pos;
    s->discard = FM_MAX(0, at - h->seg_pos);
    return;
  }
  bool map = h->pl.map_uri != NULL;
  int k = -1;
  if (!map || (h->init_len >= 0 && at >= h->init_len))
    for (int i = h->pl.n - 1; i >= h->first; i--)
      if (h->off[i] >= 0 && h->off[i] <= at) { k = i; break; }
  if (k >= 0) {
    h->item = k;
    h->wpos = h->off[k];
  } else {                                             /* from the top */
    h->item = map ? -1 : h->first;
    h->wpos = 0;
  }
  s->discard = at - h->wpos;
}

static bool start_worker(FmNetStream *s, i64 at) {
  s->cancel = 0;
  s->rd = s->wr = s->fill = 0;
  s->eof = s->failed = s->head_in = false;
  s->status = 0;
  s->req_start = at;
  s->pos = at;
  s->pending = -1;
  s->discard = 0;
  s->meta_len = 0;
  s->meta_left = s->metaint;
  if (s->hls) hls_locate(s, at);
  s->thr = fm_thread_create(s->hls ? hls_worker : worker, "netstream", s);
  return s->thr != NULL;
}

static void stop_worker(FmNetStream *s) {
  if (!s->thr) return;
  SDL_LockMutex(s->mx);
  s->cancel = 1;
  SDL_CondBroadcast(s->cv);
  SDL_UnlockMutex(s->mx);
  SDL_WaitThread(s->thr, NULL);
  s->thr = NULL;
}

/* ---- public ---------------------------------------------------------------------- */

/* stream: open streams by URL, for the video player's buffered-range bar (ns_url_buffered) */
static FmNetStream *g_open[8];
static SDL_SpinLock g_open_lk;

static void open_list(FmNetStream *s, bool add) {
  SDL_AtomicLock(&g_open_lk);
  for (int i = 0; i < FM_COUNT(g_open); i++)
    if (add ? !g_open[i] : g_open[i] == s) { g_open[i] = add ? s : NULL; break; }
  SDL_AtomicUnlock(&g_open_lk);
}

bool ns_url_buffered(const char *url, i64 *pos, i64 *end, i64 *size) {
  bool ok = false;
  SDL_AtomicLock(&g_open_lk);
  for (int i = 0; i < FM_COUNT(g_open) && !ok; i++) {
    FmNetStream *s = g_open[i];
    if (!s || strcmp(s->url, url)) continue;
    SDL_LockMutex(s->mx);
    *pos = s->pos;
    *end = s->pos + (i64)s->fill;
    *size = s->size;
    SDL_UnlockMutex(s->mx);
    ok = true;
  }
  SDL_AtomicUnlock(&g_open_lk);
  return ok;
}

static bool wait_head(FmNetStream *s, char *err, size_t errcap) {
  SDL_LockMutex(s->mx);
  u64 until = SDL_GetTicks64() + 15000;
  while (!s->head_in && !s->aborted && SDL_GetTicks64() < until) SDL_CondWaitTimeout(s->cv, s->mx, 250);
  bool ok = s->head_in && s->status >= 200 && s->status < 300 && !s->failed;
  if (!ok && err) {
    if (!s->head_in) fm_strlcpy(err, "The server did not answer", errcap);
    else if (s->status >= 400) fm_snprintf(err, errcap, "The server answered %d", s->status);
    else fm_strlcpy(err, s->error[0] ? s->error : "Connection failed", errcap);
  }
  SDL_UnlockMutex(s->mx);
  return ok;
}

/* "#t=12.5" (or "#a=b&t=12.5") in the URL's fragment; 0 when none */
static double frag_time(const char *url) {
  const char *f = strchr(url, '#');
  if (!f) return 0;
  for (const char *p = f + 1; *p;) {
    if (!strncmp(p, "t=", 2)) {
      double t = strtod(p + 2, NULL);
      return t > 0 && t < 1e7 ? t : 0;
    }
    p += strcspn(p, "&");
    if (*p) p++;
  }
  return 0;
}

/* Turns s into an HLS reader: fetches the playlist (a master picks a
** variant), refuses what cannot play, starts the segment worker. */
static bool hls_open(FmNetStream *s, char *err, size_t errcap) {
  char why[160] = "";
  Hls *h = (Hls *)fm_calloc(1, sizeof *h);
  s->hls = h;
  s->size = -1;                /* forget a plain open's answer (the playlist's own length) */
  s->metaint = 0;
  s->ctype[0] = 0;
  h->media = fm_strdup(s->url);
  FmNetResp r;
  if (hls_get(s, h->media, HLS_MAX_TEXT, &r, why, sizeof why) != FM_OK) goto fail;
  int kind = hls_kind((const char *)r.data, r.len);
  if (kind == HLS_MASTER) {
    FmHlsMaster *m = (FmHlsMaster *)fm_alloc(sizeof *m);
    FmErr e = hls_parse_master((const char *)r.data, r.len, h->media, m, why, sizeof why);
    net_resp_free(&r);
    if (e == FM_OK) {
      fm_free(h->media);
      h->media = fm_strdup(m->v[hls_pick_variant(m, 1080)].uri);
      hls_master_free(m);
    }
    fm_free(m);
    if (e != FM_OK || hls_get(s, h->media, HLS_MAX_TEXT, &r, why, sizeof why) != FM_OK) goto fail;
    kind = hls_kind((const char *)r.data, r.len);
  }
  FmErr e = kind == HLS_MEDIA ? hls_parse_media((const char *)r.data, r.len, h->media, &h->pl, why, sizeof why)
                              : FM_ERR_FORMAT;
  net_resp_free(&r);
  if (e != FM_OK) goto fail;
  if (h->pl.map_changes) {
    fm_strlcpy(why, "The stream changes its fMP4 init section midway, which is not supported", sizeof why);
    goto fail;
  }
  for (int i = 0; i < h->pl.nkey; i++)
    if (h->pl.key[i].method != HLS_KEY_AES128) {
      fm_strlcpy(why, "The video is encrypted with DRM (SAMPLE-AES), which this player cannot decrypt", sizeof why);
      goto fail;
    }
  h->live = !h->pl.endlist;
  h->init_len = h->pl.map_uri ? -1 : 0;
  h->next_seq = -1;
  if (!h->live) {
    h->off = (i64 *)fm_alloc(sizeof(i64) * (size_t)h->pl.n);
    for (int i = 0; i < h->pl.n; i++) h->off[i] = -1;
    h->first = hls_seg_at(&h->pl, frag_time(s->url));
    h->t0 = h->pl.seg[h->first].start;
  }
  s->ranges = !h->live;        /* VOD seeks by byte (see the top comment) */
  return start_worker(s, 0) && wait_head(s, err, errcap);
fail:
  if (err) fm_strlcpy(err, why[0] ? why : "Not a playable HLS stream", errcap);
  return false;
}

/* A plain open that turned out to be a playlist (no .m3u8 in the address). */
static bool hls_behind(FmNetStream *s) {
  if (hls_type_like(s->ctype)) return true;
  if (s->metaint || !fm_strnicmp(s->ctype, "audio/", 6) || !fm_strnicmp(s->ctype, "video/", 6) ||
      !fm_strnicmp(s->ctype, "image/", 6))
    return false;
  char head[16];
  size_t n = ns_peek(s, head, 7);
  return hls_sniff(head, n);
}

FmNetStream *ns_open(const char *url, const char *headers, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  if (!net_available()) {
    if (err) fm_strlcpy(err, net_backend(), errcap);
    return NULL;
  }
  FmNetStream *s = (FmNetStream *)fm_calloc(1, sizeof *s);
  s->url = fm_strdup(url);
  if (headers) fm_strlcpy(s->headers, headers, sizeof s->headers);
  s->mx = SDL_CreateMutex();
  s->cv = SDL_CreateCond();
  s->ring = (u8 *)fm_alloc(RING);
  s->cap = RING;
  s->cap_max = RING_MAX;
  s->size = -1;
  s->pending = -1;
  bool ok = s->mx && s->cv;
  if (ok && hls_url_like(url)) {
    ok = hls_open(s, err, errcap);
  } else if (ok) {
    ok = start_worker(s, 0) && wait_head(s, err, errcap);
    if (ok && hls_behind(s)) {
      stop_worker(s);
      ok = hls_open(s, err, errcap);
    }
  }
  if (!ok) {
    ns_close(s);
    return NULL;
  }
  open_list(s, true);          /* stream */
  return s;
}

/* Starts the request for a lazy seek. false when the server refused. */
static bool apply_pending(FmNetStream *s) {
  i64 at = s->pending;
  if (at < 0) return true;
  if (s->aborted) return false;
  if (s->size >= 0 && at >= s->size) {      /* at the end: nothing to fetch */
    stop_worker(s);
    s->rd = s->wr = s->fill = 0;
    s->pending = -1;
    s->pos = at;
    s->eof = true;
    return true;
  }
  stop_worker(s);
  if (!start_worker(s, at)) return false;
  return wait_head(s, NULL, 0);
}

size_t ns_peek(FmNetStream *s, void *out, size_t n) {
  if (s->pending >= 0 && !apply_pending(s)) return 0;
  SDL_LockMutex(s->mx);
  if (n > RING / 2) n = RING / 2;           /* the smallest ring: a peek never waits on growth */
  while (s->fill < n && !s->eof && !s->aborted) SDL_CondWaitTimeout(s->cv, s->mx, 500);
  size_t got = FM_MIN(n, s->fill);
  size_t first = FM_MIN(got, s->cap - s->rd);
  memcpy(out, s->ring + s->rd, first);
  if (got > first) memcpy((u8 *)out + first, s->ring, got - first);
  SDL_UnlockMutex(s->mx);
  return got;
}

/* The connection ended before the file did (idle servers drop it, networks
** hiccup): reconnect at the current position. Up to 3 times in a row. */
static bool resume(FmNetStream *s) {
  bool can = s->eof && !s->fill && s->ranges && s->size >= 0 && s->pos < s->size && s->resumes < 3 && !s->aborted;
  if (!can) return false;
  s->resumes++;
  i64 at = s->pos;
  SDL_UnlockMutex(s->mx);
  stop_worker(s);
  bool ok = start_worker(s, at) && wait_head(s, NULL, 0);
  SDL_LockMutex(s->mx);
  return ok;
}

size_t ns_read(FmNetStream *s, void *out, size_t n) {
  if (s->pending >= 0 && !apply_pending(s)) return 0;
  u8 *o = (u8 *)out;
  size_t got = 0;
  SDL_LockMutex(s->mx);
  while (got < n) {
    while (!s->fill && !s->eof && !s->aborted) SDL_CondWaitTimeout(s->cv, s->mx, 500);
    if (s->aborted) break;
    if (!s->fill && !got && resume(s)) continue;
    if (!s->fill) break;                 /* end or error */
    s->resumes = 0;
    size_t run = FM_MIN(n - got, FM_MIN(s->fill, s->cap - s->rd));
    memcpy(o + got, s->ring + s->rd, run);
    s->rd = (s->rd + run) % s->cap;
    s->fill -= run;
    s->pos += (i64)run;
    got += run;
    SDL_CondBroadcast(s->cv);             /* room for the worker */
    if (got && !s->fill) break;          /* return what we have rather than wait */
  }
  SDL_UnlockMutex(s->mx);
  return got;
}

bool ns_seek(FmNetStream *s, i64 pos) {
  if (pos < 0 || s->aborted) return false;
  SDL_LockMutex(s->mx);
  i64 cur = s->pos, size = s->size;
  bool ranges = s->ranges;
  /* a short hop forward inside buffered data: just skip */
  if (pos >= cur && pos - cur <= (i64)s->fill) {
    size_t skip = (size_t)(pos - cur);
    s->rd = (s->rd + skip) % s->cap;
    s->fill -= skip;
    s->pos = pos;
    SDL_CondBroadcast(s->cv);
    SDL_UnlockMutex(s->mx);
    return true;
  }
  SDL_UnlockMutex(s->mx);
  if (pos == cur && s->pending < 0) return true;
  /* live streams cannot seek; files can, lazily (see the top comment); live
  ** HLS can skip forward (by reading on) */
  if (s->hls && s->hls->live) {
    if (pos < cur) return false;
  } else if ((!ranges && pos != 0) || (size >= 0 && pos > size)) {
    return false;
  }
  s->pending = pos;
  s->pos = pos;
  return true;
}

i64 ns_tell(const FmNetStream *s) { return s->pos; }
i64 ns_size(const FmNetStream *s) { return s->size; }
bool ns_live(const FmNetStream *s) { return s->size < 0 && !s->ranges; }
bool ns_seekable(const FmNetStream *s) { return s->ranges && s->size > 0; }
const char *ns_content_type(const FmNetStream *s) { return s->ctype; }
bool ns_is_hls(const FmNetStream *s) { return s->hls != NULL; }

size_t ns_peek_at(FmNetStream *s, i64 pos, void *out, size_t n) {
  size_t got = 0;
  SDL_LockMutex(s->mx);
  if (s->pending < 0 && pos >= s->pos && pos - s->pos < (i64)s->fill) {
    size_t off = (size_t)(pos - s->pos);
    got = FM_MIN(n, s->fill - off);
    size_t at = (s->rd + off) % s->cap;
    size_t first = FM_MIN(got, s->cap - at);
    memcpy(out, s->ring + at, first);
    if (got > first) memcpy((u8 *)out + first, s->ring, got - first);
  }
  SDL_UnlockMutex(s->mx);
  return got;
}

size_t ns_buffered(FmNetStream *s) {
  SDL_LockMutex(s->mx);
  size_t f = s->fill;
  SDL_UnlockMutex(s->mx);
  return f;
}

void ns_limit_ring(FmNetStream *s, size_t max) {
  SDL_LockMutex(s->mx);
  s->cap_max = FM_MAX(max, (size_t)RING);
  SDL_UnlockMutex(s->mx);
}
double ns_hls_start(const FmNetStream *s) { return s->hls ? s->hls->t0 : 0; }
double ns_hls_duration(const FmNetStream *s) { return s->hls && !s->hls->live ? s->hls->pl.total : 0; }

void ns_station(const FmNetStream *s, char *out, size_t cap) {
  SDL_LockMutex(s->mx);
  fm_strlcpy(out, s->station, cap);
  SDL_UnlockMutex(s->mx);
}

void ns_now_playing(const FmNetStream *s, char *out, size_t cap) {
  SDL_LockMutex(s->mx);
  fm_strlcpy(out, s->title, cap);
  SDL_UnlockMutex(s->mx);
}

void ns_abort(FmNetStream *s) {
  if (!s) return;
  SDL_LockMutex(s->mx);
  s->aborted = 1;
  s->cancel = 1;               /* the worker stops at its next poll; ns_close joins it */
  SDL_CondBroadcast(s->cv);
  SDL_UnlockMutex(s->mx);
}

void ns_close(FmNetStream *s) {
  if (!s) return;
  open_list(s, false);         /* stream */
  stop_worker(s);
  if (s->hls) {
    fm_free(s->hls->media);
    hls_media_free(&s->hls->pl);
    fm_free(s->hls->off);
    fm_free(s->hls->key_uri);
    fm_free(s->hls);
  }
  if (s->cv) SDL_DestroyCond(s->cv);
  if (s->mx) SDL_DestroyMutex(s->mx);
  fm_free(s->ring);
  fm_free(s->url);
  fm_free(s);
}
