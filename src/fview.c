/* fview.c -- viewer selection and the chrome every viewer shares.
**
** Design decisions:
**   - view_for decides from content where it matters: images are sniffed
**     (a .png that is really a JPEG still opens; HEIC/WebP go to the
**     system), unknown files open as text only when they look like text,
**     and FFmpeg-only audio goes to the system app when FFmpeg is missing.
**   - Bars fade with ui_anim and hide on a timer (view_wake_in), so a
**     paused viewer draws nothing until something happens.
**   - Rotated textures are drawn with SDL_RenderGeometry directly (fgfx
**     only has axis-aligned UVs); the gfx batch is flushed first so the
**     draw order and clip stay right.
*/
#include "fview_int.h"
#include "fdec_img.h"
#include "fdec_vid.h"

/* ---- choosing a viewer ------------------------------------------------------ */

static bool looks_like_text(const char *path) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  u8 b[4096];
  size_t n = fread(b, 1, sizeof b, f);
  fclose(f);
  if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return true;
  int ctrl = 0;
  for (size_t i = 0; i < n; i++) {
    if (b[i] == 0) return false;
    if (b[i] < 0x09 || (b[i] > 0x0D && b[i] < 0x20 && b[i] != 0x1B)) ctrl++;
  }
  return ctrl * 50 <= (int)n;
}

static bool builtin_audio_ext(const char *path) {
  static const char *const kExt[] = { ".mp3", ".mp2", ".flac", ".wav", ".ogg", ".oga", ".aif", ".aiff" };
  for (int i = 0; i < FM_COUNT(kExt); i++)
    if (fm_ends_with_i(path, kExt[i])) return true;
  return false;
}

const FmViewer *view_for(FmType t, const char *path) {
  switch (t) {
    case FT_IMAGE:
    case FT_GIF:
    case FT_SVG:
      return img_sniff(path) != IMGK_UNKNOWN ? &g_view_image : NULL;
    case FT_AUDIO:
      return builtin_audio_ext(path) || ff_available() ? &g_view_audio : NULL;
    case FT_VIDEO:
      return &g_view_video;
    case FT_TEXT:
    case FT_CODE:
      return &g_view_text;
    case FT_FILE:
      return looks_like_text(path) ? &g_view_text : NULL;
    default:
      return NULL;
  }
}

/* ---- timers ------------------------------------------------------------------ */

static SDL_atomic_t g_wake_pending;
static u64 g_wake_at;

static Uint32 wake_cb(Uint32 interval, void *param) {
  FM_UNUSED(interval);
  FM_UNUSED(param);
  SDL_AtomicSet(&g_wake_pending, 0);
  app_wake();
  return 0;
}

void view_wake_in(u32 ms) {
  u64 now = SDL_GetTicks64();
  u64 at = now + ms;
  /* one timer at a time: keep the earliest */
  if (SDL_AtomicGet(&g_wake_pending) && g_wake_at <= at) return;
  if (SDL_AddTimer(ms ? ms : 1, wake_cb, NULL)) {
    SDL_AtomicSet(&g_wake_pending, 1);
    g_wake_at = at;
  }
}

/* ---- chrome --------------------------------------------------------------------- */

void view_chrome_poke(FmViewChrome *c) {
  c->last_input = ui.now;
  c->hidden = false;
}

void view_chrome_toggle(FmViewChrome *c) {
  if (c->shown) {
    c->hidden = true;
  } else {
    c->hidden = false;
    c->last_input = ui.now;
  }
}

float view_chrome(FmViewChrome *c, u32 id, bool autohide) {
  bool moved = (ui.mx != c->mx || ui.my != c->my) && !ui.from_touch && !ui.touch_mode;
  c->mx = ui.mx;
  c->my = ui.my;
  if (c->last_input == 0) c->last_input = ui.now;
  if (moved || ui.nkeys > 0 || ui.wheel != 0) {
    c->last_input = ui.now;
    if (moved) c->hidden = false;
  }
  bool vis = !c->hidden;
  if (autohide && vis) {
    u64 idle = ui.now - c->last_input;
    if (idle >= VIEW_HIDE_MS) vis = false;
    else view_wake_in((u32)(VIEW_HIDE_MS - idle) + 20);
  }
  c->shown = vis;
  return ui_anim(id, vis ? 1.0f : 0.0f, 12.0f);
}

