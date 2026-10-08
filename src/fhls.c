/* fhls.c -- HLS playlists: parse, pick, decrypt (see fhls.h).
**
** Design decisions:
**   - One pass over the lines with a small state (the #EXTINF duration, a
**     pending byte range, the current key) applied to the next URI line, as
**     RFC 8216 describes. Unknown tags are skipped, so newer playlists still
**     parse.
**   - Attribute lists (KEY=value,KEY="quoted, value") are read by name with
**     a scanner that honours quotes; nothing else is unescaped (HLS has no
**     escapes).
**   - Lines longer than the line buffer are cut, and a URI cut that way is
**     dropped rather than fetched wrong.
*/
#include "fhls.h"

#define LINE_MAX_ (HLS_URL_MAX + 512)

/* ---- small text helpers ---------------------------------------------------------- */

/* Copies the next line (without CR/LF) into buf; false at the end. */
static bool next_line(const char **pp, const char *end, char *buf, size_t cap, bool *cut) {
  const char *p = *pp;
  if (p >= end) return false;
  const char *e = p;
  while (e < end && *e != '\n' && *e != '\r') e++;
  size_t n = (size_t)(e - p);
  *cut = n >= cap;
  if (n >= cap) n = cap - 1;
  memcpy(buf, p, n);
  buf[n] = 0;
  if (e < end && *e == '\r') e++;
  if (e < end && *e == '\n') e++;
  *pp = e;
  return true;
}

