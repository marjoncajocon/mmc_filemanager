/* fphoto.h -- the "Online photos" screen: a photo gallery of free image sites.
**
** A full-window view like the online videos one (fonline.h): the app draws
** it in place of its panels. Search a source (fpsrc.h: Openverse, Wikimedia
** Commons, NASA, the Art Institute of Chicago, Pexels, Unsplash, Pixabay),
** browse the results as a justified photo wall, open one in a lightbox,
** keep favourites and local albums, and download in the background. The
** built-in image viewer opens over it for deep zoom, and closing the viewer
** comes back to the same wall at the same scroll position.
**
** All functions are main thread only.
*/
#ifndef FPHOTO_H
#define FPHOTO_H

#include "fui.h"

/* ---- lifetime ----------------------------------------------------------- */

/* readonly: --shot runs (nothing is written: no albums, no recent searches). */
void photo_init(bool readonly);
void photo_shutdown(void);
/* Every frame, open or not: finished searches, full images, downloads. */
void photo_pump(void);
/* Device lost: the lightbox picture is fetched again (thumbnails belong to
** the shared loader, which online_render_reset resets). */
void photo_render_reset(void);

/* What the view needs from the app (all may be NULL). */
typedef struct FmPhotoHooks {
  void (*conf_dirty)(void);              /* a setting changed: save soon */
  void (*open_settings)(void);           /* the app's settings dialog */
  void (*reveal)(const char *path);      /* "Show in folder" for a download */
} FmPhotoHooks;
void photo_set_hooks(const FmPhotoHooks *h);

/* ---- the view ----------------------------------------------------------- */

/* source: a key ("openverse", "nasa" ...) or NULL for the last one used. */
void photo_open(const char *source);
void photo_close(void);
bool photo_is_open(void);
void photo_frame(FmRect area);
/* Downloads still running (the app asks before quitting). */
int  photo_downloads_active(void);

/* ---- settings section (drawn inside the app's settings dialog) ----------- */

float photo_settings_h(float w);
void  photo_settings(FmRect *r, u32 base);
/* True once after the view asked for the settings: the dialog scrolls to
** the photos section. */
bool  photo_settings_focus(void);

/* ---- screenshots (--demo-photos SOURCE [QUERY]) -------------------------- */

/* Opens the view on `source` and searches `query` (NULL: onboarding or the
** curated page). `state` picks what the screenshot shows once results are
** in: "lightbox", "info", "zoom" (the lightbox at 3x), "select", "albums", "album", "addalbum",
** "downloads", "menu", "scroll", "favorites", "e2e" (lightbox, favourite,
** album "E2E test", download, then the image viewer; outside --shot it
** really writes), or a forced state: "nokey", "offline", "quota",
** "empty", "error", "settings". */
void photo_demo(const char *source, const char *query, const char *state);

#endif
