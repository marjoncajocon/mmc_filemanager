/* ffont.c -- glyph cache on one atlas texture, with fallback fonts.
**
** Design decisions:
**   - One atlas texture, 256x256 (256 KB) to start: the whole UI in Latin
**     scripts at 100% fills about half of it. When it fills up it is
**     replaced by one twice the size, up to 1024x1024 (4 MB; a GPU texture
**     that size costs about 6 MB of driver memory on Windows), and only when
**     that fills up too is everything dropped and re-rasterized on demand:
**     simpler and smaller than an LRU of shelves, and it only happens on big
**     size or script changes. A scale change (font_reset) starts small again.
**   - The atlas also holds a white block for untextured shapes (fgfx.c).
**   - Glyphs go into an 8-bit mirror of the atlas (64 KB at 256x256) and
**     reach the GPU in one upload per batch, not one per glyph.
**   - The glyph table is 4096 slots (112 KB).
**   - Glyphs are rasterized at integer pixel sizes and placed at integer
**     positions, which keeps small UI text sharp.
**   - Fallback fonts are memory-mapped files, so a 20 MB CJK font costs
**     only the pages actually touched.
*/
#include "ffont.h"
#include "fplat.h"
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), fm_alloc(x))
#define STBTT_free(x, u) ((void)(u), fm_free(x))
#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif

#include "gen_font_regular.h"
#include "gen_font_semibold.h"

#define ATLAS_MIN 256       /* 256 KB; the UI in Latin scripts at 100% fits */
#define ATLAS_MAX 1024      /* 4 MB, reached only by big or many-script text */
#define TABLE 4096          /* power of two */
#define MAX_FONTS 10
#define NO_FONT 255

typedef struct Font {
  stbtt_fontinfo info;
  const u8 *data;
  size_t mapped;           /* >0: plat_mmap'ed size */
  float ascent_u, descent_u, gap_u;   /* font units */
  int ok;
} Font;

typedef struct Glyph {
  u32 cp;
  u16 size;
  u8 face;                 /* requested face (key) */
  u8 font;                 /* font that has it, NO_FONT for missing */
  u8 used;
  i16 x0, y0;              /* bitmap offset from pen/baseline */
  u16 w, h, ax, ay;        /* atlas rect */
  float adv;
} Glyph;

static Font g_fonts[MAX_FONTS];
static int g_nfonts;
static int g_fallbacks_loaded;
static Glyph *g_tab;
static int g_tab_used;
static SDL_Texture *g_atlas;
static int g_sx, g_sy, g_row_h;     /* shelf packer */
static u8 g_gamma[256];
static int g_asz = ATLAS_MIN;       /* atlas edge */
static u8 *g_mirror;                /* coverage copy of the atlas, g_asz^2 bytes */
static int g_dx0, g_dy0, g_dx1, g_dy1;  /* part of the mirror not uploaded yet */
static u32 *g_rgba;                 /* upload buffer */
static int g_rgba_px;               /* its capacity in pixels */
static int g_resets, g_rasterized;

/* ---- fonts -------------------------------------------------------------- */

static bool font_load_mem(Font *f, const u8 *data, size_t mapped, int index) {
  int off = stbtt_GetFontOffsetForIndex(data, index);
  if (off < 0 || !stbtt_InitFont(&f->info, data, off)) return false;
  int a, d, g;
  stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
  f->ascent_u = (float)a; f->descent_u = (float)d; f->gap_u = (float)g;
  f->data = data;
  f->mapped = mapped;
  f->ok = 1;
  return true;
}

