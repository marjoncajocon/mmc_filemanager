/* fgfx.c -- batched, anti-aliased 2D geometry on SDL_RenderGeometry.
**
** Design decisions:
**   - One vertex/index batch; it is flushed when the texture or the clip
**     changes, when it is full, and at the end of the frame.
**   - Rounded shapes use a fixed number of points per corner chosen from the
**     radius, so inner and outer rings (feather, shadow) pair up 1:1.
*/
#include "fgfx.h"
#include <math.h>

#define MAX_VERTS 16384
#define MAX_IDX   (MAX_VERTS * 3)
#define CLIP_DEPTH 32
#define PI_F 3.14159265f

SDL_Renderer *g_ren;

static SDL_Vertex *g_v;
static int *g_i;
static int g_nv, g_ni;
static SDL_Texture *g_tex;
static int g_calls, g_calls_last;
static int g_w, g_h;
static FmRect g_clip[CLIP_DEPTH];
static int g_nclip;

/* ---- colors and rects --------------------------------------------------- */

FmColor col_alpha(FmColor c, float a) {
  float v = c.a * a;
  c.a = (u8)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
  return c;
}

FmColor col_mix(FmColor a, FmColor b, float t) {
  FmColor c;
  c.r = (u8)(a.r + (b.r - a.r) * t);
  c.g = (u8)(a.g + (b.g - a.g) * t);
  c.b = (u8)(a.b + (b.b - a.b) * t);
  c.a = (u8)(a.a + (b.a - a.a) * t);
  return c;
}

FmRect rect_inset(FmRect r, float d) { return rect_inset2(r, d, d); }

FmRect rect_inset2(FmRect r, float dx, float dy) {
  r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy;
  if (r.w < 0) r.w = 0;
  if (r.h < 0) r.h = 0;
  return r;
}

