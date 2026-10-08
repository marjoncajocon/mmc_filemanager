/* fasrc_archive.c -- Internet Archive audio (archive.org): live concerts,
** old-time radio, LibriVox audiobooks, netlabel releases, 78s. No key.
**
** Design decisions:
**   - An archive.org item is a folder of files, so results are ALBUM
**     containers; children() reads /metadata/<id> and lists the tracks.
**     Search covers mediatype audio and etree (the Live Music Archive files
**     its concerts as "etree", not "audio"), leaves out restricted items
**     (lending library: their files answer 401) and sorts by downloads, as
**     the video adapter does: relevance alone surfaces random uploads first.
**   - One recording usually exists several times (the FLAC original plus
**     VBR MP3, 64 kbps MP3 and Ogg derivatives). Files are grouped by their
**     original (a derivative names it) and each group plays its best file:
**     VBR MP3, then 128 kbps MP3, then other MP3, then FLAC (both decode
**     built in and seek over HTTP), then Ogg Vorbis only with FFmpeg (the
**     built-in Vorbis decoder needs a local file).
**   - Order: the files' track numbers ("3", "03", "3/12"), else natural name
**     order ("t2" before "t10"), which is how etree and OTRR name files.
**     Titles, artists and albums come from the file metadata when present,
**     else the file name and the item.
**   - Metadata replies can be large (an OTRR series: 344 episodes, 1 MB;
**     some items list thousands of files), so the reply cap is 16 MB, at
**     most 20,000 files are looked at and children pages hold 300 tracks.
*/
#include "fasrc_int.h"

#define IA_MAX_FILES 20000

static const struct { const char *id, *name; } kCollections[] = {
  { "etree", "Live music" },          { "oldtimeradio", "Old-time radio" },
  { "librivoxaudio", "Audiobooks (LibriVox)" }, { "netlabels", "Netlabels" },
  { "78rpm", "78 RPM records" },      { "audio_bookspoetry", "Books & poetry" },
};

static int ia_categories(const FmAsrcConf *c, FmAsrcCat *out, int max) {
  FM_UNUSED(c);
  int n = 0;
  for (int i = 0; i < FM_COUNT(kCollections) && n < max; i++, n++) {
    fm_strlcpy(out[n].id, kCollections[i].id, sizeof out[n].id);
    fm_strlcpy(out[n].name, kCollections[i].name, sizeof out[n].name);
  }
  return n;
}

/* ---- search ------------------------------------------------------------------- */

void asrc_archive_search_url(const char *q, const char *collection, int page, char *out, size_t cap) {
  char qq[500], qe[1600];
  if (collection && *collection)
    fm_snprintf(qq, sizeof qq, "collection:%.60s AND mediatype:(audio OR etree) AND -access-restricted-item:true",
                collection);
  else
    fm_snprintf(qq, sizeof qq, "(%.300s) AND mediatype:(audio OR etree) AND -access-restricted-item:true",
                q ? q : "");
  net_urlencode(qq, qe, sizeof qe);
  fm_snprintf(out, cap, "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
              "&fl%%5B%%5D=creator&fl%%5B%%5D=downloads&fl%%5B%%5D=date&fl%%5B%%5D=publicdate"
              "&sort%%5B%%5D=downloads%%20desc&rows=%d&page=%d&output=json",
              qe, ASRC_PAGE_SIZE, page < 1 ? 1 : page);
}

