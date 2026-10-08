/* fvsrc_archive.c -- Internet Archive (archive.org): public-domain films,
** TV and home video. No key, no helper program.
**
** Design decisions:
**   - Search is advancedsearch.php limited to mediatype:movies with the five
**     fields the gallery shows; durations are unknown until resolve (the
**     search index has no runtime field worth trusting). Restricted items
**     (TV news, lending library) are left out: their files answer 401.
**     Results are sorted by downloads: plain relevance puts random uploads
**     that mention the word above the classics people look for.
**   - Resolve reads /metadata/<id> and plays the file straight from
**     archive.org/download (Media Foundation and FFmpeg stream http; opens
**     in ~5 s and seeks). Many items hold several videos (DVD rips with
**     VTS_01_1..3, trailers): the longest one is taken as the main video,
**     then H.264 MP4 beats WebM beats Ogg (Ogg only with the FFmpeg
**     libraries), then the height nearest below the preferred quality.
*/
#include "fvsrc_int.h"

/* ---- search ------------------------------------------------------------------- */

void vsrc_archive_search_url(const char *query, int page, char *out, size_t cap) {
  char q[400], qe[1400];
  fm_snprintf(q, sizeof q, "(%.300s) AND mediatype:movies AND -access-restricted-item:true",
              query ? query : "");
  net_urlencode(q, qe, sizeof qe);
  fm_snprintf(out, cap, "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
              "&fl%%5B%%5D=creator&fl%%5B%%5D=downloads&fl%%5B%%5D=publicdate&sort%%5B%%5D=downloads%%20desc"
              "&rows=%d&page=%d&output=json",
              qe, VSRC_PAGE_SIZE, page < 1 ? 1 : page);
}