bool rect_has(FmRect r, float x, float y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

FmRect rect_intersect(FmRect a, FmRect b) {
  float x0 = FM_MAX(a.x, b.x), y0 = FM_MAX(a.y, b.y);
  float x1 = FM_MIN(a.x + a.w, b.x + b.w), y1 = FM_MIN(a.y + a.h, b.y + b.h);
  FmRect r = { x0, y0, x1 - x0, y1 - y0 };
  if (r.w < 0) r.w = 0;
  if (r.h < 0) r.h = 0;
  return r;
}

FmRect rect_cut_top(FmRect *r, float h) {
  h = FM_MIN(h, r->h);
  FmRect s = { r->x, r->y, r->w, h };
  r->y += h; r->h -= h;
  return s;
}

FmRect rect_cut_bottom(FmRect *r, float h) {
  h = FM_MIN(h, r->h);
  FmRect s = { r->x, r->y + r->h - h, r->w, h };
  r->h -= h;
  return s;
}

FmRect rect_cut_left(FmRect *r, float w) {
  w = FM_MIN(w, r->w);
  FmRect s = { r->x, r->y, w, r->h };
  r->x += w; r->w -= w;
  return s;
}

FmRect rect_cut_right(FmRect *r, float w) {
  w = FM_MIN(w, r->w);
  FmRect s = { r->x + r->w - w, r->y, w, r->h };
  r->w -= w;
  return s;
}

FmRect rect_center(FmRect o, float w, float h) {
  FmRect r = { o.x + (o.w - w) * 0.5f, o.y + (o.h - h) * 0.5f, w, h };
  return r;
}

/* ---- batch -------------------------------------------------------------- */

void gfx_init(SDL_Renderer *ren) {
  g_ren = ren;
  if (!g_v) {
    g_v = (SDL_Vertex *)fm_alloc(sizeof(SDL_Vertex) * MAX_VERTS);
    g_i = (int *)fm_alloc(sizeof(int) * MAX_IDX);
  }
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
}

void gfx_shutdown(void) {
  fm_free(g_v); fm_free(g_i);
  g_v = NULL; g_i = NULL;
}

void gfx_flush(void) {
  if (g_ni > 0) {
    SDL_RenderGeometry(g_ren, g_tex, g_v, g_nv, g_i, g_ni);
    g_calls++;
  }
  g_nv = g_ni = 0;
}

static void apply_clip(void) {
  if (g_nclip == 0) { SDL_RenderSetClipRect(g_ren, NULL); return; }
  FmRect c = g_clip[g_nclip - 1];
  SDL_Rect r = { (int)floorf(c.x), (int)floorf(c.y), (int)ceilf(c.w), (int)ceilf(c.h) };
  if (r.w <= 0 || r.h <= 0) { r.w = 1; r.h = 1; r.x = -10; r.y = -10; }
  SDL_RenderSetClipRect(g_ren, &r);
}

void gfx_begin(int w, int h, FmColor clear) {
  g_w = w; g_h = h;
  g_nclip = 0;
  g_calls = 0;
  g_tex = NULL;
  SDL_RenderSetClipRect(g_ren, NULL);
  SDL_SetRenderDrawColor(g_ren, clear.r, clear.g, clear.b, 255);
  SDL_RenderClear(g_ren);
}

void gfx_end(void) {
  gfx_flush();
  g_calls_last = g_calls;
  SDL_RenderPresent(g_ren);
}

int gfx_draw_calls(void) { return g_calls_last; }

/* Reserves nv vertices and ni indices for texture t; returns base vertex. */
static int reserve(SDL_Texture *t, int nv, int ni) {
  if (t != g_tex || g_nv + nv > MAX_VERTS || g_ni + ni > MAX_IDX) {
    gfx_flush();
    g_tex = t;
  }
  return g_nv;
}

static inline void vtx(float x, float y, FmColor c, float u, float v) {
  SDL_Vertex *p = &g_v[g_nv++];
  p->position.x = x; p->position.y = y;
  p->color.r = c.r; p->color.g = c.g; p->color.b = c.b; p->color.a = c.a;
  p->tex_coord.x = u; p->tex_coord.y = v;
}

static inline void tri(int a, int b, int c) {
  g_i[g_ni++] = a; g_i[g_ni++] = b; g_i[g_ni++] = c;
}

static bool culled(float x, float y, float w, float h) {
  if (x > g_w || y > g_h || x + w < 0 || y + h < 0) return true;
  if (g_nclip) {
    FmRect c = g_clip[g_nclip - 1];
    if (x > c.x + c.w || y > c.y + c.h || x + w < c.x || y + h < c.y) return true;
  }
  return false;
}

/* ---- clip --------------------------------------------------------------- */

void gfx_clip_push(FmRect r) {
  if (g_nclip > 0) r = rect_intersect(r, g_clip[g_nclip - 1]);
  gfx_flush();
  if (g_nclip < CLIP_DEPTH) g_clip[g_nclip++] = r;
  apply_clip();
}

void gfx_clip_pop(void) {
  gfx_flush();
  if (g_nclip > 0) g_nclip--;
  apply_clip();
}

FmRect gfx_clip(void) {
  if (g_nclip) return g_clip[g_nclip - 1];
  FmRect r = { 0, 0, (float)g_w, (float)g_h };
  return r;
}

bool gfx_visible(FmRect r) { return !culled(r.x, r.y, r.w, r.h); }

/* ---- generic convex fill with feather ----------------------------------- */

#define MAX_PTS 512

/* Fills a convex polygon (any winding). Per-vertex colors optional. */
static void fill_convex(const float *xy, const FmColor *cols, FmColor c, int n,
                        SDL_Texture *t, FmRect uvr) {
  if (n < 3) return;
  float area = 0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
  }
  float s = area > 0 ? 1.0f : -1.0f;   /* outward normal sign */
  int base = reserve(t, n * 2, (n - 2) * 3 + n * 6);
  for (int i = 0; i < n; i++) {
    int p = (i + n - 1) % n, q = (i + 1) % n;
    float x = xy[2 * i], y = xy[2 * i + 1];
    float e0x = x - xy[2 * p], e0y = y - xy[2 * p + 1];
    float e1x = xy[2 * q] - x, e1y = xy[2 * q + 1] - y;
    float l0 = sqrtf(e0x * e0x + e0y * e0y), l1 = sqrtf(e1x * e1x + e1y * e1y);
    if (l0 < 1e-6f) l0 = 1; if (l1 < 1e-6f) l1 = 1;
    /* edge normals (right-hand for positive area in y-down space) */
    float n0x = e0y / l0 * s, n0y = -e0x / l0 * s;
    float n1x = e1y / l1 * s, n1y = -e1x / l1 * s;
    float nx = n0x + n1x, ny = n0y + n1y;
    float nl = nx * nx + ny * ny;
    if (nl < 1e-6f) { nx = n0x; ny = n0y; } else {
      float d = (nx * n0x + ny * n0y) / sqrtf(nl);
      float k = d > 0.25f ? 1.0f / d : 4.0f;
      nl = sqrtf(nl);
      nx = nx / nl * k; ny = ny / nl * k;
    }
    FmColor ci = cols ? cols[i] : c;
    FmColor co = ci; co.a = 0;
    float ix = x - nx * 0.5f, iy = y - ny * 0.5f;
    float ox = x + nx * 0.5f, oy = y + ny * 0.5f;
    float u = 0, v = 0, uo = 0, vo = 0;
    if (t) {
      u = (ix - uvr.x) / uvr.w; v = (iy - uvr.y) / uvr.h;
      uo = (ox - uvr.x) / uvr.w; vo = (oy - uvr.y) / uvr.h;
    }
    vtx(ix, iy, ci, u, v);
    vtx(ox, oy, co, uo, vo);
  }
  for (int i = 1; i < n - 1; i++) tri(base, base + 2 * i, base + 2 * (i + 1));
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    int a = base + 2 * i, b = base + 2 * j;
    tri(a, a + 1, b + 1);
    tri(a, b + 1, b);
  }
}

