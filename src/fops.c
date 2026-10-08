/* fops.c -- the background job engine behind copy, move, delete and archives.
**
** Design decisions:
**   - One thread per job (fm_thread_create). The job owns a mutex and a
**     condition variable; the worker locks only to publish counters, to
**     check pause/cancel once per 256 KB chunk, and to hand a question to
**     the UI. The UI never blocks on a worker.
**   - Files are copied through one 256 KB buffer per job into a hidden
**     ".name.mmcfm-part" file next to the target, renamed into place when
**     complete: a cancelled or failed copy never leaves a truncated file
**     under the real name, and "Overwrite" keeps the old file until the new
**     one is whole. The mtime (and POSIX mode) of the source is kept.
**   - Moving on one volume is a rename; a folder that already exists at the
**     target is merged entry by entry. Across volumes (or when rename says
**     EXDEV) it becomes copy + delete, and a source is only deleted when
**     everything below it was copied without a skip or error.
**   - Extraction goes into a hidden staging folder inside the target, then
**     each top-level item is moved into place with the same conflict rules
**     as a copy. Archive backends therefore never overwrite anything, and
**     "Keep both" works for extracted files too.
**   - Per-file errors do not stop a job; the first message and a count are
**     reported at the end. Cancel stops at the next chunk.
*/
#include "fops.h"
#include "fapp.h"
#include "fsdl.h"

#define JOB_MAX 32
#define COPY_BUF (256 * 1024)
#define MAX_DEPTH 64

struct FmJob {
  /* spec (copied at start, read-only afterwards) */
  FmJobKind kind;
  char **srcs;
  int nsrc;
  char dst[FM_PATH_MAX];
  char arc[FM_PATH_MAX];
  char inner[FM_PATH_MAX];
  char base_dir[FM_PATH_MAX];
  char password[256];
  int arc_index;
  FmArcOpts opts;
  bool quiet, sync, demo;

  SDL_Thread *th;
  SDL_mutex *mu;
  SDL_cond *cv;

  /* shared, under mu */
  FmJobInfo info;
  bool cancel, paused, finished, reported;
  FmAsk ask;
  bool answered;
  FmConflict answer;
  bool answer_all;
  char answer_pw[256];
  bool answer_pw_ok;

  /* worker private */
  FmConflict policy;
  u8 *buf;
  u64 last_wake;
  char cur_arc[FM_PATH_MAX];
  bool pw_spec_tried, pw_cache_tried;
  u64 arc_base;              /* bytes done before the current archive */

  /* UI private: speed estimate */
  u64 t_sample, b_sample;
  float speed;
};

static FmJob *g_jobs[JOB_MAX];
static int g_njobs;
static int g_next_id = 1;
static SDL_mutex *g_pw_mu;

typedef struct PwEntry { char arc[FM_PATH_MAX]; char pw[256]; } PwEntry;
static PwEntry g_pw[4];
static int g_pw_next;

static void ops_init_once(void) {
  if (!g_pw_mu) g_pw_mu = SDL_CreateMutex();
}

/* ---- helpers ------------------------------------------------------------ */

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

bool ops_unique_name(const char *dir, const char *name, char *out, size_t cap) {
  const char *ext = fm_path_ext(name);
  char stem[512];
  size_t sl = (size_t)(ext - name);
  if (sl >= sizeof stem) sl = sizeof stem - 1;
  memcpy(stem, name, sl);
  stem[sl] = 0;
  /* "photo (2).jpg" -> continue at (3), not "photo (2) (2).jpg" */
  size_t n = strlen(stem);
  int start = 2;
  if (n > 4 && stem[n - 1] == ')') {
    size_t i = n - 2;
    while (i > 0 && stem[i] >= '0' && stem[i] <= '9') i--;
    if (i >= 1 && i < n - 2 && stem[i] == '(' && stem[i - 1] == ' ') {
      start = atoi(stem + i + 1) + 1;
      stem[i - 1] = 0;
    }
  }
  char cand[600];
  for (int k = start; k < start + 100000; k++) {
    fm_snprintf(cand, sizeof cand, "%s (%d)%s", stem, k, ext);
    if (!fm_path_join(out, cap, dir, cand)) return false;
    if (!plat_exists(out)) return true;
  }
  return false;
}

bool ops_trash_available(void) {
#if defined(FM_ANDROID) || defined(FM_WEB) || defined(FM_IOS)
  return false;
#else
  return true;
#endif
}

bool ops_valid_name(const char *name) {
  if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
  for (const char *s = name; *s; s++) {
    if (*s == '/' || (u8)*s < 0x20) return false;
#ifdef FM_WIN
    if (strchr("\\:*?\"<>|", *s)) return false;
#endif
  }
#ifdef FM_WIN
  size_t n = strlen(name);
  if (name[n - 1] == ' ' || name[n - 1] == '.') return false;
#endif
  return true;
}

void ops_password_remember(const char *arc, const char *pw) {
  ops_init_once();
  SDL_LockMutex(g_pw_mu);
  int at = -1;
  for (int i = 0; i < FM_COUNT(g_pw); i++)
    if (g_pw[i].arc[0] && same_path(g_pw[i].arc, arc)) at = i;
  if (at < 0) { at = g_pw_next; g_pw_next = (g_pw_next + 1) % FM_COUNT(g_pw); }
  fm_strlcpy(g_pw[at].arc, arc, sizeof g_pw[at].arc);
  fm_strlcpy(g_pw[at].pw, pw, sizeof g_pw[at].pw);
  SDL_UnlockMutex(g_pw_mu);
}

bool ops_password_lookup(const char *arc, char *out, int cap) {
  if (!g_pw_mu) return false;
  bool ok = false;
  SDL_LockMutex(g_pw_mu);
  for (int i = 0; i < FM_COUNT(g_pw); i++)
    if (g_pw[i].arc[0] && same_path(g_pw[i].arc, arc)) {
      fm_strlcpy(out, g_pw[i].pw, (size_t)cap);
      ok = true;
    }
  SDL_UnlockMutex(g_pw_mu);
  return ok;
}

static const char *items_word(u64 n) { return n == 1 ? "item" : "items"; }

/* ---- worker side: publishing -------------------------------------------- */

static void job_wake(FmJob *j, bool force) {
  if (j->sync) return;
  u64 now = plat_now_ms();
  if (force || now - j->last_wake >= 50) {
    j->last_wake = now;
    app_wake();
  }
}

/* Blocks while paused; true when the job is cancelled. */
static bool job_stop(FmJob *j) {
  SDL_LockMutex(j->mu);
  while (j->paused && !j->cancel) SDL_CondWait(j->cv, j->mu);
  bool c = j->cancel;
  SDL_UnlockMutex(j->mu);
  return c;
}

