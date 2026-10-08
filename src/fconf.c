/* fconf.c -- settings, bookmarks and history in PLACE_CONFIG/mmcfm.ini.
**
** Design decisions:
**   - Plain "key=value" lines, one setting per line, so the file is easy to
**     read and edit by hand. Bookmarks and history are repeated keys
**     ("bookmark=...") in order.
**   - Values are escaped (\\, \n, \r, \t) so a path containing a newline
**     cannot inject settings. Loading is defensive: unknown keys are
**     ignored, numbers are clamped, over-long lines are skipped whole.
**   - Saving writes mmcfm.ini.tmp first and then replaces the old file, so a
**     crash while writing never leaves a half-written settings file.
**     Debouncing lives in the app (it knows when the user stops dragging).
*/
#include "fconf.h"
#include "fplat.h"
#include "fui.h"

FmConf conf;

/* ---- defaults ----------------------------------------------------------- */

void conf_defaults(void) {
  memset(&conf, 0, sizeof conf);
  conf.dark = true;
  conf.accent = 0;
  conf.zoom = 1.0f;
  conf.touch = -1;
  conf.layout = LAYOUT_AUTO;
  conf.split = 0.5f;
  conf.show_hidden = false;
  conf.folders_first = true;
  conf.confirm_delete = true;
  conf.use_trash = true;
  conf.thumbnails = true;
  conf.sort[0] = conf.sort[1] = SORT_NAME;
  conf.view[0] = conf.view[1] = VIEW_LIST;
  conf.active = 0;
  conf.win_x = conf.win_y = -1;
  conf.win_w = conf.win_h = 0;
  conf.volume = 0.8f;
}

/* ---- escaping ----------------------------------------------------------- */

static void esc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 3 < cap; s++) {
    char c = *s;
    if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
    else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
    else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
    else out[o++] = c;
  }
  out[o] = 0;
}

static void unesc(const char *s, char *out, size_t cap) {
  size_t o = 0;
  for (; *s && o + 1 < cap; s++) {
    if (*s == '\\' && s[1]) {
      s++;
      out[o++] = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s;
    } else {
      out[o++] = *s;
    }
  }
  out[o] = 0;
}

/* ---- file --------------------------------------------------------------- */

static bool conf_file(char *out, size_t cap, const char *name) {
  char dir[FM_PATH_MAX];
  if (!plat_place(PLACE_CONFIG, dir, sizeof dir)) return false;
  return fm_path_join(out, cap, dir, name);
}

static bool to_bool(const char *v) { return v[0] == '1' || v[0] == 't' || v[0] == 'y'; }

static int to_int(const char *v, int lo, int hi, int def) {
  char *end;
  long n = strtol(v, &end, 10);
  if (end == v) return def;
  return (int)FM_CLAMP(n, (long)lo, (long)hi);
}

static float to_float(const char *v, float lo, float hi, float def) {
  char *end;
  double d = strtod(v, &end);
  if (end == v || d != d) return def;
  return (float)FM_CLAMP(d, (double)lo, (double)hi);
}

static void set_key(const char *k, const char *raw) {
  char v[FM_PATH_MAX];
  unesc(raw, v, sizeof v);
  if (!strcmp(k, "dark")) conf.dark = to_bool(v);
  else if (!strcmp(k, "accent")) conf.accent = to_int(v, 0, UI_ACCENTS - 1, 0);
  else if (!strcmp(k, "zoom")) conf.zoom = to_float(v, 0.75f, 2.0f, 1.0f);
  else if (!strcmp(k, "touch")) conf.touch = to_int(v, -1, 1, -1);
  else if (!strcmp(k, "layout")) conf.layout = to_int(v, LAYOUT_AUTO, LAYOUT_SINGLE, LAYOUT_AUTO);
  else if (!strcmp(k, "split")) conf.split = to_float(v, 0.2f, 0.8f, 0.5f);
  else if (!strcmp(k, "show_hidden")) conf.show_hidden = to_bool(v);
  else if (!strcmp(k, "folders_first")) conf.folders_first = to_bool(v);
  else if (!strcmp(k, "confirm_delete")) conf.confirm_delete = to_bool(v);
  else if (!strcmp(k, "use_trash")) conf.use_trash = to_bool(v);
  else if (!strcmp(k, "thumbnails")) conf.thumbnails = to_bool(v);
  else if (!strcmp(k, "sort0")) conf.sort[0] = to_int(v, SORT_NAME, SORT_TYPE, SORT_NAME);
  else if (!strcmp(k, "sort1")) conf.sort[1] = to_int(v, SORT_NAME, SORT_TYPE, SORT_NAME);
  else if (!strcmp(k, "sort_desc0")) conf.sort_desc[0] = to_bool(v);
  else if (!strcmp(k, "sort_desc1")) conf.sort_desc[1] = to_bool(v);
  else if (!strcmp(k, "view0")) conf.view[0] = to_int(v, VIEW_LIST, VIEW_GRID, VIEW_LIST);
  else if (!strcmp(k, "view1")) conf.view[1] = to_int(v, VIEW_LIST, VIEW_GRID, VIEW_LIST);
  else if (!strcmp(k, "path0")) fm_strlcpy(conf.path[0], v, FM_PATH_MAX);
  else if (!strcmp(k, "path1")) fm_strlcpy(conf.path[1], v, FM_PATH_MAX);
  else if (!strcmp(k, "active")) conf.active = to_int(v, 0, 1, 0);
  else if (!strcmp(k, "win_x")) conf.win_x = to_int(v, -100000, 100000, -1);
  else if (!strcmp(k, "win_y")) conf.win_y = to_int(v, -100000, 100000, -1);
  else if (!strcmp(k, "win_w")) conf.win_w = to_int(v, 0, 32000, 0);
  else if (!strcmp(k, "win_h")) conf.win_h = to_int(v, 0, 32000, 0);
  else if (!strcmp(k, "win_max")) conf.win_max = to_bool(v);
  else if (!strcmp(k, "volume")) conf.volume = to_float(v, 0.0f, 1.0f, 0.8f);
  else if (!strcmp(k, "bookmark")) {
    if (v[0] && conf.nbookmarks < CONF_BOOKMARKS && !conf_is_bookmark(v))
      fm_strlcpy(conf.bookmarks[conf.nbookmarks++], v, FM_PATH_MAX);
  } else if (!strcmp(k, "history")) {
    if (v[0] && conf.nhistory < CONF_HISTORY)
      fm_strlcpy(conf.history[conf.nhistory++], v, FM_PATH_MAX);
  }
}

