/* fphoto_task.c -- online photos: worker tasks, downloads, recent searches.
**
** Design decisions:
**   - The same shape as the video view's tasks (fonline_task.c): every
**     adapter call blocks, so each runs on its own thread in a PhTask the
**     main thread owns, polled every frame and joined once `done` is set.
**     Abandoned tasks (a new search, the lightbox moved on) are cancelled
**     and parked in a small list instead of being waited for.
**   - A lightbox fetch also decodes on its worker (img_load fits the
**     picture to the window), so the main thread only uploads a texture.
**   - Downloads are a queue with at most three running: "Download album"
**     on a hundred photos must not open a hundred connections. Batches get
**     one toast at the start and one at the end, not one per photo.
**   - Progress wakes the UI at most ten times a second.
**   - Recent searches are a ten-line text file next to the settings, like
**     the video view's online.txt; no settings keys.
*/
#include "fphoto_int.h"

/* ---- tasks ------------------------------------------------------------------ */

static bool fetch_cb(void *user, u64 done, u64 total) {
  PhTask *t = (PhTask *)user;
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
  PhTask *t = (PhTask *)u;
  const FmPsrc *s = t->src;
  switch (t->kind) {
    case PT_SEARCH:
      t->err = s && s->search ? s->search(&t->conf, t->query, t->token, &t->page, &t->cancel) : FM_ERR_UNSUPPORTED;
      if (t->err != FM_OK && !t->page.error[0])
        fm_strlcpy(t->page.error, t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err), sizeof t->page.error);
      break;
    case PT_FETCH:
    case PT_DOWNLOAD:
      /* a thin search item gets its picture link first */
      if (!t->item.full[0] && s && s->details) s->details(&t->conf, &t->item, &t->cancel);
      if (!t->item.full[0] && t->item.thumb[0]) fm_strlcpy(t->item.full, t->item.thumb, sizeof t->item.full);
      t->err = psrc_fetch(&t->conf, &t->item, t->kind == PT_DOWNLOAD ? t->dir : NULL, t->out, sizeof t->out, fetch_cb, t,
                          t->errtext, sizeof t->errtext, &t->cancel);
      if (t->err == FM_OK && t->kind == PT_FETCH && t->max_px > 0 && !t->cancel) {
        int edge = t->max_px;
        FmImgInfo fi;
        /* a pixel budget as well (zoomed in: the largest picture we keep) */
        if (t->max_pixels && img_info(t->out, &fi) && fi.w > 0 && fi.h > 0 &&
            (double)fi.w * (double)fi.h > (double)t->max_pixels) {
          double k = sqrt((double)t->max_pixels / ((double)fi.w * (double)fi.h));
          edge = FM_MIN(edge, FM_MAX(64, (int)((double)FM_MAX(fi.w, fi.h) * k)));
        }
        t->err = img_load(t->out, edge, edge, &t->img, &t->info, &t->cancel);
        if (t->err != FM_OK && t->err != FM_ERR_CANCEL)
          fm_strlcpy(t->errtext, "The full picture is in a format this app cannot show", sizeof t->errtext);
      }
      break;
    default: t->err = FM_ERR_UNSUPPORTED; break;
  }
  if (t->err != FM_OK && !t->errtext[0])
    fm_strlcpy(t->errtext, t->err == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(t->err), sizeof t->errtext);
  SDL_AtomicSet(&t->done, 1);
  app_wake();
  return 0;
}

