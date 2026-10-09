/* faudio_online_task.c -- online audio: worker tasks, downloads, recent
** searches, the library (favourites, recently played, subscriptions) and the
** hookup with the music player.
**
** Design decisions:
**   - Tasks are the photo view's (fphoto_task.c) in shape: every adapter call
**     blocks, so each runs on its own thread in an AoTask the main thread
**     owns, polled every frame; abandoned ones are cancelled and parked
**     until their thread returns instead of being waited for.
**   - Items travel as one line of text (aitem_ref): the library file keeps
**     them that way and the player gets the same line as an entry's ref, so
**     a favourite, a recently played row and a playing entry are all the
**     same few hundred bytes, not a 3 KB struct each. Lines are parsed only
**     when a page shows them; each keeps its key for the hearts and the
**     "now playing" check, which run every frame.
**   - The player resolves entries itself (FmAudioHooks.resolve on its
**     decoder thread), through asrc_stream: a station's click is counted
**     and its freshest address used, a track's expiring stream link is
**     fetched when it plays, and an AAC station without FFmpeg says so
**     before connecting. The settings it needs are snapshot on the main
**     thread when a list starts playing.
**   - Downloads are a queue with at most two running; only finite tracks
**     download (stations are endless).
**   - The library and the recent searches are small text files next to the
**     settings, saved right after a change; --shot runs never write.
*/
#include "faudio_online_int.h"

/* ---- item lists -------------------------------------------------------------- */

void aol_init(AoList *l, const char *name) {
  memset(l, 0, sizeof *l);
  l->id = ui_id(name);
}

void aol_free(AoList *l) {
  fm_free(l->items);
  u32 id = l->id;
  memset(l, 0, sizeof *l);
  l->id = id;
}

void aol_clear(AoList *l) {
  l->n = 0;
  memset(&l->scroll, 0, sizeof l->scroll);
}

FmAsrcItem *aol_push(AoList *l) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 40;
    l->items = (FmAsrcItem *)fm_realloc(l->items, sizeof *l->items * (size_t)l->cap);
  }
  FmAsrcItem *it = &l->items[l->n++];
  memset(it, 0, sizeof *it);
  return it;
}

int aol_find(const AoList *l, const char *src, const char *id) {
  for (int i = 0; i < l->n; i++)
    if (!strcmp(l->items[i].id, id) && !strcmp(l->items[i].source, src)) return i;
  return -1;
}

void aitem_key(const FmAsrcItem *it, char *out, size_t cap) {
  fm_snprintf(out, cap, "%s\t%d\t%s", it->source, it->kind, it->id);
}

bool aitem_same(const FmAsrcItem *a, const FmAsrcItem *b) {
  return a->kind == b->kind && !strcmp(a->id, b->id) && !strcmp(a->source, b->source);
}

bool aitem_playable(const FmAsrcItem *it) { return it->kind == AITEM_TRACK || it->kind == AITEM_STATION; }
bool aitem_container(const FmAsrcItem *it) { return it->kind == AITEM_PODCAST || it->kind == AITEM_ALBUM; }

const char *ao_src_name(const char *key) {
  const FmAsrc *s = asrc_find(key);
  return s ? s->name : key;
}

FmIcon ao_src_icon(const FmAsrc *s) {
  if (!s) return IC_MUSIC;
  if (!strcmp(s->key, "radio")) return IC_RADIO;
  if (!strcmp(s->key, "podcasts")) return IC_PODCAST;
  if (!strcmp(s->key, "freesound")) return IC_WAVEFORM;
  if (!strcmp(s->key, "archive")) return IC_LANDMARK;
  return s->icon ? s->icon : IC_MUSIC;
}

/* ---- one line per item ---------------------------------------------------------- */

static void put_esc(char *out, size_t cap, size_t *o, const char *s) {
  for (; *s && *o + 3 < cap; s++) {
    char c = *s;
    if (c == '\\' || c == '\t' || c == '\n' || c == '\r') {
      out[(*o)++] = '\\';
      out[(*o)++] = c == '\t' ? 't' : c == '\n' ? 'n' : c == '\r' ? 'r' : '\\';
    } else {
      out[(*o)++] = c;
    }
  }
}

