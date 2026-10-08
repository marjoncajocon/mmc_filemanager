/* fui.h -- immediate-mode UI: input state, theme, widgets, menus, dialogs.
**
** Every frame: ui_begin(), draw the screen calling widgets that both draw
** and report interaction, ui_end() (which draws menus, toasts, tooltips).
** Widgets are identified by a u32 id (ui_id("name"), ui_idn(base, i)).
**
** Design decisions:
**   - Layers instead of z-order bookkeeping: a widget only reacts when its
**     layer is the top one open (base < sheet < dialog < menu), so a dialog
**     blocks the panels without them knowing.
**   - Sizes are in dp; DP(x) converts to pixels with the current scale
**     (display density x user zoom). Touch mode makes rows taller.
**   - Mouse and touch share one pointer; touch adds long-press, fling and
**     pinch. A press that moves beyond the slop becomes a drag and no
**     longer clicks, so lists can scroll under the finger.
*/
#ifndef FUI_H
#define FUI_H

#include "fgfx.h"
#include "ffont.h"
#include "ficon.h"
#include "ftheme.h"

/* ---- theme -------------------------------------------------------------- */

typedef struct FmTheme {
  bool dark;
  FmColor bg;          /* window background */
  FmColor surface;     /* panels, cards */
  FmColor surface2;    /* raised: headers, inputs */
  FmColor surface3;    /* pressed/selected chips */
  FmColor border, divider;
  FmColor text, text2, text3;   /* primary, secondary, disabled */
  FmColor accent, on_accent, accent_soft;
  FmColor sel, sel_line;        /* selected rows */
  FmColor hover, press;         /* overlays */
  FmColor danger, success, warn;
  FmColor shadow, scrim;
  /* style from the theme table (ftheme.c); sizes are design px at 100% */
  int theme;                    /* index into the built-in themes */
  bool bg_grad;                 /* bg -> bg_mid -> bg2 vertical gradient */
  FmColor bg_mid, bg2;
  float bg_mid_at;
  FmColor sel_text;             /* alpha 0: selected rows keep text colours */
  FmColor panel_border;         /* alpha 0: no outline */
  float panel_border_w;
  int shadow_kind;              /* SHADOW_* */
  float radius, row_radius, row_h, font_k;
  FmColor types[TC_COUNT];      /* file-type icon colours */
} FmTheme;

extern FmTheme T;
#define UI_ACCENTS 8
extern const FmColor kAccents[UI_ACCENTS];
/* accent < 0 uses the theme's own accent; a missing mode falls back to the
** theme's other one (T.dark tells which was used). Defined in ftheme.c. */
void theme_apply(int theme, bool dark, int accent);
/* Full-window background, including the gradient themes. */
void theme_draw_bg(FmRect r);

/* ---- metrics ------------------------------------------------------------ */

typedef struct FmMetrics {
  float font, font_small, font_title, font_big;  /* px */
  float row_h;       /* list row */
  float bar_h;       /* toolbars */
  float icon;        /* toolbar icon */
  float pad;         /* standard padding */
  float radius;      /* cards */
  float hit;         /* minimum touch target */
} FmMetrics;

/* ---- input state -------------------------------------------------------- */

enum {
  UI_LAYER_BASE = 0, UI_LAYER_SHEET = 1, UI_LAYER_DIALOG = 2, UI_LAYER_MENU = 3
};

typedef struct FmKey { SDL_Keycode key; u16 mod; } FmKey;

typedef struct FmUi {
  SDL_Window *win;
  float scale;           /* px per dp */
  float zoom;            /* user zoom (settings), 1.0 default */
  float w, h;            /* window in px */
  bool touch_mode;       /* bigger rows, no hover */
  bool portrait;
  FmMetrics m;

  /* pointer */
  float mx, my;
  bool down, pressed, released, rpressed;
  int clicks;            /* 2 on double click (this frame's press) */
  float wheel, wheel_x;  /* notches this frame, +up */
  bool from_touch;
  float press_x, press_y;
  u64 press_t;
  bool moved;            /* beyond slop since press */
  bool long_fired;
  u32 drag_owner;        /* widget that captured the drag (scroll lists) */

  /* multitouch */
  int nfingers;
  float pinch;           /* scale factor this frame (1 = none) */
  float pinch_cx, pinch_cy;

  /* keyboard */
  FmKey keys[32];
  int nkeys;
  u16 mod;
  char text[128];
  int text_len;

  /* widgets */
  u32 hot, active, focus;
  int layer, top_layer, next_top;

  /* time */
  u64 now;
  float dt;
  int redraw;            /* frames still to draw */
  bool anim;             /* set by anything animating this frame */
  int cursor;            /* SDL_SystemCursor wanted this frame */
} FmUi;

extern FmUi ui;

#define DP(x) ((x) * ui.scale)

void ui_init(SDL_Window *win);
void ui_shutdown(void);
void ui_update_scale(void);      /* after resize / display change / zoom */
void ui_event(const SDL_Event *e);
void ui_begin(void);
void ui_end(void);
bool ui_needs_frame(void);
/* Milliseconds the main loop may sleep before the next frame is due
** (caret blink, toast fade, tooltip delay); -1 = only on input. */
int  ui_wait_ms(void);
void ui_redraw(void);            /* draw at least one more frame */
void ui_animate(void);           /* keep drawing (call every frame while moving) */

u32  ui_id(const char *s);
u32  ui_idn(u32 base, u32 n);

/* Layer for the widgets that follow (restore with the returned value). */
int  ui_push_layer(int layer);
void ui_pop_layer(int prev);
bool ui_input_ok(void);          /* the current layer receives input */

