/* fasrc_podcasts.c -- podcasts: the iTunes Search API finds shows, the
** show's own RSS feed lists its episodes. No key.
**
** Design decisions:
**   - Apple's directory is only the index: results are PODCAST containers
**     whose url holds the RSS feed address (feedUrl), and children() reads
**     that feed with the small XML reader (fxml.c), so episodes play
**     straight from the podcast's host, as in every podcast app. Chart
**     entries have no feedUrl; children() then asks itunes.apple.com/lookup
**     for it (one short extra request).
**   - Feeds are big (The Rest Is History: 3.3 MB, 730 episodes on
**     2026-10-08), so the reply cap is 8 MB and at most the newest 300
**     episodes are kept: when the page is full, a newer episode replaces
**     the oldest one kept (feeds are usually newest first, but serials
**     list oldest first). The page is then sorted newest first; episodes
**     without a date keep feed order.
**   - Tolerant of real feeds: CDATA titles, entities, any prefix bound to
**     the iTunes namespace, itunes:duration as seconds or [H:]MM:SS, missing
**     fields. Video enclosures are left out; AAC (m4a) episodes are listed,
**     and asrc_stream says FFmpeg is needed when it is missing.
**   - Browse is Apple's top-podcasts chart for the user's country (US when
**     unknown), overall and per genre. It comes from the older
**     itunes.apple.com/<cc>/rss/toppodcasts feed: the newer
**     rss.marketingtools.apple.com API took 15-30 s or answered 504 for
**     every size/country its cache did not hold (2026-10-08), while the old
**     one answered every store in under a second and filters by genre.
**     Chart entries have no feed address; children() looks it up.
*/
#include "fasrc_int.h"
#include "fxml.h"

#define ITUNES_NS "http://www.itunes.com/dtds/podcast-1.0.dtd"

/* ---- search ------------------------------------------------------------------- */

static void country_lower(const FmAsrcConf *c, char *out, size_t cap) {
  if (c && strlen(c->country) == 2 && c->country[0] >= 'A' && c->country[0] <= 'Z' && c->country[1] >= 'A' &&
      c->country[1] <= 'Z')
    fm_snprintf(out, cap, "%c%c", c->country[0] - 'A' + 'a', c->country[1] - 'A' + 'a');
  else
    fm_strlcpy(out, "us", cap);
}

void asrc_podcasts_search_url(const FmAsrcConf *c, const char *q, char *out, size_t cap) {
  char qq[256], qe[800], cc[4];
  fm_strlcpy(qq, q ? q : "", sizeof qq);
  net_urlencode(qq, qe, sizeof qe);
  country_lower(c, cc, sizeof cc);
  fm_snprintf(out, cap, "https://itunes.apple.com/search?term=%s&media=podcast&entity=podcast&limit=%d&country=%s%s",
              qe, ASRC_PAGE_SIZE, cc, c && c->safe_search ? "&explicit=No" : "");
}

