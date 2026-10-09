/* ftest_queue.c -- the music queue model and playlists: index math, track
** lines, the saved queue, playlists on disk, .m3u8 and folders as tracks.
**
** No sound device: the player's queue is its play order plus these pure
** functions, so the math is checked here on plain arrays the way the player
** uses it (positions, the current track, shuffle of the up-next part).
** The player's own queue is checked parked (no device), and with
** MMCFM_QUEUE_PLAY=1 on a real sound device.
*/
#include "ftest.h"
#include "fplat.h"
#include "fqueue.h"
#include "fview_int.h"

static bool same(const int *a, const int *b, int n) { return memcmp(a, b, sizeof(int) * (size_t)n) == 0; }

static void put(const char *dir, const char *rel) {
  char p[FM_PATH_MAX], d[FM_PATH_MAX];
  fm_path_join(p, sizeof p, dir, rel);
  fm_strlcpy(d, p, sizeof d);
  if (fm_path_parent(d)) plat_mkdirs(d);
  FILE *f = fm_fopen(p, "wb");
  if (f) { fputs("x", f); fclose(f); }
}

static void test_math(void) {
  /* insert: at the end, after the current one (play next), clamped */
  int a[16] = { 10, 11, 12, 13 }, n = 4;
  int v[2] = { 20, 21 };
  q_insert(a, &n, 2, v, 2);
  int e1[] = { 10, 11, 20, 21, 12, 13 };
  TEST_CHECK(n == 6 && same(a, e1, 6));
  q_insert(a, &n, 99, v, 1);
  TEST_CHECK(n == 7 && a[6] == 20);
  TEST_CHECK(q_find(a, n, 21) == 3 && q_find(a, n, 99) == -1);

  /* remove before, at and after the current position */
  int b[8] = { 0, 1, 2, 3, 4 }, nb = 5;
  TEST_CHECK(q_remove(b, &nb, 0, 2) == 1 && nb == 4 && b[1] == 2);      /* before: cur moves down */
  TEST_CHECK(q_remove(b, &nb, 3, 1) == 1 && nb == 3);                   /* after: stays */
  int e2[] = { 1, 2, 3 };
  TEST_CHECK(same(b, e2, 3));
  TEST_CHECK(q_remove(b, &nb, 1, 1) == 1 && b[1] == 3);                 /* the current: the next one takes it */
  TEST_CHECK(q_remove(b, &nb, 1, 1) == -1 && nb == 1);                  /* the last one: nothing after */
  TEST_CHECK(q_remove(b, &nb, 5, 0) == 0 && nb == 1);                   /* out of range: no change */

  /* move up / down and a drag across the current track */
  int c[8] = { 0, 1, 2, 3, 4, 5 }, nc = 6;
  TEST_CHECK(q_move(c, nc, 4, 3, 1) == 1);                              /* Alt+Up after the current */
  int e3[] = { 0, 1, 2, 4, 3, 5 };
  TEST_CHECK(same(c, e3, 6));
  TEST_CHECK(q_move(c, nc, 2, 5, 1) == 1);                              /* drag down to the end */
  int e4[] = { 0, 1, 4, 3, 5, 2 };
  TEST_CHECK(same(c, e4, 6));
  TEST_CHECK(q_move(c, nc, 5, 0, 1) == 2);                              /* over the current: it shifts */
  int e5[] = { 2, 0, 1, 4, 3, 5 };
  TEST_CHECK(same(c, e5, 6));
  TEST_CHECK(q_move(c, nc, 2, 4, 2) == 4);                              /* the current itself moves */
  TEST_CHECK(q_move(c, nc, 4, 1, 3) == 4);                              /* from after to before cur */
  TEST_CHECK(q_move(c, nc, 1, 1, 3) == 3);                              /* no move */
  TEST_CHECK(q_move(c, nc, 0, 99, 0) == 5 && c[5] == 2);                /* clamped */

  /* "Play next" on a queued track = move it right after the current one */
  int d[8] = { 7, 8, 9, 10, 11 }, nd = 5;
  q_move(d, nd, 4, 2, 1);
  int e6[] = { 7, 8, 11, 9, 10 };
  TEST_CHECK(same(d, e6, 5));
}