FmErr vsrc_archive_parse_search(const char *json, size_t len, int page, FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "archive.org sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *resp = json_get(json_root(&j), "response");
  const FmJsonNode *docs = json_get(resp, "docs");
  if (!docs || docs->type != JSON_ARR) {
    const char *e = json_str(json_get(json_root(&j), "error"), "");
    if (*e) fm_snprintf(out->error, sizeof out->error, "archive.org: %.200s", e);
    else fm_strlcpy(out->error, "archive.org sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *d = json_first(docs); d && seen < VSRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *id = vsrc_jstr(json_get(d, "identifier"), "");
    if (!vsrc_id_ok(id, "-_.", 63)) continue;
    FmVsrcItem *it = vsrc_page_add(out);
    fm_strlcpy(it->id, id, sizeof it->id);
    fm_strlcpy(it->title, vsrc_jstr(json_get(d, "title"), id), sizeof it->title);
    fm_strlcpy(it->channel, vsrc_jstr(json_get(d, "creator"), ""), sizeof it->channel);
    vsrc_iso_date(vsrc_jstr(json_get(d, "publicdate"), ""), it->published, sizeof it->published);
    double v = json_num(json_get(d, "downloads"), -1);
    it->views = v >= 0 && v < 9e15 ? (i64)v : -1;
    fm_snprintf(it->thumb, sizeof it->thumb, "https://archive.org/services/img/%s", id);
    fm_snprintf(it->page, sizeof it->page, "https://archive.org/details/%s", id);
  }
  double found = json_num(json_get(resp, "numFound"), 0);
  if (seen >= VSRC_PAGE_SIZE && (double)page * VSRC_PAGE_SIZE < found && page < 400)
    fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr ia_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = page_token && *page_token ? atoi(page_token) : 1;
  page = FM_CLAMP(page, 1, 400);
  char url[2048];
  vsrc_archive_search_url(query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    vsrc_http_error("archive.org", r.status, out->error, sizeof out->error);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_archive_parse_search((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return e;
}

/* ---- resolve ------------------------------------------------------------------- */

/* 0 = H.264 MP4, 1 = WebM, 2 = Ogg, 3 = other containers; -1 = not playable here */
static int file_rank(const char *name, bool libs) {
  const char *x = fm_path_ext(name);
  if (!fm_stricmp(x, ".mp4") || !fm_stricmp(x, ".m4v")) return 0;
  if (!fm_stricmp(x, ".webm")) return 1;
  if (!libs) return -1;
  if (!fm_stricmp(x, ".ogv")) return 2;
  static const char *const kOther[] = {".mkv", ".mov", ".avi", ".mpeg", ".mpg",
                                       ".flv", ".wmv", ".ts",  ".m2ts"};
  for (int i = 0; i < FM_COUNT(kOther); i++)
    if (!fm_stricmp(x, kOther[i])) return 3;
  return -1;
}

/* archive.org/download/<id>/<name>, each path segment percent-encoded */
static void download_url(const char *id, const char *name, char *out, size_t cap) {
  char seg[512], enc[1536];
  fm_snprintf(out, cap, "https://archive.org/download/%s/", id);
  const char *s = name;
  while (*s) {
    size_t n = strcspn(s, "/");
    if (n >= sizeof seg) n = sizeof seg - 1;
    memcpy(seg, s, n);
    seg[n] = 0;
    net_urlencode(seg, enc, sizeof enc);
    fm_strlcat(out, enc, cap);
    s += strcspn(s, "/");
    if (*s == '/') { fm_strlcat(out, "/", cap); s++; }
  }
}

FmErr vsrc_archive_pick(const char *json, size_t len, const char *id, const FmVsrcConf *c, FmVsrcStream *out,
                        char *err, size_t errcap) {
  FmJson j;
  memset(out, 0, sizeof *out);
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(err, "archive.org sent a reply this app does not understand", errcap);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *files = json_get(root, "files");
  if (!strcmp(vsrc_jstr(json_path(root, "metadata.access-restricted-item"), ""), "true")) {
    json_free(&j);
    fm_strlcpy(err, "This item is restricted on archive.org (lending or TV news); open it in the browser",
               errcap);
    return FM_ERR_ACCESS;
  }
  if (!files || files->type != JSON_ARR || json_bool(json_get(root, "is_dark"), false)) {
    json_free(&j);
    fm_strlcpy(err, "This item is not available on archive.org any more", errcap);
    return FM_ERR_NOT_FOUND;
  }
  int H = c->max_height > 0 ? c->max_height : 720;
  double maxlen = 0;
  bool ogg_only = false;
  int seen = 0;
  for (const FmJsonNode *f = json_first(files); f && seen < 4000; f = json_next(f), seen++) {
    const char *name = json_str(json_get(f, "name"), "");
    if (file_rank(name, c->have_ffmpeg_libs) < 0) {
      if (fm_ends_with_i(name, ".ogv")) ogg_only = true;
      continue;
    }
    double l = vsrc_clock_seconds(json_str(json_get(f, "length"), ""));
    if (l > maxlen) maxlen = l;
  }
  const FmJsonNode *best = NULL;
  double best_score = 1e18;
  seen = 0;
  for (const FmJsonNode *f = json_first(files); f && seen < 4000; f = json_next(f), seen++) {
    const char *name = json_str(json_get(f, "name"), "");
    int rank = file_rank(name, c->have_ffmpeg_libs);
    if (rank < 0 || !*name || strstr(name, "..") || strlen(name) > 400) continue;
    if (!strcmp(json_str(json_get(f, "private"), ""), "true")) continue;     /* answers 401 */
    double l = vsrc_clock_seconds(json_str(json_get(f, "length"), ""));
    if (maxlen > 0 && l < maxlen * 0.9) continue;         /* a trailer or a menu next to the film */
    int h = (int)json_num(json_get(f, "height"), 0);
    double fit = h <= 0 ? 5000 : h <= H ? H - h : 10000 + (h - H);
    double score = rank * 1e6 + fit;
    if (score < best_score) { best_score = score; best = f; }
  }
  if (!best) {
    json_free(&j);
    fm_strlcpy(err, ogg_only ? "This item only has Ogg video, which needs the FFmpeg libraries"
                             : "This item has no video this app can play", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  download_url(id, json_str(json_get(best, "name"), ""), out->video, sizeof out->video);
  out->duration = vsrc_clock_seconds(json_str(json_get(best, "length"), ""));
  out->width = (int)json_num(json_get(best, "width"), 0);
  out->height = (int)json_num(json_get(best, "height"), 0);
  out->local = false;
  json_free(&j);
  return FM_OK;
}

static FmErr ia_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!vsrc_id_ok(item->id, "-_.", 63)) {
    fm_strlcpy(err, "Not an archive.org item", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (cb && !cb(user, -1.0f, "Finding the video\xE2\x80\xA6")) {
    fm_strlcpy(err, "Cancelled", errcap);
    return FM_ERR_CANCEL;
  }
  char url[256];
  fm_snprintf(url, sizeof url, "https://archive.org/metadata/%s", item->id);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    vsrc_http_error("archive.org", r.status, err, errcap);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_archive_pick((const char *)r.data, r.len, item->id, c, out, err, errcap);
  net_resp_free(&r);
  return e;
}

const FmVsrc g_vsrc_archive = {
  "archive", "Internet Archive", IC_LANDMARK, VSRC_SEARCH | VSRC_DIRECT,
  ia_search, ia_resolve, NULL, NULL,
  "Free, no key needed \xC2\xB7 public-domain films, cartoons and TV",
};
