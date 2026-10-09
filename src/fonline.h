/* fonline.h -- the "Online videos" screen: search video sites, play, download.
**
** A full-window view like the media library (flib.h): the app draws it in
** place of its panels, the video player opens on top of it, and closing the
** player comes back with the results and the scroll position intact. The
** websites themselves are adapters (fvsrc.h); this module is the shared
** gallery, the player hookup, the downloads and the settings section.
**
** All functions are main thread only.
*/
#ifndef FONLINE_H
#define FONLINE_H

#include "fui.h"

/* ---- lifetime ----------------------------------------------------------- */

/* readonly: --shot runs (nothing is written: no recent searches). */
void online_init(bool readonly);
void online_shutdown(void);
/* Every frame, open or not: finished searches, downloads, thumbnails. */
void online_pump(void);
/* the "Online thumbnails" setting changed: off drops the queue, on retries */
void othumb_forget_queue(void);
void othumb_retry_skipped(void);
/* Device lost: thumbnails are fetched again. */
void online_render_reset(void);

/* What the view needs from the app (all may be NULL). */
typedef struct FmOnlineHooks {
  void (*conf_dirty)(void);              /* a setting changed: save soon */
  void (*open_settings)(void);           /* the app's settings dialog */
  void (*reveal)(const char *path);      /* "Show in folder" for a download */
} FmOnlineHooks;
void online_set_hooks(const FmOnlineHooks *h);

/* ---- the view ----------------------------------------------------------- */

/* source: a key ("youtube", "archive" ...) or NULL for the last one used. */
void online_open(const char *source);
void online_close(void);
bool online_is_open(void);
void online_frame(FmRect area);
/* Downloads still running (the app asks before quitting). */
int  online_downloads_active(void);

/* ---- settings section (drawn inside the app's settings dialog) ----------- */

/* Height of the section for a content width w (px). */
float online_settings_h(float w);
/* Draws the section at the top of *r and cuts it off. */
void online_settings(FmRect *r, u32 base);
/* True once after the view asked for the settings: the dialog scrolls to
** the online section. */
bool online_settings_focus(void);

/* ---- screenshots (--demo-online SOURCE [QUERY]) -------------------------- */

/* Opens the view on `source` and searches `query` (NULL: onboarding). The
** state flags pick what the screenshot shows once results are in:
** "detail", "prepare", "play", "downloads", "loading", "error". */
void online_demo(const char *source, const char *query, const char *state);

#endif
