/* fonline_thumb.c -- thumbnails of online videos: fetch, decode, cache.
**
** Design decisions:
**   - A fixed table of entries keyed by a hash of the URL and the width
**     asked for; no allocation per thumbnail apart from the pixels. Four
**     worker threads sleep on a condition variable, so nothing runs while
**     the gallery is idle; four, because a fetch is mostly waiting on the
**     network and a photo wall shows twenty pictures or more at once.
**   - Workers take the most recently requested entry first (visible cards
**     before the prefetch of the next screen), fetch it with net_get (capped
**     at 2 MB), decode it at the width it is shown (img_load_mem fits the
**     long edge) and crop the middle to the shape asked for: 16:9 for video
**     cards (YouTube's hqdefault is 4:3 with black bars, the crop removes
**     them and the texture is exactly the card), 1:1 for album covers, or
**     no crop at all for the photo wall (fphoto*.c), which lays pictures
**     out at their own aspect. The shape is part of the key, so both views
**     share one loader, one LRU budget and one disk cache. Textures are
**     created on the main thread only. stb_image reads no WebP, so
**     YouTube's /vi_webp/ pictures are fetched as their JPEG twin.
**   - Requests are renewed every frame by the cards that need them. One not
**     renewed for a few frames was scrolled far away: a queued one is
**     dropped and a running download is cancelled. Textures are an LRU
**     capped by count and bytes (about 120 cards, under 24 MB); the ones
**     drawn in the last two frames are never evicted.
**   - The raw downloaded files are kept in PLACE_CACHE/online-thumbs (they
**     are small JPEGs), trimmed to 40 MB when the folder passes 50 MB, so
**     coming back to a search does not hit the network again.
*/
#include "fonline_int.h"
#include "fview_int.h"
#include "fnet.h"
#include "fdec_img.h"

#define OT_ENTRIES 192
#define OT_WORKERS 4
#define OT_MAX_TEX 120
#define OT_BUDGET (24u * 1024u * 1024u)
#define OT_MAX_FILE (2u * 1024u * 1024u)
#define OT_DISK_MAX (50ull * 1024 * 1024)
#define OT_DISK_KEEP (40ull * 1024 * 1024)
#define OT_FADE_MS 220
#define OT_RETRY_MS 20000

enum { TS_FREE = 0, TS_QUEUED, TS_WORKING, TS_READY, TS_TEX, TS_FAILED };

typedef struct ThE {
  u64 key;
  int state;
  int prio;                  /* 1 visible, 0 prefetch */
  int px;
  float aspect;              /* crop to w/h; 0 = the picture's own shape */
  const char *headers;       /* extra request headers (static, an adapter's), or NULL */
  char url[512];
  FmImage img;               /* READY */
  SDL_Texture *tex;          /* TEX (main thread) */
  size_t bytes;
  u32 used;                  /* frame of the last request */
  u64 seq;                   /* request order, for LIFO */
  u64 t_tex, t_fail;
  bool undecodable;          /* the bytes came but are no picture stb reads (WebP): never again */
  volatile int cancel;
} ThE;

static struct {
  bool init;
  SDL_mutex *mx;
  SDL_cond *cv;
  SDL_Thread *thr[OT_WORKERS];
  bool quit;
  ThE e[OT_ENTRIES];
  u32 frame;
  u64 seq;
  int ntex;
  size_t tex_bytes;
  char dir[FM_PATH_MAX];
  bool dir_ok;
  bool trimmed;
} G;

/* ---- disk cache ------------------------------------------------------------------ */

static u64 hash_str(const char *s) {
  u64 h = 14695981039346656037ull;
  while (*s) { h ^= (u8)*s++; h *= 1099511628211ull; }
  return h;
}

static bool disk_path(const char *url, char *out, size_t cap) {
  if (!G.dir_ok) return false;
  char name[32];
  fm_snprintf(name, sizeof name, "%016llx.img", (unsigned long long)hash_str(url));
  return fm_path_join(out, cap, G.dir, name);
}

static u8 *disk_read(const char *url, size_t *n) {
  char p[FM_PATH_MAX];
  if (!disk_path(url, p, sizeof p)) return NULL;
  FILE *f = fm_fopen(p, "rb");
  if (!f) return NULL;
  i64 sz = fm_fsize(f);
  u8 *buf = NULL;
  if (sz > 8 && sz <= (i64)OT_MAX_FILE) {
    buf = (u8 *)fm_alloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fm_free(buf); buf = NULL; }
    else *n = (size_t)sz;
  }
  fclose(f);
  return buf;
}

