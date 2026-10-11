/* fdoc.c -- a small HTML document for the readers (see fdoc.h).
**
** Design decisions:
**   - No DOM: one pass over the tags with a little state (open block, style
**     counters, list and table stacks) builds blocks and cells directly.
**     Whatever is malformed degrades to text, never to an error.
**   - Text is one buffer; runs (offset, length, style, link) point into it,
**     and every block or cell owns a contiguous range of runs.
**   - Skipped as chrome, not content: scripts and styles, display:none, edit
**     links, reference markers, navigation boxes and message boxes (wikis),
**     Project Gutenberg's licence sections (books), icon-sized pictures.
**   - Tables: one level (a table inside a cell becomes the cell's text);
**     column widths from the text they hold, wrapped to fit.
**   - Code (pre, code, kbd...) is set in Hack, the monospace face. There is
**     no italic face: italic is drawn in a tinted colour.
*/
#include "fdoc.h"
#include "ffont.h"
#include "fgfx.h"
#include "ftheme.h"
#include <ctype.h>

/* ---- building ------------------------------------------------------------------ */

#define GROW(arr, n, cap, type)                                              \
  do {                                                                       \
    if ((n) >= (cap)) {                                                      \
      (cap) = (cap) ? (cap) * 2 : 64;                                        \
      (arr) = (type *)fm_realloc((arr), (size_t)(cap) * sizeof(type));      \
    }                                                                        \
  } while (0)

typedef struct Attrs {
  char cls[400], id[160], href[1024], src[1024], alt[400], style[240];
  int width, height, colspan;
} Attrs;

typedef struct Par {
  FmDoc *d;
  const char *base;
  int flavor;
  int bold, italic, code, small;
  int links[24];
  int nlinks;
  int pre, quote;
  struct { bool ordered; int n; } list[16];
  int nlist;
  int cur;                     /* open block, -1 = none */
  bool space;                  /* the last character put was a space (collapse) */
  int tdepth;                  /* table nesting */
  int tab, row, col, cell;     /* the outer table being built */
  bool caption;                /* text goes to the caption block */
  int divs;                    /* open <div>s */
  int chess_at, chess_cap_at;  /* div depth of a chess diagram and of its caption, 0 = none */
  /* a picture container (style position:relative): pictures in divs placed
  ** with top/left inside it are laid over its first picture */
  int ov_at, ov_img;           /* its div depth (0 = none), its first picture (-1 = none yet) */
  float ov_x[8], ov_y[8], ov_px[8], ov_py[8];   /* per div level inside it: left/top in px and in % */
  bool ov_pos[8];
} Par;

static bool has_class(const char *cls, const char *tok) {
  size_t n = strlen(tok);
  for (const char *p = cls; (p = strstr(p, tok)) != NULL; p += n) {
    bool start = p == cls || p[-1] == ' ';
    bool end = p[n] == 0 || p[n] == ' ';
    if (start && end) return true;
  }
  return false;
}

static bool tag_is(const char *t, const char *name) { return !fm_stricmp(t, name); }

static void abs_url(const char *base, const char *rel, char *out, size_t cap) {
  if (!rel[0]) { out[0] = 0; return; }
  if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) { fm_strlcpy(out, rel, cap); return; }
  if (!strncmp(rel, "//", 2)) { fm_snprintf(out, cap, "https:%s", rel); return; }
  const char *host = strstr(base, "://");
  if (rel[0] == '/') {
    size_t n = host ? (size_t)(strchr(host + 3, '/') ? strchr(host + 3, '/') - base : (ptrdiff_t)strlen(base)) : 0;
    fm_snprintf(out, cap, "%.*s%s", (int)n, base, rel);
    return;
  }
  const char *slash = strrchr(base, '/');
  size_t n = slash && (!host || slash > host + 2) ? (size_t)(slash - base + 1) : strlen(base);
  fm_snprintf(out, cap, "%.*s%s", (int)n, base, rel);
}

/* ---- entities ---- */

static int utf8_put(char *o, unsigned cp) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63));
    return 3;
  }
  if (cp > 0x10FFFF) cp = 0xFFFD;
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63));
  o[3] = (char)(0x80 | (cp & 63));
  return 4;
}

