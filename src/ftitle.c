/* ftitle.c -- the custom title bar (see ftitle.h). */
#include "ftitle.h"
#include "fapp.h"
#include "fconf.h"
#include "fui.h"
#include "fplat.h"

#define NODRAG_MAX 24

static bool g_custom;
#if !defined(FM_MOBILE) && !defined(FM_WEB)
static bool g_pip;                      /* picture in picture (title_pip) */
#endif
static FmRect g_nodrag_next[NODRAG_MAX];  /* filled during the frame */
static int g_nnodrag_next;
#if !defined(FM_MOBILE) && !defined(FM_WEB)
static FmRect g_bar;                    /* renderer pixels, last full frame */
static FmRect g_nodrag[NODRAG_MAX];
static int g_nnodrag;
static FmRect g_bar_next;
#endif

bool title_custom(void) { return g_custom; }

#if !defined(FM_MOBILE) && !defined(FM_WEB)

/* Called by SDL from the event pump with window points (not pixels). */
static SDL_HitTestResult hit_test(SDL_Window *win, const SDL_Point *pt, void *data) {
  FM_UNUSED(data);
  int ww, wh;
  SDL_GetWindowSize(win, &ww, &wh);
  Uint32 fl = SDL_GetWindowFlags(win);
  if (!(fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
    int e = 6;                          /* resize grip, in points */
    bool l = pt->x < e, r = pt->x >= ww - e, t = pt->y < e, b = pt->y >= wh - e;
    if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (t) return SDL_HITTEST_RESIZE_TOP;
    if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    if (l) return SDL_HITTEST_RESIZE_LEFT;
    if (r) return SDL_HITTEST_RESIZE_RIGHT;
  }
  float k = ui_px_per_pt();
  float x = (float)pt->x * k, y = (float)pt->y * k;
  if (!g_pip && !rect_has(g_bar, x, y)) return SDL_HITTEST_NORMAL;   /* PiP: the whole window drags */
  for (int i = 0; i < g_nnodrag; i++)
    if (rect_has(g_nodrag[i], x, y)) return SDL_HITTEST_NORMAL;
  return SDL_HITTEST_DRAGGABLE;
}

void title_apply(void) {
  if (!app.win) return;
  g_custom = !conf.system_title;
  bool own = g_custom || g_pip;
  SDL_SetWindowBordered(app.win, own ? SDL_FALSE : SDL_TRUE);
  SDL_SetWindowHitTest(app.win, own ? hit_test : NULL, NULL);
  ui_redraw();
}

void title_pip(bool on) {
  g_pip = on;
  title_apply();
}

#else

void title_apply(void) { g_custom = false; }
void title_pip(bool on) { FM_UNUSED(on); }

#endif

void title_bar(FmRect r) {
#if defined(FM_MOBILE) || defined(FM_WEB)
  FM_UNUSED(r);                         /* no window frame to drag */
#else
  /* the previous frame's layout serves hit tests until this one is done */
  g_bar = g_bar_next;
  g_nnodrag = g_nnodrag_next;
  memcpy(g_nodrag, g_nodrag_next, sizeof g_nodrag);
  g_bar_next = r;
  g_nnodrag_next = 0;
#endif
}

void title_nodrag(FmRect r) {
  if (g_nnodrag_next < NODRAG_MAX) g_nodrag_next[g_nnodrag_next++] = r;
}

/* ---- buttons ------------------------------------------------------------ */

enum { BTN_MIN, BTN_MAX, BTN_CLOSE };

static void press(int which) {
#if !defined(FM_MOBILE) && !defined(FM_WEB)
  Uint32 fl = SDL_GetWindowFlags(app.win);
  if (which == BTN_MIN) SDL_MinimizeWindow(app.win);
  else if (which == BTN_MAX) {
    if (fl & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(app.win);
    else SDL_MaximizeWindow(app.win);
  } else {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;                /* same path as the OS close button */
    SDL_PushEvent(&e);
  }
#else
  FM_UNUSED(which);
#endif
}

static const char *const kTips[] = { "Minimize", "Maximize", "Close" };

#ifdef FM_MACOS
/* Traffic lights: red close, yellow minimize, green zoom, on the left. */
void title_buttons(FmRect *in) {
  if (!g_custom) return;
  static const int order[3] = { BTN_CLOSE, BTN_MIN, BTN_MAX };
  static const u32 cols[3] = { 0xFEBC2E, 0x28C840, 0xFF5F57 };   /* by BTN_* */
  float d = DP(12), gap = DP(8);
  FmRect area = rect_cut_left(in, DP(8) + 3 * d + 2 * gap + DP(8));
  title_nodrag(area);
  u32 base = ui_id("title.btn");
  bool group_hover = ui_pointer_in(area);
  for (int i = 0; i < 3; i++) {
    FmRect b = { area.x + DP(8) + (float)i * (d + gap), area.y + (area.h - d) * 0.5f, d, d };
    int f = ui_hit(ui_idn(base, (u32)i), rect_inset(b, -DP(3)));
    float cx = b.x + d * 0.5f, cy = b.y + d * 0.5f;
    gfx_circle(cx, cy, d * 0.5f, FM_HEX(cols[order[i]]));
    if (group_hover) {
      FmColor g = FM_RGBA(0, 0, 0, 150);
      float s = d * 0.22f;
      if (order[i] == BTN_CLOSE) {
        gfx_line(cx - s, cy - s, cx + s, cy + s, DP(1.2f), g);
        gfx_line(cx - s, cy + s, cx + s, cy - s, DP(1.2f), g);
      } else if (order[i] == BTN_MIN) {
        gfx_line(cx - s - DP(1), cy, cx + s + DP(1), cy, DP(1.2f), g);
      } else {
        gfx_tri(cx - s, cy + s * 0.4f, cx - s, cy - s, cx + s * 0.4f, cy - s, g);
        gfx_tri(cx + s, cy - s * 0.4f, cx + s, cy + s, cx - s * 0.4f, cy + s, g);
      }
    }
    ui_tip_track(ui_idn(base, (u32)i), f, kTips[order[i]]);
    if (f & UI_CLICK) press(order[i]);
  }
}
#else
/* Windows / Linux: flat full-height buttons on the right, close goes red. */
void title_buttons(FmRect *in) {
  if (!g_custom) return;
  bool maxed = (SDL_GetWindowFlags(app.win) & SDL_WINDOW_MAXIMIZED) != 0;
  float bw = DP(ui.touch_mode ? 52 : 46);
  u32 base = ui_id("title.btn");
  for (int which = BTN_CLOSE; which >= BTN_MIN; which--) {
    FmRect b = rect_cut_right(in, bw);
    b.y = 0;                          /* reach the top edge, like the OS buttons */
    b.h = in->y + in->h;
    title_nodrag(b);
    int f = ui_hit(ui_idn(base, (u32)which), b);
    FmColor fg = T.text2;
    if (which == BTN_CLOSE && (f & (UI_HOVER | UI_HELD))) {
      gfx_rect(b, (f & UI_HELD) ? FM_HEX(0xC42B1C) : FM_HEX(0xE81123));
      fg = FM_HEX(0xFFFFFF);
    } else if (f & UI_HELD) {
      gfx_rect(b, T.press);
      fg = T.text;
    } else if (f & UI_HOVER) {
      gfx_rect(b, T.hover);
      fg = T.text;
    }
    float cx = floorf(b.x + b.w * 0.5f) + 0.5f, cy = floorf(b.y + b.h * 0.5f) + 0.5f;
    float s = DP(5), th = FM_MAX(1.0f, DP(1));
    if (which == BTN_CLOSE) {
      gfx_line(cx - s, cy - s, cx + s, cy + s, th, fg);
      gfx_line(cx - s, cy + s, cx + s, cy - s, th, fg);
    } else if (which == BTN_MAX) {
      if (maxed) {                    /* restore: two overlapping squares */
        float o = DP(2);
        gfx_rrect_line(FM_RECT(cx - s, cy - s + o, 2 * s - o, 2 * s - o), DP(1.5f), th, fg);
        gfx_line(cx - s + o, cy - s, cx + s, cy - s, th, fg);
        gfx_line(cx + s, cy - s, cx + s, cy + s - o, th, fg);
      } else {
        gfx_rrect_line(FM_RECT(cx - s, cy - s, 2 * s, 2 * s), DP(1.5f), th, fg);
      }
    } else {
      gfx_line(cx - s, cy, cx + s, cy, th, fg);
    }
    ui_tip_track(ui_idn(base, (u32)which), f, which == BTN_MAX && maxed ? "Restore" : kTips[which]);
    if (f & UI_CLICK) press(which);
  }
}
#endif

/* ---- window corners and outline ----------------------------------------- */

#define CORNER_RADIUS 10                /* dp, close to macOS windows */

static int g_corner_mode;               /* plat_window_corners() result */
#if !defined(FM_MOBILE) && !defined(FM_WEB)
static u32 g_corner_key[5];             /* what it was applied for */
#endif

static void update_corners(bool custom, bool maxed) {
#if !defined(FM_MOBILE) && !defined(FM_WEB)
  int ww, wh;
  SDL_GetWindowSize(app.win, &ww, &wh);
  u32 rgb = ((u32)T.border.r << 16) | ((u32)T.border.g << 8) | T.border.b;
  int rad = (int)(DP(CORNER_RADIUS) + 0.5f);
  u32 key[5] = { (u32)custom | ((u32)maxed << 1), (u32)ww, (u32)wh, rgb, (u32)rad };
  if (!memcmp(key, g_corner_key, sizeof key)) return;
  bool first = g_corner_key[1] == 0;
  memcpy(g_corner_key, key, sizeof key);
  if (!custom && first) return;         /* the OS frame was never touched */
  g_corner_mode = plat_window_corners(custom, rad, rgb, maxed);
#else
  FM_UNUSED(custom);
  FM_UNUSED(maxed);
#endif
}

void title_outline(void) {
  bool maxed = false;
#if !defined(FM_MOBILE) && !defined(FM_WEB)
  if (!app.win) return;
  maxed = (SDL_GetWindowFlags(app.win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
#endif
  update_corners(g_custom, maxed);
  if (!g_custom || maxed) return;
  if (g_corner_mode == 1) return;       /* Windows 11 draws the edge itself */
  float rad = g_corner_mode == 2 ? DP(CORNER_RADIUS) : 0;
  gfx_rrect_line(FM_RECT(0, 0, ui.w, ui.h), rad, FM_MAX(1.0f, DP(1)), T.border);
}
