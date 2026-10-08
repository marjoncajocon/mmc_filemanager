/* fasrc_audius.c -- Audius (audius.co): a free music streaming network of
** independent artists. No key; every call names the app (app_name=mmcfm).
**
** Design decisions:
**   - api.audius.co answers itself (its host list, GET /, held only
**     "https://api.audius.co" on 2026-10-08), so there is no discovery step.
**   - A track's stream is /v1/tracks/<id>/stream: a redirect to a content
**     node serving the MP3 with byte ranges. That stable address is the
**     item's url from the start; the signed node URL in the search reply
**     ("stream.url") carries a timestamp and would go stale in a list kept
**     open for an hour. Tracks marked is_streamable=false answer 404 there
**     and gated (paid / follower-only) ones need a wallet: both are left out.
**   - Replies are big (~6 KB per track, user profiles inlined): 40 tracks
**     are ~250 KB, playlists with their first tracks inlined ~40 KB each,
**     so playlists are asked for 20 at a time.
**   - Browse: trending this week / month / all time, trending playlists
**     (ALBUM containers; children() lists their tracks), then genres as
**     Audius spells them ("Hip-Hop/Rap", "Lo-Fi").
*/
#include "fasrc_int.h"

#define AUDIUS_API "https://api.audius.co/v1"
#define AUDIUS_PLAYLISTS 20

static const char *const kGenres[] = {
  "Electronic", "Hip-Hop/Rap", "Lo-Fi", "Alternative", "Pop", "Rock", "R&B/Soul", "Ambient", "House",
  "Techno", "Dubstep", "Trap", "Jazz", "Classical", "Acoustic", "Folk", "Country", "Experimental",
  "Soundtrack", "Latin", "Reggae", "Metal", "Funk", "World",
};

static int au_categories(const FmAsrcConf *c, FmAsrcCat *out, int max) {
  static const struct { const char *id, *name; } kFixed[] = {
    { "trending", "Trending this week" }, { "trending:month", "Trending this month" },
    { "trending:allTime", "All-time hits" }, { "playlists", "Trending playlists" },
  };
  FM_UNUSED(c);
  int n = 0;
  for (int i = 0; i < FM_COUNT(kFixed) && n < max; i++, n++) {
    fm_strlcpy(out[n].id, kFixed[i].id, sizeof out[n].id);
    fm_strlcpy(out[n].name, kFixed[i].name, sizeof out[n].name);
  }
  for (int i = 0; i < FM_COUNT(kGenres) && n < max; i++, n++) {
    fm_snprintf(out[n].id, sizeof out[n].id, "genre:%s", kGenres[i]);
    fm_strlcpy(out[n].name, kGenres[i], sizeof out[n].name);
  }
  return n;
}

void asrc_audius_search_url(const char *q, int offset, char *out, size_t cap) {
  char qq[256], qe[800];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, AUDIUS_API "/tracks/search?query=%s&limit=%d&offset=%d&app_name=mmcfm", qe, ASRC_PAGE_SIZE,
              offset);
}

bool asrc_audius_browse_url(const char *cat, int offset, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!cat || !*cat || !strcmp(cat, "trending")) {
    fm_snprintf(out, cap, AUDIUS_API "/tracks/trending?limit=%d&offset=%d&app_name=mmcfm", ASRC_PAGE_SIZE, offset);
    return true;
  }
  if (!strcmp(cat, "trending:month") || !strcmp(cat, "trending:allTime")) {
    fm_snprintf(out, cap, AUDIUS_API "/tracks/trending?time=%s&limit=%d&offset=%d&app_name=mmcfm", cat + 9,
                ASRC_PAGE_SIZE, offset);
    return true;
  }
  if (!strcmp(cat, "playlists")) {
    fm_snprintf(out, cap, AUDIUS_API "/playlists/trending?limit=%d&offset=%d&app_name=mmcfm", AUDIUS_PLAYLISTS,
                offset);
    return true;
  }
  if (!strncmp(cat, "genre:", 6)) {
    for (int i = 0; i < FM_COUNT(kGenres); i++)
      if (!strcmp(cat + 6, kGenres[i])) {
        char ge[96];
        net_urlencode(kGenres[i], ge, sizeof ge);
        fm_snprintf(out, cap, AUDIUS_API "/tracks/trending?genre=%s&limit=%d&offset=%d&app_name=mmcfm", ge,
                    ASRC_PAGE_SIZE, offset);
        return true;
      }
  }
  return false;
}