PhTask *ptask_new(int kind, const FmPsrc *src) {
  PhTask *t = (PhTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  t->src = src;
  t->frac = -1;
  psrc_conf_snapshot(&t->conf);
  return t;
}

bool ptask_run(PhTask *t) {
  t->mx = SDL_CreateMutex();
  if (t->mx) t->thr = fm_thread_create(task_main, "photo", t);
  if (!t->thr) {
    t->err = FM_ERR_NOMEM;
    fm_strlcpy(t->errtext, "Could not start a worker thread", sizeof t->errtext);
    SDL_AtomicSet(&t->done, 1);
    return false;
  }
  return true;
}

bool ptask_done(PhTask *t) { return t && SDL_AtomicGet(&t->done) != 0; }

float ptask_frac(PhTask *t) {
  if (!t || !t->mx) return -1;
  SDL_LockMutex(t->mx);
  float f = t->frac;
  SDL_UnlockMutex(t->mx);
  return f;
}

void ptask_free(PhTask *t, bool cancel) {
  if (!t) return;
  if (cancel) t->cancel = 1;
  if (t->thr) SDL_WaitThread(t->thr, NULL);
  if (t->mx) SDL_DestroyMutex(t->mx);
  psrc_page_free(&t->page);
  img_free(&t->img);
  fm_free(t);
}

static PhTask *g_dead[32];
static int g_ndead;

void ptask_drop(PhTask *t) {
  if (!t) return;
  t->cancel = 1;
  if (g_ndead < FM_COUNT(g_dead)) g_dead[g_ndead++] = t;
  else ptask_free(t, true);                 /* full: wait for this one */
}

void ptask_reap(void) {
  for (int i = g_ndead - 1; i >= 0; i--)
    if (ptask_done(g_dead[i])) {
      ptask_free(g_dead[i], false);
      g_dead[i] = g_dead[--g_ndead];
    }
}

void ptask_shutdown(void) {
  for (int i = 0; i < g_ndead; i++) g_dead[i]->cancel = 1;
  for (int i = 0; i < g_ndead; i++) ptask_free(g_dead[i], true);
  g_ndead = 0;
}

/* ---- downloads --------------------------------------------------------------- */

#define PDL_MAX 400
#define PDL_PARALLEL 3

static PhDl *g_dl;              /* newest first */
static int g_ndl, g_capdl;
static int g_batch_ok, g_batch_fail;

int pdl_count(void) { return g_ndl; }
PhDl *pdl_at(int i) { return i >= 0 && i < g_ndl ? &g_dl[i] : NULL; }

int pdl_active(void) {
  int n = 0;
  for (int i = 0; i < g_ndl; i++) n += g_dl[i].state == PD_RUNNING || g_dl[i].state == PD_QUEUED;
  return n;
}

float pdl_frac(void) {
  float sum = 0;
  int n = 0;
  for (int i = 0; i < g_ndl; i++) {
    if (g_dl[i].state == PD_QUEUED) { n++; continue; }
    if (g_dl[i].state != PD_RUNNING) continue;
    if (g_dl[i].frac < 0) return -1;
    sum += g_dl[i].frac;
    n++;
  }
  return n ? sum / (float)n : -1;
}

void pdl_remove(int i) {
  if (i < 0 || i >= g_ndl) return;
  if (g_dl[i].task) ptask_free(g_dl[i].task, true);
  memmove(&g_dl[i], &g_dl[i + 1], sizeof g_dl[0] * (size_t)(g_ndl - i - 1));
  g_ndl--;
}

static bool make_room(void) {
  if (g_ndl < PDL_MAX) {
    if (g_ndl == g_capdl) {
      g_capdl = g_capdl ? g_capdl * 2 : 16;
      g_dl = (PhDl *)fm_realloc(g_dl, sizeof *g_dl * (size_t)g_capdl);
    }
    return true;
  }
  for (int i = g_ndl - 1; i >= 0; i--)
    if (g_dl[i].state != PD_RUNNING && g_dl[i].state != PD_QUEUED) { pdl_remove(i); return true; }
  return false;
}

static bool add_one(const PhItem *p, bool quiet) {
  for (int i = 0; i < g_ndl; i++)
    if ((g_dl[i].state == PD_RUNNING || g_dl[i].state == PD_QUEUED) && !strcmp(g_dl[i].src, p->src) &&
        !strcmp(g_dl[i].item.id, p->it.id)) {
      if (!quiet) ui_toast("Already downloading");
      return false;
    }
  if (!make_room()) { ui_toast("Too many downloads at once"); return false; }
  memmove(&g_dl[1], &g_dl[0], sizeof g_dl[0] * (size_t)g_ndl);
  g_ndl++;
  PhDl *d = &g_dl[0];
  memset(d, 0, sizeof *d);
  d->item = p->it;
  fm_strlcpy(d->src, p->src, sizeof d->src);
  d->frac = -1;
  d->quiet = quiet;
  d->state = PD_QUEUED;
  return true;
}

void pdl_start(const PhItem *p, bool quiet) {
  if (!add_one(p, quiet)) return;
  if (!quiet) ui_toast("Downloading \xE2\x80\x9C%s\xE2\x80\x9D", p->it.title[0] ? p->it.title : "photo");
  pdl_pump();
  ui_redraw();
}

void pdl_batch(const PhItem *items, int n) {
  if (n == 1) { pdl_start(&items[0], false); return; }
  int added = 0;
  for (int i = 0; i < n; i++) added += add_one(&items[i], true);
  if (added) ui_toast("Downloading %d photos", added);
  else if (n) ui_toast("Already downloading");
  pdl_pump();
  ui_redraw();
}

void pdl_cancel(int i) {
  PhDl *d = pdl_at(i);
  if (!d) return;
  if (d->task) d->task->cancel = 1;
  else if (d->state == PD_QUEUED) d->state = PD_CANCELLED;
  ui_redraw();
}

void pdl_retry(int i) {
  PhDl *d = pdl_at(i);
  if (!d || d->state == PD_RUNNING || d->state == PD_QUEUED) return;
  d->state = PD_QUEUED;
  d->err[0] = 0;
  d->frac = -1;
  d->quiet = false;
  d->tries = 1;
  pdl_pump();
}

void pdl_clear_finished(void) {
  for (int i = g_ndl - 1; i >= 0; i--)
    if (g_dl[i].state != PD_RUNNING && g_dl[i].state != PD_QUEUED) pdl_remove(i);
}

static void start_dl(PhDl *d) {
  const FmPsrc *s = psrc_find(d->src);
  PhTask *t = ptask_new(PT_DOWNLOAD, s);
  t->item = d->item;
  odl_dir(t->dir, sizeof t->dir);
  plat_mkdirs(t->dir);
  d->task = t;
  d->state = PD_RUNNING;
  if (!ptask_run(t)) {
    d->state = PD_FAILED;
    fm_strlcpy(d->err, t->errtext, sizeof d->err);
    ptask_free(t, false);
    d->task = NULL;
  }
}

void pdl_pump(void) {
  int running = 0;
  bool quiet_left = false, finished_quiet = false;
  for (int i = 0; i < g_ndl; i++) {
    PhDl *d = &g_dl[i];
    if (d->state != PD_RUNNING || !d->task) continue;
    d->frac = ptask_frac(d->task);
    if (!ptask_done(d->task)) { running++; continue; }
    PhTask *t = d->task;
    if (t->err == FM_OK) {
      d->state = PD_DONE;
      d->frac = 1;
      fm_strlcpy(d->path, t->out, sizeof d->path);
      if (d->quiet) g_batch_ok++;
      else ui_toast("Saved \xE2\x80\x9C%s\xE2\x80\x9D", d->item.title[0] ? d->item.title : fm_path_base(d->path));
    } else if (t->err == FM_ERR_CANCEL) {
      d->state = PD_CANCELLED;
    } else if (d->tries++ == 0) {
      /* once more: a network blip, or two photos
      ** with the same title and author racing for one file name */
      d->state = PD_QUEUED;
      ptask_free(t, false);
      d->task = NULL;
      continue;
    } else {
      d->state = PD_FAILED;
      fm_strlcpy(d->err, t->errtext, sizeof d->err);
      if (d->quiet) g_batch_fail++;
      else ui_toast("Download failed: %s", d->err);
    }
    finished_quiet |= d->quiet;
    ptask_free(t, false);
    d->task = NULL;
    ui_redraw();
  }
  /* the oldest queued ones start first (the list is newest first) */
  for (int i = g_ndl - 1; i >= 0 && running < PDL_PARALLEL; i--)
    if (g_dl[i].state == PD_QUEUED) {
      start_dl(&g_dl[i]);
      running += g_dl[i].state == PD_RUNNING;
    }
  for (int i = 0; i < g_ndl; i++)
    quiet_left |= g_dl[i].quiet && (g_dl[i].state == PD_RUNNING || g_dl[i].state == PD_QUEUED);
  if (finished_quiet && !quiet_left && g_batch_ok + g_batch_fail > 0) {
    if (g_batch_fail) ui_toast("Saved %d photos, %d failed", g_batch_ok, g_batch_fail);
    else ui_toast("Saved %d photos", g_batch_ok);
    g_batch_ok = g_batch_fail = 0;
  }
}

void pdl_shutdown(void) {
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) g_dl[i].task->cancel = 1;
  for (int i = 0; i < g_ndl; i++)
    if (g_dl[i].task) ptask_free(g_dl[i].task, true);
  fm_free(g_dl);
  g_dl = NULL;
  g_ndl = g_capdl = 0;
}

