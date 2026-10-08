/* fviz.c -- music visualizer: analysis, nine styles, presets, settings panel.
**
** Design decisions:
**   - Everything is static and sized for the worst case (VIZ_MAX_BANDS,
**     VIZ_FFT, VIZ_WAVE): no allocation, about 30 KB in total. The FFT, the
**     band mapping and the smoothing run on the main thread, once per frame,
**     only while the player is visible; the audio callback just keeps the
**     last samples (fview_aud.c).
**   - The band edges (log or linear, in fractional FFT bins) are cached and
**     rebuilt only when the band count, the scale or the sample rate change.
**     Bands narrower than one bin read the interpolated magnitude at their
**     centre, so 96 log bands stay smooth in the bass.
**   - Smoothing is time based (1 - e^(-speed * dt)), so the motion looks
**     the same at 30 and 144 Hz. Peaks hold, then fall with gravity.
**     viz_update reports "still moving" until every level, peak, ring and
**     the waveform are at rest; then the player stops redrawing.
**   - Styles adapt to the rectangle they get: column counts shrink to keep
**     at least 3 dp per column, dot rows follow the height, ring styles
**     wrap the cover in the player and draw their own centre elsewhere.
**   - Smooth curves (spectrum fill, scope, neon, pulse blob) need
**     per-vertex colours and seamless strips, which fgfx shapes cannot do;
**     they go through a tiny local mesh and SDL_RenderGeometry after a
**     gfx_flush, like the rotated textures in fview.c. Edges get a 1 px
**     feather so they stay anti-aliased.
**   - Colours derive from the theme (T.accent; the "auto" second colour is
**     the accent turned 60 degrees round the hue wheel), so all 20 themes
**     work in light and dark without tuning.
**   - Picking a preset loads its knob values; touching a knob makes the
**     setup "custom" and copies it to conf.viz_saved, which the Custom chip
**     brings back after trying other presets.
*/
#include "fviz.h"
#include "feq.h"

#define VIZ_PI 3.14159265f
#define MESH_V 1024
#define MESH_I 3072
#define CURVE_MAX 512
#define RINGS 6
#define RING_LIFE 1.3f

static const char *const kStyleNames[VIZ_STYLES] = {
  "Off", "Bars", "Mirror", "Pills", "Wave", "Scope", "Radial", "Pulse", "Dots", "Neon"
};

/* analysis */
static float g_re[VIZ_FFT], g_im[VIZ_FFT], g_win[VIZ_FFT];
static float g_mag[VIZ_FFT / 2];
static bool g_win_ok;
static float g_edge[VIZ_MAX_BANDS + 1], g_fc[VIZ_MAX_BANDS];
static int g_map_n, g_map_rate, g_nbass;
static bool g_map_log;
/* state */
static int g_n = 32;
static float g_lv[VIZ_MAX_BANDS], g_pk[VIZ_MAX_BANDS], g_pkv[VIZ_MAX_BANDS], g_hold[VIZ_MAX_BANDS];
static float g_wave[VIZ_WAVE];
static float g_bass, g_bass_avg, g_beat, g_cool;
static float g_ring[RINGS];   /* age in seconds, < 0 = unused */
/* drawing scratch */
static SDL_Vertex g_mv[MESH_V];
static int g_mi[MESH_I];
static int g_nmv, g_nmi;
static float g_cx[CURVE_MAX], g_cy[CURVE_MAX], g_ct[CURVE_MAX];
static FmColor g_cc[CURVE_MAX];
static float g_alpha = 1.0f; /* viz_draw_media fades everything */
/* settings panel */
static FmScroll g_ps;
static float g_panel_h;

/* ---- presets ----------------------------------------------------------------- */

const char *viz_style_name(int style) {
  return style >= 0 && style < VIZ_STYLES ? kStyleNames[style] : "?";
}

bool viz_around_cover(int style) { return style == VIZ_RADIAL || style == VIZ_PULSE; }

void viz_preset(FmVizConf *v, int style) {
  memset(v, 0, sizeof *v);
  v->style = FM_CLAMP(style, 0, VIZ_STYLES - 1);
  v->bands = 48;
  v->gain = 1.0f;
  v->attack = 0.7f;
  v->decay = 0.45f;
  v->width = 0.7f;
  v->round = 0.35f;
  v->color = VIZ_COL_ACCENT;
  v->color2 = -1;
  v->log_scale = true;
  switch (v->style) {
    case VIZ_MIRROR:
      v->bands = 64; v->width = 0.62f; v->round = 1.0f; v->color = VIZ_COL_GRADIENT; v->decay = 0.5f;
      break;
    case VIZ_PILLS:
      v->bands = 32; v->width = 0.62f; v->round = 1.0f; v->peaks = true; v->decay = 0.35f;
      break;
    case VIZ_WAVEFILL:
      v->bands = 64; v->attack = 0.6f; v->decay = 0.4f; v->color = VIZ_COL_GRADIENT;
      break;
    case VIZ_SCOPE:
      v->gain = 1.2f; v->attack = 0.8f; v->decay = 0.5f;
      break;
    case VIZ_RADIAL:
      v->bands = 72; v->width = 0.55f; v->round = 1.0f; v->peaks = true; v->mirror = true;
      v->color = VIZ_COL_RAINBOW;
      break;
    case VIZ_PULSE:
      v->bands = 48; v->attack = 0.6f; v->decay = 0.4f; v->mirror = true; v->color = VIZ_COL_GRADIENT;
      break;
    case VIZ_DOTS:
      v->bands = 24; v->width = 0.72f; v->round = 1.0f; v->peaks = true; v->attack = 0.8f;
      v->decay = 0.5f; v->color = VIZ_COL_HEAT;
      break;
    case VIZ_NEON:
      v->bands = 48; v->attack = 0.6f; v->decay = 0.4f; v->mirror = true; v->color = VIZ_COL_RAINBOW;
      break;
    default: break;
  }
}

void viz_sanitize(FmVizConf *v) {
  v->style = FM_CLAMP(v->style, 0, VIZ_STYLES - 1);
  v->bands = FM_CLAMP(v->bands, VIZ_MIN_BANDS, VIZ_MAX_BANDS);
  if (!(v->gain == v->gain)) v->gain = 1.0f;
  v->gain = FM_CLAMP(v->gain, 0.25f, 4.0f);
  v->attack = FM_CLAMP(v->attack, 0.0f, 1.0f);
  v->decay = FM_CLAMP(v->decay, 0.0f, 1.0f);
  v->width = FM_CLAMP(v->width, 0.15f, 1.0f);
  v->round = FM_CLAMP(v->round, 0.0f, 1.0f);
  v->color = FM_CLAMP(v->color, 0, VIZ_COLS - 1);
  v->color2 = FM_CLAMP(v->color2, -1, UI_ACCENTS - 1);
}

