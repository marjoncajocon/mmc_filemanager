/* fvsrc_bilibili.c -- Bilibili: keyless search and play through the web API
** the site itself uses (there is no public API: open.bilibili.com is for
** partners). No helper programs; works wherever fnet does.
**
** Design decisions (measured 2026-10-09 from the Philippines):
**   - Requests without the site's visitor cookie are refused with HTTP 412.
**     /x/frontend/finger/spi hands out buvid3/buvid4 without loading the
**     2 MB home page; they are kept for the process and fetched again when
**     a 412 or code -412 comes back.
**   - Search, video info and stream links are "WBI" signed: the two file
**     names in /x/web-interface/nav's wbi_img, joined and shuffled by the
**     fixed table kMix, give a 32-character key that changes daily; every
**     call adds wts=<unix time> and w_rid = md5(sorted query + key), with
**     !'()* dropped from values. The key is fetched again each UTC day.
**   - Stream links: /x/player/wbi/playurl with platform=html5&fnval=1 gives
**     one progressive MP4 (H.264 + AAC) per quality, logged out up to 720p
**     (qn 64) plus 360p (qn 16); no Referer needed. That is the one format
**     Media Foundation and MediaCodec both play without FFmpeg (the usual
**     DASH reply is fragmented MP4, which Media Foundation cannot stream).
**     1080p and up need a signed-in account; not offered.
**   - The reply is turned into the format list a yt-dlp -J reply has and
**     handed to vsrc_ytdlp_pick_stream, like YouTube and Dailymotion, so the
**     quality menu behaves the same. Multi-part videos play their first part.
**   - All site state (cookies, key) is a few strings behind one lock.
*/
#include "fvsrc_int.h"
#include "fcrypt.h"
#include "fjson.h"
#include "fnet.h"
#include "fsdl.h"
#include <ctype.h>
#include <time.h>

#define BL_API "https://api.bilibili.com"
#define BL_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
              "Chrome/140.0.0.0 Safari/537.36"

static const u8 kMix[64] = { 46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35, 27, 43, 5,  49, 33, 9,
                             42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,
                             60, 51, 30, 4,  22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52 };

static SDL_SpinLock g_lock;
static char g_cookie[320];   /* "buvid3=...; buvid4=..." */
static char g_key[33];       /* the WBI mixin key */
static i64 g_key_day;        /* unix day it was fetched */

/* ---- signing ------------------------------------------------------------------ */

/* The mixin key from nav's two file names ("https://i0.hdslb.com/bfs/wbi/<stem>.png"). */
void vsrc_bilibili_mixin(const char *img_url, const char *sub_url, char out[33]) {
  char raw[160];
  raw[0] = 0;
  const char *u[2] = { img_url, sub_url };
  for (int i = 0; i < 2; i++) {
    const char *s = strrchr(u[i], '/');
    s = s ? s + 1 : u[i];
    size_t n = strcspn(s, ".");
    size_t have = strlen(raw);
    if (have + n < sizeof raw) { memcpy(raw + have, s, n); raw[have + n] = 0; }
  }
  size_t len = strlen(raw);
  int o = 0;
  for (int i = 0; i < 64 && o < 32; i++)
    if (kMix[i] < len) out[o++] = raw[kMix[i]];
  out[o] = 0;
}