FmErr asrc_podcasts_parse_search(const char *json, size_t len, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "The podcast directory sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *res = json_get(json_root(&j), "results");
  if (!res || res->type != JSON_ARR) {
    const char *m = json_str(json_get(json_root(&j), "errorMessage"), "");
    if (*m) fm_snprintf(out->error, sizeof out->error, "Podcast directory: %.200s", m);
    else fm_strlcpy(out->error, "The podcast directory sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  int seen = 0;
  for (const FmJsonNode *r = json_first(res); r && seen < ASRC_MAX_ITEMS; r = json_next(r), seen++) {
    double id = json_num(json_get(r, "collectionId"), 0);
    char feed[1024];
    if (id <= 0 || id > 9e15) continue;
    if (!psrc_url_copy(json_str(json_get(r, "feedUrl"), ""), feed, sizeof feed)) continue;
    FmAsrcItem *it = asrc_item_new(out, "podcasts", AITEM_PODCAST);
    fm_snprintf(it->id, sizeof it->id, "%.0f", id);
    asrc_text(json_str(json_get(r, "collectionName"), ""), it->title, sizeof it->title);
    asrc_text(json_str(json_get(r, "artistName"), ""), it->artist, sizeof it->artist);
    asrc_text(json_str(json_get(r, "primaryGenreName"), ""), it->album, sizeof it->album);
    if (!psrc_url_copy(json_str(json_get(r, "artworkUrl600"), ""), it->art, sizeof it->art))
      psrc_url_copy(json_str(json_get(r, "artworkUrl100"), ""), it->art, sizeof it->art);
    psrc_url_copy(json_str(json_get(r, "collectionViewUrl"), ""), it->page, sizeof it->page);
    fm_strlcpy(it->url, feed, sizeof it->url);
    vsrc_iso_date(json_str(json_get(r, "releaseDate"), ""), it->published, sizeof it->published);
  }
  json_free(&j);
  return FM_OK;
}

/* the old iTunes RSS-as-JSON: every value is {"label": ...} */
static const char *label(const FmJsonNode *n, const char *path) {
  char p[96];
  fm_snprintf(p, sizeof p, "%s.label", path);
  return json_str(json_path(n, p), "");
}

static void top_entry(const FmJsonNode *r, FmAsrcPage *out) {
  const char *id = json_str(json_path(r, "id.attributes.im:id"), "");
  if (!psrc_id_ok(id, "", 20)) return;
  FmAsrcItem *it = asrc_item_new(out, "podcasts", AITEM_PODCAST);
  fm_strlcpy(it->id, id, sizeof it->id);
  asrc_text(label(r, "im:name"), it->title, sizeof it->title);
  asrc_text(label(r, "im:artist"), it->artist, sizeof it->artist);
  asrc_text(json_str(json_path(r, "category.attributes.label"), ""), it->album, sizeof it->album);
  /* the largest picture (170 px) asked for at 600 px, as the search gives */
  const FmJsonNode *imgs = json_get(r, "im:image");
  const FmJsonNode *last = NULL;
  for (const FmJsonNode *im = json_first(imgs); im; im = json_next(im)) last = im;
  char art[512];
  if (psrc_url_copy(json_str(json_get(last, "label"), ""), art, sizeof art)) {
    char *slash = strrchr(art, '/');
    if (slash && strstr(slash, "x") && strstr(slash, "bb.") && (size_t)(slash - art) + 20 < sizeof it->art)
      fm_snprintf(it->art, sizeof it->art, "%.*s/600x600bb%s", (int)(slash - art), art, strstr(slash, "bb.") + 2);
    else fm_strlcpy(it->art, art, sizeof it->art);
  }
  psrc_url_copy(json_str(json_path(r, "link.attributes.href"), ""), it->page, sizeof it->page);
  vsrc_iso_date(label(r, "im:releaseDate"), it->published, sizeof it->published);
}

FmErr asrc_podcasts_parse_top(const char *json, size_t len, FmAsrcPage *out) {
  FmJson j;
  out->next[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "The podcast chart sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *feed = json_get(json_root(&j), "feed");
  const FmJsonNode *ent = json_get(feed, "entry");
  if (!feed || feed->type != JSON_OBJ) {
    fm_strlcpy(out->error, "The podcast chart sent a reply this app does not understand", sizeof out->error);
    json_free(&j);
    return FM_ERR_FORMAT;
  }
  asrc_page_reserve(out, ASRC_MAX_ITEMS);
  if (ent && ent->type == JSON_OBJ) top_entry(ent, out);       /* one entry comes as an object */
  else if (ent && ent->type == JSON_ARR) {
    int seen = 0;
    for (const FmJsonNode *r = json_first(ent); r && seen < ASRC_MAX_ITEMS; r = json_next(r), seen++) top_entry(r, out);
  }
  json_free(&j);
  return FM_OK;
}

bool asrc_podcasts_parse_lookup(const char *json, size_t len, char *feed, size_t cap) {
  FmJson j;
  if (cap) feed[0] = 0;
  if (!json || json_parse(&j, json, len) != FM_OK) return false;
  bool ok = psrc_url_copy(json_str(json_path(json_root(&j), "results.0.feedUrl"), ""), feed, cap);
  json_free(&j);
  return ok;
}

static FmErr pod_get_json(const char *url, FmNetResp *r, FmAsrcPage *out, volatile int *cancel) {
  FmErr e = asrc_http_get(url, NULL, ASRC_MAX_REPLY, r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  if (r->status != 200) {
    e = psrc_http_error("The podcast directory", r->status, false, out->error, sizeof out->error);
    net_resp_free(r);
    return e;
  }
  return FM_OK;
}

static FmErr pod_search(const FmAsrcConf *c, const char *query, const char *token, FmAsrcPage *out,
                        volatile int *cancel) {
  FM_UNUSED(token);                                /* one page: the directory has no paging */
  out->next[0] = out->error[0] = 0;
  if (!query || !*query) {
    fm_strlcpy(out->error, "Type a podcast name or topic", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  char url[1200];
  asrc_podcasts_search_url(c, query, url, sizeof url);
  FmNetResp r;
  FmErr e = pod_get_json(url, &r, out, cancel);
  if (e != FM_OK) return e;
  e = asrc_podcasts_parse_search((const char *)r.data, r.len, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count) fm_strlcpy(out->error, "No podcasts found", sizeof out->error);
  return e;
}

static const struct { const char *id, *name; } kGenres[] = {
  { "1489", "News" },             { "1303", "Comedy" },          { "1488", "True Crime" },
  { "1324", "Society & Culture" }, { "1487", "History" },        { "1321", "Business" },
  { "1545", "Sports" },           { "1304", "Education" },       { "1533", "Science" },
  { "1318", "Technology" },       { "1512", "Health & Fitness" }, { "1301", "Arts" },
  { "1309", "TV & Film" },        { "1310", "Music" },           { "1305", "Kids & Family" },
  { "1314", "Religion & Spirituality" }, { "1483", "Fiction" },  { "1502", "Leisure" },
};

static int pod_categories(const FmAsrcConf *c, FmAsrcCat *out, int max) {
  FM_UNUSED(c);
  int n = 0;
  if (n < max) {
    fm_strlcpy(out[n].id, "top", sizeof out[n].id);
    fm_strlcpy(out[n].name, "Top shows", sizeof out[n].name);
    n++;
  }
  for (int i = 0; i < FM_COUNT(kGenres) && n < max; i++, n++) {
    fm_snprintf(out[n].id, sizeof out[n].id, "genre:%s", kGenres[i].id);
    fm_strlcpy(out[n].name, kGenres[i].name, sizeof out[n].name);
  }
  return n;
}

bool asrc_podcasts_top_url(const FmAsrcConf *c, const char *cat, char *out, size_t cap) {
  char cc[4], genre[32];
  genre[0] = 0;
  if (cap) out[0] = 0;
  if (cat && *cat && strcmp(cat, "top")) {
    bool ok = false;
    for (int i = 0; i < FM_COUNT(kGenres) && !ok; i++)
      if (!strncmp(cat, "genre:", 6) && !strcmp(cat + 6, kGenres[i].id)) {
        fm_snprintf(genre, sizeof genre, "/genre=%s", kGenres[i].id);
        ok = true;
      }
    if (!ok) return false;
  }
  country_lower(c, cc, sizeof cc);
  fm_snprintf(out, cap, "https://itunes.apple.com/%s/rss/toppodcasts/limit=%d%s/%sjson", cc, ASRC_PAGE_SIZE, genre,
              c && c->safe_search ? "explicit=false/" : "");
  return true;
}

static FmErr pod_browse(const FmAsrcConf *c, const char *cat, const char *token, FmAsrcPage *out,
                        volatile int *cancel) {
  FM_UNUSED(token);                                /* the chart is one page */
  out->next[0] = out->error[0] = 0;
  char url[300];
  if (!asrc_podcasts_top_url(c, cat, url, sizeof url)) {
    fm_strlcpy(out->error, "Unknown category", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  FmNetResp r;
  FmErr e = pod_get_json(url, &r, out, cancel);
  if (e != FM_OK) return e;
  e = asrc_podcasts_parse_top((const char *)r.data, r.len, out);
  net_resp_free(&r);
  if (e == FM_OK && !out->count) fm_strlcpy(out->error, "The chart is empty right now", sizeof out->error);
  return e;
}

/* ---- episodes ----------------------------------------------------------------- */

/* sortable stamp from an RFC 822 / ISO date with an optional time; 0 = none */
static i64 date_key(const char *s, const char *ymd) {
  if (!ymd[0]) return 0;
  i64 key = (i64)atoi(ymd) * 10000 + atoi(ymd + 5) * 100 + atoi(ymd + 8);
  int hh = 0, mm = 0, ss = 0;
  for (const char *p = s; *p; p++)
    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] == ':' && p[3] >= '0' && p[3] <= '9') {
      hh = atoi(p);
      mm = atoi(p + 3);
      if (p[5] == ':') ss = atoi(p + 6);
      break;
    }
  return key * 100000 + (hh % 24) * 3600 + (mm % 60) * 60 + ss % 61;
}

typedef struct Episode {
  char title[256], ititle[256], author[160], url[1024], type[64], duration[32], date[64], image[512], guid[256],
       link[512];
} Episode;

typedef struct Order { i64 key; int seq; } Order;

static int by_newest(const void *a, const void *b) {
  const Order *x = (const Order *)a, *y = (const Order *)b;
  if (x->key != y->key) return x->key > y->key ? -1 : 1;
  return x->seq - y->seq;
}

static void id_from(const char *guid, const char *url, char *out, size_t cap) {
  bool ok = guid[0] && strlen(guid) < cap;
  for (const char *p = guid; ok && *p; p++)
    if ((u8)*p < ' ') ok = false;
  if (ok) { fm_strlcpy(out, guid, cap); return; }
  u64 h = 0xcbf29ce484222325ULL;                   /* FNV-1a of the enclosure */
  for (const u8 *p = (const u8 *)url; *p; p++) { h ^= *p; h *= 0x100000001b3ULL; }
  fm_snprintf(out, cap, "ep-%08x%08x", (unsigned)(h >> 32), (unsigned)h);
}

/* adds an episode, or replaces the oldest kept when the page is full and it is newer */
static void keep_episode(FmAsrcPage *out, Order *ord, int *n, int max, const Episode *ep, const FmAsrcItem *show,
                         const char *show_title, const char *show_author, const char *show_art, int seq) {
  char codec[16], url[1024], date[32];
  if (!psrc_url_copy(ep->url, url, sizeof url)) return;
  if (!fm_strnicmp(ep->type, "video/", 6)) return;
  asrc_codec_from_mime(ep->type, codec, sizeof codec);
  if (!codec[0]) asrc_codec_from_ext(url, codec, sizeof codec);
  if (!codec[0] && ep->type[0] && fm_strnicmp(ep->type, "audio/", 6)) return;   /* a PDF, an image */
  asrc_rfc822_date(ep->date, date, sizeof date);
  i64 key = date_key(ep->date, date);
  int slot;
  if (*n < max) {
    slot = (*n)++;
    asrc_page_add(out);
  } else {
    slot = 0;
    for (int i = 1; i < *n; i++)
      if (ord[i].key < ord[slot].key || (ord[i].key == ord[slot].key && ord[i].seq > ord[slot].seq)) slot = i;
    if (key <= ord[slot].key) return;
  }
  ord[slot].key = key;
  ord[slot].seq = seq;
  FmAsrcItem *it = &out->items[slot];
  memset(it, 0, sizeof *it);
  fm_strlcpy(it->source, "podcasts", sizeof it->source);
  it->kind = AITEM_TRACK;
  it->plays = -1;
  id_from(ep->guid, url, it->id, sizeof it->id);
  asrc_text(ep->title[0] ? ep->title : ep->ititle, it->title, sizeof it->title);
  if (!it->title[0]) fm_strlcpy(it->title, "Untitled episode", sizeof it->title);
  asrc_text(ep->author[0] ? ep->author : show_author, it->artist, sizeof it->artist);
  if (!it->artist[0] && show) psrc_copy(it->artist, show->artist, sizeof it->artist);
  asrc_text(show && show->title[0] ? show->title : show_title, it->album, sizeof it->album);
  if (!psrc_url_copy(ep->image, it->art, sizeof it->art) && !psrc_url_copy(show_art, it->art, sizeof it->art) &&
      show)
    fm_strlcpy(it->art, show->art, sizeof it->art);
  fm_strlcpy(it->url, url, sizeof it->url);
  if (!psrc_url_copy(ep->link, it->page, sizeof it->page) && show) fm_strlcpy(it->page, show->page, sizeof it->page);
  fm_strlcpy(it->codec, codec, sizeof it->codec);
  it->duration = asrc_duration(ep->duration);
  fm_strlcpy(it->published, date, sizeof it->published);
}

/* "<item>" / "<item " occurrences, to size the page once */
static int count_items(const char *s, size_t len) {
  int n = 0;
  for (size_t i = 0; i + 6 <= len; i++)
    if (s[i] == '<' && s[i + 1] == 'i' && !memcmp(s + i, "<item", 5) &&
        (s[i + 5] == '>' || s[i + 5] == ' ' || s[i + 5] == '\n' || s[i + 5] == '\r' || s[i + 5] == '\t'))
      n++;
  return n;
}

FmErr asrc_podcasts_parse_feed(const char *xml, size_t len, const FmAsrcItem *show, FmAsrcPage *out) {
  out->next[0] = 0;
  if (!xml || !len) {
    fm_strlcpy(out->error, "The feed is empty", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  int total = count_items(xml, len);
  int max = total < ASRC_MAX_CHILDREN ? total : ASRC_MAX_CHILDREN;
  if (max > 0) asrc_page_reserve(out, max);
  Order *ord = (Order *)fm_alloc((size_t)(max > 0 ? max : 1) * sizeof *ord);
  Episode *ep = (Episode *)fm_alloc(sizeof *ep);
  char it_ns[32] = "itunes", show_title[256] = "", show_author[160] = "", show_art[512] = "", tmp[512];
  bool root = false, rss = false, in_item = false;
  int item_depth = 0, n = 0, seq = 0;
  FmXml x;
  xml_init(&x, xml, len);
  while (xml_next(&x) != XML_EOF) {
    if (x.type == XML_END && in_item && x.depth < item_depth) {
      if (max > 0) keep_episode(out, ord, &n, max, ep, show, show_title, show_author, show_art, seq++);
      in_item = false;
      continue;
    }
    if (x.type != XML_START) continue;
    if (!root) {                                   /* <rss>, RSS 1.0 <rdf:RDF>, or an Atom <feed> (no enclosures) */
      root = true;
      rss = xml_is(&x, "rss") || xml_is(&x, "rdf:RDF") || xml_is(&x, "feed");
      if (!rss) break;
      xml_ns_prefix(&x, ITUNES_NS, it_ns, sizeof it_ns);
      continue;
    }
    if (xml_is(&x, "item")) {
      if (in_item) continue;                       /* a nested <item>: stay in the outer one */
      memset(ep, 0, sizeof *ep);
      in_item = !x.empty;
      item_depth = x.depth;
      continue;
    }
    if (in_item) {
      if (x.depth != item_depth + 1 && !(x.empty && x.depth == item_depth)) {
        if (!x.empty) xml_skip(&x);                /* inside a child: nothing we read */
        continue;
      }
      if (xml_is(&x, "title")) xml_inner_text(&x, ep->title, sizeof ep->title);
      else if (xml_is_ns(&x, it_ns, "title")) xml_inner_text(&x, ep->ititle, sizeof ep->ititle);
      else if (xml_is(&x, "enclosure") && !ep->url[0]) {
        xml_attr(&x, "url", ep->url, sizeof ep->url);
        xml_attr(&x, "type", ep->type, sizeof ep->type);
        if (!x.empty) xml_skip(&x);
      } else if (xml_is(&x, "media:content") && !ep->url[0]) {
        xml_attr(&x, "type", ep->type, sizeof ep->type);
        if (!fm_strnicmp(ep->type, "audio/", 6)) xml_attr(&x, "url", ep->url, sizeof ep->url);
        else ep->type[0] = 0;
        if (!x.empty) xml_skip(&x);
      } else if (xml_is_ns(&x, it_ns, "duration")) xml_inner_text(&x, ep->duration, sizeof ep->duration);
      else if (xml_is(&x, "pubDate")) xml_inner_text(&x, ep->date, sizeof ep->date);
      else if (xml_is_ns(&x, it_ns, "image")) {
        xml_attr(&x, "href", ep->image, sizeof ep->image);
        if (!x.empty) xml_skip(&x);
      } else if (xml_is(&x, "guid")) xml_inner_text(&x, ep->guid, sizeof ep->guid);
      else if (xml_is(&x, "link")) xml_inner_text(&x, ep->link, sizeof ep->link);
      else if (xml_is_ns(&x, it_ns, "author")) xml_inner_text(&x, ep->author, sizeof ep->author);
      else if (!x.empty) xml_skip(&x);
      continue;
    }
    /* channel level (before or between the items) */
    if (x.depth == 3 || (x.empty && x.depth == 2)) {
      if (xml_is(&x, "title") && !show_title[0]) xml_inner_text(&x, show_title, sizeof show_title);
      else if (xml_is_ns(&x, it_ns, "author") && !show_author[0])
        xml_inner_text(&x, show_author, sizeof show_author);
      else if (xml_is_ns(&x, it_ns, "image")) {            /* square and large: wins over <image> */
        if (xml_attr(&x, "href", tmp, sizeof tmp) && tmp[0]) psrc_copy(show_art, tmp, sizeof show_art);
        if (!x.empty) xml_skip(&x);
      } else if (xml_is(&x, "image") && !x.empty) {
        /* <image><url>..</url></image>: the RSS 2.0 channel image */
        int d = x.depth;
        while (xml_next(&x) != XML_EOF && !(x.type == XML_END && x.depth < d))
          if (x.type == XML_START && xml_is(&x, "url") && !show_art[0]) {
            xml_inner_text(&x, tmp, sizeof tmp);
            psrc_copy(show_art, tmp, sizeof show_art);
          }
      } else if (!x.empty) xml_skip(&x);
    }
  }
  if (in_item && max > 0)                          /* a truncated feed: keep the last episode read */
    keep_episode(out, ord, &n, max, ep, show, show_title, show_author, show_art, seq++);
  /* newest first: sort the order, then permute the items in place */
  for (int i = 0; i < n; i++) ord[i].seq = ord[i].seq * 1024 + i;    /* remember each one's slot */
  if (n > 1) {
    qsort(ord, (size_t)n, sizeof *ord, by_newest);
    int *src = (int *)fm_alloc((size_t)n * sizeof *src);
    for (int i = 0; i < n; i++) src[i] = ord[i].seq % 1024;
    FmAsrcItem *hold = (FmAsrcItem *)fm_alloc(sizeof *hold);
    for (int i = 0; i < n; i++) {                  /* cycle-follow: each item moves once */
      if (src[i] < 0 || src[i] == i) continue;
      *hold = out->items[i];
      int k = i;
      while (src[k] != i) {
        int from = src[k];
        out->items[k] = out->items[from];
        src[k] = -1;
        k = from;
      }
      out->items[k] = *hold;
      src[k] = -1;
    }
    fm_free(hold);
    fm_free(src);
  }
  fm_free(ord);
  fm_free(ep);
  if (!rss) {
    fm_strlcpy(out->error, "This is not a podcast feed", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  if (!out->count) fm_strlcpy(out->error, "This podcast has no audio episodes", sizeof out->error);
  return FM_OK;
}

static FmErr pod_children(const FmAsrcConf *c, const FmAsrcItem *show, const char *token, FmAsrcPage *out,
                          volatile int *cancel) {
  FM_UNUSED(c);
  FM_UNUSED(token);
  out->next[0] = out->error[0] = 0;
  char feed[1024];
  FmNetResp r;
  FmErr e;
  if (!psrc_url_copy(show->url, feed, sizeof feed)) {
    if (!psrc_id_ok(show->id, "", 20)) {
      fm_strlcpy(out->error, "Not a podcast", sizeof out->error);
      return FM_ERR_NOT_FOUND;
    }
    char url[200];
    fm_snprintf(url, sizeof url, "https://itunes.apple.com/lookup?id=%s&entity=podcast", show->id);
    e = pod_get_json(url, &r, out, cancel);
    if (e != FM_OK) return e;
    bool ok = asrc_podcasts_parse_lookup((const char *)r.data, r.len, feed, sizeof feed);
    net_resp_free(&r);
    if (!ok) {
      fm_strlcpy(out->error, "This podcast has no public feed", sizeof out->error);
      return FM_ERR_NOT_FOUND;
    }
  }
  e = asrc_http_get(feed, NULL, ASRC_MAX_FEED, &r, out->error, sizeof out->error, cancel);
  if (e == FM_ERR_FULL || e == FM_ERR_NOMEM) {
    fm_strlcpy(out->error, "This podcast's feed is too big (over 8 MB)", sizeof out->error);
    return e;
  }
  if (e != FM_OK) return e;
  if (r.status != 200) {
    e = psrc_http_error("The podcast's host", r.status, false, out->error, sizeof out->error);
    net_resp_free(&r);
    return e;
  }
  e = asrc_podcasts_parse_feed((const char *)r.data, r.len, show, out);
  net_resp_free(&r);
  return e;
}

const FmAsrc g_asrc_podcasts = {
  "podcasts", "Podcasts", IC_PERSON, ASRC_SEARCH | ASRC_BROWSE,
  pod_search, pod_categories, pod_browse, NULL, pod_children,
  "Every podcast in Apple's directory, played from its own feed \xC2\xB7 no key needed",
};