void aitem_ref(const FmAsrcItem *it, char *out, size_t cap) {
  char num[64];
  const char *f[15];
  char kind[8], br[16], plays[24];
  fm_snprintf(kind, sizeof kind, "%d", it->kind);
  fm_snprintf(br, sizeof br, "%d", it->bitrate);
  fm_snprintf(num, sizeof num, "%.3f", it->duration);
  fm_snprintf(plays, sizeof plays, "%lld", (long long)it->plays);
  f[0] = it->source; f[1] = kind; f[2] = it->id; f[3] = it->title; f[4] = it->artist; f[5] = it->album;
  f[6] = it->art; f[7] = it->url; f[8] = it->page; f[9] = it->codec; f[10] = it->license; f[11] = br;
  f[12] = num; f[13] = it->published; f[14] = plays;
  size_t o = 0;
  if (cap < 8) { if (cap) out[0] = 0; return; }
  out[o++] = 'a';
  out[o++] = '1';
  for (int i = 0; i < 15; i++) {
    if (o + 2 < cap) out[o++] = '\t';
    put_esc(out, cap, &o, f[i]);
  }
  out[o] = 0;
}

/* Copies field `s` (up to a tab) unescaped into out; returns past the tab. */
static const char *take(const char *s, char *out, size_t cap) {
  size_t o = 0;
  while (*s && *s != '\t') {
    char c = *s++;
    if (c == '\\' && *s) {
      char e = *s++;
      c = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
    }
    if (o + 1 < cap) out[o++] = c;
  }
  if (cap) out[o] = 0;
  return *s == '\t' ? s + 1 : s;
}

bool aitem_parse(const char *ref, FmAsrcItem *it) {
  memset(it, 0, sizeof *it);
  it->plays = -1;
  if (!ref || ref[0] != 'a' || ref[1] != '1' || ref[2] != '\t') return false;
  const char *s = ref + 3;
  char tmp[64];
  s = take(s, it->source, sizeof it->source);
  s = take(s, tmp, sizeof tmp);
  it->kind = FM_CLAMP(atoi(tmp), AITEM_TRACK, AITEM_ALBUM);
  s = take(s, it->id, sizeof it->id);
  s = take(s, it->title, sizeof it->title);
  s = take(s, it->artist, sizeof it->artist);
  s = take(s, it->album, sizeof it->album);
  s = take(s, it->art, sizeof it->art);
  s = take(s, it->url, sizeof it->url);
  s = take(s, it->page, sizeof it->page);
  s = take(s, it->codec, sizeof it->codec);
  s = take(s, it->license, sizeof it->license);
  s = take(s, tmp, sizeof tmp);
  it->bitrate = atoi(tmp);
  s = take(s, tmp, sizeof tmp);
  it->duration = atof(tmp);
  s = take(s, it->published, sizeof it->published);
  s = take(s, tmp, sizeof tmp);
  if (tmp[0]) {
    /* by hand: 32-bit msvcrt (tcc) has no strtoll */
    const char *p = tmp;
    bool neg = *p == '-';
    if (neg) p++;
    i64 v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    it->plays = neg ? -v : v;
  }
  return it->source[0] && it->id[0];
}

void aref_key(const char *ref, char *out, size_t cap) {
  if (cap) out[0] = 0;
  if (!ref || ref[0] != 'a' || ref[1] != '1' || ref[2] != '\t') return;
  char src[32], kind[8], id[160];
  const char *s = take(ref + 3, src, sizeof src);
  s = take(s, kind, sizeof kind);
  take(s, id, sizeof id);
  fm_snprintf(out, cap, "%s\t%d\t%s", src, atoi(kind), id);
}

/* ---- tasks ------------------------------------------------------------------- */

static bool dl_progress(void *user, u64 done, u64 total) {
  AoTask *t = (AoTask *)user;
  u64 now = SDL_GetTicks64();
  SDL_LockMutex(t->mx);
  t->frac = total ? (float)((double)done / (double)total) : -1.0f;
  bool wake = now - t->last_wake >= 100;
  if (wake) t->last_wake = now;
  SDL_UnlockMutex(t->mx);
  if (wake) app_wake();
  return !t->cancel;
}

