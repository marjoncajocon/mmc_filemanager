/* ftheme.h -- the 20 built-in themes (from the design mockups).
**
** A theme is one or two palette variants (dark and/or light) plus style
** knobs: corner radii, row height, font scale, panel outline, shadow kind,
** selected-text colour, file-type colours and an optional background
** gradient. theme_apply() (fui.h) turns a variant into the live T.
*/
#ifndef FTHEME_H
#define FTHEME_H

#include "fcore.h"

#define THEME_COUNT 20

enum { SHADOW_SOFT = 0, SHADOW_NONE = 1, SHADOW_HARD = 2 };
enum { TC_FOLDER, TC_IMAGE, TC_AUDIO, TC_VIDEO, TC_ARCHIVE, TC_CODE, TC_PDF, TC_DOC, TC_TEXT,
       TC_COUNT };

typedef struct FmThemeGrad { u32 top, mid, bottom; int mid_at; } FmThemeGrad;  /* top = 0: none */

typedef struct FmThemeVariant {
  bool dark;
  u32 bg, panel, raised, line, text, text2, accent, on_accent, sel;   /* 0xAARRGGBB */
  u32 sel_text;            /* alpha 0: keep the normal text colour */
  u32 border;              /* panel outline, alpha 0: none */
  u32 danger, shadow;
  int shadow_kind, shadow_dx, shadow_dy;
  float radius, row_radius, row_h, font;   /* design px at 100% */
  FmThemeGrad grad;
  u32 types[TC_COUNT];
} FmThemeVariant;

typedef struct FmThemeInfo {
  const char *name, *desc;
  int dark, light;         /* index into the variants, -1 = not offered */
} FmThemeInfo;

const FmThemeInfo *theme_info(int theme);
/* The variant for a mode; falls back to the theme's other mode. */
const FmThemeVariant *theme_variant(int theme, bool dark);
bool theme_has_mode(int theme, bool dark);
bool theme_has_both(int theme);
/* Index by name, case-insensitive, or a 1-based number; -1 if unknown. */
int theme_find(const char *name);

#endif
