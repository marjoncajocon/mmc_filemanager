/* fphoto_box.c -- the lightbox: one photo over a dark backdrop.
**
** Design decisions:
**   - An in-view lightbox rather than going straight to the image viewer:
**     the viewer needs a downloaded file and pages through files on disk,
**     while the lightbox opens instantly on the thumbnail the wall already
**     holds (scaled up), pages through the search results themselves (also
**     the ones not downloaded, and the next page loads when the end is
**     near), and carries the gallery's actions: favourite, album, download,
**     credit and licence. "Open in viewer" hands the cached full picture to
**     the real image viewer for deep zoom; closing the viewer comes back
**     here.
**   - The full picture is fetched (psrc_fetch, into the cache) and decoded
**     to fit the window on a worker; then it fades in over the thumbnail.
**     Only the current photo has a full texture (a few MB); moving on
**     cancels the fetch and frees it. The neighbours' thumbnails are
**     prefetched, so flipping shows something at once.
**   - Swipe left and right to move, down to close; arrows and keys do the
**     same on desktops. A tap shows or hides the bars.
**   - Zoom (wheel about the cursor, pinch, double tap, + - 0 1, the bar's
**     zoom group) is kept relative to the fitted picture (PhZoom), so a
**     sharper texture swapped in later moves nothing on screen. Zoomed in,
**     a drag pans (clamped so no gap opens) and swipes do nothing; moving
**     to another photo resets it.
**   - The lightbox picture is decoded to fit the window. Zooming past it
**     re-decodes the cached file larger, then (still not enough) fetches
**     the adapter's `original`; both within a pixel budget like the
**     viewer's (32 MP, 16 on phones). One sharp texture at a time, freed
**     with the photo. The original, once here, is what "Open in viewer"
**     shows.
**   - The backdrop is black in every theme, like the image viewer, and the
**     bars use the viewer chrome (fview.c) so both look the same.
**   - Nothing animates while idle: fades, slides and zoom animations run
**     through ui_anim / ui_animate, the loading ring wakes at 30 fps only
**     while a fetch runs, a pending tap is one timer.
*/
#include "fphoto_int.h"
#include "fview.h"
#include "fview_int.h"

enum { BM_VIEWER = 1, BM_INFO, BM_BROWSER, BM_COPY, BM_DOWNLOAD, BM_ZIN, BM_ZOUT, BM_ZFIT, BM_Z100 };

static struct {
  bool open;
  PhWall *w;
  int index;
  PhItem cur;                 /* a copy: the wall may change under the lightbox */
  PhTask *fetch;
  SDL_Texture *full;
  int fw, fh;
  u64 t_full;
  char path[FM_PATH_MAX];     /* the cached full picture */
  bool fail;
  char err[200];
  bool want_viewer;
  int want_zoom;              /* BM_Z* picked in the menu, done in the frame */
  bool info;
  FmViewChrome chrome;
  bool dragging, drag_axis_set, drag_vertical;
  float drag_x, drag_y;
  u32 id;
  /* zoom */
  PhZoom z;
  bool zanim;
  float z_from, z_to, z_ax, z_ay, z_px0, z_py0;
  u64 z_t0;
  bool pinching, panning;
  float last_mx, last_my;
  bool tap_pending, ignore_click;
  u64 tap_t;
  u64 badge_until;            /* the percent badge (pinch, wheel, narrow bars) */
  /* the sharper picture for zooming */
  PhTask *hi;
  bool hi_orig;               /* hi fetches the original (not a larger decode) */
  bool redecode_tried, orig_tried;
  int nat_w, nat_h;           /* size of the file B.full came from */
  char hi_path[FM_PATH_MAX];  /* the original, once fetched */
  bool demo;                  /* --demo-photos-state zoom: log the swaps (scripted check) */
} B;

static u32 ID_BMENU;

/* ---- loading ---------------------------------------------------------------- */

static void drop_full(void) {
  if (B.fetch) { ptask_drop(B.fetch); B.fetch = NULL; }
  if (B.hi) { ptask_drop(B.hi); B.hi = NULL; }
  if (B.full) SDL_DestroyTexture(B.full);
  B.full = NULL;
  B.fw = B.fh = 0;
  B.nat_w = B.nat_h = 0;
  B.redecode_tried = B.orig_tried = false;
  B.hi_path[0] = 0;
  B.t_full = 0;
  B.path[0] = 0;
  B.fail = false;
  B.err[0] = 0;
  B.want_viewer = false;
}

static int thumb_px(const PhItem *p) { return p->tpx > 0 ? p->tpx : 512; }

static void prefetch_near(void) {
  PhWall *w = B.w;
  for (int d = -1; d <= 2; d++) {
    int i = B.index + d;
    if (d == 0 || i < 0 || i >= w->n) continue;
    const PhItem *q = &w->items[i];
    if (q->it.thumb[0]) othumb_prefetch_ex(q->it.thumb, thumb_px(q), 0, psrc_item_headers(&q->it));
  }
}

static void load_cur(void) {
  drop_full();
  PhTask *t = ptask_new(PT_FETCH, ph_src(&B.cur));
  t->item = B.cur.it;
  int m = (int)FM_MAX(ui.w, ui.h);
  t->max_px = FM_CLAMP(m, 1024, FM_MIN(2560, view_max_texture()));
  if (!ptask_run(t)) {
    B.fail = true;
    fm_strlcpy(B.err, t->errtext, sizeof B.err);
    ptask_free(t, false);
    return;
  }
  B.fetch = t;
}

static void zoom_reset(void) {
  memset(&B.z, 0, sizeof B.z);
  B.z.s = 1;
  B.zanim = B.pinching = B.panning = false;
  B.tap_pending = B.ignore_click = false;
  B.badge_until = 0;
  B.want_zoom = 0;
}

static void go(int index) {
  PhWall *w = B.w;
  if (!w || index < 0 || index >= w->n) return;
  B.index = index;
  zoom_reset();
  B.cur = w->items[index];
  load_cur();
  prefetch_near();
  if (index >= w->n - 3) ph_need_more(w);
  ui_redraw();
}

