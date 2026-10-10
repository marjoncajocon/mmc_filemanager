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
**   - DEV Community (dev.to/api/articles?tag=&top=&page=, /articles/<id or user/slug>):
**     articles as HTML (body_html). There is no search in the API (the site's own
**     search answers programs with nothing), so a search is a topic tag.
**   - Stack Exchange API 2.3 (api.stackexchange.com): /questions?tagged=&sort=votes,
**     /search/advanced?q=, /questions/<id> with a filter that brings the answers in
**     the same request. 300 requests a day per network without a key; it can ask
**     callers to wait ("backoff"), which so_fetch honours.
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
#include "fsdl.h"

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

/* The HTML edition (pictures, the book's own contents with links): its
** <title> is "The Project Gutenberg eBook of <title>, by <author>". */
static bool guten_html(const char *id, WkArticle *out, volatile int *cancel) {
  char url[300], base[200];
  fm_snprintf(base, sizeof base, "https://www.gutenberg.org/cache/epub/%s/", id);
  fm_snprintf(url, sizeof url, "%spg%s-images.html", base, id);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) { net_resp_free(&r); return false; }
  const char *h = (const char *)r.data;
  const char *t0 = strstr(h, "<title>");
  if (t0) {
    t0 += 7;
    const char *t1 = strstr(t0, "</title>");
    char t[400];
    size_t n = t1 ? FM_MIN((size_t)(t1 - t0), sizeof t - 1) : 0;
    memcpy(t, t0, n);
    t[n] = 0;
    for (char *c = t; *c; c++)
      if (*c == '\n' || *c == '\r') *c = ' ';
    char *s = t;
    while (*s == ' ') s++;
    if (!strncmp(s, "The Project Gutenberg eBook of ", 31)) s += 31;
    char *by = strstr(s, ", by ");
    if (by) {
      *by = 0;
      fm_strlcpy(out->desc, by + 5, sizeof out->desc);
    }
    fm_strlcpy(out->title, s, sizeof out->title);
    size_t tl = strlen(out->title);
    while (tl && out->title[tl - 1] == ' ') out->title[--tl] = 0;
  }
  /* <meta name="dc.title" content="..."> and dc.creator ("Dale, J. B., Mrs., 1853-1921") */
  static const char *const kMeta[] = { "<meta name=\"dc.title\" content=\"", "<meta name=\"dc.creator\" content=\"" };
  for (int k = 0; k < 2; k++) {
    const char *m = strstr(h, kMeta[k]);
    if (!m) continue;
    m += strlen(kMeta[k]);
    const char *q = strchr(m, '"');
    if (!q) continue;
    char v[300];
    size_t n = FM_MIN((size_t)(q - m), sizeof v - 1);
    memcpy(v, m, n);
    v[n] = 0;
    if (k == 0) { fm_strlcpy(out->title, v, sizeof out->title); continue; }
    /* "Last, First, 1853-1921" -> "First Last" */
    char *c1 = strchr(v, ',');
    if (c1) {
      *c1 = 0;
      char *first = c1 + 1;
      while (*first == ' ') first++;
      char *c2 = strchr(first, ',');
      if (c2) *c2 = 0;
      fm_snprintf(out->desc, sizeof out->desc, "%s %s", first, v);
    } else {
      fm_strlcpy(out->desc, v, sizeof out->desc);
    }
  }
  out->doc = (FmDoc *)fm_calloc(1, sizeof *out->doc);
  doc_parse(h, r.len, base, DOC_BOOK, out->doc);
  net_resp_free(&r);
  if (out->doc->nb < 3) { doc_free(out->doc); fm_free(out->doc); out->doc = NULL; return false; }
  return true;
}

/* Straight from gutenberg.org (Gutendex, the catalog, takes ~20 s and is
** not needed: the files' addresses follow from the number, and the book
** names its title and author). The HTML edition first, else the text. */
FmErr rd_guten_book(const char *id, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!id[0] || strlen(id) > 8) { fm_strlcpy(out->error, "Not a Gutenberg book", sizeof out->error); return FM_ERR_FORMAT; }
  for (const char *c = id; *c; c++)
    if (*c < '0' || *c > '9') { fm_strlcpy(out->error, "Not a Gutenberg book", sizeof out->error); return FM_ERR_FORMAT; }
  fm_strlcpy(out->id, id, sizeof out->id);
  fm_snprintf(out->lead, sizeof out->lead, "https://www.gutenberg.org/cache/epub/%s/pg%s.cover.medium.jpg", id, id);
  fm_snprintf(out->url, sizeof out->url, "https://www.gutenberg.org/ebooks/%s", id);
  if (guten_html(id, out, cancel)) {
    if (!out->title[0]) fm_snprintf(out->title, sizeof out->title, "Book %s", id);
    out->text = fm_strdup("");
    return FM_OK;
  }
  if (cancel && *cancel) return FM_ERR_CANCEL;
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

