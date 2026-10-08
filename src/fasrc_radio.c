/* fasrc_radio.c -- Radio Browser (radio-browser.info): ~60,000 live radio
** stations, community maintained. No key; a descriptive User-Agent
** ("mmcfm/0.1", fnet sends it) is all the API asks for.
**
** Design decisions:
**   - Several mirrors serve the same database (de1, de2, fi1, nl1; on
**     2026-10-08 only de1/de2 answered from here). The last mirror that
**     worked is remembered process-wide (the one mutable global of the audio
**     adapters, behind a spin lock); a transport error or a 5xx moves on to
**     the next one, so a dead mirror costs one failed connect, once.
**   - Every list is one stations/search call, categories included ("Top
**     clicked" = order=clickcount, a tag = tag=jazz&tagExact=true, rather
**     than stations/topclick or stations/bytag): only search filters by
**     codec on the server, and paging (offset) works the same everywhere.
**   - Without FFmpeg only MP3 plays (the built-in decoders; AAC+ is half of
**     the directory). The search then asks for codec=MP3, so a page holds
**     40 stations that all play instead of 40 rows where half end in an
**     error; the about text and asrc_stream's message say FFmpeg brings the
**     rest. An AAC station that reaches resolve anyway (a favourite saved
**     with FFmpeg present) gets "This station uses AAC -- install FFmpeg".
**     HLS stations (playlists of segments) are left out: the stream reader
**     plays one continuous HTTP body.
**   - Categories are a fixed list (charts, the user's country, popular
**     genres picked from tags?order=stationcount on 2026-10-08 minus the
**     generic "music"/"radio"/"fm"): categories() has no cancel or error
**     path, so it must not wait on the network.
**   - resolve() calls json/url/<uuid>: the API asks apps to count a click
**     when a station is played, and it returns the current stream address.
**     When the click call fails the address from the search is used.
*/
#include "fasrc_int.h"
#include "fsdl.h"

static const char *const kMirrors[] = { "de1", "de2", "fi1", "nl1" };
static SDL_SpinLock g_mirror_lock;
static int g_mirror;                               /* index into kMirrors, under g_mirror_lock */

int asrc_radio_mirror_count(void) { return FM_COUNT(kMirrors); }

const char *asrc_radio_mirror(int i) {
  int n = FM_COUNT(kMirrors);
  SDL_AtomicLock(&g_mirror_lock);
  int m = g_mirror;
  SDL_AtomicUnlock(&g_mirror_lock);
  return kMirrors[((m + i) % n + n) % n];
}

static void mirror_worked(const char *name) {
  for (int i = 0; i < FM_COUNT(kMirrors); i++)
    if (!strcmp(kMirrors[i], name)) {
      SDL_AtomicLock(&g_mirror_lock);
      g_mirror = i;
      SDL_AtomicUnlock(&g_mirror_lock);
    }
}

/* GET https://<mirror>.api.radio-browser.info/json/<path>, mirror by mirror */
static FmErr rb_get(const char *path, FmNetResp *r, char *err, size_t errcap, volatile int *cancel) {
  char url[1400];
  FmErr e = FM_ERR_IO;
  SDL_AtomicLock(&g_mirror_lock);
  int start = g_mirror;                            /* one consistent order for this request */
  SDL_AtomicUnlock(&g_mirror_lock);
  for (int i = 0; i < FM_COUNT(kMirrors); i++) {
    const char *m = kMirrors[(start + i) % FM_COUNT(kMirrors)];
    fm_snprintf(url, sizeof url, "https://%s.api.radio-browser.info/json/%s", m, path);
    e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, r, err, errcap, cancel);
    if (e == FM_ERR_CANCEL || e == FM_ERR_UNSUPPORTED) return e;
    if (e == FM_OK && r->status < 500) {
      mirror_worked(m);
      if (errcap) err[0] = 0;                      /* an earlier mirror's failure is history */
      return FM_OK;
    }
    if (e == FM_OK) {
      psrc_http_error("Radio Browser", r->status, false, err, errcap);
      net_resp_free(r);
      e = FM_ERR_IO;
    }
  }
  return e;
}

/* ---- search and categories ---------------------------------------------------- */

static const struct { const char *tag, *name; } kTags[] = {
  { "pop", "Pop" },         { "rock", "Rock" },           { "news", "News" },
  { "jazz", "Jazz" },       { "classical", "Classical" }, { "dance", "Dance" },
  { "electronic", "Electronic" }, { "talk", "Talk" },     { "oldies", "Oldies" },
  { "80s", "80s" },         { "hits", "Hits" },           { "chillout", "Chillout" },
  { "country", "Country" }, { "ambient", "Ambient" },     { "hip hop", "Hip hop" },
  { "lounge", "Lounge" },   { "reggae", "Reggae" },       { "blues", "Blues" },
  { "latin", "Latin" },     { "metal", "Metal" },         { "soul", "Soul" },
  { "sports", "Sports" },   { "christian", "Christian" }, { "folk", "Folk" },
};

