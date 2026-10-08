/* f7z.c -- 7z archives: list, streaming extract, create; AES-256.
**
** Reading parses the header with the LZMA SDK (7zArcIn) and decodes data
** with our own folder decoder; writing is our own.
**
** Design decisions:
**   - The SDK's SzArEx_Extract decodes a whole folder into one buffer (GBs
**     for a solid archive), so folders are decoded here instead: every coder
**     of a folder becomes a node that pulls bytes from its inputs through
**     64 KB buffers, and the root's output is cut into the folder's files
**     as it comes. Decoding is lazy: a folder is only opened when one of its
**     files is wanted, and unwanted bytes before it are decoded and dropped.
**   - The next header is always read and, when it is an encoded header
**     (LZMA, AES or both), decoded by that same decoder. The plain header is
**     handed to SzArEx_Open through a virtual stream: the real file with a
**     patched start header pointing just past its end, where the plain
**     header sits in memory. One path for plain, packed and encrypted
**     headers; the header is capped at 64 MB.
**   - Dictionaries are sized min(declared, coder output) and refused over
**     S7_DICT_MAX (1.5 GB on 64-bit, 512 MB on 32-bit). Blocks of 64 MB and
**     more are probed with malloc first, so a hostile header gets
**     FM_ERR_NOMEM instead of fm_alloc's abort.
**   - AES: key = SHA-256 over 2^n rounds of salt + UTF-16LE password + a
**     64-bit counter (7-Zip's KDF), cached per (salt, rounds, password). A
**     wrong password shows up as a data error or a CRC mismatch before any
**     file of an encrypted folder has verified; the folder then restarts
**     after asking again (at most S7_PW_RETRIES times).
**   - Writer: LZMA2 (copy at level 0) per file, or per ~64 MB block when
**     solid, then AES-CBC with zero padding when a password is set. Sizes
**     and CRCs are known only afterwards, so the header goes last and the
**     start header is patched by seeking back. The header is LZMA-packed
**     when that helps, and always packed + encrypted with encrypt_names.
**   - The SDK's CRC table is a process global; CrcGenerateTable writes the
**     same values every time, so calling it on each open is harmless.
**   - entry.method: 0 copy, 1 LZMA, 2 LZMA2, 3 PPMd, 9 other.
*/
#include "farc_int.h"
#include "fcrypt.h"

#include "7z.h"
#include "7zCrc.h"
#include "Bcj2.h"
#include "Bra.h"
#include "Delta.h"
#include "Lzma2Dec.h"
#include "Lzma2Enc.h"
#include "LzmaDec.h"
#include "LzmaEnc.h"
#include "Ppmd7.h"

#include <errno.h>

/* ---- limits and ids ------------------------------------------------------ */

#define S7_BUF (64 * 1024)            /* each decoder node's buffer */
#define S7_OUT_BUF (256 * 1024)       /* extraction window */
#define S7_HDR_MAX ((u64)64 << 20)
#define S7_SOLID_BLOCK ((u64)64 << 20)
#define S7_PW_RETRIES 8
#define S7_MAX_NODES 8
#define S7_KDF_CYCLES 19
#if SIZE_MAX > 0xFFFFFFFFu
#  define S7_DICT_MAX ((u64)3 << 29)
#else
#  define S7_DICT_MAX ((u64)1 << 29)
#endif

enum {
  M_COPY = 0, M_DELTA = 3, M_ARM64 = 0xA, M_RISCV = 0xB, M_LZMA2 = 0x21,
  M_LZMA = 0x30101, M_PPMD = 0x30401, M_BCJ = 0x3030103, M_BCJ2 = 0x303011B,
  M_PPC = 0x3030205, M_IA64 = 0x3030401, M_ARM = 0x3030501, M_ARMT = 0x3030701,
  M_SPARC = 0x3030805, M_AES = 0x6F10701
};

enum {   /* header property ids (7zFormat.txt) */
  K_END = 0, K_HEADER = 1, K_MAIN_STREAMS = 4, K_FILES = 5, K_PACK_INFO = 6,
  K_UNPACK_INFO = 7, K_SUBSTREAMS = 8, K_SIZE = 9, K_CRC = 10, K_FOLDER = 11,
  K_CODERS_UNPACK_SIZE = 12, K_NUM_UNPACK_STREAM = 13, K_EMPTY_STREAM = 14,
  K_EMPTY_FILE = 15, K_NAME = 17, K_MTIME = 20, K_ATTRIBUTES = 21,
  K_ENCODED_HEADER = 23
};

static const u8 kSig[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };

/* ---- small helpers ------------------------------------------------------- */

static u32 get32(const u8 *p) {
  return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 get64(const u8 *p) { return (u64)get32(p) | (u64)get32(p + 4) << 32; }

static void put32(u8 *p, u32 v) {
  p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

static void *s7_alloc(ISzAllocPtr p, size_t n) {
  FM_UNUSED(p);
  if (n == 0) n = 1;
  if ((u64)n > S7_DICT_MAX + ((u64)1 << 24)) return NULL;
  if (n >= ((size_t)64 << 20)) {
    void *t = malloc(n);
    if (!t) return NULL;
    free(t);
  }
  return fm_alloc(n);
}

static void s7_free(ISzAllocPtr p, void *a) { FM_UNUSED(p); fm_free(a); }

static const ISzAlloc kAlloc = { s7_alloc, s7_free };

static FmErr s7_sres(SRes r) {
  switch (r) {
  case SZ_OK: return FM_OK;
  case SZ_ERROR_MEM: return FM_ERR_NOMEM;
  case SZ_ERROR_CRC: return FM_ERR_CRC;
  case SZ_ERROR_UNSUPPORTED: return FM_ERR_UNSUPPORTED;
  case SZ_ERROR_READ: return FM_ERR_IO;
  default: return FM_ERR_FORMAT;
  }
}

/* Windows FILETIME (100 ns since 1601) <-> unix seconds. */
#define S7_EPOCH_DIFF 11644473600LL

static i64 s7_ft_to_unix(u64 ft) { return (i64)(ft / 10000000u) - S7_EPOCH_DIFF; }

static u64 s7_unix_to_ft(i64 t) {
  if (t < -S7_EPOCH_DIFF) return 0;
  return (u64)(t + S7_EPOCH_DIFF) * 10000000u;
}

/* UTF-8 password to UTF-16LE bytes; returns the byte count. */
static size_t s7_pw_utf16(const char *pw, u8 *out, size_t cap) {
  size_t n = 0;
  while (*pw) {
    u32 cp;
    pw += utf8_decode(pw, &cp);
    if (cp >= 0x10000) {
      u32 c = cp - 0x10000, hi = 0xD800 | (c >> 10), lo = 0xDC00 | (c & 0x3FF);
      if (n + 4 > cap) break;
      out[n++] = (u8)hi; out[n++] = (u8)(hi >> 8);
      out[n++] = (u8)lo; out[n++] = (u8)(lo >> 8);
    } else {
      if (n + 2 > cap) break;
      out[n++] = (u8)cp; out[n++] = (u8)(cp >> 8);
    }
  }
  return n;
}

/* UTF-16 code units to UTF-8; out holds at least 3 * n + 1 bytes. */
static void s7_utf16_to_utf8(const UInt16 *s, size_t n, char *out) {
  size_t i = 0, o = 0;
  while (i < n && s[i]) {
    u32 cp = s[i++];
    if (cp >= 0xD800 && cp < 0xDC00 && i < n && s[i] >= 0xDC00 && s[i] < 0xE000)
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i++] - 0xDC00);
    else if (cp >= 0xD800 && cp < 0xE000)
      cp = 0xFFFD;
    o += (size_t)utf8_encode(cp, out + o);
  }
  out[o] = 0;
}

/* ---- AES key ------------------------------------------------------------- */

typedef struct Fm7zAesProps {
  int cycles, salt_len;
  u8 salt[16], iv[16];
} Fm7zAesProps;

typedef struct Fm7zKey {
  bool valid;
  int cycles, salt_len;
  u8 salt[16];
  char pw[256];
  u8 key[32];
} Fm7zKey;

static FmErr s7_aes_props(const u8 *p, unsigned n, Fm7zAesProps *o) {
  memset(o, 0, sizeof *o);
  if (n < 1) return FM_ERR_FORMAT;
  o->cycles = p[0] & 0x3F;
  if ((p[0] & 0xC0) == 0) {
    if (n != 1) return FM_ERR_FORMAT;
  } else {
    int iv_len;
    if (n < 2) return FM_ERR_FORMAT;
    o->salt_len = ((p[0] >> 7) & 1) + (p[1] >> 4);
    iv_len = ((p[0] >> 6) & 1) + (p[1] & 0x0F);
    if ((unsigned)(2 + o->salt_len + iv_len) > n) return FM_ERR_FORMAT;
    memcpy(o->salt, p + 2, (size_t)o->salt_len);
    memcpy(o->iv, p + 2 + o->salt_len, (size_t)iv_len);
  }
  if (o->cycles > 24 && o->cycles != 0x3F) return FM_ERR_UNSUPPORTED;
  return FM_OK;
}

static void s7_kdf(const char *pw, const u8 *salt, int salt_len, int cycles, u8 key[32]) {
  u8 buf[16 + 1024 + 8];
  size_t base;
  memcpy(buf, salt, (size_t)salt_len);
  base = (size_t)salt_len + s7_pw_utf16(pw, buf + salt_len, 1024);
  if (cycles == 0x3F) {
    memset(key, 0, 32);
    memcpy(key, buf, FM_MIN(base, (size_t)32));
  } else {
    FmSha256 sh;
    u64 r, rounds = (u64)1 << cycles;
    sha256_init(&sh);
    for (r = 0; r < rounds; r++) {
      put64(buf + base, r);
      sha256_update(&sh, buf, base + 8);
    }
    sha256_final(&sh, key);
    wipe(&sh, sizeof sh);
  }
  wipe(buf, sizeof buf);
}