void gfx_convex(const float *xy, int n, FmColor c) {
  if (n > MAX_PTS) n = MAX_PTS;
  FmRect none = { 0, 0, 1, 1 };
  fill_convex(xy, NULL, c, n, NULL, none);
}

void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, FmColor c) {
  float xy[6] = { x0, y0, x1, y1, x2, y2 };
  gfx_convex(xy, 3, c);
}

/* ---- rectangles --------------------------------------------------------- */

void gfx_rect(FmRect r, FmColor c) {
  if (c.a == 0 || r.w <= 0 || r.h <= 0 || culled(r.x, r.y, r.w, r.h)) return;
  int b = reserve(NULL, 4, 6);
  vtx(r.x, r.y, c, 0, 0);
  vtx(r.x + r.w, r.y, c, 0, 0);
  vtx(r.x + r.w, r.y + r.h, c, 0, 0);
  vtx(r.x, r.y + r.h, c, 0, 0);
  tri(b, b + 1, b + 2);
  tri(b, b + 2, b + 3);
}

static int corner_segs(float r) {
  int n = (int)(r * 0.35f) + 2;
  return n > 12 ? 12 : n;
}

/* Points of a rounded rectangle, clockwise from the top-left corner. */
static int rrect_pts(float *xy, FmRect r, float tl, float tr, float br, float bl, int segs) {
  float rad[4] = { tl, tr, br, bl };
  float maxr = FM_MIN(r.w, r.h) * 0.5f;
  for (int k = 0; k < 4; k++) rad[k] = FM_CLAMP(rad[k], 0, maxr);
  float cx[4] = { r.x + rad[0], r.x + r.w - rad[1], r.x + r.w - rad[2], r.x + rad[3] };
  float cy[4] = { r.y + rad[0], r.y + rad[1], r.y + r.h - rad[2], r.y + r.h - rad[3] };
  float a0[4] = { PI_F, PI_F * 1.5f, 0, PI_F * 0.5f };
  int n = 0;
  for (int k = 0; k < 4; k++) {
    for (int i = 0; i <= segs; i++) {
      float a = a0[k] + (PI_F * 0.5f) * (float)i / (float)segs;
      xy[2 * n] = cx[k] + cosf(a) * rad[k];
      xy[2 * n + 1] = cy[k] + sinf(a) * rad[k];
      n++;
    }
  }
  return n;
}

