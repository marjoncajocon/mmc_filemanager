/* ftest_cloud.c -- the cloud storage self test (area "cloud"), offline.
**
** The in-memory service (fcloud_mock.c) stands in for the network, and the
** tasks run inline (cloud_set_sync), so listing, the location chains, the
** transfer jobs and the account file are tested through the same code the
** panels use.
*/
#include "ftest.h"
#include "fcloud_app.h"

static bool write_file(const char *path, const char *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}

static size_t read_file(const char *path, char *buf, size_t cap) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return n;
}

static bool file_has(const char *path, const char *needle) {
  static char buf[65536];
  read_file(path, buf, sizeof buf);
  return strstr(buf, needle) != NULL;
}

/* ---- accounts ------------------------------------------------------------- */

static void fill_acct(FmCloudAcct *a, const char *prov, const char *label) {
  memset(a, 0, sizeof *a);
  fm_strlcpy(a->provider, prov, sizeof a->provider);
  fm_strlcpy(a->label, label, sizeof a->label);
  fm_strlcpy(a->user, "ann@example.com", sizeof a->user);
  fm_strlcpy(a->secret, "pa\\ss=wo\nrd \xC3\xA9", sizeof a->secret);
  fm_strlcpy(a->server, "https://dav.example.com/remote.php|eu|bucket", sizeof a->server);
  fm_strlcpy(a->client_id, "1234.apps.example", sizeof a->client_id);
  fm_strlcpy(a->client_secret, "CLIENT-SECRET-XYZ", sizeof a->client_secret);
  /* a full session (OAuth access + refresh tokens can pass 4 KB) */
  for (size_t i = 0; i + 1 < sizeof a->session; i++) a->session[i] = (char)('a' + i % 26);
  a->session[sizeof a->session - 1] = 0;
}

static bool same_acct(const FmCloudAcct *a, const FmCloudAcct *b) {
  return !strcmp(a->provider, b->provider) && !strcmp(a->label, b->label) && !strcmp(a->user, b->user) &&
         !strcmp(a->secret, b->secret) && !strcmp(a->server, b->server) && !strcmp(a->client_id, b->client_id) &&
         !strcmp(a->client_secret, b->client_secret) && !strcmp(a->session, b->session);
}

