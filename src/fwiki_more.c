/* fwiki_more.c -- Online reading sources besides the Wikimedia wikis (see
** fwiki_int.h): Project Gutenberg books, iNaturalist species, Fruityvice.
**
** Checked from this PC (2026-10-10), no key for any of them:
**   - Gutendex (gutendex.com), Project Gutenberg's catalog as JSON: /books/?search=
**     &languages=&page=; slow (a search took ~20 s), so only for lists. A book
**     is read from www.gutenberg.org/ebooks/<id>.txt.utf-8 directly (Noli Me
**     Tangere: 1.2 MB in ~7 s); the cover is cache/epub/<id>/pg<id>.cover.medium.jpg.
**   - iNaturalist API (api.inaturalist.org/v1): /taxa/autocomplete?q= (ranks names
**     well; plain /taxa?q= does not), /taxa?taxon_id=&rank=species&order_by=
**     observations_count (a group's best known), /taxa/<id> (Wikipedia summary,
**     ancestors, photos with attribution). locale= gives common names in a language.
**   - Fruityvice (fruityvice.com/api/fruit/all, /fruit/<name>): 49 fruits, nutrition
**     per 100 g and classification.
**
** Design decisions:
**   - Every source fills the same structs as the wikis (WkList, WkArticle,
**     WkMediaList), so the view draws them all alike. Article text is built
**     as lines ("== Heading ==" or a paragraph) and split by
**     wk_blocks_from_lines, the wikis' own splitter.
**   - A Gutenberg book's chapters are found by shape: a one-line paragraph
**     that starts "CHAPTER", "Chapter", "KABANATA", "BOOK", "PART", "ACT"...,
**     or is a Roman numeral, or is short and in capitals. Text before
**     "*** START OF" and after "*** END OF" (the licence) is left out.
*/
#include "fwiki_int.h"
#include "fjson.h"
#include "fnet.h"

#define RD_MAX_REPLY (8u << 20)

static FmErr get_json(const char *url, const char *site, FmJson *j, char *err, size_t cap, volatile int *cancel) {
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, &r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", cap); net_resp_free(&r); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, cap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status == 404) { fm_snprintf(err, cap, "%s has nothing there", site); net_resp_free(&r); return FM_ERR_NOT_FOUND; }
  if (r.status == 429) { fm_snprintf(err, cap, "%s is busy: try again in a minute", site); net_resp_free(&r); return FM_ERR_IO; }
  if (r.status != 200 || !r.data) {
    fm_snprintf(err, cap, "%s answered %d", site, r.status);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = json_parse(j, (const char *)r.data, r.len);
  net_resp_free(&r);
  if (e != FM_OK) fm_snprintf(err, cap, "%s sent a reply this app does not understand", site);
  return e;
}

static WkPage *add(WkList *l) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 32;
    l->it = (WkPage *)fm_realloc(l->it, (size_t)l->cap * sizeof *l->it);
  }
  WkPage *p = &l->it[l->n++];
  memset(p, 0, sizeof *p);
  return p;
}

/* A growing text: the article's lines. */
typedef struct Txt { char *s; size_t n, cap; } Txt;

static void tput(Txt *t, const char *s, size_t n) {
  if (t->n + n + 2 > t->cap) {
    t->cap = (t->n + n + 2) * 2;
    t->s = (char *)fm_realloc(t->s, t->cap);
  }
  memcpy(t->s + t->n, s, n);
  t->n += n;
  t->s[t->n] = 0;
}

static void tline(Txt *t, const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
  tput(t, buf, (size_t)n);
  tput(t, "\n", 1);
}

