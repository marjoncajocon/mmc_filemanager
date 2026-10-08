/* fplat.h -- the operating-system layer: files, folders, volumes, opening.
**
** Implemented by fplat_win.c (Windows) and fplat_posix.c (Linux, macOS,
** BSD, Android, web); fplat_android.c adds the Android-only pieces.
** Everything takes and returns UTF-8 paths. All functions are thread safe
** unless noted, so file operations can run on worker threads.
**
** Design decisions:
**   - Directory reading is an iterator, never a full list, so the caller
**     decides where names are stored (the panel's arena).
**   - Files are plain FILE* opened through fm_fopen, which converts the
**     path on Windows; 64-bit offsets through fm_fseek64/fm_ftell64.
*/
#ifndef FPLAT_H
#define FPLAT_H

#include "fcore.h"

/* ---- stat --------------------------------------------------------------- */

enum {
  FM_ST_DIR      = 1 << 0,
  FM_ST_LINK     = 1 << 1,   /* symlink / junction (the info is of the target) */
  FM_ST_HIDDEN   = 1 << 2,   /* dot file or hidden attribute */
  FM_ST_READONLY = 1 << 3,
  FM_ST_EXEC     = 1 << 4,
  FM_ST_SYSTEM   = 1 << 5,
  FM_ST_BROKEN   = 1 << 6,   /* dangling link */
};

typedef struct FmStat {
  u64 size;
  i64 mtime;      /* unix seconds */
  u32 flags;      /* FM_ST_* */
  u32 mode;       /* POSIX permission bits (0 on Windows) */
} FmStat;

bool plat_stat(const char *path, FmStat *st);     /* follows links */
bool plat_exists(const char *path);
bool plat_is_dir(const char *path);

/* ---- directories -------------------------------------------------------- */

typedef struct FmDir FmDir;

FmDir *plat_dir_open(const char *path, FmErr *err);
/* Next entry without "." and ".."; name points into the iterator (valid
** until the next call). Returns false at the end. */
bool   plat_dir_next(FmDir *d, const char **name, FmStat *st);
void   plat_dir_close(FmDir *d);

/* ---- changes ------------------------------------------------------------ */

FmErr plat_mkdir(const char *path);               /* one level */
FmErr plat_mkdirs(const char *path);              /* all missing levels */
FmErr plat_remove_file(const char *path);
FmErr plat_remove_dir(const char *path);          /* must be empty */
FmErr plat_rename(const char *from, const char *to);   /* same volume, fails if `to` exists */
FmErr plat_trash(const char *path);               /* recycle bin; FM_ERR_UNSUPPORTED if none */
FmErr plat_set_mtime(const char *path, i64 mtime);
FmErr plat_set_mode(const char *path, u32 mode);  /* no-op on Windows */
bool  plat_same_volume(const char *a, const char *b);

/* ---- files -------------------------------------------------------------- */

FILE *fm_fopen(const char *path, const char *mode);
int   fm_fseek64(FILE *f, i64 off, int whence);
i64   fm_ftell64(FILE *f);
i64   fm_fsize(FILE *f);                         /* -1 on error */

/* Read-only memory map of a whole file (fonts, small archives). */
void *plat_mmap(const char *path, size_t *size);
void  plat_munmap(void *p, size_t size);

/* ---- places ------------------------------------------------------------- */

typedef enum FmVolKind {
  VOL_HOME, VOL_ROOT, VOL_DRIVE, VOL_REMOVABLE, VOL_NETWORK, VOL_OPTICAL,
  VOL_INTERNAL /* Android internal storage */, VOL_SDCARD, VOL_USB, VOL_FOLDER
} FmVolKind;

typedef struct FmVolume {
  char name[64];
  char path[FM_PATH_MAX];
  FmVolKind kind;
  u64 total, free;          /* 0 when unknown */
} FmVolume;

int  plat_volumes(FmVolume *out, int max);       /* home first */
bool plat_disk_space(const char *path, u64 *total, u64 *free_bytes);

/* Common folders; false if the platform has none. */
typedef enum FmPlace {
  PLACE_HOME, PLACE_DESKTOP, PLACE_DOCUMENTS, PLACE_DOWNLOADS, PLACE_PICTURES,
  PLACE_MUSIC, PLACE_VIDEOS, PLACE_CONFIG /* our settings folder */, PLACE_CACHE,
  PLACE_TEMP
} FmPlace;
bool plat_place(FmPlace p, char *out, size_t cap);

/* ---- system ------------------------------------------------------------- */

/* Opens with the system default app (or shows the Android chooser). */
bool plat_open_external(const char *path);
/* Shows the file in the system file manager / share sheet if available. */
bool plat_share(const char *path);
void plat_random(void *buf, size_t n);           /* cryptographic */
u64  plat_now_ms(void);                          /* monotonic */
i64  plat_time_unix(void);
int  plat_cpu_count(void);

/* Rounded corners for the borderless app window. Returns 1 when the system
** compositor rounds it (Windows 11: smooth, with the native border in
** border_rgb), 2 when it is clipped to a rounded region (older Windows; the
** app draws its own outline), 0 when nothing was done. */
int plat_window_corners(bool round, int radius_px, u32 border_rgb, bool maximized);

/* Storage permission (Android 11+: all-files access). Desktop: always true. */
bool plat_storage_granted(void);
void plat_storage_request(void);

#endif