static void test_accounts(const char *tmp) {
  char store[FM_PATH_MAX];
  fm_path_join(store, sizeof store, tmp, "cloud.txt");
  for (int pass = 0; pass < 2; pass++) {
    cloud_secret_force_obfuscation(pass == 1);
    plat_remove_file(store);
    cloud_acct_set_store(store);
    TEST_CHECK(cloud_acct_count() == 0);
    FmCloudAcct a, b, t;
    fill_acct(&a, "webdav", "Work files");
    fill_acct(&b, "gdrive", "");
    b.secret[0] = 0;
    fill_acct(&t, "mega", "A public link");
    TEST_CHECK(cloud_acct_add(&a) == 0);
    TEST_CHECK(cloud_acct_add(&b) == 1);
    TEST_CHECK(cloud_acct_add_temp(&t) == 2);
    TEST_CHECK(!strcmp(cloud_acct_at(1)->label, "ann@example.com"));   /* default label */
    fm_strlcpy(b.label, "ann@example.com", sizeof b.label);
    int s0 = cloud_acct_serial(0), s1 = cloud_acct_serial(1);
    TEST_CHECK(s0 > 0 && s1 > 0 && s0 != s1);
    TEST_CHECK(cloud_acct_index(s1) == 1);
    cloud_acct_save();
    /* nothing secret in clear text */
    TEST_CHECK(!file_has(store, "CLIENT-SECRET-XYZ"));
    TEST_CHECK(!file_has(store, "abcdefghijklmnop"));
    TEST_CHECK(!file_has(store, "public link"));                       /* temporary: not saved */
    TEST_CHECK(file_has(store, "Work files"));
#ifdef FM_WIN
    TEST_CHECK(file_has(store, pass == 0 ? "secret=d:" : "secret=o:"));
#else
    TEST_CHECK(file_has(store, "secret=o:"));
#endif
    cloud_acct_set_store(store);                                       /* reload from disk */
    TEST_CHECK(cloud_acct_count() == 2);
    if (cloud_acct_count() == 2) {
      TEST_CHECK(same_acct(cloud_acct_at(0), &a));
      TEST_CHECK(same_acct(cloud_acct_at(1), &b));
    }
    /* serials are new after a reload; removing keeps the others */
    int keep = cloud_acct_serial(1);
    cloud_acct_remove(0);
    TEST_CHECK(cloud_acct_count() == 1 && cloud_acct_serial(0) == keep);
    TEST_CHECK(cloud_acct_index(keep) == 0);
    cloud_acct_set_store(store);
    TEST_CHECK(cloud_acct_count() == 1 && !strcmp(cloud_acct_at(0)->provider, "gdrive"));
    /* a session handed back by a worker is saved by the pump */
    FmCloudAcct snap;
    int sr = cloud_acct_serial(0);
    TEST_CHECK(cloud_acct_snapshot(sr, &snap));
    fm_strlcpy(snap.session, "refreshed-token", sizeof snap.session);
    snap.session_changed = true;
    cloud_acct_writeback(sr, &snap, false);
    cloud_acct_pump();
    cloud_acct_set_store(store);
    TEST_CHECK(cloud_acct_count() == 1 && !strcmp(cloud_acct_at(0)->session, "refreshed-token"));
  }
  cloud_secret_force_obfuscation(false);
  /* protect / unprotect */
  char enc[256], dec[256];
  cloud_protect("hunter2", enc, sizeof enc);
  TEST_CHECK(enc[0] && !strstr(enc, "hunter2"));
  TEST_CHECK(cloud_unprotect(enc, dec, sizeof dec) && !strcmp(dec, "hunter2"));
  cloud_secret_force_obfuscation(true);
  char enc2[256];
  cloud_protect("hunter2", enc2, sizeof enc2);
  TEST_CHECK(!strncmp(enc2, "o:", 2) && strcmp(enc, enc2) != 0);
  TEST_CHECK(cloud_unprotect(enc2, dec, sizeof dec) && !strcmp(dec, "hunter2"));
  cloud_secret_force_obfuscation(false);
  TEST_CHECK(!cloud_unprotect("x:abc", dec, sizeof dec));
  TEST_CHECK(!cloud_unprotect("d:!!!!", dec, sizeof dec));
  TEST_CHECK(cloud_unprotect("", dec, sizeof dec) && dec[0] == 0);
  cloud_acct_set_store(NULL);
}

/* ---- helpers for adapters ------------------------------------------------- */

static void test_times(void) {
  TEST_CHECK(cloud_parse_time("1970-01-01T00:00:00Z") == 0);
  TEST_CHECK(cloud_parse_time("2026-10-09T12:34:56Z") == 1791549296);
  TEST_CHECK(cloud_parse_time("2026-10-09T12:34:56.789Z") == 1791549296);
  TEST_CHECK(cloud_parse_time("2026-10-09T14:34:56+02:00") == 1791549296);
  TEST_CHECK(cloud_parse_time("2026-10-09T07:34:56-0500") == 1791549296);
  TEST_CHECK(cloud_parse_time("2026-10-09 12:34:56") == 1791549296);
  TEST_CHECK(cloud_parse_time("2026-10-09") == 1791504000);
  TEST_CHECK(cloud_parse_time("2000-02-29T00:00:00Z") == 951782400);
  TEST_CHECK(cloud_parse_time("Fri, 09 Oct 2026 12:34:56 GMT") == 1791549296);
  TEST_CHECK(cloud_parse_time("9 Oct 2026 12:34:56 GMT") == 1791549296);
  TEST_CHECK(cloud_parse_time("Friday, 09-Oct-26 12:34:56 GMT") == 1791549296);
  TEST_CHECK(cloud_parse_time("Fri, 09 Oct 2026 14:34:56 +0200") == 1791549296);
  TEST_CHECK(cloud_parse_time("") == 0);
  TEST_CHECK(cloud_parse_time(NULL) == 0);
  TEST_CHECK(cloud_parse_time("yesterday") == 0);
  TEST_CHECK(cloud_parse_time("2026-13-01T00:00:00Z") == 0);
  TEST_CHECK(cloud_parse_time("2026-10-09T12:34:56Q") == 0);
}