static FmColor bar_fg(int style, float alpha) {
  return style == VIEW_BAR_MEDIA ? col_alpha(VIEW_FG, alpha) : col_alpha(T.text, alpha);
}

bool view_topbar(FmRect area, int style, float alpha, const char *title, const char *sub, int nact,
                 FmRect *actions) {
  float h = ui.m.bar_h;
  FmRect bar = { area.x, area.y, area.w, h };
  memset(actions, 0, sizeof *actions);
  if (alpha <= 0.01f) return false;
  if (style == VIEW_BAR_MEDIA) {
    gfx_rrect_vgrad(FM_RECT(area.x, area.y, area.w, h * 1.7f), 0, col_alpha(VIEW_SCRIM, alpha),
                    FM_RGBA(0, 0, 0, 0));
  } else {
    gfx_rect(bar, col_alpha(T.surface, alpha));
    ui_divider(bar.x, bar.x + bar.w, bar.y + bar.h - 1);
  }
  FmColor fg = bar_fg(style, alpha);
  FmColor fg2 = style == VIEW_BAR_MEDIA ? col_alpha(VIEW_FG2, alpha) : col_alpha(T.text2, alpha);
  FmRect r = rect_inset2(bar, DP(4), 0);
  bool back = ui_icon_btn(ui_id("view.back"), rect_cut_left(&r, FM_MAX(h, ui.m.hit)), IC_BACK, fg, "Back");
  float aw = FM_MIN(nact * h, r.w * 0.6f);
  *actions = rect_cut_right(&r, aw);
  r = rect_inset2(r, DP(6), 0);
  if (title) {
    float fs = ui.m.font, ss = ui.m.font_small;
    float lh = font_line_h(fs), sh = sub && sub[0] ? font_line_h(ss) : 0;
    float y = r.y + (r.h - lh - sh) * 0.5f;
    font_draw_mid_ellipsis(FONT_BOLD, fs, r.x, y, title, r.w, fg);
    if (sh > 0) font_draw_ellipsis(FONT_REGULAR, ss, r.x, y + lh, sub, r.w, fg2);
  }
  return back && alpha > 0.5f;
}

bool view_bar_btn(FmRect *actions, u32 id, FmIcon ic, const char *tip, int style, float alpha, bool on) {
  if (alpha <= 0.01f || actions->w < actions->h * 0.9f) return false;
  FmRect b = rect_cut_right(actions, actions->h);
  if (on) {
    float s = FM_MIN(b.w, b.h) - DP(8);
    gfx_circle(b.x + b.w * 0.5f, b.y + b.h * 0.5f, s * 0.5f, col_alpha(T.accent, 0.35f * alpha));
  }
  FmColor fg = on ? col_alpha(style == VIEW_BAR_MEDIA ? VIEW_FG : T.accent, alpha) : bar_fg(style, alpha);
  return ui_icon_btn(id, b, ic, fg, tip) && alpha > 0.5f;
}

void view_bottom_scrim(FmRect r, float alpha) {
  if (alpha <= 0.01f) return;
  gfx_rrect_vgrad(r, 0, FM_RGBA(0, 0, 0, 0), col_alpha(VIEW_SCRIM, alpha));
}

bool view_key_back(void) {
  return ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0);
}

