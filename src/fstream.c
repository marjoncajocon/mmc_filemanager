/* fstream.c -- streaming (de)compressors: store, deflate, gzip, bzip2, xz,
** lzma and zstd behind one pull/push interface (fstream.h, farc_ext.h).
**
** Design decisions:
**   - One 64 KB input buffer per reader and one 64 KB output buffer per
**     writer, whatever the data size. Decoders write straight into the
**     caller's buffer, except deflate, which needs its 32 KB window ring.
**   - in_read fills the whole request unless the stream ends, so callers
**     (tar headers) can ask for exactly 512 bytes.
**   - gzip framing is ours (miniz only does raw deflate): multi-member
**     files are read through, the CRC32 and size of each member are
**     checked, and trailing zero padding is ignored like gzip does.
**   - The LZMA SDK encoders pull their input, but out_write pushes. xz is
**     therefore written as a multi-block xz stream (what `xz -T` makes):
**     input collects in a block buffer of twice the dictionary, each full
**     block is encoded from memory (no second copy of the window) and the
**     index is built here. Plain .lzma has no blocks, so pushed data goes
**     to a temporary file that is encoded at out_close; fsingle.c skips
**     that by encoding straight from the source (lzma_alone_encode).
**   - Codec memory comes from one allocator that refuses absurd sizes and
**     returns NULL instead of aborting, so a hostile header (a 4 GB LZMA
**     dictionary) gives FM_ERR_NOMEM instead of killing the app.
**   - Corrupt and truncated data both report FM_ERR_FORMAT; checksum
**     mismatches report FM_ERR_CRC.
*/
#include "farc_ext.h"
#include "fcrypt.h"
#include "fplat.h"
#include <errno.h>

#ifdef FM_WIN
#  include "fwin.h"
#endif

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include "bzlib.h"
#include "7zCrc.h"
#include "XzCrc64.h"
#include "Xz.h"
#include "LzmaDec.h"
#include "LzmaEnc.h"
#include "Lzma2Enc.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zstd_errors.h"

#define SBUF (64 * 1024)
#define DICT TINFL_LZ_DICT_SIZE

/* ---- codec memory ------------------------------------------------------- */

/* Small blocks go through fm_alloc (counted); big ones through malloc so a
** failure is an error, not an abort. A 16-byte tag says which. */
#define CM_HDR 16
#define CM_BIG ((size_t)32 << 20)

static void *cm_alloc(size_t n) {
  if (n > (size_t)-1 - CM_HDR) return NULL;
  if (sizeof(void *) < 8 && n > ((size_t)1 << 30)) return NULL;
  u8 *p;
  if (n >= CM_BIG) {
    p = (u8 *)malloc(n + CM_HDR);
    if (!p) return NULL;
    p[0] = 1;
  } else {
    p = (u8 *)fm_alloc(n + CM_HDR);
    p[0] = 0;
  }
  return p + CM_HDR;
}

static void cm_free(void *a) {
  if (!a) return;
  u8 *p = (u8 *)a - CM_HDR;
  if (p[0]) free(p);
  else fm_free(p);
}

static void *sz_alloc(ISzAllocPtr p, size_t n) { FM_UNUSED(p); return cm_alloc(n); }
static void sz_free(ISzAllocPtr p, void *a) { FM_UNUSED(p); cm_free(a); }
static const ISzAlloc g_sz = { sz_alloc, sz_free };

static void *bz_alloc(void *o, int n, int m) {
  FM_UNUSED(o);
  if (n < 0 || m < 0) return NULL;
  return cm_alloc((size_t)n * (size_t)m);
}
static void bz_free(void *o, void *a) { FM_UNUSED(o); cm_free(a); }

static void *zs_alloc(void *o, size_t n) { FM_UNUSED(o); return cm_alloc(n); }
static void zs_free(void *o, void *a) { FM_UNUSED(o); cm_free(a); }
static const ZSTD_customMem g_zmem = { zs_alloc, zs_free, NULL };

static volatile int g_sdk_ready;

void stream_sdk_init(void) {
#if defined(__GNUC__) || defined(__clang__)
  if (__atomic_load_n(&g_sdk_ready, __ATOMIC_ACQUIRE)) return;
  CrcGenerateTable();
  Crc64GenerateTable();
  __atomic_store_n(&g_sdk_ready, 1, __ATOMIC_RELEASE);
#else
  if (g_sdk_ready) return;
  CrcGenerateTable();
  Crc64GenerateTable();
  g_sdk_ready = 1;
#endif
}

static FmErr sres_err(SRes r) {
  switch (r) {
    case SZ_OK: return FM_OK;
    case SZ_ERROR_MEM: return FM_ERR_NOMEM;
    case SZ_ERROR_CRC: return FM_ERR_CRC;
    case SZ_ERROR_UNSUPPORTED: return FM_ERR_UNSUPPORTED;
    case SZ_ERROR_READ: case SZ_ERROR_WRITE: return FM_ERR_IO;
    case SZ_ERROR_PROGRESS: return FM_ERR_CANCEL;
    default: return FM_ERR_FORMAT;
  }
}

static FmErr write_err(void) {
#ifdef ENOSPC
  if (errno == ENOSPC) return FM_ERR_FULL;
#endif
  return FM_ERR_IO;
}

