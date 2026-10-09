/* fcloud_vfs.c -- cloud folders in the panels: locations, the listing cache
** and the background tasks that call the adapters.
**
** Design decisions:
**   - A cloud location reuses FmLoc's two strings: `path` holds the folder
**     ids from the account's root down, one per level ending in '\n';
**     `inner` holds the names the same way. An id that starts with its
**     parent's id (path-style services: "a/", "a/b/") is stored as
**     "\x01" + the rest, so deep WebDAV / S3 / Dropbox folders still fit in
**     FM_PATH_MAX. Up, the breadcrumb and the path bar need no network.
**   - Listing never blocks the UI: vfs_list starts a CT_LIST task and the
**     panel shows "Loading" (or keeps the old rows during a refresh); the
**     task's result goes into a small cache keyed "cloud:<serial>:<id>", so
**     back, up and the other panel get the folder at once. Refresh drops the
**     key first. Listings in the cache are shared by panels with a count;
**     unused ones are evicted beyond 48 folders or 4096 entries (an entry is
**     about 1 KB).
**   - A failed listing stays in the cache only to tell the waiting panels;
**     the next vfs_list of that folder asks again.
**   - Tasks are one short thread each (signing in, a listing, a new folder,
**     a rename, the quota, a stream link, a public link) on a snapshot of
**     the account; cloud_pump hands finished ones to their callbacks on the
**     main thread, and a woken frame is the only cost while idle.
*/
#include "fcloud_app.h"
#include "fapp.h"

/* ---- locations ----------------------------------------------------------- */

#define LOC_SHORT '\x01'

/* Decodes levels up to `stop`; id and name get the last one decoded. */
static int walk(const FmLoc *l, int stop, char *id, size_t icap, char *name, size_t ncap) {
  char cur[CLOUD_ID_MAX] = "";
  char nm[256] = "";
  const char *p = l->path, *q = l->inner;
  int k = 0;
  while (*p && k < stop) {
    const char *e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (*p == LOC_SHORT) {
      size_t have = strlen(cur);
      size_t add = FM_MIN(n - 1, sizeof cur - 1 - have);
      memcpy(cur + have, p + 1, add);
      cur[have + add] = 0;
    } else {
      size_t c = FM_MIN(n, sizeof cur - 1);
      memcpy(cur, p, c);
      cur[c] = 0;
    }
    const char *qe = strchr(q, '\n');
    size_t qn = qe ? (size_t)(qe - q) : strlen(q);
    size_t c = FM_MIN(qn, sizeof nm - 1);
    memcpy(nm, q, c);
    nm[c] = 0;
    p = e ? e + 1 : p + n;
    q = qe ? qe + 1 : q + qn;
    k++;
  }
  if (id) fm_strlcpy(id, cur, icap);
  if (name) fm_strlcpy(name, nm, ncap);
  return k;
}

static void acct_label(int serial, char *out, size_t cap) {
  const FmCloudAcct *a = cloud_acct_by_serial(serial);
  if (a && a->label[0]) fm_strlcpy(out, a->label, cap);
  else if (a) {
    const FmCloud *c = cloud_find(a->provider);
    fm_strlcpy(out, c ? c->name : a->provider, cap);
  } else {
    fm_strlcpy(out, "Cloud", cap);
  }
}

void cloud_loc_root(FmLoc *l, int serial) {
  memset(l, 0, sizeof *l);
  l->in_cloud = true;
  l->cloud = serial;
}

bool cloud_loc_child(FmLoc *l, const char *id, const char *name) {
  char cur[CLOUD_ID_MAX];
  walk(l, 1 << 30, cur, sizeof cur, NULL, 0);
  size_t cl = strlen(cur), il = strlen(id);
  bool shrt = cl > 0 && il > cl && strncmp(id, cur, cl) == 0;
  size_t need = shrt ? 1 + il - cl : il;
  size_t pl = strlen(l->path), nl = strlen(l->inner), name_l = strlen(name);
  if (pl + need + 2 > sizeof l->path || nl + name_l + 2 > sizeof l->inner) return false;
  if (strchr(id, '\n') || id[0] == LOC_SHORT) return false;
  char *o = l->path + pl;
  if (shrt) {
    *o++ = LOC_SHORT;
    memcpy(o, id + cl, il - cl);
    o += il - cl;
  } else {
    memcpy(o, id, il);
    o += il;
  }
  *o++ = '\n';
  *o = 0;
  o = l->inner + nl;
  for (const char *s = name; *s; s++) *o++ = (*s == '\n' || *s == '\r') ? ' ' : *s;
  *o++ = '\n';
  *o = 0;
  return true;
}

