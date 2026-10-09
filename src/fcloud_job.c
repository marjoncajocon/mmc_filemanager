/* fcloud_job.c -- uploads, downloads, cloud-to-cloud copies, moves and
** deletes as file jobs (fops.c custom jobs), with the usual card, pause,
** cancel and "This name is taken" questions.
**
** Design decisions:
**   - One engine for every direction. The worker first makes a plan: a flat
**     list of items in depth-first order (folders before their contents),
**     walked from local folders or listed from the service; the totals come
**     from it, so the progress bar is honest from the start. Then each item
**     is put into its target folder: a local folder or a cloud folder id.
**   - Cloud targets are listed once per folder to find name conflicts (the
**     listing of each level is kept while its contents are copied, and
**     what the job adds goes into it, so "Keep both" picks a free name).
**     A folder the job made itself is known to be empty and never listed.
**     Names compare without case, which is what most services do.
**   - Downloads go to a hidden part file next to the target and are renamed
**     into place when complete, like local copies; "Replace" keeps the old
**     file until the new one is whole. Cloud-to-cloud copies pass through a
**     temporary file (no service copies between accounts) and count their
**     bytes twice. A move within one account is the adapter's move.
**   - A move removes a source only when everything below it arrived
**     (nothing skipped or failed), cloud sources to the service's trash.
**   - The job works on snapshots of the accounts and hands refreshed
**     sessions back at the end.
*/
#include "fcloud_app.h"
#include "fapp.h"

#define MAX_DEPTH 64

typedef struct Item {
  char *src;                   /* local path or cloud id */
  char *name;
  char *mime, *hash;
  u64 size;
  i64 mtime;
  bool dir;
  int parent, depth;
  char *dest;                  /* the target folder made or found (local path / cloud id) */
  bool bad;                    /* something at or below it was skipped or failed */
  bool skip;
  bool fresh;                  /* a folder this job made: empty */
} Item;

typedef struct DestDir {
  bool valid;
  char handle[CLOUD_ID_MAX];
  FmCloudList list;
} DestDir;

typedef struct CJob {
  int op;
  bool move;
  /* source: local paths, or entries of a cloud account */
  int src_serial;
  const FmCloud *sc;
  FmCloudAcct sa;
  char **paths;
  int npaths;
  FmCloudEntry *entries;
  int nentries;
  char src_key[CLOUD_ID_MAX + 32];
  char src_disp[FM_PATH_MAX];
  /* target: a local folder, or a cloud folder */
  int dst_serial;
  const FmCloud *dc;
  FmCloudAcct da;
  char dst_dir[FM_PATH_MAX];
  char dst_id[CLOUD_ID_MAX];
  char dst_key[CLOUD_ID_MAX + 32];
  char dst_disp[FM_PATH_MAX];
  char open_path[FM_PATH_MAX];
  /* worker */
  FmJob *j;
  volatile int *cancel;
  FmArena ar;
  Item *it;
  int n, cap;
  DestDir dl[MAX_DEPTH + 2];
  char err[256];
} CJob;

/* ---- helpers ---------------------------------------------------------------- */

typedef struct Prog {
  FmJob *j;
  u64 size, last;
} Prog;

static bool prog_cb(void *u, u64 done, u64 total) {
  FM_UNUSED(total);
  Prog *p = (Prog *)u;
  if (p->size && done > p->size) done = p->size;
  if (done > p->last) {
    ops_job_bytes(p->j, done - p->last);
    p->last = done;
  }
  return !ops_job_stop(p->j);
}

/* The rest of a file counts as done, whatever happened, so the bar ends full. */
static void prog_end(Prog *p) {
  if (p->last < p->size) ops_job_bytes(p->j, p->size - p->last);
  p->last = p->size;
}

static void mark_bad(CJob *c, int i) {
  for (; i >= 0; i = c->it[i].parent) c->it[i].bad = true;
}

static void entry_of(const Item *it, FmCloudEntry *e) {
  memset(e, 0, sizeof *e);
  fm_strlcpy(e->id, it->src, sizeof e->id);
  fm_strlcpy(e->name, it->name, sizeof e->name);
  e->size = it->size;
  e->mtime = it->mtime;
  e->dir = it->dir;
  if (it->mime) fm_strlcpy(e->mime, it->mime, sizeof e->mime);
  if (it->hash) fm_strlcpy(e->hash, it->hash, sizeof e->hash);
}