/* Candidate system fonts, tried in order on the first missing glyph. */
static const char *const kFallback[] = {
#if defined(FM_WIN)
  "C:\\Windows\\Fonts\\segoeui.ttf",
  "C:\\Windows\\Fonts\\msyh.ttc",
  "C:\\Windows\\Fonts\\YuGothR.ttc",
  "C:\\Windows\\Fonts\\malgun.ttf",
  "C:\\Windows\\Fonts\\Nirmala.ttf",
  "C:\\Windows\\Fonts\\seguisym.ttf",
  "C:\\Windows\\Fonts\\seguiemj.ttf",
#elif defined(FM_ANDROID)
  "/system/fonts/Roboto-Regular.ttf",
  "/system/fonts/NotoSansCJK-Regular.ttc",
  "/system/fonts/NotoSansDevanagari-Regular.otf",
  "/system/fonts/NotoSansArabic-Regular.ttf",
  "/system/fonts/NotoSansThai-Regular.ttf",
  "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf",
  "/system/fonts/DroidSansFallback.ttf",
#elif defined(FM_MACOS) || defined(FM_IOS)
  "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
  "/Library/Fonts/Arial Unicode.ttf",
  "/System/Library/Fonts/Hiragino Sans GB.ttc",
  "/System/Library/Fonts/AppleSDGothicNeo.ttc",
  "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/TTF/DejaVuSans.ttf",
  "/usr/share/fonts/dejavu/DejaVuSans.ttf",
  "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
  "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
  "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
  "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
  "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
};

static void load_fallbacks(void) {
  g_fallbacks_loaded = 1;
  for (int i = 0; i < FM_COUNT(kFallback) && g_nfonts < MAX_FONTS; i++) {
    size_t sz = 0;
    void *p = plat_mmap(kFallback[i], &sz);
    if (!p) continue;
    if (!font_load_mem(&g_fonts[g_nfonts], (const u8 *)p, sz, 0)) {
      plat_munmap(p, sz);
      continue;
    }
    g_nfonts++;
  }
}

bool font_init(void) {
  g_nfonts = 0;
  if (!font_load_mem(&g_fonts[g_nfonts++], font_regular_ttf, 0, 0)) return false;
  if (!font_load_mem(&g_fonts[g_nfonts++], font_semibold_ttf, 0, 0)) return false;
  g_tab = (Glyph *)fm_calloc(TABLE, sizeof(Glyph));
  /* Slight contrast boost: thin strokes keep their weight on dark themes. */
  for (int i = 0; i < 256; i++) g_gamma[i] = (u8)(powf(i / 255.0f, 0.82f) * 255.0f + 0.5f);
  return true;
}

void font_shutdown(void) {
  if (g_atlas) SDL_DestroyTexture(g_atlas);
  g_atlas = NULL;
  for (int i = 0; i < g_nfonts; i++)
    if (g_fonts[i].mapped) plat_munmap((void *)g_fonts[i].data, g_fonts[i].mapped);
  g_nfonts = 0;
  fm_free(g_tab); fm_free(g_mirror); fm_free(g_rgba);
  g_tab = NULL; g_mirror = NULL; g_rgba = NULL;
  g_rgba_px = 0;
  gfx_set_upload_hook(NULL);
}

/* Uploads the glyphs rasterized since the last upload, in one call. fgfx
** calls it right before it submits a batch that samples the atlas. */
static void upload_dirty(SDL_Texture *t) {
  if (!g_atlas || t != g_atlas || g_dx1 <= g_dx0 || g_dy1 <= g_dy0) return;
  int w = g_dx1 - g_dx0, h = g_dy1 - g_dy0, n = w * h;
  u32 *buf = g_rgba;
  if (n > 16384) {
    buf = (u32 *)fm_alloc((size_t)n * 4);            /* the first full upload */
  } else if (n > g_rgba_px) {
    u32 *b = (u32 *)fm_realloc(g_rgba, (size_t)16384 * 4);
    if (b) { g_rgba = b; g_rgba_px = 16384; }
    buf = b;
  }
  if (!buf) return;
  for (int y = 0; y < h; y++) {
    const u8 *src = g_mirror + (size_t)(g_dy0 + y) * g_asz + g_dx0;
    u32 *dst = buf + (size_t)y * w;
    for (int x = 0; x < w; x++) dst[x] = ((u32)src[x] << 24) | 0xFFFFFFu;
  }
  SDL_Rect r = { g_dx0, g_dy0, w, h };
  SDL_UpdateTexture(g_atlas, &r, buf, w * 4);
  if (buf != g_rgba) fm_free(buf);
  g_dx0 = g_dy0 = g_dx1 = g_dy1 = 0;
}

