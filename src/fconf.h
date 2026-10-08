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
  float volume;               /* audio player 0..1 */
} FmConf;

extern FmConf conf;

void conf_defaults(void);
void conf_load(void);
void conf_save(void);
void conf_add_history(const char *path);
bool conf_is_bookmark(const char *path);
void conf_toggle_bookmark(const char *path);

#endif