static void job_current(FmJob *j, const char *name) {
  SDL_LockMutex(j->mu);
  fm_strlcpy(j->info.current, name, sizeof j->info.current);
  SDL_UnlockMutex(j->mu);
  job_wake(j, false);
}

static void job_bytes(FmJob *j, u64 n) {
  SDL_LockMutex(j->mu);
  j->info.bytes_done += n;
  SDL_UnlockMutex(j->mu);
  job_wake(j, false);
}

static void job_file_done(FmJob *j) {
  SDL_LockMutex(j->mu);
  j->info.files_done++;
  SDL_UnlockMutex(j->mu);
  job_wake(j, false);
}

static void job_skip(FmJob *j, u64 bytes) {
  SDL_LockMutex(j->mu);
  j->info.nskipped++;
  j->info.bytes_done += bytes;
  SDL_UnlockMutex(j->mu);
}

static void job_error(FmJob *j, const char *path, FmErr e, const char *msg) {
  SDL_LockMutex(j->mu);
  j->info.nerrors++;
  if (j->info.err == FM_OK || j->info.err == FM_ERR_CANCEL) {
    if (e != FM_ERR_CANCEL) j->info.err = e;
    if (!j->info.errmsg[0])
      fm_snprintf(j->info.errmsg, sizeof j->info.errmsg, "%s: %s", fm_path_base(path),
                  msg ? msg : fm_err_str(e));
  }
  SDL_UnlockMutex(j->mu);
  job_wake(j, false);
}

/* Hands a question to the UI and sleeps until it is answered or cancelled. */
static bool job_ask(FmJob *j, const FmAsk *q) {
  SDL_LockMutex(j->mu);
  j->ask = *q;
  j->answered = false;
  j->info.asking = true;
  SDL_UnlockMutex(j->mu);
  job_wake(j, true);
  SDL_LockMutex(j->mu);
  while (!j->answered && !j->cancel) SDL_CondWait(j->cv, j->mu);
  bool ok = j->answered && !j->cancel;
  j->ask.kind = ASK_NONE;
  j->info.asking = false;
  SDL_UnlockMutex(j->mu);
  return ok;
}

/* ---- conflicts ---------------------------------------------------------- */

/* `dst` exists. Returns what to do; for KEEP_BOTH, dst is changed to a free
** name. A sync job (no UI) treats ASK as SKIP. */
static FmConflict resolve(FmJob *j, const char *src, const FmStat *sst, char *dst, size_t cap) {
  FmConflict c = j->policy;
  if (c == CONFLICT_ASK) {
    if (j->sync) {
      c = CONFLICT_SKIP;
    } else {
      FmAsk q;
      memset(&q, 0, sizeof q);
      q.kind = ASK_CONFLICT;
      fm_strlcpy(q.src, src, sizeof q.src);
      fm_strlcpy(q.dst, dst, sizeof q.dst);
      if (sst) { q.src_st = *sst; q.src_known = true; }
      plat_stat(dst, &q.dst_st);
      if (!job_ask(j, &q)) return CONFLICT_CANCEL;
      SDL_LockMutex(j->mu);
      c = j->answer;
      if (j->answer_all && c != CONFLICT_CANCEL) j->policy = c;
      SDL_UnlockMutex(j->mu);
    }
  }
  if (c == CONFLICT_KEEP_BOTH) {
    char dir[FM_PATH_MAX], name[512];
    fm_strlcpy(dir, dst, sizeof dir);
    fm_strlcpy(name, fm_path_base(dst), sizeof name);
    if (!fm_path_parent(dir) || !ops_unique_name(dir, name, dst, cap)) c = CONFLICT_SKIP;
  }
  if (c == CONFLICT_CANCEL) {
    SDL_LockMutex(j->mu);
    j->cancel = true;
    SDL_UnlockMutex(j->mu);
  }
  return c;
}

/* ---- scanning ----------------------------------------------------------- */

static void scan_tree(FmJob *j, const char *path, const FmStat *st, int depth, bool follow) {
  if (depth > MAX_DEPTH) return;
  bool dir = (st->flags & FM_ST_DIR) && (follow || !(st->flags & FM_ST_LINK));
  if (!dir) {
    SDL_LockMutex(j->mu);
    j->info.files_total++;
    j->info.bytes_total += st->size;
    SDL_UnlockMutex(j->mu);
    return;
  }
  if (j->kind == JOB_DELETE) {
    SDL_LockMutex(j->mu);
    j->info.files_total++;        /* the folder itself is one step too */
    SDL_UnlockMutex(j->mu);
  }
  FmDir *d = plat_dir_open(path, NULL);
  if (!d) return;
  const char *name;
  FmStat cs;
  char child[FM_PATH_MAX];
  while (plat_dir_next(d, &name, &cs)) {
    if (job_stop(j)) break;
    if (!fm_path_join(child, sizeof child, path, name)) continue;
    scan_tree(j, child, &cs, depth + 1, follow);
  }
  plat_dir_close(d);
  job_wake(j, false);
}

static void scan_sources(FmJob *j, bool follow) {
  SDL_LockMutex(j->mu);
  j->info.scanning = true;
  SDL_UnlockMutex(j->mu);
  for (int i = 0; i < j->nsrc && !job_stop(j); i++) {
    FmStat st;
    if (plat_stat(j->srcs[i], &st)) scan_tree(j, j->srcs[i], &st, 0, follow);
  }
  SDL_LockMutex(j->mu);
  j->info.scanning = false;
  SDL_UnlockMutex(j->mu);
}

/* ---- delete ------------------------------------------------------------- */

static FmErr remove_link(const char *path) {
  FmErr e = plat_remove_file(path);
  if (e != FM_OK && plat_remove_dir(path) == FM_OK) e = FM_OK;
  return e;
}

/* Removes path and everything below; links are removed, never followed. */
static bool delete_any(FmJob *j, const char *path, int depth) {
  if (job_stop(j)) return false;
  FmStat st;
  if (!plat_stat(path, &st)) {
    if (!plat_exists(path)) return true;
    st.flags = 0;
  }
  if (depth <= 1) job_current(j, fm_path_base(path));
  bool ok = true;
  FmErr e;
  if (st.flags & (FM_ST_LINK | FM_ST_BROKEN)) {
    e = remove_link(path);
  } else if (st.flags & FM_ST_DIR) {
    if (depth > MAX_DEPTH) {
      job_error(j, path, FM_ERR_IO, "Folders nested too deeply");
      return false;
    }
    FmErr oe;
    FmDir *d = plat_dir_open(path, &oe);
    if (d) {
      const char *name;
      char child[FM_PATH_MAX];
      while (plat_dir_next(d, &name, NULL)) {
        if (!fm_path_join(child, sizeof child, path, name)) { ok = false; continue; }
        if (!delete_any(j, child, depth + 1)) ok = false;
        if (j->cancel) break;
      }
      plat_dir_close(d);
    }
    if (j->cancel) return false;
    e = plat_remove_dir(path);
  } else {
    e = plat_remove_file(path);
  }
  if (e != FM_OK) {
    job_error(j, path, e, NULL);
    ok = false;
  }
  job_file_done(j);
  return ok;
}

