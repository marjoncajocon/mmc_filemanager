/* fcloud_app.h -- cloud storage inside the app: accounts by serial, cloud
** folders in the panels, background tasks, transfer jobs and the UI.
**
** fcloud.h is the adapters' contract; this header is what the rest of the
** app uses. Modules:
**   fcloud.c       registry, saved accounts (protected secrets), helpers
**   fcloud_mock.c  an in-memory service for the self test and --demo-cloud
**   fcloud_vfs.c   cloud locations, the listing cache, background tasks
**   fcloud_job.c   uploads, downloads, cloud-to-cloud copies, deletes as jobs
**   fcloud_ui.c    places, the account sheets, settings, panel operations
**
** Threading: everything here is main thread only, except that tasks and
** jobs call the adapters on workers with a snapshot of the account.
*/
#ifndef FCLOUD_APP_H
#define FCLOUD_APP_H

#include "fcloud.h"
#include "fvfs.h"
#include "fops.h"
#include "fui.h"
#include "fsdl.h"

/* ---- registry and accounts extras (fcloud.c) ----------------------------- */

void cloud_init(bool shot);                       /* shot: nothing loaded or saved */
void cloud_shutdown(void);
void cloud_mock_enable(bool on);                  /* adds the in-memory "Demo cloud" */
bool cloud_available(const FmCloud *c);           /* written (not a stub) */

int  cloud_acct_add_temp(const FmCloudAcct *a);   /* listed, never saved (links, demo) */
bool cloud_acct_is_temp(int i);
void cloud_acct_keep(int i);
u32  cloud_acct_stamp(void);                       /* changes whenever the list or a label does */                      /* a temporary account becomes saved */
int  cloud_acct_serial(int i);                    /* stable for the session, > 0 */
int  cloud_acct_index(int serial);                /* -1 when removed */
FmCloudAcct *cloud_acct_by_serial(int serial);
void cloud_acct_lock(void);                       /* around edits of a live account */
void cloud_acct_unlock(void);
/* Workers: a copy to call the adapter on, and its changed session handed
** back (signed_in: also the secret and user, after login). */
bool cloud_acct_snapshot(int serial, FmCloudAcct *out);
void cloud_acct_writeback(int serial, FmCloudAcct *a, bool signed_in);
void cloud_acct_pump(void);                       /* saves when a worker changed a session */
void cloud_acct_set_store(const char *path);      /* tests: another accounts file (NULL = default) */

/* Secrets as stored: "d:<base64>" (DPAPI) or "o:<base64>" (obfuscated). */
void cloud_protect(const char *plain, char *out, size_t cap);
bool cloud_unprotect(const char *val, char *out, size_t cap);
void cloud_secret_force_obfuscation(bool on);     /* tests: the non-DPAPI path */

/* ---- the mock service (fcloud_mock.c) ------------------------------------ */

void cloud_mock_reset(bool demo);                 /* empty, or the demo folders */
void cloud_mock_set_delay(int ms);                /* every call sleeps (spinner shots) */
int  cloud_mock_count(void);                      /* items stored, for tests */

/* ---- locations (fcloud_vfs.c) ---------------------------------------------- */

void cloud_loc_root(FmLoc *l, int serial);
/* Into a child folder; false when the chain would not fit. */
bool cloud_loc_child(FmLoc *l, const char *id, const char *name);
bool cloud_loc_up(FmLoc *l);                      /* false at the account's root */
int  cloud_loc_depth(const FmLoc *l);             /* 0 = root */
/* Level k (0 = root: id "", the account label). */
void cloud_loc_level(const FmLoc *l, int k, char *id, size_t icap, char *name, size_t ncap);
void cloud_loc_trim(FmLoc *l, int depth);
void cloud_loc_id(const FmLoc *l, char *out, size_t cap);
void cloud_loc_title(const FmLoc *l, char *out, size_t cap);
void cloud_loc_display(const FmLoc *l, char *out, size_t cap);   /* "Label/a/b" */
/* "cloud:<serial>:<folder id>": a folder's key for the cache, jobs' touched[]. */
void cloud_loc_key(const FmLoc *l, char *out, size_t cap);
int  cloud_key_serial(const char *key);           /* 0 when not a cloud key */

/* ---- listings (fcloud_vfs.c, through vfs_list / vfs_poll) ----------------- */

FmErr cloud_vfs_list(FmListing *l, const FmLoc *loc, bool show_hidden);
bool  cloud_vfs_poll(FmListing *l, bool show_hidden);
void  cloud_vfs_release(FmListing *l);
void  cloud_vfs_invalidate(const char *key);      /* the next list of that folder fetches */
void  cloud_vfs_forget(int serial);               /* every cached folder of an account */
const FmCloudEntry *cloud_vfs_entry(const FmListing *l, const FmEntry *e);

/* ---- background tasks (fcloud_vfs.c) ---------------------------------------- */

enum { CT_LIST, CT_LOGIN, CT_MKDIR, CT_RENAME, CT_QUOTA, CT_STREAM, CT_LINK };

typedef struct FmCloudTask FmCloudTask;
typedef void (*FmCloudTaskDone)(FmCloudTask *t);

