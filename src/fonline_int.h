/* fonline_int.h -- what the online video files share: worker tasks, the
** downloads list, tool detection, recent searches and the thumbnail loader.
**
** fonline.c is the view, fonline_task.c the workers and downloads,
** fonline_thumb.c the thumbnails, fonline_set.c the settings section.
*/
#ifndef FONLINE_INT_H
#define FONLINE_INT_H

#include "fonline.h"
#include "fvsrc.h"
#include "fapp.h"
#include "fplat.h"
#include "fconf.h"

/* ---- worker tasks (fonline_task.c) -------------------------------------------- */

enum { OT_SEARCH, OT_RESOLVE, OT_DOWNLOAD, OT_INSTALL };

/* One blocking adapter call on its own thread. The main thread owns the
** struct; the worker writes the result fields and then sets `done`, and
** progress (frac, status) under `mx`. */
typedef struct OnTask {
  int kind;
  SDL_Thread *thr;
  SDL_atomic_t done;
  volatile int cancel;
  const FmVsrc *src;
  FmVsrcConf conf;
  FmVsrcItem item;
  char query[512];
  char token[256];
  char dir[FM_PATH_MAX];
  /* results */
  FmErr err;
  char errtext[512];
  FmVsrcPage page;            /* OT_SEARCH */
  FmVsrcStream stream;        /* OT_RESOLVE */
  char out[FM_PATH_MAX];      /* OT_DOWNLOAD / OT_INSTALL: the file */
  /* progress */
  SDL_mutex *mx;
  float frac;                 /* < 0 unknown */
  char status[160];
  u64 last_wake;
  u64 t0;                     /* start, ms */
} OnTask;

OnTask *otask_new(int kind, const FmVsrc *src);
bool    otask_run(OnTask *t);               /* starts the thread; false = could not */
bool    otask_done(OnTask *t);
void    otask_progress(OnTask *t, float *frac, char *status, size_t cap);
/* Joins (cancelling first when cancel is true) and frees. */
void    otask_free(OnTask *t, bool cancel);

/* ---- downloads ----------------------------------------------------------------- */

enum { DL_RUNNING, DL_DONE, DL_FAILED, DL_CANCELLED };

typedef struct OnDl {
  OnTask *task;
  FmVsrcItem item;
  const FmVsrc *src;
  int state;
  char path[FM_PATH_MAX];
  char err[256];
  float frac;
  char status[160];
} OnDl;

void  odl_start(const FmVsrc *s, const FmVsrcItem *it);
int   odl_count(void);
OnDl *odl_at(int i);                        /* newest first */
int   odl_active(void);
float odl_frac(void);                       /* combined progress of the running ones, < 0 unknown */
void  odl_cancel(int i);
void  odl_remove(int i);
void  odl_clear_finished(void);
void  odl_pump(void);                       /* main thread: progress, finished, toasts */
void  odl_shutdown(void);
/* Folder downloads go to: the setting, else the system Downloads folder. */
void  odl_dir(char *out, size_t cap);

/* ---- tools: yt-dlp, a JavaScript runtime, the installer ----------------------- */

typedef struct OnTools {
  bool ytdlp;
  char ytdlp_path[FM_PATH_MAX];
  bool js;
  char js_rt[FM_PATH_MAX];                  /* "node:<path>" */
  bool ffmpeg;                              /* ffmpeg(.exe) in conf.ffmpeg_dir */
} OnTools;

/* Cached; refresh = look again (after an install, when settings open). */
const OnTools *otools(bool refresh);
void  oinst_start(void);                    /* "Get yt-dlp" */
bool  oinst_running(float *frac, char *status, size_t cap);
void  oinst_pump(void);
void  oinst_shutdown(void);

/* ---- recent searches (PLACE_CONFIG/online.txt) -------------------------------- */

#define ORECENT_MAX 10
void  orecent_load(bool readonly);
void  orecent_add(const char *q);
void  orecent_remove(int i);
void  orecent_clear(void);
int   orecent_count(void);
const char *orecent_at(int i);              /* newest first */

/* ---- thumbnails (fonline_thumb.c) --------------------------------------------- */

void  othumb_init(void);
void  othumb_shutdown(void);
/* Main thread, once per frame: uploads decoded images, drops requests that
** were not asked for in the last frames (scrolled away). online_pump calls
** it every frame, for the photo view too: nobody else may call it. */
void  othumb_pump(void);
/* The thumbnail of url cropped to 16:9, px wide; requests it when missing.
** *fade gets the fade-in 0..1 (the caller keeps drawing while < 1). */
SDL_Texture *othumb_get(const char *url, int px, float *fade);
/* Asks for it ahead of time (lower priority than visible ones). */
void  othumb_prefetch(const char *url, int px);
/* The same for other shapes: cropped to aspect (w/h; 1 = square covers), or
** with aspect 0 the picture's own shape fitted so its long edge is px (the
** photo wall; SDL_QueryTexture tells the shape). headers: extra request
** lines the site wants (an adapter's static string, psrc_item_headers) or
** NULL. One shared LRU, budget and disk cache for both views. */
SDL_Texture *othumb_get_ex(const char *url, int px, float aspect, const char *headers, float *fade);
void  othumb_prefetch_ex(const char *url, int px, float aspect, const char *headers);
void  othumb_forget_queue(void);           /* new results: nothing queued is wanted */
void  othumb_reset(void);                   /* renderer reset: drop textures */
size_t othumb_bytes(void);

/* ---- shared helpers (fonline.c) ------------------------------------------------- */

void  online_conf_dirty(void);
void  online_open_settings(void);
void  online_reveal(const char *path);
/* "1.2M views", "3 years ago", "12:34" */
void  ofmt_views(i64 v, char *out, size_t cap);
void  ofmt_age(const char *iso, char *out, size_t cap);
void  ofmt_dur(double sec, char *out, size_t cap);
/* "Channel · 1.2M views · 3 years ago" */
void  ofmt_meta(const FmVsrcItem *it, char *out, size_t cap);

#endif
