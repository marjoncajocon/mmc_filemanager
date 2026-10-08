/* ffont.h -- text with stb_truetype: glyph atlas, fallback fonts, measuring.
**
** Sizes are in physical pixels. Text is UTF-8; code points missing from the
** embedded Poppins faces are looked up in system fonts (CJK, Cyrillic,
** symbols ...), which are memory-mapped only when first needed.
*/
#ifndef FFONT_H
#define FFONT_H

#include "fgfx.h"

enum { FONT_REGULAR = 0, FONT_BOLD = 1 };

bool  font_init(void);
void  font_shutdown(void);
/* Drops every cached glyph (renderer reset, DPI change). */
void  font_reset(void);

/* Draws from the top-left; returns the x after the last glyph. len < 0 = strlen. */
float font_draw(int face, float size, float x, float y, const char *s, int len, FmColor c);
float font_width(int face, float size, const char *s, int len);
float font_line_h(float size);            /* line height */
float font_ascent(float size);
/* Bytes of s that fit in max_w. */
int   font_fit(int face, float size, const char *s, int len, float max_w);
/* Draws, shortening with "..." to max_w; returns the width drawn. */
float font_draw_ellipsis(int face, float size, float x, float y, const char *s, float max_w,
                         FmColor c);
/* Middle ellipsis keeps the extension visible: "very_long_na...me.txt". */
float font_draw_mid_ellipsis(int face, float size, float x, float y, const char *s, float max_w,
                             FmColor c);
/* Centered in r (both axes). */
void  font_draw_center(int face, float size, FmRect r, const char *s, FmColor c);
/* Wraps at spaces inside width; returns the height used. draw=false only measures. */
float font_draw_wrap(int face, float size, float x, float y, float w, const char *s, FmColor c,
                     bool draw);

/* Glyph cache state for --perf. */
typedef struct FontStats {
  int atlas_w, atlas_h;      /* 0 when no atlas exists yet */
  int used_h;                /* rows of the atlas holding glyphs */
  int glyphs;                /* cached glyphs */
  int resets;                /* times the atlas filled up and started over */
  int rasterized;            /* glyphs rasterized since start */
} FontStats;
void  font_get_stats(FontStats *s);

#endif