/* ---- reader ------------------------------------------------------------- */

typedef struct FileSrc { FILE *f; i64 left; } FileSrc;   /* in_open: -1 = to EOF */

struct FmIn {
  FmCodec c;
  FmReadFn rd;
  void *ud;
  FileSrc fsrc;
  u8 *ib;
  size_t ipos, ilen;
  bool ieof, end;
  u64 fetched;
  FmErr err;
  /* deflate / gzip */
  tinfl_decompressor *inf;
  u8 *dict;
  size_t dofs, pstart, plen;
  bool member_done;
  u32 gcrc, gsize;
  /* bzip2 */
  bz_stream bz;
  bool bz_on, bz_ended;
  /* xz */
  CXzUnpacker *xz;
  /* lzma */
  CLzmaDec lz;
  bool lz_on, lz_known;
  u64 lz_left;
  /* zstd */
  ZSTD_DStream *zs;
  size_t zret;
};

static long file_read(void *ud, void *buf, size_t n) {
  FileSrc *s = (FileSrc *)ud;
  if (s->left == 0) return 0;
  if (s->left > 0 && (u64)n > (u64)s->left) n = (size_t)s->left;
  size_t r = fread(buf, 1, n, s->f);
  if (r == 0 && ferror(s->f)) return -1;
  if (s->left > 0) s->left -= (i64)r;
  return (long)r;
}

static bool in_fill(FmIn *s) {
  if (s->ieof) return false;
  long n = s->rd(s->ud, s->ib, SBUF);
  if (n <= 0) {
    if (n < 0 && !s->err) s->err = FM_ERR_IO;
    s->ieof = true;
    s->ipos = s->ilen = 0;
    return false;
  }
  s->ipos = 0;
  s->ilen = (size_t)n;
  s->fetched += (u64)n;
  return true;
}

static int in_byte(FmIn *s) {
  if (s->ipos == s->ilen && !in_fill(s)) return -1;
  return s->ib[s->ipos++];
}

static bool in_skip(FmIn *s, size_t n) {
  while (n--) if (in_byte(s) < 0) return false;
  return true;
}

/* gzip member header; first=false allows a clean end of input. */
static FmErr gz_header(FmIn *s, bool first) {
  int a = in_byte(s);
  if (a < 0) {
    if (first) return FM_ERR_FORMAT;
    s->end = true;
    return FM_OK;
  }
  int b = in_byte(s);
  if (a != 0x1F || b != 0x8B) {
    if (first) return FM_ERR_FORMAT;
    s->end = true;              /* trailing padding or garbage, ignored like gzip */
    return FM_OK;
  }
  int cm = in_byte(s), flg = in_byte(s);
  if (cm != 8 || flg < 0 || (flg & 0xE0)) return FM_ERR_FORMAT;
  if (!in_skip(s, 6)) return FM_ERR_FORMAT;     /* mtime, xfl, os */
  if (flg & 4) {                                /* FEXTRA */
    int l0 = in_byte(s), l1 = in_byte(s);
    if (l1 < 0 || !in_skip(s, (size_t)(l0 | (l1 << 8)))) return FM_ERR_FORMAT;
  }
  for (int bit = 8; bit <= 16; bit <<= 1) {     /* FNAME, FCOMMENT */
    if (!(flg & bit)) continue;
    int c;
    while ((c = in_byte(s)) > 0) {}
    if (c < 0) return FM_ERR_FORMAT;
  }
  if ((flg & 2) && !in_skip(s, 2)) return FM_ERR_FORMAT;   /* FHCRC */
  tinfl_init(s->inf);
  s->member_done = false;
  s->gcrc = 0;
  s->gsize = 0;
  return FM_OK;
}

static long step_store(FmIn *s, u8 *out, size_t n) {
  if (s->ipos < s->ilen) {
    size_t k = FM_MIN(n, s->ilen - s->ipos);
    memcpy(out, s->ib + s->ipos, k);
    s->ipos += k;
    return (long)k;
  }
  if (s->ieof) { s->end = true; return 0; }
  if (n >= SBUF) {                 /* big reads skip the buffer */
    long r = s->rd(s->ud, out, FM_MIN(n, (size_t)0x40000000));
    if (r < 0) { s->err = FM_ERR_IO; return 0; }
    if (r == 0) { s->ieof = s->end = true; return 0; }
    s->fetched += (u64)r;
    return r;
  }
  in_fill(s);
  return 0;
}