void viz_cycle(FmVizConf *v, const FmVizConf *saved) {
  bool has_saved = saved && saved->custom;
  if (v->custom || (v->style + 1 >= VIZ_STYLES && !has_saved)) { viz_preset(v, VIZ_BARS); return; }
  if (v->style + 1 >= VIZ_STYLES) {
    *v = *saved;
    v->custom = true;
    return;
  }
  viz_preset(v, v->style + 1);
}

/* ---- analysis ---------------------------------------------------------------- */

static void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * VIZ_PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr; im[a] += xi;
        float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

void viz_band_edges(int bands, bool log_scale, int rate, float *e) {
  if (rate <= 0) rate = 48000;
  float binhz = (float)rate / VIZ_FFT;
  float fmax = FM_MIN(16000.0f, rate * 0.5f * 0.94f);
  float b1 = FM_MIN(fmax / binhz, VIZ_FFT / 2 - 1.0f);
  float b0 = log_scale ? FM_MAX(30.0f / binhz, 0.5f) : 1.0f;
  for (int i = 0; i <= bands; i++) {
    float u = (float)i / (float)bands;
    e[i] = log_scale ? b0 * powf(b1 / b0, u) : b0 + (b1 - b0) * u;
  }
}

float viz_smooth(float level, float target, float attack, float decay, float dt) {
  float speed = target > level ? 6.0f + 54.0f * attack : 1.2f + 14.0f * decay;
  return level + (target - level) * (1.0f - expf(-speed * dt));
}

float viz_level(int b) { return b >= 0 && b < g_n ? g_lv[b] : 0; }
float viz_beat(void) { return g_beat; }

void viz_reset(void) {
  memset(g_lv, 0, sizeof g_lv);
  memset(g_pk, 0, sizeof g_pk);
  memset(g_pkv, 0, sizeof g_pkv);
  memset(g_hold, 0, sizeof g_hold);
  memset(g_wave, 0, sizeof g_wave);
  g_bass = g_bass_avg = g_beat = g_cool = 0;
  for (int i = 0; i < RINGS; i++) g_ring[i] = -1;
}

/* Linear interpolation of a per-band array at spectral position t (0..1). */
static float sample_at(const float *a, float t) {
  float x = t * g_n - 0.5f;
  if (x <= 0) return a[0];
  if (x >= g_n - 1) return a[g_n - 1];
  int i = (int)x;
  float f = x - (float)i;
  return a[i] + (a[i + 1] - a[i]) * f;
}

static float mag_at(float bin) {
  int i = (int)bin;
  if (i >= VIZ_FFT / 2 - 1) return g_mag[VIZ_FFT / 2 - 1];
  float f = bin - (float)i;
  return g_mag[i] + (g_mag[i + 1] - g_mag[i]) * f;
}

static void remap(int n, bool log_scale, int rate) {
  if (n != g_n) {
    /* keep the picture while the band count slider moves */
    float tmp[VIZ_MAX_BANDS], tp[VIZ_MAX_BANDS];
    for (int b = 0; b < n; b++) {
      float t = (b + 0.5f) / (float)n;
      tmp[b] = sample_at(g_lv, t);
      tp[b] = sample_at(g_pk, t);
    }
    memcpy(g_lv, tmp, sizeof(float) * (size_t)n);
    memcpy(g_pk, tp, sizeof(float) * (size_t)n);
    memset(g_pkv, 0, sizeof g_pkv);
    memset(g_hold, 0, sizeof g_hold);
    g_n = n;
  }
  if (n == g_map_n && log_scale == g_map_log && rate == g_map_rate) return;
  viz_band_edges(n, log_scale, rate, g_edge);
  float binhz = (float)(rate > 0 ? rate : 48000) / VIZ_FFT;
  g_nbass = 0;
  for (int b = 0; b < n; b++) {
    g_fc[b] = (g_edge[b] + g_edge[b + 1]) * 0.5f * binhz;
    if (g_fc[b] < 160.0f) g_nbass = b + 1;
  }
  if (g_nbass == 0) g_nbass = 1;
  g_map_n = n;
  g_map_log = log_scale;
  g_map_rate = rate;
}

static void beat_ring(void) {
  int oldest = 0;
  for (int i = 0; i < RINGS; i++) {
    if (g_ring[i] < 0) { oldest = i; break; }
    if (g_ring[i] > g_ring[oldest]) oldest = i;
  }
  g_ring[oldest] = 0;
}

