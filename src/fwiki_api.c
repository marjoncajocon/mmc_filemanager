/* fwiki_api.c -- Wikipedia's public APIs (see fwiki_int.h).
**
** Checked from this PC (2026-10-10), all with our plain User-Agent:
**   - Action API  https://<lang>.wikipedia.org/w/api.php (format=json, formatversion=2):
**       search    generator=search + prop=pageimages|description (thumbnails, short text)
**       article   prop=extracts&explaintext&exsectionformat=wiki: the whole article as
**                 plain text, headings as "== Name ==" lines; + pageimages (lead picture),
**                 description, info (the page URL); redirects followed
**       langlinks, list=random, links, search "morelike:" (related)
**       files     prop=videoinfo (videos, sounds: their transcodes, e.g. 240p..1080p
**                 VP9/Opus WebM and MP3) and prop=imageinfo&iiurlwidth (a picture sized
**                 for the screen)
**   - REST API    https://<lang>.wikipedia.org/api/rest_v1/
**       feed/featured/YYYY/MM/DD   featured article, most read, picture of the day,
**                                  on this day (some languages only)
**       page/media-list/<title>    the pictures, videos and sounds of an article
**   - Thumbnails come as URLs from the APIs: upload.wikimedia.org only serves its
**     standard sizes (a made-up 320px answers 400), so URLs are never built here.
**
** Design decisions:
**   - Plain text, not HTML: the app draws wrapped text and headings; infoboxes,
**     tables and formulas are left to "Open in browser". TeX formulas leave
**     "{\displaystyle ...}" in the plain text: cut out.
**   - Headings with nothing under them (References, External links: empty in
**     plain text) are dropped.
*/
#include "fwiki_int.h"
#include "fjson.h"
#include "fnet.h"
#include <time.h>

#define WK_MAX_REPLY (8u << 20)

/* Wikimedia's rate limits (mediawiki.org Wikimedia_APIs/Rate_limits, 2026):
** 10 requests a minute for a client it cannot identify, 200 for one whose
** User-Agent carries contact details. The contact the user gives goes in an
** Api-User-Agent header (accepted for identification; our session's own
** User-Agent stays as it is on every network backend). */
static char g_hdr[400];

void wk_set_contact(const char *contact) {
  char c[300];
  fm_strlcpy(c, contact ? contact : "", sizeof c);
  for (char *p = c; *p; p++)
    if ((u8)*p < ' ') *p = ' ';
  if (c[0]) fm_snprintf(g_hdr, sizeof g_hdr, "Api-User-Agent: mmcfm/" FM_VERSION " (MMC File Manager; %s)\r\n", c);
  else g_hdr[0] = 0;
}

const char *wk_headers(void) { return g_hdr[0] ? g_hdr : NULL; }

static void limited(char *err, size_t cap) {
  fm_strlcpy(err, g_hdr[0] ? "Wikipedia is limiting requests for a minute: try again soon"
                           : "Wikipedia allows only 10 requests a minute without a contact: add yours (\xE2\x8B\xAE > Wikimedia contact)",
             cap);
}

bool wk_lang_ok(const char *lang) {
  size_t n = lang ? strlen(lang) : 0;
  if (n < 2 || n >= WK_LANG) return false;
  for (size_t i = 0; i < n; i++)
    if (!((lang[i] >= 'a' && lang[i] <= 'z') || lang[i] == '-')) return false;
  return true;
}

