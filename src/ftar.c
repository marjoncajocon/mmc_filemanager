/* ftar.c -- tar archives, plain or compressed (gz, bz2, xz, zst, lzma):
** list, extract and create.
**
** Design decisions:
**   - Reading understands POSIX ustar (with the 155-byte prefix), pax
**     extended headers (path, linkpath, size, mtime; global headers are
**     skipped), GNU long names and links (L/K), GNU base-256 numbers and
**     old v7 headers. Devices, fifos, volume labels and GNU sparse files
**     are not listed.
**   - Listing a plain tar seeks over the data; a compressed tar is read
**     once to list and once more to extract (the solid-archive way),
**     streaming selected entries to the sink. Each entry keeps its data
**     offset in the uncompressed stream, so extraction never re-parses.
**   - Hard links are extracted as copies of their target's data (read
**     again from the target's offset; for compressed tars a second
**     decoder is started), so they work on file systems without links.
**   - Names that are not valid UTF-8 are read as CP437 (bsdtar on Windows
**     writes the OEM code page in ustar/gnutar mode).
**   - pax and GNU headers are capped at 1 MB; anything bigger is skipped
**     instead of being trusted for an allocation.
**   - Writing makes POSIX ustar with a pax header only when needed:
**     names over 100 bytes or not ASCII, sizes of 8 GB and more, and
**     mtimes outside the octal field. Numbers are formatted here (msvcrt's
**     printf lacks %llo on old Windows).
*/
#include "farc_int.h"
#include "farc_ext.h"
#include "fstream.h"

#define PAX_MAX (1024 * 1024)

/* ---- numbers ------------------------------------------------------------- */

static u64 tar_num(const u8 *p, int n) {
  if (p[0] & 0x80) {                 /* GNU base-256 */
    if (p[0] == 0xFF) return 0;      /* negative */
    u64 v = p[0] & 0x3F;
    for (int i = 1; i < n; i++) v = (v << 8) | p[i];
    return v;
  }
  u64 v = 0;
  int i = 0;
  while (i < n && (p[i] == ' ' || p[i] == 0)) {
    if (p[i] == 0 && i == 0) return 0;
    i++;
  }
  for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) | (u64)(p[i] - '0');
  return v;
}

static bool tar_sum_ok(const u8 *h) {
  u32 sum = 0;
  i32 ssum = 0;
  for (int i = 0; i < 512; i++) {
    u8 c = (i >= 148 && i < 156) ? ' ' : h[i];
    sum += c;
    ssum += (i8)c;
  }
  u64 want = tar_num(h + 148, 8);
  return want == sum || (i64)want == (i64)ssum;
}

static bool all_zero(const u8 *h) {
  for (int i = 0; i < 512; i++)
    if (h[i]) return false;
  return true;
}

/* ---- source: plain file or decoder -------------------------------------- */

typedef struct TarSrc {
  FILE *f;
  FmCodec codec;
  FmIn *in;              /* NULL for a plain tar */
  u64 pos;               /* offset in the uncompressed stream */
  FmErr err;
} TarSrc;

static FmErr src_open(TarSrc *t, FILE *f, FmCodec codec) {
  memset(t, 0, sizeof *t);
  t->f = f;
  t->codec = codec;
  if (fm_fseek64(f, 0, SEEK_SET) != 0) return FM_ERR_IO;
  if (codec == CODEC_STORE) return FM_OK;
  FmErr e;
  t->in = in_open(f, codec, -1, &e);
  return t->in ? FM_OK : e;
}

static void src_close(TarSrc *t) {
  if (t->in) in_close(t->in);
  t->in = NULL;
}

/* Reads up to n bytes; fewer only at the end. -1 on error. */
static long src_read(TarSrc *t, void *buf, size_t n) {
  long r;
  if (t->in) {
    r = in_read(t->in, buf, n);
    if (r < 0) t->err = in_error(t->in);
  } else {
    size_t k = fread(buf, 1, n, t->f);
    r = (k == 0 && ferror(t->f)) ? -1 : (long)k;
    if (r < 0) t->err = FM_ERR_IO;
  }
  if (r > 0) t->pos += (u64)r;
  return r;
}

