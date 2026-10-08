/* fpsrc_artic.c -- Art Institute of Chicago: 130k artworks, 50k+ of them
** public domain, served through a IIIF image server. No key.
**
** Design decisions:
**   - The IIIF server sits behind Cloudflare and answers 403 with a
**     "Just a moment..." page to plain clients, whatever the User-Agent; it
**     lets requests through that carry "AIC-User-Agent" (the header the
**     museum's API docs ask clients to send). So the adapter publishes it as
**     img_headers: thumbnails and full images need it, not only the API.
**   - Sizes: 400 px thumbnails; full is 1686 px for public-domain works
**     and 843 px for the rest (the server redirects larger requests for
**     works still in copyright to 843). Never wider than the original,
**     since IIIF level 2 does not upscale.
**   - Search depth is capped by the API at 1000 results (page 34 of 30
**     answers 403 "Invalid number of results"), so paging stops at 33.
*/
#include "fpsrc_int.h"

#define ARTIC_HEADERS "AIC-User-Agent: mmcfm/0.1 (open-source file manager; photo gallery)\r\n"
#define ARTIC_MAX_PAGE 33

void psrc_artic_url(const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap,
              "https://api.artic.edu/api/v1/artworks/search?q=%s&limit=%d&page=%d"
              "&fields=id,title,artist_title,image_id,thumbnail,is_public_domain",
              qe, PSRC_PAGE_SIZE, FM_CLAMP(page, 1, ARTIC_MAX_PAGE));
}

FmErr psrc_artic_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "The Art Institute sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *data = json_get(root, "data");
  if (!data || data->type != JSON_ARR) {
    const char *d = json_str(json_get(root, "detail"), "");
    if (*d) fm_snprintf(out->error, sizeof out->error, "Art Institute: %.200s", d);
    else fm_strlcpy(out->error, "The Art Institute sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  char iiif[256];
  if (!psrc_url_copy(json_str(json_path(root, "config.iiif_url"), ""), iiif, sizeof iiif) ||
      strncmp(iiif, "https://", 8) || strchr(iiif, '?'))
    fm_strlcpy(iiif, "https://www.artic.edu/iiif/2", sizeof iiif);
  size_t il = strlen(iiif);
  if (il && iiif[il - 1] == '/') iiif[il - 1] = 0;
  int seen = 0;
  for (const FmJsonNode *d = json_first(data); d && seen < PSRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *img = json_str(json_get(d, "image_id"), "");
    double idn = json_num(json_get(d, "id"), 0);
    if (!psrc_id_ok(img, "-", 64) || idn <= 0 || idn > 1e12) continue;
    bool pd = json_bool(json_get(d, "is_public_domain"), false);
    const FmJsonNode *th = json_get(d, "thumbnail");
    double w = json_num(json_get(th, "width"), 0), h = json_num(json_get(th, "height"), 0);
    int W = w > 0 && w < 1e6 ? (int)w : 0, H = h > 0 && h < 1e6 ? (int)h : 0;
    int fw = pd ? 1686 : 843, tw = 400;
    if (W > 0 && W < fw) fw = W;
    if (W > 0 && W < tw) tw = W;
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "artic", sizeof it->source);
    fm_snprintf(it->id, sizeof it->id, "%.0f", idn);
    psrc_copy(it->title, json_str(json_get(d, "title"), "Untitled"), sizeof it->title);
    psrc_copy(it->author, json_str(json_get(d, "artist_title"), ""), sizeof it->author);
    fm_strlcpy(it->license, pd ? "Public domain" : "\xC2\xA9 see artic.edu", sizeof it->license);
    fm_snprintf(it->thumb, sizeof it->thumb, "%s/%s/full/%d,/0/default.jpg", iiif, img, tw);
    fm_snprintf(it->full, sizeof it->full, "%s/%s/full/%d,/0/default.jpg", iiif, img, fw);
    fm_snprintf(it->page, sizeof it->page, "https://www.artic.edu/artworks/%s", it->id);
    if (W > 0 && H > 0) {
      it->width = W;
      it->height = H;
      psrc_fit_width(&it->width, &it->height, fw);
    }
  }
  double pages = json_num(json_path(root, "pagination.total_pages"), 0);
  if (seen > 0 && page < pages && page < ARTIC_MAX_PAGE) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr artic_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                          volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = psrc_page_num(token, 1, 1, ARTIC_MAX_PAGE);
  char url[1200];
  psrc_artic_url(query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, ARTIC_HEADERS, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("The Art Institute", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_artic_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No artworks found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_artic = {
  "artic", "Art Institute of Chicago", IC_LANDMARK, PSRC_SEARCH, artic_search, NULL,
  "Paintings and artworks, many public domain \xC2\xB7 no key needed",
  ARTIC_HEADERS, NULL,
};