void gfx_rrect4(FmRect r, float tl, float tr, float br, float bl, FmColor c) {
  if (c.a == 0 || r.w <= 0 || r.h <= 0 || culled(r.x - 1, r.y - 1, r.w + 2, r.h + 2)) return;
  float xy[2 * 4 * 13];
  int segs = corner_segs(FM_MAX(FM_MAX(tl, tr), FM_MAX(br, bl)));
  int n = rrect_pts(xy, r, tl, tr, br, bl, segs);
  FmRect none = { 0, 0, 1, 1 };
  fill_convex(xy, NULL, c, n, NULL, none);
}

void gfx_rrect(FmRect r, float radius, FmColor c) {
  if (radius < 0.5f) {
    /* still feathered so sub-pixel positions look smooth */
    gfx_rrect4(r, 0, 0, 0, 0, c);
    return;
  }
  gfx_rrect4(r, radius, radius, radius, radius, c);
}

void gfx_rrect_vgrad(FmRect r, float radius, FmColor top, FmColor bottom) {
  if (r.w <= 0 || r.h <= 0 || culled(r.x, r.y, r.w, r.h)) return;
  float xy[2 * 4 * 13];
  FmColor cols[4 * 13];
  int n = rrect_pts(xy, r, radius, radius, radius, radius, corner_segs(radius));
  for (int i = 0; i < n; i++) cols[i] = col_mix(top, bottom, (xy[2 * i + 1] - r.y) / r.h);
  FmRect none = { 0, 0, 1, 1 };
  fill_convex(xy, cols, top, n, NULL, none);
}

/* Ring between two point loops of equal count; alpha per loop. */
static void ring_strip(const float *a, FmColor ca, const float *b, FmColor cb, int n) {
  int base = reserve(NULL, n * 2, n * 6);
  for (int i = 0; i < n; i++) {
    vtx(a[2 * i], a[2 * i + 1], ca, 0, 0);
    vtx(b[2 * i], b[2 * i + 1], cb, 0, 0);
  }
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    int p = base + 2 * i, q = base + 2 * j;
    tri(p, p + 1, q + 1);
    tri(p, q + 1, q);
  }
}

void gfx_rrect_line(FmRect r, float radius, float thick, FmColor c) {
  if (c.a == 0 || culled(r.x - 1, r.y - 1, r.w + 2, r.h + 2)) return;
  if (thick < 1.0f) { c = col_alpha(c, thick); thick = 1.0f; }
  float h = thick * 0.5f;
  int segs = corner_segs(radius + h);
  float p0[2 * 4 * 13], p1[2 * 4 * 13], p2[2 * 4 * 13], p3[2 * 4 * 13];
  FmRect r0 = rect_inset(r, -h - 0.5f), r1 = rect_inset(r, -h + 0.5f);
  FmRect r2 = rect_inset(r, h - 0.5f), r3 = rect_inset(r, h + 0.5f);
  float rr0 = radius + h + 0.5f, rr1 = radius + h - 0.5f;
  float rr2 = FM_MAX(radius - h + 0.5f, 0), rr3 = FM_MAX(radius - h - 0.5f, 0);
  int n = rrect_pts(p0, r0, rr0, rr0, rr0, rr0, segs);
  rrect_pts(p1, r1, rr1, rr1, rr1, rr1, segs);
  rrect_pts(p2, r2, rr2, rr2, rr2, rr2, segs);
  rrect_pts(p3, r3, rr3, rr3, rr3, rr3, segs);
  FmColor z = c; z.a = 0;
  ring_strip(p0, z, p1, c, n);
  if (thick > 1.0f) ring_strip(p1, c, p2, c, n);
  ring_strip(p2, c, p3, z, n);
}

