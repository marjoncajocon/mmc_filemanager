/* farc_int.h -- what archive backends share (not for the rest of the app).
**
** farc.c owns FmArc, entry storage, the output side of extraction (safe
** paths, folders, mtimes) and dispatch. Each format implements a backend:
**   fzip.c  zip         f7z.c   7z          frar.c  rar 4/5
**   ftar.c  tar + tar.*  fgz.c / fbz2.c / fxz.c / fzst.c  single files
** and its writer (arc_create dispatches on FmArcOpts.fmt).
**
** A backend's extract walks entries in archive order and, for each one,
** asks the sink for an output (sink_begin), writes data through
** sink_write, and closes with sink_end. That keeps solid formats (7z, tar.gz)
** to one pass however many entries are selected.
*/
#ifndef FARC_INT_H
#define FARC_INT_H

#include "farc.h"
#include "fplat.h"

#define ARC_BUF (64 * 1024)

typedef struct FmArcSink FmArcSink;

typedef struct FmArcBackend {
  FmErr (*open)(FmArc *a);                                 /* fill entries */
  FmErr (*extract)(FmArc *a, const u8 *sel, FmArcSink *s); /* sel never NULL */
  void  (*close)(FmArc *a);                                /* free priv */
} FmArcBackend;

struct FmArc {
  FmArcFmt fmt;
  char path[FM_PATH_MAX];
  FILE *f;                    /* opened by farc.c before backend open */
  i64 file_size;
  FmArena arena;              /* entry names */
  FmArcEntry *e;
  int n, cap;
  const FmArcBackend *be;
  void *priv;                 /* backend state */
  FmArcCb cb;                 /* copy; zeroed functions when none */
  char password[256];         /* set once asked; reused for every entry */
  bool have_password;
  bool any_encrypted;
};

/* ---- used by backends while listing ------------------------------------- */

/* Appends an entry (path copied into the arena, '\' turned into '/',
** leading "./" and "/" removed). Returns its index. */
int  arc_add(FmArc *a, const char *path, u64 size, u64 packed, i64 mtime, bool is_dir);
FmArcEntry *arc_at(FmArc *a, int i);

/* Asks for the password through the callback (once; cached in a->password).
** retry=true forgets the cached one first. FM_ERR_PASSWORD if none given. */
FmErr arc_get_password(FmArc *a, bool retry);

/* ---- used by backends while extracting ---------------------------------- */

/* Returns FM_OK with *skip=false when the entry should be written; the sink
** has the output open. *skip=true: read past the data. */
FmErr sink_begin(FmArcSink *s, int index, bool *skip);
FmErr sink_write(FmArcSink *s, const void *data, size_t n);
/* status != FM_OK removes the partial file. Returns the final status. */
FmErr sink_end(FmArcSink *s, FmErr status);
/* True when the user cancelled (checked by sink_write too). */
bool  sink_cancelled(FmArcSink *s);
/* Number of selected entries still to come (lets backends stop early). */
int   sink_remaining(FmArcSink *s);

/* ---- writers (arc_create) ----------------------------------------------- */

/* One file or folder to store, found by farc.c's recursive walk. */
typedef struct FmArcSrc {
  char *abs;       /* absolute path on disk */
  char *name;      /* name in the archive, '/'-separated */
  u64 size;
  i64 mtime;
  u32 mode;
  bool is_dir;
} FmArcSrc;

typedef struct FmArcWriteCtx {
  const FmArcSrc *src;
  int n;
  u64 total;               /* sum of file sizes */
  const FmArcOpts *o;
  const FmArcCb *cb;
  u64 done;                /* updated by the writer; report with arc_wprogress */
} FmArcWriteCtx;

bool arc_wprogress(FmArcWriteCtx *w, u64 add);   /* false = cancelled */

FmErr zip_write(FILE *out, FmArcWriteCtx *w);
FmErr sevenz_write(FILE *out, FmArcWriteCtx *w);
FmErr tar_write(FILE *out, FmArcWriteCtx *w);    /* plain or compressed per o->fmt */
FmErr single_write(FILE *out, FmArcWriteCtx *w); /* .gz/.bz2/.xz/.zst/.lzma of one file */

/* ---- backends ------------------------------------------------------------ */

extern const FmArcBackend g_arc_zip, g_arc_7z, g_arc_rar, g_arc_tar, g_arc_single;

#endif
