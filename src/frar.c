/* frar.c -- RAR archives: list and extract RAR 1.5-4.x and RAR 5.0 (read only).
**
** Creating RAR archives is proprietary, so this module only reads. The
** RAR 2.9/3.x decoder (LZ with the RarVM standard filters, PPMd var.H)
** and the RAR 5.0 decoder (LZ with its DELTA/E8/E8E9/ARM filters) are a
** port of libarchive 3.7.7's archive_read_support_format_rar.c and
** archive_read_support_format_rar5.c, rewritten for this code base: own
** bit reader, a shared window/filter core that pushes into the extract
** sink, solid streams across files, and bounds checks everywhere. PPMd
** var.H is the vendored LZMA SDK (Ppmd7.c + Ppmd7aDec.c, the RAR range
** coder). BLAKE2sp follows the public BLAKE2 specification.
**
** The libarchive code this is derived from carries this notice:
**
**   Copyright (c) 2003-2007 Tim Kientzle
**   Copyright (c) 2011 Andres Mejia
**   Copyright (c) 2018 Grzegorz Antoniak (http://antoniak.org)
**   All rights reserved.
**
**   Redistribution and use in source and binary forms, with or without
**   modification, are permitted provided that the following conditions
**   are met:
**   1. Redistributions of source code must retain the above copyright
**      notice, this list of conditions and the following disclaimer.
**   2. Redistributions in binary form must reproduce the above copyright
**      notice, this list of conditions and the following disclaimer in the
**      documentation and/or other materials provided with the distribution.
**
**   THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
**   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
**   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
**   IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
**   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
**   NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
**   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
**   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
**   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
**   THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
** Design decisions:
**   - Listing reads every header once into a RarItem per entry (same index
**     as the FmArc entry). Headers are bounded: 64 KB for RAR4, 2 MB for
**     RAR5 (the format's own limit), so nothing is sized from file data.
**   - Both decoders write into one circular window that is allocated once
**     per extraction (sized from the largest dictionary actually needed,
**     at most 4 MB for RAR4 and 1 GB for RAR5; bigger is refused). The
**     window is flushed to the sink in 1 MB steps; a filter holds the
**     flush back until its block is complete, then runs on a copy.
**   - The window, the PPMd model and the 64 KB input buffer are the only
**     big allocations. The first two use malloc (not fm_alloc, which aborts
**     on failure) because their size comes from the archive: a phone that
**     cannot spare them gets FM_ERR_NOMEM instead of a crash.
**   - Solid archives decode in order. An unselected entry is decoded and
**     discarded when the next compressed entry continues its stream, and
**     skipped (seek) otherwise. Decoder state (tables, PPMd model, repeat
**     distances, RarVM programs) is kept between the files of a stream,
**     as unrar does; each file's data is a fresh byte stream.
**   - Untrusted input: reads past the end of the packed data return zero
**     bits and are counted; decoding stops when it overruns, when a file
**     produces more than its size plus a small slack, or when a filter
**     does not fit the window. Every loop consumes input or output, so a
**     corrupt archive cannot hang the worker.
**   - Not supported, reported as FM_ERR_UNSUPPORTED on extraction (listed
**     normally): encrypted entries (RAR4 and RAR5; encrypted headers fail
**     at open), files continued in another volume, RAR 1.5/2.x compression
**     (unpack15/20, libarchive has no reference), RarVM programs other than
**     the standard filters, RAR 7 dictionaries over 1 GB, hard links and
**     file copies. Only the first volume of a set is read.
**   - BLAKE2sp checksums (RAR5 -htb) are verified like CRC32.
*/
#include "farc_int.h"
#include "fcrypt.h"
#include "Ppmd7.h"
#include <time.h>

/* ---- limits and constants --------------------------------------------- */

#define RAR_INBUF      (64 * 1024)
#define RAR_MINWIN     ((u64)256 * 1024)
#define RAR3_MAXDICT   ((u64)4 << 20)
#define RAR5_MAXDICT   ((u64)1 << 30)
#define RAR_MARGIN     0x2000u            /* > longest match (RAR5: ~4.1 KB) */
#define RAR_SLACK      0x10000u           /* decoded bytes tolerated past a file's size */
#define RAR_MAXFILTERS 8192               /* pending filters, as unrar */
#define RAR_MAXPROGS   1024
#define RAR3_VMSIZE    0x40000u           /* RarVM memory: the largest RAR3 filter block */
#define RAR5_MAXFLT    0x400000u
#define RAR3_TABLES    (299 + 60 + 17 + 28)
#define RAR5_TABLES    (306 + 64 + 16 + 44)

enum { M_STORE = 0, M_LZ29, M_LZ50 };                       /* RarItem.method */
enum { BAD_NONE = 0, BAD_CRYPT, BAD_SPLIT, BAD_METHOD, BAD_DICT, BAD_REF };
enum { F_NONE = 0, F_E8, F_E8E9, F_DELTA, F_RGB, F_AUDIO, F_ITANIUM, F_ARM };

#define MODE_DIR  0040000u
#define MODE_REG  0100000u
#define MODE_LNK  0120000u
#define MODE_FMT  0170000u

typedef struct RarItem {
  i64 data;            /* offset of the packed data in the archive */
  u64 packed, size;
  u64 dict;            /* dictionary the file was compressed with */
  u32 crc;
  const char *link;    /* RAR5 symlink target (written as the entry's data) */
  u8 blake[32];
  u8 method, bad, solid, is_dir, has_crc, has_blake;
} RarItem;

typedef struct RarPriv {
  int ver;             /* 4 or 5 */
  bool solid;          /* archive flag */
  RarItem *it;
  int n, cap;
} RarPriv;

/* ---- small helpers ------------------------------------------------------- */

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put_le32(u8 *p, u32 v) {
  p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u64 pow2ceil(u64 v) {
  u64 p = 1;
  while (p < v && p < ((u64)1 << 62)) p <<= 1;
  return p;
}

static bool read_at(FILE *f, i64 off, void *buf, size_t n) {
  if (fm_fseek64(f, off, SEEK_SET) != 0) return false;
  return fread(buf, 1, n, f) == n;
}

/* RAR5 variable-length integer from [*p, end). */
static bool vint(const u8 **p, const u8 *end, u64 *v) {
  u64 r = 0;
  int sh;
  for (sh = 0; *p < end && sh < 70; sh += 7) {
    u8 b = *(*p)++;
    if (sh < 64) r |= (u64)(b & 0x7f) << sh;
    if (!(b & 0x80)) { *v = r; return true; }
  }
  return false;
}

static void *big_calloc(u64 n) {
  if (n == 0 || n > (u64)(size_t)-1) return NULL;
  return calloc(1, (size_t)n);
}

static void *ppmd_alloc(ISzAllocPtr p, size_t n) { FM_UNUSED(p); return malloc(n); }
static void ppmd_free(ISzAllocPtr p, void *a) { FM_UNUSED(p); free(a); }
static const ISzAlloc kRarAlloc = { ppmd_alloc, ppmd_free };

/* UTF-16 code units to UTF-8 (unpaired surrogates become U+FFFD). */
static void utf16_to_utf8(const u16 *u, int n, char *out, size_t cap) {
  size_t o = 0;
  int i;
  for (i = 0; i < n && u[i]; i++) {
    u32 cp = u[i];
    char tmp[4];
    int k, len;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
    }
    len = utf8_encode(cp, tmp);
    if (o + (size_t)len + 1 > cap) break;
    for (k = 0; k < len; k++) out[o++] = tmp[k];
  }
  out[o] = 0;
}

/* Bytes that are not valid UTF-8 are taken as Latin-1 (RAR4 without the
** Unicode flag stores names in the creator's code page). */
static void bytes_to_utf8(const u8 *s, size_t n, char *out, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n && s[i]) {
    u32 cp;
    int used = utf8_decode((const char *)s + i, &cp);
    char tmp[4];
    int k, len;
    if (cp == 0xFFFD && !(n - i >= 3 && s[i] == 0xEF && s[i + 1] == 0xBF && s[i + 2] == 0xBD)) {
      cp = s[i];
      used = 1;
    }
    if (i + (size_t)used > n) { cp = s[i]; used = 1; }
    len = utf8_encode(cp, tmp);
    if (o + (size_t)len + 1 > cap) break;
    for (k = 0; k < len; k++) out[o++] = tmp[k];
    i += (size_t)used;
  }
  out[o] = 0;
}

/* ---- BLAKE2sp (RAR5 -htb) ------------------------------------------------ */

typedef struct B2s {
  u32 h[8], t[2], f[2];
  u8 buf[64];
  u32 buflen;
  bool last_node;
} B2s;

typedef struct B2sp {
  B2s leaf[8];
  u64 total;
} B2sp;

