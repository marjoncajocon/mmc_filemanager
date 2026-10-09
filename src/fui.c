/* fui.c -- the immediate-mode toolkit behind every screen.
**
** Design decisions:
**   - Input is collected by ui_event into per-frame flags; widgets read them
**     during the frame and ui_end clears them. Nothing is retained per
**     widget except tiny tables (animations) keyed by id.
**   - Menus, toasts and tooltips are drawn by ui_end so they are always on
**     top, whatever order the screen was drawn in.
**   - Only one text field has focus at a time, so its editing state (caret,
**     selection, horizontal scroll) is a single static struct.
*/
#include "fui.h"
#include "fplat.h"
#include <math.h>

FmUi ui;
FmTheme T;

/* ---- theme -------------------------------------------------------------- */

const FmColor kAccents[UI_ACCENTS] = {
  { 0x4F, 0x8C, 0xFF, 255 },   /* blue */
  { 0x7C, 0x5C, 0xFF, 255 },   /* violet */
  { 0x14, 0xB8, 0xA6, 255 },   /* teal */
  { 0x22, 0xC5, 0x5E, 255 },   /* green */
  { 0xF5, 0x9E, 0x0B, 255 },   /* amber */
  { 0xF9, 0x73, 0x16, 255 },   /* orange */
  { 0xEC, 0x48, 0x99, 255 },   /* pink */
  { 0xEF, 0x44, 0x44, 255 },   /* red */
};

void theme_draw_bg(FmRect r) {
  if (!T.bg_grad) return;          /* the clear colour already is the bg */
  float m = r.h * T.bg_mid_at;
  gfx_rrect_vgrad(FM_RECT(r.x, r.y, r.w, m), 0, T.bg, T.bg_mid);
  gfx_rrect_vgrad(FM_RECT(r.x, r.y + m, r.w, r.h - m), 0, T.bg_mid, T.bg2);
}

/* ---- setup -------------------------------------------------------------- */

static float g_px_per_pt = 1.0f;     /* window points -> renderer pixels */
static SDL_Cursor *g_cursors[SDL_NUM_SYSTEM_CURSORS];
static int g_cursor_now = -1;

void ui_init(SDL_Window *win) {
  memset(&ui, 0, sizeof ui);
  ui.win = win;
  ui.zoom = 1.0f;
  ui.pinch = 1.0f;
#ifdef FM_MOBILE
  ui.touch_mode = true;
#endif
  ui_update_scale();
  ui.now = SDL_GetTicks64();
  ui.redraw = 2;
}

void ui_shutdown(void) {
  for (int i = 0; i < SDL_NUM_SYSTEM_CURSORS; i++)
    if (g_cursors[i]) SDL_FreeCursor(g_cursors[i]);
  memset(g_cursors, 0, sizeof g_cursors);
}

void ui_update_scale(void) {
  int ww = 1, wh = 1, pw = 1, ph = 1;
  SDL_GetWindowSize(ui.win, &ww, &wh);
  if (g_ren) SDL_GetRendererOutputSize(g_ren, &pw, &ph);
  else { pw = ww; ph = wh; }
  if (ww <= 0) ww = 1;
  g_px_per_pt = (float)pw / (float)ww;
  float density = g_px_per_pt;
  float ddpi = 0;
  int disp = SDL_GetWindowDisplayIndex(ui.win);
  if (disp < 0) disp = 0;
  if (SDL_GetDisplayDPI(disp, &ddpi, NULL, NULL) != 0) ddpi = 0;
#if defined(FM_ANDROID) || defined(FM_IOS)
  if (ddpi > 0) density = ddpi / 160.0f;
  if (density < 1.0f) density = 1.0f;
#elif defined(FM_MACOS)
  /* macOS reports points; the backing scale is the density. */
#else
  if (ddpi > 0) {
    float f = ddpi / 96.0f;
    f = floorf(f * 4.0f + 0.5f) / 4.0f;      /* 100%, 125%, 150% ... */
    if (f >= 1.0f && f <= 4.0f) density = g_px_per_pt * f;
  }
#endif
  ui.scale = density * ui.zoom;
  ui.w = (float)pw;
  ui.h = (float)ph;
  ui.portrait = ph > pw;
  FmMetrics *m = &ui.m;
  bool t = ui.touch_mode;
  /* the theme scales text and rows; touch keeps finger-sized minimums */
  float fk = T.font_k > 0 ? FM_CLAMP(T.font_k, 0.9f, 1.15f) : 1.0f;
  float rk = T.row_h > 0 ? FM_CLAMP(T.row_h / 44.0f, 0.6f, 1.2f) : 1.0f;
  m->font = DP((t ? 15 : 13.5f) * fk);
  m->font_small = DP((t ? 12.5f : 11.5f) * fk);
  m->font_title = DP(t ? 18 : 16);
  m->font_big = DP(t ? 24 : 22);
  m->row_h = DP(t ? FM_MAX(56 * rk, 48) : 38 * rk);
  m->bar_h = DP(t ? 56 : 44);
  m->icon = DP(t ? 24 : 20);
  m->pad = DP(t ? 14 : 12);
  m->radius = DP(T.radius > 0 || T.row_h > 0 ? T.radius : 14);
  m->hit = DP(t ? 44 : 32);
  /* Glyphs are cached by pixel size, so only a new scale makes the cached
  ** ones useless; a plain window resize keeps them. */
  static float s_font_scale;
  if (ui.scale != s_font_scale) {
    s_font_scale = ui.scale;
    font_reset();
  }
  ui_redraw();
}

/* ---- events ------------------------------------------------------------- */

typedef struct Finger { SDL_FingerID id; float x, y; } Finger;
static Finger g_fingers[4];
static float g_pinch_dist;

static float slop(void) { return DP(ui.from_touch ? 10 : 5); }

static void finger_update(void) {
  if (ui.nfingers >= 2) {
    float dx = g_fingers[0].x - g_fingers[1].x, dy = g_fingers[0].y - g_fingers[1].y;
    float d = sqrtf(dx * dx + dy * dy);
    if (g_pinch_dist > 1.0f && d > 1.0f) ui.pinch *= d / g_pinch_dist;
    g_pinch_dist = d;
    ui.pinch_cx = (g_fingers[0].x + g_fingers[1].x) * 0.5f;
    ui.pinch_cy = (g_fingers[0].y + g_fingers[1].y) * 0.5f;
  } else {
    g_pinch_dist = 0;
  }
}

static int g_input_trace = -1;        /* MMCFM_INPUT_TRACE=1: where presses land (device debugging) */