static int add_item(CJob *c, const char *src, const char *name, u64 size, i64 mtime, bool dir, int parent) {
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 64;
    c->it = (Item *)fm_realloc(c->it, (size_t)c->cap * sizeof *c->it);
  }
  Item *it = &c->it[c->n];
  memset(it, 0, sizeof *it);
  it->src = arena_strdup(&c->ar, src);
  it->name = arena_strdup(&c->ar, name);
  it->size = dir ? 0 : size;
  it->mtime = mtime;
  it->dir = dir;
  it->parent = parent;
  it->depth = parent < 0 ? 0 : c->it[parent].depth + 1;
  return c->n++;
}

/* "a/b/name" below the job's top, with the system separator, for questions. */
static void rel_path(const CJob *c, int i, char *out, size_t cap) {
  if (i < 0) { out[0] = 0; return; }
  rel_path(c, c->it[i].parent, out, cap);
  if (out[0]) fm_strlcat(out, FM_SEP_STR, cap);
  fm_strlcat(out, c->it[i].name, cap);
}

static FmCloudEntry *find_name(FmCloudList *l, const char *name) {
  for (int i = 0; i < l->count; i++)
    if (!fm_stricmp(l->items[i].name, name)) return &l->items[i];
  return NULL;
}

/* "name (2).ext" ... that is not in l. */
static void unique_in_list(FmCloudList *l, const char *name, char *out, size_t cap) {
  const char *ext = fm_path_ext(name);
  char stem[256];
  size_t sl = FM_MIN((size_t)(ext - name), sizeof stem - 1);
  memcpy(stem, name, sl);
  stem[sl] = 0;
  for (int k = 2; k < 100000; k++) {
    fm_snprintf(out, cap, "%s (%d)%s", stem, k, ext);
    if (!find_name(l, out)) return;
  }
}

/* The listing of target folder `handle` at depth d (cloud targets). */
static FmCloudList *dest_list(CJob *c, int d, const char *handle, bool fresh) {
  DestDir *dd = &c->dl[FM_MIN(d, MAX_DEPTH + 1)];
  if (dd->valid && !strcmp(dd->handle, handle)) return &dd->list;
  cloud_list_free(&dd->list);
  dd->valid = true;
  fm_strlcpy(dd->handle, handle, sizeof dd->handle);
  if (!fresh) {
    FmErr e = c->dc->list(&c->da, handle, &dd->list, c->cancel);
    if (e != FM_OK) {
      /* conflicts are then found by the service itself (HTTP 409) */
      fm_log("cloud job: listing the target failed: %s", dd->list.error);
      cloud_list_free(&dd->list);
    }
  }
  return &dd->list;
}

static void list_put(FmCloudList *l, const FmCloudEntry *e) {
  FmCloudEntry *o = find_name(l, e->name);
  if (!o) o = cloud_list_add(l);
  *o = *e;
}

/* Removes a local tree (a moved source). */
static bool del_local(CJob *c, const char *path, int depth) {
  FmStat st;
  if (!plat_stat(path, &st)) return plat_remove_file(path) == FM_OK || !plat_exists(path);
  if ((st.flags & FM_ST_DIR) && !(st.flags & FM_ST_LINK) && depth < MAX_DEPTH) {
    FmDir *d = plat_dir_open(path, NULL);
    bool ok = true;
    if (d) {
      const char *name;
      char ch[FM_PATH_MAX];
      while (plat_dir_next(d, &name, NULL))
        if (fm_path_join(ch, sizeof ch, path, name) && !del_local(c, ch, depth + 1)) ok = false;
      plat_dir_close(d);
    }
    return ok && plat_remove_dir(path) == FM_OK;
  }
  FmErr e = plat_remove_file(path);
  if (e != FM_OK) e = plat_remove_dir(path);
  return e == FM_OK;
}

static void temp_file(char *out, size_t cap) {
  char dir[FM_PATH_MAX], name[64];
  u8 r[6];
  if (!plat_place(PLACE_TEMP, dir, sizeof dir) && !plat_place(PLACE_CACHE, dir, sizeof dir)) fm_strlcpy(dir, ".", sizeof dir);
  plat_random(r, sizeof r);
  fm_snprintf(name, sizeof name, "mmcfm-cloud-%02x%02x%02x%02x%02x%02x.tmp", r[0], r[1], r[2], r[3], r[4], r[5]);
  fm_path_join(out, cap, dir, name);
}

/* ---- planning ----------------------------------------------------------------- */