/* Plain text from a little HTML (iNaturalist's Wikipedia summary). */
static void html_text(const char *h, Txt *t) {
  static const struct { const char *e; const char *c; } kEnt[] = {
    { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&nbsp;", " " },
    { "&apos;", "'" },
  };
  while (*h) {
    if (*h == '<') {
      const char *e = strchr(h, '>');
      bool br = !strncmp(h, "<br", 3) || !strncmp(h, "</p", 3);
      h = e ? e + 1 : h + strlen(h);
      if (br) tput(t, "\n", 1);
      continue;
    }
    if (*h == '&') {
      bool hit = false;
      for (int i = 0; i < FM_COUNT(kEnt); i++) {
        size_t n = strlen(kEnt[i].e);
        if (!strncmp(h, kEnt[i].e, n)) { tput(t, kEnt[i].c, strlen(kEnt[i].c)); h += n; hit = true; break; }
      }
      if (hit) continue;
    }
    tput(t, h, 1);
    h++;
  }
}

static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %xx and + back to text (a Wikipedia URL's title). */
static void url_decode(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 1 < cap; s++) {
    int hi, lo;
    if (*s == '%' && s[1] && s[2] && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
      out[o++] = (char)(hi * 16 + lo);
      s += 2;
    } else {
      out[o++] = *s == '+' ? ' ' : *s;
    }
  }
  out[o] = 0;
}

static void finish(WkArticle *out, Txt *t) {
  out->text = t->s ? t->s : fm_strdup("");
  wk_blocks_from_lines(out);
}

/* ---- Project Gutenberg ------------------------------------------------------------------ */

static void authors(const FmJsonNode *a, char *out, size_t cap) {
  out[0] = 0;
  size_t n = 0;
  int k = 0;
  for (const FmJsonNode *x = json_first(a); x && k < 3; x = json_next(x), k++) {
    /* "Rizal, José" -> "José Rizal" */
    char nm[160], fl[160];
    fm_strlcpy(nm, json_str(json_get(x, "name"), ""), sizeof nm);
    char *c = strchr(nm, ',');
    if (c) {
      *c = 0;
      const char *first = c + 1;
      while (*first == ' ') first++;
      fm_snprintf(fl, sizeof fl, "%s %s", first, nm);
    } else {
      fm_strlcpy(fl, nm, sizeof fl);
    }
    n += (size_t)fm_snprintf(out + n, cap - n, "%s%s", k ? ", " : "", fl);
    if (n >= cap) break;
  }
}

static const char *text_url(const FmJsonNode *formats) {
  const char *any = NULL;
  for (const FmJsonNode *f = json_first(formats); f; f = json_next(f)) {
    if (!f->key || strncmp(f->key, "text/plain", 10)) continue;
    const char *u = json_str(f, "");
    if (strstr(f->key, "utf-8")) return u;
    if (!any) any = u;
  }
  return any;
}

FmErr rd_guten_search(const char *lang, const char *query, int page, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  char qe[800], url[1200];
  net_urlencode(query ? query : "", qe, sizeof qe);
  bool lang_on = lang && lang[0] && strcmp(lang, "en") && strcmp(lang, "simple") && strlen(lang) <= 3;
  fm_snprintf(url, sizeof url, "https://gutendex.com/books/?page=%d%s%s%s%s", page < 1 ? 1 : page, qe[0] ? "&search=" : "", qe,
              lang_on ? "&languages=" : "", lang_on ? lang : "");
  FmJson j;
  FmErr e = get_json(url, "Project Gutenberg", &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  const FmJsonNode *root = json_root(&j);
  for (const FmJsonNode *b = json_first(json_get(root, "results")); b; b = json_next(b)) {
    if (!text_url(json_get(b, "formats"))) continue;            /* audio books, scans: nothing to read here */
    WkPage *p = add(out);
    fm_snprintf(p->id, sizeof p->id, "%.0f", json_num(json_get(b, "id"), 0));
    fm_strlcpy(p->title, json_str(json_get(b, "title"), ""), sizeof p->title);
    char au[200];
    authors(json_get(b, "authors"), au, sizeof au);
    double dl = json_num(json_get(b, "download_count"), 0);
    fm_snprintf(p->desc, sizeof p->desc, "%s%s%.0f downloads", au, au[0] ? " \xC2\xB7 " : "", dl);
    fm_strlcpy(p->thumb, json_str(json_path(b, "formats.image/jpeg"), ""), sizeof p->thumb);
  }
  if (json_str(json_get(root, "next"), NULL)) out->next = (page < 1 ? 1 : page) + 1;
  json_free(&j);
  if (!out->n && page <= 1) fm_strlcpy(out->error, "No books found", sizeof out->error);
  return FM_OK;
}

static bool roman(const char *s, size_t n) {
  size_t k = 0;
  while (k < n && strchr("IVXLC", s[k])) k++;
  return k > 0 && (k == n || s[k] == '.' || s[k] == ' ');
}

static int heading_level(const char *s, size_t n) {
  static const char *const kWords[] = { "CHAPTER", "Chapter", "KABANATA", "Kabanata", "BOOK ", "Book ", "PART ",
                                        "Part ", "ACT ", "Act ", "SCENE", "Scene", "CANTO", "Canto", "PROLOGUE",
                                        "EPILOGUE", "PREFACE", "Preface", "INTRODUCTION", "Introduction" };
  if (n == 0 || n > 80) return 0;
  for (int i = 0; i < FM_COUNT(kWords); i++)
    if (!strncmp(s, kWords[i], strlen(kWords[i]))) return 2;
  if (n <= 40 && roman(s, n)) return 2;
  int letters = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] >= 'a' && s[i] <= 'z') return 0;
    if (s[i] >= 'A' && s[i] <= 'Z') letters++;
  }
  return letters >= 3 && n <= 60 ? 3 : 0;
}

