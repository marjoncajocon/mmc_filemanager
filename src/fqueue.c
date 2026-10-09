/* fqueue.c -- the play queue's model and files, saved playlists, .m3u8.
**
** Design decisions:
**   - Pure where it can be: the index math works on int arrays and the
**     line format on strings, so the self test covers both without a
**     window or a sound device. The player (fview_aud.c) keeps its slots
**     and calls these on its play order.
**   - One line per track, tab separated with the fields escaped, so an
**     online item's ref (itself a tabbed line) survives untouched.
**   - Playlists are few and small: all of them live in memory and in one
**     file, rewritten (tmp + rename) at most once per frame after an edit.
**     Newest playlists first; a playlist keeps its tracks in its own order.
**   - Folders added to the queue or a playlist are walked here on the main
**     thread, capped (a few thousand files is a fast local listing; a
**     whole drive is not a sensible queue).
*/
#include "fqueue.h"
#include "fplat.h"
#include "flib.h"

/* ---- index math ------------------------------------------------------------------- */

int q_find(const int *a, int n, int v) {
  for (int i = 0; i < n; i++)
    if (a[i] == v) return i;
  return -1;
}

void q_insert(int *a, int *n, int at, const int *v, int k) {
  if (k <= 0) return;
  at = FM_CLAMP(at, 0, *n);
  memmove(a + at + k, a + at, sizeof(int) * (size_t)(*n - at));
  memcpy(a + at, v, sizeof(int) * (size_t)k);
  *n += k;
}

int q_remove(int *a, int *n, int at, int cur) {
  if (at < 0 || at >= *n) return cur;
  memmove(a + at, a + at + 1, sizeof(int) * (size_t)(*n - at - 1));
  (*n)--;
  if (at < cur) return cur - 1;
  if (at == cur && cur >= *n) return -1;
  return cur;
}

int q_move(int *a, int n, int from, int to, int cur) {
  if (n <= 0) return cur;
  from = FM_CLAMP(from, 0, n - 1);
  to = FM_CLAMP(to, 0, n - 1);
  if (from == to) return cur;
  int v = a[from];
  if (from < to) memmove(a + from, a + from + 1, sizeof(int) * (size_t)(to - from));
  else memmove(a + to + 1, a + to, sizeof(int) * (size_t)(from - to));
  a[to] = v;
  if (cur == from) return to;
  if (from < cur && to >= cur) return cur - 1;
  if (from > cur && to <= cur) return cur + 1;
  return cur;
}

void q_shuffle(int *a, int n, int from, u32 seed) {
  if (from < 0) from = 0;
  for (int i = n - 1; i > from; i--) {
    seed = seed * 1664525u + 1013904223u;
    int j = from + (int)((seed >> 8) % (u32)(i - from + 1));
    int t = a[i]; a[i] = a[j]; a[j] = t;
  }
}

void q_unshuffle(const int *nat, int nn, const int *order, int n, int *out) {
  int k = 0;
  for (int i = 0; i < nn && k < n; i++)
    if (q_find(order, n, nat[i]) >= 0 && q_find(out, k, nat[i]) < 0) out[k++] = nat[i];
  for (int i = 0; i < n && k < n; i++)
    if (q_find(out, k, order[i]) < 0) out[k++] = order[i];
}

/* ---- lines -------------------------------------------------------------------------- */

static void esc_cat(char *out, size_t cap, size_t *at, const char *s) {
  for (; s && *s && *at + 3 < cap; s++) {
    char c = *s, e = 0;
    if (c == '\\') e = '\\';
    else if (c == '\t') e = 't';
    else if (c == '\n') e = 'n';
    else if (c == '\r') e = 'r';
    if (e) { out[(*at)++] = '\\'; out[(*at)++] = e; }
    else out[(*at)++] = c;
  }
  out[*at] = 0;
}

static char *unesc_dup(const char *s, size_t n) {
  char *o = (char *)fm_alloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < n) {
      char e = s[++i];
      c = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
    }
    o[k++] = c;
  }
  o[k] = 0;
  return o;
}