static const u8 *s7_key(Fm7zKey *c, const char *pw, const Fm7zAesProps *ap) {
  if (!c->valid || c->cycles != ap->cycles || c->salt_len != ap->salt_len ||
      memcmp(c->salt, ap->salt, (size_t)ap->salt_len) != 0 || strcmp(c->pw, pw) != 0) {
    s7_kdf(pw, ap->salt, ap->salt_len, ap->cycles, c->key);
    c->cycles = ap->cycles;
    c->salt_len = ap->salt_len;
    memcpy(c->salt, ap->salt, sizeof c->salt);
    fm_strlcpy(c->pw, pw, sizeof c->pw);
    c->valid = true;
  }
  return c->key;
}

/* ---- folder decoder ------------------------------------------------------ */

/* A folder as the decoder needs it, from the main header or from an
** encoded header's own streams info. */
typedef struct Fm7zFolder {
  CSzFolder f;
  const u8 *props;   /* base of Coders[i].PropsOffset */
  u64 unpack[SZ_NUM_CODERS_IN_FOLDER_MAX];
  i64 pack_pos[SZ_NUM_PACK_STREAMS_IN_FOLDER_MAX];
  u64 pack_size[SZ_NUM_PACK_STREAMS_IN_FOLDER_MAX];
} Fm7zFolder;

enum { N_PACK, N_COPY, N_LZMA, N_LZMA2, N_PPMD, N_BRA, N_DELTA, N_AES, N_BCJ2 };

typedef struct Fm7zDec Fm7zDec;
typedef struct Fm7zNode Fm7zNode;

typedef struct Fm7zByteIn {
  IByteIn vt;
  Fm7zNode *n;
} Fm7zByteIn;

struct Fm7zNode {
  int kind;
  u32 method;
  Fm7zDec *d;
  u64 size, done;              /* output size and position */
  Fm7zNode *in[4];
  int n_in;
  i64 pos, end;                /* pack: file range still to read */
  u8 *ib;                      /* codecs: input buffer */
  size_t ip, il;
  bool ieof;
  u8 *ob;                      /* filters and AES: staging buffer */
  size_t op, oc, ol;           /* read position, converted end, filled end */
  u32 pc, x86;
  unsigned delta;
  u8 dstate[DELTA_STATE_SIZE];
  CLzmaDec lz;
  CLzma2Dec lz2;
  bool lz_on;
  CPpmd7 *ppmd;
  Fm7zByteIn bi;
  bool ppmd_started, extra;
  FmAes aes;
  u8 iv[16];
  CBcj2Dec bcj2;
  u8 *bb[4];
};

struct Fm7zDec {
  FILE *f;
  i64 fpos;                    /* where f is, -1 unknown */
  FmErr err;
  Fm7zNode node[S7_MAX_NODES];
  int nn;
  Fm7zNode *root;
};

static void s7_fail(Fm7zDec *d, FmErr e) {
  if (d->err == FM_OK) d->err = e;
}

static long s7_read(Fm7zNode *n, u8 *buf, size_t cap);

/* Codec input: 1 = bytes in ib, 0 = input ended, -1 = error. */
static int s7_fill(Fm7zNode *n) {
  long r;
  if (n->ip < n->il) return 1;
  if (n->ieof) return 0;
  r = s7_read(n->in[0], n->ib, S7_BUF);
  if (r < 0) return -1;
  n->ip = 0;
  n->il = (size_t)r;
  if (r == 0) {
    n->ieof = true;
    return 0;
  }
  return 1;
}

static long s7_pack(Fm7zNode *n, u8 *buf, size_t cap) {
  Fm7zDec *d = n->d;
  size_t got;
  if ((i64)cap > n->end - n->pos) cap = (size_t)(n->end - n->pos);
  if (cap == 0) return 0;
  if (d->fpos != n->pos) {
    if (fm_fseek64(d->f, n->pos, SEEK_SET) != 0) {
      s7_fail(d, FM_ERR_IO);
      return -1;
    }
    d->fpos = n->pos;
  }
  got = fread(buf, 1, cap, d->f);
  if (got == 0 && ferror(d->f)) {
    d->fpos = -1;
    s7_fail(d, FM_ERR_IO);
    return -1;
  }
  d->fpos += (i64)got;
  n->pos += (i64)got;
  return (long)got;
}

static long s7_lzma(Fm7zNode *n, u8 *buf, size_t cap) {
  for (;;) {
    int f = s7_fill(n);
    SizeT inl, outl = cap;
    ELzmaStatus st;
    SRes res;
    if (f < 0) return -1;
    inl = n->il - n->ip;
    if (n->kind == N_LZMA)
      res = LzmaDec_DecodeToBuf(&n->lz, buf, &outl, n->ib + n->ip, &inl, LZMA_FINISH_ANY, &st);
    else
      res = Lzma2Dec_DecodeToBuf(&n->lz2, buf, &outl, n->ib + n->ip, &inl, LZMA_FINISH_ANY, &st);
    n->ip += inl;
    if (res != SZ_OK) {
      s7_fail(n->d, FM_ERR_FORMAT);
      return -1;
    }
    if (outl > 0) return (long)outl;
    if (st == LZMA_STATUS_FINISHED_WITH_MARK || f == 0) return 0;
    if (inl == 0) {   /* input present but nothing moved: corrupt */
      s7_fail(n->d, FM_ERR_FORMAT);
      return -1;
    }
  }
}

static Byte s7_byte_in(IByteInPtr p) {
  Fm7zNode *n = ((const Fm7zByteIn *)p)->n;
  if (n->ip == n->il && s7_fill(n) <= 0) {
    n->extra = true;
    return 0;
  }
  return n->ib[n->ip++];
}

static long s7_ppmd(Fm7zNode *n, u8 *buf, size_t cap) {
  size_t i;
  if (!n->ppmd_started) {
    n->ppmd_started = true;
    if (!Ppmd7z_RangeDec_Init(&n->ppmd->rc.dec) || n->extra) {
      s7_fail(n->d, FM_ERR_FORMAT);
      return -1;
    }
  }
  for (i = 0; i < cap; i++) {
    int sym = Ppmd7z_DecodeSymbol(n->ppmd);
    if (n->extra || sym < 0) {
      s7_fail(n->d, FM_ERR_FORMAT);
      return -1;
    }
    buf[i] = (u8)sym;
  }
  return (long)cap;
}

static size_t s7_bra_conv(Fm7zNode *n, u8 *p, size_t size) {
  u8 *e;
  switch (n->method) {
  case M_BCJ: e = z7_BranchConvSt_X86_Dec(p, size, n->pc, &n->x86); break;
  case M_ARM: e = z7_BranchConv_ARM_Dec(p, size, n->pc); break;
  case M_ARMT: e = z7_BranchConv_ARMT_Dec(p, size, n->pc); break;
  case M_ARM64: e = z7_BranchConv_ARM64_Dec(p, size, n->pc); break;
  case M_PPC: e = z7_BranchConv_PPC_Dec(p, size, n->pc); break;
  case M_SPARC: e = z7_BranchConv_SPARC_Dec(p, size, n->pc); break;
  case M_IA64: e = z7_BranchConv_IA64_Dec(p, size, n->pc); break;
  default: e = z7_BranchConv_RISCV_Dec(p, size, n->pc); break;
  }
  return (size_t)(e - p);
}

/* Branch converters and AES: data staged in ob, [op, oc) ready to hand out,
** [oc, ol) still waiting for more input. */
static long s7_staged(Fm7zNode *n, u8 *buf, size_t cap) {
  for (;;) {
    long r;
    if (n->op < n->oc) {
      size_t k = FM_MIN(cap, n->oc - n->op);
      memcpy(buf, n->ob + n->op, k);
      n->op += k;
      return (long)k;
    }
    if (n->op > 0) {
      memmove(n->ob, n->ob + n->op, n->ol - n->op);
      n->ol -= n->op;
      n->op = n->oc = 0;
    }
    if (n->ieof) {
      if (n->kind == N_AES || n->ol == 0) return 0;
      n->oc = n->ol;   /* a converter's unconverted tail passes as is */
      continue;
    }
    r = s7_read(n->in[0], n->ob + n->ol, S7_BUF - n->ol);
    if (r < 0) return -1;
    if (r == 0) {
      n->ieof = true;
      continue;
    }
    n->ol += (size_t)r;
    if (n->kind == N_AES) {
      size_t m = n->ol & ~(size_t)15;
      if (m) aes_cbc_decrypt(&n->aes, n->iv, n->ob, m);
      n->oc = m;
    } else {
      n->oc = s7_bra_conv(n, n->ob, n->ol);
      n->pc += (u32)n->oc;
    }
  }
}

/* BCJ2: stream 0 main, 1 call, 2 jump, 3 range coder. */
static int s7_bcj2_fill(Fm7zNode *n, unsigned st) {
  size_t len = 0;
  while (len < S7_BUF) {
    long r = s7_read(n->in[st], n->bb[st] + len, S7_BUF - len);
    if (r < 0) return -1;
    if (r == 0) break;
    len += (size_t)r;
    if (!BCJ2_IS_32BIT_STREAM(st) || (len & 3) == 0) break;
  }
  if (BCJ2_IS_32BIT_STREAM(st)) len &= ~(size_t)3;
  n->bcj2.bufs[st] = n->bb[st];
  n->bcj2.lims[st] = n->bb[st] + len;
  return len > 0;
}