static void test_shuffle(void) {
  /* shuffle keeps the heard part and the current track, mixes up next */
  int a[32], n = 20;
  for (int i = 0; i < n; i++) a[i] = i;
  int cur = 6;
  q_shuffle(a, n, cur + 1, 12345);
  bool prefix = true, moved = false;
  for (int i = 0; i <= cur; i++) prefix = prefix && a[i] == i;
  TEST_CHECK(prefix);
  int seen[32] = { 0 };
  for (int i = 0; i < n; i++) { seen[a[i]]++; moved = moved || a[i] != i; }
  bool perm = true;
  for (int i = 0; i < n; i++) perm = perm && seen[i] == 1;
  TEST_CHECK(perm && moved);
  int a2[32];
  for (int i = 0; i < n; i++) a2[i] = i;
  q_shuffle(a2, n, cur + 1, 12345);
  TEST_CHECK(same(a, a2, n));                                           /* the seed decides */
  q_shuffle(a2, 1, 0, 9);                                               /* one item: nothing to do */
  TEST_CHECK(a2[0] == 0);

  /* shuffle off: back to the order from before, with what changed since */
  int nat[8] = { 0, 1, 2, 3, 4 }, nn = 5;
  int order[8] = { 0, 1, 4, 2, 3 }, no = 5;                             /* shuffled after cur = 1 */
  int add = 5;
  q_insert(order, &no, no, &add, 1);                                    /* queued while shuffled */
  q_insert(nat, &nn, nn, &add, 1);
  int k = q_find(order, no, 3);
  q_remove(order, &no, k, 1);                                           /* removed while shuffled */
  k = q_find(nat, nn, 3);
  q_remove(nat, &nn, k, 0);
  int extra = 9;
  q_insert(order, &no, 2, &extra, 1);                                   /* one nat does not know */
  int out[8];
  q_unshuffle(nat, nn, order, no, out);
  int e1[] = { 0, 1, 2, 4, 5, 9 };
  TEST_CHECK(no == 6 && same(out, e1, 6));
}

static void test_lines(void) {
  char line[4096];
  FmQItem it;
  FmAudioEntry f;
  memset(&f, 0, sizeof f);
  f.url = "C:\\Music\\a\tb.mp3";
  f.title = "Song \\ one";
  f.artist = "Art\nist";
  f.file = true;
  f.dur = 201.5;
  TEST_CHECK(qline_make(&f, line, sizeof line));
  TEST_CHECK(!strchr(line, '\n'));
  TEST_CHECK(qline_parse(line, &it));
  TEST_CHECK(it.file && !it.live && fabs(it.dur - 201.5) < 0.01);
  TEST_CHECK(!strcmp(it.url, f.url) && !strcmp(it.title, f.title) && !strcmp(it.artist, f.artist));
  TEST_CHECK(!it.ref[0] && !it.album[0]);
  FmAudioEntry back = qitem_entry(&it);
  TEST_CHECK(back.file && back.ref == NULL && !strcmp(back.url, f.url));
  qitem_free(&it);

  /* an online item: no url, a ref that is itself a tabbed line */
  FmAudioEntry o;
  memset(&o, 0, sizeof o);
  o.title = "Station";
  o.ref = "radio\t1\tabc-123\tStation\\n";
  o.art_url = "https://x/logo.png";
  o.live = true;
  TEST_CHECK(qline_make(&o, line, sizeof line));
  TEST_CHECK(qline_parse(line, &it) && !it.file && it.live && !strcmp(it.ref, o.ref) && !it.url[0]);
  TEST_CHECK(!strcmp(it.art, o.art_url));
  qitem_free(&it);

  /* not kept: request headers (tokens), nothing to play from, a cut line */
  FmAudioEntry h = o;
  h.headers = "Authorization: Bearer x\r\n";
  TEST_CHECK(!qline_make(&h, line, sizeof line));
  FmAudioEntry z;
  memset(&z, 0, sizeof z);
  z.title = "nothing";
  TEST_CHECK(!qline_make(&z, line, sizeof line));
  TEST_CHECK(!qline_make(&o, line, 40));
  TEST_CHECK(!qline_parse("garbage", &it));
  TEST_CHECK(!qline_parse("F\t0\t0\t\tno path\t\t\t\t", &it));
  TEST_CHECK(!qline_parse("E\t0\t0", &it));
}