void rd_guten_text(char *text, WkArticle *out) {
  /* the book between the licence lines */
  char *b = strstr(text, "*** START OF");
  if (b) {
    char *nl = strchr(b, '\n');
    b = nl ? nl + 1 : b;
  } else {
    b = text;
  }
  char *e = strstr(b, "*** END OF");
  if (e) *e = 0;
  Txt t = { 0 };
  /* paragraphs: lines up to a blank line, joined */
  char *para = NULL;
  size_t plen = 0, pcap = 0;
  int lines = 0;
  for (char *p = b;; ) {
    char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    size_t lead = 0;
    while (lead < n && (p[lead] == ' ' || p[lead] == '\t')) lead++;
    bool blank = lead == n;
    if (!blank) {
      if (plen + n + 2 > pcap) {
        pcap = (plen + n + 2) * 2;
        para = (char *)fm_realloc(para, pcap);
      }
      if (plen) para[plen++] = ' ';
      memcpy(para + plen, p + lead, n - lead);
      plen += n - lead;
      para[plen] = 0;
      lines++;
    }
    if ((blank || !nl) && plen) {
      int h = lines == 1 ? heading_level(para, plen) : 0;
      if (!strncmp(para, "[Illustration", 13)) { /* a picture that is not here */ }
      else if (h == 2) tline(&t, "== %s ==", para);
      else if (h == 3) tline(&t, "=== %s ===", para);
      else { tput(&t, para, plen); tput(&t, "\n", 1); }
      plen = 0;
      lines = 0;
    }
    if (!nl) break;
    p = nl + 1;
  }
  fm_free(para);
  finish(out, &t);
}

/* "Title: ..." in the text's own header (before the START line). */
static void header_field(const char *text, const char *name, char *out, size_t cap) {
  out[0] = 0;
  const char *end = strstr(text, "*** START OF");
  size_t nl = strlen(name);
  for (const char *p = text; p && (!end || p < end); ) {
    if (!strncmp(p, name, nl)) {
      p += nl;
      while (*p == ' ') p++;
      size_t n = strcspn(p, "\r\n");
      if (n >= cap) n = cap - 1;
      memcpy(out, p, n);
      out[n] = 0;
      return;
    }
    p = strchr(p, '\n');
    if (p) p++;
  }
}