static long s7_bcj2(Fm7zNode *n, u8 *buf, size_t cap) {
  CBcj2Dec *p = &n->bcj2;
  p->dest = buf;
  p->destLim = buf + cap;
  for (;;) {
    unsigned st;
    int f;
    if (Bcj2Dec_Decode(p) != SZ_OK) {
      s7_fail(n->d, FM_ERR_FORMAT);
      return -1;
    }
    if (p->dest == p->destLim) break;
    st = p->state;
    if (st >= BCJ2_NUM_STREAMS || p->bufs[st] != p->lims[st]) break;
    f = s7_bcj2_fill(n, st);
    if (f < 0) return -1;
    if (f == 0) break;
  }
  return (long)(p->dest - buf);
}

/* Reads up to cap bytes of a node's output: >0 bytes, 0 at its end, -1 on
** error (d->err). Running out of input before the declared size is an
** error, so callers can loop on "> 0". */
static long s7_read(Fm7zNode *n, u8 *buf, size_t cap) {
  long r;
  u64 left = n->size - n->done;
  if (n->d->err != FM_OK) return -1;
  if ((u64)cap > left) cap = (size_t)left;
  if (cap > ((size_t)1 << 20)) cap = (size_t)1 << 20;
  if (cap == 0) return 0;
  switch (n->kind) {
  case N_PACK: r = s7_pack(n, buf, cap); break;
  case N_COPY: r = s7_read(n->in[0], buf, cap); break;
  case N_LZMA: case N_LZMA2: r = s7_lzma(n, buf, cap); break;
  case N_PPMD: r = s7_ppmd(n, buf, cap); break;
  case N_DELTA:
    r = s7_read(n->in[0], buf, cap);
    if (r > 0) Delta_Decode(n->dstate, n->delta, buf, (SizeT)r);
    break;
  case N_BCJ2: r = s7_bcj2(n, buf, cap); break;
  default: r = s7_staged(n, buf, cap); break;
  }
  if (r < 0) return -1;
  if (r == 0) {
    s7_fail(n->d, FM_ERR_FORMAT);
    return -1;
  }
  n->done += (u64)r;
  return r;
}

static void s7_dec_free(Fm7zDec *d) {
  int i, k;
  if (!d) return;
  for (i = 0; i < d->nn; i++) {
    Fm7zNode *n = &d->node[i];
    fm_free(n->ib);
    fm_free(n->ob);
    for (k = 0; k < 4; k++) fm_free(n->bb[k]);
    if (n->lz_on) {
      if (n->kind == N_LZMA) LzmaDec_Free(&n->lz, &kAlloc);
      else Lzma2Dec_Free(&n->lz2, &kAlloc);
    }
    if (n->ppmd) {
      Ppmd7_Free(n->ppmd, &kAlloc);
      fm_free(n->ppmd);
    }
    wipe(&n->aes, sizeof n->aes);
  }
  fm_free(d);
}

static u64 s7_lzma2_dict(unsigned p) {
  return p >= 40 ? 0xFFFFFFFFu : (u64)(2 | (p & 1)) << (p / 2 + 11);
}

static FmErr s7_init_coder(Fm7zNode *n, const u8 *pr, unsigned pn, FmArc *a, Fm7zKey *key) {
  switch (n->method) {
  case M_COPY:
    n->kind = N_COPY;
    return FM_OK;
  case M_LZMA: {
    u8 p5[5];
    u64 dict;
    if (pn != 5) return FM_ERR_UNSUPPORTED;
    memcpy(p5, pr, 5);
    dict = FM_MIN((u64)get32(pr + 1), n->size);
    if (dict < 4096) dict = 4096;
    if (dict > S7_DICT_MAX) return FM_ERR_UNSUPPORTED;
    put32(p5 + 1, (u32)dict);
    n->kind = N_LZMA;
    LzmaDec_CONSTRUCT(&n->lz);
    if (LzmaDec_Allocate(&n->lz, p5, 5, &kAlloc) != SZ_OK) return FM_ERR_NOMEM;
    n->lz_on = true;
    LzmaDec_Init(&n->lz);
    n->ib = (u8 *)fm_alloc(S7_BUF);
    return FM_OK;
  }
  case M_LZMA2: {
    unsigned p, q;
    u64 want;
    if (pn != 1 || pr[0] > 40) return FM_ERR_UNSUPPORTED;
    p = pr[0];
    want = FM_MIN(s7_lzma2_dict(p), n->size);
    for (q = 0; q < p && s7_lzma2_dict(q) < want; q++) {}
    if (s7_lzma2_dict(q) > S7_DICT_MAX) return FM_ERR_UNSUPPORTED;
    n->kind = N_LZMA2;
    Lzma2Dec_CONSTRUCT(&n->lz2);
    if (Lzma2Dec_Allocate(&n->lz2, (Byte)q, &kAlloc) != SZ_OK) return FM_ERR_NOMEM;
    n->lz_on = true;
    Lzma2Dec_Init(&n->lz2);
    n->ib = (u8 *)fm_alloc(S7_BUF);
    return FM_OK;
  }
  case M_PPMD: {
    unsigned order;
    u32 mem;
    if (pn != 5) return FM_ERR_UNSUPPORTED;
    order = pr[0];
    mem = get32(pr + 1);
    if (order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER || mem < PPMD7_MIN_MEM_SIZE ||
        mem > PPMD7_MAX_MEM_SIZE || (u64)mem > S7_DICT_MAX)
      return FM_ERR_UNSUPPORTED;
    n->kind = N_PPMD;
    n->ppmd = (CPpmd7 *)fm_alloc(sizeof(CPpmd7));
    Ppmd7_Construct(n->ppmd);
    if (!Ppmd7_Alloc(n->ppmd, mem, &kAlloc)) {
      fm_free(n->ppmd);
      n->ppmd = NULL;
      return FM_ERR_NOMEM;
    }
    Ppmd7_Init(n->ppmd, order);
    n->bi.vt.Read = s7_byte_in;
    n->bi.n = n;
    n->ppmd->rc.dec.Stream = &n->bi.vt;
    n->ib = (u8 *)fm_alloc(S7_BUF);
    return FM_OK;
  }
  case M_DELTA:
    if (pn != 1) return FM_ERR_UNSUPPORTED;
    n->kind = N_DELTA;
    n->delta = (unsigned)pr[0] + 1;
    Delta_Init(n->dstate);
    return FM_OK;
  case M_BCJ: case M_ARM: case M_ARMT: case M_ARM64: case M_PPC: case M_SPARC: case M_IA64:
  case M_RISCV:
    if (pn == 4) n->pc = get32(pr);
    else if (pn != 0) return FM_ERR_UNSUPPORTED;
    n->kind = N_BRA;
    n->x86 = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    n->ob = (u8 *)fm_alloc(S7_BUF);
    return FM_OK;
  case M_AES: {
    Fm7zAesProps ap;
    FmErr e = s7_aes_props(pr, pn, &ap);
    if (e != FM_OK) return e;
    if (!a->have_password) return FM_ERR_PASSWORD;
    aes_init(&n->aes, s7_key(key, a->password, &ap), 32);
    memcpy(n->iv, ap.iv, 16);
    n->kind = N_AES;
    n->ob = (u8 *)fm_alloc(S7_BUF);
    return FM_OK;
  }
  case M_BCJ2: {
    int k;
    n->kind = N_BCJ2;
    for (k = 0; k < 4; k++) n->bb[k] = (u8 *)fm_alloc(S7_BUF);
    Bcj2Dec_Init(&n->bcj2);
    for (k = 0; k < 4; k++) n->bcj2.bufs[k] = n->bcj2.lims[k] = n->bb[k];
    return FM_OK;
  }
  default:
    return FM_ERR_UNSUPPORTED;
  }
}

static Fm7zNode *s7_new_node(Fm7zDec *d) {
  Fm7zNode *n;
  if (d->nn >= S7_MAX_NODES) return NULL;
  n = &d->node[d->nn++];
  n->d = d;
  return n;
}

/* Builds the node for coder ci and, recursively, its inputs. */
static FmErr s7_build(Fm7zDec *d, const Fm7zFolder *fo, u32 ci, int depth, FmArc *a, Fm7zKey *key,
                      Fm7zNode **out) {
  const CSzFolder *f = &fo->f;
  const CSzCoderInfo *c = &f->Coders[ci];
  Fm7zNode *n = s7_new_node(d);
  u32 first = 0, j, k;
  FmErr e;
  if (!n || depth > SZ_NUM_CODERS_IN_FOLDER_MAX) return FM_ERR_FORMAT;
  for (j = 0; j < ci; j++) first += f->Coders[j].NumStreams;
  n->method = c->MethodID;
  n->size = fo->unpack[ci];
  if ((c->NumStreams == 4) != (c->MethodID == M_BCJ2) || c->NumStreams == 0)
    return FM_ERR_UNSUPPORTED;
  e = s7_init_coder(n, fo->props + c->PropsOffset, c->PropsSize, a, key);
  if (e != FM_OK) return e;
  n->n_in = c->NumStreams;
  for (j = 0; j < c->NumStreams; j++) {
    u32 s = first + j;
    Fm7zNode *in = NULL;
    for (k = 0; k < f->NumBonds; k++)
      if (f->Bonds[k].InIndex == s) {
        if (f->Bonds[k].OutIndex >= f->NumCoders) return FM_ERR_FORMAT;
        e = s7_build(d, fo, f->Bonds[k].OutIndex, depth + 1, a, key, &in);
        if (e != FM_OK) return e;
        break;
      }
    if (!in) {
      for (k = 0; k < f->NumPackStreams; k++)
        if (f->PackStreams[k] == s) break;
      if (k == f->NumPackStreams) return FM_ERR_FORMAT;
      in = s7_new_node(d);
      if (!in) return FM_ERR_FORMAT;
      in->kind = N_PACK;
      in->pos = fo->pack_pos[k];
      in->end = fo->pack_pos[k] + (i64)fo->pack_size[k];
      in->size = fo->pack_size[k];
    }
    n->in[j] = in;
  }
  *out = n;
  return FM_OK;
}

