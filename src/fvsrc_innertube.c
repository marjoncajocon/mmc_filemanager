/* fvsrc_innertube.c -- YouTube without yt-dlp: the player and search
** endpoints of YouTube's own web API ("InnerTube", youtubei/v1), as the
** official apps call them. Works wherever fnet works (Android included).
**
** Design decisions:
**   - The player call poses as the visionOS app (client "VISIONOS"): it is
**     the client current yt-dlp uses when no JavaScript runtime is present,
**     because its replies carry plain stream URLs (no signature cipher, no
**     "n" parameter to compute) and need no PO token. Measured 2026-10-09:
**     every size from 144p to 2160p60 in VP9, H.264 and AV1, Opus and AAC
**     sound, served with byte ranges. Clients change; the values live in one
**     table (kPlayer) and yt-dlp stays the fallback when this path fails.
**   - YouTube answers a first anonymous call with LOGIN_REQUIRED ("confirm
**     you're not a bot") but hands out a visitor id in the same reply; one
**     retry with it plays. The id is kept for the process (one small global
**     behind a lock), so later calls need one round trip. No page scraping:
**     the 1.2 MB watch page would cost a phone more than the video starts.
**   - The reply is converted into the format list a yt-dlp -J reply has and
**     handed to vsrc_ytdlp_pick_stream, so the quality menu, codec choice
**     and ranking are the same code whichever path resolved the video.
**   - Search uses the web client (it needs no key). Its next-page tokens are
**     600-1000 characters, longer than FmVsrcPage.next, so they stay here in
**     a small ring and the page carries "i:<slot>".
**   - Formats that would need deciphering (signatureCipher), OTF segment
**     streams and live DASH are skipped.
**   - Live streams play from the HLS master playlist the same reply names
**     (streamingData.hlsManifestUrl; the visionOS client lists it, measured
**     2026-10-09): its variants become the quality menu, like Dailymotion's
**     (vsrc_hls_pick). Each variant is H.264 in MPEG-TS with the sound as a
**     separate packed-AAC playlist (the pair the player already opens), and
**     fnetstream follows the live edge. A reply without the address is asked
**     again as the iOS app (kPlayerLive) before giving up to yt-dlp; live
**     streams that have not started or have ended say so in words.
*/
#include "fvsrc.h"
#include "fvsrc_int.h"
#include "fjson.h"
#include "fnet.h"
#include "fhls.h"
#include "fsdl.h"
#include <ctype.h>
#include <stdarg.h>

#define IT_API "https://www.youtube.com/youtubei/v1/"

/* the player clients (see the header) */
typedef struct ItClient {
  const char *name, *version, *id, *make, *model, *os, *os_ver, *ua;
} ItClient;
static const ItClient kPlayer = {
  "VISIONOS", "1.02", "101", "Apple", "RealityDevice17,1", "visionOS", "26.5.23O471",
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  "Version/26.0 Safari/605.1.15",
};
/* asked only when kPlayer's reply for a live stream lacks the HLS address.
** Measured 2026-10-09 it plays but lists no hlsManifestUrl either (it wants a
** PO token for that now); kept as the cheap second try clients rotate to */
static const ItClient kPlayerLive = {
  "IOS", "20.10.4", "5", "Apple", "iPhone16,2", "iPhone", "18.3.2.22D82",
  "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
};

/* the search client */
#define IT_WEB_VERSION "2.20260708.01.00"
#define IT_WEB_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
                  "Chrome/140.0.0.0 Safari/537.36"

/* ---- a growable string -------------------------------------------------------- */

typedef struct Sb { char *p; size_t len, cap; bool oom; } Sb;

static void sb_put(Sb *b, const char *s, size_t n) {
  if (b->oom) return;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + n + 1) nc *= 2;
    char *np = (char *)fm_realloc(b->p, nc);
    if (!np) { b->oom = true; return; }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

static void sb_s(Sb *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_f(Sb *b, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) sb_put(b, tmp, FM_MIN((size_t)n, sizeof tmp - 1));
}