static FmErr src_skip(TarSrc *t, u64 n, u8 *scratch) {
  if (!t->in) {
    if (fm_fseek64(t->f, (i64)(t->pos + n), SEEK_SET) != 0) return FM_ERR_IO;
    t->pos += n;
    return FM_OK;
  }
  while (n) {
    long r = src_read(t, scratch, (size_t)FM_MIN(n, (u64)ARC_BUF));
    if (r < 0) return t->err;
    if (r == 0) return FM_ERR_FORMAT;
    n -= (u64)r;
  }
  return FM_OK;
}

/* Moves to an uncompressed offset; backwards restarts the decoder. */
static FmErr src_seek(TarSrc *t, u64 off, u8 *scratch) {
  if (!t->in) {
    if (fm_fseek64(t->f, (i64)off, SEEK_SET) != 0) return FM_ERR_IO;
    t->pos = off;
    return FM_OK;
  }
  if (off < t->pos) {
    src_close(t);
    FmErr e = src_open(t, t->f, t->codec);
    if (e) return e;
  }
  return src_skip(t, off - t->pos, scratch);
}

/* ---- listing ------------------------------------------------------------ */

typedef struct TarEnt {
  u64 data;              /* data offset in the uncompressed stream */
  u64 size;              /* bytes of data stored (hard links: 0) */
  const char *link;      /* symlink target */
  int hard;              /* hard link: index of the target, -1 none */
} TarEnt;

typedef struct TarPriv {
  FmCodec codec;
  TarEnt *t;
  int cap;
} TarPriv;

static FmCodec codec_for_fmt(FmArc *a) {
  switch (a->fmt) {
    case ARC_TAR_GZ: return CODEC_GZIP;
    case ARC_TAR_BZ2: return CODEC_BZIP2;
    case ARC_TAR_XZ: return CODEC_XZ;
    case ARC_TAR_ZST: return CODEC_ZSTD;
    default: break;
  }
  /* ARC_TAR: trust the bytes (a misnamed .tar, or .tar.lzma / .tlz) */
  u8 h[512];
  size_t n = 0;
  if (fm_fseek64(a->f, 0, SEEK_SET) == 0) n = fread(h, 1, sizeof h, a->f);
  if (n >= 3 && h[0] == 0x1F && h[1] == 0x8B) return CODEC_GZIP;
  if (n >= 3 && h[0] == 'B' && h[1] == 'Z' && h[2] == 'h') return CODEC_BZIP2;
  if (n >= 6 && memcmp(h, "\xFD" "7zXZ\0", 6) == 0) return CODEC_XZ;
  if (n >= 4 && memcmp(h, "\x28\xB5\x2F\xFD", 4) == 0) return CODEC_ZSTD;
  if (n == 512 && (memcmp(h + 257, "ustar", 5) == 0 || tar_sum_ok(h))) return CODEC_STORE;
  if (n >= 13 && h[0] < 225 && (fm_ends_with_i(a->path, ".lzma") || fm_ends_with_i(a->path, ".tlz")))
    return CODEC_LZMA;
  return CODEC_STORE;
}

/* Reads an extended header's data (capped); NULL when skipped. */
static char *read_blob(TarSrc *t, u64 size, u8 *scratch, FmErr *err) {
  *err = FM_OK;
  u64 padded = (size + 511) & ~(u64)511;
  if (size > PAX_MAX) {
    *err = src_skip(t, padded, scratch);
    return NULL;
  }
  char *b = (char *)fm_alloc((size_t)padded + 1);
  long r = padded ? src_read(t, b, (size_t)padded) : 0;
  if (r < 0 || (u64)r != padded) {
    fm_free(b);
    *err = r < 0 ? t->err : FM_ERR_FORMAT;
    return NULL;
  }
  b[size] = 0;
  return b;
}

typedef struct TarMeta {
  char *path, *link;     /* pax or GNU, heap */
  u64 size;
  i64 mtime;
  bool has_size, has_mtime;
} TarMeta;

static void meta_reset(TarMeta *m) {
  fm_free(m->path);
  fm_free(m->link);
  memset(m, 0, sizeof *m);
}

