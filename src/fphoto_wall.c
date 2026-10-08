/* fphoto_wall.c -- the justified photo wall, like Google Photos.
**
** Design decisions:
**   - Rows of photos at their real aspect ratios, each row scaled so it
**     fills the width exactly. A row takes photos while its height stays
**     above the target (about 180 dp on desktops, 120 dp on phones) and then
**     keeps or drops the last one, whichever lands closer to the target in
**     ratio terms. The last row is only justified when it is nearly full.
**   - The rows are a flat array laid out once (O(n), a few microseconds for
**     a thousand photos) and again only when the width, the count or an
**     aspect changes. Drawing touches the visible rows only, found by binary
**     search, so a frame costs the same at any result count.
**   - A photo of unknown size counts as 4:3 until its thumbnail arrives;
**     then its real aspect is learned and the rows are laid out again,
**     anchored to the first visible photo so what is on screen does not
**     jump. Relayouts are throttled while thumbnails pour in.
**   - Thumbnails come from the shared loader (fonline_thumb.c) without a
**     crop, decoded at the size the tile is drawn. Only visible tiles ask
**     for them, plus a prefetch of the next screen at low priority; the
**     loader drops whatever scrolled away. Until then the tile shows the
**     photo's dominant colour, then the picture fades in.
**   - Selection follows Google Photos: a long press (or the round check on
**     hover) starts it, taps then toggle, shift-click selects a range, and
**     selected tiles shrink a little inside an accent frame.
*/
#include "fphoto_int.h"
#include "fview_int.h"

#define AR_UNKNOWN (4.0f / 3.0f)
#define RELAYOUT_MS 120

/* ---- items ------------------------------------------------------------------- */

void ph_item_init(PhItem *p, const FmPsrcItem *it, const char *src) {
  memset(p, 0, sizeof *p);
  p->it = *it;
  /* the item names its source (psrc_fetch finds the site and its headers by it) */
  if (!p->it.source[0] && src) fm_strlcpy(p->it.source, src, sizeof p->it.source);
  fm_strlcpy(p->src, p->it.source, sizeof p->src);
  if (it->width > 0 && it->height > 0) p->ar = (float)it->width / (float)it->height;
}

const FmPsrc *ph_src(const PhItem *p) { return p && p->src[0] ? psrc_find(p->src) : NULL; }

const char *ph_src_name(const char *key) {
  const FmPsrc *s = key && key[0] ? psrc_find(key) : NULL;
  return s ? s->name : (key ? key : "");
}

/* ---- the list ---------------------------------------------------------------- */

void wall_init(PhWall *w, const char *name) {
  memset(w, 0, sizeof *w);
  w->id = ui_id(name);
  w->anchor = -1;
}

void wall_free(PhWall *w) {
  fm_free(w->items);
  fm_free(w->rows);
  u32 id = w->id;
  memset(w, 0, sizeof *w);
  w->id = id;
  w->anchor = -1;
}

void wall_clear(PhWall *w) {
  w->n = 0;
  w->nrows = 0;
  w->content = 0;
  w->lay_n = -1;
  w->anchor = -1;
  memset(&w->scroll, 0, sizeof w->scroll);
}

PhItem *wall_push(PhWall *w) {
  if (w->n == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 64;
    w->items = (PhItem *)fm_realloc(w->items, sizeof *w->items * (size_t)w->cap);
  }
  PhItem *p = &w->items[w->n++];
  memset(p, 0, sizeof *p);
  return p;
}

int wall_find(const PhWall *w, const char *src, const char *id) {
  for (int i = 0; i < w->n; i++)
    if (!strcmp(w->items[i].it.id, id) && !strcmp(w->items[i].src, src)) return i;
  return -1;
}

int wall_nsel(const PhWall *w) {
  int n = 0;
  for (int i = 0; i < w->n; i++) n += w->items[i].sel;
  return n;
}

void wall_select_all(PhWall *w, bool on) {
  for (int i = 0; i < w->n; i++) w->items[i].sel = on;
  if (!on) w->anchor = -1;
  ui_redraw();
}

/* ---- layout ------------------------------------------------------------------------ */

typedef float (*ArFn)(const void *ctx, int i);