/* Silent recursive removal for staging folders and partial output. */
static void remove_tree(const char *path) {
  FmStat st;
  if (!plat_stat(path, &st)) {
    remove_link(path);
    return;
  }
  if ((st.flags & FM_ST_DIR) && !(st.flags & FM_ST_LINK)) {
    FmDir *d = plat_dir_open(path, NULL);
    if (d) {
      const char *name;
      char child[FM_PATH_MAX];
      while (plat_dir_next(d, &name, NULL))
        if (fm_path_join(child, sizeof child, path, name)) remove_tree(child);
      plat_dir_close(d);
    }
    plat_remove_dir(path);
  } else {
    remove_link(path);
  }
}

/* ---- copy --------------------------------------------------------------- */

/* Streams src into the final path `dst` through a hidden part file. */
static FmErr copy_file(FmJob *j, const char *src, const FmStat *st, const char *dst, bool replace) {
  char dir[FM_PATH_MAX], part[FM_PATH_MAX], pname[600];
  fm_strlcpy(dir, dst, sizeof dir);
  fm_path_parent(dir);
  fm_snprintf(pname, sizeof pname, ".%s.mmcfm-part", fm_path_base(dst));
  if (!fm_path_join(part, sizeof part, dir, pname) || strlen(pname) > 250) {
    fm_strlcpy(part, dst, sizeof part);     /* name too long for a prefix: write in place */
    if (replace) plat_remove_file(dst);
    replace = false;
  }
  if (!j->buf) j->buf = (u8 *)fm_alloc(COPY_BUF);
  FILE *in = fm_fopen(src, "rb");
  if (!in) return plat_exists(src) ? FM_ERR_ACCESS : FM_ERR_NOT_FOUND;
  FILE *out = fm_fopen(part, "wb");
  if (!out) {
    fclose(in);
    return plat_is_dir(dir) ? FM_ERR_ACCESS : FM_ERR_NOT_FOUND;
  }
  FmErr err = FM_OK;
  for (;;) {
    if (job_stop(j)) { err = FM_ERR_CANCEL; break; }
    size_t n = fread(j->buf, 1, COPY_BUF, in);
    if (n == 0) {
      if (ferror(in)) err = FM_ERR_IO;
      break;
    }
    if (fwrite(j->buf, 1, n, out) != n) {
      u64 tot = 0, fr = 0;
      err = (plat_disk_space(dir, &tot, &fr) && fr < n) ? FM_ERR_FULL : FM_ERR_IO;
      break;
    }
    job_bytes(j, n);
  }
  fclose(in);
  if (fclose(out) != 0 && err == FM_OK) err = FM_ERR_FULL;
  if (err != FM_OK) {
    plat_remove_file(part);
    return err;
  }
  plat_set_mtime(part, st->mtime);
  if (st->mode) plat_set_mode(part, st->mode);
  if (strcmp(part, dst) != 0) {
    if (replace) {
      FmErr re = plat_remove_file(dst);
      if (re != FM_OK && re != FM_ERR_NOT_FOUND) {
        plat_remove_file(part);
        return re;
      }
    }
    FmErr re = plat_rename(part, dst);
    if (re != FM_OK) {
      plat_remove_file(part);
      return re;
    }
  }
  return FM_OK;
}

/* Copies src (file or folder) to the exact path dst. *clean becomes false
** when anything below was skipped or failed (a move must keep the source). */
static void copy_any(FmJob *j, const char *src, const FmStat *st, const char *dst_in, int depth,
                     bool *clean) {
  if (job_stop(j)) { *clean = false; return; }
  char dst[FM_PATH_MAX];
  fm_strlcpy(dst, dst_in, sizeof dst);
  if (depth > MAX_DEPTH) {
    job_error(j, src, FM_ERR_IO, "Folders nested too deeply");
    *clean = false;
    return;
  }
  if (st->flags & FM_ST_BROKEN) {
    job_error(j, src, FM_ERR_NOT_FOUND, "Broken link");
    *clean = false;
    return;
  }
  job_current(j, fm_path_base(src));
  FmStat dst_st;
  bool exists = plat_stat(dst, &dst_st) || plat_exists(dst);

  if (st->flags & FM_ST_DIR) {
    if (exists && !(dst_st.flags & FM_ST_DIR)) {
      FmConflict c = resolve(j, src, st, dst, sizeof dst);
      if (c == CONFLICT_SKIP || c == CONFLICT_CANCEL) {
        if (c == CONFLICT_SKIP) job_skip(j, 0);
        *clean = false;
        return;
      }
      if (c == CONFLICT_OVERWRITE) {
        FmErr e = remove_link(dst);
        if (e != FM_OK) { job_error(j, dst, e, NULL); *clean = false; return; }
      }
      exists = false;
    }
    if (!exists) {
      FmErr e = plat_mkdir(dst);
      if (e != FM_OK) { job_error(j, dst, e, NULL); *clean = false; return; }
    }
    FmErr oe;
    FmDir *d = plat_dir_open(src, &oe);
    if (!d) { job_error(j, src, oe, NULL); *clean = false; return; }
    const char *name;
    FmStat cs;
    char cs_src[FM_PATH_MAX], cs_dst[FM_PATH_MAX];
    while (plat_dir_next(d, &name, &cs)) {
      if (!fm_path_join(cs_src, sizeof cs_src, src, name) ||
          !fm_path_join(cs_dst, sizeof cs_dst, dst, name)) {
        job_error(j, name, FM_ERR_IO, "Path too long");
        *clean = false;
        continue;
      }
      copy_any(j, cs_src, &cs, cs_dst, depth + 1, clean);
      if (j->cancel) break;
    }
    plat_dir_close(d);
    plat_set_mtime(dst, st->mtime);
    return;
  }

  bool replace = false;
  if (exists) {
    if (same_path(src, dst)) {           /* copying a file onto itself */
      char dir[FM_PATH_MAX];
      fm_strlcpy(dir, dst, sizeof dir);
      fm_path_parent(dir);
      if (!ops_unique_name(dir, fm_path_base(src), dst, sizeof dst)) { *clean = false; return; }
    } else {
      FmConflict c = resolve(j, src, st, dst, sizeof dst);
      if (c == CONFLICT_SKIP || c == CONFLICT_CANCEL) {
        if (c == CONFLICT_SKIP) job_skip(j, st->size);
        *clean = false;
        return;
      }
      if (c == CONFLICT_OVERWRITE) {
        if (dst_st.flags & FM_ST_DIR) {
          job_error(j, dst, FM_ERR_EXISTS, "A folder with this name exists");
          job_bytes(j, st->size);
          *clean = false;
          return;
        }
        replace = true;
      }
    }
  }
  u64 before;
  SDL_LockMutex(j->mu);
  before = j->info.bytes_done;
  SDL_UnlockMutex(j->mu);
  FmErr e = copy_file(j, src, st, dst, replace);
  if (e != FM_OK) {
    *clean = false;
    if (e != FM_ERR_CANCEL) {
      job_error(j, src, e, NULL);
      /* keep the progress bar honest: count the rest of this file as done */
      SDL_LockMutex(j->mu);
      u64 done = j->info.bytes_done - before;
      if (done < st->size) j->info.bytes_done += st->size - done;
      SDL_UnlockMutex(j->mu);
    }
    return;
  }
  job_file_done(j);
}

