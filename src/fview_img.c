/* fview_img.c -- image viewer: stills, animated GIF, SVG; zoom, pan, swipe.
**
** Design decisions:
**   - Two slots: the current image and one neighbour preloaded in the
**     direction of travel. A loader thread fills them; pixels are uploaded
**     and freed on the main thread, so at most two textures (plus one GIF
**     frame buffer) are alive.
**   - Textures are capped by the GPU limit and by a pixel budget (32 MP on
**     desktop, 16 MP on phones). Zoom is computed on the original size, so
**     "100%" means real pixels even when the texture had to be smaller.
**   - GIFs play from a decoder thread that keeps one composed frame ready;
**     the main thread uploads it into a streaming texture when it is due
**     and sleeps until the next frame with a timer (no idle animation).
**   - SVG is rasterized for the current zoom on the loader thread and
**     re-rasterized when the zoom settles far from the last raster.
**   - Gestures: at fit zoom a horizontal drag pages and a downward drag
**     dismisses; when zoomed in a drag pans with inertia and rubber-band
**     edges. A single tap waits for a possible second tap before toggling
**     the bars, so double-tap zoom does not flash them.
*/
#include "fview_int.h"
#include "fplat.h"
#include "fdec_img.h"

enum { SK_EMPTY = 0, SK_LOADING, SK_READY, SK_ERROR };
enum { G_NONE = 0, G_PAN, G_SWIPE, G_DISMISS };

#define SLIDE_MS 4000
#define GIF_TEX_MAX_PIXELS (32u * 1024u * 1024u)

typedef struct Slot {
  int index;               /* list index, -1 = empty */
  int state;
  u32 gen;                 /* bumped when retargeted; stale results are dropped */
  bool taken;              /* the loader is working on it */
  FmErr err;
  FmImgInfo info;
  u64 fsize;
  i64 mtime;
  FmImage img;             /* decoded, waiting for upload */
  float svg_w, svg_h;      /* SVG document size */
  int box_w, box_h;        /* requested raster box */
  /* main thread */
  SDL_Texture *tex;
  int iw, ih;              /* logical size (EXIF-rotated original) */
  int raster_w;            /* SVG: width of the current raster */
} Slot;

static struct {
  bool open;
  FmArena arena;
  char **paths;
  int n;
  Slot s[2];
  int cur;                 /* index into s */
  int dir;                 /* last navigation direction */
  /* loader */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  bool quit;
  int view_w, view_h;      /* for SVG first raster */
  int max_tex;
  u32 pixel_budget;
  /* SVG re-raster request for the current slot */
  bool svg_req;
  int svg_req_w, svg_req_h;
  u32 svg_req_gen;
  FmImage svg_img;
  bool svg_ready;
  u64 last_zoom_change;
  /* GIF player */
  SDL_Thread *gif_thr;
  SDL_Texture *gif_tex;    /* streaming; shown once the first frame is in */
  bool gif_has_frame;
  bool gif_quit, gif_frame_ready, gif_stopped;
  u8 *gif_buf;
  int gif_w, gif_h, gif_delay;
  int gif_slot_index;
  u64 gif_due;
  /* view transform */
  float scale, px, py;
  int rot;
  bool fitted;             /* scale follows the fit */
  float vx, vy;
  int gesture;
  float gx0, gy0, last_mx, last_my, gvx, gvy;
  float swipe_dx, dismiss_dy;
  float slide;             /* entering slide offset, animates to 0 */
  bool pinching;
  float pinch_last_cx, pinch_last_cy;
  /* zoom animation */
  bool zanim;
  float z_from, z_to, z_ax, z_ay, z_px0, z_py0;
  u64 z_t0;
  /* chrome, taps, slideshow */
  FmViewChrome chrome;
  bool tap_pending, ignore_click;
  u64 tap_t;
  bool slideshow;
  u64 slide_next;
  bool info_open;
  u64 zoom_shown_until;
} g;

/* ---- loader thread ---------------------------------------------------------------- */

static Slot *cur_slot(void) { return &g.s[g.cur]; }
static Slot *other_slot(void) { return &g.s[g.cur ^ 1]; }

/* Box that keeps a decode inside the texture limit and the pixel budget. */
static void decode_box(int w, int h, int *bw, int *bh) {
  int m = g.max_tex;
  int fw, fh;
  img_fit(w, h, m, m, &fw, &fh);
  if ((u64)fw * fh > g.pixel_budget) {
    double k = sqrt((double)g.pixel_budget / ((double)fw * fh));
    fw = FM_MAX(1, (int)(fw * k));
    fh = FM_MAX(1, (int)(fh * k));
  }
  *bw = fw;
  *bh = fh;
}

static void load_slot_work(const char *path, Slot *res, int view_w, int view_h, const volatile int *cancel) {
  FmStat st;
  if (plat_stat(path, &st)) { res->fsize = st.size; res->mtime = st.mtime; }
  FmImgKind k = img_sniff(path);
  if (k == IMGK_SVG) {
    FmErr err;
    FmSvg *sv = svg_open(path, &err);
    if (!sv) { res->err = err; return; }
    svg_size(sv, &res->svg_w, &res->svg_h);
    res->info.kind = IMGK_SVG;
    res->info.w = (int)(res->svg_w + 0.5f);
    res->info.h = (int)(res->svg_h + 0.5f);
    res->info.frames = 1;
    res->info.orient = 1;
    /* first raster: fit the view */
    double sc = FM_MIN(view_w / res->svg_w, view_h / res->svg_h);
    int rw = FM_MAX(1, (int)(res->svg_w * sc)), rh = FM_MAX(1, (int)(res->svg_h * sc));
    decode_box(rw, rh, &rw, &rh);
    res->err = svg_render(sv, rw, rh, &res->img);
    svg_close(sv);
    return;
  }
  FmImgInfo info;
  if (!img_info(path, &info)) { res->err = plat_exists(path) ? FM_ERR_FORMAT : FM_ERR_NOT_FOUND; return; }
  if ((u64)info.w * info.h > IMG_MAX_PIXELS) { res->info = info; res->err = FM_ERR_UNSUPPORTED; return; }
  bool swap = info.orient >= 5;
  int bw, bh;
  decode_box(swap ? info.h : info.w, swap ? info.w : info.h, &bw, &bh);
  int frames = info.frames;
  res->err = img_load(path, bw, bh, &res->img, &res->info, cancel);
  res->info.frames = frames;
  if (res->info.orient == 0) res->info.orient = 1;
}