static void test_http_errors(void) {
  FmNetResp r;
  char err[200];
  memset(&r, 0, sizeof r);
  r.status = 401;
  TEST_CHECK(cloud_http_error("Google Drive", &r, FM_OK, err, sizeof err) == FM_ERR_PASSWORD);
  TEST_CHECK(strstr(err, "Signed out") != NULL);
  r.status = 403;
  TEST_CHECK(cloud_http_error("Google Drive", &r, FM_OK, err, sizeof err) == FM_ERR_ACCESS);
  TEST_CHECK(!strcmp(err, "Google Drive refused the request (HTTP 403)"));
  r.status = 404;
  TEST_CHECK(cloud_http_error("MEGA", &r, FM_OK, err, sizeof err) == FM_ERR_NOT_FOUND);
  r.status = 409;
  TEST_CHECK(cloud_http_error("MEGA", &r, FM_OK, err, sizeof err) == FM_ERR_EXISTS);
  r.status = 507;
  TEST_CHECK(cloud_http_error("WebDAV", &r, FM_OK, err, sizeof err) == FM_ERR_FULL);
  r.status = 429;
  TEST_CHECK(cloud_http_error("S3", &r, FM_OK, err, sizeof err) == FM_ERR_IO && strstr(err, "429"));
  r.status = 503;
  TEST_CHECK(cloud_http_error("S3", &r, FM_OK, err, sizeof err) == FM_ERR_IO && strstr(err, "503"));
  r.status = 200;
  TEST_CHECK(cloud_http_error("S3", &r, FM_OK, err, sizeof err) == FM_OK && err[0] == 0);
  r.status = 0;
  fm_strlcpy(r.error, "The server name cannot be resolved", sizeof r.error);
  TEST_CHECK(cloud_http_error("S3", &r, FM_ERR_IO, err, sizeof err) == FM_ERR_IO);
  TEST_CHECK(!strcmp(err, "Network error: The server name cannot be resolved"));
  TEST_CHECK(cloud_http_error("S3", NULL, FM_ERR_CANCEL, err, sizeof err) == FM_ERR_CANCEL);
  TEST_CHECK(cloud_http_error(NULL, NULL, FM_ERR_IO, err, sizeof err) == FM_ERR_IO && !strncmp(err, "Network", 7));
  /* list helpers */
  FmCloudList l;
  memset(&l, 0, sizeof l);
  for (int i = 0; i < 200; i++) {
    FmCloudEntry *e = cloud_list_add(&l);
    TEST_CHECK(e->id[0] == 0 && !e->dir);
    fm_snprintf(e->name, sizeof e->name, "f%d", i);
  }
  TEST_CHECK(l.count == 200 && !strcmp(l.items[199].name, "f199"));
  cloud_list_free(&l);
  TEST_CHECK(l.count == 0 && l.items == NULL);
  /* the registry: six services, a stub shows as unavailable, the mock only when on */
  cloud_mock_enable(false);
  TEST_CHECK(cloud_count() == 6);
  TEST_CHECK(cloud_find("gdrive") && cloud_find("s3") && cloud_find("mega") && !cloud_find("nope"));
  cloud_mock_enable(true);
  TEST_CHECK(cloud_count() == 7 && cloud_available(cloud_find("mock")));
}

/* ---- locations ---------------------------------------------------------------- */