static void mark_dirty(int x, int y, int w, int h) {
  if (g_dx1 <= g_dx0) { g_dx0 = x; g_dy0 = y; g_dx1 = x + w; g_dy1 = y + h; return; }
  if (x < g_dx0) g_dx0 = x;
  if (y < g_dy0) g_dy0 = y;
  if (x + w > g_dx1) g_dx1 = x + w;
  if (y + h > g_dy1) g_dy1 = y + h;
}

/* Drops every glyph and the atlas; the next glyph makes a new one. */
static void drop_cache(void) {
  gfx_flush();                /* uploads what the queued text still needs */
  if (g_tab) memset(g_tab, 0, sizeof(Glyph) * TABLE);
  g_tab_used = 0;
  g_sx = g_sy = g_row_h = 0;
  if (g_atlas) { SDL_DestroyTexture(g_atlas); g_atlas = NULL; }
  fm_free(g_mirror);
  g_mirror = NULL;
  g_dx0 = g_dy0 = g_dx1 = g_dy1 = 0;
}

void font_reset(void) {
  drop_cache();
  g_asz = ATLAS_MIN;          /* a new scale starts small again */
}

static bool ensure_atlas(void) {
  if (g_atlas) return true;
  int sz = g_asz;
  g_atlas = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, sz, sz);
  if (!g_atlas) return false;
  SDL_SetTextureBlendMode(g_atlas, SDL_BLENDMODE_BLEND);
  /* Glyphs are rasterized into a coverage mirror and uploaded in one call
  ** per batch (upload_dirty), the first time as a whole: SDL's Direct3D 11
  ** backend makes a staging texture for every update, and one update per
  ** glyph (or a clear in strips) left the Intel driver holding 10-16 MB
  ** more for good. */
  g_mirror = (u8 *)fm_calloc((size_t)sz * sz, 1);
  if (!g_mirror) { SDL_DestroyTexture(g_atlas); g_atlas = NULL; return false; }
  /* a white 4x4 block in the corner: shapes sample its centre, so they
  ** batch with the text (fgfx.c); glyphs are packed after it */
  for (int y = 0; y < 4; y++) memset(g_mirror + (size_t)y * sz, 255, 4);
  mark_dirty(0, 0, sz, sz);
  gfx_set_upload_hook(upload_dirty);
  g_sx = 5; g_sy = 0; g_row_h = 4;
  gfx_set_white(g_atlas, 2.0f / (float)sz, 2.0f / (float)sz);
  return true;
}

/* ---- glyph cache -------------------------------------------------------- */

static u32 hash3(u32 cp, u32 size, u32 face) {
  u32 h = cp * 2654435761u ^ (size * 40503u) ^ (face * 2246822519u);
  h ^= h >> 15;
  return h & (TABLE - 1);
}

static int pick_font(int face, u32 cp) {
  if (stbtt_FindGlyphIndex(&g_fonts[face].info, (int)cp)) return face;
  if (cp < 0x20) return face;
  if (!g_fallbacks_loaded) load_fallbacks();
  for (int i = 2; i < g_nfonts; i++)
    if (stbtt_FindGlyphIndex(&g_fonts[i].info, (int)cp)) return i;
  return NO_FONT;
}

static Glyph *get_glyph(int face, int size, u32 cp);