static void pax_parse(TarMeta *m, const char *b, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t len = 0, j = i;
    while (j < n && b[j] >= '0' && b[j] <= '9' && len < 100000000) len = len * 10 + (size_t)(b[j++] - '0');
    if (j >= n || b[j] != ' ' || len < 5 || len > n - i || b[i + len - 1] != '\n') return;
    const char *key = b + j + 1, *end = b + i + len - 1;
    const char *eq = key;
    while (eq < end && *eq != '=') eq++;
    if (eq < end) {
      size_t kl = (size_t)(eq - key);
      const char *v = eq + 1;
      size_t vl = (size_t)(end - v);
      if (kl == 4 && memcmp(key, "path", 4) == 0) {
        fm_free(m->path);
        m->path = fm_strndup(v, vl);
      } else if (kl == 8 && memcmp(key, "linkpath", 8) == 0) {
        fm_free(m->link);
        m->link = fm_strndup(v, vl);
      } else if (kl == 4 && memcmp(key, "size", 4) == 0) {
        u64 s = 0;
        for (size_t k = 0; k < vl && v[k] >= '0' && v[k] <= '9'; k++) s = s * 10 + (u64)(v[k] - '0');
        m->size = s;
        m->has_size = true;
      } else if (kl == 5 && memcmp(key, "mtime", 5) == 0) {
        i64 t = 0;
        size_t k = 0;
        bool neg = vl && v[0] == '-';
        if (neg) k++;
        for (; k < vl && v[k] >= '0' && v[k] <= '9' && t < ((i64)1 << 40); k++) t = t * 10 + (v[k] - '0');
        m->mtime = neg ? -t : t;
        m->has_mtime = true;
      }
    }
    i += len;
  }
}

static int find_entry(FmArc *a, const char *path, int before) {
  char tmp[FM_PATH_MAX];
  size_t o = 0;
  for (const char *p = path; *p && o + 1 < sizeof tmp; p++) {
    if ((*p == '/' && (o == 0 || tmp[o - 1] == '/'))) continue;
    if (*p == '.' && p[1] == '/' && (o == 0 || tmp[o - 1] == '/')) { p++; continue; }
    tmp[o++] = *p;
  }
  while (o && tmp[o - 1] == '/') o--;
  tmp[o] = 0;
  for (int i = before - 1; i >= 0; i--)
    if (strcmp(a->e[i].path, tmp) == 0) return i;
  return -1;
}