/* s as a JSON string literal, quotes included */
static void sb_q(Sb *b, const char *s) {
  sb_put(b, "\"", 1);
  for (; *s; s++) {
    u8 c = (u8)*s;
    if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_put(b, e, 2); }
    else if (c < 0x20) sb_f(b, "\\u%04x", c);
    else sb_put(b, s, 1);
  }
  sb_put(b, "\"", 1);
}

/* ---- visitor id and the next-page tokens (process-wide) ------------------------ */

static SDL_SpinLock g_lock;
static char g_visitor[1024];

#define TOK_SLOTS 16
static struct { char *tok; unsigned id; } g_tok[TOK_SLOTS];
static unsigned g_tok_next = 1;

static void visitor_get(char *out, size_t cap) {
  SDL_AtomicLock(&g_lock);
  fm_strlcpy(out, g_visitor, cap);
  SDL_AtomicUnlock(&g_lock);
}

/* only base64url and %-escapes: it goes into JSON and a header line */
static void visitor_set(const char *v) {
  if (!v || !*v || strlen(v) >= sizeof g_visitor || !vsrc_id_ok(v, "%_=-", sizeof g_visitor - 1)) return;
  SDL_AtomicLock(&g_lock);
  fm_strlcpy(g_visitor, v, sizeof g_visitor);
  SDL_AtomicUnlock(&g_lock);
}

static void visitor_forget(void) {
  SDL_AtomicLock(&g_lock);
  g_visitor[0] = 0;
  SDL_AtomicUnlock(&g_lock);
}

static void token_put(const char *tok, char *out, size_t cap) {
  out[0] = 0;
  if (!tok || !*tok) return;
  char *copy = fm_strdup(tok);
  SDL_AtomicLock(&g_lock);
  unsigned id = g_tok_next++;
  int slot = (int)(id % TOK_SLOTS);
  char *old = g_tok[slot].tok;
  g_tok[slot].tok = copy;
  g_tok[slot].id = id;
  SDL_AtomicUnlock(&g_lock);
  fm_free(old);
  fm_snprintf(out, cap, "i:%u", id);
}

/* "i:<id>" -> the stored token (fm_free), NULL when it fell out of the ring */
static char *token_get(const char *key) {
  unsigned id = (unsigned)strtoul(key + 2, NULL, 10);
  char *r = NULL;
  SDL_AtomicLock(&g_lock);
  int slot = (int)(id % TOK_SLOTS);
  if (id && g_tok[slot].id == id && g_tok[slot].tok) r = fm_strdup(g_tok[slot].tok);
  SDL_AtomicUnlock(&g_lock);
  return r;
}

/* ---- video ids from links -------------------------------------------------------- */

bool vsrc_innertube_id(const char *s, char *out, size_t cap) {
  if (!s || cap < 12) return false;
  out[0] = 0;
  if (strlen(s) == 11 && vsrc_id_ok(s, "-_", 11)) { fm_strlcpy(out, s, cap); return true; }
  const char *host = strstr(s, "://");
  if (!host) return false;
  host += 3;
  const char *p = NULL;
  static const char *const kAfter[] = { "youtu.be/", "/shorts/", "/embed/", "/live/", "/v/", "?v=", "&v=" };
  for (int i = 0; i < FM_COUNT(kAfter) && !p; i++) {
    const char *f = strstr(host, kAfter[i]);
    if (f) p = f + strlen(kAfter[i]);
  }
  if (!p || (!strstr(host, "youtube.com") && !strstr(host, "youtu.be") && !strstr(host, "youtube-nocookie.com")))
    return false;
  char id[12];
  size_t n = 0;
  while (n < 11 && p[n] && (isalnum((u8)p[n]) || p[n] == '-' || p[n] == '_')) { id[n] = p[n]; n++; }
  id[n] = 0;
  if (n != 11) return false;
  fm_strlcpy(out, id, cap);
  return true;
}

/* ---- the player call ---------------------------------------------------------------- */