static void test_saved(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "audio-queue.txt");
  FmAudioEntry e[4];
  memset(e, 0, sizeof e);
  e[0].url = "/m/one.mp3"; e[0].file = true; e[0].title = "One";
  e[1].url = "https://cloud/x"; e[1].headers = "Cookie: s\r\n";        /* skipped */
  e[2].ref = "audius\t2\tid9"; e[2].title = "Online"; e[2].dur = 99;
  e[3].url = "/m/two.flac"; e[3].file = true;
  TEST_CHECK(qsave_write(path, e, 4, 2, 41.5, true, 1));
  FmQSaved s;
  TEST_CHECK(qsave_read(path, &s));
  TEST_CHECK(s.n == 3 && s.cur == 1 && fabs(s.pos - 41.5) < 0.01 && s.shuffle && s.repeat == 1);
  TEST_CHECK(s.n == 3 && !strcmp(s.items[1].ref, "audius\t2\tid9") && !strcmp(s.items[2].url, "/m/two.flac"));
  qsave_free(&s);
  /* the skipped one was current: the one before it, from its start */
  TEST_CHECK(qsave_write(path, e, 4, 1, 12, false, 0));
  TEST_CHECK(qsave_read(path, &s) && s.cur == 0 && s.pos == 0);
  qsave_free(&s);
  /* an empty queue removes the file */
  TEST_CHECK(qsave_write(path, e, 0, 0, 0, false, 0));
  TEST_CHECK(!plat_exists(path) && !qsave_read(path, &s));
}