static float clamp_ar(float a) { return FM_CLAMP(a, 0.42f, 3.2f); }

static float item_ar(const void *ctx, int i) {
  const PhItem *p = &((const PhWall *)ctx)->items[i];
  return clamp_ar(p->ar > 0 ? p->ar : AR_UNKNOWN);
}

/* A fixed pseudo-random mix of shapes for the loading placeholders. */
static const float kSkelAr[] = { 1.5f, 0.75f, 1.33f, 1.0f, 1.78f, 0.67f, 1.5f, 1.25f, 0.8f, 1.6f, 1.33f, 0.75f };
static float skel_ar(const void *ctx, int i) {
  FM_UNUSED(ctx);
  return kSkelAr[i % FM_COUNT(kSkelAr)];
}

/* Lays n photos out in rows; returns the height (padding included). */
static float justify(PhRow **rows, int *nrows, int *rcap, ArFn ar, const void *ctx, int n, float W, float target,
                     float gap, float pad) {
  float y = pad;
  int i = 0;
  *nrows = 0;
  while (i < n) {
    float sum = 0, h = target;
    int cnt = 0;
    bool full = false;
    for (int j = i; j < n; j++) {
      float a = ar(ctx, j);
      float hn = (W - gap * (float)cnt) / (sum + a);
      if (hn <= target) {
        if (cnt > 0) {
          float hp = (W - gap * (float)(cnt - 1)) / sum;
          if (fabsf(logf(hp / target)) < fabsf(logf(hn / target))) { h = hp; full = true; break; }
        }
        sum += a;
        cnt++;
        h = hn;
        full = true;
        break;
      }
      sum += a;
      cnt++;
    }
    if (!full) {
      /* the last row: justified only when nearly full */
      float hj = (W - gap * (float)(cnt - 1)) / sum;
      if (hj < target * 1.22f) { h = hj; full = true; }
      else h = target;
    }
    if (*nrows == *rcap) {
      *rcap = *rcap ? *rcap * 2 : 64;
      *rows = (PhRow *)fm_realloc(*rows, sizeof **rows * (size_t)*rcap);
    }
    PhRow *r = &(*rows)[(*nrows)++];
    r->first = i;
    r->count = cnt;
    r->y = y;
    r->h = floorf(h + 0.5f);
    r->full = full;
    y += r->h + gap;
    i += cnt;
  }
  return n ? y - gap + pad : pad * 2;
}

static float array_ar(const void *ctx, int i) { return clamp_ar(((const float *)ctx)[i]); }

int wall_justify(const float *ars, int n, float W, float target, float gap, PhRow *out, int max) {
  PhRow *rows = NULL;
  int nrows = 0, rcap = 0;
  justify(&rows, &nrows, &rcap, array_ar, ars, n, W, target, gap, 0);
  int k = FM_MIN(nrows, max);
  if (k > 0) memcpy(out, rows, sizeof *out * (size_t)k);
  fm_free(rows);
  return k;
}