bool viz_update(const FmVizConf *v, const float *s, int rate, float dt) {
  int n = FM_CLAMP(v->bands, VIZ_MIN_BANDS, VIZ_MAX_BANDS);
  remap(n, v->log_scale, rate);
  dt = FM_CLAMP(dt, 0.0f, 0.1f);
  float gain = FM_CLAMP(v->gain, 0.25f, 4.0f);
  float target[VIZ_MAX_BANDS];
  if (s) {
    if (!g_win_ok) {
      for (int i = 0; i < VIZ_FFT; i++) g_win[i] = 0.5f - 0.5f * cosf(2.0f * VIZ_PI * i / (VIZ_FFT - 1));
      g_win_ok = true;
    }
    for (int i = 0; i < VIZ_FFT; i++) { g_re[i] = s[i] * g_win[i]; g_im[i] = 0; }
    fft(g_re, g_im, VIZ_FFT);
    /* 4 / N: a full-scale sine reads 1.0 through the Hann window */
    for (int k = 0; k < VIZ_FFT / 2; k++)
      g_mag[k] = sqrtf(g_re[k] * g_re[k] + g_im[k] * g_im[k]) * (4.0f / VIZ_FFT);
    for (int b = 0; b < n; b++) {
      float lo = g_edge[b], hi = g_edge[b + 1], m = 0;
      if (hi - lo < 1.0f) m = mag_at((lo + hi) * 0.5f);
      else
        for (int k = (int)ceilf(lo); k <= (int)hi && k < VIZ_FFT / 2; k++) m = FM_MAX(m, g_mag[k]);
      /* music falls about 3 dB per octave (3 / ln 2 = 4.33): tilt so the treble shows too */
      float tilt = FM_CLAMP(4.328085f * logf(FM_MAX(g_fc[b], 20.0f) / 1000.0f), -9.0f, 9.0f);
      float db = 20.0f * log10f(m * gain + 1e-7f) + tilt;
      target[b] = FM_CLAMP((db + 70.0f) / 58.0f, 0.0f, 1.0f);
    }
  } else {
    for (int b = 0; b < n; b++) target[b] = 0;
  }
  bool moving = false;
  for (int b = 0; b < n; b++) {
    g_lv[b] = viz_smooth(g_lv[b], target[b], v->attack, v->decay, dt);
    if (g_lv[b] >= g_pk[b]) {
      g_pk[b] = g_lv[b];
      g_pkv[b] = 0;
      g_hold[b] = 0.45f;
    } else if (g_hold[b] > 0) {
      g_hold[b] -= dt;
    } else {
      g_pkv[b] += 2.4f * dt;
      g_pk[b] = FM_MAX(g_lv[b], g_pk[b] - g_pkv[b] * dt);
    }
    if (!s && g_lv[b] < 0.003f) g_lv[b] = 0;
    if (!s && g_pk[b] < 0.003f) g_pk[b] = 0;
    if (g_lv[b] > 0 || g_pk[b] > 0) moving = true;
  }

  /* waveform: start on a rising zero crossing so the line stands still */
  int span = VIZ_WAVE * 2;
  float wk = 1.0f - expf(-dt * (s ? 12.0f + 40.0f * v->attack : 1.2f + 14.0f * v->decay));
  if (s) {
    int start = VIZ_FFT - span;
    for (int i = 1; i < VIZ_FFT - span; i++)
      if (s[i - 1] <= 0 && s[i] > 0) { start = i; break; }
    for (int i = 0; i < VIZ_WAVE; i++) {
      float x = (s[start + i * 2] + s[start + i * 2 + 1]) * 0.5f * gain * 2.5f;
      g_wave[i] += (FM_CLAMP(x, -1.0f, 1.0f) - g_wave[i]) * wk;
    }
  } else {
    for (int i = 0; i < VIZ_WAVE; i++) {
      g_wave[i] -= g_wave[i] * wk;
      if (fabsf(g_wave[i]) < 0.002f) g_wave[i] = 0;
    }
  }
  for (int i = 0; i < VIZ_WAVE && !moving; i++)
    if (g_wave[i] != 0) moving = true;

  /* bass beat: a jump above the running average starts a ring */
  float bass = 0;
  for (int b = 0; b < g_nbass && b < n; b++) bass += target[b];
  bass /= (float)FM_MIN(g_nbass, n);
  g_bass += (bass - g_bass) * (1.0f - expf(-dt * (bass > g_bass ? 30.0f : 6.0f)));
  g_bass_avg += (g_bass - g_bass_avg) * (1.0f - expf(-dt * 1.5f));
  g_cool -= dt;
  if (s && g_bass > 0.35f && g_bass > g_bass_avg * 1.18f + 0.04f && g_cool <= 0) {
    g_beat = 1.0f;
    g_cool = 0.22f;
    beat_ring();
  }
  g_beat *= expf(-dt * 5.0f);
  if (g_beat < 0.003f) g_beat = 0;
  if (!s && g_bass < 0.003f) g_bass = g_bass_avg = 0;
  if (g_beat > 0 || g_bass > 0) moving = true;
  for (int i = 0; i < RINGS; i++) {
    if (g_ring[i] < 0) continue;
    g_ring[i] += dt;
    if (g_ring[i] > RING_LIFE) g_ring[i] = -1;
    else moving = true;
  }
  return moving;
}

/* ---- colour -------------------------------------------------------------------- */

static FmColor hsv(float h, float s, float v, float a) {
  h = (h - floorf(h)) * 6.0f;
  int i = (int)h;
  float f = h - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return FM_RGBA(r * 255.0f + 0.5f, g * 255.0f + 0.5f, b * 255.0f + 0.5f, FM_CLAMP(a, 0.0f, 1.0f) * 255.0f);
}

static void to_hsv(FmColor c, float *h, float *s, float *v) {
  float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
  float mx = FM_MAX(r, FM_MAX(g, b)), mn = FM_MIN(r, FM_MIN(g, b)), d = mx - mn;
  *v = mx;
  *s = mx > 0 ? d / mx : 0;
  if (d <= 0) { *h = 0; return; }
  if (mx == r) *h = (g - b) / d / 6.0f;
  else if (mx == g) *h = ((b - r) / d + 2.0f) / 6.0f;
  else *h = ((r - g) / d + 4.0f) / 6.0f;
  if (*h < 0) *h += 1.0f;
}

static FmColor color2(const FmVizConf *v) {
  if (v->color2 >= 0 && v->color2 < UI_ACCENTS) return kAccents[v->color2];
  float h, s, l;
  to_hsv(T.accent, &h, &s, &l);
  /* a grey accent (monochrome themes) gets a soft tint of its own */
  if (s < 0.2f) return T.dark ? FM_HEX(0xFF6B8A) : FM_HEX(0xD43C5E);
  return hsv(h + 1.0f / 6.0f, FM_MIN(1.0f, s * 1.05f), FM_MAX(l, T.dark ? 0.85f : 0.6f), 1.0f);
}

/* Colour at spectral position t (0 bass .. 1 treble) for intensity lv. */
static FmColor col_raw(const FmVizConf *v, float t, float lv) {
  lv = FM_CLAMP(lv, 0.0f, 1.0f);
  switch (v->color) {
    case VIZ_COL_GRADIENT: return col_mix(T.accent, color2(v), lv);
    case VIZ_COL_RAINBOW:
      return hsv(0.97f + t * 0.72f, T.dark ? 0.62f : 0.78f, T.dark ? 1.0f : 0.86f, 0.72f + 0.28f * lv);
    case VIZ_COL_HEAT:
      if (lv < 0.5f) return col_mix(col_alpha(T.accent, 0.5f), T.accent, lv * 2.0f);
      return col_mix(T.accent, color2(v), (lv - 0.5f) * 2.0f);
    default: return col_alpha(T.accent, 0.5f + 0.5f * lv);
  }
}

static FmColor fade(FmColor c) { return g_alpha < 1.0f ? col_alpha(c, g_alpha) : c; }
static FmColor col_at(const FmVizConf *v, float t, float lv) { return fade(col_raw(v, t, lv)); }

/* ---- mesh (per-vertex colour, seamless strips) --------------------------------------- */

static void mesh_flush(void) {
  if (g_nmi > 0) {
    gfx_flush();
    SDL_RenderGeometry(g_ren, NULL, g_mv, g_nmv, g_mi, g_nmi);
  }
  g_nmv = g_nmi = 0;
}

static int mesh_reserve(int nv, int ni) {
  if (g_nmv + nv > MESH_V || g_nmi + ni > MESH_I) mesh_flush();
  return g_nmv;
}

static void mvert(float x, float y, FmColor c) {
  SDL_Vertex *p = &g_mv[g_nmv++];
  p->position.x = x; p->position.y = y;
  p->color.r = c.r; p->color.g = c.g; p->color.b = c.b; p->color.a = c.a;
  p->tex_coord.x = 0; p->tex_coord.y = 0;
}