static volatile int g_cancel_dummy;

static int loader(void *u) {
  FM_UNUSED(u);
  SDL_LockMutex(g.mx);
  while (!g.quit) {
    Slot *s = NULL;
    Slot *cs = &g.s[g.cur], *os = &g.s[g.cur ^ 1];
    if (cs->state == SK_LOADING && !cs->taken) s = cs;
    else if (os->state == SK_LOADING && !os->taken) s = os;
    if (s) {
      s->taken = true;
      u32 gen = s->gen;
      char path[FM_PATH_MAX];
      fm_strlcpy(path, g.paths[s->index], sizeof path);
      int vw = g.view_w, vh = g.view_h;
      SDL_UnlockMutex(g.mx);
      Slot res;
      memset(&res, 0, sizeof res);
      load_slot_work(path, &res, vw, vh, &g_cancel_dummy);
      SDL_LockMutex(g.mx);
      if (s->gen == gen && s->state == SK_LOADING) {
        s->err = res.err;
        s->info = res.info;
        s->fsize = res.fsize;
        s->mtime = res.mtime;
        s->svg_w = res.svg_w;
        s->svg_h = res.svg_h;
        s->img = res.img;
        s->state = res.err == FM_OK && res.img.px ? SK_READY : SK_ERROR;
        if (s->state == SK_ERROR && s->err == FM_OK) s->err = FM_ERR_FORMAT;
        if (s->state == SK_ERROR) img_free(&s->img);
      } else {
        img_free(&res.img);
      }
      s->taken = false;
      SDL_UnlockMutex(g.mx);
      app_wake();
      SDL_LockMutex(g.mx);
      continue;
    }
    if (g.svg_req) {
      g.svg_req = false;
      u32 gen = g.svg_req_gen;
      Slot *c = &g.s[g.cur];
      char path[FM_PATH_MAX];
      fm_strlcpy(path, g.paths[c->index], sizeof path);
      int w = g.svg_req_w, h = g.svg_req_h;
      SDL_UnlockMutex(g.mx);
      FmImage im;
      memset(&im, 0, sizeof im);
      FmErr err;
      FmSvg *sv = svg_open(path, &err);
      if (sv) { svg_render(sv, w, h, &im); svg_close(sv); }
      SDL_LockMutex(g.mx);
      if (gen == g.svg_req_gen && im.px) {
        img_free(&g.svg_img);
        g.svg_img = im;
        g.svg_ready = true;
      } else {
        img_free(&im);
      }
      SDL_UnlockMutex(g.mx);
      app_wake();
      SDL_LockMutex(g.mx);
      continue;
    }
    SDL_CondWait(g.cv, g.mx);
  }
  SDL_UnlockMutex(g.mx);
  return 0;
}

/* Main thread: point slot `s` at list index `idx` (or keep it). */
static void slot_target(Slot *s, int idx) {
  SDL_LockMutex(g.mx);
  if (s->index == idx && s->state != SK_EMPTY) { SDL_UnlockMutex(g.mx); return; }
  s->gen++;
  s->index = idx;
  img_free(&s->img);
  s->state = idx >= 0 ? SK_LOADING : SK_EMPTY;
  s->err = FM_OK;
  memset(&s->info, 0, sizeof s->info);
  SDL_CondBroadcast(g.cv);
  SDL_UnlockMutex(g.mx);
  if (s->tex) { SDL_DestroyTexture(s->tex); s->tex = NULL; }
  s->iw = s->ih = 0;
  s->raster_w = 0;
}

/* ---- GIF player -------------------------------------------------------------------- */

static int gif_player(void *u) {
  char *path = (char *)u;
  FmErr err;
  FmGif *gf = gif_open(path, false, &err);
  fm_free(path);
  if (!gf) return 0;
  int loops = 0, played = 0;
  size_t bytes = (size_t)gif_width(gf) * gif_height(gf) * 4;
  for (;;) {
    int delay = 100;
    int r = gif_next(gf, &delay);
    if (r == 0) {
      loops = gif_loops(gf);
      played++;
      if (loops > 0 && played > loops) break;
      if (!gif_rewind(gf)) break;
      r = gif_next(gf, &delay);
    }
    if (r != 1) break;
    SDL_LockMutex(g.mx);
    while (!g.gif_quit && g.gif_frame_ready) SDL_CondWait(g.cv, g.mx);
    if (g.gif_quit) { SDL_UnlockMutex(g.mx); break; }
    memcpy(g.gif_buf, gif_pixels(gf), bytes);
    g.gif_delay = delay;
    g.gif_frame_ready = true;
    SDL_UnlockMutex(g.mx);
    app_wake();
  }
  SDL_LockMutex(g.mx);
  g.gif_stopped = true;
  SDL_UnlockMutex(g.mx);
  gif_close(gf);
  return 0;
}