static long step_inflate(FmIn *s, u8 *out, size_t n) {
  if (s->plen) {
    size_t k = FM_MIN(n, s->plen);
    memcpy(out, s->dict + s->pstart, k);
    s->pstart += k;
    s->plen -= k;
    if (s->c == CODEC_GZIP) {
      s->gcrc = crc32_update(s->gcrc, out, k);
      s->gsize += (u32)k;
    }
    return (long)k;
  }
  if (s->member_done) {
    if (s->c != CODEC_GZIP) { s->end = true; return 0; }
    u8 t[8];
    for (int i = 0; i < 8; i++) {
      int c = in_byte(s);
      if (c < 0) { s->err = FM_ERR_FORMAT; return 0; }
      t[i] = (u8)c;
    }
    u32 crc = (u32)t[0] | ((u32)t[1] << 8) | ((u32)t[2] << 16) | ((u32)t[3] << 24);
    u32 isz = (u32)t[4] | ((u32)t[5] << 8) | ((u32)t[6] << 16) | ((u32)t[7] << 24);
    if (crc != s->gcrc || isz != s->gsize) { s->err = FM_ERR_CRC; return 0; }
    FmErr e = gz_header(s, false);
    if (e) s->err = e;
    return 0;
  }
  if (s->ipos == s->ilen) in_fill(s);
  size_t in_n = s->ilen - s->ipos, out_n = DICT - s->dofs;
  mz_uint32 flags = s->ieof ? 0 : TINFL_FLAG_HAS_MORE_INPUT;
  tinfl_status st = tinfl_decompress(s->inf, s->ib + s->ipos, &in_n, s->dict, s->dict + s->dofs,
                                     &out_n, flags);
  s->ipos += in_n;
  s->pstart = s->dofs;
  s->plen = out_n;
  s->dofs = (s->dofs + out_n) & (DICT - 1);
  if (st == TINFL_STATUS_DONE) s->member_done = true;
  else if (st < 0) s->err = FM_ERR_FORMAT;
  else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && s->ieof && !in_n && !out_n) s->err = FM_ERR_FORMAT;
  return 0;
}

static long step_bzip2(FmIn *s, u8 *out, size_t n) {
  if (s->bz_ended) {
    /* pbzip2 and friends concatenate streams: continue with the next one */
    if (s->ipos == s->ilen && !in_fill(s)) { s->end = true; return 0; }
    if (s->ib[s->ipos] != 'B') { s->end = true; return 0; }
    BZ2_bzDecompressEnd(&s->bz);
    s->bz_on = false;
    if (BZ2_bzDecompressInit(&s->bz, 0, 0) != BZ_OK) { s->err = FM_ERR_NOMEM; return 0; }
    s->bz_on = true;
    s->bz_ended = false;
  }
  if (s->ipos == s->ilen) in_fill(s);
  unsigned avail = (unsigned)(s->ilen - s->ipos);
  unsigned want = (unsigned)FM_MIN(n, (size_t)0x40000000);
  s->bz.next_in = (char *)s->ib + s->ipos;
  s->bz.avail_in = avail;
  s->bz.next_out = (char *)out;
  s->bz.avail_out = want;
  int r = BZ2_bzDecompress(&s->bz);
  size_t used = avail - s->bz.avail_in, made = want - s->bz.avail_out;
  s->ipos += used;
  if (r == BZ_STREAM_END) s->bz_ended = true;
  else if (r == BZ_MEM_ERROR) s->err = FM_ERR_NOMEM;
  else if (r != BZ_OK) s->err = FM_ERR_FORMAT;
  else if (!used && !made && s->ieof) s->err = FM_ERR_FORMAT;
  return (long)made;
}

static long step_xz(FmIn *s, u8 *out, size_t n) {
  if (s->ipos == s->ilen) in_fill(s);
  SizeT in_n = s->ilen - s->ipos, out_n = n;
  ECoderStatus status;
  bool was_finished = XzUnpacker_IsStreamWasFinished(s->xz) != 0;
  SRes r = XzUnpacker_Code(s->xz, out, &out_n, s->ib + s->ipos, &in_n, s->ieof, CODER_FINISH_ANY,
                           &status);
  s->ipos += in_n;
  if (r == SZ_ERROR_NO_ARCHIVE && was_finished) { s->end = true; return (long)out_n; }
  if (r != SZ_OK) { s->err = sres_err(r); return (long)out_n; }
  if (!in_n && !out_n && s->ieof) {
    if (XzUnpacker_IsStreamWasFinished(s->xz)) s->end = true;
    else s->err = FM_ERR_FORMAT;
  }
  return (long)out_n;
}

static long step_lzma(FmIn *s, u8 *out, size_t n) {
  if (s->lz_known && !s->lz_left) { s->end = true; return 0; }
  if (s->ipos == s->ilen) in_fill(s);
  SizeT out_n = n, in_n = s->ilen - s->ipos;
  if (s->lz_known && (u64)out_n > s->lz_left) out_n = (SizeT)s->lz_left;
  ELzmaStatus status;
  SRes r = LzmaDec_DecodeToBuf(&s->lz, out, &out_n, s->ib + s->ipos, &in_n, LZMA_FINISH_ANY,
                               &status);
  s->ipos += in_n;
  if (s->lz_known) s->lz_left -= out_n;
  if (r != SZ_OK) { s->err = FM_ERR_FORMAT; return (long)out_n; }
  if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
    if (s->lz_known && s->lz_left) s->err = FM_ERR_FORMAT;
    else s->end = true;
  } else if (!in_n && !out_n && s->ieof) {
    if (s->c == CODEC_ZIP_LZMA) s->end = true;
    else s->err = FM_ERR_FORMAT;
  }
  return (long)out_n;
}