static FmErr get_json(const char *url, FmJson *j, char *err, size_t cap, volatile int *cancel) {
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e == FM_ERR_CANCEL) { fm_strlcpy(err, "Cancelled", cap); net_resp_free(&r); return e; }
  if (e != FM_OK) {
    fm_snprintf(err, cap, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e;
  }
  if (r.status == 429) {
    limited(err, cap);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  if (r.status == 404) {
    fm_strlcpy(err, "Wikipedia has nothing there", cap);
    net_resp_free(&r);
    return FM_ERR_NOT_FOUND;
  }
  if (r.status != 200 || !r.data) {
    fm_snprintf(err, cap, "Wikipedia answered %d", r.status);
    net_resp_free(&r);
    return FM_ERR_IO;
  }
  e = json_parse(j, (const char *)r.data, r.len);
  net_resp_free(&r);
  if (e != FM_OK) fm_strlcpy(err, "Wikipedia sent a reply this app does not understand", cap);
  return e;
}

static void api_url(char *out, size_t cap, const char *lang, const char *params) {
  fm_snprintf(out, cap, "https://%s.wikipedia.org/w/api.php?format=json&formatversion=2&%s", lang, params);
}

/* A title for a REST path: spaces as underscores, then percent-encoded. */
static void rest_title(const char *title, char *out, size_t cap) {
  char t[300];
  fm_strlcpy(t, title, sizeof t);
  for (char *p = t; *p; p++)
    if (*p == ' ') *p = '_';
  net_urlencode(t, out, cap);
}

static void copy_url(char *out, size_t cap, const char *u) {
  if (!u || !u[0]) { out[0] = 0; return; }
  if (!strncmp(u, "//", 2)) fm_snprintf(out, cap, "https:%s", u);
  else fm_strlcpy(out, u, cap);
}

static WkPage *list_add(WkList *l) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 32;
    l->it = (WkPage *)fm_realloc(l->it, (size_t)l->cap * sizeof *l->it);
  }
  WkPage *p = &l->it[l->n++];
  memset(p, 0, sizeof *p);
  return p;
}

void wk_list_free(WkList *l) {
  if (!l) return;
  fm_free(l->it);
  memset(l, 0, sizeof *l);
}

static void page_from(WkPage *p, const FmJsonNode *n) {
  fm_strlcpy(p->title, json_str(json_get(n, "title"), ""), sizeof p->title);
  fm_strlcpy(p->desc, json_str(json_get(n, "description"), ""), sizeof p->desc);
  copy_url(p->thumb, sizeof p->thumb, json_str(json_path(n, "thumbnail.source"), ""));
}

/* ---- search, related, links, random -------------------------------------------------- */

typedef struct Idx { int idx; const FmJsonNode *n; } Idx;

static int idx_cmp(const void *a, const void *b) { return ((const Idx *)a)->idx - ((const Idx *)b)->idx; }

FmErr wk_parse_search(const char *json, size_t len, WkList *out) {
  FmJson j;
  out->next = -1;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikipedia sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *pages = json_path(root, "query.pages");
  /* generator results come unordered: "index" is the rank */
  Idx *ix = (Idx *)fm_calloc(512, sizeof *ix);
  int n = 0;
  for (const FmJsonNode *p = json_first(pages); p && n < 512; p = json_next(p)) {
    ix[n].idx = (int)json_num(json_get(p, "index"), n + 1000);
    ix[n++].n = p;
  }
  qsort(ix, (size_t)n, sizeof *ix, idx_cmp);
  for (int i = 0; i < n; i++) page_from(list_add(out), ix[i].n);
  fm_free(ix);
  const FmJsonNode *c = json_get(root, "continue");
  if (c) out->next = (int)json_num(json_get(c, "gsroffset"), -1);
  json_free(&j);
  return FM_OK;
}

static FmErr gen_search(const char *lang, const char *srsearch, int offset, int limit, WkList *out,
                        volatile int *cancel) {
  if (!wk_lang_ok(lang)) { fm_strlcpy(out->error, "Not a Wikipedia language", sizeof out->error); return FM_ERR_FORMAT; }
  char qe[1200], p[1600], url[2000];
  net_urlencode(srsearch, qe, sizeof qe);
  fm_snprintf(p, sizeof p,
              "action=query&generator=search&gsrsearch=%s&gsrlimit=%d&gsroffset=%d&gsrnamespace=0"
              "&prop=pageimages|description&piprop=thumbnail&pithumbsize=330&pilimit=%d&redirects=1",
              qe, limit, offset < 0 ? 0 : offset, limit);
  api_url(url, sizeof url, lang, p);
  FmJson j;
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e == FM_ERR_CANCEL) fm_strlcpy(out->error, "Cancelled", sizeof out->error);
    else if (e != FM_OK) fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    else if (r.status == 429) limited(out->error, sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "Wikipedia answered %d", r.status);
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  (void)j;
  e = wk_parse_search((const char *)r.data, r.len, out);
  net_resp_free(&r);
  return e;
}