static void drop_last(char *s) {
  size_t n = strlen(s);
  if (!n) return;
  n--;                                       /* the level's own '\n' */
  while (n > 0 && s[n - 1] != '\n') n--;
  s[n] = 0;
}

bool cloud_loc_up(FmLoc *l) {
  if (!l->path[0]) return false;
  drop_last(l->path);
  drop_last(l->inner);
  return true;
}

int cloud_loc_depth(const FmLoc *l) {
  int n = 0;
  for (const char *s = l->path; *s; s++) n += *s == '\n';
  return n;
}

void cloud_loc_level(const FmLoc *l, int k, char *id, size_t icap, char *name, size_t ncap) {
  if (k <= 0) {
    if (id) fm_strlcpy(id, "", icap);
    if (name) acct_label(l->cloud, name, ncap);
    return;
  }
  walk(l, k, id, icap, name, ncap);
}

void cloud_loc_trim(FmLoc *l, int depth) {
  while (cloud_loc_depth(l) > depth && cloud_loc_up(l)) {}
}

void cloud_loc_id(const FmLoc *l, char *out, size_t cap) {
  walk(l, 1 << 30, out, cap, NULL, 0);
}

void cloud_loc_title(const FmLoc *l, char *out, size_t cap) {
  if (!l->path[0]) { acct_label(l->cloud, out, cap); return; }
  walk(l, 1 << 30, NULL, 0, out, cap);
}

void cloud_loc_display(const FmLoc *l, char *out, size_t cap) {
  acct_label(l->cloud, out, cap);
  const char *q = l->inner;
  while (*q) {
    const char *e = strchr(q, '\n');
    size_t n = e ? (size_t)(e - q) : strlen(q);
    fm_strlcat(out, "/", cap);
    size_t o = strlen(out);
    if (o + n < cap) {
      memcpy(out + o, q, n);
      out[o + n] = 0;
    }
    q = e ? e + 1 : q + n;
  }
}

void cloud_loc_key(const FmLoc *l, char *out, size_t cap) {
  char id[CLOUD_ID_MAX];
  cloud_loc_id(l, id, sizeof id);
  fm_snprintf(out, cap, "cloud:%d:%s", l->cloud, id);
}

int cloud_key_serial(const char *key) {
  if (!key || strncmp(key, "cloud:", 6) != 0) return 0;
  return atoi(key + 6);
}

/* ---- tasks ------------------------------------------------------------------ */

static FmCloudTask *g_tasks;
static bool g_sync;

void cloud_set_sync(bool on) { g_sync = on; }

FmCloudTask *cloud_task_new(int kind, int serial) {
  FmCloudTask *t = (FmCloudTask *)fm_calloc(1, sizeof *t);
  t->kind = kind;
  t->serial = serial;
  if (serial > 0 && !cloud_acct_snapshot(serial, &t->acct)) {
    fm_free(t);
    return NULL;
  }
  return t;
}

static void task_free(FmCloudTask *t) {
  cloud_list_free(&t->list);
  memset(&t->acct, 0, sizeof t->acct);
  fm_free(t);
}