static void plan_local(CJob *c, const char *path, int parent, int depth) {
  if (ops_job_stop(c->j)) return;
  FmStat st;
  if (!plat_stat(path, &st)) {
    ops_job_error(c->j, path, FM_ERR_NOT_FOUND, NULL);
    if (parent >= 0) mark_bad(c, parent);
    return;
  }
  if (st.flags & FM_ST_BROKEN) {
    ops_job_error(c->j, path, FM_ERR_NOT_FOUND, "Broken link");
    if (parent >= 0) mark_bad(c, parent);
    return;
  }
  bool dir = (st.flags & FM_ST_DIR) != 0;
  int i = add_item(c, path, fm_path_base(path), st.size, st.mtime, dir, parent);
  if (!dir) {
    ops_job_totals(c->j, 1, st.size);
    return;
  }
  if (depth >= MAX_DEPTH) {
    ops_job_error(c->j, path, FM_ERR_IO, "Folders nested too deeply");
    mark_bad(c, i);
    return;
  }
  FmErr oe;
  FmDir *d = plat_dir_open(path, &oe);
  if (!d) {
    ops_job_error(c->j, path, oe, NULL);
    mark_bad(c, i);
    return;
  }
  /* names first: the recursion may grow c->it */
  FmArena tmp;
  arena_init(&tmp, 8 * 1024);
  char **names = NULL;
  int nn = 0, cap = 0;
  const char *name;
  while (plat_dir_next(d, &name, NULL)) {
    if (nn == cap) {
      cap = cap ? cap * 2 : 32;
      names = (char **)fm_realloc(names, (size_t)cap * sizeof *names);
    }
    names[nn++] = arena_strdup(&tmp, name);
  }
  plat_dir_close(d);
  char ch[FM_PATH_MAX];
  for (int k = 0; k < nn && !*c->cancel; k++) {
    if (!fm_path_join(ch, sizeof ch, path, names[k])) {
      ops_job_error(c->j, names[k], FM_ERR_IO, "Path too long");
      mark_bad(c, i);
      continue;
    }
    plan_local(c, ch, i, depth + 1);
  }
  fm_free(names);
  arena_free(&tmp);
}

static void plan_cloud(CJob *c, const FmCloudEntry *e, int parent, int depth, u64 weight) {
  if (ops_job_stop(c->j)) return;
  int i = add_item(c, e->id, e->name, e->size, e->mtime, e->dir, parent);
  if (e->mime[0]) c->it[i].mime = arena_strdup(&c->ar, e->mime);
  if (e->hash[0]) c->it[i].hash = arena_strdup(&c->ar, e->hash);
  if (!e->dir) {
    ops_job_totals(c->j, 1, e->size * weight);
    return;
  }
  if (depth >= MAX_DEPTH) {
    ops_job_error(c->j, e->name, FM_ERR_IO, "Folders nested too deeply");
    mark_bad(c, i);
    return;
  }
  FmCloudList l;
  memset(&l, 0, sizeof l);
  FmErr err = c->sc->list(&c->sa, e->id, &l, c->cancel);
  if (err != FM_OK) {
    if (err != FM_ERR_CANCEL) ops_job_error(c->j, e->name, err, l.error[0] ? l.error : NULL);
    mark_bad(c, i);
  } else {
    for (int k = 0; k < l.count && !*c->cancel; k++) plan_cloud(c, &l.items[k], i, depth + 1, weight);
  }
  cloud_list_free(&l);
}

/* ---- putting items ------------------------------------------------------------ */

static FmConflict ask(CJob *c, int i, const char *dst_dir_disp, const char *name, bool ddir, u64 dsize, i64 dmtime) {
  const Item *it = &c->it[i];
  FmAsk q;
  memset(&q, 0, sizeof q);
  q.kind = ASK_CONFLICT;
  if (c->src_serial == 0) {
    fm_strlcpy(q.src, it->src, sizeof q.src);
  } else {
    char rel[FM_PATH_MAX];
    rel_path(c, i, rel, sizeof rel);
    fm_path_join(q.src, sizeof q.src, c->src_disp, rel);
  }
  q.src_known = true;
  q.src_st.size = it->size;
  q.src_st.mtime = it->mtime;
  q.src_st.flags = it->dir ? FM_ST_DIR : 0;
  fm_path_join(q.dst, sizeof q.dst, dst_dir_disp, name);
  q.dst_st.size = dsize;
  q.dst_st.mtime = dmtime;
  q.dst_st.flags = ddir ? FM_ST_DIR : 0;
  return ops_job_conflict(c->j, &q);
}