void asrc_audius_stream_url(const char *id, char *out, size_t cap) {
  fm_snprintf(out, cap, AUDIUS_API "/tracks/%s/stream?app_name=mmcfm", id);
}

static void au_common(const FmJsonNode *t, FmAsrcItem *it) {
  asrc_text(json_str(json_path(t, "user.name"), ""), it->artist, sizeof it->artist);
  if (!it->artist[0]) psrc_copy(it->artist, json_str(json_path(t, "user.handle"), ""), sizeof it->artist);
  const FmJsonNode *art = json_get(t, "artwork");
  if (!psrc_url_copy(json_str(json_get(art, "480x480"), ""), it->art, sizeof it->art))
    psrc_url_copy(json_str(json_get(art, "150x150"), ""), it->art, sizeof it->art);
  const char *link = json_str(json_get(t, "permalink"), "");
  if (*link == '/' && strlen(link) < 400 && !strpbrk(link, " \"<>\\"))
    fm_snprintf(it->page, sizeof it->page, "https://audius.co%s", link);
  vsrc_iso_date(json_str(json_get(t, "release_date"), ""), it->published, sizeof it->published);
  if (!it->published[0]) vsrc_iso_date(json_str(json_get(t, "created_at"), ""), it->published, sizeof it->published);
}

