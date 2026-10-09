/* fview.h -- full-screen viewers: image/GIF/SVG, audio, video, text/hex.
**
** The app opens a viewer with app_open(); while open, app_frame hands it
** the whole window each frame. A viewer closes itself with
** app_close_viewer() (back button, Escape, swipe down).
**
** Design decisions:
**   - One viewer at a time, each a static module behind this table, so
**     adding a viewer touches no other code.
**   - The audio player keeps playing after its viewer closes; the app then
**     shows a mini player bar (audio_mini_*).
*/
#ifndef FVIEW_H
#define FVIEW_H

#include "fui.h"

typedef struct FmViewer {
  const char *name;
  /* list = sibling files of the same kind for next/prev (copied); may be NULL */
  bool (*open)(const char *path, const char *const *list, int n, int index);
  void (*frame)(FmRect area);
  void (*close)(void);
} FmViewer;

extern const FmViewer g_view_image;   /* fview_img.c: images, animated GIF, SVG */
extern const FmViewer g_view_audio;   /* fview_aud.c */
extern const FmViewer g_view_video;   /* fview_vid.c */
extern const FmViewer g_view_text;    /* fview_txt.c: text with hex mode */

/* Viewer for a file type, or NULL when the system should open it. */
const FmViewer *view_for(FmType t, const char *path);

/* Opens the video player on a stream (online videos): `video` is a URL or a
** local file, `audio` an optional separate sound track (URL or file) played
** with it. No file checks, no favorites or recents for it. True when the
** player took over the window. */
bool video_open_stream(const char *title, const char *video, const char *audio);

/* An online video resolved by an adapter (fvsrc.h), with its quality list:
** the player shows a quality chip, switches quality at the current position,
** shows buffering, and when a stream fails re-resolves once (expired links)
** and then falls back to downloading into the cache, on its own worker. All
** structs are copied. t_click: SDL ticks when the user pressed Play (for
** the time-to-first-frame log line), 0 = now. */
struct FmVsrc;
struct FmVsrcConf;
struct FmVsrcItem;
struct FmVsrcStream;
bool video_open_online(const struct FmVsrc *src, const struct FmVsrcConf *conf, const struct FmVsrcItem *item,
                       const struct FmVsrcStream *st, u64 t_click);

/* The next image viewer opened on a single file shows `title` in its bar
** instead of the file name (online photos open a cached file). NULL clears. */
void image_view_title(const char *title);

/* ---- background audio (mini player) ------------------------------------ */

bool audio_mini_active(void);          /* something loaded and the viewer is closed */
/* A video playing in the background (sound only, the player hidden): the
** mini bar takes the music player's place; tapping it brings the picture back. */
bool video_mini_active(void);
void video_mini_draw(FmRect r);
void video_bg_pump(void);              /* every frame: keeps a hidden video going */
void video_bg_stop(void);              /* music is starting: the background video ends */
bool video_pip_active(void);           /* the window is the small picture-in-picture one */
void audio_mini_draw(FmRect r);        /* compact bar: title, play/pause, next, close */
void audio_stop(void);
void audio_toggle_play(void);          /* play/pause what is loaded (the tray menu) */
void audio_next_track(void);

/* ---- playing URLs with metadata (online audio) -------------------------- */

/* One entry of a playlist given by the caller instead of files: the player
** copies every string, so the caller's page may be freed right after.
**   url      http(s) URL or local file; NULL/"" = resolve() it when its turn comes
**   art_url  cover / station logo, through the online thumbnail loader
**   headers  extra request lines for url ("Key: v\r\n"), or NULL
**   live     an endless station: no seek bar, no gapless next, "LIVE"
**   ref      the caller's own description of the item (opaque text), handed
**            back to the hooks below; NULL = none
**   file     url is a local file: tags and cover come from it, the heart is
**            the media library's (queued files, playlists)
**   dur      its length in seconds when known (saved queues), 0 = unknown */
typedef struct FmAudioEntry {
  const char *url, *title, *artist, *album, *art_url, *headers;
  bool live;
  const char *ref;
  bool file;
  double dur;
} FmAudioEntry;

