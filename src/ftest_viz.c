/* ftest_viz.c -- self test of the music visualizer and the equalizer: band
** mapping, smoothing, presets, filter gain, limiter and the settings round trip.
**
** Design decisions:
**   - Runs without a window: only the analysis half of fviz.c is used
**     (viz_update never draws), fed with generated sines.
**   - The settings round trip points the config folder at the test folder
**     first and skips itself when that does not take, so it can never
**     overwrite the user's own mmcfm.ini.
*/
#include "ftest.h"
#include "fplat.h"
#include "fviz.h"
#include "feq.h"
#ifdef FM_WIN
#  include "fwin.h"
#endif

static void sine(float *s, int n, float hz, float amp, int rate) {
  for (int i = 0; i < n; i++) s[i] = amp * sinf(6.2831853f * hz * (float)i / (float)rate);
}

/* Index of the loudest band after a few frames of a steady signal. */
static int loudest(const FmVizConf *v, const float *s, int rate) {
  viz_reset();
  for (int i = 0; i < 30; i++) viz_update(v, s, rate, 1.0f / 60.0f);
  int best = 0;
  for (int b = 1; b < v->bands; b++)
    if (viz_level(b) > viz_level(best)) best = b;
  return best;
}

static void test_mapping(void) {
  float e[VIZ_MAX_BANDS + 1];
  for (int log_scale = 0; log_scale < 2; log_scale++) {
    for (int n = VIZ_MIN_BANDS; n <= VIZ_MAX_BANDS; n += 16) {
      viz_band_edges(n, log_scale != 0, 48000, e);
      bool mono = true, inside = true;
      for (int i = 0; i < n; i++) if (!(e[i + 1] > e[i])) mono = false;
      for (int i = 0; i <= n; i++) if (e[i] < 0 || e[i] > VIZ_FFT / 2) inside = false;
      TEST_CHECK(mono);
      TEST_CHECK(inside);
    }
  }
  /* log: equal ratios; linear: equal steps */
  viz_band_edges(32, true, 44100, e);
  TEST_CHECK(fabsf(e[2] / e[1] - e[31] / e[30]) < 1e-3f);
  viz_band_edges(32, false, 44100, e);
  TEST_CHECK(fabsf((e[2] - e[1]) - (e[31] - e[30])) < 1e-3f);
  /* the top edge stays below Nyquist at a low rate */
  viz_band_edges(48, true, 8000, e);
  TEST_CHECK(e[48] < VIZ_FFT / 2);
}

static void test_spectrum(void) {
  static float s[VIZ_FFT];
  FmVizConf v;
  viz_preset(&v, VIZ_BARS);
  v.bands = 32;
  v.log_scale = true;
  sine(s, VIZ_FFT, 100.0f, 0.5f, 48000);
  int lo = loudest(&v, s, 48000);
  sine(s, VIZ_FFT, 1000.0f, 0.5f, 48000);
  int mid = loudest(&v, s, 48000);
  sine(s, VIZ_FFT, 8000.0f, 0.5f, 48000);
  int hi = loudest(&v, s, 48000);
  TEST_CHECK(lo < mid && mid < hi);
  TEST_CHECK(viz_level(hi) > 0.5f);
  /* silence: everything falls to rest and reports it */
  bool moving = true;
  for (int i = 0; i < 600 && moving; i++) moving = viz_update(&v, NULL, 48000, 1.0f / 60.0f);
  TEST_CHECK(!moving);
  for (int b = 0; b < v.bands; b++) TEST_CHECK(viz_level(b) == 0);
  /* changing the band count keeps working (cached mapping rebuilt) */
  v.bands = VIZ_MAX_BANDS;
  TEST_CHECK(viz_update(&v, s, 44100, 1.0f / 60.0f));
  v.bands = VIZ_MIN_BANDS;
  viz_update(&v, s, 44100, 1.0f / 60.0f);
  viz_reset();
}

static void test_smoothing(void) {
  /* rises faster than it falls, never overshoots, frame-rate independent */
  float up = viz_smooth(0, 1, 0.7f, 0.45f, 1.0f / 60.0f);
  float down = 1.0f - viz_smooth(1, 0, 0.7f, 0.45f, 1.0f / 60.0f);
  TEST_CHECK(up > 0 && up < 1 && down > 0 && down < up);
  float a = 0, b = 0;
  for (int i = 0; i < 4; i++) a = viz_smooth(a, 1, 0.3f, 0.3f, 1.0f / 120.0f);
  for (int i = 0; i < 2; i++) b = viz_smooth(b, 1, 0.3f, 0.3f, 1.0f / 60.0f);
  TEST_CHECK(fabsf(a - b) < 1e-4f);
  TEST_CHECK(viz_smooth(0.5f, 0.5f, 1, 1, 0.1f) == 0.5f);
  TEST_CHECK(viz_smooth(0, 1, 1, 1, 10.0f) <= 1.0f);
}

