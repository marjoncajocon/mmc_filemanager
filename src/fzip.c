/* fzip.c -- zip archives: list, extract and create, with ZipCrypto and
** WinZip AES (AE-1/AE-2, 128/192/256-bit).
**
** Design decisions:
**   - The central directory is read once at open, record by record (never
**     the whole directory in memory), into the entry arena plus a small
**     side table of offsets and methods. ZIP64 comes from the EOCD64
**     locator and the 0x0001 extra field. Data in front of the archive
**     (self-extractors) is handled by shifting every offset by the gap
**     between where the directory claims to be and where it ends.
**   - Names: UTF-8 when flag bit 11 says so, or when the bytes are valid
**     UTF-8 anyway (macOS and many Unix tools set no flag); otherwise
**     CP437, the zip default (arc_name_utf8 in farc.c). An Info-ZIP Unicode path extra (0x7075)
**     whose CRC matches the raw name wins.
**   - Times: the extended timestamp (0x5455), then NTFS (0x000a), then the
**     DOS fields, which are local time.
**   - Extraction reads data sizes from the central directory, so data
**     descriptors need no parsing. Every method goes through fstream
**     (store, deflate, bzip2, lzma, zstd, xz) via farc_ext.h, with the
**     decryption in the read callback.
**   - A wrong password shows up at the header check (ZipCrypto's check
**     byte, AES's verifier) or, rarely, only at the end (CRC or HMAC):
**     either way the partial file is dropped, the password is asked again
**     (arc_get_password(a, true)) up to three times, then FM_ERR_PASSWORD.
**     Once an entry decrypts cleanly the password counts as right and
**     later CRC errors are real CRC errors.
**   - Entries with a method we lack (PPMd, Deflate64, PKWARE strong
**     encryption) are skipped and the run ends with FM_ERR_UNSUPPORTED,
**     so the rest of the archive still comes out.
**   - Writing streams each file once: the local header is written with
**     zero sizes and patched by seeking back, so stored entries need no
**     data descriptor (Java and some unzips mishandle that). ZipCrypto
**     entries do use a descriptor (bit 3), because the check byte then
**     comes from the time and the header can be written before the CRC is
**     known. AES entries are AE-2 (no CRC), 256-bit, as WinZip and 7-Zip.
**   - ZIP64 fields are added per entry only where needed, plus the
**     EOCD64 records when the directory needs them.
*/
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L     /* localtime_r */
#endif
#include "farc_int.h"
#include "farc_ext.h"
#include "fcrypt.h"
#include <time.h>

/* ---- little-endian helpers ---------------------------------------------- */

static u16 g16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 g32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 g64(const u8 *p) { return (u64)g32(p) | ((u64)g32(p + 4) << 32); }
static u8 *p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); return p + 2; }
static u8 *p32(u8 *p, u32 v) { p = p16(p, v & 0xFFFF); return p16(p, v >> 16); }
static u8 *p64(u8 *p, u64 v) { p = p32(p, (u32)v); return p32(p, (u32)(v >> 32)); }

/* ---- times -------------------------------------------------------------- */

static i64 dos_to_unix(u16 t, u16 d) {
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = ((d >> 9) & 0x7F) + 80;
  tm.tm_mon = ((d >> 5) & 15) - 1;
  tm.tm_mday = d & 31;
  tm.tm_hour = t >> 11;
  tm.tm_min = (t >> 5) & 63;
  tm.tm_sec = (t & 31) * 2;
  tm.tm_isdst = -1;
  if (tm.tm_mon < 0 || !tm.tm_mday) return 0;
  time_t r = mktime(&tm);
  return r == (time_t)-1 ? 0 : (i64)r;
}