bool qline_make(const FmAudioEntry *e, char *out, size_t cap) {
  if (!e || cap < 32) return false;
  out[0] = 0;
  if (e->headers && e->headers[0]) return false;
  bool has_url = e->url && e->url[0], has_ref = e->ref && e->ref[0];
  if (!has_url && !has_ref) return false;
  if (e->file && !has_url) return false;
  size_t at = (size_t)fm_snprintf(out, cap, "%c\t%d\t%.3f", e->file ? 'F' : 'E', e->live ? 1 : 0,
                                  e->dur > 0 ? e->dur : 0.0);
  const char *f[] = { e->url, e->title, e->artist, e->album, e->art_url, e->ref };
  for (int i = 0; i < FM_COUNT(f); i++) {
    if (at + 2 >= cap) return false;
    out[at++] = '\t';
    out[at] = 0;
    esc_cat(out, cap, &at, f[i]);
  }
  return at + 4 < cap;          /* a cut line would be a damaged track */
}

bool qline_parse(const char *line, FmQItem *it) {
  memset(it, 0, sizeof *it);
  if (!line || (line[0] != 'F' && line[0] != 'E') || line[1] != '\t') return false;
  const char *p[9];
  size_t len[9];
  int k = 0;
  const char *s = line;
  while (k < 9) {
    const char *t = strchr(s, '\t');
    size_t n = t ? (size_t)(t - s) : strlen(s);
    while (!t && n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    p[k] = s;
    len[k++] = n;
    if (!t) break;
    s = t + 1;
  }
  if (k < 9) return false;
  it->file = line[0] == 'F';
  it->live = p[1][0] == '1';
  it->dur = atof(p[2]);
  if (it->dur < 0 || it->dur != it->dur) it->dur = 0;
  char **dst[] = { &it->url, &it->title, &it->artist, &it->album, &it->art, &it->ref };
  for (int i = 0; i < 6; i++) *dst[i] = unesc_dup(p[3 + i], len[3 + i]);
  if ((it->file && !it->url[0]) || (!it->url[0] && !it->ref[0])) {
    qitem_free(it);
    return false;
  }
  return true;
}

void qitem_free(FmQItem *it) {
  fm_free(it->url);
  fm_free(it->title);
  fm_free(it->artist);
  fm_free(it->album);
  fm_free(it->art);
  fm_free(it->ref);
  memset(it, 0, sizeof *it);
}

void qitem_copy(FmQItem *d, const FmAudioEntry *s) {
  memset(d, 0, sizeof *d);
  d->url = fm_strdup(s->url ? s->url : "");
  d->title = fm_strdup(s->title ? s->title : "");
  d->artist = fm_strdup(s->artist ? s->artist : "");
  d->album = fm_strdup(s->album ? s->album : "");
  d->art = fm_strdup(s->art_url ? s->art_url : "");
  d->ref = fm_strdup(s->ref ? s->ref : "");
  d->live = s->live;
  d->file = s->file;
  d->dur = s->dur;
}

FmAudioEntry qitem_entry(const FmQItem *it) {
  FmAudioEntry e;
  memset(&e, 0, sizeof e);
  e.url = it->url;
  e.title = it->title;
  e.artist = it->artist;
  e.album = it->album;
  e.art_url = it->art;
  e.ref = it->ref[0] ? it->ref : NULL;
  e.live = it->live;
  e.file = it->file;
  e.dur = it->dur;
  return e;
}

/* ---- files ---------------------------------------------------------------------------- */

#define LINE_MAX_Q 16384

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

static void chomp(char *line) {
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
}

bool qsave_path(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "audio-queue.txt");
}

typedef struct QW {
  const FmAudioEntry *list;
  int n, cur;
  double pos;
  bool shuffle;
  int repeat;
} QW;

