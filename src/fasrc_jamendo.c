/* fasrc_jamendo.c -- Jamendo (jamendo.com): 600,000 Creative Commons tracks
** by independent artists. Needs a free client_id (devportal.jamendo.com).
**
** Design decisions:
**   - Jamendo answers HTTP 200 even for a bad or missing client_id; the
**     verdict is in "headers" ({"status":"failed","code":5,"error_message":
**     "...Invalid Client Id..."} on 2026-10-08). Code 5 becomes the usual
**     "did not accept the API key -- check it in Settings"; other failures
**     show Jamendo's own message.
**   - audioformat=mp32 asks for the VBR high-quality MP3 stream (the
**     default mp31 is 96 kbps). That stream is the item's url: no resolve.
**   - Every track carries its licence URL; it becomes "CC BY-NC-SA 3.0"
**     style text, and downloads get the credit file the licences ask for.
**   - Browse: weekly / monthly / all-time popularity, newest releases, then
**     Jamendo's main genre tags (fuzzytags, so "rock" includes "poprock").
*/
#include "fasrc_int.h"

static const struct { const char *id, *name; } kCats[] = {
  { "week", "Popular this week" }, { "month", "Popular this month" }, { "total", "All-time favourites" },
  { "new", "New releases" },
  { "tag:rock", "Rock" },          { "tag:electronic", "Electronic" }, { "tag:pop", "Pop" },
  { "tag:hiphop", "Hip hop" },     { "tag:jazz", "Jazz" },             { "tag:classical", "Classical" },
  { "tag:ambient", "Ambient" },    { "tag:lounge", "Lounge" },         { "tag:chillout", "Chillout" },
  { "tag:metal", "Metal" },        { "tag:folk", "Folk" },             { "tag:soundtrack", "Soundtrack" },
  { "tag:world", "World" },        { "tag:acoustic", "Acoustic" },     { "tag:relaxation", "Relaxation" },
};

static int ja_categories(const FmAsrcConf *c, FmAsrcCat *out, int max) {
  FM_UNUSED(c);
  int n = 0;
  for (int i = 0; i < FM_COUNT(kCats) && n < max; i++, n++) {
    fm_strlcpy(out[n].id, kCats[i].id, sizeof out[n].id);
    fm_strlcpy(out[n].name, kCats[i].name, sizeof out[n].name);
  }
  return n;
}

void asrc_jamendo_url(const FmAsrcConf *c, const char *q, const char *cat, int offset, char *out, size_t cap) {
  char key[80], ke[240], what[300], qe[900];
  fm_strlcpy(key, c ? c->key_jamendo : "", sizeof key);
  net_urlencode(key, ke, sizeof ke);
  what[0] = 0;
  if (q && *q) {
    fm_strlcpy(what, q, sizeof what);
    net_urlencode(what, qe, sizeof qe);
    fm_snprintf(what, sizeof what, "&search=%.280s&order=relevance", qe);
  } else if (cat && !strncmp(cat, "tag:", 4) && cat[4]) {
    char t[64];
    fm_strlcpy(t, cat + 4, sizeof t);
    net_urlencode(t, qe, sizeof qe);
    fm_snprintf(what, sizeof what, "&fuzzytags=%.200s&order=popularity_month", qe);
  } else if (cat && !strcmp(cat, "new")) {
    fm_strlcpy(what, "&order=releasedate_desc", sizeof what);
  } else if (cat && (!strcmp(cat, "month") || !strcmp(cat, "total"))) {
    fm_snprintf(what, sizeof what, "&order=popularity_%s", cat);
  } else {
    fm_strlcpy(what, "&order=popularity_week", sizeof what);
  }
  fm_snprintf(out, cap, "https://api.jamendo.com/v3.0/tracks/?client_id=%s&format=json&limit=%d&offset=%d"
              "&audioformat=mp32&imagesize=300%s", ke, ASRC_PAGE_SIZE, offset, what);
}

