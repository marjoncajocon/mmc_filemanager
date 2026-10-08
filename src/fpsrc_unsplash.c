/* fpsrc_unsplash.c -- Unsplash: free high-quality photos. Needs a free
** access key (unsplash.com/developers), sent as "Client-ID <key>".
**
** Design decisions:
**   - Unsplash's API terms: images are hotlinked from their own URLs (never
**     re-hosted; our cache is a private per-user copy for the viewer), the
**     photographer and Unsplash are credited (the .txt saved next to a
**     download, and UTM parameters on the page link), and a download is
**     reported to the API. The `saved` hook does the report: a GET of
**     /photos/<id>/download with the key, which is what
**     links.download_location points to (minus the search's ixid tag).
**   - Sizes: thumb = urls.small (400 px), full = urls.raw resized by the
**     image CDN to 1920 px JPEG (urls.regular is only 1080), Download =
**     urls.full (full resolution JPEG). fm=jpg keeps WebP away: the viewer
**     does not decode it.
**   - An empty query browses the editorial feed (/photos, PSRC_BROWSE).
**   - Rate limiting answers 403 "Rate Limit Exceeded", not 429; the demo
**     tier allows 50 requests an hour.
*/
#include "fpsrc_int.h"

#define UNSPLASH_FULL_W 1920
#define UNSPLASH_UTM "utm_source=mmcfm&utm_medium=referral"

void psrc_unsplash_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap) {
  char qq[256], qe[800];
  page = FM_CLAMP(page, 1, 1000);
  if (!q || !*q) {
    fm_snprintf(out, cap, "https://api.unsplash.com/photos?per_page=%d&page=%d", PSRC_PAGE_SIZE, page);
    return;
  }
  fm_strlcpy(qq, q, sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://api.unsplash.com/search/photos?query=%s&per_page=%d&page=%d&content_filter=%s",
              qe, PSRC_PAGE_SIZE, page, c && c->safe_search ? "high" : "low");
}

bool psrc_unsplash_track_url(const FmPsrcItem *item, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!item || !psrc_id_ok(item->id, "-_", 32)) return false;
  fm_snprintf(out, cap, "https://api.unsplash.com/photos/%s/download", item->id);
  return true;
}

/* url + ("?" or "&") + extra, false when it does not fit */
static bool add_query(const char *url, const char *extra, char *out, size_t cap) {
  int n = fm_snprintf(out, cap, "%s%c%s", url, strchr(url, '?') ? '&' : '?', extra);
  if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return false; }
  return true;
}