static const struct { const char *n; unsigned cp; } kEnt[] = {
  { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' }, { "ensp", ' ' },
  { "emsp", ' ' }, { "thinsp", ' ' }, { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 },
  { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "copy", 0xA9 },
  { "reg", 0xAE }, { "deg", 0xB0 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "middot", 0xB7 }, { "bull", 0x2022 },
  { "minus", 0x2212 }, { "plusmn", 0xB1 }, { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE }, { "sect", 0xA7 },
  { "para", 0xB6 }, { "dagger", 0x2020 }, { "pound", 0xA3 }, { "euro", 0x20AC }, { "cent", 0xA2 }, { "yen", 0xA5 },
  { "rarr", 0x2192 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "shy", 0 }, { "zwj", 0 },
  { "zwnj", 0 }, { "lrm", 0 }, { "rlm", 0 }, { "aelig", 0xE6 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
  { "aacute", 0xE1 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA }, { "ntilde", 0xF1 }, { "Ntilde", 0xD1 },
  { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF }, { "ccedil", 0xE7 }, { "ecirc", 0xEA },
};

/* Decodes entities of s[0..n) into out (never longer than the input). */
static size_t decode(const char *s, size_t n, char *out) {
  size_t o = 0;
  for (size_t i = 0; i < n;) {
    if (s[i] != '&') { out[o++] = s[i++]; continue; }
    size_t j = i + 1;
    while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != ' ') j++;
    if (j >= n || s[j] != ';') { out[o++] = s[i++]; continue; }
    unsigned cp = 0;
    bool ok = false;
    if (s[i + 1] == '#') {
      bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
      for (size_t k = i + (hex ? 3 : 2); k < j; k++) {
        int v = s[k] >= '0' && s[k] <= '9' ? s[k] - '0' : hex && s[k] >= 'a' && s[k] <= 'f' ? s[k] - 'a' + 10
              : hex && s[k] >= 'A' && s[k] <= 'F' ? s[k] - 'A' + 10 : -1;
        if (v < 0) { cp = 0xFFFD; break; }
        cp = cp * (hex ? 16 : 10) + (unsigned)v;
        if (cp > 0x10FFFF) cp = 0xFFFD;
      }
      ok = true;
    } else {
      for (int k = 0; k < FM_COUNT(kEnt); k++)
        if (strlen(kEnt[k].n) == j - i - 1 && !strncmp(s + i + 1, kEnt[k].n, j - i - 1)) { cp = kEnt[k].cp; ok = true; break; }
    }
    if (!ok) { out[o++] = s[i++]; continue; }
    if (cp == 0xA0) cp = ' ';
    if (cp) {
      char u[4];
      int un = utf8_put(u, cp);
      if ((size_t)un <= j + 1 - i) { memcpy(out + o, u, (size_t)un); o += (size_t)un; }   /* always shorter than "&...;" */
    }
    i = j + 1;
  }
  return o;
}

/* ---- the document's pieces ---- */

static void put_text(FmDoc *d, const char *s, size_t n) {
  if (d->tn + n + 1 > d->tcap) {
    d->tcap = (d->tn + n + 1) * 2;
    d->text = (char *)fm_realloc(d->text, d->tcap);
  }
  memcpy(d->text + d->tn, s, n);
  d->tn += n;
  d->text[d->tn] = 0;
}

static int target_run0(Par *p) {
  if (p->tdepth >= 1 && p->cell >= 0 && !p->caption) return p->d->cells[p->cell].run0;
  return p->cur >= 0 ? p->d->b[p->cur].run0 : p->d->nruns;
}

static u8 style(Par *p) {
  return (u8)((p->bold ? DS_BOLD : 0) | (p->italic ? DS_ITALIC : 0) | (p->code ? DS_CODE : 0) |
              (p->nlinks && p->links[p->nlinks - 1] >= 0 ? DS_LINK : 0) | (p->small ? DS_SMALL : 0) | (p->pre ? DS_PRE : 0));
}

static void add_run(Par *p, const char *s, size_t n) {
  if (!n) return;
  FmDoc *d = p->d;
  u8 st = style(p);
  int link = st & DS_LINK ? p->links[p->nlinks - 1] : -1;
  int r0 = target_run0(p);
  if (d->nruns > r0) {
    DocRun *last = &d->runs[d->nruns - 1];
    if (last->st == st && last->link == link && (size_t)(last->off + last->len) == d->tn) {
      put_text(d, s, n);
      last->len += (int)n;
      return;
    }
  }
  GROW(d->runs, d->nruns, d->cruns, DocRun);
  DocRun *r = &d->runs[d->nruns++];
  r->off = (int)d->tn;
  r->len = (int)n;
  r->st = st;
  r->link = link;
  put_text(d, s, n);
}

/* trailing spaces of the runs from r0 go */
static void trim_tail(FmDoc *d, int r0) {
  while (d->nruns > r0) {
    DocRun *r = &d->runs[d->nruns - 1];
    while (r->len > 0 && (d->text[r->off + r->len - 1] == ' ' || d->text[r->off + r->len - 1] == '\n')) r->len--;
    if (r->len > 0) break;
    d->nruns--;
  }
}

static void end_block(Par *p) {
  if (p->cur < 0) return;
  FmDoc *d = p->d;
  DocBlock *b = &d->b[p->cur];
  trim_tail(d, b->run0);       /* code too: its last line break ("}\n</code></pre>") would be an empty line */
  b->nrun = d->nruns - b->run0;
  if (!b->nrun && b->kind != DB_IMG && b->kind != DB_HR && b->kind != DB_TABLE) {
    d->nruns = b->run0;
    d->nb--;                                         /* empty: never mind */
  }
  p->cur = -1;
}

static int new_block(Par *p, int kind, int level) {
  FmDoc *d = p->d;
  GROW(d->b, d->nb, d->cb, DocBlock);
  DocBlock *b = &d->b[d->nb];
  memset(b, 0, sizeof *b);
  b->kind = (u8)kind;
  b->level = (u8)FM_CLAMP(level, 0, 255);
  b->run0 = d->nruns;
  b->img = b->tab = -1;
  return d->nb++;
}

static void open_block(Par *p, int kind, int level) {
  end_block(p);
  p->cur = new_block(p, kind, level);
  p->space = true;
}

static bool in_cell(Par *p) { return p->tdepth >= 1 && p->cell >= 0 && !p->caption; }

static void ensure_target(Par *p) {
  if (in_cell(p) || p->cur >= 0) return;
  if (p->tdepth >= 1 && !p->caption) return;          /* text between table cells */
  open_block(p, p->quote ? DB_QUOTE : DB_P, p->quote);
}

static void text(Par *p, const char *s, size_t n) {
  if (!n) return;
  if (p->chess_at && !p->chess_cap_at) return;       /* a chess diagram's a-h / 1-8 labels */
  if (p->tdepth >= 1 && p->cell < 0 && !p->caption) return;
  char *buf = (char *)fm_alloc(n + 1);
  size_t m = decode(s, n, buf);
  if (p->pre) {
    size_t o = 0;
    for (size_t i = 0; i < m; i++)
      if (buf[i] != '\r') buf[o++] = buf[i];
    if (o) {
      ensure_target(p);
      add_run(p, buf, o);
    }
    fm_free(buf);
    return;
  }
  /* collapse white space */
  size_t o = 0;
  for (size_t i = 0; i < m; i++) {
    char c = buf[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f') {
      if (!p->space) { buf[o++] = ' '; p->space = true; }
    } else {
      buf[o++] = c;
      p->space = false;
    }
  }
  if (o && !(o == 1 && buf[0] == ' ' && p->cur < 0 && !in_cell(p))) {
    /* a block opened here starts without a space, but the text just read
    ** decides whether one comes next ("<p><code>--&gt;</code> is": keep it) */
    bool had = in_cell(p) || p->cur >= 0, sp = p->space;
    ensure_target(p);
    p->space = sp;
    size_t s0 = !had && buf[0] == ' ' ? 1 : 0;
    if (o > s0) add_run(p, buf + s0, o - s0);
  }
  fm_free(buf);
}

static void newline(Par *p) {
  if (!in_cell(p) && p->cur < 0) return;
  add_run(p, "\n", 1);
  p->space = true;
}

static void cell_break(Par *p) {
  FmDoc *d = p->d;
  if (!in_cell(p)) return;
  DocCell *c = &d->cells[p->cell];
  if (d->nruns > c->run0 && d->text[d->tn - 1] != '\n') { trim_tail(d, c->run0); newline(p); }
}

static void close_cell(Par *p) {
  if (p->cell < 0) return;
  FmDoc *d = p->d;
  DocCell *c = &d->cells[p->cell];
  trim_tail(d, c->run0);
  c->nrun = d->nruns - c->run0;
  p->cell = -1;
}

static char *dup_str(const char *s) { return s && s[0] ? fm_strdup(s) : NULL; }

static int add_link(Par *p, const char *href) {
  FmDoc *d = p->d;
  GROW(d->links, d->nlinks, d->clinks, char *);
  d->links[d->nlinks] = fm_strdup(href);
  return d->nlinks++;
}

static void anchor(Par *p, const char *id) {
  if (!id[0]) return;
  FmDoc *d = p->d;
  GROW(d->anch, d->nanch, d->canch, DocAnchor);
  d->anch[d->nanch].id = fm_strdup(id);
  d->anch[d->nanch++].block = p->cur >= 0 ? p->cur : d->nb;
}

static void image(Par *p, const Attrs *a) {
  FmDoc *d = p->d;
  /* a formula's picture: its TeX source as code */
  if (has_class(a->cls, "mwe-math-fallback-image-inline") || has_class(a->cls, "mwe-math-fallback-image-display")) {
    char t[400];
    fm_strlcpy(t, a->alt, sizeof t);
    char *s = t;
    if (!strncmp(s, "{\\displaystyle ", 15)) {
      s += 15;
      size_t n = strlen(s);
      if (n && s[n - 1] == '}') s[n - 1] = 0;
    }
    p->code++;
    text(p, s, strlen(s));
    p->code--;
    return;
  }
  if (!a->src[0]) return;
  char src[1100];
  /* inside a picture container, in a positioned div: a layer of its first picture */
  if (p->ov_at && p->ov_img >= 0) {
    float x = 0, y = 0, px = 0, py = 0;
    bool pos = false;
    for (int k = 1; k <= FM_MIN(p->divs - p->ov_at, 7); k++) {
      if (!p->ov_pos[k]) continue;
      pos = true;
      x += p->ov_x[k];
      y += p->ov_y[k];
      px += p->ov_px[k];
      py += p->ov_py[k];
    }
    if (pos) {
      abs_url(p->base, a->src, src, sizeof src);
      GROW(d->imgs, d->nimgs, d->cimgs, DocImage);
      DocImage *base = &d->imgs[p->ov_img], *im = &d->imgs[d->nimgs++];
      memset(im, 0, sizeof *im);
      im->src = fm_strdup(src);
      im->alt = dup_str(a->alt);
      im->w = a->width;
      im->h = a->height;
      im->layer_of = p->ov_img;
      im->lx = x + px / 100.0f * (float)base->w;
      im->ly = y + py / 100.0f * (float)base->h;
      base->has_layers = true;
      return;
    }
  }
  if (a->width > 0 && a->width < 40 && a->height > 0 && a->height < 40) return;   /* icons, flags */
  abs_url(p->base, a->src, src, sizeof src);
  GROW(d->imgs, d->nimgs, d->cimgs, DocImage);
  DocImage *im = &d->imgs[d->nimgs];
  memset(im, 0, sizeof *im);
  im->src = fm_strdup(src);
  im->alt = dup_str(a->alt);
  im->w = a->width;
  im->h = a->height;
  im->layer_of = -1;
  int link = p->nlinks ? p->links[p->nlinks - 1] : -1;
  if (link >= 0) im->href = fm_strdup(d->links[link]);
  int idx = d->nimgs++;
  if (p->ov_at && p->ov_img < 0) p->ov_img = idx;
  if (p->tdepth >= 1) {
    if (in_cell(p) && d->cells[p->cell].img < 0) d->cells[p->cell].img = idx;
    return;
  }
  end_block(p);
  int b = new_block(p, DB_IMG, 0);
  d->b[b].img = idx;
  d->b[b].nrun = 0;
}

static bool skip_this(Par *p, const char *tag, const Attrs *a) {
  static const char *const kTags[] = { "script", "style", "head", "noscript", "template", "svg", "math", "button", "form",
                                       "select", "iframe", "video", "audio", "object", "canvas", "map", "textarea" };
  for (int i = 0; i < FM_COUNT(kTags); i++)
    if (tag_is(tag, kTags[i])) return true;
  if (strstr(a->style, "display:none") || strstr(a->style, "display: none")) return true;
  if (p->flavor == DOC_WIKI) {
    static const char *const kCls[] = { "navbox", "vertical-navbox", "metadata", "ambox", "mbox-small", "mw-editsection",
                                        "noprint", "mw-jump-link", "toc", "mw-empty-elt", "sistersitebox", "authority-control",
                                        "catlinks", "printfooter", "shortdescription", "navigation-not-searchable",
                                        "mw-cite-backlink", "sidebar", "navbar", "mw-indicators", "ext-phonos" };
    for (int i = 0; i < FM_COUNT(kCls); i++)
      if (has_class(a->cls, kCls[i])) return true;
    if (tag_is(tag, "sup") && has_class(a->cls, "reference")) return true;
    if (!strcmp(a->id, "toc") || !strcmp(a->id, "coordinates")) return true;
  } else if (p->flavor == DOC_WEB) {
    /* dev.to: the copy / full screen buttons under each code block */
    if (has_class(a->cls, "highlight__panel")) return true;
  } else {
    if (has_class(a->cls, "pg-boilerplate") || !strcmp(a->id, "pg-header") || !strcmp(a->id, "pg-footer")) return true;
    /* the printed book's page numbers, set in the margin by CSS */
    if (has_class(a->cls, "pagenum") || has_class(a->cls, "pageno") || has_class(a->cls, "page-num")) return true;
  }
  return false;
}

static void finish_table(Par *p) {
  FmDoc *d = p->d;
  close_cell(p);
  DocTable *t = &d->tabs[p->tab];
  t->ncell = d->ncells - t->cell0;
  int cols = 0, rows = 0;
  for (int i = t->cell0; i < d->ncells; i++) {
    cols = FM_MAX(cols, d->cells[i].col + d->cells[i].span);
    rows = FM_MAX(rows, d->cells[i].row + 1);
  }
  t->cols = cols;
  t->rows = rows;
  if (!t->ncell) return;
  int b = new_block(p, DB_TABLE, 0);
  d->b[b].tab = p->tab;
}

/* Reads the attributes after a tag name up to '>'. */
static const char *attrs(const char *s, const char *end, Attrs *a, bool *self_close) {
  memset(a, 0, sizeof *a);
  a->colspan = 1;
  *self_close = false;
  while (s < end) {
    while (s < end && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
    if (s >= end) break;
    if (*s == '>') return s + 1;
    if (*s == '/') { *self_close = true; s++; continue; }
    const char *n0 = s;
    while (s < end && *s != '=' && *s != '>' && *s != ' ' && *s != '\n' && *s != '\t' && *s != '/') s++;
    size_t nl = (size_t)(s - n0);
    const char *v0 = s, *v1 = s;
    if (s < end && *s == '=') {
      s++;
      if (s < end && (*s == '"' || *s == '\'')) {
        char q = *s++;
        v0 = s;
        while (s < end && *s != q) s++;
        v1 = s;
        if (s < end) s++;
      } else {
        v0 = s;
        while (s < end && *s != ' ' && *s != '>' && *s != '\n' && *s != '\t') s++;
        v1 = s;
      }
    }
    char *dst = NULL;
    size_t cap = 0;
#define ATTR(name, field) if (nl == sizeof(name) - 1 && !fm_strnicmp(n0, name, nl)) { dst = a->field; cap = sizeof a->field; }
    ATTR("class", cls) else ATTR("id", id) else ATTR("href", href) else ATTR("src", src) else ATTR("alt", alt)
    else ATTR("style", style)
#undef ATTR
    char num[16];
    if (!dst && ((nl == 5 && !fm_strnicmp(n0, "width", 5)) || (nl == 6 && !fm_strnicmp(n0, "height", 6)) ||
                 (nl == 7 && !fm_strnicmp(n0, "colspan", 7)))) {
      size_t k = FM_MIN((size_t)(v1 - v0), sizeof num - 1);
      memcpy(num, v0, k);
      num[k] = 0;
      int v = atoi(num);
      if (nl == 5) a->width = v;
      else if (nl == 6) a->height = v;
      else a->colspan = FM_CLAMP(v, 1, 20);
    }
    if (dst) {
      size_t k = FM_MIN((size_t)(v1 - v0), cap - 1);
      k = decode(v0, k, dst);
      dst[k] = 0;
    }
  }
  return end;
}

static bool is_void(const char *t) {
  static const char *const kVoid[] = { "br", "img", "hr", "meta", "link", "input", "wbr", "source", "area", "col", "base",
                                       "param", "track", "embed" };
  for (int i = 0; i < FM_COUNT(kVoid); i++)
    if (tag_is(t, kVoid[i])) return true;
  return false;
}

static bool is_para(const char *t) {
  static const char *const kP[] = { "p", "div", "section", "article", "header", "footer", "main", "center", "figure", "aside",
                                    "address", "details", "summary", "fieldset", "nav", "body", "html", "span_block" };
  for (int i = 0; i < FM_COUNT(kP); i++)
    if (tag_is(t, kP[i])) return true;
  return false;
}

static int heading(const char *t) {
  return (t[0] == 'h' || t[0] == 'H') && t[1] >= '1' && t[1] <= '6' && !t[2] ? t[1] - '0' : 0;
}

/* "left:" / "top:" of an inline style, in px or %; false = not set. */
static bool css_pos(const char *style, const char *prop, float *px, float *pct) {
  size_t n = strlen(prop);
  for (const char *s = style; (s = strstr(s, prop)) != NULL; s += n) {
    if (s > style && s[-1] != ';' && s[-1] != ' ') continue;     /* not "margin-left:" */
    const char *v = s + n;
    while (*v == ' ') v++;
    char *e;
    double x = strtod(v, &e);
    if (e == v) return false;
    while (*e == ' ') e++;
    if (*e == '%') *pct = (float)x;
    else *px = (float)x;
    return true;
  }
  return false;
}

/* <div> bookkeeping: chess diagrams and picture containers (layers). */
static void div_open(Par *p, const Attrs *a) {
  p->divs++;
  if (!p->chess_at && has_class(a->cls, "chessboard")) p->chess_at = p->divs;
  else if (p->chess_at && !p->chess_cap_at && has_class(a->cls, "thumbcaption")) p->chess_cap_at = p->divs;
  if (!p->ov_at) {
    if (strstr(a->style, "position:relative") || strstr(a->style, "position: relative")) {
      p->ov_at = p->divs;
      p->ov_img = -1;
    }
    return;
  }
  int k = p->divs - p->ov_at;
  if (k < 1 || k > 7) return;
  p->ov_x[k] = p->ov_y[k] = p->ov_px[k] = p->ov_py[k] = 0;
  bool l = css_pos(a->style, "left:", &p->ov_x[k], &p->ov_px[k]);
  bool t = css_pos(a->style, "top:", &p->ov_y[k], &p->ov_py[k]);
  p->ov_pos[k] = l || t;
}

static void div_close(Par *p) {
  if (!p->divs) return;
  if (p->divs == p->chess_cap_at) p->chess_cap_at = 0;
  if (p->divs == p->chess_at) p->chess_at = p->chess_cap_at = 0;
  if (p->divs == p->ov_at) p->ov_at = 0;
  p->divs--;
}

/* Inside a chess diagram the board's frame is a table of labels: left out. */
static bool chess_table_tag(Par *p, const char *t) {
  return p->chess_at && !p->chess_cap_at &&
         (tag_is(t, "table") || tag_is(t, "tr") || tag_is(t, "td") || tag_is(t, "th") || tag_is(t, "tbody") ||
          tag_is(t, "caption"));
}

static void start_tag(Par *p, const char *t, const Attrs *a) {
  FmDoc *d = p->d;
  if (a->id[0]) anchor(p, a->id);
  int h = heading(t);
  if (tag_is(t, "div")) div_open(p, a);
  if (h) p->chess_at = p->chess_cap_at = p->ov_at = 0;   /* never inside those: unbalanced HTML */
  if (chess_table_tag(p, t)) return;
  if (tag_is(t, "br")) { newline(p); return; }
  if (tag_is(t, "img")) { image(p, a); return; }
  if (tag_is(t, "hr")) {
    if (p->tdepth) return;
    end_block(p);
    new_block(p, DB_HR, 0);
    return;
  }
  if (h) {
    if (in_cell(p)) { cell_break(p); p->bold++; return; }
    if (p->tdepth) return;
    open_block(p, DB_H, h);
    return;
  }
  if (tag_is(t, "ul") || tag_is(t, "ol") || tag_is(t, "dl")) {
    if (p->nlist < FM_COUNT(p->list)) {
      p->list[p->nlist].ordered = tag_is(t, "ol");
      p->list[p->nlist].n = 0;
    }
    p->nlist++;
    if (in_cell(p)) cell_break(p);
    else if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "li")) {
    int lv = FM_CLAMP(p->nlist, 1, FM_COUNT(p->list));
    char mk[12] = "\xE2\x80\xA2";
    if (p->nlist >= 1 && p->nlist <= FM_COUNT(p->list) && p->list[p->nlist - 1].ordered)
      fm_snprintf(mk, sizeof mk, "%d.", ++p->list[p->nlist - 1].n);
    if (in_cell(p)) {
      cell_break(p);
      add_run(p, mk, strlen(mk));
      add_run(p, " ", 1);
      p->space = true;
      return;
    }
    if (p->tdepth) return;
    open_block(p, DB_LI, lv);
    fm_strlcpy(d->b[p->cur].marker, mk, sizeof d->b[0].marker);
    return;
  }
  if (tag_is(t, "dt") || tag_is(t, "dd")) {
    if (in_cell(p)) { cell_break(p); return; }
    if (p->tdepth) return;
    open_block(p, tag_is(t, "dt") ? DB_DT : DB_DD, FM_MAX(1, p->nlist));
    return;
  }
  if (tag_is(t, "pre")) {
    p->pre++;
    if (in_cell(p)) { cell_break(p); return; }
    if (p->tdepth) return;
    open_block(p, DB_PRE, 0);
    return;
  }
  if (tag_is(t, "blockquote")) {
    if (p->tdepth) return;
    end_block(p);
    p->quote++;
    return;
  }
  if (tag_is(t, "table")) {
    p->tdepth++;
    if (p->tdepth > 1) { cell_break(p); return; }
    end_block(p);
    GROW(d->tabs, d->ntabs, d->ctabs, DocTable);
    DocTable *tb = &d->tabs[d->ntabs];
    memset(tb, 0, sizeof *tb);
    tb->cell0 = d->ncells;
    tb->box = has_class(a->cls, "infobox") || has_class(a->cls, "infobox_v2") || has_class(a->cls, "infobox_v3");
    p->tab = d->ntabs++;
    p->row = -1;
    p->col = 0;
    p->cell = -1;
    return;
  }
  /* a table inside a cell: its rows as lines, its cells spaced apart */
  if (p->tdepth > 1 && in_cell(p)) {
    if (tag_is(t, "tr")) { cell_break(p); return; }
    if (tag_is(t, "td") || tag_is(t, "th")) {
      FmDoc *dd = p->d;
      if (dd->nruns > dd->cells[p->cell].run0 && dd->tn && dd->text[dd->tn - 1] != '\n') {
        add_run(p, "   ", 3);
        p->space = true;
      }
      return;
    }
  }
  if (p->tdepth == 1 && tag_is(t, "tr")) {
    close_cell(p);
    p->row++;
    p->col = 0;
    return;
  }
  if (p->tdepth == 1 && (tag_is(t, "td") || tag_is(t, "th"))) {
    close_cell(p);
    if (p->row < 0) p->row = 0;
    GROW(d->cells, d->ncells, d->ccells, DocCell);
    DocCell *c = &d->cells[d->ncells];
    memset(c, 0, sizeof *c);
    c->row = p->row;
    c->col = p->col;
    c->span = a->colspan;
    c->head = tag_is(t, "th");
    c->run0 = d->nruns;
    c->img = -1;
    p->col += c->span;
    p->cell = d->ncells++;
    p->space = true;
    return;
  }
  if (p->tdepth == 1 && tag_is(t, "caption")) {
    close_cell(p);
    p->caption = true;
    open_block(p, DB_CAPTION, 0);
    return;
  }
  if (tag_is(t, "figcaption") || (p->flavor == DOC_WIKI && has_class(a->cls, "thumbcaption"))) {
    if (p->tdepth) { cell_break(p); return; }
    open_block(p, DB_CAPTION, 0);
    return;
  }
  if (tag_is(t, "a")) {
    int link = -1;
    if (a->href[0] && strncmp(a->href, "#cite", 5) && strncmp(a->href, "javascript:", 11)) link = add_link(p, a->href);
    if (p->nlinks < FM_COUNT(p->links)) p->links[p->nlinks] = link;
    p->nlinks++;
    return;
  }
  if (tag_is(t, "b") || tag_is(t, "strong")) { p->bold++; return; }
  if (tag_is(t, "i") || tag_is(t, "em") || tag_is(t, "cite") || tag_is(t, "dfn") || tag_is(t, "var")) { p->italic++; return; }
  if (tag_is(t, "code") || tag_is(t, "tt") || tag_is(t, "kbd") || tag_is(t, "samp")) { p->code++; return; }
  if (tag_is(t, "small") || tag_is(t, "sub") || tag_is(t, "sup")) { p->small++; return; }
  if (is_para(t)) {
    if (in_cell(p)) { cell_break(p); return; }
    if (p->tdepth) return;
    /* a paragraph inside a list item stays in the item */
    if (p->cur >= 0 && (d->b[p->cur].kind == DB_LI || d->b[p->cur].kind == DB_DD || d->b[p->cur].kind == DB_DT)) {
      if (d->nruns > d->b[p->cur].run0) newline(p);
      return;
    }
    end_block(p);
  }
}

static void end_tag(Par *p, const char *t) {
  FmDoc *d = p->d;
  int h = heading(t);
  if (chess_table_tag(p, t)) return;
  if (tag_is(t, "div")) div_close(p);
  if (h) {
    if (in_cell(p)) { if (p->bold) p->bold--; cell_break(p); return; }
    if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "ul") || tag_is(t, "ol") || tag_is(t, "dl")) {
    if (p->nlist) p->nlist--;
    if (in_cell(p)) cell_break(p);
    else if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "li") || tag_is(t, "dt") || tag_is(t, "dd")) {
    if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "pre")) {
    if (p->pre) p->pre--;
    if (in_cell(p)) { cell_break(p); return; }
    if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "blockquote")) {
    if (p->tdepth) return;
    end_block(p);
    if (p->quote) p->quote--;
    return;
  }
  if (tag_is(t, "table")) {
    if (!p->tdepth) return;
    if (p->tdepth == 1) finish_table(p);
    else cell_break(p);
    p->tdepth--;
    return;
  }
  if (p->tdepth == 1 && (tag_is(t, "td") || tag_is(t, "th"))) { close_cell(p); return; }
  if (p->tdepth == 1 && tag_is(t, "caption")) {
    end_block(p);
    p->caption = false;
    return;
  }
  if (tag_is(t, "figcaption")) {
    if (!p->tdepth) end_block(p);
    return;
  }
  if (tag_is(t, "a")) { if (p->nlinks) p->nlinks--; return; }
  if (tag_is(t, "b") || tag_is(t, "strong")) { if (p->bold) p->bold--; return; }
  if (tag_is(t, "i") || tag_is(t, "em") || tag_is(t, "cite") || tag_is(t, "dfn") || tag_is(t, "var")) {
    if (p->italic) p->italic--;
    return;
  }
  if (tag_is(t, "code") || tag_is(t, "tt") || tag_is(t, "kbd") || tag_is(t, "samp")) { if (p->code) p->code--; return; }
  if (tag_is(t, "small") || tag_is(t, "sub") || tag_is(t, "sup")) { if (p->small) p->small--; return; }
  if (is_para(t)) {
    if (in_cell(p)) { cell_break(p); return; }
    if (p->tdepth) return;
    if (p->cur >= 0 && (d->b[p->cur].kind == DB_P || d->b[p->cur].kind == DB_QUOTE || d->b[p->cur].kind == DB_CAPTION))
      end_block(p);
  }
}