/* "k=v&..." sorted by key with wts added, then &w_rid=..; values percent-encoded. */
void vsrc_bilibili_sign(BlParam *p, int n, i64 wts, const char *key, char *out, size_t cap) {
  BlParam all[12];
  int m = 0;
  for (int i = 0; i < n && m < 11; i++) all[m++] = p[i];
  all[m].k = "wts";
  fm_snprintf(all[m].v, sizeof all[m].v, "%lld", (long long)wts);
  m++;
  for (int a = 1; a < m; a++)                       /* insertion sort by key */
    for (int b = a; b > 0 && strcmp(all[b - 1].k, all[b].k) > 0; b--) {
      BlParam t = all[b];
      all[b] = all[b - 1];
      all[b - 1] = t;
    }
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < m; i++) {
    char clean[256], enc[800];
    size_t c = 0;
    for (const char *s = all[i].v; *s && c + 1 < sizeof clean; s++)
      if (!strchr("!'()*", *s)) clean[c++] = *s;
    clean[c] = 0;
    net_urlencode(clean, enc, sizeof enc);
    o += (size_t)fm_snprintf(out + o, cap - o, "%s%s=%s", i ? "&" : "", all[i].k, enc);
    if (o >= cap) { out[0] = 0; return; }
  }
  char *signed_part = (char *)fm_alloc(o + 40);
  fm_snprintf(signed_part, o + 40, "%s%s", out, key);
  char hex[33];
  md5_hex(signed_part, strlen(signed_part), hex);
  fm_free(signed_part);
  fm_snprintf(out + o, cap - o, "&w_rid=%s", hex);
}

/* ---- requests ------------------------------------------------------------------- */

static void headers(char *out, size_t cap) {
  char ck[sizeof g_cookie];
  SDL_AtomicLock(&g_lock);
  fm_strlcpy(ck, g_cookie, sizeof ck);
  SDL_AtomicUnlock(&g_lock);
  fm_snprintf(out, cap, "User-Agent: " BL_UA "\r\nReferer: https://www.bilibili.com/\r\n%s%s%s", ck[0] ? "Cookie: " : "",
              ck, ck[0] ? "\r\n" : "");
}

/* GET url; a JSON reply parsed into j with code 0, else err says why.
** FM_ERR_ACCESS = refused as a robot (cookies fetched again by the caller). */
static FmErr bl_json(const char *url, FmJson *j, char *err, size_t cap, volatile int *cancel) {
  char hdr[600];
  headers(hdr, sizeof hdr);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, hdr, VSRC_MAX_REPLY, &r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", cap); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, cap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status == 412) {
    net_resp_free(&r);
    fm_strlcpy(err, "Bilibili refused the request (anti-robot check)", cap);
    return FM_ERR_ACCESS;
  }
  if (r.status != 200 || json_parse(j, (const char *)r.data, r.len) != FM_OK) {
    vsrc_http_error("Bilibili", r.status == 200 ? 0 : r.status, err, cap);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  net_resp_free(&r);
  int code = (int)json_num(json_get(json_root(j), "code"), -1);
  if (code != 0) {
    const char *m = json_str(json_get(json_root(j), "message"), "");
    if (code == -412 || code == -352) fm_strlcpy(err, "Bilibili refused the request (anti-robot check)", cap);
    else if (code == -404 || code == 62002) fm_strlcpy(err, "Bilibili: this video does not exist or was removed", cap);
    else fm_snprintf(err, cap, "Bilibili error %d%s%.150s", code, m[0] ? ": " : "", m);
    json_free(j);
    return code == -412 || code == -352 ? FM_ERR_ACCESS : FM_ERR_IO;
  }
  return FM_OK;
}