static FmErr tar_open(FmArc *a) {
  TarPriv *tp = (TarPriv *)fm_calloc(1, sizeof *tp);
  a->priv = tp;
  tp->codec = codec_for_fmt(a);
  TarSrc t;
  FmErr e = src_open(&t, a->f, tp->codec);
  if (e) return e;
  u8 *scratch = (u8 *)fm_alloc(ARC_BUF);
  u8 h[512];
  char name[512];
  TarMeta m;
  memset(&m, 0, sizeof m);
  for (;;) {
    long r = src_read(&t, h, 512);
    if (r < 0) { e = t.err; break; }
    if (r == 0) break;                                  /* no end blocks: fine */
    if (r < 512) { e = a->n ? FM_OK : FM_ERR_FORMAT; break; }
    if (all_zero(h)) break;
    if (!tar_sum_ok(h)) {
      if (!a->n) e = FM_ERR_FORMAT;
      else fm_log("tar: bad header at %llu, listing stops", (unsigned long long)(t.pos - 512));
      break;
    }
    u8 type = h[156];
    u64 size = tar_num(h + 124, 12);
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
      char *b = read_blob(&t, size, scratch, &e);
      if (e) break;
      if (b) {
        if (type == 'x') pax_parse(&m, b, (size_t)size);
        else if (type == 'L') { fm_free(m.path); m.path = fm_strdup(b); }
        else if (type == 'K') { fm_free(m.link); m.link = fm_strdup(b); }
        fm_free(b);
      }
      continue;
    }
    if (m.has_size) size = m.size;
    u64 padded = (size + 511) & ~(u64)511;
    bool listed = type == 0 || (type >= '0' && type <= '2') || type == '5' || type == '7';
    if (!listed) {                                     /* devices, fifos, labels, sparse */
      meta_reset(&m);
      e = src_skip(&t, padded, scratch);
      if (e) break;
      continue;
    }
    const char *path = m.path;
    if (!path) {
      size_t o = 0;
      if (memcmp(h + 257, "ustar\0", 6) == 0 && h[345]) {
        for (int i = 0; i < 155 && h[345 + i]; i++) name[o++] = (char)h[345 + i];
        name[o++] = '/';
      }
      for (int i = 0; i < 100 && h[i]; i++) name[o++] = (char)h[i];
      name[o] = 0;
      path = name;
    }
    char lname[101];
    const char *link = m.link;
    if (!link) {
      memcpy(lname, h + 157, 100);
      lname[100] = 0;
      link = lname;
    }
    char *fixed = NULL;
    size_t raw = strlen(path);
    if (!arc_valid_utf8((const u8 *)path, raw)) {   /* old Windows tars: OEM code page */
      fixed = (char *)fm_alloc(3 * raw + 1);
      arc_name_utf8((const u8 *)path, raw, false, fixed);
      path = fixed;
    }
    i64 mtime = m.has_mtime ? m.mtime : (i64)tar_num(h + 136, 12);
    size_t pl = strlen(path);
    bool is_dir = type == '5' || ((type == '0' || type == 0) && pl && path[pl - 1] == '/');
    int hard = type == '1' ? find_entry(a, link, a->n) : -1;
    u64 esize = type == '2' ? strlen(link) : hard >= 0 ? a->e[hard].size : size;
    int idx = arc_add(a, path, esize, size, mtime, is_dir);
    FmArcEntry *ae = arc_at(a, idx);
    ae->mode = (u32)tar_num(h + 100, 8) & 07777;
    ae->is_link = type == '2';
    ae->method = type;
    if (idx >= tp->cap) {
      tp->cap = tp->cap ? tp->cap * 2 : 64;
      tp->t = (TarEnt *)fm_realloc(tp->t, (size_t)tp->cap * sizeof *tp->t);
    }
    TarEnt *te = &tp->t[idx];
    te->data = t.pos;
    te->size = (type == '1' || type == '2' || is_dir) ? 0 : size;
    te->link = type == '2' ? arena_strdup(&a->arena, link) : NULL;
    te->hard = hard;
    fm_free(fixed);
    meta_reset(&m);
    e = src_skip(&t, padded, scratch);
    if (e) {
      if (e == FM_ERR_FORMAT && a->n) {               /* truncated in the last file */
        fm_log("tar: archive is truncated");
        e = FM_OK;
      }
      break;
    }
  }
  meta_reset(&m);
  src_close(&t);
  fm_free(scratch);
  return e;
}

static void tar_close(FmArc *a) {
  TarPriv *tp = (TarPriv *)a->priv;
  if (!tp) return;
  fm_free(tp->t);
  fm_free(tp);
  a->priv = NULL;
}

/* ---- extraction --------------------------------------------------------- */

static FmErr copy_data(TarSrc *t, u64 off, u64 size, FmArcSink *s, u8 *buf, u8 *scratch) {
  FmErr e = src_seek(t, off, scratch);
  while (!e && size) {
    long r = src_read(t, buf, (size_t)FM_MIN(size, (u64)ARC_BUF));
    if (r < 0) return t->err;
    if (r == 0) return FM_ERR_FORMAT;
    size -= (u64)r;
    e = sink_write(s, buf, (size_t)r);
  }
  return e;
}

