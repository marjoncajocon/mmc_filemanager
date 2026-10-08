/* fphoto_int.h -- what the online photo files share: the wall, worker tasks,
** downloads, recent searches, albums and the lightbox.
**
** fphoto.c is the view (sources, search, states, selection), fphoto_wall.c
** the justified photo wall, fphoto_box.c the lightbox, fphoto_album.c the
** favourites and albums (store, covers page, dialogs), fphoto_task.c the
** workers, downloads and recent searches, fphoto_set.c the settings section.
** Thumbnails come from the online videos' loader (fonline_thumb.c).
*/
#ifndef FPHOTO_INT_H
#define FPHOTO_INT_H

#include "fphoto.h"
#include "fpsrc.h"
#include "fapp.h"
#include "fplat.h"
#include "fconf.h"
#include "fdec_img.h"
#include "fonline_int.h"

#define PH_WHITE FM_RGBA(255, 255, 255, 255)
#define PH_BLACK FM_RGBA(0, 0, 0, 255)

/* ---- items ------------------------------------------------------------------- */

/* A photo on a wall: the adapter's item and where it came from. */
typedef struct PhItem {
  FmPsrcItem it;
  char src[24];               /* source key ("openverse" ...) */
  float ar;                   /* aspect the layout uses; 0 = not known yet (4:3) */
  int tpx;                    /* thumbnail size it was last drawn at (the lightbox reuses it) */
  bool sel;
} PhItem;

void  ph_item_init(PhItem *p, const FmPsrcItem *it, const char *src);
const FmPsrc *ph_src(const PhItem *p);
const char *ph_src_name(const char *key);

/* ---- the wall (fphoto_wall.c) ---------------------------------------------------- */

typedef struct PhRow {
  int first, count;
  float y, h;
  bool full;                  /* justified to the width (the last row may not be) */
} PhRow;

typedef struct PhWall {
  PhItem *items;
  int n, cap;
  PhRow *rows;
  int nrows, rcap;
  float content;              /* height of the rows, padding included */
  float lay_w, lay_target;    /* what the rows were made for */
  int lay_n;
  bool dirty;                 /* an aspect changed: lay out again, anchored */
  FmScroll scroll;
  u32 id;
  int anchor;                 /* shift-click range start */
} PhWall;

typedef struct PhWallOpts {
  int skeleton;               /* placeholder tiles after the items (loading) */
  float footer;               /* space below the rows */
  bool fixed;                 /* placeholders only: no scrolling */
} PhWallOpts;

typedef struct PhWallRes {
  int open;                   /* tile tapped: open the lightbox, -1 none */
  int menu;                   /* right click / long press without selection, -1 none */
  float menu_x, menu_y;
  float footer_y;             /* where the footer goes (screen px) */
  bool near_end;              /* the end is within a screen and a half: load more */
} PhWallRes;

void  wall_init(PhWall *w, const char *name);
void  wall_free(PhWall *w);
void  wall_clear(PhWall *w);                /* items and scroll */
PhItem *wall_push(PhWall *w);
int   wall_find(const PhWall *w, const char *src, const char *id);
int   wall_nsel(const PhWall *w);
void  wall_select_all(PhWall *w, bool on);
PhWallRes wall_draw(PhWall *w, FmRect view, const PhWallOpts *o);
/* The row layout on its own (the self test): n aspects into at most max
** rows of width W; returns the rows made. */
int   wall_justify(const float *ars, int n, float W, float target, float gap, PhRow *out, int max);
/* Gap and corner radius of the wall at this width (the covers page matches). */
float wall_pad(float view_w);
/* The picture of a tile: the dominant colour, then the thumbnail fading in.
** Returns the thumbnail when drawn. */
SDL_Texture *ph_tile_picture(FmRect r, PhItem *p, float rad, bool learn_aspect, PhWall *w);
float ph_shimmer(int k);

/* ---- worker tasks (fphoto_task.c) -------------------------------------------------- */

