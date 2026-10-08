/* fapp.h -- the application: global state shared by the screens.
**
** fmain.c owns the window and the event loop and calls app_init,
** app_event, app_frame and app_shutdown. Everything else (panels, jobs,
** viewers, dialogs) hangs off the FmApp below.
*/
#ifndef FAPP_H
#define FAPP_H

#include "fui.h"
#include "fplat.h"

struct FmViewer;

typedef struct FmApp {
  SDL_Window *win;
  SDL_Renderer *ren;
  bool quit;
  bool background;               /* Android: app is in the background, skip drawing */
  u32 ev_wake;                   /* SDL user event that worker threads push */
  const struct FmViewer *viewer; /* open full-screen viewer, or NULL */
  char exe_dir[FM_PATH_MAX];
  int argc;
  char **argv;
} FmApp;

extern FmApp app;

void app_init(void);
void app_event(const SDL_Event *e);   /* after ui_event; app-level events */
void app_frame(void);
void app_shutdown(void);
/* Asks the user whether to quit while jobs run; returns true to quit now. */
bool app_can_quit(void);

/* Thread-safe: wakes the main loop for a redraw (job progress, thumbnails). */
void app_wake(void);

/* Opens a file: internal viewer when there is one, else the system app.
** `siblings` (may be NULL) are the other files in the folder, for next/prev. */
void app_open(const char *path, const char *const *siblings, int n, int index);
void app_close_viewer(void);

/* Device lost (Android resume, D3D reset): recreate textures. */
void app_render_reset(void);

#endif