void ui_event(const SDL_Event *e) {
  if (g_input_trace < 0) g_input_trace = getenv("MMCFM_INPUT_TRACE") != NULL;
  int trace = g_input_trace;
  if (trace && (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP))
    fm_log("input: %s at %d,%d (pt->px %.3f) ui %gx%g, top layer %d (next %d)",
           e->type == SDL_MOUSEBUTTONDOWN ? "down" : "up", e->button.x, e->button.y, (double)g_px_per_pt, (double)ui.w,
           (double)ui.h, ui.top_layer, ui.next_top);
  else if (trace && (e->type == SDL_FINGERDOWN || e->type == SDL_FINGERUP))
    fm_log("input: finger %s at %.3f,%.3f", e->type == SDL_FINGERDOWN ? "down" : "up", (double)e->tfinger.x,
           (double)e->tfinger.y);
  switch (e->type) {
    case SDL_MOUSEMOTION:
      ui.mx = e->motion.x * g_px_per_pt;
      ui.my = e->motion.y * g_px_per_pt;
      ui.from_touch = e->motion.which == SDL_TOUCH_MOUSEID;
      if (ui.down && !ui.moved) {
        float dx = ui.mx - ui.press_x, dy = ui.my - ui.press_y;
        if (dx * dx + dy * dy > slop() * slop()) ui.moved = true;
      }
      ui.redraw = FM_MAX(ui.redraw, 1);
      break;
    case SDL_MOUSEBUTTONDOWN:
      ui.mx = e->button.x * g_px_per_pt;
      ui.my = e->button.y * g_px_per_pt;
      ui.from_touch = e->button.which == SDL_TOUCH_MOUSEID;
      if (e->button.button == SDL_BUTTON_LEFT) {
        ui.down = true;
        ui.pressed = true;
        ui.clicks = e->button.clicks;
        ui.press_x = ui.mx;
        ui.press_y = ui.my;
        ui.press_t = SDL_GetTicks64();
        ui.moved = false;
        ui.long_fired = false;
      } else if (e->button.button == SDL_BUTTON_RIGHT) {
        ui.rpressed = true;
      }
      ui.redraw = FM_MAX(ui.redraw, 2);
      break;
    case SDL_MOUSEBUTTONUP:
      ui.mx = e->button.x * g_px_per_pt;
      ui.my = e->button.y * g_px_per_pt;
      if (e->button.button == SDL_BUTTON_LEFT) {
        ui.down = false;
        ui.released = true;
      }
      ui.redraw = FM_MAX(ui.redraw, 2);
      break;
    case SDL_MOUSEWHEEL: {
      float y = e->wheel.preciseY, x = e->wheel.preciseX;
      if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { y = -y; x = -x; }
      ui.wheel += y;
      ui.wheel_x += x;
      ui.from_touch = false;
      ui.redraw = FM_MAX(ui.redraw, 1);
      break;
    }
    case SDL_FINGERDOWN: {
      if (ui.nfingers < 4) {
        Finger *f = &g_fingers[ui.nfingers++];
        f->id = e->tfinger.fingerId;
        f->x = e->tfinger.x * ui.w;
        f->y = e->tfinger.y * ui.h;
      }
      g_pinch_dist = 0;
      finger_update();
      ui.redraw = FM_MAX(ui.redraw, 1);
      break;
    }
    case SDL_FINGERMOTION:
      for (int i = 0; i < ui.nfingers; i++)
        if (g_fingers[i].id == e->tfinger.fingerId) {
          g_fingers[i].x = e->tfinger.x * ui.w;
          g_fingers[i].y = e->tfinger.y * ui.h;
        }
      finger_update();
      ui.redraw = FM_MAX(ui.redraw, 1);
      break;
    case SDL_FINGERUP:
      for (int i = 0; i < ui.nfingers; i++)
        if (g_fingers[i].id == e->tfinger.fingerId) {
          g_fingers[i] = g_fingers[--ui.nfingers];
          break;
        }
      g_pinch_dist = 0;
      finger_update();
      ui.redraw = FM_MAX(ui.redraw, 1);
      break;
    case SDL_KEYDOWN:
      if (ui.nkeys < FM_COUNT(ui.keys)) {
        ui.keys[ui.nkeys].key = e->key.keysym.sym;
        ui.keys[ui.nkeys].mod = (u16)e->key.keysym.mod;
        ui.nkeys++;
      }
      ui.mod = (u16)e->key.keysym.mod;
      ui.redraw = FM_MAX(ui.redraw, 2);
      break;
    case SDL_KEYUP:
      ui.mod = (u16)SDL_GetModState();
      break;
    case SDL_TEXTINPUT: {
      int n = (int)strlen(e->text.text);
      if (ui.text_len + n < (int)sizeof ui.text) {
        memcpy(ui.text + ui.text_len, e->text.text, (size_t)n);
        ui.text_len += n;
        ui.text[ui.text_len] = 0;
      }
      ui.redraw = FM_MAX(ui.redraw, 2);
      break;
    }
    case SDL_WINDOWEVENT:
      if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
          e->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
        ui_update_scale();
      if (e->window.event == SDL_WINDOWEVENT_LEAVE) { ui.mx = ui.my = -10000; }
      ui.redraw = FM_MAX(ui.redraw, 2);
      break;
    case SDL_DISPLAYEVENT:
      ui_update_scale();
      break;
    default:
      break;
  }
}

/* ---- frame -------------------------------------------------------------- */

static u64 g_last_frame;
static u32 g_frame;

/* tooltip */
static const char *g_tip_text;
static char g_tip_buf[128];
static u32 g_tip_id, g_tip_hover_id;
static u64 g_tip_since;

/* toast */
static char g_toast[256];
static u64 g_toast_until;

/* text field */
typedef struct TextEdit {
  u32 id;
  int caret, anchor;
  float scroll;
  u64 blink0;
  bool selecting;
} TextEdit;
static TextEdit g_te;
static u32 g_tf_seen;            /* focused field drawn this frame */
static u32 g_tf_autofocus_done;

/* menus */
#define MENU_MAX 40
typedef struct Menu {
  bool open;
  u32 owner;
  float x, y;
  FmMenuItem items[MENU_MAX];
  char text[MENU_MAX][96];
  char keys[MENU_MAX][24];
  int n;
  bool armed;                    /* a press happened while open */
  int result;
  u32 result_owner;
  u64 opened;
} Menu;
static Menu g_menu;

/* dialogs */
static int g_dialog_prev_layer[8];
static int g_dialog_depth;
static u32 g_dialog_last_id, g_dialog_this_frame;

void ui_begin(void) {
  u64 now = SDL_GetTicks64();
  ui.dt = g_last_frame ? (float)(now - g_last_frame) / 1000.0f : 1.0f / 60.0f;
  if (ui.dt > 0.1f) ui.dt = 0.1f;
  g_last_frame = now;
  ui.now = now;
  g_frame++;
  ui.anim = false;
  ui.hot = 0;
  ui.layer = UI_LAYER_BASE;
  ui.top_layer = ui.next_top;
  ui.next_top = g_menu.open ? UI_LAYER_MENU : UI_LAYER_BASE;
  ui.cursor = SDL_SYSTEM_CURSOR_ARROW;
  g_tip_text = NULL;
  g_tf_seen = 0;
  g_dialog_this_frame = 0;
  if (ui.redraw > 0) ui.redraw--;
}

static void draw_menu(void);
static void draw_toast(void);
static void draw_tooltip(void);

void ui_end(void) {
  draw_menu();
  draw_toast();
  draw_tooltip();
  /* focus lost when the focused field was not drawn */
  if (ui.focus && g_tf_seen != ui.focus) {
    ui.focus = 0;
  }
  if (!ui.focus && SDL_IsTextInputActive()) {
#if defined(FM_MOBILE)
    SDL_StopTextInput();
#endif
  }
  if (!g_dialog_this_frame) g_dialog_last_id = 0;
  if (!ui.down) {
    if (ui.released || ui.active) ui.redraw = FM_MAX(ui.redraw, 1);
    ui.active = 0;
    ui.drag_owner = 0;
  }
  ui.pressed = ui.released = ui.rpressed = false;
  ui.clicks = 0;
  ui.wheel = ui.wheel_x = 0;
  ui.pinch = 1.0f;
  ui.nkeys = 0;
  ui.text_len = 0;
  ui.text[0] = 0;
  if (ui.cursor != g_cursor_now && !ui.touch_mode) {
    int c = ui.cursor;
    if (c >= 0 && c < SDL_NUM_SYSTEM_CURSORS) {
      if (!g_cursors[c]) g_cursors[c] = SDL_CreateSystemCursor((SDL_SystemCursor)c);
      if (g_cursors[c]) SDL_SetCursor(g_cursors[c]);
    }
    g_cursor_now = c;
  }
}

bool ui_needs_frame(void) {
  return ui.redraw > 0 || ui.anim || ui.down || ui.nfingers > 0;
}