/* ---- recent searches ------------------------------------------------------------- */

static char g_recent[PRECENT_MAX][256];
static int g_nrecent;
static bool g_recent_ro;

static bool recent_file(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "photo-recent.txt");
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
  fputs("# mmcfm online photos: recent searches, newest first\n", f);
  for (int i = 0; i < g_nrecent; i++) fprintf(f, "q %s\n", g_recent[i]);
  fclose(f);
}

void precent_load(bool readonly) {
  g_recent_ro = readonly;
  g_nrecent = 0;
  char path[FM_PATH_MAX], line[512];
  if (readonly || !recent_file(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  while (g_nrecent < PRECENT_MAX && fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (strncmp(line, "q ", 2) == 0 && line[2]) fm_strlcpy(g_recent[g_nrecent++], line + 2, sizeof g_recent[0]);
  }
  fclose(f);
}

void precent_add(const char *q) {
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
  bool grow = at == g_nrecent && g_nrecent < PRECENT_MAX;
  if (at == g_nrecent) at = FM_MIN(g_nrecent, PRECENT_MAX - 1);
  memmove(&g_recent[1], &g_recent[0], sizeof g_recent[0] * (size_t)at);
  fm_strlcpy(g_recent[0], b, sizeof g_recent[0]);
  if (grow) g_nrecent++;
  recent_save();
}

void precent_clear(void) {
  g_nrecent = 0;
  recent_save();
}

int precent_count(void) { return g_nrecent; }
const char *precent_at(int i) { return i >= 0 && i < g_nrecent ? g_recent[i] : ""; }