static void gif_stop(void) {
  if (!g.gif_thr) return;
  SDL_LockMutex(g.mx);
  g.gif_quit = true;
  SDL_CondBroadcast(g.cv);
  SDL_UnlockMutex(g.mx);
  SDL_WaitThread(g.gif_thr, NULL);
  g.gif_thr = NULL;
  if (g.gif_tex) SDL_DestroyTexture(g.gif_tex);
  g.gif_tex = NULL;
  g.gif_has_frame = false;
  fm_free(g.gif_buf);
  g.gif_buf = NULL;
  g.gif_frame_ready = false;
  g.gif_quit = false;
  g.gif_slot_index = -1;
}

/* Starts animating the current slot if it is a multi-frame GIF. */
static void gif_start(Slot *s) {
  gif_stop();
  g.gif_slot_index = s->index;      /* also marks "tried" for stills and huge GIFs */
  if (s->info.kind != IMGK_GIF || s->info.frames <= 1) return;
  if (s->info.w > g.max_tex || s->info.h > g.max_tex || (u64)s->info.w * s->info.h > GIF_TEX_MAX_PIXELS) return;
  SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, s->info.w, s->info.h);
  if (!t) return;
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
  g.gif_tex = t;
  g.gif_has_frame = false;
  g.gif_w = s->info.w;
  g.gif_h = s->info.h;
  g.gif_buf = (u8 *)fm_alloc((size_t)g.gif_w * g.gif_h * 4);
  g.gif_frame_ready = false;
  g.gif_stopped = false;
  g.gif_quit = false;
  g.gif_due = 0;
  g.gif_thr = fm_thread_create(gif_player, "gif", fm_strdup(g.paths[s->index]));
}

static void gif_pump(void) {
  Slot *s = cur_slot();
  if (!g.gif_thr || g.gif_slot_index != s->index || !g.gif_tex) return;
  SDL_LockMutex(g.mx);
  bool ready = g.gif_frame_ready;
  SDL_UnlockMutex(g.mx);
  if (!ready) return;
  if (ui.now < g.gif_due) { view_wake_in((u32)(g.gif_due - ui.now)); return; }
  SDL_UpdateTexture(g.gif_tex, NULL, g.gif_buf, g.gif_w * 4);
  g.gif_has_frame = true;
  SDL_LockMutex(g.mx);
  int delay = g.gif_delay;
  g.gif_frame_ready = false;
  SDL_CondBroadcast(g.cv);
  SDL_UnlockMutex(g.mx);
  /* catch up after a stall instead of fast-forwarding */
  g.gif_due = (g.gif_due && ui.now - g.gif_due < 200 ? g.gif_due : ui.now) + (u64)delay;
  view_wake_in((u32)delay);
}

/* ---- geometry ------------------------------------------------------------------------ */

static void disp_size(const Slot *s, float *dw, float *dh) {
  bool odd = g.rot & 1;
  *dw = (float)(odd ? s->ih : s->iw);
  *dh = (float)(odd ? s->iw : s->ih);
}

static float fit_scale(const Slot *s, FmRect v) {
  float dw, dh;
  disp_size(s, &dw, &dh);
  if (dw <= 0 || dh <= 0) return 1;
  float f = FM_MIN(v.w / dw, v.h / dh);
  if (s->info.kind != IMGK_SVG) f = FM_MIN(f, 1.0f);
  return f;
}

static float max_scale(const Slot *s, FmRect v) { return FM_MAX(fit_scale(s, v) * 4, 8.0f); }

static void clamp_pan(const Slot *s, FmRect v, bool hard) {
  float dw, dh;
  disp_size(s, &dw, &dh);
  float ex = FM_MAX(0.0f, (dw * g.scale - v.w) * 0.5f), ey = FM_MAX(0.0f, (dh * g.scale - v.h) * 0.5f);
  if (hard) {
    g.px = FM_CLAMP(g.px, -ex, ex);
    g.py = FM_CLAMP(g.py, -ey, ey);
    return;
  }
  /* spring back */
  float tx = FM_CLAMP(g.px, -ex, ex), ty = FM_CLAMP(g.py, -ey, ey);
  if (fabsf(tx - g.px) > 0.5f || fabsf(ty - g.py) > 0.5f) {
    float k = FM_MIN(1.0f, ui.dt * 12.0f);
    g.px += (tx - g.px) * k;
    g.py += (ty - g.py) * k;
    if (tx != g.px) g.vx = 0;
    if (ty != g.py) g.vy = 0;
    ui_animate();
  } else {
    g.px = tx;
    g.py = ty;
  }
}

static bool at_fit(const Slot *s, FmRect v) {
  return fabsf(g.scale - fit_scale(s, v)) < 0.01f * fit_scale(s, v) + 1e-4f;
}

/* Zooms to `to` keeping screen point (ax, ay) fixed. */
static void zoom_at(FmRect v, float to, float ax, float ay) {
  if (to <= 0 || g.scale <= 0) return;
  float cx = v.x + v.w * 0.5f, cy = v.y + v.h * 0.5f;
  float k = to / g.scale;
  g.px = (ax - cx) - (ax - cx - g.px) * k;
  g.py = (ay - cy) - (ay - cy - g.py) * k;
  g.scale = to;
  g.fitted = false;
  g.last_zoom_change = ui.now;
  g.zoom_shown_until = ui.now + 900;
}

static void zoom_anim(FmRect v, float to, float ax, float ay) {
  FM_UNUSED(v);
  g.zanim = true;
  g.z_from = g.scale;
  g.z_to = to;
  g.z_ax = ax;
  g.z_ay = ay;
  g.z_px0 = g.px;
  g.z_py0 = g.py;
  g.z_t0 = ui.now;
  g.vx = g.vy = 0;
}

