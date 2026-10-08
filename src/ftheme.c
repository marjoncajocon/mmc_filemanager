/* ftheme.c -- the theme table and theme_apply().
**
** The table itself is ftheme_data.h, generated from the design mockups by
** tools/gen_themes.py. This file maps a variant onto the live theme T and
** derives the colours the designs leave implicit (hover, press, disabled
** text, ...).
*/
#include "ftheme.h"
#include "fui.h"
#include <math.h>

#include "ftheme_data.h"

/* ---- lookup ------------------------------------------------------------- */

static int clamp_theme(int t) { return (t >= 0 && t < THEME_COUNT) ? t : 0; }

const FmThemeInfo *theme_info(int theme) { return &kThemes[clamp_theme(theme)]; }

bool theme_has_mode(int theme, bool dark) {
  const FmThemeInfo *i = theme_info(theme);
  return (dark ? i->dark : i->light) >= 0;
}

bool theme_has_both(int theme) { return theme_has_mode(theme, true) && theme_has_mode(theme, false); }

const FmThemeVariant *theme_variant(int theme, bool dark) {
  const FmThemeInfo *i = theme_info(theme);
  int v = dark ? i->dark : i->light;
  if (v < 0) v = dark ? i->light : i->dark;
  return &kVariants[v];
}

int theme_find(const char *name) {
  if (!name || !*name) return -1;
  char *end;
  long n = strtol(name, &end, 10);
  if (!*end) return (n >= 1 && n <= THEME_COUNT) ? (int)n - 1 : -1;
  for (int t = 0; t < THEME_COUNT; t++)
    if (!fm_stricmp(kThemes[t].name, name)) return t;
  /* also accept the first word: "dracula", "x-plore", "brutal" */
  for (int t = 0; t < THEME_COUNT; t++)
    if (!fm_strnicmp(kThemes[t].name, name, strlen(name))) return t;
  return -1;
}

/* ---- colour helpers ----------------------------------------------------- */

static FmColor argb(u32 c) {
  return FM_RGBA((c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) & 255);
}

/* c drawn over an opaque background, as one opaque colour */
static FmColor over(FmColor c, FmColor bg) {
  FmColor o = col_mix(bg, (FmColor){ c.r, c.g, c.b, 255 }, (float)c.a / 255.0f);
  o.a = 255;
  return o;
}

/* WCAG relative luminance (sRGB channels linearised) */
static float lin(u8 v) {
  float c = (float)v / 255.0f;
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float lum(FmColor c) { return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b); }

static float contrast(FmColor a, FmColor b) {
  float x = lum(a) + 0.05f, y = lum(b) + 0.05f;
  return x > y ? x / y : y / x;
}

/* ---- apply -------------------------------------------------------------- */

void theme_apply(int theme, bool dark, int accent) {
  const FmThemeVariant *v = theme_variant(theme, dark);
  bool own = accent < 0 || accent >= UI_ACCENTS;
  FmColor a = own ? argb(v->accent) : kAccents[accent];
  T.theme = clamp_theme(theme);
  T.dark = v->dark;

  /* background, with the optional vertical gradient */
  T.bg = argb(v->bg);
  T.bg_grad = v->grad.top != 0;
  FmColor base = T.bg;
  if (T.bg_grad) {
    T.bg = argb(v->grad.top);
    T.bg_mid = argb(v->grad.mid);
    T.bg2 = argb(v->grad.bottom);
    T.bg_mid_at = (float)v->grad.mid_at / 100.0f;
    base = T.bg_mid;
  }

  /* Translucent surfaces (Frost) are flattened onto the gradient's middle so
  ** dialogs and menus drawn over content stay readable. */
  T.surface = over(argb(v->panel), base);
  T.surface2 = over(argb(v->raised), base);
  T.surface3 = col_mix(T.surface2, argb(v->text), 0.08f);
  T.border = over(argb(v->line), T.surface);
  T.divider = col_alpha(T.border, T.dark ? 0.9f : 1.0f);
  T.text = over(argb(v->text), T.surface);
  T.text2 = over(argb(v->text2), T.surface);
  T.text3 = col_mix(T.text2, T.surface, 0.45f);
  FmColor ink = T.dark ? FM_HEX(0xFFFFFF) : FM_HEX(0x000000);
  T.hover = col_alpha(ink, T.dark ? 0.055f : 0.04f);
  T.press = col_alpha(ink, T.dark ? 0.10f : 0.08f);
  T.scrim = T.dark ? FM_RGBA(0, 0, 0, 140) : FM_RGBA(10, 14, 22, 90);
  T.danger = argb(v->danger);
  T.success = T.dark ? FM_HEX(0x3DD68C) : FM_HEX(0x1FA463);
  T.warn = T.dark ? FM_HEX(0xF5B841) : FM_HEX(0xD99000);

  T.accent = a;
  T.on_accent = own ? argb(v->on_accent)
                    : (contrast(a, FM_HEX(0x111111)) > contrast(a, FM_HEX(0xFFFFFF)) ? FM_HEX(0x111111)
                                                                         : FM_HEX(0xFFFFFF));
  T.accent_soft = col_alpha(a, T.dark ? 0.20f : 0.13f);
  T.sel = own ? argb(v->sel) : col_alpha(a, T.dark ? 0.24f : 0.15f);
  T.sel_line = col_alpha(a, 0.85f);

  /* selected rows: the design's text colour, or inverse video when the
  ** selection is a solid block the normal text would vanish into */
  T.sel_text = argb(v->sel_text);
  if (T.sel_text.a == 0 && T.sel.a == 255 && contrast(T.sel, T.text) < 3.0f)
    T.sel_text = contrast(T.sel, T.surface) >= contrast(T.sel, FM_HEX(0xFFFFFF)) ? T.surface
                                                                                  : FM_HEX(0xFFFFFF);

  T.panel_border = argb(v->border);
  /* an outline in the text colour is a deliberate heavy frame (Brutal Blocks) */
  T.panel_border_w = (T.panel_border.a && v->border == v->text) ? 2.0f : 1.0f;

  T.shadow_kind = v->shadow_kind;
  T.shadow = v->shadow_kind == SHADOW_NONE ? FM_RGBA(0, 0, 0, 0) : argb(v->shadow);
  gfx_shadow_style(v->shadow_kind, (float)v->shadow_dx, (float)v->shadow_dy);

  T.radius = v->radius;
  T.row_radius = v->row_radius;
  T.row_h = v->row_h;
  T.font_k = v->font / 13.0f;
  for (int i = 0; i < TC_COUNT; i++) T.types[i] = argb(v->types[i]);

  /* radii, rows and font sizes depend on the theme */
  if (ui.win) ui_update_scale();
  else ui_redraw();
}