static bool s7_folder_has(const Fm7zFolder *fo, u32 method) {
  u32 i;
  for (i = 0; i < fo->f.NumCoders; i++)
    if (fo->f.Coders[i].MethodID == method) return true;
  return false;
}

static FmErr s7_dec_open(FILE *f, const Fm7zFolder *fo, FmArc *a, Fm7zKey *key, Fm7zDec **out) {
  Fm7zDec *d = (Fm7zDec *)fm_calloc(1, sizeof *d);
  FmErr e;
  d->f = f;
  d->fpos = -1;
  e = s7_build(d, fo, fo->f.UnpackStream, 0, a, key, &d->root);
  if (e != FM_OK) {
    s7_dec_free(d);
    return e;
  }
  *out = d;
  return FM_OK;
}

/* ---- header reading ------------------------------------------------------ */

typedef struct Fm7zRd {
  const u8 *p, *end;
  bool bad;
} Fm7zRd;

static u8 rd_byte(Fm7zRd *r) {
  if (r->p >= r->end) {
    r->bad = true;
    return 0;
  }
  return *r->p++;
}

static u64 rd_num(Fm7zRd *r) {
  u8 first = rd_byte(r), mask = 0x80;
  u64 v = 0;
  int i;
  for (i = 0; i < 8; i++) {
    if ((first & mask) == 0) return v | ((u64)(first & (mask - 1)) << (8 * i));
    v |= (u64)rd_byte(r) << (8 * i);
    mask >>= 1;
  }
  return v;
}

static void rd_skip(Fm7zRd *r, u64 n) {
  if (n > (u64)(r->end - r->p)) {
    r->bad = true;
    r->p = r->end;
  } else {
    r->p += (size_t)n;
  }
}

/* Digests: an all-defined byte or a bit vector, then a CRC per defined one.
** Returns whether the first is defined, with its value. */
static bool rd_digests(Fm7zRd *r, u64 count, u32 *first) {
  u64 i, defined = 0;
  bool first_def = false;
  if (count > (u64)(r->end - r->p) * 8 + 8) {
    r->bad = true;
    return false;
  }
  if (rd_byte(r)) {
    defined = count;
    first_def = count > 0;
  } else {
    u8 b = 0;
    for (i = 0; i < count; i++) {
      if ((i & 7) == 0) b = rd_byte(r);
      if (b & (0x80 >> (i & 7))) {
        if (i == 0) first_def = true;
        defined++;
      }
    }
  }
  for (i = 0; i < defined && !r->bad; i++) {
    u32 v;
    if (r->end - r->p < 4) {
      r->bad = true;
      break;
    }
    v = get32(r->p);
    r->p += 4;
    if (i == 0 && first) *first = v;
  }
  return first_def;
}

/* Streams info of an encoded header: one folder, its pack streams, CRC. */
static FmErr s7_parse_encoded(const u8 *h, size_t len, i64 file_size, Fm7zFolder *fo,
                              bool *has_crc, u32 *crc) {
  Fm7zRd r;
  u64 id, pack_pos, np, k, sizes[SZ_NUM_PACK_STREAMS_IN_FOLDER_MAX], pos;
  u32 i;
  CSzData sd;
  memset(fo, 0, sizeof *fo);
  memset(sizes, 0, sizeof sizes);
  *has_crc = false;
  r.p = h + 1;
  r.end = h + len;
  r.bad = false;
  if (rd_num(&r) != K_PACK_INFO) return FM_ERR_FORMAT;
  pack_pos = rd_num(&r);
  np = rd_num(&r);
  if (np == 0 || np > SZ_NUM_PACK_STREAMS_IN_FOLDER_MAX) return FM_ERR_UNSUPPORTED;
  for (;;) {
    id = rd_num(&r);
    if (r.bad) return FM_ERR_FORMAT;
    if (id == K_END) break;
    if (id == K_SIZE) {
      for (k = 0; k < np; k++) sizes[k] = rd_num(&r);
    } else if (id == K_CRC) {
      rd_digests(&r, np, NULL);
    } else {
      rd_skip(&r, rd_num(&r));
    }
  }
  if (rd_num(&r) != K_UNPACK_INFO || rd_num(&r) != K_FOLDER || rd_num(&r) != 1 || rd_byte(&r) != 0 ||
      r.bad)
    return FM_ERR_FORMAT;
  sd.Data = r.p;
  sd.Size = (size_t)(r.end - r.p);
  if (SzGetNextFolderItem(&fo->f, &sd) != SZ_OK) return FM_ERR_UNSUPPORTED;
  fo->props = r.p;
  r.p = sd.Data;
  if (fo->f.NumPackStreams != np) return FM_ERR_FORMAT;
  if (rd_num(&r) != K_CODERS_UNPACK_SIZE) return FM_ERR_FORMAT;
  for (i = 0; i < fo->f.NumCoders; i++) fo->unpack[i] = rd_num(&r);
  for (;;) {
    id = rd_num(&r);
    if (r.bad) return FM_ERR_FORMAT;
    if (id == K_END) break;
    if (id == K_CRC) *has_crc = rd_digests(&r, 1, crc);
    else rd_skip(&r, rd_num(&r));
  }
  if (rd_num(&r) != K_END || r.bad) return FM_ERR_UNSUPPORTED;
  pos = k7zStartHeaderSize + pack_pos;
  for (k = 0; k < np; k++) {
    if (pos > (u64)file_size || sizes[k] > (u64)file_size - pos) return FM_ERR_FORMAT;
    fo->pack_pos[k] = (i64)pos;
    fo->pack_size[k] = sizes[k];
    pos += sizes[k];
  }
  return FM_OK;
}