void conf_load(void) {
  conf_defaults();
  char path[FM_PATH_MAX];
  if (!conf_file(path, sizeof path, "mmcfm.ini")) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  char line[FM_PATH_MAX * 2 + 64];
  bool skipping = false;
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    bool complete = n > 0 && line[n - 1] == '\n';
    if (skipping) {                 /* rest of an over-long line */
      if (complete) skipping = false;
      continue;
    }
    if (!complete && !feof(f)) {
      skipping = true;
      continue;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    set_key(line, eq + 1);
  }
  fclose(f);
  if (conf.accent < 0 || conf.accent >= UI_ACCENTS) conf.accent = 0;
}

static void put(FILE *f, const char *k, const char *v) {
  char e[FM_PATH_MAX * 2 + 8];
  esc(v, e, sizeof e);
  fprintf(f, "%s=%s\n", k, e);
}

static void put_i(FILE *f, const char *k, int v) { fprintf(f, "%s=%d\n", k, v); }
static void put_f(FILE *f, const char *k, float v) { fprintf(f, "%s=%.3f\n", k, (double)v); }

void conf_save(void) {
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!conf_file(path, sizeof path, "mmcfm.ini") || !conf_file(tmp, sizeof tmp, "mmcfm.ini.tmp"))
    return;
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) {
    fm_log("settings: cannot write %s", tmp);
    return;
  }
  fprintf(f, "# MMC File Manager %s settings\n", FM_VERSION);
  put_i(f, "dark", conf.dark);
  put_i(f, "accent", conf.accent);
  put_f(f, "zoom", conf.zoom);
  put_i(f, "touch", conf.touch);
  put_i(f, "layout", conf.layout);
  put_f(f, "split", conf.split);
  put_i(f, "show_hidden", conf.show_hidden);
  put_i(f, "folders_first", conf.folders_first);
  put_i(f, "confirm_delete", conf.confirm_delete);
  put_i(f, "use_trash", conf.use_trash);
  put_i(f, "thumbnails", conf.thumbnails);
  put_i(f, "sort0", conf.sort[0]);
  put_i(f, "sort1", conf.sort[1]);
  put_i(f, "sort_desc0", conf.sort_desc[0]);
  put_i(f, "sort_desc1", conf.sort_desc[1]);
  put_i(f, "view0", conf.view[0]);
  put_i(f, "view1", conf.view[1]);
  put(f, "path0", conf.path[0]);
  put(f, "path1", conf.path[1]);
  put_i(f, "active", conf.active);
  put_i(f, "win_x", conf.win_x);
  put_i(f, "win_y", conf.win_y);
  put_i(f, "win_w", conf.win_w);
  put_i(f, "win_h", conf.win_h);
  put_i(f, "win_max", conf.win_max);
  put_f(f, "volume", conf.volume);
  for (int i = 0; i < conf.nbookmarks; i++) put(f, "bookmark", conf.bookmarks[i]);
  for (int i = 0; i < conf.nhistory; i++) put(f, "history", conf.history[i]);
  bool ok = !ferror(f);
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    plat_remove_file(tmp);
    fm_log("settings: write failed");
    return;
  }
  /* plat_rename never replaces; the old file goes first */
  plat_remove_file(path);
  if (plat_rename(tmp, path) != FM_OK) fm_log("settings: cannot replace %s", path);
}

/* ---- bookmarks and history ---------------------------------------------- */

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

void conf_add_history(const char *path) {
  if (!path || !path[0] || strlen(path) >= FM_PATH_MAX) return;
  int at = -1;
  for (int i = 0; i < conf.nhistory; i++)
    if (same_path(conf.history[i], path)) { at = i; break; }
  if (at == 0) return;
  if (at < 0) {
    at = conf.nhistory < CONF_HISTORY ? conf.nhistory++ : CONF_HISTORY - 1;
  }
  /* most recent first: shift [0, at) down by one */
  memmove(conf.history[1], conf.history[0], (size_t)at * FM_PATH_MAX);
  fm_strlcpy(conf.history[0], path, FM_PATH_MAX);
}

bool conf_is_bookmark(const char *path) {
  for (int i = 0; i < conf.nbookmarks; i++)
    if (same_path(conf.bookmarks[i], path)) return true;
  return false;
}

void conf_toggle_bookmark(const char *path) {
  if (!path || !path[0]) return;
  for (int i = 0; i < conf.nbookmarks; i++) {
    if (same_path(conf.bookmarks[i], path)) {
      memmove(conf.bookmarks[i], conf.bookmarks[i + 1],
              (size_t)(conf.nbookmarks - i - 1) * FM_PATH_MAX);
      conf.nbookmarks--;
      return;
    }
  }
  if (conf.nbookmarks < CONF_BOOKMARKS)
    fm_strlcpy(conf.bookmarks[conf.nbookmarks++], path, FM_PATH_MAX);
}
