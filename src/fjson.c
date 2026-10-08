/* fjson.c -- JSON parser (see fjson.h).
**
** Design decisions:
**   - Two passes over the text: the first counts nodes and string bytes, the
**     second fills exactly-sized arrays. No reallocation, no per-node malloc.
**   - Recursive descent with a depth limit (64), so hostile input cannot
**     blow the stack.
*/
#include "fjson.h"

#define JSON_MAX_DEPTH 64

typedef struct Parser {
  const char *p, *end;
  FmJson *j;
  bool fill;            /* second pass: write nodes and strings */
  int nodes;            /* counted / written */
  size_t sbytes;
  char *spos;
  int depth;
  const char *err;
} Parser;

static void ws(Parser *ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static bool fail(Parser *ps, const char *msg) {
  if (!ps->err) ps->err = msg;
  return false;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hex4(Parser *ps, u32 *v) {
  if (ps->end - ps->p < 4) return false;
  u32 r = 0;
  for (int i = 0; i < 4; i++) {
    int h = hexv(ps->p[i]);
    if (h < 0) return false;
    r = r << 4 | (u32)h;
  }
  ps->p += 4;
  *v = r;
  return true;
}

static void put_utf8(Parser *ps, u32 cp) {
  u8 b[4];
  int n;
  if (cp < 0x80) { b[0] = (u8)cp; n = 1; }
  else if (cp < 0x800) { b[0] = (u8)(0xC0 | cp >> 6); b[1] = (u8)(0x80 | (cp & 63)); n = 2; }
  else if (cp < 0x10000) {
    b[0] = (u8)(0xE0 | cp >> 12); b[1] = (u8)(0x80 | ((cp >> 6) & 63)); b[2] = (u8)(0x80 | (cp & 63)); n = 3;
  } else {
    b[0] = (u8)(0xF0 | cp >> 18); b[1] = (u8)(0x80 | ((cp >> 12) & 63));
    b[2] = (u8)(0x80 | ((cp >> 6) & 63)); b[3] = (u8)(0x80 | (cp & 63)); n = 4;
  }
  if (ps->fill) memcpy(ps->spos, b, (size_t)n), ps->spos += n;
  ps->sbytes += (size_t)n;
}

/* Parses a string at ps->p (on the quote); returns its pooled copy (fill pass). */
static bool str(Parser *ps, const char **out) {
  ps->p++;                                     /* opening quote */
  char *start = ps->spos;
  for (;;) {
    if (ps->p >= ps->end) return fail(ps, "unterminated string");
    char c = *ps->p++;
    if (c == '"') break;
    if ((u8)c < 0x20) return fail(ps, "control character in string");
    if (c != '\\') {
      if (ps->fill) *ps->spos++ = c;
      ps->sbytes++;
      continue;
    }
    if (ps->p >= ps->end) return fail(ps, "bad escape");
    char e = *ps->p++;
    u32 cp;
    switch (e) {
      case '"': cp = '"'; break;
      case '\\': cp = '\\'; break;
      case '/': cp = '/'; break;
      case 'b': cp = 8; break;
      case 'f': cp = 12; break;
      case 'n': cp = '\n'; break;
      case 'r': cp = '\r'; break;
      case 't': cp = '\t'; break;
      case 'u':
        if (!hex4(ps, &cp)) return fail(ps, "bad \\u escape");
        if (cp >= 0xD800 && cp <= 0xDBFF) {      /* surrogate pair */
          u32 lo;
          if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
            ps->p += 2;
            if (!hex4(ps, &lo)) return fail(ps, "bad \\u escape");
            cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
          } else {
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        break;
      default: return fail(ps, "bad escape");
    }
    put_utf8(ps, cp);
  }
  if (ps->fill) *ps->spos++ = 0;
  ps->sbytes++;
  if (out) *out = ps->fill ? start : NULL;
  return true;
}

static bool lit(Parser *ps, const char *w) {
  size_t n = strlen(w);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n)) return fail(ps, "bad literal");
  ps->p += n;
  return true;
}

static bool value(Parser *ps, int *idx_out, const char *key);

static int new_node(Parser *ps, u8 type, const char *key) {
  int i = ps->nodes++;
  if (ps->fill) {
    FmJsonNode *n = &ps->j->nodes[i];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->next = n->child = 0;
    n->key = key;
  }
  return i;
}

static bool container(Parser *ps, int self, bool obj) {
  char close = obj ? '}' : ']';
  ps->p++;
  ws(ps);
  if (ps->p < ps->end && *ps->p == close) { ps->p++; return true; }
  if (++ps->depth > JSON_MAX_DEPTH) return fail(ps, "nested too deeply");
  int prev = -1, count = 0;
  for (;;) {
    ws(ps);
    const char *key = NULL;
    if (obj) {
      if (ps->p >= ps->end || *ps->p != '"') return fail(ps, "expected a member name");
      if (!str(ps, &key)) return false;
      ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') return fail(ps, "expected ':'");
      ps->p++;
      ws(ps);
    }
    int ci;
    if (!value(ps, &ci, key)) return false;
    if (ps->fill) {                /* links are offsets from the node itself */
      if (prev < 0) ps->j->nodes[self].child = ci - self;
      else ps->j->nodes[prev].next = ci - prev;
    }
    prev = ci;
    count++;
    ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (*ps->p == ',') { ps->p++; continue; }
    if (*ps->p == close) { ps->p++; break; }
    return fail(ps, obj ? "expected ',' or '}'" : "expected ',' or ']'");
  }
  ps->depth--;
  if (ps->fill) ps->j->nodes[self].count = count;
  return true;
}

static bool value(Parser *ps, int *idx_out, const char *key) {
  ws(ps);
  if (ps->p >= ps->end) return fail(ps, "unexpected end");
  char c = *ps->p;
  int i;
  if (c == '{' || c == '[') {
    i = new_node(ps, c == '{' ? JSON_OBJ : JSON_ARR, key);
    *idx_out = i;
    return container(ps, i, c == '{');
  }
  if (c == '"') {
    i = new_node(ps, JSON_STR, key);
    const char *s;
    if (!str(ps, &s)) return false;
    if (ps->fill) ps->j->nodes[i].s = s;
  } else if (c == 't') { i = new_node(ps, JSON_TRUE, key); if (!lit(ps, "true")) return false; }
  else if (c == 'f') { i = new_node(ps, JSON_FALSE, key); if (!lit(ps, "false")) return false; }
  else if (c == 'n') { i = new_node(ps, JSON_NULL, key); if (!lit(ps, "null")) return false; }
  else if (c == '-' || (c >= '0' && c <= '9')) {
    i = new_node(ps, JSON_NUM, key);
    char buf[64];
    size_t n = 0;
    while (ps->p < ps->end && n < sizeof buf - 1 &&
           (strchr("+-.eE", *ps->p) || (*ps->p >= '0' && *ps->p <= '9')))
      buf[n++] = *ps->p++;
    buf[n] = 0;
    char *e;
    double d = strtod(buf, &e);
    if (e == buf) return fail(ps, "bad number");
    if (ps->fill) ps->j->nodes[i].n = d;
  } else {
    return fail(ps, "unexpected character");
  }
  *idx_out = i;
  return true;
}

FmErr json_parse(FmJson *j, const char *text, size_t len) {
  memset(j, 0, sizeof *j);
  if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) text += 3, len -= 3;
  Parser ps;
  memset(&ps, 0, sizeof ps);
  ps.p = text;
  ps.end = text + len;
  ps.j = j;
  int root;
  bool ok = value(&ps, &root, NULL);
  if (ok) { ws(&ps); if (ps.p != ps.end) ok = fail(&ps, "text after the value"); }
  if (!ok) {
    fm_snprintf(j->error, sizeof j->error, "%s at byte %d", ps.err ? ps.err : "error", (int)(ps.p - text));
    return FM_ERR_FORMAT;
  }
  int nodes = ps.nodes;
  size_t sbytes = ps.sbytes;
  j->nodes = (FmJsonNode *)fm_alloc((size_t)nodes * sizeof(FmJsonNode));
  j->strings = (char *)fm_alloc(sbytes ? sbytes : 1);
  memset(&ps, 0, sizeof ps);
  ps.p = text;
  ps.end = text + len;
  ps.j = j;
  ps.fill = true;
  ps.spos = j->strings;
  value(&ps, &root, NULL);
  j->count = nodes;
  return FM_OK;
}

