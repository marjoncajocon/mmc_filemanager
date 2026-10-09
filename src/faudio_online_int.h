/* faudio_online_int.h -- what the online audio files share: item lists,
** worker tasks, downloads, recent searches, the library and the player
** hookup.
**
** faudio_online.c is the view (sources, search, browse, pages, states),
** faudio_online_list.c draws the lists (station tiles, track rows, cover
** cards, the podcast / album header), faudio_online_task.c runs the
** workers, downloads, the library file and the player hooks,
** faudio_online_set.c is the settings section. Artwork comes from the
** online videos' thumbnail loader (fonline_thumb.c).
*/
#ifndef FAUDIO_ONLINE_INT_H
#define FAUDIO_ONLINE_INT_H

#include "faudio_online.h"
#include "fasrc.h"
#include "fapp.h"
#include "fplat.h"
#include "fconf.h"
#include "fview.h"
#include "fonline_int.h"
#include "fqueue.h"

#define AO_WHITE FM_RGBA(255, 255, 255, 255)
#define AO_REF_MAX 4096           /* a serialized item */
#define AO_PLAY_MAX 300           /* entries handed to the player at once */

/* ---- item lists ------------------------------------------------------------------ */

typedef struct AoList {
  FmAsrcItem *items;
  int n, cap;
  FmScroll scroll;
  u32 id;
} AoList;

void  aol_init(AoList *l, const char *name);
void  aol_free(AoList *l);
void  aol_clear(AoList *l);                  /* items and scroll */
FmAsrcItem *aol_push(AoList *l);
int   aol_find(const AoList *l, const char *src, const char *id);

/* "radio\t1\t<id>": what identifies an item across pages and the library */
void  aitem_key(const FmAsrcItem *it, char *out, size_t cap);
bool  aitem_same(const FmAsrcItem *a, const FmAsrcItem *b);
bool  aitem_playable(const FmAsrcItem *it);  /* a track or a station (not a container) */
bool  aitem_container(const FmAsrcItem *it);
/* One line of text for the library file and the player's ref (tabs and
** line breaks escaped); parse fills a cleared item. */
void  aitem_ref(const FmAsrcItem *it, char *out, size_t cap);
bool  aitem_parse(const char *ref, FmAsrcItem *out);
/* The key of a ref without parsing all of it. */
void  aref_key(const char *ref, char *out, size_t cap);
const char *ao_src_name(const char *key);
FmIcon ao_src_icon(const FmAsrc *s);

/* ---- worker tasks (faudio_online_task.c) --------------------------------------------- */

enum { AT_SEARCH, AT_BROWSE, AT_CATS, AT_CHILDREN, AT_DOWNLOAD };

#define AO_MAX_CATS 40

typedef struct AoTask {
  int kind;
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  const FmAsrc *src;
  FmAsrcConf conf;
  FmAsrcItem item;            /* AT_CHILDREN / AT_DOWNLOAD */
  char query[512];
  char token[256];
  char cat[64];
  char dir[FM_PATH_MAX];
  /* results */
  FmErr err;
  char errtext[256];
  FmAsrcPage page;
  FmAsrcCat cats[AO_MAX_CATS];
  int ncats;
  char out[FM_PATH_MAX];      /* AT_DOWNLOAD: the file */
  /* progress */
  SDL_mutex *mx;
  float frac;
  u64 last_wake;
} AoTask;

AoTask *atask_new(int kind, const FmAsrc *src);
bool    atask_run(AoTask *t);
bool    atask_done(AoTask *t);
float   atask_frac(AoTask *t);
void    atask_free(AoTask *t, bool cancel);
/* Abandons a task without waiting; joined later by atask_reap (every frame). */
void    atask_drop(AoTask *t);
void    atask_reap(void);
void    atask_shutdown(void);

/* ---- downloads ------------------------------------------------------------------------- */

enum { AD_QUEUED, AD_RUNNING, AD_DONE, AD_FAILED, AD_CANCELLED };

typedef struct AoDl {
  AoTask *task;
  FmAsrcItem item;
  int state;
  char path[FM_PATH_MAX];
  char err[200];
  float frac;
  int tries;
} AoDl;

void  adl_start(const FmAsrcItem *it);
int   adl_count(void);
AoDl *adl_at(int i);                          /* newest first */
int   adl_active(void);
float adl_frac(void);
bool  adl_has(const FmAsrcItem *it);          /* queued, running or saved */
void  adl_cancel(int i);
void  adl_remove(int i);
void  adl_retry(int i);
void  adl_clear_finished(void);
void  adl_pump(void);
void  adl_shutdown(void);

/* ---- recent searches (PLACE_CONFIG/audio-recent.txt) ----------------------------------- */

#define ARECENT_MAX 10
void  arecent_load(bool readonly);
void  arecent_add(const char *q);
void  arecent_clear(void);
int   arecent_count(void);
const char *arecent_at(int i);

