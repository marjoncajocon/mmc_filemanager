/* fvsrc_dailymotion.c -- Dailymotion: keyless search with the public Data
** API, play and download through yt-dlp.
**
** Design decisions:
**   - api.dailymotion.com/videos answers without a key for read-only search;
**     `fields` keeps each reply to the eight values the gallery shows (plus
**     "onair" for the LIVE badge). The key "owner.screenname" contains a
**     dot, so it is read with json_get, not json_path.
**   - Streams are HLS only, which yt-dlp fetches natively (no ffmpeg); see
**     fvsrc_ytdlp.c for the cache and format choice.
*/
#include "fvsrc_int.h"

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

static FmErr dm_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  return vsrc_ytdlp_resolve("dailymotion", c, item, out, cb, user, err, errcap, cancel);
}

static FmErr dm_download(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path,
                         size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                         volatile int *cancel) {
  return vsrc_ytdlp_download("dailymotion", c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
}

const FmVsrc g_vsrc_dailymotion = {
  "dailymotion", "Dailymotion", IC_TV, VSRC_SEARCH | VSRC_YTDLP,
  dm_search, dm_resolve, dm_download, NULL,
  "Free search, no key needed \xC2\xB7 plays through yt-dlp",
};
