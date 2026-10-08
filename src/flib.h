/* flib.h -- the media library: songs and videos, favorites, recently played.
**
** The library indexes the platform Music and Videos folders plus any folders
** the user adds, in a background thread, and shows them in a full-screen
** view (flib_ui.c) with Songs, Artists, Albums, Videos, Favorites, Recent
** and Folders sections. Favorites and recents are plain paths, so any media
** file can be starred, also one outside the library folders.
**
** Design decisions:
**   - The index is one string pool (u32 offsets) plus a 40-byte struct per
**     file; sorted views and the path hash are int arrays built on the main
**     thread whenever a scan publishes. No per-file allocations.
**   - The scanner builds its own copy and hands finished snapshots to the
**     main thread (lib_pump), so the UI never waits and never locks while
**     drawing. Tags already read are reused from the previous index and an
**     on-disk cache when path, size and mtime match.
**   - Favorites live in insertion order with a sorted index next to them:
**     lib_is_fav is a binary search, cheap enough for every frame.
**   - Everything persistent is in PLACE_CONFIG/library.txt (folders,
**     favorites, recents), one "key path" per line; the tag cache is
**     PLACE_CACHE/library-index.txt and may be deleted at any time.
**   - All functions are main thread only.
*/
#ifndef FLIB_H
#define FLIB_H

#include "fui.h"

/* ---- for the players ---------------------------------------------------- */

bool lib_is_fav(const char *path);           /* binary search; fine every frame */
void lib_fav_toggle(const char *path);
void lib_fav_set(const char *path, bool on);
void lib_note_played(const char *path);      /* most recent first, capped */

/* ---- lifetime ----------------------------------------------------------- */

/* Loads library.txt (small; the index is only scanned once the library is
** opened). readonly: --shot runs, nothing is written. */
void lib_init(bool readonly);
void lib_shutdown(void);
/* Main thread, every frame: takes scan results, saves pending changes. */
void lib_pump(void);

/* ---- folders ------------------------------------------------------------ */

bool lib_is_media(const char *path);         /* audio or video by extension */
bool lib_has_folder(const char *dir);        /* dir is one of the sources */
/* Adds or removes a source folder (removing a default one hides it). */
void lib_folder_set(const char *dir, bool on);
/* Effective sources (defaults that exist and are not hidden, then added ones). */
int  lib_folder_count(void);
const char *lib_folder_at(int i, bool *is_default);
/* Default folders the user hid, for "add folder" offers. */
int  lib_hidden_count(void);
const char *lib_hidden_at(int i);

/* ---- scanning ----------------------------------------------------------- */

void lib_rescan(void);                       /* starts (or restarts) a scan */
bool lib_scanning(void);
int  lib_scan_found(void);                   /* files seen by the running scan */
void lib_scan_wait(void);                    /* blocks until the scan ends (tests, demo) */

/* ---- index (read by the view; valid until the next lib_pump) ------------- */

enum { LIB_AUDIO = 0, LIB_VIDEO = 1 };

typedef struct FmLibItem {
  u32 path, title, artist, album;            /* offsets into the pool; 0 = "" */
  i64 mtime;
  u64 size;
  u8 kind;                                   /* LIB_AUDIO / LIB_VIDEO */
  u8 tagged;                                 /* tags were read (or there are none) */
} FmLibItem;

typedef struct FmLibGroup {
  u32 name;                                  /* artist or album (pool offset) */
  int first, count;                          /* range in by_artist / by_album */
} FmLibGroup;

typedef struct FmLibData {
  const char *pool;
  const FmLibItem *items;
  int n;
  const int *songs; int nsongs;              /* audio by title */
  const int *videos; int nvideos;            /* video by name */
  const int *by_artist; const FmLibGroup *artists; int nartists;
  const int *by_album; const FmLibGroup *albums; int nalbums;
  u32 gen;                                   /* changes with every update */
} FmLibData;

const FmLibData *lib_data(void);
const char *lib_str(u32 off);
int  lib_find(const char *path);             /* item index or -1 (hash lookup) */
/* Display title: the tag, else the file name without extension. */
void lib_item_title(const FmLibItem *it, char *out, size_t cap);

int  lib_fav_count(void);
const char *lib_fav_at(int i);               /* newest first */
int  lib_recent_count(void);
const char *lib_recent_at(int i);            /* newest first */
void lib_recent_clear(void);
/* Drops favorites and recents whose file is gone (the folder still exists,
** so files on an unplugged drive are kept). Returns how many were removed. */
int  lib_prune_missing(void);

/* ---- the library view (flib_ui.c) --------------------------------------- */

enum {
  LIB_SEC_SONGS, LIB_SEC_ARTISTS, LIB_SEC_ALBUMS, LIB_SEC_VIDEOS, LIB_SEC_FAVS, LIB_SEC_RECENT,
  LIB_SEC_FOLDERS, LIB_SEC_COUNT
};

/* hint_dir: the active panel's folder, offered by "Add folder". */
void lib_ui_open(int section, const char *hint_dir);
void lib_ui_close(void);
bool lib_ui_is_open(void);
/* Draws the whole window; the app calls it instead of its panels. */
void lib_ui_frame(FmRect area);
/* "Show in folder": the app closes the library and selects the file. */
void lib_ui_set_reveal(void (*fn)(const char *path));
/* Section by name ("songs", "videos", "favorites" ...) or -1. */
int  lib_ui_section(const char *name);

/* ---- self test and --demo-library --------------------------------------- */

/* Forgets everything and keeps state in `dir` (library.txt and the cache)
** with no default folders. NULL dir: back to normal. */
void lib_test_reset(const char *dir, bool readonly);
/* Demo for screenshots: `dir` as the only source, scanned synchronously,
** a few favorites and recents, then the view opens on `section`. */
void lib_demo(const char *dir, int section);

#endif
