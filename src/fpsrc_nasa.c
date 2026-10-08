/* fpsrc_nasa.c -- NASA Image and Video Library (images.nasa.gov): 140k+
** mission photos, mostly public domain. No key.
**
** Design decisions:
**   - Search results already list the rendered sizes of each photo
**     (~thumb, ~small, ~medium 1280, ~large 1920, ~orig) with their pixel
**     sizes, so no second request is needed: thumb = ~thumb, full = ~large
**     (else ~medium, else a viewable ~orig), Download = ~orig, which is
**     often a 50 MB TIFF the viewer could not show anyway.
**   - details() covers the rare item whose search entry lists no sizes: it
**     reads the item's collection.json (a list of every asset) and picks
**     the same way.
**   - NASA ids and asset names may contain blanks ("Moon to Mars ..."):
**     they are percent-encoded when copied into URLs.
**   - Licence text is "NASA (public domain*)": NASA material is not
**     copyrighted, but some photos carry third-party rights or logos.
*/
#include "fpsrc_int.h"

#define NASA_LICENSE "NASA (public domain*)"

void psrc_nasa_url(const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://images-api.nasa.gov/search?q=%s&media_type=image&page=%d&page_size=%d", qe,
              page < 1 ? 1 : page, PSRC_PAGE_SIZE);
}

/* rank of an asset name: 3 ~large, 2 ~medium, 1 ~orig (viewable), 0 other */
static int asset_rank(const char *url) {
  if (strstr(url, "~large.")) return 3;
  if (strstr(url, "~medium.")) return 2;
  if (strstr(url, "~orig.") && psrc_url_viewable(url)) return 1;
  return 0;
}

/* http:// asset links (collection.json has them) are served over https too */
static bool asset_url(const char *s, char *out, size_t cap) {
  char tmp[1100];
  if (!strncmp(s, "http://images-assets.nasa.gov/", 30)) {
    fm_snprintf(tmp, sizeof tmp, "https://%s", s + 7);
    s = tmp;
  }
  return psrc_url_copy(s, out, cap);
}

static bool id_ok(const char *id) {
  size_t n = strlen(id);
  if (!n || n > 90 || strstr(id, "..") || strchr(id, '/') || strchr(id, '\\')) return false;
  for (const char *s = id; *s; s++)
    if ((u8)*s < ' ' || *s == '"' || *s == '<' || *s == '>') return false;
  return true;
}

FmErr psrc_nasa_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "NASA Images sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *coll = json_get(root, "collection");
  const FmJsonNode *items = json_get(coll, "items");
  if (!items || items->type != JSON_ARR) {
    const char *r = json_str(json_get(root, "reason"), "");
    if (*r) fm_snprintf(out->error, sizeof out->error, "NASA Images: %.200s", r);
    else fm_strlcpy(out->error, "NASA Images sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *x = json_first(items); x && seen < PSRC_MAX_ITEMS; x = json_next(x), seen++) {
    const FmJsonNode *d = json_at(json_get(x, "data"), 0);
    const char *id = json_str(json_get(d, "nasa_id"), "");
    if (!id_ok(id)) continue;
    const char *mt = json_str(json_get(d, "media_type"), "image");
    if (strcmp(mt, "image")) continue;
    char thumb[512], full[1024], orig[1024], u[1100];
    int best = -1, fw = 0, fh = 0;
    thumb[0] = full[0] = orig[0] = 0;
    int links = 0;
    for (const FmJsonNode *l = json_first(json_get(x, "links")); l && links < 16; l = json_next(l), links++) {
      if (!asset_url(json_str(json_get(l, "href"), ""), u, sizeof u)) continue;
      const char *rel = json_str(json_get(l, "rel"), "");
      if (!strcmp(rel, "preview") || strstr(u, "~thumb.")) {
        if (!thumb[0] && strlen(u) < sizeof thumb) fm_strlcpy(thumb, u, sizeof thumb);
        continue;
      }
      if (!strcmp(rel, "canonical") || strstr(u, "~orig.")) fm_strlcpy(orig, u, sizeof orig);
      int r = asset_rank(u);
      if (r > best) {
        best = r;
        fm_strlcpy(full, u, sizeof full);
        fw = (int)json_num(json_get(l, "width"), 0);
        fh = (int)json_num(json_get(l, "height"), 0);
      }
    }
    if (!thumb[0] && !full[0]) continue;
    if (best <= 0) {                       /* only a thumbnail: details() looks further */
      fm_strlcpy(full, thumb[0] ? thumb : full, sizeof full);
      fw = fh = 0;
    }
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "nasa", sizeof it->source);
    psrc_copy(it->id, id, sizeof it->id);
    psrc_copy(it->title, json_str(json_get(d, "title"), id), sizeof it->title);
    const char *by = json_str(json_get(d, "photographer"), "");
    if (!*by) by = json_str(json_get(d, "secondary_creator"), "");
    if (*by) psrc_copy(it->author, by, sizeof it->author);
    else fm_snprintf(it->author, sizeof it->author, "NASA %.60s", json_str(json_get(d, "center"), ""));
    size_t al = strlen(it->author);
    while (al && it->author[al - 1] == ' ') it->author[--al] = 0;
    fm_strlcpy(it->license, NASA_LICENSE, sizeof it->license);
    fm_strlcpy(it->thumb, thumb[0] ? thumb : full, sizeof it->thumb);
    fm_strlcpy(it->full, full, sizeof it->full);
    if (best > 0 && orig[0] && strcmp(orig, full)) fm_strlcpy(it->original, orig, sizeof it->original);
    char ide[300];
    net_urlencode(it->id, ide, sizeof ide);
    fm_snprintf(it->page, sizeof it->page, "https://images.nasa.gov/details/%s", ide);
    if (fw > 0 && fh > 0 && fw < 100000 && fh < 100000) { it->width = fw; it->height = fh; }
  }
  bool more = false;
  for (const FmJsonNode *l = json_first(json_get(coll, "links")); l; l = json_next(l))
    if (!strcmp(json_str(json_get(l, "rel"), ""), "next")) more = true;
  if (more && seen > 0 && page < 300) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