static bool same(const FmVizConf *a, const FmVizConf *b) {
  return a->style == b->style && a->bands == b->bands && fabsf(a->gain - b->gain) < 0.002f &&
         fabsf(a->attack - b->attack) < 0.002f && fabsf(a->decay - b->decay) < 0.002f &&
         fabsf(a->width - b->width) < 0.002f && fabsf(a->round - b->round) < 0.002f &&
         a->color == b->color && a->color2 == b->color2 && a->peaks == b->peaks &&
         a->mirror == b->mirror && a->log_scale == b->log_scale && a->custom == b->custom;
}

static void test_presets(void) {
  for (int s = 0; s < VIZ_STYLES; s++) {
    FmVizConf v, c;
    viz_preset(&v, s);
    c = v;
    viz_sanitize(&c);
    TEST_CHECK(same(&v, &c));          /* presets are inside the knob ranges */
    TEST_CHECK(v.style == s && !v.custom);
    TEST_CHECK(strcmp(viz_style_name(s), "?") != 0);
  }
  FmVizConf bad;
  memset(&bad, 0, sizeof bad);
  bad.style = 99; bad.bands = 5000; bad.gain = 1e9f; bad.width = -3; bad.color = 42; bad.color2 = -7;
  viz_sanitize(&bad);
  TEST_CHECK(bad.style < VIZ_STYLES && bad.bands == VIZ_MAX_BANDS && bad.gain <= 4.0f);
  TEST_CHECK(bad.width >= 0.15f && bad.color < VIZ_COLS && bad.color2 == -1);
  /* tapping cycles every style but "off", then the custom setup */
  FmVizConf v, saved;
  viz_preset(&v, VIZ_BARS);
  viz_preset(&saved, VIZ_DOTS);
  saved.bands = 80;
  saved.custom = true;
  int seen = 0;
  for (int i = 0; i < VIZ_STYLES; i++) {
    viz_cycle(&v, &saved);
    TEST_CHECK(v.style != VIZ_OFF);
    if (v.custom) { seen++; TEST_CHECK(v.bands == 80); }
  }
  TEST_CHECK(seen == 1);
}

/* Points PLACE_CONFIG at dir; returns false when the platform has no such knob. */
static bool redirect_config(const char *dir, char *old, size_t cap) {
#if defined(FM_WIN)
  wchar_t w[1024], o[1024];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", o, 1024);
  if (n == 0 || n >= 1024) return false;
  WideCharToMultiByte(CP_UTF8, 0, o, -1, old, (int)cap, NULL, NULL);
  if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1024)) return false;
  return SetEnvironmentVariableW(L"APPDATA", w) != 0;
#elif defined(FM_LINUX) || defined(FM_BSD)
  const char *o = getenv("XDG_CONFIG_HOME");
  fm_strlcpy(old, o ? o : "", cap);
  return setenv("XDG_CONFIG_HOME", dir, 1) == 0;
#else
  FM_UNUSED(dir); FM_UNUSED(old); FM_UNUSED(cap);
  return false;
#endif
}

static void restore_config(const char *old) {
#if defined(FM_WIN)
  wchar_t w[1024];
  if (MultiByteToWideChar(CP_UTF8, 0, old, -1, w, 1024)) SetEnvironmentVariableW(L"APPDATA", w);
#elif defined(FM_LINUX) || defined(FM_BSD)
  if (old[0]) setenv("XDG_CONFIG_HOME", old, 1);
  else unsetenv("XDG_CONFIG_HOME");
#else
  FM_UNUSED(old);
#endif
}

static void test_conf(const char *tmp) {
  char old[FM_PATH_MAX], place[FM_PATH_MAX];
  if (!redirect_config(tmp, old, sizeof old)) return;
  bool safe = plat_place(PLACE_CONFIG, place, sizeof place) && fm_path_is_inside(place, tmp);
  if (safe) {
    FmConf keep = conf;
    conf_defaults();
    viz_preset(&conf.viz, VIZ_NEON);
    conf.viz.bands = 72; conf.viz.gain = 2.5f; conf.viz.attack = 0.2f; conf.viz.decay = 0.9f;
    conf.viz.width = 0.4f; conf.viz.round = 0.1f; conf.viz.color = VIZ_COL_HEAT; conf.viz.color2 = 3;
    conf.viz.peaks = true; conf.viz.mirror = false; conf.viz.log_scale = false; conf.viz.custom = true;
    conf.viz_saved = conf.viz;
    conf.viz_saved.style = VIZ_DOTS;
    eq_preset(&conf.eq, EQ_ROCK);
    conf.eq.on = true;
    conf.eq.band[3] = -7.5f;
    conf.eq.custom[9] = 11.0f;
    conf.eq.preset = EQ_CUSTOM;
    conf.eq.width = 1.5f;
    conf.eq.balance = -0.25f;
    FmEqConf eq = conf.eq;
    FmVizConf a = conf.viz, b = conf.viz_saved;
    conf_save();
    conf_defaults();
    TEST_CHECK(!same(&conf.viz, &a));
    conf_load();
    TEST_CHECK(same(&conf.viz, &a));
    TEST_CHECK(same(&conf.viz_saved, &b));
    bool eq_ok = conf.eq.on == eq.on && conf.eq.preset == eq.preset &&
                 fabsf(conf.eq.width - eq.width) < 0.002f && fabsf(conf.eq.balance - eq.balance) < 0.002f &&
                 fabsf(conf.eq.preamp - eq.preamp) < 0.002f;
    for (int i = 0; i < EQ_BANDS; i++)
      if (fabsf(conf.eq.band[i] - eq.band[i]) > 0.002f || fabsf(conf.eq.custom[i] - eq.custom[i]) > 0.002f)
        eq_ok = false;
    TEST_CHECK(eq_ok);
    conf = keep;
  }
  printf("  settings round trip %s\n", safe ? "checked" : "skipped (config folder not redirected)");
  restore_config(old);
}

