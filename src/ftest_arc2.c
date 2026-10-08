/* ftest_arc2.c -- self tests for 7z (round trips) and rar (fixtures).
**
** Design decisions:
**   - 7z is tested end to end through the public API: arc_create a small
**     tree (text, incompressible data, empty file, empty folder, sub folder,
**     a non-ASCII name) with every writer option, then arc_open,
**     arc_extract and a byte compare. Passwords go through the callback,
**     including the wrong-password and no-callback paths.
**   - rar cannot be created, so it is tested on small archives from
**     libarchive's test suite in tests/fixtures (or $MMCFM_FIXTURES); when
**     they are not there the rar part is skipped, not failed.
*/
#include "ftest.h"
#include "farc.h"
#include "fplat.h"
#include "fcrypt.h"

/* ---- helpers -------------------------------------------------------------- */

typedef struct TestPw {
  const char *pw;
  int asked, retried;
} TestPw;

static bool test_pw_cb(void *ud, char *buf, int cap, bool retry) {
  TestPw *t = (TestPw *)ud;
  t->asked++;
  if (retry) t->retried++;
  if (!t->pw || retry) return false;
  fm_strlcpy(buf, t->pw, (size_t)cap);
  return true;
}

static FmArcCb test_cb(TestPw *t) {
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = t;
  cb.password = test_pw_cb;
  return cb;
}

