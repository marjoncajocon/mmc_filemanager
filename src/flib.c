/* flib.c -- media library index, background scanner, favorites, recents.
**
** Design decisions:
**   - One Index = a string pool addressed by u32 offsets + an FmLibItem
**     array. Cloning one is two memcpy calls, which is how the scanner hands
**     snapshots to the main thread and how the main thread lends the old
**     index to the scanner as a tag cache. Nothing is shared while in use.
**   - The walk is bounded: depth, folders, files per folder and files in
**     total are capped; hidden, system and well-known junk folders and
**     symlinked folders are skipped, and a folder with a ".nomedia" file is
**     dropped with everything below it (the Android convention).
**   - The scan is incremental: the walk publishes as soon as it is done
**     (names first), tags follow in a second pass that only reads files
**     whose size or mtime changed; tags persist in a cache file so the next
**     start shows the library before the walk has finished.
**   - Favorites and recents that point to deleted files are pruned by the
**     scanner, but only when the folder still exists: a drive that is not
**     plugged in keeps its favorites.
**   - library.txt is rewritten (tmp + rename) at most once per frame when
**     something changed; it is a few KB.
*/
#include "flib.h"
#include "fplat.h"
#include "fapp.h"
#include "fdec_aud.h"

#define LIB_MAX_ITEMS 20000
#define LIB_MAX_PER_DIR 3000
#define LIB_MAX_DIRS 8000
#define LIB_MAX_DEPTH 12
#define LIB_MAX_FOLDERS 32
#define LIB_RECENT_MAX 40
#define LIB_FAV_MAX 4000
#define LIB_POOL_MAX (64u * 1024u * 1024u)
#define LIB_PUBLISH_MS 2500
#define LIB_TS_MIN (256 * 1024)        /* smaller .ts files are TypeScript, not video */

/* ---- string pool and index ------------------------------------------------ */

typedef struct Pool { char *s; u32 len, cap; } Pool;

typedef struct Index {
  Pool sp;
  FmLibItem *items;
  int n, cap;
} Index;

static void pool_init(Pool *p) {
  p->cap = 1024;
  p->s = (char *)fm_alloc(p->cap);
  p->s[0] = 0;                 /* offset 0 is the empty string */
  p->len = 1;
}

static u32 pool_add(Pool *p, const char *s) {
  if (!s || !s[0]) return 0;
  size_t n = strlen(s) + 1;
  if ((size_t)p->len + n > LIB_POOL_MAX) return 0;
  if (p->len + n > p->cap) {
    u32 c = p->cap;
    while (p->len + n > c) c *= 2;
    p->s = (char *)fm_realloc(p->s, c);
    p->cap = c;
  }
  u32 off = p->len;
  memcpy(p->s + off, s, n);
  p->len += (u32)n;
  return off;
}

static void ix_init(Index *x) {
  memset(x, 0, sizeof *x);
  pool_init(&x->sp);
}

static void ix_free(Index *x) {
  fm_free(x->sp.s);
  fm_free(x->items);
  memset(x, 0, sizeof *x);
}

/* Exact-size copy: snapshots carry no slack. */
static void ix_clone(Index *d, const Index *s) {
  memset(d, 0, sizeof *d);
  d->sp.cap = d->sp.len = s->sp.len ? s->sp.len : 1;
  d->sp.s = (char *)fm_alloc(d->sp.cap);
  if (s->sp.len) memcpy(d->sp.s, s->sp.s, s->sp.len);
  else d->sp.s[0] = 0;
  d->n = d->cap = s->n;
  if (s->n) {
    d->items = (FmLibItem *)fm_alloc(sizeof(FmLibItem) * (size_t)s->n);
    memcpy(d->items, s->items, sizeof(FmLibItem) * (size_t)s->n);
  }
}

static FmLibItem *ix_add(Index *x) {
  if (x->n == x->cap) {
    x->cap = x->cap ? x->cap * 2 : 256;
    x->items = (FmLibItem *)fm_realloc(x->items, sizeof(FmLibItem) * (size_t)x->cap);
  }
  FmLibItem *it = &x->items[x->n++];
  memset(it, 0, sizeof *it);
  return it;
}

/* ---- paths ------------------------------------------------------------------ */

static int path_cmp(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b);
#else
  return strcmp(a, b);
#endif
}

static u32 path_hash(const char *s) {
  u32 h = 2166136261u;
  for (; *s; s++) {
    u8 c = (u8)*s;
#ifdef FM_WIN
    if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
#endif
    h = (h ^ c) * 16777619u;
  }
  return h;
}

/* Gone for good: the file is missing but its folder is there. */
static bool path_gone(const char *path) {
  if (plat_exists(path)) return false;
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  return fm_path_parent(dir) && plat_is_dir(dir);
}

static int media_kind(const char *name, u64 size) {
  FmType t = fm_type_from_name(name);
  if (t == FT_AUDIO) return LIB_AUDIO;
  if (t == FT_VIDEO) return fm_ends_with_i(name, ".ts") && size < LIB_TS_MIN ? -1 : LIB_VIDEO;
  return -1;
}

bool lib_is_media(const char *path) {
  FmType t = fm_type_from_name(path ? path : "");
  return t == FT_AUDIO || t == FT_VIDEO;
}

