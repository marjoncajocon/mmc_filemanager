/* ftest_fs.c -- self test of the job engine (copy, move, delete) and fvfs.
**
** Design decisions:
**   - Runs the real job code through ops_run_sync on real files in the
**     scratch folder, so what is tested is exactly what the UI runs, minus
**     the thread.
**   - Contents are compared byte by byte; one file is larger than the copy
**     buffer so chunking is exercised. Mtimes are set to a fixed past time
**     and must survive copy and move.
**   - The archive part only runs when the archive module can create zips,
**     so the test passes against the placeholder archive code too.
*/
#include "ftest.h"
#include "fops.h"
#include "fvfs.h"
#include "fconf.h"
#include "fsdl.h"

#define T_MTIME 1600000000

/* ---- helpers ------------------------------------------------------------ */

static bool write_file(const char *path, u32 seed, size_t size) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  u32 x = seed * 2654435761u + 1;
  u8 buf[4096];
  size_t left = size;
  while (left > 0) {
    size_t n = left < sizeof buf ? left : sizeof buf;
    for (size_t i = 0; i < n; i++) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      buf[i] = (u8)x;
    }
    fwrite(buf, 1, n, f);
    left -= n;
  }
  fclose(f);
  return plat_set_mtime(path, T_MTIME) == FM_OK;
}