/* Cookies and today's key; fresh = forget them first (after a refusal). */
static FmErr session(bool fresh, char *key, char *err, size_t cap, volatile int *cancel) {
  i64 day = (i64)time(NULL) / 86400;
  SDL_AtomicLock(&g_lock);
  if (fresh) { g_cookie[0] = 0; g_key[0] = 0; }
  bool need_ck = !g_cookie[0], need_key = !g_key[0] || g_key_day != day;
  fm_strlcpy(key, g_key, 33);
  SDL_AtomicUnlock(&g_lock);
  FmJson j;
  if (need_ck) {
    FmErr e = bl_json(BL_API "/x/frontend/finger/spi", &j, err, cap, cancel);
    if (e != FM_OK) return e;
    const char *b3 = json_str(json_path(json_root(&j), "data.b_3"), "");
    const char *b4 = json_str(json_path(json_root(&j), "data.b_4"), "");
    char b4e[200];
    net_urlencode(b4, b4e, sizeof b4e);
    SDL_AtomicLock(&g_lock);
    if (vsrc_id_ok(b3, "-", 60)) fm_snprintf(g_cookie, sizeof g_cookie, "buvid3=%s; buvid4=%s", b3, b4e);
    SDL_AtomicUnlock(&g_lock);
    json_free(&j);
  }
  if (need_key) {
    /* nav answers code -101 (not signed in) but still carries wbi_img */
    char hdr[600];
    headers(hdr, sizeof hdr);
    FmNetResp r;
    memset(&r, 0, sizeof r);
    FmErr e = net_get(BL_API "/x/web-interface/nav", hdr, VSRC_MAX_REPLY, &r, cancel);
    if (e != FM_OK || r.status != 200 || json_parse(&j, (const char *)r.data, r.len) != FM_OK) {
      if (e == FM_OK) e = FM_ERR_IO;
      vsrc_http_error("Bilibili", r.status, err, cap);
      net_resp_free(&r);
      return e;
    }
    net_resp_free(&r);
    char k[33];
    vsrc_bilibili_mixin(json_str(json_path(json_root(&j), "data.wbi_img.img_url"), ""),
                        json_str(json_path(json_root(&j), "data.wbi_img.sub_url"), ""), k);
    json_free(&j);
    if (strlen(k) != 32) { fm_strlcpy(err, "Bilibili changed its request signing; this app needs an update", cap); return FM_ERR_FORMAT; }
    SDL_AtomicLock(&g_lock);
    fm_strlcpy(g_key, k, sizeof g_key);
    g_key_day = day;
    SDL_AtomicUnlock(&g_lock);
    fm_strlcpy(key, k, 33);
  }
  return FM_OK;
}

/* A signed API call; one retry with a new session when refused as a robot. */
static FmErr signed_call(const char *path, BlParam *p, int n, FmJson *j, char *err, size_t cap, volatile int *cancel) {
  FmErr e = FM_ERR_IO;
  for (int round = 0; round < 2; round++) {
    char key[33], q[2048], url[2300];
    e = session(round > 0, key, err, cap, cancel);
    if (e != FM_OK) return e;
    vsrc_bilibili_sign(p, n, (i64)time(NULL), key, q, sizeof q);
    fm_snprintf(url, sizeof url, BL_API "%s?%s", path, q);
    e = bl_json(url, j, err, cap, cancel);
    if (e != FM_ERR_ACCESS) break;
  }
  return e;
}

/* ---- search ----------------------------------------------------------------------- */

/* "<em class=\"keyword\">cat</em> video &amp; more" -> "cat video & more" */
static void clean_title(const char *s, char *out, size_t cap) {
  char tmp[512];
  size_t o = 0;
  for (; *s && o + 1 < sizeof tmp; s++) {
    if (*s == '<') {
      const char *e = strchr(s, '>');
      if (e) { s = e; continue; }
    }
    tmp[o++] = *s;
  }
  tmp[o] = 0;
  vsrc_html_unescape(tmp, out, cap);
}

/* "//i0.hdslb.com/bfs/archive/x.jpg" -> a 480x270 JPEG of it (hdslb resizes on request) */
static void thumb_url(const char *pic, char *out, size_t cap) {
  out[0] = 0;
  if (!pic || !*pic) return;
  char base[400];
  if (!strncmp(pic, "//", 2)) fm_snprintf(base, sizeof base, "https:%s", pic);
  else if (!strncmp(pic, "http://", 7)) fm_snprintf(base, sizeof base, "https://%s", pic + 7);
  else fm_strlcpy(base, pic, sizeof base);
  if (!vsrc_url_ok(base)) return;
  fm_snprintf(out, cap, "%s@480w_270h_1c.jpg", base);
}

