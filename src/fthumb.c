/* fthumb.c -- background thumbnails: worker pool, LRU texture cache, disk cache.
**
** Design decisions:
**   - Requests go on a LIFO stack: the rows the user just scrolled to are
**     decoded first, and asking again for a queued file moves it to the
**     top. When the stack is full the oldest request is dropped; the panel
**     simply asks again if the row is still visible.
**   - Workers return downscaled RGBA only; textures are created on the main
**     thread (thumb_get or thumb_pump) and kept in an LRU bounded by bytes.
**     Textures used in the last two frames are never evicted, so a screen
**     with more thumbnails than the budget degrades to icons instead of
**     thrashing.
**   - The disk cache stores raw RGBA with a 12-byte header under
**     PLACE_CACHE/thumbs, named by a 64-bit hash of path, size, mtime and
**     edge. Only expensive decodes are stored (big images, videos, audio
**     covers); the folder is trimmed to 48 MB at start-up.
**   - Entry state lives under one mutex; workers never touch textures.
*/
#include "fthumb.h"
#include "fview_int.h"
#include "fplat.h"
#include "fdec_img.h"
#include "fdec_aud.h"
#include "fdec_vid.h"

#define TH_ENTRIES 2048
#define TH_BUCKETS 1024
#define TH_STACK 192
#define TH_BUDGET (12u * 1024u * 1024u)
#define TH_MAX_FILE (64ll * 1024 * 1024)
#define TH_DISK_MIN (256 * 1024)
#define TH_DISK_MAX (64ull * 1024 * 1024)
#define TH_DISK_KEEP (48ull * 1024 * 1024)

enum { TS_FREE = 0, TS_QUEUED, TS_WORKING, TS_READY, TS_TEX, TS_FAILED };

typedef struct ThEntry {
  u64 key;
  int state;
  int next;                /* hash chain, -1 = end */
  char *path;
  i64 mtime;
  u64 size;
  int px;
  FmType type;
  FmImage img;             /* READY */
  SDL_Texture *tex;        /* TEX (main thread only) */
  size_t bytes;
  u32 used;                /* frame of the last thumb_get */
} ThEntry;

static struct {
  bool init;
  SDL_mutex *mx;
  SDL_cond *cv;
  SDL_Thread *thr[2];
  int nthr;
  bool quit;
  ThEntry e[TH_ENTRIES];
  int bucket[TH_BUCKETS];
  int stack[TH_STACK];
  int nstack;
  size_t tex_bytes;
  u32 frame;
  char cache_dir[FM_PATH_MAX];
  bool cache_ok;
  bool trimmed;
} g;

/* ---- hashing ------------------------------------------------------------------- */