static int task_main(void *u) {
  AoTask *t = (AoTask *)u;
  const FmAsrc *s = t->src;
  switch (t->kind) {
    case AT_SEARCH:
      t->err = s && s->search ? s->search(&t->conf, t->query, t->token, &t->page, &t->cancel) : FM_ERR_UNSUPPORTED;
      break;
    case AT_BROWSE:
      t->err = s && s->browse ? s->browse(&t->conf, t->cat, t->token, &t->page, &t->cancel) : FM_ERR_UNSUPPORTED;
      break;
    case AT_CATS:
      t->ncats = s && s->categories ? s->categories(&t->conf, t->cats, AO_MAX_CATS) : 0;
      t->ncats = FM_CLAMP(t->ncats, 0, AO_MAX_CATS);
      t->err = FM_OK;
      break;
    case AT_CHILDREN:
      t->err = s && s->children ? s->children(&t->conf, &t->item, t->token, &t->page, &t->cancel) : FM_ERR_UNSUPPORTED;
      break;
    case AT_DOWNLOAD:
      t->err = asrc_download(&t->conf, &t->item, t->dir, t->out, sizeof t->out, dl_progress, t, t->errtext,
                             sizeof t->errtext, &t->cancel);
      break;
    default: t->err = FM_ERR_UNSUPPORTED; break;
  }
  if (t->err != FM_OK && !t->page.error[0] && t->kind != AT_DOWNLOAD)
    fm_strlcpy(t->page.error, t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err), sizeof t->page.error);
  if (t->err != FM_OK && !t->errtext[0])
    fm_strlcpy(t->errtext, t->page.error[0] ? t->page.error : t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err),
               sizeof t->errtext);
  SDL_AtomicSet(&t->done, 1);
  app_wake();
  return 0;
}

AoTask *atask_new(int kind, const FmAsrc *src) {
  AoTask *t = (AoTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  t->src = src;
  t->frac = -1;
  asrc_conf_snapshot(&t->conf);
  return t;
}

bool atask_run(AoTask *t) {
  t->mx = SDL_CreateMutex();
  if (t->mx) t->thr = fm_thread_create(task_main, "aonline", t);
  if (!t->thr) {
    t->err = FM_ERR_NOMEM;
    fm_strlcpy(t->errtext, "Could not start a worker thread", sizeof t->errtext);
    fm_strlcpy(t->page.error, t->errtext, sizeof t->page.error);
    SDL_AtomicSet(&t->done, 1);
    return false;
  }
  return true;
}

bool atask_done(AoTask *t) { return t && SDL_AtomicGet(&t->done) != 0; }

float atask_frac(AoTask *t) {
  if (!t || !t->mx) return -1;
  SDL_LockMutex(t->mx);
  float f = t->frac;
  SDL_UnlockMutex(t->mx);
  return f;
}

void atask_free(AoTask *t, bool cancel) {
  if (!t) return;
  if (cancel) t->cancel = 1;
  if (t->thr) SDL_WaitThread(t->thr, NULL);
  if (t->mx) SDL_DestroyMutex(t->mx);
  asrc_page_free(&t->page);
  fm_free(t);
}

static AoTask *g_dead[32];
static int g_ndead;

void atask_drop(AoTask *t) {
  if (!t) return;
  t->cancel = 1;
  if (g_ndead < FM_COUNT(g_dead)) g_dead[g_ndead++] = t;
  else atask_free(t, true);
}

void atask_reap(void) {
  for (int i = g_ndead - 1; i >= 0; i--)
    if (atask_done(g_dead[i])) {
      atask_free(g_dead[i], false);
      g_dead[i] = g_dead[--g_ndead];
    }
}

void atask_shutdown(void) {
  for (int i = 0; i < g_ndead; i++) g_dead[i]->cancel = 1;
  for (int i = 0; i < g_ndead; i++) atask_free(g_dead[i], true);
  g_ndead = 0;
}

/* ---- downloads ------------------------------------------------------------------ */

#define ADL_MAX 200
#define ADL_PARALLEL 2

static AoDl *g_dl;              /* newest first */
static int g_ndl, g_capdl;

int adl_count(void) { return g_ndl; }
AoDl *adl_at(int i) { return i >= 0 && i < g_ndl ? &g_dl[i] : NULL; }

int adl_active(void) {
  int n = 0;
  for (int i = 0; i < g_ndl; i++) n += g_dl[i].state == AD_RUNNING || g_dl[i].state == AD_QUEUED;
  return n;
}

float adl_frac(void) {
  float sum = 0;
  int n = 0;
  for (int i = 0; i < g_ndl; i++) {
    if (g_dl[i].state == AD_QUEUED) { n++; continue; }
    if (g_dl[i].state != AD_RUNNING) continue;
    if (g_dl[i].frac < 0) return -1;
    sum += g_dl[i].frac;
    n++;
  }
  return n ? sum / (float)n : -1;
}

bool adl_has(const FmAsrcItem *it) {
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].state != AD_FAILED && g_dl[i].state != AD_CANCELLED && aitem_same(&g_dl[i].item, it)) return true;
  return false;
}