/* ---- DEV Community (dev.to) ------------------------------------------------------------ */

/* A reply's body as it came; err says why there is none. */
static FmErr get_body(const char *url, const char *site, FmNetResp *r, char *err, size_t cap, volatile int *cancel) {
  memset(r, 0, sizeof *r);
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", cap); net_resp_free(r); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, cap, "Network error: %s", r->error[0] ? r->error : fm_err_str(e));
    net_resp_free(r);
    return e;
  }
  if (r->status == 404) { fm_snprintf(err, cap, "%s has nothing there", site); net_resp_free(r); return FM_ERR_NOT_FOUND; }
  if (r->status == 429) { fm_snprintf(err, cap, "%s is busy: try again in a minute", site); net_resp_free(r); return FM_ERR_IO; }
  if (r->status != 200 || !r->data) {
    fm_snprintf(err, cap, "%s answered %d", site, r->status);
    net_resp_free(r);
    return FM_ERR_IO;
  }
  return FM_OK;
}

static void tputs(Txt *t, const char *s) { tput(t, s, strlen(s)); }

/* Text into HTML (names and tags around a page's own HTML). */
static void hput(Txt *t, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '<': tputs(t, "&lt;"); break;
      case '>': tputs(t, "&gt;"); break;
      case '&': tputs(t, "&amp;"); break;
      case '"': tputs(t, "&quot;"); break;
      default: tput(t, s, 1); break;
    }
  }
}

/* A topic as dev.to tags it: letters and digits ("C++" -> "cpp", "C#" -> "csharp"). */
void rd_devto_tag(const char *q, char *out, size_t cap) {
  size_t o = 0;
  for (const char *s = q; *s && o + 6 < cap; s++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    else if (c == '+') out[o++] = 'p';
    else if (c == '#') { memcpy(out + o, "sharp", 5); o += 5; }
  }
  out[o] = 0;
}

/* dev.to's picture server picks WebP for "format=auto", which this app does
** not decode: JPEG instead (same length, so in place; also "%2C"-escaped). */
static void devto_jpeg(char *s) {
  for (char *f = s; (f = strstr(f, "format=auto")) != NULL; f += 11) memcpy(f + 7, "jpeg", 4);
}

static void devto_page(WkPage *p, const FmJsonNode *a) {
  fm_snprintf(p->id, sizeof p->id, "%.0f", json_num(json_get(a, "id"), 0));
  fm_strlcpy(p->title, json_str(json_get(a, "title"), ""), sizeof p->title);
  fm_snprintf(p->desc, sizeof p->desc, "%s \xC2\xB7 %s \xC2\xB7 %.0f min read \xC2\xB7 \xE2\x99\xA5 %.0f",
              json_str(json_path(a, "user.name"), ""), json_str(json_get(a, "readable_publish_date"), ""),
              json_num(json_get(a, "reading_time_minutes"), 1), json_num(json_get(a, "public_reactions_count"), 0));
  const char *th = json_str(json_get(a, "cover_image"), "");
  if (!th[0]) th = json_str(json_path(a, "user.profile_image_90"), "");
  fm_strlcpy(p->thumb, th, sizeof p->thumb);
  devto_jpeg(p->thumb);
}