struct FmCloudTask {
  int kind;                    /* CT_* */
  int serial;                  /* the account (0 for CT_LINK) */
  FmCloudAcct acct;            /* snapshot; CT_LINK: the pseudo account it fills */
  FmCloudEntry entry;          /* RENAME / STREAM: the item; MKDIR / LINK: the result */
  char arg[CLOUD_ID_MAX];      /* LIST: folder id; MKDIR: parent id; LINK: the link */
  char name[256];              /* MKDIR / RENAME: the name */
  char key[CLOUD_ID_MAX + 32]; /* LIST: the folder's key */
  FmCloudList list;            /* LIST: the result (taken by the done callback) */
  u64 used, total;             /* QUOTA */
  char url[2048];              /* STREAM */
  char headers[1024];
  FmErr err;
  char msg[256];               /* the reason, when err != FM_OK */
  FmCloudTaskDone done;        /* main thread, then the task is freed */
  int ud_int;
  char ud_str[FM_PATH_MAX];
  volatile int cancel;
  /* private */
  SDL_Thread *th;
  SDL_atomic_t fin;
  FmCloudTask *next;
};

/* A task for the account `serial` (snapshot taken now); NULL when it is gone. */
FmCloudTask *cloud_task_new(int kind, int serial);
void cloud_task_start(FmCloudTask *t);
bool cloud_task_busy(int kind, int serial);       /* one of that kind still runs */
void cloud_task_cancel(int kind, int serial);
void cloud_set_sync(bool on);                     /* tests: tasks run inline */
/* Each frame on the main thread: finished tasks, account saving. */
void cloud_pump(void);
void cloud_tasks_shutdown(void);

/* Storage used/total of an account as last fetched; asks again when it is
** older than two minutes. False while unknown. */
bool cloud_quota(int serial, u64 *used, u64 *total);
void cloud_quota_stale(int serial);

/* ---- jobs (fcloud_job.c) ------------------------------------------------------ */

enum { CJ_TRANSFER, CJ_DELETE, CJ_OPEN };

typedef struct FmCloudJobSpec {
  int op;                          /* CJ_* */
  bool move;                       /* TRANSFER: remove the sources after a clean copy */
  const char *const *paths;        /* local sources ... */
  int npaths;
  const FmLoc *src_loc;            /* ... or cloud entries in this cloud folder */
  const FmCloudEntry *entries;
  int nentries;
  const char *dst_dir;             /* TRANSFER target: a local folder ... */
  const FmLoc *dst_loc;            /* ... or a cloud folder */
  const char *open_path;           /* OPEN: the cache file to write (entries[0]) */
  FmConflict conflict;
  bool quiet;
} FmCloudJobSpec;

/* Starts the job (sync = run here, for tests: CONFLICT_ASK skips). Async:
** FM_OK when it started. Sync: the job's result, out filled. */
FmErr cloud_job_run(const FmCloudJobSpec *s, bool sync, FmJobInfo *out);
/* PLACE_CACHE/cloud/<hash>/<name> for an entry of an account. */
bool  cloud_cache_path(int serial, const FmCloudEntry *e, char *out, size_t cap);

/* ---- UI (fcloud_ui.c) ------------------------------------------------------------ */

struct FmPanel;

typedef struct FmCloudHooks {
  void (*activate)(int panel);
  void (*open_settings)(void);
  int  (*active)(void);
  void (*close_dialog)(void);   /* the app's own dialog (settings) gives way to a sheet */
} FmCloudHooks;

void  cloud_ui_bind(struct FmPanel *panels /* [2] */, const FmCloudHooks *h);
/* Its own sheets and dialogs; true while one is open (job questions wait). */
bool  cloud_ui_frame(void);
bool  cloud_ui_is_open(void);
void  cloud_ui_home(void);                        /* the "Cloud storage" sheet */
void  cloud_ui_add(void);                         /* pick a service */
void  cloud_ui_go(int serial, int panel);         /* an account's root in a panel */
void  cloud_ui_signin(int serial);
float cloud_settings_h(float w);
bool  cloud_settings_focus(void);                 /* once: the settings should scroll to the section */
void  cloud_settings(FmRect *r, u32 base);

/* Panel operations on cloud folders (the app routes them here). */
bool  cloud_panel_writable(const struct FmPanel *p);
void  cloud_ui_open_item(struct FmPanel *p, int item);
void  cloud_ui_transfer(bool move, struct FmPanel *src, struct FmPanel *dst);
void  cloud_ui_upload_paths(struct FmPanel *dst, char **paths, int n, bool move);
void  cloud_ui_delete(struct FmPanel *p);
bool  cloud_ui_rename(struct FmPanel *p, const char *orig, const char *name, char *err, size_t cap);
bool  cloud_ui_mkdir(struct FmPanel *p, const char *name, char *err, size_t cap);
void  cloud_ui_touched(const char *key);          /* a job changed that folder */
/* Status line: "1.2 GB of 15 GB used" for the panel's account. */
bool  cloud_ui_quota_text(const struct FmPanel *p, char *out, size_t cap, float *used);
/* Panel header/state helpers. */
FmIcon cloud_ui_icon(int serial);
void  cloud_ui_demo(const char *state);           /* --demo-cloud [STATE] */

#endif