static u64 fnv(u64 h, const void *p, size_t n) {
  const u8 *b = (const u8 *)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

static u64 thumb_key(const char *path, i64 mtime, u64 size, int px) {
  u64 h = fnv(14695981039346656037ull, path, strlen(path));
  h = fnv(h, &mtime, sizeof mtime);
  h = fnv(h, &size, sizeof size);
  h = fnv(h, &px, sizeof px);
  return h ? h : 1;
}

static int find_entry(u64 key, const char *path) {
  for (int i = g.bucket[key % TH_BUCKETS]; i >= 0; i = g.e[i].next)
    if (g.e[i].key == key && g.e[i].state != TS_FREE && strcmp(g.e[i].path, path) == 0) return i;
  return -1;
}

static void unlink_entry(int i) {
  int *pp = &g.bucket[g.e[i].key % TH_BUCKETS];
  while (*pp >= 0) {
    if (*pp == i) { *pp = g.e[i].next; break; }
    pp = &g.e[*pp].next;
  }
}

/* Main thread (destroys textures) with g.mx held. */
static void free_entry(int i) {
  ThEntry *e = &g.e[i];
  if (e->state == TS_FREE) return;
  unlink_entry(i);
  if (e->tex) { SDL_DestroyTexture(e->tex); g.tex_bytes -= e->bytes; }
  img_free(&e->img);
  fm_free(e->path);
  memset(e, 0, sizeof *e);
  e->next = -1;
}

static void stack_remove(int i) {
  for (int k = 0; k < g.nstack; k++)
    if (g.stack[k] == i) {
      memmove(g.stack + k, g.stack + k + 1, sizeof(int) * (size_t)(g.nstack - k - 1));
      g.nstack--;
      return;
    }
}

/* A free slot, evicting failed or least recently used finished entries. */
static int alloc_entry(void) {
  int best = -1;
  u32 best_age = 0;
  for (int i = 0; i < TH_ENTRIES; i++) {
    ThEntry *e = &g.e[i];
    if (e->state == TS_FREE) return i;
    if (e->state == TS_QUEUED || e->state == TS_WORKING) continue;
    if (g.frame - e->used < 2) continue;
    u32 age = g.frame - e->used + (e->state == TS_FAILED ? 0x40000000u : 0);
    if (best < 0 || age > best_age) { best = i; best_age = age; }
  }
  if (best >= 0) free_entry(best);
  return best;
}

/* ---- disk cache ----------------------------------------------------------------- */

static void cache_name(char *out, size_t cap, u64 key) {
  char n[32];
  fm_snprintf(n, sizeof n, "%016llx.thb", (unsigned long long)key);
  fm_path_join(out, cap, g.cache_dir, n);
}

static bool cache_read(u64 key, int px, FmImage *out) {
  if (!g.cache_ok) return false;
  char p[FM_PATH_MAX];
  cache_name(p, sizeof p, key);
  FILE *f = fm_fopen(p, "rb");
  if (!f) return false;
  u8 h[12];
  bool ok = false;
  if (fread(h, 1, 12, f) == 12 && memcmp(h, "MMTH", 4) == 0 && h[4] == 1) {
    int w = h[6] | h[7] << 8, hh = h[8] | h[9] << 8;
    if (w > 0 && hh > 0 && w <= px && hh <= px) {
      out->w = w;
      out->h = hh;
      out->px = (u8 *)fm_alloc((size_t)w * hh * 4);
      ok = fread(out->px, 4, (size_t)w * hh, f) == (size_t)w * hh;
      if (!ok) img_free(out);
    }
  }
  fclose(f);
  return ok;
}

static void cache_write(u64 key, const FmImage *im) {
  if (!g.cache_ok || !im->px) return;
  char p[FM_PATH_MAX], tmp[FM_PATH_MAX];
  cache_name(p, sizeof p, key);
  fm_snprintf(tmp, sizeof tmp, "%s.%llu.tmp", p, (unsigned long long)plat_now_ms());
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  u8 h[12] = { 'M', 'M', 'T', 'H', 1, 0, (u8)im->w, (u8)(im->w >> 8), (u8)im->h, (u8)(im->h >> 8), 0, 0 };
  bool ok = fwrite(h, 1, 12, f) == 12 && fwrite(im->px, 4, (size_t)im->w * im->h, f) == (size_t)im->w * im->h;
  ok = fclose(f) == 0 && ok;
  if (!ok || plat_rename(tmp, p) != FM_OK) plat_remove_file(tmp);
}

typedef struct CacheFile { i64 mtime; u64 size; char name[40]; } CacheFile;

static int cf_cmp(const void *a, const void *b) {
  i64 x = ((const CacheFile *)a)->mtime, y = ((const CacheFile *)b)->mtime;
  return x < y ? -1 : x > y;
}

/* Keeps the cache folder under TH_DISK_MAX, deleting the oldest files. */
static void cache_trim(void) {
  FmErr err;
  FmDir *d = plat_dir_open(g.cache_dir, &err);
  if (!d) return;
  struct { CacheFile *items; int count, cap; } v = { 0 };
  u64 total = 0;
  const char *name;
  FmStat st;
  while (plat_dir_next(d, &name, &st)) {
    if (st.flags & FM_ST_DIR) continue;
    CacheFile cf;
    cf.mtime = st.mtime;
    cf.size = st.size;
    fm_strlcpy(cf.name, name, sizeof cf.name);
    FM_VEC_PUSH(v, cf);
    total += st.size;
  }
  plat_dir_close(d);
  if (total > TH_DISK_MAX) {
    qsort(v.items, (size_t)v.count, sizeof(CacheFile), cf_cmp);
    char p[FM_PATH_MAX];
    for (int i = 0; i < v.count && total > TH_DISK_KEEP; i++) {
      fm_path_join(p, sizeof p, g.cache_dir, v.items[i].name);
      if (plat_remove_file(p) == FM_OK) total -= v.items[i].size;
    }
  }
  FM_VEC_FREE(v);
}

/* ---- decoding ---------------------------------------------------------------- */

static FmErr decode(const char *path, FmType type, u64 size, int px, FmImage *out) {
  memset(out, 0, sizeof *out);
  switch (type) {
    case FT_IMAGE:
    case FT_GIF:
    case FT_SVG:
      if ((i64)size > TH_MAX_FILE) return FM_ERR_UNSUPPORTED;
      return img_load(path, px, px, out, NULL, NULL);
    case FT_VIDEO:
      return vid_thumb(path, px, out);
    case FT_AUDIO: {
      FmAudMeta m;
      FmErr err = FM_ERR_FORMAT;
      if (aud_meta(path, &m, true) && m.cover) err = img_load_mem(m.cover, m.cover_len, px, out);
      aud_meta_free(&m);
      return err;
    }
    default:
      return FM_ERR_UNSUPPORTED;
  }
}

static int worker(void *u) {
  FM_UNUSED(u);
  SDL_LockMutex(g.mx);
  if (!g.trimmed && g.cache_ok) {
    g.trimmed = true;
    SDL_UnlockMutex(g.mx);
    cache_trim();
    SDL_LockMutex(g.mx);
  }
  for (;;) {
    while (!g.quit && g.nstack == 0) SDL_CondWait(g.cv, g.mx);
    if (g.quit) break;
    int i = g.stack[--g.nstack];
    ThEntry *e = &g.e[i];
    if (e->state != TS_QUEUED) continue;
    e->state = TS_WORKING;
    char path[FM_PATH_MAX];
    fm_strlcpy(path, e->path, sizeof path);
    u64 key = e->key, size = e->size;
    int px = e->px;
    FmType type = e->type;
    SDL_UnlockMutex(g.mx);

    FmImage img;
    memset(&img, 0, sizeof img);
    bool cached = cache_read(key, px, &img);
    FmErr err = cached ? FM_OK : decode(path, type, size, px, &img);
    if (err == FM_OK && !cached && (size >= TH_DISK_MIN || type == FT_VIDEO || type == FT_AUDIO))
      cache_write(key, &img);

    SDL_LockMutex(g.mx);
    if (e->state == TS_WORKING && e->key == key) {
      if (err == FM_OK && img.px) { e->img = img; e->state = TS_READY; }
      else { img_free(&img); e->state = TS_FAILED; }
    } else {
      img_free(&img);                 /* cancelled or reset meanwhile */
    }
    SDL_UnlockMutex(g.mx);
    app_wake();
    SDL_LockMutex(g.mx);
  }
  SDL_UnlockMutex(g.mx);
  return 0;
}

/* ---- public ------------------------------------------------------------------ */

void thumb_init(void) {
  if (g.init) return;
  memset(&g, 0, sizeof g);
  for (int i = 0; i < TH_BUCKETS; i++) g.bucket[i] = -1;
  for (int i = 0; i < TH_ENTRIES; i++) g.e[i].next = -1;
  g.mx = SDL_CreateMutex();
  g.cv = SDL_CreateCond();
  if (!g.mx || !g.cv) return;
  char base[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, base, sizeof base) && fm_path_join(g.cache_dir, sizeof g.cache_dir, base, "thumbs"))
    g.cache_ok = plat_mkdirs(g.cache_dir) == FM_OK || plat_is_dir(g.cache_dir);
  g.nthr = plat_cpu_count() > 2 ? 2 : 1;
  for (int i = 0; i < g.nthr; i++) g.thr[i] = fm_thread_create(worker, "thumb", NULL);
  g.init = true;
}

