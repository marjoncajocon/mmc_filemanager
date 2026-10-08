/* ftest_arc.c -- self tests for the archive core: crypto vectors, the
** streaming codecs, and create/list/extract round trips for zip, tar.* and
** the single-file formats.
**
** Design decisions:
**   - Known-answer vectors from the standards (FIPS-197, SP 800-38A,
**     RFC 2202, RFC 6070) rather than our own outputs.
**   - The archive tests build a small tree (about 5 MB, with an
**     incompressible and a compressible file, empty file and folder, a
**     UTF-8 name) and push it through every format we can write, then
**     compare the extracted bytes with the originals.
**   - Hostile cases are made by hand: a zip with "../" names, a wrong
**     password, a cancel in the middle, truncated and corrupted streams.
*/
#include "ftest.h"
#include "farc_int.h"
#include "fcrypt.h"
#include "fasm.h"
#include "fstream.h"

/* ---- helpers ------------------------------------------------------------ */

static int unhex(const char *h, u8 *out) {
  int n = 0;
  while (h[0] && h[1]) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
      char c = h[i];
      v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    out[n++] = (u8)v;
    h += 2;
  }
  return n;
}

static bool eq_hex(const u8 *got, const char *hex) {
  u8 want[64];
  int n = unhex(hex, want);
  return memcmp(got, want, (size_t)n) == 0;
}

static u64 g_rng = 88172645463325252ull;
static u64 rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}
static void rnd_fill(u8 *p, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = (u8)(rnd() >> 24);
}

/* ---- crypto ------------------------------------------------------------- */

static void check_aes(const char *key, const char *pt, const char *ct) {
  u8 k[32], p[16], c[16], o[16];
  int kl = unhex(key, k);
  unhex(pt, p);
  unhex(ct, c);
  FmAes a;
  aes_init(&a, k, kl);
  aes_encrypt_block(&a, p, o);
  TEST_CHECK(memcmp(o, c, 16) == 0);
  aes_decrypt_block(&a, c, o);
  TEST_CHECK(memcmp(o, p, 16) == 0);
  aes_init_soft(&a, k, kl);
  aes_encrypt_block(&a, p, o);
  TEST_CHECK(memcmp(o, c, 16) == 0);
  aes_decrypt_block(&a, c, o);
  TEST_CHECK(memcmp(o, p, 16) == 0);
}

static void check_hmac(const u8 *key, size_t kl, const char *msg, const char *want) {
  FmHmacSha1 h;
  u8 out[20];
  hmac_sha1_init(&h, key, kl);
  hmac_sha1_update(&h, msg, strlen(msg));
  hmac_sha1_final(&h, out);
  TEST_CHECK(eq_hex(out, want));
}

static void check_pbkdf2(const char *pw, const char *salt, u32 iters, const char *want) {
  u8 out[32];
  size_t n = strlen(want) / 2;
  pbkdf2_sha1((const u8 *)pw, strlen(pw), (const u8 *)salt, strlen(salt), iters, out, n);
  TEST_CHECK(eq_hex(out, want));
}