/* ---- hash over an index --------------------------------------------------------- */

typedef struct Hash { int *slot; u32 mask; } Hash;

static void hash_build(Hash *h, const Index *x) {
  u32 size = 64;
  while (size < (u32)x->n * 2) size *= 2;
  h->slot = (int *)fm_alloc(sizeof(int) * size);
  memset(h->slot, 0xFF, sizeof(int) * size);
  h->mask = size - 1;
  for (int i = 0; i < x->n; i++) {
    u32 k = path_hash(x->sp.s + x->items[i].path) & h->mask;
    while (h->slot[k] >= 0) k = (k + 1) & h->mask;
    h->slot[k] = i;
  }
}

static int hash_find(const Hash *h, const Index *x, const char *path) {
  if (!h->slot) return -1;
  for (u32 k = path_hash(path) & h->mask; h->slot[k] >= 0; k = (k + 1) & h->mask)
    if (path_cmp(x->sp.s + x->items[h->slot[k]].path, path) == 0) return h->slot[k];
  return -1;
}

static void hash_free(Hash *h) {
  fm_free(h->slot);
  h->slot = NULL;
  h->mask = 0;
}

/* ---- state -------------------------------------------------------------------- */

typedef struct ScanJob {
  char **dirs;
  int ndirs;
  char **check;               /* favorites and recents to verify */
  int ncheck;
  Index prev;                 /* tag cache (owned) */
  bool load_cache, save_cache, wake;
  char cache_file[FM_PATH_MAX];
} ScanJob;

static struct {
  bool init, readonly, defaults, no_wake;
  char conf_file[FM_PATH_MAX], cache_file[FM_PATH_MAX];
  /* folders */
  char *added[LIB_MAX_FOLDERS];
  int nadded;
  char *hidden[LIB_MAX_FOLDERS];
  int nhidden;
  char *src[LIB_MAX_FOLDERS + 2];
  bool src_def[LIB_MAX_FOLDERS + 2];
  int nsrc;
  bool src_dirty;
  /* favorites (insertion order) and recents (newest first) */
  char **fav;
  int nfav, capfav;
  int *fav_sorted;
  char *recent[LIB_RECENT_MAX];
  int nrecent;
  bool dirty;
  /* index and views */
  Index ix;
  Hash hash;
  int *views;
  FmLibGroup *groups;
  FmLibData data;
  bool scanned_once, cache_tried;
  /* scanner */
  SDL_mutex *mx;
  SDL_Thread *thr;
  SDL_atomic_t cancel, running, found, has_pub;
  Index *pub;
  char **pub_missing;
  int npub_missing;
} g;

/* ---- favorites ------------------------------------------------------------------- */

static int cmp_fav(const void *a, const void *b) {
  return path_cmp(g.fav[*(const int *)a], g.fav[*(const int *)b]);
}

static void fav_resort(void) {
  fm_free(g.fav_sorted);
  g.fav_sorted = NULL;
  if (g.nfav == 0) return;
  g.fav_sorted = (int *)fm_alloc(sizeof(int) * (size_t)g.nfav);
  for (int i = 0; i < g.nfav; i++) g.fav_sorted[i] = i;
  qsort(g.fav_sorted, (size_t)g.nfav, sizeof(int), cmp_fav);
}

