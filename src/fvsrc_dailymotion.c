/* fvsrc_dailymotion.c -- Dailymotion: keyless search with the public Data
** API; play and download without helper programs (yt-dlp is the fallback).
**
** Design decisions:
**   - api.dailymotion.com/videos answers without a key for read-only search;
**     `fields` keeps each reply to the eight values the gallery shows (plus
**     "onair" for the LIVE badge). The key "owner.screenname" contains a
**     dot, so it is read with json_get, not json_path.
**   - Native resolve (2026-10-09), so phones play without yt-dlp:
**       1. GET www.dailymotion.com/player/metadata/video/<id> (no key, no
**          cookie): title, duration, mode (vod/live), stream_formats, and
**          qualities.auto[0].url, a signed HLS master playlist. Deleted,
**          private and geo-blocked videos come back with an "error" object,
**          whose message is shown as is.
**       2. GET that master with browser request headers (see
**          kDmMasterHeaders) and list its variants as the quality menu:
**          RESOLUTION, FRAME-RATE, CODECS, BANDWIDTH. Each variant URL is a
**          media playlist the player opens directly: fnetstream.c joins its
**          segments into one stream (TS or fMP4, both seen on Dailymotion:
**          stream_formats says {"380":"fMP4","480":"mpegts"}).
**     Measured on Windows: metadata 0.1-0.5 s, master 0.2 s.
**   - Decodable: H.264 + AAC everywhere Dailymotion serves today. Media
**     Foundation (Windows) and MediaCodec (Android 9+, os_mp4) read both the
**     joined TS and the joined fMP4; FFmpeg opens the playlist itself. Other
**     systems without FFmpeg mark every quality needs_ffmpeg.
**   - yt-dlp stays the fallback: when the native resolve fails (the site
**     changed, a CDN refusal that outlasts the retries) and yt-dlp is
**     installed, the old path runs (fvsrc_ytdlp.c); downloads prefer yt-dlp
**     when it is there and otherwise save the joined segments (.ts/.mp4).
*/
#include "fvsrc_int.h"
#include "fhls.h"
#include "fnetstream.h"
#include "fplat.h"
#include "fsdl.h"

void vsrc_dailymotion_search_url(const FmVsrcConf *c, const char *query, int page, char *out, size_t cap) {
  char q[256], qe[800];
  fm_strlcpy(q, query ? query : "", sizeof q);
  net_urlencode(q, qe, sizeof qe);
  fm_snprintf(out, cap,
              "https://api.dailymotion.com/videos?search=%s&fields=id,title,duration,views_total,"
              "thumbnail_360_url,owner.screenname,url,created_time,onair&limit=%d&page=%d&sort=relevance"
              "&family_filter=%s",
              qe, VSRC_PAGE_SIZE, page < 1 ? 1 : page, c->safe_search ? "true" : "false");
}

