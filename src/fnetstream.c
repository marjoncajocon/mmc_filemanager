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
*/
#include "fnetstream.h"
#include "fnet.h"
#include "fsdl.h"

#define RING (256u * 1024u)
#define CHUNK (8ll * 1024 * 1024)

struct FmNetStream {
  char *url;
  char headers[512];           /* caller's extra headers */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  volatile int cancel;
  /* ring, guarded by mx */
  u8 *ring;
  size_t rd, wr, fill;         /* read and write index, bytes buffered */
  bool eof, failed, head_in;
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

/* Appends audio bytes to the ring, waiting while it is full. */
static bool put_audio(FmNetStream *s, const u8 *p, size_t n) {
  while (n > 0) {
    while (s->fill == RING && !s->cancel) SDL_CondWaitTimeout(s->cv, s->mx, 500);
    if (s->cancel) return false;
    size_t room = RING - s->fill;
    size_t run = FM_MIN(n, FM_MIN(room, RING - s->wr));
    memcpy(s->ring + s->wr, p, run);
    s->wr = (s->wr + run) % RING;
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
  s->thr = fm_thread_create(worker, "netstream", s);
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
  while (!s->head_in && SDL_GetTicks64() < until) SDL_CondWaitTimeout(s->cv, s->mx, 250);
  bool ok = s->head_in && s->status >= 200 && s->status < 300 && !s->failed;
  if (!ok && err) {
    if (!s->head_in) fm_strlcpy(err, "The server did not answer", errcap);
    else if (s->status >= 400) fm_snprintf(err, errcap, "The server answered %d", s->status);
    else fm_strlcpy(err, s->error[0] ? s->error : "Connection failed", errcap);
  }
  SDL_UnlockMutex(s->mx);
  return ok;
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
  s->size = -1;
  s->pending = -1;
  if (!s->mx || !s->cv || !start_worker(s, 0) || !wait_head(s, err, errcap)) {
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
  if (n > RING / 2) n = RING / 2;
  while (s->fill < n && !s->eof) SDL_CondWaitTimeout(s->cv, s->mx, 500);
  size_t got = FM_MIN(n, s->fill);
  size_t first = FM_MIN(got, RING - s->rd);
  memcpy(out, s->ring + s->rd, first);
  if (got > first) memcpy((u8 *)out + first, s->ring, got - first);
  SDL_UnlockMutex(s->mx);
  return got;
}

/* The connection ended before the file did (idle servers drop it, networks
** hiccup): reconnect at the current position. Up to 3 times in a row. */
static bool resume(FmNetStream *s) {
  bool can = s->eof && !s->fill && s->ranges && s->size >= 0 && s->pos < s->size && s->resumes < 3;
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
    while (!s->fill && !s->eof) SDL_CondWaitTimeout(s->cv, s->mx, 500);
    if (!s->fill && !got && resume(s)) continue;
    if (!s->fill) break;                 /* end or error */
    s->resumes = 0;
    size_t run = FM_MIN(n - got, FM_MIN(s->fill, RING - s->rd));
    memcpy(o + got, s->ring + s->rd, run);
    s->rd = (s->rd + run) % RING;
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
  if (pos < 0) return false;
  SDL_LockMutex(s->mx);
  i64 cur = s->pos, size = s->size;
  bool ranges = s->ranges;
  /* a short hop forward inside buffered data: just skip */
  if (pos >= cur && pos - cur <= (i64)s->fill) {
    size_t skip = (size_t)(pos - cur);
    s->rd = (s->rd + skip) % RING;
    s->fill -= skip;
    s->pos = pos;
    SDL_CondBroadcast(s->cv);
    SDL_UnlockMutex(s->mx);
    return true;
  }
  SDL_UnlockMutex(s->mx);
  if (pos == cur && s->pending < 0) return true;
  /* live streams cannot seek; files can, lazily (see the top comment) */
  if ((!ranges && pos != 0) || (size >= 0 && pos > size)) return false;
  s->pending = pos;
  s->pos = pos;
  return true;
}

i64 ns_tell(const FmNetStream *s) { return s->pos; }
i64 ns_size(const FmNetStream *s) { return s->size; }
bool ns_live(const FmNetStream *s) { return s->size < 0 && !s->ranges; }
bool ns_seekable(const FmNetStream *s) { return s->ranges && s->size > 0; }
const char *ns_content_type(const FmNetStream *s) { return s->ctype; }

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

void ns_close(FmNetStream *s) {
  if (!s) return;
  open_list(s, false);         /* stream */
  stop_worker(s);
  if (s->cv) SDL_DestroyCond(s->cv);
  if (s->mx) SDL_DestroyMutex(s->mx);
  fm_free(s->ring);
  fm_free(s->url);
  fm_free(s);
}