static FmErr player_call(const ItClient *cl, const char *id, const char *visitor, FmNetResp *r, char *err,
                         size_t errcap, volatile int *cancel) {
  Sb body = { 0 };
  sb_s(&body, "{\"context\":{\"client\":{\"clientName\":");
  sb_q(&body, cl->name);
  sb_s(&body, ",\"clientVersion\":");
  sb_q(&body, cl->version);
  sb_s(&body, ",\"deviceMake\":");
  sb_q(&body, cl->make);
  sb_s(&body, ",\"deviceModel\":");
  sb_q(&body, cl->model);
  sb_s(&body, ",\"userAgent\":");
  sb_q(&body, cl->ua);
  sb_s(&body, ",\"osName\":");
  sb_q(&body, cl->os);
  sb_s(&body, ",\"osVersion\":");
  sb_q(&body, cl->os_ver);
  sb_s(&body, ",\"hl\":\"en\"");
  if (visitor[0]) { sb_s(&body, ",\"visitorData\":"); sb_q(&body, visitor); }
  sb_s(&body, "}},\"videoId\":");
  sb_q(&body, id);
  sb_s(&body, ",\"contentCheckOk\":true,\"racyCheckOk\":true}");
  char hdr[2048];
  int n = fm_snprintf(hdr, sizeof hdr,
                      "Content-Type: application/json\r\nX-YouTube-Client-Name: %s\r\n"
                      "X-YouTube-Client-Version: %s\r\nOrigin: https://www.youtube.com\r\nUser-Agent: %s\r\n",
                      cl->id, cl->version, cl->ua);
  if (visitor[0] && n > 0 && (size_t)n < sizeof hdr)
    fm_snprintf(hdr + n, sizeof hdr - (size_t)n, "X-Goog-Visitor-Id: %s\r\n", visitor);
  memset(r, 0, sizeof *r);
  FmErr e = body.oom ? FM_ERR_NOMEM
                     : net_post(IT_API "player?prettyPrint=false", hdr, body.p, body.len, VSRC_MAX_REPLY, r, cancel);
  fm_free(body.p);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, errcap, "Network error: %s", r->error[0] ? r->error : fm_err_str(e));
    net_resp_free(r);
    return e;
  }
  if (r->status != 200) {
    vsrc_http_error("YouTube", r->status, err, errcap);
    net_resp_free(r);
    return FM_ERR_IO;
  }
  return FM_OK;
}

/* Why a reply with a status other than OK does not play, in words.
** FM_ERR_NOT_FOUND = a live stream that is not on air (yt-dlp would not
** play it either), FM_ERR_ACCESS = sign-in or age checks. */
static FmErr refusal(const FmJsonNode *root, char *err, size_t errcap) {
  const char *st = json_str(json_path(root, "playabilityStatus.status"), "");
  if (!strcmp(st, "LIVE_STREAM_OFFLINE")) {
    if (json_bool(json_path(root, "videoDetails.isUpcoming"), false)) {
      double at = json_num(json_path(root, "playabilityStatus.liveStreamability.liveStreamabilityRenderer."
                                           "offlineSlate.liveStreamOfflineSlateRenderer.scheduledStartTime"), 0);
      char when[32];
      if (at > 0) fm_snprintf(err, errcap, "This live stream hasn't started yet (it starts %s)",
                              fm_fmt_time((i64)at, when, sizeof when));
      else fm_strlcpy(err, "This live stream hasn't started yet", errcap);
    } else {
      fm_strlcpy(err, "This live stream is offline", errcap);
    }
    return FM_ERR_NOT_FOUND;
  }
  const char *why = json_str(json_path(root, "playabilityStatus.reason"), "");
  if (!why[0]) why = json_str(json_path(root, "playabilityStatus.messages.0"), "");
  if (why[0]) fm_snprintf(err, errcap, "YouTube: %s", why);
  else fm_snprintf(err, errcap, "YouTube will not play this video here (%s)", st[0] ? st : "no status");
  return !strcmp(st, "LOGIN_REQUIRED") || !strcmp(st, "AGE_VERIFICATION_REQUIRED") ? FM_ERR_ACCESS
                                                                                     : FM_ERR_UNSUPPORTED;
}