FmErr vsrc_dailymotion_parse_search(const char *json, size_t len, int page, FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Dailymotion sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *list = json_get(root, "list");
  if (!list || list->type != JSON_ARR) {
    const char *m = json_str(json_path(root, "error.message"), "");
    if (*m) fm_snprintf(out->error, sizeof out->error, "Dailymotion: %.200s", m);
    else fm_strlcpy(out->error, "Dailymotion sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *d = json_first(list); d && seen < VSRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *id = json_str(json_get(d, "id"), "");
    if (!vsrc_id_ok(id, NULL, 32)) continue;
    FmVsrcItem *it = vsrc_page_add(out);
    fm_strlcpy(it->id, id, sizeof it->id);
    fm_strlcpy(it->title, json_str(json_get(d, "title"), id), sizeof it->title);
    fm_strlcpy(it->channel, json_str(json_get(d, "owner.screenname"), ""), sizeof it->channel);
    const char *th = json_str(json_get(d, "thumbnail_360_url"), "");
    if (vsrc_url_ok(th)) fm_strlcpy(it->thumb, th, sizeof it->thumb);
    const char *pg = json_str(json_get(d, "url"), "");
    if (vsrc_url_ok(pg)) fm_strlcpy(it->page, pg, sizeof it->page);
    else fm_snprintf(it->page, sizeof it->page, "https://www.dailymotion.com/video/%s", id);
    double t = json_num(json_get(d, "created_time"), 0);
    if (t > 0 && t < 1e11) vsrc_unix_date((i64)t, it->published, sizeof it->published);
    double du = json_num(json_get(d, "duration"), 0);
    it->duration = du > 0 && du < 1e7 ? du : 0;
    double v = json_num(json_get(d, "views_total"), -1);
    it->views = v >= 0 && v < 9e15 ? (i64)v : -1;
    it->live = json_bool(json_get(d, "onair"), false);
  }
  if (json_bool(json_get(root, "has_more"), false) && seen > 0 && page < 40)
    fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr dm_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = page_token && *page_token ? atoi(page_token) : 1;
  page = FM_CLAMP(page, 1, 40);
  char url[1400];
  vsrc_dailymotion_search_url(c, query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  /* error replies are JSON too; the parser turns them into text */
  e = vsrc_dailymotion_parse_search((const char *)r.data, r.len, page, out);
  if (e != FM_OK && !out->error[0]) vsrc_http_error("Dailymotion", r.status, out->error, sizeof out->error);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return e;
}

/* ---- native resolve: player metadata + HLS ------------------------------------------ */

/* The master playlist host (cdndirector) refuses requests that do not look
** like a browser (403, X-Error-Code E005). Measured 2026-10-09:
**   - no browser User-Agent: every try refused; the User-Agent alone: about
**     6 in 10 passed; the headers a page's player sends (a CORS fetch from
**     www.dailymotion.com): 20 of 20, but only with Host as the FIRST
**     header. WinHTTP puts its own Host after every other header (all
**     refused, same TLS stack as curl.exe, which passed); an explicit Host
**     line first in our headers moves it up (WinHTTP sorts the headers it
**     knows: Host lands before Referer and User-Agent). libcurl and
**     Android's HttpURLConnection send custom headers in our order.
** The variant playlists and segments it points to (vod*.dmcdn.net, signed
** URLs) need no headers, so the player gets plain URLs. */
static const char kDmMasterHeaders[] =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/147.0.0.0 Safari/537.36\r\n"
    "Accept: */*\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n"
    "Origin: https://www.dailymotion.com\r\n"
    "Referer: https://www.dailymotion.com/\r\n"
    "Sec-Fetch-Mode: cors\r\n"
    "Sec-Fetch-Site: same-site\r\n"
    "Sec-Fetch-Dest: empty\r\n";

static void dm_master_headers(const char *master, char *out, size_t cap) {
  const char *h = strstr(master, "://");
  h = h ? h + 3 : master;
  fm_snprintf(out, cap, "Host: %.*s\r\n%s", (int)strcspn(h, "/?#"), h, kDmMasterHeaders);
}

FmErr vsrc_dailymotion_parse_meta(const char *json, size_t len, FmDmMeta *m, char *err, size_t errcap) {
  memset(m, 0, sizeof *m);
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(err, "Dailymotion sent a reply this app does not understand", errcap);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *er = json_get(root, "error");
  if (er && er->type == JSON_OBJ) {                /* deleted, private, geo-blocked ... */
    const char *t = json_str(json_get(er, "message"), json_str(json_get(er, "title"), ""));
    fm_snprintf(err, errcap, "Dailymotion: %.200s", *t ? t : "this video cannot be played");
    json_free(&j);
    return FM_ERR_NOT_FOUND;
  }
  const char *u = json_str(json_path(root, "qualities.auto.0.url"), "");
  if (!vsrc_url_ok(u) || strlen(u) >= sizeof m->master) {
    fm_strlcpy(err, "Dailymotion lists no stream for this video", errcap);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  fm_strlcpy(m->master, u, sizeof m->master);
  double d = json_num(json_get(root, "duration"), 0);
  m->duration = d > 0 && d < 1e7 ? d : 0;
  const char *mode = json_str(json_get(root, "mode"), "");
  m->live = !strcmp(mode, "live") || !strcmp(json_str(json_get(root, "stream_type"), ""), "live");
  json_free(&j);
  return FM_OK;
}

/* "avc1.64001f,mp4a.40.2" -> the picture's codec name and what decodes it */
static void dm_codecs(const char *codecs, char *name, size_t cap, bool *h264, bool *aac) {
  *h264 = !codecs[0] || strstr(codecs, "avc1") || strstr(codecs, "avc3");   /* unlisted: Dailymotion is H.264 */
  *aac = !codecs[0] || strstr(codecs, "mp4a") || !strstr(codecs, ",");     /* AAC, or no sound listed */
  const char *n = *h264 ? "H.264"
                  : (strstr(codecs, "hvc1") || strstr(codecs, "hev1")) ? "H.265"
                  : strstr(codecs, "av01") ? "AV1"
                  : strstr(codecs, "vp09") ? "VP9" : "";
  fm_strlcpy(name, n, cap);
}

FmErr vsrc_dailymotion_pick(const char *m3u8, size_t len, const char *base, const FmDmMeta *m,
                            const FmVsrcConf *c, FmVsrcStream *out) {
  out->nq = 0;
  out->cur = -1;
  FmHlsMaster *ms = (FmHlsMaster *)fm_alloc(sizeof *ms);
  if (hls_parse_master(m3u8, len, base, ms, NULL, 0) != FM_OK) {
    fm_free(ms);
    return FM_ERR_FORMAT;
  }
  /* tallest first; one entry per picture size (the more bits win) */
  int order[HLS_MAX_VARIANTS], n = 0;
  for (int i = 0; i < ms->n; i++) {
    const FmHlsVariant *v = &ms->v[i];
    if (strlen(v->uri) >= VSRC_QURL || (v->audio_uri && strlen(v->audio_uri) >= sizeof out->audio)) continue;
    int h = v->width > 0 && v->width < v->height ? v->width : v->height;
    int at = n;
    bool dup = false;
    for (int k = 0; k < n; k++) {
      const FmHlsVariant *o = &ms->v[order[k]];
      int oh = o->width > 0 && o->width < o->height ? o->width : o->height;
      if (oh == h && o->fps == v->fps && !strcmp(o->codecs, v->codecs)) {
        if (v->bandwidth > o->bandwidth) order[k] = i;
        dup = true;
        break;
      }
      if (h > oh && at == n) at = k;
    }
    if (dup) continue;
    memmove(order + at + 1, order + at, sizeof(int) * (size_t)(n - at));
    order[at] = i;
    n++;
  }
  for (int k = 0; k < n && out->nq < VSRC_QMAX; k++) {
    const FmHlsVariant *v = &ms->v[order[k]];
    FmVsrcQuality *q = &out->q[out->nq++];
    memset(q, 0, sizeof *q);
    bool h264, aac;
    dm_codecs(v->codecs, q->codec, sizeof q->codec, &h264, &aac);
    q->height = v->width > 0 && v->width < v->height ? v->width : v->height;
    q->fps = v->fps > 0 ? (int)(v->fps + 0.5) : 0;
    if (q->height > 0) fm_snprintf(q->label, sizeof q->label, q->fps > 30 ? "%dp%d" : "%dp", q->height, q->fps);
    else fm_snprintf(q->label, sizeof q->label, "%d kbps", (v->avg_bandwidth ? v->avg_bandwidth : v->bandwidth) / 1000);
    q->kbps = (v->avg_bandwidth ? v->avg_bandwidth : v->bandwidth) / 1000;
    q->bytes = m->duration > 0 && q->kbps > 0 ? (i64)(q->kbps * 125.0 * m->duration) : 0;
    /* the stream reader joins TS or fMP4 segments into one file; Media
    ** Foundation and MediaCodec read both (H.264 + AAC), FFmpeg anything */
    q->playable = c->have_ffmpeg_libs || (c->os_mp4 && h264 && aac);
    q->needs_ffmpeg = !q->playable;
    q->muxed = !v->audio_uri;
    fm_strlcpy(q->url, v->uri, sizeof q->url);
  }
  /* what plays: the tallest that fits max_height, else the smallest */
  int cur = -1;
  for (int i = 0; i < out->nq; i++)
    if (out->q[i].playable && (out->q[i].height <= c->max_height || out->q[i].height <= 0)) { cur = i; break; }
  for (int i = out->nq - 1; cur < 0 && i >= 0; i--)
    if (out->q[i].playable) cur = i;
  if (cur >= 0) {
    const FmVsrcQuality *q = &out->q[cur];
    const FmHlsVariant *v = NULL;
    for (int i = 0; i < ms->n && !v; i++)
      if (!strcmp(ms->v[i].uri, q->url)) v = &ms->v[i];
    out->cur = cur;
    fm_strlcpy(out->video, q->url, sizeof out->video);
    out->audio[0] = 0;
    if (v && v->audio_uri) fm_strlcpy(out->audio, v->audio_uri, sizeof out->audio);
    out->width = v ? v->width : 0;
    out->height = v ? v->height : 0;
  }
  out->duration = m->duration;
  out->local = false;
  out->headers[0] = 0;
  hls_master_free(ms);
  fm_free(ms);
  return !out->nq ? FM_ERR_FORMAT : cur >= 0 ? FM_OK : FM_ERR_UNSUPPORTED;
}

/* The id of an item: its id, or the one in a /video/<id> page URL. */
static bool dm_id(const FmVsrcItem *item, char *out, size_t cap) {
  const char *id = item->id;
  char tmp[64];
  if (!id[0]) {
    const char *p = strstr(item->page, "/video/");
    if (!p) return false;
    p += 7;
    size_t n = strcspn(p, "?#/&");
    if (n >= sizeof tmp) return false;
    memcpy(tmp, p, n);
    tmp[n] = 0;
    id = tmp;
  }
  /* "x" and base 36: x8j6xqc, x51xwf */
  if (id[0] != 'x' || strlen(id) < 4 || !vsrc_id_ok(id, NULL, 31)) return false;
  fm_strlcpy(out, id, cap);
  return true;
}

/* metadata -> master playlist -> qualities, without helper programs */
static FmErr dm_native(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, char *err, size_t errcap,
                       volatile int *cancel) {
  char id[32], url[256];
  if (!dm_id(item, id, sizeof id)) {
    fm_strlcpy(err, "Not a Dailymotion video address", errcap);
    return FM_ERR_NOT_FOUND;
  }
  fm_snprintf(url, sizeof url, "https://www.dailymotion.com/player/metadata/video/%s", id);
  FmDmMeta *m = (FmDmMeta *)fm_alloc(sizeof *m);
  FmErr e = FM_ERR_IO;
  /* the master URL is signed per request and the CDN still refuses one
  ** now and then (403): ask again with a fresh signature */
  for (int attempt = 0; attempt < 4; attempt++) {
    FmNetResp r;
    e = vsrc_http_get(url, &r, err, errcap, cancel);
    if (e != FM_OK) break;
    e = vsrc_dailymotion_parse_meta((const char *)r.data, r.len, m, err, errcap);
    if (e != FM_OK && !err[0]) vsrc_http_error("Dailymotion", r.status, err, errcap);
    net_resp_free(&r);
    if (e != FM_OK) break;
    memset(&r, 0, sizeof r);
    char hdr[700];
    dm_master_headers(m->master, hdr, sizeof hdr);
    e = net_get(m->master, hdr, VSRC_MAX_REPLY, &r, cancel);
    if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); break; }
    if (e != FM_OK) {
      fm_snprintf(err, errcap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
      net_resp_free(&r);
      break;
    }
    if (r.status == 403 || r.status == 429 || r.status >= 500) {
      vsrc_http_error("Dailymotion", r.status, err, errcap);
      net_resp_free(&r);
      e = FM_ERR_ACCESS;
      if (attempt < 3) SDL_Delay(150u << attempt);
      continue;
    }
    e = r.status == 200 ? vsrc_dailymotion_pick((const char *)r.data, r.len, m->master, m, c, out) : FM_ERR_FORMAT;
    if (e == FM_ERR_FORMAT) vsrc_http_error("Dailymotion", r.status == 200 ? 0 : r.status, err, errcap);
    if (e == FM_ERR_UNSUPPORTED)
      fm_strlcpy(err, c->have_ffmpeg_libs ? "None of this video's streams can be decoded here"
                                          : "This video (H.264 in HLS) needs the FFmpeg libraries on this system",
                 errcap);
    net_resp_free(&r);
    break;
  }
  fm_free(m);
  return e;
}

/* Saves the HLS stream into dir as one .ts or .mp4 (the joined segments). */
static FmErr dm_save_hls(const FmVsrcItem *item, const FmVsrcStream *st, const char *dir, char *out_path,
                         size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  FmNetStream *ns = ns_open(st->video, NULL, err, errcap);
  if (!ns) return FM_ERR_IO;
  if (ns_live(ns)) {
    ns_close(ns);
    fm_strlcpy(err, "A live stream cannot be saved", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  char title[160], base[256], name[300], dest[FM_PATH_MAX], part[FM_PATH_MAX + 8];
  vsrc_safe_name(item->title[0] ? item->title : "video", title, 151);
  fm_snprintf(base, sizeof base, "%s [%s]", title, item->id);
  const char *ext = strstr(ns_content_type(ns), "mp4") ? ".mp4" : ".ts";
  for (int i = 1; i < 100; i++) {
    if (i == 1) fm_snprintf(name, sizeof name, "%s%s", base, ext);
    else fm_snprintf(name, sizeof name, "%s (%d)%s", base, i, ext);
    fm_path_join(dest, sizeof dest, dir, name);
    if (!plat_exists(dest)) break;
  }
  fm_snprintf(part, sizeof part, "%s.part", dest);
  FILE *f = fm_fopen(part, "wb");
  if (!f) {
    ns_close(ns);
    fm_snprintf(err, errcap, "Cannot write into %s", dir);
    return FM_ERR_IO;
  }
  i64 want = st->cur >= 0 ? st->q[st->cur].bytes : 0, done = 0;
  u8 *buf = (u8 *)fm_alloc(65536);
  size_t n;
  FmErr e = FM_OK;
  u64 shown = 0;
  while ((n = ns_read(ns, buf, 65536)) > 0) {
    if ((cancel && *cancel) || fwrite(buf, 1, n, f) != n) { e = cancel && *cancel ? FM_ERR_CANCEL : FM_ERR_IO; break; }
    done += (i64)n;
    if (cb && plat_now_ms() - shown > 250) {
      char line[96], sz[32];
      shown = plat_now_ms();
      float frac = want > 0 ? FM_MIN(0.99f, (float)done / (float)want) : -1.0f;
      fm_snprintf(line, sizeof line, "Downloading %s", fm_fmt_size((u64)done, sz, sizeof sz));
      if (!cb(user, frac, line)) { e = FM_ERR_CANCEL; break; }
    }
  }
  if (e == FM_OK && (ns_size(ns) < 0 || done < ns_size(ns))) e = FM_ERR_IO;   /* ended early */
  fm_free(buf);
  ns_close(ns);
  if (fclose(f) != 0 && e == FM_OK) e = FM_ERR_IO;
  if (e == FM_OK) e = plat_rename(part, dest);
  if (e != FM_OK) {
    plat_remove_file(part);
    if (e == FM_ERR_CANCEL) fm_strlcpy(err, "Cancelled", errcap);
    else fm_snprintf(err, errcap, "Download failed: %s", fm_err_str(e));
    return e;
  }
  fm_strlcpy(out_path, dest, cap);
  if (cb) cb(user, 1.0f, "Saved");
  return FM_OK;
}

static FmErr dm_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  bool ytdlp = c->ytdlp[0] != 0;
  if (c->force_cache && ytdlp)               /* the player asked for the cache: only yt-dlp fills it */
    return vsrc_ytdlp_resolve("dailymotion", c, item, out, cb, user, err, errcap, cancel);
  u64 t0 = plat_now_ms();
  FmErr e = dm_native(c, item, out, err, errcap, cancel);
  fm_log("dailymotion: native resolve %s in %d ms: %s", fm_err_str(e), (int)(plat_now_ms() - t0),
         e == FM_OK ? out->q[out->cur].label : err);
  if (e == FM_OK || e == FM_ERR_CANCEL || !ytdlp) return e;
  if (e == FM_ERR_NOT_FOUND && strstr(err, "Dailymotion: ")) return e;   /* the site said why: final */
  err[0] = 0;
  return vsrc_ytdlp_resolve("dailymotion", c, item, out, cb, user, err, errcap, cancel);
}

static FmErr dm_download(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path,
                         size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                         volatile int *cancel) {
  if (c->ytdlp[0])                           /* merges into a plain MP4 */
    return vsrc_ytdlp_download("dailymotion", c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
  if (cap) out_path[0] = 0;
  const char *to = dir && *dir ? dir : c->download_dir;
  if (plat_mkdirs(to) != FM_OK && !plat_is_dir(to)) {
    fm_snprintf(err, errcap, "Cannot create the folder %s", to);
    return FM_ERR_IO;
  }
  FmVsrcConf c2 = *c;
  c2.max_height = FM_MAX(c->max_height, 1080);
  c2.have_ffmpeg_libs = true;                /* saving needs no decoder */
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = dm_native(&c2, item, st, err, errcap, cancel);
  if (e == FM_OK) e = dm_save_hls(item, st, to, out_path, cap, cb, user, err, errcap, cancel);
  fm_free(st);
  return e;
}

const FmVsrc g_vsrc_dailymotion = {
  "dailymotion", "Dailymotion", IC_TV, VSRC_SEARCH | VSRC_DIRECT,
  dm_search, dm_resolve, dm_download, NULL,
  "Free search, no key needed \xC2\xB7 streams without helpers",
};