void gfx_shadow(FmRect r, float radius, float blur, FmColor c) {
  if (c.a == 0 || culled(r.x - blur, r.y - blur, r.w + 2 * blur, r.h + 2 * blur)) return;
  int segs = corner_segs(radius + blur);
  float in[2 * 4 * 13], mid[2 * 4 * 13], out[2 * 4 * 13];
  /* Two-step falloff looks close to a gaussian. */
  FmRect ri = rect_inset(r, blur * 0.35f);
  FmRect rm = rect_inset(r, -blur * 0.3f);
  FmRect ro = rect_inset(r, -blur);
  float radi = FM_MAX(radius - blur * 0.35f, 0);
  int n = rrect_pts(in, ri, radi, radi, radi, radi, segs);
  rrect_pts(mid, rm, radius + blur * 0.3f, radius + blur * 0.3f, radius + blur * 0.3f,
            radius + blur * 0.3f, segs);
  rrect_pts(out, ro, radius + blur, radius + blur, radius + blur, radius + blur, segs);
  FmColor cm = col_alpha(c, 0.45f), z = c; z.a = 0;
  /* center */
  int base = reserve(NULL, n, (n - 2) * 3);
  for (int i = 0; i < n; i++) vtx(in[2 * i], in[2 * i + 1], c, 0, 0);
  for (int i = 1; i < n - 1; i++) tri(base, base + i, base + i + 1);
  ring_strip(in, c, mid, cm, n);
  ring_strip(mid, cm, out, z, n);
}

/* ---- circles and arcs --------------------------------------------------- */

static int circle_segs(float r) {
  int n = (int)(r * 1.2f) + 8;
  return n > 96 ? 96 : n;
}

void gfx_circle(float cx, float cy, float radius, FmColor c) {
  if (c.a == 0 || culled(cx - radius - 1, cy - radius - 1, radius * 2 + 2, radius * 2 + 2)) return;
  float xy[2 * 96];
  int n = circle_segs(radius);
  for (int i = 0; i < n; i++) {
    float a = 2 * PI_F * (float)i / (float)n;
    xy[2 * i] = cx + cosf(a) * radius;
    xy[2 * i + 1] = cy + sinf(a) * radius;
  }
  FmRect none = { 0, 0, 1, 1 };
  fill_convex(xy, NULL, c, n, NULL, none);
}

void gfx_arc(float cx, float cy, float radius, float thick, float a0, float a1, FmColor c) {
  if (c.a == 0 || culled(cx - radius - thick, cy - radius - thick, 2 * (radius + thick),
                         2 * (radius + thick)))
    return;
  if (thick < 1.0f) { c = col_alpha(c, thick); thick = 1.0f; }
  float span = a1 - a0;
  int n = (int)(fabsf(span) / (2 * PI_F) * (float)circle_segs(radius + thick)) + 2;
  if (n > 200) n = 200;
  float h = thick * 0.5f;
  float rr[4] = { radius - h - 0.5f, radius - h + 0.5f, radius + h - 0.5f, radius + h + 0.5f };
  if (rr[0] < 0) rr[0] = 0;
  if (rr[1] < 0) rr[1] = 0;
  FmColor z = c; z.a = 0;
  FmColor cc[4] = { z, c, c, z };
  int base = reserve(NULL, (n + 1) * 4, n * 18);
  for (int i = 0; i <= n; i++) {
    float a = a0 + span * (float)i / (float)n;
    float ca = cosf(a), sa = sinf(a);
    for (int k = 0; k < 4; k++) vtx(cx + ca * rr[k], cy + sa * rr[k], cc[k], 0, 0);
  }
  for (int i = 0; i < n; i++) {
    int p = base + i * 4, q = p + 4;
    for (int k = 0; k < 3; k++) {
      tri(p + k, q + k, q + k + 1);
      tri(p + k, q + k + 1, p + k + 1);
    }
  }
}