/* The player reply for id as client cl, parsed into j (json_free). FM_OK
** only when the video plays; else err says why in words. */
static FmErr player(const ItClient *cl, const char *id, FmJson *j, char *err, size_t errcap, volatile int *cancel) {
  char visitor[1024];
  visitor_get(visitor, sizeof visitor);
  for (int round = 0; round < 2; round++) {
    FmNetResp r;
    FmErr e = player_call(cl, id, visitor, &r, err, errcap, cancel);
    if (e != FM_OK) return e;
    e = json_parse(j, (const char *)r.data, r.len);
    net_resp_free(&r);
    if (e != FM_OK) { fm_strlcpy(err, "YouTube sent a reply this app does not understand", errcap); return e; }
    const FmJsonNode *root = json_root(j);
    const char *vd = json_str(json_path(root, "responseContext.visitorData"), "");
    const char *st = json_str(json_path(root, "playabilityStatus.status"), "");
    if (!strcmp(st, "OK")) {
      if (vd[0]) visitor_set(vd);
      return FM_OK;
    }
    /* the anonymous first call: retry once with the visitor id it handed out */
    if (!strcmp(st, "LOGIN_REQUIRED") && round == 0 && vd[0] && strcmp(vd, visitor) != 0) {
      fm_strlcpy(visitor, vd, sizeof visitor);
      visitor_set(vd);
      json_free(j);
      continue;
    }
    e = refusal(root, err, errcap);
    json_free(j);
    return e;
  }
  return FM_ERR_ACCESS;
}

/* ---- the reply as a yt-dlp format list ---------------------------------------------- */

/* "video/webm; codecs=\"vp9\"" -> type "video/webm", codecs "vp9" */
static void split_mime(const char *m, char *type, size_t tcap, char *codecs, size_t ccap) {
  size_t n = strcspn(m, ";");
  fm_strlcpy(type, m, FM_MIN(tcap, n + 1));
  codecs[0] = 0;
  const char *c = strstr(m, "codecs=\"");
  if (c) {
    c += 8;
    size_t k = strcspn(c, "\"");
    fm_strlcpy(codecs, c, FM_MIN(ccap, k + 1));
  }
}

static void add_format(Sb *b, const FmJsonNode *f, bool adaptive, bool *first) {
  const char *url = json_str(json_get(f, "url"), "");
  if (!vsrc_url_ok(url)) return;                                   /* ciphered: skipped */
  if (!strcmp(json_str(json_get(f, "type"), ""), "FORMAT_STREAM_TYPE_OTF")) return;
  char type[48], codecs[96];
  split_mime(json_str(json_get(f, "mimeType"), ""), type, sizeof type, codecs, sizeof codecs);
  bool audio = !strncmp(type, "audio/", 6);
  bool webm = strstr(type, "/webm") != NULL;
  bool mp4 = strstr(type, "/mp4") != NULL;
  if (!webm && !mp4) return;                                       /* 3gpp and the like */
  const char *ext = webm ? "webm" : audio ? "m4a" : "mp4";
  /* "avc1.4d401e, mp4a.40.2" (muxed) or one codec */
  char vc[48] = "none", ac[48] = "none";
  char *comma = strchr(codecs, ',');
  if (comma) {
    *comma = 0;
    const char *second = comma + 1;
    while (*second == ' ') second++;
    fm_strlcpy(vc, codecs, sizeof vc);
    fm_strlcpy(ac, second, sizeof ac);
  } else if (audio) {
    fm_strlcpy(ac, codecs, sizeof ac);
  } else {
    fm_strlcpy(vc, codecs, sizeof vc);
  }
  int itag = (int)json_num(json_get(f, "itag"), 0);
  bool drc = json_bool(json_get(f, "isDrc"), false);
  double br = json_num(json_get(f, "averageBitrate"), 0);
  if (br <= 0) br = json_num(json_get(f, "bitrate"), 0);
  const char *tc = json_str(json_path(f, "colorInfo.transferCharacteristics"), "");
  bool hdr = strstr(tc, "2084") || strstr(tc, "B67");
  const FmJsonNode *track = json_get(f, "audioTrack");
  double lang = !track ? -1 : json_bool(json_get(track, "audioIsDefault"), false) ? 10 : -10;

  if (!*first) sb_s(b, ",");
  *first = false;
  sb_s(b, "{\"url\":");
  sb_q(b, url);
  sb_f(b, ",\"format_id\":\"%d%s\",\"ext\":\"%s\",\"protocol\":\"https\"", itag, drc ? "-drc" : "", ext);
  sb_s(b, ",\"vcodec\":");
  sb_q(b, vc);
  sb_s(b, ",\"acodec\":");
  sb_q(b, ac);
  if (adaptive) sb_f(b, ",\"container\":\"%s_dash\"", webm ? "webm" : audio ? "m4a" : "mp4");
  if (!audio) {
    sb_f(b, ",\"width\":%d,\"height\":%d", (int)json_num(json_get(f, "width"), 0),
         (int)json_num(json_get(f, "height"), 0));
    double fps = json_num(json_get(f, "fps"), 0);
    if (fps > 0) sb_f(b, ",\"fps\":%g", fps);
  }
  if (br > 0) sb_f(b, ",\"tbr\":%.3f", br / 1000.0);
  double len = json_num(json_get(f, "contentLength"), 0);                 /* a numeric string */
  if (len > 0) sb_f(b, ",\"filesize\":%.0f", len);
  sb_f(b, ",\"dynamic_range\":\"%s\",\"language_preference\":%g", hdr ? "HDR" : "SDR", lang);
  if (drc) sb_s(b, ",\"format_note\":\"DRC\"");
  sb_s(b, "}");
}