FmErr wk_search(const char *lang, const char *query, int offset, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->next = -1;
  if (!query || !query[0]) { fm_strlcpy(out->error, "Type something to search for", sizeof out->error); return FM_ERR_NOT_FOUND; }
  FmErr e = gen_search(lang, query, offset, 20, out, cancel);
  if (e == FM_OK && !out->n && offset <= 0) fm_strlcpy(out->error, "No articles found", sizeof out->error);
  return e;
}

FmErr wk_related(const char *lang, const char *title, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->next = -1;
  char q[320];
  fm_snprintf(q, sizeof q, "morelike:%s", title);
  FmErr e = gen_search(lang, q, 0, 20, out, cancel);
  out->next = -1;
  return e;
}

FmErr wk_links(const char *lang, const char *title, WkList *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  out->next = -1;
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  char te[900], p[1200], url[1600];
  net_urlencode(title, te, sizeof te);
  fm_snprintf(p, sizeof p, "action=query&prop=links&titles=%s&pllimit=500&plnamespace=0&redirects=1", te);
  api_url(url, sizeof url, lang, p);
  FmJson j;
  FmErr e = get_json(url, &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  for (const FmJsonNode *l = json_first(json_path(json_root(&j), "query.pages.0.links")); l; l = json_next(l))
    fm_strlcpy(list_add(out)->title, json_str(json_get(l, "title"), ""), sizeof out->it[0].title);
  json_free(&j);
  if (!out->n) fm_strlcpy(out->error, "This article links to no other article", sizeof out->error);
  return FM_OK;
}

FmErr wk_random(const char *lang, char *title, size_t cap, volatile int *cancel) {
  title[0] = 0;
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  char url[400], err[200];
  api_url(url, sizeof url, lang, "action=query&list=random&rnnamespace=0&rnlimit=1");
  FmJson j;
  FmErr e = get_json(url, &j, err, sizeof err, cancel);
  if (e != FM_OK) return e;
  fm_strlcpy(title, json_str(json_path(json_root(&j), "query.random.0.title"), ""), cap);
  json_free(&j);
  return title[0] ? FM_OK : FM_ERR_NOT_FOUND;
}

/* ---- the featured feed ---------------------------------------------------------------- */

void wk_feed_free(WkFeed *f) {
  if (!f) return;
  wk_list_free(&f->most);
  memset(f, 0, sizeof *f);
}

static void feed_page(WkPage *p, const FmJsonNode *n) {
  const char *t = json_str(json_path(n, "titles.normalized"), "");
  fm_strlcpy(p->title, t[0] ? t : json_str(json_get(n, "title"), ""), sizeof p->title);
  fm_strlcpy(p->desc, json_str(json_get(n, "description"), ""), sizeof p->desc);
  copy_url(p->thumb, sizeof p->thumb, json_str(json_path(n, "thumbnail.source"), ""));
}

FmErr wk_parse_feed(const char *json, size_t len, WkFeed *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikipedia sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *root = json_root(&j);
  const FmJsonNode *tfa = json_get(root, "tfa");
  if (tfa) {
    out->has_tfa = true;
    feed_page(&out->tfa, tfa);
    fm_strlcpy(out->tfa_extract, json_str(json_get(tfa, "extract"), ""), sizeof out->tfa_extract);
  }
  int k = 0;
  for (const FmJsonNode *a = json_first(json_path(root, "mostread.articles")); a && k < 30; a = json_next(a), k++) {
    const char *t = json_str(json_path(a, "titles.normalized"), json_str(json_get(a, "title"), ""));
    if (!strcmp(t, "Main Page") || strchr(t, ':')) continue;     /* Special:, the main page */
    feed_page(list_add(&out->most), a);
  }
  const FmJsonNode *img = json_get(root, "image");
  if (img) {
    out->has_potd = true;
    fm_strlcpy(out->potd_title, json_str(json_get(img, "title"), ""), sizeof out->potd_title);
    copy_url(out->potd_thumb, sizeof out->potd_thumb, json_str(json_path(img, "thumbnail.source"), ""));
    copy_url(out->potd_full, sizeof out->potd_full, json_str(json_path(img, "image.source"), ""));
    fm_strlcpy(out->potd_desc, json_str(json_path(img, "description.text"), ""), sizeof out->potd_desc);
  }
  for (const FmJsonNode *o = json_first(json_get(root, "onthisday")); o && out->notd < FM_COUNT(out->otd);
       o = json_next(o)) {
    WkOtd *d = &out->otd[out->notd++];
    d->year = (int)json_num(json_get(o, "year"), 0);
    fm_strlcpy(d->text, json_str(json_get(o, "text"), ""), sizeof d->text);
    feed_page(&d->page, json_at(json_get(o, "pages"), 0));
  }
  json_free(&j);
  return FM_OK;
}

FmErr wk_feed(const char *lang, WkFeed *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  time_t now = time(NULL);
  struct tm g;
  const struct tm *gp = gmtime(&now);                /* tcc's msvcrt has no gmtime_s */
  if (!gp) return FM_ERR_IO;
  g = *gp;
  char url[300];
  fm_snprintf(url, sizeof url, "https://%s.wikipedia.org/api/rest_v1/feed/featured/%04d/%02d/%02d", lang,
              g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e == FM_OK && r.status == 429) limited(out->error, sizeof out->error);
    else if (e == FM_OK) fm_strlcpy(out->error, "No featured page in this language: search, or try Random", sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_NOT_FOUND;
  }
  e = wk_parse_feed((const char *)r.data, r.len, out);
  net_resp_free(&r);
  return e;
}

/* ---- articles ------------------------------------------------------------------------ */

void wk_article_free(WkArticle *a) {
  if (!a) return;
  fm_free(a->text);
  fm_free(a->b);
  memset(a, 0, sizeof *a);
}

/* Cuts "{\displaystyle ...}" (a TeX formula's source) out of s, in place. */
static void cut_tex(char *s) {
  static const char kTex[] = "{\\displaystyle";
  char *o = s;
  for (char *p = s; *p;) {
    if (*p == '{' && !strncmp(p, kTex, sizeof kTex - 1)) {
      int depth = 0;
      while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) { p++; break; }
        p++;
      }
      continue;
    }
    *o++ = *p++;
  }
  *o = 0;
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  return s;
}