enum { PT_SEARCH, PT_FETCH, PT_DOWNLOAD };

/* One blocking adapter call on its own thread, as OnTask in the video view. */
typedef struct PhTask {
  int kind;
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  const FmPsrc *src;
  FmPsrcConf conf;
  FmPsrcItem item;            /* PT_FETCH / PT_DOWNLOAD (details() may fill it in) */
  char query[512];
  char token[256];
  char dir[FM_PATH_MAX];      /* PT_DOWNLOAD */
  int max_px;                 /* PT_FETCH: decode to fit this long edge (0 = only fetch) */
  u32 max_pixels;             /* PT_FETCH: and to at most this many pixels (0 = no limit) */
  /* results */
  FmErr err;
  char errtext[512];
  FmPsrcPage page;            /* PT_SEARCH */
  char out[FM_PATH_MAX];      /* PT_FETCH / PT_DOWNLOAD: the file */
  FmImage img;                /* PT_FETCH: decoded */
  FmImgInfo info;             /* PT_FETCH: the file's own size (before fitting) */
  /* progress */
  SDL_mutex *mx;
  float frac;                 /* < 0 unknown */
  u64 last_wake;
} PhTask;

PhTask *ptask_new(int kind, const FmPsrc *src);
bool    ptask_run(PhTask *t);
bool    ptask_done(PhTask *t);
float   ptask_frac(PhTask *t);
void    ptask_free(PhTask *t, bool cancel);
/* Abandons a task without waiting: it is cancelled and joined once its
** thread returns (ptask_reap, every frame). */
void    ptask_drop(PhTask *t);
void    ptask_reap(void);
void    ptask_shutdown(void);

/* ---- downloads ------------------------------------------------------------------------ */

enum { PD_QUEUED, PD_RUNNING, PD_DONE, PD_FAILED, PD_CANCELLED };

typedef struct PhDl {
  PhTask *task;
  FmPsrcItem item;
  char src[24];
  int state;
  char path[FM_PATH_MAX];
  char err[200];
  float frac;
  bool quiet;                 /* part of a batch: one toast for all */
  int tries;                  /* a failed one is queued again once */
} PhDl;

/* Queues a download; at most three run at once. quiet: no toast (batches). */
void  pdl_start(const PhItem *p, bool quiet);
void  pdl_batch(const PhItem *items, int n);   /* several, one toast */
int   pdl_count(void);
PhDl *pdl_at(int i);                          /* newest first */
int   pdl_active(void);                       /* queued or running */
float pdl_frac(void);
void  pdl_cancel(int i);
void  pdl_remove(int i);
void  pdl_clear_finished(void);
void  pdl_retry(int i);
void  pdl_pump(void);
void  pdl_shutdown(void);

/* ---- recent searches (PLACE_CONFIG/photo-recent.txt) ----------------------------- */

#define PRECENT_MAX 10
void  precent_load(bool readonly);
void  precent_add(const char *q);
void  precent_clear(void);
int   precent_count(void);
const char *precent_at(int i);

/* ---- albums (fphoto_album.c) ------------------------------------------------------- */

#define PALB_FAV 0                            /* album 0 is Favorites */

void  palb_load(bool readonly);
void  palb_pump(void);                        /* saves a changed store */
void  palb_shutdown(void);
int   palb_count(void);
const char *palb_name(int a);
int   palb_size(int a);
/* Index of the photo in album a, or -1. */
int   palb_find(int a, const char *src, const char *id);
bool  palb_add(int a, const PhItem *p);        /* false when already there */
void  palb_remove(int a, const char *src, const char *id);
void  palb_get(int a, int i, PhItem *out);
const char *palb_cover(int a);                /* thumbnail URL of the newest photo, "" */
int   palb_create(const char *name);          /* index, or -1 */
bool  palb_is_fav(const PhItem *p);
void  palb_fav_toggle(const PhItem *p);
void  palb_download(int a);
int   palb_find_name(const char *name);