static void mtri(int a, int b, int c) { g_mi[g_nmi++] = a; g_mi[g_nmi++] = b; g_mi[g_nmi++] = c; }

static void mquad(int a, int b, int c, int d) { mtri(a, b, c); mtri(a, c, d); }

/* Unit normal of the curve at point i (averaged over both neighbours). */
static void normal_at(const float *x, const float *y, int n, int i, bool closed, float *nx, float *ny) {
  int a = i - 1, b = i + 1;
  if (closed) { a = (a + n) % n; b = b % n; }
  else { a = FM_MAX(a, 0); b = FM_MIN(b, n - 1); }
  float dx = x[b] - x[a], dy = y[b] - y[a];
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 1e-4f) { *nx = 0; *ny = -1; return; }
  *nx = -dy / l;
  *ny = dx / l;
}

/* Thick line through the points with per-point colours and a 1 px feather. */
static void strip(const float *x, const float *y, const FmColor *c, int n, float thick, float alpha, bool closed) {
  if (n < 2) return;
  float h = FM_MAX(thick * 0.5f - 0.5f, 0.25f), f = h + 1.0f;
  int segs = closed ? n : n - 1;
  float nx0, ny0;
  normal_at(x, y, n, 0, closed, &nx0, &ny0);
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    float nx1, ny1;
    normal_at(x, y, n, j, closed, &nx1, &ny1);
    int base = mesh_reserve(8, 18);
    FmColor ca = col_alpha(c[i], alpha), cb = col_alpha(c[j], alpha);
    FmColor za = col_alpha(ca, 0), zb = col_alpha(cb, 0);
    mvert(x[i] + nx0 * f, y[i] + ny0 * f, za);
    mvert(x[i] + nx0 * h, y[i] + ny0 * h, ca);
    mvert(x[i] - nx0 * h, y[i] - ny0 * h, ca);
    mvert(x[i] - nx0 * f, y[i] - ny0 * f, za);
    mvert(x[j] + nx1 * f, y[j] + ny1 * f, zb);
    mvert(x[j] + nx1 * h, y[j] + ny1 * h, cb);
    mvert(x[j] - nx1 * h, y[j] - ny1 * h, cb);
    mvert(x[j] - nx1 * f, y[j] - ny1 * f, zb);
    mquad(base, base + 4, base + 5, base + 1);
    mquad(base + 1, base + 5, base + 6, base + 2);
    mquad(base + 2, base + 6, base + 7, base + 3);
    nx0 = nx1;
    ny0 = ny1;
  }
  mesh_flush();
}

/* Area between the curve and y = base. The alpha is one linear ramp in y
** (a_top at base - height), so neighbouring columns of different heights
** meet without seams. */
static void fill_under(const float *x, const float *y, const FmColor *c, int n, float base, float height,
                       float a_top, float a_base) {
  float k = height > 0 ? (a_top - a_base) / height : 0;
  for (int i = 0; i + 1 < n; i++) {
    int b = mesh_reserve(4, 6);
    mvert(x[i], y[i], col_alpha(c[i], a_base + (base - y[i]) * k));
    mvert(x[i + 1], y[i + 1], col_alpha(c[i + 1], a_base + (base - y[i + 1]) * k));
    mvert(x[i + 1], base, col_alpha(c[i + 1], a_base));
    mvert(x[i], base, col_alpha(c[i], a_base));
    mquad(b, b + 1, b + 2, b + 3);
  }
  mesh_flush();
}

/* Closed star-shaped outline filled from its centre. */
static void fan(float cx, float cy, const float *x, const float *y, const FmColor *c, int n, float alpha) {
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    int b = mesh_reserve(3, 3);
    mvert(cx, cy, col_alpha(c[i], alpha * 0.4f));
    mvert(x[i], y[i], col_alpha(c[i], alpha));
    mvert(x[j], y[j], col_alpha(c[j], alpha));
    mtri(b, b + 1, b + 2);
  }
  mesh_flush();
}

/* ---- layout helpers ---------------------------------------------------------- */

static int cols_for(const FmVizConf *v, float w, float min_pitch) {
  int maxc = (int)(w / FM_MAX(min_pitch, 1.0f));
  return FM_CLAMP(v->bands, 4, FM_MAX(4, maxc));
}

/* Spectral position of column c; mirrored puts the bass in the middle. */
static float col_t(int c, int cols, bool mirror) {
  float u = (c + 0.5f) / (float)cols;
  if (!mirror) return u;
  return FM_CLAMP(fabsf(u * 2.0f - 1.0f), 0.0f, 1.0f);
}

/* Spectrum curve through the column tops, Catmull-Rom smoothed into g_c*. */
static int curve(const FmVizConf *v, FmRect r, float base, float height, bool peaks) {
  int cols = cols_for(v, r.w, DP(4));
  float vx[VIZ_MAX_BANDS + 2], vy[VIZ_MAX_BANDS + 2], vt[VIZ_MAX_BANDS + 2];
  int nv = 0;
  for (int c = -1; c <= cols; c++) {
    int cc = FM_CLAMP(c, 0, cols - 1);
    float t = col_t(cc, cols, v->mirror);
    float lv = sample_at(peaks ? g_pk : g_lv, t);
    vx[nv] = c < 0 ? r.x : c >= cols ? r.x + r.w : r.x + (c + 0.5f) * r.w / cols;
    vy[nv] = base - height * lv;
    vt[nv] = t;
    nv++;
  }
  int sub = FM_CLAMP((CURVE_MAX - 2) / nv, 1, 6);
  int n = 0;
  for (int i = 0; i + 1 < nv; i++) {
    float x0 = vx[FM_MAX(i - 1, 0)], y0 = vy[FM_MAX(i - 1, 0)];
    float x3 = vx[FM_MIN(i + 2, nv - 1)], y3 = vy[FM_MIN(i + 2, nv - 1)];
    for (int k = 0; k < sub && n < CURVE_MAX - 1; k++) {
      float u = (float)k / (float)sub, u2 = u * u, u3 = u2 * u;
      float a = -0.5f * u3 + u2 - 0.5f * u, b = 1.5f * u3 - 2.5f * u2 + 1.0f;
      float c = -1.5f * u3 + 2.0f * u2 + 0.5f * u, d = 0.5f * u3 - 0.5f * u2;
      g_cx[n] = a * x0 + b * vx[i] + c * vx[i + 1] + d * x3;
      g_cy[n] = FM_CLAMP(a * y0 + b * vy[i] + c * vy[i + 1] + d * y3, base - height, base);
      g_ct[n] = vt[i] + (vt[i + 1] - vt[i]) * u;
      n++;
    }
  }
  g_cx[n] = vx[nv - 1];
  g_cy[n] = vy[nv - 1];
  g_ct[n] = vt[nv - 1];
  n++;
  for (int i = 0; i < n; i++) g_cc[i] = col_at(v, g_ct[i], height > 0 ? (base - g_cy[i]) / height : 0);
  return n;
}