static void test_locations(int serial) {
  FmLoc l, m;
  cloud_loc_root(&l, serial);
  TEST_CHECK(l.in_cloud && cloud_loc_depth(&l) == 0 && !loc_up(&l));
  char id[CLOUD_ID_MAX], name[256], buf[FM_PATH_MAX];
  cloud_loc_title(&l, buf, sizeof buf);
  TEST_CHECK(!strcmp(buf, "Test cloud"));
  /* path-style ids are stored short */
  TEST_CHECK(cloud_loc_child(&l, "photos/", "photos"));
  TEST_CHECK(cloud_loc_child(&l, "photos/2026/", "2026"));
  TEST_CHECK(cloud_loc_child(&l, "photos/2026/summer trip/", "summer trip"));
  TEST_CHECK(cloud_loc_depth(&l) == 3);
  TEST_CHECK(strlen(l.path) < 40);
  cloud_loc_id(&l, id, sizeof id);
  TEST_CHECK(!strcmp(id, "photos/2026/summer trip/"));
  cloud_loc_level(&l, 2, id, sizeof id, name, sizeof name);
  TEST_CHECK(!strcmp(id, "photos/2026/") && !strcmp(name, "2026"));
  cloud_loc_level(&l, 0, id, sizeof id, name, sizeof name);
  TEST_CHECK(id[0] == 0 && !strcmp(name, "Test cloud"));
  loc_display(&l, buf, sizeof buf);
  TEST_CHECK(!strcmp(buf, "Test cloud/photos/2026/summer trip"));
  loc_title(&l, buf, sizeof buf);
  TEST_CHECK(!strcmp(buf, "summer trip"));
  cloud_loc_key(&l, buf, sizeof buf);
  TEST_CHECK(cloud_key_serial(buf) == serial && strstr(buf, "summer trip/"));
  m = l;
  TEST_CHECK(loc_equal(&l, &m));
  TEST_CHECK(loc_up(&m));
  TEST_CHECK(!loc_equal(&l, &m));
  cloud_loc_id(&m, id, sizeof id);
  TEST_CHECK(!strcmp(id, "photos/2026/"));
  loc_title(&m, buf, sizeof buf);
  TEST_CHECK(!strcmp(buf, "2026"));
  cloud_loc_trim(&l, 1);
  cloud_loc_id(&l, id, sizeof id);
  TEST_CHECK(cloud_loc_depth(&l) == 1 && !strcmp(id, "photos/"));
  /* id-style ids, a name with a newline */
  cloud_loc_root(&l, serial);
  TEST_CHECK(cloud_loc_child(&l, "1AbcDEF", "Work\nstuff"));
  TEST_CHECK(cloud_loc_child(&l, "1Abc", "x"));          /* shorter than its parent: stored whole */
  cloud_loc_id(&l, id, sizeof id);
  TEST_CHECK(!strcmp(id, "1Abc"));
  cloud_loc_level(&l, 1, id, sizeof id, name, sizeof name);
  TEST_CHECK(!strcmp(id, "1AbcDEF") && !strcmp(name, "Work stuff"));
  /* too deep for the chain: refused, location unchanged */
  char big[400];
  memset(big, 'z', sizeof big - 1);
  big[sizeof big - 1] = 0;
  int ok = 0;
  for (int i = 0; i < 10; i++) ok += cloud_loc_child(&l, big, "deep") ? 1 : 0;
  TEST_CHECK(ok >= 1 && ok < 10);
  TEST_CHECK(cloud_loc_depth(&l) == 2 + ok);
  FmLoc o;
  loc_local(&o, "C:\\x");
  cloud_loc_root(&m, serial);
  TEST_CHECK(!loc_equal(&o, &m));
}

/* ---- listings and jobs on the mock ------------------------------------------- */

static int find_entry(const FmListing *l, const char *name) {
  for (int i = 0; i < l->count; i++)
    if (!strcmp(l->items[i].name, name)) return i;
  return -1;
}

static void relist(FmListing *l, const FmLoc *loc) {
  char key[CLOUD_ID_MAX + 32];
  cloud_loc_key(loc, key, sizeof key);
  cloud_vfs_invalidate(key);
  vfs_list(l, loc, true);
}

static void child_loc(const FmListing *l, const char *name, FmLoc *out) {
  *out = l->loc;
  int i = find_entry(l, name);
  if (i < 0) return;
  const FmCloudEntry *e = cloud_vfs_entry(l, &l->items[i]);
  if (e) cloud_loc_child(out, e->id, e->name);
}

static FmErr transfer(bool move, const char *const *paths, int np, const FmLoc *src, const FmListing *sl,
                      const char *const *names, int nn, const char *dst_dir, const FmLoc *dst, FmConflict pol,
                      FmJobInfo *in) {
  FmCloudEntry ents[8];
  for (int i = 0; i < nn; i++) {
    int k = find_entry(sl, names[i]);
    TEST_CHECK(k >= 0);
    if (k >= 0) ents[i] = *cloud_vfs_entry(sl, &sl->items[k]);
  }
  FmCloudJobSpec s;
  memset(&s, 0, sizeof s);
  s.op = CJ_TRANSFER;
  s.move = move;
  s.paths = paths;
  s.npaths = np;
  s.src_loc = src;
  s.entries = ents;
  s.nentries = nn;
  s.dst_dir = dst_dir;
  s.dst_loc = dst;
  s.conflict = pol;
  return cloud_job_run(&s, true, in);
}