FmErr asrc_archive_parse_search(const char *json, size_t len, int page, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "archive.org sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *resp = json_get(json_root(&j), "response");
  const FmJsonNode *docs = json_get(resp, "docs");
  if (!docs || docs->type != JSON_ARR) {
    const char *e = json_str(json_get(json_root(&j), "error"), "");
    if (*e) fm_snprintf(out->error, sizeof out->error, "archive.org: %.200s", e);
    else fm_strlcpy(out->error, "archive.org sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  int seen = 0;
  for (const FmJsonNode *d = json_first(docs); d && seen < ASRC_MAX_ITEMS; d = json_next(d), seen++) {
    const char *id = vsrc_jstr(json_get(d, "identifier"), "");
    if (!psrc_id_ok(id, "-_.", 100)) continue;
    FmAsrcItem *it = asrc_item_new(out, "archive", AITEM_ALBUM);
    fm_strlcpy(it->id, id, sizeof it->id);
    asrc_text(vsrc_jstr(json_get(d, "title"), id), it->title, sizeof it->title);
    asrc_text(vsrc_jstr(json_get(d, "creator"), ""), it->artist, sizeof it->artist);
    vsrc_iso_date(vsrc_jstr(json_get(d, "date"), ""), it->published, sizeof it->published);
    if (!it->published[0]) vsrc_iso_date(vsrc_jstr(json_get(d, "publicdate"), ""), it->published, sizeof it->published);
    double v = json_num(json_get(d, "downloads"), -1);
    it->plays = v >= 0 && v < 9e15 ? (i64)v : -1;
    fm_snprintf(it->art, sizeof it->art, "https://archive.org/services/img/%s", id);
    fm_snprintf(it->page, sizeof it->page, "https://archive.org/details/%s", id);
  }
  double found = json_num(json_get(resp, "numFound"), 0);
  if (seen >= ASRC_PAGE_SIZE && (double)page * ASRC_PAGE_SIZE < found && page * ASRC_PAGE_SIZE < ASRC_MAX_OFFSET)
    fm_snprintf(out->next, sizeof out->next, "%d", page + 1);
  json_free(&j);
  return FM_OK;
}

static FmErr ia_fetch_search(const char *url, int page, FmAsrcPage *out, volatile int *cancel) {
  FmNetResp r;
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("archive.org", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_archive_parse_search((const char *)r.data, r.len, page, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count && page == 1) fm_strlcpy(out->error, "No recordings found", sizeof out->error);
  return e;
}

static FmErr ia_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type something to search for", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = psrc_page_num(token, 1, 1, ASRC_MAX_OFFSET / ASRC_PAGE_SIZE);
  char url[2400];
  asrc_archive_search_url(query, NULL, page, url, sizeof url);
  return ia_fetch_search(url, page, out, cancel);
}

static FmErr ia_browse(const FmAsrcConf *c, const char *cat, const char *token, FmAsrcPage *out,
                       volatile int *cancel) {
  FM_UNUSED(c);
  out->next[0] = out->error[0] = 0;
  const char *coll = NULL;
  for (int i = 0; i < FM_COUNT(kCollections); i++)
    if (cat && !strcmp(cat, kCollections[i].id)) coll = kCollections[i].id;
  if (!cat || !*cat) coll = kCollections[0].id;
  if (!coll) {
    fm_strlcpy(out->error, "Unknown category", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int page = psrc_page_num(token, 1, 1, ASRC_MAX_OFFSET / ASRC_PAGE_SIZE);
  char url[2400];
  asrc_archive_search_url(NULL, coll, page, url, sizeof url);
  return ia_fetch_search(url, page, out, cancel);
}

/* ---- tracks of an item -------------------------------------------------------- */

/* 0 VBR MP3, 1 128 kbps MP3, 2 other MP3, 3 FLAC, 4 Ogg (FFmpeg only); -1 = skip */
static int file_rank(const char *format, const char *name, bool ffmpeg) {
  if (!fm_stricmp(format, "VBR MP3")) return 0;
  if (!fm_stricmp(format, "128Kbps MP3")) return 1;
  if (fm_stristr(format, "MP3") && fm_ends_with_i(name, ".mp3")) return 2;
  if (fm_stristr(format, "Flac") && fm_ends_with_i(name, ".flac")) return 3;
  if (ffmpeg && !fm_stricmp(format, "Ogg Vorbis")) return 4;
  return -1;
}

typedef struct IaFile {
  const FmJsonNode *f, *meta;     /* the file played; the recording's original (titles live there) */
  const char *name, *group;
  int rank, track;
} IaFile;

static int by_group(const void *a, const void *b) {
  const IaFile *x = (const IaFile *)a, *y = (const IaFile *)b;
  int c = strcmp(x->group, y->group);
  return c ? c : x->rank - y->rank;
}

static int by_track(const void *a, const void *b) {
  const IaFile *x = (const IaFile *)a, *y = (const IaFile *)b;
  if (x->track > 0 && y->track > 0 && x->track != y->track) return x->track < y->track ? -1 : 1;
  return fm_natcmp(x->group, y->group);
}

/* archive.org/download/<id>/<name>, each path segment percent-encoded */
static bool download_url(const char *id, const char *name, char *out, size_t cap) {
  char seg[512], enc[1536];
  int k = fm_snprintf(out, cap, "https://archive.org/download/%s/", id);
  if (k < 0 || (size_t)k >= cap) return false;
  const char *s = name;
  while (*s) {
    size_t n = strcspn(s, "/");
    if (n >= sizeof seg) return false;
    memcpy(seg, s, n);
    seg[n] = 0;
    net_urlencode(seg, enc, sizeof enc);
    if (fm_strlcat(out, enc, cap) >= cap) return false;
    s += n;
    if (*s == '/') { if (fm_strlcat(out, "/", cap) >= cap) return false; s++; }
  }
  return true;
}

/* "03", "3/12", "Track 3" -> 3; 0 when none */
static int track_num(const char *s) {
  while (*s && (*s < '0' || *s > '9')) s++;
  int v = 0;
  for (int k = 0; *s >= '0' && *s <= '9' && k < 6; s++, k++) v = v * 10 + (*s - '0');
  return v;
}

/* the file name without folders and extension, underscores as blanks */
static void name_title(const char *name, char *out, size_t cap) {
  char tmp[300];
  const char *b = strrchr(name, '/');
  fm_strlcpy(tmp, b ? b + 1 : name, sizeof tmp);
  char *dot = strrchr(tmp, '.');
  if (dot && dot != tmp) *dot = 0;
  for (char *p = tmp; *p; p++)
    if (*p == '_') *p = ' ';
  asrc_text(tmp, out, cap);
}

FmErr asrc_archive_parse_files(const FmAsrcConf *c, const char *json, size_t len, const FmAsrcItem *album,
                               int offset, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "archive.org sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *meta = json_get(root, "metadata");
  const FmJsonNode *files = json_get(root, "files");
  if (!strcmp(vsrc_jstr(json_get(meta, "access-restricted-item"), ""), "true")) {
    json_free(&j);
    fm_strlcpy(out->error, "This item is restricted on archive.org (lending); open it in the browser",
               sizeof out->error);
    return FM_ERR_ACCESS;
  }
  if (!files || files->type != JSON_ARR || json_bool(json_get(root, "is_dark"), false)) {
    json_free(&j);
    fm_strlcpy(out->error, "This item is not available on archive.org any more", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  const char *id = album->id;
  int nf = files->count < IA_MAX_FILES ? files->count : IA_MAX_FILES;
  IaFile *v = (IaFile *)fm_alloc((size_t)(nf > 0 ? nf : 1) * sizeof *v);
  int n = 0, seen = 0;
  bool ogg_only = false;
  for (const FmJsonNode *f = json_first(files); f && seen < nf; f = json_next(f), seen++) {
    const char *name = json_str(json_get(f, "name"), "");
    const char *format = json_str(json_get(f, "format"), "");
    if (!*name || strstr(name, "..") || strlen(name) > 400 || name[0] == '/') continue;
    if (!strcmp(json_str(json_get(f, "private"), ""), "true")) continue;     /* answers 401 */
    int rank = file_rank(format, name, c && c->have_ffmpeg);
    if (rank < 0) {
      if (!fm_stricmp(format, "Ogg Vorbis")) ogg_only = true;
      continue;
    }
    const char *orig = json_str(json_get(f, "original"), "");
    v[n].f = v[n].meta = f;
    v[n].name = name;
    v[n].group = !strcmp(json_str(json_get(f, "source"), ""), "derivative") && *orig ? orig : name;
    v[n].rank = rank;
    v[n].track = track_num(json_str(json_get(f, "track"), ""));
    n++;
  }
  /* best file of each recording */
  qsort(v, (size_t)n, sizeof *v, by_group);
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (m && !strcmp(v[m - 1].group, v[i].group)) {
      if (!v[m - 1].track && v[i].track) v[m - 1].track = v[i].track;   /* numbers often sit on one copy */
      if (!strcmp(v[i].name, v[i].group)) v[m - 1].meta = v[i].f;
      continue;
    }
    v[m++] = v[i];
  }
  qsort(v, (size_t)m, sizeof *v, by_track);

  char license[64], date[32];
  asrc_cc_license(vsrc_jstr(json_get(meta, "licenseurl"), ""), license, sizeof license);
  vsrc_iso_date(vsrc_jstr(json_get(meta, "date"), ""), date, sizeof date);
  if (!date[0]) fm_strlcpy(date, album->published, sizeof date);
  const char *item_artist = vsrc_jstr(json_get(meta, "creator"), "");
  const char *item_title = vsrc_jstr(json_get(meta, "title"), "");
  if (offset < 0) offset = 0;
  int want = m - offset;
  if (want > ASRC_MAX_CHILDREN) want = ASRC_MAX_CHILDREN;
  if (want > 0) asrc_page_reserve(out, want);
  for (int i = offset; i < m && out->count < ASRC_MAX_CHILDREN; i++) {
    const FmJsonNode *f = v[i].f;
    char url[1024];
    if (!download_url(id, v[i].name, url, sizeof url)) continue;
    FmAsrcItem *it = asrc_item_new(out, "archive", AITEM_TRACK);
    int k = fm_snprintf(it->id, sizeof it->id, "%s/%s", id, v[i].name);
    if (k < 0 || (size_t)k >= sizeof it->id) fm_snprintf(it->id, sizeof it->id, "%.100s#%d", id, i);
    const FmJsonNode *g = v[i].meta;
    asrc_text(vsrc_jstr(json_get(f, "title"), vsrc_jstr(json_get(g, "title"), "")), it->title, sizeof it->title);
    if (!it->title[0]) name_title(v[i].name, it->title, sizeof it->title);
    const char *artist = vsrc_jstr(json_get(f, "artist"), "");
    if (!*artist) artist = vsrc_jstr(json_get(f, "creator"), "");
    if (!*artist) artist = vsrc_jstr(json_get(g, "artist"), "");
    if (!*artist) artist = vsrc_jstr(json_get(g, "creator"), "");
    if (!*artist) artist = *item_artist ? item_artist : album->artist;
    asrc_text(artist, it->artist, sizeof it->artist);
    const char *alb = vsrc_jstr(json_get(f, "album"), vsrc_jstr(json_get(g, "album"), ""));
    asrc_text(*alb ? alb : *item_title ? item_title : album->title, it->album, sizeof it->album);
    fm_strlcpy(it->art, album->art, sizeof it->art);
    if (!it->art[0]) fm_snprintf(it->art, sizeof it->art, "https://archive.org/services/img/%s", id);
    fm_snprintf(it->page, sizeof it->page, "https://archive.org/details/%s", id);
    fm_strlcpy(it->url, url, sizeof it->url);
    fm_strlcpy(it->codec, v[i].rank <= 2 ? "MP3" : v[i].rank == 3 ? "FLAC" : "OGG", sizeof it->codec);
    fm_strlcpy(it->license, license, sizeof it->license);
    fm_strlcpy(it->published, date, sizeof it->published);
    it->duration = vsrc_clock_seconds(json_str(json_get(f, "length"), ""));
    double br = json_num(json_get(f, "bitrate"), 0);
    it->bitrate = br > 0 && br < 10000 ? (int)br : 0;
  }
  if (offset + ASRC_MAX_CHILDREN < m) fm_snprintf(out->next, sizeof out->next, "%d", offset + ASRC_MAX_CHILDREN);
  fm_free(v);
  json_free(&j);
  if (!m && offset == 0) {
    fm_strlcpy(out->error, ogg_only ? "This item only has Ogg audio, which needs FFmpeg"
                                    : "This item has no audio this app can play", sizeof out->error);
    return FM_ERR_UNSUPPORTED;
  }
  return FM_OK;
}

static FmErr ia_children(const FmAsrcConf *c, const FmAsrcItem *album, const char *token, FmAsrcPage *out,
                         volatile int *cancel) {
  out->next[0] = out->error[0] = 0;
  if (!psrc_id_ok(album->id, "-_.", 100)) {
    fm_strlcpy(out->error, "Not an archive.org item", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  int offset = psrc_page_num(token, 0, 0, IA_MAX_FILES);
  char url[256];
  fm_snprintf(url, sizeof url, "https://archive.org/metadata/%s", album->id);
  FmNetResp r;
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_META, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("archive.org", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  if (r.len < 4) {                                 /* "{}" = no such item */
    net_resp_free(&r);
    fm_strlcpy(out->error, "This item is not on archive.org", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  e = asrc_archive_parse_files(c, (const char *)r.data, r.len, album, offset, out);
  net_resp_free(&r);
  return e;
}

const FmAsrc g_asrc_archive = {
  "archive", "Internet Archive", IC_LANDMARK, ASRC_SEARCH | ASRC_BROWSE,
  ia_search, ia_categories, ia_browse, NULL, ia_children,
  "Concerts, old-time radio, audiobooks, 78s \xC2\xB7 no key needed",
};
