/* fconf.h -- settings, bookmarks and history, saved as plain key=value text.
**
** One global FmConf loaded at start and saved on change and at exit, in
** PLACE_CONFIG/mmcfm.ini.
*/
#ifndef FCONF_H
#define FCONF_H

#include "fcore.h"

#define CONF_BOOKMARKS 32
#define CONF_HISTORY 24

enum { SORT_NAME = 0, SORT_SIZE, SORT_DATE, SORT_TYPE };
enum { LAYOUT_AUTO = 0, LAYOUT_SIDE, LAYOUT_STACK, LAYOUT_SINGLE };
enum { VIEW_LIST = 0, VIEW_GRID };

/* Music visualizer knobs (styles, limits and presets live in fviz.h). */
typedef struct FmVizConf {
  int style;                  /* VIZ_* */
  int bands;                  /* VIZ_MIN_BANDS .. VIZ_MAX_BANDS */
  float gain;                 /* sensitivity, 0.25 .. 4 */
  float attack, decay;        /* 0 .. 1: how fast bars rise and fall */
  float width;                /* bar width / pitch, 0.15 .. 1 */
  float round;                /* corner roundness 0 .. 1 */
  int color;                  /* VIZ_COL_* */
  int color2;                 /* second colour: kAccents index, -1 = auto */
  bool peaks, mirror, log_scale;
  bool custom;                /* knobs were tuned by hand (in the saved slot: slot is in use) */
} FmVizConf;

/* Equalizer (presets and DSP in feq.h). Bands: 31 Hz .. 16 kHz, octaves. */
#define EQ_BANDS 10
typedef struct FmEqConf {
  bool on;
  int preset;                 /* EQ_* preset, EQ_CUSTOM = the user's curve */
  float band[EQ_BANDS];       /* dB, -12 .. 12: what plays */
  float custom[EQ_BANDS];     /* the user's own curve (Custom chip) */
  float preamp;               /* dB, -12 .. 12 */
  float width;                /* stereo width 0 .. 2, 1 = as recorded */
  float balance;              /* -1 left .. 1 right */
} FmEqConf;

typedef struct FmConf {
  int theme;                  /* built-in theme, 0 .. THEME_COUNT-1 */
  bool dark;                  /* preferred mode when the theme has both */
  int accent;                 /* index into kAccents, -1 = the theme's own */
  float zoom;                 /* UI zoom, 0.75 .. 2.0 */
  int touch;                  /* -1 auto, 0 off, 1 on */
  int layout;                 /* LAYOUT_* */
  float split;                /* panel divider 0.2 .. 0.8 */
  bool show_hidden;
  bool folders_first;
  bool confirm_delete;
  bool use_trash;
  bool thumbnails;
  int sort[2];                /* per panel */
  bool sort_desc[2];
  int view[2];                /* VIEW_* per panel */
  char path[2][FM_PATH_MAX];  /* last folder per panel */
  int active;                 /* last active panel */
  char bookmarks[CONF_BOOKMARKS][FM_PATH_MAX];
  int nbookmarks;
  char history[CONF_HISTORY][FM_PATH_MAX];
  int nhistory;
  int win_x, win_y, win_w, win_h;
  bool win_max;
  bool system_title;          /* OS window frame instead of the themed title bar */
  bool tray;                  /* icon in the notification area (Windows, ftray.c) */
  bool tray_min;              /* minimize hides the window to that icon */
  bool tray_close;            /* the close button too (Quit is in the icon's menu) */
  /* online videos (fvsrc*.c, fonline*.c) */
  char online_source[16];     /* last used source key: "youtube", "archive", ... */
  char yt_api_key[128];       /* YouTube Data API v3 key, "" = none */
  char ytdlp_path[FM_PATH_MAX];   /* yt-dlp executable, "" = find it */
  char js_runtime[FM_PATH_MAX];   /* "node:<path>" / "deno:<path>" for yt-dlp, "" = find it */
  char ffmpeg_dir[FM_PATH_MAX];   /* folder with ffmpeg(.exe) for merging downloads, "" = none */
  char online_dl_dir[FM_PATH_MAX];/* downloads, "" = the system Downloads folder */
  int online_height;          /* preferred quality: 360, 480, 720, 1080 */
  bool online_safe;           /* safe search */
  /* online photos (fpsrc*.c, fphoto*.c) */
  char photo_source[16];      /* last used photo source key */
  char key_pexels[96], key_unsplash[96], key_pixabay[96];   /* free API keys, "" = none */
  /* online audio (fasrc*.c, faudio_online*.c) */
  char audio_source[16];      /* last used audio source key */
  char key_jamendo[64], key_freesound[96];                   /* free API keys, "" = none */
  float volume;               /* audio player 0..1 */
  FmVizConf viz;              /* the visualizer as shown */
  FmVizConf viz_saved;        /* the user's own custom setup ("Custom" chip) */
  FmEqConf eq;                /* equalizer for the music and video players */
} FmConf;

extern FmConf conf;

void conf_defaults(void);
void conf_load(void);
void conf_save(void);
void conf_add_history(const char *path);
bool conf_is_bookmark(const char *path);
void conf_toggle_bookmark(const char *path);

#endif
