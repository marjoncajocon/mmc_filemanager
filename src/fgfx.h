/* fgfx.h -- 2D drawing on SDL_Renderer: anti-aliased shapes, textures, clip.
**
** All coordinates are physical pixels (floats). Shapes are triangulated on
** the CPU and batched into one SDL_RenderGeometry call per texture switch,
** so a full frame is a handful of draw calls even on the software renderer.
**
** Design decisions:
**   - Anti-aliasing is geometric: every filled shape gets a 1 px feather ring
**     whose outer vertices have alpha 0. No MSAA or shaders needed, works
**     with every SDL backend.
**   - The clip stack intersects rectangles and flushes the batch on change.
*/
#ifndef FGFX_H
#define FGFX_H

#include "fcore.h"
#include "fsdl.h"

typedef struct FmRect { float x, y, w, h; } FmRect;
typedef struct FmColor { u8 r, g, b, a; } FmColor;

#define FM_RGBA(r, g, b, a) ((FmColor){ (u8)(r), (u8)(g), (u8)(b), (u8)(a) })
#define FM_HEX(h) ((FmColor){ (u8)((h) >> 16), (u8)((h) >> 8), (u8)(h), 255 })
#define FM_RECT(x, y, w, h) ((FmRect){ (float)(x), (float)(y), (float)(w), (float)(h) })

FmColor col_alpha(FmColor c, float a);          /* multiplies alpha */
FmColor col_mix(FmColor a, FmColor b, float t);  /* t=0 -> a, t=1 -> b */

FmRect rect_inset(FmRect r, float d);
FmRect rect_inset2(FmRect r, float dx, float dy);
bool   rect_has(FmRect r, float x, float y);
FmRect rect_intersect(FmRect a, FmRect b);
/* Cut a strip off a rectangle and return it (layout helpers). */
FmRect rect_cut_top(FmRect *r, float h);
FmRect rect_cut_bottom(FmRect *r, float h);
FmRect rect_cut_left(FmRect *r, float w);
FmRect rect_cut_right(FmRect *r, float w);
FmRect rect_center(FmRect outer, float w, float h);

/* ---- frame -------------------------------------------------------------- */

extern SDL_Renderer *g_ren;

void gfx_init(SDL_Renderer *ren);
void gfx_shutdown(void);
void gfx_begin(int w, int h, FmColor clear);
void gfx_end(void);                 /* flush + present */
void gfx_flush(void);
int  gfx_draw_calls(void);          /* last frame, for the debug overlay */

/* ---- shapes ------------------------------------------------------------- */

void gfx_rect(FmRect r, FmColor c);                              /* crisp, no AA */
void gfx_rrect(FmRect r, float radius, FmColor c);
void gfx_rrect4(FmRect r, float tl, float tr, float br, float bl, FmColor c);
void gfx_rrect_vgrad(FmRect r, float radius, FmColor top, FmColor bottom);
void gfx_rrect_line(FmRect r, float radius, float thick, FmColor c);
void gfx_shadow(FmRect r, float radius, float blur, FmColor c);   /* soft drop shadow */
/* How gfx_shadow draws: 0 soft, 1 nothing, 2 a solid copy offset by dx,dy
** (design px, scaled with the blur size). Set by the theme. */
void gfx_shadow_style(int kind, float dx, float dy);
void gfx_circle(float cx, float cy, float radius, FmColor c);
void gfx_ring(float cx, float cy, float radius, float thick, FmColor c);
/* Arc from a0 to a1 (radians, 0 = right, clockwise on screen). */
void gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1, FmColor c);
void gfx_line(float x0, float y0, float x1, float y1, float thick, FmColor c);
/* Round-capped, round-joined polyline. xy = x0,y0,x1,y1,... */
void gfx_polyline(const float *xy, int n, float thick, FmColor c, bool closed);
/* Convex polygon fill with AA edge. */
void gfx_convex(const float *xy, int n, FmColor c);
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, FmColor c);

/* ---- textures ----------------------------------------------------------- */

/* src in texture pixels (NULL = whole); tint multiplies (white = as is). */
void gfx_tex(SDL_Texture *t, const FmRect *src, FmRect dst, FmColor tint);
/* Texture drawn with rounded corners (clipped by geometry, not a mask). */
void gfx_tex_rounded(SDL_Texture *t, FmRect dst, float radius, FmColor tint);
/* Raw textured triangles; uv in 0..1. Used by the font module. */
void gfx_quad_uv(SDL_Texture *t, FmRect dst, float u0, float v0, float u1, float v1, FmColor c);
/* A texture with an opaque white texel at (u, v), surrounded by white so
** linear filtering stays white (the glyph atlas). Untextured shapes then
** draw with it and batch together with text. NULL turns it off. */
void gfx_set_white(SDL_Texture *t, float u, float v);
/* Called with the batch texture right before a batch is submitted, so a
** texture filled lazily (the glyph atlas) can upload what it needs. */
void gfx_set_upload_hook(void (*fn)(SDL_Texture *t));

/* ---- clip --------------------------------------------------------------- */

void   gfx_clip_push(FmRect r);     /* intersected with the current clip */
void   gfx_clip_pop(void);
FmRect gfx_clip(void);
bool   gfx_visible(FmRect r);       /* overlaps the current clip */

/* ---- statistics (--perf, debug overlay) --------------------------------- */

typedef struct GfxStats {
  /* the last presented frame */
  int calls, verts, indices;
  int flush_tex, flush_clip, flush_full;   /* why batches were split */
  double present_ms;                       /* time inside SDL_RenderPresent */
  u32 frames;                              /* frames presented since start */
  /* every SDL texture the program owns (see the redirect below) */
  int tex_live, tex_peak;
  size_t tex_bytes, tex_bytes_peak;
  u32 tex_created;
} GfxStats;

void gfx_get_stats(GfxStats *s);

/* Texture accounting in one place: every SDL_CreateTexture and
** SDL_DestroyTexture in files that include this header goes through these,
** which keep a count and the bytes of the live textures. They behave
** exactly like the SDL calls. fgfx.c defines FGFX_IMPL to reach SDL. */
SDL_Texture *gfx_texture_create(SDL_Renderer *ren, Uint32 format, int access, int w, int h);
void gfx_texture_destroy(SDL_Texture *t);
int  gfx_texture_query(SDL_Texture *t, Uint32 *format, int *access, int *w, int *h);
#ifndef FGFX_IMPL
#  undef SDL_CreateTexture
#  undef SDL_DestroyTexture
#  undef SDL_QueryTexture
#  define SDL_CreateTexture gfx_texture_create
#  define SDL_DestroyTexture gfx_texture_destroy
#  define SDL_QueryTexture gfx_texture_query
#endif

/* Views: a w x h rectangle of `page` handed out as an SDL_Texture pointer,
** so many small images (thumbnails) share one GPU texture. A view works
** with gfx_tex, gfx_tex_rounded, gfx_quad_uv and SDL_QueryTexture (which
** reports the view's own size); hand it to no other SDL call. Destroying
** a view (gfx_view_free or SDL_DestroyTexture) never touches the page. */
SDL_Texture *gfx_view_new(SDL_Texture *page, int x, int y, int w, int h);
void gfx_view_free(SDL_Texture *view);

#endif
