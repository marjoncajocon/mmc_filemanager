/* fcloud_mock.c -- an in-memory cloud service for the self test and the
** --demo-cloud screenshots.
**
** Design decisions:
**   - It behaves like an id-based service (Google Drive, MEGA): items are
**     "m<number>", the root is "". So the self test drives the same breadcrumb,
**     listing, transfer and conflict code the real adapters go through.
**   - One mutex guards the tree; every call may run on any worker. Contents
**     live in memory, except demo files without contents, which download as
**     a repeating pattern of their size.
**   - It needs a session like the real ones: an account without one lists
**     as "Signed out", and login with the password "wrong" fails, so the
**     sign-in paths can be tested offline.
**   - Only registered while cloud_mock_enable(true) (fcloud.c).
*/
#include "fcloud.h"
#include "fcloud_app.h"

typedef struct MNode {
  int parent;                  /* index, -1 for the root */
  char name[256];
  bool dir, used;
  u8 *data;                    /* NULL: `size` bytes of pattern */
  u64 size;
  i64 mtime;
} MNode;

static MNode *g_n;
static int g_nn, g_cap;
static SDL_mutex *g_mu;
static int g_delay;

static int node_new(int parent, const char *name, bool dir, u64 size, i64 mtime);

static void lock(void) {
  if (!g_mu) g_mu = SDL_CreateMutex();
  SDL_LockMutex(g_mu);
  if (!g_nn) node_new(-1, "", true, 0, 0);    /* the root */
}
static void unlock(void) { SDL_UnlockMutex(g_mu); }

static int node_new(int parent, const char *name, bool dir, u64 size, i64 mtime) {
  /* ids are never reused, like a real service's */
  if (g_nn == g_cap) {
    g_cap = g_cap ? g_cap * 2 : 64;
    g_n = (MNode *)fm_realloc(g_n, (size_t)g_cap * sizeof *g_n);
  }
  int i = g_nn++;
  MNode *n = &g_n[i];
  memset(n, 0, sizeof *n);
  n->parent = parent;
  fm_strlcpy(n->name, name, sizeof n->name);
  n->dir = dir;
  n->used = true;
  n->size = size;
  n->mtime = mtime;
  return i;
}

static int by_id(const char *id) {
  if (!id || !id[0]) return 0;
  if (id[0] != 'm') return -1;
  int i = atoi(id + 1);
  return (i > 0 && i < g_nn && g_n[i].used) ? i : -1;
}

static void fill(int i, FmCloudEntry *e) {
  memset(e, 0, sizeof *e);
  fm_snprintf(e->id, sizeof e->id, "m%d", i);
  fm_strlcpy(e->name, g_n[i].name, sizeof e->name);
  e->dir = g_n[i].dir;
  e->size = g_n[i].dir ? 0 : g_n[i].size;
  e->mtime = g_n[i].mtime;
}

static int child(int parent, const char *name) {
  for (int i = 1; i < g_nn; i++)
    if (g_n[i].used && g_n[i].parent == parent && !strcmp(g_n[i].name, name)) return i;
  return -1;
}

static void text_file(int parent, const char *name, const char *text, i64 t) {
  int i = node_new(parent, name, false, strlen(text), t);
  g_n[i].data = (u8 *)fm_strdup(text);
}

void cloud_mock_reset(bool demo) {
  lock();
  for (int i = 0; i < g_nn; i++) fm_free(g_n[i].data);
  g_nn = 0;
  node_new(-1, "", true, 0, 0);
  if (demo) {
    i64 now = plat_time_unix(), day = 86400;
    int docs = node_new(0, "Documents", true, 0, now - 3 * day);
    int photos = node_new(0, "Photos", true, 0, now - 1 * day);
    int music = node_new(0, "Music", true, 0, now - 40 * day);
    int backup = node_new(0, "Backups", true, 0, now - 9 * day);
    node_new(0, "Shared with me", true, 0, now - 2 * day);
    text_file(0, "Welcome.txt",
              "Welcome to your demo cloud.\n\nCopy files between this panel and a local folder with the "
              "Copy and Move buttons, or drag them across.\n", now - 12 * day);
    node_new(0, "Project plan.docx", false, 182400, now - 5 * day);
    node_new(0, "Trip to the coast.mp4", false, 48ull << 20, now - 20 * day);
    node_new(docs, "Report 2026.pdf", false, 2400000, now - 3600);
    node_new(docs, "Budget.xlsx", false, 64200, now - 4 * day);
    text_file(docs, "Notes.txt", "Buy milk.\nCall the bank.\nBook the train.\n", now - 7 * day);
    int hol = node_new(photos, "Holiday 2026", true, 0, now - 30 * day);
    for (int k = 0; k < 6; k++) {
      char nm[64];
      fm_snprintf(nm, sizeof nm, "IMG_2026081%d_1830%02d.jpg", k, 10 + k * 7);
      node_new(hol, nm, false, 2100000 + (u64)k * 173000, now - (30 - k) * day);
    }
    node_new(photos, "Profile.png", false, 412000, now - 60 * day);
    node_new(music, "Morning walk.mp3", false, 5200000, now - 41 * day);
    node_new(backup, "phone-2026-09.zip", false, 734003200, now - 9 * day);
  }
  unlock();
}