void adl_remove(int i) {
  if (i < 0 || i >= g_ndl) return;
  if (g_dl[i].task) atask_free(g_dl[i].task, true);
  memmove(&g_dl[i], &g_dl[i + 1], sizeof g_dl[0] * (size_t)(g_ndl - i - 1));
  g_ndl--;
}

void adl_start(const FmAsrcItem *it) {
  if (it->kind != AITEM_TRACK) {
    ui_toast(it->kind == AITEM_STATION ? "Live stations cannot be downloaded" : "Open it to download its tracks");
    return;
  }
  for (int i = 0; i < g_ndl; i++)
    if ((g_dl[i].state == AD_RUNNING || g_dl[i].state == AD_QUEUED) && aitem_same(&g_dl[i].item, it)) {
      ui_toast("Already downloading");
      return;
    }
  if (g_ndl >= ADL_MAX) {
    int k = -1;
    for (int i = g_ndl - 1; i >= 0 && k < 0; i--)
      if (g_dl[i].state != AD_RUNNING && g_dl[i].state != AD_QUEUED) k = i;
    if (k < 0) { ui_toast("Too many downloads at once"); return; }
    adl_remove(k);
  }
  if (g_ndl == g_capdl) {
    g_capdl = g_capdl ? g_capdl * 2 : 8;
    g_dl = (AoDl *)fm_realloc(g_dl, sizeof *g_dl * (size_t)g_capdl);
  }
  memmove(&g_dl[1], &g_dl[0], sizeof g_dl[0] * (size_t)g_ndl);
  g_ndl++;
  AoDl *d = &g_dl[0];
  memset(d, 0, sizeof *d);
  d->item = *it;
  d->frac = -1;
  d->state = AD_QUEUED;
  ui_toast("Downloading \xE2\x80\x9C%s\xE2\x80\x9D", it->title[0] ? it->title : "track");
  adl_pump();
  ui_redraw();
}

void adl_cancel(int i) {
  AoDl *d = adl_at(i);
  if (!d) return;
  if (d->task) d->task->cancel = 1;
  else if (d->state == AD_QUEUED) d->state = AD_CANCELLED;
  ui_redraw();
}

void adl_retry(int i) {
  AoDl *d = adl_at(i);
  if (!d || d->state == AD_RUNNING || d->state == AD_QUEUED) return;
  d->state = AD_QUEUED;
  d->err[0] = 0;
  d->frac = -1;
  d->tries = 1;
  adl_pump();
}

void adl_clear_finished(void) {
  for (int i = g_ndl - 1; i >= 0; i--)
    if (g_dl[i].state != AD_RUNNING && g_dl[i].state != AD_QUEUED) adl_remove(i);
}

static void start_dl(AoDl *d) {
  AoTask *t = atask_new(AT_DOWNLOAD, asrc_find(d->item.source));
  t->item = d->item;
  fm_strlcpy(t->dir, t->conf.download_dir, sizeof t->dir);
  if (!t->dir[0]) odl_dir(t->dir, sizeof t->dir);
  plat_mkdirs(t->dir);
  d->task = t;
  d->state = AD_RUNNING;
  if (!atask_run(t)) {
    d->state = AD_FAILED;
    fm_strlcpy(d->err, t->errtext, sizeof d->err);
    atask_free(t, false);
    d->task = NULL;
  }
}

