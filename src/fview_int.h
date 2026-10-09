/* fview_int.h -- what the viewers share: chrome helpers (fview.c), the text
** line indexer (fview_txt.c, also used by the self test) and the hooks
** between the audio and video players.
**
** Design decisions:
**   - Image and video viewers sit on black in both themes, so their chrome
**     uses the fixed viewer palette below (white on a dark scrim); the text
**     viewer and the audio player follow the theme (T.*).
**   - Waking the app later (GIF frame, slideshow, auto-hide) goes through
**     one SDL timer that calls app_wake, so nothing animates while idle.
*/
#ifndef FVIEW_INT_H
#define FVIEW_INT_H

#include "fview.h"
#include "fapp.h"

/* ---- viewer palette ----------------------------------------------------------- */

#define VIEW_BG     FM_RGBA(0, 0, 0, 255)
#define VIEW_FG     FM_RGBA(255, 255, 255, 255)
#define VIEW_FG2    FM_RGBA(255, 255, 255, 175)
#define VIEW_SCRIM  FM_RGBA(0, 0, 0, 160)
#define VIEW_HIDE_MS 2500

/* ---- chrome ----------------------------------------------------------------- */

enum { VIEW_BAR_MEDIA = 0, VIEW_BAR_THEME = 1 };

typedef struct FmViewChrome {
  u64 last_input;      /* ui.now of the last pointer/key activity */
  bool hidden;         /* user toggled the bars off (tap) */
  bool shown;          /* visible as of the last view_chrome call */
  float mx, my;        /* pointer at the last check */
} FmViewChrome;

/* Returns the bar opacity 0..1. With autohide the bars fade after
** VIEW_HIDE_MS without input; any pointer movement or key brings them back.
** Schedules its own wake-up, so the caller never animates while idle. */
float view_chrome(FmViewChrome *c, u32 id, bool autohide);
void  view_chrome_poke(FmViewChrome *c);
void  view_chrome_toggle(FmViewChrome *c);

/* Top bar: back button, title and subtitle. Returns true when back was
** pressed; *actions receives the strip on the right reserved for `nact` buttons. */
bool view_topbar(FmRect area, int style, float alpha, const char *title, const char *sub, int nact,
                 FmRect *actions);
/* Icon button cut from the right of *actions. */
bool view_bar_btn(FmRect *actions, u32 id, FmIcon ic, const char *tip, int style, float alpha, bool on);
/* Dark gradient at the bottom for controls on media. */
void view_bottom_scrim(FmRect r, float alpha);

/* Escape / Android back (consumed). */
bool view_key_back(void);
/* "1:05" or "1:02:03". */
void view_fmt_time(double sec, char *buf, size_t cap);
/* Centred icon + title + detail (errors, unsupported formats). */
void view_message(FmRect area, FmIcon ic, const char *title, const char *detail, FmColor fg, FmColor fg2);

/* Draws texture t rotated by rot*90 degrees clockwise so that it fills dst
** (dst is the on-screen box of the rotated picture). */
void view_tex_rot(SDL_Texture *t, FmRect dst, int rot, FmColor tint);
/* RGBA texture from pixels (static, blended, linear filtering). */
SDL_Texture *view_tex_rgba(const u8 *px, int w, int h);
/* Largest texture edge the renderer accepts (at least 2048). */
int view_max_texture(void);

/* Wakes the main loop in `ms` (one-shot SDL timer -> app_wake). */
void view_wake_in(u32 ms);

/* Information card: label/value rows. *open turns false when dismissed. */
void view_info_dialog(u32 id, const char *title, const char *const *keys, const char *const *vals, int n,
                      bool *open);

/* ---- audio <-> video ------------------------------------------------------- */

/* Pauses background music (a video is about to play). */
void audio_pause(void);
/* Self test: loads list parked (no device, no thread) at cur / pos, so the
** queue functions run on the real player state; n = 0 unloads it. */
void audio_queue_test(const FmAudioEntry *list, int n, int cur, double pos);

/* ---- text line index (fview_txt.c) ------------------------------------------------ */

#define TXT_MARK 128   /* a mark every 128 lines: 1 GB of logs needs ~1 MB of marks */

typedef struct FmTxtIndex {
  FILE *f;             /* scanner's handle */
  FILE *rf;            /* reader's handle (line lookups, page reads) */
  u64 size;
  int unit;            /* 1 = bytes (UTF-8 / Latin-1), 2 = UTF-16 */
  bool be;             /* UTF-16 big endian */
  u64 start;           /* first byte after the BOM */
  u64 *marks;          /* offset of line k * TXT_MARK */
  int nmarks, cap;
  u64 lines;           /* line starts found so far */
  u64 scanned;         /* absolute offset scanned up to */
  bool pending_cr;
  bool done;
  bool binary;         /* NUL bytes near the start: show hex */
  SDL_mutex *mx;       /* guards marks/lines/scanned/done; NULL = single thread */
  u8 *buf;
} FmTxtIndex;

bool txt_index_open(FmTxtIndex *ix, const char *path);
/* Scans up to `bytes` more; returns true while there is more to scan. */
bool txt_index_step(FmTxtIndex *ix, size_t bytes);
/* Byte offset where line `line` starts (clamped to the lines known so far). */
u64  txt_line_offset(FmTxtIndex *ix, u64 line);
void txt_index_close(FmTxtIndex *ix);

#endif
