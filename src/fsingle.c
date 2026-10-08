/* fsingle.c -- one compressed file: .gz, .bz2, .xz, .zst, .lzma.
**
** Shown as an archive with a single entry, so the panel can browse into it
** and extract it like anything else.
**
** Design decisions:
**   - The entry name is gzip's FNAME when present (only its last path
**     part, and never "." or ".."), else the file name without its
**     compression extension.
**   - Sizes are read without decompressing: gzip's ISIZE (the size modulo
**     4 GB, so only used for files under 4 GB), the xz index (walked
**     backwards over concatenated streams), the zstd frame header and the
**     .lzma header. bzip2 has none: 0, unknown.
**   - Writing streams through fstream. gzip framing is written here so
**     the header carries the name and mtime like gzip does; .lzma is
**     encoded straight from the source file (its header wants the size
**     first, and the LZMA SDK encoder pulls its input).
*/
#include "farc_int.h"
#include "farc_ext.h"
#include "fcrypt.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

static FmCodec codec_of(FmArcFmt f) {
  switch (f) {
    case ARC_GZ: return CODEC_GZIP;
    case ARC_BZ2: return CODEC_BZIP2;
    case ARC_XZ: return CODEC_XZ;
    case ARC_ZST: return CODEC_ZSTD;
    default: return CODEC_LZMA;
  }
}