/* Decodes a whole (small) folder into memory. */
static FmErr s7_decode_mem(FmArc *a, Fm7zKey *key, const Fm7zFolder *fo, u8 **out, size_t *out_len) {
  u64 size = fo->unpack[fo->f.UnpackStream];
  Fm7zDec *d;
  u8 *buf;
  size_t got = 0;
  FmErr e;
  if (size > S7_HDR_MAX) return FM_ERR_UNSUPPORTED;
  e = s7_dec_open(a->f, fo, a, key, &d);
  if (e != FM_OK) return e;
  buf = (u8 *)fm_alloc((size_t)size + 1);
  while (got < (size_t)size) {
    long r = s7_read(d->root, buf + got, (size_t)size - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  e = got == (size_t)size ? FM_OK : (d->err != FM_OK ? d->err : FM_ERR_FORMAT);
  s7_dec_free(d);
  if (e != FM_OK) {
    fm_free(buf);
    return e;
  }
  *out = buf;
  *out_len = got;
  return FM_OK;
}

static FmErr s7_unpack_header(FmArc *a, Fm7zKey *key, u8 **hdr, size_t *len) {
  int round, tries = 0;
  for (round = 0; round < 4 && *len > 0 && (*hdr)[0] == K_ENCODED_HEADER; round++) {
    Fm7zFolder fo;
    bool has_crc, aes;
    u32 crc = 0;
    u8 *plain = NULL;
    size_t plen = 0;
    FmErr e = s7_parse_encoded(*hdr, *len, a->file_size, &fo, &has_crc, &crc);
    if (e != FM_OK) return e;
    aes = s7_folder_has(&fo, M_AES);
    for (;;) {
      if (aes && !a->have_password) {
        e = arc_get_password(a, false);
        if (e != FM_OK) return FM_ERR_PASSWORD;
      }
      e = s7_decode_mem(a, key, &fo, &plain, &plen);
      if (e == FM_OK && has_crc && crc32_update(0, plain, plen) != crc) {
        fm_free(plain);
        e = FM_ERR_CRC;
      }
      if (e == FM_OK) break;
      if (!aes || (e != FM_ERR_FORMAT && e != FM_ERR_CRC)) return e;
      if (++tries > S7_PW_RETRIES || arc_get_password(a, true) != FM_OK) return FM_ERR_PASSWORD;
    }
    fm_free(*hdr);
    *hdr = plain;
    *len = plen;
  }
  return FM_OK;
}

/* The SDK reads the archive through this: the real file, except that the
** start header points to the plain header placed right after the end. */
typedef struct Fm7zVStream {
  ISeekInStream vt;
  FILE *f;
  i64 real, pos;
  const u8 *hdr;
  size_t hdr_len;
  u8 sig[k7zStartHeaderSize];
} Fm7zVStream;

static SRes s7_vread(ISeekInStreamPtr p, void *buf, size_t *size) {
  Fm7zVStream *v = (Fm7zVStream *)(uintptr_t)p;
  size_t want = *size, got = 0;
  *size = 0;
  if (want == 0) return SZ_OK;
  if (v->pos < k7zStartHeaderSize) {
    got = FM_MIN(want, (size_t)(k7zStartHeaderSize - v->pos));
    memcpy(buf, v->sig + v->pos, got);
  } else if (v->pos < v->real) {
    size_t k = (size_t)FM_MIN((i64)want, v->real - v->pos);
    if (fm_fseek64(v->f, v->pos, SEEK_SET) != 0) return SZ_ERROR_READ;
    got = fread(buf, 1, k, v->f);
    if (got == 0) return SZ_ERROR_READ;
  } else if (v->pos < v->real + (i64)v->hdr_len) {
    size_t off = (size_t)(v->pos - v->real);
    got = FM_MIN(want, v->hdr_len - off);
    memcpy(buf, v->hdr + off, got);
  }
  v->pos += (i64)got;
  *size = got;
  return SZ_OK;
}

static SRes s7_vseek(ISeekInStreamPtr p, Int64 *pos, ESzSeek origin) {
  Fm7zVStream *v = (Fm7zVStream *)(uintptr_t)p;
  i64 base = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? v->pos : v->real + (i64)v->hdr_len;
  i64 np = base + (i64)*pos;
  if (np < 0) return SZ_ERROR_PARAM;
  v->pos = np;
  *pos = np;
  return SZ_OK;
}

/* ---- backend: open / extract / close -------------------------------------- */

typedef struct Fm7z {
  CSzArEx db;
  bool db_open;
  int *ent;          /* file index -> entry index (-1: not listed) */
  u8 *folder_aes;
  Fm7zKey key;
} Fm7z;

static FmErr s7_db_folder(const CSzArEx *db, u32 fi, i64 file_size, Fm7zFolder *fo) {
  const CSzAr *ar = &db->db;
  CSzData sd;
  u32 i, cu, ps;
  memset(fo, 0, sizeof *fo);
  sd.Data = ar->CodersData + ar->FoCodersOffsets[fi];
  sd.Size = ar->FoCodersOffsets[fi + 1] - ar->FoCodersOffsets[fi];
  fo->props = sd.Data;
  if (SzGetNextFolderItem(&fo->f, &sd) != SZ_OK) return FM_ERR_UNSUPPORTED;
  cu = ar->FoToCoderUnpackSizes[fi];
  if (ar->FoToCoderUnpackSizes[fi + 1] - cu != fo->f.NumCoders) return FM_ERR_FORMAT;
  for (i = 0; i < fo->f.NumCoders; i++) fo->unpack[i] = ar->CoderUnpackSizes[cu + i];
  ps = ar->FoStartPackStreamIndex[fi];
  if (ar->FoStartPackStreamIndex[fi + 1] - ps != fo->f.NumPackStreams) return FM_ERR_FORMAT;
  for (i = 0; i < fo->f.NumPackStreams; i++) {
    u64 pos = db->dataPos + ar->PackPositions[ps + i];
    u64 size = ar->PackPositions[ps + i + 1] - ar->PackPositions[ps + i];
    if (pos > (u64)file_size || size > (u64)file_size - pos) return FM_ERR_FORMAT;
    fo->pack_pos[i] = (i64)pos;
    fo->pack_size[i] = size;
  }
  return FM_OK;
}

static int s7_method_class(const Fm7zFolder *fo) {
  if (s7_folder_has(fo, M_PPMD)) return 3;
  if (s7_folder_has(fo, M_LZMA2)) return 2;
  if (s7_folder_has(fo, M_LZMA)) return 1;
  if (fo->f.NumCoders == 1 && fo->f.Coders[0].MethodID == M_COPY) return 0;
  return 9;
}

static FmErr s7_list(FmArc *a, Fm7z *z) {
  CSzArEx *db = &z->db;
  u32 i, nf = db->db.NumFolders;
  u32 *count = (u32 *)fm_calloc(nf + 1, sizeof(u32));
  u64 *packed = (u64 *)fm_calloc(nf + 1, sizeof(u64));
  u8 *klass = (u8 *)fm_calloc(nf + 1, 1);
  UInt16 *name16 = NULL;
  char *name8 = NULL;
  size_t name_cap = 0;
  z->folder_aes = (u8 *)fm_calloc(nf + 1, 1);
  z->ent = (int *)fm_alloc(((size_t)db->NumFiles + 1) * sizeof(int));
  for (i = 0; i < nf; i++) {
    Fm7zFolder fo;
    u32 k;
    klass[i] = 9;
    if (s7_db_folder(db, i, a->file_size, &fo) == FM_OK) {
      z->folder_aes[i] = s7_folder_has(&fo, M_AES);
      klass[i] = (u8)s7_method_class(&fo);
      for (k = 0; k < fo.f.NumPackStreams; k++) packed[i] += fo.pack_size[k];
    }
  }
  for (i = 0; i < db->NumFiles; i++)
    if (db->FileToFolder[i] < nf) count[db->FileToFolder[i]]++;
  for (i = 0; i < db->NumFiles; i++) {
    size_t len = SzArEx_GetFileNameUtf16(db, i, NULL);
    u32 fo = db->FileToFolder[i], attr = 0;
    bool is_dir = SzArEx_IsDir(db, i) != 0, has_attr = SzBitWithVals_Check(&db->Attribs, i);
    i64 mtime = 0;
    u64 size = SzArEx_GetFileSize(db, i);
    int idx;
    FmArcEntry *e;
    if (len + 1 > name_cap) {
      name_cap = len + 64;
      fm_free(name16);
      fm_free(name8);
      name16 = (UInt16 *)fm_alloc(name_cap * sizeof(UInt16));
      name8 = (char *)fm_alloc(name_cap * 3 + 1);
    }
    SzArEx_GetFileNameUtf16(db, i, name16);
    s7_utf16_to_utf8(name16, len, name8);
    if (has_attr) {
      attr = db->Attribs.Vals[i];
      if (attr & 0x10) is_dir = true;
    }
    if (SzBitWithVals_Check(&db->MTime, i)) {
      const CNtfsFileTime *t = &db->MTime.Vals[i];
      mtime = s7_ft_to_unix((u64)t->High << 32 | t->Low);
    }
    idx = arc_add(a, name8, is_dir ? 0 : size, (fo < nf && count[fo] == 1) ? packed[fo] : 0, mtime,
                  is_dir);
    z->ent[i] = idx;
    e = idx >= 0 ? arc_at(a, idx) : NULL;
    if (!e) continue;
    if (SzBitWithVals_Check(&db->CRCs, i)) e->crc = db->CRCs.Vals[i];
    if (has_attr && (attr & 0x8000)) {
      e->mode = attr >> 16;
      if ((e->mode & 0170000) == 0120000 && !is_dir) e->is_link = 1;
    }
    if (fo < nf) {
      e->encrypted = z->folder_aes[fo];
      e->method = klass[fo];
      if (e->encrypted) a->any_encrypted = true;
    }
  }
  fm_free(name16);
  fm_free(name8);
  fm_free(count);
  fm_free(packed);
  fm_free(klass);
  return FM_OK;
}

static FmErr s7_open(FmArc *a) {
  Fm7z *z = (Fm7z *)fm_calloc(1, sizeof *z);
  u8 sig[k7zStartHeaderSize], *hdr = NULL;
  u64 off, size;
  size_t len;
  FmErr e;
  Fm7zVStream vs;
  CLookToRead2 look;
  SRes res;
  a->priv = z;
  SzArEx_Init(&z->db);
  CrcGenerateTable();
  if (fm_fseek64(a->f, 0, SEEK_SET) != 0 || fread(sig, 1, sizeof sig, a->f) != sizeof sig ||
      memcmp(sig, kSig, sizeof kSig) != 0)
    return FM_ERR_FORMAT;
  if (sig[6] != 0) return FM_ERR_UNSUPPORTED;
  if (crc32_update(0, sig + 12, 20) != get32(sig + 8)) return FM_ERR_CRC;
  off = get64(sig + 12);
  size = get64(sig + 20);
  if (size == 0) return FM_OK;   /* empty archive */
  if (a->file_size < k7zStartHeaderSize || off > (u64)a->file_size - k7zStartHeaderSize ||
      size > (u64)a->file_size - k7zStartHeaderSize - off)
    return FM_ERR_FORMAT;
  if (size > S7_HDR_MAX) return FM_ERR_UNSUPPORTED;
  len = (size_t)size;
  hdr = (u8 *)fm_alloc(len);
  if (fm_fseek64(a->f, (i64)(k7zStartHeaderSize + off), SEEK_SET) != 0 ||
      fread(hdr, 1, len, a->f) != len) {
    fm_free(hdr);
    return FM_ERR_IO;
  }
  if (crc32_update(0, hdr, len) != get32(sig + 28)) {
    fm_free(hdr);
    return FM_ERR_CRC;
  }
  e = s7_unpack_header(a, &z->key, &hdr, &len);
  if (e == FM_OK && (len == 0 || hdr[0] != K_HEADER)) e = FM_ERR_FORMAT;
  if (e != FM_OK) {
    fm_free(hdr);
    return e;
  }

  memset(&vs, 0, sizeof vs);
  vs.vt.Read = s7_vread;
  vs.vt.Seek = s7_vseek;
  vs.f = a->f;
  vs.real = a->file_size;
  vs.hdr = hdr;
  vs.hdr_len = len;
  memcpy(vs.sig, sig, sizeof vs.sig);
  put64(vs.sig + 12, (u64)(a->file_size - k7zStartHeaderSize));
  put64(vs.sig + 20, (u64)len);
  put32(vs.sig + 28, crc32_update(0, hdr, len));
  put32(vs.sig + 8, crc32_update(0, vs.sig + 12, 20));

  memset(&look, 0, sizeof look);
  LookToRead2_CreateVTable(&look, False);
  look.buf = (Byte *)fm_alloc(S7_BUF);
  look.bufSize = S7_BUF;
  look.realStream = &vs.vt;
  LookToRead2_INIT(&look);
  res = SzArEx_Open(&z->db, &look.vt, &kAlloc, &kAlloc);
  fm_free(look.buf);
  fm_free(hdr);
  if (res != SZ_OK) return s7_sres(res);
  z->db_open = true;
  return s7_list(a, z);
}

static void s7_close(FmArc *a) {
  Fm7z *z = (Fm7z *)a->priv;
  if (!z) return;
  SzArEx_Free(&z->db, &kAlloc);
  fm_free(z->ent);
  fm_free(z->folder_aes);
  wipe(&z->key, sizeof z->key);
  fm_free(z);
  a->priv = NULL;
}

/* Extraction state while walking the files. */
typedef struct Fm7zX {
  FmArc *a;
  Fm7z *z;
  FmArcSink *s;
  Fm7zDec *d;
  u32 folder;        /* folder d decodes */
  u64 dpos;          /* bytes of its output consumed */
  u8 *buf;
} Fm7zX;

/* Decodes file i (folder fo, at off within it) into the sink. */
static FmErr s7_file(Fm7zX *x, u32 i, u32 fo, u64 off, u64 size) {
  CSzArEx *db = &x->z->db;
  u32 crc = 0;
  u64 left;
  FmErr e;
  if (x->d && (x->folder != fo || x->dpos > off)) {
    s7_dec_free(x->d);
    x->d = NULL;
  }
  if (!x->d) {
    Fm7zFolder f;
    e = s7_db_folder(db, fo, x->a->file_size, &f);
    if (e != FM_OK) return e;
    if (x->z->folder_aes[fo] && !x->a->have_password && arc_get_password(x->a, false) != FM_OK)
      return FM_ERR_PASSWORD;
    e = s7_dec_open(x->a->f, &f, x->a, &x->z->key, &x->d);
    if (e != FM_OK) return e;
    x->folder = fo;
    x->dpos = 0;
  }
  while (x->dpos < off) {
    long r = s7_read(x->d->root, x->buf, (size_t)FM_MIN((u64)S7_OUT_BUF, off - x->dpos));
    if (r <= 0) return x->d->err != FM_OK ? x->d->err : FM_ERR_FORMAT;
    x->dpos += (u64)r;
    if (sink_cancelled(x->s)) return FM_ERR_CANCEL;
  }
  for (left = size; left > 0;) {
    long r = s7_read(x->d->root, x->buf, (size_t)FM_MIN((u64)S7_OUT_BUF, left));
    if (r <= 0) return x->d->err != FM_OK ? x->d->err : FM_ERR_FORMAT;
    x->dpos += (u64)r;
    left -= (u64)r;
    crc = crc32_update(crc, x->buf, (size_t)r);
    e = sink_write(x->s, x->buf, (size_t)r);
    if (e != FM_OK) return e;
  }
  if (SzBitWithVals_Check(&db->CRCs, i) && db->CRCs.Vals[i] != crc) return FM_ERR_CRC;
  return FM_OK;
}

static FmErr s7_extract(FmArc *a, const u8 *sel, FmArcSink *s) {
  Fm7z *z = (Fm7z *)a->priv;
  CSzArEx *db = &z->db;
  Fm7zX x;
  FmErr st = FM_OK;
  u32 i, verified_folder = (u32)-1;
  int retries = 0;
  FM_UNUSED(sel);
  if (!z->db_open) return FM_OK;
  memset(&x, 0, sizeof x);
  x.a = a;
  x.z = z;
  x.s = s;
  x.folder = (u32)-1;
  x.buf = (u8 *)fm_alloc(S7_OUT_BUF);
  for (i = 0; i < db->NumFiles && st == FM_OK; i++) {
    int e = z->ent[i];
    u32 fo = db->FileToFolder[i];
    bool skip;
    if (sink_cancelled(s)) {
      st = FM_ERR_CANCEL;
      break;
    }
    if (sink_remaining(s) <= 0) break;
    if (e < 0) continue;
    st = sink_begin(s, e, &skip);
    if (st != FM_OK || skip) continue;
    if (fo >= db->db.NumFolders) {
      st = sink_end(s, FM_OK);
      continue;
    }
    for (;;) {
      u64 off = db->UnpackPositions[i] - db->UnpackPositions[db->FolderToFile[fo]];
      FmErr fe = s7_file(&x, i, fo, off, SzArEx_GetFileSize(db, i));
      bool maybe_pw = z->folder_aes[fo] && verified_folder != fo &&
                      (fe == FM_ERR_FORMAT || fe == FM_ERR_CRC);
      if (fe == FM_OK) {
        if (SzBitWithVals_Check(&db->CRCs, i)) verified_folder = fo;
        st = sink_end(s, FM_OK);
        break;
      }
      if (!maybe_pw) {
        st = sink_end(s, fe);
        if (st == FM_OK) st = fe;
        break;
      }
      sink_end(s, FM_ERR_PASSWORD);
      s7_dec_free(x.d);
      x.d = NULL;
      if (++retries > S7_PW_RETRIES || arc_get_password(a, true) != FM_OK) {
        st = FM_ERR_PASSWORD;
        break;
      }
      st = sink_begin(s, e, &skip);
      if (st != FM_OK || skip) break;
    }
  }
  s7_dec_free(x.d);
  fm_free(x.buf);
  return st;
}

const FmArcBackend g_arc_7z = { s7_open, s7_extract, s7_close };

/* ---- writer: buffers ------------------------------------------------------ */

typedef struct Fm7zBuf {
  u8 *p;
  size_t n, cap;
} Fm7zBuf;

static void b_put(Fm7zBuf *b, const void *d, size_t k) {
  if (b->n + k > b->cap) {
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + k) c *= 2;
    b->p = (u8 *)fm_realloc(b->p, c);
    b->cap = c;
  }
  memcpy(b->p + b->n, d, k);
  b->n += k;
}

static void b_byte(Fm7zBuf *b, u8 v) { b_put(b, &v, 1); }

static void b_num(Fm7zBuf *b, u64 v) {
  u8 out[9], first = 0, mask = 0x80;
  int n;
  for (n = 0; n < 8; n++) {
    if (v < ((u64)1 << (7 * (n + 1)))) {
      first |= (u8)(v >> (8 * n));
      break;
    }
    first |= mask;
    mask >>= 1;
  }
  out[0] = first;
  for (int i = 0; i < n; i++) out[1 + i] = (u8)(v >> (8 * i));
  b_put(b, out, (size_t)n + 1);
}

static void b_u32(Fm7zBuf *b, u32 v) {
  u8 t[4];
  put32(t, v);
  b_put(b, t, 4);
}

static void b_u64(Fm7zBuf *b, u64 v) {
  u8 t[8];
  put64(t, v);
  b_put(b, t, 8);
}

/* n bits, MSB first, set where bits[i] != 0. */
static void b_bits(Fm7zBuf *b, const u8 *bits, int n) {
  u8 cur = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (bits[i]) cur |= (u8)(0x80 >> (i & 7));
    if ((i & 7) == 7) {
      b_byte(b, cur);
      cur = 0;
    }
  }
  if (n & 7) b_byte(b, cur);
}