int ui_wait_ms(void) {
  if (ui_needs_frame()) return 0;
  int w = -1;
  u64 now = SDL_GetTicks64();
  if (ui.focus) {
    u64 t = 530 - ((now - g_te.blink0) % 530);
    w = (int)t;
  }
  if (g_toast_until > now) {
    int t = (int)(g_toast_until - now);
    if (w < 0 || t < w) w = t;
  }
  if (g_tip_hover_id && g_tip_id != g_tip_hover_id) {
    int t = (int)(g_tip_since + 650 > now ? g_tip_since + 650 - now : 0);
    if (w < 0 || t < w) w = t;
  }
  return w;
}

void ui_redraw(void) { if (ui.redraw < 2) ui.redraw = 2; }
void ui_animate(void) { ui.anim = true; }

u32 ui_id(const char *s) {
  u32 h = 2166136261u;
  while (*s) { h ^= (u8)*s++; h *= 16777619u; }
  return h ? h : 1;
}

u32 ui_idn(u32 base, u32 n) {
  u32 h = base ^ (n * 2654435761u);
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h ? h : 1;
}

int ui_push_layer(int layer) {
  int prev = ui.layer;
  ui.layer = layer;
  if (layer > ui.next_top) ui.next_top = layer;
  return prev;
}

void ui_pop_layer(int prev) { ui.layer = prev; }

bool ui_input_ok(void) { return ui.layer >= ui.top_layer; }

/* ---- interaction -------------------------------------------------------- */

static bool pointer_in(FmRect r) {
  return rect_has(r, ui.mx, ui.my) && rect_has(gfx_clip(), ui.mx, ui.my);
}

bool ui_hover(FmRect r) {
  if (!ui_input_ok()) return false;
  if (ui.from_touch && !ui.down) return false;
  return pointer_in(r);
}

int ui_hit(u32 id, FmRect r) {
  if (!ui_input_ok()) return 0;
  int f = 0;
  bool inside = pointer_in(r);
  if (inside && !(ui.from_touch && !ui.down) && (ui.drag_owner == 0 || ui.drag_owner == id)) {
    f |= UI_HOVER;
    ui.hot = id;
  }
  if (g_input_trace > 0 && (ui.pressed || ui.released) && inside)
    fm_log("input: %s hits %08x [%g,%g %gx%g] layer %d", ui.pressed ? "press" : "release", id, (double)r.x,
           (double)r.y, (double)r.w, (double)r.h, ui.layer);
  if (ui.pressed && inside && rect_has(r, ui.press_x, ui.press_y)) {
    ui.active = id;
    f |= UI_PRESS;
    if (ui.clicks >= 2) f |= UI_DCLICK;
  }
  if (ui.active == id) {
    if (ui.down) {
      f |= UI_HELD;
      if (ui.moved) f |= UI_DRAG;
      if (!ui.moved && !ui.long_fired && ui.drag_owner == 0 && ui.now - ui.press_t >= 480) {
        ui.long_fired = true;
        f |= UI_LONG;
      }
    }
    if (ui.released && inside && !ui.moved && !ui.long_fired &&
        (ui.drag_owner == 0 || ui.drag_owner == id))
      f |= UI_CLICK;
  }
  if (ui.rpressed && inside) f |= UI_RCLICK;
  return f;
}

static u16 norm_mod(u16 m) {
  u16 r = 0;
  if (m & KMOD_CTRL) r |= 1;
#ifdef FM_MACOS
  if (m & KMOD_GUI) r |= 1;
#endif
  if (m & KMOD_SHIFT) r |= 2;
  if (m & KMOD_ALT) r |= 4;
  return r;
}

bool ui_key(SDL_Keycode k, u16 mod) {
  if (!ui_input_ok()) return false;
  u16 want = norm_mod(mod);
  for (int i = 0; i < ui.nkeys; i++) {
    if (ui.keys[i].key != k) continue;
    if (norm_mod(ui.keys[i].mod) != want) continue;
    /* a focused text field owns plain keys */
    if (ui.focus && !(want & 1) && k != SDLK_ESCAPE && k != SDLK_AC_BACK &&
        !(k >= SDLK_F1 && k <= SDLK_F12))
      return false;
    ui.keys[i].key = 0;
    return true;
  }
  return false;
}

bool ui_key_any(SDL_Keycode k) {
  if (!ui_input_ok()) return false;
  for (int i = 0; i < ui.nkeys; i++) {
    if (ui.keys[i].key == k) {
      if (ui.focus && k != SDLK_ESCAPE && k != SDLK_AC_BACK) return false;
      ui.keys[i].key = 0;
      return true;
    }
  }
  return false;
}

void ui_set_cursor(int c) { ui.cursor = c; }

void ui_tooltip(const char *text) {
  if (!text || ui.from_touch) return;
  g_tip_text = text;
}

/* ---- animation ---------------------------------------------------------- */

#define ANIM_N 256
typedef struct Anim { u32 id; float v; u32 frame; } Anim;
static Anim g_anim[ANIM_N];

static Anim *anim_slot(u32 id, float init) {
  u32 h = id & (ANIM_N - 1);
  Anim *oldest = NULL;
  for (int i = 0; i < 8; i++) {
    Anim *a = &g_anim[(h + (u32)i) & (ANIM_N - 1)];
    if (a->id == id) return a;
    if (!oldest || a->frame < oldest->frame) oldest = a;
  }
  oldest->id = id;
  oldest->v = init;
  oldest->frame = g_frame;
  return oldest;
}

float ui_anim(u32 id, float target, float speed) {
  Anim *a = anim_slot(id, target);
  bool stale = g_frame - a->frame > 2;
  a->frame = g_frame;
  if (stale) { a->v = target; return target; }
  float d = target - a->v;
  if (fabsf(d) < 0.002f) { a->v = target; return target; }
  float k = 1.0f - expf(-speed * ui.dt);
  a->v += d * k;
  ui.anim = true;
  return a->v;
}

void ui_anim_set(u32 id, float value) {
  Anim *a = anim_slot(id, value);
  a->v = value;
  a->frame = g_frame;
}