static Glyph *rasterize(Glyph *g, int face, int size, u32 cp) {
  u32 want = cp;
  g->cp = cp; g->size = (u16)size; g->face = (u8)face; g->used = 1;
  int fi = pick_font(face, cp);
  g->font = (u8)fi;
  if (fi == NO_FONT) {
    /* draw the replacement box from the main font */
    fi = face;
    cp = 0x25A1;
    if (!stbtt_FindGlyphIndex(&g_fonts[fi].info, (int)cp)) cp = '?';
  }
  Font *f = &g_fonts[fi];
  /* Every font is scaled by em size, so fallback glyphs match in size. */
  float scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
  int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
  int adv, lsb;
  stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
  g->adv = adv * scale;
  int x0, y0, x1, y1;
  stbtt_GetGlyphBitmapBox(&f->info, gi, scale, scale, &x0, &y0, &x1, &y1);
  int w = x1 - x0, h = y1 - y0;
  g->x0 = (i16)x0; g->y0 = (i16)y0;
  if (w <= 0 || h <= 0 || w > 250 || h > 250) { g->w = g->h = 0; return g; }
  if (!ensure_atlas()) { g->w = g->h = 0; return g; }
  if (g_sx + w + 1 > g_asz) { g_sx = 0; g_sy += g_row_h + 1; g_row_h = 0; }
  if (g_sy + h + 1 > g_asz) {
    /* Atlas full: a bigger one if allowed, else start over; either way
    ** the glyphs are rasterized again on demand. Retry this one (the
    ** table was cleared, so not from g). */
    if (g_asz < ATLAS_MAX) g_asz *= 2;
    else g_resets++;
    drop_cache();
    return get_glyph(face, size, want);
  }
  g_rasterized++;
  u8 *dst = g_mirror + (size_t)g_sy * g_asz + g_sx;
  stbtt_MakeGlyphBitmap(&f->info, dst, w, h, g_asz, scale, scale, gi);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) dst[(size_t)y * g_asz + x] = g_gamma[dst[(size_t)y * g_asz + x]];
  mark_dirty(g_sx, g_sy, w, h);
  g->ax = (u16)g_sx; g->ay = (u16)g_sy; g->w = (u16)w; g->h = (u16)h;
  g_sx += w + 1;
  if (h > g_row_h) g_row_h = h;
  return g;
}

static Glyph *get_glyph(int face, int size, u32 cp) {
  if (!g_tab) return NULL;
  if (g_tab_used > TABLE * 3 / 4) drop_cache();
  u32 h = hash3(cp, (u32)size, (u32)face);
  for (;;) {
    Glyph *g = &g_tab[h];
    if (!g->used) { g_tab_used++; return rasterize(g, face, size, cp); }
    if (g->cp == cp && g->size == size && g->face == face) return g;
    h = (h + 1) & (TABLE - 1);
  }
}

static int isize(float size) {
  int s = (int)(size + 0.5f);
  return s < 4 ? 4 : s > 240 ? 240 : s;
}

float font_ascent(float size) {
  Font *f = &g_fonts[FONT_REGULAR];
  return f->ascent_u * stbtt_ScaleForMappingEmToPixels(&f->info, (float)isize(size));
}

float font_line_h(float size) {
  Font *f = &g_fonts[FONT_REGULAR];
  float s = stbtt_ScaleForMappingEmToPixels(&f->info, (float)isize(size));
  return (f->ascent_u - f->descent_u) * s;
}

/* ---- drawing ------------------------------------------------------------ */

float font_draw(int face, float size, float x, float y, const char *s, int len, FmColor c) {
  if (len < 0) len = (int)strlen(s);
  int sz = isize(size);
  float base = floorf(y + font_ascent(size) + 0.5f);
  float pen = x;
  const char *end = s + len;
  while (s < end) {
    u32 cp;
    s += utf8_decode(s, &cp);
    if (cp == '\t') cp = ' ';
    Glyph *g = get_glyph(face, sz, cp);
    if (!g) break;
    if (g->w && c.a) {
      FmRect d = { floorf(pen + 0.5f) + g->x0, base + g->y0, (float)g->w, (float)g->h };
      float k = 1.0f / (float)g_asz;
      gfx_quad_uv(g_atlas, d, (float)g->ax * k, (float)g->ay * k, (float)(g->ax + g->w) * k,
                  (float)(g->ay + g->h) * k, c);
    }
    pen += g->adv;
  }
  return pen;
}

