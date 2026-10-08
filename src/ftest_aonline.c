/* ftest_aonline.c -- the online audio view: item lines, the library file,
** list layouts, and (with MMCFM_AONLINE_PLAY) the player's way to a stream.
**
** The view needs a window; what can go wrong without one is the one-line
** item format (odd characters, damaged lines, keys), the library store (a
** round trip through its file, order, the caps, toggles) and the choice of
** layout. The library runs on a file in the test folder, never the user's.
**
** MMCFM_AONLINE_PLAY="radio:jazz|audius:lofi|podcasts:history" searches each
** source, takes the first playable item (a podcast's first episode) and
** walks the player's own path: the item line, the resolve hook, aud_open_ex,
** three seconds decoded, a seek on finite tracks, the station's title.
*/
#include "ftest.h"
#include "faudio_online_int.h"
#include "fdec_aud.h"

static void make(FmAsrcItem *it, const char *src, int kind, const char *id, const char *title) {
  memset(it, 0, sizeof *it);
  fm_strlcpy(it->source, src, sizeof it->source);
  it->kind = kind;
  fm_strlcpy(it->id, id, sizeof it->id);
  fm_strlcpy(it->title, title, sizeof it->title);
  it->plays = -1;
}

static void test_ref(void) {
  FmAsrcItem a, b;
  make(&a, "radio", AITEM_STATION, "9617a958-0601-11e8-ae97-52543be04c81", "Tab\there, line\nbreak \\ back");
  fm_strlcpy(a.artist, "C\xC3\xB4te d'Ivoire", sizeof a.artist);
  fm_strlcpy(a.album, "jazz,smooth jazz", sizeof a.album);
  fm_strlcpy(a.art, "https://example.org/logo.png?a=1&b=2", sizeof a.art);
  fm_strlcpy(a.url, "http://stream.example.org:8000/live", sizeof a.url);
  fm_strlcpy(a.codec, "MP3", sizeof a.codec);
  a.bitrate = 128;
  a.duration = 0;
  a.plays = 12345678901ll;
  char ref[AO_REF_MAX];
  aitem_ref(&a, ref, sizeof ref);
  TEST_CHECK(!strchr(ref, '\n') && !strchr(ref, '\r'));
  TEST_CHECK(aitem_parse(ref, &b));
  TEST_CHECK(!strcmp(a.title, b.title) && !strcmp(a.artist, b.artist) && !strcmp(a.album, b.album));
  TEST_CHECK(!strcmp(a.url, b.url) && !strcmp(a.art, b.art) && !strcmp(a.codec, b.codec));
  TEST_CHECK(b.kind == AITEM_STATION && b.bitrate == 128 && b.plays == a.plays && aitem_same(&a, &b));
  char k1[192], k2[192];
  aitem_key(&a, k1, sizeof k1);
  aref_key(ref, k2, sizeof k2);
  TEST_CHECK(k1[0] && !strcmp(k1, k2));
  /* durations keep their fraction, kinds stay in range */
  make(&a, "podcasts", AITEM_TRACK, "ep-1", "Episode");
  a.duration = 3723.5;
  aitem_ref(&a, ref, sizeof ref);
  TEST_CHECK(aitem_parse(ref, &b) && fabs(b.duration - 3723.5) < 0.01);
  /* damaged lines */
  TEST_CHECK(!aitem_parse("", &b));
  TEST_CHECK(!aitem_parse("garbage", &b));
  TEST_CHECK(!aitem_parse("a1\t\t0\t", &b));
  TEST_CHECK(aitem_parse("a1\tradio\t99\tx", &b) && b.kind == AITEM_ALBUM);
  aref_key("nope", k2, sizeof k2);
  TEST_CHECK(k2[0] == 0);
  /* a tiny buffer still ends in a NUL */
  char small[12];
  aitem_ref(&a, small, sizeof small);
  TEST_CHECK(strlen(small) < sizeof small);
}