/* ---- move --------------------------------------------------------------- */

static void move_any(FmJob *j, const char *src, const FmStat *st, const char *dst_in, int depth,
                     bool *clean);

/* Copy, then delete the source if the copy was complete. */
static void move_by_copy(FmJob *j, const char *src, const FmStat *st, const char *dst, int depth,
                         bool *clean) {
  bool ok = true;
  copy_any(j, src, st, dst, depth, &ok);
  if (ok && !j->cancel) {
    u64 fd;
    SDL_LockMutex(j->mu);
    fd = j->info.files_done;
    SDL_UnlockMutex(j->mu);
    if (!delete_any(j, src, depth + 1)) ok = false;
    /* the files were counted by the copy; the delete steps are not progress */
    SDL_LockMutex(j->mu);
    j->info.files_done = fd;
    SDL_UnlockMutex(j->mu);
  }
  if (!ok) *clean = false;
}

static void move_any(FmJob *j, const char *src, const FmStat *st, const char *dst_in, int depth,
                     bool *clean) {
  if (job_stop(j)) { *clean = false; return; }
  char dst[FM_PATH_MAX];
  fm_strlcpy(dst, dst_in, sizeof dst);
  if (depth <= 1) job_current(j, fm_path_base(src));
  FmErr e = plat_rename(src, dst);
  if (e == FM_OK) {
    SDL_LockMutex(j->mu);
    j->info.files_done++;
    j->info.bytes_done += st->size;
    SDL_UnlockMutex(j->mu);
    job_wake(j, false);
    return;
  }
  if (e == FM_ERR_EXISTS) {
    FmStat ds;
    bool have = plat_stat(dst, &ds);
    bool src_dir = (st->flags & FM_ST_DIR) && !(st->flags & FM_ST_LINK);
    if (have && src_dir && (ds.flags & FM_ST_DIR) && depth < MAX_DEPTH) {
      /* merge folders: move every child, then drop the emptied source */
      FmErr oe;
      FmDir *d = plat_dir_open(src, &oe);
      if (!d) { job_error(j, src, oe, NULL); *clean = false; return; }
      const char *name;
      FmStat cs;
      char a[FM_PATH_MAX], b[FM_PATH_MAX];
      bool sub_clean = true;
      /* collect first: renaming while iterating may confuse readdir */
      FmArena ar;
      arena_init(&ar, 16 * 1024);
      char **names = NULL;
      FmStat *stats = NULL;
      int n = 0, cap = 0;
      while (plat_dir_next(d, &name, &cs)) {
        if (n == cap) {
          cap = cap ? cap * 2 : 32;
          names = (char **)fm_realloc(names, (size_t)cap * sizeof *names);
          stats = (FmStat *)fm_realloc(stats, (size_t)cap * sizeof *stats);
        }
        names[n] = arena_strdup(&ar, name);
        stats[n] = cs;
        n++;
      }
      plat_dir_close(d);
      for (int i = 0; i < n && !j->cancel; i++) {
        if (!fm_path_join(a, sizeof a, src, names[i]) || !fm_path_join(b, sizeof b, dst, names[i])) {
          sub_clean = false;
          continue;
        }
        move_any(j, a, &stats[i], b, depth + 1, &sub_clean);
      }
      fm_free(names);
      fm_free(stats);
      arena_free(&ar);
      if (sub_clean && !j->cancel) plat_remove_dir(src);
      else *clean = false;
      return;
    }
    FmConflict c = resolve(j, src, st, dst, sizeof dst);
    if (c == CONFLICT_SKIP || c == CONFLICT_CANCEL) {
      if (c == CONFLICT_SKIP) job_skip(j, st->size);
      *clean = false;
      return;
    }
    if (c == CONFLICT_OVERWRITE) {
      if (have && (ds.flags & FM_ST_DIR) && !(ds.flags & FM_ST_LINK)) {
        job_error(j, dst, FM_ERR_EXISTS, "A folder with this name exists");
        *clean = false;
        return;
      }
      FmErr re = remove_link(dst);
      if (re != FM_OK) { job_error(j, dst, re, NULL); *clean = false; return; }
    }
    e = plat_rename(src, dst);
    if (e == FM_OK) {
      SDL_LockMutex(j->mu);
      j->info.files_done++;
      j->info.bytes_done += st->size;
      SDL_UnlockMutex(j->mu);
      return;
    }
  }
  if (e == FM_ERR_UNSUPPORTED || e == FM_ERR_IO) {
    move_by_copy(j, src, st, dst, depth, clean);
    return;
  }
  job_error(j, src, e, NULL);
  *clean = false;
}

/* ---- copy / move jobs --------------------------------------------------- */

static void parent_of(const char *path, char *out, size_t cap) {
  fm_strlcpy(out, path, cap);
  fm_path_parent(out);
}