/* Splits the plain text into headings and paragraphs (pointers into a->text). */
static void split_blocks(WkArticle *a) {
  int cap = 64;
  a->b = (WkBlock *)fm_alloc((size_t)cap * sizeof *a->b);
  a->nb = 0;
  for (char *line = a->text; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *t = trim(line);
    if (*t) {
      int kind = WB_PARA;
      size_t n = strlen(t);
      if (n >= 4 && t[0] == '=' && t[1] == '=' && t[n - 1] == '=') {
        int lv = 0;
        while (t[lv] == '=') lv++;
        int rv = 0;
        while (rv < (int)n && t[n - 1 - rv] == '=') rv++;
        if (lv >= 2 && rv >= 2 && (size_t)(lv + rv) < n) {
          t[n - (size_t)rv] = 0;
          t = trim(t + lv);
          kind = lv == 2 ? WB_H2 : lv == 3 ? WB_H3 : WB_H4;
        }
      }
      if (*t) {
        if (a->nb == cap) {
          cap *= 2;
          a->b = (WkBlock *)fm_realloc(a->b, (size_t)cap * sizeof *a->b);
        }
        a->b[a->nb].kind = kind;
        a->b[a->nb++].s = t;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  /* headings with nothing under them go (References, External links...) */
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < a->nb; i++) {
      if (a->b[i].kind == WB_PARA) continue;
      bool empty = i + 1 >= a->nb || (a->b[i + 1].kind != WB_PARA && a->b[i + 1].kind <= a->b[i].kind);
      if (!empty) continue;
      memmove(&a->b[i], &a->b[i + 1], (size_t)(a->nb - i - 1) * sizeof *a->b);
      a->nb--;
      again = true;
      break;
    }
  }
}

FmErr wk_parse_article(const char *json, size_t len, const char *lang, WkArticle *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikipedia sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *p = json_path(json_root(&j), "query.pages.0");
  if (!p || json_bool(json_get(p, "missing"), false) || json_get(p, "invalid")) {
    fm_strlcpy(out->error, "Wikipedia has no article with this name", sizeof out->error);
    json_free(&j);
    return FM_ERR_NOT_FOUND;
  }
  fm_strlcpy(out->lang, lang, sizeof out->lang);
  fm_strlcpy(out->title, json_str(json_get(p, "title"), ""), sizeof out->title);
  fm_strlcpy(out->desc, json_str(json_get(p, "description"), ""), sizeof out->desc);
  fm_strlcpy(out->url, json_str(json_get(p, "fullurl"), ""), sizeof out->url);
  const FmJsonNode *th = json_get(p, "thumbnail");
  copy_url(out->lead, sizeof out->lead, json_str(json_get(th, "source"), ""));
  out->lead_w = (int)json_num(json_get(th, "width"), 0);
  out->lead_h = (int)json_num(json_get(th, "height"), 0);
  const char *ex = json_str(json_get(p, "extract"), "");
  out->text = fm_strdup(ex);
  json_free(&j);
  cut_tex(out->text);
  split_blocks(out);
  if (!out->nb) {
    fm_strlcpy(out->error, "This article has no text (a list or a disambiguation page?): try Open in browser",
               sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  return FM_OK;
}

FmErr wk_article(const char *lang, const char *title, WkArticle *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wk_lang_ok(lang)) { fm_strlcpy(out->error, "Not a Wikipedia language", sizeof out->error); return FM_ERR_FORMAT; }
  char te[900], p[1400], url[1800];
  net_urlencode(title, te, sizeof te);
  fm_snprintf(p, sizeof p,
              "action=query&prop=extracts|pageimages|description|info&explaintext=1&exsectionformat=wiki"
              "&titles=%s&redirects=1&piprop=thumbnail&pithumbsize=960&inprop=url",
              te);
  api_url(url, sizeof url, lang, p);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e == FM_ERR_CANCEL) fm_strlcpy(out->error, "Cancelled", sizeof out->error);
    else if (e != FM_OK) fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    else if (r.status == 429) limited(out->error, sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "Wikipedia answered %d", r.status);
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  e = wk_parse_article((const char *)r.data, r.len, lang, out);
  net_resp_free(&r);
  return e;
}

/* ---- media --------------------------------------------------------------------------------- */

void wk_media_free(WkMediaList *m) {
  if (!m) return;
  fm_free(m->it);
  memset(m, 0, sizeof *m);
}

FmErr wk_parse_media(const char *json, size_t len, WkMediaList *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikipedia sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *items = json_get(json_root(&j), "items");
  int cap = 0;
  for (const FmJsonNode *x = json_first(items); x; x = json_next(x)) cap++;
  out->it = (WkMedia *)fm_calloc((size_t)(cap ? cap : 1), sizeof *out->it);
  for (const FmJsonNode *x = json_first(items); x; x = json_next(x)) {
    const char *ty = json_str(json_get(x, "type"), "");
    int type = !strcmp(ty, "video") ? WM_VIDEO : !strcmp(ty, "audio") ? WM_AUDIO : !strcmp(ty, "image") ? WM_IMAGE : -1;
    /* pictures marked out of the gallery are icons and flags; sounds (an
    ** anthem in an infobox) are marked so too, and are kept */
    if (type == WM_IMAGE && !json_bool(json_get(x, "showInGallery"), true)) continue;
    const char *file = json_str(json_get(x, "title"), "");
    if (type < 0 || strncmp(file, "File:", 5)) continue;
    WkMedia *m = &out->it[out->n++];
    m->type = type;
    fm_strlcpy(m->file, file, sizeof m->file);
    /* srcset: the 1x, 1.5x, 2x renders; the 1.5x is about grid size */
    const FmJsonNode *ss = json_get(x, "srcset");
    const FmJsonNode *pick = json_at(ss, 1) ? json_at(ss, 1) : json_at(ss, 0);
    copy_url(m->thumb, sizeof m->thumb, json_str(json_get(pick, "src"), ""));
    fm_strlcpy(m->caption, json_str(json_path(x, "caption.text"), ""), sizeof m->caption);
  }
  json_free(&j);
  if (!out->n) fm_strlcpy(out->error, "This article has no pictures, videos or sounds", sizeof out->error);
  return FM_OK;
}

FmErr wk_media(const char *lang, const char *title, WkMediaList *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  char te[900], url[1200];
  rest_title(title, te, sizeof te);
  fm_snprintf(url, sizeof url, "https://%s.wikipedia.org/api/rest_v1/page/media-list/%s", lang, te);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e != FM_OK) fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    else if (r.status == 429) limited(out->error, sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "Wikipedia answered %d", r.status);
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  e = wk_parse_media((const char *)r.data, r.len, out);
  net_resp_free(&r);
  return e;
}

