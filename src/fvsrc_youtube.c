/* fvsrc_youtube.c -- YouTube: search with the Data API v3 (or yt-dlp without
** a key), play and download through yt-dlp.
**
** Design decisions:
**   - One search page costs 100 quota units of the free 10,000 a day; the
**     follow-up videos?part=contentDetails,statistics call for the same 24
**     ids costs 1 and fills durations and view counts, so it is always made.
**     If it fails the page still shows, just without those two.
**   - No key: yt-dlp's "ytsearch" gives the same results without quota
**     (page tokens "y:<n>", see fvsrc_ytdlp.c). A spent quota also falls
**     back to it when yt-dlp is there, so search keeps working all day.
**     Safe search cannot be asked of yt-dlp; the key path honours it.
**   - Thumbnails are built from the id (mqdefault, 320x180): the API's
**     thumbnail objects add nothing and some are 16:9 letterboxed 4:3.
**   - API titles are HTML-escaped ("&#39;"); they are unescaped here.
**   - A pasted youtube.com link is looked up with yt-dlp instead of the API.
*/
#include "fvsrc_int.h"
#include "fproc.h"

#define YT_API "https://www.googleapis.com/youtube/v3/"

/* ---- URLs and parsing ------------------------------------------------------------ */

void vsrc_youtube_search_url(const FmVsrcConf *c, const char *query, const char *token, char *out,
                             size_t cap) {
  char q[256], qe[800], k[400], t[400], region[32];
  fm_strlcpy(q, query ? query : "", sizeof q);      /* the API rejects very long queries anyway */
  net_urlencode(q, qe, sizeof qe);
  net_urlencode(c->api_key_youtube, k, sizeof k);
  t[0] = region[0] = 0;
  if (token && *token && vsrc_id_ok(token, "-_", 200)) fm_snprintf(t, sizeof t, "&pageToken=%s", token);
  if (vsrc_id_ok(c->region, NULL, 2) && strlen(c->region) == 2)
    fm_snprintf(region, sizeof region, "&regionCode=%s", c->region);
  fm_snprintf(out, cap, YT_API "search?part=snippet&type=video&maxResults=%d&q=%s&key=%s&safeSearch=%s%s%s",
              VSRC_PAGE_SIZE, qe, k, c->safe_search ? "moderate" : "none", region, t);
}

FmErr vsrc_youtube_parse_search(const char *json, size_t len, FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "YouTube sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *items = root && root->type == JSON_OBJ ? json_get(root, "items") : NULL;
  if (!items || items->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "YouTube sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *n = json_first(items); n && seen < VSRC_MAX_ITEMS; n = json_next(n), seen++) {
    const char *id = json_str(json_path(n, "id.videoId"), "");
    if (!vsrc_id_ok(id, "-_", 32)) continue;
    const FmJsonNode *sn = json_get(n, "snippet");
    const char *lbc = json_str(json_get(sn, "liveBroadcastContent"), "none");
    if (!strcmp(lbc, "upcoming")) continue;                  /* nothing to play yet */
    FmVsrcItem *it = vsrc_page_add(out);
    fm_strlcpy(it->id, id, sizeof it->id);
    vsrc_html_unescape(json_str(json_get(sn, "title"), id), it->title, sizeof it->title);
    vsrc_html_unescape(json_str(json_get(sn, "channelTitle"), ""), it->channel, sizeof it->channel);
    vsrc_iso_date(json_str(json_get(sn, "publishedAt"), ""), it->published, sizeof it->published);
    it->live = !strcmp(lbc, "live");
    fm_snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    fm_snprintf(it->page, sizeof it->page, "https://www.youtube.com/watch?v=%s", id);
  }
  const char *next = json_str(json_get(root, "nextPageToken"), "");
  if (vsrc_id_ok(next, "-_", 200)) fm_strlcpy(out->next, next, sizeof out->next);
  json_free(&j);
  return FM_OK;
}

