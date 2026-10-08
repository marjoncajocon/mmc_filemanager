/* feq.c -- 10-band equalizer: presets, RBJ biquads, width, balance, limiter, UI.
**
** Design decisions:
**   - It runs in the audio callbacks, after the ring (music) or the
**     converter (video), so a slider move is heard at once instead of
**     after the 0.75 s the decoder runs ahead. Ten biquads on two channels
**     cost about 50 flops per frame, far below a millisecond per callback.
**   - Filters are RBJ cookbook biquads in transposed direct form II, float:
**     a low shelf at 31 Hz, a high shelf at 16 kHz and peaking filters
**     (one octave, Q 1.41) in between. The coefficients are designed on
**     the main thread only when the settings or the rate change, then
**     copied in under the player's lock; nothing is allocated or designed
**     in the callback. Bands at 0 dB are skipped, and a flat, centred,
**     full-width setting (or "off") bypasses everything.
**   - Boosts can clip. Presets carry a negative preamp, and a limiter
**     follows: the gain drops at once when a peak would pass 0.9 and
**     recovers over about 80 ms, and a soft knee above 0.85 keeps every
**     sample strictly inside -1..1 whatever comes in.
**   - The settings UI draws the true response of the designed filters (not
**     straight lines between the sliders) behind ten vertical sliders.
**     Moving any slider makes the curve the user's Custom one.
*/
#include "feq.h"
#include "fviz.h"

#define EQ_PI 3.14159265f
#define EQ_LIMIT 0.9f
#define EQ_KNEE 0.85f

const float kEqFreq[EQ_BANDS] = { 31.25f, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

static const char *const kEqNames[EQ_PRESETS] = {
  "Flat", "Bass boost", "Full bass", "Bass & treble", "Treble boost", "Rock", "Pop", "Jazz",
  "Classical", "Dance", "Hip-hop", "Vocal", "Loudness", "Laptop", "Headphones", "Custom"
};

/* dB per band (31 Hz .. 16 kHz), then the preamp. */
static const signed char kEqCurve[EQ_PRESETS - 1][EQ_BANDS + 1] = {
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },          /* flat */
  { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0, -2 },         /* bass boost */
  { 8, 8, 7, 5, 2, 0, -1, -2, -2, -2, -4 },     /* full bass */
  { 7, 6, 4, 1, -2, -2, 0, 3, 6, 7, -4 },       /* bass & treble */
  { 0, 0, 0, 0, 0, 1, 3, 5, 6, 7, -3 },         /* treble boost */
  { 5, 4, 3, 1, -1, -1, 1, 3, 4, 5, -3 },       /* rock */
  { -1, 0, 2, 4, 5, 4, 2, 0, -1, -1, -3 },      /* pop */
  { 3, 2, 1, 2, -1, -1, 0, 1, 2, 3, -2 },       /* jazz */
  { 4, 3, 2, 1, -1, -1, 0, 2, 3, 4, -2 },       /* classical */
  { 7, 6, 3, 0, 0, -1, 0, 2, 4, 5, -4 },        /* dance / electronic */
  { 7, 6, 4, 1, -1, 0, 1, 0, 2, 3, -4 },        /* hip-hop */
  { -3, -3, -2, 0, 2, 4, 4, 3, 1, -1, -2 },     /* vocal / speech */
  { 6, 4, 1, 0, -1, 0, 0, 1, 4, 6, -3 },        /* loudness */
  { -4, -2, 2, 4, 3, 1, 1, 2, 3, 2, -2 },       /* small speakers: no deep bass, fuller mids */
  { 3, 2, 0, 0, 0, -1, 1, 2, 1, 0, -1 },        /* headphones */
};

/* ---- settings ------------------------------------------------------------------- */

const char *eq_preset_name(int p) { return p >= 0 && p < EQ_PRESETS ? kEqNames[p] : "?"; }

void eq_preset(FmEqConf *c, int p) {
  p = FM_CLAMP(p, 0, EQ_PRESETS - 1);
  c->preset = p;
  if (p == EQ_CUSTOM) {
    memcpy(c->band, c->custom, sizeof c->band);
    return;
  }
  for (int i = 0; i < EQ_BANDS; i++) c->band[i] = kEqCurve[p][i];
  c->preamp = kEqCurve[p][EQ_BANDS];
}

void eq_defaults(FmEqConf *c) {
  memset(c, 0, sizeof *c);
  c->width = 1.0f;
  eq_preset(c, EQ_FLAT);
}