static bool write_queue(FILE *f, void *ud) {
  QW *w = (QW *)ud;
  char *line = (char *)fm_alloc(LINE_MAX_Q);
  /* the current track's index among the lines actually written */
  int cur = 0, k = 0;
  double pos = 0;
  for (int i = 0; i < w->n; i++) {
    if (!qline_make(&w->list[i], line, LINE_MAX_Q)) continue;
    if (i <= w->cur) { cur = k; pos = i == w->cur ? w->pos : 0; }
    k++;
  }
  fputs("# mmcfm music queue: play order, the track heard and where\n", f);
  fprintf(f, "cur %d %.2f\nshuffle %d\nrepeat %d\n", cur, pos, w->shuffle ? 1 : 0, w->repeat);
  for (int i = 0; i < w->n; i++)
    if (qline_make(&w->list[i], line, LINE_MAX_Q)) fprintf(f, "t %s\n", line);
  fm_free(line);
  return true;
}

bool qsave_write(const char *path, const FmAudioEntry *list, int n, int cur, double pos, bool shuffle, int repeat) {
  if (n <= 0) {
    plat_remove_file(path);
    return true;
  }
  QW w = { list, n, cur, pos, shuffle, repeat };
  return save_file(path, write_queue, &w);
}

bool qsave_read(const char *path, FmQSaved *out) {
  memset(out, 0, sizeof *out);
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  char *line = (char *)fm_alloc(LINE_MAX_Q);
  int cap = 0;
  while (fgets(line, LINE_MAX_Q, f)) {
    chomp(line);
    if (!strncmp(line, "cur ", 4)) {
      out->cur = atoi(line + 4);
      const char *sp = strchr(line + 4, ' ');
      out->pos = sp ? atof(sp + 1) : 0;
    } else if (!strncmp(line, "shuffle ", 8)) {
      out->shuffle = atoi(line + 8) != 0;
    } else if (!strncmp(line, "repeat ", 7)) {
      out->repeat = FM_CLAMP(atoi(line + 7), 0, 2);
    } else if (!strncmp(line, "t ", 2)) {
      FmQItem it;
      if (!qline_parse(line + 2, &it)) continue;
      if (out->n == cap) {
        cap = cap ? cap * 2 : 64;
        out->items = (FmQItem *)fm_realloc(out->items, sizeof(FmQItem) * (size_t)cap);
      }
      out->items[out->n++] = it;
    }
  }
  fm_free(line);
  fclose(f);
  out->cur = out->n > 0 ? FM_CLAMP(out->cur, 0, out->n - 1) : 0;
  if (out->pos < 0 || out->pos != out->pos) out->pos = 0;
  return out->n > 0;
}

void qsave_free(FmQSaved *s) {
  for (int i = 0; i < s->n; i++) qitem_free(&s->items[i]);
  fm_free(s->items);
  memset(s, 0, sizeof *s);
}

/* ---- entries from files ------------------------------------------------------------------ */

typedef struct EntList {
  FmAudioEntry *e;
  int n, cap, max;
} EntList;

static void ent_push_file(EntList *l, const char *path) {
  if (l->n >= l->max) return;
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 32;
    l->e = (FmAudioEntry *)fm_realloc(l->e, sizeof(FmAudioEntry) * (size_t)l->cap);
  }
  FmAudioEntry *e = &l->e[l->n++];
  memset(e, 0, sizeof *e);
  e->file = true;
  e->url = fm_strdup(path);
  int k = lib_find(path);
  if (k >= 0) {
    const FmLibItem *it = &lib_data()->items[k];
    char t[512];
    lib_item_title(it, t, sizeof t);
    e->title = fm_strdup(t);
    e->artist = fm_strdup(lib_str(it->artist));
    e->album = fm_strdup(lib_str(it->album));
  }
}

static int cmp_name(const void *a, const void *b) { return fm_natcmp(*(char *const *)a, *(char *const *)b); }