FmErr vsrc_youtube_parse_videos(const char *json, size_t len, FmVsrcPage *p) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *items = root && root->type == JSON_OBJ ? json_get(root, "items") : NULL;
  int seen = 0;
  for (const FmJsonNode *n = json_first(items); n && seen < VSRC_MAX_ITEMS; n = json_next(n), seen++) {
    const char *id = json_str(json_get(n, "id"), "");
    for (int i = 0; i < p->count; i++) {
      if (strcmp(p->items[i].id, id)) continue;
      double d = vsrc_iso_duration(json_str(json_path(n, "contentDetails.duration"), ""));
      if (d > 0) p->items[i].duration = d;
      double v = json_num(json_path(n, "statistics.viewCount"), -1);   /* a numeric string */
      if (v >= 0 && v < 9e15) p->items[i].views = (i64)v;
    }
  }
  json_free(&j);
  return items && items->type == JSON_ARR ? FM_OK : FM_ERR_FORMAT;
}

const char *vsrc_youtube_error(const char *json, size_t len, int status, char *out, size_t cap) {
  FmJson j;
  char reason[64], msg[160], detail[64];
  reason[0] = msg[0] = detail[0] = 0;
  if (json && json_parse(&j, json, len) == FM_OK) {
    const FmJsonNode *e = json_get(json_root(&j), "error");
    fm_strlcpy(reason, json_str(json_path(e, "errors.0.reason"), ""), sizeof reason);
    fm_strlcpy(msg, json_str(json_get(e, "message"), ""), sizeof msg);
    for (const FmJsonNode *d = json_first(json_get(e, "details")); d && !detail[0]; d = json_next(d))
      fm_strlcpy(detail, json_str(json_get(d, "reason"), ""), sizeof detail);
    json_free(&j);
  }
  if (!strcmp(reason, "quotaExceeded") || !strcmp(reason, "dailyLimitExceeded")) {
    fm_strlcpy(out, "Daily YouTube quota used up \xE2\x80\x94 try again tomorrow or use another source", cap);
    return "quota";
  }
  if (!strcmp(reason, "keyInvalid") || !strcmp(detail, "API_KEY_INVALID") ||
      strstr(msg, "API key not valid")) {
    fm_strlcpy(out, "The YouTube API key is not valid \xE2\x80\x94 check it in Settings", cap);
    return "key";
  }
  if (!strcmp(reason, "keyExpired") || !strcmp(detail, "API_KEY_EXPIRED")) {
    fm_strlcpy(out, "The YouTube API key has expired \xE2\x80\x94 renew it in the Google Cloud Console", cap);
    return "key";
  }
  if (!strcmp(reason, "accessNotConfigured") || !strcmp(detail, "SERVICE_DISABLED")) {
    fm_strlcpy(out,
               "YouTube Data API v3 is not enabled for this key \xE2\x80\x94 enable it in the Google Cloud "
               "Console",
               cap);
    return "disabled";
  }
  if (!strcmp(reason, "ipRefererBlocked") || !strncmp(detail, "API_KEY_", 8) ||
      !strcmp(reason, "forbidden")) {
    fm_strlcpy(out, "This API key is restricted and cannot be used by this app \xE2\x80\x94 check its "
               "restrictions in the Google Cloud Console", cap);
    return "restricted";
  }
  if (!strcmp(reason, "rateLimitExceeded") || !strcmp(reason, "userRateLimitExceeded") || status == 429) {
    fm_strlcpy(out, "Too many YouTube requests \xE2\x80\x94 wait a minute and try again", cap);
    return "rate";
  }
  if (!strcmp(reason, "invalidSearchFilter") || !strcmp(reason, "invalidRegionCode")) {
    fm_strlcpy(out, "YouTube did not accept this search", cap);
    return "other";
  }
  if (msg[0]) fm_snprintf(out, cap, "YouTube error %d: %s", status, msg);
  else vsrc_http_error("YouTube", status, out, cap);
  return "other";
}