/* The playable reply in yt-dlp's -J shape ({"duration", "formats": [...]}). */
static char *as_ytdlp(const FmJsonNode *root, size_t *len) {
  Sb b = { 0 };
  double dur = json_num(json_path(root, "videoDetails.lengthSeconds"), 0);
  sb_f(&b, "{\"duration\":%.3f,\"formats\":[", dur);
  bool first = true;
  const FmJsonNode *sd = json_get(root, "streamingData");
  for (const FmJsonNode *f = json_first(json_get(sd, "formats")); f; f = json_next(f)) add_format(&b, f, false, &first);
  for (const FmJsonNode *f = json_first(json_get(sd, "adaptiveFormats")); f; f = json_next(f))
    add_format(&b, f, true, &first);
  sb_s(&b, "]}");
  if (b.oom) { fm_free(b.p); return NULL; }
  *len = b.len;
  return b.p;
}

static void fill_item(const FmJsonNode *root, const char *id, FmVsrcItem *it) {
  const FmJsonNode *vd = json_get(root, "videoDetails");
  fm_strlcpy(it->id, id, sizeof it->id);
  fm_strlcpy(it->title, json_str(json_get(vd, "title"), id), sizeof it->title);
  fm_strlcpy(it->channel, json_str(json_get(vd, "author"), ""), sizeof it->channel);
  it->duration = json_num(json_get(vd, "lengthSeconds"), 0);
  it->views = (i64)json_num(json_get(vd, "viewCount"), -1);
  it->live = json_bool(json_get(vd, "isLive"), false);       /* on air now (isLiveContent: ever was) */
  fm_snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/hqdefault.jpg", id);
  fm_snprintf(it->page, sizeof it->page, "https://www.youtube.com/watch?v=%s", id);
}

static FmErr pick(const FmJsonNode *root, const FmVsrcConf *c, FmVsrcStream *out, char *err, size_t errcap);

FmErr vsrc_innertube_pick(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                          size_t errcap) {
  memset(out, 0, sizeof *out);
  out->cur = -1;
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  FmErr e = pick(json_root(&j), c, out, err, errcap);
  json_free(&j);
  return e;
}

/* ---- live streams ------------------------------------------------------------------------ */

static bool is_live(const FmJsonNode *root) { return json_bool(json_path(root, "videoDetails.isLive"), false); }

/* the HLS master playlist's address in a live reply, NULL when none */
static const char *live_hls(const FmJsonNode *root) {
  const char *u = json_str(json_path(root, "streamingData.hlsManifestUrl"), "");
  return vsrc_url_ok(u) && strlen(u) < HLS_URL_MAX ? u : NULL;
}