static bool same_content(const char *a, const char *b) {
  FILE *fa = fm_fopen(a, "rb"), *fb = fm_fopen(b, "rb");
  bool same = fa && fb;
  u8 ba[4096], bb[4096];
  while (same) {
    size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
    if (na != nb || memcmp(ba, bb, na) != 0) same = false;
    if (na == 0) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  return same;
}

static void join(char *out, const char *a, const char *b) {
  fm_path_join(out, FM_PATH_MAX, a, b);
}

static int count_entries(const char *dir) {
  FmDir *d = plat_dir_open(dir, NULL);
  if (!d) return -1;
  const char *name;
  int n = 0;
  while (plat_dir_next(d, &name, NULL)) n++;
  plat_dir_close(d);
  return n;
}

static FmErr run(FmJobKind kind, const char *const *srcs, int n, const char *dst, FmConflict c,
                 FmJobInfo *info) {
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = kind;
  s.srcs = srcs;
  s.nsrc = n;
  s.dst = dst;
  s.conflict = c;
  return ops_run_sync(&s, info);
}

static bool file_ok(const char *a, const char *b) {
  FmStat sa, sb;
  return plat_stat(a, &sa) && plat_stat(b, &sb) && sa.size == sb.size && sb.mtime == T_MTIME &&
         same_content(a, b);
}

/* ---- tests -------------------------------------------------------------- */

int test_fs(const char *tmp) {
  int before = g_test_fail;
  char src[FM_PATH_MAX], sub[FM_PATH_MAX], deep[FM_PATH_MAX], dst[FM_PATH_MAX];
  char a[FM_PATH_MAX], b[FM_PATH_MAX], c[FM_PATH_MAX], hid[FM_PATH_MAX], p[FM_PATH_MAX];
  FmJobInfo info;

  /* src/a.txt, src/.hidden, src/sub/big.bin (> 256 KB), src/sub/deep/c.txt */
  join(src, tmp, "src");
  join(sub, src, "sub");
  join(deep, sub, "deep");
  TEST_CHECK(plat_mkdirs(deep) == FM_OK);
  join(a, src, "a.txt");
  join(hid, src, ".hidden");
  join(b, sub, "big.bin");
  join(c, deep, "c.txt");
  TEST_CHECK(write_file(a, 1, 1000));
  TEST_CHECK(write_file(hid, 2, 10));
  TEST_CHECK(write_file(b, 3, 700 * 1024 + 17));
  TEST_CHECK(write_file(c, 4, 0));
  plat_set_mtime(deep, T_MTIME);
  plat_set_mtime(sub, T_MTIME);

  /* vfs: counts and flags (sorting is the panel's job) */
  {
    FmListing l;
    FmLoc loc;
    memset(&l, 0, sizeof l);
    loc_local(&loc, src);
    TEST_CHECK(vfs_list(&l, &loc, false) == FM_OK);
    TEST_CHECK(l.count == 2);
    int dirs = 0, files = 0;
    for (int i = 0; i < l.count; i++) {
      if (l.items[i].flags & FM_ST_DIR) { dirs++; TEST_CHECK(l.items[i].type == FT_DIR); }
      else { files++; TEST_CHECK(l.items[i].size == 1000 && l.items[i].type == FT_TEXT); }
      TEST_CHECK(l.items[i].arc_index == -1);
    }
    TEST_CHECK(dirs == 1 && files == 1);
    TEST_CHECK(l.total_size == 1000);
    TEST_CHECK(vfs_list(&l, &loc, true) == FM_OK);
    TEST_CHECK(l.count == 3);
    int hidden = 0;
    for (int i = 0; i < l.count; i++) if (l.items[i].flags & FM_ST_HIDDEN) hidden++;
    TEST_CHECK(hidden == 1);
    TEST_CHECK(vfs_entry_path(&l, &l.items[0], p, sizeof p) && plat_exists(p));
    char t[256];
    loc_title(&loc, t, sizeof t);
    TEST_CHECK(strcmp(t, "src") == 0);
    FmLoc up = loc;
    TEST_CHECK(loc_up(&up) && !loc_equal(&up, &loc));
    loc_local(&loc, deep);
    TEST_CHECK(loc_up(&loc) && loc_up(&loc));
    loc_local(&up, src);
    TEST_CHECK(loc_equal(&loc, &up));
    join(p, tmp, "missing");
    loc_local(&loc, p);
    TEST_CHECK(vfs_list(&l, &loc, false) == FM_ERR_NOT_FOUND && l.count == 0);
    vfs_free(&l);
  }

  /* copy a tree */
  join(dst, tmp, "dst");
  TEST_CHECK(plat_mkdir(dst) == FM_OK);
  {
    const char *s1[] = { src };
    TEST_CHECK(run(JOB_COPY, s1, 1, dst, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(info.nerrors == 0 && info.files_done == 4 && info.files_total == 4);
    TEST_CHECK(info.bytes_done == 1000 + 10 + 700 * 1024 + 17);
    join(p, dst, "src/a.txt");
    TEST_CHECK(file_ok(a, p));
    join(p, dst, "src/sub/big.bin");
    TEST_CHECK(file_ok(b, p));
    join(p, dst, "src/sub/deep/c.txt");
    TEST_CHECK(file_ok(c, p));
    join(p, dst, "src/.hidden");
    TEST_CHECK(file_ok(hid, p));
    FmStat st;
    join(p, dst, "src/sub");
    TEST_CHECK(plat_stat(p, &st) && (st.flags & FM_ST_DIR) && st.mtime == T_MTIME);
    join(p, dst, "src");
    TEST_CHECK(count_entries(p) == 3);
    TEST_CHECK(strstr(info.touched[0], "dst") != NULL);
  }

  /* conflicts: skip, keep both, overwrite */
  {
    const char *s1[] = { a };
    char d2[FM_PATH_MAX];
    join(d2, dst, "src");
    TEST_CHECK(run(JOB_COPY, s1, 1, d2, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(info.nskipped == 1 && count_entries(d2) == 3);
    TEST_CHECK(run(JOB_COPY, s1, 1, d2, CONFLICT_KEEP_BOTH, &info) == FM_OK);
    join(p, d2, "a (2).txt");
    TEST_CHECK(file_ok(a, p));
    TEST_CHECK(run(JOB_COPY, s1, 1, d2, CONFLICT_KEEP_BOTH, &info) == FM_OK);
    join(p, d2, "a (3).txt");
    TEST_CHECK(plat_exists(p));
    TEST_CHECK(ops_unique_name(d2, "a (2).txt", p, sizeof p) &&
               strcmp(fm_path_base(p), "a (4).txt") == 0);
    /* overwrite replaces the content and leaves no part file behind */
    join(p, d2, "a.txt");
    TEST_CHECK(write_file(p, 9, 5));
    TEST_CHECK(run(JOB_COPY, s1, 1, d2, CONFLICT_OVERWRITE, &info) == FM_OK);
    TEST_CHECK(file_ok(a, p));
    TEST_CHECK(count_entries(d2) == 5);
    /* a folder copied onto itself becomes "src (2)" */
    const char *s2[] = { d2 };
    TEST_CHECK(run(JOB_COPY, s2, 1, dst, CONFLICT_SKIP, &info) == FM_OK);
    join(p, dst, "src (2)");
    TEST_CHECK(plat_is_dir(p) && count_entries(p) == 5);
  }

  /* refuse to copy or move a folder into itself */
  {
    const char *s1[] = { src };
    TEST_CHECK(run(JOB_COPY, s1, 1, sub, CONFLICT_SKIP, &info) == FM_ERR_UNSUPPORTED);
    TEST_CHECK(info.nerrors == 1 && count_entries(sub) == 2);
    TEST_CHECK(run(JOB_MOVE, s1, 1, deep, CONFLICT_SKIP, &info) == FM_ERR_UNSUPPORTED);
    TEST_CHECK(plat_is_dir(src) && count_entries(deep) == 1);
  }

  /* move (same volume: rename), then merge into an existing folder */
  {
    char moved[FM_PATH_MAX], d2[FM_PATH_MAX];
    join(moved, tmp, "moved");
    TEST_CHECK(plat_mkdir(moved) == FM_OK);
    join(d2, dst, "src");
    const char *s1[] = { d2 };
    TEST_CHECK(run(JOB_MOVE, s1, 1, moved, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(!plat_exists(d2));
    join(p, moved, "src/sub/big.bin");
    TEST_CHECK(file_ok(b, p));
    TEST_CHECK(info.touched[1][0] != 0);
    /* move "src (2)" renamed to an existing "src": merge, skip the clashes */
    char again[FM_PATH_MAX], src2[FM_PATH_MAX];
    join(src2, dst, "src (2)");
    join(again, dst, "src");
    TEST_CHECK(plat_rename(src2, again) == FM_OK);
    const char *s2[] = { again };
    TEST_CHECK(run(JOB_MOVE, s2, 1, moved, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(info.nskipped >= 1);
    TEST_CHECK(plat_exists(again));          /* skipped files keep the source */
    TEST_CHECK(run(JOB_MOVE, s2, 1, moved, CONFLICT_OVERWRITE, &info) == FM_OK);
    TEST_CHECK(!plat_exists(again));
    join(p, moved, "src");
    TEST_CHECK(count_entries(p) == 5);
  }

  /* delete */
  {
    char moved[FM_PATH_MAX];
    join(moved, tmp, "moved");
    const char *s1[] = { moved };
    TEST_CHECK(run(JOB_DELETE, s1, 1, NULL, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(!plat_exists(moved) && info.nerrors == 0);
    TEST_CHECK(count_entries(dst) == 0);
  }

  /* a threaded job hands its conflict to the "UI" and waits for the answer */
  {
    char d3[FM_PATH_MAX];
    join(d3, tmp, "threaded");
    TEST_CHECK(plat_mkdir(d3) == FM_OK);
    join(p, d3, "a.txt");
    TEST_CHECK(write_file(p, 7, 3));
    const char *s1[] = { a };
    FmJobSpec s;
    memset(&s, 0, sizeof s);
    s.kind = JOB_COPY;
    s.srcs = s1;
    s.nsrc = 1;
    s.dst = d3;
    s.conflict = CONFLICT_ASK;
    FmJob *j = ops_start(&s);
    TEST_CHECK(j != NULL);
    bool asked = false;
    FmJob *done = NULL;
    u64 t0 = plat_now_ms();
    while (j && !done && plat_now_ms() - t0 < 10000) {
      FmAsk q;
      FmJob *w = ops_question(&q);
      if (w) {
        asked = w == j && q.kind == ASK_CONFLICT && !strcmp(fm_path_base(q.dst), "a.txt") &&
                q.src_known && q.src_st.size == 1000;
        ops_answer_conflict(w, CONFLICT_OVERWRITE, false);
      }
      done = ops_take_finished();
      SDL_Delay(2);
    }
    TEST_CHECK(asked && done == j);
    if (done) {
      FmJobInfo in;
      ops_info(done, &in);
      TEST_CHECK(in.err == FM_OK && in.done && in.nerrors == 0);
      ops_free(done);
    }
    TEST_CHECK(file_ok(a, p) && count_entries(d3) == 1);
  }

  /* folder size */
  {
    const char *s1[] = { src };
    TEST_CHECK(run(JOB_SIZE, s1, 1, NULL, CONFLICT_SKIP, &info) == FM_OK);
    TEST_CHECK(info.files_done == 4 && info.dirs == 2);
    TEST_CHECK(info.bytes_done == 1000 + 10 + 700 * 1024 + 17);
  }

  /* names */
  TEST_CHECK(ops_valid_name("report.txt"));
  TEST_CHECK(!ops_valid_name("a/b") && !ops_valid_name("..") && !ops_valid_name(""));

  /* archives: compress, list inside, extract (only with the real archive code) */
  if (arc_fmt_can_create(ARC_ZIP)) {
    char zip[FM_PATH_MAX], out[FM_PATH_MAX];
    join(zip, tmp, "t.zip");
    FmJobSpec s;
    memset(&s, 0, sizeof s);
    const char *s1[] = { src };
    s.kind = JOB_COMPRESS;
    s.srcs = s1;
    s.nsrc = 1;
    s.dst = zip;
    s.base_dir = tmp;
    s.arc_opts.fmt = ARC_ZIP;
    s.arc_opts.level = 6;
    TEST_CHECK(ops_run_sync(&s, &info) == FM_OK && plat_exists(zip));

    FmListing l;
    FmLoc loc;
    memset(&l, 0, sizeof l);
    memset(&loc, 0, sizeof loc);
    fm_strlcpy(loc.path, zip, sizeof loc.path);
    loc.in_arc = true;
    TEST_CHECK(vfs_list(&l, &loc, true) == FM_OK);
    TEST_CHECK(l.count == 1 && (l.items[0].flags & FM_ST_DIR));
    fm_strlcpy(loc.inner, "src/sub/", sizeof loc.inner);
    TEST_CHECK(vfs_list(&l, &loc, true) == FM_OK && l.arc != NULL);
    TEST_CHECK(l.count == 2);
    int bin = -1;
    for (int i = 0; i < l.count; i++) if (!strcmp(l.items[i].name, "big.bin")) bin = i;
    TEST_CHECK(bin >= 0 && l.items[bin].size == 700 * 1024 + 17 && l.items[bin].arc_index >= 0);
    if (bin >= 0) {
      TEST_CHECK(vfs_materialize(&l, &l.items[bin], p, sizeof p) == FM_OK && file_ok(b, p));
    }
    FmLoc up = loc;
    TEST_CHECK(loc_up(&up) && strcmp(up.inner, "src/") == 0);
    TEST_CHECK(loc_up(&up) && up.in_arc && up.inner[0] == 0);
    TEST_CHECK(loc_up(&up) && !up.in_arc);
    vfs_free(&l);

    /* extract "sub" from inside the archive (strip prefix "src/") */
    join(out, tmp, "out");
    plat_mkdir(out);
    memset(&s, 0, sizeof s);
    const char *names[] = { "sub" };
    s.kind = JOB_COPY;
    s.srcs = names;
    s.nsrc = 1;
    s.dst = out;
    s.arc = zip;
    s.arc_inner = "src/";
    s.conflict = CONFLICT_SKIP;
    TEST_CHECK(ops_run_sync(&s, &info) == FM_OK);
    join(p, out, "sub/big.bin");
    TEST_CHECK(file_ok(b, p));
    TEST_CHECK(count_entries(out) == 1);        /* no staging folder left */
    /* extracting again merges the folder; "keep both" renames the files */
    s.conflict = CONFLICT_KEEP_BOTH;
    TEST_CHECK(ops_run_sync(&s, &info) == FM_OK);
    join(p, out, "sub/big (2).bin");
    TEST_CHECK(file_ok(b, p) && count_entries(out) == 1);
  }

  return g_test_fail - before;
}