float ui_ease_out(float t) {
  t = FM_CLAMP(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* ---- widgets ------------------------------------------------------------ */

static void hover_overlay(u32 id, FmRect r, float radius, int f) {
  float h = ui_anim(id ^ 0x9e3779b9u, (f & UI_HOVER) ? 1.0f : 0.0f, 18.0f);
  if (h > 0.01f) gfx_rrect(r, radius, col_alpha(T.hover, h));
  if (f & UI_HELD) gfx_rrect(r, radius, T.press);
}

bool ui_button(u32 id, FmRect r, FmIcon ic, const char *label, int style) {
  int f = ui_hit(id, r);
  float rad = FM_MIN(r.h * 0.5f, DP(12));
  FmColor fg = T.text;
  switch (style) {
    case UI_BTN_FILLED: gfx_rrect(r, rad, T.accent); fg = T.on_accent; break;
    case UI_BTN_DANGER: gfx_rrect(r, rad, T.danger); fg = FM_HEX(0xFFFFFF); break;
    case UI_BTN_TONAL: gfx_rrect(r, rad, T.accent_soft); fg = T.accent; break;
    case UI_BTN_OUTLINE: gfx_rrect_line(r, rad, DP(1), T.border); break;
    default: fg = T.accent; break;
  }
  hover_overlay(id, r, rad, f);
  float tw = label ? font_width(FONT_BOLD, ui.m.font, label, -1) : 0;
  float is = ic ? DP(18) : 0;
  float gap = (ic && label) ? DP(8) : 0;
  float x = r.x + (r.w - tw - is - gap) * 0.5f;
  if (ic) {
    icon_draw(ic, FM_RECT(x, r.y + (r.h - is) * 0.5f, is, is), fg);
    x += is + gap;
  }
  if (label)
    font_draw(FONT_BOLD, ui.m.font, x, r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, label, -1, fg);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

static void tip_track(u32 id, int f, const char *tip) {
  if (!tip) return;
  if (f & UI_HOVER) {
    if (g_tip_hover_id != id) { g_tip_hover_id = id; g_tip_since = ui.now; g_tip_id = 0; }
    if (ui.now - g_tip_since >= 650) {
      g_tip_id = id;
      fm_strlcpy(g_tip_buf, tip, sizeof g_tip_buf);
      ui_tooltip(g_tip_buf);
    }
  } else if (g_tip_hover_id == id) {
    g_tip_hover_id = 0;
    g_tip_id = 0;
  }
  if (f & (UI_PRESS | UI_HELD)) { g_tip_since = ui.now + 100000; }
}

bool ui_icon_btn(u32 id, FmRect r, FmIcon ic, FmColor c, const char *tip) {
  int f = ui_hit(id, r);
  float s = FM_MIN(r.w, r.h);
  FmRect b = rect_center(r, s, s);
  hover_overlay(id, rect_inset(b, DP(2)), s * 0.5f, f);
  float is = FM_MIN(ui.m.icon, s * 0.7f);
  icon_draw(ic, rect_center(b, is, is), c);
  tip_track(id, f, tip);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

bool ui_toggle_btn(u32 id, FmRect r, FmIcon ic, bool on, const char *tip) {
  int f = ui_hit(id, r);
  float s = FM_MIN(r.w, r.h);
  FmRect b = rect_inset(rect_center(r, s, s), DP(2));
  float t = ui_anim(id, on ? 1.0f : 0.0f, 16.0f);
  if (t > 0.01f) gfx_rrect(b, s * 0.5f, col_alpha(T.accent_soft, t * 1.6f));
  hover_overlay(id, b, s * 0.5f, f);
  float is = FM_MIN(ui.m.icon, s * 0.7f);
  icon_draw(ic, rect_center(b, is, is), col_mix(T.text2, T.accent, t));
  tip_track(id, f, tip);
  return (f & UI_CLICK) != 0;
}

bool ui_check(u32 id, FmRect r, const char *label, bool *v) {
  int f = ui_hit(id, r);
  float bs = DP(18);
  FmRect box = { r.x + DP(2), r.y + (r.h - bs) * 0.5f, bs, bs };
  float t = ui_anim(id, *v ? 1.0f : 0.0f, 20.0f);
  hover_overlay(id, rect_inset(box, -DP(8)), DP(20), f);
  if (t < 0.99f) gfx_rrect_line(box, DP(5), DP(1.6f), col_alpha(T.text2, 1.0f - t));
  if (t > 0.01f) {
    gfx_rrect(box, DP(5), col_alpha(T.accent, t));
    icon_draw(IC_CHECK, rect_inset(box, DP(2)), col_alpha(T.on_accent, t));
  }
  if (label)
    font_draw(FONT_REGULAR, ui.m.font, box.x + bs + DP(10),
              r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, label, -1, T.text);
  if (f & UI_CLICK) { *v = !*v; return true; }
  return false;
}

bool ui_switch(u32 id, FmRect r, const char *label, bool *v) {
  int f = ui_hit(id, r);
  float sw = DP(40), sh = DP(24);
  FmRect tr = { r.x + r.w - sw - DP(2), r.y + (r.h - sh) * 0.5f, sw, sh };
  float t = ui_anim(id, *v ? 1.0f : 0.0f, 18.0f);
  gfx_rrect(tr, sh * 0.5f, col_mix(T.surface3, T.accent, t));
  float kr = sh * 0.5f - DP(3);
  float kx = tr.x + sh * 0.5f + (sw - sh) * t;
  gfx_circle(kx, tr.y + sh * 0.5f, kr, t > 0.5f ? T.on_accent : (T.dark ? T.text2 : FM_HEX(0xFFFFFF)));
  if (label)
    font_draw(FONT_REGULAR, ui.m.font, r.x, r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, label, -1,
              T.text);
  hover_overlay(id, r, DP(8), f);
  if (f & UI_CLICK) { *v = !*v; return true; }
  return false;
}

bool ui_radio(u32 id, FmRect r, const char *label, bool on) {
  int f = ui_hit(id, r);
  float cs = DP(18);
  float cx = r.x + DP(2) + cs * 0.5f, cy = r.y + r.h * 0.5f;
  hover_overlay(id, FM_RECT(cx - DP(16), cy - DP(16), DP(32), DP(32)), DP(16), f);
  float t = ui_anim(id, on ? 1.0f : 0.0f, 20.0f);
  gfx_ring(cx, cy, cs * 0.5f - DP(1), DP(1.8f), col_mix(T.text2, T.accent, t));
  if (t > 0.01f) gfx_circle(cx, cy, cs * 0.25f * t, T.accent);
  if (label)
    font_draw(FONT_REGULAR, ui.m.font, r.x + cs + DP(12), r.y + (r.h - font_line_h(ui.m.font)) * 0.5f,
              label, -1, T.text);
  return (f & UI_CLICK) != 0;
}

bool ui_slider(u32 id, FmRect r, float *v, float lo, float hi) {
  int f = ui_hit(id, r);
  float t = hi > lo ? (*v - lo) / (hi - lo) : 0;
  t = FM_CLAMP(t, 0.0f, 1.0f);
  float th = DP(4), kr = DP((f & (UI_HELD | UI_HOVER)) ? 8 : 7);
  float x0 = r.x + kr, x1 = r.x + r.w - kr, cy = r.y + r.h * 0.5f;
  gfx_rrect(FM_RECT(x0, cy - th * 0.5f, x1 - x0, th), th * 0.5f, T.surface3);
  gfx_rrect(FM_RECT(x0, cy - th * 0.5f, (x1 - x0) * t, th), th * 0.5f, T.accent);
  gfx_circle(x0 + (x1 - x0) * t, cy, kr, T.accent);
  bool changed = false;
  if (f & UI_HELD) {
    ui.drag_owner = id;
    float nt = (ui.mx - x0) / (x1 - x0);
    nt = FM_CLAMP(nt, 0.0f, 1.0f);
    float nv = lo + (hi - lo) * nt;
    if (nv != *v) { *v = nv; changed = true; }
  }
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return changed;
}

bool ui_segmented(u32 id, FmRect r, const char *const *labels, int n, int *sel) {
  if (n <= 0) return false;
  gfx_rrect(r, r.h * 0.5f, T.surface2);
  float w = r.w / (float)n;
  float pos = ui_anim(id, (float)*sel, 16.0f);
  gfx_rrect(FM_RECT(r.x + DP(3) + w * pos, r.y + DP(3), w - DP(6), r.h - DP(6)), (r.h - DP(6)) * 0.5f,
            T.dark ? T.surface3 : T.surface);
  bool changed = false;
  for (int i = 0; i < n; i++) {
    FmRect c = { r.x + w * i, r.y, w, r.h };
    int f = ui_hit(ui_idn(id, (u32)i + 1), c);
    if ((f & UI_CLICK) && *sel != i) { *sel = i; changed = true; }
    font_draw_center(i == *sel ? FONT_BOLD : FONT_REGULAR, ui.m.font_small, c, labels[i],
                     i == *sel ? T.text : T.text2);
  }
  return changed;
}

void ui_progress(FmRect r, float t) {
  float rad = r.h * 0.5f;
  gfx_rrect(r, rad, T.surface3);
  if (t < 0) {
    float ph = fmodf((float)ui.now / 1100.0f, 1.0f);
    float w = r.w * 0.3f;
    float x = r.x - w + (r.w + w) * ph;
    gfx_clip_push(r);
    gfx_rrect(FM_RECT(x, r.y, w, r.h), rad, T.accent);
    gfx_clip_pop();
    ui_animate();
  } else {
    t = FM_CLAMP(t, 0.0f, 1.0f);
    if (t > 0) gfx_rrect(FM_RECT(r.x, r.y, FM_MAX(r.w * t, r.h), r.h), rad, T.accent);
  }
}

void ui_spinner(FmRect r, FmColor c) {
  float s = FM_MIN(r.w, r.h);
  float a = (float)(ui.now % 1000) / 1000.0f * 6.2831853f;
  float span = 1.6f + 0.9f * sinf((float)ui.now / 350.0f);
  gfx_arc(r.x + r.w * 0.5f, r.y + r.h * 0.5f, s * 0.38f, FM_MAX(s * 0.11f, DP(2)), a, a + span, c);
  ui_animate();
}

void ui_divider(float x0, float x1, float y) {
  gfx_rect(FM_RECT(x0, floorf(y), x1 - x0, FM_MAX(1.0f, floorf(DP(1)))), T.divider);
}

void ui_label(FmRect r, const char *s, int face, float size, FmColor c, int align) {
  float lh = font_line_h(size);
  float y = r.y + (r.h - lh) * 0.5f;
  if (align == UI_LEFT) { font_draw_ellipsis(face, size, r.x, y, s, r.w, c); return; }
  float w = font_width(face, size, s, -1);
  if (w > r.w) { font_draw_ellipsis(face, size, r.x, y, s, r.w, c); return; }
  float x = align == UI_CENTER ? r.x + (r.w - w) * 0.5f : r.x + r.w - w;
  font_draw(face, size, x, y, s, -1, c);
}

/* ---- text field --------------------------------------------------------- */

static int word_left(const char *s, int p) {
  while (p > 0 && s[p - 1] == ' ') p--;
  while (p > 0 && s[p - 1] != ' ' && s[p - 1] != '.' && s[p - 1] != '/' && s[p - 1] != '\\') p--;
  return p;
}

static int word_right(const char *s, int p) {
  int n = (int)strlen(s);
  while (p < n && s[p] == ' ') p++;
  while (p < n && s[p] != ' ' && s[p] != '.' && s[p] != '/' && s[p] != '\\') p++;
  return p;
}

static int utf8_next(const char *s, int p) {
  if (!s[p]) return p;
  u32 cp;
  return p + utf8_decode(s + p, &cp);
}

static void te_delete_sel(char *buf) {
  int a = FM_MIN(g_te.caret, g_te.anchor), b = FM_MAX(g_te.caret, g_te.anchor);
  if (a == b) return;
  memmove(buf + a, buf + b, strlen(buf + b) + 1);
  g_te.caret = g_te.anchor = a;
}

static bool te_insert(char *buf, int cap, const char *s, int n) {
  int len = (int)strlen(buf);
  /* drop control chars and newlines */
  char clean[256];
  int m = 0;
  for (int i = 0; i < n && m < (int)sizeof clean - 1; i++)
    if ((u8)s[i] >= 0x20 && s[i] != 0x7F) clean[m++] = s[i];
  /* Shortened to fit: never split a code point. Only then: clean[m] past the
  ** cleaned text is not initialised, and reading it dropped typed characters
  ** at random (every key arrives on its own on Android). */
  if (len + m >= cap) {
    m = cap - 1 - len;
    while (m > 0 && ((u8)clean[m] & 0xC0) == 0x80) m--;
  }
  if (m <= 0) return false;
  memmove(buf + g_te.caret + m, buf + g_te.caret, (size_t)(len - g_te.caret + 1));
  memcpy(buf + g_te.caret, clean, (size_t)m);
  g_te.caret += m;
  g_te.anchor = g_te.caret;
  return true;
}

/* Display string (password bullets) and byte mapping. */
static int te_display(const char *buf, bool pw, char *out, int cap, int *map, int map_cap) {
  if (!pw) {
    fm_strlcpy(out, buf, (size_t)cap);
    int n = (int)strlen(out);
    for (int i = 0; i <= n && i < map_cap; i++) map[i] = i;
    return n;
  }
  int o = 0, i = 0;
  while (buf[i] && o + 3 < cap) {
    if (i < map_cap) map[i] = o;
    u32 cp;
    int k = utf8_decode(buf + i, &cp);
    for (int j = 1; j < k && i + j < map_cap; j++) map[i + j] = o;
    out[o++] = (char)0xE2; out[o++] = (char)0x80; out[o++] = (char)0xA2;   /* bullet */
    i += k;
  }
  if (i < map_cap) map[i] = o;
  out[o] = 0;
  return o;
}


/* Caret byte position in buf nearest to screen x px (text starts at x0). */
static int te_hit(const char *buf, bool pw, float x0, float px) {
  int best = 0, i = 0, n = (int)strlen(buf);
  float bestd = 1e9f, x = x0;
  float bw = pw ? font_width(FONT_REGULAR, ui.m.font, "\xE2\x80\xA2", 3) : 0;
  for (;;) {
    float d = fabsf(px - x);
    if (d < bestd) { bestd = d; best = i; }
    if (i >= n) break;
    u32 cp;
    int k = utf8_decode(buf + i, &cp);
    x += pw ? bw : font_width(FONT_REGULAR, ui.m.font, buf + i, k);
    i += k;
  }
  return best;
}

void ui_focus(u32 id) {
  if (ui.focus == id) return;
  ui.focus = id;
  g_te.id = 0;          /* re-initialised by the field on its next draw */
  if (id) SDL_StartTextInput();
}

int ui_textfield(u32 id, FmRect r, char *buf, int cap, const char *hint, int flags) {
  int res = 0;
  bool pw = (flags & UI_TF_PASSWORD) != 0;
  bool ro = (flags & UI_TF_READONLY) != 0;
  int f = ui_hit(id, r);
  float pad = DP(12);
  float x0 = r.x + pad;

  if ((flags & UI_TF_FOCUS) && g_tf_autofocus_done != id && ui_input_ok()) {
    g_tf_autofocus_done = id;
    ui_focus(id);
  }
  if ((f & UI_PRESS) && ui.focus != id) ui_focus(id);
  bool focused = ui.focus == id;
  if (focused) g_tf_seen = id;

  /* fresh focus: select all, or only the stem (name without extension) */
  if (focused && g_te.id != id) {
    g_te.id = id;
    int n = (int)strlen(buf);
    g_te.caret = n;
    g_te.anchor = 0;
    if (flags & UI_TF_SELECT_STEM) {
      const char *dot = strrchr(buf, '.');
      if (dot && dot != buf) g_te.caret = (int)(dot - buf);
    }
    g_te.scroll = 0;
    g_te.blink0 = ui.now;
    g_te.selecting = false;
    if (f & UI_PRESS) g_te.caret = g_te.anchor = te_hit(buf, pw, x0, ui.mx);
    f &= ~UI_PRESS;
  }

  if (focused && ui_input_ok()) {
    int n = (int)strlen(buf);
    if (g_te.caret > n) g_te.caret = n;
    if (g_te.anchor > n) g_te.anchor = n;
    /* mouse */
    if (f & UI_PRESS) {
      g_te.caret = te_hit(buf, pw, x0 - g_te.scroll, ui.mx);
      if (!(ui.mod & KMOD_SHIFT)) g_te.anchor = g_te.caret;
      if (ui.clicks >= 2) {
        g_te.anchor = word_left(buf, g_te.caret);
        g_te.caret = word_right(buf, g_te.caret);
      }
      g_te.selecting = !ui.from_touch;
      g_te.blink0 = ui.now;
    }
    if (g_te.selecting && ui.down && ui.active == id) {
      ui.drag_owner = id;
      g_te.caret = te_hit(buf, pw, x0 - g_te.scroll, ui.mx);
    }
    if (!ui.down) g_te.selecting = false;
    /* a press anywhere else drops focus */
    if (ui.pressed && !rect_has(r, ui.press_x, ui.press_y)) ui.focus = 0;

    /* keys */
    for (int i = 0; i < ui.nkeys; i++) {
      SDL_Keycode k = ui.keys[i].key;
      u16 m = norm_mod(ui.keys[i].mod);
      bool shift = (m & 2) != 0, ctrl = (m & 1) != 0;
      bool used = true;
      int len = (int)strlen(buf);
      switch (k) {
        case SDLK_LEFT:
          if (!shift && g_te.caret != g_te.anchor) g_te.caret = FM_MIN(g_te.caret, g_te.anchor);
          else g_te.caret = ctrl ? word_left(buf, g_te.caret) : utf8_prev(buf, g_te.caret);
          if (!shift) g_te.anchor = g_te.caret;
          break;
        case SDLK_RIGHT:
          if (!shift && g_te.caret != g_te.anchor) g_te.caret = FM_MAX(g_te.caret, g_te.anchor);
          else g_te.caret = ctrl ? word_right(buf, g_te.caret) : utf8_next(buf, g_te.caret);
          if (!shift) g_te.anchor = g_te.caret;
          break;
        case SDLK_HOME: g_te.caret = 0; if (!shift) g_te.anchor = 0; break;
        case SDLK_END: g_te.caret = len; if (!shift) g_te.anchor = len; break;
        case SDLK_BACKSPACE:
          if (ro) break;
          if (g_te.caret == g_te.anchor)
            g_te.anchor = ctrl ? word_left(buf, g_te.caret) : utf8_prev(buf, g_te.caret);
          te_delete_sel(buf);
          res |= UI_TF_CHANGED;
          break;
        case SDLK_DELETE:
          if (ro) break;
          if (g_te.caret == g_te.anchor)
            g_te.anchor = ctrl ? word_right(buf, g_te.caret) : utf8_next(buf, g_te.caret);
          te_delete_sel(buf);
          res |= UI_TF_CHANGED;
          break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
          res |= UI_TF_SUBMIT;
          break;
        case SDLK_ESCAPE: case SDLK_AC_BACK:
          res |= UI_TF_CANCEL;
          ui.focus = 0;
          break;
        case SDLK_a:
          if (ctrl) { g_te.anchor = 0; g_te.caret = len; } else used = false;
          break;
        case SDLK_c: case SDLK_x:
          if (ctrl && !pw && g_te.caret != g_te.anchor) {
            int a = FM_MIN(g_te.caret, g_te.anchor), b = FM_MAX(g_te.caret, g_te.anchor);
            char *s = fm_strndup(buf + a, (size_t)(b - a));
            ui_clipboard_set(s);
            fm_free(s);
            if (k == SDLK_x && !ro) { te_delete_sel(buf); res |= UI_TF_CHANGED; }
          } else {
            used = ctrl;
          }
          break;
        case SDLK_v:
          if (ctrl && !ro) {
            char *s = ui_clipboard_get();
            if (s) {
              te_delete_sel(buf);
              if (te_insert(buf, cap, s, (int)strlen(s))) res |= UI_TF_CHANGED;
              fm_free(s);
            }
          } else {
            used = ctrl;
          }
          break;
        default:
          used = false;
          break;
      }
      if (used) { ui.keys[i].key = 0; g_te.blink0 = ui.now; }
    }
    if (ui.text_len > 0 && !ro) {
      te_delete_sel(buf);
      if (te_insert(buf, cap, ui.text, ui.text_len)) res |= UI_TF_CHANGED;
      ui.text_len = 0;
      ui.text[0] = 0;
      g_te.blink0 = ui.now;
    }
  }
  focused = ui.focus == id;

  /* draw */
  float t = ui_anim(id, focused ? 1.0f : 0.0f, 20.0f);
  float rad = DP(10);
  gfx_rrect(r, rad, T.surface2);
  gfx_rrect_line(r, rad, DP(1) + DP(0.6f) * t, col_mix(T.border, T.accent, t));
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_IBEAM);

  char disp[1024];
  int map[512];
  te_display(buf, pw, disp, sizeof disp, map, FM_COUNT(map));
  float fh = font_line_h(ui.m.font);
  float ty = r.y + (r.h - fh) * 0.5f;
  gfx_clip_push(rect_inset2(r, pad - DP(2), 0));
  if (!buf[0]) {
    if (hint) font_draw(FONT_REGULAR, ui.m.font, x0, ty, hint, -1, T.text3);
  }
  {
    int dl = (int)strlen(disp);
    int cpos = g_te.caret < FM_COUNT(map) ? map[g_te.caret] : dl;
    int apos = g_te.anchor < FM_COUNT(map) ? map[g_te.anchor] : dl;
    if (!focused) cpos = apos = 0;
    float cx = font_width(FONT_REGULAR, ui.m.font, disp, cpos);
    if (focused) {
      float vis = r.w - 2 * pad;
      if (cx - g_te.scroll > vis) g_te.scroll = cx - vis;
      if (cx - g_te.scroll < 0) g_te.scroll = cx;
      if (g_te.scroll < 0) g_te.scroll = 0;
    }
    float sx = focused ? g_te.scroll : 0;
    if (focused && cpos != apos) {
      float ax = font_width(FONT_REGULAR, ui.m.font, disp, apos);
      float a = FM_MIN(ax, cx), b = FM_MAX(ax, cx);
      gfx_rect(FM_RECT(x0 - sx + a, ty, b - a, fh), col_alpha(T.accent, 0.35f));
    }
    if (disp[0]) font_draw(FONT_REGULAR, ui.m.font, x0 - sx, ty, disp, -1, ro ? T.text2 : T.text);
    if (focused && ((ui.now - g_te.blink0) / 530) % 2 == 0)
      gfx_rect(FM_RECT(floorf(x0 - sx + cx), ty + DP(1), FM_MAX(1.0f, DP(1.5f)), fh - DP(2)), T.accent);
    if (focused) {
      SDL_Rect ir = { (int)(r.x / g_px_per_pt), (int)(r.y / g_px_per_pt),
                      (int)(r.w / g_px_per_pt), (int)(r.h / g_px_per_pt) };
      SDL_SetTextInputRect(&ir);
    }
  }
  gfx_clip_pop();
  return res;
}

/* ---- scrolling ---------------------------------------------------------- */

void ui_scroll(FmScroll *s, u32 id, FmRect view, float content_h) {
  s->max = FM_MAX(0.0f, content_h - view.h);
  bool inside = ui_input_ok() && rect_has(view, ui.mx, ui.my);
  bool dragging = ui.drag_owner == id;

  if (inside && ui.wheel != 0 && !(norm_mod(ui.mod) & 1)) {
    s->y -= ui.wheel * DP(ui.touch_mode ? 90 : 72);
    s->vel = 0;
    s->last_move = ui.now;
    ui.wheel = 0;
  }
  /* a press that moves mostly vertically turns into a scroll drag */
  if (!dragging && ui.down && ui.drag_owner == 0 && ui.moved && ui_input_ok() &&
      rect_has(view, ui.press_x, ui.press_y) && ui.nfingers < 2 &&
      (ui.from_touch || ui.touch_mode) &&
      fabsf(ui.my - ui.press_y) > fabsf(ui.mx - ui.press_x)) {
    ui.drag_owner = id;
    s->drag_y0 = ui.my;
    s->drag_off0 = s->y;
    s->vel = 0;
    dragging = true;
  }
  if (dragging) {
    if (ui.down) {
      float ny = s->drag_off0 - (ui.my - s->drag_y0);
      /* rubber band past the ends */
      if (ny < 0) ny = ny / 3.0f;
      else if (ny > s->max) ny = s->max + (ny - s->max) / 3.0f;
      if (ui.dt > 0) {
        float v = (ny - s->y) / ui.dt;
        s->vel = s->vel * 0.6f + v * 0.4f;
      }
      s->y = ny;
      s->last_move = ui.now;
    }
  } else if (fabsf(s->vel) > 1.0f) {
    s->y += s->vel * ui.dt;
    s->vel *= expf(-ui.dt * 3.2f);
    if (fabsf(s->vel) < DP(12)) s->vel = 0;
    if (s->y < -DP(60) || s->y > s->max + DP(60)) s->vel = 0;
    s->last_move = ui.now;
    ui_animate();
  }
  if (!dragging || !ui.down) {
    float target = FM_CLAMP(s->y, 0.0f, s->max);
    if (fabsf(target - s->y) > 0.5f) {
      s->y += (target - s->y) * FM_MIN(1.0f, ui.dt * 14.0f);
      ui_animate();
    } else {
      s->y = target;
    }
  }
}

void ui_scrollbar(FmScroll *s, FmRect view, float content_h) {
  if (s->max <= 0.5f) return;
  bool recent = ui.now - s->last_move < 900;
  bool near = !ui.touch_mode && ui_hover(FM_RECT(view.x + view.w - DP(14), view.y, DP(14), view.h));
  u32 base = (u32)((uintptr_t)s >> 3);
  float a = ui_anim(ui_idn(base, 77), (recent || near) ? 1.0f : 0.0f, 8.0f);
  if (recent) ui.redraw = FM_MAX(ui.redraw, 1);
  float th = FM_MAX(view.h * view.h / content_h, DP(32));
  float t = FM_CLAMP(s->y / s->max, 0.0f, 1.0f);
  float w = near ? DP(6) : DP(4);
  FmRect bar = { view.x + view.w - w - DP(3), view.y + (view.h - th) * t, w, th };
  u32 id = ui_idn(base, 78);
  int f = ui.touch_mode ? 0 : ui_hit(id, rect_inset2(bar, -DP(5), 0));
  if (f & UI_HELD) {
    ui.drag_owner = id;
    float nt = (ui.my - view.y - th * 0.5f) / (view.h - th);
    s->y = FM_CLAMP(nt, 0.0f, 1.0f) * s->max;
    s->vel = 0;
    s->last_move = ui.now;
  }
  if (a > 0.01f) gfx_rrect(bar, w * 0.5f, col_alpha(T.text2, 0.55f * a));
}

void ui_scroll_to(FmScroll *s, float top, float bottom, float view_h) {
  if (top < s->y) s->y = top;
  else if (bottom > s->y + view_h) s->y = bottom - view_h;
  s->vel = 0;
}

/* ---- menus -------------------------------------------------------------- */

void ui_menu_open(u32 owner, float x, float y, const FmMenuItem *items, int n) {
  Menu *m = &g_menu;
  m->open = true;
  m->owner = owner;
  m->x = x;
  m->y = y;
  m->n = FM_MIN(n, MENU_MAX);
  m->armed = false;
  m->result = -1;
  m->result_owner = 0;
  m->opened = ui.now;
  for (int i = 0; i < m->n; i++) {
    m->items[i] = items[i];
    fm_strlcpy(m->text[i], items[i].label ? items[i].label : "", sizeof m->text[i]);
    fm_strlcpy(m->keys[i], items[i].shortcut ? items[i].shortcut : "", sizeof m->keys[i]);
    m->items[i].label = m->text[i];
    m->items[i].shortcut = m->keys[i];
  }
  ui.next_top = UI_LAYER_MENU;
  ui_anim_set(ui_id("menu.anim"), 0.0f);
  ui_redraw();
}

bool ui_menu_is_open(void) { return g_menu.open; }

int ui_menu_result(u32 owner) {
  if (g_menu.result_owner == owner && g_menu.result >= 0) {
    int r = g_menu.result;
    g_menu.result = -1;
    g_menu.result_owner = 0;
    return r;
  }
  return -1;
}

static void draw_menu(void) {
  Menu *m = &g_menu;
  if (!m->open) return;
  int prev = ui_push_layer(UI_LAYER_MENU);
  ui.top_layer = UI_LAYER_MENU;   /* the menu always gets input */
  float t = ui_ease_out(ui_anim(ui_id("menu.anim"), 1.0f, 16.0f));
  bool sheet = ui.touch_mode && ui.portrait;
  float rh = DP(ui.touch_mode ? 50 : 34), sep = DP(9);
  float h = DP(8) * 2;
  float w = DP(ui.touch_mode ? 260 : 230);
  for (int i = 0; i < m->n; i++) {
    h += (m->items[i].flags & UI_MI_SEP) ? sep : rh;
    float tw = font_width(FONT_REGULAR, ui.m.font, m->text[i], -1) + DP(72) +
               (m->keys[i][0] ? font_width(FONT_REGULAR, ui.m.font_small, m->keys[i], -1) + DP(24) : 0);
    if (tw > w) w = tw;
  }
  FmRect r;
  if (sheet) {
    w = ui.w;
    h += DP(16);
    if (h > ui.h * 0.8f) h = ui.h * 0.8f;
    r = FM_RECT(0, ui.h - h * t, w, h);
    gfx_rect(FM_RECT(0, 0, ui.w, ui.h), col_alpha(T.scrim, t));
  } else {
    if (w > ui.w - DP(16)) w = ui.w - DP(16);
    if (h > ui.h - DP(16)) h = ui.h - DP(16);
    float x = m->x, y = m->y;
    if (x + w > ui.w - DP(8)) x = ui.w - DP(8) - w;
    if (y + h > ui.h - DP(8)) y = FM_MAX(DP(8), m->y - h);
    if (x < DP(8)) x = DP(8);
    r = FM_RECT(x, y - DP(6) * (1 - t), w, h);
  }
  float rad = DP(12);
  gfx_shadow(r, rad, DP(18), col_alpha(T.shadow, t));
  if (sheet) gfx_rrect4(r, rad * 1.5f, rad * 1.5f, 0, 0, T.surface2);
  else {
    gfx_rrect(r, rad, col_alpha(T.dark ? T.surface2 : T.surface, t));
    gfx_rrect_line(r, rad, DP(1), col_alpha(T.border, t));
  }

  if (ui.pressed) m->armed = true;
  bool chosen = false;
  float y = r.y + DP(8) + (sheet ? DP(10) : 0);
  if (sheet) gfx_rrect(FM_RECT(r.x + r.w * 0.5f - DP(18), r.y + DP(8), DP(36), DP(4)), DP(2), T.text3);
  gfx_clip_push(r);
  for (int i = 0; i < m->n; i++) {
    FmMenuItem *it = &m->items[i];
    if (it->flags & UI_MI_SEP) {
      ui_divider(r.x + DP(12), r.x + r.w - DP(12), y + sep * 0.5f);
      y += sep;
      continue;
    }
    FmRect row = { r.x + DP(6), y, r.w - DP(12), rh };
    bool dis = (it->flags & UI_MI_DISABLED) != 0;
    int f = dis ? 0 : ui_hit(ui_idn(ui_id("menu.item"), (u32)i), row);
    if (f & (UI_HOVER | UI_HELD)) gfx_rrect(row, DP(8), (f & UI_HELD) ? T.press : T.hover);
    FmColor c = dis ? T.text3 : (it->flags & UI_MI_DANGER) ? T.danger : T.text;
    if (it->icon)
      icon_draw(it->icon, FM_RECT(row.x + DP(10), row.y + (rh - DP(18)) * 0.5f, DP(18), DP(18)),
                dis ? T.text3 : (it->flags & UI_MI_DANGER) ? T.danger : T.text2);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font, row.x + DP(40),
                       row.y + (rh - font_line_h(ui.m.font)) * 0.5f, it->label, row.w - DP(48), c);
    if (it->flags & UI_MI_CHECKED) {
      icon_draw(IC_CHECK, FM_RECT(row.x + row.w - DP(26), row.y + (rh - DP(16)) * 0.5f, DP(16), DP(16)),
                T.accent);
    } else if (it->shortcut && it->shortcut[0]) {
      float kw = font_width(FONT_REGULAR, ui.m.font_small, it->shortcut, -1);
      font_draw(FONT_REGULAR, ui.m.font_small, row.x + row.w - kw - DP(10),
                row.y + (rh - font_line_h(ui.m.font_small)) * 0.5f, it->shortcut, -1, T.text3);
    }
    if ((f & UI_CLICK) && m->armed) {
      m->result = it->id;
      m->result_owner = m->owner;
      chosen = true;
    }
    y += rh;
  }
  gfx_clip_pop();
  bool outside = (ui.pressed || ui.rpressed) && !rect_has(r, ui.mx, ui.my);
  bool esc = false;
  for (int i = 0; i < ui.nkeys; i++)
    if (ui.keys[i].key == SDLK_ESCAPE || ui.keys[i].key == SDLK_AC_BACK) {
      esc = true;
      ui.keys[i].key = 0;
    }
  if (chosen || outside || esc) {
    m->open = false;
    ui.next_top = UI_LAYER_BASE;
    ui_redraw();
    if (outside) { ui.pressed = false; ui.active = 0; }
  }
  ui_pop_layer(prev);
}

/* ---- dialogs ------------------------------------------------------------ */

FmRect ui_dialog_begin(u32 id, const char *title, float w_dp, float h_dp, bool *cancel) {
  if (g_dialog_depth < FM_COUNT(g_dialog_prev_layer))
    g_dialog_prev_layer[g_dialog_depth++] = ui_push_layer(UI_LAYER_DIALOG);
  if (g_dialog_last_id != id) {
    ui_anim_set(ui_idn(id, 1), 0.0f);
    g_tf_autofocus_done = 0;
  }
  g_dialog_last_id = id;
  g_dialog_this_frame = id;
  float t = ui_ease_out(ui_anim(ui_idn(id, 1), 1.0f, 14.0f));
  gfx_rect(FM_RECT(0, 0, ui.w, ui.h), col_alpha(T.scrim, t));
  bool sheet = ui.portrait && ui.w < DP(560);
  /* lay out in the part of the window the on-screen keyboard leaves free */
  float vis_h = ui.h - ui_keyboard_h();
  float w = FM_MIN(DP(w_dp), ui.w - DP(24));
  float h = FM_MIN(DP(h_dp), vis_h - DP(24));
  float rad = DP(20);
  FmRect r;
  if (sheet) {
    w = ui.w;
    h = FM_MIN(DP(h_dp) + DP(12), vis_h - DP(40));
    r = FM_RECT(0, vis_h - h * t, w, h);
    gfx_shadow(r, rad, DP(24), col_alpha(T.shadow, t));
    gfx_rrect4(r, rad, rad, 0, 0, T.surface);
    gfx_rrect(FM_RECT(r.x + r.w * 0.5f - DP(18), r.y + DP(8), DP(36), DP(4)), DP(2), T.text3);
    r.y += DP(10);
    r.h -= DP(10);
  } else {
    r = rect_center(FM_RECT(0, 0, ui.w, vis_h), w, h);
    r.y += DP(16) * (1 - t);
    gfx_shadow(r, rad, DP(28), col_alpha(T.shadow, t));
    gfx_rrect(r, rad, col_alpha(T.surface, FM_MIN(1.0f, t * 1.5f)));
  }
  if (cancel) {
    *cancel = false;
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0)) *cancel = true;
    if (ui_input_ok() && ui.pressed && !rect_has(r, ui.mx, ui.my) && t > 0.9f) *cancel = true;
  }
  FmRect c = rect_inset(r, DP(20));
  if (title && title[0]) {
    FmRect tr = rect_cut_top(&c, font_line_h(ui.m.font_title) + DP(12));
    font_draw_ellipsis(FONT_BOLD, ui.m.font_title, tr.x, tr.y, title, tr.w, T.text);
  }
  return c;
}