/* ---- the source -------------------------------------------------------------------- */

static bool ytdlp_usable(const FmVsrcConf *c) { return c->ytdlp[0] && proc_available(); }

static FmErr yt_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  char q[512];
  out->next[0] = out->error[0] = 0;
  const char *s = query ? query : "";
  while (*s == ' ' || *s == '\t') s++;
  fm_strlcpy(q, s, sizeof q);
  size_t n = strlen(q);
  while (n && (u8)q[n - 1] <= ' ') q[--n] = 0;
  if (!n) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }

  bool is_url = !strncmp(q, "https://", 8) || !strncmp(q, "http://", 7);
  bool ytdlp_token = page_token && !strncmp(page_token, "y:", 2);
  bool native_token = page_token && !strncmp(page_token, "i:", 2);
  char vid[16];
  if (is_url && vsrc_innertube_id(q, vid, sizeof vid)) {       /* one video: no helper needed */
    FmVsrcItem it;
    FmErr e = vsrc_innertube_item(vid, &it, out->error, sizeof out->error, cancel);
    if (e == FM_OK) {
      FmVsrcItem *dst = vsrc_page_add(out);
      if (dst) *dst = it;
      out->error[0] = 0;
      return FM_OK;
    }
    if (e == FM_ERR_CANCEL || !ytdlp_usable(c)) return e;
    out->error[0] = 0;
  }
  if (is_url || ytdlp_token) {
    if (ytdlp_usable(c)) return vsrc_ytdlp_search(c, "youtube", is_url ? NULL : "ytsearch", q, page_token, out, cancel);
    fm_strlcpy(out->error, "Opening playlists and channels needs yt-dlp: use \"Get yt-dlp\" in Settings",
               sizeof out->error);
    return proc_available() ? FM_ERR_NOT_FOUND : FM_ERR_UNSUPPORTED;
  }
  if (native_token || !c->api_key_youtube[0]) {
    FmErr e = vsrc_innertube_search(c, q, page_token, out, cancel);
    if (e == FM_OK || e == FM_ERR_CANCEL || native_token || !ytdlp_usable(c)) return e;
    out->error[0] = 0;                                         /* web search failed: yt-dlp */
    return vsrc_ytdlp_search(c, "youtube", "ytsearch", q, NULL, out, cancel);
  }

  char url[2048];
  vsrc_youtube_search_url(c, q, page_token, url, sizeof url);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    const char *kind =
        vsrc_youtube_error((const char *)r.data, r.len, r.status, out->error, sizeof out->error);
    net_resp_free(&r);
    if (!strcmp(kind, "quota") && !(page_token && *page_token)) {
      /* the day's quota is spent: YouTube's web search, then yt-dlp */
      char keep[256];
      fm_strlcpy(keep, out->error, sizeof keep);
      out->error[0] = 0;
      e = vsrc_innertube_search(c, q, NULL, out, cancel);
      if (e != FM_OK && e != FM_ERR_CANCEL && ytdlp_usable(c)) {
        vsrc_page_free(out);
        out->error[0] = 0;
        e = vsrc_ytdlp_search(c, "youtube", "ytsearch", q, NULL, out, cancel);
      }
      if (e == FM_OK) return e;
      if (e != FM_ERR_CANCEL) fm_strlcpy(out->error, keep, sizeof out->error);
      return e;
    }
    return !strcmp(kind, "rate") || !strcmp(kind, "other") ? FM_ERR_IO : FM_ERR_ACCESS;
  }
  e = vsrc_youtube_parse_search((const char *)r.data, r.len, out);
  net_resp_free(&r);
  if (e != FM_OK || !out->count) {
    if (e == FM_OK && !(page_token && *page_token))
      fm_strlcpy(out->error, "No videos found", sizeof out->error);
    return e;
  }
  if (cancel && *cancel) return FM_ERR_CANCEL;

  /* durations and views: 1 unit for the whole page */
  char ids[VSRC_MAX_ITEMS * 36], k[400];
  ids[0] = 0;
  for (int i = 0; i < out->count; i++) {
    if (i) fm_strlcat(ids, "%2C", sizeof ids);
    fm_strlcat(ids, out->items[i].id, sizeof ids);
  }
  net_urlencode(c->api_key_youtube, k, sizeof k);
  fm_snprintf(url, sizeof url, YT_API "videos?part=contentDetails,statistics&id=%s&key=%s", ids, k);
  char ignored[160];
  if (vsrc_http_get(url, &r, ignored, sizeof ignored, cancel) == FM_OK) {
    if (r.status == 200) vsrc_youtube_parse_videos((const char *)r.data, r.len, out);
    net_resp_free(&r);
  }
  if (cancel && *cancel) return FM_ERR_CANCEL;
  return FM_OK;
}