static void disk_write(const char *url, const u8 *data, size_t n) {
  char p[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!disk_path(url, p, sizeof p)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.part", p);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  bool ok = fwrite(data, 1, n, f) == n;
  ok = fclose(f) == 0 && ok;
  if (!ok || plat_rename(tmp, p) != FM_OK) plat_remove_file(tmp);
}

typedef struct DiskFile { i64 mtime; u64 size; char name[40]; } DiskFile;

static int cmp_mtime(const void *a, const void *b) {
  i64 x = ((const DiskFile *)a)->mtime, y = ((const DiskFile *)b)->mtime;
  return x < y ? -1 : x > y;
}

/* Oldest files go first until the folder is back under OT_DISK_KEEP. */
static void disk_trim(void) {
  FmDir *d = plat_dir_open(G.dir, NULL);
  if (!d) return;
  int cap = 1024, n = 0;
  DiskFile *list = (DiskFile *)fm_alloc(sizeof *list * (size_t)cap);
  u64 total = 0;
  const char *name;
  FmStat st;
  while (plat_dir_next(d, &name, &st)) {
    if (st.flags & FM_ST_DIR) continue;
    total += st.size;
    if (strlen(name) >= sizeof list[0].name) continue;
    if (n == cap) { cap *= 2; list = (DiskFile *)fm_realloc(list, sizeof *list * (size_t)cap); }
    list[n].mtime = st.mtime;
    list[n].size = st.size;
    fm_strlcpy(list[n].name, name, sizeof list[n].name);
    n++;
  }
  plat_dir_close(d);
  if (total > OT_DISK_MAX) {
    qsort(list, (size_t)n, sizeof *list, cmp_mtime);
    char p[FM_PATH_MAX];
    for (int i = 0; i < n && total > OT_DISK_KEEP; i++)
      if (fm_path_join(p, sizeof p, G.dir, list[i].name) && plat_remove_file(p) == FM_OK) total -= list[i].size;
  }
  fm_free(list);
}

/* ---- worker ------------------------------------------------------------------------ */

/* Middle band of im with aspect a = w/h (rows of a tall one, columns of a wide one). */
static FmImage crop_aspect(FmImage im, float a) {
  int w = im.w, h = im.h;
  if (a <= 0) return im;
  int ch = (int)((float)w / a + 0.5f), cw = w;
  if (ch > h) { ch = h; cw = (int)((float)h * a + 0.5f); }
  if (cw == w && ch == h) return im;
  if (cw < 1 || ch < 1) return im;
  int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
  FmImage out = { cw, ch, (u8 *)fm_alloc((size_t)cw * ch * 4) };
  for (int y = 0; y < ch; y++)
    memcpy(out.px + (size_t)y * cw * 4, im.px + ((size_t)(y0 + y) * w + x0) * 4, (size_t)cw * 4);
  img_free(&im);
  return out;
}

/* stb_image has no WebP; YouTube serves the same pictures as JPEG under
** /vi/ (yt-dlp often hands out the /vi_webp/ ones). False when not one. */
static bool webp_to_jpeg(const char *url, char *out, size_t cap) {
  const char *p = strstr(url, "i.ytimg.com/vi_webp/");
  if (!p) return false;
  const char *id = p + strlen("i.ytimg.com/vi_webp/");
  const char *slash = strchr(id, '/');
  if (!slash || slash - id > 32) return false;
  fm_snprintf(out, cap, "https://i.ytimg.com/vi/%.*s/hqdefault.jpg", (int)(slash - id), id);
  return true;
}

static int pick(void) {
  int best = -1;
  for (int i = 0; i < OT_ENTRIES; i++) {
    ThE *e = &G.e[i];
    if (e->state != TS_QUEUED) continue;
    if (best < 0 || e->prio > G.e[best].prio || (e->prio == G.e[best].prio && e->seq > G.e[best].seq)) best = i;
  }
  return best;
}

static int worker(void *u) {
  int idx = (int)(intptr_t)u;
  if (idx == 0 && !G.trimmed) { G.trimmed = true; disk_trim(); }
  char url[512];
  SDL_LockMutex(G.mx);
  while (!G.quit) {
    int i = pick();
    if (i < 0) { SDL_CondWait(G.cv, G.mx); continue; }
    ThE *e = &G.e[i];
    e->state = TS_WORKING;
    e->cancel = 0;
    u64 key = e->key;
    int px = e->px;
    float aspect = e->aspect;
    const char *headers = e->headers;
    fm_strlcpy(url, e->url, sizeof url);
    SDL_UnlockMutex(G.mx);

    FmImage im;
    memset(&im, 0, sizeof im);
    FmErr err = FM_ERR_IO;
    size_t n = 0;
    u8 *data = disk_read(url, &n);
    bool from_disk = data != NULL;
    if (!data) {
      FmNetResp r;
      memset(&r, 0, sizeof r);
      char jpg[160];
      const char *get = webp_to_jpeg(url, jpg, sizeof jpg) ? jpg : url;
      if (net_get(get, headers, OT_MAX_FILE, &r, &e->cancel) == FM_OK && r.status == 200 && r.data && r.len > 8) {
        data = r.data;
        n = r.len;
        r.data = NULL;
      }
      net_resp_free(&r);
    }
    bool fetched = data != NULL;
    if (data) {
      /* the long edge is what img_load_mem fits: a 4:3 picture px wide is
      ** px * 3/4 high, which still covers a px-wide 16:9 crop */
      err = img_load_mem(data, n, px, &im);
      if (err == FM_OK && !from_disk) disk_write(url, data, n);
      fm_free(data);
      if (err == FM_OK) im = crop_aspect(im, aspect);
    }

    SDL_LockMutex(G.mx);
    if (e->key == key && e->state == TS_WORKING) {
      if (err == FM_OK && im.px) {
        e->img = im;
        e->state = TS_READY;
        im.px = NULL;
      } else if (e->cancel) {
        e->state = TS_FREE;               /* scrolled away; asked again later */
      } else {
        e->state = TS_FAILED;
        e->t_fail = SDL_GetTicks64();
        e->undecodable = fetched;
      }
    }
    if (im.px) img_free(&im);
    SDL_UnlockMutex(G.mx);
    app_wake();
    SDL_LockMutex(G.mx);
  }
  SDL_UnlockMutex(G.mx);
  return 0;
}

static void lazy_init(void) {
  if (G.init) return;
  G.init = true;
  G.mx = SDL_CreateMutex();
  G.cv = SDL_CreateCond();
  char cache[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, cache, sizeof cache) && fm_path_join(G.dir, sizeof G.dir, cache, "online-thumbs"))
    G.dir_ok = plat_mkdirs(G.dir) == FM_OK || plat_is_dir(G.dir);
  if (!G.mx || !G.cv) return;
  for (int i = 0; i < OT_WORKERS; i++) G.thr[i] = fm_thread_create(worker, "othumb", (void *)(intptr_t)i);
}