static void reset_view(void) {
  g.fitted = true;
  g.px = g.py = 0;
  g.vx = g.vy = 0;
  g.zanim = false;
  g.rot = 0;
  g.gesture = G_NONE;
  g.swipe_dx = g.dismiss_dy = 0;
}

/* ---- navigation ----------------------------------------------------------------------- */

static void go_to(int idx, float slide_from) {
  if (g.n <= 0) return;
  if (idx < 0 || idx >= g.n) return;
  Slot *c = cur_slot();
  if (c->index == idx) return;
  g.dir = idx > c->index ? 1 : -1;
  gif_stop();
  Slot *o = other_slot();
  if (o->index == idx) g.cur ^= 1;          /* preloaded: swap in */
  else slot_target(c, idx);
  reset_view();
  SDL_LockMutex(g.mx);
  g.svg_req_gen++;
  g.svg_req = false;
  g.svg_ready = false;
  img_free(&g.svg_img);
  SDL_UnlockMutex(g.mx);
  g.slide = slide_from;
  g.info_open = false;
  ui_redraw();
}

static void nav(int d, bool wrap) {
  Slot *c = cur_slot();
  int idx = c->index + d;
  if (wrap) idx = (idx % g.n + g.n) % g.n;
  if (idx < 0 || idx >= g.n) {
    ui_toast(d > 0 ? "Last image" : "First image");
    return;
  }
  go_to(idx, d > 0 ? g.view_w * 0.25f : -g.view_w * 0.25f);
}

/* ---- open / close ---------------------------------------------------------------------- */

static void img_close(void) {
  if (!g.open) return;
  gif_stop();
  SDL_LockMutex(g.mx);
  g.quit = true;
  SDL_CondBroadcast(g.cv);
  SDL_UnlockMutex(g.mx);
  if (g.thr) SDL_WaitThread(g.thr, NULL);
  for (int i = 0; i < 2; i++) {
    img_free(&g.s[i].img);
    if (g.s[i].tex) SDL_DestroyTexture(g.s[i].tex);
  }
  img_free(&g.svg_img);
  SDL_DestroyCond(g.cv);
  SDL_DestroyMutex(g.mx);
  arena_free(&g.arena);
  memset(&g, 0, sizeof g);
}

/* A title for the next single-file open (online photos), then the current one. */
static char g_next_title[256], g_title[256];

void image_view_title(const char *title) { fm_strlcpy(g_next_title, title ? title : "", sizeof g_next_title); }

static bool img_open(const char *path, const char *const *list, int n, int index) {
  img_close();
  memset(&g, 0, sizeof g);
  fm_strlcpy(g_title, (list && n > 1) ? "" : g_next_title, sizeof g_title);
  g_next_title[0] = 0;
  g.open = true;
  arena_init(&g.arena, 16 * 1024);
  if (list && n > 0 && index >= 0 && index < n) {
    g.paths = (char **)arena_alloc(&g.arena, sizeof(char *) * (size_t)n);
    for (int i = 0; i < n; i++) g.paths[i] = arena_strdup(&g.arena, list[i]);
    g.n = n;
  } else {
    g.paths = (char **)arena_alloc(&g.arena, sizeof(char *));
    g.paths[0] = arena_strdup(&g.arena, path);
    g.n = 1;
    index = 0;
  }
  g.mx = SDL_CreateMutex();
  g.cv = SDL_CreateCond();
  g.max_tex = view_max_texture();
#ifdef FM_MOBILE
  g.pixel_budget = 16u * 1024u * 1024u;
  if (g.max_tex > 4096) g.max_tex = 4096;
#else
  g.pixel_budget = 32u * 1024u * 1024u;
#endif
  g.view_w = (int)ui.w;
  g.view_h = (int)ui.h;
  g.dir = 1;
  g.gif_slot_index = -1;
  g.s[0].index = g.s[1].index = -1;
  reset_view();
  slot_target(&g.s[0], index);
  g.thr = fm_thread_create(loader, "img-load", NULL);
  view_chrome_poke(&g.chrome);
  return g.mx && g.cv && g.thr;
}

/* ---- upload --------------------------------------------------------------------------- */

static void upload_slot(Slot *s) {
  SDL_LockMutex(g.mx);
  bool ready = s->state == SK_READY && s->img.px;
  FmImage im = s->img;
  if (ready) memset(&s->img, 0, sizeof s->img);
  SDL_UnlockMutex(g.mx);
  if (!ready) return;
  if (s->tex) SDL_DestroyTexture(s->tex);
  s->tex = view_tex_rgba(im.px, im.w, im.h);
  if (s->info.kind == IMGK_SVG) {
    s->iw = FM_MAX(1, (int)(s->svg_w + 0.5f));
    s->ih = FM_MAX(1, (int)(s->svg_h + 0.5f));
    s->raster_w = im.w;
  } else {
    bool swap = s->info.orient >= 5;
    s->iw = swap ? s->info.h : s->info.w;
    s->ih = swap ? s->info.w : s->info.h;
    if (s->iw <= 0 || s->ih <= 0) { s->iw = im.w; s->ih = im.h; }
  }
  img_free(&im);
  if (!s->tex) {
    SDL_LockMutex(g.mx);
    s->state = SK_ERROR;
    s->err = FM_ERR_NOMEM;
    SDL_UnlockMutex(g.mx);
  }
}