void thumb_shutdown(void) {
  if (!g.init) return;
  SDL_LockMutex(g.mx);
  g.quit = true;
  SDL_CondBroadcast(g.cv);
  SDL_UnlockMutex(g.mx);
  for (int i = 0; i < g.nthr; i++)
    if (g.thr[i]) SDL_WaitThread(g.thr[i], NULL);
  for (int i = 0; i < TH_ENTRIES; i++) free_entry(i);
  SDL_DestroyCond(g.cv);
  SDL_DestroyMutex(g.mx);
  memset(&g, 0, sizeof g);
}

bool thumb_supported(FmType t) {
  return t == FT_IMAGE || t == FT_GIF || t == FT_SVG || t == FT_VIDEO || t == FT_AUDIO;
}

/* Main thread, g.mx held. */
static void upload(ThEntry *e) {
  e->tex = view_tex_rgba(e->img.px, e->img.w, e->img.h);
  if (e->tex) {
    e->bytes = (size_t)e->img.w * e->img.h * 4;
    g.tex_bytes += e->bytes;
    e->state = TS_TEX;
  } else {
    e->state = TS_FAILED;
  }
  img_free(&e->img);
}

SDL_Texture *thumb_get(const char *path, i64 mtime, u64 size, int px) {
  if (!path || !*path) return NULL;
  if (!g.init) thumb_init();
  if (!g.init) return NULL;
  FmType type = fm_type_from_name(path);
  if (!thumb_supported(type)) return NULL;
  px = FM_CLAMP(px, 16, 512);
  u64 key = thumb_key(path, mtime, size, px);
  SDL_Texture *tex = NULL;
  SDL_LockMutex(g.mx);
  int i = find_entry(key, path);
  if (i >= 0) {
    ThEntry *e = &g.e[i];
    e->used = g.frame;
    if (e->state == TS_READY) { upload(e); ui_redraw(); }
    if (e->state == TS_TEX) tex = e->tex;
    else if (e->state == TS_QUEUED) {
      /* asked again: most recent first */
      stack_remove(i);
      g.stack[g.nstack++] = i;
    }
  } else {
    i = alloc_entry();
    if (i >= 0) {
      ThEntry *e = &g.e[i];
      e->key = key;
      e->path = fm_strdup(path);
      e->mtime = mtime;
      e->size = size;
      e->px = px;
      e->type = type;
      e->used = g.frame;
      e->state = TS_QUEUED;
      e->next = g.bucket[key % TH_BUCKETS];
      g.bucket[key % TH_BUCKETS] = i;
      if (g.nstack == TH_STACK) {
        int old = g.stack[0];
        memmove(g.stack, g.stack + 1, sizeof(int) * (TH_STACK - 1));
        g.nstack--;
        free_entry(old);
      }
      g.stack[g.nstack++] = i;
      SDL_CondSignal(g.cv);
    }
  }
  SDL_UnlockMutex(g.mx);
  return tex;
}

