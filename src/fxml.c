/* fxml.c -- a small, bounded, tolerant XML reader (see fxml.h).
**
** Design decisions:
**   - A pull reader over the caller's buffer instead of a tree: an 8 MB
**     podcast feed is walked once with no allocation at all, and only the
**     few fields a caller asks for are ever decoded (into its buffers).
**   - Tolerant rather than validating: mismatched end tags only move the
**     depth counter, stray '<' characters become text, unknown entities
**     (HTML's &nbsp; in a careless feed) stay as written. Real feeds break
**     the rules often enough that a strict parser would lose episodes.
**   - Safe by construction: DOCTYPE (and its [internal subset]) is skipped
**     unread, so entity definitions, external entities and "billion laughs"
**     never happen; every scan is bounded by the length, never by a NUL.
**   - Encodings: UTF-8 (BOM skipped) as is; ISO-8859-1 / windows-1252 feeds
**     (declared in <?xml?>) are widened to UTF-8 while decoding. UTF-16
**     feeds are not supported (they read as no elements).
*/
#include "fxml.h"

/* ---- helpers ------------------------------------------------------------------- */

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* index of `pat` in s[from..len), or len when missing */
static size_t find(const char *s, size_t len, size_t from, const char *pat) {
  size_t m = strlen(pat);
  if (m == 0 || len < m) return len;
  for (size_t i = from; i + m <= len; i++)
    if (s[i] == pat[0] && !memcmp(s + i, pat, m)) return i;
  return len;
}

static bool starts(const FmXml *x, size_t at, const char *pat) {
  size_t m = strlen(pat);
  return at + m <= x->len && !memcmp(x->s + at, pat, m);
}

/* Drops an incomplete UTF-8 sequence at the end of out[0..n). */
static size_t utf8_trim(char *out, size_t n) {
  size_t i = n;
  int back = 0;
  while (i > 0 && back < 4 && ((u8)out[i - 1] & 0xC0) == 0x80) { i--; back++; }
  if (i == 0) return back ? 0 : n;
  u8 lead = (u8)out[i - 1];
  if (lead < 0x80) return n;
  int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  return need == back + 1 ? n : i - 1;
}

/* writes one byte of the source (widened when latin1); false when full */
static bool put_src(char *out, size_t cap, size_t *o, u8 c, bool latin1) {
  if (latin1 && c >= 0x80) {
    if (*o + 3 > cap) return false;
    out[(*o)++] = (char)(0xC0 | (c >> 6));
    out[(*o)++] = (char)(0x80 | (c & 0x3F));
    return true;
  }
  if (*o + 2 > cap) return false;
  out[(*o)++] = (char)c;
  return true;
}

size_t xml_decode(const char *s, size_t n, bool latin1, char *out, size_t cap) {
  static const struct { const char *name; char c; } kEnt[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
  };
  size_t o = 0;
  if (!cap) return 0;
  for (size_t i = 0; i < n;) {
    if (s[i] == '&') {
      bool done = false;
      for (int k = 0; k < FM_COUNT(kEnt) && !done; k++) {
        size_t m = strlen(kEnt[k].name);
        if (i + 1 + m <= n && !memcmp(s + i + 1, kEnt[k].name, m)) {
          if (o + 2 > cap) goto full;
          out[o++] = kEnt[k].c;
          i += 1 + m;
          done = true;
        }
      }
      if (!done && i + 2 < n && s[i + 1] == '#') {
        size_t p = i + 2;
        bool hex = s[p] == 'x' || s[p] == 'X';
        u32 cp = 0;
        int digits = 0;
        if (hex) p++;
        for (; p < n && digits < 7; p++, digits++) {
          char c = s[p];
          int d = (c >= '0' && c <= '9') ? c - '0'
                : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
          if (d < 0) break;
          cp = cp * (hex ? 16 : 10) + (u32)d;
        }
        if (digits && p < n && s[p] == ';' && cp && cp < 0x110000 && (cp < 0xD800 || cp > 0xDFFF)) {
          char u[4];
          int m = utf8_encode(cp, u);
          if (o + (size_t)m + 1 > cap) goto full;
          memcpy(out + o, u, (size_t)m);
          o += (size_t)m;
          i = p + 1;
          done = true;
        }
      }
      if (done) continue;
    }
    if (!put_src(out, cap, &o, (u8)s[i], latin1)) goto full;
    i++;
  }
  out[o] = 0;
  return o;
full:
  o = utf8_trim(out, o);
  out[o] = 0;
  return o;
}

/* ---- tokens -------------------------------------------------------------------- */

void xml_init(FmXml *x, const char *s, size_t len) {
  memset(x, 0, sizeof *x);
  x->s = s ? s : "";
  x->len = s ? len : 0;
  if (x->len >= 3 && !memcmp(x->s, "\xEF\xBB\xBF", 3)) x->pos = 3;
}