static void test_libfile(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "audio-library.txt");
  alib_test_file(path);
  alib_load(false);
  TEST_CHECK(alib_count(ALIB_FAV) == 0);
  FmAsrcItem a, b, c, got;
  make(&a, "radio", AITEM_STATION, "st-1", "Station one");
  make(&b, "audius", AITEM_TRACK, "tr-1", "Track\tone");
  make(&c, "podcasts", AITEM_PODCAST, "pod-1", "A podcast");
  u32 v = alib_version();
  alib_add(ALIB_FAV, &a);
  alib_add(ALIB_FAV, &b);
  TEST_CHECK(alib_version() != v);
  TEST_CHECK(alib_count(ALIB_FAV) == 2 && alib_has(ALIB_FAV, &a) && alib_has(ALIB_FAV, &b));
  TEST_CHECK(alib_get(ALIB_FAV, 0, &got) && aitem_same(&got, &b));       /* newest first */
  TEST_CHECK(!alib_toggle(ALIB_FAV, &a) && !alib_has(ALIB_FAV, &a));
  TEST_CHECK(alib_toggle(ALIB_SUBS, &c) && alib_has(ALIB_SUBS, &c));
  /* recently played: playing again moves it to the front, no duplicates */
  alib_add(ALIB_RECENT, &a);
  alib_add(ALIB_RECENT, &b);
  alib_add(ALIB_RECENT, &a);
  TEST_CHECK(alib_count(ALIB_RECENT) == 2);
  TEST_CHECK(alib_get(ALIB_RECENT, 0, &got) && aitem_same(&got, &a));
  /* the cap drops the oldest */
  for (int i = 0; i < 70; i++) {
    char id[16];
    fm_snprintf(id, sizeof id, "x%d", i);
    FmAsrcItem t;
    make(&t, "audius", AITEM_TRACK, id, id);
    alib_add(ALIB_RECENT, &t);
  }
  TEST_CHECK(alib_count(ALIB_RECENT) == 60);
  TEST_CHECK(alib_get(ALIB_RECENT, 0, &got) && !strcmp(got.id, "x69"));
  TEST_CHECK(!alib_has(ALIB_RECENT, &a));
  /* through the file and back */
  alib_pump();
  alib_load(false);
  TEST_CHECK(alib_count(ALIB_FAV) == 1 && alib_has(ALIB_FAV, &b));
  TEST_CHECK(alib_get(ALIB_FAV, 0, &got) && !strcmp(got.title, "Track\tone"));
  TEST_CHECK(alib_count(ALIB_SUBS) == 1 && alib_count(ALIB_RECENT) == 60);
  TEST_CHECK(alib_get(ALIB_RECENT, 59, &got) && !strcmp(got.id, "x10"));
  /* a damaged file loads what it can */
  FILE *f = fm_fopen(path, "ab");
  if (f) {
    fputs("F broken line\nZ a1\tradio\t1\tq\tw\n\nF a1\tradio\t1\tok-1\tFine\n", f);
    fclose(f);
  }
  alib_load(false);
  TEST_CHECK(alib_count(ALIB_FAV) == 2);
  alib_test_file(NULL);
  alib_load(true);
}

static void test_layout(void) {
  AoList l;
  aol_init(&l, "test.list");
  TEST_CHECK(ao_layout_for(&l) == AL_ROWS);
  for (int i = 0; i < 3; i++) make(aol_push(&l), "radio", AITEM_STATION, "s", "S");
  TEST_CHECK(ao_layout_for(&l) == AL_STATIONS);
  make(aol_push(&l), "audius", AITEM_TRACK, "t", "T");
  TEST_CHECK(ao_layout_for(&l) == AL_ROWS);
  aol_clear(&l);
  make(aol_push(&l), "podcasts", AITEM_PODCAST, "p", "P");
  make(aol_push(&l), "archive", AITEM_ALBUM, "a", "A");
  TEST_CHECK(ao_layout_for(&l) == AL_COVERS);
  TEST_CHECK(aol_find(&l, "archive", "a") == 1 && aol_find(&l, "radio", "a") < 0);
  aol_free(&l);
  char d[32];
  ao_fmt_dur(185, d, sizeof d);
  TEST_CHECK(!strcmp(d, "3:05"));
  ao_fmt_dur(3723, d, sizeof d);
  TEST_CHECK(!strcmp(d, "1:02:03"));
  ao_fmt_dur(0, d, sizeof d);
  TEST_CHECK(d[0] == 0);
}

/* ---- the player's path to a stream, live ------------------------------------------------ */

static bool first_playable(const FmAsrc *s, const FmAsrcConf *c, const char *q, FmAsrcItem *out) {
  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  FmErr e = s->search(c, q, "", &p, NULL);
  printf("  %s search \"%s\": %s, %d items %s\n", s->key, q, fm_err_str(e), p.count, p.error);
  bool ok = false;
  for (int i = 0; i < p.count && !ok; i++)
    if (aitem_playable(&p.items[i])) { *out = p.items[i]; ok = true; }
  for (int i = 0; i < p.count && !ok && s->children; i++) {
    if (!aitem_container(&p.items[i])) continue;
    FmAsrcPage ch;
    memset(&ch, 0, sizeof ch);
    e = s->children(c, &p.items[i], "", &ch, NULL);
    printf("  %s children of \"%s\": %s, %d items %s\n", s->key, p.items[i].title, fm_err_str(e), ch.count, ch.error);
    for (int k = 0; k < ch.count && !ok; k++)
      if (aitem_playable(&ch.items[k])) { *out = ch.items[k]; ok = true; }
    asrc_page_free(&ch);
  }
  asrc_page_free(&p);
  return ok;
}