/* ---- the library (PLACE_CONFIG/audio-library.txt) -------------------------------------- */

enum { ALIB_FAV, ALIB_RECENT, ALIB_SUBS, ALIB_COUNT };

void  alib_load(bool readonly);
void  alib_pump(void);                        /* saves a changed library */
void  alib_shutdown(void);
int   alib_count(int lib);
bool  alib_get(int lib, int i, FmAsrcItem *out);   /* newest first */
bool  alib_has(int lib, const FmAsrcItem *it);
bool  alib_has_key(int lib, const char *key);
void  alib_add(int lib, const FmAsrcItem *it);     /* to the front (recent: moves it there) */
void  alib_remove(int lib, const FmAsrcItem *it);
/* Favourite / subscribe toggles with a toast; return the new state. */
bool  alib_toggle(int lib, const FmAsrcItem *it);
u32   alib_version(void);                     /* changes with every edit */
void  alib_test_file(const char *path);       /* self test: another file */

/* ---- the player (faudio_online_task.c) ----------------------------------------------- */

/* Registers the player hooks (resolve, favourites, recently played). */
void  aplay_init(void);
/* The player's resolve hook: a ref to a stream (asrc_stream), any thread. */
bool  aplay_resolve(const char *ref, FmAudioStream *out, volatile int *cancel);
/* Takes the settings resolve() uses (main thread; aplay_list does it). */
void  aplay_snapshot(void);
/* Plays items[index] with the playable items of the list as the queue. */
void  aplay_list(const FmAsrcItem *items, int n, int index);
/* One item as a player entry: its ref (AO_REF_MAX) and second line (512)
** are written into the caller's buffers, which the entry points to. */
void  aplay_entry(const FmAsrcItem *it, FmAudioEntry *e, char *ref, char *who);
/* "Play next" / "Add to queue" with a toast; "Add to playlist..." picker. */
void  aplay_queue(const FmAsrcItem *it, bool next);
void  aplay_pick_playlist(const FmAsrcItem *it, float x, float y);
/* What the player is on, for the rows: AUDIO_* and its key ("" when idle
** or not an online item). Read once per frame. */
int   aplay_state(char *key, size_t cap);

/* ---- drawing the lists (faudio_online_list.c) ------------------------------------------- */

enum { AL_ROWS, AL_STATIONS, AL_COVERS };

typedef struct AoListOpts {
  int layout;                 /* AL_* */
  int skeleton;               /* placeholder tiles / rows (loading) */
  bool fixed;                 /* placeholders only: no scrolling */
  float header;               /* room above the items (the detail header scrolls with them) */
  float footer;
  bool episodes;              /* rows show the date (podcast episodes) */
  bool numbered;              /* rows show the track number (album) */
  const char *play_key;       /* what the player is on */
  int play_state;             /* AUDIO_* */
} AoListOpts;

typedef struct AoListRes {
  int play;                   /* play this one, -1 none */
  int open;                   /* open a container (podcast, album) */
  int menu;                   /* the More menu (or right click / long press) */
  float menu_x, menu_y;
  int fav, dl;                /* heart, download buttons */
  bool toggle;                /* the playing row's button: pause / resume */
  float header_y;             /* where the header goes (screen px) */
  float footer_y;
  bool near_end;              /* load more */
} AoListRes;

int   ao_layout_for(const AoList *l);
AoListRes ao_list_draw(AoList *l, FmRect view, const AoListOpts *o);
/* Artwork: the picture, else a letter avatar in a colour from the name. */
void  ao_art(FmRect r, const FmAsrcItem *it, float rad);
void  ao_avatar(FmRect r, const char *name, float rad, bool round);
/* The animated "now playing" bars (moving only while playing). */
void  ao_bars(FmRect r, FmColor c, bool moving);
float ao_shimmer(int k);
/* Wakes the app once in ms, with at most one timer pending per `t`.
** view_wake_in shares one flag for the whole app and re-arms whenever it
** is clear, so two periodic users (the playing row, the mini player's
** clock) can end up with parallel timer chains that add up to 60 frames a
** second; a ticker per purpose never has more than one. */
void  ao_tick(SDL_atomic_t *t, u32 ms);
/* "MP3 128" */
void  ao_badge_text(const FmAsrcItem *it, char *out, size_t cap);
/* "3:25", "1:02:03" */
void  ao_fmt_dur(double sec, char *out, size_t cap);

/* ---- the view (faudio_online.c) -------------------------------------------------------- */

void  ao_conf_dirty(void);
void  ao_open_settings(void);
void  ao_reveal(const char *path);
void  ao_open_url(const char *url);
/* Text in up to `lines` lines, the last one ellipsized; returns the lines used. */
int   ao_text_lines(int face, float size, float x, float y, float w, const char *s, int lines, FmColor c, bool draw);
bool  ao_chip(u32 id, FmRect c, FmIcon ic, const char *label, bool on);
float ao_chip_w(FmIcon ic, const char *label, float max);

#endif