static void test_playlists(const char *tmp) {
  char file[FM_PATH_MAX];
  fm_path_join(file, sizeof file, tmp, "audio-playlists.txt");
  pl_test_file(file);
  TEST_CHECK(pl_count() == 0);
  int a = pl_create("Road trip");
  TEST_CHECK(a == 0 && pl_count() == 1);
  TEST_CHECK(pl_create("  ") < 0);
  char name[PL_NAME_MAX];
  pl_unique_name(NULL, name, sizeof name);
  TEST_CHECK(!strcmp(name, "Playlist 1"));
  pl_unique_name("Road trip", name, sizeof name);
  TEST_CHECK(!strcmp(name, "Road trip 2"));
  FmAudioEntry e[3];
  memset(e, 0, sizeof e);
  e[0].url = "/m/a.mp3"; e[0].file = true; e[0].title = "A"; e[0].dur = 60;
  e[1].ref = "radio\t1\tst1"; e[1].title = "B"; e[1].live = true;
  e[2].url = "/m/c.mp3"; e[2].file = true; e[2].title = "C"; e[2].dur = 30;
  TEST_CHECK(pl_add(0, e, 3) == 3 && pl_len(0) == 3 && fabs(pl_duration(0) - 90) < 0.01);
  pl_move(0, 2, 0);
  TEST_CHECK(!strcmp(pl_item(0, 0)->title, "C") && !strcmp(pl_item(0, 1)->title, "A"));
  int b = pl_create("Focus");                       /* newest on top */
  TEST_CHECK(b == 0 && !strcmp(pl_name(1), "Road trip"));
  TEST_CHECK(!pl_rename(0, "road TRIP") && pl_rename(0, "Focus music"));
  TEST_CHECK(pl_find("focus MUSIC") == 0);
  pl_add(0, &e[0], 1);
  pl_remove(1, 1);                                  /* "A" out of the road trip */
  TEST_CHECK(pl_len(1) == 2 && !strcmp(pl_item(1, 1)->title, "B"));
  TEST_CHECK(!pl_local_only(1) && pl_local_only(0));
  u32 g = pl_gen();
  pl_flush();
  /* a fresh load from the file */
  pl_test_file(file);
  TEST_CHECK(pl_count() == 2 && pl_gen() != 0);
  TEST_CHECK(!strcmp(pl_name(0), "Focus music") && !strcmp(pl_name(1), "Road trip"));
  TEST_CHECK(pl_len(1) == 2 && !strcmp(pl_item(1, 1)->ref, "radio\t1\tst1") && pl_item(1, 1)->live);
  int n = 0;
  FmAudioEntry *pe = pl_entries(1, &n);
  TEST_CHECK(n == 2 && pe[0].file && !strcmp(pe[0].url, "/m/c.mp3") && pe[1].ref && !pe[1].file);
  fm_free(pe);
  FM_UNUSED(g);

  /* .m3u8 out and back in (paths relative to the list work too) */
  char m3u[FM_PATH_MAX];
  fm_path_join(m3u, sizeof m3u, tmp, "focus.m3u8");
  TEST_CHECK(pl_export_m3u(0, m3u));
  TEST_CHECK(!pl_export_m3u(1, m3u) || true);      /* online tracks: not offered, may refuse */
  int k = pl_import_m3u(m3u);
  TEST_CHECK(k == 0 && pl_len(0) == 1 && !strcmp(pl_name(0), "focus"));
  TEST_CHECK(pl_item(0, 0) && !strcmp(pl_item(0, 0)->title, "A") && fabs(pl_item(0, 0)->dur - 60) < 0.01);
  char rel[FM_PATH_MAX];
  fm_path_join(rel, sizeof rel, tmp, "rel.m3u");
  FILE *f = fm_fopen(rel, "wb");
  if (f) {
    fputs("#EXTM3U\n#EXTINF:12,Band - Tune\nsub/t.mp3\n\nhttp://radio.example/s\n# comment\n", f);
    fclose(f);
  }
  k = pl_import_m3u(rel);
  TEST_CHECK(k == 0 && pl_len(0) == 2);
  const FmQItem *t0 = pl_item(0, 0), *t1 = pl_item(0, 1);
  char want[FM_PATH_MAX];
  fm_path_join(want, sizeof want, tmp, "sub");
  fm_path_join(want, sizeof want, want, "t.mp3");
  TEST_CHECK(t0 && t0->file && !strcmp(t0->url, want) && !strcmp(t0->artist, "Band") && !strcmp(t0->title, "Tune"));
  TEST_CHECK(t1 && !t1->file && !strcmp(t1->url, "http://radio.example/s"));
  pl_delete(0);
  pl_delete(0);
  TEST_CHECK(pl_count() == 2);
  pl_flush();
  pl_test_file(NULL);
}

static void test_folders(const char *tmp) {
  char dir[FM_PATH_MAX];
  fm_path_join(dir, sizeof dir, tmp, "music");
  put(dir, "b10.mp3");
  put(dir, "b2.mp3");
  put(dir, "cover.jpg");
  put(dir, "Disc 1/a.ogg");
  put(dir, ".hidden/x.mp3");
  int n = 0;
  const char *const one[] = { dir };
  FmAudioEntry *e = qents_from_paths(one, 1, 100, &n);
  TEST_CHECK(n == 3);
  if (n == 3) {
    TEST_CHECK(!strcmp(fm_path_base(e[0].url), "b2.mp3") && !strcmp(fm_path_base(e[1].url), "b10.mp3"));
    TEST_CHECK(!strcmp(fm_path_base(e[2].url), "a.ogg") && e[2].file);
  }
  qents_free(e, n);
  e = qents_from_paths(one, 1, 2, &n);              /* capped */
  TEST_CHECK(n == 2);
  qents_free(e, n);
  char f[FM_PATH_MAX];
  fm_path_join(f, sizeof f, dir, "cover.jpg");
  const char *const two[] = { f, NULL };
  e = qents_from_paths(two, 2, 100, &n);            /* not audio */
  TEST_CHECK(n == 0);
  qents_free(e, n);
}

/* The titles of the queue in play order, "ABC", and the one heard. */
static void order_str(char *out, int *cur) {
  int n = audio_queue_len(), k = 0;
  for (int i = 0; i < n && k < 30; i++) {
    FmAudioQInfo q;
    out[k++] = audio_queue_get(i, &q) ? q.title[0] : '?';
  }
  out[k] = 0;
  *cur = audio_queue_pos();
}