void view_fmt_time(double sec, char *buf, size_t cap) {
  if (!(sec >= 0)) sec = 0;
  long s = (long)sec;
  if (s >= 3600) fm_snprintf(buf, cap, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  else fm_snprintf(buf, cap, "%ld:%02ld", s / 60, s % 60);
}

void view_message(FmRect area, FmIcon ic, const char *title, const char *detail, FmColor fg, FmColor fg2) {
  float is = DP(56);
  float tw = FM_MIN(area.w - DP(48), DP(460));
  float th = font_line_h(ui.m.font_title);
  float dh = detail ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, tw, detail, fg2, false) : 0;
  float total = is + DP(16) + th + (dh > 0 ? DP(8) + dh : 0);
  float y = area.y + (area.h - total) * 0.5f;
  if (ic) icon_draw(ic, FM_RECT(area.x + (area.w - is) * 0.5f, y, is, is), fg2);
  y += is + DP(16);
  if (title) font_draw_center(FONT_BOLD, ui.m.font_title, FM_RECT(area.x, y, area.w, th), title, fg);
  y += th + DP(8);
  if (detail) {
    /* centre each wrapped line by drawing into a centred column */
    float x = area.x + (area.w - tw) * 0.5f;
    float w1 = font_width(FONT_REGULAR, ui.m.font, detail, -1);
    if (w1 < tw) x = area.x + (area.w - w1) * 0.5f;
    font_draw_wrap(FONT_REGULAR, ui.m.font, x, y, tw, detail, fg2, true);
  }
}

/* ---- textures -------------------------------------------------------------------- */

void view_tex_rot(SDL_Texture *t, FmRect d, int rot, FmColor c) {
  if (!t || !gfx_visible(d)) return;
  static const float kUv[4][8] = {
    { 0, 0, 1, 0, 1, 1, 0, 1 },
    { 0, 1, 0, 0, 1, 0, 1, 1 },
    { 1, 1, 0, 1, 0, 0, 1, 0 },
    { 1, 0, 1, 1, 0, 1, 0, 0 },
  };
  const float *uv = kUv[((rot % 4) + 4) % 4];
  float xs[4] = { d.x, d.x + d.w, d.x + d.w, d.x };
  float ys[4] = { d.y, d.y, d.y + d.h, d.y + d.h };
  SDL_Vertex v[4];
  for (int i = 0; i < 4; i++) {
    v[i].position.x = xs[i];
    v[i].position.y = ys[i];
    v[i].color.r = c.r; v[i].color.g = c.g; v[i].color.b = c.b; v[i].color.a = c.a;
    v[i].tex_coord.x = uv[i * 2];
    v[i].tex_coord.y = uv[i * 2 + 1];
  }
  static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
  gfx_flush();
  SDL_RenderGeometry(g_ren, t, v, 4, idx, 6);
}

SDL_Texture *view_tex_rgba(const u8 *px, int w, int h) {
  if (!px || w <= 0 || h <= 0) return NULL;
  SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (!t) return NULL;
  SDL_UpdateTexture(t, NULL, px, w * 4);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
  return t;
}

int view_max_texture(void) {
  static int cached;
  if (cached) return cached;
  SDL_RendererInfo info;
  int m = 0;
  if (g_ren && SDL_GetRendererInfo(g_ren, &info) == 0)
    m = FM_MIN(info.max_texture_width, info.max_texture_height);
  if (m <= 0) m = 16384;                 /* software renderer: no hard limit */
  cached = FM_CLAMP(m, 2048, 16384);
  return cached;
}

/* ---- info card ------------------------------------------------------------------- */

void view_info_dialog(u32 id, const char *title, const char *const *keys, const char *const *vals, int n,
                      bool *open) {
  float row = DP(ui.touch_mode ? 44 : 34);
  bool cancel = false;
  FmRect c = ui_dialog_begin(id, title, 440, 96 + n * (ui.touch_mode ? 44 : 34) + 40, &cancel);
  float kw = FM_MIN(DP(110), c.w * 0.35f);
  for (int i = 0; i < n; i++) {
    FmRect r = rect_cut_top(&c, row);
    FmRect k = rect_cut_left(&r, kw);
    ui_label(k, keys[i], FONT_REGULAR, ui.m.font_small, T.text2, UI_LEFT);
    font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font, r.x, r.y + (r.h - font_line_h(ui.m.font)) * 0.5f,
                           vals[i] ? vals[i] : "", r.w, T.text);
  }
  static const char *const kBtn[] = { "Close" };
  if (ui_dialog_buttons(c, kBtn, 1) == 0) cancel = true;
  ui_dialog_end();
  if (cancel) *open = false;
}