static void parse_search(const FmJsonNode *root, int page, FmVsrcPage *out) {
  const FmJsonNode *data = json_get(root, "data");
  for (const FmJsonNode *it = json_first(json_get(data, "result")); it; it = json_next(it)) {
    const char *bv = json_str(json_get(it, "bvid"), "");
    if (strncmp(bv, "BV", 2) || !vsrc_id_ok(bv, "", 16) || out->count >= VSRC_MAX_ITEMS) continue;
    FmVsrcItem *x = vsrc_page_add(out);
    if (!x) break;
    fm_strlcpy(x->id, bv, sizeof x->id);
    clean_title(json_str(json_get(it, "title"), bv), x->title, sizeof x->title);
    clean_title(json_str(json_get(it, "author"), ""), x->channel, sizeof x->channel);
    thumb_url(json_str(json_get(it, "pic"), ""), x->thumb, sizeof x->thumb);
    fm_snprintf(x->page, sizeof x->page, "https://www.bilibili.com/video/%s", bv);
    x->duration = vsrc_clock_seconds(json_str(json_get(it, "duration"), ""));
    double play = json_num(json_get(it, "play"), -1);
    x->views = play >= 0 ? (i64)play : -1;
    double pub = json_num(json_get(it, "pubdate"), 0);
    if (pub > 0) vsrc_unix_date((i64)pub, x->published, sizeof x->published);
  }
  int pages = (int)json_num(json_get(data, "numPages"), 0);
  if (page < pages && out->count) fm_snprintf(out->next, sizeof out->next, "b:%d", page + 1);
}

FmErr vsrc_bilibili_parse_search(const char *json, size_t len, int page, FmVsrcPage *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  parse_search(json_root(&j), page, out);
  json_free(&j);
  return FM_OK;
}

/* ---- links ------------------------------------------------------------------------- */