void adl_pump(void) {
  int running = 0;
  for (int i = 0; i < g_ndl; i++) {
    AoDl *d = &g_dl[i];
    if (d->state != AD_RUNNING || !d->task) continue;
    d->frac = atask_frac(d->task);
    if (!atask_done(d->task)) { running++; continue; }
    AoTask *t = d->task;
    if (t->err == FM_OK) {
      d->state = AD_DONE;
      d->frac = 1;
      fm_strlcpy(d->path, t->out, sizeof d->path);
      ui_toast("Saved \xE2\x80\x9C%s\xE2\x80\x9D", fm_path_base(d->path));
    } else if (t->err == FM_ERR_CANCEL) {
      d->state = AD_CANCELLED;
    } else if (d->tries++ == 0 && fm_stristr(t->errtext, "network")) {
      d->state = AD_QUEUED;                 /* once more after a network blip */
      atask_free(t, false);
      d->task = NULL;
      continue;
    } else {
      d->state = AD_FAILED;
      fm_strlcpy(d->err, t->errtext, sizeof d->err);
      ui_toast("Download failed: %s", d->err);
    }
    atask_free(t, false);
    d->task = NULL;
    ui_redraw();
  }
  for (int i = g_ndl - 1; i >= 0 && running < ADL_PARALLEL; i--)
    if (g_dl[i].state == AD_QUEUED) {
      start_dl(&g_dl[i]);
      running += g_dl[i].state == AD_RUNNING;
    }
}

void adl_shutdown(void) {
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) g_dl[i].task->cancel = 1;
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) atask_free(g_dl[i].task, true);
  fm_free(g_dl);
  g_dl = NULL;
  g_ndl = g_capdl = 0;
}

/* ---- small text files next to the settings ---------------------------------------- */

static bool config_file(const char *name, char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, name);
}

static FILE *open_write(const char *path) {
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  fm_path_parent(dir);
  plat_mkdirs(dir);
  return fm_fopen(path, "wb");
}

static void chomp(char *line) {
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
}

/* ---- recent searches ------------------------------------------------------------- */

static char g_recent[ARECENT_MAX][256];
static int g_nrecent;
static bool g_recent_ro;

static void recent_save(void) {
  char path[FM_PATH_MAX];
  if (g_recent_ro || !config_file("audio-recent.txt", path, sizeof path)) return;
  FILE *f = open_write(path);
  if (!f) return;
  fputs("# mmcfm online audio: recent searches, newest first\n", f);
  for (int i = 0; i < g_nrecent; i++) fprintf(f, "q %s\n", g_recent[i]);
  fclose(f);
}