static void run_copy_move(FmJob *j) {
  bool move = j->kind == JOB_MOVE;
  bool rename_ok = move && j->nsrc > 0 && plat_same_volume(j->srcs[0], j->dst);
  if (rename_ok) {
    SDL_LockMutex(j->mu);
    j->info.files_total = (u64)j->nsrc;
    SDL_UnlockMutex(j->mu);
  } else {
    scan_sources(j, true);
  }
  for (int i = 0; i < j->nsrc && !job_stop(j); i++) {
    const char *src = j->srcs[i];
    FmStat st;
    if (!plat_stat(src, &st)) {
      job_error(j, src, FM_ERR_NOT_FOUND, NULL);
      continue;
    }
    char dst[FM_PATH_MAX];
    if (!fm_path_join(dst, sizeof dst, j->dst, fm_path_base(src))) {
      job_error(j, src, FM_ERR_IO, "Path too long");
      continue;
    }
    bool is_dir = (st.flags & FM_ST_DIR) != 0;
    if (same_path(src, dst)) {
      if (move) {                        /* already there */
        job_file_done(j);
        continue;
      }
      if (is_dir) {
        /* duplicate a folder in place: "name (2)" */
        if (!ops_unique_name(j->dst, fm_path_base(src), dst, sizeof dst)) continue;
      }
    }
    if (is_dir && !(st.flags & FM_ST_LINK) && fm_path_is_inside(dst, src)) {
      job_error(j, src, FM_ERR_UNSUPPORTED,
                move ? "Can't move a folder into itself" : "Can't copy a folder into itself");
      continue;
    }
    bool clean = true;
    if (move && rename_ok) move_any(j, src, &st, dst, 0, &clean);
    else if (move) move_by_copy(j, src, &st, dst, 0, &clean);
    else copy_any(j, src, &st, dst, 0, &clean);
  }
  fm_strlcpy(j->info.touched[0], j->dst, FM_PATH_MAX);
  if (move && j->nsrc > 0) parent_of(j->srcs[0], j->info.touched[1], FM_PATH_MAX);
}

static void run_delete(FmJob *j) {
  if (j->kind == JOB_TRASH) {
    SDL_LockMutex(j->mu);
    j->info.files_total = (u64)j->nsrc;
    SDL_UnlockMutex(j->mu);
    for (int i = 0; i < j->nsrc && !job_stop(j); i++) {
      job_current(j, fm_path_base(j->srcs[i]));
      FmErr e = plat_trash(j->srcs[i]);
      if (e == FM_ERR_UNSUPPORTED) job_error(j, j->srcs[i], e, "The trash is not available here");
      else if (e != FM_OK) job_error(j, j->srcs[i], e, NULL);
      job_file_done(j);
    }
  } else {
    scan_sources(j, false);
    for (int i = 0; i < j->nsrc && !job_stop(j); i++) delete_any(j, j->srcs[i], 0);
  }
  if (j->nsrc > 0) parent_of(j->srcs[0], j->info.touched[0], FM_PATH_MAX);
}

/* ---- archive callbacks -------------------------------------------------- */

static bool cb_progress(void *ud, u64 done, u64 total) {
  FmJob *j = (FmJob *)ud;
  SDL_LockMutex(j->mu);
  j->info.bytes_done = j->arc_base + done;
  if (j->arc_base + total > j->info.bytes_total) j->info.bytes_total = j->arc_base + total;
  SDL_UnlockMutex(j->mu);
  job_wake(j, false);
  return !job_stop(j);
}

static void cb_entry(void *ud, const char *name) {
  FmJob *j = (FmJob *)ud;
  job_current(j, fm_path_base(name));
}

static bool cb_password(void *ud, char *buf, int cap, bool retry) {
  FmJob *j = (FmJob *)ud;
  if (!j->pw_spec_tried && j->password[0]) {
    j->pw_spec_tried = true;
    fm_strlcpy(buf, j->password, (size_t)cap);
    return true;
  }
  if (!j->pw_cache_tried) {
    j->pw_cache_tried = true;
    char pw[256];
    if (ops_password_lookup(j->cur_arc, pw, sizeof pw)) {
      fm_strlcpy(buf, pw, (size_t)cap);
      return true;
    }
  }
  if (j->sync) return false;
  FmAsk q;
  memset(&q, 0, sizeof q);
  q.kind = ASK_PASSWORD;
  fm_strlcpy(q.src, j->cur_arc, sizeof q.src);
  q.retry = retry || j->pw_spec_tried;
  if (!job_ask(j, &q)) return false;
  SDL_LockMutex(j->mu);
  bool ok = j->answer_pw_ok;
  if (ok) fm_strlcpy(buf, j->answer_pw, (size_t)cap);
  memset(j->answer_pw, 0, sizeof j->answer_pw);
  SDL_UnlockMutex(j->mu);
  if (ok) ops_password_remember(j->cur_arc, buf);
  else {
    SDL_LockMutex(j->mu);
    j->cancel = true;
    SDL_UnlockMutex(j->mu);
  }
  return ok;
}

static FmArcCb job_cb(FmJob *j) {
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = j;
  cb.progress = cb_progress;
  cb.password = cb_password;
  cb.entry = cb_entry;
  return cb;
}

static FmErr job_arc_open(FmJob *j, const char *path, FmArc **out) {
  fm_strlcpy(j->cur_arc, path, sizeof j->cur_arc);
  j->pw_cache_tried = false;
  FmArcCb cb = job_cb(j);
  return arc_open(path, &cb, out);
}

/* ---- extract ------------------------------------------------------------ */

/* Moves every item of `staging` into j->dst with the copy conflict rules. */
static void unstage(FmJob *j, const char *staging) {
  FmDir *d = plat_dir_open(staging, NULL);
  if (!d) return;
  FmArena ar;
  arena_init(&ar, 8 * 1024);
  char **names = NULL;
  int n = 0, cap = 0;
  const char *name;
  while (plat_dir_next(d, &name, NULL)) {
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      names = (char **)fm_realloc(names, (size_t)cap * sizeof *names);
    }
    names[n++] = arena_strdup(&ar, name);
  }
  plat_dir_close(d);
  u64 fd, fb;
  SDL_LockMutex(j->mu);
  fd = j->info.files_done;
  fb = j->info.bytes_done;
  SDL_UnlockMutex(j->mu);
  for (int i = 0; i < n && !j->cancel; i++) {
    char a[FM_PATH_MAX], b[FM_PATH_MAX];
    FmStat st;
    if (!fm_path_join(a, sizeof a, staging, names[i]) ||
        !fm_path_join(b, sizeof b, j->dst, names[i]) || !plat_stat(a, &st))
      continue;
    bool clean = true;
    move_any(j, a, &st, b, 0, &clean);
  }
  SDL_LockMutex(j->mu);
  j->info.files_done = fd;
  j->info.bytes_done = fb;
  SDL_UnlockMutex(j->mu);
  fm_free(names);
  arena_free(&ar);
}

/* Is entry path p the named item under prefix (itself or below it)? */
static bool entry_selected(const char *p, const char *prefix, size_t pl, char **names, int n) {
  if (strncmp(p, prefix, pl) != 0) return false;
  const char *rest = p + pl;
  for (int i = 0; i < n; i++) {
    size_t nl = strlen(names[i]);
    if (strncmp(rest, names[i], nl) == 0 && (rest[nl] == 0 || rest[nl] == '/')) return true;
  }
  return false;
}

