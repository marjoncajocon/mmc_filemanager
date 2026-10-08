/* fasrc_freesound.c -- Freesound (freesound.org): 600,000+ Creative Commons
** sound effects, field recordings and samples. Needs a free API key
** (freesound.org/apiv2/apply), sent as token=.
**
** Design decisions:
**   - Token authentication only reaches the previews; the original files
**     need an OAuth2 sign-in per user. The high-quality MP3 preview
**     (previews["preview-hq-mp3"], ~128 kbps) is what plays and what
**     Download saves, which suits sound effects; the page links to the
**     original.
**   - Sounds are TRACKs: title = the sound's name, artist = the uploader,
**     album = the first tags, art = the waveform picture Freesound draws.
**   - Search only: Freesound has no charts worth browsing without a query.
**     Pages are Freesound's page numbers; "next" is null on the last one.
*/
#include "fasrc_int.h"

void asrc_freesound_url(const FmAsrcConf *c, const char *q, int page, char *out, size_t cap) {
  char key[100], ke[300], qq[256], qe[800];
  fm_strlcpy(key, c ? c->key_freesound : "", sizeof key);
  net_urlencode(key, ke, sizeof ke);
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://freesound.org/apiv2/search/text/?query=%s&token=%s"
              "&fields=id,name,username,duration,previews,license,images,url,created,tags"
              "&page_size=%d&page=%d", qe, ke, ASRC_PAGE_SIZE, page < 1 ? 1 : page);
}

static void first_tags(const FmJsonNode *tags, char *out, size_t cap) {
  char tmp[200];
  tmp[0] = 0;
  int n = 0;
  for (const FmJsonNode *t = json_first(tags); t && n < 4; t = json_next(t), n++) {
    if (n) fm_strlcat(tmp, ", ", sizeof tmp);
    fm_strlcat(tmp, json_str(t, ""), sizeof tmp);
  }
  asrc_text(tmp, out, cap);
}

FmErr asrc_freesound_parse(const char *json, size_t len, int page, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Freesound sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *res = json_get(root, "results");
  if (!res || res->type != JSON_ARR) {
    const char *d = json_str(json_get(root, "detail"), "");
    if (*d) fm_snprintf(out->error, sizeof out->error, "Freesound: %.200s", d);
    else fm_strlcpy(out->error, "Freesound sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  int seen = 0;
  for (const FmJsonNode *s = json_first(res); s && seen < ASRC_MAX_ITEMS; s = json_next(s), seen++) {
    double id = json_num(json_get(s, "id"), 0);
    char url[1024];
    if (id <= 0 || id > 9e15) continue;
    const FmJsonNode *pv = json_get(s, "previews");
    if (!psrc_url_copy(json_str(json_get(pv, "preview-hq-mp3"), ""), url, sizeof url) &&
        !psrc_url_copy(json_str(json_get(pv, "preview-lq-mp3"), ""), url, sizeof url))
      continue;
    FmAsrcItem *it = asrc_item_new(out, "freesound", AITEM_TRACK);
    fm_snprintf(it->id, sizeof it->id, "%.0f", id);
    asrc_text(json_str(json_get(s, "name"), ""), it->title, sizeof it->title);
    asrc_text(json_str(json_get(s, "username"), ""), it->artist, sizeof it->artist);
    first_tags(json_get(s, "tags"), it->album, sizeof it->album);
    psrc_url_copy(json_str(json_path(s, "images.waveform_m"), ""), it->art, sizeof it->art);
    fm_strlcpy(it->url, url, sizeof it->url);
    if (!psrc_url_copy(json_str(json_get(s, "url"), ""), it->page, sizeof it->page))
      fm_snprintf(it->page, sizeof it->page, "https://freesound.org/s/%s/", it->id);
    fm_strlcpy(it->codec, "MP3", sizeof it->codec);
    asrc_cc_license(json_str(json_get(s, "license"), ""), it->license, sizeof it->license);
    double d = json_num(json_get(s, "duration"), 0);
    it->duration = d > 0 && d < 1e7 ? d : 0;
    vsrc_iso_date(json_str(json_get(s, "created"), ""), it->published, sizeof it->published);
  }
  const FmJsonNode *next = json_get(root, "next");
  if (next && next->type == JSON_STR && seen > 0 && page * ASRC_PAGE_SIZE < ASRC_MAX_OFFSET)
    fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr fs_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type a sound to search for (rain, door, birds...)", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  if (!psrc_have_key("Freesound", c->key_freesound, out->error, sizeof out->error)) return FM_ERR_ACCESS;
  int page = psrc_page_num(token, 1, 1, ASRC_MAX_OFFSET / ASRC_PAGE_SIZE);
  char url[1600];
  asrc_freesound_url(c, query, page, url, sizeof url);
  FmNetResp r;
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Freesound", r.status, true, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_freesound_parse((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No sounds found", sizeof out->error);
  return e;
}

const FmAsrc g_asrc_freesound = {
  "freesound", "Freesound", IC_VOLUME, ASRC_SEARCH | ASRC_NEEDKEY,
  fs_search, NULL, NULL, NULL, NULL,
  "Creative Commons sound effects and field recordings \xC2\xB7 free key (freesound.org/apiv2/apply)",
};