/* ---- styles -------------------------------------------------------------------------- */

/* Rounded bar; the radius stays just under half the short side, since
** coinciding corner points leave seams in the feathered fill. */
static void bar(FmRect r, float rad, FmColor top, FmColor bot) {
  rad = FM_MIN(rad, FM_MIN(r.w, r.h) * 0.5f - 0.6f);
  gfx_rrect_vgrad(r, FM_MAX(rad, 0.0f), top, bot);
}

static void cap_at(FmRect r, float rad, FmColor c) {
  if (rad >= FM_MIN(r.w, r.h) * 0.5f - 0.6f && fabsf(r.w - r.h) < 0.5f) {
    gfx_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w * 0.5f, c);
    return;
  }
  rad = FM_MIN(rad, FM_MIN(r.w, r.h) * 0.5f - 0.6f);
  gfx_rrect(r, FM_MAX(rad, 0.0f), c);
}

static void draw_bars(const FmVizConf *v, FmRect r) {
  bool pills = v->style == VIZ_PILLS, mirror2 = v->style == VIZ_MIRROR;
  int cols = cols_for(v, r.w, DP(3));
  float pitch = r.w / cols;
  float bw = FM_MAX(1.0f, pitch * v->width);
  float rad = v->round * bw * 0.5f;
  float cap = pills ? FM_MAX(DP(2.5f), bw * 0.42f) : DP(2.5f);
  float bottom = r.y + r.h;
  float room = r.h - (v->peaks ? cap + DP(3) : 0);
  float cy = r.y + r.h * 0.5f, gap = DP(2);
  for (int c = 0; c < cols; c++) {
    float t = col_t(c, cols, v->mirror);
    float lv = sample_at(g_lv, t);
    float x = r.x + c * pitch + (pitch - bw) * 0.5f;
    FmColor top = col_at(v, t, lv), bot = col_at(v, t, 0);
    if (v->color == VIZ_COL_HEAT || v->color == VIZ_COL_RAINBOW) bot = col_alpha(top, 0.8f);
    if (mirror2) {
      float half = (r.h - gap) * 0.5f - (v->peaks ? cap + DP(2) : 0);
      float hh = FM_MAX(FM_MIN(bw, DP(3)), half * lv);
      bar(FM_RECT(x, cy - gap * 0.5f - hh, bw, hh), rad, top, bot);
      bar(FM_RECT(x, cy + gap * 0.5f, bw, hh), rad, col_alpha(bot, 0.55f), col_alpha(top, 0.3f));
      if (v->peaks) {
        float ph = half * sample_at(g_pk, t) + DP(2);
        cap_at(FM_RECT(x, cy - gap * 0.5f - ph - cap, bw, cap), rad, top);
        cap_at(FM_RECT(x, cy + gap * 0.5f + ph, bw, cap), rad, col_alpha(top, 0.4f));
      }
      continue;
    }
    float minh = pills ? FM_MIN(bw, room) : DP(2);
    float h = FM_MAX(minh, room * lv);
    bar(FM_RECT(x, bottom - h, bw, h), rad, top, bot);
    if (v->peaks) {
      float pk = sample_at(g_pk, t);
      float y = bottom - FM_MAX(minh, room * pk) - DP(3) - cap;
      FmColor pc = pills ? col_mix(top, T.text, 0.25f) : top;
      cap_at(FM_RECT(x, FM_MAX(r.y, y), bw, cap), rad, col_alpha(pc, 0.6f + 0.4f * pk));
    }
  }
}

static void draw_dots(const FmVizConf *v, FmRect r) {
  int cols = cols_for(v, r.w, DP(5));
  float pitch = r.w / cols;
  int rows = FM_CLAMP((int)(r.h / pitch), 4, 32);
  rows = FM_MIN(rows, FM_MAX(4, 1400 / cols));
  float ch = r.h / rows;
  float d = FM_MAX(DP(1.5f), FM_MIN(pitch, ch) * v->width);
  float rad = v->round * d * 0.5f;
  FmColor off = fade(col_alpha(T.text, T.dark ? 0.07f : 0.08f));
  for (int c = 0; c < cols; c++) {
    float t = col_t(c, cols, v->mirror);
    float lv = sample_at(g_lv, t), pk = sample_at(g_pk, t);
    int lit = (int)(lv * rows + 0.35f), prow = (int)(pk * rows - 0.01f);
    float x = r.x + c * pitch + (pitch - d) * 0.5f;
    for (int k = 0; k < rows; k++) {
      float y = r.y + r.h - (k + 1) * ch + (ch - d) * 0.5f;
      FmColor col = off;
      float hk = (k + 0.5f) / rows;
      if (k < lit) col = col_at(v, t, v->color == VIZ_COL_ACCENT ? lv : hk);
      else if (v->peaks && k == prow && pk > 0.02f) col = col_at(v, t, 1.0f);
      cap_at(FM_RECT(x, y, d, d), rad, col);
    }
  }
}

static void draw_wave(const FmVizConf *v, FmRect r) {
  float base = r.y + r.h, height = r.h - DP(3);
  if (v->peaks) {
    int np = curve(v, r, base, height, true);
    strip(g_cx, g_cy, g_cc, np, DP(1.2f), 0.45f, false);
  }
  int n = curve(v, r, base, height, false);
  fill_under(g_cx, g_cy, g_cc, n, base, height, T.dark ? 0.75f : 0.6f, 0.04f);
  strip(g_cx, g_cy, g_cc, n, DP(2.2f), 1.0f, false);
}

static void draw_neon(const FmVizConf *v, FmRect r) {
  float base = v->mirror ? r.y + r.h * 0.64f : r.y + r.h - DP(4);
  float height = (v->mirror ? r.h * 0.6f : r.h - DP(8));
  int n = curve(v, r, base, height, v->peaks);
  strip(g_cx, g_cy, g_cc, n, DP(10), 0.1f, false);
  strip(g_cx, g_cy, g_cc, n, DP(5), 0.22f, false);
  if (v->peaks) {
    n = curve(v, r, base, height, false);
    strip(g_cx, g_cy, g_cc, n, DP(5), 0.15f, false);
  }
  for (int i = 0; i < n; i++)
    g_cc[i] = T.dark ? col_mix(g_cc[i], FM_HEX(0xFFFFFF), 0.35f) : g_cc[i];
  strip(g_cx, g_cy, g_cc, n, DP(1.8f), 1.0f, false);
  if (v->mirror) {
    float k = (r.y + r.h - base - DP(2)) / FM_MAX(height, 1.0f);
    for (int i = 0; i < n; i++) g_cy[i] = base + (base - g_cy[i]) * k;
    strip(g_cx, g_cy, g_cc, n, DP(4), 0.12f, false);
    strip(g_cx, g_cy, g_cc, n, DP(1.4f), 0.3f, false);
  }
}

