/* fvsrc_peertube.c -- PeerTube: the federated video network, searched
** through SepiaSearch (sepiasearch.org, run by Framasoft). No key.
**
** Design decisions:
**   - Each result lives on its own PeerTube server; resolve asks that server
**     (https://<host>/api/v1/videos/<uuid>) for the files and returns a URL
**     the player streams directly.
**   - Plain "web video" files (files[]) beat the HLS copies
**     (streamingPlaylists[].files[]): the HLS ones are fragmented MP4,
**     which Media Foundation decodes less reliably. Servers that keep only
**     HLS, or that store audio separately (PeerTube 7), still work: the
**     audio-only file becomes FmVsrcStream.audio.
*/
#include "fvsrc_int.h"

/* ---- search ------------------------------------------------------------------- */

void vsrc_peertube_search_url(const FmVsrcConf *c, const char *query, int start, char *out, size_t cap) {
  char q[256], qe[800];
  fm_strlcpy(q, query ? query : "", sizeof q);
  net_urlencode(q, qe, sizeof qe);
  fm_snprintf(out, cap, "https://sepiasearch.org/api/v1/search/videos?search=%s&count=%d&start=%d&nsfw=%s",
              qe, VSRC_PAGE_SIZE, start < 0 ? 0 : start, c->safe_search ? "false" : "both");
}

FmErr vsrc_peertube_parse_search(const char *json, size_t len, int start, FmVsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "SepiaSearch sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *data = json_get(root, "data");
  if (!data || data->type != JSON_ARR) {
    json_free(&j);
    fm_strlcpy(out->error, "SepiaSearch sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *d = json_first(data); d && seen < VSRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *uuid = json_str(json_get(d, "uuid"), "");
    const char *page = json_str(json_get(d, "url"), "");
    if (!vsrc_id_ok(uuid, "-", 63) || !vsrc_url_ok(page)) continue;
    FmVsrcItem *it = vsrc_page_add(out);
    fm_strlcpy(it->id, uuid, sizeof it->id);
    fm_strlcpy(it->title, json_str(json_get(d, "name"), uuid), sizeof it->title);
    const char *ch = json_str(json_path(d, "account.displayName"), "");
    if (!*ch) ch = json_str(json_path(d, "channel.displayName"), "");
    fm_strlcpy(it->channel, ch, sizeof it->channel);
    const char *th = json_str(json_get(d, "thumbnailUrl"), "");
    if (!vsrc_url_ok(th)) th = json_str(json_get(d, "previewUrl"), "");
    if (vsrc_url_ok(th)) fm_strlcpy(it->thumb, th, sizeof it->thumb);
    fm_strlcpy(it->page, page, sizeof it->page);
    vsrc_iso_date(json_str(json_get(d, "publishedAt"), ""), it->published, sizeof it->published);
    double du = json_num(json_get(d, "duration"), 0);
    it->duration = du > 0 && du < 1e7 ? du : 0;
    double v = json_num(json_get(d, "views"), -1);
    it->views = v >= 0 && v < 9e15 ? (i64)v : -1;
    it->live = json_bool(json_get(d, "isLive"), false);
  }
  double total = json_num(json_get(root, "total"), 0);
  if (seen >= VSRC_PAGE_SIZE && start + VSRC_PAGE_SIZE < total && start < 1000)
    fm_snprintf(out->next, sizeof out->next, "%d", start + VSRC_PAGE_SIZE);
  json_free(&j);
  return FM_OK;
}

static FmErr pt_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int start = page_token && *page_token ? atoi(page_token) : 0;
  start = FM_CLAMP(start, 0, 1000);
  char url[1200];
  vsrc_peertube_search_url(c, query, start, url, sizeof url);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    vsrc_http_error("SepiaSearch", r.status, out->error, sizeof out->error);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = vsrc_peertube_parse_search((const char *)r.data, r.len, start, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && !start) fm_strlcpy(out->error, "No videos found", sizeof out->error);
  return e;
}

/* ---- resolve ------------------------------------------------------------------- */

typedef struct PtFile { const FmJsonNode *n; int res; bool video, audio, web; } PtFile;

static const char *file_url(const FmJsonNode *f) {
  const char *u = json_str(json_get(f, "fileUrl"), "");
  return vsrc_url_ok(u) ? u : json_str(json_get(f, "fileDownloadUrl"), "");
}

static bool playable(const FmJsonNode *f, bool libs) {
  char tmp[600];
  const char *u = file_url(f);
  if (!vsrc_url_ok(u) || strlen(u) >= sizeof tmp) return false;
  fm_strlcpy(tmp, u, sizeof tmp);
  tmp[strcspn(tmp, "?#")] = 0;
  return fm_ends_with_i(tmp, ".mp4") || fm_ends_with_i(tmp, ".m4a") ||
      (libs &&
       (fm_ends_with_i(tmp, ".webm") || fm_ends_with_i(tmp, ".mkv") || fm_ends_with_i(tmp, ".ogg") ||
        fm_ends_with_i(tmp, ".ogv")));
}

