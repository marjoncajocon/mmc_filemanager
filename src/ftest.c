/* ftest.c -- self-test runner and the core (string/path/utf-8) tests.
**
** Design decisions:
**   - Runs without a window: only SDL's base library is loaded, so it works
**     in CI and over ssh.
**   - Each area gets its own empty folder under the system temp folder,
**     removed afterwards unless MMCFM_KEEP_TEST=1.
*/
#include "ftest.h"
#include "fplat.h"

int g_test_fail;

static void rm_tree(const char *path) {
  FmErr err;
  FmDir *d = plat_dir_open(path, &err);
  if (d) {
    const char *name;
    FmStat st;
    char child[FM_PATH_MAX];
    while (plat_dir_next(d, &name, &st)) {
      if (!fm_path_join(child, sizeof child, path, name)) continue;
      if ((st.flags & FM_ST_DIR) && !(st.flags & FM_ST_LINK)) rm_tree(child);
      else plat_remove_file(child);
    }
    plat_dir_close(d);
  }
  plat_remove_dir(path);
}

int test_core(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  char p[FM_PATH_MAX];

  TEST_CHECK(fm_natcmp("file2", "file10") < 0);
  TEST_CHECK(fm_natcmp("File10", "file2") > 0);
  TEST_CHECK(fm_natcmp("a", "a") == 0);
  TEST_CHECK(fm_stricmp("ABC", "abc") == 0);
  TEST_CHECK(fm_ends_with_i("photo.JPG", ".jpg"));
  TEST_CHECK(strcmp(fm_path_ext("a/b.tar.gz"), ".gz") == 0);
  TEST_CHECK(strcmp(fm_path_ext(".bashrc"), "") == 0);
  TEST_CHECK(fm_type_from_name("x.mp3") == FT_AUDIO);
  TEST_CHECK(fm_type_from_name("x.GIF") == FT_GIF);
  TEST_CHECK(fm_type_from_name("x.tar.zst") == FT_ARCHIVE);

#ifdef FM_WIN
  fm_strlcpy(p, "C:\\a\\b\\..\\c\\.\\d\\", sizeof p);
  fm_path_normalize(p);
  TEST_CHECK(strcmp(p, "C:\\a\\c\\d") == 0);
  fm_strlcpy(p, "C:\\a", sizeof p);
  TEST_CHECK(fm_path_parent(p) && strcmp(p, "C:\\") == 0);
  TEST_CHECK(!fm_path_parent(p));
  TEST_CHECK(fm_path_is_inside("C:\\x\\y", "c:\\X"));
#else
  fm_strlcpy(p, "/a/b/../c/./d/", sizeof p);
  fm_path_normalize(p);
  TEST_CHECK(strcmp(p, "/a/c/d") == 0);
  fm_strlcpy(p, "/a", sizeof p);
  TEST_CHECK(fm_path_parent(p) && strcmp(p, "/") == 0);
  TEST_CHECK(!fm_path_parent(p));
  TEST_CHECK(fm_path_is_inside("/x/y", "/x"));
  TEST_CHECK(!fm_path_is_inside("/xy", "/x"));
#endif
  TEST_CHECK(fm_path_is_safe_relative("a/b/c.txt"));
  TEST_CHECK(!fm_path_is_safe_relative("../evil"));
  TEST_CHECK(!fm_path_is_safe_relative("a/../../evil"));
  TEST_CHECK(!fm_path_is_safe_relative("/etc/passwd"));
  TEST_CHECK(!fm_path_is_safe_relative("C:\\x"));

  u32 cp;
  TEST_CHECK(utf8_decode("\xE2\x82\xAC", &cp) == 3 && cp == 0x20AC);
  TEST_CHECK(utf8_decode("\xF0\x9F\x98\x80", &cp) == 4 && cp == 0x1F600);
  TEST_CHECK(utf8_decode("\xC0\xAF", &cp) == 1 && cp == 0xFFFD);   /* overlong */
  TEST_CHECK(utf8_len("h\xC3\xA9llo") == 5);

  char b[32];
  TEST_CHECK(strcmp(fm_fmt_size(1536, b, sizeof b), "1.50 KB") == 0);

  FmArena a;
  arena_init(&a, 128);
  for (int i = 0; i < 100; i++) {
    char *s = arena_strdup(&a, "hello arena");
    TEST_CHECK(strcmp(s, "hello arena") == 0);
  }
  arena_free(&a);
  return g_test_fail - before;
}

typedef struct TestArea {
  const char *name;
  int (*fn)(const char *tmp);
} TestArea;

static const TestArea kAreas[] = {
  { "core", test_core },
  { "crypto", test_crypto },
  { "archives", test_archives },
  { "7z-rar", test_archives2 },
  { "fs", test_fs },
  { "media", test_media },
};

int test_run_all(void) {
  char base[FM_PATH_MAX], dir[FM_PATH_MAX];
  if (!plat_place(PLACE_TEMP, base, sizeof base)) fm_strlcpy(base, ".", sizeof base);
  char sub[64];
  fm_snprintf(sub, sizeof sub, "mmcfm-test-%llu", (unsigned long long)plat_now_ms());
  fm_path_join(base, sizeof base, base, sub);
  plat_mkdirs(base);
  printf("mmcfm %s self test in %s\n", FM_VERSION, base);
  int total = 0;
  for (int i = 0; i < FM_COUNT(kAreas); i++) {
    fm_path_join(dir, sizeof dir, base, kAreas[i].name);
    plat_mkdirs(dir);
    u64 t0 = plat_now_ms();
    g_test_fail = 0;
    int f = kAreas[i].fn(dir);
    if (f < g_test_fail) f = g_test_fail;
    printf("%s %-9s (%llu ms)\n", f ? "FAIL" : "PASS", kAreas[i].name,
           (unsigned long long)(plat_now_ms() - t0));
    total += f;
  }
  const char *keep = getenv("MMCFM_KEEP_TEST");
  if (!keep || strcmp(keep, "1") != 0) rm_tree(base);
  printf(total ? "%d failure(s)\n" : "all tests passed\n", total);
  return total ? 1 : 0;
}
