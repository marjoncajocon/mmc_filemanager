/* ftest_lib.c -- media library: scanning, favorites, recents, persistence.
**
** Builds a small tree of dummy media in the scratch folder (with a hidden
** folder, a .nomedia folder and a tiny .ts that is TypeScript), scans it
** synchronously and checks the index, the tag pass, favorites and recents
** surviving a reload, pruning of deleted files, and that nothing leaks.
*/
#include "ftest.h"
#include "fplat.h"
#include "flib.h"

static void put_file(const char *dir, const char *rel, const void *data, size_t n) {
  char p[FM_PATH_MAX], d[FM_PATH_MAX];
  fm_path_join(p, sizeof p, dir, rel);
  fm_strlcpy(d, p, sizeof d);
  if (fm_path_parent(d)) plat_mkdirs(d);
  FILE *f = fm_fopen(p, "wb");
  if (!f) return;
  if (n) fwrite(data, 1, n, f);
  fclose(f);
}

/* ID3v2.3 text frame: id, BE size, flags, ISO-8859-1 marker, text. */
static size_t id3_frame(u8 *o, const char *id, const char *text) {
  size_t n = strlen(text) + 1;
  memcpy(o, id, 4);
  o[4] = (u8)(n >> 24); o[5] = (u8)(n >> 16); o[6] = (u8)(n >> 8); o[7] = (u8)n;
  o[8] = o[9] = 0;
  o[10] = 0;
  memcpy(o + 11, text, n - 1);
  return 10 + n;
}

static size_t make_tagged_mp3(u8 *buf) {
  size_t n = 10;
  n += id3_frame(buf + n, "TIT2", "Song Alpha");
  n += id3_frame(buf + n, "TPE1", "Test Artist");
  n += id3_frame(buf + n, "TALB", "Test Album");
  size_t body = n - 10;
  memcpy(buf, "ID3\x03\x00\x00", 6);
  buf[6] = (u8)((body >> 21) & 0x7F); buf[7] = (u8)((body >> 14) & 0x7F);
  buf[8] = (u8)((body >> 7) & 0x7F); buf[9] = (u8)(body & 0x7F);
  return n;
}

static bool in_list(int (*count)(void), const char *(*at)(int), const char *p) {
  for (int i = 0; i < count(); i++)
    if (strcmp(at(i), p) == 0) return true;
  return false;
}