/* ---- writer: folders ------------------------------------------------------ */

typedef struct Fm7zWFolder {
  int count;           /* files in it */
  u64 unpack;          /* uncompressed bytes */
  u64 coded;           /* LZMA2/copy output (AES input) */
  u64 pack;            /* bytes in the archive */
  u8 lzma2_prop;
  u8 iv[16];
} Fm7zWFolder;

typedef struct Fm7zW {
  FILE *out;
  FmArcWriteCtx *w;
  FmErr err;
  int level;
  bool lzma2, aes;
  FmAes ctx;
  u8 salt[16], iv[16];
  /* source side of the folder being written */
  const int *files;
  int nfiles, cur;
  FILE *src;
  u64 *fsize;
  u32 *fcrc;
  /* output side */
  u8 *ob;
  size_t ol;
  u64 coded, pack;
} Fm7zW;

typedef struct Fm7zWIn { ISeqInStream vt; Fm7zW *w; } Fm7zWIn;
typedef struct Fm7zWOut { ISeqOutStream vt; Fm7zW *w; } Fm7zWOut;

static FmErr s7w_io_err(void) { return errno == ENOSPC ? FM_ERR_FULL : FM_ERR_IO; }

static FmErr s7w_raw(Fm7zW *w, const u8 *d, size_t n) {
  if (n && fwrite(d, 1, n, w->out) != n) return s7w_io_err();
  w->pack += n;
  return FM_OK;
}

static FmErr s7w_emit(Fm7zW *w, const u8 *d, size_t n) {
  w->coded += n;
  if (!w->aes) return s7w_raw(w, d, n);
  while (n) {
    size_t k = FM_MIN(n, (size_t)S7_BUF - w->ol);
    memcpy(w->ob + w->ol, d, k);
    w->ol += k;
    d += k;
    n -= k;
    if (w->ol == S7_BUF) {
      FmErr e;
      aes_cbc_encrypt(&w->ctx, w->iv, w->ob, S7_BUF);
      e = s7w_raw(w, w->ob, S7_BUF);
      w->ol = 0;
      if (e != FM_OK) return e;
    }
  }
  return FM_OK;
}

static FmErr s7w_flush(Fm7zW *w) {
  size_t pad;
  FmErr e;
  if (!w->aes || w->ol == 0) return FM_OK;
  pad = (16 - (w->ol & 15)) & 15;
  memset(w->ob + w->ol, 0, pad);
  w->ol += pad;
  aes_cbc_encrypt(&w->ctx, w->iv, w->ob, w->ol);
  e = s7w_raw(w, w->ob, w->ol);
  w->ol = 0;
  return e;
}