/* A folder's audio files in name order, then its subfolders' (depth capped). */
static void walk_dir(EntList *l, const char *dir, int depth) {
  if (depth > 8 || l->n >= l->max) return;
  FmErr err;
  FmDir *d = plat_dir_open(dir, &err);
  if (!d) return;
  char **files = NULL, **dirs = NULL;
  int nf = 0, nd = 0, cf = 0, cd = 0;
  const char *name;
  FmStat st;
  while (plat_dir_next(d, &name, &st)) {
    if (name[0] == '.') continue;
    bool isdir = (st.flags & FM_ST_DIR) != 0;
    if (isdir && (st.flags & FM_ST_LINK)) continue;
    if (!isdir && fm_type_from_name(name) != FT_AUDIO) continue;
    char ***arr = isdir ? &dirs : &files;
    int *n = isdir ? &nd : &nf, *c = isdir ? &cd : &cf;
    if (*n == *c) {
      *c = *c ? *c * 2 : 32;
      *arr = (char **)fm_realloc(*arr, sizeof(char *) * (size_t)*c);
    }
    (*arr)[(*n)++] = fm_strdup(name);
  }
  plat_dir_close(d);
  if (nf) qsort(files, (size_t)nf, sizeof(char *), cmp_name);
  if (nd) qsort(dirs, (size_t)nd, sizeof(char *), cmp_name);
  char p[FM_PATH_MAX];
  for (int i = 0; i < nf; i++)
    if (fm_path_join(p, sizeof p, dir, files[i])) ent_push_file(l, p);
  for (int i = 0; i < nd; i++)
    if (fm_path_join(p, sizeof p, dir, dirs[i])) walk_dir(l, p, depth + 1);
  for (int i = 0; i < nf; i++) fm_free(files[i]);
  for (int i = 0; i < nd; i++) fm_free(dirs[i]);
  fm_free(files);
  fm_free(dirs);
}

FmAudioEntry *qents_from_paths(const char *const *paths, int n, int max, int *out_n) {
  EntList l = { NULL, 0, 0, max > 0 ? max : 5000 };
  for (int i = 0; i < n; i++) {
    if (!paths[i] || !paths[i][0]) continue;
    if (plat_is_dir(paths[i])) walk_dir(&l, paths[i], 0);
    else if (fm_type_from_name(paths[i]) == FT_AUDIO) ent_push_file(&l, paths[i]);
  }
  *out_n = l.n;
  return l.e;
}

void qents_free(FmAudioEntry *e, int n) {
  if (!e) return;
  for (int i = 0; i < n; i++) {
    fm_free((void *)e[i].url);
    fm_free((void *)e[i].title);
    fm_free((void *)e[i].artist);
    fm_free((void *)e[i].album);
    fm_free((void *)e[i].art_url);
    fm_free((void *)e[i].headers);
    fm_free((void *)e[i].ref);
  }
  fm_free(e);
}

static char *dup_opt(const char *s) { return s ? fm_strdup(s) : NULL; }

FmAudioEntry *qents_dup(const FmAudioEntry *e, int n) {
  FmAudioEntry *d = (FmAudioEntry *)fm_calloc((size_t)FM_MAX(n, 1), sizeof *d);
  for (int i = 0; i < n; i++) {
    d[i] = e[i];
    d[i].url = dup_opt(e[i].url);
    d[i].title = dup_opt(e[i].title);
    d[i].artist = dup_opt(e[i].artist);
    d[i].album = dup_opt(e[i].album);
    d[i].art_url = dup_opt(e[i].art_url);
    d[i].headers = dup_opt(e[i].headers);
    d[i].ref = dup_opt(e[i].ref);
  }
  return d;
}

/* ---- playlists ------------------------------------------------------------------------------ */

typedef struct Pl {
  char name[PL_NAME_MAX];
  FmQItem *items;
  int n, cap;
} Pl;

static struct {
  Pl *pl;
  int n, cap;
  bool ro, dirty, loaded;
  u32 gen;
  char file[FM_PATH_MAX];       /* the self test's, else "" */
} G;

static bool pl_path(char *out, size_t cap) {
  if (G.file[0]) { fm_strlcpy(out, G.file, cap); return true; }
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "audio-playlists.txt");
}

static void pl_free_one(Pl *p) {
  for (int i = 0; i < p->n; i++) qitem_free(&p->items[i]);
  fm_free(p->items);
  memset(p, 0, sizeof *p);
}

static void pl_clear(void) {
  for (int i = 0; i < G.n; i++) pl_free_one(&G.pl[i]);
  fm_free(G.pl);
  G.pl = NULL;
  G.n = G.cap = 0;
  G.gen++;
}