void box_open(PhWall *w, int index) {
  if (!w || index < 0 || index >= w->n) return;
  if (!ID_BMENU) ID_BMENU = ui_id("ph.box.menu");
  B.open = true;
  B.w = w;
  B.id = ui_id("ph.box");
  ui_anim_set(ui_idn(B.id, 1), 0.0f);
  B.dragging = false;
  B.drag_x = B.drag_y = 0;
  memset(&B.chrome, 0, sizeof B.chrome);
  view_chrome_poke(&B.chrome);
  ui_focus(0);
  go(index);
}

void box_close(void) {
  if (!B.open) return;
  drop_full();
  B.open = false;
  B.w = NULL;
  ui_redraw();
}

bool box_is_open(void) { return B.open; }
void box_set_info(bool on) { B.info = on; }

void box_reset(void) {
  /* device lost: the picture comes back from the cache (sharper again when zoomed) */
  if (B.full) {
    SDL_DestroyTexture(B.full);
    B.full = NULL;
  }
  if (B.open && !B.fetch) load_cur();
}

void box_demo_zoom(float s) {
  if (!B.open) return;
  zoom_reset();
  B.z.s = FM_CLAMP(s, 1.0f, 32.0f);
  B.demo = true;
  ui_redraw();
}

void box_shutdown(void) {
  drop_full();
  memset(&B, 0, sizeof B);
}

static void open_viewer(void) {
  if (B.hi_path[0] && !B.hi) {
    /* zoomed in far enough to fetch the original: the viewer gets that */
    B.want_viewer = false;
    image_view_title(B.cur.it.title[0] ? B.cur.it.title : NULL);
    app_open(B.hi_path, NULL, 0, 0);
    image_view_title(NULL);
    return;
  }
  if (!B.path[0]) {
    if (!B.fetch) load_cur();
    B.want_viewer = true;
    ui_toast("Getting the full picture\xE2\x80\xA6");
    return;
  }
  B.want_viewer = false;
  image_view_title(B.cur.it.title[0] ? B.cur.it.title : NULL);
  app_open(B.path, NULL, 0, 0);
  image_view_title(NULL);                   /* taken by the viewer, or not used at all */
}

void box_view_full(void) { open_viewer(); }

/* The size of the file itself, as shown (EXIF rotation applied). */
static void task_native(const PhTask *t, int *w, int *h) {
  bool swap = t->info.orient >= 5;
  *w = swap ? t->info.h : t->info.w;
  *h = swap ? t->info.w : t->info.h;
  if (*w <= 0 || *h <= 0) { *w = t->img.w; *h = t->img.h; }
}

/* The sharper picture arrived: swap it in (same place on screen). */
static void hi_pump(void) {
  PhTask *t = B.hi;
  if (!t || !ptask_done(t)) return;
  B.hi = NULL;
  if (t->err == FM_OK && t->img.px && (i64)t->img.w * t->img.h > (i64)B.fw * B.fh) {
    SDL_Texture *tx = view_tex_rgba(t->img.px, t->img.w, t->img.h);
    if (tx) {
      if (B.full) SDL_DestroyTexture(B.full);
      B.full = tx;
      B.fw = t->img.w;
      B.fh = t->img.h;
      if (!B.t_full) B.t_full = SDL_GetTicks64();
      task_native(t, &B.nat_w, &B.nat_h);
      if (B.hi_orig) fm_strlcpy(B.hi_path, t->out, sizeof B.hi_path);
      if (B.demo)
        SDL_Log("photo zoom: sharper picture %dx%d from the %s (file %dx%d)", B.fw, B.fh,
                B.hi_orig ? "original" : "cached file", B.nat_w, B.nat_h);
    }
  } else if (B.demo) {
    SDL_Log("photo zoom: no sharper picture from the %s (%s)", B.hi_orig ? "original" : "cached file",
            t->err == FM_OK ? "not larger" : t->errtext);
  }
  /* a failed original (TIFF, too big, gone) keeps the picture we have */
  ptask_free(t, false);
  ui_redraw();
}

void box_pump(void) {
  hi_pump();
  PhTask *t = B.fetch;
  if (!t || !ptask_done(t)) return;
  B.fetch = NULL;
  if (t->err == FM_OK) {
    fm_strlcpy(B.path, t->out, sizeof B.path);
    if (t->img.px) {
      B.full = view_tex_rgba(t->img.px, t->img.w, t->img.h);
      B.fw = t->img.w;
      B.fh = t->img.h;
      B.t_full = SDL_GetTicks64();
      task_native(t, &B.nat_w, &B.nat_h);
    }
    /* details() may have filled the licence or the size in */
    B.cur.it = t->item;
    if (B.w && B.index < B.w->n && !strcmp(B.w->items[B.index].it.id, t->item.id)) B.w->items[B.index].it = t->item;
    if (B.want_viewer) open_viewer();
  } else if (t->err != FM_ERR_CANCEL) {
    B.fail = true;
    fm_strlcpy(B.err, t->errtext, sizeof B.err);
    if (B.want_viewer) ui_toast("Could not get the picture: %s", B.err);
    B.want_viewer = false;
  }
  ptask_free(t, false);
  ui_redraw();
}

/* ---- actions ----------------------------------------------------------------- */

static void nav(int d) {
  PhWall *w = B.w;
  int i = B.index + d;
  if (i < 0 || i >= w->n) {
    ui_toast(d > 0 ? "That is the last photo" : "That is the first photo");
    return;
  }
  /* the new photo slides in from the side it comes from */
  ui_anim_set(ui_idn(B.id, 2), d > 0 ? DP(60) : -DP(60));
  go(i);
}