static SRes s7w_read(ISeqInStreamPtr p, void *buf, size_t *size) {
  Fm7zW *w = ((const Fm7zWIn *)p)->w;
  size_t want = *size;
  *size = 0;
  while (want > 0 && w->cur < w->nfiles) {
    int fi = w->files[w->cur];
    size_t r;
    if (!w->src) {
      w->src = fm_fopen(w->w->src[fi].abs, "rb");
      if (!w->src) {
        w->err = FM_ERR_IO;
        return SZ_ERROR_READ;
      }
      w->fsize[fi] = 0;
      w->fcrc[fi] = 0;
    }
    r = fread(buf, 1, want, w->src);
    if (r > 0) {
      w->fsize[fi] += r;
      w->fcrc[fi] = crc32_update(w->fcrc[fi], buf, r);
      *size = r;
      if (!arc_wprogress(w->w, r)) {
        w->err = FM_ERR_CANCEL;
        return SZ_ERROR_READ;
      }
      return SZ_OK;
    }
    if (ferror(w->src)) {
      w->err = FM_ERR_IO;
      return SZ_ERROR_READ;
    }
    fclose(w->src);
    w->src = NULL;
    w->cur++;
  }
  return SZ_OK;
}

static size_t s7w_write(ISeqOutStreamPtr p, const void *buf, size_t size) {
  Fm7zW *w = ((const Fm7zWOut *)p)->w;
  FmErr e = s7w_emit(w, (const u8 *)buf, size);
  if (e != FM_OK) {
    if (w->err == FM_OK) w->err = e;
    return 0;
  }
  return size;
}

static const u32 kDict[10] = {
  0, 256u << 10, 1u << 20, 2u << 20, 4u << 20, 8u << 20, 8u << 20, 16u << 20, 16u << 20, 16u << 20
};

static FmErr s7w_folder(Fm7zW *w, Fm7zWFolder *fo, const int *files, int count) {
  Fm7zWIn in;
  Fm7zWOut out;
  u64 total = 0;
  FmErr e = FM_OK;
  int i;
  memset(fo, 0, sizeof *fo);
  fo->count = count;
  for (i = 0; i < count; i++) total += w->w->src[files[i]].size;
  w->files = files;
  w->nfiles = count;
  w->cur = 0;
  w->src = NULL;
  w->coded = w->pack = 0;
  w->ol = 0;
  w->err = FM_OK;
  if (w->aes) {
    plat_random(fo->iv, 16);
    memcpy(w->iv, fo->iv, 16);
  }
  in.vt.Read = s7w_read;
  in.w = w;
  out.vt.Write = s7w_write;
  out.w = w;
  if (w->lzma2) {
    CLzma2EncHandle enc = Lzma2Enc_Create(&kAlloc, &kAlloc);
    CLzma2EncProps pr;
    SRes res;
    if (!enc) return FM_ERR_NOMEM;
    Lzma2EncProps_Init(&pr);
    pr.lzmaProps.level = w->level;
    pr.lzmaProps.dictSize = kDict[w->level];
    pr.lzmaProps.reduceSize = total;
    pr.lzmaProps.numThreads = 1;
    pr.numBlockThreads_Max = 1;
    pr.numBlockThreads_Reduced = 1;
    pr.numTotalThreads = 1;
    pr.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
    res = Lzma2Enc_SetProps(enc, &pr);
    if (res == SZ_OK) {
      Lzma2Enc_SetDataSize(enc, total);
      fo->lzma2_prop = Lzma2Enc_WriteProperties(enc);
      res = Lzma2Enc_Encode2(enc, &out.vt, NULL, NULL, &in.vt, NULL, 0, NULL);
    }
    Lzma2Enc_Destroy(enc);
    if (res != SZ_OK) e = w->err != FM_OK ? w->err : s7_sres(res);
  } else {
    u8 *buf = (u8 *)fm_alloc(S7_BUF);
    for (;;) {
      size_t n = S7_BUF;
      if (s7w_read(&in.vt, buf, &n) != SZ_OK) {
        e = w->err;
        break;
      }
      if (n == 0) break;
      e = s7w_emit(w, buf, n);
      if (e != FM_OK) break;
    }
    fm_free(buf);
  }
  if (w->src) {
    fclose(w->src);
    w->src = NULL;
  }
  if (e == FM_OK) e = s7w_flush(w);
  for (i = 0; i < count; i++) fo->unpack += w->fsize[files[i]];
  fo->coded = w->coded;
  fo->pack = w->pack;
  return e;
}

/* ---- writer: header ------------------------------------------------------- */

static void s7w_aes_props(u8 out[34], const u8 salt[16], const u8 iv[16]) {
  out[0] = (u8)(S7_KDF_CYCLES | 0x80 | 0x40);
  out[1] = 0xFF;   /* 16-byte salt and IV */
  memcpy(out + 2, salt, 16);
  memcpy(out + 18, iv, 16);
}

static void s7w_coder(Fm7zBuf *b, const u8 *id, int id_len, const u8 *props, int props_len) {
  b_byte(b, (u8)(id_len | (props_len ? 0x20 : 0)));
  b_put(b, id, (size_t)id_len);
  if (props_len) {
    b_num(b, (u64)props_len);
    b_put(b, props, (size_t)props_len);
  }
}

static const u8 kIdAes[4] = { 0x06, 0xF1, 0x07, 0x01 };
static const u8 kIdLzma[3] = { 0x03, 0x01, 0x01 };

/* One folder in 7-Zip's order: AES first, then the main coder (LZMA2, LZMA
** or copy) reading AES's output; either may be missing. Coder unpack sizes
** follow the same order. */
static void s7w_folder_desc(Fm7zBuf *b, const u8 *id, int id_len, const u8 *props, int props_len,
                            const u8 *aes_props) {
  int n = (id ? 1 : 0) + (aes_props ? 1 : 0);
  b_num(b, (u64)n);
  if (aes_props) s7w_coder(b, kIdAes, 4, aes_props, 34);
  if (id) s7w_coder(b, id, id_len, props, props_len);
  if (n == 2) {   /* bond: coder 1's input <- coder 0's output */
    b_num(b, 1);
    b_num(b, 0);
  }
}

static void s7w_main_folder(Fm7zW *w, Fm7zBuf *b, const Fm7zWFolder *fo) {
  static const u8 kIdLzma2[1] = { 0x21 }, kIdCopy[1] = { 0x00 };
  u8 aes[34];
  if (w->aes) s7w_aes_props(aes, w->salt, fo->iv);
  if (w->lzma2) s7w_folder_desc(b, kIdLzma2, 1, &fo->lzma2_prop, 1, w->aes ? aes : NULL);
  else if (w->aes) s7w_folder_desc(b, NULL, 0, NULL, 0, aes);
  else s7w_folder_desc(b, kIdCopy, 1, NULL, 0, NULL);
}

static void s7w_build_header(Fm7zW *w, Fm7zBuf *h, const Fm7zWFolder *fol, int nfol,
                             const u8 *has_stream) {
  const FmArcSrc *src = w->w->src;
  int n = w->w->n, i, k, f, n_empty = 0, n_empty_file = 0, n_mtime = 0;
  u8 *bits = (u8 *)fm_calloc((size_t)n + 1, 1);
  b_byte(h, K_HEADER);
  if (nfol > 0) {
    b_byte(h, K_MAIN_STREAMS);
    b_byte(h, K_PACK_INFO);
    b_num(h, 0);
    b_num(h, (u64)nfol);
    b_byte(h, K_SIZE);
    for (f = 0; f < nfol; f++) b_num(h, fol[f].pack);
    b_byte(h, K_END);
    b_byte(h, K_UNPACK_INFO);
    b_byte(h, K_FOLDER);
    b_num(h, (u64)nfol);
    b_byte(h, 0);
    for (f = 0; f < nfol; f++) s7w_main_folder(w, h, &fol[f]);
    b_byte(h, K_CODERS_UNPACK_SIZE);
    for (f = 0; f < nfol; f++) {
      if (w->lzma2 && w->aes) b_num(h, fol[f].coded);
      b_num(h, fol[f].unpack);
    }
    b_byte(h, K_END);
    b_byte(h, K_SUBSTREAMS);
    b_byte(h, K_NUM_UNPACK_STREAM);
    for (f = 0; f < nfol; f++) b_num(h, (u64)fol[f].count);
    b_byte(h, K_SIZE);
    for (i = 0, f = 0; f < nfol; f++) {
      for (k = 0; k < fol[f].count; i++) {
        if (!has_stream[i]) continue;
        if (k < fol[f].count - 1) b_num(h, w->fsize[i]);
        k++;
      }
    }
    b_byte(h, K_CRC);
    b_byte(h, 1);
    for (i = 0; i < n; i++)
      if (has_stream[i]) b_u32(h, w->fcrc[i]);
    b_byte(h, K_END);
    b_byte(h, K_END);
  }
  if (n > 0) {
    Fm7zBuf names = { 0 };
    b_byte(h, K_FILES);
    b_num(h, (u64)n);
    for (i = 0; i < n; i++) {
      bits[i] = !has_stream[i];
      n_empty += bits[i];
      if (src[i].mtime != 0) n_mtime++;
    }
    if (n_empty > 0) {
      Fm7zBuf t = { 0 };
      b_bits(&t, bits, n);
      b_byte(h, K_EMPTY_STREAM);
      b_num(h, t.n);
      b_put(h, t.p, t.n);
      t.n = 0;
      for (i = 0, k = 0; i < n; i++)
        if (!has_stream[i]) {
          bits[k] = !src[i].is_dir;
          n_empty_file += bits[k];
          k++;
        }
      if (n_empty_file > 0) {
        b_bits(&t, bits, k);
        b_byte(h, K_EMPTY_FILE);
        b_num(h, t.n);
        b_put(h, t.p, t.n);
      }
      fm_free(t.p);
    }
    b_byte(&names, 0);   /* not external */
    for (i = 0; i < n; i++) {
      u8 tmp[2048];
      size_t len = s7_pw_utf16(src[i].name, tmp, sizeof tmp - 2);
      tmp[len] = tmp[len + 1] = 0;
      b_put(&names, tmp, len + 2);
    }
    b_byte(h, K_NAME);
    b_num(h, names.n);
    b_put(h, names.p, names.n);
    fm_free(names.p);
    if (n_mtime > 0) {
      b_byte(h, K_MTIME);
      if (n_mtime == n) {
        b_num(h, 2 + (u64)n * 8);
        b_byte(h, 1);
      } else {
        Fm7zBuf t = { 0 };
        for (i = 0; i < n; i++) bits[i] = src[i].mtime != 0;
        b_bits(&t, bits, n);
        b_num(h, 2 + t.n + (u64)n_mtime * 8);
        b_byte(h, 0);
        b_put(h, t.p, t.n);
        fm_free(t.p);
      }
      b_byte(h, 0);
      for (i = 0; i < n; i++)
        if (src[i].mtime != 0) b_u64(h, s7_unix_to_ft(src[i].mtime));
    }
    b_byte(h, K_ATTRIBUTES);
    b_num(h, 2 + (u64)n * 4);
    b_byte(h, 1);
    b_byte(h, 0);
    for (i = 0; i < n; i++) {
      u32 at = src[i].is_dir ? 0x10 : 0x20, m = src[i].mode & 0xFFFF;
      if (m) {
        if ((m & 0170000) == 0) m |= src[i].is_dir ? 0040000 : 0100000;
        at |= 0x8000 | (m << 16);
      }
      b_u32(h, at);
    }
    b_byte(h, K_END);
  }
  b_byte(h, K_END);
  fm_free(bits);
}

