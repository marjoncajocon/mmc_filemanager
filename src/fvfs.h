/* fvfs.h -- what a panel lists: a local folder or a folder inside an archive.
**
** A location (FmLoc) is a local folder, or an archive file plus the folder
** inside it. vfs_list fills an FmListing with entries in an arena, so a
** 10 000-file folder is one allocation chain freed at once.
*/
#ifndef FVFS_H
#define FVFS_H

#include "fcore.h"
#include "fplat.h"
#include "farc.h"

typedef struct FmLoc {
  char path[FM_PATH_MAX];     /* local folder, or the archive file */
  char inner[FM_PATH_MAX];    /* folder inside the archive: "" or "a/b/" */
  bool in_arc;
} FmLoc;

typedef struct FmEntry {
  char *name;                 /* display name (arena) */
  u64 size;
  i64 mtime;
  u32 flags;                  /* FM_ST_* */
  u16 type;                   /* FmType */
  u8 selected;
  u8 encrypted;               /* archive entry needs a password */
  int arc_index;              /* index in the archive, -1 for local / implied folders */
} FmEntry;

typedef struct FmListing {
  FmLoc loc;
  FmArena arena;
  FmEntry *items;
  int count, cap;
  FmErr err;
  FmArc *arc;                 /* open while browsing inside an archive */
  u64 total_size;             /* files in this folder */
} FmListing;

void  loc_local(FmLoc *loc, const char *path);
bool  loc_up(FmLoc *loc);                       /* false at a filesystem root */
void  loc_title(const FmLoc *loc, char *out, size_t cap);    /* last component */
void  loc_display(const FmLoc *loc, char *out, size_t cap);  /* full, for the path bar */
bool  loc_equal(const FmLoc *a, const FmLoc *b);

/* Lists loc into l (which is reset first; an open archive is reused when
** loc stays inside the same archive). */
FmErr vfs_list(FmListing *l, const FmLoc *loc, bool show_hidden);
void  vfs_free(FmListing *l);
/* Absolute path of a local entry. */
bool  vfs_entry_path(const FmListing *l, const FmEntry *e, char *out, size_t cap);
/* Archive entry: extract to the cache folder and return that path (blocks). */
FmErr vfs_materialize(FmListing *l, const FmEntry *e, char *out, size_t cap);

#endif
