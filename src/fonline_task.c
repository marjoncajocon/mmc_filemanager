/* fonline_task.c -- online videos: worker tasks, downloads, tools, recents.
**
** Design decisions:
**   - Every adapter call (fvsrc.h) blocks, so each runs on its own thread
**     in an OnTask the main thread owns. The worker only writes the result
**     fields and then flags `done`; progress goes through a small mutex.
**     The main thread polls `done` in online_pump, joins (instant by then)
**     and frees, so no task outlives the module and nothing is detached.
**   - Progress callbacks wake the UI at most ten times a second: a yt-dlp
**     download reports far more often than that, and every wake is a frame.
**   - Downloads keep running when the view is closed; finished ones stay in
**     the tray for the session with Play and Show in folder.
**   - Tool detection (yt-dlp, a JavaScript runtime, ffmpeg) searches PATH,
**     so it is cached and refreshed only on demand (settings opened, after
**     an install), never per frame.
**   - Recent searches are a ten-line text file next to the settings, like
**     the media library's library.txt; no settings keys.
*/
#include "fonline_int.h"

/* ---- tasks ------------------------------------------------------------------ */

static bool task_cb(void *user, float frac, const char *status) {
  OnTask *t = (OnTask *)user;
  u64 now = SDL_GetTicks64();
  SDL_LockMutex(t->mx);
  t->frac = frac;
  if (status) fm_strlcpy(t->status, status, sizeof t->status);
  bool wake = now - t->last_wake >= 100;
  if (wake) t->last_wake = now;
  SDL_UnlockMutex(t->mx);
  if (wake) app_wake();
  return !t->cancel;
}

static int task_main(void *u) {
  OnTask *t = (OnTask *)u;
  const FmVsrc *s = t->src;
  switch (t->kind) {
    case OT_SEARCH:
      t->err = s && s->search ? s->search(&t->conf, t->query, t->token, &t->page, &t->cancel) : FM_ERR_UNSUPPORTED;
      if (t->err != FM_OK && !t->page.error[0])
        fm_strlcpy(t->page.error, t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err), sizeof t->page.error);
      break;
    case OT_RESOLVE:
      t->err = s && s->resolve ? s->resolve(&t->conf, &t->item, &t->stream, task_cb, t, t->errtext, sizeof t->errtext,
                                            &t->cancel)
                               : FM_ERR_UNSUPPORTED;
      break;
    case OT_DOWNLOAD:
      if (!s) t->err = FM_ERR_UNSUPPORTED;
      else if (s->download)
        t->err = s->download(&t->conf, &t->item, t->dir, t->out, sizeof t->out, task_cb, t, t->errtext,
                             sizeof t->errtext, &t->cancel);
      else
        t->err = vsrc_download(s, &t->conf, &t->item, t->dir, t->out, sizeof t->out, task_cb, t, t->errtext,
                               sizeof t->errtext, &t->cancel);
      break;
    case OT_INSTALL:
      t->err = vsrc_install_ytdlp(t->out, sizeof t->out, task_cb, t, &t->cancel);
      break;
    default: t->err = FM_ERR_UNSUPPORTED; break;
  }
  if (t->err != FM_OK && !t->errtext[0])
    fm_strlcpy(t->errtext, t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err), sizeof t->errtext);
  SDL_AtomicSet(&t->done, 1);
  app_wake();
  return 0;
}