/* Runs a stereo sine through an equalizer; returns the peak of the last tenth. */
static float eq_run(const FmEqConf *c, float hz, float amp, float *maxabs) {
  static float buf[4800 * 2];
  FmEq e;
  memset(&e, 0, sizeof e);
  eq_design(c, 48000, &e.c);
  float peak = 0, all = 0;
  long t = 0;
  for (int blk = 0; blk < 10; blk++) {
    for (int i = 0; i < 4800; i++, t++) {
      float x = amp * sinf(6.2831853f * hz * (float)t / 48000.0f);
      buf[i * 2] = x;
      buf[i * 2 + 1] = x;
    }
    eq_process(&e, buf, 4800);
    for (int i = 0; i < 4800 * 2; i++) {
      float a = fabsf(buf[i]);
      if (a > all) all = a;
      if (blk == 9 && a > peak) peak = a;
    }
  }
  if (maxabs) *maxabs = all;
  return peak;
}

static void test_eq(void) {
  FmEqConf c;
  /* off, or on and flat: bit-exact pass-through */
  eq_defaults(&c);
  c.on = true;
  TEST_CHECK(eq_is_flat(&c));
  {
    static float buf[512 * 2], ref[512 * 2];
    for (int i = 0; i < 512 * 2; i++) ref[i] = buf[i] = 0.7f * sinf((float)i * 0.37f);
    FmEq e;
    memset(&e, 0, sizeof e);
    eq_design(&c, 44100, &e.c);
    eq_process(&e, buf, 512);
    float d = 0;
    for (int i = 0; i < 512 * 2; i++) d = FM_MAX(d, fabsf(buf[i] - ref[i]));
    TEST_CHECK(d <= 1e-4f);
  }
  /* +6 dB peaking at 1 kHz doubles a 1 kHz sine, leaves 100 Hz alone */
  eq_defaults(&c);
  c.on = true;
  c.band[5] = 6.0f;
  float g = eq_run(&c, 1000.0f, 0.2f, NULL) / 0.2f;
  TEST_CHECK(g > 1.9f && g < 2.1f);
  float g2 = eq_run(&c, 100.0f, 0.2f, NULL) / 0.2f;
  TEST_CHECK(g2 > 0.95f && g2 < 1.08f);
  FmEqCoef k;
  eq_design(&c, 48000, &k);
  TEST_CHECK(fabsf(eq_response_db(&k, 48000, 1000.0f) - 6.0f) < 0.1f);
  /* off bypasses even a wild curve */
  c.on = false;
  eq_design(&c, 48000, &k);
  TEST_CHECK(!k.active);
  /* everything boosted and a hot input: the limiter keeps it inside 1.0 */
  eq_defaults(&c);
  c.on = true;
  for (int i = 0; i < EQ_BANDS; i++) c.band[i] = EQ_MAX_DB;
  c.preamp = EQ_MAX_DB;
  float mx = 0;
  eq_run(&c, 100.0f, 1.0f, &mx);
  TEST_CHECK(mx <= 1.0f);
  eq_run(&c, 6000.0f, 2.0f, &mx);
  TEST_CHECK(mx <= 1.0f);
  /* presets stay in range, and pull the preamp down rather than up */
  for (int p = 0; p < EQ_PRESETS; p++) {
    FmEqConf q, z;
    eq_defaults(&q);
    eq_preset(&q, p);
    z = q;
    eq_sanitize(&z);
    TEST_CHECK(q.preset == p && q.preamp <= 0);
    TEST_CHECK(memcmp(&q, &z, sizeof q) == 0);
    TEST_CHECK(strcmp(eq_preset_name(p), "?") != 0);
  }
  /* the custom chip brings back the user's curve */
  eq_defaults(&c);
  c.custom[2] = 4.5f;
  eq_preset(&c, EQ_BASS);
  eq_preset(&c, EQ_CUSTOM);
  TEST_CHECK(c.band[2] == 4.5f && c.band[0] == 0);
}

int test_viz(const char *tmp) {
  int before = g_test_fail;
  test_mapping();
  test_spectrum();
  test_smoothing();
  test_presets();
  test_eq();
  test_conf(tmp);
  return g_test_fail - before;
}