static FmErr tar_extract(FmArc *a, const u8 *sel, FmArcSink *s) {
  TarPriv *tp = (TarPriv *)a->priv;
  TarSrc t, t2;
  bool t2_open = false;
  FmErr e = src_open(&t, a->f, tp->codec);
  if (e) return e;
  u8 *buf = (u8 *)fm_alloc(ARC_BUF), *scratch = (u8 *)fm_alloc(ARC_BUF);
  for (int i = 0; i < a->n && !e; i++) {
    if (!sel[i]) continue;
    if (sink_remaining(s) <= 0) break;
    bool skip;
    e = sink_begin(s, i, &skip);
    if (e || skip) continue;
    const TarEnt *te = &tp->t[i];
    FmErr ee = FM_OK;
    if (te->link) {
      ee = sink_write(s, te->link, strlen(te->link));
    } else if (te->hard >= 0) {
      const TarEnt *tg = &tp->t[te->hard];
      if (!tp->codec) {                                /* plain: same file, seek */
        ee = copy_data(&t, tg->data, tg->size, s, buf, scratch);
      } else {
        if (!t2_open) {
          ee = src_open(&t2, a->f, tp->codec);
          t2_open = !ee;
        }
        if (!ee) ee = copy_data(&t2, tg->data, tg->size, s, buf, scratch);
      }
    } else {
      ee = copy_data(&t, te->data, te->size, s, buf, scratch);
    }
    e = sink_end(s, ee);
  }
  if (t2_open) src_close(&t2);
  src_close(&t);
  fm_free(buf);
  fm_free(scratch);
  return e;
}

const FmArcBackend g_arc_tar = { tar_open, tar_extract, tar_close };

/* ---- writing ------------------------------------------------------------ */

static void put_octal(u8 *p, int width, u64 v) {     /* width digits + NUL */
  p[width] = 0;
  for (int i = width - 1; i >= 0; i--) {
    p[i] = (u8)('0' + (v & 7));
    v >>= 3;
  }
}

static void put_size(u8 *p, u64 v) {                 /* 12-byte field */
  if (v < ((u64)1 << 33)) {
    put_octal(p, 11, v);
  } else {                                           /* GNU base-256 */
    memset(p, 0, 12);
    p[0] = 0x80;
    for (int i = 11; i >= 4; i--) { p[i] = (u8)v; v >>= 8; }
  }
}

static void finish_header(u8 *h) {
  memcpy(h + 257, "ustar\0" "00", 8);
  memset(h + 148, ' ', 8);
  u32 sum = 0;
  for (int i = 0; i < 512; i++) sum += h[i];
  put_octal(h + 148, 6, sum);
  h[155] = ' ';
}