/* A BV id from a BV id or a bilibili.com/video/ link (b23.tv short links are
** followed by the caller first). */
bool vsrc_bilibili_id(const char *s, char *out, size_t cap) {
  const char *p = strstr(s, "BV");
  if (!p || (strstr(s, "://") && !strstr(s, "bilibili.com"))) return false;
  size_t n = 0;
  while (p[n] && isalnum((u8)p[n]) && n < 16) n++;
  if (n != 12 || cap < 13) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

/* b23.tv/xxxx -> where it redirects (one request, not followed) */
static bool follow_short(const char *url, char *out, size_t cap, volatile int *cancel) {
  FmNetReq rq;
  memset(&rq, 0, sizeof rq);
  rq.no_redirect = true;
  rq.headers = "User-Agent: " BL_UA "\r\n";
  FmNetResp r;
  bool ok = net_request(url, &rq, &r, cancel) == FM_OK && r.status >= 300 && r.status < 400 &&
            net_resp_header(&r, "Location", out, cap);
  net_resp_free(&r);
  return ok;
}

/* ---- resolve ---------------------------------------------------------------------- */

/* playurl's durl for one quality into the yt-dlp-shaped list b */
static void add_durl(const FmJsonNode *data, int vw, int vh, char **b, size_t *len, size_t *cap, bool *first) {
  int qn = (int)json_num(json_get(data, "quality"), 0);
  const FmJsonNode *d0 = json_first(json_get(data, "durl"));
  const char *url = json_str(json_get(d0, "url"), "");
  if (!vsrc_url_ok(url) || json_next(d0)) return;            /* split files: not offered */
  int short_side = qn >= 80 ? 1080 : qn >= 64 ? 720 : qn >= 32 ? 480 : 360;
  int w = short_side, h = short_side;
  if (vw > 0 && vh > 0) {
    if (vw >= vh) w = short_side * vw / vh;
    else h = short_side * vh / vw;
  }
  double size = json_num(json_get(d0, "size"), 0);
  size_t need = strlen(url) + 400;
  if (*len + need > *cap) {
    *cap = (*len + need) * 2;
    *b = (char *)fm_realloc(*b, *cap);
  }
  /* the URL has no characters JSON must escape (checked by vsrc_url_ok) except possibly \ and " */
  char *esc = (char *)fm_alloc(strlen(url) * 2 + 1);
  size_t o = 0;
  for (const char *s = url; *s; s++) {
    if (*s == '"' || *s == '\\') esc[o++] = '\\';
    esc[o++] = *s;
  }
  esc[o] = 0;
  *len += (size_t)fm_snprintf(*b + *len, *cap - *len,
                              "%s{\"url\":\"%s\",\"format_id\":\"bl%d\",\"ext\":\"mp4\",\"protocol\":\"https\","
                              "\"vcodec\":\"avc1\",\"acodec\":\"mp4a\",\"width\":%d,\"height\":%d,\"filesize\":%.0f}",
                              *first ? "" : ",", esc, qn, w, h, size);
  fm_free(esc);
  *first = false;
}

FmErr vsrc_bilibili_resolve(const FmVsrcConf *c, const char *bv, FmVsrcStream *out, char *err, size_t errcap,
                            volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->cur = -1;
  if (!vsrc_need_net(err, errcap)) return FM_ERR_UNSUPPORTED;
  BlParam p[6];
  memset(p, 0, sizeof p);
  p[0].k = "bvid";
  fm_strlcpy(p[0].v, bv, sizeof p[0].v);
  FmJson j;
  FmErr e = signed_call("/x/web-interface/wbi/view", p, 1, &j, err, errcap, cancel);
  if (e != FM_OK) return e;
  const FmJsonNode *d = json_get(json_root(&j), "data");
  double cid = json_num(json_get(d, "cid"), 0);
  double dur = json_num(json_get(d, "duration"), 0);
  int vw = (int)json_num(json_path(d, "dimension.width"), 0), vh = (int)json_num(json_path(d, "dimension.height"), 0);
  if ((int)json_num(json_path(d, "dimension.rotate"), 0)) { int t = vw; vw = vh; vh = t; }
  json_free(&j);
  if (cid <= 0) { fm_strlcpy(err, "Bilibili: this video has no playable part", errcap); return FM_ERR_FORMAT; }

  size_t len = 0, cap = 2048;
  char *b = (char *)fm_alloc(cap);
  len = (size_t)fm_snprintf(b, cap, "{\"duration\":%.0f,\"formats\":[", dur);
  bool first = true;
  static const int kQn[] = { 64, 16 };                        /* 720p and 360p: what logged-out gets */
  int got = 0;
  for (int i = 0; i < FM_COUNT(kQn); i++) {
    memset(p, 0, sizeof p);
    p[0].k = "bvid";
    fm_strlcpy(p[0].v, bv, sizeof p[0].v);
    p[1].k = "cid";
    fm_snprintf(p[1].v, sizeof p[1].v, "%.0f", cid);
    p[2].k = "qn";
    fm_snprintf(p[2].v, sizeof p[2].v, "%d", kQn[i]);
    p[3].k = "fnval";
    fm_strlcpy(p[3].v, "1", sizeof p[3].v);
    p[4].k = "platform";
    fm_strlcpy(p[4].v, "html5", sizeof p[4].v);
    p[5].k = "high_quality";
    fm_strlcpy(p[5].v, "1", sizeof p[5].v);
    char e2[160];
    if (signed_call("/x/player/wbi/playurl", p, 6, &j, e2, sizeof e2, cancel) != FM_OK) {
      if (cancel && *cancel) { fm_free(b); fm_strlcpy(err, "Cancelled", errcap); return FM_ERR_CANCEL; }
      if (!got) fm_strlcpy(err, e2, errcap);
      continue;
    }
    const FmJsonNode *pd = json_get(json_root(&j), "data");
    if ((int)json_num(json_get(pd, "quality"), 0) == kQn[i] || i == 0) {      /* the 360p ask may return 720p again */
      add_durl(pd, vw, vh, &b, &len, &cap, &first);
      got++;
    }
    json_free(&j);
  }
  if (len + 4 > cap) b = (char *)fm_realloc(b, len + 4);
  memcpy(b + len, "]}", 3);
  len += 2;
  if (!got) { fm_free(b); return FM_ERR_IO; }
  e = vsrc_ytdlp_pick_stream(b, len, c, out);
  fm_free(b);
  if (e == FM_ERR_UNSUPPORTED)
    fm_strlcpy(err, "Bilibili's MP4 (H.264) needs the system video decoders or FFmpeg on this computer", errcap);
  else if (e != FM_OK)
    fm_strlcpy(err, "Bilibili sent a reply this app does not understand", errcap);
  return e;
}

/* ---- the source ---------------------------------------------------------------------- */

static FmErr bl_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!vsrc_need_net(out->error, sizeof out->error)) return FM_ERR_UNSUPPORTED;
  char q[400];
  const char *s = query ? query : "";
  while (*s == ' ') s++;
  fm_strlcpy(q, s, sizeof q);
  size_t n = strlen(q);
  while (n && (u8)q[n - 1] <= ' ') q[--n] = 0;
  if (!n) { fm_strlcpy(out->error, "Type something to search for", sizeof out->error); return FM_ERR_NOT_FOUND; }

  /* a link: that one video */
  char bv[16], loc[1024];
  if (strstr(q, "b23.tv/") && follow_short(strncmp(q, "http", 4) ? "https://b23.tv/" : q, loc, sizeof loc, cancel))
    fm_strlcpy(q, loc, sizeof q);
  if (vsrc_bilibili_id(q, bv, sizeof bv) && (strstr(q, "://") || strlen(q) == 12)) {
    BlParam p[1];
    memset(p, 0, sizeof p);
    p[0].k = "bvid";
    fm_strlcpy(p[0].v, bv, sizeof p[0].v);
    FmJson j;
    FmErr e = signed_call("/x/web-interface/wbi/view", p, 1, &j, out->error, sizeof out->error, cancel);
    if (e != FM_OK) return e;
    const FmJsonNode *d = json_get(json_root(&j), "data");
    FmVsrcItem *x = vsrc_page_add(out);
    if (x) {
      fm_strlcpy(x->id, bv, sizeof x->id);
      clean_title(json_str(json_get(d, "title"), bv), x->title, sizeof x->title);
      clean_title(json_str(json_path(d, "owner.name"), ""), x->channel, sizeof x->channel);
      thumb_url(json_str(json_get(d, "pic"), ""), x->thumb, sizeof x->thumb);
      fm_snprintf(x->page, sizeof x->page, "https://www.bilibili.com/video/%s", bv);
      x->duration = json_num(json_get(d, "duration"), 0);
      x->views = (i64)json_num(json_path(d, "stat.view"), -1);
      double pub = json_num(json_get(d, "pubdate"), 0);
      if (pub > 0) vsrc_unix_date((i64)pub, x->published, sizeof x->published);
    }
    json_free(&j);
    return FM_OK;
  }

  int page = page_token && !strncmp(page_token, "b:", 2) ? atoi(page_token + 2) : 1;
  if (page < 1) page = 1;
  BlParam p[3];
  memset(p, 0, sizeof p);
  p[0].k = "search_type";
  fm_strlcpy(p[0].v, "video", sizeof p[0].v);
  p[1].k = "keyword";
  fm_strlcpy(p[1].v, q, sizeof p[1].v);
  p[2].k = "page";
  fm_snprintf(p[2].v, sizeof p[2].v, "%d", page);
  FmJson j;
  FmErr e = signed_call("/x/web-interface/wbi/search/type", p, 3, &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  parse_search(json_root(&j), page, out);
  json_free(&j);
  if (!out->count && page == 1) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return FM_OK;
}

static FmErr bl_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb, void *user,
                        char *err, size_t errcap, volatile int *cancel) {
  FM_UNUSED(cb);
  FM_UNUSED(user);
  if (errcap) err[0] = 0;
  char bv[16];
  if (!vsrc_bilibili_id(item->id, bv, sizeof bv) && !vsrc_bilibili_id(item->page, bv, sizeof bv)) {
    fm_strlcpy(err, "Not a Bilibili video", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (c->force_cache) {
    fm_strlcpy(err, "Bilibili videos stream; downloading first is not offered", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  return vsrc_bilibili_resolve(c, bv, out, err, errcap, cancel);
}

const FmVsrc g_vsrc_bilibili = {
  "bilibili", "Bilibili", IC_PLAY_BADGE, VSRC_SEARCH | VSRC_DIRECT,
  bl_search, bl_resolve, NULL, NULL,
  "Search and play, no key needed: up to 720p signed out. Paste bilibili.com or b23.tv links too",
};