static void play_one(const char *spec) {
  char key[32], q[256];
  const char *colon = strchr(spec, ':');
  fm_strlcpy(key, spec, colon ? FM_MIN((size_t)(colon - spec) + 1, sizeof key) : sizeof key);
  fm_strlcpy(q, colon ? colon + 1 : "music", sizeof q);
  const FmAsrc *s = asrc_find(key);
  if (!s) { printf("  no source %s\n", key); TEST_CHECK(s != NULL); return; }
  FmAsrcConf *c = (FmAsrcConf *)fm_alloc(sizeof *c);
  asrc_conf_snapshot(c);
  FmAsrcItem *it = (FmAsrcItem *)fm_alloc(sizeof *it);
  char *ref = (char *)fm_alloc(AO_REF_MAX);
  FmAudioStream *st = (FmAudioStream *)fm_calloc(1, sizeof *st);
  if (!first_playable(s, c, q, it)) {
    TEST_CHECK(!"nothing playable");
  } else {
    aitem_ref(it, ref, AO_REF_MAX);
    aplay_snapshot();
    u64 t0 = plat_now_ms();
    bool ok = aplay_resolve(ref, st, NULL);
    printf("  resolve \"%s\": %s %s (%d ms)\n", it->title, ok ? "OK" : "FAILED", ok ? st->url : st->err,
           (int)(plat_now_ms() - t0));
    TEST_CHECK(ok);
    FmErr e = FM_OK;
    FmAudio *a = ok ? aud_open_ex(st->url, st->headers[0] ? st->headers : NULL, &e) : NULL;
    u64 t1 = plat_now_ms();
    if (ok && !a) printf("    aud_open_ex: %s (%d ms)\n", fm_err_str(e), (int)(t1 - t0));
    TEST_CHECK(!ok || a);
    if (a) {
      int rate = aud_rate(a);
      float *buf = (float *)fm_alloc(4096 * 2 * sizeof(float));
      i64 got = 0, want = (i64)rate * 3;
      float peak = 0;
      while (got < want) {
        int k = aud_read(a, buf, 4096);
        if (k <= 0) break;
        for (int i = 0; i < k * aud_channels(a); i++) peak = FM_MAX(peak, fabsf(buf[i]));
        got += k;
      }
      bool sk = true;
      if (!aud_is_live(a) && aud_length(a) > (u64)rate * 20) {
        sk = aud_seek(a, aud_length(a) / 2) && aud_read(a, buf, 4096) > 0;
      }
      char now[256];
      aud_now_playing(a, now, sizeof now);
      printf("    %s %d Hz x%d %s, %.1f s decoded in %d ms, peak %.2f, seek %s%s%s\n", aud_codec(a), rate,
             aud_channels(a), aud_is_live(a) ? "LIVE" : "track", (double)got / FM_MAX(rate, 1),
             (int)(plat_now_ms() - t1), peak, aud_is_live(a) ? "n/a" : sk ? "ok" : "FAILED", now[0] ? ", now: " : "",
             now);
      TEST_CHECK(got >= want && peak > 0.0001f && sk);
      TEST_CHECK(aud_is_live(a) == (it->kind == AITEM_STATION) || st->live == aud_is_live(a));
      fm_free(buf);
      aud_close(a);
    }
  }
  fm_free(st);
  fm_free(ref);
  fm_free(it);
  fm_free(c);
}

static void test_play(void) {
  const char *spec = getenv("MMCFM_AONLINE_PLAY");
  if (!spec || !*spec) return;
  char one[300];
  while (*spec) {
    size_t n = strcspn(spec, "|");
    fm_strlcpy(one, spec, FM_MIN(n + 1, sizeof one));
    play_one(one);
    spec += n;
    if (*spec == '|') spec++;
  }
}

int test_aonline_ui(const char *tmp) {
  int before = g_test_fail;
  test_ref();
  test_libfile(tmp);
  test_layout();
  test_play();
  return g_test_fail - before;
}