static u32 le32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool rd_at(FILE *f, i64 off, void *buf, size_t n) {
  return fm_fseek64(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* ---- sizes and names ---------------------------------------------------- */

static bool get_varint(FILE *f, i64 *left, u64 *v) {
  *v = 0;
  for (int i = 0; i < 9; i++) {
    if (*left <= 0) return false;
    int c = fgetc(f);
    if (c == EOF) return false;
    (*left)--;
    *v |= (u64)(c & 0x7F) << (7 * i);
    if (!(c & 0x80)) return true;
  }
  return false;
}

/* Sum of the uncompressed sizes in every xz stream's index; 0 if unsure. */
static u64 xz_size(FILE *f, i64 fsize) {
  i64 pos = fsize;
  u64 total = 0;
  while (pos > 0) {
    u8 b[12];
    while (pos >= 4 && rd_at(f, pos - 4, b, 4) && !le32(b)) pos -= 4;   /* stream padding */
    if (pos < 24 || !rd_at(f, pos - 12, b, 12) || b[10] != 'Y' || b[11] != 'Z') return 0;
    i64 isize = ((i64)le32(b + 4) + 1) * 4;
    i64 idx = pos - 12 - isize;
    if (idx < 12 || fm_fseek64(f, idx, SEEK_SET) != 0 || fgetc(f) != 0) return 0;
    i64 left = isize - 1;
    u64 count, padded = 0;
    if (!get_varint(f, &left, &count) || count > (u64)isize) return 0;
    for (u64 i = 0; i < count; i++) {
      u64 unpadded, size;
      if (!get_varint(f, &left, &unpadded) || !get_varint(f, &left, &size)) return 0;
      padded += (unpadded + 3) & ~(u64)3;
      total += size;
    }
    if (padded > (u64)idx) return 0;
    pos = idx - (i64)padded - 12;
    if (pos < 0) return 0;
  }
  return total;
}

static const char *const kExts[] = { ".gz", ".z", ".bz2", ".bz", ".xz", ".zst", ".lzma" };

static FmErr single_open(FmArc *a) {
  char name[FM_PATH_MAX];
  const char *base = fm_path_base(a->path);
  fm_strlcpy(name, base, sizeof name);
  bool cut = false;
  for (int i = 0; i < FM_COUNT(kExts) && !cut; i++) {
    size_t n = strlen(name), k = strlen(kExts[i]);
    if (n > k && fm_ends_with_i(name, kExts[i])) {
      name[n - k] = 0;
      cut = true;
    }
  }
  if (!cut) fm_strlcat(name, ".out", sizeof name);
  u64 size = 0;
  i64 mtime = 0;
  u8 h[32];
  size_t hn = 0;
  if (fm_fseek64(a->f, 0, SEEK_SET) == 0) hn = fread(h, 1, sizeof h, a->f);
  switch (a->fmt) {
    case ARC_GZ: {
      if (hn < 10) return FM_ERR_FORMAT;
      mtime = le32(h + 4);
      if (h[3] & 8) {                      /* FNAME */
        i64 off = 10;
        if (h[3] & 4) off += 2 + (h[10] | (h[11] << 8));
        char fn[FM_PATH_MAX];
        size_t n = 0;
        if (fm_fseek64(a->f, off, SEEK_SET) == 0) {
          int c;
          while (n + 1 < sizeof fn && (c = fgetc(a->f)) > 0) fn[n++] = (char)c;
        }
        fn[n] = 0;
        const char *b = fn;
        for (const char *p = fn; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
        if (b[0] && strcmp(b, ".") != 0 && strcmp(b, "..") != 0 && !(b[0] && b[1] == ':'))
          fm_strlcpy(name, b, sizeof name);
      }
      u8 t[4];
      if (a->file_size >= 18 && a->file_size < ((i64)1 << 32) && rd_at(a->f, a->file_size - 4, t, 4))
        size = le32(t);
      break;
    }
    case ARC_XZ:
      size = xz_size(a->f, a->file_size);
      break;
    case ARC_ZST: {
      unsigned long long cs = ZSTD_getFrameContentSize(h, hn);
      if (cs != ZSTD_CONTENTSIZE_UNKNOWN && cs != ZSTD_CONTENTSIZE_ERROR) size = cs;
      break;
    }
    case ARC_LZMA: {
      if (hn < 13) return FM_ERR_FORMAT;
      u64 s = (u64)le32(h + 5) | ((u64)le32(h + 9) << 32);
      if (s != (u64)-1) size = s;
      break;
    }
    default:
      break;
  }
  if (!mtime) {
    FmStat st;
    if (plat_stat(a->path, &st)) mtime = st.mtime;
  }
  int idx = arc_add(a, name, size, (u64)a->file_size, mtime, false);
  arc_at(a, idx)->method = (u8)codec_of(a->fmt);
  return FM_OK;
}

static FmErr single_extract(FmArc *a, const u8 *sel, FmArcSink *s) {
  if (!a->n || !sel[0]) return FM_OK;
  bool skip;
  FmErr e = sink_begin(s, 0, &skip);
  if (e || skip) return e;
  if (fm_fseek64(a->f, 0, SEEK_SET) != 0) return sink_end(s, FM_ERR_IO);
  FmIn *in = in_open(a->f, codec_of(a->fmt), -1, &e);
  if (!in) return sink_end(s, e);
  u8 *buf = (u8 *)fm_alloc(ARC_BUF);
  for (;;) {
    long n = in_read(in, buf, ARC_BUF);
    if (n < 0) { e = in_error(in); break; }
    if (n == 0) break;
    e = sink_write(s, buf, (size_t)n);
    if (e) break;
  }
  in_close(in);
  fm_free(buf);
  return sink_end(s, e);
}

static void single_close(FmArc *a) { FM_UNUSED(a); }

const FmArcBackend g_arc_single = { single_open, single_extract, single_close };

/* ---- writing ------------------------------------------------------------ */

static FmErr fwr(void *ud, const void *buf, size_t n) {
  return (n && fwrite(buf, 1, n, (FILE *)ud) != n) ? FM_ERR_FULL : FM_OK;
}

static bool lz_progress(void *ud, u64 done) {
  FmArcWriteCtx *w = (FmArcWriteCtx *)ud;
  return arc_wprogress(w, done - w->done);
}

FmErr single_write(FILE *out, FmArcWriteCtx *w) {
  if (w->n != 1 || w->src[0].is_dir) return FM_ERR_UNSUPPORTED;
  const FmArcSrc *src = &w->src[0];
  FmArcFmt fmt = w->o->fmt;
  int level = w->o->level;
  if (w->cb && w->cb->entry) w->cb->entry(w->cb->ud, src->name);
  FILE *in = fm_fopen(src->abs, "rb");
  if (!in) return FM_ERR_ACCESS;
  FmErr e = FM_OK;
  if (fmt == ARC_LZMA) {
    e = lzma_alone_encode(in, src->size, level, fwr, out, lz_progress, w);
    fclose(in);
    return e;
  }
  if (fmt == ARC_GZ) {
    /* gzip header with the original name and mtime */
    u8 h[10] = { 0x1F, 0x8B, 8, 8, 0, 0, 0, 0, 0, 0xFF };
    u32 mt = (src->mtime > 0 && src->mtime < ((i64)1 << 32)) ? (u32)src->mtime : 0;
    for (int i = 0; i < 4; i++) h[4 + i] = (u8)(mt >> (8 * i));
    h[8] = level == 9 ? 2 : (level == 1 ? 4 : 0);
#ifdef FM_WIN
    h[9] = 11;
#else
    h[9] = 3;
#endif
    const char *b = fm_path_base(src->name);
    e = fwr(out, h, 10);
    if (!e) e = fwr(out, b, strlen(b) + 1);
  }
  FmOut *o = e ? NULL : out_open(out, fmt == ARC_GZ ? CODEC_DEFLATE : codec_of(fmt), level, &e);
  u8 *buf = (u8 *)fm_alloc(ARC_BUF);
  u32 crc = 0, isz = 0;
  while (o && !e) {
    size_t k = fread(buf, 1, ARC_BUF, in);
    if (!k) {
      if (ferror(in)) e = FM_ERR_IO;
      break;
    }
    crc = crc32_update(crc, buf, k);
    isz += (u32)k;
    e = out_write(o, buf, k);
    if (!e && !arc_wprogress(w, k)) e = FM_ERR_CANCEL;
  }
  if (o) {
    FmErr ce = out_close(o);
    if (!e) e = ce;
  }
  if (!e && fmt == ARC_GZ) {
    u8 t[8];
    for (int i = 0; i < 4; i++) {
      t[i] = (u8)(crc >> (8 * i));
      t[4 + i] = (u8)(isz >> (8 * i));
    }
    e = fwr(out, t, 8);
  }
  fm_free(buf);
  fclose(in);
  return e;
}