static const char *find_ci(const char *s, const char *end, const char *needle) {
  size_t n = strlen(needle);
  for (; s + n <= end; s++)
    if (!fm_strnicmp(s, needle, n)) return s;
  return NULL;
}

void doc_parse(const char *html, size_t len, const char *base, int flavor, FmDoc *d) {
  memset(d, 0, sizeof *d);
  Par p;
  memset(&p, 0, sizeof p);
  p.d = d;
  p.base = base ? base : "";
  p.flavor = flavor;
  p.cur = p.cell = -1;
  p.space = true;
  const char *s = html, *end = html + len;
  /* only the body, when there is one */
  const char *body = find_ci(s, end, "<body");
  if (body) s = body;
  int skip = 0;
  char skip_tag[16] = "";
  Attrs a;
  while (s < end) {
    const char *lt = memchr(s, '<', (size_t)(end - s));
    if (!lt) lt = end;
    if (!skip && lt > s) text(&p, s, (size_t)(lt - s));
    if (lt >= end) break;
    s = lt + 1;
    if (s < end && *s == '!') {                       /* comment, doctype, CDATA */
      const char *e2 = !strncmp(s, "!--", 3) ? find_ci(s + 3, end, "-->") : memchr(s, '>', (size_t)(end - s));
      s = e2 ? e2 + (e2[0] == '-' ? 3 : 1) : end;
      continue;
    }
    bool close = s < end && *s == '/';
    if (close) s++;
    char tag[16];
    size_t tn = 0;
    while (s < end && tn + 1 < sizeof tag && (isalnum((u8)*s) || *s == '-' || *s == ':')) tag[tn++] = (char)tolower((u8)*s++);
    tag[tn] = 0;
    if (!tn) {                                         /* a lone '<' in text */
      if (!skip) text(&p, "<", 1);
      continue;
    }
    bool self_close;
    s = attrs(s, end, &a, &self_close);
    if (skip) {
      if (!strcmp(tag, skip_tag) && !is_void(tag)) {
        if (close) skip--;
        else if (!self_close) skip++;
      }
      continue;
    }
    if (close) { end_tag(&p, tag); continue; }
    if (skip_this(&p, tag, &a)) {
      if (is_void(tag) || self_close) continue;
      if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "textarea")) {   /* raw text */
        char closer[24];
        fm_snprintf(closer, sizeof closer, "</%s", tag);
        const char *e2 = find_ci(s, end, closer);
        s = e2 ? e2 : end;
        continue;
      }
      skip = 1;
      fm_strlcpy(skip_tag, tag, sizeof skip_tag);
      continue;
    }
    start_tag(&p, tag, &a);
    if (self_close && !is_void(tag)) end_tag(&p, tag);
  }
  if (p.tdepth) { p.tdepth = 1; finish_table(&p); }
  end_block(&p);
}