static void draw_scope(const FmVizConf *v, FmRect r) {
  float cy = r.y + r.h * 0.5f, amp = r.h * 0.46f;
  gfx_rect(FM_RECT(r.x, floorf(cy), r.w, FM_MAX(1.0f, floorf(DP(1)))), fade(col_alpha(T.text, 0.08f)));
  int n = FM_MIN(VIZ_WAVE, FM_MAX(16, (int)(r.w / DP(1.5f))));
  for (int i = 0; i < n; i++) {
    float w = g_wave[i * VIZ_WAVE / n];
    g_cx[i] = r.x + r.w * i / (float)(n - 1);
    g_cy[i] = cy - w * amp;
    g_cc[i] = col_at(v, (float)i / (n - 1), FM_MIN(1.0f, fabsf(w) * 2.0f + 0.4f));
  }
  if (v->mirror) {
    for (int i = 0; i < n; i++) g_cy[i] = cy + (cy - g_cy[i]) * 0.7f;
    strip(g_cx, g_cy, g_cc, n, DP(1.5f), 0.28f, false);
    for (int i = 0; i < n; i++) g_cy[i] = cy - (g_cy[i] - cy) / 0.7f;
  }
  strip(g_cx, g_cy, g_cc, n, DP(7), 0.14f, false);
  strip(g_cx, g_cy, g_cc, n, DP(2.2f), 1.0f, false);
}

/* Ring geometry: centre, inner radius and bar room. */
static void ring_geom(FmRect r, float hole, float *cx, float *cy, float *r0, float *len) {
  float R = FM_MIN(r.w, r.h) * 0.5f;
  *cx = r.x + r.w * 0.5f;
  *cy = r.y + r.h * 0.5f;
  *r0 = hole > 0 ? hole + DP(6) : R * 0.46f;     /* hole < 0: an empty centre */
  *len = FM_MAX(R - *r0 - DP(2), DP(4));
}

static void centre_disc(float cx, float cy, float r0, float scale) {
  float d = (r0 - DP(5)) * scale;
  gfx_circle(cx, cy, d, fade(T.surface2));
  float is = d * 0.9f;
  if (is > DP(10)) icon_draw(IC_MUSIC, FM_RECT(cx - is * 0.5f, cy - is * 0.5f, is, is), fade(T.text3));
}

static void draw_radial(const FmVizConf *v, FmRect r, float hole) {
  float cx, cy, r0, len;
  ring_geom(r, hole, &cx, &cy, &r0, &len);
  int maxc = (int)(2.0f * VIZ_PI * r0 / DP(3));
  int cols = FM_CLAMP(v->bands, 8, FM_MAX(8, maxc));
  float th = FM_MAX(DP(1.2f), 2.0f * VIZ_PI * r0 / cols * v->width);
  gfx_ring(cx, cy, r0 - DP(2), DP(1), fade(col_alpha(T.text, 0.1f)));
  if (hole == 0) centre_disc(cx, cy, r0, 1.0f);
  for (int c = 0; c < cols; c++) {
    float u = (c + 0.5f) / (float)cols;
    float t = v->mirror ? 1.0f - fabsf(u * 2.0f - 1.0f) : u;   /* bass on top */
    float lv = sample_at(g_lv, t);
    float a = -VIZ_PI * 0.5f + u * 2.0f * VIZ_PI;
    float ca = cosf(a), sa = sinf(a);
    float l = FM_MAX(DP(2), len * lv * (v->peaks ? 0.9f : 1.0f));
    float x0 = cx + ca * r0, y0 = cy + sa * r0, x1 = cx + ca * (r0 + l), y1 = cy + sa * (r0 + l);
    FmColor col = col_at(v, t, lv);
    gfx_line(x0, y0, x1, y1, th, col);
    if (v->round > 0.5f && th >= DP(2)) {
      gfx_circle(x0, y0, th * 0.5f, col);
      gfx_circle(x1, y1, th * 0.5f, col);
    }
    if (v->peaks) {
      float pr = r0 + FM_MAX(DP(2), len * 0.9f * sample_at(g_pk, t)) + th + DP(2);
      gfx_circle(cx + ca * pr, cy + sa * pr, FM_MAX(th * 0.5f, DP(1.2f)), col_alpha(col, 0.85f));
    }
  }
}

static void draw_pulse(const FmVizConf *v, FmRect r, float hole) {
  float cx, cy, r0, len;
  ring_geom(r, hole, &cx, &cy, &r0, &len);
  FmColor c2 = v->color == VIZ_COL_ACCENT ? T.accent : color2(v);
  /* beat rings travel outwards and fade */
  for (int i = 0; i < RINGS; i++) {
    if (g_ring[i] < 0) continue;
    float e = ui_ease_out(g_ring[i] / RING_LIFE);
    float rad = r0 + (len + DP(4)) * e;
    gfx_ring(cx, cy, rad, DP(1) + DP(2.5f) * (1.0f - e), fade(col_alpha(c2, 0.75f * (1.0f - e))));
  }
  /* bass glow */
  gfx_circle(cx, cy, r0 + len * 0.35f * g_bass, fade(col_alpha(T.accent, 0.12f + 0.18f * g_beat)));
  /* spectrum blob */
  int n = FM_MIN(CURVE_MAX, 128);
  for (int i = 0; i < n; i++) {
    float u = (float)i / (float)n;
    float t = v->mirror ? 1.0f - fabsf(u * 2.0f - 1.0f) : u;
    float lv = sample_at(v->peaks ? g_pk : g_lv, t);
    float a = -VIZ_PI * 0.5f + u * 2.0f * VIZ_PI;
    float rad = r0 + DP(2) + len * 0.82f * lv;
    g_cx[i] = cx + cosf(a) * rad;
    g_cy[i] = cy + sinf(a) * rad;
    g_cc[i] = col_at(v, t, lv);
  }
  fan(cx, cy, g_cx, g_cy, g_cc, n, T.dark ? 0.4f : 0.32f);
  strip(g_cx, g_cy, g_cc, n, DP(2), 1.0f, true);
  if (hole == 0) centre_disc(cx, cy, r0, 1.0f + 0.06f * g_beat);
}