static void extract_one_archive(FmJob *j, const char *arc_path, const char *prefix, char **names,
                                int nnames) {
  FmArc *a = NULL;
  job_current(j, fm_path_base(arc_path));
  FmErr e = job_arc_open(j, arc_path, &a);
  if (e != FM_OK) {
    if (!j->cancel) job_error(j, arc_path, e, NULL);
    return;
  }
  u8 *sel = NULL;
  int cnt = arc_count(a);
  size_t pl = strlen(prefix);
  if (nnames > 0 || pl > 0) {
    sel = (u8 *)fm_calloc((size_t)(cnt > 0 ? cnt : 1), 1);
    u64 total = 0;
    for (int i = 0; i < cnt; i++) {
      const FmArcEntry *en = arc_entry(a, i);
      if (!en) continue;
      bool s = nnames > 0 ? entry_selected(en->path, prefix, pl, names, nnames)
                          : strncmp(en->path, prefix, pl) == 0;
      sel[i] = s ? 1 : 0;
      if (s) total += en->size;
    }
    SDL_LockMutex(j->mu);
    j->info.bytes_total += total;
    SDL_UnlockMutex(j->mu);
  } else {
    SDL_LockMutex(j->mu);
    j->info.bytes_total += arc_total_size(a);
    SDL_UnlockMutex(j->mu);
  }
  char staging[FM_PATH_MAX], sname[64];
  u8 rnd[6];
  plat_random(rnd, sizeof rnd);
  fm_snprintf(sname, sizeof sname, ".mmcfm-extract-%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1],
              rnd[2], rnd[3], rnd[4], rnd[5]);
  if (!fm_path_join(staging, sizeof staging, j->dst, sname) || plat_mkdirs(staging) != FM_OK) {
    job_error(j, j->dst, FM_ERR_ACCESS, NULL);
    fm_free(sel);
    arc_close(a);
    return;
  }
  SDL_LockMutex(j->mu);
  j->arc_base = j->info.bytes_done;
  SDL_UnlockMutex(j->mu);
  FmArcCb cb = job_cb(j);
  e = arc_extract(a, sel, staging, prefix, &cb);
  arc_close(a);
  fm_free(sel);
  if (e == FM_OK || (e != FM_ERR_CANCEL && !j->cancel)) unstage(j, staging);
  if (e != FM_OK && !j->cancel) job_error(j, arc_path, e, NULL);
  remove_tree(staging);
  SDL_LockMutex(j->mu);
  j->info.files_done++;
  SDL_UnlockMutex(j->mu);
}

static void run_extract(FmJob *j) {
  if (j->arc[0]) {
    SDL_LockMutex(j->mu);
    j->info.files_total = 1;
    SDL_UnlockMutex(j->mu);
    extract_one_archive(j, j->arc, j->inner, j->srcs, j->nsrc);
  } else {
    SDL_LockMutex(j->mu);
    j->info.files_total = (u64)j->nsrc;
    SDL_UnlockMutex(j->mu);
    for (int i = 0; i < j->nsrc && !job_stop(j); i++) extract_one_archive(j, j->srcs[i], "", NULL, 0);
  }
  fm_strlcpy(j->info.touched[0], j->dst, FM_PATH_MAX);
}

/* ---- compress ----------------------------------------------------------- */

static void run_compress(FmJob *j) {
  char out[FM_PATH_MAX];
  fm_strlcpy(out, j->dst, sizeof out);
  parent_of(out, j->info.touched[0], FM_PATH_MAX);
  if (plat_exists(out)) {
    FmConflict c = resolve(j, out, NULL, out, sizeof out);
    if (c == CONFLICT_SKIP) { job_skip(j, 0); return; }
    if (c == CONFLICT_CANCEL) return;
    if (c == CONFLICT_OVERWRITE) {
      if (plat_is_dir(out)) {
        job_error(j, out, FM_ERR_EXISTS, "A folder with this name exists");
        return;
      }
      FmErr e = plat_remove_file(out);
      if (e != FM_OK) { job_error(j, out, e, NULL); return; }
    }
  }
  fm_strlcpy(j->cur_arc, out, sizeof j->cur_arc);
  scan_sources(j, true);
  SDL_LockMutex(j->mu);
  j->arc_base = 0;
  fm_strlcpy(j->info.result, out, FM_PATH_MAX);
  SDL_UnlockMutex(j->mu);
  FmArcCb cb = job_cb(j);
  FmArcOpts o = j->opts;
  o.password = j->password[0] ? j->password : NULL;
  FmErr e = arc_create(out, (const char *const *)j->srcs, j->nsrc, j->base_dir, &o, &cb);
  if (e != FM_OK) {
    plat_remove_file(out);
    if (e != FM_ERR_CANCEL && !j->cancel) job_error(j, out, e, NULL);
    j->info.result[0] = 0;
  } else {
    SDL_LockMutex(j->mu);
    j->info.files_done = j->info.files_total;
    SDL_UnlockMutex(j->mu);
  }
}

/* ---- size and open ------------------------------------------------------ */

static void size_tree(FmJob *j, const char *path, const FmStat *st, int depth) {
  if (depth > MAX_DEPTH || job_stop(j)) return;
  bool dir = (st->flags & FM_ST_DIR) && !(st->flags & FM_ST_LINK);
  SDL_LockMutex(j->mu);
  if (dir) j->info.dirs++;
  else { j->info.files_done++; j->info.bytes_done += st->size; }
  SDL_UnlockMutex(j->mu);
  if (!dir) return;
  FmDir *d = plat_dir_open(path, NULL);
  if (!d) return;
  const char *name;
  FmStat cs;
  char child[FM_PATH_MAX];
  while (plat_dir_next(d, &name, &cs)) {
    if (!fm_path_join(child, sizeof child, path, name)) continue;
    size_tree(j, child, &cs, depth + 1);
    if (j->cancel) break;
  }
  plat_dir_close(d);
  job_wake(j, false);
}

static void run_size(FmJob *j) {
  for (int i = 0; i < j->nsrc && !job_stop(j); i++) {
    FmStat st;
    if (!plat_stat(j->srcs[i], &st)) continue;
    /* the top folders themselves are not counted as "folders inside" */
    if ((st.flags & FM_ST_DIR) && !(st.flags & FM_ST_LINK)) {
      size_tree(j, j->srcs[i], &st, 0);
      SDL_LockMutex(j->mu);
      if (j->info.dirs) j->info.dirs--;
      SDL_UnlockMutex(j->mu);
    } else {
      size_tree(j, j->srcs[i], &st, 0);
    }
  }
  if (j->nsrc == 1 && !plat_is_dir(j->srcs[0])) {
    FmArcFmt f = arc_detect(j->srcs[0]);
    if (f != ARC_NONE) {
      FmArc *a = NULL;
      FmErr e = arc_open(j->srcs[0], NULL, &a);
      SDL_LockMutex(j->mu);
      j->info.arc_fmt = (int)f;
      if (e == FM_OK && a) {
        j->info.arc_fmt = (int)arc_format(a);
        j->info.arc_entries = arc_count(a);
        j->info.arc_encrypted = arc_has_encrypted(a);
        j->info.arc_unpacked = arc_total_size(a);
      } else if (e == FM_ERR_PASSWORD) {
        j->info.arc_encrypted = true;
        j->info.arc_entries = -1;
      } else {
        j->info.arc_entries = -1;
      }
      SDL_UnlockMutex(j->mu);
      if (a) arc_close(a);
    }
  }
}