void doc_free(FmDoc *d) {
  if (!d) return;
  for (int i = 0; i < d->nimgs; i++) { fm_free(d->imgs[i].src); fm_free(d->imgs[i].href); fm_free(d->imgs[i].alt); }
  for (int i = 0; i < d->nlinks; i++) fm_free(d->links[i]);
  for (int i = 0; i < d->nanch; i++) fm_free(d->anch[i].id);
  fm_free(d->text);
  fm_free(d->runs);
  fm_free(d->b);
  fm_free(d->cells);
  fm_free(d->tabs);
  fm_free(d->imgs);
  fm_free(d->links);
  fm_free(d->anch);
  fm_free(d->frag);
  fm_free(d->hl);
  memset(d, 0, sizeof *d);
}

static char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

int doc_find(FmDoc *d, const char *q) {
  fm_free(d->hl);
  d->hl = NULL;
  d->nhl = 0;
  d->hl_cur = -1;
  size_t n = q ? strlen(q) : 0;
  d->hl_len = (int)n;
  if (!n || !d->text) return 0;
  int cap = 0;
  for (size_t i = 0; i + n <= d->tn; i++) {
    if (fold(d->text[i]) != fold(q[0])) continue;
    size_t k = 1;
    while (k < n && fold(d->text[i + k]) == fold(q[k])) k++;
    if (k < n) continue;
    if (d->nhl == cap) {
      cap = cap ? cap * 2 : 64;
      d->hl = (int *)fm_realloc(d->hl, (size_t)cap * sizeof *d->hl);
    }
    d->hl[d->nhl++] = (int)i;
    i += n - 1;
  }
  return d->nhl;
}