static void fail(CJob *c, int i, FmErr e, const char *msg) {
  if (e != FM_ERR_CANCEL && !*c->cancel) ops_job_error(c->j, c->it[i].name, e, msg && msg[0] ? msg : NULL);
  mark_bad(c, i);
}

static void skipped(CJob *c, int i) {
  c->it[i].skip = true;
  if (!c->it[i].dir) ops_job_skip(c->j, c->it[i].size * (c->src_serial && c->dst_serial ? 2u : 1u));
  mark_bad(c, i);
}

/* Cloud file -> local file `dst` through a hidden part file. */
static FmErr download_to(CJob *c, int i, const char *dst, bool replace) {
  const Item *it = &c->it[i];
  char dir[FM_PATH_MAX], part[FM_PATH_MAX], pn[300];
  fm_strlcpy(dir, dst, sizeof dir);
  fm_path_parent(dir);
  fm_snprintf(pn, sizeof pn, ".%s.mmcfm-cloud", fm_path_base(dst));
  if (strlen(pn) > 240 || !fm_path_join(part, sizeof part, dir, pn)) fm_strlcpy(part, dst, sizeof part);
  FmCloudEntry e;
  entry_of(it, &e);
  Prog p = { c->j, it->size, 0 };
  c->err[0] = 0;
  FmErr r = c->sc->download(&c->sa, &e, part, prog_cb, &p, c->err, sizeof c->err, c->cancel);
  prog_end(&p);
  if (r == FM_OK && strcmp(part, dst) != 0) {
    if (it->mtime) plat_set_mtime(part, it->mtime);
    if (replace) {
      FmErr re = plat_remove_file(dst);
      if (re != FM_OK && re != FM_ERR_NOT_FOUND) r = re;
    }
    if (r == FM_OK) r = plat_rename(part, dst);
  } else if (r == FM_OK && it->mtime) {
    plat_set_mtime(dst, it->mtime);
  }
  if (r != FM_OK && strcmp(part, dst) != 0) {
    char pp[FM_PATH_MAX + 8];
    plat_remove_file(part);
    fm_snprintf(pp, sizeof pp, "%s.part", part);
    plat_remove_file(pp);
  }
  return r;
}

/* Local file (or a cloud file through a temporary copy) -> cloud folder. */
static FmErr upload_to(CJob *c, int i, const char *parent_id, const char *name, FmCloudEntry *out) {
  const Item *it = &c->it[i];
  char tmp[FM_PATH_MAX] = "";
  const char *local = it->src;
  c->err[0] = 0;
  if (c->src_serial) {
    temp_file(tmp, sizeof tmp);
    FmCloudEntry e;
    entry_of(it, &e);
    Prog p = { c->j, it->size, 0 };
    FmErr r = c->sc->download(&c->sa, &e, tmp, prog_cb, &p, c->err, sizeof c->err, c->cancel);
    prog_end(&p);
    if (r != FM_OK) {
      plat_remove_file(tmp);
      return r;
    }
    local = tmp;
  }
  Prog p = { c->j, it->size, 0 };
  FmErr r = c->dc->upload(&c->da, parent_id, local, name, out, prog_cb, &p, c->err, sizeof c->err, c->cancel);
  prog_end(&p);
  if (tmp[0]) plat_remove_file(tmp);
  return r;
}

static void put_local(CJob *c, int i, const char *pdest) {
  Item *it = &c->it[i];
  char dst[FM_PATH_MAX];
  if (!fm_path_join(dst, sizeof dst, pdest, it->name)) {
    fail(c, i, FM_ERR_IO, "Path too long");
    return;
  }
  FmStat ds;
  bool exists = plat_stat(dst, &ds) || plat_exists(dst);
  if (exists && it->dir && (ds.flags & FM_ST_DIR)) {      /* merge */
    it->dest = arena_strdup(&c->ar, dst);
    return;
  }
  bool replace = false;
  if (exists) {
    FmConflict k = ask(c, i, pdest, it->name, (ds.flags & FM_ST_DIR) != 0, ds.size, ds.mtime);
    if (k == CONFLICT_SKIP || k == CONFLICT_CANCEL) {
      if (k == CONFLICT_SKIP) skipped(c, i);
      else mark_bad(c, i);
      return;
    }
    if (k == CONFLICT_KEEP_BOTH) {
      if (!ops_unique_name(pdest, it->name, dst, sizeof dst)) { skipped(c, i); return; }
    } else if (ds.flags & FM_ST_DIR) {
      fail(c, i, FM_ERR_EXISTS, "A folder with this name exists");
      if (!it->dir) ops_job_bytes(c->j, it->size);
      return;
    } else {
      replace = true;
    }
  }
  if (it->dir) {
    if (replace) plat_remove_file(dst);
    FmErr e = plat_mkdir(dst);
    if (e != FM_OK) { fail(c, i, e, NULL); return; }
    it->dest = arena_strdup(&c->ar, dst);
    it->fresh = true;
    return;
  }
  FmErr e = download_to(c, i, dst, replace);
  if (e != FM_OK) { fail(c, i, e, c->err); return; }
  ops_job_file_done(c->j);
}