void box_menu_results(void) {
  if (!ID_BMENU) return;
  int r = ui_menu_result(ID_BMENU);
  if (r < 0 || !B.open) return;
  switch (r) {
    case BM_VIEWER: open_viewer(); break;
    case BM_INFO: B.info = !B.info; break;
    case BM_BROWSER: ph_open_url(B.cur.it.page); break;
    case BM_COPY: ph_copy_link(&B.cur); break;
    case BM_DOWNLOAD: pdl_start(&B.cur, false); break;
    case BM_ZIN: case BM_ZOUT: case BM_ZFIT: B.want_zoom = r; break;
    default: break;
  }
  ui_redraw();
}

static void more_menu(float x, float y) {
  FmMenuItem m[10];
  int n = 0;
  memset(m, 0, sizeof m);
  m[n].id = BM_DOWNLOAD; m[n].icon = IC_DOWNLOAD; m[n++].label = "Download";
  m[n].id = BM_VIEWER; m[n].icon = IC_FULLSCREEN; m[n++].label = "Open in viewer";
  m[n].id = BM_INFO; m[n].icon = IC_INFO; m[n].label = "Details"; m[n++].flags = B.info ? UI_MI_CHECKED : 0;
  m[n++].flags = UI_MI_SEP;
  m[n].id = BM_BROWSER; m[n].icon = IC_OPEN_WITH; m[n++].label = "Open in browser";
  m[n].id = BM_COPY; m[n].icon = IC_LINK; m[n++].label = "Copy link";
  m[n++].flags = UI_MI_SEP;
  m[n].id = BM_ZIN; m[n].icon = IC_ZOOM_IN; m[n++].label = "Zoom in";
  if (B.z.s > 1.001f) {
    m[n].id = BM_ZOUT; m[n].icon = IC_ZOOM_OUT; m[n++].label = "Zoom out";
    m[n].id = BM_ZFIT; m[n].icon = IC_FIT; m[n++].label = "Fit to screen";
  }
  ui_menu_open(ID_BMENU, x, y, m, n);
}

/* ---- zoom ------------------------------------------------------------------------- */

FmRect pzoom_rect(const PhZoom *z, FmRect fit) {
  float w = fit.w * z->s, h = fit.h * z->s;
  float cx = fit.x + fit.w * 0.5f + z->px, cy = fit.y + fit.h * 0.5f + z->py;
  return FM_RECT(cx - w * 0.5f, cy - h * 0.5f, w, h);
}

void pzoom_at(PhZoom *z, FmRect fit, float to, float ax, float ay) {
  if (to <= 0 || z->s <= 0) return;
  float cx = fit.x + fit.w * 0.5f, cy = fit.y + fit.h * 0.5f;
  float k = to / z->s;
  z->px = (ax - cx) - (ax - cx - z->px) * k;
  z->py = (ay - cy) - (ay - cy - z->py) * k;
  z->s = to;
}

/* One axis: the picture's centre c + p with this size, over the view v0..v0+vs. */
static float clamp_axis(float p, float size, float c, float v0, float vs) {
  float vc = v0 + vs * 0.5f;
  if (size <= vs) return vc - c;                 /* smaller than the view: centred */
  float e = (size - vs) * 0.5f;                  /* how far the centre may move */
  return FM_CLAMP(p, vc - c - e, vc - c + e);
}

void pzoom_clamp(PhZoom *z, FmRect fit, FmRect view) {
  if (z->s <= 1.0001f) {
    /* fitted (or smaller, pinching): exactly where the unzoomed picture sits */
    z->px = z->py = 0;
    return;
  }
  z->px = clamp_axis(z->px, fit.w * z->s, fit.x + fit.w * 0.5f, view.x, view.w);
  z->py = clamp_axis(z->py, fit.h * z->s, fit.y + fit.h * 0.5f, view.y, view.h);
}

float pzoom_max(float one) { return FM_CLAMP(one * 4.0f, 4.0f, 32.0f); }

float pzoom_toggle(float s, float one, float smax) {
  if (s > 1.01f) return 1.0f;
  return FM_MIN(one > 1.25f ? one : 2.0f, smax);
}

/* Scale (relative to fit) at which one picture pixel is one screen pixel:
** the largest size we know of the file over the fitted size. */
static float one_scale(FmRect fit0) {
  float fl = FM_MAX(fit0.w, fit0.h);
  if (fl < 1) return 1;
  int nl = 0;
  if (B.full) nl = FM_MAX(FM_MAX(B.nat_w, B.nat_h), FM_MAX(B.fw, B.fh));
  if (nl <= 0) nl = FM_MAX(B.cur.it.width, B.cur.it.height);
  return nl > 0 ? (float)nl / fl : 1.0f;
}

static void zoom_anim(float to, float ax, float ay) {
  B.zanim = true;
  B.z_from = B.z.s;
  B.z_to = to;
  B.z_ax = ax;
  B.z_ay = ay;
  B.z_px0 = B.z.px;
  B.z_py0 = B.z.py;
  B.z_t0 = ui.now;
  ui_redraw();
}

static void zoom_tick(FmRect fit0) {
  if (!B.zanim) return;
  float t = FM_MIN(1.0f, (float)(ui.now - B.z_t0) / 220.0f);
  float e = ui_ease_out(t);
  B.z.s = B.z_from;
  B.z.px = B.z_px0;
  B.z.py = B.z_py0;
  pzoom_at(&B.z, fit0, B.z_from + (B.z_to - B.z_from) * e, B.z_ax, B.z_ay);
  if (t >= 1.0f) B.zanim = false;
  else ui_animate();
}

static u32 hi_budget(void) {
#ifdef FM_MOBILE
  return 16u * 1024u * 1024u;
#else
  return 32u * 1024u * 1024u;
#endif
}

static int hi_edge(void) {
  int m = view_max_texture();
#ifdef FM_MOBILE
  if (m > 4096) m = 4096;
#endif
  return m;
}

