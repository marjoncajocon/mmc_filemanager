/* fvfs.h -- what a panel lists: a local folder, a folder inside an archive,
** or a folder of a cloud account.
**
** A location (FmLoc) is a local folder, or an archive file plus the folder
** inside it, or a cloud folder (fcloud_vfs.c: path holds the chain of folder
** ids from the account's root, inner the chain of names, cloud the account's
** serial). vfs_list fills an FmListing with entries in an arena, so a
** 10 000-file folder is one allocation chain freed at once.
**
** Cloud folders list on a worker: vfs_list returns at once with `loading`
** set (and the previous items kept when the folder is the same, a refresh);
** vfs_poll fills the listing when the worker is done.
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
  bool in_cloud;              /* a cloud folder: path = ids, inner = names (fcloud_vfs.c) */
  int cloud;                  /* its account's serial (fcloud.c) */
} FmLoc;

typedef struct FmEntry {
  char *name;                 /* display name (arena) */
  u64 size;
  i64 mtime;
  u32 flags;                  /* FM_ST_* */
  u16 type;                   /* FmType */
  u8 selected;
  u8 encrypted;               /* archive entry needs a password */
  int arc_index;              /* index in the archive (cloud: in the listing's entries), -1 for local / implied folders */
} FmEntry;

typedef struct FmListing {
  FmLoc loc;
  FmArena arena;
  FmEntry *items;
  int count, cap;
  FmErr err;
  FmArc *arc;                 /* open while browsing inside an archive */
  u64 total_size;             /* files in this folder */
  /* cloud folders */
  void *cdir;                 /* the cached FmCloudList behind items (shared, counted) */
  bool loading;               /* a worker is listing loc; items are the previous ones or none */
  char errmsg[256];           /* the service's own words when err != FM_OK */
} FmListing;

void  loc_local(FmLoc *loc, const char *path);
bool  loc_up(FmLoc *loc);                       /* false at a filesystem root */
void  loc_title(const FmLoc *loc, char *out, size_t cap);    /* last component */
void  loc_display(const FmLoc *loc, char *out, size_t cap);  /* full, for the path bar */
bool  loc_equal(const FmLoc *a, const FmLoc *b);

/* Lists loc into l (which is reset first; an open archive is reused when
** loc stays inside the same archive). */
FmErr vfs_list(FmListing *l, const FmLoc *loc, bool show_hidden);
/* A cloud folder that was loading has arrived: true when l changed. */
bool  vfs_poll(FmListing *l, bool show_hidden);
void  vfs_free(FmListing *l);
/* Absolute path of a local entry. */
bool  vfs_entry_path(const FmListing *l, const FmEntry *e, char *out, size_t cap);
/* Archive entry: extract to the cache folder and return that path (blocks). */
FmErr vfs_materialize(FmListing *l, const FmEntry *e, char *out, size_t cap);

#endif