static void run_open(FmJob *j) {
  FmArc *a = NULL;
  FmErr e = job_arc_open(j, j->arc, &a);
  if (e != FM_OK) {
    if (!j->cancel) job_error(j, j->arc, e, NULL);
    return;
  }
  const FmArcEntry *en = arc_entry(a, j->arc_index);
  if (!en) {
    arc_close(a);
    job_error(j, j->arc, FM_ERR_NOT_FOUND, NULL);
    return;
  }
  SDL_LockMutex(j->mu);
  j->info.bytes_total = en->size;
  j->info.files_total = 1;
  SDL_UnlockMutex(j->mu);
  job_current(j, fm_path_base(en->path));
  char dir[FM_PATH_MAX];
  parent_of(j->dst, dir, sizeof dir);
  plat_mkdirs(dir);
  FmArcCb cb = job_cb(j);
  e = arc_extract_one(a, j->arc_index, j->dst, &cb);
  arc_close(a);
  if (e != FM_OK) {
    plat_remove_file(j->dst);
    if (e != FM_ERR_CANCEL && !j->cancel) job_error(j, j->dst, e, NULL);
    return;
  }
  SDL_LockMutex(j->mu);
  j->info.files_done = 1;
  fm_strlcpy(j->info.result, j->dst, FM_PATH_MAX);
  SDL_UnlockMutex(j->mu);
}

/* ---- job lifecycle ------------------------------------------------------ */

static void run_job(FmJob *j) {
  switch (j->kind) {
    case JOB_COPY:
    case JOB_MOVE:
      if (j->arc[0]) run_extract(j);       /* out of an archive: extract the selection */
      else run_copy_move(j);
      break;
    case JOB_DELETE:
    case JOB_TRASH: run_delete(j); break;
    case JOB_EXTRACT: run_extract(j); break;
    case JOB_COMPRESS: run_compress(j); break;
    case JOB_SIZE: run_size(j); break;
    case JOB_OPEN: run_open(j); break;
  }
  SDL_LockMutex(j->mu);
  if (j->cancel) j->info.err = FM_ERR_CANCEL;
  j->info.done = true;
  j->info.current[0] = 0;
  j->finished = true;
  SDL_UnlockMutex(j->mu);
  job_wake(j, true);
}

static int SDLCALL job_thread(void *ud) {
  run_job((FmJob *)ud);
  return 0;
}

/* Where the items go, for titles: the last path component. */
static const char *short_dir(const char *path) {
  const char *b = fm_path_base(path);
  return b[0] ? b : path;
}

static void make_title(FmJob *j) {
  char *t = j->info.title;
  size_t cap = sizeof j->info.title;
  char what[300];
  if (j->nsrc == 1) fm_snprintf(what, sizeof what, "\"%s\"", fm_path_base(j->srcs[0]));
  else fm_snprintf(what, sizeof what, "%d %s", j->nsrc, items_word((u64)j->nsrc));
  switch (j->kind) {
    case JOB_COPY:
      if (j->arc[0] && j->nsrc == 0) fm_snprintf(t, cap, "Extracting to %s", short_dir(j->dst));
      else fm_snprintf(t, cap, "Copying %s to %s", what, short_dir(j->dst));
      break;
    case JOB_MOVE: fm_snprintf(t, cap, "Moving %s to %s", what, short_dir(j->dst)); break;
    case JOB_DELETE: fm_snprintf(t, cap, "Deleting %s", what); break;
    case JOB_TRASH: fm_snprintf(t, cap, "Moving %s to the trash", what); break;
    case JOB_EXTRACT:
      if (j->arc[0]) fm_snprintf(t, cap, "Extracting from %s", fm_path_base(j->arc));
      else fm_snprintf(t, cap, "Extracting %s", what);
      break;
    case JOB_COMPRESS: fm_snprintf(t, cap, "Compressing into %s", fm_path_base(j->dst)); break;
    case JOB_SIZE: fm_snprintf(t, cap, "Calculating the size of %s", what); break;
    case JOB_OPEN: fm_snprintf(t, cap, "Opening %s", fm_path_base(j->dst)); break;
  }
}

static FmJob *job_new(const FmJobSpec *s, bool sync) {
  ops_init_once();
  FmJob *j = (FmJob *)fm_calloc(1, sizeof *j);
  j->kind = s->kind;
  j->nsrc = s->nsrc > 0 ? s->nsrc : 0;
  j->srcs = (char **)fm_calloc((size_t)(j->nsrc ? j->nsrc : 1), sizeof(char *));
  for (int i = 0; i < j->nsrc; i++) j->srcs[i] = fm_strdup(s->srcs[i]);
  if (s->dst) fm_strlcpy(j->dst, s->dst, sizeof j->dst);
  if (s->arc) fm_strlcpy(j->arc, s->arc, sizeof j->arc);
  if (s->arc_inner) fm_strlcpy(j->inner, s->arc_inner, sizeof j->inner);
  if (s->base_dir) fm_strlcpy(j->base_dir, s->base_dir, sizeof j->base_dir);
  if (s->password) fm_strlcpy(j->password, s->password, sizeof j->password);
  if (s->kind == JOB_COMPRESS && s->arc_opts.password)
    fm_strlcpy(j->password, s->arc_opts.password, sizeof j->password);
  j->arc_index = s->arc_index;
  j->opts = s->arc_opts;
  j->opts.password = NULL;
  j->quiet = s->quiet;
  j->sync = sync;
  j->policy = s->conflict;
  j->mu = SDL_CreateMutex();
  j->cv = SDL_CreateCond();
  j->info.id = g_next_id++;
  j->info.kind = s->kind;
  j->info.quiet = s->quiet;
  j->info.eta_s = -1;
  j->info.fraction = -1;
  make_title(j);
  return j;
}

static void job_destroy(FmJob *j) {
  for (int i = 0; i < j->nsrc; i++) fm_free(j->srcs[i]);
  fm_free(j->srcs);
  fm_free(j->buf);
  memset(j->password, 0, sizeof j->password);
  if (j->cv) SDL_DestroyCond(j->cv);
  if (j->mu) SDL_DestroyMutex(j->mu);
  fm_free(j);
}