static void hi_start(bool original) {
  PhTask *t = ptask_new(PT_FETCH, ph_src(&B.cur));
  t->item = B.cur.it;
  if (original) fm_strlcpy(t->item.full, B.cur.it.original, sizeof t->item.full);
  t->max_px = hi_edge();
  t->max_pixels = hi_budget();
  if (original) B.orig_tried = true;
  else B.redecode_tried = true;
  if (!ptask_run(t)) {
    ptask_free(t, false);
    return;
  }
  B.hi = t;
  B.hi_orig = original;
  if (B.demo) SDL_Log("photo zoom: %.1fx, fetching a sharper picture (%s)", B.z.s, original ? "original" : "cached file");
}

/* Zoomed past the texture: a sharper one, once the zoom settles. First the
** cached file decoded larger, then the original when the site has one. */
static void hi_maybe(FmRect fit0) {
  if (!B.full || B.fetch || B.hi || !B.path[0] || B.zanim || B.pinching) return;
  float shown = FM_MAX(fit0.w, fit0.h) * B.z.s;
  int tl = FM_MAX(B.fw, B.fh);
  if (shown <= (float)tl * 1.15f) return;
  /* already as large as we keep */
  if ((u64)B.fw * (u64)B.fh >= (u64)hi_budget() * 9 / 10 || tl >= hi_edge() - 16) return;
  if (!B.redecode_tried && FM_MAX(B.nat_w, B.nat_h) > tl + 8) {
    hi_start(false);
  } else if (!B.orig_tried && B.cur.it.original[0] && strcmp(B.cur.it.original, B.cur.it.full) != 0) {
    hi_start(true);
  }
}

/* ---- drawing ----------------------------------------------------------------------- */

static FmRect fit(FmRect box, float ar) {
  if (ar <= 0) ar = 4.0f / 3.0f;
  float w = box.w, h = w / ar;
  if (h > box.h) { h = box.h; w = h * ar; }
  return FM_RECT(floorf(box.x + (box.w - w) * 0.5f + 0.5f), floorf(box.y + (box.h - h) * 0.5f + 0.5f), floorf(w + 0.5f),
                 floorf(h + 0.5f));
}