int test_library(const char *tmp) {
  size_t mem0 = fm_mem_in_use();
  char media[FM_PATH_MAX], state[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX], v[FM_PATH_MAX];
  fm_path_join(media, sizeof media, tmp, "media");
  fm_path_join(state, sizeof state, tmp, "state");
  plat_mkdirs(media);
  plat_mkdirs(state);
  u8 tag[256];
  size_t tn = make_tagged_mp3(tag);
  put_file(media, "a.mp3", tag, tn);
  put_file(media, "b.mp3", "x", 1);
  put_file(media, "clip.mp4", "x", 1);
  put_file(media, "notes.txt", "x", 1);
  put_file(media, "code.ts", "x", 1);
  put_file(media, "sub" FM_SEP_STR "deep" FM_SEP_STR "d.flac", "x", 1);
  put_file(media, ".hidden" FM_SEP_STR "h.mp3", "x", 1);
  put_file(media, "skip" FM_SEP_STR ".nomedia", NULL, 0);
  put_file(media, "skip" FM_SEP_STR "s.mp3", "x", 1);
  put_file(media, "skip" FM_SEP_STR "inner" FM_SEP_STR "s2.mp3", "x", 1);
  fm_path_join(a, sizeof a, media, "a.mp3");
  fm_path_join(b, sizeof b, media, "b.mp3");
  fm_path_join(v, sizeof v, media, "clip.mp4");

  /* scan */
  lib_test_reset(state, false);
  TEST_CHECK(lib_folder_count() == 0);
  lib_folder_set(media, true);
  TEST_CHECK(lib_has_folder(media));
  TEST_CHECK(lib_folder_count() == 1);
  lib_rescan();
  lib_scan_wait();
  TEST_CHECK(!lib_scanning());
  const FmLibData *d = lib_data();
  TEST_CHECK(d->nsongs == 3);          /* a, b, sub/deep/d: not hidden, not .nomedia */
  TEST_CHECK(d->nvideos == 1);         /* clip.mp4; the 1-byte code.ts is not a video */
  TEST_CHECK(d->n == 4);
  int ia = lib_find(a);
  TEST_CHECK(ia >= 0);
  TEST_CHECK(lib_find(v) >= 0);
  if (ia >= 0) {
    char t[256];
    lib_item_title(&d->items[ia], t, sizeof t);
    TEST_CHECK(strcmp(t, "Song Alpha") == 0);
    TEST_CHECK(strcmp(lib_str(d->items[ia].artist), "Test Artist") == 0);
  }
  int ib = lib_find(b);
  if (ib >= 0) {
    char t[256];
    lib_item_title(&d->items[ib], t, sizeof t);
    TEST_CHECK(strcmp(t, "b") == 0);   /* no tags: the name without extension */
  }
  TEST_CHECK(d->nartists == 2);        /* Test Artist + unknown */
  TEST_CHECK(d->nalbums == 2);

  /* favorites and recents */
  TEST_CHECK(!lib_is_fav(a));
  lib_fav_toggle(a);
  lib_fav_toggle(v);
  lib_fav_toggle(b);
  TEST_CHECK(lib_is_fav(a) && lib_is_fav(v) && lib_is_fav(b));
  lib_fav_toggle(b);
  TEST_CHECK(!lib_is_fav(b));
  TEST_CHECK(lib_fav_count() == 2);
  TEST_CHECK(strcmp(lib_fav_at(0), v) == 0);    /* newest first */
  lib_note_played(a);
  lib_note_played(v);
  lib_note_played(a);
  TEST_CHECK(lib_recent_count() == 2);
  TEST_CHECK(strcmp(lib_recent_at(0), a) == 0);
  lib_fav_set("/no-such-dir-mmcfm/x.mp3", true);  /* folder gone (unplugged): kept */
  lib_pump();                                     /* saves library.txt */

  /* reload from disk */
  lib_test_reset(state, false);
  TEST_CHECK(lib_is_fav(a) && lib_is_fav(v) && !lib_is_fav(b));
  TEST_CHECK(lib_fav_count() == 3);
  TEST_CHECK(lib_recent_count() == 2 && strcmp(lib_recent_at(0), a) == 0);
  TEST_CHECK(lib_has_folder(media));

  /* second scan reuses the tag cache file */
  lib_rescan();
  lib_scan_wait();
  d = lib_data();
  TEST_CHECK(d->n == 4);
  ia = lib_find(a);
  TEST_CHECK(ia >= 0 && strcmp(lib_str(d->items[ia].album), "Test Album") == 0);

  /* a deleted file leaves favorites and recents */
  TEST_CHECK(plat_remove_file(v) == FM_OK);
  TEST_CHECK(lib_prune_missing() == 2);
  TEST_CHECK(!lib_is_fav(v) && lib_is_fav(a));
  TEST_CHECK(!in_list(lib_recent_count, lib_recent_at, v));
  TEST_CHECK(lib_is_fav("/no-such-dir-mmcfm/x.mp3"));
  lib_recent_clear();
  TEST_CHECK(lib_recent_count() == 0);

  /* removing the folder empties the index */
  lib_folder_set(media, false);
  TEST_CHECK(!lib_has_folder(media));
  lib_rescan();
  lib_scan_wait();
  TEST_CHECK(lib_data()->n == 0);

  lib_shutdown();
  TEST_CHECK(fm_mem_in_use() == mem0);
  return 0;
}