FmErr rd_devto_list(const char *tag, int page, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  if (page < 1) page = 1;
  char tg[64], url[400];
  rd_devto_tag(tag ? tag : "", tg, sizeof tg);
  /* a topic: the year's most liked; none: the week's */
  fm_snprintf(url, sizeof url, "https://dev.to/api/articles?per_page=30&page=%d&top=%d%s%s", page, tg[0] ? 365 : 7,
              tg[0] ? "&tag=" : "", tg);
  FmJson j;
  FmErr e = get_json(url, "DEV", &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  int n = 0;
  for (const FmJsonNode *a = json_first(json_root(&j)); a; a = json_next(a), n++) devto_page(add(out), a);
  json_free(&j);
  if (n >= 30 && page < 30) out->next = page + 1;
  if (!out->n && page <= 1)
    fm_snprintf(out->error, sizeof out->error,
                "No articles tagged \xE2\x80\x9C%s\xE2\x80\x9D. DEV finds articles by topic, one word: c, python, javascript, beginners...", tg);
  return FM_OK;
}

FmErr rd_devto_parse(const char *json, size_t len, WkArticle *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "DEV sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *a = json_root(&j);
  const char *body = json_str(json_get(a, "body_html"), "");
  if (!body[0]) {
    json_free(&j);
    fm_strlcpy(out->error, "This article came empty", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  WkPage p;
  memset(&p, 0, sizeof p);
  devto_page(&p, a);
  fm_strlcpy(out->id, p.id, sizeof out->id);
  fm_strlcpy(out->title, p.title, sizeof out->title);
  fm_strlcpy(out->desc, p.desc, sizeof out->desc);
  fm_strlcpy(out->url, json_str(json_get(a, "url"), ""), sizeof out->url);
  fm_strlcpy(out->lead, json_str(json_get(a, "cover_image"), ""), sizeof out->lead);
  devto_jpeg(out->lead);
  if (out->lead[0]) { out->lead_w = 1000; out->lead_h = 420; }   /* dev.to cuts covers to 1000x420 */
  Txt h = { 0 };
  tputs(&h, body);
  tputs(&h, "<hr><p><small>From DEV Community (dev.to), by ");
  hput(&h, json_str(json_path(a, "user.name"), ""));
  const char *tags = json_str(json_get(a, "tags"), "");
  if (tags[0]) {
    tputs(&h, ". Tags: ");
    hput(&h, tags);
  }
  tputs(&h, ".</small></p>");
  json_free(&j);
  devto_jpeg(h.s);
  out->doc = (FmDoc *)fm_calloc(1, sizeof *out->doc);
  doc_parse(h.s, h.n, "https://dev.to/", DOC_WEB, out->doc);
  fm_free(h.s);
  out->text = fm_strdup("");
  return FM_OK;
}

/* id: the number, or "user/slug" (a link to dev.to inside a page). */
FmErr rd_devto_article(const char *id, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  bool ok = id && id[0] && strlen(id) < 400;
  for (const char *c = id; ok && *c; c++)
    ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_/", *c);
  if (!ok) { fm_strlcpy(out->error, "Not a DEV article", sizeof out->error); return FM_ERR_FORMAT; }
  char url[500];
  fm_snprintf(url, sizeof url, "https://dev.to/api/articles/%s", id);
  FmNetResp r;
  FmErr e = get_body(url, "DEV", &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  e = rd_devto_parse((const char *)r.data, r.len, out);
  net_resp_free(&r);
  return e;
}

/* ---- Stack Overflow (Stack Exchange API 2.3) -------------------------------------------- */

/* Asks for the question's body and its answers' (bodies, owners, scores,
** accepted) in one request. Made once with /2.3/filters/create; filters
** never expire. */
#define SO_FILTER "!6WPIomnMNcVD9"
#define SO_API "https://api.stackexchange.com/2.3/"

static volatile u32 g_so_wait;   /* SDL ticks before which Stack Exchange asked for no request ("backoff") */

/* A reply, or why not: the API explains its errors in JSON (error_name,
** error_message), the daily limit among them. */
static FmErr so_fetch(const char *url, FmNetResp *r, char *err, size_t cap, volatile int *cancel) {
  memset(r, 0, sizeof *r);
  for (;;) {
    i32 left = (i32)(g_so_wait - SDL_GetTicks());
    if (left <= 0) break;
    if (left > 20000) { fm_snprintf(err, cap, "Stack Overflow asks to wait %d s", (int)(left / 1000)); return FM_ERR_IO; }
    if (cancel && *cancel) { fm_strlcpy(err, "Cancelled", cap); return FM_ERR_CANCEL; }
    SDL_Delay(100);
  }
  FmErr e = net_get(url, NULL, RD_MAX_REPLY, r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", cap); net_resp_free(r); return e; }
  if (e != FM_OK || !r->data) {
    if (e != FM_OK) fm_snprintf(err, cap, "Network error: %s", r->error[0] ? r->error : fm_err_str(e));
    else fm_snprintf(err, cap, "Stack Overflow answered %d", r->status);
    net_resp_free(r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  FmJson j;
  if (json_parse(&j, (const char *)r->data, r->len) != FM_OK) {
    if (r->status == 200) fm_strlcpy(err, "Stack Overflow sent a reply this app does not understand", cap);
    else fm_snprintf(err, cap, "Stack Overflow answered %d", r->status);
    net_resp_free(r);
    return FM_ERR_IO;
  }
  const FmJsonNode *root = json_root(&j);
  double back = json_num(json_get(root, "backoff"), 0);
  if (back > 0) g_so_wait = SDL_GetTicks() + (u32)(FM_MIN(back, 3600) * 1000);
  const char *ename = json_str(json_get(root, "error_name"), NULL);
  if (ename || r->status != 200) {
    const char *msg = json_str(json_get(root, "error_message"), "");
    if (ename && !strcmp(ename, "throttle_violation"))
      fm_snprintf(err, cap, "Stack Overflow's limit for this network is used up (300 requests a day without a key): %s", msg);
    else if (ename) fm_snprintf(err, cap, "Stack Overflow: %s", msg[0] ? msg : ename);
    else fm_snprintf(err, cap, "Stack Overflow answered %d", r->status);
    json_free(&j);
    net_resp_free(r);
    return FM_ERR_IO;
  }
  json_free(&j);
  return FM_OK;
}

/* The API's text is HTML-escaped ("don&#39;t"): back to plain text. */
static void unescape(const char *s, char *out, size_t cap) {
  Txt t = { 0 };
  html_text(s, &t);
  fm_strlcpy(out, t.s ? t.s : "", cap);
  fm_free(t.s);
}

static void tags_of(const FmJsonNode *tags, int max, char *out, size_t cap) {
  out[0] = 0;
  size_t n = 0;
  int k = 0;
  for (const FmJsonNode *t = json_first(tags); t && k < max && n < cap; t = json_next(t), k++)
    n += (size_t)fm_snprintf(out + n, cap - n, "%s%s", k ? ", " : "", json_str(t, ""));
}

static void so_page(WkPage *p, const FmJsonNode *q) {
  fm_snprintf(p->id, sizeof p->id, "%.0f", json_num(json_get(q, "question_id"), 0));
  unescape(json_str(json_get(q, "title"), ""), p->title, sizeof p->title);
  char tg[120];
  tags_of(json_get(q, "tags"), 4, tg, sizeof tg);
  int na = (int)json_num(json_get(q, "answer_count"), 0);
  fm_snprintf(p->desc, sizeof p->desc, "\xE2\x96\xB2 %.0f \xC2\xB7 %d answer%s%s \xC2\xB7 %s", json_num(json_get(q, "score"), 0), na,
              na == 1 ? "" : "s", json_get(q, "accepted_answer_id") ? " \xE2\x9C\x93" : "", tg);
}

static FmErr so_list(const char *url, int page, WkList *out, volatile int *cancel) {
  FmNetResp r;
  FmErr e = so_fetch(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  FmJson j;
  e = json_parse(&j, (const char *)r.data, r.len);   /* so_fetch parsed it once already */
  net_resp_free(&r);
  if (e != FM_OK) return e;
  const FmJsonNode *root = json_root(&j);
  for (const FmJsonNode *q = json_first(json_get(root, "items")); q; q = json_next(q)) so_page(add(out), q);
  if (json_bool(json_get(root, "has_more"), false) && page < 25) out->next = page + 1;
  json_free(&j);
  if (!out->n && page <= 1) fm_strlcpy(out->error, "No questions found", sizeof out->error);
  return FM_OK;
}

/* A tag's best voted questions ("" = all of Stack Overflow's). */
FmErr rd_so_list(const char *tag, int page, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  if (page < 1) page = 1;
  char te[200] = "", url[600];
  net_urlencode(tag ? tag : "", te, sizeof te);
  fm_snprintf(url, sizeof url, SO_API "questions?order=desc&sort=votes&site=stackoverflow&pagesize=30&page=%d%s%s", page,
              te[0] ? "&tagged=" : "", te);
  return so_list(url, page, out, cancel);
}

/* Words in titles and bodies; only questions with an answer. */
FmErr rd_so_search(const char *query, int page, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof out[0]);
  out->next = -1;
  if (page < 1) page = 1;
  if (!query || !query[0]) { fm_strlcpy(out->error, "Type a question to search for", sizeof out->error); return FM_ERR_NOT_FOUND; }
  char qe[800], url[1200];
  net_urlencode(query, qe, sizeof qe);
  fm_snprintf(url, sizeof url,
              SO_API "search/advanced?order=desc&sort=relevance&answers=1&site=stackoverflow&pagesize=30&page=%d&q=%s", page, qe);
  return so_list(url, page, out, cancel);
}

static int answer_cmp(const void *a, const void *b) {
  const FmJsonNode *x = *(const FmJsonNode *const *)a, *y = *(const FmJsonNode *const *)b;
  bool ax = json_bool(json_get(x, "is_accepted"), false), ay = json_bool(json_get(y, "is_accepted"), false);
  if (ax != ay) return ax ? -1 : 1;
  double sx = json_num(json_get(x, "score"), 0), sy = json_num(json_get(y, "score"), 0);
  return sx > sy ? -1 : sx < sy ? 1 : 0;
}

/* The question, then its answers: the accepted one first, then by votes. */
FmErr rd_so_parse(const char *json, size_t len, WkArticle *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Stack Overflow sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *q = json_first(json_get(json_root(&j), "items"));
  if (!q) {
    json_free(&j);
    fm_strlcpy(out->error, "This question is gone (deleted, or not on Stack Overflow)", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  WkPage p;
  memset(&p, 0, sizeof p);
  so_page(&p, q);
  fm_strlcpy(out->id, p.id, sizeof out->id);
  fm_strlcpy(out->title, p.title, sizeof out->title);
  char who[120], tg[200];
  unescape(json_str(json_path(q, "owner.display_name"), ""), who, sizeof who);
  tags_of(json_get(q, "tags"), 8, tg, sizeof tg);
  fm_snprintf(out->desc, sizeof out->desc, "Asked by %s \xC2\xB7 \xE2\x96\xB2 %.0f \xC2\xB7 %.0f views \xC2\xB7 %s",
              who[0] ? who : "someone", json_num(json_get(q, "score"), 0), json_num(json_get(q, "view_count"), 0), tg);
  fm_strlcpy(out->url, json_str(json_get(q, "link"), ""), sizeof out->url);
  if (!out->url[0]) fm_snprintf(out->url, sizeof out->url, "https://stackoverflow.com/questions/%s", out->id);
  /* the API's strings are HTML already: names and bodies go in as they are */
  Txt h = { 0 };
  tputs(&h, json_str(json_get(q, "body"), ""));
  const FmJsonNode *ans = json_get(q, "answers");
  int na = 0;
  const FmJsonNode **v = (const FmJsonNode **)fm_alloc(sizeof *v * (size_t)FM_MAX(1, ans ? ans->count : 0));
  for (const FmJsonNode *a = json_first(ans); a; a = json_next(a)) v[na++] = a;
  qsort(v, (size_t)na, sizeof *v, answer_cmp);
  char line[400];
  fm_snprintf(line, sizeof line, "<h2>%d answer%s</h2>", na, na == 1 ? "" : "s");
  tputs(&h, line);
  if (!na) tputs(&h, "<p>No answers yet.</p>");
  for (int i = 0; i < na; i++) {
    const FmJsonNode *a = v[i];
    bool acc = json_bool(json_get(a, "is_accepted"), false);
    fm_snprintf(line, sizeof line, "<h3>%s \xC2\xB7 \xE2\x96\xB2 %.0f</h3><p><small>by %s</small></p>",
                acc ? "\xE2\x9C\x93 Accepted answer" : "Answer", json_num(json_get(a, "score"), 0),
                json_str(json_path(a, "owner.display_name"), "someone"));
    tputs(&h, line);
    tputs(&h, json_str(json_get(a, "body"), ""));
  }
  fm_free(v);
  tputs(&h, "<hr><p><small>From Stack Overflow, by its users (CC BY-SA). Open in browser for comments and votes.</small></p>");
  json_free(&j);
  out->doc = (FmDoc *)fm_calloc(1, sizeof *out->doc);
  doc_parse(h.s, h.n, "https://stackoverflow.com/", DOC_WEB, out->doc);
  fm_free(h.s);
  out->text = fm_strdup("");
  return FM_OK;
}

FmErr rd_so_question(const char *id, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  bool ok = id && id[0] && strlen(id) <= 12;
  for (const char *c = id; ok && *c; c++) ok = *c >= '0' && *c <= '9';
  if (!ok) { fm_strlcpy(out->error, "Not a Stack Overflow question", sizeof out->error); return FM_ERR_FORMAT; }
  char url[300];
  fm_snprintf(url, sizeof url, SO_API "questions/%s?site=stackoverflow&filter=" SO_FILTER, id);
  FmNetResp r;
  FmErr e = so_fetch(url, &r, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  e = rd_so_parse((const char *)r.data, r.len, out);
  net_resp_free(&r);
  return e;
}