/* Straight from gutenberg.org (Gutendex, the catalog, takes ~20 s and is
** not needed: the text's address follows from the number, and its header
** names the title and the author). */
FmErr rd_guten_book(const char *id, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!id[0] || strlen(id) > 8) { fm_strlcpy(out->error, "Not a Gutenberg book", sizeof out->error); return FM_ERR_FORMAT; }
  for (const char *c = id; *c; c++)
    if (*c < '0' || *c > '9') { fm_strlcpy(out->error, "Not a Gutenberg book", sizeof out->error); return FM_ERR_FORMAT; }
  char url[300];
  /* the https file itself (ebooks/<id>.txt.utf-8 redirects to plain http) */
  fm_snprintf(url, sizeof url, "https://www.gutenberg.org/cache/epub/%s/pg%s.txt", id, id);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e == FM_OK && r.status == 404) fm_strlcpy(out->error, "This book has no text to read here (try Open in browser)", sizeof out->error);
    else if (e == FM_OK) fm_snprintf(out->error, sizeof out->error, "Project Gutenberg answered %d", r.status);
    else fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  const char *text = (const char *)r.data;
  fm_strlcpy(out->id, id, sizeof out->id);
  header_field(text, "Title:", out->title, sizeof out->title);
  if (!out->title[0]) fm_snprintf(out->title, sizeof out->title, "Book %s", id);
  char au[200];
  header_field(text, "Author:", au, sizeof au);
  fm_strlcpy(out->desc, au, sizeof out->desc);
  fm_snprintf(out->lead, sizeof out->lead, "https://www.gutenberg.org/cache/epub/%s/pg%s.cover.medium.jpg", id, id);
  fm_snprintf(out->url, sizeof out->url, "https://www.gutenberg.org/ebooks/%s", id);
  rd_guten_text((char *)r.data, out);
  net_resp_free(&r);
  if (!out->nb) { fm_strlcpy(out->error, "This book's text came empty", sizeof out->error); return FM_ERR_FORMAT; }
  return FM_OK;
}

/* ---- iNaturalist ------------------------------------------------------------------------ */