/* SVG: ask for a sharper (or smaller) raster once the zoom has settled. */
static void svg_maybe_rerender(Slot *s, FmRect v) {
  if (s->info.kind != IMGK_SVG || !s->tex || s->raster_w <= 0) return;
  SDL_LockMutex(g.mx);
  if (g.svg_ready) {
    FmImage im = g.svg_img;
    memset(&g.svg_img, 0, sizeof g.svg_img);
    g.svg_ready = false;
    SDL_UnlockMutex(g.mx);
    SDL_Texture *t = view_tex_rgba(im.px, im.w, im.h);
    if (t) { SDL_DestroyTexture(s->tex); s->tex = t; s->raster_w = im.w; }
    img_free(&im);
    return;
  }
  SDL_UnlockMutex(g.mx);
  if (ui.now - g.last_zoom_change < 250) { view_wake_in(260); return; }
  float want = s->iw * g.scale;
  /* only the visible part matters, but a raster is whole: cap it */
  int bw, bh;
  int ww = (int)want, wh = (int)(s->ih * g.scale);
  decode_box(FM_MAX(ww, 1), FM_MAX(wh, 1), &bw, &bh);
  FM_UNUSED(v);
  if (bw > s->raster_w * 1.4f || bw < s->raster_w * 0.6f) {
    if (abs(bw - s->raster_w) < 8) return;
    SDL_LockMutex(g.mx);
    g.svg_req = true;
    g.svg_req_w = bw;
    g.svg_req_h = bh;
    g.svg_req_gen++;
    SDL_CondBroadcast(g.cv);
    SDL_UnlockMutex(g.mx);
    s->raster_w = bw;              /* do not ask again until it arrives */
  }
}

/* ---- input ---------------------------------------------------------------------------- */

static void handle_gestures(FmRect v, Slot *s, bool ready, float chrome) {
  u32 id = ui_id("img.view");
  FmRect hv = v;
  if (chrome > 0.5f) rect_cut_top(&hv, ui.m.bar_h);   /* the bar's buttons own that strip */
  int f = ui_hit(id, hv);
  float fit = ready ? fit_scale(s, v) : 1;

  /* wheel zoom around the cursor */
  if (ready && ui.wheel != 0 && ui_input_ok() && rect_has(v, ui.mx, ui.my)) {
    float to = g.scale * powf(1.18f, ui.wheel);
    to = FM_CLAMP(to, fit * 0.5f, max_scale(s, v));
    g.zanim = false;
    zoom_at(v, to, ui.mx, ui.my);
    ui.wheel = 0;
  }
  /* pinch */
  if (ready && ui.nfingers >= 2) {
    if (!g.pinching) { g.pinching = true; g.pinch_last_cx = ui.pinch_cx; g.pinch_last_cy = ui.pinch_cy; }
    g.gesture = G_PAN;
    g.swipe_dx = g.dismiss_dy = 0;
    if (ui.pinch != 1.0f) {
      float to = FM_CLAMP(g.scale * ui.pinch, fit * 0.5f, max_scale(s, v) * 1.2f);
      g.zanim = false;
      zoom_at(v, to, ui.pinch_cx, ui.pinch_cy);
    }
    g.px += ui.pinch_cx - g.pinch_last_cx;
    g.py += ui.pinch_cy - g.pinch_last_cy;
    g.pinch_last_cx = ui.pinch_cx;
    g.pinch_last_cy = ui.pinch_cy;
    ui.drag_owner = id;
  } else if (g.pinching && ui.nfingers == 0) {
    g.pinching = false;
    g.last_mx = ui.mx;
    g.last_my = ui.my;
  }

  /* double click / double tap: fit <-> 100% (or 2x) */
  if ((f & UI_DCLICK) && ready) {
    g.tap_pending = false;
    g.ignore_click = true;
    float to;
    if (at_fit(s, v)) to = fit < 0.95f ? 1.0f : fit * 2.0f;
    else to = fit;
    zoom_anim(v, to, ui.mx, ui.my);
  }
  if (f & UI_PRESS) {
    g.gx0 = ui.mx; g.gy0 = ui.my;
    g.last_mx = ui.mx; g.last_my = ui.my;
    g.gvx = g.gvy = 0;
    g.vx = g.vy = 0;
    g.gesture = G_NONE;
  }
  if ((f & UI_DRAG) && !g.pinching && ui.nfingers < 2) {
    ui.drag_owner = id;
    float dx = ui.mx - g.last_mx, dy = ui.my - g.last_my;
    if (g.gesture == G_NONE) {
      float tx = ui.mx - g.gx0, ty = ui.my - g.gy0;
      if (ready && at_fit(s, v)) {
        if (fabsf(tx) > fabsf(ty)) g.gesture = g.n > 1 ? G_SWIPE : G_NONE;
        else g.gesture = ty > 0 ? G_DISMISS : G_NONE;
        if (g.gesture == G_NONE) g.gesture = G_PAN;
      } else {
        g.gesture = G_PAN;
      }
      dx = tx;
      dy = ty;
    }
    if (g.gesture == G_PAN && ready) { g.px += dx; g.py += dy; g.fitted = false; }
    else if (g.gesture == G_SWIPE) g.swipe_dx = ui.mx - g.gx0;
    else if (g.gesture == G_DISMISS) g.dismiss_dy = FM_MAX(0.0f, ui.my - g.gy0);
    if (ui.dt > 0) {
      g.gvx = g.gvx * 0.5f + (dx / ui.dt) * 0.5f;
      g.gvy = g.gvy * 0.5f + (dy / ui.dt) * 0.5f;
    }
    g.last_mx = ui.mx;
    g.last_my = ui.my;
  }
  if (ui.released && g.gesture != G_NONE) {
    if (g.gesture == G_PAN) {
      g.vx = FM_CLAMP(g.gvx, -DP(4000), DP(4000));
      g.vy = FM_CLAMP(g.gvy, -DP(4000), DP(4000));
    } else if (g.gesture == G_SWIPE) {
      bool fling = fabsf(g.gvx) > DP(600);
      int d = 0;
      if (g.swipe_dx < -v.w * 0.2f || (fling && g.gvx < 0)) d = 1;
      else if (g.swipe_dx > v.w * 0.2f || (fling && g.gvx > 0)) d = -1;
      int idx = s->index + d;
      if (d != 0 && idx >= 0 && idx < g.n) {
        float from = g.swipe_dx + (d > 0 ? v.w : -v.w);
        g.swipe_dx = 0;
        go_to(idx, from);
      }
    } else if (g.gesture == G_DISMISS) {
      if (g.dismiss_dy > v.h * 0.15f || g.gvy > DP(900)) {
        app_close_viewer();
        return;
      }
    }
    g.gesture = G_NONE;
  }
  /* a click without movement: maybe the first of two */
  if (f & UI_CLICK) {
    if (g.ignore_click) g.ignore_click = false;
    else { g.tap_pending = true; g.tap_t = ui.now; view_wake_in(300); }
  }
  if (g.tap_pending && ui.now - g.tap_t >= 280) {
    g.tap_pending = false;
    view_chrome_toggle(&g.chrome);
  }
}