static long step_zstd(FmIn *s, u8 *out, size_t n) {
  if (s->ipos == s->ilen) in_fill(s);
  ZSTD_inBuffer in = { s->ib + s->ipos, s->ilen - s->ipos, 0 };
  ZSTD_outBuffer ob = { out, n, 0 };
  size_t r = ZSTD_decompressStream(s->zs, &ob, &in);
  s->ipos += in.pos;
  if (ZSTD_isError(r)) {
    s->err = ZSTD_getErrorCode(r) == ZSTD_error_memory_allocation ? FM_ERR_NOMEM
           : ZSTD_getErrorCode(r) == ZSTD_error_checksum_wrong ? FM_ERR_CRC : FM_ERR_FORMAT;
    return (long)ob.pos;
  }
  /* At the end of input, an idle call asks for the next frame's header;
  ** what counts is whether the previous call finished a frame. */
  if (!in.pos && !ob.pos && s->ieof) {
    if (s->zret == 0) s->end = true;
    else s->err = FM_ERR_FORMAT;
    return 0;
  }
  s->zret = r;
  return (long)ob.pos;
}

/* Reads the 13-byte .lzma header, or zip's 4 + 5 bytes, and sets up LzmaDec. */
static FmErr lzma_start(FmIn *s) {
  u8 h[13];
  int hl = s->c == CODEC_ZIP_LZMA ? 9 : 13;
  for (int i = 0; i < hl; i++) {
    int c = in_byte(s);
    if (c < 0) return FM_ERR_FORMAT;
    h[i] = (u8)c;
  }
  u8 *props = h;
  s->lz_known = false;
  if (s->c == CODEC_ZIP_LZMA) {
    if (h[2] != 5 || h[3] != 0) return FM_ERR_FORMAT;
    props = h + 4;
  } else {
    u64 sz = 0;
    for (int i = 0; i < 8; i++) sz |= (u64)h[5 + i] << (8 * i);
    if (sz != (u64)-1) {
      s->lz_known = true;
      s->lz_left = sz;
      /* A dictionary larger than the whole output is never used. */
      u32 dict = (u32)props[1] | ((u32)props[2] << 8) | ((u32)props[3] << 16) |
                 ((u32)props[4] << 24);
      if ((u64)dict > sz) {
        u32 d = sz < 4096 ? 4096 : (u32)sz;
        props[1] = (u8)d; props[2] = (u8)(d >> 8); props[3] = (u8)(d >> 16); props[4] = (u8)(d >> 24);
      }
    }
  }
  if (props[0] >= 9 * 5 * 5) return FM_ERR_FORMAT;
  LzmaDec_Construct(&s->lz);
  SRes r = LzmaDec_Allocate(&s->lz, props, LZMA_PROPS_SIZE, &g_sz);
  if (r != SZ_OK) return r == SZ_ERROR_MEM ? FM_ERR_NOMEM : FM_ERR_FORMAT;
  s->lz_on = true;
  LzmaDec_Init(&s->lz);
  return FM_OK;
}

FmIn *in_open_cb(FmCodec c, FmReadFn rd, void *ud, FmErr *err) {
  FmIn *s = (FmIn *)fm_calloc(1, sizeof *s);
  s->c = c;
  s->rd = rd;
  s->ud = ud;
  s->ib = (u8 *)fm_alloc(SBUF);
  FmErr e = FM_OK;
  switch ((int)c) {
    case CODEC_STORE:
      break;
    case CODEC_DEFLATE:
    case CODEC_GZIP:
      s->inf = (tinfl_decompressor *)fm_alloc(sizeof *s->inf);
      s->dict = (u8 *)fm_alloc(DICT);
      tinfl_init(s->inf);
      if (c == CODEC_GZIP) e = gz_header(s, true);
      break;
    case CODEC_BZIP2:
      s->bz.bzalloc = bz_alloc;
      s->bz.bzfree = bz_free;
      if (BZ2_bzDecompressInit(&s->bz, 0, 0) != BZ_OK) e = FM_ERR_NOMEM;
      else s->bz_on = true;
      break;
    case CODEC_XZ:
      stream_sdk_init();
      s->xz = (CXzUnpacker *)fm_alloc(sizeof *s->xz);
      XzUnpacker_Construct(s->xz, &g_sz);
      XzUnpacker_Init(s->xz);
      break;
    case CODEC_LZMA:
    case CODEC_ZIP_LZMA:
      e = lzma_start(s);
      break;
    case CODEC_ZSTD:
      s->zs = ZSTD_createDStream_advanced(g_zmem);
      s->zret = 1;
      if (!s->zs) e = FM_ERR_NOMEM;
      break;
    default:
      e = FM_ERR_UNSUPPORTED;
  }
  if (!e && s->err) e = s->err;
  if (e) {
    in_close(s);
    s = NULL;
  }
  if (err) *err = e;
  return s;
}

FmIn *in_open(FILE *f, FmCodec c, i64 limit, FmErr *err) {
  /* Header codecs read during in_open_cb, before the reader exists, so
  ** they read through a stand-in that is then moved into the reader. */
  FileSrc src = { f, limit < 0 ? -1 : limit };
  FmIn *s = in_open_cb(c, file_read, &src, err);
  if (!s) return NULL;
  s->fsrc = src;
  s->ud = &s->fsrc;
  return s;
}