/* <?xml version="1.0" encoding="ISO-8859-1"?> */
static void read_decl(FmXml *x, size_t from, size_t to) {
  size_t e = find(x->s, to, from, "encoding");
  if (e >= to) return;
  e += 8;
  while (e < to && (is_ws(x->s[e]) || x->s[e] == '=' || x->s[e] == '"' || x->s[e] == '\'')) e++;
  size_t n = to - e;
  if ((n >= 10 && !fm_strnicmp(x->s + e, "iso-8859-1", 10)) || (n >= 6 && !fm_strnicmp(x->s + e, "latin1", 6)) ||
      (n >= 12 && !fm_strnicmp(x->s + e, "windows-1252", 12)) || (n >= 8 && !fm_strnicmp(x->s + e, "us-ascii", 8)))
    x->latin1 = true;
}

/* end of a tag starting at `from`: the '>' outside quotes, or len */
static size_t tag_end(const FmXml *x, size_t from) {
  char q = 0;
  for (size_t i = from; i < x->len; i++) {
    char c = x->s[i];
    if (q) { if (c == q) q = 0; }
    else if (c == '"' || c == '\'') q = c;
    else if (c == '>') return i;
  }
  return x->len;
}

/* <!DOCTYPE ... [ ... ]> and other declarations: skipped unread */
static size_t decl_end(const FmXml *x, size_t from) {
  int bracket = 0;
  char q = 0;
  for (size_t i = from; i < x->len; i++) {
    char c = x->s[i];
    if (q) { if (c == q) q = 0; continue; }
    if (c == '"' || c == '\'') q = c;
    else if (c == '[') bracket++;
    else if (c == ']') { if (bracket) bracket--; }
    else if (c == '>' && !bracket) return i;
  }
  return x->len;
}

static bool name_char(char c) { return c && !is_ws(c) && c != '/' && c != '>' && c != '<' && c != '='; }

int xml_next(FmXml *x) {
  for (;;) {
    x->name = x->attrs = x->text = NULL;
    x->name_len = x->attrs_len = x->text_len = 0;
    x->empty = x->cdata = false;
    if (x->pos >= x->len) return x->type = XML_EOF;
    const char *s = x->s;
    size_t p = x->pos;
    if (s[p] != '<') {
      size_t e = p;
      while (e < x->len && s[e] != '<') e++;
      x->text = s + p;
      x->text_len = e - p;
      x->pos = e;
      return x->type = XML_TEXT;
    }
    if (starts(x, p, "<!--")) {
      size_t e = find(s, x->len, p + 4, "-->");
      x->pos = e >= x->len ? x->len : e + 3;
      continue;
    }
    if (starts(x, p, "<![CDATA[")) {
      size_t b = p + 9, e = find(s, x->len, b, "]]>");
      x->text = s + b;
      x->text_len = e - b;
      x->cdata = true;
      x->pos = e >= x->len ? x->len : e + 3;
      return x->type = XML_TEXT;
    }
    if (starts(x, p, "<!")) {
      size_t e = decl_end(x, p + 2);
      x->pos = e >= x->len ? x->len : e + 1;
      continue;
    }
    if (starts(x, p, "<?")) {
      size_t e = find(s, x->len, p + 2, "?>");
      if (starts(x, p, "<?xml") && p + 5 < x->len && is_ws(s[p + 5])) read_decl(x, p + 5, e);
      x->pos = e >= x->len ? x->len : e + 2;
      continue;
    }
    bool end = p + 1 < x->len && s[p + 1] == '/';
    size_t nb = p + (end ? 2 : 1), ne = nb;
    while (ne < x->len && name_char(s[ne])) ne++;
    if (ne == nb) {                              /* "a < b": a stray '<' is text */
      size_t e = p + 1;
      while (e < x->len && s[e] != '<') e++;
      x->text = s + p;
      x->text_len = e - p;
      x->pos = e;
      return x->type = XML_TEXT;
    }
    size_t te = tag_end(x, ne);
    if (te >= x->len) { x->pos = x->len; return x->type = XML_EOF; }   /* truncated tag */
    x->name = s + nb;
    x->name_len = ne - nb;
    x->pos = te + 1;
    if (end) {
      if (x->depth > 0) x->depth--;
      return x->type = XML_END;
    }
    size_t ae = te;
    if (ae > ne && s[ae - 1] == '/') { x->empty = true; ae--; }
    x->attrs = s + ne;
    x->attrs_len = ae - ne;
    if (!x->empty && x->depth < XML_MAX_DEPTH) x->depth++;
    return x->type = XML_START;
  }
}

/* ---- names and attributes ------------------------------------------------------- */

bool xml_is(const FmXml *x, const char *qname) {
  size_t n = strlen(qname);
  return (x->type == XML_START || x->type == XML_END) && x->name_len == n && !memcmp(x->name, qname, n);
}