static const u32 kB2Iv[8] = {
  0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
};
static const u8 kB2Sigma[10][16] = {
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
  { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
  { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
  { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
  { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
  { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
  { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
  { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
  { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
  { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

#define B2ROT(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define B2G(a, b, c, d, x, y) do { \
    v[a] += v[b] + (x); v[d] = B2ROT(v[d] ^ v[a], 16); v[c] += v[d]; v[b] = B2ROT(v[b] ^ v[c], 12); \
    v[a] += v[b] + (y); v[d] = B2ROT(v[d] ^ v[a], 8); v[c] += v[d]; v[b] = B2ROT(v[b] ^ v[c], 7); \
  } while (0)

static void b2s_compress(B2s *s, const u8 *blk) {
  u32 m[16], v[16];
  int i, r;
  for (i = 0; i < 16; i++) m[i] = le32(blk + 4 * i);
  for (i = 0; i < 8; i++) { v[i] = s->h[i]; v[i + 8] = kB2Iv[i]; }
  v[12] ^= s->t[0]; v[13] ^= s->t[1]; v[14] ^= s->f[0]; v[15] ^= s->f[1];
  for (r = 0; r < 10; r++) {
    const u8 *g = kB2Sigma[r];
    B2G(0, 4, 8, 12, m[g[0]], m[g[1]]);
    B2G(1, 5, 9, 13, m[g[2]], m[g[3]]);
    B2G(2, 6, 10, 14, m[g[4]], m[g[5]]);
    B2G(3, 7, 11, 15, m[g[6]], m[g[7]]);
    B2G(0, 5, 10, 15, m[g[8]], m[g[9]]);
    B2G(1, 6, 11, 12, m[g[10]], m[g[11]]);
    B2G(2, 7, 8, 13, m[g[12]], m[g[13]]);
    B2G(3, 4, 9, 14, m[g[14]], m[g[15]]);
  }
  for (i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
}

static void b2s_inc(B2s *s, u32 n) {
  s->t[0] += n;
  if (s->t[0] < n) s->t[1]++;
}

/* Tree parameters of BLAKE2sp: fanout 8, depth 2, inner length 32. */
static void b2s_init(B2s *s, u32 node_offset, u32 node_depth, bool last) {
  int i;
  memset(s, 0, sizeof *s);
  for (i = 0; i < 8; i++) s->h[i] = kB2Iv[i];
  s->h[0] ^= 32u | (8u << 16) | (2u << 24);
  s->h[2] ^= node_offset;
  s->h[3] ^= (node_depth << 16) | (32u << 24);
  s->last_node = last;
}

static void b2s_update(B2s *s, const u8 *in, size_t n) {
  if (n == 0) return;
  if (n > 64 - s->buflen) {
    size_t fill = 64 - s->buflen;
    memcpy(s->buf + s->buflen, in, fill);
    b2s_inc(s, 64);
    b2s_compress(s, s->buf);
    s->buflen = 0;
    in += fill; n -= fill;
    while (n > 64) {
      b2s_inc(s, 64);
      b2s_compress(s, in);
      in += 64; n -= 64;
    }
  }
  memcpy(s->buf + s->buflen, in, n);
  s->buflen += (u32)n;
}

static void b2s_final(B2s *s, u8 out[32]) {
  int i;
  b2s_inc(s, s->buflen);
  s->f[0] = 0xFFFFFFFFu;
  if (s->last_node) s->f[1] = 0xFFFFFFFFu;
  memset(s->buf + s->buflen, 0, 64 - s->buflen);
  b2s_compress(s, s->buf);
  for (i = 0; i < 8; i++) put_le32(out + 4 * i, s->h[i]);
}

static void b2sp_init(B2sp *s) {
  int i;
  for (i = 0; i < 8; i++) b2s_init(&s->leaf[i], (u32)i, 0, i == 7);
  s->total = 0;
}

/* 64-byte stripes go round-robin to the eight leaves. */
static void b2sp_update(B2sp *s, const u8 *p, size_t n) {
  while (n) {
    size_t k = 64 - (size_t)(s->total & 63);
    if (k > n) k = n;
    b2s_update(&s->leaf[(s->total >> 6) & 7], p, k);
    s->total += k;
    p += k; n -= k;
  }
}

static void b2sp_final(B2sp *s, u8 out[32]) {
  B2s root;
  u8 h[32];
  int i;
  b2s_init(&root, 0, 1, true);
  for (i = 0; i < 8; i++) {
    b2s_final(&s->leaf[i], h);
    b2s_update(&root, h, 32);
  }
  b2s_final(&root, out);
}

/* ---- input: packed bytes as an MSB-first bit stream -------------------- */

typedef struct RarIn {
  FILE *f;
  u8 *buf;             /* RAR_INBUF */
  u32 pos, len;
  u64 left;            /* packed bytes not yet read from the file */
  u64 real;            /* bytes the stream really has */
  u64 fed;             /* bytes moved into the cache, zero padding included */
  u64 cache;
  int avail;           /* valid low bits in cache */
  bool ioerr;
} RarIn;

static FmErr in_begin(RarIn *r, FILE *f, i64 off, u64 n) {
  r->f = f;
  r->pos = r->len = 0;
  r->left = r->real = n;
  r->fed = 0;
  r->cache = 0;
  r->avail = 0;
  r->ioerr = false;
  return fm_fseek64(f, off, SEEK_SET) == 0 ? FM_OK : FM_ERR_IO;
}

static u32 in_slow_byte(RarIn *r) {
  if (r->left > 0 && !r->ioerr) {
    size_t want = r->left < RAR_INBUF ? (size_t)r->left : RAR_INBUF;
    size_t got = fread(r->buf, 1, want, r->f);
    if (got == 0) {          /* truncated archive: what we have is all there is */
      r->ioerr = true;
      r->real = r->fed;
      r->left = 0;
      return 0;
    }
    r->left -= got;
    r->len = (u32)got;
    r->pos = 1;
    return r->buf[0];
  }
  return 0;
}

static void in_fill(RarIn *r) {
  while (r->avail <= 56) {
    u32 b = r->pos < r->len ? r->buf[r->pos++] : in_slow_byte(r);
    r->cache = (r->cache << 8) | b;
    r->avail += 8;
    r->fed++;
  }
}

/* n <= 32 */
static inline u32 in_peek(RarIn *r, int n) {
  if (r->avail < n) in_fill(r);
  return (u32)(r->cache >> (r->avail - n)) & (u32)(((u64)1 << n) - 1);
}

static inline void in_skip(RarIn *r, int n) {
  if (r->avail < n) in_fill(r);
  r->avail -= n;
}

static inline u32 in_bits(RarIn *r, int n) {
  u32 v = in_peek(r, n);
  r->avail -= n;
  return v;
}

static inline u64 in_bitpos(const RarIn *r) { return r->fed * 8 - (u64)r->avail; }
static inline bool in_over(const RarIn *r, u32 slack) { return in_bitpos(r) > (r->real + slack) * 8; }
static void in_align(RarIn *r) { r->avail &= ~7; }

static void in_skip_to(RarIn *r, u64 bitpos) {
  while (in_bitpos(r) < bitpos) {
    u64 d = bitpos - in_bitpos(r);
    in_skip(r, d > 32 ? 32 : (int)d);
  }
}

/* PPMd pulls bytes through this. */
typedef struct RarByteIn {
  IByteIn vt;
  RarIn *in;
} RarByteIn;

static Byte rar_ppmd_read(IByteInPtr p) {
  const RarByteIn *b = (const RarByteIn *)(const void *)p;
  return (Byte)in_bits(b->in, 8);
}

/* ---- output: one entry's bytes to the sink (or discarded) ------------- */

typedef struct RarOut {
  FmArcSink *s;
  bool write;          /* false: decoded only to keep a solid stream going */
  bool blake;
  u64 size, done;
  u32 crc;
  B2sp b2;
} RarOut;

static FmErr out_put(RarOut *o, const u8 *p, size_t n) {
  if (o->done < o->size && n) {
    u64 room = o->size - o->done;
    size_t k = (u64)n > room ? (size_t)room : n;
    if (o->write) {
      FmErr err;
      o->crc = crc32_update(o->crc, p, k);
      if (o->blake) b2sp_update(&o->b2, p, k);
      if ((err = sink_write(o->s, p, k)) != FM_OK) return err;
    }
  }
  o->done += n;
  return FM_OK;
}

/* ---- canonical Huffman decoding (RAR3 and RAR5 share the scheme) ------- */

#define HUFF_MAXSYM 306

typedef struct RarHuff {
  u32 len[16];         /* left-aligned 16-bit upper limit of each code length */
  u32 pos[16];         /* first index in num[] of each code length */
  u32 qbits, size;
  u8 qlen[1 << 10];
  u16 qnum[1 << 10];
  u16 num[HUFF_MAXSYM];
} RarHuff;

static void huff_build(RarHuff *h, const u8 *lens, int size) {
  u32 lc[16], cpos[16], upper = 0, code, cur = 1, qsize;
  int i;
  memset(lc, 0, sizeof lc);
  memset(h->num, 0, sizeof h->num);
  h->size = (u32)size;
  h->qbits = size >= 298 ? 10 : 7;
  for (i = 0; i < size; i++) lc[lens[i] & 15]++;
  lc[0] = 0;
  h->pos[0] = 0;
  h->len[0] = 0;
  for (i = 1; i < 16; i++) {
    upper += lc[i];
    h->len[i] = upper << (16 - i);
    h->pos[i] = h->pos[i - 1] + lc[i - 1];
    upper <<= 1;
  }
  memcpy(cpos, h->pos, sizeof cpos);
  for (i = 0; i < size; i++) {
    u32 c = lens[i] & 15;
    if (c) h->num[cpos[c]++] = (u16)i;
  }
  qsize = 1u << h->qbits;
  for (code = 0; code < qsize; code++) {
    u32 bf = code << (16 - h->qbits), dist, p;
    while (cur < 16 && bf >= h->len[cur]) cur++;
    h->qlen[code] = (u8)cur;
    dist = (bf - h->len[cur - 1]) >> (16 - cur);
    p = h->pos[cur & 15] + dist;
    h->qnum[code] = (cur < 16 && p < h->size) ? h->num[p] : 0;
  }
}

static inline u32 huff_dec(const RarHuff *h, RarIn *in) {
  u32 bf = in_peek(in, 16) & 0xfffe, bits = 15, dist, p, i;
  if (bf < h->len[h->qbits]) {
    u32 c = bf >> (16 - h->qbits);
    in->avail -= h->qlen[c];
    return h->qnum[c];
  }
  for (i = h->qbits + 1; i < 15; i++)
    if (bf < h->len[i]) { bits = i; break; }
  in->avail -= (int)bits;
  dist = (bf - h->len[bits - 1]) >> (16 - bits);
  p = h->pos[bits] + dist;
  if (p >= h->size) p = 0;
  return h->num[p];
}

/* ---- window and filters (shared by both decoders) --------------------- */

typedef struct RarFilter {
  u64 start;           /* absolute window position */
  u32 len;             /* bytes taken from the window */
  u32 dlen;            /* bytes the filter works on (RAR3: register R4) */
  u32 arg0, arg1;      /* channels / width, posR */
  u8 type;
} RarFilter;

typedef struct RarLz {
  u8 *win;
  u64 wsize, wmask;
  u64 pos;             /* bytes decoded since the extraction started */
  u64 wr;              /* bytes flushed */
  u64 fstart;          /* pos when the current file started */
  u64 next_flush, chunk;
  RarFilter *flt;
  int fhead, nflt, fcap;
  u8 *fbuf;
  size_t fbuf_cap;
  bool rar5;
} RarLz;

static inline void lz_put(RarLz *z, u32 b) {
  z->win[(size_t)(z->pos & z->wmask)] = (u8)b;
  z->pos++;
}

static void lz_copy(RarLz *z, u64 dist, u32 len) {
  size_t d = (size_t)(z->pos & z->wmask), s = (size_t)((z->pos - dist) & z->wmask);
  size_t ws = (size_t)z->wsize, i, m = (size_t)z->wmask;
  if (d + len <= ws && s + len <= ws && (s + len <= d || d + len <= s)) {
    memcpy(z->win + d, z->win + s, len);
  } else {
    for (i = 0; i < len; i++) z->win[(d + i) & m] = z->win[(s + i) & m];
  }
  z->pos += len;
}

static FmErr lz_add_filter(RarLz *z, const RarFilter *f) {
  if (f->len == 0) return FM_OK;
  if (z->nflt == z->fcap && z->fhead > 0) {
    memmove(z->flt, z->flt + z->fhead, (size_t)(z->nflt - z->fhead) * sizeof *z->flt);
    z->nflt -= z->fhead;
    z->fhead = 0;
  }
  if (z->nflt == z->fcap) {
    if (z->fcap >= RAR_MAXFILTERS) return FM_ERR_FORMAT;
    z->fcap = z->fcap ? z->fcap * 2 : 16;
    z->flt = (RarFilter *)fm_realloc(z->flt, (size_t)z->fcap * sizeof *z->flt);
  }
  z->flt[z->nflt++] = *f;
  return FM_OK;
}

static FmErr lz_emit(RarLz *z, RarOut *o, u64 from, u64 to) {
  while (from < to) {
    size_t off = (size_t)(from & z->wmask);
    u64 n = to - from;
    FmErr err;
    if (n > z->wsize - off) n = z->wsize - off;
    if ((err = out_put(o, z->win + off, (size_t)n)) != FM_OK) return err;
    from += n;
  }
  return FM_OK;
}

static bool flt_e8(u8 *d, u32 len, u32 fpos, bool e9, bool mod) {
  const u32 fsize = 0x1000000;
  u32 i = 0;
  while (i + 4 < len) {
    u8 b = d[i++];
    if (b == 0xE8 || (e9 && b == 0xE9)) {
      u32 off = fpos + i, addr = le32(d + i);
      if (mod) off %= fsize;
      if (addr & 0x80000000u) {
        if (((addr + off) & 0x80000000u) == 0) put_le32(d + i, addr + fsize);
      } else if ((addr - fsize) & 0x80000000u) {
        put_le32(d + i, addr - off);
      }
      i += 4;
    }
  }
  return true;
}

static bool flt_delta(u8 *buf, u32 len, u32 ch) {
  u8 *dst = buf + len;
  u32 c, j, s = 0;
  if (ch == 0 || ch > 1024) return false;
  for (c = 0; c < ch; c++) {
    u8 prev = 0;
    for (j = c; j < len; j += ch) {
      prev = (u8)(prev - buf[s++]);
      dst[j] = prev;
    }
  }
  return true;
}

static bool flt_rgb(u8 *buf, u32 len, u32 width, u32 posr) {
  u8 *dst = buf + len;
  u32 c, j, s = 0;
  if (len < 3 || width < 3 || width > len || posr > 2) return false;
  for (c = 0; c < 3; c++) {
    u32 prev = 0;
    for (j = c; j < len; j += 3) {
      if (j >= width) {
        int up = dst[j - width + 3], ul = dst[j - width];
        int pa = abs(up - ul), pb = abs((int)prev - ul), pc = abs(up - ul + (int)prev - ul);
        if (pa > pb || pa > pc) prev = pb <= pc ? (u32)up : (u32)ul;
      }
      prev = (u8)(prev - buf[s++]);
      dst[j] = (u8)prev;
    }
  }
  for (j = posr; j + 2 < len; j += 3) {
    dst[j] = (u8)(dst[j] + dst[j + 1]);
    dst[j + 2] = (u8)(dst[j + 2] + dst[j + 1]);
  }
  return true;
}

static bool flt_audio(u8 *buf, u32 len, u32 ch) {
  u8 *dst = buf + len;
  u32 c, j, s = 0;
  if (ch == 0 || ch > 128) return false;
  for (c = 0; c < ch; c++) {
    int w[3] = { 0, 0, 0 }, dl[3] = { 0, 0, 0 }, err[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int lastdelta = 0, count = 0;
    u32 last = 0;
    for (j = c; j < len; j += ch) {
      int delta = (i8)buf[s++], pe, k, idx = 0;
      u32 pred, b;
      dl[2] = dl[1];
      dl[1] = lastdelta - dl[0];
      dl[0] = lastdelta;
      pred = (u32)((8 * (int)last + w[0] * dl[0] + w[1] * dl[1] + w[2] * dl[2]) >> 3) & 0xFF;
      b = (pred - (u32)delta) & 0xFF;
      pe = delta * 8;
      err[0] += abs(pe);
      err[1] += abs(pe - dl[0]); err[2] += abs(pe + dl[0]);
      err[3] += abs(pe - dl[1]); err[4] += abs(pe + dl[1]);
      err[5] += abs(pe - dl[2]); err[6] += abs(pe + dl[2]);
      lastdelta = (i8)(u8)(b - last);
      dst[j] = (u8)b;
      last = b;
      if (!(count++ & 0x1F)) {
        for (k = 1; k < 7; k++)
          if (err[k] < err[idx]) idx = k;
        memset(err, 0, sizeof err);
        switch (idx) {
          case 1: if (w[0] >= -16) w[0]--; break;
          case 2: if (w[0] < 16) w[0]++; break;
          case 3: if (w[1] >= -16) w[1]--; break;
          case 4: if (w[1] < 16) w[1]++; break;
          case 5: if (w[2] >= -16) w[2]--; break;
          case 6: if (w[2] < 16) w[2]++; break;
          default: break;
        }
      }
    }
  }
  return true;
}

/* Itanium bundles: in each 16-byte bundle the template selects which of
** the three 41-bit slots hold a branch whose 20-bit offset is relative. */
static bool flt_itanium(u8 *d, u32 len, u32 fpos) {
  static const u8 kMasks[16] = { 4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0 };
  u32 off = fpos >> 4, p;
  for (p = 0; p + 21 < len; p += 16, off++) {
    int t = (d[p] & 0x1f) - 0x10, slot;
    if (t < 0 || !kMasks[t]) continue;
    for (slot = 0; slot < 3; slot++) {
      u32 bit, raw, v, sh;
      u8 *q;
      if (!(kMasks[t] & (1 << slot))) continue;
      bit = (u32)slot * 41 + 5;
      if (((le32(d + p + (bit + 37) / 8) >> ((bit + 37) & 7)) & 15) != 5) continue;
      q = d + p + (bit + 13) / 8;
      sh = (bit + 13) & 7;
      raw = le32(q);
      v = ((raw >> sh) - off) & 0xFFFFF;
      raw = (raw & ~(0xFFFFFu << sh)) | (v << sh);
      put_le32(q, raw);
    }
  }
  return true;
}

static bool flt_arm(u8 *d, u32 len, u32 fpos) {
  u32 i;
  for (i = 0; i + 3 < len; i += 4) {
    if (d[i + 3] == 0xEB) {
      u32 off = (le32(d + i) & 0x00FFFFFF) - (fpos + i) / 4;
      put_le32(d + i, (off & 0x00FFFFFF) | 0xEB000000u);
    }
  }
  return true;
}

/* Runs one filter on buf[0..n); the result is moved back to buf[0..n). */
static FmErr flt_exec(RarLz *z, const RarFilter *f, u32 n, u32 fpos) {
  u8 *b = z->fbuf;
  bool ok, moved = false;
  switch (f->type) {
    case F_E8:      ok = flt_e8(b, n, fpos, false, z->rar5); break;
    case F_E8E9:    ok = flt_e8(b, n, fpos, true, z->rar5); break;
    case F_ITANIUM: ok = flt_itanium(b, n, fpos); break;
    case F_ARM:     ok = flt_arm(b, n, fpos); break;
    case F_DELTA:   ok = flt_delta(b, n, f->arg0); moved = true; break;
    case F_RGB:     ok = flt_rgb(b, n, f->arg0, f->arg1); moved = true; break;
    case F_AUDIO:   ok = flt_audio(b, n, f->arg0); moved = true; break;
    default:        return FM_ERR_UNSUPPORTED;
  }
  if (!ok) return FM_ERR_FORMAT;
  if (moved) memmove(b, b + n, n);
  return FM_OK;
}

/* Runs the filter at the head (and the ones chained on the same block). */
static FmErr lz_run_head(RarLz *z, RarOut *o) {
  RarFilter *f = &z->flt[z->fhead];
  u64 start = f->start;
  u32 len = f->len, n = len;
  size_t need = 2 * (size_t)len + 16;
  size_t off = (size_t)(start & z->wmask), first;
  FmErr err;
  if (need > z->fbuf_cap) {
    z->fbuf = (u8 *)fm_realloc(z->fbuf, need);
    z->fbuf_cap = need;
  }
  first = (size_t)z->wsize - off;
  if (first >= len) {
    memcpy(z->fbuf, z->win + off, len);
  } else {
    memcpy(z->fbuf, z->win + off, first);
    memcpy(z->fbuf + first, z->win, len - first);
  }
  memset(z->fbuf + len, 0, 8);
  for (;;) {
    if (f->dlen > n) return FM_ERR_FORMAT;
    n = f->dlen;
    if ((err = flt_exec(z, f, n, (u32)(start - z->fstart))) != FM_OK) return err;
    z->fhead++;
    if (z->fhead < z->nflt && z->flt[z->fhead].start == start && z->flt[z->fhead].len == n) {
      f = &z->flt[z->fhead];
      continue;
    }
    break;
  }
  if (z->fhead == z->nflt) z->fhead = z->nflt = 0;
  z->wr = start + len;
  return out_put(o, z->fbuf, n);
}

static FmErr lz_flush(RarLz *z, RarOut *o) {
  FmErr err;
  u64 room = z->wsize - RAR_MARGIN, nf;
  while (z->wr < z->pos) {
    if (z->fhead < z->nflt) {
      RarFilter *f = &z->flt[z->fhead];
      if (f->start < z->wr) return FM_ERR_FORMAT;
      if (f->start > z->wr) {
        u64 e = f->start < z->pos ? f->start : z->pos;
        if ((err = lz_emit(z, o, z->wr, e)) != FM_OK) return err;
        z->wr = e;
        continue;
      }
      if (z->pos - f->start < f->len) break;
      if ((err = lz_run_head(z, o)) != FM_OK) return err;
      continue;
    }
    if ((err = lz_emit(z, o, z->wr, z->pos)) != FM_OK) return err;
    z->wr = z->pos;
  }
  if (!o->write && sink_cancelled(o->s)) return FM_ERR_CANCEL;
  nf = z->wr + z->chunk;
  if (z->fhead < z->nflt && z->flt[z->fhead].start == z->wr) {
    if (z->flt[z->fhead].len > room) return FM_ERR_FORMAT;    /* cannot fit the window */
    nf = z->wr + z->flt[z->fhead].len;
  }
  z->next_flush = nf;
  return FM_OK;
}

static void lz_file_start(RarLz *z) {
  z->fhead = z->nflt = 0;
  z->fstart = z->wr = z->pos;
  z->next_flush = z->pos + z->chunk;
}

static FmErr lz_finish(RarLz *z, RarOut *o) {
  FmErr err = lz_flush(z, o);
  if (err != FM_OK) return err;
  if (z->wr != z->pos) return FM_ERR_FORMAT;    /* a filter block was cut short */
  z->fhead = z->nflt = 0;
  return FM_OK;
}

/* ---- RAR 2.9/3.x decoder (unpack29) ------------------------------------ */

typedef struct Rar3Prog {
  u32 oldlen;
  u8 type;
} Rar3Prog;

typedef struct Rar3 {
  RarHuff main, dist, low, rlen;
  u8 lens[RAR3_TABLES];          /* previous code lengths: tables are delta coded */
  u32 old[4], lastdist, lastlen, lowdist, lowrep;
  bool tables, ppm, ppm_ok;
  int esc;
  CPpmd7 ppmd;
  RarByteIn bin;
  Rar3Prog progs[RAR_MAXPROGS];
  u32 nprogs, lastfilter;
  u8 code[0x10000];              /* filter bytes */
} Rar3;

typedef struct RarDec {
  FmArc *a;
  RarIn in;
  RarLz z;
  RarOut o;
  Rar3 *r3;
  struct Rar5 *r5;
} RarDec;

static const u8 kLenBase[28] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128,
  160, 192, 224
};
static const u8 kLenBits[28] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5
};
static const u32 kDistBase[60] = {
  0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536,
  2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072,
  196608, 262144, 327680, 393216, 458752, 524288, 589824, 655360, 720896, 786432, 851968,
  917504, 983040, 1048576, 1310720, 1572864, 1835008, 2097152, 2359296, 2621440, 2883584,
  3145728, 3407872, 3670016, 3932160
};
static const u8 kDistBits[60] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
  13, 14, 14, 15, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 18, 18, 18, 18,
  18, 18, 18, 18, 18, 18, 18, 18
};
static const u8 kShortBase[8] = { 0, 4, 8, 16, 32, 64, 128, 192 };
static const u8 kShortBits[8] = { 2, 2, 3, 4, 5, 6, 6, 6 };

static void r3_reset(Rar3 *r) {
  memset(r->lens, 0, sizeof r->lens);
  memset(r->old, 0, sizeof r->old);
  r->lastdist = r->lastlen = r->lowdist = r->lowrep = 0;
  r->tables = r->ppm = r->ppm_ok = false;
  r->esc = 2;
  r->nprogs = r->lastfilter = 0;
}

static FmErr r3_ppm_init(RarDec *d) {
  Rar3 *r = d->r3;
  RarIn *in = &d->in;
  u32 flags = in_bits(in, 7), mb = 0;
  if (flags & 0x20) mb = in_bits(in, 8);
  else if (!r->ppm_ok) return FM_ERR_FORMAT;
  if (flags & 0x40) r->esc = (int)in_bits(in, 8);
  r->bin.in = in;
  r->ppmd.rc.dec.Stream = &r->bin.vt;
  if (!Ppmd7a_RangeDec_Init(&r->ppmd.rc.dec)) return FM_ERR_FORMAT;
  if (flags & 0x20) {
    u32 order = (flags & 0x1f) + 1;
    r->ppm_ok = false;
    if (order > 16) order = 16 + (order - 16) * 3;
    if (order < PPMD7_MIN_ORDER) return FM_ERR_FORMAT;
    if (!Ppmd7_Alloc(&r->ppmd, (mb + 1) << 20, &kRarAlloc)) return FM_ERR_NOMEM;
    Ppmd7_Init(&r->ppmd, order);
    r->ppm_ok = true;
  }
  r->ppm = true;
  return in_over(in, 0) ? FM_ERR_FORMAT : FM_OK;
}

static FmErr r3_tables(RarDec *d) {
  Rar3 *r = d->r3;
  RarIn *in = &d->in;
  RarHuff *pre;
  u8 bl[20];
  int i = 0;
  in_align(in);
  if (in_bits(in, 1)) return r3_ppm_init(d);
  r->ppm = false;
  r->lowdist = r->lowrep = 0;
  if (!in_bits(in, 1)) memset(r->lens, 0, sizeof r->lens);
  while (i < 20) {
    u32 l = in_bits(in, 4);
    if (l == 15) {
      u32 zc = in_bits(in, 4);
      if (zc) {
        zc += 2;
        while (zc-- && i < 20) bl[i++] = 0;
        continue;
      }
    }
    bl[i++] = (u8)l;
  }
  pre = &r->rlen;     /* rebuilt below, so it serves as the precode table */
  huff_build(pre, bl, 20);
  for (i = 0; i < RAR3_TABLES;) {
    u32 v = huff_dec(pre, in), n;
    if (v < 16) {
      r->lens[i] = (u8)((r->lens[i] + v) & 15);
      i++;
    } else if (v < 18) {
      n = v == 16 ? in_bits(in, 3) + 3 : in_bits(in, 7) + 11;
      if (i == 0) return FM_ERR_FORMAT;
      while (n-- && i < RAR3_TABLES) { r->lens[i] = r->lens[i - 1]; i++; }
    } else {
      n = v == 18 ? in_bits(in, 3) + 3 : in_bits(in, 7) + 11;
      while (n-- && i < RAR3_TABLES) r->lens[i++] = 0;
    }
  }
  if (in_over(in, 0)) return FM_ERR_FORMAT;
  huff_build(&r->main, r->lens, 299);
  huff_build(&r->dist, r->lens + 299, 60);
  huff_build(&r->low, r->lens + 299 + 60, 17);
  huff_build(&r->rlen, r->lens + 299 + 60 + 17, 28);
  r->tables = true;
  return FM_OK;
}

/* RarVM bytecode is only identified, never run: the standard filters are
** recognised by length and CRC32, like unrar and libarchive do. */
static u8 r3_std_filter(u32 len, u32 crc) {
  static const struct { u32 len, crc; u8 type; } kStd[] = {
    { 53, 0xad576887u, F_E8 },     { 57, 0x3cd7e57eu, F_E8E9 },  { 120, 0x3769893fu, F_ITANIUM },
    { 29, 0x0e06077du, F_DELTA },  { 149, 0x1c2c5dc8u, F_RGB },  { 216, 0xbc85e701u, F_AUDIO },
  };
  int i;
  for (i = 0; i < FM_COUNT(kStd); i++)
    if (kStd[i].len == len && kStd[i].crc == crc) return kStd[i].type;
  return F_NONE;
}

typedef struct MemBits {
  const u8 *p;
  u32 n, off;
  u64 bits;
  int avail;
  bool eof;
} MemBits;

static u32 mb_bits(MemBits *b, int k) {
  while (b->avail < k) {
    if (b->off >= b->n) { b->eof = true; return 0; }
    b->bits = (b->bits << 8) | b->p[b->off++];
    b->avail += 8;
  }
  b->avail -= k;
  return (u32)(b->bits >> b->avail) & (u32)(((u64)1 << k) - 1);
}

static u32 mb_num(MemBits *b) {
  u32 v;
  switch (mb_bits(b, 2)) {
    case 0: return mb_bits(b, 4);
    case 1:
      v = mb_bits(b, 8);
      if (v >= 16) return v;
      return 0xFFFFFF00u | (v << 4) | mb_bits(b, 4);
    case 2: return mb_bits(b, 16);
    default: return mb_bits(b, 32);
  }
}

static FmErr r3_add_filter(RarDec *d, u32 n, u32 flags) {
  Rar3 *r = d->r3;
  MemBits b;
  RarFilter f;
  u32 num, regs[7], i, len;
  Rar3Prog *prog;
  memset(&b, 0, sizeof b);
  b.p = r->code;
  b.n = n;
  memset(regs, 0, sizeof regs);
  if (flags & 0x80) {
    num = mb_num(&b);
    if (num == 0) {
      r->nprogs = 0;
      d->z.fhead = d->z.nflt = 0;
    } else {
      num--;
    }
    if (num > r->nprogs) return FM_ERR_FORMAT;
    r->lastfilter = num;
  } else {
    num = r->lastfilter;
  }
  if (num > r->nprogs) return FM_ERR_FORMAT;
  prog = num < r->nprogs ? &r->progs[num] : NULL;
  memset(&f, 0, sizeof f);
  f.start = d->z.pos + mb_num(&b);
  if (flags & 0x40) f.start += 258;
  len = (flags & 0x20) ? mb_num(&b) : (prog ? prog->oldlen : 0);
  regs[4] = len;
  if (flags & 0x10) {
    u32 mask = mb_bits(&b, 7);
    for (i = 0; i < 7; i++)
      if (mask & (1u << i)) regs[i] = mb_num(&b);
  }
  if (!prog) {
    u32 clen = mb_num(&b), crc, x = 0;
    u8 c0;
    if (clen == 0 || clen > 0x10000 || r->nprogs >= RAR_MAXPROGS) return FM_ERR_FORMAT;
    c0 = (u8)mb_bits(&b, 8);
    crc = crc32_update(0, &c0, 1);
    for (i = 1; i < clen; i++) {
      u8 c = (u8)mb_bits(&b, 8);
      x ^= c;
      crc = crc32_update(crc, &c, 1);
    }
    if (b.eof || x != c0) return FM_ERR_FORMAT;
    prog = &r->progs[r->nprogs++];
    prog->type = r3_std_filter(clen, crc);
  }
  prog->oldlen = len;
  if (flags & 0x08) {
    u32 glen = mb_num(&b);
    if (glen > 0x2000 - 0x40) return FM_ERR_FORMAT;
    for (i = 0; i < glen; i++) mb_bits(&b, 8);
  }
  if (b.eof) return FM_ERR_FORMAT;
  if (len > RAR3_VMSIZE) return FM_ERR_FORMAT;
  f.len = len;
  f.dlen = regs[4];
  f.arg0 = regs[0];
  f.arg1 = regs[1];
  f.type = prog->type;
  return lz_add_filter(&d->z, &f);
}

/* One byte of filter description, from the bit stream or from PPMd. */
static int r3_byte(RarDec *d, bool ppm) {
  if (ppm) return Ppmd7a_DecodeSymbol(&d->r3->ppmd);
  return (int)in_bits(&d->in, 8);
}

static FmErr r3_read_filter(RarDec *d, bool ppm) {
  int b = r3_byte(d, ppm), b2;
  u32 flags, len, i;
  if (b < 0) return FM_ERR_FORMAT;
  flags = (u32)b;
  len = (flags & 7) + 1;
  if (len == 7) {
    if ((b = r3_byte(d, ppm)) < 0) return FM_ERR_FORMAT;
    len = (u32)b + 7;
  } else if (len == 8) {
    if ((b = r3_byte(d, ppm)) < 0 || (b2 = r3_byte(d, ppm)) < 0) return FM_ERR_FORMAT;
    len = ((u32)b << 8) | (u32)b2;
  }
  for (i = 0; i < len; i++) {
    if ((b = r3_byte(d, ppm)) < 0) return FM_ERR_FORMAT;
    d->r3->code[i] = (u8)b;
  }
  if (in_over(&d->in, 16)) return FM_ERR_FORMAT;
  return r3_add_filter(d, len, flags);
}

static FmErr r3_decode(RarDec *d, const RarItem *it) {
  Rar3 *r = d->r3;
  RarIn *in = &d->in;
  RarLz *z = &d->z;
  u64 lim = it->size + RAR_SLACK;
  FmErr err;
  if (!it->solid) r3_reset(r);
  lz_file_start(z);
  if ((!it->solid || !r->tables) && (err = r3_tables(d)) != FM_OK) return err;
  for (;;) {
    u32 sym, dist, len;
    if (z->pos >= z->next_flush && (err = lz_flush(z, &d->o)) != FM_OK) return err;
    if (z->pos - z->fstart > lim) break;
    if (in_over(in, 16)) {
      if (z->pos - z->fstart >= it->size) break;
      return FM_ERR_FORMAT;
    }
    if (r->ppm) {
      int c = Ppmd7a_DecodeSymbol(&r->ppmd), n, c2, k;
      if (c >= 0 && c == r->esc) {
        n = Ppmd7a_DecodeSymbol(&r->ppmd);
        if (n < 0) c = n;
        else if (n == 0) {
          if ((err = r3_tables(d)) != FM_OK) return err;
          continue;
        } else if (n == 2) {
          break;
        } else if (n == 3) {
          if ((err = r3_read_filter(d, true)) != FM_OK) return err;
          continue;
        } else if (n == 4) {
          dist = 0;
          for (k = 0; k < 3; k++) {
            if ((c2 = Ppmd7a_DecodeSymbol(&r->ppmd)) < 0) return FM_ERR_FORMAT;
            dist = (dist << 8) | (u32)c2;
          }
          if ((c2 = Ppmd7a_DecodeSymbol(&r->ppmd)) < 0) return FM_ERR_FORMAT;
          lz_copy(z, (u64)dist + 2, (u32)c2 + 32);
          continue;
        } else if (n == 5) {
          if ((c2 = Ppmd7a_DecodeSymbol(&r->ppmd)) < 0) return FM_ERR_FORMAT;
          lz_copy(z, 1, (u32)c2 + 4);
          continue;
        }
      }
      if (c < 0) {
        if (z->pos - z->fstart >= it->size) break;
        return FM_ERR_FORMAT;
      }
      lz_put(z, (u32)c);
      continue;
    }
    sym = huff_dec(&r->main, in);
    if (sym < 256) {
      lz_put(z, sym);
      continue;
    }
    if (sym == 256) {
      if (in_bits(in, 1)) {
        if ((err = r3_tables(d)) != FM_OK) return err;
        continue;
      }
      r->tables = in_bits(in, 1) == 0;     /* "01": new tables open the next file */
      break;
    }
    if (sym == 257) {
      if ((err = r3_read_filter(d, false)) != FM_OK) return err;
      continue;
    }
    if (sym == 258) {
      if (r->lastlen == 0) continue;
      dist = r->lastdist;
      len = r->lastlen;
    } else if (sym < 263) {
      u32 idx = sym - 259, ls, i;
      dist = r->old[idx];
      ls = huff_dec(&r->rlen, in);
      if (ls >= 28) return FM_ERR_FORMAT;
      len = kLenBase[ls] + 2 + in_bits(in, kLenBits[ls]);
      for (i = idx; i > 0; i--) r->old[i] = r->old[i - 1];
      r->old[0] = dist;
    } else if (sym < 271) {
      u32 k = sym - 263;
      dist = kShortBase[k] + 1 + in_bits(in, kShortBits[k]);
      len = 2;
      r->old[3] = r->old[2]; r->old[2] = r->old[1]; r->old[1] = r->old[0]; r->old[0] = dist;
    } else if (sym < 299) {
      u32 k = sym - 271, ds, nb;
      len = kLenBase[k] + 3 + in_bits(in, kLenBits[k]);
      ds = huff_dec(&r->dist, in);
      if (ds >= 60) return FM_ERR_FORMAT;
      dist = kDistBase[ds] + 1;
      nb = kDistBits[ds];
      if (nb > 0) {
        if (ds > 9) {
          if (nb > 4) dist += in_bits(in, (int)nb - 4) << 4;
          if (r->lowrep > 0) {
            r->lowrep--;
            dist += r->lowdist;
          } else {
            u32 low = huff_dec(&r->low, in);
            if (low == 16) {
              r->lowrep = 15;
              dist += r->lowdist;
            } else {
              dist += low;
              r->lowdist = low;
            }
          }
        } else {
          dist += in_bits(in, (int)nb);
        }
      }
      if (dist >= 0x2000) len++;
      if (dist >= 0x40000) len++;
      r->old[3] = r->old[2]; r->old[2] = r->old[1]; r->old[1] = r->old[0]; r->old[0] = dist;
    } else {
      return FM_ERR_FORMAT;
    }
    r->lastdist = dist;
    r->lastlen = len;
    lz_copy(z, dist, len);
  }
  return lz_finish(z, &d->o);
}

/* ---- RAR 5.0 decoder --------------------------------------------------- */

typedef struct Rar5 {
  RarHuff ld, dd, ldd, rd, bd;
  u64 dist[4];
  u32 lastlen;
  bool tables;
} Rar5;

static FmErr r5_tables(RarDec *d) {
  Rar5 *r = d->r5;
  RarIn *in = &d->in;
  u8 bl[20], t[RAR5_TABLES];
  int w = 0, i;
  while (w < 20) {
    u32 v = in_bits(in, 4);
    if (v == 15) {
      u32 n = in_bits(in, 4);
      if (n == 0) {
        bl[w++] = 15;
      } else {
        n += 2;
        while (n-- && w < 20) bl[w++] = 0;
      }
    } else {
      bl[w++] = (u8)v;
    }
  }
  huff_build(&r->bd, bl, 20);
  for (i = 0; i < RAR5_TABLES;) {
    u32 v = huff_dec(&r->bd, in), n;
    if (v < 16) {
      t[i++] = (u8)v;
    } else if (v < 18) {
      n = v == 16 ? in_bits(in, 3) + 3 : in_bits(in, 7) + 11;
      if (i == 0) return FM_ERR_FORMAT;
      while (n-- && i < RAR5_TABLES) { t[i] = t[i - 1]; i++; }
    } else {
      n = v == 18 ? in_bits(in, 3) + 3 : in_bits(in, 7) + 11;
      while (n-- && i < RAR5_TABLES) t[i++] = 0;
    }
  }
  huff_build(&r->ld, t, 306);
  huff_build(&r->dd, t + 306, 64);
  huff_build(&r->ldd, t + 306 + 64, 16);
  huff_build(&r->rd, t + 306 + 64 + 16, 44);
  r->tables = true;
  return FM_OK;
}

static u32 r5_len(RarIn *in, u32 code) {
  u32 lbits;
  if (code < 8) return 2 + code;
  lbits = code / 4 - 1;
  return 2 + ((4 | (code & 3)) << lbits) + in_bits(in, (int)lbits);
}

static u32 r5_fdata(RarIn *in) {
  u32 n = in_bits(in, 2) + 1, v = 0, i;
  for (i = 0; i < n; i++) v |= in_bits(in, 8) << (8 * i);
  return v;
}

static FmErr r5_filter(RarDec *d) {
  RarIn *in = &d->in;
  RarLz *z = &d->z;
  RarFilter f;
  u32 start = r5_fdata(in), len = r5_fdata(in), type = in_bits(in, 3);
  static const u8 kType[4] = { F_DELTA, F_E8, F_E8E9, F_ARM };
  memset(&f, 0, sizeof f);
  if (type > 3 || len > RAR5_MAXFLT) return FM_ERR_FORMAT;
  if (type == 0) f.arg0 = in_bits(in, 5) + 1;
  f.type = kType[type];
  f.start = z->pos + start;
  f.len = f.dlen = len;
  if (z->nflt > z->fhead) {
    const RarFilter *p = &z->flt[z->nflt - 1];
    if (f.start < p->start + p->len) return FM_ERR_FORMAT;
  }
  return lz_add_filter(z, &f);
}

static FmErr r5_decode(RarDec *d, const RarItem *it) {
  Rar5 *r = d->r5;
  RarIn *in = &d->in;
  RarLz *z = &d->z;
  u64 lim = it->size + RAR_SLACK;
  FmErr err;
  if (!it->solid) {
    memset(r->dist, 0, sizeof r->dist);
    r->lastlen = 0;
    r->tables = false;
  }
  lz_file_start(z);
  for (;;) {
    u32 fl, ck, nb, bsize = 0, i;
    u64 bstart, bend, bnext;
    in_align(in);
    if (in_bitpos(in) + 24 > in->real * 8) return FM_ERR_FORMAT;
    fl = in_bits(in, 8);
    ck = in_bits(in, 8);
    nb = ((fl >> 3) & 7) + 1;
    if (nb > 3) return FM_ERR_FORMAT;
    for (i = 0; i < nb; i++) bsize |= in_bits(in, 8) << (8 * i);
    if (((0x5A ^ fl ^ bsize ^ (bsize >> 8) ^ (bsize >> 16)) & 0xFF) != ck) return FM_ERR_FORMAT;
    bstart = in_bitpos(in);
    bnext = bstart + (u64)bsize * 8;
    if (bnext > in->real * 8) return FM_ERR_FORMAT;
    bend = bsize ? bstart + (u64)(bsize - 1) * 8 + (fl & 7) + 1 : bstart;
    if (fl & 0x80) {
      if ((err = r5_tables(d)) != FM_OK) return err;
      if (in_bitpos(in) > bend) return FM_ERR_FORMAT;
    }
    if (!r->tables) return FM_ERR_FORMAT;
    while (in_bitpos(in) < bend) {
      u32 num;
      if (z->pos >= z->next_flush && (err = lz_flush(z, &d->o)) != FM_OK) return err;
      if (z->pos - z->fstart > lim) return FM_ERR_FORMAT;
      num = huff_dec(&r->ld, in);
      if (num < 256) {
        lz_put(z, num);
      } else if (num >= 262) {
        u32 len = r5_len(in, num - 262), slot = huff_dec(&r->dd, in);
        u64 dist = 1;
        if (slot < 4) {
          dist += slot;
        } else {
          u32 dbits = slot / 2 - 1;
          dist += (u64)(2 | (slot & 1)) << dbits;
          if (dbits >= 4) {
            if (dbits > 4) dist += (u64)in_bits(in, (int)dbits - 4) << 4;
            dist += huff_dec(&r->ldd, in);
          } else {
            dist += in_bits(in, (int)dbits);
          }
        }
        if (dist > 0x100) {
          len++;
          if (dist > 0x2000) {
            len++;
            if (dist > 0x40000) len++;
          }
        }
        r->dist[3] = r->dist[2]; r->dist[2] = r->dist[1]; r->dist[1] = r->dist[0]; r->dist[0] = dist;
        r->lastlen = len;
        lz_copy(z, dist, len);
      } else if (num == 256) {
        if ((err = r5_filter(d)) != FM_OK) return err;
      } else if (num == 257) {
        if (r->lastlen) lz_copy(z, r->dist[0], r->lastlen);
      } else {
        u32 idx = num - 258, len, k;
        u64 dist = r->dist[idx];
        for (k = idx; k > 0; k--) r->dist[k] = r->dist[k - 1];
        r->dist[0] = dist;
        len = r5_len(in, huff_dec(&r->rd, in));
        r->lastlen = len;
        lz_copy(z, dist, len);
      }
    }
    if (in_bitpos(in) > bend) return FM_ERR_FORMAT;
    in_skip_to(in, bnext);
    if (fl & 0x40) break;
  }
  return lz_finish(z, &d->o);
}

/* ---- RAR 1.5-4.x headers ----------------------------------------------- */

static i64 dos_time(u32 t) {
  struct tm tm;
  time_t r;
  if (t == 0) return 0;
  memset(&tm, 0, sizeof tm);
  tm.tm_sec = (int)(t & 31) * 2;
  tm.tm_min = (int)(t >> 5) & 63;
  tm.tm_hour = (int)(t >> 11) & 31;
  tm.tm_mday = (int)(t >> 16) & 31;
  tm.tm_mon = (int)((t >> 21) & 15) - 1;
  tm.tm_year = (int)((t >> 25) & 127) + 80;
  tm.tm_isdst = -1;
  r = mktime(&tm);
  return r == (time_t)-1 ? 0 : (i64)r;
}

/* RAR4 Unicode names: an ASCII name, a NUL, then UTF-16 packed against it. */
static void r4_unicode_name(const u8 *p, u32 n, u16 *u) {
  u32 off = (u32)strlen((const char *)p) + 1, cnt = 0, high = 0, flagbits = 0, flagbyte = 0;
  if (off < n) high = p[off++];
  while (off < n && cnt < n) {
    if (!flagbits) { flagbyte = p[off++]; flagbits = 8; }
    flagbits -= 2;
    switch ((flagbyte >> flagbits) & 3) {
      case 0:
        if (off >= n) continue;
        u[cnt++] = p[off++];
        break;
      case 1:
        if (off >= n) continue;
        u[cnt++] = (u16)((high << 8) | p[off++]);
        break;
      case 2:
        if (off + 1 >= n) { off = n; continue; }
        u[cnt++] = (u16)(p[off] | (p[off + 1] << 8));
        off += 2;
        break;
      default: {
        u32 len, extra = 0, hb = 0;
        if (off >= n) continue;
        len = p[off++];
        if (len & 0x80) {
          if (off >= n) continue;
          extra = p[off++];
          hb = high;
        }
        len = (len & 0x7f) + 2;
        while (len-- && cnt < n) {
          u[cnt] = (u16)((hb << 8) | ((p[cnt] + extra) & 0xFF));
          cnt++;
        }
        break;
      }
    }
  }
  u[cnt] = 0;
}

static FmErr rar_push(FmArc *a, RarPriv *pv, const char *name, const RarItem *it, i64 mtime,
                      u32 mode, bool encrypted, bool is_link, u8 method) {
  FmArcEntry *e;
  int idx = arc_add(a, name, it->size, it->packed, mtime, it->is_dir != 0);
  if (idx < 0) return FM_ERR_NOMEM;
  if (idx != pv->n) return FM_ERR_FORMAT;
  if (pv->n == pv->cap) {
    pv->cap = pv->cap ? pv->cap * 2 : 32;
    pv->it = (RarItem *)fm_realloc(pv->it, (size_t)pv->cap * sizeof *pv->it);
  }
  pv->it[pv->n++] = *it;
  e = arc_at(a, idx);
  if (e) {
    e->crc = it->crc;
    e->mode = mode;
    e->encrypted = encrypted;
    e->is_link = is_link;
    e->method = method;
  }
  if (encrypted) a->any_encrypted = true;
  return FM_OK;
}

static FmErr r4_file(FmArc *a, RarPriv *pv, const u8 *h, u32 hsize, i64 off, u64 pack) {
  u32 fl = le16(h + 3), host = h[15], ver = h[24], meth = h[25], nsize = le16(h + 26);
  u32 attr = le32(h + 28), p = 32, mode;
  i64 mtime = dos_time(le32(h + 20));
  u64 unp = le32(h + 11);
  RarItem it;
  char *name;
  u8 *tmp;
  size_t cap;
  bool is_link = false;
  FmErr err;
  memset(&it, 0, sizeof it);
  if (fl & 0x100) {
    if (hsize < 40) return FM_ERR_FORMAT;
    unp |= (u64)le32(h + 36) << 32;
    p = 40;
  }
  if (p + nsize > hsize || nsize == 0) return FM_ERR_FORMAT;
  cap = (size_t)nsize * 3 + 8;
  name = (char *)fm_alloc(cap);
  tmp = (u8 *)fm_alloc((size_t)nsize + 1);
  memcpy(tmp, h + p, nsize);
  tmp[nsize] = 0;
  if ((fl & 0x200) && memchr(tmp, 0, nsize)) {
    u16 *u = (u16 *)fm_alloc(((size_t)nsize + 1) * sizeof(u16));
    r4_unicode_name(tmp, nsize, u);
    utf16_to_utf8(u, (int)nsize, name, cap);
    fm_free(u);
  } else {
    bytes_to_utf8(tmp, nsize, name, cap);
  }
  fm_free(tmp);
  p += nsize;
  if (fl & 0x400) p += 8;
  if ((fl & 0x1000) && p + 2 <= hsize && ((le16(h + p) >> 12) & 0xC) == 0xC) mtime += 1;

  if (host <= 2) {
    it.is_dir = (attr & 0x10) != 0;
    mode = (it.is_dir ? (MODE_DIR | 0755) : (MODE_REG | 0644)) & ~((attr & 1) ? 0222u : 0u);
  } else {
    mode = attr & 0xFFFF;
    it.is_dir = (mode & MODE_FMT) == MODE_DIR;
    is_link = (mode & MODE_FMT) == MODE_LNK;
  }
  if ((fl & 0xE0) == 0xE0) it.is_dir = 1;
  it.data = off + hsize;
  it.packed = pack;
  it.size = it.is_dir ? 0 : unp;
  it.crc = le32(h + 16);
  it.has_crc = !it.is_dir;
  it.solid = (fl & 0x10) != 0;
  it.dict = (u64)64 * 1024 << ((fl >> 5) & 7);
  if (it.dict > RAR3_MAXDICT) it.dict = RAR3_MAXDICT;
  if (meth == 0x30) it.method = M_STORE;
  else if (meth >= 0x31 && meth <= 0x35 && ver >= 29 && ver <= 36) it.method = M_LZ29;
  else { it.method = M_LZ29; it.bad = BAD_METHOD; }
  if (fl & 0x03) it.bad = BAD_SPLIT;
  if (fl & 0x04) it.bad = BAD_CRYPT;
  if (it.is_dir) it.bad = BAD_NONE;
  err = rar_push(a, pv, name, &it, mtime, mode, (fl & 4) != 0, is_link,
                 (u8)(meth >= 0x30 ? meth - 0x30 : meth));
  fm_free(name);
  return err;
}

/* RAR 1.5-2.x keep comments inside the main and file headers; their CRC
** then covers only the part before the comment. */
static bool r4_crc_ok(const u8 *h, u32 hsize) {
  u32 fl = le16(h + 3), n = hsize;
  if ((crc32_update(0, h + 2, hsize - 2) & 0xFFFF) == le16(h)) return true;
  if (h[2] == 0x73 && (fl & 0x02)) n = 13;
  else if (h[2] == 0x74 && (fl & 0x08)) n = ((fl & 0x100) ? 40 : 32) + le16(h + 26);
  if (n >= hsize) return false;
  return (crc32_update(0, h + 2, n - 2) & 0xFFFF) == le16(h);
}

static FmErr r4_list(FmArc *a, RarPriv *pv, i64 off) {
  u8 *h = (u8 *)fm_alloc(65536 + 16);
  FmErr err = FM_OK;
  while (off + 7 <= a->file_size) {
    u32 type, fl, hsize;
    u64 add = 0;
    i64 next;
    if (!read_at(a->f, off, h, 7)) break;
    type = h[2];
    fl = le16(h + 3);
    hsize = le16(h + 5);
    if (hsize < 7) { err = FM_ERR_FORMAT; break; }
    if (off + hsize > a->file_size) break;            /* truncated: keep what was listed */
    if (!read_at(a->f, off, h, hsize)) { err = FM_ERR_IO; break; }
    if (type == 0x74 || type == 0x7a) {
      if (hsize < 32) { err = FM_ERR_FORMAT; break; }
      add = le32(h + 7);
      if (fl & 0x100) {
        if (hsize < 40) { err = FM_ERR_FORMAT; break; }
        add |= (u64)le32(h + 32) << 32;
      }
    } else if (fl & 0x8000) {
      if (hsize < 11) { err = FM_ERR_FORMAT; break; }
      add = le32(h + 7);
    }
    if ((type == 0x73 || type == 0x74) && !r4_crc_ok(h, hsize)) {
      err = FM_ERR_FORMAT;
      break;
    }
    if (type == 0x73) {
      if (fl & 0x80) {          /* the file list itself is encrypted */
        a->any_encrypted = true;
        fm_log("rar: encrypted headers are not supported");
        err = FM_ERR_UNSUPPORTED;
        break;
      }
      pv->solid = (fl & 0x08) != 0;
    } else if (type == 0x74) {
      if ((err = r4_file(a, pv, h, hsize, off, add)) != FM_OK) break;
    } else if (type == 0x7b) {
      break;
    }
    if (add > (u64)a->file_size) break;
    next = off + (i64)hsize + (i64)add;
    if (next <= off) { err = FM_ERR_FORMAT; break; }
    off = next;
  }
  fm_free(h);
  return err;
}

/* ---- RAR 5.0 headers ---------------------------------------------------- */

#define R5_MAXHDR (2 * 1024 * 1024)

static FmErr r5_file(FmArc *a, RarPriv *pv, const u8 *p, const u8 *end, u64 extra, u32 hfl,
                     i64 data_off, u64 data_size) {
  const u8 *xs;
  u64 fflags, unp, attr, comp, host, nlen;
  u32 crc = 0, mtime32 = 0, mode;
  i64 mtime = 0;
  RarItem it;
  char *name, *link = NULL;
  bool encrypted = false, is_link = false;
  u32 method, version;
  FmErr err;
  if (extra > (u64)(end - p)) return FM_ERR_FORMAT;
  xs = end - extra;
  memset(&it, 0, sizeof it);
  if (!vint(&p, xs, &fflags) || !vint(&p, xs, &unp) || !vint(&p, xs, &attr)) return FM_ERR_FORMAT;
  if (fflags & 2) {
    if (xs - p < 4) return FM_ERR_FORMAT;
    mtime32 = le32(p); p += 4;
    mtime = (i64)mtime32;
  }
  if (fflags & 4) {
    if (xs - p < 4) return FM_ERR_FORMAT;
    crc = le32(p); p += 4;
    it.has_crc = 1;
  }
  if (!vint(&p, xs, &comp) || !vint(&p, xs, &host) || !vint(&p, xs, &nlen)) return FM_ERR_FORMAT;
  if (nlen == 0 || nlen > (u64)(xs - p)) return FM_ERR_FORMAT;
  name = (char *)fm_alloc((size_t)nlen + 1);
  memcpy(name, p, (size_t)nlen);
  name[nlen] = 0;

  /* extra area: records of (size, type, data) */
  p = xs;
  while (p < end) {
    u64 rsize, rtype;
    const u8 *rend;
    if (!vint(&p, end, &rsize) || rsize == 0 || rsize > (u64)(end - p)) break;
    rend = p + rsize;
    if (!vint(&p, rend, &rtype)) break;
    if (rtype == 1) {
      encrypted = true;
    } else if (rtype == 2) {
      u64 ht;
      if (vint(&p, rend, &ht) && ht == 0 && rend - p >= 32) {
        memcpy(it.blake, p, 32);
        it.has_blake = 1;
      }
    } else if (rtype == 3) {
      u64 tf;
      if (vint(&p, rend, &tf) && (tf & 2)) {
        if (tf & 1) {
          if (rend - p >= 4) mtime = (i64)le32(p);
        } else if (rend - p >= 8) {
          u64 ft = (u64)le32(p) | (u64)le32(p + 4) << 32;
          mtime = (i64)(ft / 10000000u) - (i64)11644473600LL;
        }
      }
    } else if (rtype == 5) {
      u64 rt, rf, tl;
      if (vint(&p, rend, &rt) && vint(&p, rend, &rf) && vint(&p, rend, &tl) && tl > 0 &&
          tl <= (u64)(rend - p)) {
        if (rt >= 1 && rt <= 3) {
          char *t = (char *)fm_alloc((size_t)tl + 1);
          memcpy(t, p, (size_t)tl);
          t[tl] = 0;
          link = arena_strdup(&a->arena, t);
          fm_free(t);
          is_link = true;
        } else {
          it.bad = BAD_REF;        /* hard link / file copy: data lives in another entry */
        }
      }
    }
    p = rend;
  }

  it.is_dir = (fflags & 1) != 0;
  version = (u32)(comp & 0x3f);
  method = (u32)((comp >> 7) & 7);
  it.solid = (comp & 0x40) != 0;
  it.dict = (u64)128 * 1024 << ((comp >> 10) & 15);
  it.data = data_off;
  it.packed = data_size;
  it.size = it.is_dir ? 0 : unp;
  it.crc = crc;
  it.method = method == 0 ? M_STORE : M_LZ50;
  if (!it.is_dir && method != 0) {
    if (version != 0 || method > 5) it.bad = BAD_METHOD;
    else if (it.dict > RAR5_MAXDICT) it.bad = BAD_DICT;
  }
  if (fflags & 8) it.bad = BAD_METHOD;          /* unknown unpacked size */
  if (hfl & 0x18) it.bad = BAD_SPLIT;
  if (encrypted) it.bad = BAD_CRYPT;
  if (link) {
    it.link = link;
    it.size = strlen(link);
    it.has_crc = 0;
    it.has_blake = 0;
  }
  if (it.is_dir) it.bad = BAD_NONE;
  if (host == 0) {
    mode = (it.is_dir ? (MODE_DIR | 0755) : (MODE_REG | 0644)) & ~((attr & 1) ? 0222u : 0u);
    if (is_link) mode = MODE_LNK | 0777;
  } else {
    mode = (u32)attr & 0xFFFF;
    if (is_link) mode = (mode & ~MODE_FMT) | MODE_LNK;
  }
  err = rar_push(a, pv, name, &it, mtime, mode, encrypted, is_link, (u8)method);
  fm_free(name);
  return err;
}

static FmErr r5_list(FmArc *a, RarPriv *pv, i64 off) {
  u8 *buf = NULL;
  size_t cap = 0;
  FmErr err = FM_OK;
  while (off + 7 <= a->file_size) {
    u8 h0[16];
    const u8 *p, *end;
    u64 hsize, type, hfl, extra = 0, data = 0;
    size_t want = (size_t)FM_MIN((i64)sizeof h0, a->file_size - off), total, vlen;
    i64 next;
    if (!read_at(a->f, off, h0, want)) { err = FM_ERR_IO; break; }
    p = h0 + 4;
    if (!vint(&p, h0 + want, &hsize)) break;
    vlen = (size_t)(p - (h0 + 4));
    if (hsize == 0 || hsize > R5_MAXHDR) { err = FM_ERR_FORMAT; break; }
    total = 4 + vlen + (size_t)hsize;
    if (off + (i64)total > a->file_size) break;       /* truncated */
    if (total > cap) {
      cap = total;
      buf = (u8 *)fm_realloc(buf, cap);
    }
    if (!read_at(a->f, off, buf, total)) { err = FM_ERR_IO; break; }
    if (crc32_update(0, buf + 4, total - 4) != le32(buf)) { err = FM_ERR_FORMAT; break; }
    p = buf + 4 + vlen;
    end = buf + total;
    if (!vint(&p, end, &type) || !vint(&p, end, &hfl)) { err = FM_ERR_FORMAT; break; }
    if ((hfl & 1) && !vint(&p, end, &extra)) { err = FM_ERR_FORMAT; break; }
    if ((hfl & 2) && !vint(&p, end, &data)) { err = FM_ERR_FORMAT; break; }
    if (type == 1) {
      u64 af;
      if (!vint(&p, end, &af)) { err = FM_ERR_FORMAT; break; }
      pv->solid = (af & 4) != 0;
    } else if (type == 2) {
      if ((err = r5_file(a, pv, p, end, extra, (u32)hfl, off + (i64)total, data)) != FM_OK) break;
    } else if (type == 4) {
      a->any_encrypted = true;
      fm_log("rar: encrypted headers are not supported");
      err = FM_ERR_UNSUPPORTED;
      break;
    } else if (type == 5) {
      break;
    }
    if (data > (u64)a->file_size) break;
    next = off + (i64)total + (i64)data;
    off = next;
  }
  fm_free(buf);
  return err;
}

/* ---- backend ------------------------------------------------------------ */

static const u8 kSig4[7] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x00 };
static const u8 kSig5[8] = { 'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00 };

/* Finds the signature at the start or, for self-extracting archives,
** within the first 4 MB. Returns its offset and version, or -1. */
static i64 rar_find(FILE *f, i64 size, int *ver) {
  u8 *b = (u8 *)fm_alloc(RAR_INBUF + 8);
  i64 base = 0, found = -1;
  while (base < size && base < ((i64)4 << 20) && found < 0) {
    size_t n = (size_t)FM_MIN((i64)RAR_INBUF + 8, size - base), i;
    if (n < 7 || !read_at(f, base, b, n)) break;
    for (i = 0; i + 7 <= n; i++) {
      if (b[i] != 'R' || memcmp(b + i, kSig4, 6) != 0) continue;
      if (b[i + 6] == 0x00) { *ver = 4; found = base + (i64)i; break; }
      if (i + 8 <= n && memcmp(b + i, kSig5, 8) == 0) { *ver = 5; found = base + (i64)i; break; }
    }
    base += RAR_INBUF;
  }
  fm_free(b);
  return found;
}

static FmErr rar_open(FmArc *a) {
  RarPriv *pv;
  int ver = 0;
  i64 off = rar_find(a->f, a->file_size, &ver);
  if (off < 0) return FM_ERR_FORMAT;
  pv = (RarPriv *)fm_calloc(1, sizeof *pv);
  pv->ver = ver;
  a->priv = pv;
  return ver == 4 ? r4_list(a, pv, off + 7) : r5_list(a, pv, off + 8);
}

static void rar_close(FmArc *a) {
  RarPriv *pv = (RarPriv *)a->priv;
  if (!pv) return;
  fm_free(pv->it);
  fm_free(pv);
  a->priv = NULL;
}

static bool is_comp(const RarItem *it) {
  return !it->is_dir && !it->bad && !it->link && it->method != M_STORE && it->packed > 0;
}

/* True when the next compressed entry continues this one's stream. */
static bool continues(const RarPriv *pv, int i) {
  int j;
  for (j = i + 1; j < pv->n; j++) {
    const RarItem *n = &pv->it[j];
    if (n->is_dir || n->link || n->method == M_STORE || n->packed == 0) continue;
    return n->solid && !n->bad;
  }
  return false;
}

static u64 rar_window(const RarPriv *pv) {
  u64 need = RAR_MINWIN;
  int i;
  for (i = 0; i < pv->n; i++) {
    const RarItem *it = &pv->it[i];
    u64 n;
    if (!is_comp(it)) continue;
    n = it->dict;
    if (!pv->solid && !it->solid) {
      u64 f = pow2ceil(it->size + RAR_MARGIN);
      if (f < n) n = f;
    }
    if (n > need) need = n;
  }
  return pow2ceil(need);
}

static FmErr rar_prepare(RarDec *d, const RarPriv *pv) {
  RarLz *z = &d->z;
  if (!z->win) {
    u64 ws = rar_window(pv);
    z->win = (u8 *)big_calloc(ws);
    if (!z->win) return FM_ERR_NOMEM;
    z->wsize = ws;
    z->wmask = ws - 1;
    z->chunk = FM_MIN(ws / 4, (u64)1 << 20);
    z->rar5 = pv->ver == 5;
  }
  if (pv->ver == 4 && !d->r3) {
    d->r3 = (Rar3 *)fm_calloc(1, sizeof *d->r3);
    d->r3->bin.vt.Read = rar_ppmd_read;
    Ppmd7_Construct(&d->r3->ppmd);
    r3_reset(d->r3);
  }
  if (pv->ver == 5 && !d->r5) d->r5 = (struct Rar5 *)fm_calloc(1, sizeof(Rar5));
  return FM_OK;
}

static FmErr rar_store(RarDec *d, const RarItem *it) {
  u64 left = it->packed;
  FmErr err;
  if ((err = in_begin(&d->in, d->a->f, it->data, it->packed)) != FM_OK) return err;
  while (left > 0) {
    size_t n = left < RAR_INBUF ? (size_t)left : RAR_INBUF;
    if (fread(d->in.buf, 1, n, d->a->f) != n) return FM_ERR_FORMAT;
    if ((err = out_put(&d->o, d->in.buf, n)) != FM_OK) return err;
    left -= n;
    if (!d->o.write && sink_cancelled(d->o.s)) return FM_ERR_CANCEL;
  }
  return FM_OK;
}

static FmErr rar_item(RarDec *d, const RarPriv *pv, const RarItem *it, FmArcSink *s, bool write) {
  RarOut *o = &d->o;
  FmErr err;
  memset(o, 0, sizeof *o);
  o->s = s;
  o->write = write;
  o->size = it->size;
  o->blake = write && it->has_blake;
  if (o->blake) b2sp_init(&o->b2);
  if (it->link) {
    err = out_put(o, (const u8 *)it->link, strlen(it->link));
  } else if (it->packed == 0) {
    err = FM_OK;
  } else if (it->method == M_STORE) {
    err = rar_store(d, it);
  } else {
    if ((err = rar_prepare(d, pv)) != FM_OK) return err;
    if ((err = in_begin(&d->in, d->a->f, it->data, it->packed)) != FM_OK) return err;
    err = pv->ver == 4 ? r3_decode(d, it) : r5_decode(d, it);
  }
  if (err != FM_OK) return err;
  if (o->done < it->size) return FM_ERR_FORMAT;
  if (write && it->has_crc && o->crc != it->crc) return FM_ERR_CRC;
  if (o->blake) {
    u8 h[32];
    b2sp_final(&o->b2, h);
    if (memcmp(h, it->blake, 32) != 0) return FM_ERR_CRC;
  }
  return FM_OK;
}

static bool fatal(FmErr e) {
  return e == FM_ERR_CANCEL || e == FM_ERR_IO || e == FM_ERR_NOMEM || e == FM_ERR_FULL ||
         e == FM_ERR_ACCESS;
}

static FmErr rar_extract(FmArc *a, const u8 *sel, FmArcSink *s) {
  RarPriv *pv = (RarPriv *)a->priv;
  RarDec *d;
  FmErr result = FM_OK, chain_err = FM_OK;
  int i;
  FM_UNUSED(sel);
  if (!pv) return FM_ERR_FORMAT;
  d = (RarDec *)fm_calloc(1, sizeof *d);
  d->a = a;
  d->in.buf = (u8 *)fm_alloc(RAR_INBUF);
  for (i = 0; i < pv->n && sink_remaining(s) > 0; i++) {
    const RarItem *it = &pv->it[i];
    bool skip = true, comp = is_comp(it);
    FmErr err = sink_begin(s, i, &skip);
    if (err != FM_OK) { result = err; break; }
    if (it->is_dir) {
      if (!skip) sink_end(s, FM_OK);
      continue;
    }
    if (it->bad) {
      if (it->method != M_STORE && it->packed > 0) chain_err = FM_ERR_UNSUPPORTED;
      if (!skip) {
        static const char *const kWhy[] = { "", "encrypted", "continued in the next volume",
                                            "unsupported method", "dictionary over 1 GB",
                                            "hard link or file copy" };
        fm_log("rar: %s: %s", arc_at(a, i) ? arc_at(a, i)->path : "?", kWhy[it->bad]);
        sink_end(s, FM_ERR_UNSUPPORTED);
        if (result == FM_OK) result = FM_ERR_UNSUPPORTED;
      }
      continue;
    }
    if (skip && !(comp && continues(pv, i))) {
      if (comp) chain_err = FM_ERR_UNSUPPORTED;
      continue;
    }
    if (comp && !it->solid) chain_err = FM_OK;
    if (comp && it->solid && chain_err != FM_OK) {
      err = chain_err;
    } else {
      err = rar_item(d, pv, it, s, !skip);
      if (comp) chain_err = err;
    }
    if (!skip) err = sink_end(s, err);
    if (err != FM_OK) {
      if (result == FM_OK || fatal(err)) result = err;
      if (fatal(err)) break;
    }
  }
  free(d->z.win);
  fm_free(d->z.flt);
  fm_free(d->z.fbuf);
  if (d->r3) {
    Ppmd7_Free(&d->r3->ppmd, &kRarAlloc);
    fm_free(d->r3);
  }
  fm_free(d->r5);
  fm_free(d->in.buf);
  fm_free(d);
  return result;
}

const FmArcBackend g_arc_rar = { rar_open, rar_extract, rar_close };