void thumb_pump(void) {
  if (!g.init) return;
  bool any = false;
  SDL_LockMutex(g.mx);
  g.frame++;
  for (int i = 0; i < TH_ENTRIES; i++)
    if (g.e[i].state == TS_READY) { upload(&g.e[i]); any = true; }
  /* evict least recently used textures over the budget */
  while (g.tex_bytes > TH_BUDGET) {
    int victim = -1;
    for (int i = 0; i < TH_ENTRIES; i++) {
      ThEntry *e = &g.e[i];
      if (e->state != TS_TEX || g.frame - e->used < 2) continue;
      if (victim < 0 || (g.frame - e->used) > (g.frame - g.e[victim].used)) victim = i;
    }
    if (victim < 0) break;
    free_entry(victim);
  }
  SDL_UnlockMutex(g.mx);
  if (any) ui_redraw();
}

void thumb_reset(void) {
  if (!g.init) return;
  SDL_LockMutex(g.mx);
  /* textures must be recreated: drop them, the panels ask again */
  for (int i = 0; i < TH_ENTRIES; i++)
    if (g.e[i].state == TS_TEX) free_entry(i);
  SDL_UnlockMutex(g.mx);
}

void thumb_cancel_all(void) {
  if (!g.init) return;
  SDL_LockMutex(g.mx);
  for (int k = 0; k < g.nstack; k++) free_entry(g.stack[k]);
  g.nstack = 0;
  /* running decodes finish but their result is dropped */
  for (int i = 0; i < TH_ENTRIES; i++)
    if (g.e[i].state == TS_WORKING) free_entry(i);
  SDL_UnlockMutex(g.mx);
}