float font_width(int face, float size, const char *s, int len) {
  if (len < 0) len = (int)strlen(s);
  int sz = isize(size);
  float w = 0;
  const char *end = s + len;
  while (s < end) {
    u32 cp;
    s += utf8_decode(s, &cp);
    if (cp == '\t') cp = ' ';
    Glyph *g = get_glyph(face, sz, cp);
    if (!g) break;
    w += g->adv;
  }
  return w;
}

int font_fit(int face, float size, const char *s, int len, float max_w) {
  if (len < 0) len = (int)strlen(s);
  int sz = isize(size);
  float w = 0;
  int i = 0;
  while (i < len) {
    u32 cp;
    int n = utf8_decode(s + i, &cp);
    Glyph *g = get_glyph(face, sz, cp == '\t' ? ' ' : cp);
    if (!g || w + g->adv > max_w) break;
    w += g->adv;
    i += n;
  }
  return i;
}

float font_draw_ellipsis(int face, float size, float x, float y, const char *s, float max_w,
                         FmColor c) {
  int len = (int)strlen(s);
  float w = font_width(face, size, s, len);
  if (w <= max_w) { font_draw(face, size, x, y, s, len, c); return w; }
  float ew = font_width(face, size, "\xE2\x80\xA6", 3);
  int n = font_fit(face, size, s, len, max_w - ew);
  float e = font_draw(face, size, x, y, s, n, c);
  e = font_draw(face, size, e, y, "\xE2\x80\xA6", 3, c);
  return e - x;
}

float font_draw_mid_ellipsis(int face, float size, float x, float y, const char *s, float max_w,
                             FmColor c) {
  int len = (int)strlen(s);
  float w = font_width(face, size, s, len);
  if (w <= max_w) { font_draw(face, size, x, y, s, len, c); return w; }
  float ew = font_width(face, size, "\xE2\x80\xA6", 3);
  /* keep the tail (extension + a few chars) */
  int tail = len;
  float tw = 0;
  float tail_budget = (max_w - ew) * 0.4f;
  while (tail > 0) {
    int p = utf8_prev(s, tail);
    float cw = font_width(face, size, s + p, tail - p);
    if (tw + cw > tail_budget) break;
    tw += cw;
    tail = p;
  }
  int head = font_fit(face, size, s, tail, max_w - ew - tw);
  float e = font_draw(face, size, x, y, s, head, c);
  e = font_draw(face, size, e, y, "\xE2\x80\xA6", 3, c);
  e = font_draw(face, size, e, y, s + tail, len - tail, c);
  return e - x;
}

void font_draw_center(int face, float size, FmRect r, const char *s, FmColor c) {
  float w = font_width(face, size, s, -1);
  float h = font_line_h(size);
  font_draw(face, size, r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, s, -1, c);
}

float font_draw_wrap(int face, float size, float x, float y, float w, const char *s, FmColor c,
                     bool draw) {
  float lh = font_line_h(size) * 1.15f;
  float yy = y;
  while (*s) {
    int len = (int)strlen(s);
    const char *nl = strchr(s, '\n');
    int line_end = nl ? (int)(nl - s) : len;
    int n = font_fit(face, size, s, line_end, w);
    if (n < line_end) {
      int sp = n;
      while (sp > 0 && s[sp] != ' ') sp--;
      if (sp > 0) n = sp;
      if (n == 0) n = utf8_decode(s, &(u32){0});
    }
    if (draw) font_draw(face, size, x, yy, s, n, c);
    yy += lh;
    s += n;
    while (*s == ' ') s++;
    if (*s == '\n') s++;
  }
  return yy - y;
}

void font_get_stats(FontStats *s) {
  memset(s, 0, sizeof *s);
  if (g_atlas) { s->atlas_w = g_asz; s->atlas_h = g_asz; }
  s->used_h = g_sy + g_row_h;
  s->glyphs = g_tab_used;
  s->resets = g_resets;
  s->rasterized = g_rasterized;
}
