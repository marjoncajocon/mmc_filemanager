/* fpsrc_openverse.c -- Openverse (api.openverse.org): 800M+ Creative
** Commons and public-domain images gathered from Flickr, Wikimedia, museums.
** No key.
**
** Design decisions:
**   - Anonymous clients may ask for at most 20 per page and 240 results in
**     all (deeper pages answer 401 "pagination depth"), so pages are 20 and
**     the token stops at page 12. A key (OAuth client credentials) would
**     lift this; not worth a settings field for a gallery.
**   - `thumbnail` is Openverse's own resizing proxy (fast, always JPEG or
**     PNG); `url` is the original at the provider. When the original is a
**     type the viewer cannot decode (WebP, TIFF), the viewer gets the proxy
**     image and Download still saves the original.
**   - The proxy allows anonymous clients 1000 thumbnails a day
**     (x-ratelimit-limit-anon_thumbnail), about 50 pages. Most results are
**     Flickr photos, whose static URLs name their size ("_b" = 1024 px), so
**     those thumbnails come straight from Flickr as "_w" (400 px) and the
**     quota is left for the rest.
**   - Through WinHTTP (no Accept header) the proxy answered WebP, which the
**     image decoder cannot read; images are asked for with an explicit
**     JPEG/PNG Accept header (img_headers).
**   - Licence codes ("by-sa", "cc0", "pdm") become the names people know
**     ("CC BY-SA 2.0", "CC0 1.0", "Public domain mark").
*/
#include "fpsrc_int.h"

void psrc_openverse_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://api.openverse.org/v1/images/?q=%s&page_size=%d&page=%d%s", qe, PSRC_OV_PAGE,
              FM_CLAMP(page, 1, PSRC_OV_MAX_PAGE), c && c->safe_search ? "&mature=false" : "");
}

static void ov_license(const char *code, const char *ver, char *out, size_t cap) {
  if (!*code) { out[0] = 0; return; }
  if (!strcmp(code, "pdm")) { fm_strlcpy(out, "Public domain mark", cap); return; }
  if (!strcmp(code, "cc0")) { fm_snprintf(out, cap, "CC0 %s", *ver ? ver : "1.0"); return; }
  if (!strcmp(code, "sampling+") || !strcmp(code, "nc-sampling+")) {
    fm_snprintf(out, cap, "CC %s", code);
    return;
  }
  char up[32];
  size_t o = 0;
  for (const char *s = code; *s && o + 1 < sizeof up; s++)
    up[o++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s);
  up[o] = 0;
  if (*ver) fm_snprintf(out, cap, "CC %s %s", up, ver);
  else fm_snprintf(out, cap, "CC %s", up);
}

bool psrc_flickr_thumb(const char *url, char *out, size_t cap) {
  /* https://live.staticflickr.com/<server>/<id>_<secret>[_<size>].jpg -> _w (400 px) */
  if (cap) out[0] = 0;
  if (!url) return false;
  const char *host = strstr(url, "://");
  if (!host) return false;
  host += 3;
  const char *slash = strchr(host, '/');
  if (!slash || (size_t)(slash - host) <= 17 || strncmp(slash - 17, ".staticflickr.com", 17) ||
      strchr(url, '?'))
    return false;
  const char *name = strrchr(url, '/') + 1;
  size_t n = strlen(name);
  if (n < 8 || fm_stricmp(name + n - 4, ".jpg")) return false;
  size_t i = 0;
  while (name[i] >= '0' && name[i] <= '9') i++;
  if (!i || name[i] != '_') return false;
  size_t s = ++i;
  while ((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')) i++;
  if (i == s) return false;
  size_t stem = i;                                     /* end of <id>_<secret> */
  if (name[i] == '_' && name[i + 1] >= 'a' && name[i + 1] <= 'z' && name + i + 2 == name + n - 4) i += 2;
  if (name + i != name + n - 4) return false;
  int k = fm_snprintf(out, cap, "%.*s_w.jpg", (int)(name - url + stem), url);
  if (k < 0 || (size_t)k >= cap) { out[0] = 0; return false; }
  return true;
}

FmErr psrc_openverse_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Openverse sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *res = json_get(root, "results");
  if (!res || res->type != JSON_ARR) {
    const char *d = json_str(json_get(root, "detail"), "");
    if (*d) fm_snprintf(out->error, sizeof out->error, "Openverse: %.200s", d);
    else fm_strlcpy(out->error, "Openverse sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *r = json_first(res); r && seen < PSRC_MAX_ITEMS; r = json_next(r), seen++) {
    const char *id = json_str(json_get(r, "id"), "");
    char thumb[512], full[1024];
    if (!psrc_id_ok(id, "-", 64)) continue;
    bool th_ok = psrc_url_copy(json_str(json_get(r, "thumbnail"), ""), thumb, sizeof thumb);
    bool full_ok = psrc_url_copy(json_str(json_get(r, "url"), ""), full, sizeof full);
    if (!th_ok && !full_ok) continue;
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "openverse", sizeof it->source);
    fm_strlcpy(it->id, id, sizeof it->id);
    psrc_copy(it->title, json_str(json_get(r, "title"), ""), sizeof it->title);
    psrc_copy(it->author, json_str(json_get(r, "creator"), ""), sizeof it->author);
    ov_license(json_str(json_get(r, "license"), ""), json_str(json_get(r, "license_version"), ""), it->license,
               sizeof it->license);
    char fl[512];
    if (full_ok && psrc_flickr_thumb(full, fl, sizeof fl)) fm_strlcpy(it->thumb, fl, sizeof it->thumb);
    else fm_strlcpy(it->thumb, th_ok ? thumb : full, sizeof it->thumb);
    if (full_ok && psrc_url_viewable(full)) {
      fm_strlcpy(it->full, full, sizeof it->full);
    } else {
      fm_strlcpy(it->full, thumb, sizeof it->full);
      if (full_ok) fm_strlcpy(it->original, full, sizeof it->original);
    }
    char pg[512];
    if (psrc_url_copy(json_str(json_get(r, "foreign_landing_url"), ""), pg, sizeof pg))
      fm_strlcpy(it->page, pg, sizeof it->page);
    else fm_snprintf(it->page, sizeof it->page, "https://openverse.org/image/%s", id);
    double w = json_num(json_get(r, "width"), 0), h = json_num(json_get(r, "height"), 0);
    if (w > 0 && h > 0 && w < 1e6 && h < 1e6 && !strcmp(it->full, full)) {
      it->width = (int)w;
      it->height = (int)h;
    }
  }
  int pages = (int)json_num(json_get(root, "page_count"), 0);
  if (seen > 0 && page < pages && page < PSRC_OV_MAX_PAGE) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr ov_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = psrc_page_num(token, 1, 1, PSRC_OV_MAX_PAGE);
  char url[1200];
  psrc_openverse_url(c, query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    /* 401 here means "too deep for anonymous use", not a key problem */
    e = psrc_http_error("Openverse", r.status, false, out->error, sizeof out->error);
    if (r.status == 401) fm_strlcpy(out->error, "Openverse shows at most 240 results without an account",
                                    sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_openverse_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_openverse = {
  "openverse", "Openverse", IC_IMAGE, PSRC_SEARCH, ov_search, NULL,
  "800 million Creative Commons photos \xC2\xB7 no key needed",
  "Accept: image/jpeg, image/png;q=0.9, */*;q=0.5\r\n", NULL,
};