long in_read(FmIn *s, void *buf, size_t n) {
  if (!s) return -1;
  u8 *o = (u8 *)buf;
  size_t got = 0;
  int stall = 0;
  while (got < n && !s->end && !s->err) {
    u64 f0 = s->fetched;
    size_t p0 = s->ipos;
    long k;
    switch ((int)s->c) {
      case CODEC_STORE: k = step_store(s, o + got, n - got); break;
      case CODEC_DEFLATE: case CODEC_GZIP: k = step_inflate(s, o + got, n - got); break;
      case CODEC_BZIP2: k = step_bzip2(s, o + got, n - got); break;
      case CODEC_XZ: k = step_xz(s, o + got, n - got); break;
      case CODEC_LZMA: case CODEC_ZIP_LZMA: k = step_lzma(s, o + got, n - got); break;
      case CODEC_ZSTD: k = step_zstd(s, o + got, n - got); break;
      default: s->err = FM_ERR_UNSUPPORTED; k = 0;
    }
    got += (size_t)k;
    /* A decoder that neither reads nor writes is fed garbage it cannot use. */
    if (!k && f0 == s->fetched && p0 == s->ipos && ++stall > 4) s->err = FM_ERR_FORMAT;
    else if (k || f0 != s->fetched || p0 != s->ipos) stall = 0;
  }
  if (!got && s->err) return -1;
  return (long)got;
}

FmErr in_error(const FmIn *s) { return s ? s->err : FM_ERR_IO; }

u64 in_consumed(const FmIn *s) { return s ? s->fetched - (s->ilen - s->ipos) : 0; }

void in_close(FmIn *s) {
  if (!s) return;
  if (s->bz_on) BZ2_bzDecompressEnd(&s->bz);
  if (s->xz) { XzUnpacker_Free(s->xz); fm_free(s->xz); }
  if (s->lz_on) LzmaDec_Free(&s->lz, &g_sz);
  if (s->zs) ZSTD_freeDStream(s->zs);
  fm_free(s->inf);
  fm_free(s->dict);
  fm_free(s->ib);
  fm_free(s);
}

/* ---- writer ------------------------------------------------------------- */

typedef struct XzRec { u64 unpadded, size; } XzRec;

struct FmOut {
  FmCodec c;
  FmWriteFn wr;
  void *ud;
  u8 *ob;
  u64 produced;
  FmErr err;
  int level;
  /* deflate / gzip */
  tdefl_compressor *def;
  u32 gcrc, gsize;
  /* bzip2 */
  bz_stream bz;
  bool bz_on;
  /* zstd */
  ZSTD_CCtx *zc;
  /* xz */
  CLzma2EncHandle l2;
  u8 l2prop;
  u8 *blk;
  size_t blk_n, blk_cap;
  XzRec *rec;
  int nrec, caprec;
  /* lzma alone: spooled to a temporary file */
  FILE *tmp;
  char tmp_path[FM_PATH_MAX];
  u64 tmp_size;
};

static FmErr file_write(void *ud, const void *buf, size_t n) {
  if (n && fwrite(buf, 1, n, (FILE *)ud) != n) return write_err();
  return FM_OK;
}

static FmErr out_put(FmOut *s, const void *buf, size_t n) {
  if (!n) return FM_OK;
  FmErr e = s->wr(s->ud, buf, n);
  if (e) { s->err = e; return e; }
  s->produced += n;
  return FM_OK;
}

static int lvl(int level, int def) { return (level < 1 || level > 9) ? def : level; }

/* deflate */

static FmErr deflate_run(FmOut *s, const u8 *p, size_t n, tdefl_flush flush) {
  for (;;) {
    size_t in_n = n, out_n = SBUF;
    tdefl_status st = tdefl_compress(s->def, p, &in_n, s->ob, &out_n, flush);
    if (st < 0) return s->err = FM_ERR_FORMAT;
    if (out_put(s, s->ob, out_n)) return s->err;
    p += in_n;
    n -= in_n;
    if (flush == TDEFL_FINISH) {
      if (st == TDEFL_STATUS_DONE) return FM_OK;
    } else if (!n && out_n < SBUF) {
      return FM_OK;
    }
  }
}

/* xz: one block per full buffer */