void eq_sanitize(FmEqConf *c) {
  c->preset = FM_CLAMP(c->preset, 0, EQ_PRESETS - 1);
  for (int i = 0; i < EQ_BANDS; i++) {
    if (!(c->band[i] == c->band[i])) c->band[i] = 0;
    if (!(c->custom[i] == c->custom[i])) c->custom[i] = 0;
    c->band[i] = FM_CLAMP(c->band[i], -EQ_MAX_DB, EQ_MAX_DB);
    c->custom[i] = FM_CLAMP(c->custom[i], -EQ_MAX_DB, EQ_MAX_DB);
  }
  if (!(c->preamp == c->preamp)) c->preamp = 0;
  if (!(c->width == c->width)) c->width = 1;
  if (!(c->balance == c->balance)) c->balance = 0;
  c->preamp = FM_CLAMP(c->preamp, -EQ_MAX_DB, EQ_MAX_DB);
  c->width = FM_CLAMP(c->width, 0.0f, 2.0f);
  c->balance = FM_CLAMP(c->balance, -1.0f, 1.0f);
}

bool eq_is_flat(const FmEqConf *c) {
  for (int i = 0; i < EQ_BANDS; i++)
    if (fabsf(c->band[i]) >= 0.05f) return false;
  return fabsf(c->preamp) < 0.05f && fabsf(c->width - 1.0f) < 0.005f && fabsf(c->balance) < 0.005f;
}

static bool same_conf(const FmEqConf *a, const FmEqConf *b) {
  if (a->on != b->on || a->preamp != b->preamp || a->width != b->width || a->balance != b->balance) return false;
  for (int i = 0; i < EQ_BANDS; i++)
    if (a->band[i] != b->band[i]) return false;
  return true;
}

/* ---- design ---------------------------------------------------------------------- */

/* RBJ cookbook; kind 0 peaking, 1 low shelf, 2 high shelf. */
static void biquad(int kind, float f0, float db, int rate, float *k) {
  float A = powf(10.0f, db / 40.0f);
  float w0 = 2.0f * EQ_PI * f0 / (float)rate;
  float cw = cosf(w0), sw = sinf(w0);
  float b0, b1, b2, a0, a1, a2;
  if (kind == 0) {
    float alpha = sw / (2.0f * 1.41f);
    b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
    a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  } else {
    float alpha = sw * 0.5f * 1.41421356f;     /* shelf slope S = 1 */
    float sa = 2.0f * sqrtf(A) * alpha;
    if (kind == 1) {
      b0 = A * ((A + 1) - (A - 1) * cw + sa);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - sa);
      a0 = (A + 1) + (A - 1) * cw + sa;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - sa;
    } else {
      b0 = A * ((A + 1) + (A - 1) * cw + sa);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - sa;
    }
  }
  k[0] = b0 / a0; k[1] = b1 / a0; k[2] = b2 / a0; k[3] = a1 / a0; k[4] = a2 / a0;
}

void eq_design(const FmEqConf *c, int rate, FmEqCoef *o) {
  memset(o, 0, sizeof *o);
  if (rate <= 0) rate = 48000;
  o->pre = 1.0f;
  o->width = 1.0f;
  o->gl = o->gr = 1.0f;
  o->rel = 1.0f - expf(-1.0f / (0.08f * (float)rate));
  if (!c->on || eq_is_flat(c)) return;
  o->active = true;
  for (int i = 0; i < EQ_BANDS; i++) {
    float db = FM_CLAMP(c->band[i], -EQ_MAX_DB, EQ_MAX_DB);
    if (fabsf(db) < 0.05f || kEqFreq[i] >= rate * 0.45f) continue;
    biquad(i == 0 ? 1 : i == EQ_BANDS - 1 ? 2 : 0, kEqFreq[i], db, rate, o->k[i]);
    o->mask |= 1u << i;
  }
  o->pre = powf(10.0f, FM_CLAMP(c->preamp, -EQ_MAX_DB, EQ_MAX_DB) / 20.0f);
  o->width = FM_CLAMP(c->width, 0.0f, 2.0f);
  o->gl = c->balance > 0 ? 1.0f - c->balance : 1.0f;
  o->gr = c->balance < 0 ? 1.0f + c->balance : 1.0f;
}