void gfx_ring(float cx, float cy, float radius, float thick, FmColor c) {
  gfx_arc(cx, cy, radius, thick, 0, 2 * PI_F, c);
}

/* ---- lines -------------------------------------------------------------- */

void gfx_line(float x0, float y0, float x1, float y1, float thick, FmColor c) {
  if (c.a == 0) return;
  float minx = FM_MIN(x0, x1), miny = FM_MIN(y0, y1);
  if (culled(minx - thick, miny - thick, fabsf(x1 - x0) + 2 * thick, fabsf(y1 - y0) + 2 * thick))
    return;
  if (thick < 1.0f) { c = col_alpha(c, thick); thick = 1.0f; }
  float dx = x1 - x0, dy = y1 - y0;
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 1e-4f) return;
  float nx = -dy / l, ny = dx / l;
  float h = thick * 0.5f;
  float o[4] = { -h - 0.5f, -h + 0.5f, h - 0.5f, h + 0.5f };
  FmColor z = c; z.a = 0;
  FmColor cc[4] = { z, c, c, z };
  int base = reserve(NULL, 8, 18);
  for (int k = 0; k < 4; k++) vtx(x0 + nx * o[k], y0 + ny * o[k], cc[k], 0, 0);
  for (int k = 0; k < 4; k++) vtx(x1 + nx * o[k], y1 + ny * o[k], cc[k], 0, 0);
  for (int k = 0; k < 3; k++) {
    tri(base + k, base + 4 + k, base + 4 + k + 1);
    tri(base + k, base + 4 + k + 1, base + k + 1);
  }
}

void gfx_polyline(const float *xy, int n, float thick, FmColor c, bool closed) {
  if (n < 2) return;
  int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; i++) {
    int j = (i + 1) % n;
    gfx_line(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], thick, c);
  }
  /* round joins and caps */
  if (thick >= 2.0f) {
    for (int i = 0; i < n; i++) gfx_circle(xy[2 * i], xy[2 * i + 1], thick * 0.5f, c);
  }
}

/* ---- textures ----------------------------------------------------------- */

void gfx_quad_uv(SDL_Texture *t, FmRect d, float u0, float v0, float u1, float v1, FmColor c) {
  if (culled(d.x, d.y, d.w, d.h)) return;
  int b = reserve(t, 4, 6);
  vtx(d.x, d.y, c, u0, v0);
  vtx(d.x + d.w, d.y, c, u1, v0);
  vtx(d.x + d.w, d.y + d.h, c, u1, v1);
  vtx(d.x, d.y + d.h, c, u0, v1);
  tri(b, b + 1, b + 2);
  tri(b, b + 2, b + 3);
}

void gfx_tex(SDL_Texture *t, const FmRect *src, FmRect dst, FmColor tint) {
  if (!t) return;
  int tw, th;
  SDL_QueryTexture(t, NULL, NULL, &tw, &th);
  if (tw <= 0 || th <= 0) return;
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  if (src) {
    u0 = src->x / tw; v0 = src->y / th;
    u1 = (src->x + src->w) / tw; v1 = (src->y + src->h) / th;
  }
  gfx_quad_uv(t, dst, u0, v0, u1, v1, tint);
}

void gfx_tex_rounded(SDL_Texture *t, FmRect dst, float radius, FmColor tint) {
  if (!t || culled(dst.x, dst.y, dst.w, dst.h)) return;
  float xy[2 * 4 * 13];
  int n = rrect_pts(xy, rect_inset(dst, 0.5f), radius, radius, radius, radius, corner_segs(radius));
  fill_convex(xy, NULL, tint, n, t, dst);
}