void viz_draw(const FmVizConf *v, FmRect r, float hole) {
  if (r.w < 4 || r.h < 4) return;
  switch (v->style) {
    case VIZ_BARS: case VIZ_MIRROR: case VIZ_PILLS: draw_bars(v, r); break;
    case VIZ_WAVEFILL: draw_wave(v, r); break;
    case VIZ_SCOPE: draw_scope(v, r); break;
    case VIZ_RADIAL: draw_radial(v, r, hole); break;
    case VIZ_PULSE: draw_pulse(v, r, hole); break;
    case VIZ_DOTS: draw_dots(v, r); break;
    case VIZ_NEON: draw_neon(v, r); break;
    default: break;
  }
}

void viz_curve(const float *x, const float *y, int n, float base, FmColor line, FmColor fill, float thick) {
  n = FM_MIN(n, CURVE_MAX);
  if (n < 2) return;
  for (int i = 0; i < n; i++) g_cc[i] = fill;
  /* one alpha everywhere, so columns of different heights leave no seams */
  if (fill.a > 0) fill_under(x, y, g_cc, n, base, 0, 1.0f, 1.0f);
  for (int i = 0; i < n; i++) g_cc[i] = line;
  strip(x, y, g_cc, n, thick, 1.0f, false);
}

void viz_draw_media(const FmVizConf *v, FmRect r, float hole, float alpha) {
  FmTheme keep = T;
  T.dark = true;
  T.text = FM_HEX(0xFFFFFF);
  T.text3 = FM_RGBA(255, 255, 255, 110);
  T.surface2 = FM_RGBA(255, 255, 255, 30);
  g_alpha = FM_CLAMP(alpha, 0.0f, 1.0f);
  viz_draw(v, r, hole);
  g_alpha = 1.0f;
  T = keep;
}

/* ---- settings panel ----------------------------------------------------------------- */

static FmRect g_body;     /* content column of the panel */
static float g_y;         /* next row, in screen px */

static FmRect next_row(float h) {
  FmRect r = { g_body.x, g_y, g_body.w, h };
  g_y += h;
  return r;
}

static void section(const char *title) {
  g_y += DP(6);
  FmRect r = next_row(DP(26));
  ui_label(r, title, FONT_BOLD, ui.m.font_small, T.text3, UI_LEFT);
}

static void touched(void) {
  conf.viz.custom = true;
  conf.viz_saved = conf.viz;
}

static float row_h(void) { return DP(ui.touch_mode ? 50 : 38); }

/* Label, slider, value text. */
static bool knob(const char *name, u32 id, float *v, float lo, float hi, const char *val) {
  FmRect r = next_row(row_h());
  if (!gfx_visible(r)) return false;
  FmRect lab = rect_cut_left(&r, FM_MIN(DP(118), r.w * 0.34f));
  FmRect vr = rect_cut_right(&r, DP(50));
  ui_label(lab, name, FONT_REGULAR, ui.m.font, T.text, UI_LEFT);
  ui_label(vr, val, FONT_REGULAR, ui.m.font_small, T.text2, UI_RIGHT);
  float sh = DP(ui.touch_mode ? 40 : 28);
  return ui_slider(id, rect_inset2(r, DP(4), FM_MAX(0.0f, (r.h - sh) * 0.5f)), v, lo, hi);
}

static bool toggle(const char *name, u32 id, bool *v) {
  FmRect r = next_row(row_h());
  if (!gfx_visible(r)) return false;
  return ui_switch(id, r, name, v);
}

static void preset_chip(FmRect chip, const FmVizConf *pv, const char *name, bool on, bool *clicked) {
  *clicked = false;
  if (!gfx_visible(chip)) return;
  u32 id = ui_idn(ui_id("viz.chip"), (u32)(pv->custom ? 99 : pv->style));
  int f = ui_hit(id, chip);
  float rad = DP(12);
  gfx_rrect(chip, rad, on ? T.accent_soft : T.surface2);
  if (on) gfx_rrect_line(chip, rad, DP(2), T.accent);
  else if (f & UI_HOVER) gfx_rrect(chip, rad, T.hover);
  if (f & UI_HELD) gfx_rrect(chip, rad, T.press);
  float lh = font_line_h(ui.m.font_small);
  FmRect lab = { chip.x, chip.y + chip.h - lh - DP(6), chip.w, lh };
  FmRect pr = rect_inset(FM_RECT(chip.x, chip.y, chip.w, chip.h - lh - DP(6)), DP(8));
  pr.h += DP(2);
  if (pv->style == VIZ_OFF) {
    float is = FM_MIN(pr.w, pr.h) * 0.55f;
    icon_draw(IC_EYE_OFF, rect_center(pr, is, is), T.text3);
  } else {
    gfx_clip_push(rect_inset(chip, DP(2)));
    viz_draw(pv, pr, 0);
    gfx_clip_pop();
  }
  ui_label(lab, name, on ? FONT_BOLD : FONT_REGULAR, ui.m.font_small, on ? T.accent : T.text2, UI_CENTER);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  *clicked = (f & UI_CLICK) != 0;
}

static void chips(void) {
  float gap = DP(8);
  float want = DP(ui.touch_mode ? 104 : 92);
  int per = FM_MAX(2, (int)((g_body.w + gap) / (want + gap)));
  float cw = (g_body.w - gap * (per - 1)) / per;
  float chh = cw * 0.56f + font_line_h(ui.m.font_small) + DP(8);
  int count = VIZ_STYLES + (conf.viz_saved.custom ? 1 : 0);
  for (int i = 0; i < count; i++) {
    int col = i % per;
    if (col == 0 && i > 0) g_y += chh + gap;
    FmRect chip = { g_body.x + col * (cw + gap), g_y, cw, chh };
    FmVizConf pv;
    const char *name;
    bool on;
    if (i < VIZ_STYLES) {
      viz_preset(&pv, i);
      pv.bands = FM_CLAMP(pv.bands / 2, VIZ_MIN_BANDS, 32);
      name = kStyleNames[i];
      on = !conf.viz.custom && conf.viz.style == i;
    } else {
      pv = conf.viz_saved;
      pv.bands = FM_MIN(pv.bands, 32);
      name = "Custom";
      on = conf.viz.custom;
    }
    bool clicked;
    preset_chip(chip, &pv, name, on, &clicked);
    if (!clicked) continue;
    if (i < VIZ_STYLES) viz_preset(&conf.viz, i);
    else { conf.viz = conf.viz_saved; conf.viz.custom = true; }
  }
  g_y += chh;
}