static void animate_view(FmRect v, Slot *s, bool ready) {
  /* zoom animation */
  if (g.zanim) {
    float t = FM_MIN(1.0f, (float)(ui.now - g.z_t0) / 220.0f);
    float e = ui_ease_out(t);
    float sc = g.z_from + (g.z_to - g.z_from) * e;
    g.scale = g.z_from;
    g.px = g.z_px0;
    g.py = g.z_py0;
    zoom_at(v, sc, g.z_ax, g.z_ay);
    if (t >= 1.0f) {
      g.zanim = false;
      if (ready && fabsf(g.z_to - fit_scale(s, v)) < 1e-4f) { g.fitted = true; g.px = g.py = 0; }
    }
    clamp_pan(s, v, true);
    ui_animate();
  }
  if (g.gesture == G_NONE) {
    /* swipe / dismiss spring back */
    if (fabsf(g.swipe_dx) > 0.5f) { g.swipe_dx *= expf(-ui.dt * 14); ui_animate(); } else g.swipe_dx = 0;
    if (g.dismiss_dy > 0.5f) { g.dismiss_dy *= expf(-ui.dt * 14); ui_animate(); } else g.dismiss_dy = 0;
    /* inertia */
    if (fabsf(g.vx) > DP(20) || fabsf(g.vy) > DP(20)) {
      g.px += g.vx * ui.dt;
      g.py += g.vy * ui.dt;
      float k = expf(-ui.dt * 4.0f);
      g.vx *= k;
      g.vy *= k;
      ui_animate();
    } else {
      g.vx = g.vy = 0;
    }
    /* below fit: ease back to fit */
    if (ready && !g.zanim && !g.pinching && g.scale < fit_scale(s, v) * 0.999f) {
      zoom_anim(v, fit_scale(s, v), v.x + v.w * 0.5f, v.y + v.h * 0.5f);
    }
    if (ready && !g.pinching) clamp_pan(s, v, false);
  }
  if (fabsf(g.slide) > 0.5f) { g.slide *= expf(-ui.dt * 12); ui_animate(); } else g.slide = 0;
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void draw_slot(Slot *s, FmRect v, float offx, float offy, float extra_scale, float alpha) {
  if (!s->tex || s->iw <= 0) return;
  float dw, dh;
  disp_size(s, &dw, &dh);
  float sc = (s == cur_slot() ? g.scale : fit_scale(s, v)) * extra_scale;
  float w = dw * sc, h = dh * sc;
  float cx = v.x + v.w * 0.5f + offx + (s == cur_slot() ? g.px : 0);
  float cy = v.y + v.h * 0.5f + offy + (s == cur_slot() ? g.py : 0);
  FmRect d = { cx - w * 0.5f, cy - h * 0.5f, w, h };
  SDL_Texture *t = s->tex;
  if (s == cur_slot() && g.gif_has_frame && g.gif_slot_index == s->index) t = g.gif_tex;
  view_tex_rot(t, d, s == cur_slot() ? g.rot : 0, col_alpha(VIEW_FG, alpha));
}

static void info_card(Slot *s) {
  char dims[48], size[32], frames[24], when[40], type[32];
  fm_snprintf(dims, sizeof dims, "%d \xC3\x97 %d", s->info.orient >= 5 ? s->info.h : s->info.w,
              s->info.orient >= 5 ? s->info.w : s->info.h);
  fm_fmt_size(s->fsize, size, sizeof size);
  fm_snprintf(frames, sizeof frames, "%d", s->info.frames);
  fm_fmt_time(s->mtime, when, sizeof when);
  fm_strlcpy(type, img_kind_name(s->info.kind), sizeof type);
  const char *keys[7] = { "Name", "Dimensions", "Size", "Type", "Modified", "Path", "Frames" };
  const char *vals[7] = { fm_path_base(g.paths[s->index]), dims, size, type, when, g.paths[s->index], frames };
  int n = s->info.kind == IMGK_GIF ? 7 : 6;
  view_info_dialog(ui_id("img.info"), "Image info", keys, vals, n, &g.info_open);
}

enum { IM_SHARE = 1, IM_OPEN, IM_INFO, IM_SLIDE };

static void img_frame(FmRect area) {
  gfx_rect(area, VIEW_BG);
  g.view_w = (int)area.w;
  g.view_h = (int)area.h;
  Slot *s = cur_slot();
  upload_slot(s);
  upload_slot(other_slot());
  bool ready = s->tex != NULL && s->iw > 0;
  FmRect v = area;

  /* first frame of a fresh image: fit, then animate GIFs */
  if (ready && g.fitted) g.scale = fit_scale(s, v);
  if (ready && s->info.kind == IMGK_GIF && s->info.frames > 1 && g.gif_slot_index != s->index) gif_start(s);
  gif_pump();
  /* preload the neighbour once the current one is in */
  if (s->state != SK_LOADING && g.n > 1) {
    int want = s->index + g.dir;
    if (want < 0 || want >= g.n) want = s->index - g.dir;
    if (want >= 0 && want < g.n && other_slot()->index != want) slot_target(other_slot(), want);
  }

  float chrome = view_chrome(&g.chrome, ui_id("img.chrome"), g.slideshow);

  /* keys */
  if (ui_key(SDLK_RIGHT, 0) || ui_key(SDLK_PAGEDOWN, 0)) nav(1, false);
  if (ui_key(SDLK_LEFT, 0) || ui_key(SDLK_PAGEUP, 0)) nav(-1, false);
  if (ui_key(SDLK_HOME, 0)) go_to(0, -g.view_w * 0.25f);
  if (ui_key(SDLK_END, 0)) go_to(g.n - 1, g.view_w * 0.25f);
  if (ready && (ui_key(SDLK_EQUALS, 0) || ui_key(SDLK_PLUS, 0) || ui_key(SDLK_KP_PLUS, 0)))
    zoom_anim(v, FM_MIN(g.scale * 1.5f, max_scale(s, v)), v.x + v.w * 0.5f, v.y + v.h * 0.5f);
  if (ready && (ui_key(SDLK_MINUS, 0) || ui_key(SDLK_KP_MINUS, 0)))
    zoom_anim(v, FM_MAX(g.scale / 1.5f, fit_scale(s, v)), v.x + v.w * 0.5f, v.y + v.h * 0.5f);
  if (ready && ui_key(SDLK_0, 0)) zoom_anim(v, fit_scale(s, v), v.x + v.w * 0.5f, v.y + v.h * 0.5f);
  if (ready && ui_key(SDLK_1, 0)) zoom_anim(v, 1.0f, v.x + v.w * 0.5f, v.y + v.h * 0.5f);
  bool rotate = ui_key(SDLK_r, 0);
  bool toggle_slide = ui_key(SDLK_SPACE, 0) || ui_key(SDLK_F5, 0);
  if (ui_key(SDLK_i, 0) && s->state == SK_READY) g.info_open = !g.info_open;

  handle_gestures(v, s, ready, chrome);
  if (!g.open) return;
  animate_view(v, s, ready);

  /* draw: neighbours while swiping, the image, dismiss fade */
  float dismiss_t = FM_CLAMP(g.dismiss_dy / (v.h * 0.6f), 0.0f, 1.0f);
  if (dismiss_t > 0) {
    gfx_rect(area, T.bg);
    gfx_rect(area, col_alpha(VIEW_BG, 1.0f - dismiss_t));
  }
  if (g.swipe_dx != 0) {
    Slot *o = other_slot();
    int nb = s->index + (g.swipe_dx < 0 ? 1 : -1);
    if (o->index == nb && o->tex) draw_slot(o, v, g.swipe_dx + (g.swipe_dx < 0 ? v.w : -v.w) + (g.swipe_dx < 0 ? DP(16) : -DP(16)), 0, 1, 1);
  }
  gfx_clip_push(v);
  if (ready) {
    float ds = 1.0f - dismiss_t * 0.25f;
    draw_slot(s, v, g.swipe_dx + g.slide, g.dismiss_dy, ds, 1.0f);
  } else if (s->state == SK_LOADING) {
    float sp = DP(40);
    ui_spinner(rect_center(v, sp, sp), VIEW_FG2);
  }
  gfx_clip_pop();
  if (s->state == SK_ERROR) {
    const char *why = s->err == FM_ERR_UNSUPPORTED ? "This image is too large or uses an unsupported feature."
                    : s->err == FM_ERR_NOT_FOUND ? "The file is gone." : "The file could not be decoded.";
    view_message(v, IC_IMAGE, "Cannot show this image", why, VIEW_FG, VIEW_FG2);
    float bw = DP(220), bh = DP(ui.touch_mode ? 46 : 38);
    if (ui_button(ui_id("img.ext"), FM_RECT(v.x + (v.w - bw) * 0.5f, v.y + v.h * 0.5f + DP(110), bw, bh),
                  IC_OPEN_WITH, "Open with system app", UI_BTN_TONAL))
      plat_open_external(g.paths[s->index]);
  }

  /* desktop chevrons */
  if (!ui.touch_mode && g.n > 1 && chrome > 0.01f && dismiss_t == 0) {
    float bs = DP(44);
    for (int k = -1; k <= 1; k += 2) {
      int idx = s->index + k;
      if (idx < 0 || idx >= g.n) continue;
      FmRect b = { k < 0 ? v.x + DP(12) : v.x + v.w - bs - DP(12), v.y + (v.h - bs) * 0.5f, bs, bs };
      FmRect zone = { k < 0 ? v.x : v.x + v.w - v.w * 0.2f, v.y + ui.m.bar_h, v.w * 0.2f, v.h - ui.m.bar_h * 2 };
      float a = ui_anim(ui_idn(ui_id("img.chev"), (u32)(k + 1)), ui_hover(zone) ? 1.0f : 0.0f, 12.0f) * chrome;
      if (a < 0.02f) continue;
      gfx_circle(b.x + bs * 0.5f, b.y + bs * 0.5f, bs * 0.5f, col_alpha(VIEW_SCRIM, a));
      if (ui_icon_btn(ui_idn(ui_id("img.chevb"), (u32)(k + 1)), b, k < 0 ? IC_CHEVRON_LEFT : IC_CHEVRON_RIGHT,
                      col_alpha(VIEW_FG, a), k < 0 ? "Previous" : "Next"))
        nav(k, false);
    }
  }

  /* zoom badge */
  if (ready && ui.now < g.zoom_shown_until) {
    char zb[16];
    fm_snprintf(zb, sizeof zb, "%d%%", (int)(g.scale * 100 + 0.5f));
    float w = font_width(FONT_BOLD, ui.m.font_small, zb, -1) + DP(20), h = DP(28);
    FmRect r = { v.x + (v.w - w) * 0.5f, v.y + v.h - h - DP(24), w, h };
    gfx_rrect(r, h * 0.5f, VIEW_SCRIM);
    font_draw_center(FONT_BOLD, ui.m.font_small, r, zb, VIEW_FG);
    view_wake_in((u32)(g.zoom_shown_until - ui.now) + 10);
  }

  /* top bar */
  char sub[96];
  if (ready || s->state == SK_READY) {
    int w = s->info.orient >= 5 ? s->info.h : s->info.w, h = s->info.orient >= 5 ? s->info.w : s->info.h;
    if (g.n > 1) fm_snprintf(sub, sizeof sub, "%d / %d  \xC2\xB7  %d \xC3\x97 %d", s->index + 1, g.n, w, h);
    else fm_snprintf(sub, sizeof sub, "%d \xC3\x97 %d", w, h);
  } else if (g.n > 1) {
    fm_snprintf(sub, sizeof sub, "%d / %d", s->index + 1, g.n);
  } else {
    sub[0] = 0;
  }
  float ca = chrome * (1.0f - dismiss_t);
  bool narrow = ui.w < DP(520);
  FmRect act;
  const char *title = g_title[0] && g.n == 1 ? g_title : fm_path_base(g.paths[s->index]);
  if (view_topbar(area, VIEW_BAR_MEDIA, ca, title, sub, narrow ? 3 : 5, &act)) {
    app_close_viewer();
    return;
  }
  u32 mid = ui_id("img.menu");
  if (narrow) {
    if (view_bar_btn(&act, ui_id("img.more"), IC_MORE, "More", VIEW_BAR_MEDIA, ca, false)) {
      FmMenuItem items[] = {
        { IM_SHARE, IC_SHARE, "Share", NULL, 0 },
        { IM_OPEN, IC_OPEN_WITH, "Open with system app", NULL, 0 },
        { IM_INFO, IC_INFO, "Image info", "I", s->state == SK_READY ? 0 : UI_MI_DISABLED },
      };
      ui_menu_open(mid, act.x + act.w, area.y + ui.m.bar_h, items, FM_COUNT(items));
    }
  } else {
    if (view_bar_btn(&act, ui_id("img.info"), IC_INFO, "Info (I)", VIEW_BAR_MEDIA, ca, g.info_open) &&
        s->state == SK_READY)
      g.info_open = true;
    if (view_bar_btn(&act, ui_id("img.open"), IC_OPEN_WITH, "Open with system app", VIEW_BAR_MEDIA, ca, false))
      plat_open_external(g.paths[s->index]);
    if (view_bar_btn(&act, ui_id("img.share"), IC_SHARE, "Share", VIEW_BAR_MEDIA, ca, false))
      plat_share(g.paths[s->index]);
  }
  if (view_bar_btn(&act, ui_id("img.rot"), IC_ROTATE, "Rotate (R)", VIEW_BAR_MEDIA, ca, false)) rotate = true;
  if (g.n > 1 && view_bar_btn(&act, ui_id("img.slide"), g.slideshow ? IC_PAUSE : IC_PLAY,
                              g.slideshow ? "Stop slideshow (Space)" : "Slideshow (Space)", VIEW_BAR_MEDIA, ca,
                              g.slideshow))
    toggle_slide = true;
  switch (ui_menu_result(mid)) {
    case IM_SHARE: plat_share(g.paths[s->index]); break;
    case IM_OPEN: plat_open_external(g.paths[s->index]); break;
    case IM_INFO: g.info_open = true; break;
    default: break;
  }

  if (rotate && ready) {
    g.rot = (g.rot + 1) & 3;
    g.fitted = true;
    g.px = g.py = 0;
    g.zanim = false;
  }
  if (toggle_slide && g.n > 1) {
    g.slideshow = !g.slideshow;
    g.slide_next = ui.now + SLIDE_MS;
    if (g.slideshow) g.chrome.last_input = ui.now - VIEW_HIDE_MS + 600;
    ui_toast(g.slideshow ? "Slideshow" : "Slideshow stopped");
  }
  if (g.slideshow) {
    if (ui.now >= g.slide_next && s->state != SK_LOADING) {
      nav(1, true);
      g.slide_next = ui.now + SLIDE_MS;
    }
    view_wake_in((u32)(g.slide_next > ui.now ? g.slide_next - ui.now : 50));
  }
  if (ready) svg_maybe_rerender(s, v);
  if (g.info_open && s->state == SK_READY) info_card(s);

  if (view_key_back()) {
    if (g.slideshow) { g.slideshow = false; ui_toast("Slideshow stopped"); }
    else app_close_viewer();
  }
}

const FmViewer g_view_image = { "image", img_open, img_frame, img_close };
