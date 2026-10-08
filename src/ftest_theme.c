/* ftest_theme.c -- every built-in theme stays readable.
**
** Applies each theme in each mode it offers and checks contrast of the
** text, the selected rows and accent buttons, plus the style metrics and
** name lookup. Runs without a window (theme_apply skips the rescale).
*/
#include "ftest.h"
#include "fui.h"
#include <math.h>

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

/* c over an opaque background */
static FmColor over(FmColor c, FmColor bg) {
  FmColor o = col_mix(bg, (FmColor){ c.r, c.g, c.b, 255 }, (float)c.a / 255.0f);
  o.a = 255;
  return o;
}

static void check_theme(int t, bool dark, const char *mode) {
  int before = g_test_fail;
  theme_apply(t, dark, -1);
  TEST_CHECK(T.theme == t);
  TEST_CHECK(T.dark == dark);
  TEST_CHECK(T.surface.a == 255 && T.surface2.a == 255);
  TEST_CHECK(contrast(T.text, T.surface) >= 4.5f);
  TEST_CHECK(contrast(T.text2, T.surface) >= 2.5f);
  /* a selected row: name in sel_text (or text) on sel over the panel */
  FmColor row = over(T.sel, T.surface);
  FmColor name = T.sel_text.a ? T.sel_text : T.text;
  TEST_CHECK(contrast(name, row) >= 3.0f);
  /* the selection must be visible against the panel */
  TEST_CHECK(T.sel.a == 0 ? T.sel_text.a != 0 : contrast(row, T.surface) > 1.02f);
  TEST_CHECK(contrast(T.on_accent, T.accent) >= 2.5f);
  TEST_CHECK(T.row_h >= 20 && T.row_h <= 64);
  TEST_CHECK(T.radius >= 0 && T.radius <= 32 && T.row_radius >= 0);
  TEST_CHECK(T.font_k > 0.8f && T.font_k < 1.3f);
  TEST_CHECK(T.shadow_kind >= SHADOW_SOFT && T.shadow_kind <= SHADOW_HARD);
  for (int i = 0; i < TC_COUNT; i++) TEST_CHECK(T.types[i].a == 255);
  /* a fixed accent swatch keeps its button text readable too */
  for (int a = 0; a < UI_ACCENTS; a++) {
    theme_apply(t, dark, a);
    TEST_CHECK(contrast(T.on_accent, T.accent) >= 2.0f);
  }
  if (g_test_fail > before) printf("  ^ theme %d %s (%s)\n", t + 1, theme_info(t)->name, mode);
}

int test_themes(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail, variants = 0;
  for (int t = 0; t < THEME_COUNT; t++) {
    const FmThemeInfo *i = theme_info(t);
    TEST_CHECK(i->name && *i->name);
    TEST_CHECK(i->dark >= 0 || i->light >= 0);
    TEST_CHECK(theme_find(i->name) == t);
    char num[8];
    fm_snprintf(num, sizeof num, "%d", t + 1);
    TEST_CHECK(theme_find(num) == t);
    if (theme_has_mode(t, true)) { check_theme(t, true, "dark"); variants++; }
    if (theme_has_mode(t, false)) { check_theme(t, false, "light"); variants++; }
    /* a missing mode falls back to the other one */
    if (!theme_has_both(t)) {
      theme_apply(t, !theme_has_mode(t, true), -1);
      TEST_CHECK(T.dark == theme_has_mode(t, true));
    }
  }
  TEST_CHECK(theme_find("nope") == -1 && theme_find("0") == -1 && theme_find("21") == -1);
  TEST_CHECK(theme_find("dracula") == 4);
  printf("  %d themes, %d palettes checked\n", THEME_COUNT, variants);
  theme_apply(0, true, -1);
  return g_test_fail - before;
}