/* the fragment holding a text offset, in blocks and in table cells */
float doc_offset_y(const FmDoc *d, int off) {
  for (int i = 0; i < d->nb; i++) {
    const DocBlock *b = &d->b[i];
    if (b->kind == DB_TABLE) {
      const DocTable *t = &d->tabs[b->tab];
      for (int c = t->cell0; c < t->cell0 + t->ncell; c++) {
        const DocCell *cl = &d->cells[c];
        for (int f = cl->f0; f < cl->f0 + cl->nf; f++)
          if (off >= d->frag[f].off && off < d->frag[f].off + d->frag[f].len) return b->y + cl->y + d->frag[f].y;
      }
      continue;
    }
    for (int f = b->f0; f < b->f0 + b->nf; f++)
      if (off >= d->frag[f].off && off < d->frag[f].off + d->frag[f].len) return b->y + d->frag[f].y;
  }
  return 0;
}

void doc_image_shape(FmDoc *d, DocImage *im, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (im->w > 0 && im->h > 0) return;
  if (im->w <= 0) im->w = w;
  im->h = (int)((float)im->w * (float)h / (float)w + 0.5f);
  d->lay_w = -1;                                     /* lay out again with the real shape */
}

void doc_block_text(const FmDoc *d, int block, char *out, size_t cap) {
  out[0] = 0;
  if (block < 0 || block >= d->nb || !cap) return;
  size_t o = 0;
  const DocBlock *b = &d->b[block];
  for (int r = b->run0; r < b->run0 + b->nrun && o + 1 < cap; r++) {
    size_t n = FM_MIN((size_t)d->runs[r].len, cap - 1 - o);
    memcpy(out + o, d->text + d->runs[r].off, n);
    o += n;
  }
  out[o] = 0;
  for (char *c = out; *c; c++)
    if (*c == '\n') *c = ' ';
}