static void put_cloud(CJob *c, int i, const char *pdest, bool pfresh, const char *pdisp) {
  Item *it = &c->it[i];
  FmCloudList *l = dest_list(c, it->depth, pdest, pfresh);
  FmCloudEntry *ex = find_name(l, it->name);
  char name[256];
  fm_strlcpy(name, it->name, sizeof name);
  if (ex && it->dir && ex->dir) {                           /* merge */
    it->dest = arena_strdup(&c->ar, ex->id);
    return;
  }
  if (ex) {
    FmCloudEntry old = *ex;
    FmConflict k = ask(c, i, pdisp, it->name, old.dir, old.size, old.mtime);
    if (k == CONFLICT_SKIP || k == CONFLICT_CANCEL) {
      if (k == CONFLICT_SKIP) skipped(c, i);
      else mark_bad(c, i);
      return;
    }
    if (k == CONFLICT_KEEP_BOTH) {
      unique_in_list(l, it->name, name, sizeof name);
    } else if (old.dir || it->dir) {
      /* a folder never silently replaces a file or the other way round */
      fail(c, i, FM_ERR_EXISTS, old.dir ? "A folder with this name exists" : "A file with this name exists");
      if (!it->dir) ops_job_bytes(c->j, it->size * (c->src_serial ? 2u : 1u));
      return;
    }
  }
  FmCloudEntry out;
  memset(&out, 0, sizeof out);
  c->err[0] = 0;
  if (it->dir) {
    FmErr e = c->dc->mkdir(&c->da, pdest, name, &out, c->err, sizeof c->err, c->cancel);
    if (e != FM_OK || !out.id[0]) { fail(c, i, e != FM_OK ? e : FM_ERR_IO, c->err); return; }
    it->dest = arena_strdup(&c->ar, out.id);
    it->fresh = true;
    list_put(l, &out);
    return;
  }
  FmErr e = upload_to(c, i, pdest, name, &out);
  if (e != FM_OK) { fail(c, i, e, c->err); return; }
  if (!out.name[0]) {
    fm_strlcpy(out.name, name, sizeof out.name);
    out.size = it->size;
  }
  list_put(l, &out);
  ops_job_file_done(c->j);
}

/* ---- the jobs ------------------------------------------------------------------- */

static bool need(CJob *c, const FmCloud *svc, bool ok, const char *what) {
  if (ok) return true;
  char m[160];
  fm_snprintf(m, sizeof m, "%s can't %s yet", svc ? svc->name : "This service", what);
  ops_job_error(c->j, svc ? svc->name : "cloud", FM_ERR_UNSUPPORTED, m);
  return false;
}