static void task_run(FmCloudTask *t) {
  const FmCloud *c = cloud_find(t->acct.provider);
  FmCloudAcct *a = &t->acct;
  char *m = t->msg;
  size_t mc = sizeof t->msg;
  FmErr e = FM_ERR_UNSUPPORTED;
  m[0] = 0;
  if (!c) {
    fm_strlcpy(m, "This service is not part of this build", mc);
    t->err = FM_ERR_UNSUPPORTED;
    return;
  }
  bool have = true;
  switch (t->kind) {
    case CT_LIST:
      if ((have = c->list != NULL)) {
        e = c->list(a, t->arg, &t->list, &t->cancel);
        if (e != FM_OK) fm_strlcpy(m, t->list.error, mc);
      }
      break;
    case CT_LOGIN:
      if ((have = c->login != NULL)) e = c->login(a, m, mc, &t->cancel);
      break;
    case CT_MKDIR:
      if ((have = c->mkdir != NULL)) e = c->mkdir(a, t->arg, t->name, &t->entry, m, mc, &t->cancel);
      break;
    case CT_RENAME:
      if ((have = c->rename != NULL)) e = c->rename(a, &t->entry, t->name, m, mc, &t->cancel);
      break;
    case CT_QUOTA:
      if ((have = c->quota != NULL)) e = c->quota(a, &t->used, &t->total, m, mc, &t->cancel);
      break;
    case CT_STREAM:
      if ((have = c->stream_url != NULL))
        e = c->stream_url(a, &t->entry, t->url, sizeof t->url, t->headers, sizeof t->headers, m, mc, &t->cancel);
      break;
    case CT_LINK:
      if ((have = c->open_link != NULL)) e = c->open_link(t->arg, a, &t->entry, m, mc, &t->cancel);
      break;
    default: break;
  }
  if (!have) fm_snprintf(m, mc, "%s can't do this yet", c->name);
  if (e != FM_OK && !m[0]) fm_strlcpy(m, fm_err_str(e), mc);
  t->err = e;
  if (t->serial > 0) cloud_acct_writeback(t->serial, a, t->kind == CT_LOGIN && e == FM_OK);
}

static int SDLCALL task_thread(void *ud) {
  FmCloudTask *t = (FmCloudTask *)ud;
  task_run(t);
  SDL_AtomicSet(&t->fin, 1);
  app_wake();
  return 0;
}

void cloud_task_start(FmCloudTask *t) {
  if (!t) return;
  if (!g_sync) {
    t->next = g_tasks;
    g_tasks = t;
    t->th = fm_thread_create(task_thread, "mmcfm-cloud", t);
    if (t->th) return;
    g_tasks = t->next;                         /* no threads (web): run it here */
  }
  task_run(t);
  if (t->done) t->done(t);
  task_free(t);
}

bool cloud_task_busy(int kind, int serial) {
  for (FmCloudTask *t = g_tasks; t; t = t->next)
    if (t->kind == kind && t->serial == serial && !SDL_AtomicGet(&t->fin)) return true;
  return false;
}

void cloud_task_cancel(int kind, int serial) {
  for (FmCloudTask *t = g_tasks; t; t = t->next)
    if (t->kind == kind && t->serial == serial) t->cancel = 1;
}

void cloud_pump(void) {
  for (;;) {
    FmCloudTask **pp = &g_tasks, *t = NULL;
    while (*pp) {
      if (SDL_AtomicGet(&(*pp)->fin)) { t = *pp; *pp = t->next; break; }
      pp = &(*pp)->next;
    }
    if (!t) break;
    SDL_WaitThread(t->th, NULL);
    if (t->done) t->done(t);
    task_free(t);
  }
  cloud_acct_pump();
}

void cloud_tasks_shutdown(void) {
  for (FmCloudTask *t = g_tasks; t; t = t->next) t->cancel = 1;
  while (g_tasks) {
    FmCloudTask *t = g_tasks;
    g_tasks = t->next;
    SDL_WaitThread(t->th, NULL);
    task_free(t);
  }
}

/* ---- the listing cache -------------------------------------------------------- */

#define CACHE_DIRS 48
#define CACHE_ENTRIES 4096

typedef struct CDir {
  char key[CLOUD_ID_MAX + 32];  /* "" once detached: freed when the last panel lets go */
  int serial;
  FmCloudList list;
  FmErr err;
  int refs;
  u64 stamp;
  struct CDir *next;
} CDir;

static CDir *g_dirs;
static u64 g_stamp;

static CDir *find(const char *key, bool failed_too) {
  for (CDir *d = g_dirs; d; d = d->next)
    if (d->key[0] && !strcmp(d->key, key) && (failed_too || d->err == FM_OK)) return d;
  return NULL;
}

static void dir_free(CDir *d) {
  CDir **pp = &g_dirs;
  while (*pp && *pp != d) pp = &(*pp)->next;
  if (*pp) *pp = d->next;
  cloud_list_free(&d->list);
  fm_free(d);
}