static float item_aspect(const PhItem *p, SDL_Texture *thumb) {
  int tw = 0, th = 0;
  if (thumb && SDL_QueryTexture(thumb, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) return (float)tw / (float)th;
  if (p->ar > 0) return p->ar;
  return 4.0f / 3.0f;
}

static SDL_Texture *item_thumb(PhItem *p, float *fade) {
  *fade = 1;
  return p->it.thumb[0] ? othumb_get_ex(p->it.thumb, thumb_px(p), 0, psrc_item_headers(&p->it), fade) : NULL;
}

/* The current photo fitted in box (what zoom 1 shows). */
static FmRect cur_fit(FmRect box) {
  float fade;
  PhItem *p = &B.w->items[B.index];
  float ar = B.full ? (float)B.fw / (float)FM_MAX(1, B.fh) : item_aspect(p, item_thumb(p, &fade));
  return fit(box, ar);
}

/* A photo fitted in box: the thumbnail scaled up, then (current photo
** only) the full picture fading in over it, zoomed by z when given.
** Returns the rect drawn. */
static FmRect draw_photo(FmRect box, PhItem *p, bool current, float alpha, const PhZoom *z) {
  float fade = 1;
  SDL_Texture *th = item_thumb(p, &fade);
  float ar = current && B.full ? (float)B.fw / (float)FM_MAX(1, B.fh) : item_aspect(p, th);
  FmRect d = fit(box, ar);
  if (z) d = pzoom_rect(z, d);
  if (!gfx_visible(d)) return d;
  float ff = 0;
  if (current && B.full) {
    ff = FM_MIN(1.0f, (float)(SDL_GetTicks64() - B.t_full) / 200.0f);
    if (ff < 1) ui_animate();
  }
  if (ff < 1) {
    if (!th || fade < 1) {
      FmColor c = p->it.color ? FM_HEX(p->it.color) : FM_RGBA(40, 40, 44, 255);
      gfx_rect(d, col_alpha(c, alpha));
    }
    if (th) {
      gfx_tex(th, NULL, d, col_alpha(PH_WHITE, alpha * fade));
      if (fade < 1) ui_animate();
    }
  }
  if (current && B.full) gfx_tex(B.full, NULL, d, col_alpha(PH_WHITE, alpha * ff));
  return d;
}

/* The loading ring at the bottom right of the picture. */
static void loading_ring(FmRect d, float alpha) {
  float s = DP(30);
  float cx = d.x + d.w - s * 0.5f - DP(12), cy = d.y + d.h - s * 0.5f - DP(12);
  gfx_circle(cx, cy, s * 0.5f, FM_RGBA(0, 0, 0, (u8)(150 * alpha)));
  float fr = ptask_frac(B.fetch ? B.fetch : B.hi);
  float rr = s * 0.5f - DP(6);
  gfx_ring(cx, cy, rr, DP(2.5f), FM_RGBA(255, 255, 255, (u8)(60 * alpha)));
  if (fr >= 0) {
    gfx_arc(cx, cy, rr, DP(2.5f), -1.5707963f, -1.5707963f + 6.2831853f * FM_MAX(0.04f, fr), col_alpha(PH_WHITE, alpha));
    view_wake_in(100);
  } else {
    float a0 = (float)(ui.now % 1000) / 1000.0f * 6.2831853f;
    gfx_arc(cx, cy, rr, DP(2.5f), a0, a0 + 1.6f, col_alpha(PH_WHITE, alpha));
    view_wake_in(33);
  }
}

/* A round arrow at the side; returns true when clicked. */
static bool side_arrow(u32 id, float cx, float cy, FmIcon ic, float alpha) {
  float s = DP(46);
  FmRect b = { cx - s * 0.5f, cy - s * 0.5f, s, s };
  int f = ui_hit(id, b);
  float hv = ui_anim(ui_idn(id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 16.0f);
  gfx_circle(cx, cy, s * 0.5f, FM_RGBA(255, 255, 255, (u8)((22 + 40 * hv) * alpha)));
  icon_draw(ic, rect_center(b, DP(22), DP(22)), col_alpha(PH_WHITE, alpha * (0.8f + 0.2f * hv)));
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) && alpha > 0.5f;
}

static bool zoom_key(SDL_Keycode k) {
  return ui_key(k, 0) || ui_key(k, KMOD_SHIFT) || ui_key(k, KMOD_CTRL) || ui_key(k, KMOD_CTRL | KMOD_SHIFT);
}

/* The zoom group in the top bar: [-] 100% [+] [fit]. Returns the step
** wanted: BM_ZIN, BM_ZOUT, BM_ZFIT, BM_Z100 (actual size) or 0. */
static int zoom_group(FmRect *act, float alpha, float one, float smax) {
  float h = act->h, lw = DP(54);
  if (alpha <= 0.01f || act->w < h * 3 + lw) return 0;
  FmRect g = rect_cut_right(act, h * 3 + lw);
  rect_cut_right(&g, DP(6));                    /* a gap before the actions */
  FmRect pill = rect_inset2(g, 0, (h - DP(36)) * 0.5f);
  gfx_rrect(pill, pill.h * 0.5f, FM_RGBA(255, 255, 255, (u8)(24 * alpha)));
  FmRect r = g;
  FmRect bm = rect_cut_left(&r, h - DP(6)), lb = rect_cut_left(&r, lw), bp = rect_cut_left(&r, h - DP(6));
  FmRect bf = r;
  bool at_fit = B.z.s <= 1.001f && !B.zanim, at_max = B.z.s >= smax * 0.999f;
  int want = 0;
  if (ui_icon_btn(ui_idn(B.id, 50), bm, IC_MINUS, col_alpha(PH_WHITE, alpha * (at_fit ? 0.35f : 1.0f)),
                  "Zoom out (-)") && alpha > 0.5f && !at_fit)
    want = BM_ZOUT;
  char pct[16];
  fm_snprintf(pct, sizeof pct, "%d%%", (int)(B.z.s / FM_MAX(0.01f, one) * 100.0f + 0.5f));
  int f = ui_hit(ui_idn(B.id, 51), lb);
  if (f & UI_HOVER) {
    gfx_rrect(rect_inset2(lb, 0, (h - DP(28)) * 0.5f), DP(14), FM_RGBA(255, 255, 255, (u8)(30 * alpha)));
    ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  }
  ui_tip_track(ui_idn(B.id, 51), f, at_fit ? "Actual size (1)" : "Fit to screen (0)");
  font_draw_center(FONT_BOLD, ui.m.font_small, lb, pct, col_alpha(PH_WHITE, alpha));
  if ((f & UI_CLICK) && alpha > 0.5f) want = at_fit ? BM_Z100 : BM_ZFIT;
  if (ui_icon_btn(ui_idn(B.id, 52), bp, IC_PLUS, col_alpha(PH_WHITE, alpha * (at_max ? 0.35f : 1.0f)),
                  "Zoom in (+)") && alpha > 0.5f && !at_max)
    want = BM_ZIN;
  if (ui_icon_btn(ui_idn(B.id, 53), bf, IC_FIT, col_alpha(PH_WHITE, alpha * (at_fit ? 0.35f : 1.0f)),
                  "Fit to screen (0)") && alpha > 0.5f && !at_fit)
    want = BM_ZFIT;
  return want;
}

/* "250%" in a pill at the bottom of the picture for a moment after a zoom
** (pinch, wheel), when the bar's zoom group is not on screen. */
static void zoom_badge(FmRect box, float one) {
  if (ui.now >= B.badge_until) return;
  char zb[16];
  fm_snprintf(zb, sizeof zb, "%d%%", (int)(B.z.s / FM_MAX(0.01f, one) * 100.0f + 0.5f));
  float w = font_width(FONT_BOLD, ui.m.font_small, zb, -1) + DP(20), h = DP(28);
  FmRect r = { box.x + (box.w - w) * 0.5f, box.y + box.h - h - DP(16), w, h };
  gfx_rrect(r, h * 0.5f, VIEW_SCRIM);
  font_draw_center(FONT_BOLD, ui.m.font_small, r, zb, VIEW_FG);
  view_wake_in((u32)(B.badge_until - ui.now) + 10);
}

static void info_row(float x, float *y, float w, const char *k, const char *v, float alpha) {
  if (!v || !v[0]) return;
  float fs = ui.m.font_small, kw = DP(84);
  font_draw(FONT_REGULAR, fs, x, *y, k, -1, FM_RGBA(255, 255, 255, (u8)(150 * alpha)));
  font_draw_mid_ellipsis(FONT_REGULAR, fs, x + kw, *y, v, w - kw, col_alpha(PH_WHITE, alpha));
  *y += font_line_h(fs) + DP(4);
}

/* Height of the caption: title, the credit line and, with details on, the
** information rows. */
static float caption_h(float w) {
  float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
  int lines = ph_text_lines(FONT_BOLD, ui.m.font, 0, 0, w, B.cur.it.title[0] ? B.cur.it.title : "Untitled", 2,
                            PH_WHITE, false);
  float h = DP(14) + (float)lines * lh + DP(2) + ls + DP(16);
  if (B.info) h += DP(10) + (ls + DP(4)) * 5 + DP(6);
  return h;
}

static void caption(FmRect r, float alpha) {
  float x = r.x, y = r.y + DP(14), w = r.w;
  float lh = font_line_h(ui.m.font);
  int lines = ph_text_lines(FONT_BOLD, ui.m.font, x, y, w, B.cur.it.title[0] ? B.cur.it.title : "Untitled", 2,
                            col_alpha(PH_WHITE, alpha), true);
  y += (float)lines * lh + DP(2);
  char meta[400];
  ph_meta(&B.cur, meta, sizeof meta, true);
  /* the source page as a link at the end of the line */
  const char *link = "Source page";
  float fs = ui.m.font_small;
  float lw = B.cur.it.page[0] ? font_width(FONT_BOLD, fs, link, -1) + DP(18) : 0;
  font_draw_ellipsis(FONT_REGULAR, fs, x, y, meta, w - lw - DP(10), FM_RGBA(255, 255, 255, (u8)(190 * alpha)));
  if (lw > 0) {
    FmRect lb = { x + w - lw, y - DP(4), lw, font_line_h(fs) + DP(8) };
    int f = ui_hit(ui_idn(B.id, 20), lb);
    float ex = font_draw(FONT_BOLD, fs, lb.x, y, link, -1, col_alpha(PH_WHITE, alpha));
    icon_draw(IC_OPEN_WITH, FM_RECT(ex + DP(4), y + (font_line_h(fs) - DP(12)) * 0.5f, DP(12), DP(12)),
              col_alpha(PH_WHITE, alpha));
    if (f & UI_HOVER) {
      gfx_rect(FM_RECT(lb.x, y + font_line_h(fs) - DP(2), ex - lb.x, DP(1)), col_alpha(PH_WHITE, alpha));
      ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    }
    if ((f & UI_CLICK) && alpha > 0.5f) ph_open_url(B.cur.it.page);
  }
  y += font_line_h(fs);
  if (B.info) {
    y += DP(10);
    gfx_rect(FM_RECT(x, y - DP(5), w, DP(1)), FM_RGBA(255, 255, 255, (u8)(40 * alpha)));
    y += DP(2);
    char size[64] = "";
    int pw = B.cur.it.width, phh = B.cur.it.height;
    if (pw > 0 && phh > 0) fm_snprintf(size, sizeof size, "%d \xC3\x97 %d", pw, phh);
    else if (B.full) fm_snprintf(size, sizeof size, "about %d \xC3\x97 %d (shown)", B.fw, B.fh);
    info_row(x, &y, w, "Author", B.cur.it.author[0] ? B.cur.it.author : "Unknown", alpha);
    info_row(x, &y, w, "Licence", B.cur.it.license[0] ? B.cur.it.license : "See the source page", alpha);
    info_row(x, &y, w, "Size", size[0] ? size : "Unknown", alpha);
    info_row(x, &y, w, "Source", ph_src_name(B.cur.src), alpha);
    info_row(x, &y, w, "Page", B.cur.it.page[0] ? B.cur.it.page : B.cur.it.full, alpha);
  }
}

void box_frame(FmRect area) {
  if (!B.open) return;
  PhWall *w = B.w;
  /* the wall changed under us (a page loaded, an album lost a photo): follow the photo */
  if (B.index >= w->n || strcmp(w->items[B.index].it.id, B.cur.it.id) != 0) {
    int i = wall_find(w, B.cur.src, B.cur.it.id);
    if (i >= 0) B.index = i;
    else if (w->n == 0) { box_close(); return; }
    else { go(FM_MIN(B.index, w->n - 1)); }
  }
  w->items[B.index].tpx = FM_MAX(w->items[B.index].tpx, B.cur.tpx);
  box_menu_results();
  int prev_layer = ui_push_layer(UI_LAYER_SHEET);
  float t = ui_ease_out(ui_anim(ui_idn(B.id, 1), 1.0f, 12.0f));
  bool narrow = area.w < DP(620);
  float ca = view_chrome(&B.chrome, ui_idn(B.id, 3), false) * t;
  /* where the photo goes: under the bar, above the caption, beside the arrows */
  float cap_w = FM_MIN(area.w - DP(32), DP(760));
  float cap_h = caption_h(cap_w);
  float side = narrow || ui.touch_mode ? DP(0) : DP(72);
  FmRect box = { area.x + side, area.y + ui.m.bar_h, area.w - side * 2, area.h - ui.m.bar_h - cap_h };
  if (narrow) box = FM_RECT(area.x, area.y + ui.m.bar_h * 0.5f, area.w, area.h - ui.m.bar_h * 0.5f - cap_h * 0.6f);
  /* zoom: relative to the fitted picture, drawn over the whole area, kept over the box */
  FmRect fit0 = cur_fit(box);
  float one = one_scale(fit0), smax = pzoom_max(one);
  float zcx = box.x + box.w * 0.5f, zcy = box.y + box.h * 0.5f;   /* keys and buttons zoom about the middle */
  zoom_tick(fit0);
  bool zoomed = B.z.s > 1.001f || B.zanim || B.pinching;
  float down = B.dragging && B.drag_vertical ? FM_CLAMP(B.drag_y / (area.h * 0.5f), 0.0f, 1.0f) : 0;
  gfx_rect(area, col_alpha(VIEW_BG, (1.0f - 0.6f * down) * t));
  /* gestures on the backdrop (drawn first: the buttons on top take their clicks) */
  u32 hid = ui_idn(B.id, 10);
  int f = ui_hit(hid, area);
  bool input = ui_input_ok() && !ui_menu_is_open();
  /* pinch: zoom about the fingers, move with them */
  if (input && ui.nfingers >= 2) {
    if (!B.pinching) {
      B.pinching = true;
      B.last_mx = ui.pinch_cx;
      B.last_my = ui.pinch_cy;
      B.dragging = B.panning = false;
      B.drag_x = B.drag_y = 0;
      B.zanim = false;
      B.tap_pending = false;
      ui_anim_set(ui_idn(B.id, 2), 0.0f);
    }
    if (ui.pinch != 1.0f) pzoom_at(&B.z, fit0, FM_CLAMP(B.z.s * ui.pinch, 0.6f, smax * 1.2f), ui.pinch_cx, ui.pinch_cy);
    B.z.px += ui.pinch_cx - B.last_mx;
    B.z.py += ui.pinch_cy - B.last_my;
    B.last_mx = ui.pinch_cx;
    B.last_my = ui.pinch_cy;
    B.badge_until = ui.now + 900;
    ui.drag_owner = hid;
    zoomed = true;
  } else if (B.pinching && ui.nfingers == 0) {
    B.pinching = false;
    B.ignore_click = false;
    if (B.z.s < 1.0f) zoom_anim(1.0f, B.last_mx, B.last_my);
    else if (B.z.s > smax) zoom_anim(smax, B.last_mx, B.last_my);
  }
  if ((f & UI_DRAG) && ui.down && !B.pinching && ui.nfingers < 2) {
    if (!B.dragging && !B.panning) {
      if (zoomed) {
        /* zoomed in: the drag pans, from where the press was */
        B.panning = true;
        B.last_mx = ui.press_x;
        B.last_my = ui.press_y;
      } else {
        float dx = ui.mx - ui.press_x, dy = ui.my - ui.press_y;
        B.dragging = true;
        B.drag_vertical = fabsf(dy) > fabsf(dx) && dy > 0;
      }
    }
    if (B.panning) {
      B.z.px += ui.mx - B.last_mx;
      B.z.py += ui.my - B.last_my;
      B.last_mx = ui.mx;
      B.last_my = ui.my;
      B.zanim = false;
      ui.drag_owner = hid;
      ui_set_cursor(SDL_SYSTEM_CURSOR_SIZEALL);
    } else {
      float dx = ui.mx - ui.press_x, dy = ui.my - ui.press_y;
      B.drag_x = B.drag_vertical ? 0 : dx;
      B.drag_y = B.drag_vertical ? FM_MAX(0.0f, dy) : 0;
    }
  } else if (B.panning && !ui.down) {
    B.panning = false;
  } else if (B.dragging && !ui.down) {
    B.dragging = false;
    if (B.drag_vertical) {
      if (B.drag_y > DP(110)) {
        ui_pop_layer(prev_layer);
        box_close();
        return;
      }
    } else if (fabsf(B.drag_x) > FM_MIN(DP(90), area.w * 0.18f)) {
      int d = B.drag_x < 0 ? 1 : -1;
      int i = B.index + d;
      if (i >= 0 && i < w->n) {
        ui_anim_set(ui_idn(B.id, 2), B.drag_x + (d > 0 ? area.w : -area.w));
        go(i);
        fit0 = cur_fit(box);
      }
    } else {
      ui_anim_set(ui_idn(B.id, 2), B.drag_x);
    }
    B.drag_x = B.drag_y = 0;
  }
  if (zoomed && ui_hover(box) && !B.panning) ui_set_cursor(SDL_SYSTEM_CURSOR_SIZEALL);
  /* double click / double tap: fit <-> actual size (or 2x) at that point;
  ** a single tap waits for a possible second one, then shows or hides the bars */
  if ((f & UI_DCLICK) && !B.pinching) {
    B.tap_pending = false;
    B.ignore_click = true;
    zoom_anim(pzoom_toggle(B.z.s, one, smax), ui.mx, ui.my);
    B.badge_until = narrow || ca < 0.5f ? ui.now + 900 : 0;
  }
  if (f & UI_CLICK) {
    if (B.ignore_click) B.ignore_click = false;
    else {
      B.tap_pending = true;
      B.tap_t = ui.now;
      view_wake_in(300);
    }
  }
  if (B.tap_pending && ui.now - B.tap_t >= 280) {
    B.tap_pending = false;
    view_chrome_toggle(&B.chrome);
  }
  /* the wheel zooms about the cursor */
  if (input && ui_hover(area) && ui.wheel != 0) {
    float to = FM_CLAMP(B.z.s * powf(1.18f, ui.wheel), 1.0f, smax);
    if (B.zanim) { B.zanim = false; B.z.s = FM_MAX(B.z.s, 1.0f); }
    pzoom_at(&B.z, fit0, to, ui.mx, ui.my);
    if (narrow || ca < 0.5f) B.badge_until = ui.now + 900;
    ui.wheel = 0;
  }
  if (!B.pinching) pzoom_clamp(&B.z, fit0, box);
  zoomed = B.z.s > 1.001f || B.zanim || B.pinching;
  if (zoomed) ui_anim_set(ui_idn(B.id, 2), 0.0f);    /* no slide under a zoomed picture */
  hi_maybe(fit0);
  float slide = ui_anim(ui_idn(B.id, 2), 0.0f, 16.0f);
  if (fabsf(slide) < 0.5f) slide = 0;
  float off = B.drag_x + slide;
  float sc = 1.0f - 0.12f * down;
  FmRect pb = { box.x + off + box.w * (1 - sc) * 0.5f, box.y + B.drag_y + box.h * (1 - sc) * 0.5f, box.w * sc, box.h * sc };
  gfx_clip_push(area);
  FmRect d = draw_photo(pb, &w->items[B.index], true, t * (1.0f - 0.3f * down), zoomed ? &B.z : NULL);
  gfx_clip_pop();
  /* the neighbour sliding in during a swipe */
  if (fabsf(off) > 1) {
    int ni = B.index + (off < 0 ? 1 : -1);
    if (ni >= 0 && ni < w->n) {
      FmRect nb = pb;
      nb.x += off < 0 ? area.w + DP(16) : -area.w - DP(16);
      draw_photo(nb, &w->items[ni], false, t, NULL);
    }
  }
  FmRect vis = rect_intersect(d, box);
  if (vis.w < DP(60) || vis.h < DP(60)) vis = box;
  if ((B.fetch || B.hi) && !B.dragging) loading_ring(vis, t);
  if (B.fail && !B.full && ca > 0.01f) {
    const char *msg = "Showing the preview: the full picture did not load";
    float fs = ui.m.font_small;
    float mw = FM_MIN(vis.w - DP(16), font_width(FONT_REGULAR, fs, msg, -1) + DP(24));
    FmRect mr = { vis.x + (vis.w - mw) * 0.5f, vis.y + vis.h - DP(40), mw, DP(28) };
    if (mw > DP(60)) {
      gfx_rrect(mr, DP(14), FM_RGBA(0, 0, 0, (u8)(170 * ca)));
      font_draw_ellipsis(FONT_REGULAR, fs, mr.x + DP(12), mr.y + (mr.h - font_line_h(fs)) * 0.5f, msg, mr.w - DP(24),
                         col_alpha(PH_WHITE, ca));
    }
  }
  /* arrows */
  if (!ui.touch_mode && ca > 0.01f) {
    float cy = box.y + box.h * 0.5f;
    if (B.index > 0 && side_arrow(ui_idn(B.id, 30), area.x + DP(36), cy, IC_CHEVRON_LEFT, ca)) nav(-1);
    if (B.index < w->n - 1 && side_arrow(ui_idn(B.id, 31), area.x + area.w - DP(36), cy, IC_CHEVRON_RIGHT, ca)) nav(1);
  }
  /* caption (out of the way while zoomed in) */
  float cca = ca * (1.0f - FM_CLAMP((B.z.s - 1.0f) * 5.0f, 0.0f, 1.0f));
  if (cca > 0.01f) {
    FmRect cr = { area.x + (area.w - cap_w) * 0.5f, area.y + area.h - cap_h, cap_w, cap_h };
    view_bottom_scrim(FM_RECT(area.x, cr.y - DP(40), area.w, cap_h + DP(40)), cca * (B.info ? 1.0f : 0.85f));
    caption(cr, cca);
  }
  zoom_badge(box, one);
  /* top bar */
  char sub[160];
  fm_snprintf(sub, sizeof sub, "%d of %d  \xC2\xB7  %s", B.index + 1, w->n, ph_src_name(B.cur.src));
  FmRect act;
  int nact = narrow ? 3 : 12;                   /* 7 actions and the zoom group */
  const char *title = B.cur.it.title[0] ? B.cur.it.title : "Photo";
  bool back = view_topbar(area, VIEW_BAR_MEDIA, ca, title, sub, nact, &act);
  bool fav = palb_is_fav(&B.cur);
  if (narrow) {
    if (view_bar_btn(&act, ui_idn(B.id, 40), IC_MORE, "More", VIEW_BAR_MEDIA, ca, false))
      more_menu(act.x + act.w, area.y + ui.m.bar_h);
  } else {
    if (view_bar_btn(&act, ui_idn(B.id, 41), IC_LINK, "Copy link", VIEW_BAR_MEDIA, ca, false)) ph_copy_link(&B.cur);
    if (view_bar_btn(&act, ui_idn(B.id, 42), IC_OPEN_WITH, "Open in browser", VIEW_BAR_MEDIA, ca, false))
      ph_open_url(B.cur.it.page);
    if (view_bar_btn(&act, ui_idn(B.id, 43), IC_INFO, "Details (I)", VIEW_BAR_MEDIA, ca, B.info)) B.info = !B.info;
    if (view_bar_btn(&act, ui_idn(B.id, 44), IC_FULLSCREEN, "Open in viewer (Enter)", VIEW_BAR_MEDIA, ca, false))
      open_viewer();
  }
  if (!narrow && view_bar_btn(&act, ui_idn(B.id, 45), IC_DOWNLOAD, "Download (Ctrl+S)", VIEW_BAR_MEDIA, ca, false))
    pdl_start(&B.cur, false);
  if (view_bar_btn(&act, ui_idn(B.id, 46), IC_ALBUM_ADD, "Add to album", VIEW_BAR_MEDIA, ca, false))
    palb_pick_open(&B.cur, 1);
  if (view_bar_btn(&act, ui_idn(B.id, 47), fav ? IC_HEART_FILL : IC_HEART, fav ? "Remove from Favorites" : "Favorite (F)",
                   VIEW_BAR_MEDIA, ca, false))
    palb_fav_toggle(&B.cur);
  int zw = narrow ? 0 : zoom_group(&act, ca, one, smax);
  if (B.want_zoom) {
    zw = B.want_zoom;
    B.want_zoom = 0;
  }
  /* keys */
  bool close = back;
  if (ui_input_ok() && !ui_menu_is_open() && !ui.focus) {
    if (ui_key(SDLK_LEFT, 0)) nav(-1);
    if (ui_key(SDLK_RIGHT, 0) || ui_key(SDLK_SPACE, 0)) nav(1);
    if (ui_key(SDLK_HOME, 0) && B.index > 0) go(0);
    if (ui_key(SDLK_END, 0) && B.index < w->n - 1) go(w->n - 1);
    if (ui_key(SDLK_RETURN, 0)) open_viewer();
    if (ui_key(SDLK_i, 0)) B.info = !B.info;
    if (ui_key(SDLK_f, 0)) palb_fav_toggle(&B.cur);
    if (ui_key(SDLK_c, KMOD_CTRL)) ph_copy_link(&B.cur);
    if (ui_key(SDLK_s, KMOD_CTRL)) pdl_start(&B.cur, false);
    if (zoom_key(SDLK_EQUALS) || zoom_key(SDLK_PLUS) || zoom_key(SDLK_KP_PLUS)) zw = BM_ZIN;
    if (zoom_key(SDLK_MINUS) || zoom_key(SDLK_KP_MINUS)) zw = BM_ZOUT;
    if (ui_key(SDLK_0, 0) || ui_key(SDLK_0, KMOD_CTRL) || ui_key(SDLK_KP_0, 0)) zw = BM_ZFIT;
    if (ui_key(SDLK_1, 0) || ui_key(SDLK_KP_1, 0)) zw = BM_Z100;
    if (view_key_back()) {
      if (B.z.s > 1.001f) zw = BM_ZFIT;          /* back first leaves the zoom */
      else close = true;
    }
  }
  if (zw) {
    float to = B.zanim ? B.z_to : B.z.s;
    if (zw == BM_ZIN) to = FM_MIN(to * 1.5f, smax);
    else if (zw == BM_ZOUT) to = FM_MAX(to / 1.5f, 1.0f);
    else if (zw == BM_ZFIT) to = 1.0f;
    else to = FM_CLAMP(one, 1.0f, smax);
    if (fabsf(to - B.z.s) > 1e-4f) zoom_anim(to, zcx, zcy);
    if (narrow || ca < 0.5f) B.badge_until = ui.now + 900;
  }
  ui_pop_layer(prev_layer);
  if (close) box_close();
}