static void run_transfer(CJob *c) {
  bool up = c->dst_serial != 0, down = c->src_serial != 0;
  if (down && !need(c, c->sc, c->sc->list && c->sc->download, "download")) return;
  if (up && !need(c, c->dc, c->dc->list && c->dc->upload && c->dc->mkdir, "upload")) return;
  if (c->move && down && !need(c, c->sc, c->sc->remove != NULL, "move files out")) return;
  ops_job_scanning(c->j, true);
  if (down) {
    u64 w = up ? 2 : 1;
    for (int k = 0; k < c->nentries && !*c->cancel; k++) plan_cloud(c, &c->entries[k], -1, 0, w);
  } else {
    for (int k = 0; k < c->npaths && !*c->cancel; k++) plan_local(c, c->paths[k], -1, 0);
  }
  ops_job_scanning(c->j, false);
  /* the target folder's display path per item, for questions */
  for (int i = 0; i < c->n && !ops_job_stop(c->j); i++) {
    Item *it = &c->it[i];
    if (it->parent >= 0) {
      const Item *pa = &c->it[it->parent];
      if (pa->skip || !pa->dest) {
        skipped(c, i);
        continue;
      }
    }
    if (it->depth <= 1 || !it->dir) ops_job_current(c->j, it->name);
    const char *pdest = it->parent < 0 ? (up ? c->dst_id : c->dst_dir) : c->it[it->parent].dest;
    if (up) {
      char disp[FM_PATH_MAX], rel[FM_PATH_MAX];
      rel_path(c, it->parent, rel, sizeof rel);
      if (rel[0]) fm_path_join(disp, sizeof disp, c->dst_disp, rel);
      else fm_strlcpy(disp, c->dst_disp, sizeof disp);
      put_cloud(c, i, pdest, it->parent >= 0 && c->it[it->parent].fresh, disp);
    } else {
      put_local(c, i, pdest);
    }
  }
  if (!c->move || *c->cancel) return;
  for (int i = 0; i < c->n && !*c->cancel; i++) {
    const Item *it = &c->it[i];
    if (it->parent >= 0 || it->bad || it->skip) continue;
    if (down) {
      FmCloudEntry e;
      entry_of(it, &e);
      c->err[0] = 0;
      FmErr r = c->sc->remove(&c->sa, &e, c->err, sizeof c->err, c->cancel);
      if (r != FM_OK) ops_job_error(c->j, it->name, r, c->err[0] ? c->err : "Copied, but the original could not be removed");
    } else if (!del_local(c, it->src, 0)) {
      ops_job_error(c->j, it->src, FM_ERR_ACCESS, "Copied, but the original could not be removed");
    }
  }
}

/* Within one account: the service moves (and renames for "Keep both"). */
static void run_move_same(CJob *c) {
  ops_job_totals(c->j, (u64)c->nentries, 0);
  FmCloudList *l = dest_list(c, 0, c->dst_id, false);
  for (int k = 0; k < c->nentries && !ops_job_stop(c->j); k++) {
    FmCloudEntry e = c->entries[k];
    ops_job_current(c->j, e.name);
    if (!strcmp(e.id, c->dst_id)) {
      ops_job_error(c->j, e.name, FM_ERR_UNSUPPORTED, "Can't move a folder into itself");
      continue;
    }
    FmCloudEntry *ex = find_name(l, e.name);
    if (ex && !strcmp(ex->id, e.id)) {                     /* already there */
      ops_job_file_done(c->j);
      continue;
    }
    char newname[256] = "";
    if (ex) {
      FmCloudEntry old = *ex;
      int i = add_item(c, e.id, e.name, e.size, e.mtime, e.dir, -1);
      FmConflict q = ask(c, i, c->dst_disp, e.name, old.dir, old.size, old.mtime);
      if (q == CONFLICT_SKIP) { ops_job_skip(c->j, 0); continue; }
      if (q == CONFLICT_CANCEL) break;
      if (q == CONFLICT_KEEP_BOTH) {
        unique_in_list(l, e.name, newname, sizeof newname);
      } else if (old.dir || e.dir) {
        ops_job_error(c->j, e.name, FM_ERR_EXISTS, old.dir ? "A folder with this name exists" : "A file with this name exists");
        continue;
      } else {
        c->err[0] = 0;
        FmErr r = c->dc->remove(&c->da, &old, c->err, sizeof c->err, c->cancel);
        if (r != FM_OK) { ops_job_error(c->j, e.name, r, c->err); continue; }
      }
    }
    c->err[0] = 0;
    FmErr r = c->dc->move(&c->da, &e, c->dst_id, c->err, sizeof c->err, c->cancel);
    if (r == FM_OK && newname[0]) r = c->dc->rename(&c->da, &e, newname, c->err, sizeof c->err, c->cancel);
    if (r != FM_OK) {
      if (r != FM_ERR_CANCEL) ops_job_error(c->j, e.name, r, c->err[0] ? c->err : NULL);
      continue;
    }
    if (newname[0]) fm_strlcpy(e.name, newname, sizeof e.name);
    list_put(l, &e);
    ops_job_file_done(c->j);
  }
}

static void run_delete(CJob *c) {
  if (!need(c, c->sc, c->sc->remove != NULL, "delete")) return;
  ops_job_totals(c->j, (u64)c->nentries, 0);
  for (int k = 0; k < c->nentries && !ops_job_stop(c->j); k++) {
    ops_job_current(c->j, c->entries[k].name);
    c->err[0] = 0;
    FmErr r = c->sc->remove(&c->sa, &c->entries[k], c->err, sizeof c->err, c->cancel);
    if (r != FM_OK) {
      if (r != FM_ERR_CANCEL) ops_job_error(c->j, c->entries[k].name, r, c->err[0] ? c->err : NULL);
      continue;
    }
    ops_job_file_done(c->j);
  }
}