/* Copies at most cap bytes of s without cutting a UTF-8 sequence. */
static void put_name(u8 *dst, size_t cap, const char *s) {
  size_t n = strlen(s);
  if (n > cap) {
    n = cap;
    while (n && ((u8)s[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, s, n);
}

static int dec_len(u64 v) {
  int n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

/* Appends one "len key=value\n" record. */
static size_t pax_rec(char *out, const char *key, const char *val) {
  size_t body = 1 + strlen(key) + 1 + strlen(val) + 1;   /* " key=value\n" */
  size_t len = body + (size_t)dec_len(body);
  if ((size_t)dec_len(len) != (size_t)dec_len(body)) len++;
  char num[24];
  int k = 0;
  u64 v = len;
  char rev[24];
  do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (int i = 0; i < k; i++) num[i] = rev[k - 1 - i];
  num[k] = 0;
  size_t o = 0;
  memcpy(out + o, num, (size_t)k); o += (size_t)k;
  out[o++] = ' ';
  memcpy(out + o, key, strlen(key)); o += strlen(key);
  out[o++] = '=';
  memcpy(out + o, val, strlen(val)); o += strlen(val);
  out[o++] = '\n';
  return o;
}

static void u64_dec(char *out, i64 sv) {
  char rev[24];
  int k = 0;
  bool neg = sv < 0;
  u64 v = neg ? (u64)(-sv) : (u64)sv;
  do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v);
  int o = 0;
  if (neg) out[o++] = '-';
  while (k) out[o++] = rev[--k];
  out[o] = 0;
}

static FmErr tar_put(FmOut *o, const void *p, size_t n) { return out_write(o, p, n); }

static FmErr tar_pad(FmOut *o, u64 size, const u8 *zeros) {
  size_t pad = (size_t)((512 - (size & 511)) & 511);
  return pad ? tar_put(o, zeros, pad) : FM_OK;
}

static FmErr write_entry(FmOut *o, FmArcWriteCtx *w, const FmArcSrc *src, u8 *buf,
                         const u8 *zeros) {
  size_t nl = strlen(src->name);
  char *name = (char *)fm_alloc(nl + 2);
  memcpy(name, src->name, nl);
  if (src->is_dir) name[nl++] = '/';
  name[nl] = 0;
  bool ascii = true;
  for (size_t i = 0; i < nl; i++) if ((u8)name[i] >= 0x80) ascii = false;
  u64 size = src->is_dir ? 0 : src->size;
  i64 mt = src->mtime;
  bool long_name = nl > 100 || !ascii;
  bool big = size >= ((u64)1 << 33);
  bool odd_time = mt < 0 || mt >= ((i64)1 << 33);
  u8 h[512];
  FmErr e = FM_OK;
  if (long_name || big || odd_time) {
    char *pax = (char *)fm_alloc(nl + 128);
    size_t pl = 0;
    char num[32];
    if (long_name) pl += pax_rec(pax + pl, "path", name);
    if (big) { u64_dec(num, (i64)size); pl += pax_rec(pax + pl, "size", num); }
    if (odd_time) { u64_dec(num, mt); pl += pax_rec(pax + pl, "mtime", num); }
    memset(h, 0, 512);
    memcpy(h, "PaxHeader/", 10);
    put_name(h + 10, 90, fm_path_base(src->name));
    put_octal(h + 100, 7, 0644);
    put_octal(h + 108, 7, 0);
    put_octal(h + 116, 7, 0);
    put_size(h + 124, pl);
    put_octal(h + 136, 11, odd_time ? 0 : (u64)mt);
    h[156] = 'x';
    finish_header(h);
    e = tar_put(o, h, 512);
    if (!e) e = tar_put(o, pax, pl);
    if (!e) e = tar_pad(o, pl, zeros);
    fm_free(pax);
  }
  if (!e) {
    memset(h, 0, 512);
    put_name(h, 100, name);
    put_octal(h + 100, 7, src->mode & 07777);
    put_octal(h + 108, 7, 0);
    put_octal(h + 116, 7, 0);
    put_size(h + 124, size);
    put_octal(h + 136, 11, odd_time ? 0 : (u64)mt);
    h[156] = src->is_dir ? '5' : '0';
    finish_header(h);
    e = tar_put(o, h, 512);
  }
  fm_free(name);
  if (e || src->is_dir) return e;
  FILE *in = fm_fopen(src->abs, "rb");
  if (!in) return FM_ERR_ACCESS;
  u64 left = size;
  while (!e && left) {
    size_t k = fread(buf, 1, (size_t)FM_MIN(left, (u64)ARC_BUF), in);
    if (!k) {
      e = FM_ERR_IO;                  /* shorter than when listed: tar needs the exact size */
      break;
    }
    e = tar_put(o, buf, k);
    left -= k;
    if (!e && !arc_wprogress(w, k)) e = FM_ERR_CANCEL;
  }
  fclose(in);
  if (!e) e = tar_pad(o, size, zeros);
  return e;
}

FmErr tar_write(FILE *out, FmArcWriteCtx *w) {
  FmCodec c;
  switch (w->o->fmt) {
    case ARC_TAR_GZ: c = CODEC_GZIP; break;
    case ARC_TAR_BZ2: c = CODEC_BZIP2; break;
    case ARC_TAR_XZ: c = CODEC_XZ; break;
    case ARC_TAR_ZST: c = CODEC_ZSTD; break;
    default: c = CODEC_STORE; break;
  }
  FmErr e;
  FmOut *o = out_open(out, c, w->o->level, &e);
  if (!o) return e;
  u8 *buf = (u8 *)fm_alloc(ARC_BUF);
  u8 *zeros = (u8 *)fm_calloc(1, 1024);
  for (int i = 0; i < w->n && !e; i++) {
    if (w->cb && w->cb->entry) w->cb->entry(w->cb->ud, w->src[i].name);
    e = write_entry(o, w, &w->src[i], buf, zeros);
  }
  if (!e) e = tar_put(o, zeros, 1024);
  FmErr ce = out_close(o);
  if (!e) e = ce;
  fm_free(buf);
  fm_free(zeros);
  return e;
}