void arecent_load(bool readonly) {
  g_recent_ro = readonly;
  g_nrecent = 0;
  char path[FM_PATH_MAX], line[512];
  if (readonly || !config_file("audio-recent.txt", path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  while (g_nrecent < ARECENT_MAX && fgets(line, sizeof line, f)) {
    chomp(line);
    if (!strncmp(line, "q ", 2) && line[2]) fm_strlcpy(g_recent[g_nrecent++], line + 2, sizeof g_recent[0]);
  }
  fclose(f);
}

void arecent_add(const char *q) {
  char s[256];
  fm_strlcpy(s, q, sizeof s);
  for (char *p = s; *p; p++)
    if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
  char *b = s;
  while (*b == ' ') b++;
  size_t n = strlen(b);
  while (n > 0 && b[n - 1] == ' ') b[--n] = 0;
  if (!b[0]) return;
  int at = g_nrecent;
  for (int i = 0; i < g_nrecent; i++)
    if (fm_stricmp(g_recent[i], b) == 0) { at = i; break; }
  if (at == 0 && g_nrecent > 0 && strcmp(g_recent[0], b) == 0) return;
  bool grow = at == g_nrecent && g_nrecent < ARECENT_MAX;
  if (at == g_nrecent) at = FM_MIN(g_nrecent, ARECENT_MAX - 1);
  memmove(&g_recent[1], &g_recent[0], sizeof g_recent[0] * (size_t)at);
  fm_strlcpy(g_recent[0], b, sizeof g_recent[0]);
  if (grow) g_nrecent++;
  recent_save();
}

void arecent_clear(void) {
  g_nrecent = 0;
  recent_save();
}

int arecent_count(void) { return g_nrecent; }
const char *arecent_at(int i) { return i >= 0 && i < g_nrecent ? g_recent[i] : ""; }

/* ---- the library -------------------------------------------------------------------- */

typedef struct LibRow {
  char *ref;                  /* heap */
  char key[192];
} LibRow;

static const int kLibMax[ALIB_COUNT] = { 500, 60, 200 };
static const char kLibTag[ALIB_COUNT] = { 'F', 'R', 'S' };

static struct {
  LibRow *rows[ALIB_COUNT];
  int n[ALIB_COUNT], cap[ALIB_COUNT];
  bool ro, dirty;
  u32 version;
  char file[FM_PATH_MAX];     /* the self test's, else "" */
} L;

static bool lib_file(char *out, size_t cap) {
  if (L.file[0]) { fm_strlcpy(out, L.file, cap); return true; }
  return config_file("audio-library.txt", out, cap);
}

static void lib_clear(void) {
  for (int l = 0; l < ALIB_COUNT; l++) {
    for (int i = 0; i < L.n[l]; i++) fm_free(L.rows[l][i].ref);
    fm_free(L.rows[l]);
    L.rows[l] = NULL;
    L.n[l] = L.cap[l] = 0;
  }
}

static int lib_find(int l, const char *key) {
  for (int i = 0; i < L.n[l]; i++)
    if (!strcmp(L.rows[l][i].key, key)) return i;
  return -1;
}

static void lib_del(int l, int i) {
  fm_free(L.rows[l][i].ref);
  memmove(&L.rows[l][i], &L.rows[l][i + 1], sizeof(LibRow) * (size_t)(L.n[l] - i - 1));
  L.n[l]--;
}

/* Inserts a line at the front (at_end: at the back, while loading). */
static void lib_put(int l, const char *ref, bool at_end) {
  char key[192];
  aref_key(ref, key, sizeof key);
  if (!key[0]) return;
  int old = lib_find(l, key);
  if (old >= 0) {
    if (at_end) return;
    lib_del(l, old);
  }
  if (L.n[l] >= kLibMax[l]) {
    if (at_end) return;
    lib_del(l, L.n[l] - 1);                 /* the oldest goes */
  }
  if (L.n[l] == L.cap[l]) {
    L.cap[l] = L.cap[l] ? L.cap[l] * 2 : 16;
    L.rows[l] = (LibRow *)fm_realloc(L.rows[l], sizeof(LibRow) * (size_t)L.cap[l]);
  }
  int at = at_end ? L.n[l] : 0;
  memmove(&L.rows[l][at + 1], &L.rows[l][at], sizeof(LibRow) * (size_t)(L.n[l] - at));
  L.rows[l][at].ref = fm_strdup(ref);
  fm_strlcpy(L.rows[l][at].key, key, sizeof L.rows[l][at].key);
  L.n[l]++;
}

static void lib_save(void) {
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX + 8];
  if (L.ro || !lib_file(path, sizeof path)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.new", path);
  FILE *f = open_write(tmp);
  if (!f) return;
  fputs("# mmcfm online audio: F favourite, R recently played, S subscription; newest first\n", f);
  bool ok = true;
  for (int l = 0; l < ALIB_COUNT; l++)
    for (int i = 0; i < L.n[l]; i++) ok = ok && fprintf(f, "%c %s\n", kLibTag[l], L.rows[l][i].ref) > 0;
  ok = fclose(f) == 0 && ok;
  /* replace the old file only once the new one is complete */
  if (ok) {
    plat_remove_file(path);
    ok = plat_rename(tmp, path) == FM_OK;
  }
  if (!ok) plat_remove_file(tmp);
}

void alib_test_file(const char *path) { fm_strlcpy(L.file, path ? path : "", sizeof L.file); }

void alib_load(bool readonly) {
  lib_clear();
  L.ro = readonly;
  L.dirty = false;
  char path[FM_PATH_MAX];
  /* read-only runs (--shot) still read it, so a screenshot shows what persisted */
  if (!lib_file(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  char *line = (char *)fm_alloc(AO_REF_MAX + 8);
  while (fgets(line, AO_REF_MAX + 8, f)) {
    chomp(line);
    if (line[1] != ' ') continue;
    for (int l = 0; l < ALIB_COUNT; l++)
      if (line[0] == kLibTag[l]) lib_put(l, line + 2, true);
  }
  fm_free(line);
  fclose(f);
  L.version++;
}

void alib_pump(void) {
  if (!L.dirty) return;
  L.dirty = false;
  lib_save();
}

void alib_shutdown(void) {
  alib_pump();
  lib_clear();
}

static void lib_changed(void) {
  L.dirty = true;
  L.version++;
  ui_redraw();
}

int alib_count(int l) { return l >= 0 && l < ALIB_COUNT ? L.n[l] : 0; }
u32 alib_version(void) { return L.version; }

bool alib_get(int l, int i, FmAsrcItem *out) {
  if (l < 0 || l >= ALIB_COUNT || i < 0 || i >= L.n[l]) return false;
  return aitem_parse(L.rows[l][i].ref, out);
}

bool alib_has_key(int l, const char *key) { return l >= 0 && l < ALIB_COUNT && key[0] && lib_find(l, key) >= 0; }

bool alib_has(int l, const FmAsrcItem *it) {
  char key[192];
  aitem_key(it, key, sizeof key);
  return alib_has_key(l, key);
}

void alib_add(int l, const FmAsrcItem *it) {
  if (l < 0 || l >= ALIB_COUNT || !it->id[0]) return;
  char *ref = (char *)fm_alloc(AO_REF_MAX);
  aitem_ref(it, ref, AO_REF_MAX);
  lib_put(l, ref, false);
  fm_free(ref);
  lib_changed();
}

void alib_remove(int l, const FmAsrcItem *it) {
  char key[192];
  aitem_key(it, key, sizeof key);
  int i = l >= 0 && l < ALIB_COUNT ? lib_find(l, key) : -1;
  if (i < 0) return;
  lib_del(l, i);
  lib_changed();
}

bool alib_toggle(int l, const FmAsrcItem *it) {
  bool had = alib_has(l, it);
  if (had) alib_remove(l, it);
  else alib_add(l, it);
  const char *name = it->title[0] ? it->title : "it";
  if (l == ALIB_SUBS)
    ui_toast(had ? "Unsubscribed from \xE2\x80\x9C%s\xE2\x80\x9D" : "Subscribed to \xE2\x80\x9C%s\xE2\x80\x9D", name);
  else if (l == ALIB_FAV)
    ui_toast(had ? "Removed \xE2\x80\x9C%s\xE2\x80\x9D from Favorites" : "Added \xE2\x80\x9C%s\xE2\x80\x9D to Favorites", name);
  return !had;
}

/* ---- the player ------------------------------------------------------------------------- */

static SDL_mutex *g_play_mx;
static FmAsrcConf g_play_conf;      /* settings for resolve(), taken on the main thread */

bool aplay_resolve(const char *ref, FmAudioStream *out, volatile int *cancel) {
  FmAsrcItem *it = (FmAsrcItem *)fm_alloc(sizeof *it);
  FmAsrcConf *c = (FmAsrcConf *)fm_alloc(sizeof *c);
  FmAsrcStream *st = (FmAsrcStream *)fm_calloc(1, sizeof *st);
  bool ok = false;
  if (!aitem_parse(ref, it)) {
    fm_strlcpy(out->err, "this item is damaged", sizeof out->err);
  } else {
    SDL_LockMutex(g_play_mx);
    *c = g_play_conf;
    SDL_UnlockMutex(g_play_mx);
    char err[200];
    err[0] = 0;
    FmErr e = asrc_stream(c, it, st, err, sizeof err, cancel);
    if (e == FM_OK && st->url[0]) {
      fm_strlcpy(out->url, st->url, sizeof out->url);
      fm_strlcpy(out->headers, st->headers, sizeof out->headers);
      out->live = st->live || it->kind == AITEM_STATION;
      ok = true;
    } else {
      fm_strlcpy(out->err, err[0] ? err : e == FM_OK ? "no stream address" : fm_err_str(e), sizeof out->err);
    }
  }
  fm_free(st);
  fm_free(c);
  fm_free(it);
  return ok;
}

static bool hook_is_fav(const char *ref) {
  char key[192];
  aref_key(ref, key, sizeof key);
  return alib_has_key(ALIB_FAV, key);
}

static void hook_fav_toggle(const char *ref) {
  FmAsrcItem *it = (FmAsrcItem *)fm_alloc(sizeof *it);
  if (aitem_parse(ref, it)) alib_toggle(ALIB_FAV, it);
  fm_free(it);
}

static void hook_played(const char *ref) {
  FmAsrcItem *it = (FmAsrcItem *)fm_alloc(sizeof *it);
  if (aitem_parse(ref, it)) alib_add(ALIB_RECENT, it);
  fm_free(it);
}

void aplay_snapshot(void) {
  if (!g_play_mx) g_play_mx = SDL_CreateMutex();
  FmAsrcConf *c = (FmAsrcConf *)fm_alloc(sizeof *c);
  asrc_conf_snapshot(c);
  SDL_LockMutex(g_play_mx);
  g_play_conf = *c;
  SDL_UnlockMutex(g_play_mx);
  fm_free(c);
}

void aplay_init(void) {
  /* no settings snapshot yet: it loads FFmpeg to check for it, which
  ** startup should not pay for; aplay_list takes one before any resolve */
  if (!g_play_mx) g_play_mx = SDL_CreateMutex();
  /* prepare: a queue restored or added to elsewhere takes the settings
  ** snapshot before its first resolve */
  FmAudioHooks h = { aplay_resolve, hook_is_fav, hook_fav_toggle, hook_played, aplay_snapshot };
  audio_set_hooks(&h);
}

void aplay_entry(const FmAsrcItem *it, FmAudioEntry *e, char *ref, char *who) {
  memset(e, 0, sizeof *e);
  aitem_ref(it, ref, AO_REF_MAX);
  /* a station's second line: its country and tags */
  if (it->kind == AITEM_STATION && it->artist[0] && it->album[0])
    fm_snprintf(who, 512, "%s \xC2\xB7 %s", it->artist, it->album);
  else fm_strlcpy(who, it->artist[0] ? it->artist : it->album, 512);
  /* url left empty: asrc_stream on the player's thread picks it (and
  ** counts a station's click, checks the codec, fetches fresh links) */
  e->url = NULL;
  e->title = it->title;
  e->artist = who;
  e->album = it->kind == AITEM_STATION ? NULL : it->album;
  e->art_url = it->art;
  e->live = it->kind == AITEM_STATION;
  e->ref = ref;
  e->dur = it->duration > 0 ? it->duration : 0;
}

void aplay_queue(const FmAsrcItem *it, bool next) {
  if (!aitem_playable(it)) return;
  char *ref = (char *)fm_alloc(AO_REF_MAX);
  char *who = (char *)fm_alloc(512);
  FmAudioEntry e;
  aplay_entry(it, &e, ref, who);
  aplay_snapshot();
  if (audio_queue_add(&e, 1, next ? AQ_NEXT : AQ_END))
    ui_toast(next ? "\xE2\x80\x9C%s\xE2\x80\x9D plays next" : "Added \xE2\x80\x9C%s\xE2\x80\x9D to the queue",
             it->title[0] ? it->title : "it");
  fm_free(ref);
  fm_free(who);
}

void aplay_pick_playlist(const FmAsrcItem *it, float x, float y) {
  if (!aitem_playable(it)) return;
  char *ref = (char *)fm_alloc(AO_REF_MAX);
  char *who = (char *)fm_alloc(512);
  FmAudioEntry e;
  aplay_entry(it, &e, ref, who);
  qui_pick_playlist(&e, 1, x, y);         /* copied */
  fm_free(ref);
  fm_free(who);
}

void aplay_list(const FmAsrcItem *items, int n, int index) {
  if (index < 0 || index >= n || !aitem_playable(&items[index])) return;
  /* the playable items around the one tapped, in list order */
  int first = index, last = index, count = 1;
  while (count < AO_PLAY_MAX && (first > 0 || last < n - 1)) {
    if (last < n - 1) { last++; count += aitem_playable(&items[last]); }
    if (count < AO_PLAY_MAX && first > 0 && (last == n - 1 || index - first < AO_PLAY_MAX / 3)) {
      first--;
      count += aitem_playable(&items[first]);
    }
  }
  FmAudioEntry *e = (FmAudioEntry *)fm_calloc((size_t)count, sizeof *e);
  char **bufs = (char **)fm_calloc((size_t)count * 2, sizeof(char *));
  int k = 0, at = 0;
  for (int i = first; i <= last && k < count; i++) {
    const FmAsrcItem *it = &items[i];
    if (!aitem_playable(it)) continue;
    if (i == index) at = k;
    char *ref = (char *)fm_alloc(AO_REF_MAX);
    char *who = (char *)fm_alloc(512);
    aplay_entry(it, &e[k], ref, who);
    bufs[k * 2] = ref;
    bufs[k * 2 + 1] = who;
    k++;
  }
  aplay_snapshot();
  if (!audio_play_entries(e, k, at)) ui_toast("No audio output on this device");
  for (int i = 0; i < k * 2; i++) fm_free(bufs[i]);
  fm_free(bufs);
  fm_free(e);
}

int aplay_state(char *key, size_t cap) {
  static char ref[AO_REF_MAX];
  int st = audio_state(ref, sizeof ref);
  if (st == AUDIO_IDLE) { if (cap) key[0] = 0; return st; }
  aref_key(ref, key, cap);
  return st;
}
