/* fvsrc_eporner.c -- Eporner (adult videos): its public search API plus
** the player's own stream request. No key, no helper program.
**
** Adult content: the source refuses to search or play while "Safe search"
** (Online videos settings, on by default) is on, so it is an explicit choice.
**
** Design decisions:
**   - Search is the documented API: /api/v2/video/search/?query=&page=&per_page=
**     (JSON; numbers come as strings). An empty query lists the week's top.
**   - Playing: the video page holds a 32-hex "hash"; the player asks
**     /xhr/video/<id>?hash=<the hash's four 8-digit hex parts in base 36>
**     and gets plain progressive MP4 files (240p..1080p, H.264 + AAC) that
**     take byte ranges with no cookie or referer: they stream like any MP4.
**     The formats go through vsrc_ytdlp_pick_stream, so the quality menu
**     and the preferred height work as for the other sites.
**   - A pasted eporner.com video link searches to that one video.
*/
#include "fvsrc_int.h"

#define EP_SITE "https://www.eporner.com"

static bool ep_allowed(const FmVsrcConf *c, char *err, size_t cap) {
  if (!c->safe_search) return true;
  fm_strlcpy(err, "Adult content: turn off Safe search in the Online video settings to use Eporner", cap);
  return false;
}

/* The id in a pasted link: /video-<id>/..., /embed/<id>/, or a bare id. */
bool vsrc_eporner_id(const char *s, char *out, size_t cap) {
  if (!s) return false;
  const char *p = strstr(s, "eporner.com/");
  if (p) {
    const char *v = strstr(p, "/video-");
    const char *e = strstr(p, "/embed/");
    if (v) p = v + 7;
    else if (e) p = e + 7;
    else return false;
  } else {
    p = s;
  }
  size_t n = strcspn(p, "/?#&");
  if (n < 6 || n >= cap || n > 20) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return vsrc_id_ok(out, "", 20);
}

static double num_str(const FmJsonNode *n) {
  const char *s = json_str(n, NULL);
  return s ? atof(s) : json_num(n, 0);
}

/* ---- search ------------------------------------------------------------------- */