/* The best rendition: a picture sized for the screen (imageinfo thumburl);
** for video a VP9/Opus WebM no taller than 480 (the built-in decoder plays it
** on every system), else any WebM, else the original; for sound the MP3
** transcode (streams everywhere), else the original. */
FmErr wk_parse_file(const char *json, size_t len, int type, WkFile *out) {
  FmJson j;
  if (json_parse(&j, json, len) != FM_OK) {
    fm_strlcpy(out->error, "Wikipedia sent a reply this app does not understand", sizeof out->error);
    return FM_ERR_FORMAT;
  }
  const FmJsonNode *p = json_path(json_root(&j), "query.pages.0");
  FmErr e = FM_OK;
  if (type == WM_IMAGE) {
    const FmJsonNode *ii = json_at(json_get(p, "imageinfo"), 0);
    const char *u = json_str(json_get(ii, "thumburl"), "");
    if (!u[0]) u = json_str(json_get(ii, "url"), "");
    copy_url(out->url, sizeof out->url, u);
    fm_strlcpy(out->mime, json_str(json_get(ii, "mime"), ""), sizeof out->mime);
    out->w = (int)json_num(json_get(ii, "thumbwidth"), json_num(json_get(ii, "width"), 0));
    out->h = (int)json_num(json_get(ii, "thumbheight"), json_num(json_get(ii, "height"), 0));
  } else {
    const FmJsonNode *vi = json_at(json_get(p, "videoinfo"), 0);
    const char *best = NULL;
    int best_h = -1;
    const char *any_webm = NULL, *mp3 = NULL;
    for (const FmJsonNode *d = json_first(json_get(vi, "derivatives")); d; d = json_next(d)) {
      const char *ty = json_str(json_get(d, "type"), "");
      const char *src = json_str(json_get(d, "src"), "");
      if (!src[0]) continue;
      if (strstr(ty, "audio/mpeg")) mp3 = src;
      if (!strstr(ty, "video/webm")) continue;
      if (!any_webm) any_webm = src;
      int h = (int)json_num(json_get(d, "height"), 0);
      if (strstr(ty, "vp9") && h <= 480 && h > best_h) { best = src; best_h = h; }   /* downloaded whole: kept small */
    }
    const char *orig = json_str(json_get(vi, "url"), "");
    const char *u = type == WM_AUDIO ? (mp3 ? mp3 : orig) : best ? best : any_webm ? any_webm : orig;
    copy_url(out->url, sizeof out->url, u);
    fm_strlcpy(out->mime, json_str(json_get(vi, "mime"), ""), sizeof out->mime);
    out->w = (int)json_num(json_get(vi, "width"), 0);
    out->h = best_h > 0 ? best_h : (int)json_num(json_get(vi, "height"), 0);
  }
  if (!out->url[0]) {
    fm_strlcpy(out->error, "Wikipedia lists no file for this", sizeof out->error);
    e = FM_ERR_NOT_FOUND;
  }
  json_free(&j);
  return e;
}