/* Writes the header (packed and maybe encrypted) at the current position and
** returns the bytes the start header must point to in next. */
static FmErr s7w_write_header(Fm7zW *w, const Fm7zBuf *h, i64 base, bool encrypt, Fm7zBuf *next) {
  CLzmaEncHandle enc;
  CLzmaEncProps pr;
  u8 props[LZMA_PROPS_SIZE], aes[34], iv[16];
  SizeT props_len = LZMA_PROPS_SIZE, packed_len;
  u8 *packed;
  size_t cap;
  SRes res;
  FmErr e;
  i64 at;
  if (h->n == 0) return FM_OK;
  cap = h->n + h->n / 2 + 1024;
  packed = (u8 *)fm_alloc(cap + 16);
  packed_len = cap;
  enc = LzmaEnc_Create(&kAlloc);
  if (!enc) {
    fm_free(packed);
    return FM_ERR_NOMEM;
  }
  LzmaEncProps_Init(&pr);
  pr.level = 5;
  pr.dictSize = 1u << 20;
  pr.reduceSize = h->n;
  pr.numThreads = 1;
  res = LzmaEnc_SetProps(enc, &pr);
  if (res == SZ_OK) res = LzmaEnc_WriteProperties(enc, props, &props_len);
  if (res == SZ_OK)
    res = LzmaEnc_MemEncode(enc, packed, &packed_len, h->p, h->n, 0, NULL, &kAlloc, &kAlloc);
  LzmaEnc_Destroy(enc, &kAlloc, &kAlloc);
  if (res != SZ_OK || (!encrypt && packed_len + 64 >= h->n)) {
    fm_free(packed);
    if (encrypt) return res == SZ_OK ? FM_ERR_FORMAT : s7_sres(res);
    b_put(next, h->p, h->n);   /* packing does not pay: plain header */
    return FM_OK;
  }
  at = fm_ftell64(w->out);
  if (at < 0) {
    fm_free(packed);
    return FM_ERR_IO;
  }
  {
    size_t stored = packed_len;
    if (encrypt) {
      FmAes ctx = w->ctx;
      u8 civ[16];
      stored = (packed_len + 15) & ~(size_t)15;
      memset(packed + packed_len, 0, stored - packed_len);
      plat_random(iv, 16);
      memcpy(civ, iv, 16);
      aes_cbc_encrypt(&ctx, civ, packed, stored);
      wipe(&ctx, sizeof ctx);
      s7w_aes_props(aes, w->salt, iv);
    }
    e = fwrite(packed, 1, stored, w->out) == stored ? FM_OK : s7w_io_err();
    fm_free(packed);
    if (e != FM_OK) return e;
    b_byte(next, K_ENCODED_HEADER);
    b_byte(next, K_PACK_INFO);
    b_num(next, (u64)(at - base - k7zStartHeaderSize));
    b_num(next, 1);
    b_byte(next, K_SIZE);
    b_num(next, stored);
  }
  b_byte(next, K_END);
  b_byte(next, K_UNPACK_INFO);
  b_byte(next, K_FOLDER);
  b_num(next, 1);
  b_byte(next, 0);
  s7w_folder_desc(next, kIdLzma, 3, props, (int)props_len, encrypt ? aes : NULL);
  b_byte(next, K_CODERS_UNPACK_SIZE);
  if (encrypt) b_num(next, packed_len);
  b_num(next, h->n);
  b_byte(next, K_CRC);
  b_byte(next, 1);
  b_u32(next, crc32_update(0, h->p, h->n));
  b_byte(next, K_END);
  b_byte(next, K_END);
  return FM_OK;
}

FmErr sevenz_write(FILE *out, FmArcWriteCtx *wc) {
  const FmArcOpts *o = wc->o;
  int n = wc->n, i, nfol = 0, *list;
  Fm7zW w;
  Fm7zWFolder *fol;
  u8 *has_stream, sig[k7zStartHeaderSize];
  Fm7zBuf h = { 0 }, next = { 0 };
  bool pw = o->password && o->password[0];
  FmErr e = FM_OK;
  i64 base = fm_ftell64(out), end;

  memset(&w, 0, sizeof w);
  w.out = out;
  w.w = wc;
  w.level = o->level < 0 ? 5 : FM_CLAMP(o->level, 0, 9);
  w.lzma2 = w.level > 0;
  w.aes = pw;
  if (base < 0) base = 0;
  memset(sig, 0, sizeof sig);
  if (fwrite(sig, 1, sizeof sig, out) != sizeof sig) return s7w_io_err();
  if (pw) {
    u8 key[32];
    plat_random(w.salt, sizeof w.salt);
    s7_kdf(o->password, w.salt, 16, S7_KDF_CYCLES, key);
    aes_init(&w.ctx, key, 32);
    wipe(key, sizeof key);
  }
  w.ob = (u8 *)fm_alloc(S7_BUF);
  w.fsize = (u64 *)fm_calloc((size_t)n + 1, sizeof(u64));
  w.fcrc = (u32 *)fm_calloc((size_t)n + 1, sizeof(u32));
  has_stream = (u8 *)fm_calloc((size_t)n + 1, 1);
  list = (int *)fm_alloc(((size_t)n + 1) * sizeof(int));
  fol = (Fm7zWFolder *)fm_calloc((size_t)n + 1, sizeof(Fm7zWFolder));
  for (i = 0; i < n; i++) has_stream[i] = !wc->src[i].is_dir && wc->src[i].size > 0;

  for (i = 0; i < n && e == FM_OK;) {
    int count = 0;
    u64 total = 0;
    if (!has_stream[i]) {
      i++;
      continue;
    }
    while (i < n && (count == 0 || (o->solid && total < S7_SOLID_BLOCK))) {
      if (has_stream[i]) {
        list[count++] = i;
        total += wc->src[i].size;
      }
      i++;
    }
    e = s7w_folder(&w, &fol[nfol], list, count);
    nfol++;
  }

  if (e == FM_OK) {
    s7w_build_header(&w, &h, fol, nfol, has_stream);
    if (n == 0) h.n = 0;
    e = s7w_write_header(&w, &h, base, pw && o->encrypt_names, &next);
  }
  if (e == FM_OK && next.n > 0 && fwrite(next.p, 1, next.n, out) != next.n) e = s7w_io_err();
  if (e == FM_OK) {
    end = fm_ftell64(out);
    memcpy(sig, kSig, sizeof kSig);
    sig[6] = 0;
    sig[7] = 4;
    put64(sig + 12, next.n ? (u64)(end - base - k7zStartHeaderSize - (i64)next.n) : 0);
    put64(sig + 20, next.n);
    put32(sig + 28, next.n ? crc32_update(0, next.p, next.n) : 0);
    put32(sig + 8, crc32_update(0, sig + 12, 20));
    if (end < 0 || fm_fseek64(out, base, SEEK_SET) != 0 ||
        fwrite(sig, 1, sizeof sig, out) != sizeof sig || fm_fseek64(out, end, SEEK_SET) != 0 ||
        fflush(out) != 0)
      e = s7w_io_err();
  }
  wipe(&w.ctx, sizeof w.ctx);
  fm_free(w.ob);
  fm_free(w.fsize);
  fm_free(w.fcrc);
  fm_free(has_stream);
  fm_free(list);
  fm_free(fol);
  fm_free(h.p);
  fm_free(next.p);
  return e;
}
