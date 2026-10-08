/* fpsrc_pexels.c -- Pexels: free stock photos. Needs a free API key
** (pexels.com/api), sent as the Authorization header.
**
** Design decisions:
**   - An empty query browses the curated feed (PSRC_BROWSE), which is what
**     the Pexels home page shows.
**   - Sizes: thumb = src.medium (350 px high), full = src.large2x
**     (940 px at 2x = ~1880 px wide), Download = src.original.
**   - A key with characters outside [A-Za-z0-9-_.] is refused before any
**     request: it would end up inside an HTTP header.
*/
#include "fpsrc_int.h"

#define PEXELS_FULL_W 1880

void psrc_pexels_url(const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800];
  page = FM_CLAMP(page, 1, 1000);
  if (!q || !*q) {
    fm_snprintf(out, cap, "https://api.pexels.com/v1/curated?per_page=%d&page=%d", PSRC_PAGE_SIZE, page);
    return;
  }
  fm_strlcpy(qq, q, sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://api.pexels.com/v1/search?query=%s&per_page=%d&page=%d", qe, PSRC_PAGE_SIZE,
              page);
}

FmErr psrc_pexels_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Pexels sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *photos = json_get(root, "photos");
  if (!photos || photos->type != JSON_ARR) {
    const char *m = json_str(json_get(root, "error"), "");
    if (!*m) m = json_str(json_get(root, "message"), "");
    if (*m) fm_snprintf(out->error, sizeof out->error, "Pexels: %.200s", m);
    else fm_strlcpy(out->error, "Pexels sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *p = json_first(photos); p && seen < PSRC_MAX_ITEMS; p = json_next(p), seen++) {
    double idn = json_num(json_get(p, "id"), 0);
    const FmJsonNode *src = json_get(p, "src");
    char thumb[512], full[1024], orig[1024], pg[512];
    if (idn <= 0 || idn > 1e15) continue;
    if (!psrc_url_copy(json_str(json_get(src, "medium"), ""), thumb, sizeof thumb)) continue;
    if (!psrc_url_copy(json_str(json_get(src, "large2x"), ""), full, sizeof full) &&
        !psrc_url_copy(json_str(json_get(src, "large"), ""), full, sizeof full))
      fm_strlcpy(full, thumb, sizeof full);
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "pexels", sizeof it->source);
    fm_snprintf(it->id, sizeof it->id, "%.0f", idn);
    psrc_copy(it->title, json_str(json_get(p, "alt"), ""), sizeof it->title);
    psrc_copy(it->author, json_str(json_get(p, "photographer"), ""), sizeof it->author);
    fm_strlcpy(it->license, "Pexels License", sizeof it->license);
    fm_strlcpy(it->thumb, thumb, sizeof it->thumb);
    fm_strlcpy(it->full, full, sizeof it->full);
    if (psrc_url_copy(json_str(json_get(src, "original"), ""), orig, sizeof orig) && strcmp(orig, full))
      fm_strlcpy(it->original, orig, sizeof it->original);
    if (psrc_url_copy(json_str(json_get(p, "url"), ""), pg, sizeof pg)) fm_strlcpy(it->page, pg, sizeof it->page);
    else fm_snprintf(it->page, sizeof it->page, "https://www.pexels.com/photo/%s/", it->id);
    double w = json_num(json_get(p, "width"), 0), h = json_num(json_get(p, "height"), 0);
    if (w > 0 && h > 0 && w < 1e6 && h < 1e6) {
      it->width = (int)w;
      it->height = (int)h;
      psrc_fit_width(&it->width, &it->height, PEXELS_FULL_W);
    }
    it->color = psrc_hex_color(json_str(json_get(p, "avg_color"), ""));
  }
  if (seen > 0 && *json_str(json_get(root, "next_page"), "") && page < 1000)
    fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr pexels_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                           volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!psrc_have_key("Pexels", c->key_pexels, out->error, sizeof out->error)) return FM_ERR_ACCESS;
  if (!psrc_id_ok(c->key_pexels, "-_.", 95)) {
    fm_strlcpy(out->error, "The Pexels key in Settings is not valid (check for typos)", sizeof out->error);
    return FM_ERR_ACCESS;
  }
  int page = psrc_page_num(token, 1, 1, 1000);
  char url[1200], hdr[160];
  psrc_pexels_url(query, page, url, sizeof url);
  fm_snprintf(hdr, sizeof hdr, "Authorization: %s\r\n", c->key_pexels);
  FmNetResp r;
  FmErr e = psrc_http_get(url, hdr, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Pexels", r.status, true, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_pexels_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_pexels = {
  "pexels", "Pexels", IC_PICTURES, PSRC_SEARCH | PSRC_NEEDKEY | PSRC_BROWSE, pexels_search, NULL,
  "Free stock photos \xC2\xB7 needs a free key from pexels.com/api",
  NULL, NULL,
};