static void test_mock(const char *tmp, int serial) {
  FmLoc root;
  cloud_loc_root(&root, serial);
  FmListing l;
  memset(&l, 0, sizeof l);
  TEST_CHECK(vfs_list(&l, &root, true) == FM_OK);
  TEST_CHECK(!l.loading && l.count == 0 && l.err == FM_OK);

  /* local tree to upload */
  char up[FM_PATH_MAX], sub[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX];
  fm_path_join(up, sizeof up, tmp, "up");
  fm_path_join(sub, sizeof sub, up, "sub");
  plat_mkdirs(sub);
  fm_path_join(a, sizeof a, up, "a.txt");
  fm_path_join(b, sizeof b, sub, "b.bin");
  TEST_CHECK(write_file(a, "hello cloud", 11));
  char *blob = (char *)fm_alloc(100000);
  for (int i = 0; i < 100000; i++) blob[i] = (char)(i * 7);
  TEST_CHECK(write_file(b, blob, 100000));

  /* upload a file and a folder */
  const char *srcs[] = { a, sub };
  FmJobInfo in;
  TEST_CHECK(transfer(false, srcs, 2, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_ASK, &in) == FM_OK);
  TEST_CHECK(in.files_done == 2 && in.files_total == 2 && in.bytes_total == 100011 && in.nerrors == 0);
  TEST_CHECK(in.kind == JOB_COPY && strstr(in.title, "Uploading 2 items") != NULL);
  TEST_CHECK(cloud_key_serial(in.touched[0]) == serial);
  relist(&l, &root);
  TEST_CHECK(l.count == 2 && find_entry(&l, "a.txt") >= 0 && find_entry(&l, "sub") >= 0);
  FmLoc subloc;
  child_loc(&l, "sub", &subloc);
  FmListing ls;
  memset(&ls, 0, sizeof ls);
  vfs_list(&ls, &subloc, true);
  TEST_CHECK(ls.count == 1 && ls.items[0].size == 100000);
  TEST_CHECK(cloud_loc_depth(&subloc) == 1);

  /* conflicts: ask = skip in sync mode, keep both, overwrite */
  const char *one[] = { a };
  TEST_CHECK(transfer(false, one, 1, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_ASK, &in) == FM_OK);
  TEST_CHECK(in.nskipped == 1 && in.files_done == 0);
  TEST_CHECK(transfer(false, one, 1, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_KEEP_BOTH, &in) == FM_OK);
  TEST_CHECK(in.files_done == 1);
  TEST_CHECK(transfer(false, one, 1, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_KEEP_BOTH, &in) == FM_OK);
  write_file(a, "hello again", 11);
  TEST_CHECK(transfer(false, one, 1, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_OVERWRITE, &in) == FM_OK);
  relist(&l, &root);
  TEST_CHECK(l.count == 4 && find_entry(&l, "a (2).txt") >= 0 && find_entry(&l, "a (3).txt") >= 0);
  /* merging into an existing cloud folder */
  TEST_CHECK(transfer(false, (const char *const[]){ sub }, 1, NULL, NULL, NULL, 0, NULL, &root, CONFLICT_OVERWRITE,
                      &in) == FM_OK);
  TEST_CHECK(in.files_done == 1 && in.nerrors == 0);
  relist(&l, &root);
  TEST_CHECK(l.count == 4);

  /* small operations as tasks: new folder, rename */
  FmCloudTask *t = cloud_task_new(CT_MKDIR, serial);
  TEST_CHECK(t != NULL);
  if (t) {
    fm_strlcpy(t->name, "New folder", sizeof t->name);
    cloud_task_start(t);
  }
  int ki = -1;
  relist(&l, &root);
  TEST_CHECK((ki = find_entry(&l, "New folder")) >= 0 && (l.items[ki].flags & FM_ST_DIR));
  t = cloud_task_new(CT_RENAME, serial);
  if (t && ki >= 0) {
    t->entry = *cloud_vfs_entry(&l, &l.items[ki]);
    fm_strlcpy(t->name, "Renamed", sizeof t->name);
    cloud_task_start(t);
  }
  relist(&l, &root);
  TEST_CHECK(find_entry(&l, "Renamed") >= 0 && find_entry(&l, "New folder") < 0);

  /* download a file and a folder */
  char down[FM_PATH_MAX], got[FM_PATH_MAX], buf[256];
  fm_path_join(down, sizeof down, tmp, "down");
  plat_mkdirs(down);
  const char *names[] = { "a.txt", "sub" };
  TEST_CHECK(transfer(false, NULL, 0, &root, &l, names, 2, down, NULL, CONFLICT_ASK, &in) == FM_OK);
  TEST_CHECK(in.files_done == 2 && in.nerrors == 0 && strstr(in.title, "Downloading 2 items"));
  fm_path_join(got, sizeof got, down, "a.txt");
  TEST_CHECK(read_file(got, buf, sizeof buf) == 11 && !strcmp(buf, "hello again"));
  fm_path_join(got, sizeof got, down, "sub");
  fm_path_join(got, sizeof got, got, "b.bin");
  {
    char *chk = (char *)fm_alloc(100001);
    TEST_CHECK(read_file(got, chk, 100001) == 100000 && memcmp(chk, blob, 100000) == 0);
    fm_free(chk);
  }
  /* again: the existing local files are skipped, then kept both */
  TEST_CHECK(transfer(false, NULL, 0, &root, &l, names, 1, down, NULL, CONFLICT_ASK, &in) == FM_OK);
  TEST_CHECK(in.nskipped == 1);
  TEST_CHECK(transfer(false, NULL, 0, &root, &l, names, 1, down, NULL, CONFLICT_KEEP_BOTH, &in) == FM_OK);
  fm_path_join(got, sizeof got, down, "a (2).txt");
  TEST_CHECK(plat_exists(got));
  /* no part files left behind */
  {
    FmDir *d = plat_dir_open(down, NULL);
    const char *n;
    int parts = 0;
    while (d && plat_dir_next(d, &n, NULL)) parts += strstr(n, "mmcfm") != NULL;
    if (d) plat_dir_close(d);
    TEST_CHECK(parts == 0);
  }

  /* cloud to cloud copy within the account (through a temporary file) */
  FmLoc ren;
  child_loc(&l, "Renamed", &ren);
  TEST_CHECK(transfer(false, NULL, 0, &root, &l, (const char *const[]){ "sub" }, 1, NULL, &ren, CONFLICT_ASK,
                      &in) == FM_OK);
  TEST_CHECK(in.files_done == 1 && in.bytes_total == 200000 && in.bytes_done == 200000);
  FmListing lr;
  memset(&lr, 0, sizeof lr);
  vfs_list(&lr, &ren, true);
  TEST_CHECK(lr.count == 1 && find_entry(&lr, "sub") >= 0);

  /* move within the account: the service's move */
  TEST_CHECK(transfer(true, NULL, 0, &root, &l, (const char *const[]){ "a (3).txt" }, 1, NULL, &ren, CONFLICT_ASK,
                      &in) == FM_OK);
  TEST_CHECK(in.files_done == 1 && in.kind == JOB_MOVE);
  relist(&l, &root);
  relist(&lr, &ren);
  TEST_CHECK(find_entry(&l, "a (3).txt") < 0 && find_entry(&lr, "a (3).txt") >= 0);

  /* move out of the cloud: downloaded, then removed there */
  char down2[FM_PATH_MAX];
  fm_path_join(down2, sizeof down2, tmp, "down2");
  plat_mkdirs(down2);
  TEST_CHECK(transfer(true, NULL, 0, &root, &l, (const char *const[]){ "a (2).txt" }, 1, down2, NULL, CONFLICT_ASK,
                      &in) == FM_OK);
  relist(&l, &root);
  fm_path_join(got, sizeof got, down2, "a (2).txt");
  TEST_CHECK(plat_exists(got) && find_entry(&l, "a (2).txt") < 0);
  TEST_CHECK(cloud_key_serial(in.touched[1]) == serial);

  /* move into the cloud: the local file goes */
  TEST_CHECK(transfer(true, (const char *const[]){ got }, 1, NULL, NULL, NULL, 0, NULL, &ren, CONFLICT_ASK, &in) ==
             FM_OK);
  relist(&lr, &ren);
  TEST_CHECK(!plat_exists(got) && find_entry(&lr, "a (2).txt") >= 0);

  /* open: download into the cache file */
  char cache[FM_PATH_MAX];
  int ai = find_entry(&l, "a.txt");
  TEST_CHECK(ai >= 0);
  if (ai >= 0) {
    const FmCloudEntry *e = cloud_vfs_entry(&l, &l.items[ai]);
    TEST_CHECK(cloud_cache_path(serial, e, cache, sizeof cache));
    FmCloudJobSpec s;
    memset(&s, 0, sizeof s);
    s.op = CJ_OPEN;
    s.src_loc = &root;
    s.entries = e;
    s.nentries = 1;
    s.open_path = cache;
    TEST_CHECK(cloud_job_run(&s, true, &in) == FM_OK);
    TEST_CHECK(in.kind == JOB_OPEN && !strcmp(in.result, cache));
    TEST_CHECK(read_file(cache, buf, sizeof buf) == 11);
    plat_remove_file(cache);
  }

  /* delete */
  int before = cloud_mock_count();
  {
    int ri = find_entry(&l, "Renamed");
    FmCloudJobSpec s;
    memset(&s, 0, sizeof s);
    s.op = CJ_DELETE;
    s.src_loc = &root;
    s.entries = ri >= 0 ? cloud_vfs_entry(&l, &l.items[ri]) : NULL;
    s.nentries = ri >= 0 ? 1 : 0;
    TEST_CHECK(cloud_job_run(&s, true, &in) == FM_OK && in.files_done == 1);
  }
  relist(&l, &root);
  TEST_CHECK(find_entry(&l, "Renamed") < 0 && cloud_mock_count() < before - 2);

  /* the listing cache: back to a cached folder needs no call */
  vfs_list(&ls, &subloc, true);
  TEST_CHECK(!ls.loading && ls.count == 1);
  /* hidden names follow the setting */
  t = cloud_task_new(CT_MKDIR, serial);
  if (t) {
    fm_strlcpy(t->name, ".hidden", sizeof t->name);
    cloud_task_start(t);
  }
  relist(&l, &root);
  int all = l.count;
  char key[CLOUD_ID_MAX + 32];
  cloud_loc_key(&root, key, sizeof key);
  vfs_list(&l, &root, false);
  TEST_CHECK(l.count == all - 1);

  vfs_free(&l);
  vfs_free(&ls);
  vfs_free(&lr);
  fm_free(blob);
}