/* master playlist text (fetched from base) -> the qualities */
static FmErr live_pick(const char *m3u8, size_t len, const char *base, const FmVsrcConf *c, FmVsrcStream *out,
                       char *err, size_t errcap) {
  FmErr e = vsrc_hls_pick(m3u8, len, base, 0, c, out);
  out->live = true;
  if (e == FM_ERR_FORMAT) fm_strlcpy(err, "YouTube sent a live playlist this app does not understand", errcap);
  else if (e == FM_ERR_UNSUPPORTED)
    fm_strlcpy(err, c->have_ffmpeg_libs ? "None of this live stream's formats can be decoded here"
                                        : "Live streams (H.264 in HLS) need the FFmpeg libraries on this system",
               errcap);
  return e;
}

FmErr vsrc_innertube_live_pick(const char *json, size_t len, const char *m3u8, size_t mlen, const FmVsrcConf *c,
                               FmVsrcStream *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  out->cur = -1;
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  const FmJsonNode *root = json_root(&j);
  const char *hls = live_hls(root);
  FmErr e;
  if (!is_live(root)) e = FM_ERR_FORMAT;
  else if (!hls) e = pick(root, c, out, err, errcap);                  /* the refusal */
  else e = live_pick(m3u8, mlen, hls, c, out, err, errcap);
  json_free(&j);
  return e;
}

/* The live stream of a reply: its HLS address, asked again as kPlayerLive
** when kPlayer's reply has none, then the master playlist's qualities. */
static FmErr live_resolve(const char *id, const FmJsonNode *root, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                          size_t errcap, volatile int *cancel) {
  char url[HLS_URL_MAX];
  const char *hls = live_hls(root);
  if (hls) {
    fm_strlcpy(url, hls, sizeof url);
  } else {
    FmJson j;
    url[0] = 0;
    char ignored[160];
    if (player(&kPlayerLive, id, &j, ignored, sizeof ignored, cancel) == FM_OK) {
      if ((hls = live_hls(json_root(&j))) != NULL) fm_strlcpy(url, hls, sizeof url);
      json_free(&j);
    }
    if (cancel && *cancel) { fm_strlcpy(err, "Cancelled", errcap); return FM_ERR_CANCEL; }
    if (!url[0]) {
      fm_strlcpy(err, "This live stream can't be played without yt-dlp", errcap);
      return FM_ERR_UNSUPPORTED;
    }
    fm_log("youtube: live address from the %s client", kPlayerLive.name);
  }
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, VSRC_MAX_REPLY, &r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); net_resp_free(&r); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, errcap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status != 200) {
    vsrc_http_error("YouTube", r.status, err, errcap);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = live_pick((const char *)r.data, r.len, url, c, out, err, errcap);
  net_resp_free(&r);
  return e;
}