void cloud_mock_set_delay(int ms) { g_delay = ms; }

int cloud_mock_count(void) {
  lock();
  int n = 0;
  for (int i = 1; i < g_nn; i++) n += g_n[i].used;
  unlock();
  return n;
}

static FmErr wait(volatile int *cancel) {
  for (int t = 0; t < g_delay; t += 20) {
    if (cancel && *cancel) return FM_ERR_CANCEL;
    SDL_Delay(20);
  }
  return (cancel && *cancel) ? FM_ERR_CANCEL : FM_OK;
}

static FmErr signed_in(const FmCloudAcct *a, char *err, size_t cap) {
  if (a->session[0]) return FM_OK;
  fm_strlcpy(err, "Signed out: sign in again", cap);
  return FM_ERR_PASSWORD;
}

static FmErr m_login(FmCloudAcct *a, char *err, size_t errcap, volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  if (!a->user[0] || !strcmp(a->secret, "wrong")) {
    fm_strlcpy(err, "Wrong e-mail or password", errcap);
    return FM_ERR_PASSWORD;
  }
  fm_snprintf(a->session, sizeof a->session, "mock-session:%s", a->user);
  a->secret[0] = 0;
  a->session_changed = true;
  return FM_OK;
}

static FmErr m_list(FmCloudAcct *a, const char *dir_id, FmCloudList *out, volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  FmErr e = signed_in(a, out->error, sizeof out->error);
  if (e != FM_OK) return e;
  lock();
  int d = by_id(dir_id);
  if (d < 0 || !g_n[d].dir) {
    unlock();
    fm_strlcpy(out->error, "Demo cloud: not found (HTTP 404)", sizeof out->error);
    return FM_ERR_NOT_FOUND;
  }
  for (int i = 1; i < g_nn; i++)
    if (g_n[i].used && g_n[i].parent == d) fill(i, cloud_list_add(out));
  unlock();
  return FM_OK;
}

static FmErr m_download(FmCloudAcct *a, const FmCloudEntry *e, const char *local_path, FmNetProgress cb, void *user,
                        char *err, size_t errcap, volatile int *cancel) {
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  int i = by_id(e->id);
  if (i < 0 || g_n[i].dir) {
    unlock();
    fm_strlcpy(err, "Demo cloud: not found (HTTP 404)", errcap);
    return FM_ERR_NOT_FOUND;
  }
  u64 size = g_n[i].size;
  u8 *data = g_n[i].data ? (u8 *)fm_alloc(size + 1) : NULL;
  if (data) memcpy(data, g_n[i].data, size);
  unlock();
  char part[FM_PATH_MAX + 8];
  fm_snprintf(part, sizeof part, "%s.part", local_path);
  FILE *f = fm_fopen(part, "wb");
  if (!f) {
    fm_free(data);
    fm_strlcpy(err, "Can't write the file", errcap);
    return FM_ERR_ACCESS;
  }
  u8 buf[16384];
  for (size_t k = 0; k < sizeof buf; k++) buf[k] = (u8)("mmcfm demo cloud "[k % 17]);
  u64 done = 0;
  r = FM_OK;
  while (done < size) {
    size_t n = (size_t)FM_MIN((u64)sizeof buf, size - done);
    const u8 *src = data ? data + done : buf;
    if (fwrite(src, 1, n, f) != n) { r = FM_ERR_FULL; break; }
    done += n;
    if ((cancel && *cancel) || (cb && !cb(user, done, size))) { r = FM_ERR_CANCEL; break; }
  }
  fm_free(data);
  if (fclose(f) != 0 && r == FM_OK) r = FM_ERR_FULL;
  if (r == FM_OK) {
    plat_remove_file(local_path);
    r = plat_rename(part, local_path);
  }
  if (r != FM_OK) {
    plat_remove_file(part);
    fm_strlcpy(err, r == FM_ERR_CANCEL ? "Cancelled" : fm_err_str(r), errcap);
  }
  return r;
}