static void test_signin(int serial_unused) {
  FM_UNUSED(serial_unused);
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "mock", sizeof a.provider);
  fm_strlcpy(a.user, "bob", sizeof a.user);
  fm_strlcpy(a.secret, "wrong", sizeof a.secret);
  int i = cloud_acct_add_temp(&a);
  TEST_CHECK(i >= 0);
  int serial = cloud_acct_serial(i);
  FmLoc root;
  cloud_loc_root(&root, serial);
  FmListing l;
  memset(&l, 0, sizeof l);
  vfs_list(&l, &root, true);
  TEST_CHECK(l.err == FM_ERR_PASSWORD && strstr(l.errmsg, "Signed out"));
  FmCloudTask *t = cloud_task_new(CT_LOGIN, serial);
  cloud_task_start(t);
  TEST_CHECK(cloud_acct_by_serial(serial)->session[0] == 0);
  cloud_acct_lock();
  fm_strlcpy(cloud_acct_by_serial(serial)->secret, "right", sizeof a.secret);
  cloud_acct_unlock();
  t = cloud_task_new(CT_LOGIN, serial);
  cloud_task_start(t);
  TEST_CHECK(!strcmp(cloud_acct_by_serial(serial)->session, "mock-session:bob"));
  TEST_CHECK(cloud_acct_by_serial(serial)->secret[0] == 0);
  vfs_list(&l, &root, true);
  TEST_CHECK(l.err == FM_OK && !l.loading);
  /* a removed account: its panel says so */
  cloud_acct_remove(cloud_acct_index(serial));
  cloud_vfs_forget(serial);
  vfs_list(&l, &root, true);
  TEST_CHECK(l.err == FM_ERR_NOT_FOUND && l.errmsg[0]);
  TEST_CHECK(cloud_task_new(CT_LIST, serial) == NULL);
  vfs_free(&l);
}

int test_cloud(const char *tmp) {
  int before = g_test_fail;
  test_times();
  test_http_errors();
  test_accounts(tmp);

  /* the rest on the in-memory service, with tasks inline */
  char store[FM_PATH_MAX];
  fm_path_join(store, sizeof store, tmp, "cloud-mock.txt");
  cloud_acct_set_store(store);
  cloud_mock_enable(true);
  cloud_mock_reset(false);
  cloud_set_sync(true);
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "mock", sizeof a.provider);
  fm_strlcpy(a.label, "Test cloud", sizeof a.label);
  fm_strlcpy(a.session, "mock-session:test", sizeof a.session);
  int i = cloud_acct_add_temp(&a);
  int serial = cloud_acct_serial(i);
  test_locations(serial);
  test_mock(tmp, serial);
  test_signin(serial);
  cloud_set_sync(false);
  cloud_mock_enable(false);
  cloud_acct_set_store(NULL);
  return g_test_fail - before;
}