static int row_of_item(const PhWall *w, int item) {
  int lo = 0, hi = w->nrows - 1;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (w->rows[mid].first <= item) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

/* First row whose bottom is below y. */
static int row_at_y(const PhRow *rows, int nrows, float y) {
  int lo = 0, hi = nrows;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (rows[mid].y + rows[mid].h < y) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

float wall_pad(float view_w) { return DP(view_w < DP(520) ? 8 : 16); }

static float wall_gap(float view_w) { return DP(view_w < DP(520) ? 3 : 6); }

static float wall_target(float view_w) {
  bool phone = view_w < DP(600) || (ui.portrait && ui.touch_mode);
  return DP(phone ? 120 : 180);
}

static void relayout(PhWall *w, float W, float target, float gap, float pad) {
  /* anchor: the first photo of the first visible row keeps its place */
  int anchor = -1;
  float off = 0;
  if (w->nrows > 0 && w->scroll.y > 0) {
    int r = row_at_y(w->rows, w->nrows, w->scroll.y);
    if (r < w->nrows) {
      anchor = w->rows[r].first;
      off = w->rows[r].y - w->scroll.y;
    }
  }
  w->content = justify(&w->rows, &w->nrows, &w->rcap, item_ar, w, w->n, W, target, gap, pad);
  w->lay_w = W;
  w->lay_target = target;
  w->lay_n = w->n;
  w->dirty = false;
  if (anchor >= 0 && w->nrows > 0) {
    int r = row_of_item(w, anchor);
    w->scroll.y = FM_MAX(0.0f, w->rows[r].y - off);
  }
}

/* ---- tiles --------------------------------------------------------------------------- */

float ph_shimmer(int k) {
  return 0.5f + 0.5f * sinf((float)(ui.now % 100000) / 1600.0f * 6.2831853f - (float)k * 0.45f);
}

static FmColor placeholder(const PhItem *p) {
  if (p && p->it.color) return col_mix(T.surface2, FM_HEX(p->it.color), T.dark ? 0.75f : 0.85f);
  return col_mix(T.surface2, T.text, T.dark ? 0.07f : 0.06f);
}

static u64 g_last_learn;

SDL_Texture *ph_tile_picture(FmRect r, PhItem *p, float rad, bool learn_aspect, PhWall *w) {
  /* whole pixels: crisp pictures and texture coordinates inside 0..1 */
  float x1 = floorf(r.x + r.w + 0.5f), y1 = floorf(r.y + r.h + 0.5f);
  r.x = floorf(r.x + 0.5f);
  r.y = floorf(r.y + 0.5f);
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  if (r.w < 1 || r.h < 1) return NULL;
  int px = (int)FM_MAX(r.w, r.h);
  float fade = 1;
  const char *hdr = psrc_item_headers(&p->it);
  SDL_Texture *tex = p->it.thumb[0] ? othumb_get_ex(p->it.thumb, px, 0, hdr, &fade) : NULL;
  if (tex) p->tpx = px;
  if (!tex || fade < 1) gfx_rrect(r, rad, placeholder(p));
  if (tex) {
    if (learn_aspect) {
      int tw = 0, th = 0;
      if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) {
        float a = (float)tw / (float)th;
        if (p->ar <= 0 || fabsf(a - p->ar) > p->ar * 0.03f) {
          p->ar = a;
          if (w) w->dirty = true;
          g_last_learn = ui.now;
        }
      }
    }
    gfx_tex_rounded(tex, r, rad, col_alpha(PH_WHITE, fade));
    if (fade < 1) ui_animate();
  }
  return tex;
}

/* The round check at a tile's top-left corner. */
static void check_mark(float cx, float cy, float rr, bool on, float alpha) {
  if (alpha <= 0.01f) return;
  if (on) {
    gfx_circle(cx, cy, rr + DP(1.5f), col_alpha(T.surface, alpha));
    gfx_circle(cx, cy, rr, col_alpha(T.accent, alpha));
    icon_draw(IC_CHECK, FM_RECT(cx - rr * 0.68f, cy - rr * 0.68f, rr * 1.36f, rr * 1.36f), col_alpha(T.on_accent, alpha));
  } else {
    gfx_circle(cx, cy, rr + DP(1), FM_RGBA(0, 0, 0, (u8)(70 * alpha)));
    gfx_ring(cx, cy, rr - DP(0.5f), DP(2), col_alpha(PH_WHITE, 0.95f * alpha));
  }
}

typedef struct TileCtx {
  PhWall *w;
  int nsel;
  float rad;
  PhWallRes *res;
} TileCtx;

static void select_range(PhWall *w, int a, int b) {
  if (a > b) { int t = a; a = b; b = t; }
  for (int i = a; i <= b; i++) w->items[i].sel = true;
}

static void tile(TileCtx *c, FmRect r, int i) {
  PhWall *w = c->w;
  PhItem *p = &w->items[i];
  u32 id = ui_idn(w->id, (u32)i + 1);
  int f = ui_hit(id, r);
  bool selecting = c->nsel > 0;
  float hv = ui.touch_mode ? 0 : ui_anim(ui_idn(id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 14.0f);
  float st = ui_anim(ui_idn(id, 2), p->sel ? 1.0f : 0.0f, 18.0f);
  float rad = c->rad;
  FmRect pic = r;
  if (st > 0.01f) {
    gfx_rrect(r, rad, col_alpha(T.accent_soft, st));
    float in = DP(r.w < DP(110) ? 7 : 11) * st;
    pic = rect_inset(r, in);
  }
  ph_tile_picture(pic, p, rad * (1.0f - 0.3f * st), true, w);
  if (f & UI_HELD) gfx_rrect(pic, rad, FM_RGBA(0, 0, 0, 40));
  /* hover: the author on a soft scrim, desktop only */
  if (hv > 0.01f && !selecting && r.w >= DP(120) && r.h >= DP(90)) {
    float sh = FM_MIN(r.h * 0.45f, DP(56));
    FmRect sr = { pic.x, pic.y + pic.h - sh, pic.w, sh };
    gfx_rrect_vgrad(sr, rad, FM_RGBA(0, 0, 0, 0), FM_RGBA(0, 0, 0, (u8)(150 * hv)));
    const char *who = p->it.author[0] ? p->it.author : p->it.title;
    float fs = ui.m.font_small;
    font_draw_ellipsis(FONT_BOLD, fs, sr.x + DP(10), sr.y + sh - font_line_h(fs) - DP(7), who, sr.w - DP(20),
                       col_alpha(PH_WHITE, hv));
  }
  /* favourite: a small heart at the bottom left */
  bool in_favs = ph_page() == PG_ALBUM && ph_cur_album() == PALB_FAV;
  if (!in_favs && r.w >= DP(56) && palb_is_fav(p)) {
    float hs = DP(16);
    FmRect hb = { pic.x + DP(7), pic.y + pic.h - hs - DP(7), hs, hs };
    icon_draw(IC_HEART_FILL, rect_inset(hb, -DP(1)), FM_RGBA(0, 0, 0, 90));
    icon_draw(IC_HEART_FILL, hb, PH_WHITE);
  }
  /* the check: always while selecting, on hover otherwise */
  float rr = DP(ui.touch_mode ? 11 : 10);
  float ca = selecting ? 1.0f : hv;
  FmRect cb = { r.x, r.y, rr * 2 + DP(16), rr * 2 + DP(16) };
  float ccx = r.x + DP(8) + rr, ccy = r.y + DP(8) + rr;
  if (st > 0.01f) check_mark(ccx, ccy, rr, true, FM_MAX(st, ca));
  else check_mark(ccx, ccy, rr, false, ca * (selecting ? 0.9f : 0.85f));
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  bool on_check = rect_has(cb, ui.press_x, ui.press_y);
  if (f & UI_LONG) {
    p->sel = !p->sel;
    w->anchor = i;
    ui_redraw();
  } else if (f & UI_CLICK) {
    if (selecting || (on_check && !ui.touch_mode)) {
      if ((ui.mod & KMOD_SHIFT) && w->anchor >= 0 && w->anchor < w->n) select_range(w, w->anchor, i);
      else p->sel = !p->sel;
      w->anchor = i;
      ui_redraw();
    } else {
      c->res->open = i;
    }
  } else if ((f & UI_RCLICK) && !selecting) {
    c->res->menu = i;
    c->res->menu_x = ui.mx;
    c->res->menu_y = ui.my;
  }
}

static void skeleton_tile(FmRect r, float rad, int k) {
  float wv = ph_shimmer(k);
  gfx_rrect(r, rad, col_mix(T.surface2, T.text, (T.dark ? 0.05f : 0.04f) + 0.045f * wv));
}

/* ---- drawing ------------------------------------------------------------------------- */

static PhRow *g_skel_rows;
static int g_skel_n, g_skel_cap;

/* Lays the tiles of one row out from the left; the last one of a full row
** ends exactly at the right edge. */
static void row_tiles(const PhRow *row, ArFn ar, const void *ctx, float x0, float y, float W, float gap, FmRect *out) {
  float x = x0, right = x0 + W;
  float top = floorf(y + 0.5f), bot = floorf(y + row->h + 0.5f);
  for (int k = 0; k < row->count; k++) {
    float wk = ar(ctx, row->first + k) * row->h;
    float a = floorf(x + 0.5f);
    float b = (k == row->count - 1 && row->full) ? floorf(right + 0.5f) : floorf(x + wk + 0.5f);
    out[k] = FM_RECT(a, top, FM_MAX(1.0f, b - a), bot - top);
    x += wk + gap;
  }
}

PhWallRes wall_draw(PhWall *w, FmRect view, const PhWallOpts *o) {
  PhWallRes res;
  memset(&res, 0, sizeof res);
  res.open = res.menu = -1;
  float pad = wall_pad(view.w), gap = wall_gap(view.w);
  float W = view.w - pad * 2, target = wall_target(view.w);
  float rad = DP(view.w < DP(520) ? 6 : 10);
  if (W < DP(40)) return res;
  bool resized = fabsf(w->lay_w - W) > 0.5f || fabsf(w->lay_target - target) > 0.5f || w->lay_n != w->n;
  if (resized || (w->dirty && ui.now - g_last_learn >= RELAYOUT_MS)) relayout(w, W, target, gap, pad);
  else if (w->dirty) view_wake_in(RELAYOUT_MS);
  /* placeholders: a full screen while the first page loads, a row or two
  ** under the photos while the next one does */
  float skel_top = w->n ? w->content - pad + gap : pad;
  float skel_h = 0;
  g_skel_n = 0;
  if (o->skeleton > 0) {
    skel_h = justify(&g_skel_rows, &g_skel_n, &g_skel_cap, skel_ar, NULL, o->skeleton, W, target, gap, 0);
  }
  float content = (w->n ? w->content : 0) + (skel_h > 0 ? skel_h + (w->n ? gap : pad) : 0) + o->footer;
  if (o->fixed) {
    content = view.h;
    w->scroll.y = 0;
  } else {
    ui_scroll(&w->scroll, w->id, view, content);
  }
  float sy = w->scroll.y;
  gfx_clip_push(view);
  TileCtx tc = { w, wall_nsel(w), rad, &res };
  FmRect tiles[64];
  int r0 = row_at_y(w->rows, w->nrows, sy);
  int last_vis = r0 - 1;
  while (last_vis + 1 < w->nrows && view.y + w->rows[last_vis + 1].y - sy <= view.y + view.h) last_vis++;
  /* bottom-up: the loader serves the latest request first, so the top-left
  ** photo, asked for last, arrives first */
  for (int r = last_vis; r >= r0; r--) {
    const PhRow *row = &w->rows[r];
    float y = view.y + row->y - sy;
    int cnt = FM_MIN(row->count, FM_COUNT(tiles));
    PhRow tmp = *row;
    tmp.count = cnt;
    row_tiles(&tmp, item_ar, w, view.x + pad, y, W, gap, tiles);
    for (int k = cnt - 1; k >= 0; k--) tile(&tc, tiles[k], row->first + k);
  }
  /* placeholders */
  for (int r = 0; r < g_skel_n; r++) {
    const PhRow *row = &g_skel_rows[r];
    float y = view.y + skel_top + row->y - sy;
    if (y > view.y + view.h) break;
    if (y + row->h < view.y) continue;
    int cnt = FM_MIN(row->count, FM_COUNT(tiles));
    PhRow tmp = *row;
    tmp.count = cnt;
    row_tiles(&tmp, skel_ar, NULL, view.x + pad, y, W, gap, tiles);
    for (int k = 0; k < cnt; k++) skeleton_tile(tiles[k], rad, row->first + k + r);
  }
  gfx_clip_pop();
  if (!o->fixed) ui_scrollbar(&w->scroll, view, content);
  /* thumbnails for the next screen, so they are there when it scrolls in */
  float ahead = sy + view.h * 2;
  for (int r = last_vis + 1; r >= 0 && r < w->nrows && w->rows[r].y < ahead; r++) {
    const PhRow *row = &w->rows[r];
    for (int k = 0; k < row->count; k++) {
      PhItem *p = &w->items[row->first + k];
      float tw = item_ar(w, row->first + k) * row->h;
      if (p->it.thumb[0])
        othumb_prefetch_ex(p->it.thumb, (int)FM_MAX(tw, row->h), 0, psrc_item_headers(&p->it));
    }
  }
  res.footer_y = view.y + content - o->footer - sy;
  res.near_end = w->n > 0 && sy + view.h * 2.5f >= content;
  if (g_skel_n > 0) view_wake_in(33);       /* gentle shimmer at 30 fps, only while loading */
  return res;
}