void ui_dialog_end(void) {
  if (g_dialog_depth > 0) ui_pop_layer(g_dialog_prev_layer[--g_dialog_depth]);
}

int ui_dialog_buttons(FmRect content, const char *const *labels, int n) {
  float bh = DP(ui.touch_mode ? 46 : 38);
  float y = content.y + content.h - bh;
  float x = content.x + content.w;
  int res = -1;
  for (int i = n - 1; i >= 0; i--) {
    float bw = font_width(FONT_BOLD, ui.m.font, labels[i], -1) + DP(36);
    x -= bw;
    bool primary = i == n - 1;
    if (ui_button(ui_idn(ui_id("dlg.btn"), (u32)i), FM_RECT(x, y, bw, bh), IC_NONE, labels[i],
                  primary ? UI_BTN_FILLED : UI_BTN_TEXT))
      res = i;
    x -= DP(8);
  }
  if (res < 0 && n > 0 && !ui.focus && (ui_key(SDLK_RETURN, 0) || ui_key(SDLK_KP_ENTER, 0)))
    res = n - 1;
  return res;
}

/* ---- toast and tooltip -------------------------------------------------- */

void ui_toast(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_toast, sizeof g_toast, fmt, ap);
  va_end(ap);
  g_toast_until = SDL_GetTicks64() + 2600;
  ui_redraw();
}