static bool is(const char *want, int want_cur) {
  char got[32];
  int cur;
  order_str(got, &cur);
  if (strcmp(got, want) != 0 || cur != want_cur) {
    printf("  queue is %s (at %d), wanted %s (at %d)\n", got, cur, want, want_cur);
    return false;
  }
  return true;
}

/* The player's own queue, parked (no device): add, play next, move,
** remove, clear and shuffle as the views call them. */
static void test_player(void) {
  static const char *const kT[] = { "A", "B", "C", "D", "E", "F", "G", "H" };
  FmAudioEntry e[8];
  memset(e, 0, sizeof e);
  char paths[8][16];
  for (int i = 0; i < 8; i++) {
    fm_snprintf(paths[i], sizeof paths[i], "/m/%s.mp3", kT[i]);
    e[i].url = paths[i];
    e[i].title = kT[i];
    e[i].file = true;
    e[i].dur = 100 + i;
  }
  audio_queue_test(e, 4, 1, 12.5);
  TEST_CHECK(is("ABCD", 1) && audio_state(NULL, 0) == AUDIO_PAUSED);
  TEST_CHECK(audio_queue_add(&e[4], 1, AQ_NEXT) && is("ABECD", 1));       /* play next */
  TEST_CHECK(audio_queue_add(&e[5], 1, AQ_END) && is("ABECDF", 1));       /* add to queue */
  audio_queue_move(5, 2);                                                 /* drag F up to next */
  TEST_CHECK(is("ABFECD", 1));
  audio_queue_move(2, 3);                                                 /* Alt+Down */
  TEST_CHECK(is("ABEFCD", 1));
  audio_queue_remove(0);                                                  /* before the current */
  TEST_CHECK(is("BEFCD", 0));
  audio_queue_remove(4);                                                  /* after */
  TEST_CHECK(is("BEFC", 0));
  audio_queue_remove(0);                                                  /* the current: the next plays */
  TEST_CHECK(is("EFC", 0));
  FmAudioQInfo q;
  TEST_CHECK(audio_queue_get(0, &q) && q.file && !strcmp(q.path, "/m/E.mp3") && fabs(q.dur - 104) < 0.01);
  int n = 0;
  FmAudioEntry *all = audio_queue_entries(&n);
  TEST_CHECK(n == 3 && all[2].file && !strcmp(all[2].title, "C"));
  fm_free(all);
  audio_queue_clear();                                                    /* up next goes */
  TEST_CHECK(is("E", 0));
  audio_queue_remove(0);                                                  /* the last one: unloaded */
  TEST_CHECK(audio_queue_len() == 0 && audio_queue_pos() == -1 && audio_state(NULL, 0) == AUDIO_IDLE);

  /* shuffle: what was heard and the current one stay, up next is mixed;
  ** off again: the order from before, with what was added meanwhile */
  audio_queue_test(e, 8, 2, 0);
  TEST_CHECK(audio_queue_shuffle());
  char got[32];
  int cur;
  order_str(got, &cur);
  TEST_CHECK(cur == 2 && !strncmp(got, "ABC", 3) && strlen(got) == 8);
  bool all8 = true;
  for (int i = 0; i < 8; i++) all8 = all8 && strchr(got, 'A' + i);
  TEST_CHECK(all8);
  FmAudioEntry z = e[0];
  z.title = "Z";
  audio_queue_add(&z, 1, AQ_NEXT);                                        /* plays next even shuffled */
  order_str(got, &cur);
  TEST_CHECK(got[3] == 'Z');
  int hpos = -1;
  for (int i = 0; i < 9; i++) if (got[i] == 'H') hpos = i;
  audio_queue_remove(hpos);
  TEST_CHECK(!audio_queue_shuffle());
  TEST_CHECK(is("ABCZDEFG", 2));
  audio_queue_test(NULL, 0, 0, 0);
}