FmJob *ops_start(const FmJobSpec *spec) {
  if (g_njobs >= JOB_MAX) return NULL;
  FmJob *j = job_new(spec, false);
  j->info.scanning = true;
  j->t_sample = plat_now_ms();
  g_jobs[g_njobs++] = j;
  j->th = fm_thread_create(job_thread, "mmcfm-job", j);
  if (!j->th) {
    /* no threads (a web build without pthreads): run it here; conflicts are
    ** skipped since nobody can answer while the main thread is busy */
    j->sync = true;
    if (j->policy == CONFLICT_ASK) j->policy = CONFLICT_SKIP;
    run_job(j);
  }
  return j;
}

FmErr ops_run_sync(const FmJobSpec *spec, FmJobInfo *out) {
  FmJob *j = job_new(spec, true);
  run_job(j);
  if (out) *out = j->info;
  FmErr e = j->info.err;
  job_destroy(j);
  return e;
}

void ops_demo(void) {
  static const char *const srcs[] = { "Holiday photos 2026" };
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = JOB_COPY;
  s.srcs = srcs;
  s.nsrc = 1;
  s.dst = "Backup";
  if (g_njobs >= JOB_MAX) return;
  FmJob *j = job_new(&s, false);
  j->demo = true;
  j->info.files_total = 1204;
  j->info.files_done = 517;
  j->info.bytes_total = 4ull * 1024 * 1024 * 1024 + 300ull * 1024 * 1024;
  j->info.bytes_done = j->info.bytes_total * 43 / 100;
  j->speed = 86.0f * 1024 * 1024;
  j->t_sample = plat_now_ms();
  j->b_sample = j->info.bytes_done;
  fm_strlcpy(j->info.current, "IMG_20260814_183022.jpg", sizeof j->info.current);
  g_jobs[g_njobs++] = j;
}

int ops_count(void) { return g_njobs; }
FmJob *ops_at(int i) { return (i >= 0 && i < g_njobs) ? g_jobs[i] : NULL; }
bool ops_is_quiet(FmJob *j) { return j->quiet; }

void ops_info(FmJob *j, FmJobInfo *out) {
  SDL_LockMutex(j->mu);
  *out = j->info;
  out->paused = j->paused;
  SDL_UnlockMutex(j->mu);
  /* speed: exponential average over ~0.25 s samples (UI thread only) */
  u64 now = plat_now_ms();
  if (!j->demo && now - j->t_sample >= 250) {
    float dt = (float)(now - j->t_sample) / 1000.0f;
    float inst = out->bytes_done >= j->b_sample ? (float)(out->bytes_done - j->b_sample) / dt : 0;
    if (out->paused || out->asking) inst = 0;
    j->speed = j->speed <= 0 ? inst : j->speed * 0.7f + inst * 0.3f;
    j->t_sample = now;
    j->b_sample = out->bytes_done;
  }
  out->speed = j->speed;
  if (out->bytes_total > 0 && !out->scanning)
    out->fraction = (float)((double)out->bytes_done / (double)out->bytes_total);
  else if (out->files_total > 0 && !out->scanning)
    out->fraction = (float)((double)out->files_done / (double)out->files_total);
  else
    out->fraction = -1;
  if (out->fraction > 1) out->fraction = 1;
  out->eta_s = -1;
  if (out->bytes_total > 0 && j->speed > 1024 && out->bytes_done <= out->bytes_total &&
      !out->scanning)
    out->eta_s = (int)((double)(out->bytes_total - out->bytes_done) / j->speed + 0.5);
}

int ops_running(bool include_quiet) {
  int n = 0;
  for (int i = 0; i < g_njobs; i++) {
    FmJob *j = g_jobs[i];
    if (j->quiet && !include_quiet) continue;
    SDL_LockMutex(j->mu);
    if (!j->finished) n++;
    SDL_UnlockMutex(j->mu);
  }
  return n;
}

void ops_pause(FmJob *j, bool pause) {
  SDL_LockMutex(j->mu);
  j->paused = pause;
  SDL_CondBroadcast(j->cv);
  SDL_UnlockMutex(j->mu);
}

void ops_cancel(FmJob *j) {
  SDL_LockMutex(j->mu);
  j->cancel = true;
  j->paused = false;
  if (j->demo) { j->finished = true; j->info.done = true; j->info.err = FM_ERR_CANCEL; }
  SDL_CondBroadcast(j->cv);
  SDL_UnlockMutex(j->mu);
}

void ops_cancel_all(void) {
  for (int i = 0; i < g_njobs; i++) ops_cancel(g_jobs[i]);
}

FmJob *ops_question(FmAsk *out) {
  for (int i = 0; i < g_njobs; i++) {
    FmJob *j = g_jobs[i];
    bool got = false;
    SDL_LockMutex(j->mu);
    if (j->ask.kind != ASK_NONE && !j->answered && !j->cancel) {
      *out = j->ask;
      got = true;
    }
    SDL_UnlockMutex(j->mu);
    if (got) return j;
  }
  return NULL;
}

void ops_answer_conflict(FmJob *j, FmConflict c, bool apply_all) {
  SDL_LockMutex(j->mu);
  j->answer = c;
  j->answer_all = apply_all;
  j->answered = true;
  if (c == CONFLICT_CANCEL) j->cancel = true;
  SDL_CondBroadcast(j->cv);
  SDL_UnlockMutex(j->mu);
}

void ops_answer_password(FmJob *j, const char *pw) {
  SDL_LockMutex(j->mu);
  j->answer_pw_ok = pw != NULL;
  if (pw) fm_strlcpy(j->answer_pw, pw, sizeof j->answer_pw);
  j->answered = true;
  SDL_CondBroadcast(j->cv);
  SDL_UnlockMutex(j->mu);
}

FmJob *ops_take_finished(void) {
  for (int i = 0; i < g_njobs; i++) {
    FmJob *j = g_jobs[i];
    bool take = false;
    SDL_LockMutex(j->mu);
    if (j->finished && !j->reported) { j->reported = true; take = true; }
    SDL_UnlockMutex(j->mu);
    if (take) return j;
  }
  return NULL;
}

void ops_free(FmJob *j) {
  for (int i = 0; i < g_njobs; i++) {
    if (g_jobs[i] == j) {
      memmove(&g_jobs[i], &g_jobs[i + 1], (size_t)(g_njobs - i - 1) * sizeof *g_jobs);
      g_njobs--;
      break;
    }
  }
  if (j->th) SDL_WaitThread(j->th, NULL);
  job_destroy(j);
}

void ops_shutdown(void) {
  ops_cancel_all();
  while (g_njobs > 0) ops_free(g_jobs[g_njobs - 1]);
  if (g_pw_mu) {
    memset(g_pw, 0, sizeof g_pw);
    SDL_DestroyMutex(g_pw_mu);
    g_pw_mu = NULL;
  }
}