static void draw_toast(void) {
  if (!g_toast[0] || ui.now >= g_toast_until + 250) return;
  float left = (float)((i64)g_toast_until - (i64)ui.now);
  float a = left > 0 ? FM_MIN(1.0f, (2600 - left) / 150.0f + 0.01f) : 1.0f + left / 250.0f;
  a = FM_CLAMP(a, 0.0f, 1.0f);
  float w = FM_MIN(font_width(FONT_REGULAR, ui.m.font, g_toast, -1) + DP(40), ui.w - DP(32));
  float h = DP(44);
  FmRect r = { (ui.w - w) * 0.5f, ui.h - h - DP(28) - DP(8) * (1 - a), w, h };
  FmColor bg = T.dark ? FM_HEX(0xE8EAF0) : FM_HEX(0x23272F);
  FmColor fg = T.dark ? FM_HEX(0x161A22) : FM_HEX(0xF2F4F8);
  gfx_shadow(r, h * 0.5f, DP(16), col_alpha(T.shadow, a));
  gfx_rrect(r, h * 0.5f, col_alpha(bg, a));
  font_draw_ellipsis(FONT_REGULAR, ui.m.font, r.x + DP(20), r.y + (h - font_line_h(ui.m.font)) * 0.5f,
                     g_toast, w - DP(40), col_alpha(fg, a));
  if (a < 1.0f) ui_animate();
}