/* The video id of an item (search results carry it; pasted links in page). */
static bool item_id(const FmVsrcItem *item, char *out, size_t cap) {
  return vsrc_innertube_id(item->id, out, cap) || vsrc_innertube_id(item->page, out, cap);
}

/* Built in first (fvsrc_innertube.c); yt-dlp when that fails and it is
** there, and for the cache path (force_cache), which is yt-dlp's job. */
static FmErr yt_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  char vid[16];
  FmErr e = FM_ERR_NOT_FOUND;
  if (errcap) err[0] = 0;
  if (!c->force_cache && item_id(item, vid, sizeof vid)) {
    e = vsrc_innertube_resolve(c, vid, out, err, errcap, cancel);
    /* yt-dlp plays no live stream either: one not on air (NOT_FOUND) or one
    ** listed but not decodable here (live set) keeps this message */
    if (e == FM_OK || e == FM_ERR_CANCEL || e == FM_ERR_NOT_FOUND || out->live) return e;
    fm_log("youtube: built-in resolve failed: %s", err);
  }
  if (ytdlp_usable(c)) {
    if (errcap) err[0] = 0;
    return vsrc_ytdlp_resolve("youtube", c, item, out, cb, user, err, errcap, cancel);
  }
  if (c->force_cache) fm_strlcpy(err, "Downloading before playing needs yt-dlp (Settings)", errcap);
  else if (!err[0]) fm_strlcpy(err, "Not a YouTube video", errcap);
  return e == FM_ERR_NOT_FOUND && c->force_cache ? FM_ERR_UNSUPPORTED : e;
}

/* yt-dlp when it is there (it merges with ffmpeg); else the built-in
** streams saved with vsrc_save_stream (video + sound as two files unless
** ffmpeg is present to merge them). */
static FmErr yt_download(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path,
                         size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                         volatile int *cancel) {
  if (ytdlp_usable(c)) return vsrc_ytdlp_download("youtube", c, item, dir, out_path, cap, cb, user, err, errcap, cancel);
  char vid[16];
  if (cap) out_path[0] = 0;
  if (!item_id(item, vid, sizeof vid)) { fm_strlcpy(err, "Not a YouTube video", errcap); return FM_ERR_NOT_FOUND; }
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmErr e = vsrc_innertube_resolve(c, vid, st, err, errcap, cancel);
  if (e == FM_OK && st->live) {
    fm_strlcpy(err, "A live stream cannot be downloaded", errcap);
    e = FM_ERR_UNSUPPORTED;
  }
  if (e == FM_OK)
    e = vsrc_save_stream(c, item, st, dir && *dir ? dir : c->download_dir, out_path, cap, cb, user, err, errcap,
                         cancel);
  fm_free(st);
  return e;
}

const FmVsrc g_vsrc_youtube = {
  "youtube", "YouTube", IC_PLAY_BADGE, VSRC_SEARCH | VSRC_DIRECT,
  yt_search, yt_resolve, yt_download, NULL,
  "Search and play, no key or helper needed. A free API key (Settings) makes search official",
};