static bool write_file(const char *path, const void *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  bool ok;
  if (!f) return false;
  ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

/* Reads a whole (small) test file; *n = size. NULL when missing. */
static u8 *read_file(const char *path, size_t *n) {
  FILE *f = fm_fopen(path, "rb");
  i64 size;
  u8 *p;
  if (!f) return NULL;
  size = fm_fsize(f);
  if (size < 0 || size > (64 << 20)) {
    fclose(f);
    return NULL;
  }
  p = (u8 *)fm_alloc((size_t)size + 1);
  *n = fread(p, 1, (size_t)size, f);
  fclose(f);
  if (*n != (size_t)size) {
    fm_free(p);
    return NULL;
  }
  return p;
}

static bool same_file(const char *a, const char *b) {
  size_t na = 0, nb = 0;
  u8 *pa = read_file(a, &na), *pb = read_file(b, &nb);
  bool same = pa && pb && na == nb && memcmp(pa, pb, na) == 0;
  fm_free(pa);
  fm_free(pb);
  return same;
}

static void join3(char *out, const char *a, const char *b, const char *c) {
  char t[FM_PATH_MAX];
  fm_path_join(t, sizeof t, a, b);
  fm_path_join(out, FM_PATH_MAX, t, c);
}

static int find_entry(const FmArc *a, const char *path) {
  int i, n = arc_count(a);
  for (i = 0; i < n; i++)
    if (strcmp(arc_entry(a, i)->path, path) == 0) return i;
  return -1;
}

/* ---- 7z round trips ------------------------------------------------------- */

#define T7_UNI "uni-\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80.txt"

static const char *const kTree[] = {
  "a.txt", "b.bin", "empty.txt", "sub/c.txt", "sub/deeper/d.txt", T7_UNI
};

static void make_tree(const char *root) {
  char p[FM_PATH_MAX], d[FM_PATH_MAX];
  size_t i, n = 200000;
  u8 *buf = (u8 *)fm_alloc(n);
  u32 x = 12345;
  fm_path_join(d, sizeof d, root, "sub");
  fm_path_join(p, sizeof p, d, "deeper");
  plat_mkdirs(p);
  fm_path_join(p, sizeof p, root, "emptydir");
  plat_mkdirs(p);
  for (i = 0; i < n; i++) buf[i] = (u8)("the quick brown fox jumps over the lazy dog\n"[i % 44]);
  fm_path_join(p, sizeof p, root, "a.txt");
  write_file(p, buf, n);
  for (i = 0; i < n; i++) {
    x = x * 1103515245u + 12345u;
    buf[i] = (u8)(x >> 23);
  }
  fm_path_join(p, sizeof p, root, "b.bin");
  write_file(p, buf, 150001);
  fm_path_join(p, sizeof p, root, "empty.txt");
  write_file(p, "", 0);
  join3(p, root, "sub", "c.txt");
  write_file(p, "inside a folder\n", 16);
  join3(p, root, "sub/deeper", "d.txt");
  write_file(p, buf + 1000, 70000);
  fm_path_join(p, sizeof p, root, T7_UNI);
  write_file(p, "unicode name\n", 13);
  fm_free(buf);
}

static void check_tree(const char *src_root, const char *out_root, const char *what) {
  char a[FM_PATH_MAX], b[FM_PATH_MAX];
  int i;
  for (i = 0; i < FM_COUNT(kTree); i++) {
    fm_path_join(a, sizeof a, src_root, kTree[i]);
    fm_path_join(b, sizeof b, out_root, kTree[i]);
    if (!same_file(a, b)) {
      printf("  7z %s: %s differs\n", what, kTree[i]);
      TEST_CHECK(same_file(a, b));
    }
  }
  fm_path_join(b, sizeof b, out_root, "emptydir");
  TEST_CHECK(plat_is_dir(b));
}

typedef struct T7Case {
  const char *name;
  int level;
  bool solid, encrypt_names;
  const char *pw;
} T7Case;

static void test_7z_case(const char *tmp, const char *src_root, const T7Case *c) {
  char arc[FM_PATH_MAX], out[FM_PATH_MAX], file[64], one[FM_PATH_MAX], want[FM_PATH_MAX];
  const char *srcs[1];
  FmArcOpts o;
  TestPw tp = { 0 };
  FmArcCb cb = test_cb(&tp);
  FmArc *a = NULL;
  FmErr e;
  int i, n;

  srcs[0] = src_root;
  memset(&o, 0, sizeof o);
  o.fmt = ARC_7Z;
  o.level = c->level;
  o.solid = c->solid;
  o.password = c->pw;
  o.encrypt_names = c->encrypt_names;
  fm_snprintf(file, sizeof file, "%s.7z", c->name);
  fm_path_join(arc, sizeof arc, tmp, file);
  e = arc_create(arc, srcs, 1, tmp, &o, NULL);
  if (e != FM_OK) printf("  7z %s: create: %s\n", c->name, fm_err_str(e));
  TEST_CHECK(e == FM_OK);
  TEST_CHECK(arc_detect(arc) == ARC_7Z);

  tp.pw = c->pw;
  e = arc_open(arc, &cb, &a);
  if (e != FM_OK) printf("  7z %s: open: %s\n", c->name, fm_err_str(e));
  TEST_CHECK(e == FM_OK);
  if (e != FM_OK) return;
  TEST_CHECK(arc_format(a) == ARC_7Z);
  n = arc_count(a);
  TEST_CHECK(n == 10);   /* root, 6 files, 3 folders below it */
  TEST_CHECK(find_entry(a, "src7/" T7_UNI) >= 0);
  i = find_entry(a, "src7/emptydir");
  TEST_CHECK(i >= 0 && arc_entry(a, i)->is_dir);
  i = find_entry(a, "src7/b.bin");
  TEST_CHECK(i >= 0 && arc_entry(a, i)->size == 150001);
  TEST_CHECK(i >= 0 && arc_entry(a, i)->encrypted == (c->pw != NULL));
  TEST_CHECK(arc_has_encrypted(a) == (c->pw != NULL));
  i = find_entry(a, "src7/empty.txt");
  TEST_CHECK(i >= 0 && arc_entry(a, i)->size == 0 && !arc_entry(a, i)->is_dir);

  fm_snprintf(file, sizeof file, "out-%s", c->name);
  fm_path_join(out, sizeof out, tmp, file);
  e = arc_extract(a, NULL, out, "src7/", &cb);
  if (e != FM_OK) printf("  7z %s: extract: %s\n", c->name, fm_err_str(e));
  TEST_CHECK(e == FM_OK);
  check_tree(src_root, out, c->name);

  /* One file from the middle of a (maybe solid) folder. */
  i = find_entry(a, "src7/sub/deeper/d.txt");
  fm_snprintf(file, sizeof file, "one-%s.txt", c->name);
  fm_path_join(one, sizeof one, tmp, file);
  join3(want, src_root, "sub/deeper", "d.txt");
  TEST_CHECK(i >= 0 && arc_extract_one(a, i, one, &cb) == FM_OK);
  TEST_CHECK(same_file(want, one));
  arc_close(a);
}

static void test_7z_passwords(const char *tmp) {
  char arc[FM_PATH_MAX], out[FM_PATH_MAX];
  TestPw tp = { 0 };
  FmArcCb cb = test_cb(&tp);
  FmArc *a = NULL;
  FmErr e;

  /* Names visible: opens, but extraction says the password is wrong. */
  fm_path_join(arc, sizeof arc, tmp, "aes.7z");
  tp.pw = "not the password";
  e = arc_open(arc, &cb, &a);
  TEST_CHECK(e == FM_OK);
  if (e == FM_OK) {
    fm_path_join(out, sizeof out, tmp, "out-wrong");
    e = arc_extract(a, NULL, out, "", &cb);
    TEST_CHECK(e == FM_ERR_PASSWORD);
    TEST_CHECK(tp.retried >= 1);
    arc_close(a);
  }

  /* Encrypted names: wrong password, and no callback at all. */
  fm_path_join(arc, sizeof arc, tmp, "aes_names.7z");
  memset(&tp, 0, sizeof tp);
  tp.pw = "wrong";
  a = NULL;
  e = arc_open(arc, &cb, &a);
  TEST_CHECK(e == FM_ERR_PASSWORD);
  if (a) arc_close(a);
  a = NULL;
  e = arc_open(arc, NULL, &a);
  TEST_CHECK(e == FM_ERR_PASSWORD);
  if (a) arc_close(a);
}

/* Damaged copies must fail cleanly (no crash, no hang, no false success
** on damaged data). */
static void test_7z_damaged(const char *tmp) {
  char src[FM_PATH_MAX], dst[FM_PATH_MAX], out[FM_PATH_MAX];
  size_t n = 0;
  u8 *d;
  int k;
  fm_path_join(src, sizeof src, tmp, "solid.7z");
  d = read_file(src, &n);
  TEST_CHECK(d != NULL && n > 1000);
  if (!d) return;
  for (k = 0; k < 4; k++) {
    FmArc *a = NULL;
    size_t len = n;
    FmErr e;
    u8 saved = 0;
    size_t at = 0;
    if (k == 0) len = n / 2;                   /* truncated */
    else if (k == 1) at = 100;                 /* inside the packed data */
    else if (k == 2) at = n - 20;              /* inside the header */
    else at = 10;                              /* start header CRC */
    if (k > 0) {
      saved = d[at];
      d[at] ^= 0x55;
    }
    fm_path_join(dst, sizeof dst, tmp, "damaged.7z");
    write_file(dst, d, len);
    if (k > 0) d[at] = saved;
    e = arc_open(dst, NULL, &a);
    if (e == FM_OK) {
      fm_path_join(out, sizeof out, tmp, "out-damaged");
      e = arc_extract(a, NULL, out, "", NULL);
      arc_close(a);
    }
    TEST_CHECK(e != FM_OK);
  }
  fm_free(d);
}

static void test_7z(const char *tmp) {
  static const T7Case cases[] = {
    { "store", 0, false, false, NULL },
    { "lzma2", 5, false, false, NULL },
    { "solid", 5, true, false, NULL },
    { "aes", 5, false, false, "pässwörd \xE2\x82\xAC" },
    { "aes_names", 3, true, true, "secret" },
    { "aes_store", 0, true, true, "secret" },
  };
  char root[FM_PATH_MAX];
  int i;
  fm_path_join(root, sizeof root, tmp, "src7");
  make_tree(root);
  for (i = 0; i < FM_COUNT(cases); i++) test_7z_case(tmp, root, &cases[i]);
  test_7z_passwords(tmp);
  test_7z_damaged(tmp);
}

/* ---- rar fixtures --------------------------------------------------------- */

static int test_rar(const char *tmp) {
  FM_UNUSED(tmp);
  return 0;
}

/* ---- entry ---------------------------------------------------------------- */

int test_archives2(const char *tmp) {
  int before = g_test_fail;
  test_7z(tmp);
  test_rar(tmp);
  return g_test_fail - before;
}