/* ---- interaction -------------------------------------------------------- */

enum {
  UI_HOVER  = 1 << 0,
  UI_PRESS  = 1 << 1,   /* went down this frame */
  UI_HELD   = 1 << 2,   /* down and owned */
  UI_CLICK  = 1 << 3,   /* released inside without dragging */
  UI_DCLICK = 1 << 4,   /* double click / double tap */
  UI_LONG   = 1 << 5,   /* long press (touch or mouse), fires once */
  UI_RCLICK = 1 << 6,   /* right click */
  UI_DRAG   = 1 << 7,   /* held and moved beyond slop */
};

int  ui_hit(u32 id, FmRect r);
bool ui_hover(FmRect r);
/* Key pressed this frame with exactly these modifiers (KMOD_CTRL etc. or 0);
** consumes it. KMOD_CTRL also matches Cmd on macOS. */
bool ui_key(SDL_Keycode k, u16 mod);
bool ui_key_any(SDL_Keycode k);  /* any modifiers */
void ui_set_cursor(int sdl_system_cursor);
void ui_tooltip(const char *text);

/* ---- widgets ------------------------------------------------------------ */

enum { UI_BTN_TEXT, UI_BTN_FILLED, UI_BTN_TONAL, UI_BTN_OUTLINE, UI_BTN_DANGER };

bool ui_button(u32 id, FmRect r, FmIcon ic, const char *label, int style);
bool ui_icon_btn(u32 id, FmRect r, FmIcon ic, FmColor c, const char *tooltip);
/* Icon button with a filled circular background when `on`. */
bool ui_toggle_btn(u32 id, FmRect r, FmIcon ic, bool on, const char *tooltip);
bool ui_check(u32 id, FmRect r, const char *label, bool *v);
bool ui_switch(u32 id, FmRect r, const char *label, bool *v);
bool ui_radio(u32 id, FmRect r, const char *label, bool on);
bool ui_slider(u32 id, FmRect r, float *v, float lo, float hi);
/* Segmented control; returns true when the selection changed. */
bool ui_segmented(u32 id, FmRect r, const char *const *labels, int n, int *sel);
void ui_progress(FmRect r, float t);           /* t < 0: indeterminate */
void ui_spinner(FmRect r, FmColor c);
void ui_divider(float x0, float x1, float y);

enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };
void ui_label(FmRect r, const char *s, int face, float size, FmColor c, int align);

/* Single-line text field. Returns UI_TF_* bits. */
enum { UI_TF_PASSWORD = 1, UI_TF_FOCUS = 2, UI_TF_SELECT_STEM = 4, UI_TF_READONLY = 8 };
enum { UI_TF_CHANGED = 1, UI_TF_SUBMIT = 2, UI_TF_CANCEL = 4 };
int  ui_textfield(u32 id, FmRect r, char *buf, int cap, const char *hint, int flags);
void ui_focus(u32 id);            /* give keyboard focus (0 = none) */

/* ---- scrolling ---------------------------------------------------------- */

typedef struct FmScroll {
  float y;          /* offset, 0 = top */
  float vel;        /* px/s while flinging */
  float max;        /* content - view, >= 0 */
  float drag_y0, drag_off0;
  u64 last_move;
  float bar_alpha;
} FmScroll;

/* Handles wheel, drag, fling and clamping for `view`; call before drawing
** the content at y - s->y. */
void ui_scroll(FmScroll *s, u32 id, FmRect view, float content_h);
void ui_scrollbar(FmScroll *s, FmRect view, float content_h);
void ui_scroll_to(FmScroll *s, float top, float bottom, float view_h);   /* make visible */

/* ---- menus -------------------------------------------------------------- */

enum { UI_MI_DISABLED = 1, UI_MI_SEP = 2, UI_MI_CHECKED = 4, UI_MI_DANGER = 8 };

typedef struct FmMenuItem {
  int id;
  FmIcon icon;
  const char *label;
  const char *shortcut;     /* may be NULL */
  int flags;
} FmMenuItem;

/* Opens a popup at x,y (px) owned by `owner`; items are copied. */
void ui_menu_open(u32 owner, float x, float y, const FmMenuItem *items, int n);
bool ui_menu_is_open(void);
/* Returns the chosen item id once (or -1), for the owner that opened it. */
int  ui_menu_result(u32 owner);

/* ---- dialogs ------------------------------------------------------------ */

/* Starts a modal card centred on screen (a bottom sheet on narrow portrait
** screens). Returns the content rect below the title. Draw widgets, then
** ui_dialog_end. `*cancel` becomes true on Escape / back / scrim tap. */
FmRect ui_dialog_begin(u32 id, const char *title, float w_dp, float h_dp, bool *cancel);
void   ui_dialog_end(void);
/* Standard button row at the bottom of `content`; returns the index pressed
** (0 = first label) or -1. The last button is the primary one. */
int    ui_dialog_buttons(FmRect content, const char *const *labels, int n);

/* ---- feedback ----------------------------------------------------------- */

void ui_toast(const char *fmt, ...) FM_PRINTF(1, 2);

/* ---- animation ---------------------------------------------------------- */

/* Smoothly approaches target (exponential, `speed` ~ 1/seconds). */
float ui_anim(u32 id, float target, float speed);
void  ui_anim_set(u32 id, float value);
float ui_ease_out(float t);

/* ---- clipboard ---------------------------------------------------------- */

void ui_clipboard_set(const char *s);
/* Caller frees with fm_free; NULL when empty. */
char *ui_clipboard_get(void);

#endif