FmErr vsrc_eporner_parse_search(const char *json, size_t len, int page, FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Eporner sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *vids = json_get(root, "videos");
  if (!vids || vids->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "Eporner sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *d = json_first(vids); d && seen < VSRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *id = json_str(json_get(d, "id"), "");
    const char *page_url = json_str(json_get(d, "url"), "");
    if (!vsrc_id_ok(id, "", 20) || !vsrc_url_ok(page_url)) continue;
    FmVsrcItem *it = vsrc_page_add(out);
    fm_strlcpy(it->id, id, sizeof it->id);
    vsrc_html_unescape(json_str(json_get(d, "title"), id), it->title, sizeof it->title);
    fm_strlcpy(it->channel, "Eporner", sizeof it->channel);
    const char *th = json_str(json_path(d, "default_thumb.src"), "");
    if (vsrc_url_ok(th)) fm_strlcpy(it->thumb, th, sizeof it->thumb);
    fm_strlcpy(it->page, page_url, sizeof it->page);
    const char *added = json_str(json_get(d, "added"), "");   /* "2025-08-14 13:00:18" */
    if (strlen(added) >= 10) {
      memcpy(it->published, added, 10);
      it->published[10] = 0;
    }
    double du = num_str(json_get(d, "length_sec"));
    it->duration = du > 0 && du < 1e7 ? du : 0;
    double v = num_str(json_get(d, "views"));
    it->views = v > 0 && v < 9e15 ? (i64)v : -1;
  }
  int pages = (int)num_str(json_get(root, "total_pages"));
  if (seen > 0 && page < pages && page < 200) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr ep_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!ep_allowed(c, out->error, sizeof out->error)) return FM_ERR_UNSUPPORTED;
  char id[32];
  if (query && strstr(query, "eporner.com/") && vsrc_eporner_id(query, id, sizeof id)) {
    FmVsrcItem *it = vsrc_page_add(out);                 /* a pasted link: that video */
    fm_strlcpy(it->id, id, sizeof it->id);
    fm_strlcpy(it->title, "Eporner video", sizeof it->title);
    fm_strlcpy(it->channel, "Eporner", sizeof it->channel);
    fm_snprintf(it->page, sizeof it->page, EP_SITE "/video-%s/", id);
    return FM_OK;
  }
  int page = page_token && *page_token ? atoi(page_token) : 1;
  page = FM_CLAMP(page, 1, 200);
  char q[256], qe[800], url[1200];
  fm_strlcpy(q, query ? query : "", sizeof q);
  net_urlencode(q, qe, sizeof qe);
  fm_snprintf(url, sizeof url,
              EP_SITE "/api/v2/video/search/?query=%s&per_page=%d&page=%d&thumbsize=big&order=%s&gay=0&lq=1"
              "&format=json",
              qe[0] ? qe : "all", VSRC_PAGE_SIZE, page, qe[0] ? "most-popular" : "top-weekly");
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    vsrc_http_error("Eporner", r.status, out->error, sizeof out->error);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_eporner_parse_search((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return e;
}

/* ---- resolve ------------------------------------------------------------------- */

/* The xhr hash: each 8 hex digits of the page's 32, written in base 36. */
bool vsrc_eporner_hash(const char *hex32, char *out, size_t cap) {
  static const char kD[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  size_t o = 0;
  for (int part = 0; part < 4; part++) {
    u32 v = 0;
    for (int k = 0; k < 8; k++) {
      char ch = hex32[part * 8 + k];
      int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
      if (d < 0) return false;
      v = v << 4 | (u32)d;
    }
    char tmp[8];
    int n = 0;
    do { tmp[n++] = kD[v % 36]; v /= 36; } while (v);
    if (o + (size_t)n + 1 > cap) return false;
    while (n) out[o++] = tmp[--n];
  }
  out[o] = 0;
  return true;
}

/* The page's player hash: `hash: '<32 hex>'` (or hash = "..."). */
static bool page_hash(const char *html, size_t len, char out[33]) {
  for (size_t i = 0; i + 4 + 40 < len; i++) {
    if (memcmp(html + i, "hash", 4) != 0) continue;
    const char *q = html + i + 4;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != ':' && *q != '=') continue;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '\'' && *q != '"') continue;
    q++;
    int n = 0;
    while (n < 32 && ((q[n] >= '0' && q[n] <= '9') || (q[n] >= 'a' && q[n] <= 'f'))) n++;
    if (n == 32 && (q[32] == '\'' || q[32] == '"')) {
      memcpy(out, q, 32);
      out[32] = 0;
      return true;
    }
  }
  return false;
}