float eq_response_db(const FmEqCoef *o, int rate, float f) {
  if (!o->active) return 0;
  float w = 2.0f * EQ_PI * f / (float)(rate > 0 ? rate : 48000);
  float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
  float db = 20.0f * log10f(FM_MAX(o->pre, 1e-6f));
  for (int i = 0; i < EQ_BANDS; i++) {
    if (!(o->mask & (1u << i))) continue;
    const float *k = o->k[i];
    float nr = k[0] + k[1] * c1 + k[2] * c2, ni = -(k[1] * s1 + k[2] * s2);
    float dr = 1.0f + k[3] * c1 + k[4] * c2, di = -(k[3] * s1 + k[4] * s2);
    float num = nr * nr + ni * ni, den = FM_MAX(dr * dr + di * di, 1e-12f);
    db += 10.0f * log10f(FM_MAX(num / den, 1e-12f));
  }
  return db;
}

void eq_follow(FmEq *e, int rate, SDL_mutex *mx) {
  if (e->rate == rate && same_conf(&e->applied, &conf.eq)) return;
  FmEqCoef c;
  eq_design(&conf.eq, rate, &c);
  if (mx) SDL_LockMutex(mx);
  /* a band that was off starts from silence instead of stale state */
  for (int i = 0; i < EQ_BANDS; i++)
    if (!(e->c.mask & (1u << i)) || !e->c.active) memset(e->z[i], 0, sizeof e->z[i]);
  if (!e->c.active) e->lim = 1.0f;
  e->c = c;
  if (mx) SDL_UnlockMutex(mx);
  e->applied = conf.eq;
  e->rate = rate;
}

/* ---- processing (audio thread) ------------------------------------------------------ */

static float soft(float x) {
  float a = fabsf(x);
  if (a <= EQ_KNEE) return x;
  float t = (a - EQ_KNEE) / (1.0f - EQ_KNEE);
  float y = EQ_KNEE + (1.0f - EQ_KNEE) * t / (1.0f + t);   /* approaches 1, never reaches it */
  return x < 0 ? -y : y;
}

void eq_process(FmEq *e, float *lr, int frames) {
  const FmEqCoef *c = &e->c;
  if (!c->active || frames <= 0) return;
  if (e->lim <= 0 || e->lim > 1.0f) e->lim = 1.0f;
  for (int i = 0; i < frames; i++) {
    float l = lr[i * 2] * c->pre, r = lr[i * 2 + 1] * c->pre;
    for (int b = 0; b < EQ_BANDS; b++) {
      if (!(c->mask & (1u << b))) continue;
      const float *k = c->k[b];
      float *zl = e->z[b][0], *zr = e->z[b][1];
      float yl = k[0] * l + zl[0];
      zl[0] = k[1] * l - k[3] * yl + zl[1];
      zl[1] = k[2] * l - k[4] * yl;
      float yr = k[0] * r + zr[0];
      zr[0] = k[1] * r - k[3] * yr + zr[1];
      zr[1] = k[2] * r - k[4] * yr;
      l = yl;
      r = yr;
    }
    if (c->width != 1.0f) {
      float m = (l + r) * 0.5f, s = (l - r) * 0.5f * c->width;
      l = m + s;
      r = m - s;
    }
    l *= c->gl;
    r *= c->gr;
    /* limiter: instant attack, smooth release, soft knee as the last guard */
    float peak = FM_MAX(fabsf(l), fabsf(r)) * e->lim;
    if (peak > EQ_LIMIT) e->lim *= EQ_LIMIT / peak;
    else e->lim += (1.0f - e->lim) * c->rel;
    lr[i * 2] = soft(l * e->lim);
    lr[i * 2 + 1] = soft(r * e->lim);
  }
  /* keep denormals out of the filter state during silence */
  for (int b = 0; b < EQ_BANDS; b++)
    for (int ch = 0; ch < 2; ch++)
      for (int j = 0; j < 2; j++)
        if (fabsf(e->z[b][ch][j]) < 1e-20f) e->z[b][ch][j] = 0;
}

/* ---- settings UI ------------------------------------------------------------------- */

static FmScroll g_es;
static float g_eh;

static void touched(void) {
  conf.eq.on = true;
  conf.eq.preset = EQ_CUSTOM;
  memcpy(conf.eq.custom, conf.eq.band, sizeof conf.eq.custom);
}

static float row_h(void) { return DP(ui.touch_mode ? 50 : 38); }