static void title_case(char *s) {
  if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

static void taxon_page(WkPage *p, const FmJsonNode *t) {
  fm_snprintf(p->id, sizeof p->id, "%.0f", json_num(json_get(t, "id"), 0));
  const char *name = json_str(json_get(t, "name"), "");
  const char *common = json_str(json_get(t, "preferred_common_name"), "");
  fm_strlcpy(p->title, common[0] ? common : name, sizeof p->title);
  title_case(p->title);
  const char *rank = json_str(json_get(t, "rank"), "");
  fm_snprintf(p->desc, sizeof p->desc, "%s%s%s", common[0] ? name : "", common[0] ? " \xC2\xB7 " : "", rank);
  fm_strlcpy(p->thumb, json_str(json_path(t, "default_photo.medium_url"), ""), sizeof p->thumb);
}

static FmErr inat_list(const char *url, WkList *out, int page, volatile int *cancel) {
  FmJson j;
  FmErr e = get_json(url, "iNaturalist", &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  const FmJsonNode *root = json_root(&j);
  for (const FmJsonNode *t = json_first(json_get(root, "results")); t; t = json_next(t)) taxon_page(add(out), t);
  double total = json_num(json_get(root, "total_results"), 0);
  if (page > 0 && total > page * 30.0 && page < 30) out->next = page + 1;
  json_free(&j);
  if (!out->n && page <= 1) fm_strlcpy(out->error, "Nothing found", sizeof out->error);
  return FM_OK;
}

static void locale_of(const char *lang, char *out, size_t cap) {
  fm_strlcpy(out, lang && wk_lang_ok(lang) && strcmp(lang, "simple") ? lang : "en", cap);
}

FmErr rd_inat_search(const char *locale, const char *query, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  if (!query || !query[0]) { fm_strlcpy(out->error, "Type a plant or an animal to search for", sizeof out->error); return FM_ERR_NOT_FOUND; }
  char qe[800], url[1200], lc[16];
  net_urlencode(query, qe, sizeof qe);
  locale_of(locale, lc, sizeof lc);
  fm_snprintf(url, sizeof url, "https://api.inaturalist.org/v1/taxa/autocomplete?q=%s&per_page=30&locale=%s", qe, lc);
  return inat_list(url, out, 0, cancel);
}

FmErr rd_inat_group(const char *locale, const char *taxon, int page, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  for (const char *c = taxon; *c; c++)
    if (*c < '0' || *c > '9') return FM_ERR_FORMAT;
  char url[600], lc[16];
  locale_of(locale, lc, sizeof lc);
  if (page < 1) page = 1;
  fm_snprintf(url, sizeof url,
              "https://api.inaturalist.org/v1/taxa?taxon_id=%s&rank=species&order_by=observations_count&order=desc"
              "&per_page=30&page=%d&locale=%s",
              taxon, page, lc);
  return inat_list(url, out, page, cancel);
}

FmErr rd_inat_parse_taxon(const char *json, size_t len, WkArticle *out, WkMediaList *photos) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "iNaturalist sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *t = json_at(json_get(json_root(&j), "results"), 0);
  if (!t) {
    json_free(&j);
    fm_strlcpy(out->error, "iNaturalist has no such species", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  WkPage p;
  memset(&p, 0, sizeof p);
  taxon_page(&p, t);
  fm_strlcpy(out->id, p.id, sizeof out->id);
  fm_strlcpy(out->title, p.title, sizeof out->title);
  fm_strlcpy(out->desc, p.desc, sizeof out->desc);
  fm_strlcpy(out->lead, json_str(json_path(t, "default_photo.medium_url"), ""), sizeof out->lead);
  out->lead_w = (int)json_num(json_path(t, "default_photo.original_dimensions.width"), 0);
  out->lead_h = (int)json_num(json_path(t, "default_photo.original_dimensions.height"), 0);
  fm_snprintf(out->url, sizeof out->url, "https://www.inaturalist.org/taxa/%s", p.id);
  const char *wu = json_str(json_get(t, "wikipedia_url"), "");
  const char *slash = strstr(wu, "/wiki/");
  if (slash) {
    char tmp[256];
    fm_strlcpy(tmp, slash + 6, sizeof tmp);
    url_decode(tmp, out->wiki, sizeof out->wiki);
    for (char *c = out->wiki; *c; c++)
      if (*c == '_') *c = ' ';
  }
  Txt x = { 0 };
  const char *sum = json_str(json_get(t, "wikipedia_summary"), "");
  if (sum[0]) {
    html_text(sum, &x);
    tput(&x, "\n", 1);
  }
  static const char *const kRanks[] = { "kingdom", "phylum", "class", "order", "family", "genus", "species" };
  tline(&x, "== Classification ==");
  for (const FmJsonNode *a = json_first(json_get(t, "ancestors")); a; a = json_next(a)) {
    const char *rank = json_str(json_get(a, "rank"), "");
    for (int k = 0; k < FM_COUNT(kRanks); k++) {
      if (strcmp(rank, kRanks[k])) continue;
      const char *cn = json_str(json_get(a, "preferred_common_name"), "");
      char r[24];
      fm_strlcpy(r, rank, sizeof r);
      title_case(r);
      tline(&x, "%s: %s%s%s%s", r, json_str(json_get(a, "name"), ""), cn[0] ? " (" : "", cn, cn[0] ? ")" : "");
    }
  }
  {
    char r[24];
    fm_strlcpy(r, json_str(json_get(t, "rank"), ""), sizeof r);
    title_case(r);
    tline(&x, "%s: %s", r, json_str(json_get(t, "name"), ""));
  }
  tline(&x, "== Facts ==");
  tline(&x, "Seen and shared on iNaturalist %.0f times.", json_num(json_get(t, "observations_count"), 0));
  const FmJsonNode *cs = json_get(t, "conservation_status");
  if (cs && cs->type == JSON_OBJ) {
    const char *st = json_str(json_get(cs, "status_name"), json_str(json_get(cs, "status"), ""));
    const char *by = json_str(json_get(cs, "authority"), "");
    if (st[0]) tline(&x, "Conservation status: %s%s%s%s", st, by[0] ? " (" : "", by, by[0] ? ")" : "");
  }
  if (json_bool(json_get(t, "extinct"), false)) tline(&x, "This species is extinct.");
  /* the photos, for the media page */
  int n = 0;
  for (const FmJsonNode *ph = json_first(json_get(t, "taxon_photos")); ph; ph = json_next(ph)) n++;
  if (photos) {
    memset(photos, 0, sizeof *photos);
    photos->it = (WkMedia *)fm_calloc((size_t)(n ? n : 1), sizeof *photos->it);
    for (const FmJsonNode *ph = json_first(json_get(t, "taxon_photos")); ph; ph = json_next(ph)) {
      const FmJsonNode *o = json_get(ph, "photo");
      const char *sq = json_str(json_get(o, "url"), "");
      const char *md = json_str(json_get(o, "medium_url"), sq);
      if (!md[0]) continue;
      WkMedia *m = &photos->it[photos->n++];
      m->type = WM_IMAGE;
      fm_strlcpy(m->thumb, md, sizeof m->thumb);
      fm_strlcpy(m->url, md, sizeof m->url);
      char *sqp = strstr(m->url, "/medium.");                 /* the large rendition next to it */
      if (sqp && strlen(m->url) + 1 < sizeof m->url) {
        char rest[64];
        fm_strlcpy(rest, sqp + 8, sizeof rest);
        fm_snprintf(sqp, sizeof m->url - (size_t)(sqp - m->url), "/large.%s", rest);
      }
      fm_snprintf(m->file, sizeof m->file, "iNaturalist photo %.0f", json_num(json_get(o, "id"), 0));
      fm_strlcpy(m->caption, json_str(json_get(o, "attribution"), ""), sizeof m->caption);
    }
    if (!photos->n) fm_strlcpy(photos->error, "No photos of this one yet", sizeof photos->error);
  }
  json_free(&j);
  finish(out, &x);
  return FM_OK;
}

FmErr rd_inat_taxon(const char *locale, const char *id, WkArticle *out, WkMediaList *photos, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  for (const char *c = id; *c; c++)
    if (*c < '0' || *c > '9') { fm_strlcpy(out->error, "Not an iNaturalist species", sizeof out->error); return FM_ERR_FORMAT; }
  char url[300], lc[16];
  locale_of(locale, lc, sizeof lc);
  fm_snprintf(url, sizeof url, "https://api.inaturalist.org/v1/taxa/%s?locale=%s", id, lc);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e == FM_OK) fm_snprintf(out->error, sizeof out->error, "iNaturalist answered %d", r.status);
    else fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  e = rd_inat_parse_taxon((const char *)r.data, r.len, out, photos);
  net_resp_free(&r);
  return e;
}

/* ---- Fruityvice ------------------------------------------------------------------------------ */

static int page_cmp(const void *a, const void *b) {
  return fm_stricmp(((const WkPage *)a)->title, ((const WkPage *)b)->title);
}

FmErr rd_fruit_list(const char *query, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  FmJson j;
  FmErr e = get_json("https://www.fruityvice.com/api/fruit/all", "Fruityvice", &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  for (const FmJsonNode *f = json_first(json_root(&j)); f; f = json_next(f)) {
    const char *name = json_str(json_get(f, "name"), "");
    if (!name[0] || (query && query[0] && !fm_stristr(name, query))) continue;
    WkPage *p = add(out);
    fm_strlcpy(p->id, name, sizeof p->id);
    fm_strlcpy(p->title, name, sizeof p->title);
    fm_snprintf(p->desc, sizeof p->desc, "%s family \xC2\xB7 %.0f kcal per 100 g", json_str(json_get(f, "family"), ""),
                json_num(json_path(f, "nutritions.calories"), 0));
  }
  json_free(&j);
  if (out->n) qsort(out->it, (size_t)out->n, sizeof *out->it, page_cmp);
  else fm_strlcpy(out->error, "No fruit with that name (Fruityvice knows 49)", sizeof out->error);
  return FM_OK;
}

FmErr rd_fruit(const char *name, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  char ne[300], url[500];
  net_urlencode(name, ne, sizeof ne);
  fm_snprintf(url, sizeof url, "https://www.fruityvice.com/api/fruit/%s", ne);
  FmJson j;
  FmErr e = get_json(url, "Fruityvice", &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  const FmJsonNode *f = json_root(&j);
  const FmJsonNode *nu = json_get(f, "nutritions");
  fm_strlcpy(out->id, name, sizeof out->id);
  fm_strlcpy(out->title, json_str(json_get(f, "name"), name), sizeof out->title);
  fm_snprintf(out->desc, sizeof out->desc, "Fruit \xC2\xB7 %s family", json_str(json_get(f, "family"), ""));
  fm_strlcpy(out->url, "https://www.fruityvice.com/", sizeof out->url);
  fm_strlcpy(out->wiki, out->title, sizeof out->wiki);
  Txt x = { 0 };
  tline(&x, "What 100 grams of %s gives you, from Fruityvice.", out->title);
  tline(&x, "== Nutrition (per 100 g) ==");
  tline(&x, "Calories: %.0f kcal", json_num(json_get(nu, "calories"), 0));
  tline(&x, "Carbohydrates: %.1f g", json_num(json_get(nu, "carbohydrates"), 0));
  tline(&x, "Sugar: %.1f g", json_num(json_get(nu, "sugar"), 0));
  tline(&x, "Protein: %.1f g", json_num(json_get(nu, "protein"), 0));
  tline(&x, "Fat: %.1f g", json_num(json_get(nu, "fat"), 0));
  tline(&x, "== Classification ==");
  tline(&x, "Order: %s", json_str(json_get(f, "order"), "?"));
  tline(&x, "Family: %s", json_str(json_get(f, "family"), "?"));
  tline(&x, "Genus: %s", json_str(json_get(f, "genus"), "?"));
  json_free(&j);
  finish(out, &x);
  return FM_OK;
}

/* ---- Free Dictionary (dictionaryapi.dev) ------------------------------------------------------ */

/* [{word, phonetic, phonetics: [{text, audio}], meanings: [{partOfSpeech,
** definitions: [{definition, example, synonyms}], synonyms, antonyms}],
** sourceUrls}] -- one entry per etymology. 404: no definition. The
** pronunciations (MP3) become the media page's sounds. */
FmErr rd_dict_parse(const char *json, size_t len, WkArticle *out, WkMediaList *sounds) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "The dictionary sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  if (root->type != JSON_ARR || !json_at(root, 0)) {
    fm_strlcpy(out->error, "No definition found for this word", sizeof out->error);
    json_free(&j);
    return FM_ERR_NOT_FOUND;
  }
  const FmJsonNode *e0 = json_at(root, 0);
  fm_strlcpy(out->title, json_str(json_get(e0, "word"), ""), sizeof out->title);
  fm_strlcpy(out->id, out->title, sizeof out->id);
  fm_strlcpy(out->wiki, out->title, sizeof out->wiki);
  fm_snprintf(out->url, sizeof out->url, "https://en.wiktionary.org/wiki/%s", out->title);
  int ns = 0;
  for (const FmJsonNode *e = json_first(root); e; e = json_next(e))
    for (const FmJsonNode *p = json_first(json_get(e, "phonetics")); p; p = json_next(p)) ns++;
  if (sounds) {
    memset(sounds, 0, sizeof *sounds);
    sounds->it = (WkMedia *)fm_calloc((size_t)(ns ? ns : 1), sizeof *sounds->it);
  }
  Txt x = { 0 };
  char phon[200] = "";
  for (const FmJsonNode *e = json_first(root); e; e = json_next(e)) {
    if (!phon[0]) fm_strlcpy(phon, json_str(json_get(e, "phonetic"), ""), sizeof phon);
    for (const FmJsonNode *p = json_first(json_get(e, "phonetics")); p; p = json_next(p)) {
      const char *txt = json_str(json_get(p, "text"), "");
      const char *au = json_str(json_get(p, "audio"), "");
      if (!phon[0] && txt[0]) fm_strlcpy(phon, txt, sizeof phon);
      if (!sounds || !au[0]) continue;
      WkMedia *m = &sounds->it[sounds->n++];
      m->type = WM_AUDIO;
      if (!strncmp(au, "//", 2)) fm_snprintf(m->url, sizeof m->url, "https:%s", au);
      else fm_strlcpy(m->url, au, sizeof m->url);
      /* ".../hello-uk.mp3": where it is said */
      const char *accent = strstr(au, "-uk.") ? "UK" : strstr(au, "-us.") ? "US" : strstr(au, "-au.") ? "Australia" : "";
      fm_snprintf(m->file, sizeof m->file, "%s (%s%s%s)", out->title, txt[0] ? txt : "pronunciation",
                  accent[0] ? ", " : "", accent);
      fm_strlcpy(m->caption, m->file, sizeof m->caption);
    }
  }
  fm_snprintf(out->desc, sizeof out->desc, "%s%sEnglish dictionary", phon, phon[0] ? " \xC2\xB7 " : "");
  for (const FmJsonNode *e = json_first(root); e; e = json_next(e)) {
    for (const FmJsonNode *m = json_first(json_get(e, "meanings")); m; m = json_next(m)) {
      char pos[64];
      fm_strlcpy(pos, json_str(json_get(m, "partOfSpeech"), "meaning"), sizeof pos);
      title_case(pos);
      tline(&x, "== %s ==", pos);
      int k = 1;
      for (const FmJsonNode *d = json_first(json_get(m, "definitions")); d; d = json_next(d), k++) {
        tline(&x, "%d. %s", k, json_str(json_get(d, "definition"), ""));
        const char *ex = json_str(json_get(d, "example"), "");
        if (ex[0]) tline(&x, "    \xE2\x80\x9C%s\xE2\x80\x9D", ex);
      }
      char syn[600] = "";
      size_t sn = 0;
      for (const FmJsonNode *s2 = json_first(json_get(m, "synonyms")); s2 && sn + 40 < sizeof syn; s2 = json_next(s2))
        sn += (size_t)fm_snprintf(syn + sn, sizeof syn - sn, "%s%s", sn ? ", " : "", json_str(s2, ""));
      if (syn[0]) tline(&x, "Similar: %s", syn);
      char ant[600] = "";
      size_t an = 0;
      for (const FmJsonNode *a2 = json_first(json_get(m, "antonyms")); a2 && an + 40 < sizeof ant; a2 = json_next(a2))
        an += (size_t)fm_snprintf(ant + an, sizeof ant - an, "%s%s", an ? ", " : "", json_str(a2, ""));
      if (ant[0]) tline(&x, "Opposite: %s", ant);
    }
  }
  if (sounds && !sounds->n) fm_strlcpy(sounds->error, "No recorded pronunciation for this word", sizeof sounds->error);
  json_free(&j);
  finish(out, &x);
  if (!out->nb) { fm_strlcpy(out->error, "No definition found for this word", sizeof out->error); return FM_ERR_NOT_FOUND; }
  return FM_OK;
}

FmErr rd_dict(const char *word, WkArticle *out, WkMediaList *sounds, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (sounds) memset(sounds, 0, sizeof *sounds);
  if (!word || !word[0]) { fm_strlcpy(out->error, "Type a word", sizeof out->error); return FM_ERR_NOT_FOUND; }
  char we[400], url[600];
  net_urlencode(word, we, sizeof we);
  fm_snprintf(url, sizeof url, "https://api.dictionaryapi.dev/api/v2/entries/en/%s", we);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, &r, cancel);
  if (e != FM_OK || !r.data || (r.status != 200 && r.status != 404)) {
    if (e != FM_OK) fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    else if (r.status >= 520) fm_strlcpy(out->error, "The dictionary service is down right now: try again later, or use Wiktionary",
                                        sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "The dictionary answered %d", r.status);
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  if (r.status == 404) {
    net_resp_free(&r);
    fm_snprintf(out->error, sizeof out->error, "No definition found for \xE2\x80\x9C%s\xE2\x80\x9D", word);
    return FM_ERR_NOT_FOUND;
  }
  e = rd_dict_parse((const char *)r.data, r.len, out, sounds);
  net_resp_free(&r);
  return e;
}