int doc_anchor(const FmDoc *d, const char *id) {
  for (int i = 0; i < d->nanch; i++)
    if (!strcmp(d->anch[i].id, id)) return FM_MIN(d->anch[i].block, d->nb - 1);
  return -1;
}

static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

void doc_url_decode(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 1 < cap; s++) {
    int hi, lo;
    if (*s == '%' && s[1] && s[2] && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
      out[o++] = (char)(hi * 16 + lo);
      s += 2;
    } else {
      out[o++] = *s == '_' ? ' ' : *s;
    }
  }
  out[o] = 0;
}

/* ---- layout ------------------------------------------------------------------------- */

typedef struct Lay {
  FmDoc *d;
  float fs;                    /* body text size */
} Lay;

static int run_face(const DocRun *r, bool bold) { return bold || (r->st & DS_BOLD) ? FONT_BOLD : FONT_REGULAR; }
static float run_size(const DocRun *r, float fs) { return r->st & DS_SMALL ? fs * 0.8f : fs; }
static bool run_mono(const DocRun *r, bool pre) { return pre || (r->st & (DS_CODE | DS_PRE)); }

static int mono_face(bool bold) { return bold ? FONT_MONO_BOLD : FONT_MONO; }
static float mono_cell(float size) { return font_width(FONT_MONO, size * 0.92f, " ", 1); }

