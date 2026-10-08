/* faudio_online.h -- the "Online audio" screen: radio, music and podcasts.
**
** A full-window view like the online videos and photos ones (fonline.h,
** fphoto.h): the app draws it in place of its panels. Search or browse a
** source (fasrc.h: Radio Browser, Audius, the Internet Archive, podcasts,
** Jamendo, Freesound), play through the music player (fview.h,
** audio_play_entries), keep favourites, recently played and podcast
** subscriptions on this device, and download finite tracks in the
** background. The full player opens over the view; closing it comes back
** to the same list at the same scroll position.
**
** All functions are main thread only.
*/
#ifndef FAUDIO_ONLINE_H
#define FAUDIO_ONLINE_H

#include "fui.h"

/* ---- lifetime ----------------------------------------------------------- */

/* readonly: --shot runs (nothing is written: no library, no recent searches). */
void aonline_init(bool readonly);
void aonline_shutdown(void);
/* Every frame, open or not: finished searches, downloads, the library file. */
void aonline_pump(void);

/* What the view needs from the app (all may be NULL). */
typedef struct FmAonlineHooks {
  void (*conf_dirty)(void);              /* a setting changed: save soon */
  void (*open_settings)(void);           /* the app's settings dialog */
  void (*reveal)(const char *path);      /* "Show in folder" for a download */
} FmAonlineHooks;
void aonline_set_hooks(const FmAonlineHooks *h);

/* ---- the view ----------------------------------------------------------- */

/* source: a key ("radio", "audius" ...) or NULL for the last one used. */
void aonline_open(const char *source);
void aonline_close(void);
bool aonline_is_open(void);
void aonline_frame(FmRect area);
/* Downloads still running (the app asks before quitting). */
int  aonline_downloads_active(void);

/* ---- settings section (drawn inside the app's settings dialog) ----------- */

float aonline_settings_h(float w);
void  aonline_settings(FmRect *r, u32 base);
/* True once after the view asked for the settings: the dialog scrolls to
** the online audio section. */
bool  aonline_settings_focus(void);

/* ---- screenshots (--demo-audio-online SOURCE [QUERY]) -------------------- */

/* Opens the view on `source` and searches `query` (NULL: its browse page or
** onboarding). `state` picks what the screenshot shows once results are in:
** "play[N]" (item N plays, mini player), "player[N]" (and the full player),
** "detail[N]" (a podcast or album opened), "menu[N]", "downloads",
** "favorites" / "recent" / "subs" (with a query: the results added in memory;
** without: the library as saved on disk), "e2e[N]" (play, favourite and
** download; outside --shot it really writes), or a forced state: "nokey",
** "offline", "empty", "error", "onboarding", "settings". */
void aonline_demo(const char *source, const char *query, const char *state);

#endif