static FmErr m_upload(FmCloudAcct *a, const char *dir_id, const char *local_path, const char *name, FmCloudEntry *out,
                      FmNetProgress cb, void *user, char *err, size_t errcap, volatile int *cancel) {
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  FILE *f = fm_fopen(local_path, "rb");
  if (!f) {
    fm_strlcpy(err, "Can't read the file", errcap);
    return FM_ERR_NOT_FOUND;
  }
  i64 sz = fm_fsize(f);
  if (sz < 0 || sz > (64 << 20)) {
    fclose(f);
    fm_strlcpy(err, "Demo cloud: files up to 64 MB", errcap);
    return FM_ERR_FULL;
  }
  u8 *data = (u8 *)fm_alloc((size_t)sz + 1);
  size_t got = 0;
  while (got < (size_t)sz) {
    size_t n = fread(data + got, 1, FM_MIN((size_t)65536, (size_t)sz - got), f);
    if (n == 0) break;
    got += n;
    if ((cancel && *cancel) || (cb && !cb(user, got, (u64)sz))) {
      fclose(f);
      fm_free(data);
      return FM_ERR_CANCEL;
    }
  }
  fclose(f);
  lock();
  int d = by_id(dir_id);
  if (d < 0 || !g_n[d].dir) {
    unlock();
    fm_free(data);
    fm_strlcpy(err, "Demo cloud: not found (HTTP 404)", errcap);
    return FM_ERR_NOT_FOUND;
  }
  int i = child(d, name);
  if (i >= 0 && g_n[i].dir) {
    unlock();
    fm_free(data);
    fm_strlcpy(err, "Demo cloud: a folder has this name (HTTP 409)", errcap);
    return FM_ERR_EXISTS;
  }
  if (i < 0) i = node_new(d, name, false, 0, 0);
  fm_free(g_n[i].data);
  g_n[i].data = data;
  g_n[i].size = got;
  g_n[i].mtime = plat_time_unix();
  if (out) fill(i, out);
  unlock();
  return FM_OK;
}

static FmErr m_mkdir(FmCloudAcct *a, const char *parent_id, const char *name, FmCloudEntry *out, char *err,
                     size_t errcap, volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  int d = by_id(parent_id);
  if (d < 0 || !g_n[d].dir) {
    unlock();
    fm_strlcpy(err, "Demo cloud: not found (HTTP 404)", errcap);
    return FM_ERR_NOT_FOUND;
  }
  if (child(d, name) >= 0) {
    unlock();
    fm_strlcpy(err, "Demo cloud: an item with this name already exists (HTTP 409)", errcap);
    return FM_ERR_EXISTS;
  }
  int i = node_new(d, name, true, 0, plat_time_unix());
  if (out) fill(i, out);
  unlock();
  return FM_OK;
}

static void drop(int i) {
  for (int k = 1; k < g_nn; k++)
    if (g_n[k].used && g_n[k].parent == i) drop(k);
  fm_free(g_n[i].data);
  g_n[i].data = NULL;
  g_n[i].used = false;
}

static FmErr m_remove(FmCloudAcct *a, const FmCloudEntry *e, char *err, size_t errcap, volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  int i = by_id(e->id);
  if (i <= 0) {
    unlock();
    fm_strlcpy(err, "Demo cloud: not found (HTTP 404)", errcap);
    return FM_ERR_NOT_FOUND;
  }
  drop(i);
  unlock();
  return FM_OK;
}

static FmErr m_rename(FmCloudAcct *a, const FmCloudEntry *e, const char *new_name, char *err, size_t errcap,
                      volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  int i = by_id(e->id);
  int o = i > 0 ? child(g_n[i].parent, new_name) : -1;
  if (i <= 0 || (o >= 0 && o != i)) {
    unlock();
    fm_strlcpy(err, i <= 0 ? "Demo cloud: not found (HTTP 404)" : "Demo cloud: an item with this name already exists",
               errcap);
    return i <= 0 ? FM_ERR_NOT_FOUND : FM_ERR_EXISTS;
  }
  fm_strlcpy(g_n[i].name, new_name, sizeof g_n[i].name);
  unlock();
  return FM_OK;
}

static FmErr m_move(FmCloudAcct *a, const FmCloudEntry *e, const char *new_parent_id, char *err, size_t errcap,
                    volatile int *cancel) {
  if (wait(cancel) != FM_OK) return FM_ERR_CANCEL;
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  int i = by_id(e->id), d = by_id(new_parent_id);
  bool loop = false;
  for (int k = d; k > 0 && !loop; k = g_n[k].parent) loop = k == i;
  if (i <= 0 || d < 0 || !g_n[d].dir || loop || child(d, g_n[i].name) >= 0) {
    unlock();
    fm_strlcpy(err, loop ? "Can't move a folder into itself" : "Demo cloud: can't move there", errcap);
    return loop ? FM_ERR_UNSUPPORTED : FM_ERR_EXISTS;
  }
  g_n[i].parent = d;
  unlock();
  return FM_OK;
}

static FmErr m_quota(FmCloudAcct *a, u64 *used, u64 *total, char *err, size_t errcap, volatile int *cancel) {
  FM_UNUSED(cancel);
  FmErr r = signed_in(a, err, errcap);
  if (r != FM_OK) return r;
  lock();
  u64 u = 0;
  for (int i = 1; i < g_nn; i++)
    if (g_n[i].used && !g_n[i].dir) u += g_n[i].size;
  unlock();
  *used = u;
  *total = 15ull << 30;
  return FM_OK;
}

const FmCloud g_cloud_mock = {
  "mock", "Demo cloud", IC_CLOUD, CLOUD_PASSWORD | CLOUD_UPLOAD,
  m_login, m_list, m_download, m_upload, m_mkdir, m_remove, m_rename, m_move, m_quota, NULL, NULL,
  "A pretend service kept in memory, for trying things out",
};