FmErr asrc_audius_parse(const char *json, size_t len, int offset, int want, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Audius sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *data = json_get(json_root(&j), "data");
  if (!data || data->type != JSON_ARR) {
    const char *m = json_str(json_get(json_root(&j), "error"), "");
    if (*m) fm_snprintf(out->error, sizeof out->error, "Audius: %.200s", m);
    else fm_strlcpy(out->error, "Audius sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  if (want < 1) want = 1;
  int cap = want < data->count ? want : data->count;
  asrc_page_reserve(out, cap > 0 ? cap : 1);
  int seen = 0;
  for (const FmJsonNode *t = json_first(data); t && seen < want; t = json_next(t), seen++) {
    const char *id = json_str(json_get(t, "id"), "");
    if (!psrc_id_ok(id, "", 32) || json_bool(json_get(t, "is_delete"), false)) continue;
    if (json_get(t, "playlist_name")) {                       /* a playlist or album */
      if (json_bool(json_get(t, "is_private"), false) || json_bool(json_get(t, "is_stream_gated"), false)) continue;
      FmAsrcItem *it = asrc_item_new(out, "audius", AITEM_ALBUM);
      fm_strlcpy(it->id, id, sizeof it->id);
      asrc_text(json_str(json_get(t, "playlist_name"), ""), it->title, sizeof it->title);
      au_common(t, it);
      fm_strlcpy(it->album, json_bool(json_get(t, "is_album"), false) ? "Album" : "Playlist", sizeof it->album);
      double plays = json_num(json_get(t, "total_play_count"), -1);
      it->plays = plays >= 0 && plays < 9e15 ? (i64)plays : -1;
      continue;
    }
    if (!json_bool(json_get(t, "is_streamable"), true) || json_bool(json_get(t, "is_stream_gated"), false) ||
        json_bool(json_get(t, "is_unlisted"), false))
      continue;
    FmAsrcItem *it = asrc_item_new(out, "audius", AITEM_TRACK);
    fm_strlcpy(it->id, id, sizeof it->id);
    asrc_text(json_str(json_get(t, "title"), ""), it->title, sizeof it->title);
    au_common(t, it);
    psrc_copy(it->album, json_str(json_get(t, "genre"), ""), sizeof it->album);
    asrc_audius_stream_url(id, it->url, sizeof it->url);
    fm_strlcpy(it->codec, "MP3", sizeof it->codec);
    psrc_copy(it->license, json_str(json_get(t, "license"), ""), sizeof it->license);
    double d = json_num(json_get(t, "duration"), 0);
    it->duration = d > 0 && d < 1e7 ? d : 0;
    double plays = json_num(json_get(t, "play_count"), -1);
    it->plays = plays >= 0 && plays < 9e15 ? (i64)plays : -1;
  }
  if (want <= ASRC_PAGE_SIZE && seen >= want && offset + want <= ASRC_MAX_OFFSET)
    fm_snprintf(out->next, sizeof out->next, "%d", offset + want);
  json_free(&j);
  return FM_OK;
}

static FmErr au_fetch(const char *url, int offset, int want, FmAsrcPage *out, volatile int *cancel) {
  FmNetResp r;
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("Audius", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_audius_parse((const char *)r.data, r.len, offset, want, out);
  net_resp_free(&r);
  return e;
}

static FmErr au_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int offset = psrc_page_num(token, 0, 0, ASRC_MAX_OFFSET);
  char url[1200];
  asrc_audius_search_url(query, offset, url, sizeof url);
  FmErr e = au_fetch(url, offset, ASRC_PAGE_SIZE, out, cancel);
  if (e == FM_OK && !out->count && offset == 0) fm_strlcpy(out->error, "No tracks found", sizeof out->error);
  return e;
}

static FmErr au_browse(const FmAsrcConf *c, const char *cat, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  int offset = psrc_page_num(token, 0, 0, ASRC_MAX_OFFSET);
  char url[600];
  if (!asrc_audius_browse_url(cat, offset, url, sizeof url)) {
    fm_strlcpy(out->error, "Unknown category", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  bool lists = cat && !strcmp(cat, "playlists");
  FmErr e = au_fetch(url, offset, lists ? AUDIUS_PLAYLISTS : ASRC_PAGE_SIZE, out, cancel);
  if (e == FM_OK && !out->count && offset == 0) fm_strlcpy(out->error, "Nothing here right now", sizeof out->error);
  return e;
}

static FmErr au_children(const FmAsrcConf *c, const FmAsrcItem *album, const char *token, FmAsrcPage *out,
                         volatile int *cancel) {
  FM_UNUSED(c);
  FM_UNUSED(token);
  out->next[0] = out->error[0] = 0;
  if (!psrc_id_ok(album->id, "", 32)) {
    fm_strlcpy(out->error, "Not an Audius playlist", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  char url[300];
  fm_snprintf(url, sizeof url, AUDIUS_API "/playlists/%s/tracks?app_name=mmcfm", album->id);
  FmErr e = au_fetch(url, 0, ASRC_MAX_CHILDREN, out, cancel);
  out->next[0] = 0;
  for (int i = 0; e == FM_OK && i < out->count; i++)        /* the playlist, not the genre */
    psrc_copy(out->items[i].album, album->title, sizeof out->items[i].album);
  if (e == FM_OK && !out->count) fm_strlcpy(out->error, "This playlist has no playable tracks", sizeof out->error);
  return e;
}

static FmErr au_resolve(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                        volatile int *cancel) {
  FM_UNUSED(c);
  FM_UNUSED(cancel);
  memset(out, 0, sizeof *out);
  if (!psrc_id_ok(item->id, "", 32)) {
    fm_strlcpy(err, "Not an Audius track", errcap);
    return FM_ERR_NOT_FOUND;
  }
  asrc_audius_stream_url(item->id, out->url, sizeof out->url);
  fm_strlcpy(out->codec, "MP3", sizeof out->codec);
  return FM_OK;
}

const FmAsrc g_asrc_audius = {
  "audius", "Audius", IC_EQUALIZER, ASRC_SEARCH | ASRC_BROWSE,
  au_search, au_categories, au_browse, au_resolve, au_children,
  "Independent artists, free streaming \xC2\xB7 no key needed",
};