FmErr wk_file(const char *lang, const char *file, int type, WkFile *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  char fe[900], p[1300], url[1700];
  net_urlencode(file, fe, sizeof fe);
  if (type == WM_IMAGE)
    fm_snprintf(p, sizeof p, "action=query&titles=%s&prop=imageinfo&iiprop=url|mime|size&iiurlwidth=1920", fe);
  else
    fm_snprintf(p, sizeof p, "action=query&titles=%s&prop=videoinfo&viprop=url|mime|size|derivatives", fe);
  api_url(url, sizeof url, lang, p);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  FmErr e = net_get(url, wk_headers(), WK_MAX_REPLY, &r, cancel);
  if (e != FM_OK || r.status != 200 || !r.data) {
    if (e != FM_OK) fm_snprintf(out->error, sizeof out->error, "Network error: %s", r.error[0] ? r.error : fm_err_str(e));
    else if (r.status == 429) limited(out->error, sizeof out->error);
    else fm_snprintf(out->error, sizeof out->error, "Wikipedia answered %d", r.status);
    net_resp_free(&r);
    return e != FM_OK ? e : FM_ERR_IO;
  }
  e = wk_parse_file((const char *)r.data, r.len, type, out);
  net_resp_free(&r);
  return e;
}

/* ---- languages ------------------------------------------------------------------------------ */

