/* fpsrc_pixabay.c -- Pixabay: free stock photos. Needs a free API key
** (pixabay.com/api/docs), passed as the `key` query parameter.
**
** Design decisions:
**   - A wrong or missing key answers HTTP 400 with plain text ("[ERROR
**     400] Invalid or missing API key"), not 401 and not JSON; that reply
**     becomes the usual "check the key" message.
**   - Sizes: thumb = webformatURL (640 px), full = largeImageURL (1280 px,
**     the largest a free key gets). Both URLs expire after 24 hours, which
**     is why the gallery never stores them beyond a session.
**   - The API returns at most 500 hits per query, so paging stops there.
**   - An empty query lists popular photos (PSRC_BROWSE).
*/
#include "fpsrc_int.h"

#define PIXABAY_FULL_W 1280
#define PIXABAY_MAX_HITS 500

void psrc_pixabay_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800], ke[300];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  net_urlencode(c->key_pixabay, ke, sizeof ke);
  fm_snprintf(out, cap,
              "https://pixabay.com/api/?key=%s&q=%s&per_page=%d&page=%d&image_type=photo&safesearch=%s", ke,
              qe, PSRC_PAGE_SIZE, FM_CLAMP(page, 1, PIXABAY_MAX_HITS / PSRC_PAGE_SIZE + 1),
              c->safe_search ? "true" : "false");
}

FmErr psrc_pixabay_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    char head[400];
    size_t n = json && len < sizeof head ? len : 0;
    if (n) memcpy(head, json, n);
    head[n] = 0;
    if (n && fm_stristr(head, "key"))
      fm_strlcpy(out->error, "Pixabay did not accept the API key \xE2\x80\x94 check it in Settings",
                 sizeof out->error);
    else fm_strlcpy(out->error, "Pixabay sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *hits = json_get(root, "hits");
  if (!hits || hits->type != JSON_ARR) {
    fm_strlcpy(out->error, "Pixabay sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *h = json_first(hits); h && seen < PSRC_MAX_ITEMS; h = json_next(h), seen++) {
    double idn = json_num(json_get(h, "id"), 0);
    char thumb[512], full[1024], pg[512];
    if (idn <= 0 || idn > 1e15) continue;
    if (!psrc_url_copy(json_str(json_get(h, "webformatURL"), ""), thumb, sizeof thumb)) continue;
    if (!psrc_url_copy(json_str(json_get(h, "largeImageURL"), ""), full, sizeof full))
      fm_strlcpy(full, thumb, sizeof full);
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "pixabay", sizeof it->source);
    fm_snprintf(it->id, sizeof it->id, "%.0f", idn);
    psrc_copy(it->title, json_str(json_get(h, "tags"), ""), sizeof it->title);
    if (it->title[0] >= 'a' && it->title[0] <= 'z') it->title[0] = (char)(it->title[0] - 'a' + 'A');
    psrc_copy(it->author, json_str(json_get(h, "user"), ""), sizeof it->author);
    fm_strlcpy(it->license, "Pixabay Content License", sizeof it->license);
    fm_strlcpy(it->thumb, thumb, sizeof it->thumb);
    fm_strlcpy(it->full, full, sizeof it->full);
    if (psrc_url_copy(json_str(json_get(h, "pageURL"), ""), pg, sizeof pg)) fm_strlcpy(it->page, pg, sizeof it->page);
    else fm_snprintf(it->page, sizeof it->page, "https://pixabay.com/photos/id-%s/", it->id);
    double w = json_num(json_get(h, "imageWidth"), 0), ht = json_num(json_get(h, "imageHeight"), 0);
    if (w > 0 && ht > 0 && w < 1e6 && ht < 1e6) {
      it->width = (int)w;
      it->height = (int)ht;
      psrc_fit_width(&it->width, &it->height, PIXABAY_FULL_W);
    }
  }
  double total = json_num(json_get(root, "totalHits"), 0);
  if (total > PIXABAY_MAX_HITS) total = PIXABAY_MAX_HITS;
  if (seen > 0 && (double)page * PSRC_PAGE_SIZE < total) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr pixabay_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                            volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!psrc_have_key("Pixabay", c->key_pixabay, out->error, sizeof out->error)) return FM_ERR_ACCESS;
  int page = psrc_page_num(token, 1, 1, PIXABAY_MAX_HITS / PSRC_PAGE_SIZE + 1);
  char url[1600];
  psrc_pixabay_url(c, query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, NULL, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    bool key = r.status == 401 || r.status == 403 || (r.status == 400 && r.data && fm_stristr((const char *)r.data, "key"));
    e = psrc_http_error("Pixabay", key ? 401 : r.status, true, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_pixabay_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

const FmPsrc g_psrc_pixabay = {
  "pixabay", "Pixabay", IC_GRID, PSRC_SEARCH | PSRC_NEEDKEY | PSRC_BROWSE, pixabay_search, NULL,
  "Free stock photos \xC2\xB7 needs a free key from pixabay.com/api/docs",
  NULL, NULL,
};