/* The xhr reply's sources.mp4 as yt-dlp-shaped formats for the shared picker. */
FmErr vsrc_eporner_pick(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                        size_t errcap) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(err, "Eporner sent a reply this app does not understand", errcap);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  if (!json_bool(json_get(root, "available"), true)) {
    const char *m = json_str(json_get(root, "message"), "");
    fm_snprintf(err, errcap, "Eporner: %s", m[0] ? m : "this video is not available");
    json_free(&j);
    return FM_ERR_NOT_FOUND;
  }
  double dur = num_str(json_get(root, "duration"));
  size_t cap = 4096, n = 0;
  char *b = (char *)fm_alloc(cap);
  n = (size_t)fm_snprintf(b, cap, "{\"duration\":%.0f,\"formats\":[", dur > 0 && dur < 1e7 ? dur : 0);
  int got = 0;
  for (const FmJsonNode *f = json_first(json_path(root, "sources.mp4")); f; f = json_next(f)) {
    const char *src = json_str(json_get(f, "src"), "");
    int h = f->key ? atoi(f->key) : 0;                    /* "1080p HD", "720p HD", "480p" ... */
    if (!vsrc_url_ok(src) || h <= 0 || strchr(src, '"') || strchr(src, '\\')) continue;
    size_t need = strlen(src) + 300;
    if (n + need > cap) {
      cap = (n + need) * 2;
      b = (char *)fm_realloc(b, cap);
    }
    n += (size_t)fm_snprintf(b + n, cap - n,
                             "%s{\"url\":\"%s\",\"format_id\":\"ep%d\",\"ext\":\"mp4\",\"protocol\":\"https\","
                             "\"vcodec\":\"avc1\",\"acodec\":\"mp4a\",\"width\":%d,\"height\":%d}",
                             got ? "," : "", src, h, h * 16 / 9, h);
    got++;
  }
  json_free(&j);
  if (n + 4 > cap) b = (char *)fm_realloc(b, n + 4);
  memcpy(b + n, "]}", 3);
  n += 2;
  if (!got) {
    fm_free(b);
    fm_strlcpy(err, "Eporner listed no MP4 file for this video", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  FmErr e = vsrc_ytdlp_pick_stream(b, n, c, out);
  fm_free(b);
  if (e == FM_ERR_UNSUPPORTED)
    fm_strlcpy(err, "Eporner's MP4 (H.264) needs the system video decoders or FFmpeg on this computer", errcap);
  else if (e != FM_OK)
    fm_strlcpy(err, "Eporner sent a reply this app does not understand", errcap);
  return e;
}

static FmErr ep_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->cur = -1;
  if (!ep_allowed(c, err, errcap)) return FM_ERR_UNSUPPORTED;
  char id[32];
  if (!vsrc_eporner_id(item->id[0] ? item->id : item->page, id, sizeof id) &&
      !vsrc_eporner_id(item->page, id, sizeof id)) {
    fm_strlcpy(err, "Not an Eporner video", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (cb && !cb(user, -1.0f, "Finding the video\xE2\x80\xA6")) {
    fm_strlcpy(err, "Cancelled", errcap);
    return FM_ERR_CANCEL;
  }
  char url[512];
  fm_snprintf(url, sizeof url, EP_SITE "/video-%s/", id);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, err, errcap, cancel);
  if (e != FM_OK) return e;
  char hex[33];
  bool ok = r.status == 200 && r.data && page_hash((const char *)r.data, r.len, hex);
  int st = r.status;
  net_resp_free(&r);
  if (!ok) {
    if (st == 404 || st == 410) fm_strlcpy(err, "This video was removed from Eporner", errcap);
    else if (st != 200) vsrc_http_error("Eporner", st, err, errcap);
    else fm_strlcpy(err, "Eporner changed its video page (no player hash found)", errcap);
    return st == 404 || st == 410 ? FM_ERR_NOT_FOUND : FM_ERR_FORMAT;
  }
  char h36[64];
  if (!vsrc_eporner_hash(hex, h36, sizeof h36)) {
    fm_strlcpy(err, "Eporner changed its video page (bad player hash)", errcap);
    return FM_ERR_FORMAT;
  }
  fm_snprintf(url, sizeof url, EP_SITE "/xhr/video/%s?hash=%s&device=generic&domain=www.eporner.com&fallback=false",
              id, h36);
  e = vsrc_http_get(url, &r, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    vsrc_http_error("Eporner", r.status, err, errcap);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_eporner_pick((const char *)r.data, r.len, c, out, err, errcap);
  net_resp_free(&r);
  return e;
}

const FmVsrc g_vsrc_eporner = {
  "eporner", "Eporner", IC_VIDEO, VSRC_SEARCH | VSRC_DIRECT,
  ep_search, ep_resolve, NULL, NULL,
  "Adult (18+) \xC2\xB7 free, no key needed \xC2\xB7 needs Safe search off",
};