/* What resolve() fills in for an entry without a url. */
typedef struct FmAudioStream {
  char url[2048];
  char headers[256];
  bool live;
  char err[200];           /* why not, when it returns false */
} FmAudioStream;

typedef struct FmAudioHooks {
  /* decoder thread, may block (network); polls *cancel */
  bool (*resolve)(const char *ref, FmAudioStream *out, volatile int *cancel);
  /* main thread: the heart in the player for entries with a ref */
  bool (*is_fav)(const char *ref);
  void (*fav_toggle)(const char *ref);
  /* main thread: the entry became the one heard ("Recently played") */
  void (*played)(const char *ref);
  /* main thread: an entry with a ref is about to be resolved (a queue
  ** restored after a restart, items queued from elsewhere); may be NULL */
  void (*prepare)(void);
} FmAudioHooks;
void audio_set_hooks(const FmAudioHooks *h);

/* Starts playing list[index] in the background (the mini player shows);
** Next / Previous move through the list. Replaces what was playing without
** waiting for a slow stream to give up. False when nothing could start. */
bool audio_play_entries(const FmAudioEntry *list, int n, int index);
/* Opens the full player over whatever is on screen (no-op when idle). */
void audio_show_player(void);

/* ---- the play queue ------------------------------------------------------ */

/* The queue is the player's list in play order: positions before the
** current one were heard, the ones after it are "Up next". Shuffle reorders
** the up-next part only (turning it off puts back the order from before,
** keeping what was added or removed meanwhile); repeat all goes round the
** whole queue. All main thread. */
enum { AQ_END, AQ_NEXT };
/* Adds entries at the end or right after the current track; with nothing
** loaded (or a finished queue) the first one starts playing. */
bool audio_queue_add(const FmAudioEntry *list, int n, int where);
int  audio_queue_len(void);
int  audio_queue_pos(void);            /* position of the track heard, -1 idle */
u32  audio_queue_gen(void);            /* changes with every edit and track change */

typedef struct FmAudioQInfo {
  char title[256], artist[256];
  const char *path;                    /* file path or stream URL ("" = resolved later) */
  const char *art_url;                 /* "" none */
  const char *ref;                     /* "" none */
  bool file, live;
  double dur;                          /* 0 unknown */
} FmAudioQInfo;
/* Info of the track at a position (strings valid until the next edit). */
bool audio_queue_get(int pos, FmAudioQInfo *out);
/* The whole queue as entries in play order (pointers into the player, valid
** until the next edit; fm_free the array). */
FmAudioEntry *audio_queue_entries(int *n);
void audio_queue_move(int from, int to);
void audio_queue_remove(int pos);
void audio_queue_clear(void);          /* the up-next part */
void audio_queue_jump(int pos);        /* plays that one now */
/* A new list played in a random order (shuffle turns on). */
bool audio_play_shuffled(const FmAudioEntry *list, int n);
bool audio_queue_shuffle(void);
/* Opens the full player with the queue showing. */
void audio_show_queue(void);
/* The saved queue (PLACE_CONFIG/audio-queue.txt): restored paused at the
** same track and position (nothing opens until Play); readonly: --shot. */
void audio_queue_init(bool readonly);
void audio_queue_pump(void);           /* every frame: saves a changed queue */
void audio_queue_save_now(void);       /* the app goes to the background or quits */

enum { AUDIO_IDLE, AUDIO_BUFFERING, AUDIO_PLAYING, AUDIO_PAUSED, AUDIO_FAILED };
/* State of the entry being heard; ref (may be NULL) gets its ref ("" for
** files). For the online view's "now playing" rows. */
int  audio_state(char *ref, size_t cap);

#endif