static int cp_bytes(u8 c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static float measure(const char *s, int n, int face, float size, bool mono) {
  if (!mono) return font_width(face, size, s, n);
  return font_width(mono_face(face == FONT_BOLD), size * 0.92f, s, n);
}

static void add_frag(FmDoc *d, float x, float y, float w, int run, int off, int len) {
  GROW(d->frag, d->nfrag, d->cfrag, DocFrag);
  DocFrag *f = &d->frag[d->nfrag++];
  f->x = x;
  f->y = y;
  f->w = w;
  f->run = run;
  f->off = off;
  f->len = len;
}

/* Lays runs out in maxw; fragments from d->nfrag on; returns the height. */
static float lay_inline(FmDoc *d, int run0, int nrun, float maxw, float fs, bool bold, bool pre) {
  float lh = font_line_h(pre ? fs * 0.92f : fs);
  float x = 0, y = 0;
  bool pending_space = false, any = false;
  maxw = FM_MAX(maxw, fs * 2);
  for (int r = run0; r < run0 + nrun; r++) {
    const DocRun *rn = &d->runs[r];
    int face = run_face(rn, bold);
    float size = run_size(rn, fs);
    bool mono = run_mono(rn, pre);
    bool rp = pre || (rn->st & DS_PRE);
    const char *t = d->text;
    int i = rn->off, e = rn->off + rn->len;
    float space_w = mono ? mono_cell(size) : font_width(face, size, " ", 1);
    while (i < e) {
      char c = t[i];
      if (c == '\n') { x = 0; y += lh; pending_space = false; i++; continue; }
      if (!rp && c == ' ') { pending_space = true; i++; continue; }
      /* a word (pre: up to the line end, spaces kept) */
      int j = i;
      if (rp) {
        while (j < e && t[j] != '\n') j++;
      } else {
        while (j < e && t[j] != ' ' && t[j] != '\n') j++;
      }
      int wl = j - i;
      float w = measure(t + i, wl, face, size, mono);
      float sp = pending_space && x > 0 ? space_w : 0;
      if (x + sp + w > maxw && x > 0) { x = 0; y += lh; sp = 0; }
      x += sp;
      pending_space = false;
      if (w <= maxw - x) {
        add_frag(d, x, y, w, r, i, wl);
        x += w;
        i = j;
        any = true;
        continue;
      }
      /* longer than the line: cut where it fits */
      while (i < j) {
        int k = i, fit = i;
        float fw = 0;
        while (k < j) {
          int cl = cp_bytes((u8)t[k]);
          float nw = measure(t + i, k + cl - i, face, size, mono);
          if (nw > maxw - x && fit > i) break;
          fit = k + cl;
          fw = nw;
          k += cl;
        }
        add_frag(d, x, y, fw, r, i, fit - i);
        any = true;
        i = fit;
        if (i < j) { x = 0; y += lh; }
        else x += fw;
      }
    }
  }
  return any ? y + lh : 0;
}

static float block_size(const DocBlock *b, float fs) {
  if (b->kind == DB_H) {
    static const float kH[] = { 1.0f, 1.55f, 1.35f, 1.18f, 1.08f, 1.0f, 1.0f };
    return fs * kH[FM_CLAMP(b->level, 1, 6)];
  }
  if (b->kind == DB_CAPTION) return fs * 0.86f;
  if (b->kind == DB_PRE) return fs * 0.92f;
  return fs;
}

static float block_indent(const DocBlock *b) {
  switch (b->kind) {
    case DB_LI: return DP(22) * b->level;
    case DB_DD: return DP(22) * FM_MAX(1, b->level);
    case DB_QUOTE: return DP(18) * FM_MAX(1, b->level);
    case DB_PRE: return DP(12);
    default: return 0;
  }
}

static void image_size(const DocImage *im, float maxw, float *w, float *h) {
  float nw = im->w > 0 ? DP(im->w) : maxw * 0.8f;
  float nh = im->h > 0 ? DP(im->h) : 0;
  if (nh <= 0) nh = im->w > 0 && im->h <= 0 ? nw * 0.7f : nw * 0.66f;
  if (nw > maxw) { nh *= maxw / nw; nw = maxw; }
  if (nh > DP(520)) { nw *= DP(520) / nh; nh = DP(520); }
  *w = nw;
  *h = nh;
}

static float cell_text_w(FmDoc *d, const DocCell *c, float fs, float *word_max) {
  float total = 0, line = 0;
  *word_max = 0;
  for (int r = c->run0; r < c->run0 + c->nrun; r++) {
    const DocRun *rn = &d->runs[r];
    int face = run_face(rn, c->head);
    float size = run_size(rn, fs);
    bool mono = run_mono(rn, false), rp = rn->st & DS_PRE;
    const char *t = d->text;
    int i = rn->off, e = rn->off + rn->len;
    while (i < e) {
      int j = i;
      while (j < e && (rp || t[j] != ' ') && t[j] != '\n') j++;
      float w = measure(t + i, j - i, face, size, mono);
      *word_max = FM_MAX(*word_max, w);
      line += w + (j < e && t[j] == ' ' ? font_width(face, size, " ", 1) : 0);
      if (j < e && t[j] == '\n') { total = FM_MAX(total, line); line = 0; }
      i = j + 1;
    }
  }
  return FM_MAX(total, line);
}

static float lay_table(FmDoc *d, DocTable *t, float maxw, float fs) {
  int cols = FM_MAX(1, t->cols);
  float *want = (float *)fm_calloc((size_t)cols, sizeof(float));
  float *minw = (float *)fm_calloc((size_t)cols, sizeof(float));
  float *colw = (float *)fm_calloc((size_t)cols, sizeof(float));
  float pad = DP(6), cfs = fs * 0.92f;
  for (int i = t->cell0; i < t->cell0 + t->ncell; i++) {
    DocCell *c = &d->cells[i];
    if (c->span != 1) continue;
    float wm;
    float tw = cell_text_w(d, c, cfs, &wm);
    if (c->img >= 0) {
      float iw, ih;
      image_size(&d->imgs[c->img], maxw, &iw, &ih);
      tw = FM_MAX(tw, iw);
    }
    want[c->col] = FM_MAX(want[c->col], FM_MIN(tw, maxw * 0.7f) + pad * 2);
    minw[c->col] = FM_MAX(minw[c->col], FM_MIN(wm, DP(110)) + pad * 2);
  }
  float sw = 0, sm = 0;
  for (int k = 0; k < cols; k++) {
    if (want[k] <= 0) want[k] = minw[k] = DP(40);
    sw += want[k];
    sm += minw[k];
  }
  for (int k = 0; k < cols; k++) {
    if (sw <= maxw) colw[k] = want[k];
    else if (sm < maxw && sw > sm) colw[k] = minw[k] + (maxw - sm) * (want[k] - minw[k]) / (sw - sm);
    else colw[k] = maxw / cols;
  }
  float *rowh = (float *)fm_calloc((size_t)FM_MAX(1, t->rows), sizeof(float));
  for (int i = t->cell0; i < t->cell0 + t->ncell; i++) {
    DocCell *c = &d->cells[i];
    float w = 0;
    for (int k = c->col; k < FM_MIN(cols, c->col + c->span); k++) w += colw[k];
    c->w = w;
    float h = pad;
    if (c->img >= 0) {
      float iw, ih;
      image_size(&d->imgs[c->img], w - pad * 2, &iw, &ih);
      h += ih + DP(4);
    }
    c->f0 = d->nfrag;
    float th = lay_inline(d, c->run0, c->nrun, w - pad * 2, cfs, c->head, false);
    c->nf = d->nfrag - c->f0;
    for (int f = c->f0; f < d->nfrag; f++) {
      d->frag[f].x += pad;
      d->frag[f].y += h;
    }
    h += th + pad;
    c->h = h;
    if (c->row < t->rows) rowh[c->row] = FM_MAX(rowh[c->row], h);
  }
  float y = 0;
  float *rowy = (float *)fm_calloc((size_t)FM_MAX(1, t->rows), sizeof(float));
  for (int r = 0; r < t->rows; r++) { rowy[r] = y; y += rowh[r]; }
  for (int i = t->cell0; i < t->cell0 + t->ncell; i++) {
    DocCell *c = &d->cells[i];
    float x = 0;
    for (int k = 0; k < FM_MIN(cols, c->col); k++) x += colw[k];
    c->x = x;
    c->y = c->row < t->rows ? rowy[c->row] : 0;
    c->h = c->row < t->rows ? rowh[c->row] : c->h;
  }
  fm_free(want);
  fm_free(minw);
  fm_free(colw);
  fm_free(rowh);
  fm_free(rowy);
  return y;
}

float doc_layout(FmDoc *d, float w, float fs) {
  if (d->lay_w == w && d->lay_fs == fs && d->lay_gen == font_generation() && d->frag) return d->total;
  d->lay_gen = font_generation();
  d->nfrag = 0;
  d->lay_w = w;
  d->lay_fs = fs;
  float y = 0;
  for (int i = 0; i < d->nb; i++) {
    DocBlock *b = &d->b[i];
    b->y = y;
    b->f0 = d->nfrag;
    float ind = block_indent(b), size = block_size(b, fs);
    float h = 0;
    switch (b->kind) {
      case DB_H: {
        float top = b->level <= 2 ? DP(22) : DP(14);
        h = top + lay_inline(d, b->run0, b->nrun, w, size, true, false) + (b->level <= 2 ? DP(12) : DP(6));
        for (int f = b->f0; f < d->nfrag; f++) d->frag[f].y += top;
        break;
      }
      case DB_IMG: {
        float iw, ih;
        image_size(&d->imgs[b->img], w, &iw, &ih);
        h = ih + DP(12);
        break;
      }
      case DB_HR: h = DP(24); break;
      case DB_TABLE: h = lay_table(d, &d->tabs[b->tab], w, fs) + DP(14); break;
      case DB_PRE: {
        float pad = DP(10);
        h = lay_inline(d, b->run0, b->nrun, w - ind * 2 - pad * 2, size, false, true);
        for (int f = b->f0; f < d->nfrag; f++) { d->frag[f].x += ind + pad; d->frag[f].y += pad; }
        h += pad * 2 + DP(12);
        break;
      }
      default: {
        bool bold = b->kind == DB_DT;
        h = lay_inline(d, b->run0, b->nrun, w - ind, size, bold, false);
        for (int f = b->f0; f < d->nfrag; f++) d->frag[f].x += ind;
        h += b->kind == DB_LI || b->kind == DB_DT || b->kind == DB_DD ? DP(5) : DP(11);
        break;
      }
    }
    b->nf = d->nfrag - b->f0;
    b->h = h;
    y += h;
  }
  d->total = y;
  return y;
}

/* ---- drawing ------------------------------------------------------------------------ */

static FmColor run_color(const DocRun *r, FmColor base) {
  if (r->st & DS_LINK) return T.accent;
  if (r->st & DS_ITALIC) return col_mix(base, T.accent, 0.3f);
  return base;
}

static void draw_mono(const char *s, int n, float x, float y, float size, bool bold, FmColor c) {
  font_draw(mono_face(bold), size * 0.92f, x, y, s, n, c);
}

static void draw_frags(FmDoc *d, int f0, int nf, float ox, float oy, float fs, bool bold, bool pre, FmRect clip,
                       u32 id, DocHit *hit, FmColor base) {
  for (int k = f0; k < f0 + nf; k++) {
    DocFrag *f = &d->frag[k];
    const DocRun *r = &d->runs[f->run];
    float size = run_size(r, fs);
    float lh = font_line_h(pre ? fs * 0.92f : fs);
    float x = ox + f->x, y = oy + f->y;
    if (y > clip.y + clip.h || y + lh < clip.y) continue;
    FmColor c = run_color(r, base);
    bool mono = run_mono(r, pre);
    if (d->nhl) {                                    /* find in page */
      int lo = 0, hi = d->nhl;
      while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->hl[mid] + d->hl_len <= f->off) lo = mid + 1;
        else hi = mid;
      }
      for (int m = lo; m < d->nhl && d->hl[m] < f->off + f->len; m++) {
        int s0 = FM_MAX(d->hl[m], f->off), s1 = FM_MIN(d->hl[m] + d->hl_len, f->off + f->len);
        float x0 = measure(d->text + f->off, s0 - f->off, run_face(r, bold), size, mono);
        float x1 = measure(d->text + f->off, s1 - f->off, run_face(r, bold), size, mono);
        FmColor hc = m == d->hl_cur ? col_alpha(T.accent, 0.55f) : FM_RGBA(255, 210, 60, 90);
        gfx_rrect(FM_RECT(x + x0 - DP(1), y + lh * 0.06f, x1 - x0 + DP(2), lh * 0.88f), DP(3), hc);
      }
    }
    if ((r->st & DS_CODE) && !(r->st & DS_PRE) && !pre) gfx_rrect(FM_RECT(x - DP(2), y + lh * 0.08f, f->w + DP(4), lh * 0.86f), DP(4), T.surface2);
    float yy = r->st & DS_SMALL ? y + (font_line_h(fs) - font_line_h(size)) * 0.5f : y;
    if (mono) draw_mono(d->text + f->off, f->len, x, yy, size, run_face(r, bold) == FONT_BOLD, c);
    else font_draw(run_face(r, bold), size, x, yy, d->text + f->off, f->len, c);
    if ((r->st & DS_LINK) && r->link >= 0) {
      FmRect hr = { x, y, f->w, lh };
      int fl = ui_hit(ui_idn(id, (u32)k + 1), hr);
      if (fl & UI_HOVER) {
        gfx_rect(FM_RECT(x, y + lh * 0.86f, f->w, DP(1)), c);
        ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
      }
      if (fl & UI_CLICK) hit->link = r->link;
    }
  }
}