OnTask *otask_new(int kind, const FmVsrc *src) {
  OnTask *t = (OnTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  t->src = src;
  t->frac = -1;
  t->t0 = SDL_GetTicks64();
  if (src || kind == OT_DOWNLOAD) vsrc_conf_snapshot(&t->conf);
  return t;
}

bool otask_run(OnTask *t) {
  t->mx = SDL_CreateMutex();
  if (t->mx) t->thr = fm_thread_create(task_main, "online", t);
  if (!t->thr) {
    t->err = FM_ERR_NOMEM;
    fm_strlcpy(t->errtext, "Could not start a worker thread", sizeof t->errtext);
    SDL_AtomicSet(&t->done, 1);
    return false;
  }
  return true;
}

bool otask_done(OnTask *t) { return t && SDL_AtomicGet(&t->done) != 0; }

void otask_progress(OnTask *t, float *frac, char *status, size_t cap) {
  if (!t->mx) { *frac = -1; if (cap) status[0] = 0; return; }
  SDL_LockMutex(t->mx);
  *frac = t->frac;
  fm_strlcpy(status, t->status, cap);
  SDL_UnlockMutex(t->mx);
}

void otask_free(OnTask *t, bool cancel) {
  if (!t) return;
  if (cancel) t->cancel = 1;
  if (t->thr) SDL_WaitThread(t->thr, NULL);
  if (t->mx) SDL_DestroyMutex(t->mx);
  vsrc_page_free(&t->page);
  fm_free(t);
}

/* ---- downloads ------------------------------------------------------------- */

#define DL_MAX 24
static OnDl g_dl[DL_MAX];
static int g_ndl;

void odl_dir(char *out, size_t cap) {
  if (conf.online_dl_dir[0]) { fm_strlcpy(out, conf.online_dl_dir, cap); return; }
  if (plat_place(PLACE_DOWNLOADS, out, cap)) return;
  if (!plat_place(PLACE_HOME, out, cap)) fm_strlcpy(out, ".", cap);
}

void odl_start(const FmVsrc *s, const FmVsrcItem *it) {
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].state == DL_RUNNING && g_dl[i].src == s && strcmp(g_dl[i].item.id, it->id) == 0) {
      ui_toast("Already downloading");
      return;
    }
  /* the oldest finished entry makes room; running ones are never dropped */
  if (g_ndl == DL_MAX) {
    int drop = -1;
    for (int i = g_ndl - 1; i >= 0 && drop < 0; i--)
      if (g_dl[i].state != DL_RUNNING) drop = i;
    if (drop < 0) { ui_toast("Too many downloads at once"); return; }
    odl_remove(drop);
  }
  memmove(&g_dl[1], &g_dl[0], sizeof g_dl[0] * (size_t)g_ndl);
  g_ndl++;
  OnDl *d = &g_dl[0];
  memset(d, 0, sizeof *d);
  d->item = *it;
  d->src = s;
  d->frac = -1;
  fm_strlcpy(d->status, "Starting\xE2\x80\xA6", sizeof d->status);
  OnTask *t = otask_new(OT_DOWNLOAD, s);
  t->item = *it;
  odl_dir(t->dir, sizeof t->dir);
  plat_mkdirs(t->dir);
  d->task = t;
  if (!otask_run(t)) {
    d->state = DL_FAILED;
    fm_strlcpy(d->err, t->errtext, sizeof d->err);
    otask_free(t, false);
    d->task = NULL;
  }
  ui_toast("Downloading \xE2\x80\x9C%s\xE2\x80\x9D", it->title);
}

int odl_count(void) { return g_ndl; }
OnDl *odl_at(int i) { return i >= 0 && i < g_ndl ? &g_dl[i] : NULL; }

int odl_active(void) {
  int n = 0;
  for (int i = 0; i < g_ndl; i++) n += g_dl[i].state == DL_RUNNING;
  return n;
}

float odl_frac(void) {
  float sum = 0;
  int n = 0;
  for (int i = 0; i < g_ndl; i++) {
    if (g_dl[i].state != DL_RUNNING) continue;
    if (g_dl[i].frac < 0) return -1;
    sum += g_dl[i].frac;
    n++;
  }
  return n ? sum / (float)n : -1;
}

void odl_cancel(int i) {
  OnDl *d = odl_at(i);
  if (d && d->task) d->task->cancel = 1;
}

void odl_remove(int i) {
  if (i < 0 || i >= g_ndl) return;
  if (g_dl[i].task) otask_free(g_dl[i].task, true);
  memmove(&g_dl[i], &g_dl[i + 1], sizeof g_dl[0] * (size_t)(g_ndl - i - 1));
  g_ndl--;
}

void odl_clear_finished(void) {
  for (int i = g_ndl - 1; i >= 0; i--)
    if (g_dl[i].state != DL_RUNNING) odl_remove(i);
}

void odl_pump(void) {
  for (int i = 0; i < g_ndl; i++) {
    OnDl *d = &g_dl[i];
    if (d->state != DL_RUNNING || !d->task) continue;
    otask_progress(d->task, &d->frac, d->status, sizeof d->status);
    if (!otask_done(d->task)) continue;
    OnTask *t = d->task;
    if (t->err == FM_OK) {
      d->state = DL_DONE;
      d->frac = 1;
      fm_strlcpy(d->path, t->out, sizeof d->path);
      ui_toast("Downloaded \xE2\x80\x9C%s\xE2\x80\x9D", d->item.title);
    } else if (t->err == FM_ERR_CANCEL) {
      d->state = DL_CANCELLED;
    } else {
      d->state = DL_FAILED;
      fm_strlcpy(d->err, t->errtext, sizeof d->err);
      ui_toast("Download failed: %s", d->err);
    }
    otask_free(t, false);
    d->task = NULL;
    ui_redraw();
  }
}

void odl_shutdown(void) {
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) g_dl[i].task->cancel = 1;
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) otask_free(g_dl[i].task, true);
  g_ndl = 0;
}