static void detach(CDir *d) {
  d->key[0] = 0;
  if (d->refs <= 0) dir_free(d);
}

static void evict(void) {
  for (;;) {
    int ndirs = 0, nent = 0;
    CDir *old = NULL;
    for (CDir *d = g_dirs; d; d = d->next) {
      ndirs++;
      nent += d->list.count;
      if (d->refs == 0 && (!old || d->stamp < old->stamp)) old = d;
    }
    if ((ndirs <= CACHE_DIRS && nent <= CACHE_ENTRIES) || !old) break;
    dir_free(old);
  }
}

void cloud_vfs_invalidate(const char *key) {
  for (CDir *d = g_dirs, *n; d; d = n) {
    n = d->next;
    if (d->key[0] && !strcmp(d->key, key)) detach(d);
  }
}

void cloud_vfs_forget(int serial) {
  for (CDir *d = g_dirs, *n; d; d = n) {
    n = d->next;
    if (d->serial == serial && d->key[0]) detach(d);
  }
}

void cloud_vfs_release(FmListing *l) {
  CDir *d = (CDir *)l->cdir;
  l->cdir = NULL;
  if (!d) return;
  d->refs--;
  if (d->refs <= 0 && !d->key[0]) dir_free(d);
}

const FmCloudEntry *cloud_vfs_entry(const FmListing *l, const FmEntry *e) {
  const CDir *d = (const CDir *)l->cdir;
  if (!d || !e || e->arc_index < 0 || e->arc_index >= d->list.count) return NULL;
  return &d->list.items[e->arc_index];
}

static void list_done(FmCloudTask *t) {
  if (t->err == FM_ERR_CANCEL) return;
  /* an older copy of this folder may still be shown: it stays until let go */
  for (CDir *d = g_dirs, *n; d; d = n) {
    n = d->next;
    if (d->key[0] && !strcmp(d->key, t->key)) detach(d);
  }
  CDir *d = (CDir *)fm_calloc(1, sizeof *d);
  fm_strlcpy(d->key, t->key, sizeof d->key);
  d->serial = t->serial;
  d->list = t->list;                         /* taken over */
  memset(&t->list, 0, sizeof t->list);
  d->err = t->err;
  if (t->err != FM_OK) fm_strlcpy(d->list.error, t->msg, sizeof d->list.error);
  d->stamp = ++g_stamp;
  d->next = g_dirs;
  g_dirs = d;
  evict();
}

static bool list_pending(const char *key) {
  for (FmCloudTask *t = g_tasks; t; t = t->next)
    if (t->kind == CT_LIST && !strcmp(t->key, key)) return true;
  return false;
}

static FmEntry *push(FmListing *l) {
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->items = (FmEntry *)fm_realloc(l->items, (size_t)l->cap * sizeof *l->items);
  }
  FmEntry *e = &l->items[l->count++];
  memset(e, 0, sizeof *e);
  e->arc_index = -1;
  return e;
}

static void reset_items(FmListing *l) {
  if (l->arena.block_size == 0) arena_init(&l->arena, 64 * 1024);
  else arena_reset(&l->arena);
  l->count = 0;
  l->total_size = 0;
}

static void fill(FmListing *l, CDir *d, bool show_hidden) {
  reset_items(l);
  if (l->cdir != d) {
    cloud_vfs_release(l);
    l->cdir = d;
    d->refs++;
  }
  d->stamp = ++g_stamp;
  for (int i = 0; i < d->list.count; i++) {
    const FmCloudEntry *ce = &d->list.items[i];
    if (!ce->name[0]) continue;
    bool hidden = ce->name[0] == '.';
    if (hidden && !show_hidden) continue;
    FmEntry *e = push(l);
    e->name = arena_strdup(&l->arena, ce->name);
    e->size = ce->dir ? 0 : ce->size;
    e->mtime = ce->mtime;
    e->flags = (ce->dir ? FM_ST_DIR : 0) | (hidden ? FM_ST_HIDDEN : 0);
    e->type = (u16)(ce->dir ? FT_DIR : fm_type_from_name(ce->name));
    e->arc_index = i;
    if (!ce->dir) l->total_size += ce->size;
  }
  l->loading = false;
  l->err = FM_OK;
  l->errmsg[0] = 0;
}