static const char *skip_bom_ws(const char *p, const char *end) {
  if (end - p >= 3 && (u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF) p += 3;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

static bool mem_has(const char *hay, size_t n, const char *needle) {
  size_t k = strlen(needle);
  for (size_t i = 0; i + k <= n; i++)
    if (hay[i] == needle[0] && !memcmp(hay + i, needle, k)) return true;
  return false;
}

/* A decimal integer (optional '-'); *end after it (== s when none). By hand:
** 32-bit msvcrt (tcc) has no strtoll. Saturates instead of overflowing. */
static i64 dec_i64(const char *s, const char **end) {
  const char *p = s;
  while (*p == ' ' || *p == '	') p++;
  bool neg = *p == '-';
  if (*p == '-' || *p == '+') p++;
  const char *d = p;
  u64 v = 0;
  while (*p >= '0' && *p <= '9') {
    if (v < 922337203685477580ull) v = v * 10 + (u64)(*p - '0');
    p++;
  }
  *end = p == d ? s : p;
  return neg ? -(i64)v : (i64)v;
}

static bool starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

/* An attribute's value from "A=1,B=\"x,y\",C=z" (quotes removed). */
static bool attr(const char *list, const char *name, char *out, size_t cap) {
  size_t nl = strlen(name);
  const char *p = list;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    const char *k = p;
    while (*p && *p != '=' && *p != ',') p++;
    bool match = (size_t)(p - k) == nl && !strncmp(k, name, nl);
    if (*p != '=') continue;
    p++;
    const char *v = p, *ve;
    if (*p == '"') {
      v = ++p;
      while (*p && *p != '"') p++;
      ve = p;
      if (*p == '"') p++;
    } else {
      while (*p && *p != ',') p++;
      ve = p;
    }
    if (match) {
      size_t n = FM_MIN((size_t)(ve - v), cap - 1);
      memcpy(out, v, n);
      out[n] = 0;
      return true;
    }
  }
  return false;
}

static i64 attr_i64(const char *list, const char *name, i64 def) {
  char v[32];
  if (!attr(list, name, v, sizeof v)) return def;
  const char *e;
  i64 x = dec_i64(v, &e);
  return e == v ? def : x;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* "0x000102..." -> 16 bytes (shorter values are right-aligned) */
static bool parse_iv(const char *s, u8 iv[16]) {
  if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
  s += 2;
  size_t n = strlen(s);
  if (!n || n > 32) return false;
  memset(iv, 0, 16);
  for (size_t i = 0; i < n; i++) {
    int v = hexval(s[n - 1 - i]);
    if (v < 0) return false;
    iv[15 - i / 2] |= (u8)(i & 1 ? v << 4 : v);
  }
  return true;
}

/* "n[@o]" byte range; *off stays when no offset is given */
static bool parse_range(const char *s, i64 *len, i64 *off) {
  const char *e;
  i64 n = dec_i64(s, &e);
  if (e == s || n < 0) return false;
  *len = n;
  if (*e == '@') {
    const char *o0 = e + 1;
    i64 o = dec_i64(o0, &e);
    if (e == o0 || o < 0) return false;
    *off = o;
  }
  return true;
}

/* ---- URLs ------------------------------------------------------------------------ */

bool hls_sniff(const void *s, size_t n) {
  const char *p = skip_bom_ws((const char *)s, (const char *)s + n);
  size_t left = (size_t)((const char *)s + n - p);
  return left >= 7 && !memcmp(p, "#EXTM3U", 7);
}

/* application/vnd.apple.mpegurl, application/x-mpegURL, audio/mpegurl ... */
bool hls_type_like(const char *t) { return t && fm_stristr(t, "mpegurl") != NULL; }

bool hls_url_like(const char *url) {
  const char *q = url + strcspn(url, "?#");
  size_t n = (size_t)(q - url);
  return (n >= 5 && !fm_strnicmp(q - 5, ".m3u8", 5)) || (n >= 4 && !fm_strnicmp(q - 4, ".m3u", 4));
}

/* length of "scheme:" when s starts with one (a colon before any / ? #) */
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static size_t scheme_len(const char *s) {
  if (!is_alpha(*s)) return 0;
  size_t i = 1;
  while (is_alpha(s[i]) || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
  return s[i] == ':' ? i + 1 : 0;
}

/* RFC 3986 5.2.4 on path[0..n) in place; returns the new length */
static size_t remove_dots(char *path, size_t n) {
  char *in = (char *)fm_alloc(n + 1), *out = path;
  memcpy(in, path, n);
  in[n] = 0;
  const char *p = in;
  size_t o = 0;
  while (*p) {
    if (starts(p, "../")) p += 3;
    else if (starts(p, "./")) p += 2;
    else if (starts(p, "/./")) p += 2;
    else if (!strcmp(p, "/.")) { in[n - 1] = '/'; p = in + n - 1; }   /* the end: "/" remains */
    else if (starts(p, "/../") || !strcmp(p, "/..")) {
      if (p[3] == 0) { in[n - 1] = '/'; p = in + n - 1; }
      else p += 3;
      while (o > 0 && out[o - 1] != '/') o--;        /* drop the last output segment */
      if (o > 0) o--;
    } else if (!strcmp(p, ".") || !strcmp(p, "..")) {
      break;
    } else {
      size_t k = 0;
      if (p[0] == '/') k = 1;
      while (p[k] && p[k] != '/') k++;
      memcpy(out + o, p, k);
      o += k;
      p += k;
    }
  }
  fm_free(in);
  return o;
}

void hls_resolve(const char *base, const char *ref, char *out, size_t cap) {
  size_t rn = strcspn(ref, "#");
  size_t cap2 = strlen(base) + rn + 4;
  char *buf = (char *)fm_alloc(cap2);
  size_t bl = 0;
  size_t rs = scheme_len(ref);
  size_t bs = scheme_len(base);
  /* the base's parts: scheme, authority end, path end */
  size_t b_auth = bs, b_pathend;
  if (!strncmp(base + bs, "//", 2)) b_auth = bs + 2 + strcspn(base + bs + 2, "/?#");
  b_pathend = b_auth + strcspn(base + b_auth, "?#");
  size_t pstart;                                   /* where the path begins in buf */
  if (rs) {                                        /* absolute */
    memcpy(buf, ref, rn);
    bl = rn;
    pstart = rs;
    if (!strncmp(ref + rs, "//", 2)) pstart = rs + 2 + strcspn(ref + rs + 2, "/?#");
    if (pstart > bl) pstart = bl;
  } else if (!strncmp(ref, "//", 2)) {             /* network-path reference */
    memcpy(buf, base, bs);
    memcpy(buf + bs, ref, rn);
    bl = bs + rn;
    pstart = bs + 2 + strcspn(ref + 2, "/?#");
    if (pstart > bl) pstart = bl;
  } else if (rn == 0) {                            /* same document */
    size_t n = b_pathend + strcspn(base + b_pathend, "#");
    memcpy(buf, base, n);
    bl = n;
    pstart = b_auth;
  } else if (ref[0] == '/') {
    memcpy(buf, base, b_auth);
    memcpy(buf + b_auth, ref, rn);
    bl = b_auth + rn;
    pstart = b_auth;
  } else if (ref[0] == '?') {
    memcpy(buf, base, b_pathend);
    memcpy(buf + b_pathend, ref, rn);
    bl = b_pathend + rn;
    pstart = b_auth;
  } else {                                         /* merge with the base's directory */
    size_t dir = b_pathend;
    while (dir > b_auth && base[dir - 1] != '/') dir--;
    memcpy(buf, base, dir);
    bl = dir;
    if (dir == b_auth && b_auth > bs) buf[bl++] = '/';   /* "http://h" + "a" = "http://h/a" */
    memcpy(buf + bl, ref, rn);
    bl += rn;
    pstart = b_auth;
  }
  buf[bl] = 0;
  /* dot segments, in the path only */
  size_t pend = pstart + strcspn(buf + pstart, "?");
  size_t np = remove_dots(buf + pstart, pend - pstart);
  memmove(buf + pstart + np, buf + pend, bl - pend + 1);
  if (strlen(buf) < cap) fm_strlcpy(out, buf, cap);
  else if (cap) out[0] = 0;
  fm_free(buf);
}

/* ---- playlists --------------------------------------------------------------------- */

int hls_kind(const char *text, size_t len) {
  if (!text || !hls_sniff(text, len)) return HLS_NONE;
  return mem_has(text, len, "#EXT-X-STREAM-INF") ? HLS_MASTER : HLS_MEDIA;
}

static void set_err(char *err, size_t cap, const char *msg) {
  if (err && cap) fm_strlcpy(err, msg, cap);
}

typedef struct AudioGroup { char id[32]; const char *uri; bool def; } AudioGroup;

FmErr hls_parse_master(const char *text, size_t len, const char *base, FmHlsMaster *m, char *err, size_t errcap) {
  memset(m, 0, sizeof *m);
  arena_init(&m->a, 16384);
  if (hls_kind(text, len) != HLS_MASTER) {
    set_err(err, errcap, "not a master playlist");
    return FM_ERR_FORMAT;
  }
  char *line = (char *)fm_alloc(LINE_MAX_);
  char *inf = (char *)fm_alloc(LINE_MAX_);
  char *u = (char *)fm_alloc(HLS_URL_MAX);
  char groups_of[HLS_MAX_VARIANTS][32];
  AudioGroup ag[16];
  int nag = 0;
  bool pending = false, cut;
  const char *p = text, *end = text + len;
  while (next_line(&p, end, line, LINE_MAX_, &cut)) {
    if (starts(line, "#EXT-X-STREAM-INF:")) {
      fm_strlcpy(inf, line + 18, LINE_MAX_);
      pending = !cut;
    } else if (starts(line, "#EXT-X-MEDIA:")) {
      char type[16], gid[32], v[8];
      if (!attr(line + 13, "TYPE", type, sizeof type) || strcmp(type, "AUDIO") || nag == FM_COUNT(ag)) continue;
      if (!attr(line + 13, "GROUP-ID", gid, sizeof gid)) continue;
      AudioGroup *g = &ag[nag++];
      memset(g, 0, sizeof *g);
      fm_strlcpy(g->id, gid, sizeof g->id);
      g->def = attr(line + 13, "DEFAULT", v, sizeof v) && !strcmp(v, "YES");
      if (!cut && attr(line + 13, "URI", u, HLS_URL_MAX)) {
        char *abs_ = (char *)fm_alloc(HLS_URL_MAX);
        hls_resolve(base, u, abs_, HLS_URL_MAX);
        if (abs_[0]) g->uri = arena_strdup(&m->a, abs_);
        fm_free(abs_);
      }
    } else if (line[0] && line[0] != '#' && pending) {
      pending = false;
      if (cut || m->n == HLS_MAX_VARIANTS) continue;
      hls_resolve(base, line, u, HLS_URL_MAX);
      if (!u[0]) continue;
      FmHlsVariant *v = &m->v[m->n];
      memset(v, 0, sizeof *v);
      v->uri = arena_strdup(&m->a, u);
      v->bandwidth = (int)FM_CLAMP(attr_i64(inf, "BANDWIDTH", 0), 0, 2000000000);
      v->avg_bandwidth = (int)FM_CLAMP(attr_i64(inf, "AVERAGE-BANDWIDTH", 0), 0, 2000000000);
      char t[64];
      if (attr(inf, "RESOLUTION", t, sizeof t)) {
        int w = 0, h = 0;
        if (sscanf(t, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
          v->width = w;
          v->height = h;
        }
      }
      if (attr(inf, "FRAME-RATE", t, sizeof t)) {
        double f = strtod(t, NULL);
        v->fps = f > 0 && f < 1000 ? f : 0;
      }
      attr(inf, "CODECS", v->codecs, sizeof v->codecs);
      attr(inf, "NAME", v->name, sizeof v->name);
      if (!attr(inf, "AUDIO", groups_of[m->n], sizeof groups_of[0])) groups_of[m->n][0] = 0;
      m->n++;
    }
  }
  /* separate sound: the group's default rendition (or its first with a URI) */
  for (int i = 0; i < m->n; i++) {
    if (!groups_of[i][0]) continue;
    const char *pick = NULL;
    for (int k = 0; k < nag; k++) {
      if (strcmp(ag[k].id, groups_of[i]) || !ag[k].uri) continue;
      if (!pick || ag[k].def) pick = ag[k].uri;
      if (ag[k].def) break;
    }
    m->v[i].audio_uri = pick;
  }
  fm_free(line);
  fm_free(inf);
  fm_free(u);
  if (!m->n) {
    set_err(err, errcap, "the playlist lists no streams");
    hls_master_free(m);
    return FM_ERR_FORMAT;
  }
  return FM_OK;
}

void hls_master_free(FmHlsMaster *m) {
  arena_free(&m->a);
  m->n = 0;
}

static bool is_avc(const FmHlsVariant *v) {
  return !v->codecs[0] || strstr(v->codecs, "avc1") || strstr(v->codecs, "avc3");
}

/* the smaller side of the picture, as the quality menu counts ("720p") */
static int res_of(const FmHlsVariant *v) {
  return v->width > 0 && v->width < v->height ? v->width : v->height;
}

int hls_pick_variant(const FmHlsMaster *m, int max_height) {
  int best = -1, low = -1;
  bool any_h = false;
  for (int i = 0; i < m->n; i++) any_h |= m->v[i].height > 0;
  for (int i = 0; i < m->n; i++) {
    const FmHlsVariant *v = &m->v[i];
    if (!any_h) {                                /* bandwidth only: the best up to 5 Mbit/s */
      bool fits = v->bandwidth <= 5000000;
      if (best < 0) { best = i; continue; }
      bool bfits = m->v[best].bandwidth <= 5000000;
      if (fits != bfits ? fits : fits ? v->bandwidth > m->v[best].bandwidth : v->bandwidth < m->v[best].bandwidth)
        best = i;
      continue;
    }
    int h = res_of(v);
    if (h > 0 && (low < 0 || h < res_of(&m->v[low]))) low = i;
    if (h <= 0 || h > max_height) continue;
    if (best < 0) { best = i; continue; }
    const FmHlsVariant *b = &m->v[best];
    int bh = res_of(b);
    if (h != bh) { if (h > bh) best = i; continue; }
    if (is_avc(v) != is_avc(b)) { if (is_avc(v)) best = i; continue; }
    if (v->bandwidth > b->bandwidth) best = i;
  }
  return best >= 0 ? best : low >= 0 ? low : m->n ? 0 : -1;
}

static FmHlsSeg *add_seg(FmHlsMedia *p) {
  if (p->n == p->segcap) {
    p->segcap = p->segcap ? p->segcap * 2 : 64;
    p->seg = (FmHlsSeg *)fm_realloc(p->seg, sizeof *p->seg * (size_t)p->segcap);
  }
  FmHlsSeg *s = &p->seg[p->n++];
  memset(s, 0, sizeof *s);
  return s;
}

FmErr hls_parse_media(const char *text, size_t len, const char *base, FmHlsMedia *p, char *err, size_t errcap) {
  memset(p, 0, sizeof *p);
  arena_init(&p->a, 16384);
  if (hls_kind(text, len) != HLS_MEDIA) {
    set_err(err, errcap, hls_kind(text, len) == HLS_MASTER ? "a master playlist, not a media playlist"
                                                            : "not an HLS playlist");
    return FM_ERR_FORMAT;
  }
  p->base = arena_strdup(&p->a, base);
  p->target = 0;
  p->map_len = -1;
  char *line = (char *)fm_alloc(LINE_MAX_);
  char *u = (char *)fm_alloc(HLS_URL_MAX);
  double dur = -1;
  bool disc = false, cut, have_br = false;
  i64 br_len = -1, br_off = 0, br_next = 0;
  int key = -1;
  i64 seq = 0;
  const char *last_uri = NULL;
  const char *q = text, *end = text + len;
  while (next_line(&q, end, line, LINE_MAX_, &cut)) {
    if (line[0] == '#') {
      if (starts(line, "#EXTINF:")) {
        dur = strtod(line + 8, NULL);
        if (!(dur >= 0 && dur < 86400)) dur = 0;
      } else if (starts(line, "#EXT-X-TARGETDURATION:")) {
        double t = strtod(line + 22, NULL);
        p->target = t > 0 && t < 86400 ? t : 0;
      } else if (starts(line, "#EXT-X-MEDIA-SEQUENCE:")) {
        const char *e;
        i64 v = dec_i64(line + 22, &e);
        if (!p->n && v >= 0 && e != line + 22) seq = v;
      } else if (starts(line, "#EXT-X-PLAYLIST-TYPE:")) {
        p->vod = !strncmp(line + 21, "VOD", 3);
      } else if (starts(line, "#EXT-X-ENDLIST")) {
        p->endlist = true;
      } else if (starts(line, "#EXT-X-DISCONTINUITY") && !starts(line, "#EXT-X-DISCONTINUITY-")) {
        disc = true;
      } else if (starts(line, "#EXT-X-BYTERANGE:")) {
        br_off = br_next;
        have_br = parse_range(line + 17, &br_len, &br_off);
      } else if (starts(line, "#EXT-X-KEY:")) {
        char m[32];
        if (!attr(line + 11, "METHOD", m, sizeof m) || !strcmp(m, "NONE")) { key = -1; continue; }
        if (p->nkey == HLS_MAX_KEYS) { key = -1; continue; }
        if (p->nkey == p->keycap) {
          p->keycap = p->keycap ? p->keycap * 2 : 4;
          p->key = (FmHlsKey *)fm_realloc(p->key, sizeof *p->key * (size_t)p->keycap);
        }
        FmHlsKey *k = &p->key[p->nkey];
        memset(k, 0, sizeof *k);
        k->method = !strcmp(m, "AES-128") ? HLS_KEY_AES128 : HLS_KEY_OTHER;
        if (!cut && attr(line + 11, "URI", u, HLS_URL_MAX)) {
          char *abs_ = (char *)fm_alloc(HLS_URL_MAX);
          hls_resolve(base, u, abs_, HLS_URL_MAX);
          if (abs_[0]) k->uri = arena_strdup(&p->a, abs_);
          fm_free(abs_);
        }
        char iv[48];
        if (attr(line + 11, "IV", iv, sizeof iv)) k->has_iv = parse_iv(iv, k->iv);
        if (k->method == HLS_KEY_AES128 && !k->uri) k->method = HLS_KEY_OTHER;   /* unusable */
        key = p->nkey++;
      } else if (starts(line, "#EXT-X-MAP:")) {
        char *abs_ = (char *)fm_alloc(HLS_URL_MAX);
        abs_[0] = 0;
        if (!cut && attr(line + 11, "URI", u, HLS_URL_MAX)) hls_resolve(base, u, abs_, HLS_URL_MAX);
        if (abs_[0]) {
          i64 ml = -1, mo = 0;
          char r[64];
          if (attr(line + 11, "BYTERANGE", r, sizeof r)) parse_range(r, &ml, &mo);
          if (!p->map_uri) {
            p->map_uri = arena_strdup(&p->a, abs_);
            p->map_len = ml;
            p->map_off = mo;
          } else if (strcmp(p->map_uri, abs_) || ml != p->map_len || mo != p->map_off) {
            p->map_changes = true;
          }
        }
        fm_free(abs_);
      }
      continue;
    }
    if (!line[0]) continue;
    /* a segment URI */
    if (cut || p->n >= HLS_MAX_SEGS) { dur = -1; disc = false; have_br = false; continue; }
    FmHlsSeg *s = add_seg(p);
    s->uri = last_uri && !strcmp(last_uri, line) ? last_uri : arena_strdup(&p->a, line);
    last_uri = s->uri;
    s->dur = dur >= 0 ? dur : p->target;
    s->start = p->total;
    s->seq = seq++;
    s->key = key;
    s->disc = disc;
    s->br_len = have_br ? br_len : -1;
    s->br_off = have_br ? br_off : 0;
    if (have_br) br_next = br_off + br_len;
    p->total += s->dur;
    dur = -1;
    disc = have_br = false;
  }
  fm_free(line);
  fm_free(u);
  p->first_seq = p->n ? p->seg[0].seq : seq;
  if (p->vod) p->endlist = true;
  if (!p->target && p->n) {                         /* tolerate a missing target duration */
    for (int i = 0; i < p->n; i++) p->target = FM_MAX(p->target, p->seg[i].dur);
  }
  if (!p->n && p->endlist) {
    set_err(err, errcap, "the playlist has no segments");
    hls_media_free(p);
    return FM_ERR_FORMAT;
  }
  return FM_OK;
}

void hls_media_free(FmHlsMedia *p) {
  fm_free(p->seg);
  fm_free(p->key);
  arena_free(&p->a);
  memset(p, 0, sizeof *p);
}

int hls_seg_at(const FmHlsMedia *p, double t) {
  if (p->n <= 0 || t <= 0) return 0;
  int lo = 0, hi = p->n - 1;
  while (lo < hi) {                                 /* the last segment starting at or before t */
    int mid = (lo + hi + 1) / 2;
    if (p->seg[mid].start <= t) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

/* ---- AES-128 -------------------------------------------------------------------- */

void hls_seq_iv(i64 seq, u8 iv[16]) {
  memset(iv, 0, 16);
  u64 v = (u64)seq;
  for (int i = 15; i >= 8; i--, v >>= 8) iv[i] = (u8)v;
}

void hls_dec_init(FmHlsDec *d, const u8 key[16], const u8 iv[16]) {
  memset(d, 0, sizeof *d);
  aes_init(&d->aes, key, 16);
  memcpy(d->iv, iv, 16);
}

bool hls_dec_feed(FmHlsDec *d, const u8 *p, size_t n, FmHlsSink sink, void *user) {
  u8 out[4096];
  size_t no = 0;
  while (n > 0) {
    size_t take = FM_MIN((size_t)(16 - d->nin), n);
    memcpy(d->in + d->nin, p, take);
    d->nin += (int)take;
    p += take;
    n -= take;
    if (d->nin < 16) break;
    d->nin = 0;
    if (d->held) {                       /* not the last block after all */
      memcpy(out + no, d->hold, 16);
      no += 16;
      if (no == sizeof out) {
        if (!sink(user, out, no)) return false;
        no = 0;
      }
    }
    aes_decrypt_block(&d->aes, d->in, d->hold);
    for (int i = 0; i < 16; i++) d->hold[i] ^= d->iv[i];
    memcpy(d->iv, d->in, 16);
    d->held = true;
  }
  return !no || sink(user, out, no);
}

bool hls_dec_end(FmHlsDec *d, FmHlsSink sink, void *user) {
  if (d->nin) return false;
  if (!d->held) return true;
  int pad = d->hold[15];
  if (pad < 1 || pad > 16) return false;
  for (int i = 16 - pad; i < 16; i++)
    if (d->hold[i] != pad) return false;
  d->held = false;
  return pad == 16 || sink(user, d->hold, (size_t)(16 - pad));
}