/* Preset chips that wrap; returns the height used. */
static float chips(FmRect r) {
  float h = DP(ui.touch_mode ? 40 : 32), gap = DP(8);
  float x = r.x, y = r.y;
  u32 base = ui_id("eq.chip");
  for (int p = 0; p < EQ_PRESETS; p++) {
    float w = font_width(FONT_REGULAR, ui.m.font_small, kEqNames[p], -1) + DP(28);
    if (x + w > r.x + r.w && x > r.x) { x = r.x; y += h + gap; }
    FmRect c = { x, y, w, h };
    bool on = conf.eq.on && conf.eq.preset == p;
    if (gfx_visible(c)) {
      int f = ui_hit(ui_idn(base, (u32)p), c);
      gfx_rrect(c, h * 0.5f, on ? T.accent_soft : T.surface2);
      if (on) gfx_rrect_line(c, h * 0.5f, DP(1.5f), T.accent);
      else if (f & UI_HOVER) gfx_rrect(c, h * 0.5f, T.hover);
      font_draw_center(on ? FONT_BOLD : FONT_REGULAR, ui.m.font_small, c, kEqNames[p], on ? T.accent : T.text2);
      if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
      if (f & UI_CLICK) {
        eq_preset(&conf.eq, p);
        conf.eq.on = true;
      }
    }
    x += w + gap;
  }
  return y + h - r.y;
}

static void db_text(char *b, size_t cap, float db) {
  if (fabsf(db) < 0.05f) fm_strlcpy(b, "0", cap);
  else fm_snprintf(b, cap, "%+.1f", (double)db);
}

/* Ten vertical sliders over the response curve. */
static void bands(FmRect r) {
  float top_lab = font_line_h(ui.m.font_small) + DP(6);
  float bot_lab = font_line_h(ui.m.font_small) + DP(8);
  FmRect plot = { r.x, r.y + top_lab, r.w, r.h - top_lab - bot_lab };
  float cw = plot.w / EQ_BANDS;
  float mid = plot.y + plot.h * 0.5f, half = plot.h * 0.5f - DP(10);
  bool on = conf.eq.on;
  gfx_rrect(rect_inset2(plot, 0, -DP(4)), DP(12), T.dark ? col_alpha(T.bg, 0.5f) : col_alpha(T.surface2, 0.7f));
  /* grid: 0 dB and +-6 dB */
  for (int g = -1; g <= 1; g++) {
    float y = floorf(mid - g * half * 0.5f);
    gfx_rect(FM_RECT(plot.x + DP(6), y, plot.w - DP(12), FM_MAX(1.0f, floorf(DP(1)))),
             col_alpha(T.text, g == 0 ? 0.16f : 0.07f));
  }
  /* response of the real filters, log frequency like the columns */
  FmEqCoef k;
  FmEqConf shown = conf.eq;
  shown.on = true;
  eq_design(&shown, 48000, &k);
  float xs[97], ys[97];
  int n = 0;
  for (int i = 0; i <= 96; i++) {
    float u = (float)i / 96.0f;
    float f = kEqFreq[0] * powf(2.0f, u * EQ_BANDS - 0.5f);
    float db = eq_response_db(&k, 48000, f);
    xs[n] = plot.x + u * plot.w;
    ys[n] = mid - FM_CLAMP(db, -EQ_MAX_DB * 1.25f, EQ_MAX_DB * 1.25f) / EQ_MAX_DB * half;
    n++;
  }
  FmColor lc = on ? T.accent : T.text3;
  viz_curve(xs, ys, n, mid, lc, col_alpha(lc, 0.16f), DP(2.2f));
  /* sliders */
  u32 base = ui_id("eq.band");
  float kr = DP(ui.touch_mode ? 11 : 9);
  for (int i = 0; i < EQ_BANDS; i++) {
    FmRect col = { plot.x + i * cw, plot.y - DP(4), cw, plot.h + DP(8) };
    u32 id = ui_idn(base, (u32)i);
    int f = ui_hit(id, col);
    float v = conf.eq.band[i];
    if (f & UI_HELD) {
      ui.drag_owner = id;
      float nv = (mid - ui.my) / half * EQ_MAX_DB;
      nv = FM_CLAMP(floorf(nv * 2.0f + 0.5f) * 0.5f, -EQ_MAX_DB, EQ_MAX_DB);
      if (nv != v) { conf.eq.band[i] = nv; touched(); }
    }
    if ((f & UI_DCLICK) && v != 0) { conf.eq.band[i] = 0; touched(); }
    v = conf.eq.band[i];
    float cx = col.x + cw * 0.5f, ky = mid - v / EQ_MAX_DB * half;
    float tw = DP(4);
    gfx_rrect(FM_RECT(cx - tw * 0.5f, mid - half, tw, half * 2), tw * 0.5f, col_alpha(T.text, 0.1f));
    float y0 = FM_MIN(ky, mid), y1 = FM_MAX(ky, mid);
    if (y1 - y0 > 1) gfx_rrect(FM_RECT(cx - tw * 0.5f, y0, tw, y1 - y0), tw * 0.5f, col_alpha(lc, 0.75f));
    bool hot = (f & (UI_HELD | UI_HOVER)) != 0;
    gfx_circle(cx, ky, kr + (hot ? DP(2) : 0), on ? T.accent : T.text2);
    gfx_circle(cx, ky, kr * 0.42f, T.surface);
    char b[16];
    db_text(b, sizeof b, v);
    font_draw_center(FONT_REGULAR, ui.m.font_small, FM_RECT(col.x, r.y, cw, top_lab), b,
                     (f & UI_HELD) ? T.accent : T.text3);
    const char *fl[EQ_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    font_draw_center(FONT_REGULAR, ui.m.font_small, FM_RECT(col.x, r.y + r.h - bot_lab + DP(4), cw, bot_lab - DP(4)),
                     fl[i], T.text2);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_SIZENS);
  }
}