FmErr vsrc_innertube_resolve(const FmVsrcConf *c, const char *id, FmVsrcStream *out, char *err, size_t errcap,
                             volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->cur = -1;
  if (errcap) err[0] = 0;
  if (!vsrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  if (!id || strlen(id) != 11 || !vsrc_id_ok(id, "-_", 11)) {
    fm_strlcpy(err, "Not a YouTube video id", errcap);
    return FM_ERR_FORMAT;
  }
  if (c->fresh) visitor_forget();            /* the old session's links were refused */
  /* YouTube now and then refuses a session's links (403 on the very first
  ** byte; seen on a phone, a few times an hour). One byte of the chosen
  ** picture says so in ~0.2 s, and a new session gets links that play,
  ** instead of a failed open and the player's reconnect (~3 s). */
  FmErr e = FM_OK;
  for (int round = 0; round < 2; round++) {
    FmJson j;
    e = player(&kPlayer, id, &j, err, errcap, cancel);
    if (e != FM_OK) return e;
    if (is_live(json_root(&j))) {
      e = live_resolve(id, json_root(&j), c, out, err, errcap, cancel);
      json_free(&j);
      return e;
    }
    e = pick(json_root(&j), c, out, err, errcap);
    json_free(&j);
    if (e != FM_OK || round == 1) break;
    /* the picture, then the sound beside it: a refused sound link alone
    ** played the picture silent */
    int st = 0;
    for (int k = 0; k < 2 && st != 403; k++) {
      const char *u = k ? out->audio : out->video;
      if (!u[0]) continue;
      FmNetResp r;
      memset(&r, 0, sizeof r);
      FmErr pe = net_get(u, "Range: bytes=0-0\r\n", 4096, &r, cancel);
      st = r.status;
      net_resp_free(&r);
      if (pe == FM_ERR_CANCEL) return pe;
    }
    if (st != 403) break;
    fm_log("youtube: links refused (403), asking again with a new session");
    visitor_forget();
  }
  return e;
}

/* A reply's formats (an on-demand video). A live one has no formats this
** path plays without its HLS address (live_resolve); a refusal says why. */
static FmErr pick(const FmJsonNode *root, const FmVsrcConf *c, FmVsrcStream *out, char *err, size_t errcap) {
  const char *st = json_str(json_path(root, "playabilityStatus.status"), "OK");
  if (strcmp(st, "OK") != 0) return refusal(root, err, errcap);
  if (is_live(root)) {
    fm_strlcpy(err, "This live stream can't be played without yt-dlp", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  size_t len = 0;
  char *fmts = as_ytdlp(root, &len);
  if (!fmts) { fm_strlcpy(err, "Out of memory", errcap); return FM_ERR_NOMEM; }
  FmErr e = vsrc_ytdlp_pick_stream(fmts, len, c, out);
  fm_free(fmts);
  if (e == FM_ERR_UNSUPPORTED)
    fm_strlcpy(err, out->nq ? "None of this video's formats streams with this player's decoders"
                            : "YouTube listed no formats this app can fetch", errcap);
  else if (e != FM_OK)
    fm_strlcpy(err, "YouTube sent a reply this app does not understand", errcap);
  return e;
}

FmErr vsrc_innertube_item(const char *id, FmVsrcItem *it, char *err, size_t errcap, volatile int *cancel) {
  memset(it, 0, sizeof *it);
  it->views = -1;
  FmJson j;
  FmErr e = player(&kPlayer, id, &j, err, errcap, cancel);
  if (e != FM_OK) return e;
  fill_item(json_root(&j), id, it);
  json_free(&j);
  return FM_OK;
}

/* ---- search ---------------------------------------------------------------------------- */

static const char *runs_text(const FmJsonNode *n) {
  const char *s = json_str(json_get(n, "simpleText"), NULL);
  return s ? s : json_str(json_path(n, "runs.0.text"), "");
}

/* "1,234,567 views" / "9,247 watching" -> 1234567; -1 when there are no digits */
static i64 count_of(const char *s) {
  i64 v = 0;
  bool any = false;
  for (; *s && *s != ' '; s++) {
    if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); any = true; }
    else if (*s != ',' && *s != '.') break;
  }
  return any ? v : -1;
}

static void add_video(const FmJsonNode *v, FmVsrcPage *out) {
  const char *id = json_str(json_get(v, "videoId"), "");
  if (strlen(id) != 11 || !vsrc_id_ok(id, "-_", 11) || out->count >= VSRC_MAX_ITEMS) return;
  for (int i = 0; i < out->count; i++)
    if (!strcmp(out->items[i].id, id)) return;                    /* shelves repeat results */
  FmVsrcItem *it = vsrc_page_add(out);
  if (!it) return;
  fm_strlcpy(it->id, id, sizeof it->id);
  fm_strlcpy(it->title, runs_text(json_get(v, "title")), sizeof it->title);
  const FmJsonNode *owner = json_get(v, "ownerText");
  if (!owner) owner = json_get(v, "longBylineText");
  fm_strlcpy(it->channel, runs_text(owner), sizeof it->channel);
  it->duration = vsrc_clock_seconds(json_str(json_path(v, "lengthText.simpleText"), ""));
  const FmJsonNode *vc = json_get(v, "viewCountText");
  char views[64];
  fm_strlcpy(views, runs_text(vc), sizeof views);
  it->views = count_of(views);
  for (const FmJsonNode *bd = json_first(json_get(v, "badges")); bd; bd = json_next(bd))
    if (!strcmp(json_str(json_path(bd, "metadataBadgeRenderer.style"), ""), "BADGE_STYLE_TYPE_LIVE_NOW"))
      it->live = true;
  const FmJsonNode *th = json_path(v, "thumbnail.thumbnails");
  const FmJsonNode *last = NULL;
  for (const FmJsonNode *t = json_first(th); t; t = json_next(t)) last = t;
  const char *tu = json_str(json_get(last, "url"), "");
  /* "hq720.jpg?sqp=...&rs=..." is served as WebP (no decoder here); the
  ** same path without the query is the JPEG */
  if (vsrc_url_ok(tu)) fm_strlcpy(it->thumb, tu, FM_MIN(sizeof it->thumb, strcspn(tu, "?") + 1));
  else fm_snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/hqdefault.jpg", id);
  fm_snprintf(it->page, sizeof it->page, "https://www.youtube.com/watch?v=%s", id);
}

/* Every videoRenderer, wherever the layout of the day puts it, and the
** continuation of the result list (continuationItemRenderer). */
static void walk(const FmJsonNode *n, int depth, FmVsrcPage *out, const char **next) {
  if (!n || depth > 40) return;
  for (const FmJsonNode *c = json_first(n); c; c = json_next(c)) {
    if (c->key && !strcmp(c->key, "videoRenderer")) { add_video(c, out); continue; }
    if (c->key && !strcmp(c->key, "continuationItemRenderer")) {
      const char *t = json_str(json_path(c, "continuationEndpoint.continuationCommand.token"), NULL);
      if (t) *next = t;
      continue;
    }
    if (c->type == JSON_OBJ || c->type == JSON_ARR) walk(c, depth + 1, out, next);
  }
}

FmErr vsrc_innertube_parse_search(const char *json, size_t len, FmVsrcPage *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  const char *next = NULL;
  walk(json_root(&j), 0, out, &next);
  token_put(next, out->next, sizeof out->next);
  json_free(&j);
  return FM_OK;
}

FmErr vsrc_innertube_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                            volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!vsrc_need_net(out->error, sizeof out->error)) return FM_ERR_UNSUPPORTED;
  char *tok = NULL;
  if (page_token && *page_token) {
    if (strncmp(page_token, "i:", 2) || !(tok = token_get(page_token))) return FM_OK;   /* no more pages */
  }
  Sb body = { 0 };
  sb_s(&body, "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" IT_WEB_VERSION "\",\"hl\":\"en\"");
  if (c->region[0] && vsrc_id_ok(c->region, "", 4)) sb_f(&body, ",\"gl\":\"%s\"", c->region);
  sb_s(&body, "}}");
  if (tok) {
    sb_s(&body, ",\"continuation\":");
    sb_q(&body, tok);
  } else {
    sb_s(&body, ",\"query\":");
    sb_q(&body, query);
    sb_s(&body, ",\"params\":\"EgIQAQ%3D%3D\"");                    /* videos only */
  }
  sb_s(&body, "}");
  fm_free(tok);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = body.oom ? FM_ERR_NOMEM
                     : net_post(IT_API "search?prettyPrint=false",
                                "Content-Type: application/json\r\nX-YouTube-Client-Name: 1\r\n"
                                "X-YouTube-Client-Version: " IT_WEB_VERSION "\r\nOrigin: https://www.youtube.com\r\n"
                                "User-Agent: " IT_WEB_UA "\r\n",
                                body.p, body.len, VSRC_MAX_REPLY, &r, cancel);
  fm_free(body.p);
  if (e == FM_ERR_CANCEL) return e;
  if (e != FM_OK) {
    fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status != 200) {
    vsrc_http_error("YouTube", r.status, out->error, sizeof out->error);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_innertube_parse_search((const char *)r.data, r.len, out);
  net_resp_free(&r);
  if (e != FM_OK) vsrc_http_error("YouTube", 0, out->error, sizeof out->error);
  else if (!out->count && !(page_token && *page_token)) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return e;
}