int test_crypto(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  u8 out[64];

  /* FIPS-197 appendix C */
  check_aes("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff",
            "69c4e0d86a7b0430d8cdb78070b4c55a");
  check_aes("000102030405060708090a0b0c0d0e0f1011121314151617",
            "00112233445566778899aabbccddeeff", "dda97ca4864cdfe06eaf70a0ec0d7191");
  check_aes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
            "00112233445566778899aabbccddeeff", "8ea2b7ca516745bfeafc49904b496089");

  /* SP 800-38A F.2.1 / F.2.2, CBC-AES128 */
  {
    u8 key[16], iv[16], buf[32];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("000102030405060708090a0b0c0d0e0f", iv);
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", buf);
    FmAes a;
    aes_init(&a, key, 16);
    aes_cbc_encrypt(&a, iv, buf, 32);
    TEST_CHECK(eq_hex(buf, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"));
    unhex("000102030405060708090a0b0c0d0e0f", iv);
    aes_cbc_decrypt(&a, iv, buf, 32);
    TEST_CHECK(eq_hex(buf, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"));
  }

  /* CTR, little-endian counter from 1, split at odd offsets */
  {
    u8 key[32], data[100], ref[100], blk[16], ctr[16];
    rnd_fill(key, 32);
    rnd_fill(data, sizeof data);
    memcpy(ref, data, sizeof data);
    FmAes a;
    aes_init(&a, key, 32);
    memset(ctr, 0, 16);
    for (int i = 0; i < 100; i += 16) {
      ctr[0]++;
      aes_encrypt_block(&a, ctr, blk);
      for (int k = 0; k < 16 && i + k < 100; k++) ref[i + k] ^= blk[k];
    }
    FmAesCtr c;
    memset(&c, 0, sizeof c);
    aes_ctr_le(&a, &c, data, 7);
    aes_ctr_le(&a, &c, data + 7, 40);
    aes_ctr_le(&a, &c, data + 47, 53);
    TEST_CHECK(memcmp(data, ref, sizeof data) == 0);
  }

  /* hardware AES against the C tables on random data */
  {
    FmAes hw, sw;
    u8 key[32], in[64], o1[64], o2[64];
    for (int round = 0; round < 3; round++) {
      int kl = 16 + 8 * round;
      rnd_fill(key, 32);
      rnd_fill(in, sizeof in);
      aes_init(&hw, key, kl);
      aes_init_soft(&sw, key, kl);
      for (int b = 0; b < 64; b += 16) {
        aes_encrypt_block(&hw, in + b, o1 + b);
        aes_encrypt_block(&sw, in + b, o2 + b);
      }
      TEST_CHECK(memcmp(o1, o2, 64) == 0);
      for (int b = 0; b < 64; b += 16) {
        aes_decrypt_block(&hw, o1 + b, o1 + b);
        aes_decrypt_block(&sw, o2 + b, o2 + b);
      }
      TEST_CHECK(memcmp(o1, in, 64) == 0 && memcmp(o2, in, 64) == 0);
    }
    printf("  aes: %s\n", hw.hw ? "hardware path checked against C" : "C tables (no hardware AES)");
  }

  /* SHA-1, SHA-256 */
  {
    FmSha1 s;
    sha1_init(&s);
    sha1_update(&s, "abc", 3);
    sha1_final(&s, out);
    TEST_CHECK(eq_hex(out, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    FmSha256 t;
    sha256_init(&t);
    sha256_update(&t, "abc", 3);
    sha256_final(&t, out);
    TEST_CHECK(eq_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    u8 *a = (u8 *)fm_alloc(1000);
    memset(a, 'a', 1000);
    sha256_init(&t);
    sha1_init(&s);
    for (int i = 0; i < 1000; i++) {
      sha256_update(&t, a, 1000);
      sha1_update(&s, a, 1000);
    }
    sha256_final(&t, out);
    TEST_CHECK(eq_hex(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    sha1_final(&s, out);
    TEST_CHECK(eq_hex(out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
    fm_free(a);
  }

  /* HMAC-SHA1, RFC 2202 cases 1, 2 and 6 */
  {
    u8 k[80];
    memset(k, 0x0b, 20);
    check_hmac(k, 20, "Hi There", "b617318655057264e28bc0b6fb378c8ef146be00");
    check_hmac((const u8 *)"Jefe", 4, "what do ya want for nothing?",
               "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    memset(k, 0xaa, 80);
    check_hmac(k, 80, "Test Using Larger Than Block-Size Key - Hash Key First",
               "aa4ae5e15272d00e95705637ce8a3b55ed402112");
  }

  /* PBKDF2-HMAC-SHA1, RFC 6070 */
  check_pbkdf2("password", "salt", 1, "0c60c80f961f0e71f3a9b524af6012062fe037a6");
  check_pbkdf2("password", "salt", 2, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
  check_pbkdf2("password", "salt", 4096, "4b007901b765489abead49d926f721d065a429c1");
  check_pbkdf2("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
               "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");

  /* CRC32, one shot and in uneven pieces */
  TEST_CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
  {
    u8 *d = (u8 *)fm_alloc(10000);
    rnd_fill(d, 10000);
    u32 whole = crc32_update(0, d, 10000), part = 0;
    for (size_t i = 0, step = 1; i < 10000; i += step, step = step * 3 % 997 + 1)
      part = crc32_update(part, d + i, FM_MIN(step, (size_t)10000 - i));
    TEST_CHECK(whole == part);
    fm_free(d);
  }

  /* ZipCrypto round trip; the keys depend on the plaintext */
  {
    u8 d[64], ref[64];
    rnd_fill(d, 64);
    memcpy(ref, d, 64);
    FmZipCrypto z;
    zipcrypto_init(&z, "pässword");
    zipcrypto_encrypt(&z, d, 64);
    TEST_CHECK(memcmp(d, ref, 64) != 0);
    zipcrypto_init(&z, "pässword");
    zipcrypto_decrypt(&z, d, 30);
    zipcrypto_decrypt(&z, d + 30, 34);
    TEST_CHECK(memcmp(d, ref, 64) == 0);
  }

  {
    u8 w[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    wipe(w, 8);
    TEST_CHECK(w[0] == 0 && w[7] == 0);
  }
  return g_test_fail - before;
}

/* ---- files -------------------------------------------------------------- */

static bool write_file(const char *path, const void *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

static bool files_equal(const char *a, const char *b) {
  FILE *fa = fm_fopen(a, "rb"), *fb = fm_fopen(b, "rb");
  bool eq = fa && fb;
  u8 *x = (u8 *)fm_alloc(ARC_BUF), *y = (u8 *)fm_alloc(ARC_BUF);
  while (eq) {
    size_t na = fread(x, 1, ARC_BUF, fa), nb = fread(y, 1, ARC_BUF, fb);
    if (na != nb || memcmp(x, y, na) != 0) eq = false;
    if (!na) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  fm_free(x);
  fm_free(y);
  return eq;
}

static int count_part_files(const char *dir) {
  FmErr err;
  FmDir *d = plat_dir_open(dir, &err);
  if (!d) return 0;
  int n = 0;
  const char *name;
  FmStat st;
  char child[FM_PATH_MAX];
  while (plat_dir_next(d, &name, &st)) {
    fm_path_join(child, sizeof child, dir, name);
    if (st.flags & FM_ST_DIR) n += count_part_files(child);
    else if (fm_ends_with_i(name, ".mmcpart") || fm_ends_with_i(name, ".part")) n++;
  }
  plat_dir_close(d);
  return n;
}

static void pjoin(char *out, const char *a, const char *b) { fm_path_join(out, FM_PATH_MAX, a, b); }

/* ---- codecs ------------------------------------------------------------- */

static void test_codecs(const char *tmp) {
  static const FmCodec kC[] = { CODEC_STORE, CODEC_DEFLATE, CODEC_GZIP, CODEC_BZIP2,
                                CODEC_XZ, CODEC_LZMA, CODEC_ZSTD };
  size_t n = 300000;
  u8 *src = (u8 *)fm_alloc(n), *back = (u8 *)fm_alloc(n + 16);
  for (size_t i = 0; i < n; i++) src[i] = i < n / 2 ? (u8)(rnd() >> 30) : (u8)("abcab"[i % 5]);
  char path[FM_PATH_MAX];
  pjoin(path, tmp, "codec.bin");
  for (int c = 0; c < FM_COUNT(kC); c++) {
    FmErr e;
    FILE *f = fm_fopen(path, "wb");
    FmOut *o = out_open(f, kC[c], 3, &e);
    TEST_CHECK(o != NULL);
    if (!o) { fclose(f); continue; }
    for (size_t i = 0; i < n; i += 7777) out_write(o, src + i, FM_MIN((size_t)7777, n - i));
    TEST_CHECK(out_close(o) == FM_OK);
    fclose(f);
    f = fm_fopen(path, "rb");
    i64 packed = fm_fsize(f);
    FmIn *in = in_open(f, kC[c], -1, &e);
    TEST_CHECK(in != NULL);
    size_t got = 0;
    long r = -1;
    while (in && (r = in_read(in, back + got, FM_MIN((size_t)5000, n + 16 - got))) > 0) got += (size_t)r;
    TEST_CHECK(got == n && memcmp(src, back, n) == 0);
    TEST_CHECK(r == 0 && in && in_error(in) == FM_OK);       /* a clean end, not an error */
    if (in) in_close(in);
    /* truncated: half the compressed bytes */
    if (kC[c] != CODEC_STORE && packed > 64) {
      fm_fseek64(f, 0, SEEK_SET);
      in = in_open(f, kC[c], packed / 2, &e);
      got = 0;
      while (in && (r = in_read(in, back, 4096)) > 0) got += (size_t)r;
      TEST_CHECK(!in || (r < 0 && in_error(in) == FM_ERR_FORMAT));
      if (in) in_close(in);
    }
    fclose(f);
  }
  /* a flipped byte inside gzip data must not go unnoticed */
  {
    FILE *f = fm_fopen(path, "wb");
    FmErr e;
    FmOut *o = out_open(f, CODEC_GZIP, 6, &e);
    out_write(o, src, n);
    out_close(o);
    fclose(f);
    f = fm_fopen(path, "r+b");
    fm_fseek64(f, fm_fsize(f) - 6, SEEK_SET);
    fputc(0x5A, f);
    fclose(f);
    f = fm_fopen(path, "rb");
    FmIn *in = in_open(f, CODEC_GZIP, -1, &e);
    long r;
    while (in && (r = in_read(in, back, 65536)) > 0) {}
    TEST_CHECK(in && in_error(in) == FM_ERR_CRC);
    if (in) in_close(in);
    fclose(f);
  }
  plat_remove_file(path);
  fm_free(src);
  fm_free(back);
}

/* ---- archive round trips ------------------------------------------------- */

#define UTF8_NAME "h\xC3\xA9llo w\xC3\xB6rld \xE2\x9C\x93.txt"

static const char *const kFiles[] = {
  "src/a.txt", "src/empty.txt", "src/rand.bin", "src/comp.txt", "src/sub/b.txt",
  "src/sub/deep/c.txt", ("src/" UTF8_NAME),
};
static const char *const kDirs[] = { "src", "src/sub", "src/sub/deep", "src/emptydir" };

static void native(char *out, const char *base, const char *rel) {
  char r[FM_PATH_MAX];
  fm_strlcpy(r, rel, sizeof r);
  for (char *p = r; *p; p++) if (*p == '/') *p = FM_SEP;
  pjoin(out, base, r);
}

static bool make_tree(const char *tmp) {
  char p[FM_PATH_MAX];
  for (int i = 0; i < FM_COUNT(kDirs); i++) {
    native(p, tmp, kDirs[i]);
    if (plat_mkdirs(p)) return false;
  }
  size_t big = 3 << 20, comp = 2 << 20;
  u8 *buf = (u8 *)fm_alloc(big);
  rnd_fill(buf, big);
  native(p, tmp, "src/rand.bin");
  bool ok = write_file(p, buf, big);
  size_t o = 0;
  for (int line = 0; o < comp; line++) {
    char l[80];
    int k = fm_snprintf(l, sizeof l, "line %d of a rather repetitive text file, %d\n", line,
                        line % 17);
    size_t take = FM_MIN((size_t)k, comp - o);
    memcpy(buf + o, l, take);
    o += take;
  }
  native(p, tmp, "src/comp.txt");
  ok = ok && write_file(p, buf, comp);
  fm_free(buf);
  native(p, tmp, "src/a.txt");
  ok = ok && write_file(p, "hello world\n", 12);
  native(p, tmp, "src/empty.txt");
  ok = ok && write_file(p, "", 0);
  native(p, tmp, "src/sub/b.txt");
  ok = ok && write_file(p, "bee\n", 4);
  native(p, tmp, "src/sub/deep/c.txt");
  ok = ok && write_file(p, "deep sea\n", 9);
  native(p, tmp, "src/" UTF8_NAME);
  ok = ok && write_file(p, "unicode \xE2\x9C\x93\n", 12);
  return ok;
}

static int find(const FmArc *a, const char *path) {
  for (int i = 0; i < arc_count(a); i++)
    if (strcmp(arc_entry(a, i)->path, path) == 0) return i;
  return -1;
}

typedef struct PwCtx { const char *first, *then; int asked; } PwCtx;

static bool pw_cb(void *ud, char *buf, int cap, bool retry) {
  PwCtx *p = (PwCtx *)ud;
  p->asked++;
  fm_strlcpy(buf, retry ? p->then : p->first, (size_t)cap);
  return true;
}

typedef struct CancelCtx { u64 at; bool hit; } CancelCtx;

static bool cancel_cb(void *ud, u64 done, u64 total) {
  CancelCtx *c = (CancelCtx *)ud;
  FM_UNUSED(total);
  if (done >= c->at) { c->hit = true; return false; }
  return true;
}

typedef struct Case { FmArcFmt fmt; int level; const char *pw; bool aes; const char *file; } Case;

static void round_trip(const char *tmp, const Case *c, int k) {
  char src[FM_PATH_MAX], arc[FM_PATH_MAX], dst[FM_PATH_MAX], p1[FM_PATH_MAX], p2[FM_PATH_MAX];
  native(src, tmp, "src");
  pjoin(arc, tmp, c->file);
  char sub[32];
  fm_snprintf(sub, sizeof sub, "x%d", k);
  pjoin(dst, tmp, sub);
  FmArcOpts o;
  memset(&o, 0, sizeof o);
  o.fmt = c->fmt;
  o.level = c->level;
  o.password = c->pw;
  o.zip_aes = c->aes;
  PwCtx pw = { c->pw, c->pw, 0 };
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = &pw;
  cb.password = pw_cb;
  const char *srcs[1] = { src };
  FmErr e = arc_create(arc, srcs, 1, tmp, &o, &cb);
  TEST_CHECK(e == FM_OK);
  if (e) { printf("  %s: create failed: %s\n", c->file, fm_err_str(e)); return; }
  TEST_CHECK(arc_detect(arc) == c->fmt);
  FmArc *a = NULL;
  e = arc_open(arc, &cb, &a);
  TEST_CHECK(e == FM_OK && a);
  if (!a) { printf("  %s: open failed: %s\n", c->file, fm_err_str(e)); return; }
  TEST_CHECK(arc_count(a) == FM_COUNT(kFiles) + FM_COUNT(kDirs));
  for (int i = 0; i < FM_COUNT(kFiles); i++) {
    int idx = find(a, kFiles[i]);
    TEST_CHECK(idx >= 0);
    if (idx < 0) { printf("  %s: missing %s\n", c->file, kFiles[i]); continue; }
    FmStat st;
    native(p1, tmp, kFiles[i]);
    plat_stat(p1, &st);
    TEST_CHECK(arc_entry(a, idx)->size == st.size && !arc_entry(a, idx)->is_dir);
  }
  for (int i = 0; i < FM_COUNT(kDirs); i++) {
    int idx = find(a, kDirs[i]);
    TEST_CHECK(idx >= 0 && arc_entry(a, idx)->is_dir);
  }
  TEST_CHECK(arc_has_encrypted(a) == (c->pw != NULL));
  e = arc_extract(a, NULL, dst, "", &cb);
  TEST_CHECK(e == FM_OK);
  if (e) printf("  %s: extract failed: %s\n", c->file, fm_err_str(e));
  for (int i = 0; i < FM_COUNT(kFiles); i++) {
    native(p1, tmp, kFiles[i]);
    native(p2, dst, kFiles[i]);
    bool same = files_equal(p1, p2);
    TEST_CHECK(same);
    if (!same) printf("  %s: %s differs\n", c->file, kFiles[i]);
  }
  native(p2, dst, "src/emptydir");
  TEST_CHECK(plat_is_dir(p2));
  /* mtimes survive (2 s DOS granularity at worst) */
  {
    FmStat s1, s2;
    native(p1, tmp, "src/comp.txt");
    native(p2, dst, "src/comp.txt");
    TEST_CHECK(plat_stat(p1, &s1) && plat_stat(p2, &s2) && s1.mtime - s2.mtime <= 2 &&
               s2.mtime - s1.mtime <= 2);
  }
  arc_close(a);
}

static void single_trip(const char *tmp, FmArcFmt fmt, const char *file, bool size_known) {
  char src[FM_PATH_MAX], arc[FM_PATH_MAX], dst[FM_PATH_MAX], base[FM_PATH_MAX];
  native(base, tmp, "src");
  native(src, tmp, "src/comp.txt");
  pjoin(arc, tmp, file);
  FmArcOpts o;
  memset(&o, 0, sizeof o);
  o.fmt = fmt;
  o.level = -1;
  const char *srcs[1] = { src };
  FmErr e = arc_create(arc, srcs, 1, base, &o, NULL);
  TEST_CHECK(e == FM_OK);
  if (e) { printf("  %s: create failed: %s\n", file, fm_err_str(e)); return; }
  TEST_CHECK(arc_detect(arc) == fmt);
  FmArc *a = NULL;
  e = arc_open(arc, NULL, &a);
  TEST_CHECK(e == FM_OK && a && arc_count(a) == 1);
  if (!a) return;
  TEST_CHECK(strcmp(arc_entry(a, 0)->path, "comp.txt") == 0);
  if (size_known) TEST_CHECK(arc_entry(a, 0)->size == (2u << 20));
  pjoin(dst, tmp, "single-out.txt");
  e = arc_extract_one(a, 0, dst, NULL);
  TEST_CHECK(e == FM_OK && files_equal(src, dst));
  if (e) printf("  %s: extract failed: %s\n", file, fm_err_str(e));
  plat_remove_file(dst);
  arc_close(a);
  /* more than one file is not a single-file format */
  const char *two[2] = { src, src };
  pjoin(dst, tmp, "two.gz");
  TEST_CHECK(arc_create(dst, two, 2, base, &o, NULL) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(!plat_exists(dst));
}

/* A stored zip entry by hand (for names our writer would never produce). */
static size_t zip_local(u8 *p, const char *name, const char *data, u32 *crc) {
  size_t nl = strlen(name), dl = strlen(data);
  *crc = crc32_update(0, data, dl);
  u8 h[30] = { 'P', 'K', 3, 4, 10, 0 };
  h[14] = (u8)*crc; h[15] = (u8)(*crc >> 8); h[16] = (u8)(*crc >> 16); h[17] = (u8)(*crc >> 24);
  h[18] = h[22] = (u8)dl;
  h[26] = (u8)nl;
  memcpy(p, h, 30);
  memcpy(p + 30, name, nl);
  memcpy(p + 30 + nl, data, dl);
  return 30 + nl + dl;
}

static size_t zip_central(u8 *p, const char *name, const char *data, u32 crc, u32 off) {
  size_t nl = strlen(name), dl = strlen(data);
  u8 h[46] = { 'P', 'K', 1, 2, 20, 0, 10, 0 };
  h[16] = (u8)crc; h[17] = (u8)(crc >> 8); h[18] = (u8)(crc >> 16); h[19] = (u8)(crc >> 24);
  h[20] = h[24] = (u8)dl;
  h[28] = (u8)nl;
  h[42] = (u8)off; h[43] = (u8)(off >> 8);
  memcpy(p, h, 46);
  memcpy(p + 46, name, nl);
  return 46 + nl;
}

static void test_zip_slip(const char *tmp) {
  static const char *const names[] = { "../evil.txt", "good.txt", "a/../../evil2.txt",
                                        "C:\\evil3.txt", "dir\\inner.txt" };
  u8 *z = (u8 *)fm_alloc(4096);
  size_t n = 0, cd;
  u32 crc[5], off[5];
  for (int i = 0; i < 5; i++) {
    off[i] = (u32)n;
    n += zip_local(z + n, names[i], "payload\n", &crc[i]);
  }
  cd = n;
  for (int i = 0; i < 5; i++) n += zip_central(z + n, names[i], "payload\n", crc[i], off[i]);
  u8 eo[22] = { 'P', 'K', 5, 6 };
  eo[8] = eo[10] = 5;
  eo[12] = (u8)(n - cd); eo[13] = (u8)((n - cd) >> 8);
  eo[16] = (u8)cd; eo[17] = (u8)(cd >> 8);
  memcpy(z + n, eo, 22);
  n += 22;
  char dir[FM_PATH_MAX], arc[FM_PATH_MAX], dst[FM_PATH_MAX], p[FM_PATH_MAX];
  pjoin(dir, tmp, "slip");
  plat_mkdirs(dir);
  pjoin(arc, dir, "slip.zip");
  TEST_CHECK(write_file(arc, z, n));
  fm_free(z);
  pjoin(dst, dir, "inside");
  FmArc *a = NULL;
  TEST_CHECK(arc_open(arc, NULL, &a) == FM_OK && a && arc_count(a) == 5);
  if (!a) return;
  FmErr e = arc_extract(a, NULL, dst, "", NULL);
  TEST_CHECK(e == FM_ERR_ACCESS);          /* the rest extracted, the bad ones refused */
  pjoin(p, dir, "evil.txt");
  TEST_CHECK(!plat_exists(p));
  pjoin(p, tmp, "evil.txt");
  TEST_CHECK(!plat_exists(p));
  pjoin(p, tmp, "evil2.txt");
  TEST_CHECK(!plat_exists(p));
  pjoin(p, dir, "evil2.txt");
  TEST_CHECK(!plat_exists(p));
  pjoin(p, dst, "good.txt");
  TEST_CHECK(plat_exists(p));
  native(p, dst, "dir/inner.txt");
  TEST_CHECK(plat_exists(p));
  arc_close(a);
}

static void test_passwords(const char *tmp, const char *zip, bool aes) {
  char arc[FM_PATH_MAX], dst[FM_PATH_MAX], p1[FM_PATH_MAX], p2[FM_PATH_MAX];
  pjoin(arc, tmp, zip);
  /* wrong first, right on the retry */
  PwCtx pw = { "wrong", "secret", 0 };
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = &pw;
  cb.password = pw_cb;
  FmArc *a = NULL;
  TEST_CHECK(arc_open(arc, &cb, &a) == FM_OK && a);
  if (!a) return;
  pjoin(dst, tmp, aes ? "pw-aes" : "pw-zc");
  FmErr e = arc_extract(a, NULL, dst, "", &cb);
  TEST_CHECK(e == FM_OK && pw.asked == 2);
  native(p1, tmp, "src/rand.bin");
  native(p2, dst, "src/rand.bin");
  TEST_CHECK(files_equal(p1, p2));
  arc_close(a);
  /* always wrong: gives up with FM_ERR_PASSWORD and leaves nothing behind */
  PwCtx bad = { "nope", "still wrong", 0 };
  cb.ud = &bad;
  TEST_CHECK(arc_open(arc, &cb, &a) == FM_OK && a);
  if (!a) return;
  pjoin(dst, tmp, aes ? "pw-aes-bad" : "pw-zc-bad");
  e = arc_extract(a, NULL, dst, "", &cb);
  TEST_CHECK(e == FM_ERR_PASSWORD && bad.asked >= 2 && bad.asked <= 5);
  TEST_CHECK(count_part_files(dst) == 0);
  native(p2, dst, "src/rand.bin");
  TEST_CHECK(!plat_exists(p2));
  arc_close(a);
  /* no callback at all */
  TEST_CHECK(arc_open(arc, NULL, &a) == FM_OK && a);
  if (!a) return;
  TEST_CHECK(arc_extract(a, NULL, dst, "", NULL) == FM_ERR_PASSWORD);
  arc_close(a);
}

static void test_partial(const char *tmp, const char *file) {
  char arc[FM_PATH_MAX], dst[FM_PATH_MAX], p1[FM_PATH_MAX], p2[FM_PATH_MAX];
  pjoin(arc, tmp, file);
  FmArc *a = NULL;
  TEST_CHECK(arc_open(arc, NULL, &a) == FM_OK && a);
  if (!a) return;
  /* one entry to an exact path */
  int idx = find(a, "src/sub/deep/c.txt");
  TEST_CHECK(idx >= 0);
  pjoin(dst, tmp, "one");
  pjoin(p2, dst, "copy-of-c.txt");
  TEST_CHECK(arc_extract_one(a, idx, p2, NULL) == FM_OK);
  native(p1, tmp, "src/sub/deep/c.txt");
  TEST_CHECK(files_equal(p1, p2));
  /* a folder with its parents stripped */
  u8 *sel = (u8 *)fm_calloc((size_t)arc_count(a), 1);
  idx = find(a, "src/sub");
  TEST_CHECK(idx >= 0);
  if (idx >= 0) sel[idx] = 1;
  char sub[64];
  fm_snprintf(sub, sizeof sub, "strip-%s", file);
  pjoin(dst, tmp, sub);
  TEST_CHECK(arc_extract(a, sel, dst, "src/", NULL) == FM_OK);
  native(p2, dst, "sub/deep/c.txt");
  TEST_CHECK(files_equal(p1, p2));
  native(p1, tmp, "src/sub/b.txt");
  native(p2, dst, "sub/b.txt");
  TEST_CHECK(files_equal(p1, p2));
  native(p2, dst, "a.txt");
  TEST_CHECK(!plat_exists(p2));
  native(p2, dst, "src");
  TEST_CHECK(!plat_exists(p2));
  fm_free(sel);
  /* cancel in the middle of the big file */
  CancelCtx cc = { 1 << 20, false };
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = &cc;
  cb.progress = cancel_cb;
  fm_snprintf(sub, sizeof sub, "cancel-%s", file);
  pjoin(dst, tmp, sub);
  sel = (u8 *)fm_calloc((size_t)arc_count(a), 1);
  sel[find(a, "src/rand.bin")] = 1;
  TEST_CHECK(arc_extract(a, sel, dst, "", &cb) == FM_ERR_CANCEL && cc.hit);
  native(p2, dst, "src/rand.bin");
  TEST_CHECK(!plat_exists(p2));
  TEST_CHECK(count_part_files(dst) == 0);
  fm_free(sel);
  arc_close(a);
}

static void test_cancel_create(const char *tmp) {
  char src[FM_PATH_MAX], arc[FM_PATH_MAX];
  native(src, tmp, "src");
  pjoin(arc, tmp, "cancelled.zip");
  FmArcOpts o;
  memset(&o, 0, sizeof o);
  o.fmt = ARC_ZIP;
  o.level = 1;
  CancelCtx cc = { 1 << 20, false };
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = &cc;
  cb.progress = cancel_cb;
  const char *srcs[1] = { src };
  TEST_CHECK(arc_create(arc, srcs, 1, tmp, &o, &cb) == FM_ERR_CANCEL && cc.hit);
  TEST_CHECK(!plat_exists(arc));
  char part[FM_PATH_MAX];
  fm_snprintf(part, sizeof part, "%s.part", arc);
  TEST_CHECK(!plat_exists(part));
}

static void test_detect_peek(const char *tmp) {
  /* a tar.gz under a plain ".gz" name is still recognised as a tar */
  char a[FM_PATH_MAX], b[FM_PATH_MAX];
  pjoin(a, tmp, "t-gz.tar.gz");
  pjoin(b, tmp, "renamed.gz");
  FILE *in = fm_fopen(a, "rb"), *out = fm_fopen(b, "wb");
  TEST_CHECK(in && out);
  if (in && out) {
    u8 buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
  }
  if (in) fclose(in);
  if (out) fclose(out);
  TEST_CHECK(arc_detect(b) == ARC_TAR_GZ);
  plat_remove_file(b);
  pjoin(b, tmp, "nothing.bin");
  write_file(b, "just some bytes that are no archive at all", 42);
  TEST_CHECK(arc_detect(b) == ARC_NONE);
  FmArc *x = NULL;
  TEST_CHECK(arc_open(b, NULL, &x) == FM_ERR_FORMAT && !x);
}

int test_archives(const char *tmp) {
  int before = g_test_fail;
  test_codecs(tmp);
  TEST_CHECK(make_tree(tmp));
  static const Case kCases[] = {
    { ARC_ZIP, 0, NULL, false, "store.zip" },
    { ARC_ZIP, 6, NULL, false, "deflate.zip" },
    { ARC_ZIP, 1, "secret", false, "zipcrypto.zip" },
    { ARC_ZIP, 1, "secret", true, "aes.zip" },
    { ARC_TAR, -1, NULL, false, "plain.tar" },
    { ARC_TAR_GZ, 1, NULL, false, "t-gz.tar.gz" },
    { ARC_TAR_BZ2, 1, NULL, false, "t-bz2.tar.bz2" },
    { ARC_TAR_XZ, 1, NULL, false, "t-xz.tar.xz" },
    { ARC_TAR_ZST, -1, NULL, false, "t-zst.tar.zst" },
  };
  for (int i = 0; i < FM_COUNT(kCases); i++) {
    u64 t0 = plat_now_ms();
    int f0 = g_test_fail;
    round_trip(tmp, &kCases[i], i);
    printf("  %-14s %s (%llu ms)\n", kCases[i].file, g_test_fail == f0 ? "ok" : "FAILED",
           (unsigned long long)(plat_now_ms() - t0));
  }
  single_trip(tmp, ARC_GZ, "comp.txt.gz", true);
  single_trip(tmp, ARC_BZ2, "comp.txt.bz2", false);
  single_trip(tmp, ARC_XZ, "comp.txt.xz", true);
  single_trip(tmp, ARC_ZST, "comp.txt.zst", false);
  single_trip(tmp, ARC_LZMA, "comp.txt.lzma", true);
  test_partial(tmp, "deflate.zip");
  test_partial(tmp, "t-gz.tar.gz");
  test_partial(tmp, "plain.tar");
  test_passwords(tmp, "zipcrypto.zip", false);
  test_passwords(tmp, "aes.zip", true);
  test_zip_slip(tmp);
  test_cancel_create(tmp);
  test_detect_peek(tmp);
  return g_test_fail - before;
}