static int rb_categories(const FmAsrcConf *c, FmAsrcCat *out, int max) {
  static const struct { const char *id, *name; } kCharts[] = {
    { "top", "Top clicked" }, { "votes", "Top voted" }, { "trending", "Trending" },
  };
  int n = 0;
  for (int i = 0; i < FM_COUNT(kCharts) && n < max; i++, n++) {
    fm_strlcpy(out[n].id, kCharts[i].id, sizeof out[n].id);
    fm_strlcpy(out[n].name, kCharts[i].name, sizeof out[n].name);
  }
  if (n < max && c && strlen(c->country) == 2) {
    fm_snprintf(out[n].id, sizeof out[n].id, "country:%s", c->country);
    fm_snprintf(out[n].name, sizeof out[n].name, "Stations in %s", c->country);
    n++;
  }
  for (int i = 0; i < FM_COUNT(kTags) && n < max; i++, n++) {
    fm_snprintf(out[n].id, sizeof out[n].id, "tag:%s", kTags[i].tag);
    fm_strlcpy(out[n].name, kTags[i].name, sizeof out[n].name);
  }
  return n;
}

bool asrc_radio_path(const FmAsrcConf *c, const char *query, const char *cat, int offset, char *out, size_t cap) {
  char what[400], enc[1200];
  bool tag = false;
  const char *order = "clickcount";
  if (cap) out[0] = 0;
  what[0] = 0;
  if (query && *query) {
    if (!fm_strnicmp(query, "tag:", 4)) { tag = true; query += 4; }
    while (*query == ' ') query++;
    if (!*query) return false;
    fm_strlcpy(what, query, sizeof what);
  } else if (cat && *cat) {
    if (!strcmp(cat, "top")) order = "clickcount";
    else if (!strcmp(cat, "votes")) order = "votes";
    else if (!strcmp(cat, "trending")) order = "clicktrend";
    else if (!strncmp(cat, "tag:", 4) && cat[4]) { tag = true; fm_strlcpy(what, cat + 4, sizeof what); }
    else if (!strncmp(cat, "country:", 8) && strlen(cat + 8) == 2) {
      char cc[3] = { cat[8], cat[9], 0 };
      if (!((cc[0] >= 'A' && cc[0] <= 'Z') && (cc[1] >= 'A' && cc[1] <= 'Z'))) return false;
      fm_snprintf(out, cap, "stations/search?countrycode=%s&order=clickcount&reverse=true&limit=%d&offset=%d"
                  "&hidebroken=true%s", cc, ASRC_PAGE_SIZE, offset, c && c->have_ffmpeg ? "" : "&codec=MP3");
      return true;
    } else return false;
  } else return false;
  net_urlencode(what, enc, sizeof enc);
  int k;
  if (what[0])
    k = fm_snprintf(out, cap, "stations/search?%s=%s%s&order=clickcount&reverse=true&limit=%d&offset=%d"
                    "&hidebroken=true%s", tag ? "tag" : "name", enc, tag ? "&tagExact=true" : "", ASRC_PAGE_SIZE,
                    offset, c && c->have_ffmpeg ? "" : "&codec=MP3");
  else
    k = fm_snprintf(out, cap, "stations/search?order=%s&reverse=true&limit=%d&offset=%d&hidebroken=true%s", order,
                    ASRC_PAGE_SIZE, offset, c && c->have_ffmpeg ? "" : "&codec=MP3");
  if (k < 0 || (size_t)k >= cap) { out[0] = 0; return false; }
  return true;
}

/* "easy listening,jazz,smooth jazz" -> "easy listening, jazz, smooth jazz" */
static void tags_text(const char *tags, char *out, size_t cap) {
  char tmp[400];
  size_t o = 0;
  for (const char *s = tags; *s && o + 3 < sizeof tmp; s++) {
    if (*s == ',') {
      while (s[1] == ' ') s++;
      if (s[1] && o) { tmp[o++] = ','; tmp[o++] = ' '; }
      continue;
    }
    tmp[o++] = *s;
  }
  tmp[o] = 0;
  if (o >= cap) {                                  /* cut back to the last whole tag */
    tmp[cap - 1] = 0;
    char *comma = strrchr(tmp, ',');
    if (comma) *comma = 0;
  }
  psrc_copy(out, tmp, cap);
}