static void changed(void) {
  G.dirty = true;
  G.gen++;
}

static Pl *pl_new_at(int at) {
  if (G.n == G.cap) {
    G.cap = G.cap ? G.cap * 2 : 8;
    G.pl = (Pl *)fm_realloc(G.pl, sizeof(Pl) * (size_t)G.cap);
  }
  at = FM_CLAMP(at, 0, G.n);
  memmove(G.pl + at + 1, G.pl + at, sizeof(Pl) * (size_t)(G.n - at));
  G.n++;
  memset(&G.pl[at], 0, sizeof(Pl));
  return &G.pl[at];
}

static void pl_push_item(Pl *p, const FmQItem *it) {
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 16;
    p->items = (FmQItem *)fm_realloc(p->items, sizeof(FmQItem) * (size_t)p->cap);
  }
  p->items[p->n++] = *it;
}

/* Names are one line, trimmed. */
static void clean_name(const char *in, char *out, size_t cap) {
  fm_strlcpy(out, in ? in : "", cap);
  for (char *c = out; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  char *b = out;
  while (*b == ' ') b++;
  if (b != out) memmove(out, b, strlen(b) + 1);
  size_t n = strlen(out);
  while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static void pl_load(void) {
  G.loaded = true;
  char path[FM_PATH_MAX];
  if (!pl_path(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  char *line = (char *)fm_alloc(LINE_MAX_Q);
  Pl *cur = NULL;
  while (fgets(line, LINE_MAX_Q, f)) {
    chomp(line);
    if (!strncmp(line, "p ", 2)) {
      char *nm = unesc_dup(line + 2, strlen(line + 2));
      cur = pl_new_at(G.n);
      clean_name(nm, cur->name, sizeof cur->name);
      if (!cur->name[0]) fm_strlcpy(cur->name, "Playlist", sizeof cur->name);
      fm_free(nm);
    } else if (!strncmp(line, "t ", 2) && cur) {
      FmQItem it;
      if (qline_parse(line + 2, &it)) pl_push_item(cur, &it);
    }
  }
  fm_free(line);
  fclose(f);
  G.gen++;
}

static void ensure(void) {
  if (!G.loaded) pl_load();
}

static bool write_pl(FILE *f, void *ud) {
  FM_UNUSED(ud);
  char *line = (char *)fm_alloc(LINE_MAX_Q);
  fputs("# mmcfm playlists: p <name>, then its tracks in order\n", f);
  for (int i = 0; i < G.n; i++) {
    size_t at = 0;
    line[0] = 0;
    esc_cat(line, LINE_MAX_Q, &at, G.pl[i].name);
    fprintf(f, "p %s\n", line);
    for (int k = 0; k < G.pl[i].n; k++) {
      FmAudioEntry e = qitem_entry(&G.pl[i].items[k]);
      if (qline_make(&e, line, LINE_MAX_Q)) fprintf(f, "t %s\n", line);
    }
  }
  fm_free(line);
  return true;
}

void pl_flush(void) {
  if (!G.dirty) return;
  G.dirty = false;
  char path[FM_PATH_MAX];
  if (G.ro || !pl_path(path, sizeof path)) return;
  if (!save_file(path, write_pl, NULL)) fm_log("playlists: can't save %s", path);
}

void pl_init(bool readonly) {
  pl_clear();
  G.ro = readonly;
  G.loaded = false;
  G.dirty = false;
}

void pl_shutdown(void) {
  pl_flush();
  pl_clear();
  G.loaded = false;
}

void pl_pump(void) { pl_flush(); }

void pl_test_file(const char *path) {
  pl_clear();
  fm_strlcpy(G.file, path ? path : "", sizeof G.file);
  G.ro = false;
  G.loaded = false;
  G.dirty = false;
}

int pl_count(void) { ensure(); return G.n; }
const char *pl_name(int p) { ensure(); return p >= 0 && p < G.n ? G.pl[p].name : ""; }
int pl_len(int p) { ensure(); return p >= 0 && p < G.n ? G.pl[p].n : 0; }
u32 pl_gen(void) { ensure(); return G.gen; }

const FmQItem *pl_item(int p, int k) {
  ensure();
  if (p < 0 || p >= G.n || k < 0 || k >= G.pl[p].n) return NULL;
  return &G.pl[p].items[k];
}

double pl_duration(int p) {
  double d = 0;
  for (int k = 0; k < pl_len(p); k++) d += G.pl[p].items[k].dur;
  return d;
}

int pl_find(const char *name) {
  ensure();
  char n[PL_NAME_MAX];
  clean_name(name, n, sizeof n);
  for (int i = 0; i < G.n; i++)
    if (fm_stricmp(G.pl[i].name, n) == 0) return i;
  return -1;
}

void pl_unique_name(const char *base, char *out, size_t cap) {
  char b[PL_NAME_MAX];
  clean_name(base && base[0] ? base : "Playlist", b, sizeof b);
  if (!b[0]) fm_strlcpy(b, "Playlist", sizeof b);
  bool numbered = !base || !base[0] || !strcmp(base, "Playlist");
  for (int i = numbered ? 1 : 0; i < 10000; i++) {
    if (i == 0) fm_strlcpy(out, b, cap);
    else fm_snprintf(out, cap, "%s %d", b, i + (numbered ? 0 : 1));
    if (pl_find(out) < 0) return;
  }
  fm_strlcpy(out, b, cap);
}

int pl_create(const char *name) {
  ensure();
  char n[PL_NAME_MAX];
  clean_name(name, n, sizeof n);
  if (!n[0]) return -1;
  Pl *p = pl_new_at(0);
  fm_strlcpy(p->name, n, sizeof p->name);
  changed();
  return 0;
}

bool pl_rename(int p, const char *name) {
  ensure();
  char n[PL_NAME_MAX];
  clean_name(name, n, sizeof n);
  if (p < 0 || p >= G.n || !n[0]) return false;
  int other = pl_find(n);
  if (other >= 0 && other != p) return false;
  fm_strlcpy(G.pl[p].name, n, sizeof G.pl[p].name);
  changed();
  return true;
}

void pl_delete(int p) {
  ensure();
  if (p < 0 || p >= G.n) return;
  pl_free_one(&G.pl[p]);
  memmove(G.pl + p, G.pl + p + 1, sizeof(Pl) * (size_t)(G.n - p - 1));
  G.n--;
  changed();
}

int pl_add(int p, const FmAudioEntry *e, int n) {
  ensure();
  if (p < 0 || p >= G.n) return 0;
  char *line = (char *)fm_alloc(LINE_MAX_Q);
  int took = 0;
  for (int i = 0; i < n; i++) {
    if (!qline_make(&e[i], line, LINE_MAX_Q)) continue;     /* not something a list can keep */
    FmQItem it;
    qitem_copy(&it, &e[i]);
    pl_push_item(&G.pl[p], &it);
    took++;
  }
  fm_free(line);
  if (took) changed();
  return took;
}

void pl_move(int p, int from, int to) {
  ensure();
  if (p < 0 || p >= G.n) return;
  Pl *l = &G.pl[p];
  if (from < 0 || from >= l->n) return;
  to = FM_CLAMP(to, 0, l->n - 1);
  if (from == to) return;
  FmQItem v = l->items[from];
  if (from < to) memmove(l->items + from, l->items + from + 1, sizeof(FmQItem) * (size_t)(to - from));
  else memmove(l->items + to + 1, l->items + to, sizeof(FmQItem) * (size_t)(from - to));
  l->items[to] = v;
  changed();
}

void pl_remove(int p, int k) {
  ensure();
  if (p < 0 || p >= G.n || k < 0 || k >= G.pl[p].n) return;
  Pl *l = &G.pl[p];
  qitem_free(&l->items[k]);
  memmove(l->items + k, l->items + k + 1, sizeof(FmQItem) * (size_t)(l->n - k - 1));
  l->n--;
  changed();
}

FmAudioEntry *pl_entries(int p, int *n) {
  *n = pl_len(p);
  FmAudioEntry *e = (FmAudioEntry *)fm_calloc((size_t)FM_MAX(*n, 1), sizeof *e);
  for (int k = 0; k < *n; k++) e[k] = qitem_entry(&G.pl[p].items[k]);
  return e;
}

bool pl_local_only(int p) {
  int n = pl_len(p);
  for (int k = 0; k < n; k++)
    if (!G.pl[p].items[k].file) return false;
  return n > 0;
}

/* ---- .m3u8 ---------------------------------------------------------------------------- */

bool pl_export_m3u(int p, const char *path) {
  if (!pl_local_only(p)) return false;
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  fputs("#EXTM3U\n", f);
  Pl *l = &G.pl[p];
  for (int k = 0; k < l->n; k++) {
    const FmQItem *it = &l->items[k];
    int secs = it->dur > 0 ? (int)(it->dur + 0.5) : -1;
    if (it->title[0] && it->artist[0]) fprintf(f, "#EXTINF:%d,%s - %s\n", secs, it->artist, it->title);
    else if (it->title[0]) fprintf(f, "#EXTINF:%d,%s\n", secs, it->title);
    fprintf(f, "%s\n", it->url);
  }
  bool ok = !ferror(f);
  if (fclose(f) != 0) ok = false;
  return ok;
}

static bool path_is_abs(const char *p) {
  if (p[0] == '/' || p[0] == '\\') return true;
  return ((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':';
}

int pl_import_m3u(const char *path) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return -1;
  char dir[FM_PATH_MAX], name[PL_NAME_MAX], line[FM_PATH_MAX + 64], title[256], full[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  fm_path_parent(dir);
  fm_strlcpy(name, fm_path_base(path), sizeof name);
  char *dot = strrchr(name, '.');
  if (dot && dot != name) *dot = 0;
  FmAudioEntry *e = NULL;
  int n = 0, cap = 0;
  double dur = 0;
  title[0] = 0;
  bool first = true;
  while (fgets(line, sizeof line, f)) {
    chomp(line);
    char *s = line;
    if (first && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s += 3;   /* BOM */
    first = false;
    while (*s == ' ' || *s == '\t') s++;
    if (!s[0]) continue;
    if (!fm_strnicmp(s, "#EXTINF:", 8)) {
      dur = atof(s + 8);
      const char *c = strchr(s, ',');
      fm_strlcpy(title, c ? c + 1 : "", sizeof title);
      continue;
    }
    if (s[0] == '#') continue;
    bool url = !fm_strnicmp(s, "http://", 7) || !fm_strnicmp(s, "https://", 8);
    if (!fm_strnicmp(s, "file://", 7)) s += 7;
    if (!url) {
      if (path_is_abs(s)) fm_strlcpy(full, s, sizeof full);
      else if (!fm_path_join(full, sizeof full, dir, s)) continue;
#ifdef FM_WIN
      for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
#else
      for (char *c = full; *c; c++) if (*c == '\\') *c = '/';
#endif
      fm_path_normalize(full);
    }
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      e = (FmAudioEntry *)fm_realloc(e, sizeof(FmAudioEntry) * (size_t)cap);
    }
    FmAudioEntry *it = &e[n++];
    memset(it, 0, sizeof *it);
    it->url = fm_strdup(url ? s : full);
    it->file = !url;
    it->dur = dur > 0 ? dur : 0;
    /* "Artist - Title" from #EXTINF */
    const char *dash = strstr(title, " - ");
    if (dash) {
      it->artist = fm_strdup(title);
      ((char *)it->artist)[dash - title] = 0;
      it->title = fm_strdup(dash + 3);
    } else {
      it->title = fm_strdup(title);
    }
    title[0] = 0;
    dur = 0;
  }
  fclose(f);
  if (n == 0) { fm_free(e); return -1; }
  char uniq[PL_NAME_MAX];
  pl_unique_name(name, uniq, sizeof uniq);
  int p = pl_create(uniq);
  if (p >= 0) pl_add(p, e, n);
  qents_free(e, n);
  return p;
}