/* lower is better: web files first, then the height nearest below H */
static double file_score(const PtFile *f, int H) {
  double fit = f->res <= 0 ? 5000 : f->res <= H ? H - f->res : 10000 + (f->res - H);
  return (f->web ? 0 : 1e6) + fit;
}

FmErr vsrc_peertube_pick(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                         size_t errcap) {
  FmJson j;
  memset(out, 0, sizeof *out);
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(err, "The PeerTube server sent a reply this app does not understand", errcap);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  if (json_bool(json_get(root, "isLive"), false)) {
    json_free(&j);
    fm_strlcpy(err, "Live streams cannot be played here yet; use Open in browser", errcap);
    return FM_ERR_UNSUPPORTED;
  }
  PtFile *fs = (PtFile *)fm_alloc(128 * sizeof *fs);
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {
    const FmJsonNode *lists[16];
    int nl = 0;
    if (pass == 0) lists[nl++] = json_get(root, "files");
    else
      for (const FmJsonNode *p = json_first(json_get(root, "streamingPlaylists")); p && nl < 16;
           p = json_next(p))
        lists[nl++] = json_get(p, "files");
    for (int l = 0; l < nl; l++)
      for (const FmJsonNode *f = json_first(lists[l]); f && n < 128; f = json_next(f)) {
        if (f->type != JSON_OBJ || !playable(f, c->have_ffmpeg_libs || c->os_dash)) continue;
        PtFile *p = &fs[n++];
        p->n = f;
        p->res = (int)json_num(json_path(f, "resolution.id"), 0);
        p->video = json_bool(json_get(f, "hasVideo"), p->res > 0);
        p->audio = json_bool(json_get(f, "hasAudio"), true);
        p->web = pass == 0;
      }
  }
  int H = c->max_height > 0 ? c->max_height : 720;
  int both = -1, vonly = -1, aonly = -1;
  for (int i = 0; i < n; i++) {
    int *slot = fs[i].video && fs[i].audio ? &both : fs[i].video ? &vonly : fs[i].audio ? &aonly : NULL;
    if (!slot) continue;
    if (*slot < 0 || file_score(&fs[i], H) < file_score(&fs[*slot], H)) *slot = i;
  }
  FmErr e = FM_OK;
  if (both >= 0) {
    fm_strlcpy(out->video, file_url(fs[both].n), sizeof out->video);
    out->height = fs[both].res;
  } else if (vonly >= 0) {
    fm_strlcpy(out->video, file_url(fs[vonly].n), sizeof out->video);
    if (aonly >= 0) fm_strlcpy(out->audio, file_url(fs[aonly].n), sizeof out->audio);
    out->height = fs[vonly].res;
  } else {
    fm_strlcpy(err, "This video has no file this app can play (it may still be processing)", errcap);
    e = FM_ERR_UNSUPPORTED;
  }
  double d = json_num(json_get(root, "duration"), 0);
  out->duration = d > 0 && d < 1e7 ? d : 0;
  fm_free(fs);
  json_free(&j);
  return e;
}

static FmErr pt_resolve(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                        void *user, char *err, size_t errcap, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  /* the server is the host of the item's page */
  char host[256];
  const char *p = item->page;
  if (!strncmp(p, "https://", 8)) p += 8;
  else if (!strncmp(p, "http://", 7)) p += 7;
  else p = "";
  size_t hn = strcspn(p, "/?#");
  if (!hn || hn >= sizeof host) {
    fm_strlcpy(err, "This item has no PeerTube server address", errcap);
    return FM_ERR_NOT_FOUND;
  }
  memcpy(host, p, hn);
  host[hn] = 0;
  if (!vsrc_id_ok(host, ".-:", 255) || !vsrc_id_ok(item->id, "-", 63)) {
    fm_strlcpy(err, "This item has no valid PeerTube address", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (cb && !cb(user, -1.0f, "Finding the video\xE2\x80\xA6")) {
    fm_strlcpy(err, "Cancelled", errcap);
    return FM_ERR_CANCEL;
  }
  char url[512];
  fm_snprintf(url, sizeof url, "https://%s/api/v1/videos/%s", host, item->id);
  FmNetResp r;
  FmErr e = vsrc_http_get(url, &r, err, errcap, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    if (r.status == 404) fm_strlcpy(err, "This video was removed from its PeerTube server", errcap);
    else vsrc_http_error(host, r.status, err, errcap);
    net_resp_free(&r);
    return r.status == 404 ? FM_ERR_NOT_FOUND : FM_ERR_IO;
  }
  e = vsrc_peertube_pick((const char *)r.data, r.len, c, out, err, errcap);
  net_resp_free(&r);
  return e;
}

const FmVsrc g_vsrc_peertube = {
  "peertube", "PeerTube", IC_NETWORK, VSRC_SEARCH | VSRC_DIRECT,
  pt_search, pt_resolve, NULL, NULL,
  "Free, no key needed \xC2\xB7 videos from PeerTube servers, via SepiaSearch",
};