static void run_open(CJob *c) {
  if (c->nentries < 1 || !need(c, c->sc, c->sc->download != NULL, "download")) return;
  const FmCloudEntry *e = &c->entries[0];
  ops_job_totals(c->j, 1, e->size);
  ops_job_current(c->j, e->name);
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, c->open_path, sizeof dir);
  fm_path_parent(dir);
  plat_mkdirs(dir);
  Prog p = { c->j, e->size, 0 };
  c->err[0] = 0;
  FmErr r = c->sc->download(&c->sa, e, c->open_path, prog_cb, &p, c->err, sizeof c->err, c->cancel);
  prog_end(&p);
  if (r != FM_OK) {
    plat_remove_file(c->open_path);
    if (r != FM_ERR_CANCEL) ops_job_error(c->j, e->name, r, c->err[0] ? c->err : NULL);
    return;
  }
  ops_job_file_done(c->j);
  ops_job_result(c->j, c->open_path);
}

static void job_run(FmJob *j, void *data) {
  CJob *c = (CJob *)data;
  c->j = j;
  c->cancel = ops_job_cancel_flag(j);
  arena_init(&c->ar, 32 * 1024);
  switch (c->op) {
    case CJ_TRANSFER:
      if (c->move && c->src_serial && c->src_serial == c->dst_serial && c->dc->move && c->dc->list) run_move_same(c);
      else run_transfer(c);
      break;
    case CJ_DELETE: run_delete(c); break;
    case CJ_OPEN: run_open(c); break;
    default: break;
  }
  /* refreshed tokens go back to the saved account */
  if (c->src_serial) cloud_acct_writeback(c->src_serial, &c->sa, false);
  if (c->dst_serial) cloud_acct_writeback(c->dst_serial, &c->da, false);
  if (c->op == CJ_TRANSFER) {
    ops_job_touch(j, 0, c->dst_serial ? c->dst_key : c->dst_dir);
    if (c->move) {
      if (c->src_serial) ops_job_touch(j, 1, c->src_key);
      else if (c->npaths > 0) {
        char d[FM_PATH_MAX];
        fm_strlcpy(d, c->paths[0], sizeof d);
        fm_path_parent(d);
        ops_job_touch(j, 1, d);
      }
    }
  } else if (c->op == CJ_DELETE) {
    ops_job_touch(j, 0, c->src_key);
  }
}

static void job_free(void *data) {
  CJob *c = (CJob *)data;
  for (int i = 0; i < c->npaths; i++) fm_free(c->paths[i]);
  fm_free(c->paths);
  fm_free(c->entries);
  fm_free(c->it);
  arena_free(&c->ar);
  for (int d = 0; d < MAX_DEPTH + 2; d++) cloud_list_free(&c->dl[d].list);
  memset(&c->sa, 0, sizeof c->sa);
  memset(&c->da, 0, sizeof c->da);
  fm_free(c);
}

/* ---- starting --------------------------------------------------------------------- */

static bool side(int serial, FmCloudAcct *a, const FmCloud **svc) {
  if (!cloud_acct_snapshot(serial, a)) return false;
  *svc = cloud_find(a->provider);
  return *svc != NULL;
}