bool xml_is_ns(const FmXml *x, const char *prefix, const char *local) {
  size_t pn = prefix ? strlen(prefix) : 0, ln = strlen(local);
  if (x->type != XML_START && x->type != XML_END) return false;
  if (!pn) return x->name_len == ln && !memcmp(x->name, local, ln);
  return x->name_len == pn + 1 + ln && !memcmp(x->name, prefix, pn) && x->name[pn] == ':' &&
         !memcmp(x->name + pn + 1, local, ln);
}

/* Calls fn for each attribute (name, raw value) until it returns true. */
typedef bool (*AttrFn)(const char *name, size_t nl, const char *val, size_t vl, void *user);

static bool each_attr(const FmXml *x, AttrFn fn, void *user) {
  if (x->type != XML_START || !x->attrs) return false;
  const char *a = x->attrs;
  size_t n = x->attrs_len, i = 0;
  while (i < n) {
    while (i < n && is_ws(a[i])) i++;
    size_t nb = i;
    while (i < n && !is_ws(a[i]) && a[i] != '=') i++;
    size_t nl = i - nb;
    while (i < n && is_ws(a[i])) i++;
    size_t vb = i, vl = 0;
    if (i < n && a[i] == '=') {
      i++;
      while (i < n && is_ws(a[i])) i++;
      if (i < n && (a[i] == '"' || a[i] == '\'')) {
        char q = a[i++];
        vb = i;
        while (i < n && a[i] != q) i++;
        vl = i - vb;
        if (i < n) i++;
      } else {
        vb = i;
        while (i < n && !is_ws(a[i])) i++;
        vl = i - vb;
      }
    }
    if (nl && fn(a + nb, nl, a + vb, vl, user)) return true;
    if (nl == 0 && i == nb) i++;                 /* garbage: always move on */
  }
  return false;
}

typedef struct AttrFind { const char *name; const char *val; size_t vl; } AttrFind;

static bool attr_named(const char *name, size_t nl, const char *val, size_t vl, void *user) {
  AttrFind *f = (AttrFind *)user;
  if (nl != strlen(f->name) || memcmp(name, f->name, nl)) return false;
  f->val = val;
  f->vl = vl;
  return true;
}

bool xml_attr(const FmXml *x, const char *name, char *out, size_t cap) {
  AttrFind f;
  if (cap) out[0] = 0;
  f.name = name;
  f.val = NULL;
  f.vl = 0;
  if (!each_attr(x, attr_named, &f)) return false;
  if (cap) xml_decode(f.val, f.vl, x->latin1, out, cap);
  return true;
}

typedef struct NsFind { const char *uri; const char *prefix; size_t pl; } NsFind;

static bool attr_ns(const char *name, size_t nl, const char *val, size_t vl, void *user) {
  NsFind *f = (NsFind *)user;
  if (nl <= 6 || memcmp(name, "xmlns:", 6) || vl != strlen(f->uri) || memcmp(val, f->uri, vl)) return false;
  f->prefix = name + 6;
  f->pl = nl - 6;
  return true;
}

bool xml_ns_prefix(const FmXml *x, const char *uri, char *out, size_t cap) {
  NsFind f;
  if (cap) out[0] = 0;
  f.uri = uri;
  f.prefix = NULL;
  f.pl = 0;
  if (!each_attr(x, attr_ns, &f) || f.pl + 1 > cap) return false;
  memcpy(out, f.prefix, f.pl);
  out[f.pl] = 0;
  return true;
}

/* ---- text ---------------------------------------------------------------------- */

size_t xml_text_append(const FmXml *x, char *out, size_t cap) {
  if (!cap) return 0;
  size_t o = 0;
  while (o + 1 < cap && out[o]) o++;
  out[o] = 0;
  if (x->type != XML_TEXT || !x->text_len) return o;
  if (x->cdata) {
    for (size_t i = 0; i < x->text_len; i++)
      if (!put_src(out, cap, &o, (u8)x->text[i], x->latin1)) {
        o = utf8_trim(out, o);
        break;
      }
    out[o] = 0;
    return o;
  }
  return o + xml_decode(x->text, x->text_len, x->latin1, out + o, cap - o);
}

void xml_skip(FmXml *x) {
  if (x->type != XML_START || x->empty) return;
  int target = x->depth - 1;
  while (xml_next(x) != XML_EOF)
    if (x->type == XML_END && x->depth <= target) return;
}

void xml_inner_text(FmXml *x, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (x->type != XML_START || x->empty) return;
  int target = x->depth - 1;
  while (xml_next(x) != XML_EOF) {
    if (x->type == XML_END && x->depth <= target) break;
    if (x->type == XML_TEXT) xml_text_append(x, out, cap);
  }
  /* trim */
  size_t b = 0, n = strlen(out);
  while (b < n && is_ws(out[b])) b++;
  while (n > b && is_ws(out[n - 1])) n--;
  if (b) memmove(out, out + b, n - b);
  out[n - b] = 0;
}