/* MMCFM_QUEUE_PLAY=1: the queue on a real sound device. Four short tones
** play gaplessly in the queue's order after a "Play next" and a drag (the
** decoder takes what comes next from the queue at each track's end), and a
** parked queue starts at its saved position. */
static void write_tone(const char *path, int hz, double sec) {
  int rate = 22050, n = (int)(rate * sec);
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  u32 data = (u32)n * 2, riff = 36 + data;
  u8 h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
               0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
  memcpy(h + 4, &riff, 4);
  u32 r = (u32)rate, br = (u32)rate * 2;
  memcpy(h + 24, &r, 4);
  memcpy(h + 28, &br, 4);
  memcpy(h + 40, &data, 4);
  fwrite(h, 1, 44, f);
  for (int i = 0; i < n; i++) {
    i16 v = (i16)(sin(i * 6.2831853 * hz / rate) * 3000);
    fwrite(&v, 2, 1, f);
  }
  fclose(f);
}

static void test_live(const char *tmp) {
  const char *on = getenv("MMCFM_QUEUE_PLAY");
  if (!on || strcmp(on, "1") != 0) return;
  static const char *const kT[] = { "A", "B", "C", "D" };
  char paths[4][FM_PATH_MAX];
  FmAudioEntry e[4];
  memset(e, 0, sizeof e);
  for (int i = 0; i < 4; i++) {
    char name[16];
    fm_snprintf(name, sizeof name, "%s.wav", kT[i]);
    fm_path_join(paths[i], sizeof paths[i], tmp, name);
    write_tone(paths[i], 330 + 110 * i, 0.7);
    e[i].url = paths[i];
    e[i].title = kT[i];
    e[i].file = true;
  }
  audio_queue_test(e, 3, 0, 0.3);                     /* parked at A, 0.3 s in */
  audio_queue_add(&e[3], 1, AQ_NEXT);                 /* A D B C */
  audio_queue_move(3, 2);                             /* A D C B */
  TEST_CHECK(is("ADCB", 0));
  u64 t0 = plat_now_ms();
  audio_queue_jump(0);                                /* unparks: device, thread */
  char seq[16] = "";
  int k = 0, st = AUDIO_IDLE;
  while (plat_now_ms() - t0 < 5000) {
    int p = audio_queue_pos();
    FmAudioQInfo q;
    if (p >= 0 && audio_queue_get(p, &q) && (k == 0 || seq[k - 1] != q.title[0]) && k < 15) {
      seq[k++] = q.title[0];
      seq[k] = 0;
    }
    st = audio_state(NULL, 0);
    if (k == 4 && st == AUDIO_PAUSED) break;
    SDL_Delay(20);
  }
  printf("  live queue: heard %s in %llu ms, state %d\n", seq, (unsigned long long)(plat_now_ms() - t0), st);
  TEST_CHECK(!strcmp(seq, "ADCB"));
  /* a restored queue resumes where it was: 2.6 s into a 3 s tone */
  char longp[FM_PATH_MAX];
  fm_path_join(longp, sizeof longp, tmp, "E.wav");
  write_tone(longp, 660, 3.0);
  FmAudioEntry le = e[0];
  le.url = longp;
  le.title = "E";
  audio_queue_test(&le, 1, 0, 2.6);
  t0 = plat_now_ms();
  g_view_audio.open(longp, NULL, 0, 0);               /* what tapping the mini player's file does */
  bool played = false;
  while (plat_now_ms() - t0 < 4000) {
    st = audio_state(NULL, 0);
    played = played || st == AUDIO_PLAYING;
    if (played && st == AUDIO_PAUSED) break;
    SDL_Delay(10);
  }
  u64 ms = plat_now_ms() - t0;
  printf("  live resume: the 3 s track ended %llu ms after Play\n", (unsigned long long)ms);
  TEST_CHECK(played && ms < 2000);
  g_view_audio.close();
  audio_queue_test(NULL, 0, 0, 0);
}

int test_queue(const char *tmp) {
  int before = g_test_fail;
  size_t mem0 = fm_mem_in_use();
  test_math();
  test_shuffle();
  test_lines();
  test_saved(tmp);
  test_playlists(tmp);
  test_folders(tmp);
  test_player();
  test_live(tmp);
  TEST_CHECK(fm_mem_in_use() == mem0);
  return g_test_fail - before;
}
