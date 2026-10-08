/* fops.h -- file operations as background jobs: copy, move, delete, trash,
** extract, compress, folder size, open-from-archive.
**
** Every job runs on its own worker thread and only touches its own FmJob.
** The UI reads a snapshot (ops_info) each frame and answers questions the
** worker asks (a name conflict, an archive password) through ops_question /
** ops_answer_*; the worker sleeps on a condition variable meanwhile.
**
** Design decisions:
**   - Jobs never read UI state or settings: everything they need is copied
**     from the FmJobSpec at ops_start, so the UI may change anything while
**     they run.
**   - ops_run_sync runs the same code inline with a fixed conflict policy,
**     which is what the self test (and any headless caller) uses.
**   - Workers wake the main loop with app_wake at most ~20 times a second;
**     the speed and ETA are computed on the UI side from the snapshots.
*/
#ifndef FOPS_H
#define FOPS_H

#include "fcore.h"
#include "fplat.h"
#include "farc.h"

typedef enum FmJobKind {
  JOB_COPY,       /* srcs -> dst folder (from an archive when spec.arc is set) */
  JOB_MOVE,       /* rename on one volume, else copy + delete */
  JOB_DELETE,     /* permanent */
  JOB_TRASH,      /* recycle bin / XDG trash */
  JOB_EXTRACT,    /* archives (srcs, or spec.arc) -> dst folder */
  JOB_COMPRESS,   /* srcs -> archive file spec.dst */
  JOB_SIZE,       /* count files / folders / bytes (properties) */
  JOB_OPEN        /* extract one archive entry to spec.dst (viewer cache) */
} FmJobKind;

typedef enum FmConflict {
  CONFLICT_ASK = 0, CONFLICT_OVERWRITE, CONFLICT_SKIP, CONFLICT_KEEP_BOTH, CONFLICT_CANCEL
} FmConflict;

typedef struct FmJobSpec {
  FmJobKind kind;
  /* Local absolute paths; or, when `arc` is set, names relative to
  ** arc_inner (folders included without a trailing '/'). */
  const char *const *srcs;
  int nsrc;
  const char *dst;          /* folder (copy/move/extract) or file (compress/open) */
  const char *arc;          /* archive the sources live in (copy/extract/open) */
  const char *arc_inner;    /* folder inside it: "" or "a/b/" */
  int arc_index;            /* JOB_OPEN: entry index */
  const char *base_dir;     /* JOB_COMPRESS: names are stored relative to it */
  FmArcOpts arc_opts;       /* JOB_COMPRESS (the password is copied) */
  const char *password;     /* archive password to try first (may be NULL) */
  FmConflict conflict;      /* initial policy; CONFLICT_ASK asks the UI */
  bool quiet;               /* no progress card and no toast */
} FmJobSpec;

typedef struct FmJobInfo {
  int id;
  FmJobKind kind;
  bool quiet;
  char title[192];          /* "Copying 3 items to Downloads" */
  char current[256];        /* file being worked on */
  u64 bytes_done, bytes_total;
  u64 files_done, files_total;
  u64 dirs;                 /* JOB_SIZE: folders seen */
  bool scanning;            /* still counting what to do */
  bool paused, asking, done;
  FmErr err;                /* first error, FM_ERR_CANCEL when cancelled */
  int nerrors, nskipped;
  char errmsg[320];         /* "name: Permission denied" */
  float fraction;           /* 0..1, or -1 when unknown */
  float speed;              /* bytes per second (smoothed) */
  int eta_s;                /* seconds left, -1 unknown */
  char touched[2][FM_PATH_MAX];   /* folders whose content changed */
  char result[FM_PATH_MAX];       /* JOB_OPEN: file written; JOB_COMPRESS: archive */
  /* JOB_SIZE on a single archive file */
  int arc_fmt;              /* FmArcFmt, ARC_NONE when not an archive */
  int arc_entries;
  bool arc_encrypted;
  u64 arc_unpacked;
} FmJobInfo;

typedef struct FmJob FmJob;

typedef enum FmAskKind { ASK_NONE = 0, ASK_CONFLICT, ASK_PASSWORD } FmAskKind;

typedef struct FmAsk {
  FmAskKind kind;
  char src[FM_PATH_MAX];    /* conflict: the incoming item; password: the archive */
  char dst[FM_PATH_MAX];    /* conflict: the existing item */
  FmStat src_st, dst_st;
  bool src_known;           /* src_st is valid (not for archive entries) */
  bool retry;               /* password: the previous one was wrong */
} FmAsk;

/* ---- jobs (UI thread) --------------------------------------------------- */

FmJob *ops_start(const FmJobSpec *spec);   /* NULL when too many jobs run */
int    ops_count(void);
FmJob *ops_at(int i);
void   ops_info(FmJob *j, FmJobInfo *out);  /* snapshot */
bool   ops_is_quiet(FmJob *j);
int    ops_running(bool include_quiet);
void   ops_pause(FmJob *j, bool pause);
void   ops_cancel(FmJob *j);
void   ops_cancel_all(void);

/* A job waiting for an answer (its question in *out), or NULL. */
FmJob *ops_question(FmAsk *out);
void   ops_answer_conflict(FmJob *j, FmConflict c, bool apply_all);
void   ops_answer_password(FmJob *j, const char *pw);   /* NULL = cancel */

/* Next finished job not yet reported; read it with ops_info, then ops_free. */
FmJob *ops_take_finished(void);
void   ops_free(FmJob *j);
void   ops_shutdown(void);                  /* cancels and joins every job */

/* Inline, on the calling thread; CONFLICT_ASK behaves as CONFLICT_SKIP. */
FmErr  ops_run_sync(const FmJobSpec *spec, FmJobInfo *out);

/* A fake job card for screenshots (--demo-job). */
void   ops_demo(void);

/* ---- helpers ------------------------------------------------------------ */

/* First free "name (2).ext", "name (3).ext" ... in dir. */
bool ops_unique_name(const char *dir, const char *name, char *out, size_t cap);
/* Whether JOB_TRASH can work on this platform. */
bool ops_trash_available(void);
/* Remembered archive passwords (thread safe), so listing, viewing and
** extracting the same archive asks only once. */
void ops_password_remember(const char *arc, const char *pw);
bool ops_password_lookup(const char *arc, char *out, int cap);
/* Rejects names with separators or characters the OS forbids. */
bool ops_valid_name(const char *name);

/* ---- fvfs.c extras (fvfs.h is frozen) ----------------------------------- */

/* Cache file for an archive entry: PLACE_CACHE/view/<hash>/<name>. */
bool vfs_cache_path(const char *arc, const FmArcEntry *e, char *out, size_t cap);

#endif