static void unix_to_dos(i64 t, u16 *dt, u16 *dd) {
  time_t tt = (time_t)t;
  struct tm tm;
  bool ok;
#ifdef FM_WIN
  struct tm *p = localtime(&tt);       /* msvcrt keeps it per thread */
  ok = p != NULL;
  if (ok) tm = *p;
#else
  ok = localtime_r(&tt, &tm) != NULL;
#endif
  if (!ok || tm.tm_year < 80) {
    *dt = 0;
    *dd = (1 << 5) | 1;                /* 1980-01-01 */
    return;
  }
  if (tm.tm_year > 207) tm.tm_year = 207;
  *dt = (u16)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  *dd = (u16)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

/* ---- reading: central directory ----------------------------------------- */

typedef struct ZipEnt {
  u64 off;              /* local header, already shifted */
  u64 csize;
  u32 crc;
  u16 flags, method;    /* method: the compression (inside AES) */
  u16 dtime;
  u8 aes, aes_ver;      /* strength 1..3, AE-1/AE-2 */
} ZipEnt;

typedef struct ZipPriv {
  ZipEnt *z;
  int cap;
  bool pw_ok;           /* the cached password decrypted an entry cleanly */
  bool wrong_pw;        /* the last failure was a password check */
} ZipPriv;

static bool rd_at(FILE *f, i64 off, void *buf, size_t n) {
  return fm_fseek64(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* Finds the end of central directory record in the last 64 KB + 22 bytes. */
static FmErr find_eocd(FmArc *a, i64 *eocd_pos, u8 rec[22]) {
  i64 fsz = a->file_size;
  if (fsz < 22) return FM_ERR_FORMAT;
  size_t tail = (size_t)FM_MIN(fsz, (i64)(22 + 65535));
  u8 *b = (u8 *)fm_alloc(tail);
  FmErr e = FM_ERR_FORMAT;
  if (!rd_at(a->f, fsz - (i64)tail, b, tail)) {
    e = FM_ERR_IO;
  } else {
    /* Prefer a record whose comment ends exactly at the end of the file. */
    i64 loose = -1;
    for (size_t i = tail - 22 + 1; i-- > 0;) {
      if (b[i] != 'P' || b[i + 1] != 'K' || b[i + 2] != 5 || b[i + 3] != 6) continue;
      size_t clen = g16(b + i + 20);
      if (i + 22 + clen == tail) { loose = (i64)i; break; }
      if (loose < 0 && i + 22 + clen <= tail) loose = (i64)i;
    }
    if (loose >= 0) {
      memcpy(rec, b + loose, 22);
      *eocd_pos = fsz - (i64)tail + loose;
      e = FM_OK;
    }
  }
  fm_free(b);
  return e;
}

static void parse_extra(const u8 *x, size_t n, u64 *usize, u64 *csize, u64 *off, u32 usize32,
                        u32 csize32, u32 off32, i64 *mtime, int *mt_rank, ZipEnt *z,
                        const u8 **upath, size_t *upath_len, u32 *upath_crc) {
  size_t i = 0;
  while (i + 4 <= n) {
    u16 id = g16(x + i), sz = g16(x + i + 2);
    const u8 *d = x + i + 4;
    if (i + 4 + sz > n) break;
    if (id == 0x0001) {                         /* ZIP64 */
      size_t k = 0;
      if (usize32 == 0xFFFFFFFF && k + 8 <= sz) { *usize = g64(d + k); k += 8; }
      if (csize32 == 0xFFFFFFFF && k + 8 <= sz) { *csize = g64(d + k); k += 8; }
      if (off32 == 0xFFFFFFFF && k + 8 <= sz) { *off = g64(d + k); k += 8; }
    } else if (id == 0x5455 && sz >= 5 && (d[0] & 1)) {     /* extended timestamp */
      *mtime = (i64)(i32)g32(d + 1);
      *mt_rank = 3;
    } else if (id == 0x000A && sz >= 32 && *mt_rank < 2) {  /* NTFS */
      size_t k = 4;
      while (k + 4 <= sz) {
        u16 tag = g16(d + k), tsz = g16(d + k + 2);
        if (k + 4 + tsz > sz) break;
        if (tag == 1 && tsz >= 24) {
          u64 ft = g64(d + k + 4);
          if (ft) {
            *mtime = (i64)(ft / 10000000u) - 11644473600LL;
            *mt_rank = 2;
          }
        }
        k += 4 + (size_t)tsz;
      }
    } else if (id == 0x7075 && sz >= 6 && d[0] == 1) {       /* Info-ZIP Unicode path */
      *upath_crc = g32(d + 1);
      *upath = d + 5;
      *upath_len = (size_t)sz - 5;
    } else if (id == 0x9901 && sz >= 7 && d[2] == 'A' && d[3] == 'E') {   /* WinZip AES */
      z->aes_ver = (u8)g16(d);
      z->aes = d[4];
      z->method = g16(d + 5);
    }
    i += 4 + (size_t)sz;
  }
}

static int zpush(ZipPriv *zp, int idx) {
  if (idx >= zp->cap) {
    zp->cap = zp->cap ? zp->cap * 2 : 64;
    while (zp->cap <= idx) zp->cap *= 2;
    zp->z = (ZipEnt *)fm_realloc(zp->z, (size_t)zp->cap * sizeof *zp->z);
  }
  return idx;
}

static FmErr zip_open(FmArc *a) {
  u8 eo[22];
  i64 eocd_pos;
  FmErr e = find_eocd(a, &eocd_pos, eo);
  if (e) return e;
  u64 count = g16(eo + 10), cd_size = g32(eo + 12), cd_off = g32(eo + 16);
  i64 cd_end = eocd_pos;
  /* ZIP64 end of central directory, through its locator */
  u8 loc[20], z64[56];
  if (eocd_pos >= 20 + 56 && rd_at(a->f, eocd_pos - 20, loc, 20) && g32(loc) == 0x07064b50) {
    i64 z64_pos = (i64)g64(loc + 8);
    bool ok = z64_pos >= 0 && z64_pos + 56 <= eocd_pos - 20 && rd_at(a->f, z64_pos, z64, 56) &&
              g32(z64) == 0x06064b50;
    if (!ok) {                       /* offsets shifted by a prefix: it sits right before */
      z64_pos = eocd_pos - 20 - 56;
      ok = rd_at(a->f, z64_pos, z64, 56) && g32(z64) == 0x06064b50;
    }
    if (ok) {
      count = g64(z64 + 32);
      cd_size = g64(z64 + 40);
      cd_off = g64(z64 + 48);
      cd_end = z64_pos;
    }
  }
  if (cd_size > (u64)cd_end || cd_off > (u64)cd_end - cd_size) return FM_ERR_FORMAT;
  u64 shift = (u64)cd_end - cd_size - cd_off;  /* bytes in front of the archive */
  u64 pos = cd_off + shift, end = pos + cd_size;

  ZipPriv *zp = (ZipPriv *)fm_calloc(1, sizeof *zp);
  a->priv = zp;
  u8 *name = (u8 *)fm_alloc(65536), *extra = (u8 *)fm_alloc(65536);
  char *u8name = (char *)fm_alloc(3 * 65536 + 1);
  if (fm_fseek64(a->f, (i64)pos, SEEK_SET) != 0) e = FM_ERR_IO;
  u64 seen = 0;
  while (!e && pos + 46 <= end) {
    u8 h[46];
    if (fread(h, 1, 46, a->f) != 46) { e = FM_ERR_FORMAT; break; }
    if (g32(h) != 0x02014b50) { e = FM_ERR_FORMAT; break; }
    size_t nlen = g16(h + 28), xlen = g16(h + 30), clen = g16(h + 32);
    if (pos + 46 + nlen + xlen + clen > end) { e = FM_ERR_FORMAT; break; }
    if (fread(name, 1, nlen, a->f) != nlen || fread(extra, 1, xlen, a->f) != xlen) {
      e = FM_ERR_FORMAT;
      break;
    }
    if (clen && fm_fseek64(a->f, (i64)clen, SEEK_CUR) != 0) { e = FM_ERR_IO; break; }
    pos += 46 + nlen + xlen + clen;

    u16 made = g16(h + 4), flags = g16(h + 8), method = g16(h + 10);
    u32 csize32 = g32(h + 20), usize32 = g32(h + 24), off32 = g32(h + 42), eattr = g32(h + 38);
    u64 usize = usize32, csize = csize32, off = off32;
    ZipEnt z;
    memset(&z, 0, sizeof z);
    z.flags = flags;
    z.method = method;
    z.crc = g32(h + 16);
    z.dtime = g16(h + 12);
    i64 mtime = 0;
    int rank = 0;
    const u8 *up = NULL;
    size_t uplen = 0;
    u32 upcrc = 0;
    parse_extra(extra, xlen, &usize, &csize, &off, usize32, csize32, off32, &mtime, &rank, &z,
                &up, &uplen, &upcrc);
    if (method != 99) { z.aes = 0; z.method = method; }
    if (!rank) mtime = dos_to_unix(z.dtime, g16(h + 14));
    if (up && crc32_update(0, name, nlen) == upcrc && arc_valid_utf8(up, uplen))
      arc_name_utf8(up, uplen, true, u8name);
    else
      arc_name_utf8(name, nlen, (flags & 0x800) != 0, u8name);

    int host = made >> 8;
    u32 mode = 0;
    bool is_dir = nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\');
    bool is_link = false;
    if (host == 3 || host == 19) {           /* Unix, OS X */
      mode = eattr >> 16;
      if ((mode & 0170000) == 0040000) is_dir = true;
      if ((mode & 0170000) == 0120000) is_link = true;
    } else if (eattr & 0x10) {
      is_dir = true;
    }
    if (off > (u64)a->file_size || off + shift > (u64)a->file_size) { e = FM_ERR_FORMAT; break; }
    z.off = off + shift;
    z.csize = csize;
    int idx = arc_add(a, u8name, usize, csize, mtime, is_dir);
    zpush(zp, idx);
    zp->z[idx] = z;
    FmArcEntry *ae = arc_at(a, idx);
    ae->crc = z.crc;
    ae->mode = mode & 07777;
    ae->is_link = is_link && !is_dir;
    ae->method = (u8)(z.method > 255 ? 255 : z.method);
    ae->encrypted = (flags & 1) != 0;
    if (ae->encrypted) a->any_encrypted = true;
    seen++;
  }
  fm_free(name);
  fm_free(extra);
  fm_free(u8name);
  if (!e && seen != count && count != 0xFFFF) fm_log("zip: directory lists %llu entries, found %llu",
                                                    (unsigned long long)count,
                                                    (unsigned long long)seen);
  return e;
}

static void zip_close(FmArc *a) {
  ZipPriv *zp = (ZipPriv *)a->priv;
  if (!zp) return;
  fm_free(zp->z);
  fm_free(zp);
  a->priv = NULL;
}

/* ---- reading: entry data ------------------------------------------------ */

typedef struct ZRd {
  FILE *f;
  u64 left;
  int mode;             /* 0 plain, 1 ZipCrypto, 2 AES */
  FmZipCrypto zc;
  FmAes aes;
  FmAesCtr ctr;
  FmHmacSha1 mac;
} ZRd;

static long zrd_read(void *ud, void *buf, size_t n) {
  ZRd *r = (ZRd *)ud;
  if (!r->left) return 0;
  if ((u64)n > r->left) n = (size_t)r->left;
  size_t k = fread(buf, 1, n, r->f);
  if (!k) return ferror(r->f) ? -1 : 0;
  r->left -= k;
  if (r->mode == 1) {
    zipcrypto_decrypt(&r->zc, (u8 *)buf, k);
  } else if (r->mode == 2) {
    hmac_sha1_update(&r->mac, buf, k);
    aes_ctr_le(&r->aes, &r->ctr, (u8 *)buf, k);
  }
  return (long)k;
}

static bool codec_for(u16 method, FmCodec *c) {
  switch (method) {
    case 0: *c = CODEC_STORE; return true;
    case 8: *c = CODEC_DEFLATE; return true;
    case 12: *c = CODEC_BZIP2; return true;
    case 14: *c = CODEC_ZIP_LZMA; return true;
    case 93: *c = CODEC_ZSTD; return true;
    case 95: *c = CODEC_XZ; return true;
    default: return false;
  }
}

/* Decodes entry i into the sink (already begun). */
static FmErr zip_entry(FmArc *a, int i, FmArcSink *s, u8 *buf, ZRd *r) {
  ZipPriv *zp = (ZipPriv *)a->priv;
  const ZipEnt *z = &zp->z[i];
  const FmArcEntry *ae = &a->e[i];
  zp->wrong_pw = false;
  if (z->flags & 0x40) return FM_ERR_UNSUPPORTED;          /* PKWARE strong encryption */
  FmCodec codec;
  if (!codec_for(z->method, &codec)) return FM_ERR_UNSUPPORTED;
  u8 lh[30];
  if (!rd_at(a->f, (i64)z->off, lh, 30) || g32(lh) != 0x04034b50) return FM_ERR_FORMAT;
  u64 data = z->off + 30 + g16(lh + 26) + g16(lh + 28);
  if (data > (u64)a->file_size || z->csize > (u64)a->file_size - data) return FM_ERR_FORMAT;
  if (fm_fseek64(a->f, (i64)data, SEEK_SET) != 0) return FM_ERR_IO;
  memset(r, 0, sizeof *r);
  r->f = a->f;
  r->left = z->csize;
  if (z->flags & 1) {
    FmErr e = arc_get_password(a, false);
    if (e) return e;
    const char *pw = a->password;
    if (z->aes) {
      int kl = 8 + 8 * z->aes, sl = 4 + 4 * z->aes;
      u8 salt[16], ver[2], keys[66];
      if (z->aes > 3) return FM_ERR_UNSUPPORTED;
      if (z->csize < (u64)sl + 2 + 10) return FM_ERR_FORMAT;
      if (fread(salt, 1, (size_t)sl, a->f) != (size_t)sl || fread(ver, 1, 2, a->f) != 2)
        return FM_ERR_FORMAT;
      pbkdf2_sha1((const u8 *)pw, strlen(pw), salt, (size_t)sl, 1000, keys, (size_t)(2 * kl + 2));
      if (keys[2 * kl] != ver[0] || keys[2 * kl + 1] != ver[1]) {
        wipe(keys, sizeof keys);
        zp->wrong_pw = true;
        return FM_ERR_PASSWORD;
      }
      aes_init(&r->aes, keys, kl);
      hmac_sha1_init(&r->mac, keys + kl, (size_t)kl);
      wipe(keys, sizeof keys);
      r->mode = 2;
      r->left = z->csize - (u64)sl - 2 - 10;
    } else {
      u8 hdr[12];
      if (z->csize < 12) return FM_ERR_FORMAT;
      if (fread(hdr, 1, 12, a->f) != 12) return FM_ERR_FORMAT;
      zipcrypto_init(&r->zc, pw);
      zipcrypto_decrypt(&r->zc, hdr, 12);
      bool ok = hdr[11] == (u8)(z->crc >> 24) || ((z->flags & 8) && hdr[11] == (u8)(z->dtime >> 8));
      if (!ok) {
        zp->wrong_pw = true;
        return FM_ERR_PASSWORD;
      }
      r->mode = 1;
      r->left = z->csize - 12;
    }
  }
  FmErr e;
  FmIn *in = in_open_cb(codec, zrd_read, r, &e);
  if (!in) return e == FM_ERR_FORMAT && (z->flags & 1) && !zp->pw_ok ? FM_ERR_CRC : e;
  u32 crc = 0;
  u64 got = 0;
  for (;;) {
    long n = in_read(in, buf, ARC_BUF);
    if (n < 0) { e = in_error(in); break; }
    if (n == 0) break;
    crc = crc32_update(crc, buf, (size_t)n);
    got += (u64)n;
    if (got > ae->size) { e = FM_ERR_FORMAT; break; }     /* more than declared: corrupt */
    e = sink_write(s, buf, (size_t)n);
    if (e) break;
    if (got == ae->size && codec == CODEC_ZIP_LZMA) break;   /* no end marker */
  }
  in_close(in);
  if (e) return e;
  if (r->mode == 2) {
    while (r->left) {                     /* the MAC covers every encrypted byte */
      long k = zrd_read(r, buf, ARC_BUF);
      if (k <= 0) return FM_ERR_FORMAT;
    }
    u8 code[10], mac[20];
    if (fread(code, 1, 10, a->f) != 10) return FM_ERR_FORMAT;
    hmac_sha1_final(&r->mac, mac);
    if (memcmp(code, mac, 10) != 0) return FM_ERR_CRC;
  }
  if (got != ae->size) return FM_ERR_FORMAT;
  bool check_crc = !(r->mode == 2 && z->aes_ver == 2);
  if (check_crc && crc != z->crc) return FM_ERR_CRC;
  if (z->flags & 1) zp->pw_ok = true;
  return FM_OK;
}

static FmErr zip_extract(FmArc *a, const u8 *sel, FmArcSink *s) {
  ZipPriv *zp = (ZipPriv *)a->priv;
  u8 *buf = (u8 *)fm_alloc(ARC_BUF);
  ZRd *r = (ZRd *)fm_alloc(sizeof *r);
  FmErr e = FM_OK;
  bool unsupported = false;
  for (int i = 0; i < a->n && !e; i++) {
    if (!sel[i]) continue;
    if (sink_remaining(s) <= 0) break;
    for (int tries = 0;; tries++) {
      bool skip;
      e = sink_begin(s, i, &skip);
      if (e || skip) break;
      FmErr ee = zip_entry(a, i, s, buf, r);
      bool enc = (zp->z[i].flags & 1) != 0;
      bool retry = enc && tries < 3 &&
                   (zp->wrong_pw || ((ee == FM_ERR_CRC || ee == FM_ERR_FORMAT) && !zp->pw_ok));
      if (retry) {
        sink_end(s, FM_ERR_PASSWORD);
        zp->pw_ok = false;
        e = arc_get_password(a, true);
        if (e) break;
        continue;
      }
      if (enc && zp->wrong_pw) ee = FM_ERR_PASSWORD;
      e = sink_end(s, ee);
      if (e == FM_ERR_UNSUPPORTED) {     /* PPMd, Deflate64 ...: do the rest, report at the end */
        unsupported = true;
        e = FM_OK;
      }
      break;
    }
  }
  if (!e && unsupported) e = FM_ERR_UNSUPPORTED;
  wipe(r, sizeof *r);
  fm_free(r);
  fm_free(buf);
  return e;
}

const FmArcBackend g_arc_zip = { zip_open, zip_extract, zip_close };

/* ---- writing ------------------------------------------------------------ */

typedef struct CdRec {
  u64 off, csize, usize;
  u32 crc, eattr;
  i64 mtime;
  u16 flags, method, dtime, ddate, need;
  const char *name;
  bool dir, aes;
} CdRec;

typedef struct ZWr {
  FILE *f;
  u64 pos;              /* bytes written so far = current offset */
  u64 csize;            /* bytes of the current entry's data */
  int mode;             /* 0 plain, 1 ZipCrypto, 2 AES */
  FmZipCrypto zc;
  FmAes aes;
  FmAesCtr ctr;
  FmHmacSha1 mac;
  u8 *ebuf;             /* encryption needs a writable copy */
} ZWr;

static FmErr zw_raw(ZWr *w, const void *p, size_t n) {
  if (n && fwrite(p, 1, n, w->f) != n) return FM_ERR_FULL;
  w->pos += n;
  return FM_OK;
}

static FmErr zw_data(void *ud, const void *buf, size_t n) {
  ZWr *w = (ZWr *)ud;
  const u8 *p = (const u8 *)buf;
  while (n) {
    size_t k = FM_MIN(n, (size_t)ARC_BUF);
    const u8 *out = p;
    if (w->mode) {
      memcpy(w->ebuf, p, k);
      if (w->mode == 1) {
        zipcrypto_encrypt(&w->zc, w->ebuf, k);
      } else {
        aes_ctr_le(&w->aes, &w->ctr, w->ebuf, k);
        hmac_sha1_update(&w->mac, w->ebuf, k);
      }
      out = w->ebuf;
    }
    FmErr e = zw_raw(w, out, k);
    if (e) return e;
    w->csize += k;
    p += k;
    n -= k;
  }
  return FM_OK;
}

static size_t z64_extra(u8 *x, const CdRec *c, bool cd) {
  u8 *p = x + 4;
  if (!cd || c->usize >= 0xFFFFFFFFu) p = p64(p, c->usize);
  if (!cd || c->csize >= 0xFFFFFFFFu) p = p64(p, c->csize);
  if (cd && c->off >= 0xFFFFFFFFu) p = p64(p, c->off);
  size_t n = (size_t)(p - x);
  if (n == 4) return 0;
  p16(x, 0x0001);
  p16(x + 2, (u32)(n - 4));
  return n;
}

static size_t other_extras(u8 *p, const CdRec *c) {
  u8 *s = p;
  p = p16(p, 0x5455);
  p = p16(p, 5);
  *p++ = 1;
  p = p32(p, (u32)(c->mtime < 0 ? 0 : c->mtime > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (u32)c->mtime));
  if (c->aes) {
    p = p16(p, 0x9901);
    p = p16(p, 7);
    p = p16(p, 2);                 /* AE-2 */
    *p++ = 'A';
    *p++ = 'E';
    *p++ = 3;                      /* AES-256 */
    p = p16(p, c->method);
  }
  return (size_t)(p - s);
}

/* Streams one file's data (with encryption headers) and fills c. */
static FmErr zip_put_data(ZWr *zw, FmArcWriteCtx *w, const FmArcSrc *src, CdRec *c, int level,
                          const char *pw, u8 *buf, bool z64) {
  zw->csize = 0;
  zw->mode = 0;
  FmErr e = FM_OK;
  if (pw) {
    if (c->aes) {
      u8 salt[16], keys[66];
      plat_random(salt, sizeof salt);
      pbkdf2_sha1((const u8 *)pw, strlen(pw), salt, 16, 1000, keys, 66);
      aes_init(&zw->aes, keys, 32);
      hmac_sha1_init(&zw->mac, keys + 32, 32);
      memset(&zw->ctr, 0, sizeof zw->ctr);
      e = zw_raw(zw, salt, 16);
      if (!e) e = zw_raw(zw, keys + 64, 2);
      wipe(keys, sizeof keys);
      zw->csize = 18;
      zw->mode = 2;
    } else {
      u8 hdr[12];
      plat_random(hdr, 11);
      hdr[11] = (u8)(c->dtime >> 8);     /* bit 3 set: the check byte is the time */
      zipcrypto_init(&zw->zc, pw);
      zipcrypto_encrypt(&zw->zc, hdr, 12);
      e = zw_raw(zw, hdr, 12);
      zw->csize = 12;
      zw->mode = 1;
    }
    if (e) return e;
  }
  FILE *in = fm_fopen(src->abs, "rb");
  if (!in) return FM_ERR_ACCESS;
  FmOut *out = out_open_cb(c->method == 8 ? CODEC_DEFLATE : CODEC_STORE, level, zw_data, zw, &e);
  u32 crc = 0;
  u64 n = 0;
  while (out && !e) {
    size_t k = fread(buf, 1, ARC_BUF, in);
    if (!k) {
      if (ferror(in)) e = FM_ERR_IO;
      break;
    }
    crc = crc32_update(crc, buf, k);
    n += k;
    e = out_write(out, buf, k);
    if (!e && !arc_wprogress(w, k)) e = FM_ERR_CANCEL;
  }
  if (out) {
    FmErr ce = out_close(out);
    if (!e) e = ce;
  }
  fclose(in);
  if (e) return e;
  if (zw->mode == 2) {
    u8 mac[20];
    hmac_sha1_final(&zw->mac, mac);
    e = zw_raw(zw, mac, 10);
    zw->csize += 10;
    if (e) return e;
  }
  zw->mode = 0;
  c->crc = c->aes ? 0 : crc;
  c->usize = n;
  c->csize = zw->csize;
  if (!z64 && (c->usize >= 0xFFFFFFFFu || c->csize >= 0xFFFFFFFFu)) return FM_ERR_IO;  /* grew */
  return FM_OK;
}

FmErr zip_write(FILE *out, FmArcWriteCtx *w) {
  const FmArcOpts *o = w->o;
  const char *pw = (o->password && o->password[0]) ? o->password : NULL;
  int level = o->level < 0 ? 6 : FM_MIN(o->level, 9);
  CdRec *cd = (CdRec *)fm_calloc((size_t)(w->n > 0 ? w->n : 1), sizeof *cd);
  u8 *buf = (u8 *)fm_alloc(ARC_BUF);
  ZWr zw;
  memset(&zw, 0, sizeof zw);
  zw.f = out;
  zw.ebuf = (u8 *)fm_alloc(ARC_BUF);
  u8 *h = (u8 *)fm_alloc(30 + 65536 + 128);
  FmErr e = FM_OK;
  int nrec = 0;
#ifdef FM_WIN
  u16 made = 63;
#else
  u16 made = (3 << 8) | 63;
#endif
  for (int i = 0; i < w->n && !e; i++) {
    const FmArcSrc *src = &w->src[i];
    if (w->cb && w->cb->entry) w->cb->entry(w->cb->ud, src->name);
    CdRec *c = &cd[nrec];
    memset(c, 0, sizeof *c);
    size_t nlen = strlen(src->name);
    if (nlen + 1 > 65535) continue;
    c->name = src->name;
    c->dir = src->is_dir;
    c->mtime = src->mtime;
    unix_to_dos(src->mtime, &c->dtime, &c->ddate);
    bool enc = pw && !c->dir;
    c->aes = enc && o->zip_aes;
    c->method = (c->dir || level == 0 || src->size == 0) ? 0 : 8;
    bool z64 = !c->dir && src->size >= 0xFFFF0000u;
    c->flags = 0x800;
    if (enc) c->flags |= 1;
    if (enc && !c->aes) c->flags |= 8;
    if (c->method == 8 && level >= 8) c->flags |= 2;
    else if (c->method == 8 && level <= 2) c->flags |= 4;
    c->need = c->aes ? 51 : z64 ? 45 : (c->method == 8 || c->dir || enc) ? 20 : 10;
    u32 mode = src->mode & 07777;
#ifdef FM_WIN
    FM_UNUSED(mode);
    c->eattr = c->dir ? 0x10 : 0x20;
#else
    c->eattr = ((mode | (c->dir ? 0040000u : 0100000u)) << 16) | (c->dir ? 0x10u : 0);
#endif
    c->off = zw.pos;
    /* local header */
    u8 *p = h;
    p = p32(p, 0x04034b50);
    p = p16(p, c->need);
    p = p16(p, c->flags);
    p = p16(p, c->aes ? 99 : c->method);
    p = p16(p, c->dtime);
    p = p16(p, c->ddate);
    p = p32(p, 0);
    p = p32(p, z64 ? 0xFFFFFFFFu : 0);
    p = p32(p, z64 ? 0xFFFFFFFFu : 0);
    p = p16(p, (u32)(nlen + (c->dir ? 1 : 0)));
    u8 *xlen = p;
    p += 2;
    memcpy(p, src->name, nlen);
    p += nlen;
    if (c->dir) *p++ = '/';
    u8 *x0 = p;
    size_t z64_at = 0;
    if (z64) {
      z64_at = (size_t)(p - h);
      p += z64_extra(p, c, false);
    }
    p += other_extras(p, c);
    p16(xlen, (u32)(p - x0));
    e = zw_raw(&zw, h, (size_t)(p - h));
    if (e) break;
    if (!c->dir) {
      e = zip_put_data(&zw, w, src, c, level, enc ? pw : NULL, buf, z64);
      if (e) break;
      if (c->flags & 8) {               /* data descriptor */
        p = h;
        p = p32(p, 0x08074b50);
        p = p32(p, c->crc);
        if (z64) { p = p64(p, c->csize); p = p64(p, c->usize); }
        else { p = p32(p, (u32)c->csize); p = p32(p, (u32)c->usize); }
        e = zw_raw(&zw, h, (size_t)(p - h));
      } else {                          /* patch the local header */
        u8 fix[12];
        p = p32(fix, c->crc);
        p32(p, z64 ? 0xFFFFFFFFu : (u32)c->csize);
        p32(p + 4, z64 ? 0xFFFFFFFFu : (u32)c->usize);
        if (fm_fseek64(out, (i64)c->off + 14, SEEK_SET) != 0 || fwrite(fix, 1, 12, out) != 12)
          e = FM_ERR_IO;
        if (!e && z64) {
          u8 zx[16];
          p64(zx, c->usize);
          p64(zx + 8, c->csize);
          if (fm_fseek64(out, (i64)(c->off + z64_at + 4), SEEK_SET) != 0 ||
              fwrite(zx, 1, 16, out) != 16)
            e = FM_ERR_IO;
        }
        if (!e && fm_fseek64(out, 0, SEEK_END) != 0) e = FM_ERR_IO;
      }
    }
    nrec++;
  }
  /* central directory */
  u64 cd_off = zw.pos;
  for (int i = 0; i < nrec && !e; i++) {
    const CdRec *c = &cd[i];
    size_t nlen = strlen(c->name) + (c->dir ? 1 : 0);
    u8 *p = h;
    p = p32(p, 0x02014b50);
    bool big = c->off >= 0xFFFFFFFFu || c->csize >= 0xFFFFFFFFu || c->usize >= 0xFFFFFFFFu;
    p = p16(p, made);
    p = p16(p, big ? FM_MAX(c->need, 45) : c->need);
    p = p16(p, c->flags);
    p = p16(p, c->aes ? 99 : c->method);
    p = p16(p, c->dtime);
    p = p16(p, c->ddate);
    p = p32(p, c->crc);
    p = p32(p, c->csize >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)c->csize);
    p = p32(p, c->usize >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)c->usize);
    p = p16(p, (u32)nlen);
    u8 *xlen = p;
    p += 2;
    p = p16(p, 0);                      /* comment */
    p = p16(p, 0);                      /* disk */
    p = p16(p, 0);                      /* internal attributes */
    p = p32(p, c->eattr);
    p = p32(p, c->off >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)c->off);
    memcpy(p, c->name, strlen(c->name));
    p += strlen(c->name);
    if (c->dir) *p++ = '/';
    u8 *x0 = p;
    p += z64_extra(p, c, true);
    p += other_extras(p, c);
    p16(xlen, (u32)(p - x0));
    e = zw_raw(&zw, h, (size_t)(p - h));
  }
  u64 cd_size = zw.pos - cd_off;
  if (!e && (nrec >= 0xFFFF || cd_off >= 0xFFFFFFFFu || cd_size >= 0xFFFFFFFFu)) {
    u64 z64_pos = zw.pos;
    u8 *p = h;
    p = p32(p, 0x06064b50);
    p = p64(p, 44);
    p = p16(p, made);
    p = p16(p, 45);
    p = p32(p, 0);
    p = p32(p, 0);
    p = p64(p, (u64)nrec);
    p = p64(p, (u64)nrec);
    p = p64(p, cd_size);
    p = p64(p, cd_off);
    p = p32(p, 0x07064b50);
    p = p32(p, 0);
    p = p64(p, z64_pos);
    p = p32(p, 1);
    e = zw_raw(&zw, h, (size_t)(p - h));
  }
  if (!e) {
    u8 *p = h;
    p = p32(p, 0x06054b50);
    p = p16(p, 0);
    p = p16(p, 0);
    p = p16(p, nrec >= 0xFFFF ? 0xFFFF : (u32)nrec);
    p = p16(p, nrec >= 0xFFFF ? 0xFFFF : (u32)nrec);
    p = p32(p, cd_size >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)cd_size);
    p = p32(p, cd_off >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)cd_off);
    p = p16(p, 0);
    e = zw_raw(&zw, h, (size_t)(p - h));
  }
  wipe(&zw.zc, sizeof zw.zc);
  wipe(&zw.aes, sizeof zw.aes);
  wipe(&zw.mac, sizeof zw.mac);
  fm_free(zw.ebuf);
  fm_free(h);
  fm_free(buf);
  fm_free(cd);
  return e;
}