/* ---- tools ------------------------------------------------------------------ */

static OnTools g_tools;
static bool g_tools_ok;

const OnTools *otools(bool refresh) {
  if (g_tools_ok && !refresh) return &g_tools;
  g_tools_ok = true;
  memset(&g_tools, 0, sizeof g_tools);
  g_tools.ytdlp = vsrc_find_ytdlp(g_tools.ytdlp_path, sizeof g_tools.ytdlp_path);
  g_tools.js = vsrc_find_js_runtime(g_tools.js_rt, sizeof g_tools.js_rt);
  if (conf.ffmpeg_dir[0]) {
    char p[FM_PATH_MAX];
#ifdef FM_WIN
    const char *exe = "ffmpeg.exe";
#else
    const char *exe = "ffmpeg";
#endif
    g_tools.ffmpeg = fm_path_join(p, sizeof p, conf.ffmpeg_dir, exe) && plat_exists(p);
  }
  return &g_tools;
}

static OnTask *g_inst;

void oinst_start(void) {
  if (g_inst) return;
  g_inst = otask_new(OT_INSTALL, NULL);
  fm_strlcpy(g_inst->status, "Connecting\xE2\x80\xA6", sizeof g_inst->status);
  if (!otask_run(g_inst)) {
    ui_toast("Could not start the download");
    otask_free(g_inst, false);
    g_inst = NULL;
  }
  ui_redraw();
}

bool oinst_running(float *frac, char *status, size_t cap) {
  if (!g_inst) return false;
  float f;
  char st[160];
  otask_progress(g_inst, &f, st, sizeof st);
  if (frac) *frac = f;
  if (status) fm_strlcpy(status, st[0] ? st : "Downloading yt-dlp\xE2\x80\xA6", cap);
  return true;
}

void oinst_pump(void) {
  if (!g_inst || !otask_done(g_inst)) return;
  if (g_inst->err == FM_OK) ui_toast("yt-dlp is ready");
  else if (g_inst->err != FM_ERR_CANCEL) ui_toast("Could not get yt-dlp: %s", g_inst->errtext);
  otask_free(g_inst, false);
  g_inst = NULL;
  otools(true);
  ui_redraw();
}

void oinst_shutdown(void) {
  otask_free(g_inst, true);
  g_inst = NULL;
}

/* ---- recent searches ----------------------------------------------------------- */

static char g_recent[ORECENT_MAX][256];
static int g_nrecent;
static bool g_recent_ro;

static bool recent_file(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "online.txt");
}

static void recent_save(void) {
  if (g_recent_ro) return;
  char path[FM_PATH_MAX], dir[FM_PATH_MAX];
  if (!recent_file(path, sizeof path)) return;
  fm_strlcpy(dir, path, sizeof dir);
  fm_path_parent(dir);
  plat_mkdirs(dir);
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  fputs("# mmcfm online videos: recent searches, newest first\n", f);
  for (int i = 0; i < g_nrecent; i++) fprintf(f, "q %s\n", g_recent[i]);
  fclose(f);
}

void orecent_load(bool readonly) {
  g_recent_ro = readonly;
  g_nrecent = 0;
  char path[FM_PATH_MAX], line[512];
  if (readonly || !recent_file(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  while (g_nrecent < ORECENT_MAX && fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (strncmp(line, "q ", 2) == 0 && line[2]) fm_strlcpy(g_recent[g_nrecent++], line + 2, sizeof g_recent[0]);
  }
  fclose(f);
}

void orecent_add(const char *q) {
  char s[256];
  fm_strlcpy(s, q, sizeof s);
  /* trimmed, one line */
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
  if (at == g_nrecent) at = FM_MIN(g_nrecent, ORECENT_MAX - 1);
  else if (at == 0 && strcmp(g_recent[0], b) == 0) return;
  memmove(&g_recent[1], &g_recent[0], sizeof g_recent[0] * (size_t)at);
  fm_strlcpy(g_recent[0], b, sizeof g_recent[0]);
  if (g_nrecent < ORECENT_MAX && at == g_nrecent) g_nrecent++;
  recent_save();
}

void orecent_remove(int i) {
  if (i < 0 || i >= g_nrecent) return;
  memmove(&g_recent[i], &g_recent[i + 1], sizeof g_recent[0] * (size_t)(g_nrecent - i - 1));
  g_nrecent--;
  recent_save();
}

void orecent_clear(void) {
  g_nrecent = 0;
  recent_save();
}

int orecent_count(void) { return g_nrecent; }
const char *orecent_at(int i) { return i >= 0 && i < g_nrecent ? g_recent[i] : ""; }