FmErr cloud_job_run(const FmCloudJobSpec *s, bool sync, FmJobInfo *out) {
  CJob *c = (CJob *)fm_calloc(1, sizeof *c);
  c->op = s->op;
  c->move = s->move;
  if (s->src_loc && s->src_loc->in_cloud) {
    c->src_serial = s->src_loc->cloud;
    if (!side(c->src_serial, &c->sa, &c->sc)) { job_free(c); return FM_ERR_NOT_FOUND; }
    cloud_loc_key(s->src_loc, c->src_key, sizeof c->src_key);
    loc_display(s->src_loc, c->src_disp, sizeof c->src_disp);
    for (char *p = c->src_disp; *p; p++) if (*p == '/') *p = FM_SEP;
    c->nentries = s->nentries;
    c->entries = (FmCloudEntry *)fm_alloc((size_t)(s->nentries > 0 ? s->nentries : 1) * sizeof *c->entries);
    if (s->nentries > 0) memcpy(c->entries, s->entries, (size_t)s->nentries * sizeof *c->entries);
  } else {
    c->npaths = s->npaths;
    c->paths = (char **)fm_calloc((size_t)(s->npaths > 0 ? s->npaths : 1), sizeof(char *));
    for (int i = 0; i < s->npaths; i++) c->paths[i] = fm_strdup(s->paths[i]);
  }
  if (s->op == CJ_TRANSFER) {
    if (s->dst_loc && s->dst_loc->in_cloud) {
      c->dst_serial = s->dst_loc->cloud;
      if (!side(c->dst_serial, &c->da, &c->dc)) { job_free(c); return FM_ERR_NOT_FOUND; }
      cloud_loc_id(s->dst_loc, c->dst_id, sizeof c->dst_id);
      cloud_loc_key(s->dst_loc, c->dst_key, sizeof c->dst_key);
      loc_display(s->dst_loc, c->dst_disp, sizeof c->dst_disp);
      for (char *p = c->dst_disp; *p; p++) if (*p == '/') *p = FM_SEP;
    } else if (s->dst_dir) {
      fm_strlcpy(c->dst_dir, s->dst_dir, sizeof c->dst_dir);
      fm_strlcpy(c->dst_disp, s->dst_dir, sizeof c->dst_disp);
    }
    if (!c->src_serial && !c->dst_serial) { job_free(c); return FM_ERR_UNSUPPORTED; }
  }
  if (s->open_path) fm_strlcpy(c->open_path, s->open_path, sizeof c->open_path);

  /* the card's title */
  char what[300], to[256], title[400];
  int n = c->src_serial ? c->nentries : c->npaths;
  if (n == 1) fm_snprintf(what, sizeof what, "\"%s\"", c->src_serial ? c->entries[0].name : fm_path_base(c->paths[0]));
  else fm_snprintf(what, sizeof what, "%d items", n);
  FmJobKind kind = JOB_COPY;
  if (s->op == CJ_TRANSFER) {
    if (c->dst_serial) loc_title(s->dst_loc, to, sizeof to);
    else fm_strlcpy(to, fm_path_base(c->dst_dir)[0] ? fm_path_base(c->dst_dir) : c->dst_dir, sizeof to);
    const char *verb = c->move ? "Moving" : !c->src_serial ? "Uploading" : !c->dst_serial ? "Downloading" : "Copying";
    fm_snprintf(title, sizeof title, "%s %s to %s", verb, what, to);
    kind = c->move ? JOB_MOVE : JOB_COPY;
  } else if (s->op == CJ_DELETE) {
    loc_title(s->src_loc, to, sizeof to);
    fm_snprintf(title, sizeof title, "Deleting %s from %s", what, to);
    kind = JOB_DELETE;
  } else {
    fm_snprintf(title, sizeof title, "Opening %s", what);
    kind = JOB_OPEN;
  }

  FmJobSpec js;
  memset(&js, 0, sizeof js);
  js.kind = kind;
  js.conflict = s->conflict;
  js.quiet = s->quiet;
  js.run = job_run;
  js.run_data = c;
  js.run_free = job_free;
  js.title = title;
  if (sync) return ops_run_sync(&js, out);
  if (!ops_start(&js)) {
    job_free(c);
    return FM_ERR_IO;
  }
  return FM_OK;
}

bool cloud_cache_path(int serial, const FmCloudEntry *e, char *out, size_t cap) {
  const FmCloudAcct *a = cloud_acct_by_serial(serial);
  if (!a) return false;
  u64 h = 1469598103934665603ull;
  const char *parts[4] = { a->provider, a->user, a->server, e->id };
  for (int k = 0; k < 4; k++) {
    for (const char *s = parts[k]; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    h ^= 0xFF;
    h *= 1099511628211ull;
  }
  h ^= e->size;
  h *= 1099511628211ull;
  h ^= (u64)e->mtime;
  h *= 1099511628211ull;
  char base[FM_PATH_MAX], sub[48];
  if (!plat_place(PLACE_CACHE, base, sizeof base)) return false;
  fm_snprintf(sub, sizeof sub, "cloud%c%08x%08x", FM_SEP, (unsigned)(h >> 32), (unsigned)h);
  if (!fm_path_join(base, sizeof base, base, sub)) return false;
  char name[256];
  fm_strlcpy(name, e->name, sizeof name);
  for (char *s = name; *s; s++)
    if ((u8)*s < 0x20 || strchr("/\\:*?\"<>|", *s)) *s = '_';
  if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) fm_strlcpy(name, "file", sizeof name);
  return fm_path_join(out, cap, base, name);
}