FmErr cloud_vfs_list(FmListing *l, const FmLoc *loc, bool show_hidden) {
  if (l->arc) {
    arc_close(l->arc);
    l->arc = NULL;
  }
  bool same = l->loc.in_cloud && loc_equal(&l->loc, loc) && l->err == FM_OK;
  l->loc = *loc;
  l->errmsg[0] = 0;
  l->err = FM_OK;
  char key[CLOUD_ID_MAX + 32];
  cloud_loc_key(loc, key, sizeof key);
  CDir *d = find(key, false);
  if (d) {
    fill(l, d, show_hidden);
    return FM_OK;
  }
  if (!same) {
    reset_items(l);
    cloud_vfs_release(l);
  }
  l->loading = true;
  if (cloud_acct_index(loc->cloud) < 0) {
    reset_items(l);
    cloud_vfs_release(l);
    l->loading = false;
    l->err = FM_ERR_NOT_FOUND;
    fm_strlcpy(l->errmsg, "This account was removed", sizeof l->errmsg);
    return l->err;
  }
  if (!list_pending(key)) {
    /* a failed answer stays only until someone asks again */
    CDir *f = find(key, true);
    if (f) detach(f);
    FmCloudTask *t = cloud_task_new(CT_LIST, loc->cloud);
    if (t) {
      cloud_loc_id(loc, t->arg, sizeof t->arg);
      fm_strlcpy(t->key, key, sizeof t->key);
      t->done = list_done;
      cloud_task_start(t);
    }
  }
  cloud_vfs_poll(l, show_hidden);              /* the sync mode answers at once */
  return l->err;
}

bool cloud_vfs_poll(FmListing *l, bool show_hidden) {
  if (!l->loading) return false;
  char key[CLOUD_ID_MAX + 32];
  cloud_loc_key(&l->loc, key, sizeof key);
  CDir *d = find(key, true);
  if (!d) return false;
  if (d->err == FM_OK) {
    fill(l, d, show_hidden);
    return true;
  }
  l->loading = false;
  if (l->count > 0) {
    /* a refresh that failed: keep what was there and say why */
    ui_toast("%s", d->list.error[0] ? d->list.error : fm_err_str(d->err));
    return true;
  }
  l->err = d->err;
  fm_strlcpy(l->errmsg, d->list.error, sizeof l->errmsg);
  return true;
}

/* ---- quota ---------------------------------------------------------------------- */

typedef struct Quota { int serial; u64 used, total; u64 t; bool have; } Quota;
static Quota g_quota[8];

static Quota *quota_slot(int serial, bool make) {
  Quota *old = &g_quota[0];
  for (int i = 0; i < FM_COUNT(g_quota); i++) {
    if (g_quota[i].serial == serial) return &g_quota[i];
    if (g_quota[i].t < old->t) old = &g_quota[i];
  }
  if (!make) return NULL;
  memset(old, 0, sizeof *old);
  old->serial = serial;
  return old;
}

static void quota_done(FmCloudTask *t) {
  Quota *q = quota_slot(t->serial, true);
  q->t = plat_now_ms();
  q->have = t->err == FM_OK;
  q->used = t->used;
  q->total = t->total;
  ui_redraw();
}

bool cloud_quota(int serial, u64 *used, u64 *total) {
  Quota *q = quota_slot(serial, false);
  u64 now = plat_now_ms();
  if ((!q || !q->t || now - q->t > 120000) && !cloud_task_busy(CT_QUOTA, serial)) {
    const FmCloudAcct *a = cloud_acct_by_serial(serial);
    const FmCloud *c = a ? cloud_find(a->provider) : NULL;
    if (c && c->quota) {
      if (!q) q = quota_slot(serial, true);
      q->t = now;                              /* no new ask while this one runs */
      FmCloudTask *t = cloud_task_new(CT_QUOTA, serial);
      if (t) {
        t->done = quota_done;
        cloud_task_start(t);
      }
      q = quota_slot(serial, false);
    }
  }
  if (!q || !q->have) return false;
  *used = q->used;
  *total = q->total;
  return true;
}

void cloud_quota_stale(int serial) {
  Quota *q = quota_slot(serial, false);
  if (q) q->t = 1;
}