static void draw_tooltip(void) {
  if (!g_tip_text || ui.down) return;
  float fs = ui.m.font_small;
  float w = font_width(FONT_REGULAR, fs, g_tip_text, -1) + DP(16);
  float h = font_line_h(fs) + DP(10);
  float x = FM_MIN(ui.mx + DP(10), ui.w - w - DP(4));
  float y = ui.my + DP(22);
  if (y + h > ui.h) y = ui.my - h - DP(8);
  FmRect r = { x, y, w, h };
  gfx_rrect(r, DP(6), T.dark ? FM_HEX(0x3A404C) : FM_HEX(0x2B303A));
  font_draw(FONT_REGULAR, fs, x + DP(8), y + DP(5), g_tip_text, -1, FM_HEX(0xF2F4F8));
}

/* ---- clipboard ---------------------------------------------------------- */

void ui_clipboard_set(const char *s) { SDL_SetClipboardText(s); }

char *ui_clipboard_get(void) {
  if (!SDL_HasClipboardText()) return NULL;
  char *s = SDL_GetClipboardText();
  if (!s) return NULL;
  char *r = s[0] ? fm_strdup(s) : NULL;
  SDL_free(s);
  return r;
}

/* ---- small exports for other modules ------------------------------------ */

/* Renderer pixels the soft keyboard covers at the bottom (0 when hidden or
** on desktops). Only counted while text input is on, so a keyboard closing
** late never leaves a dialog floating. */
float ui_keyboard_h(void) {
  if (!SDL_IsTextInputActive()) return 0;
  return (float)plat_ime_inset() * g_px_per_pt;
}

float ui_px_per_pt(void) { return g_px_per_pt; }
bool ui_pointer_in(FmRect r) { return pointer_in(r); }
void ui_tip_track(u32 id, int f, const char *tip) { tip_track(id, f, tip); }