static void xz_crc32_le(u8 *p, u32 v) {
  p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

typedef struct XzSink { ISeqOutStream vt; FmOut *o; u64 n; } XzSink;

static size_t xz_sink_write(ISeqOutStreamPtr p, const void *buf, size_t size) {
  XzSink *w = (XzSink *)(void *)(uintptr_t)p;
  if (out_put(w->o, buf, size)) return 0;
  w->n += size;
  return size;
}

static FmErr xz_flush_block(FmOut *s) {
  if (!s->blk_n) return FM_OK;
  static const u8 zeros[8];
  u8 hdr[12];
  memset(hdr, 0, sizeof hdr);
  hdr[0] = 12 / 4 - 1;
  hdr[1] = 0;                   /* one filter, no size fields */
  hdr[2] = XZ_ID_LZMA2;
  hdr[3] = 1;
  hdr[4] = s->l2prop;
  xz_crc32_le(hdr + 8, crc32_update(0, hdr, 8));
  if (out_put(s, hdr, 12)) return s->err;
  XzSink w;
  w.vt.Write = xz_sink_write;
  w.o = s;
  w.n = 0;
  SRes r = Lzma2Enc_Encode2(s->l2, &w.vt, NULL, NULL, NULL, s->blk, s->blk_n, NULL);
  if (r != SZ_OK) return s->err ? s->err : (s->err = sres_err(r));
  if (out_put(s, zeros, (size_t)((4 - (w.n & 3)) & 3))) return s->err;
  u64 crc = CRC64_GET_DIGEST(Crc64Update(CRC64_INIT_VAL, s->blk, s->blk_n));
  u8 ck[8];
  for (int i = 0; i < 8; i++) ck[i] = (u8)(crc >> (8 * i));
  if (out_put(s, ck, 8)) return s->err;
  XzRec rec = { 12 + w.n + 8, s->blk_n };
  if (s->nrec == s->caprec) {
    s->caprec = s->caprec ? s->caprec * 2 : 16;
    s->rec = (XzRec *)fm_realloc(s->rec, (size_t)s->caprec * sizeof *s->rec);
  }
  s->rec[s->nrec++] = rec;
  s->blk_n = 0;
  return FM_OK;
}

static FmErr xz_finish(FmOut *s) {
  if (xz_flush_block(s)) return s->err;
  /* index: indicator, count, records, padding, CRC32 */
  u8 buf[32];
  u32 crc = 0;
  u64 isize = 0;
  buf[0] = 0;
  unsigned k = 1 + Xz_WriteVarInt(buf + 1, (UInt64)s->nrec);
  crc = crc32_update(crc, buf, k);
  isize += k;
  if (out_put(s, buf, k)) return s->err;
  for (int i = 0; i < s->nrec; i++) {
    k = Xz_WriteVarInt(buf, s->rec[i].unpadded);
    k += Xz_WriteVarInt(buf + k, s->rec[i].size);
    crc = crc32_update(crc, buf, k);
    isize += k;
    if (out_put(s, buf, k)) return s->err;
  }
  memset(buf, 0, 4);
  k = (unsigned)((4 - (isize & 3)) & 3);
  crc = crc32_update(crc, buf, k);
  isize += k + 4;
  xz_crc32_le(buf + k, crc);
  if (out_put(s, buf, k + 4)) return s->err;
  /* stream footer */
  u8 ft[12];
  xz_crc32_le(ft + 4, (u32)(isize / 4 - 1));
  ft[8] = 0;
  ft[9] = XZ_CHECK_CRC64;
  xz_crc32_le(ft, crc32_update(0, ft + 4, 6));
  ft[10] = 'Y';
  ft[11] = 'Z';
  return out_put(s, ft, 12);
}

static FmErr xz_start(FmOut *s) {
  static const u32 kDict[10] = { 0, 1u << 20, 2u << 20, 4u << 20, 4u << 20, 8u << 20,
                                 8u << 20, 16u << 20, 32u << 20, 32u << 20 };
  stream_sdk_init();
  int l = lvl(s->level, 6);
  s->l2 = Lzma2Enc_Create(&g_sz, &g_sz);
  if (!s->l2) return FM_ERR_NOMEM;
  CLzma2EncProps p;
  Lzma2EncProps_Init(&p);
  p.lzmaProps.level = l;
  p.lzmaProps.dictSize = kDict[l];
  p.lzmaProps.numThreads = 1;
  p.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
  p.numBlockThreads_Max = 1;
  p.numTotalThreads = 1;
  if (Lzma2Enc_SetProps(s->l2, &p) != SZ_OK) return FM_ERR_UNSUPPORTED;
  s->l2prop = Lzma2Enc_WriteProperties(s->l2);
  s->blk_cap = (size_t)kDict[l] * 2;
  s->blk = (u8 *)cm_alloc(s->blk_cap);
  if (!s->blk) return FM_ERR_NOMEM;
  u8 h[12] = { 0xFD, '7', 'z', 'X', 'Z', 0, 0, XZ_CHECK_CRC64 };
  xz_crc32_le(h + 8, crc32_update(0, h + 6, 2));
  return out_put(s, h, 12);
}

/* lzma alone */

typedef struct LzIn { ISeqInStream vt; FILE *f; u64 done; bool (*progress)(void *, u64); void *pud; } LzIn;
typedef struct LzOut { ISeqOutStream vt; FmWriteFn wr; void *ud; FmErr err; } LzOut;

static SRes lz_in_read(ISeqInStreamPtr p, void *buf, size_t *size) {
  LzIn *in = (LzIn *)(void *)(uintptr_t)p;
  size_t r = fread(buf, 1, *size, in->f);
  if (r == 0 && ferror(in->f)) return SZ_ERROR_READ;
  *size = r;
  in->done += r;
  if (in->progress && !in->progress(in->pud, in->done)) return SZ_ERROR_PROGRESS;
  return SZ_OK;
}

static size_t lz_out_write(ISeqOutStreamPtr p, const void *buf, size_t size) {
  LzOut *o = (LzOut *)(void *)(uintptr_t)p;
  FmErr e = o->wr(o->ud, buf, size);
  if (e) { o->err = e; return 0; }
  return size;
}

FmErr lzma_alone_encode(FILE *in, u64 size, int level, FmWriteFn wr, void *wud,
                        bool (*progress)(void *ud, u64 done), void *pud) {
  CLzmaEncHandle enc = LzmaEnc_Create(&g_sz);
  if (!enc) return FM_ERR_NOMEM;
  CLzmaEncProps p;
  LzmaEncProps_Init(&p);
  p.level = lvl(level, 6);
  if (p.dictSize == 0) {
    static const u32 kDict[10] = { 0, 1u << 20, 2u << 20, 4u << 20, 4u << 20, 8u << 20,
                                   8u << 20, 16u << 20, 32u << 20, 32u << 20 };
    p.dictSize = kDict[p.level];
  }
  p.reduceSize = size;
  p.numThreads = 1;
  FmErr e = FM_OK;
  if (LzmaEnc_SetProps(enc, &p) != SZ_OK) e = FM_ERR_UNSUPPORTED;
  u8 hdr[13];
  SizeT hl = LZMA_PROPS_SIZE;
  if (!e && LzmaEnc_WriteProperties(enc, hdr, &hl) != SZ_OK) e = FM_ERR_UNSUPPORTED;
  if (!e) {
    for (int i = 0; i < 8; i++) hdr[5 + i] = (u8)(size >> (8 * i));
    e = wr(wud, hdr, 13);
  }
  if (!e) {
    LzIn li;
    li.vt.Read = lz_in_read;
    li.f = in;
    li.done = 0;
    li.progress = progress;
    li.pud = pud;
    LzOut lo;
    lo.vt.Write = lz_out_write;
    lo.wr = wr;
    lo.ud = wud;
    lo.err = FM_OK;
    SRes r = LzmaEnc_Encode(enc, &lo.vt, &li.vt, NULL, &g_sz, &g_sz);
    if (lo.err) e = lo.err;
    else if (r != SZ_OK) e = sres_err(r);
    else if (li.done != size) e = FM_ERR_IO;      /* file changed while packing */
  }
  LzmaEnc_Destroy(enc, &g_sz, &g_sz);
  return e;
}

static FmErr lzma_spool_start(FmOut *s) {
  char dir[FM_PATH_MAX];
  u8 rnd[8];
  if (!plat_place(PLACE_TEMP, dir, sizeof dir)) return FM_ERR_IO;
  plat_random(rnd, sizeof rnd);
  char name[48];
  fm_snprintf(name, sizeof name, "mmcfm-lzma-%02x%02x%02x%02x%02x%02x%02x%02x.tmp", rnd[0], rnd[1],
              rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
  if (!fm_path_join(s->tmp_path, sizeof s->tmp_path, dir, name)) return FM_ERR_IO;
  s->tmp = fm_fopen(s->tmp_path, "w+b");
  return s->tmp ? FM_OK : FM_ERR_IO;
}

/* public */

FmOut *out_open_cb(FmCodec c, int level, FmWriteFn wr, void *ud, FmErr *err) {
  FmOut *s = (FmOut *)fm_calloc(1, sizeof *s);
  s->c = c;
  s->wr = wr;
  s->ud = ud;
  s->level = level;
  FmErr e = FM_OK;
  switch ((int)c) {
    case CODEC_STORE:
      break;
    case CODEC_DEFLATE:
    case CODEC_GZIP: {
      s->ob = (u8 *)fm_alloc(SBUF);
      s->def = (tdefl_compressor *)fm_alloc(sizeof *s->def);
      int l = (level < 0 || level > 9) ? 6 : level;
      mz_uint flags = tdefl_create_comp_flags_from_zip_params(l, -MZ_DEFAULT_WINDOW_BITS,
                                                              MZ_DEFAULT_STRATEGY);
      if (tdefl_init(s->def, NULL, NULL, (int)flags) != TDEFL_STATUS_OKAY) e = FM_ERR_UNSUPPORTED;
      if (!e && c == CODEC_GZIP) {
        static const u8 h[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF };
        e = out_put(s, h, 10);
      }
      break;
    }
    case CODEC_BZIP2:
      s->ob = (u8 *)fm_alloc(SBUF);
      s->bz.bzalloc = bz_alloc;
      s->bz.bzfree = bz_free;
      if (BZ2_bzCompressInit(&s->bz, lvl(level, 9), 0, 0) != BZ_OK) e = FM_ERR_NOMEM;
      else s->bz_on = true;
      break;
    case CODEC_XZ:
      e = xz_start(s);
      break;
    case CODEC_LZMA:
      e = lzma_spool_start(s);
      break;
    case CODEC_ZSTD: {
      static const int kZl[10] = { 3, 1, 2, 3, 5, 7, 9, 12, 15, 19 };
      s->ob = (u8 *)fm_alloc(SBUF);
      s->zc = ZSTD_createCCtx_advanced(g_zmem);
      if (!s->zc) { e = FM_ERR_NOMEM; break; }
      ZSTD_CCtx_setParameter(s->zc, ZSTD_c_compressionLevel, kZl[lvl(level, 0)]);
      ZSTD_CCtx_setParameter(s->zc, ZSTD_c_checksumFlag, 1);
      break;
    }
    default:
      e = FM_ERR_UNSUPPORTED;
  }
  if (e) {
    s->err = e;
    out_close(s);
    s = NULL;
  }
  if (err) *err = e;
  return s;
}

FmOut *out_open(FILE *f, FmCodec c, int level, FmErr *err) {
  return out_open_cb(c, level, file_write, f, err);
}

FmErr out_write(FmOut *s, const void *buf, size_t n) {
  if (!s) return FM_ERR_IO;
  if (s->err) return s->err;
  const u8 *p = (const u8 *)buf;
  switch (s->c) {
    case CODEC_STORE:
      return out_put(s, p, n);
    case CODEC_DEFLATE:
    case CODEC_GZIP:
      if (s->c == CODEC_GZIP) {
        s->gcrc = crc32_update(s->gcrc, p, n);
        s->gsize += (u32)n;
      }
      return n ? deflate_run(s, p, n, TDEFL_NO_FLUSH) : FM_OK;
    case CODEC_BZIP2:
      while (n) {
        unsigned take = (unsigned)FM_MIN(n, (size_t)0x40000000);
        s->bz.next_in = (char *)(uintptr_t)p;
        s->bz.avail_in = take;
        while (s->bz.avail_in) {
          s->bz.next_out = (char *)s->ob;
          s->bz.avail_out = SBUF;
          if (BZ2_bzCompress(&s->bz, BZ_RUN) != BZ_RUN_OK) return s->err = FM_ERR_FORMAT;
          if (out_put(s, s->ob, SBUF - s->bz.avail_out)) return s->err;
        }
        p += take;
        n -= take;
      }
      return FM_OK;
    case CODEC_XZ:
      while (n) {
        size_t take = FM_MIN(n, s->blk_cap - s->blk_n);
        memcpy(s->blk + s->blk_n, p, take);
        s->blk_n += take;
        p += take;
        n -= take;
        if (s->blk_n == s->blk_cap && xz_flush_block(s)) return s->err;
      }
      return FM_OK;
    case CODEC_LZMA:
      if (n && fwrite(p, 1, n, s->tmp) != n) return s->err = write_err();
      s->tmp_size += n;
      return FM_OK;
    case CODEC_ZSTD: {
      ZSTD_inBuffer in = { p, n, 0 };
      while (in.pos < in.size) {
        ZSTD_outBuffer ob = { s->ob, SBUF, 0 };
        size_t r = ZSTD_compressStream2(s->zc, &ob, &in, ZSTD_e_continue);
        if (ZSTD_isError(r)) return s->err = FM_ERR_NOMEM;
        if (out_put(s, s->ob, ob.pos)) return s->err;
      }
      return FM_OK;
    }
    default:
      return s->err = FM_ERR_UNSUPPORTED;
  }
}

static FmErr out_finish(FmOut *s) {
  switch (s->c) {
    case CODEC_STORE:
      return FM_OK;
    case CODEC_DEFLATE:
    case CODEC_GZIP:
      if (deflate_run(s, NULL, 0, TDEFL_FINISH)) return s->err;
      if (s->c == CODEC_GZIP) {
        u8 t[8];
        xz_crc32_le(t, s->gcrc);
        xz_crc32_le(t + 4, s->gsize);
        return out_put(s, t, 8);
      }
      return FM_OK;
    case CODEC_BZIP2:
      for (;;) {
        s->bz.next_in = NULL;
        s->bz.avail_in = 0;
        s->bz.next_out = (char *)s->ob;
        s->bz.avail_out = SBUF;
        int r = BZ2_bzCompress(&s->bz, BZ_FINISH);
        if (r != BZ_FINISH_OK && r != BZ_STREAM_END) return s->err = FM_ERR_FORMAT;
        if (out_put(s, s->ob, SBUF - s->bz.avail_out)) return s->err;
        if (r == BZ_STREAM_END) return FM_OK;
      }
    case CODEC_XZ:
      return xz_finish(s);
    case CODEC_LZMA:
      if (fflush(s->tmp) != 0 || fm_fseek64(s->tmp, 0, SEEK_SET) != 0) return s->err = FM_ERR_IO;
      return s->err = lzma_alone_encode(s->tmp, s->tmp_size, s->level, s->wr, s->ud, NULL, NULL);
    case CODEC_ZSTD:
      for (;;) {
        ZSTD_inBuffer in = { NULL, 0, 0 };
        ZSTD_outBuffer ob = { s->ob, SBUF, 0 };
        size_t r = ZSTD_compressStream2(s->zc, &ob, &in, ZSTD_e_end);
        if (ZSTD_isError(r)) return s->err = FM_ERR_NOMEM;
        if (out_put(s, s->ob, ob.pos)) return s->err;
        if (r == 0) return FM_OK;
      }
    default:
      return FM_ERR_UNSUPPORTED;
  }
}

FmErr out_close(FmOut *s) {
  if (!s) return FM_ERR_IO;
  FmErr e = s->err ? s->err : out_finish(s);
  if (!e && s->err) e = s->err;
  if (s->bz_on) BZ2_bzCompressEnd(&s->bz);
  if (s->zc) ZSTD_freeCCtx(s->zc);
  if (s->l2) Lzma2Enc_Destroy(s->l2);
  cm_free(s->blk);
  fm_free(s->rec);
  if (s->tmp) {
    fclose(s->tmp);
    plat_remove_file(s->tmp_path);
  }
  fm_free(s->def);
  fm_free(s->ob);
  fm_free(s);
  return e;
}

u64 out_produced(const FmOut *s) { return s ? s->produced : 0; }
