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

/* ---- background audio (mini player) ------------------------------------ */

bool audio_mini_active(void);          /* something loaded and the viewer is closed */
void audio_mini_draw(FmRect r);        /* compact bar: title, play/pause, next, close */
void audio_stop(void);

#endif
