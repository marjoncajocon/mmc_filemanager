/* fqueue.h -- the music play queue and saved playlists: the model, the
** files they live in, and their views.
**
** The music player (fview_aud.c) owns the queue itself (what plays now and
** what comes next); this module has what does not need the player: the
** index math on a play order, one line of text per track for the files,
** the saved queue, the named playlists, .m3u8 import and export, and the
** views (fqueue_ui.c): the Queue panel inside the player, the Playlists
** section of the media library, and the "Add to playlist" picker.
**
** Design decisions:
**   - A track is an FmAudioEntry (fview.h) everywhere: a local file (file =
**     true, url = its path) or an online item (url empty, ref = what the
**     online audio view needs to resolve it again when its turn comes). So
**     one queue and one playlist can mix both, and a playlist saved today
**     plays tomorrow even though stream links expire.
**   - Items with request headers (cloud files: signed, short-lived links
**     and tokens) are not saved: a token does not belong in a text file
**     and the link would not work tomorrow anyway.
**   - The play order is a plain int array of slots; the index math (insert,
**     move, remove, shuffle the part after the current track, undo the
**     shuffle) is pure functions here, so the self test checks it without
**     a sound device.
**   - Files: PLACE_CONFIG/audio-queue.txt (the queue, rewritten a couple of
**     seconds after a change and when the app goes away) and
**     PLACE_CONFIG/audio-playlists.txt (all playlists in one small file).
**     Both are written to a .tmp and swapped in, so a crash never leaves
**     half a file.
*/
#ifndef FQUEUE_H
#define FQUEUE_H

#include "fview.h"

/* ---- play order index math (pure; positions are 0-based) ---------------- */

int  q_find(const int *a, int n, int v);
/* Inserts v[0..k) at `at` (clamped); the array has room for n + k. */
void q_insert(int *a, int *n, int at, const int *v, int k);
/* Removes a[at]; returns where `cur` is afterwards: it moves down by one
** when an earlier item goes, and stays on the same position (now the next
** item) when the current one goes; -1 when nothing is left there. */
int  q_remove(int *a, int *n, int at, int cur);
/* Moves a[from] to position `to` (both clamped); returns where `cur` is
** afterwards. */
int  q_move(int *a, int n, int from, int to, int cur);
/* Shuffles a[from..n) in place (Fisher-Yates on a small LCG from `seed`). */
void q_shuffle(int *a, int n, int from, u32 seed);
/* Shuffle off: the items of `order` in the order of `nat` (the order from
** before shuffling), then any item `nat` does not have (added since), in
** their current order. out has room for n. */
void q_unshuffle(const int *nat, int nn, const int *order, int n, int *out);

/* ---- one track per line -------------------------------------------------- */

/* A track with its own strings (heap). */
typedef struct FmQItem {
  char *url, *title, *artist, *album, *art, *ref;
  bool live, file;
  double dur;
} FmQItem;

/* "F|E \t live \t dur \t url \t title \t artist \t album \t art \t ref", the
** fields escaped (\\ \t \n \r). False when the entry is not worth saving
** (headers, or nothing to play it from). */
bool qline_make(const FmAudioEntry *e, char *out, size_t cap);
/* Parses a line made by qline_make into it (strings allocated). */
bool qline_parse(const char *line, FmQItem *it);
void qitem_free(FmQItem *it);
void qitem_copy(FmQItem *dst, const FmAudioEntry *src);
/* A view of the item as an entry (pointers into it). */
FmAudioEntry qitem_entry(const FmQItem *it);

/* ---- the saved queue ------------------------------------------------------ */

typedef struct FmQSaved {
  FmQItem *items;             /* in play order */
  int n;
  int cur;                    /* the track that was playing */
  double pos;                 /* seconds into it */
  bool shuffle;
  int repeat;
} FmQSaved;

/* Writes the queue (in play order) to `path`; n == 0 removes the file. */
bool qsave_write(const char *path, const FmAudioEntry *list, int n, int cur, double pos, bool shuffle, int repeat);
bool qsave_read(const char *path, FmQSaved *out);
void qsave_free(FmQSaved *s);
/* PLACE_CONFIG/audio-queue.txt */
bool qsave_path(char *out, size_t cap);

/* ---- tracks from files ----------------------------------------------------- */

/* Entries for local paths: audio files as they are, folders as their audio
** files (subfolders too, in name order, at most `max` in all); titles from
** the media library's index when it knows the file. Free with qents_free. */
FmAudioEntry *qents_from_paths(const char *const *paths, int n, int max, int *out_n);
void qents_free(FmAudioEntry *e, int n);
/* A deep copy of entries (strings duplicated); free with qents_free. */
FmAudioEntry *qents_dup(const FmAudioEntry *e, int n);

/* ---- playlists ---------------------------------------------------------------- */

#define PL_NAME_MAX 120

/* Loads PLACE_CONFIG/audio-playlists.txt (readonly: --shot, never written). */
void pl_init(bool readonly);
void pl_shutdown(void);
void pl_pump(void);                           /* writes a changed file */
/* Self test: another file (NULL: back to normal), memory cleared. */
void pl_test_file(const char *path);
void pl_flush(void);                          /* writes now if changed */

int  pl_count(void);
const char *pl_name(int p);
int  pl_len(int p);
const FmQItem *pl_item(int p, int k);
double pl_duration(int p);                    /* known lengths added up */
u32  pl_gen(void);                            /* changes with every edit */
int  pl_find(const char *name);               /* case-insensitive, -1 */
/* A free name like "Playlist 3" (or base, base 2 ...). */
void pl_unique_name(const char *base, char *out, size_t cap);

int  pl_create(const char *name);             /* index of the new one (at the top), -1 */
bool pl_rename(int p, const char *name);
void pl_delete(int p);
/* Adds entries at the end; returns how many were taken. */
int  pl_add(int p, const FmAudioEntry *e, int n);
void pl_move(int p, int from, int to);
void pl_remove(int p, int k);
/* The playlist's tracks as entries (pointers into it; fm_free the array). */
FmAudioEntry *pl_entries(int p, int *n);
bool pl_local_only(int p);                    /* every track is a file */

/* .m3u8: writes the playlist's files (#EXTINF lines) to `path`. */
bool pl_export_m3u(int p, const char *path);
/* .m3u / .m3u8: a new playlist named after the file with its tracks (local
** paths relative to the list, http(s) streams); -1 when nothing was read. */
int  pl_import_m3u(const char *path);

/* ---- views (fqueue_ui.c) ------------------------------------------------------ */

/* The Queue panel the player draws in its side column or over the cover. */
void qui_queue_panel(FmRect r);
/* "Add to playlist": a menu of the playlists plus "New playlist...";
** entries are copied. */
void qui_pick_playlist(const FmAudioEntry *e, int n, float x, float y);
/* Same for local paths (folders become their audio files). */
void qui_pick_playlist_paths(const char *const *paths, int n, float x, float y);
/* Queue local paths: next = right after the current track. */
void qui_queue_paths(const char *const *paths, int n, bool next);
/* "Save queue as playlist..." */
void qui_save_queue(void);
/* The library's Playlists section (list, or one playlist opened), drawn in
** its body card; back: true when it went back a level itself. */
void qui_playlists(FmRect body);
bool qui_playlists_back(void);
void qui_playlists_reset(void);
/* Menus, dialogs and the picker; the app calls it every frame on top of
** whatever is shown (inside its dialog pass). */
void qui_overlay(void);
/* Screenshots (--demo-queue-state): "drag" holds the second up-next row
** half way down, "playlist" opens the first playlist. */
void qui_demo(const char *state);

#endif