FmErr psrc_unsplash_parse(const char *json, size_t len, int page, FmPsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Unsplash sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *list = root && root->type == JSON_ARR ? root : json_get(root, "results");
  if (!list || list->type != JSON_ARR) {
    const char *m = json_str(json_at(json_get(root, "errors"), 0), "");
    if (*m) fm_snprintf(out->error, sizeof out->error, "Unsplash: %.200s", m);
    else fm_strlcpy(out->error, "Unsplash sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  int seen = 0;
  for (const FmJsonNode *p = json_first(list); p && seen < PSRC_MAX_ITEMS; p = json_next(p), seen++) {
    const char *id = json_str(json_get(p, "id"), "");
    const FmJsonNode *urls = json_get(p, "urls");
    char thumb[512], raw[1024], full[1024], orig[1024], pg[512];
    if (!psrc_id_ok(id, "-_", 32)) continue;
    if (!psrc_url_copy(json_str(json_get(urls, "small"), ""), thumb, sizeof thumb)) continue;
    if (!psrc_url_copy(json_str(json_get(urls, "raw"), ""), raw, sizeof raw) ||
        !add_query(raw, "w=1920&fit=max&fm=jpg&q=85", full, sizeof full)) {
      if (!psrc_url_copy(json_str(json_get(urls, "regular"), ""), full, sizeof full))
        fm_strlcpy(full, thumb, sizeof full);
    }
    FmPsrcItem *it = psrc_page_add(out);
    fm_strlcpy(it->source, "unsplash", sizeof it->source);
    fm_strlcpy(it->id, id, sizeof it->id);
    const char *t = json_str(json_get(p, "description"), "");
    if (!*t) t = json_str(json_get(p, "alt_description"), "");
    psrc_copy(it->title, t, sizeof it->title);
    if (it->title[0] >= 'a' && it->title[0] <= 'z') it->title[0] = (char)(it->title[0] - 'a' + 'A');
    psrc_copy(it->author, json_str(json_path(p, "user.name"), ""), sizeof it->author);
    fm_strlcpy(it->license, "Unsplash License", sizeof it->license);
    fm_strlcpy(it->thumb, thumb, sizeof it->thumb);
    fm_strlcpy(it->full, full, sizeof it->full);
    if (psrc_url_copy(json_str(json_get(urls, "full"), ""), orig, sizeof orig) && strcmp(orig, full))
      fm_strlcpy(it->original, orig, sizeof it->original);
    if (!psrc_url_copy(json_str(json_path(p, "links.html"), ""), pg, sizeof pg) ||
        !add_query(pg, UNSPLASH_UTM, it->page, sizeof it->page))
      fm_snprintf(it->page, sizeof it->page, "https://unsplash.com/photos/%s?" UNSPLASH_UTM, id);
    double w = json_num(json_get(p, "width"), 0), h = json_num(json_get(p, "height"), 0);
    if (w > 0 && h > 0 && w < 1e6 && h < 1e6) {
      it->width = (int)w;
      it->height = (int)h;
      psrc_fit_width(&it->width, &it->height, UNSPLASH_FULL_W);
    }
    it->color = psrc_hex_color(json_str(json_get(p, "color"), ""));
  }
  if (root->type == JSON_ARR) {
    if (seen >= PSRC_PAGE_SIZE && page < 1000) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  } else {
    double pages = json_num(json_get(root, "total_pages"), 0);
    if (seen > 0 && page < pages && page < 1000) fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  }
  json_free(&j);
  return FM_OK;
}

static bool key_header(const FmPsrcConf *c, char *hdr, size_t cap, char *err, size_t errcap) {
  if (!psrc_have_key("Unsplash", c->key_unsplash, err, errcap)) return false;
  if (!psrc_id_ok(c->key_unsplash, "-_.", 95)) {
    fm_strlcpy(err, "The Unsplash key in Settings is not valid (check for typos)", errcap);
    return false;
  }
  fm_snprintf(hdr, cap, "Authorization: Client-ID %s\r\nAccept-Version: v1\r\n", c->key_unsplash);
  return true;
}

static FmErr unsplash_search(const FmPsrcConf *c, const char *query, const char *token, FmPsrcPage *out,
                             volatile int *cancel) {
  char url[1200], hdr[200];
  out->next[0] = out->error[0] = 0;
  if (!key_header(c, hdr, sizeof hdr, out->error, sizeof out->error)) return FM_ERR_ACCESS;
  int page = psrc_page_num(token, 1, 1, 1000);
  psrc_unsplash_url(c, query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = psrc_http_get(url, hdr, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    if (r.status == 403 && r.data && fm_stristr((const char *)r.data, "rate limit"))
      e = psrc_http_error("Unsplash", 429, true, out->error, sizeof out->error);
    else e = psrc_http_error("Unsplash", r.status, true, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = psrc_unsplash_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No photos found", sizeof out->error);
  return e;
}

static void unsplash_saved(const FmPsrcConf *c, const FmPsrcItem *item, volatile int *cancel) {
  char url[256], hdr[200], err[256];
  if (!key_header(c, hdr, sizeof hdr, err, sizeof err) || !psrc_unsplash_track_url(item, url, sizeof url)) return;
  FmNetResp r;
  if (psrc_http_get(url, hdr, &r, err, sizeof err, cancel) == FM_OK) net_resp_free(&r);
}

const FmPsrc g_psrc_unsplash = {
  "unsplash", "Unsplash", IC_SLIDE, PSRC_SEARCH | PSRC_NEEDKEY | PSRC_BROWSE, unsplash_search, NULL,
  "Beautiful free photos \xC2\xB7 needs a free key from unsplash.com/developers",
  NULL, unsplash_saved,
};