FmErr asrc_radio_parse(const FmAsrcConf *c, const char *json, size_t len, int offset, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK || json_root(&j)->type != JSON_ARR) {
    if (json) json_free(&j);
    fm_strlcpy(out->error, "Radio Browser sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  int seen = 0;
  for (const FmJsonNode *s = json_first(json_root(&j)); s && seen < ASRC_MAX_ITEMS; s = json_next(s), seen++) {
    const char *id = json_str(json_get(s, "stationuuid"), "");
    char url[1024], codec[16];
    if (!psrc_id_ok(id, "-", 64)) continue;
    const char *u = json_str(json_get(s, "url_resolved"), "");
    if (!*u) u = json_str(json_get(s, "url"), "");
    if (!psrc_url_copy(u, url, sizeof url)) continue;
    if (json_num(json_get(s, "hls"), 0) != 0) continue;
    asrc_codec_norm(json_str(json_get(s, "codec"), ""), codec, sizeof codec);
    if (c && !c->have_ffmpeg && !asrc_codec_builtin(codec)) continue;
    FmAsrcItem *it = asrc_item_new(out, "radio", AITEM_STATION);
    fm_strlcpy(it->id, id, sizeof it->id);
    asrc_text(json_str(json_get(s, "name"), ""), it->title, sizeof it->title);
    if (!it->title[0]) fm_strlcpy(it->title, "Unnamed station", sizeof it->title);
    psrc_copy(it->artist, json_str(json_get(s, "country"), ""), sizeof it->artist);
    tags_text(json_str(json_get(s, "tags"), ""), it->album, sizeof it->album);
    fm_strlcpy(it->url, url, sizeof it->url);
    psrc_url_copy(json_str(json_get(s, "favicon"), ""), it->art, sizeof it->art);
    psrc_url_copy(json_str(json_get(s, "homepage"), ""), it->page, sizeof it->page);
    fm_strlcpy(it->codec, codec, sizeof it->codec);
    double br = json_num(json_get(s, "bitrate"), 0);
    it->bitrate = br > 0 && br < 10000 ? (int)br : 0;
    double clicks = json_num(json_get(s, "clickcount"), -1);
    it->plays = clicks >= 0 && clicks < 9e15 ? (i64)clicks : -1;
  }
  if (seen >= ASRC_PAGE_SIZE && offset + ASRC_PAGE_SIZE <= ASRC_MAX_OFFSET)
    fm_snprintf(out->next, sizeof out->next, "%d", offset + ASRC_PAGE_SIZE);
  json_free(&j);
  return FM_OK;
}

static FmErr rb_list(const FmAsrcConf *c, const char *query, const char *cat, const char *token, FmAsrcPage *out,
                     volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  int offset = psrc_page_num(token, 0, 0, ASRC_MAX_OFFSET);
  char path[1400];
  if (!asrc_radio_path(c, query, cat, offset, path, sizeof path)) {
    fm_strlcpy(out->error, cat ? "Unknown category" : "Type a station name to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  FmNetResp r;
  FmErr e = rb_get(path, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Radio Browser", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_radio_parse(c, (const char *)r.data, r.len, offset, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && offset == 0)
    fm_strlcpy(out->error, c && !c->have_ffmpeg ? "No MP3 stations found (FFmpeg adds AAC and Ogg stations)"
                                                : "No stations found", sizeof out->error);
  return e;
}

static FmErr rb_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  return rb_list(c, query, NULL, token, out, cancel);
}

static FmErr rb_browse(const FmAsrcConf *c, const char *cat, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  return rb_list(c, NULL, cat && *cat ? cat : "top", token, out, cancel);
}

/* ---- resolve ------------------------------------------------------------------ */

bool asrc_radio_parse_click(const char *json, size_t len, char *url, size_t cap) {
  FmJson j;
  if (cap) url[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  const FmJsonNode *root = json_root(&j);
  bool ok = json_bool(json_get(root, "ok"), false) &&
            psrc_url_copy(json_str(json_get(root, "url"), ""), url, cap);
  json_free(&j);
  return ok;
}

static FmErr rb_resolve(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                        volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!psrc_id_ok(item->id, "-", 64)) {
    fm_strlcpy(err, "Not a Radio Browser station", errcap);
    return FM_ERR_NOT_FOUND;
  }
  /* no network for a station that cannot play here anyway */
  if (asrc_codec_blocked(c, item, item->codec, err, errcap)) return FM_ERR_UNSUPPORTED;
  char path[128];
  fm_snprintf(path, sizeof path, "url/%s", item->id);
  FmNetResp r;
  char e2[160];
  FmErr e = rb_get(path, &r, e2, sizeof e2, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", errcap); return e; }
  if (e == FM_OK) {
    if (r.status == 200) asrc_radio_parse_click((const char *)r.data, r.len, out->url, sizeof out->url);
    net_resp_free(&r);
  }
  if (!out->url[0] && !psrc_url_copy(item->url, out->url, sizeof out->url)) {
    fm_strlcpy(err, e != FM_OK ? e2 : "Radio Browser has no address for this station", errcap);
    return e != FM_OK ? e : FM_ERR_NOT_FOUND;
  }
  fm_strlcpy(out->codec, item->codec, sizeof out->codec);
  out->live = true;
  return FM_OK;
}

const FmAsrc g_asrc_radio = {
  "radio", "Radio Browser", IC_NETWORK, ASRC_SEARCH | ASRC_BROWSE | ASRC_LIVE,
  rb_search, rb_categories, rb_browse, rb_resolve, NULL,
  "60,000 live radio stations \xC2\xB7 no key needed (MP3 built in, AAC with FFmpeg)",
};