void wk_langs_free(WkLangs *l) {
  if (!l) return;
  fm_free(l->it);
  memset(l, 0, sizeof *l);
}

FmErr wk_langs(const char *lang, const char *title, WkLangs *out, volatile int *cancel) {
  memset(out, 0, sizeof *out);
  if (!wk_lang_ok(lang)) return FM_ERR_FORMAT;
  char te[900], p[1200], url[1600];
  net_urlencode(title, te, sizeof te);
  fm_snprintf(p, sizeof p, "action=query&prop=langlinks&titles=%s&lllimit=500&llprop=autonym&redirects=1", te);
  api_url(url, sizeof url, lang, p);
  FmJson j;
  FmErr e = get_json(url, &j, out->error, sizeof out->error, cancel);
  if (e != FM_OK) return e;
  const FmJsonNode *ll = json_path(json_root(&j), "query.pages.0.langlinks");
  int cap = 0;
  for (const FmJsonNode *x = json_first(ll); x; x = json_next(x)) cap++;
  out->it = (WkLang *)fm_calloc((size_t)(cap ? cap : 1), sizeof *out->it);
  for (const FmJsonNode *x = json_first(ll); x; x = json_next(x)) {
    const char *code = json_str(json_get(x, "lang"), "");
    if (!wk_lang_ok(code)) continue;
    WkLang *l = &out->it[out->n++];
    fm_strlcpy(l->lang, code, sizeof l->lang);
    fm_strlcpy(l->autonym, json_str(json_get(x, "autonym"), code), sizeof l->autonym);
    fm_strlcpy(l->title, json_str(json_get(x, "title"), ""), sizeof l->title);
  }
  json_free(&j);
  if (!out->n) fm_strlcpy(out->error, "This article is in no other language", sizeof out->error);
  return FM_OK;
}