/* A picture, then what is laid over it (chess pieces), at its scale. */
static void draw_picture(FmDoc *d, int idx, FmRect r, DocThumbFn thumb) {
  DocImage *im = &d->imgs[idx];
  thumb(d, im, r);
  if (!im->has_layers || im->w <= 0) return;
  float k = r.w / (float)im->w;
  for (int i = idx + 1; i < d->nimgs; i++) {
    DocImage *l = &d->imgs[i];
    if (l->layer_of != idx) continue;
    float lw = (float)(l->w > 0 ? l->w : 16), lh = (float)(l->h > 0 ? l->h : lw);
    thumb(d, l, FM_RECT(r.x + l->lx * k, r.y + l->ly * k, lw * k, lh * k));
  }
}

DocHit doc_draw(FmDoc *d, float x, float y, FmRect clip, u32 id, DocThumbFn thumb) {
  DocHit hit = { -1, -1 };
  float fs = d->lay_fs, w = d->lay_w;
  for (int i = 0; i < d->nb; i++) {
    DocBlock *b = &d->b[i];
    float by = y + b->y;
    if (by > clip.y + clip.h) break;
    if (by + b->h < clip.y) continue;
    float size = block_size(b, fs), ind = block_indent(b);
    switch (b->kind) {
      case DB_IMG: {
        DocImage *im = &d->imgs[b->img];
        float iw, ih;
        image_size(im, w, &iw, &ih);
        FmRect r = { x + (w - iw) * 0.5f, by + DP(4), iw, ih };
        if (thumb) draw_picture(d, b->img, r, thumb);
        int fl = ui_hit(ui_idn(id, 2000000u + (u32)b->img), r);
        if (fl & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
        if (fl & UI_CLICK) hit.img = b->img;
        break;
      }
      case DB_HR: gfx_rect(FM_RECT(x, by + DP(12), w, DP(1)), T.border); break;
      case DB_PRE: {
        FmRect box = { x + ind, by, w - ind * 2, b->h - DP(12) };
        gfx_rrect(box, DP(8), T.surface2);
        gfx_rrect_line(box, DP(8), DP(1), T.border);
        gfx_clip_push(box);
        draw_frags(d, b->f0, b->nf, x, by, size, false, true, clip, id, &hit, T.text);
        gfx_clip_pop();
        break;
      }
      case DB_TABLE: {
        DocTable *t = &d->tabs[b->tab];
        for (int c = t->cell0; c < t->cell0 + t->ncell; c++) {
          DocCell *cl = &d->cells[c];
          FmRect r = { x + cl->x, by + cl->y, cl->w, cl->h };
          if (r.y > clip.y + clip.h || r.y + r.h < clip.y) continue;
          if (cl->head) gfx_rect(r, T.surface2);
          else if (t->box) gfx_rect(r, T.surface);
          gfx_rect(FM_RECT(r.x, r.y, r.w, DP(1)), T.border);
          gfx_rect(FM_RECT(r.x, r.y, DP(1), r.h), T.border);
          gfx_rect(FM_RECT(r.x + r.w - DP(1), r.y, DP(1), r.h), T.border);
          gfx_rect(FM_RECT(r.x, r.y + r.h - DP(1), r.w, DP(1)), T.border);
          float pad = DP(6), top = pad;
          if (cl->img >= 0) {
            float iw, ih;
            image_size(&d->imgs[cl->img], cl->w - pad * 2, &iw, &ih);
            FmRect ir = { r.x + (r.w - iw) * 0.5f, r.y + pad, iw, ih };
            if (thumb) draw_picture(d, cl->img, ir, thumb);
            int fl = ui_hit(ui_idn(id, 2000000u + (u32)cl->img), ir);
            if (fl & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
            if (fl & UI_CLICK) hit.img = cl->img;
            top += ih + DP(4);
          }
          FM_UNUSED(top);
          draw_frags(d, cl->f0, cl->nf, r.x, r.y, fs * 0.92f, cl->head, false, clip, id, &hit, T.text);
        }
        break;
      }
      default: {
        bool bold = b->kind == DB_H || b->kind == DB_DT;
        if (b->kind == DB_LI && b->marker[0]) {
          const DocFrag *f0 = b->nf ? &d->frag[b->f0] : NULL;
          float mw = font_width(FONT_REGULAR, size, b->marker, -1);
          font_draw(FONT_REGULAR, size, x + ind - mw - DP(6), by + (f0 ? f0->y : 0), b->marker, -1, T.text2);
        }
        if (b->kind == DB_QUOTE) gfx_rect(FM_RECT(x + ind - DP(10), by, DP(3), b->h - DP(11)), T.border);
        draw_frags(d, b->f0, b->nf, x, by, size, bold, false, clip, id, &hit, b->kind == DB_CAPTION ? T.text2 : T.text);
        if (b->kind == DB_H && b->level <= 2) {
          const DocFrag *fl = b->nf ? &d->frag[b->f0 + b->nf - 1] : NULL;
          float ly = by + (fl ? fl->y : 0) + font_line_h(size) + DP(4);
          gfx_rect(FM_RECT(x, ly, w, DP(1)), T.border);
        }
        break;
      }
    }
  }
  return hit;
}