/* ---- main thread ---------------------------------------------------------------------- */

void othumb_init(void) { memset(&G, 0, sizeof G); }

static void drop_tex(ThE *e) {
  if (e->tex) {
    SDL_DestroyTexture(e->tex);
    G.ntex--;
    G.tex_bytes -= e->bytes;
  }
  e->tex = NULL;
  e->bytes = 0;
}

void othumb_shutdown(void) {
  if (!G.init) return;
  SDL_LockMutex(G.mx);
  G.quit = true;
  for (int i = 0; i < OT_ENTRIES; i++) G.e[i].cancel = 1;
  SDL_CondBroadcast(G.cv);
  SDL_UnlockMutex(G.mx);
  for (int i = 0; i < OT_WORKERS; i++)
    if (G.thr[i]) SDL_WaitThread(G.thr[i], NULL);
  for (int i = 0; i < OT_ENTRIES; i++) {
    drop_tex(&G.e[i]);
    img_free(&G.e[i].img);
  }
  if (G.cv) SDL_DestroyCond(G.cv);
  if (G.mx) SDL_DestroyMutex(G.mx);
  memset(&G, 0, sizeof G);
}

/* Least recently used entry that may be recycled, or -1. */
static int victim(void) {
  int best = -1;
  for (int i = 0; i < OT_ENTRIES; i++) {
    ThE *e = &G.e[i];
    if (e->state == TS_FREE) return i;
    if (e->state == TS_WORKING || e->state == TS_READY) continue;
    if (e->used + 2 > G.frame) continue;
    if (best < 0 || e->used < G.e[best].used) best = i;
  }
  return best;
}