FmErr psrc_nasa_parse_assets(const char *json, size_t len, FmPsrcItem *item) {
  FmJson j;
  if (!json || json_parse(&j, json, len) != FM_OK) return FM_ERR_FORMAT;
  const FmJsonNode *arr = json_root(&j);
  if (!arr || arr->type != JSON_ARR) { json_free(&j); return FM_ERR_FORMAT; }
  char u[1100], full[1024], orig[1024];
  int best = 0, seen = 0;
  full[0] = orig[0] = 0;
  for (const FmJsonNode *a = json_first(arr); a && seen < 64; a = json_next(a), seen++) {
    if (!asset_url(json_str(a, ""), u, sizeof u) || strlen(u) >= sizeof full) continue;
    if (strstr(u, "~orig.")) fm_strlcpy(orig, u, sizeof orig);
    int r = asset_rank(u);
    if (r > best) { best = r; fm_strlcpy(full, u, sizeof full); }
  }
  json_free(&j);
  if (!best) return FM_ERR_NOT_FOUND;
  if (strcmp(item->full, full)) { item->width = item->height = 0; }   /* the size of the new file is unknown */
  fm_strlcpy(item->full, full, sizeof item->full);
  if (orig[0] && strcmp(orig, full)) fm_strlcpy(item->original, orig, sizeof item->original);
  return FM_OK;
}

static FmErr nasa_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                         volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = psrc_page_num(token, 1, 1, 300);
  char url[1200];
  psrc_nasa_url(query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("NASA Images", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_nasa_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

static FmErr nasa_details(const FmPsrcConf *c, FmPsrcItem *item, volatile int *cancel) {
  FM_UNUSED(c);
  if (item->original[0] || asset_rank(item->full) > 0) return FM_OK;   /* search had the sizes */
  if (!id_ok(item->id)) return FM_ERR_NOT_FOUND;
  char ide[300], url[512], err[256];
  net_urlencode(item->id, ide, sizeof ide);
  fm_snprintf(url, sizeof url, "https://images-assets.nasa.gov/image/%s/collection.json", ide);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, err, sizeof err, cancel);
  if (e != FM_OK) return e;
  e = r.status == 200 ? psrc_nasa_parse_assets((const char *)r.data, r.len, item) : FM_ERR_NOT_FOUND;
  net_resp_free(&r);
  return e;
}

const FmPsrc g_psrc_nasa = {
  "nasa", "NASA Images", IC_MOON, PSRC_SEARCH, nasa_search, nasa_details,
  "Space and mission photos \xC2\xB7 public domain, no key needed",
  NULL, NULL,
};