void json_free(FmJson *j) {
  fm_free(j->nodes);
  fm_free(j->strings);
  memset(j, 0, sizeof *j);
}

/* ---- walking --------------------------------------------------------------- */

/* Links are relative offsets, so a node pointer alone is enough to walk. */

const FmJsonNode *json_root(const FmJson *j) { return j->count ? &j->nodes[0] : NULL; }

const FmJsonNode *json_first(const FmJsonNode *n) {
  return n && (n->type == JSON_ARR || n->type == JSON_OBJ) && n->child ? n + n->child : NULL;
}

const FmJsonNode *json_next(const FmJsonNode *n) { return n && n->next ? n + n->next : NULL; }

const FmJsonNode *json_get(const FmJsonNode *obj, const char *key) {
  if (!obj || obj->type != JSON_OBJ) return NULL;
  for (const FmJsonNode *c = json_first(obj); c; c = json_next(c))
    if (c->key && !strcmp(c->key, key)) return c;
  return NULL;
}

const FmJsonNode *json_at(const FmJsonNode *arr, int i) {
  if (!arr || (arr->type != JSON_ARR && arr->type != JSON_OBJ) || i < 0) return NULL;
  const FmJsonNode *c = json_first(arr);
  while (c && i-- > 0) c = json_next(c);
  return c;
}

const FmJsonNode *json_path(const FmJsonNode *n, const char *dotted) {
  char part[96];
  while (n && *dotted) {
    size_t k = 0;
    while (*dotted && *dotted != '.' && k < sizeof part - 1) part[k++] = *dotted++;
    part[k] = 0;
    if (*dotted == '.') dotted++;
    bool num = k > 0;
    for (size_t i = 0; i < k; i++) if (part[i] < '0' || part[i] > '9') num = false;
    n = (num && n->type == JSON_ARR) ? json_at(n, atoi(part)) : json_get(n, part);
  }
  return n;
}

const char *json_str(const FmJsonNode *n, const char *def) { return n && n->type == JSON_STR ? n->s : def; }

double json_num(const FmJsonNode *n, double def) {
  if (!n) return def;
  if (n->type == JSON_NUM) return n->n;
  if (n->type == JSON_STR && n->s[0]) {
    char *e;
    double d = strtod(n->s, &e);
    if (e != n->s && !*e) return d;
  }
  return def;
}

bool json_bool(const FmJsonNode *n, bool def) {
  if (!n) return def;
  if (n->type == JSON_TRUE) return true;
  if (n->type == JSON_FALSE) return false;
  return def;
}