/* The pages and dialogs. */
void  palb_page(FmRect body);                 /* the covers of every album */
void  palb_pick_open(const PhItem *items, int n);   /* "Add to album" */
void  palb_new_open(void);
void  palb_rename_open(int a);
void  palb_delete_open(int a);
void  palb_album_menu(int a, float x, float y);
bool  palb_dialogs(void);                     /* draws the open dialog; true when one is open */
void  palb_menu_results(void);
void  palb_demo(const PhItem *items, int n);  /* in-memory albums for screenshots */
void  palb_test_file(const char *path);       /* self test: another file instead of the user's */

/* ---- lightbox (fphoto_box.c) ------------------------------------------------------------ */

void  box_open(PhWall *w, int index);
void  box_close(void);
bool  box_is_open(void);
void  box_frame(FmRect area);
void  box_pump(void);
void  box_reset(void);
void  box_shutdown(void);
void  box_set_info(bool on);
void  box_view_full(void);                    /* "Open in viewer" (now or once fetched) */
void  box_menu_results(void);
void  box_demo_zoom(float s);                 /* screenshots: zoom the open photo (1 = fit) */

/* Lightbox zoom, kept relative to the fitted picture so a sharper texture
** arriving later changes nothing on screen. s: 1 = fit; px, py: offset of
** the picture's centre from the fitted centre (screen px). Pure math, also
** used by the self test. */
typedef struct PhZoom {
  float s, px, py;
} PhZoom;

/* Where the picture is drawn, given its fitted rect. */
FmRect pzoom_rect(const PhZoom *z, FmRect fit);
/* Zooms to `to` keeping the screen point (ax, ay) still. */
void  pzoom_at(PhZoom *z, FmRect fit, float to, float ax, float ay);
/* Keeps the picture over `view`: a side larger than the view cannot leave
** a gap, a smaller side is centred. */
void  pzoom_clamp(PhZoom *z, FmRect fit, FmRect view);
/* Double tap: from fit to 100% (`one` = the scale of actual pixels, or 2x
** when that is barely larger), from anything else back to fit. */
float pzoom_toggle(float s, float one, float smax);
/* Largest zoom: 4x the fit, or 4x actual pixels when that is more (at most 32x). */
float pzoom_max(float one);

/* ---- the view (fphoto.c) ------------------------------------------------------------------ */

enum { PG_SOURCE, PG_ALBUMS, PG_ALBUM };

void  ph_conf_dirty(void);
void  ph_open_settings(void);
void  ph_reveal(const char *path);
void  ph_open_url(const char *url);
void  ph_copy_link(const PhItem *p);
/* The lightbox reached the end of the results: the next page, if any. */
void  ph_need_more(PhWall *w);
/* An album's photos changed: the open album page follows. */
void  ph_album_changed(int a);
void  ph_album_removed(int a);              /* album a was deleted: later ones moved down */
void  ph_show_album(int a);
void  ph_show_albums(void);
void  ph_show_source(void);
int   ph_page(void);
int   ph_cur_album(void);
/* Text in up to `lines` lines, the last one ellipsized; returns the lines used. */
int   ph_text_lines(int face, float size, float x, float y, float w, const char *s, int lines, FmColor c,
                    bool draw);
bool  ph_chip(u32 id, FmRect c, FmIcon ic, const char *label, bool on);
float ph_chip_w(FmIcon ic, const char *label, float max);
/* "by Author  ·  CC BY 4.0  ·  Openverse" */
void  ph_meta(const PhItem *p, char *out, size_t cap, bool with_source);
/* Centred icon, title, text and buttons for empty pages. Returns the button
** pressed (0..n-1) or -1. */
int   ph_state_view(FmRect body, FmIcon ic, FmColor icol, const char *title, const char *sub, const char *detail,
                    const char *const *labels, const FmIcon *icons, int n);

#endif