/* Label, slider, value. */
static bool knob(FmRect r, const char *name, u32 id, float *v, float lo, float hi, const char *val) {
  if (!gfx_visible(r)) return false;
  FmRect lab = rect_cut_left(&r, FM_MIN(DP(118), r.w * 0.34f));
  FmRect vr = rect_cut_right(&r, DP(56));
  ui_label(lab, name, FONT_REGULAR, ui.m.font, T.text, UI_LEFT);
  ui_label(vr, val, FONT_REGULAR, ui.m.font_small, T.text2, UI_RIGHT);
  float sh = DP(ui.touch_mode ? 40 : 28);
  return ui_slider(id, rect_inset2(r, DP(4), FM_MAX(0.0f, (r.h - sh) * 0.5f)), v, lo, hi);
}

void eq_ui(FmRect c) {
  u32 sid = ui_id("eq.scroll");
  ui_scroll(&g_es, sid, c, g_eh);
  gfx_clip_push(c);
  FmRect body = rect_inset2(c, DP(2), 0);
  body.w -= DP(6);
  float y = c.y - g_es.y + DP(4), y0 = y;
  FmEqConf *q = &conf.eq;

  FmRect sw = { body.x, y, body.w, row_h() };
  char lab[64];
  fm_snprintf(lab, sizeof lab, "Equalizer \xC2\xB7 %s", q->on ? eq_preset_name(q->preset) : "off");
  if (gfx_visible(sw)) ui_switch(ui_id("eq.on"), sw, lab, &q->on);
  y += sw.h + DP(6);

  y += chips(FM_RECT(body.x, y, body.w, 0)) + DP(14);

  float bh = FM_CLAMP(body.w * 0.6f, DP(190), DP(270));
  FmRect br = { body.x, y, body.w, bh };
  if (gfx_visible(br)) bands(br);
  y += bh + DP(12);

  char val[32];
  float rh = row_h();
  fm_snprintf(val, sizeof val, "%+.1f dB", (double)q->preamp);
  if (knob(FM_RECT(body.x, y, body.w, rh), "Preamp", ui_id("eq.pre"), &q->preamp, -EQ_MAX_DB, EQ_MAX_DB, val)) {
    q->preamp = floorf(q->preamp * 2.0f + 0.5f) * 0.5f;
    q->on = true;
  }
  y += rh;
  fm_snprintf(val, sizeof val, "%d%%", (int)(q->width * 100.0f + 0.5f));
  if (knob(FM_RECT(body.x, y, body.w, rh), "Stereo width", ui_id("eq.width"), &q->width, 0.0f, 2.0f, val)) {
    if (fabsf(q->width - 1.0f) < 0.04f) q->width = 1.0f;
    q->on = true;
  }
  y += rh;
  if (fabsf(q->balance) < 0.02f) fm_strlcpy(val, "Centre", sizeof val);
  else fm_snprintf(val, sizeof val, "%s %d", q->balance < 0 ? "L" : "R", (int)(fabsf(q->balance) * 100.0f + 0.5f));
  if (knob(FM_RECT(body.x, y, body.w, rh), "Balance", ui_id("eq.bal"), &q->balance, -1.0f, 1.0f, val)) {
    if (fabsf(q->balance) < 0.04f) q->balance = 0;
    q->on = true;
  }
  y += rh + DP(8);

  g_eh = y - y0 + DP(8);
  gfx_clip_pop();
  ui_scrollbar(&g_es, c, g_eh);
  eq_sanitize(q);
}