FmErr asrc_jamendo_parse(const char *json, size_t len, int offset, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Jamendo sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *head = json_get(root, "headers");
  const FmJsonNode *res = json_get(root, "results");
  if (strcmp(json_str(json_get(head, "status"), "success"), "success") || !res || res->type != JSON_ARR) {
    int code = (int)json_num(json_get(head, "code"), 0);
    FmErr e = FM_ERR_FORMAT;
    if (code == 5) {
      fm_strlcpy(out->error, "Jamendo did not accept the API key \xE2\x80\x94 check it in Settings", sizeof out->error);
      e = FM_ERR_ACCESS;
    } else {
      const char *m = json_str(json_get(head, "error_message"), "");
      if (*m) {
        fm_snprintf(out->error, sizeof out->error, "Jamendo: %.200s", m);
        e = FM_ERR_IO;
      } else fm_strlcpy(out->error, "Jamendo sent a reply this app does not understand", sizeof out->error);
    }
    json_free(&j);
    return e;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  int seen = 0;
  for (const FmJsonNode *t = json_first(res); t && seen < ASRC_MAX_ITEMS; t = json_next(t), seen++) {
    const char *id = json_str(json_get(t, "id"), "");
    char url[1024];
    if (!psrc_id_ok(id, "", 20)) continue;
    if (!psrc_url_copy(json_str(json_get(t, "audio"), ""), url, sizeof url)) continue;
    FmAsrcItem *it = asrc_item_new(out, "jamendo", AITEM_TRACK);
    fm_strlcpy(it->id, id, sizeof it->id);
    asrc_text(json_str(json_get(t, "name"), ""), it->title, sizeof it->title);
    asrc_text(json_str(json_get(t, "artist_name"), ""), it->artist, sizeof it->artist);
    asrc_text(json_str(json_get(t, "album_name"), ""), it->album, sizeof it->album);
    if (!psrc_url_copy(json_str(json_get(t, "image"), ""), it->art, sizeof it->art))
      psrc_url_copy(json_str(json_get(t, "album_image"), ""), it->art, sizeof it->art);
    fm_strlcpy(it->url, url, sizeof it->url);
    psrc_url_copy(json_str(json_get(t, "shareurl"), ""), it->page, sizeof it->page);
    fm_strlcpy(it->codec, "MP3", sizeof it->codec);
    asrc_cc_license(json_str(json_get(t, "license_ccurl"), ""), it->license, sizeof it->license);
    double d = json_num(json_get(t, "duration"), 0);
    it->duration = d > 0 && d < 1e7 ? d : 0;
    vsrc_iso_date(json_str(json_get(t, "releasedate"), ""), it->published, sizeof it->published);
  }
  if (seen >= ASRC_PAGE_SIZE && offset + ASRC_PAGE_SIZE <= ASRC_MAX_OFFSET)
    fm_snprintf(out->next, sizeof out->next, "%d", offset + ASRC_PAGE_SIZE);
  json_free(&j);
  return FM_OK;
}

static FmErr ja_list(const FmAsrcConf *c, const char *query, const char *cat, const char *token, FmAsrcPage *out,
                     volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!psrc_have_key("Jamendo", c->key_jamendo, out->error, sizeof out->error)) return FM_ERR_ACCESS;
  int offset = psrc_page_num(token, 0, 0, ASRC_MAX_OFFSET);
  char url[1600];
  asrc_jamendo_url(c, query, cat, offset, url, sizeof url);
  FmNetResp r;
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Jamendo", r.status, true, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_jamendo_parse((const char *)r.data, r.len, offset, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && offset == 0) fm_strlcpy(out->error, "No tracks found", sizeof out->error);
  return e;
}

static FmErr ja_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  if (!query || !*query) {
    out->next[0] = 0;
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  return ja_list(c, query, NULL, token, out, cancel);
}

static FmErr ja_browse(const FmAsrcConf *c, const char *cat, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  bool known = !cat || !*cat;
  for (int i = 0; i < FM_COUNT(kCats) && !known; i++) known = !strcmp(cat, kCats[i].id);
  if (!known) {
    out->next[0] = 0;
    fm_strlcpy(out->error, "Unknown category", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  return ja_list(c, NULL, cat, token, out, cancel);
}

const FmAsrc g_asrc_jamendo = {
  "jamendo", "Jamendo", IC_MUSIC, ASRC_SEARCH | ASRC_BROWSE | ASRC_NEEDKEY,
  ja_search, ja_categories, ja_browse, NULL, NULL,
  "Creative Commons music by independent artists \xC2\xB7 free key (devportal.jamendo.com)",
};