static void swatches(void) {
  FmRect r = next_row(FM_MAX(ui.m.hit, DP(40)));
  if (!gfx_visible(r)) return;
  FmRect lab = rect_cut_left(&r, FM_MIN(DP(118), r.w * 0.34f));
  ui_label(lab, "Second colour", FONT_REGULAR, ui.m.font, T.text, UI_LEFT);
  float s = FM_MIN(r.w / (UI_ACCENTS + 1), r.h);
  for (int i = -1; i < UI_ACCENTS; i++) {
    FmRect b = { r.x + (i + 1) * s, r.y + (r.h - s) * 0.5f, s, s };
    int f = ui_hit(ui_idn(ui_id("viz.sw"), (u32)(i + 1)), b);
    float cx = b.x + s * 0.5f, cy = b.y + s * 0.5f, d = FM_MIN(s * 0.36f, DP(12));
    bool on = conf.viz.color2 == i;
    if (i < 0) {
      FmVizConf tmp = conf.viz;
      tmp.color2 = -1;
      gfx_circle(cx, cy, d, color2(&tmp));
      gfx_ring(cx, cy, d, DP(1.5f), T.accent);
    } else {
      gfx_circle(cx, cy, d, kAccents[i]);
    }
    if (on) gfx_ring(cx, cy, d + DP(4), DP(2), T.text);
    else if (f & UI_HOVER) gfx_ring(cx, cy, d + DP(4), DP(1.5f), T.text3);
    if (f & UI_CLICK) { conf.viz.color2 = i; touched(); }
  }
}

static int g_tab;   /* 0 visualizer, 1 equalizer */

void viz_panel_tab(int tab) { g_tab = tab ? 1 : 0; }

bool viz_panel(FmRect r) {
  bool open = true;
  gfx_rrect(r, ui.m.radius, T.surface);
  FmRect c = rect_inset2(r, DP(ui.touch_mode ? 14 : 16), DP(8));
  float hh = FM_MAX(ui.m.hit, DP(40));
  FmRect hdr = rect_cut_top(&c, hh);
  if (ui_icon_btn(ui_id("viz.close"), rect_cut_right(&hdr, hh), IC_CLOSE, T.text2, "Close")) open = false;
  const char *rl = "Reset";
  float rw = font_width(FONT_BOLD, ui.m.font, rl, -1) + DP(44);
  FmRect rb = rect_cut_right(&hdr, rw);
  if (ui_button(ui_id("viz.reset"), rect_center(rb, rw, FM_MIN(rb.h, DP(ui.touch_mode ? 44 : 34))), IC_REFRESH,
                rl, UI_BTN_TEXT)) {
    if (g_tab == 1) {
      FmEqConf keep = conf.eq;
      eq_defaults(&conf.eq);
      memcpy(conf.eq.custom, keep.custom, sizeof conf.eq.custom);
      conf.eq.on = keep.on;
      ui_toast("Equalizer: flat");
    } else {
      viz_preset(&conf.viz, conf.viz.style);
      ui_toast("%s: preset values", kStyleNames[conf.viz.style]);
    }
  }
  static const char *const kTabs[] = { "Visualizer", "Equalizer" };
  FmRect tr = rect_center(hdr, FM_MIN(hdr.w - DP(8), DP(270)), FM_MIN(hdr.h, DP(ui.touch_mode ? 42 : 34)));
  tr.x = hdr.x;
  ui_segmented(ui_id("viz.tab"), tr, kTabs, 2, &g_tab);
  rect_cut_top(&c, DP(6));
  if (g_tab == 1) {
    eq_ui(c);
    return open;
  }

  u32 sid = ui_id("viz.scroll");
  ui_scroll(&g_ps, sid, c, g_panel_h);
  gfx_clip_push(c);
  g_body = rect_inset2(c, DP(2), 0);
  g_body.w -= DP(6);
  g_y = c.y - g_ps.y + DP(4);
  float y0 = g_y;

  {
    char title[64];
    fm_snprintf(title, sizeof title, "STYLE \xC2\xB7 %s%s", kStyleNames[conf.viz.style],
                conf.viz.custom ? " (CUSTOM)" : "");
    g_y -= DP(6);
    section(title);
  }
  chips();
  FmVizConf *v = &conf.viz;
  char val[32];
  section("SHAPE");
  float fb = (float)v->bands;
  fm_snprintf(val, sizeof val, "%d", v->bands);
  if (knob("Bands", ui_id("viz.bands"), &fb, VIZ_MIN_BANDS, VIZ_MAX_BANDS, val)) {
    v->bands = FM_CLAMP(((int)(fb + 2.0f) / 4) * 4, VIZ_MIN_BANDS, VIZ_MAX_BANDS);
    touched();
  }
  fm_snprintf(val, sizeof val, "%d%%", (int)(v->width * 100.0f + 0.5f));
  if (knob("Bar width", ui_id("viz.width"), &v->width, 0.15f, 1.0f, val)) touched();
  fm_snprintf(val, sizeof val, "%d%%", (int)(v->round * 100.0f + 0.5f));
  if (knob("Roundness", ui_id("viz.round"), &v->round, 0.0f, 1.0f, val)) touched();
  if (toggle("Mirror", ui_id("viz.mirror"), &v->mirror)) touched();
  if (toggle("Peak hold", ui_id("viz.peaks"), &v->peaks)) touched();
  if (toggle("Log frequency scale", ui_id("viz.log"), &v->log_scale)) touched();

  section("MOTION");
  float lg = logf(v->gain) * 1.442695f;   /* log2, which tcc lacks */
  fm_snprintf(val, sizeof val, "%.1fx", (double)v->gain);
  if (knob("Sensitivity", ui_id("viz.gain"), &lg, -2.0f, 2.0f, val)) {
    v->gain = FM_CLAMP(powf(2.0f, lg), 0.25f, 4.0f);
    touched();
  }
  fm_snprintf(val, sizeof val, "%d%%", (int)(v->attack * 100.0f + 0.5f));
  if (knob("Rise speed", ui_id("viz.attack"), &v->attack, 0.0f, 1.0f, val)) touched();
  fm_snprintf(val, sizeof val, "%d%%", (int)(v->decay * 100.0f + 0.5f));
  if (knob("Fall speed", ui_id("viz.decay"), &v->decay, 0.0f, 1.0f, val)) touched();

  section("COLOUR");
  {
    static const char *const kCols[] = { "Accent", "Gradient", "Rainbow", "Heat" };
    FmRect sr = next_row(DP(ui.touch_mode ? 46 : 36));
    if (gfx_visible(sr) && ui_segmented(ui_id("viz.color"), rect_inset2(sr, 0, DP(2)), kCols, 4, &v->color))
      touched();
  }
  g_y += DP(6);
  if (v->color == VIZ_COL_GRADIENT || v->color == VIZ_COL_HEAT || v->style == VIZ_PULSE) swatches();
  g_y += DP(8);

  g_panel_h = g_y - y0 + DP(8);
  gfx_clip_pop();
  ui_scrollbar(&g_ps, c, g_panel_h);
  viz_sanitize(&conf.viz);
  return open;
}