static SDL_Texture *request(const char *url, int px, float aspect, const char *headers, int prio, float *fade) {
  if (fade) *fade = 1;
  if (!url || !url[0] || px <= 0) return NULL;
  lazy_init();
  if (!G.mx) return NULL;
  px = FM_CLAMP((px + 63) / 64 * 64, 64, 1280);   /* small resizes reuse the texture */
  if (aspect < 0) aspect = 0;
  u64 key = hash_str(url) ^ ((u64)px * 0x9E3779B97F4A7C15ull) ^ ((u64)(aspect * 1000.0f + 0.5f) << 40);
  SDL_LockMutex(G.mx);
  ThE *e = NULL;
  for (int i = 0; i < OT_ENTRIES; i++)
    if (G.e[i].state != TS_FREE && G.e[i].key == key) { e = &G.e[i]; break; }
  SDL_Texture *tex = NULL;
  if (e) {
    e->used = G.frame;
    if (prio > e->prio || e->state != TS_QUEUED) e->prio = FM_MAX(e->prio, prio);
    if (e->state == TS_QUEUED && prio) e->seq = ++G.seq;
    if (e->state == TS_TEX) {
      tex = e->tex;
      if (fade) *fade = FM_MIN(1.0f, (float)(ui.now - e->t_tex) / OT_FADE_MS);
    } else if (e->state == TS_FAILED && !e->undecodable && SDL_GetTicks64() - e->t_fail > OT_RETRY_MS) {
      e->state = TS_QUEUED;
      e->seq = ++G.seq;
      SDL_CondSignal(G.cv);
    }
  } else {
    int v = victim();
    if (v >= 0) {
      e = &G.e[v];
      drop_tex(e);
      img_free(&e->img);
      memset(e, 0, sizeof *e);
      e->key = key;
      e->px = px;
      e->aspect = aspect;
      e->headers = headers;
      e->prio = prio;
      fm_strlcpy(e->url, url, sizeof e->url);
      e->used = G.frame;
      e->seq = ++G.seq;
      e->state = TS_QUEUED;
      SDL_CondSignal(G.cv);
    }
  }
  SDL_UnlockMutex(G.mx);
  return tex;
}

#define VIDEO_ASPECT (16.0f / 9.0f)

SDL_Texture *othumb_get(const char *url, int px, float *fade) {
  return request(url, px, VIDEO_ASPECT, NULL, 1, fade);
}
void othumb_prefetch(const char *url, int px) { request(url, px, VIDEO_ASPECT, NULL, 0, NULL); }

SDL_Texture *othumb_get_ex(const char *url, int px, float aspect, const char *headers, float *fade) {
  return request(url, px, aspect, headers, 1, fade);
}
void othumb_prefetch_ex(const char *url, int px, float aspect, const char *headers) {
  request(url, px, aspect, headers, 0, NULL);
}

void othumb_pump(void) {
  if (!G.init || !G.mx) return;
  G.frame++;
  SDL_LockMutex(G.mx);
  for (int i = 0; i < OT_ENTRIES; i++) {
    ThE *e = &G.e[i];
    if (e->state == TS_READY) {
      e->tex = view_tex_rgba(e->img.px, e->img.w, e->img.h);
      e->bytes = (size_t)e->img.w * e->img.h * 4;
      img_free(&e->img);
      if (e->tex) {
        e->state = TS_TEX;
        e->t_tex = SDL_GetTicks64();
        G.ntex++;
        G.tex_bytes += e->bytes;
        ui_redraw();
      } else {
        e->bytes = 0;
        e->state = TS_FAILED;
        e->t_fail = SDL_GetTicks64();
      }
    } else if (e->state == TS_QUEUED && e->used + 3 < G.frame) {
      e->state = TS_FREE;                 /* scrolled away before it started */
    } else if (e->state == TS_WORKING && e->used + 30 < G.frame) {
      e->cancel = 1;                      /* far away: stop the download */
    }
  }
  /* LRU: over the count or byte budget, drop textures not drawn lately */
  while (G.ntex > OT_MAX_TEX || G.tex_bytes > OT_BUDGET) {
    int best = -1;
    for (int i = 0; i < OT_ENTRIES; i++) {
      ThE *e = &G.e[i];
      if (e->state != TS_TEX || e->used + 2 > G.frame) continue;
      if (best < 0 || e->used < G.e[best].used) best = i;
    }
    if (best < 0) break;
    drop_tex(&G.e[best]);
    G.e[best].state = TS_FREE;
  }
  SDL_UnlockMutex(G.mx);
}

void othumb_forget_queue(void) {
  if (!G.init || !G.mx) return;
  SDL_LockMutex(G.mx);
  for (int i = 0; i < OT_ENTRIES; i++)
    if (G.e[i].state == TS_QUEUED) G.e[i].state = TS_FREE;
  SDL_UnlockMutex(G.mx);
}

void othumb_reset(void) {
  if (!G.init || !G.mx) return;
  SDL_LockMutex(G.mx);
  for (int i = 0; i < OT_ENTRIES; i++) {
    ThE *e = &G.e[i];
    if (e->state == TS_TEX) { drop_tex(e); e->state = TS_FREE; }
    else if (e->state == TS_QUEUED) e->state = TS_FREE;
  }
  SDL_UnlockMutex(G.mx);
}

size_t othumb_bytes(void) { return G.tex_bytes; }