/* Index into g.fav, or -1. */
static int fav_find(const char *path) {
  int lo = 0, hi = g.nfav - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = path_cmp(g.fav[g.fav_sorted[mid]], path);
    if (c == 0) return g.fav_sorted[mid];
    if (c < 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

static void fav_push(const char *path) {
  if (g.nfav == g.capfav) {
    g.capfav = g.capfav ? g.capfav * 2 : 32;
    g.fav = (char **)fm_realloc(g.fav, sizeof(char *) * (size_t)g.capfav);
  }
  g.fav[g.nfav++] = fm_strdup(path);
}

static void fav_remove_at(int i) {
  fm_free(g.fav[i]);
  memmove(g.fav + i, g.fav + i + 1, sizeof(char *) * (size_t)(g.nfav - i - 1));
  g.nfav--;
}

bool lib_is_fav(const char *path) {
  return path && path[0] && g.nfav > 0 && fav_find(path) >= 0;
}

void lib_fav_set(const char *path, bool on) {
  if (!path || !path[0] || strchr(path, '\n')) return;
  int i = fav_find(path);
  if (on == (i >= 0)) return;
  if (on) {
    if (g.nfav >= LIB_FAV_MAX) fav_remove_at(0);      /* the oldest goes */
    fav_push(path);
  } else {
    fav_remove_at(i);
  }
  fav_resort();
  g.dirty = true;
  g.data.gen++;
}

void lib_fav_toggle(const char *path) { lib_fav_set(path, !lib_is_fav(path)); }

int lib_fav_count(void) { return g.nfav; }
const char *lib_fav_at(int i) { return i >= 0 && i < g.nfav ? g.fav[g.nfav - 1 - i] : ""; }

/* ---- recents ---------------------------------------------------------------------- */

static void recent_remove_at(int i) {
  fm_free(g.recent[i]);
  memmove(g.recent + i, g.recent + i + 1, sizeof(char *) * (size_t)(g.nrecent - i - 1));
  g.nrecent--;
}

void lib_note_played(const char *path) {
  if (!path || !path[0] || strchr(path, '\n')) return;
  /* files opened from archives are temporary copies: not worth remembering */
  char cache[FM_PATH_MAX];
  if (plat_place(PLACE_CACHE, cache, sizeof cache) && fm_path_is_inside(path, cache)) return;
  if (g.nrecent > 0 && path_cmp(g.recent[0], path) == 0) return;
  for (int i = 0; i < g.nrecent; i++)
    if (path_cmp(g.recent[i], path) == 0) { recent_remove_at(i); break; }
  if (g.nrecent == LIB_RECENT_MAX) recent_remove_at(g.nrecent - 1);
  memmove(g.recent + 1, g.recent, sizeof(char *) * (size_t)g.nrecent);
  g.recent[0] = fm_strdup(path);
  g.nrecent++;
  g.dirty = true;
  g.data.gen++;
}

int lib_recent_count(void) { return g.nrecent; }
const char *lib_recent_at(int i) { return i >= 0 && i < g.nrecent ? g.recent[i] : ""; }

void lib_recent_clear(void) {
  while (g.nrecent > 0) recent_remove_at(g.nrecent - 1);
  g.dirty = true;
  g.data.gen++;
}

/* Removes favorites and recents listed in `paths`. */
static int drop_paths(char **paths, int n) {
  int removed = 0;
  for (int k = 0; k < n; k++) {
    int i = fav_find(paths[k]);
    if (i >= 0) { fav_remove_at(i); fav_resort(); removed++; }
    for (int r = 0; r < g.nrecent; r++)
      if (path_cmp(g.recent[r], paths[k]) == 0) { recent_remove_at(r); removed++; break; }
  }
  if (removed) { g.dirty = true; g.data.gen++; }
  return removed;
}

int lib_prune_missing(void) {
  char **gone = (char **)fm_alloc(sizeof(char *) * (size_t)(g.nfav + g.nrecent + 1));
  int n = 0;
  for (int i = 0; i < g.nfav; i++)
    if (path_gone(g.fav[i])) gone[n++] = fm_strdup(g.fav[i]);
  for (int i = 0; i < g.nrecent; i++)
    if (path_gone(g.recent[i])) gone[n++] = fm_strdup(g.recent[i]);
  int removed = drop_paths(gone, n);
  for (int i = 0; i < n; i++) fm_free(gone[i]);
  fm_free(gone);
  return removed;
}

/* ---- folders ---------------------------------------------------------------------- */

static int find_str(char **list, int n, const char *s) {
  for (int i = 0; i < n; i++)
    if (path_cmp(list[i], s) == 0) return i;
  return -1;
}

static void remove_str(char **list, int *n, int i) {
  fm_free(list[i]);
  memmove(list + i, list + i + 1, sizeof(char *) * (size_t)(*n - i - 1));
  (*n)--;
}

static int default_dirs(char out[2][FM_PATH_MAX]) {
  int n = 0;
  if (!g.defaults) return 0;
  if (plat_place(PLACE_MUSIC, out[n], FM_PATH_MAX)) n++;
  if (plat_place(PLACE_VIDEOS, out[n], FM_PATH_MAX) && (n == 0 || path_cmp(out[0], out[n]) != 0)) n++;
  return n;
}

static void refresh_sources(void) {
  if (!g.src_dirty) return;
  for (int i = 0; i < g.nsrc; i++) fm_free(g.src[i]);
  g.nsrc = 0;
  char def[2][FM_PATH_MAX];
  int nd = default_dirs(def);
  for (int i = 0; i < nd; i++) {
    if (find_str(g.hidden, g.nhidden, def[i]) >= 0 || !plat_is_dir(def[i])) continue;
    g.src_def[g.nsrc] = true;
    g.src[g.nsrc++] = fm_strdup(def[i]);
  }
  for (int i = 0; i < g.nadded && g.nsrc < FM_COUNT(g.src); i++) {
    if (find_str(g.src, g.nsrc, g.added[i]) >= 0) continue;
    g.src_def[g.nsrc] = false;
    g.src[g.nsrc++] = fm_strdup(g.added[i]);
  }
  g.src_dirty = false;
}

int lib_folder_count(void) {
  refresh_sources();
  return g.nsrc;
}

const char *lib_folder_at(int i, bool *is_default) {
  refresh_sources();
  if (i < 0 || i >= g.nsrc) return "";
  if (is_default) *is_default = g.src_def[i];
  return g.src[i];
}

int lib_hidden_count(void) { return g.nhidden; }
const char *lib_hidden_at(int i) { return i >= 0 && i < g.nhidden ? g.hidden[i] : ""; }

bool lib_has_folder(const char *dir) {
  refresh_sources();
  return dir && find_str(g.src, g.nsrc, dir) >= 0;
}

void lib_folder_set(const char *dir, bool on) {
  if (!dir || !dir[0] || strchr(dir, '\n')) return;
  char def[2][FM_PATH_MAX];
  int nd = default_dirs(def);
  bool is_def = false;
  for (int i = 0; i < nd; i++)
    if (path_cmp(def[i], dir) == 0) is_def = true;
  int a = find_str(g.added, g.nadded, dir), h = find_str(g.hidden, g.nhidden, dir);
  if (on) {
    if (h >= 0) remove_str(g.hidden, &g.nhidden, h);
    if (!is_def && a < 0 && g.nadded < LIB_MAX_FOLDERS) g.added[g.nadded++] = fm_strdup(dir);
  } else {
    if (a >= 0) remove_str(g.added, &g.nadded, a);
    if (is_def && h < 0 && g.nhidden < LIB_MAX_FOLDERS) g.hidden[g.nhidden++] = fm_strdup(dir);
  }
  g.src_dirty = true;
  g.dirty = true;
  if (g.scanned_once || lib_scanning()) lib_rescan();
}

/* ---- library.txt -------------------------------------------------------------------- */

static void strip_eol(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

/* fgets that drops lines too long for the buffer; false at the end. */
static bool read_line(FILE *f, char *buf, int cap, bool *ok) {
  if (!fgets(buf, cap, f)) return false;
  size_t n = strlen(buf);
  *ok = true;
  if (n > 0 && buf[n - 1] != '\n' && !feof(f)) {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
    *ok = false;
  }
  strip_eol(buf);
  return true;
}

static void load_conf(void) {
  FILE *f = g.conf_file[0] ? fm_fopen(g.conf_file, "rb") : NULL;
  if (!f) return;
  char line[FM_PATH_MAX + 32];
  bool ok;
  while (read_line(f, line, sizeof line, &ok)) {
    if (!ok || !line[0] || line[0] == '#') continue;
    char *sp = strchr(line, ' ');
    if (!sp || !sp[1]) continue;
    *sp = 0;
    const char *v = sp + 1;
    if (!strcmp(line, "folder")) {
      if (g.nadded < LIB_MAX_FOLDERS && find_str(g.added, g.nadded, v) < 0) g.added[g.nadded++] = fm_strdup(v);
    } else if (!strcmp(line, "hide")) {
      if (g.nhidden < LIB_MAX_FOLDERS && find_str(g.hidden, g.nhidden, v) < 0) g.hidden[g.nhidden++] = fm_strdup(v);
    } else if (!strcmp(line, "fav")) {
      if (g.nfav < LIB_FAV_MAX) fav_push(v);
    } else if (!strcmp(line, "recent")) {
      if (g.nrecent < LIB_RECENT_MAX && find_str(g.recent, g.nrecent, v) < 0) g.recent[g.nrecent++] = fm_strdup(v);
    }
  }
  fclose(f);
  /* drop duplicate favorites, keeping the first */
  fav_resort();
  for (int i = g.nfav - 1; i > 0; i--)
    for (int k = 0; k < i; k++)
      if (path_cmp(g.fav[k], g.fav[i]) == 0) { fav_remove_at(i); break; }
  fav_resort();
  g.src_dirty = true;
}

/* Writes tmp, then swaps it in, so a crash never leaves half a file. */
static bool save_file(const char *path, bool (*write)(FILE *f, void *ud), void *ud) {
  char dir[FM_PATH_MAX], tmp[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  if (fm_path_parent(dir)) plat_mkdirs(dir);
  fm_snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return false;
  bool ok = write(f, ud);
  ok = !ferror(f) && ok;
  if (fclose(f) != 0) ok = false;
  if (!ok) { plat_remove_file(tmp); return false; }
  plat_remove_file(path);
  return plat_rename(tmp, path) == FM_OK;
}

static bool write_conf(FILE *f, void *ud) {
  FM_UNUSED(ud);
  fputs("# mmcfm media library: folder, hide (a default folder), fav, recent\n", f);
  for (int i = 0; i < g.nadded; i++) fprintf(f, "folder %s\n", g.added[i]);
  for (int i = 0; i < g.nhidden; i++) fprintf(f, "hide %s\n", g.hidden[i]);
  for (int i = 0; i < g.nfav; i++) fprintf(f, "fav %s\n", g.fav[i]);
  for (int i = 0; i < g.nrecent; i++) fprintf(f, "recent %s\n", g.recent[i]);
  return true;
}

static void save_conf(void) {
  g.dirty = false;
  if (g.readonly || !g.conf_file[0]) return;
  if (!save_file(g.conf_file, write_conf, NULL)) fm_log("library: can't save %s", g.conf_file);
}

/* ---- tag cache file ------------------------------------------------------------------ */

/* One file per line: kind tagged mtime size path title artist album, tab separated. */
static void put_field(FILE *f, const char *s) {
  for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

static bool write_cache(FILE *f, void *ud) {
  const Index *x = (const Index *)ud;
  fputs("mmcfm-lib 1\n", f);
  for (int i = 0; i < x->n; i++) {
    const FmLibItem *it = &x->items[i];
    const char *p = x->sp.s + it->path;
    if (strpbrk(p, "\t\n\r")) continue;
    fprintf(f, "%c\t%d\t%lld\t%llu\t%s\t", it->kind == LIB_VIDEO ? 'V' : 'A', it->tagged ? 1 : 0,
            (long long)it->mtime, (unsigned long long)it->size, p);
    put_field(f, x->sp.s + it->title);
    fputc('\t', f);
    put_field(f, x->sp.s + it->artist);
    fputc('\t', f);
    put_field(f, x->sp.s + it->album);
    fputc('\n', f);
  }
  return true;
}

/* 32-bit msvcrt (tcc) has no strtoll; mtimes before 1970 are not worth a sign. */
static u64 parse_u64(const char *s) {
  u64 v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
  return v;
}

static void load_cache(Index *x, const char *path) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  char line[FM_PATH_MAX + 3 * AUD_META_TEXT + 64];
  bool ok;
  if (!read_line(f, line, sizeof line, &ok) || strcmp(line, "mmcfm-lib 1") != 0) { fclose(f); return; }
  while (x->n < LIB_MAX_ITEMS && read_line(f, line, sizeof line, &ok)) {
    if (!ok) continue;
    char *fld[8];
    int nf = 0;
    char *s = line;
    while (nf < 8) {
      fld[nf++] = s;
      char *t = strchr(s, '\t');
      if (!t) break;
      *t = 0;
      s = t + 1;
    }
    if (nf != 8 || !fld[4][0]) continue;
    FmLibItem *it = ix_add(x);
    it->kind = fld[0][0] == 'V' ? LIB_VIDEO : LIB_AUDIO;
    it->tagged = fld[1][0] == '1';
    it->mtime = (i64)parse_u64(fld[2]);
    it->size = parse_u64(fld[3]);
    it->path = pool_add(&x->sp, fld[4]);
    it->title = pool_add(&x->sp, fld[5]);
    it->artist = pool_add(&x->sp, fld[6]);
    it->album = pool_add(&x->sp, fld[7]);
    if (!it->path) x->n--;
  }
  fclose(f);
}

/* ---- scanner thread ------------------------------------------------------------------ */

static bool cancelled(void) { return SDL_AtomicGet(&g.cancel) != 0; }

static void publish(Index *x, bool final, char **missing, int nmissing, bool wake) {
  Index *copy = (Index *)fm_alloc(sizeof *copy);
  if (final) { *copy = *x; memset(x, 0, sizeof *x); }
  else ix_clone(copy, x);
  SDL_LockMutex(g.mx);
  if (g.pub) { ix_free(g.pub); fm_free(g.pub); }
  g.pub = copy;
  if (final) { g.pub_missing = missing; g.npub_missing = nmissing; }
  SDL_AtomicSet(&g.has_pub, 1);
  SDL_UnlockMutex(g.mx);
  if (wake) app_wake();
}

static bool skip_dir(const char *name) {
  static const char *const kSkip[] = {
    "node_modules", "$RECYCLE.BIN", "System Volume Information", "Windows", "Program Files",
    "Program Files (x86)", "ProgramData", "AppData", "__pycache__", "proc", "sys", "dev",
    "Library", "lost+found", "Android"
  };
  for (int i = 0; i < FM_COUNT(kSkip); i++)
    if (fm_stricmp(name, kSkip[i]) == 0) return true;
  return false;
}

typedef struct WalkDir { u32 off; int depth; } WalkDir;
typedef struct WalkStack { WalkDir *items; int count, cap; } WalkStack;

static void scan_tree(Index *cur, const Index *prev, const Hash *ph, const char *root, u64 *last_pub,
                      bool wake) {
  Pool dp;
  pool_init(&dp);
  WalkStack st;
  memset(&st, 0, sizeof st);
  WalkDir w0 = { pool_add(&dp, root), 0 };
  FM_VEC_PUSH(st, w0);
  int ndirs = 0;
  char dir[FM_PATH_MAX], path[FM_PATH_MAX];
  while (st.count > 0 && !cancelled() && cur->n < LIB_MAX_ITEMS && ndirs < LIB_MAX_DIRS) {
    WalkDir w = st.items[--st.count];
    fm_strlcpy(dir, dp.s + w.off, sizeof dir);
    dp.len = w.off ? w.off : 1;         /* LIFO: the popped name is the last one in the pool */
    ndirs++;
    FmDir *d = plat_dir_open(dir, NULL);
    if (!d) continue;
    int item_mark = cur->n, stack_mark = st.count, nfiles = 0;
    u32 pool_mark = cur->sp.len, dir_mark = dp.len;
    bool nomedia = false;
    const char *name;
    FmStat s;
    while (plat_dir_next(d, &name, &s)) {
      if (cancelled()) break;
      if (name[0] == '.') {
        if (!strcmp(name, ".nomedia")) nomedia = true;
        continue;
      }
      if (s.flags & (FM_ST_HIDDEN | FM_ST_SYSTEM | FM_ST_BROKEN)) continue;
      if (s.flags & FM_ST_DIR) {
        if ((s.flags & FM_ST_LINK) || w.depth + 1 > LIB_MAX_DEPTH || skip_dir(name)) continue;
        if (!fm_path_join(path, sizeof path, dir, name)) continue;
        WalkDir sub = { pool_add(&dp, path), w.depth + 1 };
        if (sub.off) FM_VEC_PUSH(st, sub);
        continue;
      }
      int kind = media_kind(name, s.size);
      if (kind < 0 || nfiles >= LIB_MAX_PER_DIR || cur->n >= LIB_MAX_ITEMS) continue;
      if (!fm_path_join(path, sizeof path, dir, name)) continue;
      u32 po = pool_add(&cur->sp, path);
      if (!po) continue;
      FmLibItem *it = ix_add(cur);
      it->path = po;
      it->kind = (u8)kind;
      it->mtime = s.mtime;
      it->size = s.size;
      it->tagged = kind == LIB_VIDEO;
      int k = hash_find(ph, prev, path);
      if (k >= 0 && kind == LIB_AUDIO && prev->items[k].tagged && prev->items[k].mtime == s.mtime &&
          prev->items[k].size == s.size) {
        const FmLibItem *o = &prev->items[k];
        it->title = pool_add(&cur->sp, prev->sp.s + o->title);
        it->artist = pool_add(&cur->sp, prev->sp.s + o->artist);
        it->album = pool_add(&cur->sp, prev->sp.s + o->album);
        it->tagged = 1;
      }
      nfiles++;
      SDL_AtomicSet(&g.found, cur->n);
    }
    plat_dir_close(d);
    if (nomedia) {             /* the folder asked not to be indexed */
      cur->n = item_mark;
      cur->sp.len = pool_mark;
      st.count = stack_mark;
      dp.len = dir_mark;
      SDL_AtomicSet(&g.found, cur->n);
    }
    if (plat_now_ms() - *last_pub >= LIB_PUBLISH_MS) {
      publish(cur, false, NULL, 0, wake);
      *last_pub = plat_now_ms();
    }
  }
  FM_VEC_FREE(st);
  fm_free(dp.s);
}

static void trim(char *s) {
  size_t n = strlen(s), a = 0;
  while (n > 0 && (u8)s[n - 1] <= ' ') s[--n] = 0;
  while (s[a] && (u8)s[a] <= ' ') a++;
  if (a) memmove(s, s + a, n - a + 1);
}

static void free_strs(char **v, int n) {
  for (int i = 0; i < n; i++) fm_free(v[i]);
  fm_free(v);
}

static int SDLCALL scan_thread(void *ud) {
  ScanJob *j = (ScanJob *)ud;
  bool wake = j->wake;
  if (j->load_cache) {
    load_cache(&j->prev, j->cache_file);
    if (j->prev.n > 0) publish(&j->prev, false, NULL, 0, wake);   /* shown before the walk ends */
  }
  Index cur;
  ix_init(&cur);
  Hash ph;
  memset(&ph, 0, sizeof ph);
  if (j->prev.n > 0) hash_build(&ph, &j->prev);
  u64 last_pub = plat_now_ms();
  for (int i = 0; i < j->ndirs && !cancelled(); i++) {
    /* a folder inside another source is already covered */
    bool inner = false;
    for (int k = 0; k < j->ndirs; k++)
      if (k != i && fm_path_is_inside(j->dirs[i], j->dirs[k]) &&
          (path_cmp(j->dirs[i], j->dirs[k]) != 0 || k < i))
        inner = true;
    if (!inner) scan_tree(&cur, &j->prev, &ph, j->dirs[i], &last_pub, wake);
  }
  hash_free(&ph);
  ix_free(&j->prev);
  if (!cancelled()) {
    publish(&cur, false, NULL, 0, wake);
    last_pub = plat_now_ms();
  }
  /* second pass: tags of new or changed songs */
  FmAudMeta m;
  for (int i = 0; i < cur.n && !cancelled(); i++) {
    if (cur.items[i].tagged) continue;
    char path[FM_PATH_MAX];
    fm_strlcpy(path, cur.sp.s + cur.items[i].path, sizeof path);
    memset(&m, 0, sizeof m);
    if (aud_meta(path, &m, false)) {
      trim(m.title);
      trim(m.artist);
      trim(m.album);
      cur.items[i].title = pool_add(&cur.sp, m.title);
      cur.items[i].artist = pool_add(&cur.sp, m.artist);
      cur.items[i].album = pool_add(&cur.sp, m.album);
    }
    aud_meta_free(&m);
    cur.items[i].tagged = 1;
    if (plat_now_ms() - last_pub >= LIB_PUBLISH_MS) {
      publish(&cur, false, NULL, 0, wake);
      last_pub = plat_now_ms();
    }
  }
  char **missing = NULL;
  int nmissing = 0;
  if (!cancelled()) {
    missing = (char **)fm_alloc(sizeof(char *) * (size_t)(j->ncheck + 1));
    for (int i = 0; i < j->ncheck && !cancelled(); i++)
      if (path_gone(j->check[i])) missing[nmissing++] = fm_strdup(j->check[i]);
    if (j->save_cache && !cancelled() && !save_file(j->cache_file, write_cache, &cur))
      fm_log("library: can't save %s", j->cache_file);
    publish(&cur, true, missing, nmissing, wake);
  } else {
    ix_free(&cur);
  }
  free_strs(j->dirs, j->ndirs);
  free_strs(j->check, j->ncheck);
  fm_free(j);
  SDL_AtomicSet(&g.running, 0);
  if (wake) app_wake();
  return 0;
}

/* ---- views ---------------------------------------------------------------------- */

static const char *sort_title(const FmLibItem *it) {
  return it->title ? g.ix.sp.s + it->title : fm_path_base(g.ix.sp.s + it->path);
}

static int cmp_title(const void *a, const void *b) {
  const FmLibItem *x = &g.ix.items[*(const int *)a], *y = &g.ix.items[*(const int *)b];
  int c = fm_natcmp(sort_title(x), sort_title(y));
  return c ? c : (*(const int *)a - *(const int *)b);
}

static int cmp_name(const void *a, const void *b) {
  const FmLibItem *x = &g.ix.items[*(const int *)a], *y = &g.ix.items[*(const int *)b];
  int c = fm_natcmp(fm_path_base(g.ix.sp.s + x->path), fm_path_base(g.ix.sp.s + y->path));
  return c ? c : (*(const int *)a - *(const int *)b);
}

/* Empty names sort last ("Unknown artist" at the end). */
static int cmp_key(u32 a, u32 b) {
  if (!a || !b) return (a ? 0 : 1) - (b ? 0 : 1);
  return fm_stricmp(g.ix.sp.s + a, g.ix.sp.s + b);
}

static int cmp_artist(const void *a, const void *b) {
  const FmLibItem *x = &g.ix.items[*(const int *)a], *y = &g.ix.items[*(const int *)b];
  int c = cmp_key(x->artist, y->artist);
  if (!c) c = cmp_key(x->album, y->album);
  return c ? c : cmp_title(a, b);
}

static int cmp_album(const void *a, const void *b) {
  const FmLibItem *x = &g.ix.items[*(const int *)a], *y = &g.ix.items[*(const int *)b];
  int c = cmp_key(x->album, y->album);
  return c ? c : cmp_title(a, b);
}

static int make_groups(const int *order, int n, bool artist, FmLibGroup *out) {
  int ng = 0;
  for (int i = 0; i < n; i++) {
    const FmLibItem *it = &g.ix.items[order[i]];
    u32 key = artist ? it->artist : it->album;
    if (ng > 0 && cmp_key(out[ng - 1].name, key) == 0) { out[ng - 1].count++; continue; }
    out[ng].name = key;
    out[ng].first = i;
    out[ng].count = 1;
    ng++;
  }
  return ng;
}

static void build_views(void) {
  fm_free(g.views);
  fm_free(g.groups);
  hash_free(&g.hash);
  u32 gen = g.data.gen + 1;
  memset(&g.data, 0, sizeof g.data);
  int n = g.ix.n;
  g.views = (int *)fm_alloc(sizeof(int) * (size_t)(n * 4 + 1));
  int *songs = g.views, *videos = g.views + n, *by_artist = g.views + 2 * n, *by_album = g.views + 3 * n;
  int ns = 0, nv = 0;
  for (int i = 0; i < n; i++) {
    if (g.ix.items[i].kind == LIB_VIDEO) videos[nv++] = i;
    else songs[ns++] = i;
  }
  qsort(songs, (size_t)ns, sizeof(int), cmp_title);
  qsort(videos, (size_t)nv, sizeof(int), cmp_name);
  memcpy(by_artist, songs, sizeof(int) * (size_t)ns);
  memcpy(by_album, songs, sizeof(int) * (size_t)ns);
  qsort(by_artist, (size_t)ns, sizeof(int), cmp_artist);
  qsort(by_album, (size_t)ns, sizeof(int), cmp_album);
  g.groups = (FmLibGroup *)fm_alloc(sizeof(FmLibGroup) * (size_t)(ns * 2 + 1));
  int na = make_groups(by_artist, ns, true, g.groups);
  int nb = make_groups(by_album, ns, false, g.groups + na);
  hash_build(&g.hash, &g.ix);
  g.data.pool = g.ix.sp.s;
  g.data.items = g.ix.items;
  g.data.n = n;
  g.data.songs = songs;
  g.data.nsongs = ns;
  g.data.videos = videos;
  g.data.nvideos = nv;
  g.data.by_artist = by_artist;
  g.data.artists = g.groups;
  g.data.nartists = na;
  g.data.by_album = by_album;
  g.data.albums = g.groups + na;
  g.data.nalbums = nb;
  g.data.gen = gen;
}

const FmLibData *lib_data(void) {
  if (!g.data.pool) build_views();
  return &g.data;
}

const char *lib_str(u32 off) { return g.ix.sp.s && off < g.ix.sp.len ? g.ix.sp.s + off : ""; }

int lib_find(const char *path) { return path ? hash_find(&g.hash, &g.ix, path) : -1; }

void lib_item_title(const FmLibItem *it, char *out, size_t cap) {
  if (it->title) { fm_strlcpy(out, lib_str(it->title), cap); return; }
  const char *b = fm_path_base(lib_str(it->path));
  fm_strlcpy(out, b, cap);
  const char *e = fm_path_ext(b);
  if (e[0] && e != b) out[FM_MIN((size_t)(e - b), cap - 1)] = 0;
}

/* ---- scan control ------------------------------------------------------------------ */

/* Main thread: takes a published snapshot. */
static void take_pub(void) {
  if (!SDL_AtomicGet(&g.has_pub)) return;
  SDL_LockMutex(g.mx);
  Index *x = g.pub;
  char **missing = g.pub_missing;
  int nmissing = g.npub_missing;
  g.pub = NULL;
  g.pub_missing = NULL;
  g.npub_missing = 0;
  SDL_AtomicSet(&g.has_pub, 0);
  SDL_UnlockMutex(g.mx);
  if (!x) return;
  ix_free(&g.ix);
  g.ix = *x;
  fm_free(x);
  build_views();
  if (missing) {
    g.scanned_once = true;
    drop_paths(missing, nmissing);
    free_strs(missing, nmissing);
  }
  ui_redraw();
}

static void stop_scan(void) {
  if (!g.thr) return;
  SDL_AtomicSet(&g.cancel, 1);
  SDL_WaitThread(g.thr, NULL);
  g.thr = NULL;
  SDL_AtomicSet(&g.cancel, 0);
}

bool lib_scanning(void) { return SDL_AtomicGet(&g.running) != 0; }
int lib_scan_found(void) { return SDL_AtomicGet(&g.found); }

void lib_scan_wait(void) {
  if (g.thr) {
    SDL_WaitThread(g.thr, NULL);
    g.thr = NULL;
  }
  take_pub();
}

void lib_rescan(void) {
  if (!g.init) return;
  stop_scan();
  take_pub();
  refresh_sources();
  ScanJob *j = (ScanJob *)fm_calloc(1, sizeof *j);
  j->dirs = (char **)fm_alloc(sizeof(char *) * (size_t)(g.nsrc + 1));
  for (int i = 0; i < g.nsrc; i++) j->dirs[j->ndirs++] = fm_strdup(g.src[i]);
  j->check = (char **)fm_alloc(sizeof(char *) * (size_t)(g.nfav + g.nrecent + 1));
  for (int i = 0; i < g.nfav; i++) j->check[j->ncheck++] = fm_strdup(g.fav[i]);
  for (int i = 0; i < g.nrecent; i++) j->check[j->ncheck++] = fm_strdup(g.recent[i]);
  if (g.ix.n > 0) {
    ix_clone(&j->prev, &g.ix);
  } else {
    ix_init(&j->prev);
    j->load_cache = !g.cache_tried && !g.readonly && g.cache_file[0];
    g.cache_tried = true;
  }
  j->save_cache = !g.readonly && g.cache_file[0];
  j->wake = !g.no_wake;
  fm_strlcpy(j->cache_file, g.cache_file, sizeof j->cache_file);
  SDL_AtomicSet(&g.found, 0);
  SDL_AtomicSet(&g.running, 1);
  g.thr = fm_thread_create(scan_thread, "mmcfm-lib", j);
  if (!g.thr) scan_thread(j);         /* no threads: scan now, the UI waits once */
}

/* ---- lifetime --------------------------------------------------------------------- */

static void free_all(void) {
  stop_scan();
  take_pub();
  for (int i = 0; i < g.nadded; i++) fm_free(g.added[i]);
  for (int i = 0; i < g.nhidden; i++) fm_free(g.hidden[i]);
  for (int i = 0; i < g.nsrc; i++) fm_free(g.src[i]);
  for (int i = 0; i < g.nfav; i++) fm_free(g.fav[i]);
  for (int i = 0; i < g.nrecent; i++) fm_free(g.recent[i]);
  fm_free(g.fav);
  fm_free(g.fav_sorted);
  ix_free(&g.ix);
  hash_free(&g.hash);
  fm_free(g.views);
  fm_free(g.groups);
  SDL_mutex *mx = g.mx;
  memset(&g, 0, sizeof g);
  g.mx = mx;
}

static void setup(const char *dir, bool readonly) {
  free_all();
  if (!g.mx) g.mx = SDL_CreateMutex();
  g.init = true;
  g.readonly = readonly;
  g.defaults = dir == NULL;
  g.no_wake = dir != NULL;
  g.src_dirty = true;
  ix_init(&g.ix);
  char base[FM_PATH_MAX];
  if (dir) {
    fm_path_join(g.conf_file, sizeof g.conf_file, dir, "library.txt");
    fm_path_join(g.cache_file, sizeof g.cache_file, dir, "library-index.txt");
  } else {
    if (plat_place(PLACE_CONFIG, base, sizeof base))
      fm_path_join(g.conf_file, sizeof g.conf_file, base, "library.txt");
    if (plat_place(PLACE_CACHE, base, sizeof base))
      fm_path_join(g.cache_file, sizeof g.cache_file, base, "library-index.txt");
  }
  if (!readonly || dir) load_conf();
}

void lib_init(bool readonly) { setup(NULL, readonly); }

void lib_test_reset(const char *dir, bool readonly) { setup(dir, readonly); }

void lib_shutdown(void) {
  if (!g.init) return;
  stop_scan();
  if (g.dirty) save_conf();
  free_all();
  if (g.mx) SDL_DestroyMutex(g.mx);
  g.mx = NULL;
}

void lib_pump(void) {
  if (!g.init) return;
  take_pub();
  if (g.thr && !lib_scanning()) {
    SDL_WaitThread(g.thr, NULL);
    g.thr = NULL;
    take_pub();
  }
  if (g.dirty) save_conf();
}
