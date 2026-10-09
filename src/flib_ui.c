/* flib_ui.c -- the media library view: sections, lists, grids, favorites.
**
** Design decisions:
**   - A full-window screen drawn by the app in place of its panels (not a
**     viewer): the players open on top of it as usual, and closing a player
**     comes back to the library with its scroll and section intact. The
**     window title bar, the mini player and job questions keep working.
**   - Sections sit in a rail on wide screens and in a horizontally
**     scrolling chip strip on narrow ones (portrait phones), so the list
**     always gets the full height.
**   - The rows of the current section are an int array rebuilt only when
**     the index, the filter, the section or the favorites change (the
**     library generation number); drawing touches the visible rows only.
**   - One tap plays (a media library is not a file list: there is nothing
**     to select); right click or long press opens the item menu, which
**     also queues songs (Play next, Add to queue) and adds them to
**     playlists; artists and albums have the same menu for all their songs.
**   - Playlists are a section like the others, but their list and editor
**     are drawn by fqueue_ui.c, which owns them.
*/
#include "flib.h"
#include "fapp.h"
#include "fplat.h"
#include "fview.h"
#include "fthumb.h"
#include "ftitle.h"
#include "fconf.h"
#include "fdec_aud.h"
#include "fqueue.h"

typedef struct SecInfo { const char *label; FmIcon icon; } SecInfo;
static const SecInfo kSec[LIB_SEC_COUNT] = {
  { "Songs", IC_MUSIC }, { "Artists", IC_PERSON }, { "Albums", IC_DISC }, { "Videos", IC_VIDEO },
  { "Favorites", IC_HEART }, { "Recent", IC_HISTORY }, { "Playlists", IC_QUEUE }, { "Folders", IC_FOLDER },
};

enum { KIND_ALL = 0, KIND_MUSIC, KIND_VIDEO };
enum {
  MI_PLAY = 1, MI_FAV, MI_REVEAL, MI_SYSTEM, MI_COPY_PATH, MI_FORGET, MI_NEXT, MI_QUEUE, MI_PLADD,
  MI_ADD_HINT = 20, MI_ADD_HIDDEN = 40,
  GM_PLAY = 60, GM_NEXT, GM_QUEUE, GM_PLADD     /* an artist or album */
};

static struct {
  bool open;
  int sec;
  /* drill-down into one artist or album */
  bool in_group;
  char group_key[AUD_META_TEXT];
  int group;                   /* resolved index, -1 = gone */
  char hint[FM_PATH_MAX];
  FmScroll scroll[LIB_SEC_COUNT], sub_scroll;
  float chip_x, chip_x0;
  bool chip_reveal;
  bool search_open, search_focus;
  char filter[128];
  int kind[LIB_SEC_COUNT];     /* favorites / recent: all, music, videos */
  /* rows of the current section */
  int *rows;
  int nrows, caprows;
  u32 rows_gen;
  int rows_sec, rows_kind;
  bool rows_group, rows_ok;
  char rows_filter[128];
  /* item menu */
  char menu_path[FM_PATH_MAX];
  int menu_sec;
  int menu_group;              /* the group menu's artist or album */
  u64 last_scan;
  void (*reveal)(const char *path);
} U;

static u32 ID_MENU, ID_ADD, ID_GROUP;

/* ---- helpers ------------------------------------------------------------------ */

static float row_h(void) { return FM_MAX(ui.m.row_h, DP(ui.touch_mode ? 64 : 54)); }
static float btn_size(void) { return DP(ui.touch_mode ? 44 : 36); }

static const FmLibGroup *cur_groups(int *n) {
  const FmLibData *d = lib_data();
  if (U.sec == LIB_SEC_ARTISTS) { *n = d->nartists; return d->artists; }
  if (U.sec == LIB_SEC_ALBUMS) { *n = d->nalbums; return d->albums; }
  *n = 0;
  return NULL;
}

static const int *group_order(void) {
  const FmLibData *d = lib_data();
  return U.sec == LIB_SEC_ARTISTS ? d->by_artist : d->by_album;
}

static const char *group_name(const FmLibGroup *gr) {
  if (gr->name) return lib_str(gr->name);
  return U.sec == LIB_SEC_ARTISTS ? "Unknown artist" : "Unknown album";
}

static bool sec_has_groups(void) { return U.sec == LIB_SEC_ARTISTS || U.sec == LIB_SEC_ALBUMS; }

static int sec_count(int s) {
  const FmLibData *d = lib_data();
  switch (s) {
    case LIB_SEC_SONGS: return d->nsongs;
    case LIB_SEC_ARTISTS: return d->nartists;
    case LIB_SEC_ALBUMS: return d->nalbums;
    case LIB_SEC_VIDEOS: return d->nvideos;
    case LIB_SEC_FAVS: return lib_fav_count();
    case LIB_SEC_RECENT: return lib_recent_count();
    case LIB_SEC_PLAYLISTS: return pl_count();
    default: return lib_folder_count();
  }
}

static void fmt_n(int n, const char *one, const char *many, char *out, size_t cap) {
  fm_snprintf(out, cap, "%d %s", n, n == 1 ? one : many);
}

static void push_row(int v) {
  if (U.nrows == U.caprows) {
    U.caprows = U.caprows ? U.caprows * 2 : 256;
    U.rows = (int *)fm_realloc(U.rows, sizeof(int) * (size_t)U.caprows);
  }
  U.rows[U.nrows++] = v;
}

static bool item_matches(const FmLibItem *it, const char *f) {
  if (!f[0]) return true;
  return fm_stristr(fm_path_base(lib_str(it->path)), f) || fm_stristr(lib_str(it->title), f) ||
         fm_stristr(lib_str(it->artist), f) || fm_stristr(lib_str(it->album), f);
}

static bool path_matches(const char *path, const char *f) {
  if (!f[0]) return true;
  int k = lib_find(path);
  return k >= 0 ? item_matches(&lib_data()->items[k], f) : fm_stristr(fm_path_base(path), f) != NULL;
}

static bool kind_ok(const char *path, int kind) {
  if (kind == KIND_ALL) return true;
  FmType t = fm_type_from_name(path);
  return kind == KIND_MUSIC ? t == FT_AUDIO : t == FT_VIDEO;
}

static void resolve_group(void) {
  U.group = -1;
  if (!U.in_group) return;
  int n;
  const FmLibGroup *gr = cur_groups(&n);
  for (int i = 0; i < n; i++) {
    const char *name = gr[i].name ? lib_str(gr[i].name) : "";
    if (fm_stricmp(name, U.group_key) == 0) { U.group = i; return; }
  }
}

static void build_rows(void) {
  const FmLibData *d = lib_data();
  int kind = U.kind[U.sec];
  if (U.rows_ok && U.rows_gen == d->gen && U.rows_sec == U.sec && U.rows_group == U.in_group &&
      U.rows_kind == kind && strcmp(U.rows_filter, U.filter) == 0)
    return;
  U.rows_ok = true;
  U.rows_gen = d->gen;
  U.rows_sec = U.sec;
  U.rows_group = U.in_group;
  U.rows_kind = kind;
  fm_strlcpy(U.rows_filter, U.filter, sizeof U.rows_filter);
  U.nrows = 0;
  const char *f = U.filter;
  switch (U.sec) {
    case LIB_SEC_SONGS:
      for (int i = 0; i < d->nsongs; i++)
        if (item_matches(&d->items[d->songs[i]], f)) push_row(d->songs[i]);
      break;
    case LIB_SEC_VIDEOS:
      for (int i = 0; i < d->nvideos; i++)
        if (item_matches(&d->items[d->videos[i]], f)) push_row(d->videos[i]);
      break;
    case LIB_SEC_ARTISTS:
    case LIB_SEC_ALBUMS: {
      int n;
      const FmLibGroup *gr = cur_groups(&n);
      const int *order = group_order();
      resolve_group();
      if (U.in_group) {
        if (U.group < 0) break;
        const FmLibGroup *g0 = &gr[U.group];
        for (int i = 0; i < g0->count; i++)
          if (item_matches(&d->items[order[g0->first + i]], f)) push_row(order[g0->first + i]);
        break;
      }
      for (int i = 0; i < n; i++) {
        bool ok = !f[0] || fm_stristr(group_name(&gr[i]), f);
        for (int k = 0; !ok && k < gr[i].count; k++) ok = item_matches(&d->items[order[gr[i].first + k]], f);
        if (ok) push_row(i);
      }
      break;
    }
    case LIB_SEC_FAVS:
      for (int i = 0; i < lib_fav_count(); i++)
        if (kind_ok(lib_fav_at(i), kind) && path_matches(lib_fav_at(i), f)) push_row(i);
      break;
    case LIB_SEC_RECENT:
      for (int i = 0; i < lib_recent_count(); i++)
        if (kind_ok(lib_recent_at(i), kind) && path_matches(lib_recent_at(i), f)) push_row(i);
      break;
    case LIB_SEC_PLAYLISTS:
      break;
    default:
      for (int i = 0; i < lib_folder_count(); i++) push_row(i);
      break;
  }
}

/* What a row shows, from the index or (favorites of files outside it) the path. */
typedef struct Info {
  const char *path;
  char title[AUD_META_TEXT];
  const char *artist, *album;
  char sub[AUD_META_TEXT * 2];
  int kind;
  i64 mtime;
  u64 size;
} Info;

static void info_for_path(const char *path, Info *in) {
  memset(in, 0, sizeof *in);
  in->path = path;
  in->artist = in->album = "";
  int k = lib_find(path);
  if (k >= 0) {
    const FmLibItem *it = &lib_data()->items[k];
    lib_item_title(it, in->title, sizeof in->title);
    in->artist = lib_str(it->artist);
    in->album = lib_str(it->album);
    in->kind = it->kind;
    in->mtime = it->mtime;
    in->size = it->size;
  } else {
    fm_strlcpy(in->title, fm_path_base(path), sizeof in->title);
    char *e = (char *)fm_path_ext(in->title);
    if (e[0] && e != in->title) *e = 0;
    in->kind = fm_type_from_name(path) == FT_VIDEO ? LIB_VIDEO : LIB_AUDIO;
  }
  if (in->kind == LIB_VIDEO || (!in->artist[0] && !in->album[0])) {
    char dir[FM_PATH_MAX], a[32];
    fm_strlcpy(dir, path, sizeof dir);
    fm_path_parent(dir);
    if (in->size) fm_snprintf(in->sub, sizeof in->sub, "%s  \xC2\xB7  %s", fm_fmt_size(in->size, a, sizeof a), fm_path_base(dir));
    else fm_strlcpy(in->sub, fm_path_base(dir), sizeof in->sub);
  } else if (in->artist[0] && in->album[0]) {
    fm_snprintf(in->sub, sizeof in->sub, "%s  \xC2\xB7  %s", in->artist, in->album);
  } else {
    fm_strlcpy(in->sub, in->artist[0] ? in->artist : in->album, sizeof in->sub);
  }
}

static const char *row_path(int r) {
  int v = U.rows[r];
  if (U.sec == LIB_SEC_FAVS) return lib_fav_at(v);
  if (U.sec == LIB_SEC_RECENT) return lib_recent_at(v);
  return lib_str(lib_data()->items[v].path);
}

/* ---- opening ------------------------------------------------------------------ */

static void open_path(const char *path, bool audio_list) {
  if (!plat_exists(path)) {
    ui_toast("%s is not there any more", fm_path_base(path));
    return;
  }
  if (!audio_list || fm_type_from_name(path) != FT_AUDIO) {
    app_open(path, NULL, 0, 0);
    return;
  }
  /* the songs of this list become the playlist */
  const char **list = (const char **)fm_alloc(sizeof(char *) * (size_t)(U.nrows + 1));
  int n = 0, index = 0;
  for (int r = 0; r < U.nrows; r++) {
    const char *p = row_path(r);
    if (fm_type_from_name(p) != FT_AUDIO) continue;
    if (p == path || strcmp(p, path) == 0) index = n;
    list[n++] = p;
  }
  app_open(path, list, n, index);
  fm_free((void *)list);
}

static bool rows_are_items(void) {
  return U.sec == LIB_SEC_SONGS || U.sec == LIB_SEC_VIDEOS || U.sec == LIB_SEC_FAVS ||
         U.sec == LIB_SEC_RECENT || (sec_has_groups() && U.in_group);
}

static void play_all(void) {
  if (!rows_are_items()) return;
  for (int r = 0; r < U.nrows; r++)
    if (fm_type_from_name(row_path(r)) == FT_AUDIO) { open_path(row_path(r), true); return; }
}

static void enter_group(int gi) {
  int n;
  const FmLibGroup *gr = cur_groups(&n);
  if (gi < 0 || gi >= n) return;
  fm_strlcpy(U.group_key, gr[gi].name ? lib_str(gr[gi].name) : "", sizeof U.group_key);
  U.in_group = true;
  U.group = gi;
  memset(&U.sub_scroll, 0, sizeof U.sub_scroll);
  ui_redraw();
}

static void set_section(int s) {
  if (s == LIB_SEC_PLAYLISTS && U.sec != s) qui_playlists_reset();
  if (s == U.sec && !U.in_group) return;
  U.sec = FM_CLAMP(s, 0, LIB_SEC_COUNT - 1);
  U.in_group = false;
  U.chip_reveal = true;
  ui_redraw();
}

/* ---- item menu ---------------------------------------------------------------- */

static void open_item_menu(const char *path, float x, float y) {
  fm_strlcpy(U.menu_path, path, sizeof U.menu_path);
  U.menu_sec = U.sec;
  bool fav = lib_is_fav(path);
  FmMenuItem m[14];
  int n = 0;
  memset(m, 0, sizeof m);
  bool audio = fm_type_from_name(path) == FT_AUDIO;
  m[n].id = MI_PLAY; m[n].icon = IC_PLAY; m[n++].label = "Play";
  if (audio) {
    m[n].id = MI_NEXT; m[n].icon = IC_PLAY_NEXT; m[n++].label = "Play next";
    m[n].id = MI_QUEUE; m[n].icon = IC_QUEUE; m[n++].label = "Add to queue";
    m[n].id = MI_PLADD; m[n].icon = IC_PLAYLIST_ADD; m[n++].label = "Add to playlist\xE2\x80\xA6";
    m[n++].flags = UI_MI_SEP;
  }
  m[n].id = MI_FAV; m[n].icon = fav ? IC_HEART_FILL : IC_HEART;
  m[n++].label = fav ? "Remove from favorites" : "Add to favorites";
  if (U.sec == LIB_SEC_RECENT) { m[n].id = MI_FORGET; m[n].icon = IC_CLOSE; m[n++].label = "Remove from recent"; }
  m[n++].flags = UI_MI_SEP;
  if (U.reveal) { m[n].id = MI_REVEAL; m[n].icon = IC_FOLDER_OPEN; m[n++].label = "Show in folder"; }
  m[n].id = MI_SYSTEM; m[n].icon = IC_SHARE; m[n++].label = "Open with system app";
  m[n].id = MI_COPY_PATH; m[n].icon = IC_COPY; m[n++].label = "Copy path";
  ui_menu_open(ID_MENU, x, y, m, n);
}

/* An artist's or album's songs: play, queue or add them to a playlist. */
static void open_group_menu(int gi, float x, float y) {
  U.menu_group = gi;
  U.menu_sec = U.sec;
  FmMenuItem m[5];
  int n = 0;
  memset(m, 0, sizeof m);
  m[n].id = GM_PLAY; m[n].icon = IC_PLAY; m[n++].label = "Play";
  m[n].id = GM_NEXT; m[n].icon = IC_PLAY_NEXT; m[n++].label = "Play next";
  m[n].id = GM_QUEUE; m[n].icon = IC_QUEUE; m[n++].label = "Add to queue";
  m[n].id = GM_PLADD; m[n].icon = IC_PLAYLIST_ADD; m[n++].label = "Add to playlist\xE2\x80\xA6";
  ui_menu_open(ID_GROUP, x, y, m, n);
}

/* The group's songs as queue entries, in the library's order. */
static FmAudioEntry *group_entries(int gi, int *n) {
  int ng;
  *n = 0;
  if (U.menu_sec != U.sec) return NULL;
  const FmLibGroup *gr = cur_groups(&ng);
  if (!gr || gi < 0 || gi >= ng) return NULL;
  const char **paths = (const char **)fm_alloc(sizeof(char *) * (size_t)FM_MAX(gr[gi].count, 1));
  int k = 0;
  for (int i = 0; i < gr[gi].count; i++) {
    const char *p = lib_str(lib_data()->items[group_order()[gr[gi].first + i]].path);
    if (fm_type_from_name(p) == FT_AUDIO) paths[k++] = p;
  }
  FmAudioEntry *e = qents_from_paths(paths, k, 5000, n);
  fm_free((void *)paths);
  return e;
}

static void group_action(int r) {
  int n = 0;
  FmAudioEntry *e = group_entries(U.menu_group, &n);
  if (n == 0) { qents_free(e, n); return; }
  if (r == GM_PLAY) {
    if (audio_play_entries(e, n, 0)) audio_show_player();
  } else if (r == GM_NEXT || r == GM_QUEUE) {
    if (audio_queue_add(e, n, r == GM_NEXT ? AQ_NEXT : AQ_END))
      ui_toast(r == GM_NEXT ? "%d songs to play next" : "Added %d songs to the queue", n);
  } else if (r == GM_PLADD) {
    qui_pick_playlist(e, n, ui.mx, ui.my);
  }
  qents_free(e, n);
}

static void forget_recent(const char *path) {
  /* rebuild the list without it: recents are short */
  int n = lib_recent_count();
  char **keep = (char **)fm_alloc(sizeof(char *) * (size_t)(n + 1));
  int k = 0;
  for (int i = n - 1; i >= 0; i--)
    if (strcmp(lib_recent_at(i), path) != 0) keep[k++] = fm_strdup(lib_recent_at(i));
  lib_recent_clear();
  for (int i = 0; i < k; i++) { lib_note_played(keep[i]); fm_free(keep[i]); }
  fm_free(keep);
}

static void menu_results(void) {
  int r = ui_menu_result(ID_MENU);
  const char *p = U.menu_path;
  switch (r) {
    case MI_PLAY: open_path(p, U.menu_sec == U.sec); break;
    case MI_NEXT: case MI_QUEUE: qui_queue_paths(&p, 1, r == MI_NEXT); break;
    case MI_PLADD: qui_pick_playlist_paths(&p, 1, ui.mx, ui.my); break;
    case MI_FAV: lib_fav_toggle(p); ui_toast(lib_is_fav(p) ? "Added to favorites" : "Removed from favorites"); break;
    case MI_FORGET: forget_recent(p); break;
    case MI_REVEAL: if (U.reveal) U.reveal(p); break;
    case MI_SYSTEM: if (!plat_open_external(p)) ui_toast("No app found to open %s", fm_path_base(p)); break;
    case MI_COPY_PATH: ui_clipboard_set(p); ui_toast("Path copied"); break;
    default: break;
  }
  r = ui_menu_result(ID_GROUP);
  if (r >= GM_PLAY) group_action(r);
  r = ui_menu_result(ID_ADD);
  if (r == MI_ADD_HINT && U.hint[0]) {
    lib_folder_set(U.hint, true);
    ui_toast("Added %s", fm_path_base(U.hint));
  } else if (r >= MI_ADD_HIDDEN && r - MI_ADD_HIDDEN < lib_hidden_count()) {
    char d[FM_PATH_MAX];
    fm_strlcpy(d, lib_hidden_at(r - MI_ADD_HIDDEN), sizeof d);
    lib_folder_set(d, true);
    ui_toast("Added %s", fm_path_base(d));
  }
}

static void open_add_menu(float x, float y) {
  FmMenuItem m[LIB_SEC_COUNT + 12];
  char label[FM_PATH_MAX + 40];
  int n = 0;
  memset(m, 0, sizeof m);
  if (U.hint[0] && !lib_has_folder(U.hint)) {
    fm_snprintf(label, sizeof label, "Current folder: %s", fm_path_base(U.hint)[0] ? fm_path_base(U.hint) : U.hint);
    m[n].id = MI_ADD_HINT; m[n].icon = IC_FOLDER; m[n++].label = label;
  }
  for (int i = 0; i < lib_hidden_count() && n < FM_COUNT(m); i++) {
    m[n].id = MI_ADD_HIDDEN + i; m[n].icon = IC_FOLDER; m[n++].label = fm_path_base(lib_hidden_at(i));
  }
  if (n == 0) {
    ui_toast("Open a folder in a panel, then choose \"Add to media library\" in its menu");
    return;
  }
  ui_menu_open(ID_ADD, x, y, m, n);
}

/* ---- art ------------------------------------------------------------------------ */

/* Cover or frame, fitted in box on a rounded backdrop; the type glyph meanwhile. */
static void draw_art(FmRect box, const Info *in, float rad) {
  FmType t = in->kind == LIB_VIDEO ? FT_VIDEO : FT_AUDIO;
  FmColor tc = icon_type_color(t);
  SDL_Texture *tex = conf.thumbnails ? thumb_get(in->path, in->mtime, in->size, (int)FM_MAX(box.w, box.h)) : NULL;
  int tw = 0, th = 0;
  if (tex && SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) {
    gfx_rrect(box, rad, T.dark ? FM_RGBA(0, 0, 0, 255) : T.surface3);
    float s = FM_MIN(box.w / (float)tw, box.h / (float)th);
    FmRect d = rect_center(box, (float)tw * s, (float)th * s);
    gfx_tex_rounded(tex, d, FM_MIN(rad, FM_MIN(d.w, d.h) * 0.5f), FM_HEX(0xFFFFFF));
    return;
  }
  gfx_rrect_vgrad(box, rad, col_alpha(tc, T.dark ? 0.30f : 0.20f), col_alpha(tc, T.dark ? 0.16f : 0.10f));
  float is = FM_MIN(box.w, box.h) * 0.46f;
  icon_draw(t == FT_VIDEO ? IC_VIDEO : IC_MUSIC, rect_center(box, is, is), tc);
}

static bool heart_btn(u32 id, FmRect r, const char *path, bool show_outline) {
  bool fav = lib_is_fav(path);
  if (!fav && !show_outline) return false;
  bool hit = ui_icon_btn(id, r, fav ? IC_HEART_FILL : IC_HEART, fav ? T.danger : T.text3,
                         fav ? "Remove from favorites" : "Add to favorites");
  if (hit) lib_fav_toggle(path);
  return hit;
}

/* ---- rows ------------------------------------------------------------------------ */

static void row_bg(FmRect r, int f) {
  float rad = FM_MIN(DP(T.row_radius > 0 ? T.row_radius : 10), r.h * 0.5f);
  if (f & UI_HELD) gfx_rrect(r, rad, T.press);
  else if (f & UI_HOVER) gfx_rrect(r, rad, T.hover);
}

/* One song or video in a list row: art, title, subtitle, heart. */
static void media_row(FmRect row, u32 id, int r) {
  Info in;
  info_for_path(row_path(r), &in);
  int f = ui_hit(id, rect_inset2(row, 0, DP(1)));
  row_bg(rect_inset2(row, 0, DP(1.5f)), f);
  float rh = row.h;
  float x = row.x + DP(8);
  float is = FM_MIN(rh - DP(14), DP(46));
  FmRect art = { x, row.y + (rh - is) * 0.5f, is, is };
  draw_art(art, &in, DP(8));
  x += art.w + DP(12);
  float hb = FM_MIN(rh, ui.m.hit);
  FmRect heart = { row.x + row.w - hb - DP(2), row.y + (rh - hb) * 0.5f, hb, hb };
  float right = heart.x - DP(4);
  float fs = ui.m.font, fss = ui.m.font_small;
  float lh = font_line_h(fs), lhs = font_line_h(fss);
  /* mixed lists (favorites, recent) keep one layout for songs and videos */
  bool mixed = U.sec == LIB_SEC_FAVS || U.sec == LIB_SEC_RECENT;
  bool cols = in.kind == LIB_AUDIO && !mixed && right - x > DP(560) && !ui.touch_mode;
  if (cols) {
    /* desktop: title | artist | album */
    float w = right - x;
    float tw = w * 0.44f, aw = w * 0.28f;
    float ty = row.y + (rh - lh) * 0.5f, sy = row.y + (rh - lhs) * 0.5f;
    font_draw_ellipsis(FONT_REGULAR, fs, x, ty, in.title, tw - DP(12), T.text);
    font_draw_ellipsis(FONT_REGULAR, fss, x + tw, sy, in.artist[0] ? in.artist : "Unknown artist", aw - DP(12),
                       in.artist[0] ? T.text2 : T.text3);
    font_draw_ellipsis(FONT_REGULAR, fss, x + tw + aw, sy, in.album[0] ? in.album : "\xE2\x80\x94",
                       w - tw - aw - DP(8), in.album[0] ? T.text2 : T.text3);
  } else {
    float y0 = row.y + (rh - lh - lhs - DP(2)) * 0.5f;
    font_draw_ellipsis(FONT_REGULAR, fs, x, y0, in.title, right - x, T.text);
    font_draw_ellipsis(FONT_REGULAR, fss, x, y0 + lh + DP(2), in.sub, right - x, T.text2);
  }
  bool hovered = (f & UI_HOVER) || ui_pointer_in(heart);
  bool hit = heart_btn(ui_idn(id, 1), heart, in.path, ui.touch_mode || hovered);
  if (hit) return;
  if (f & UI_CLICK) open_path(in.path, true);
  else if ((f & UI_RCLICK) || (f & UI_LONG)) open_item_menu(in.path, ui.mx, ui.my);
}

/* Artist initial in a soft accent circle. */
static void avatar(FmRect b, const char *name) {
  gfx_circle(b.x + b.w * 0.5f, b.y + b.h * 0.5f, b.w * 0.5f, T.accent_soft);
  u32 cp = '?';
  if (name[0]) utf8_decode(name, &cp);
  if (cp >= 'a' && cp <= 'z') cp -= 32;
  char s[8];
  int n = utf8_encode(cp, s);
  s[n] = 0;
  font_draw_center(FONT_BOLD, ui.m.font_title, b, s, T.accent);
}

static void group_row(FmRect row, u32 id, int gi) {
  int n;
  const FmLibGroup *gr = &cur_groups(&n)[gi];
  const FmLibData *d = lib_data();
  int f = ui_hit(id, rect_inset2(row, 0, DP(1)));
  row_bg(rect_inset2(row, 0, DP(1.5f)), f);
  float rh = row.h;
  float is = FM_MIN(rh - DP(14), DP(46));
  FmRect b = { row.x + DP(8), row.y + (rh - is) * 0.5f, is, is };
  bool artist = U.sec == LIB_SEC_ARTISTS;
  if (artist) {
    if (gr->name) avatar(b, lib_str(gr->name));
    else {
      gfx_circle(b.x + is * 0.5f, b.y + is * 0.5f, is * 0.5f, T.surface3);
      icon_draw(IC_PERSON, rect_inset(b, is * 0.24f), T.text3);
    }
  } else {
    Info in;
    info_for_path(lib_str(d->items[group_order()[gr->first]].path), &in);
    draw_art(b, &in, DP(8));
  }
  float x = b.x + is + DP(12);
  float right = row.x + row.w - DP(36);
  char sub[64];
  fmt_n(gr->count, "song", "songs", sub, sizeof sub);
  float lh = font_line_h(ui.m.font), lhs = font_line_h(ui.m.font_small);
  float y0 = row.y + (rh - lh - lhs - DP(2)) * 0.5f;
  font_draw_ellipsis(FONT_REGULAR, ui.m.font, x, y0, group_name(gr), right - x, gr->name ? T.text : T.text2);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y0 + lh + DP(2), sub, right - x, T.text2);
  float cs = DP(16);
  icon_draw(IC_CHEVRON_RIGHT, FM_RECT(row.x + row.w - DP(26), row.y + (rh - cs) * 0.5f, cs, cs), T.text3);
  if (f & UI_CLICK) enter_group(gi);
  else if ((f & UI_RCLICK) || (f & UI_LONG)) open_group_menu(gi, ui.mx, ui.my);
}

static void folder_row(FmRect row, u32 id, int i) {
  bool def = false;
  const char *path = lib_folder_at(i, &def);
  float rh = row.h;
  float is = FM_MIN(rh - DP(16), DP(40));
  FmRect b = { row.x + DP(8), row.y + (rh - is) * 0.5f, is, is };
  icon_file(FT_DIR, b, T.dark);
  float bs = FM_MIN(rh, ui.m.hit);
  FmRect rm = { row.x + row.w - bs, row.y + (rh - bs) * 0.5f, bs, bs };
  float x = b.x + is + DP(12), right = rm.x - DP(6);
  float lh = font_line_h(ui.m.font), lhs = font_line_h(ui.m.font_small);
  float y0 = row.y + (rh - lh - lhs - DP(2)) * 0.5f;
  const char *name = fm_path_base(path)[0] ? fm_path_base(path) : path;
  float nw = font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y0, name, right - x - (def ? DP(70) : 0), T.text);
  if (def) {
    const char *tag = "Default";
    float tw = font_width(FONT_REGULAR, ui.m.font_small, tag, -1) + DP(14);
    FmRect chip = { x + nw + DP(8), y0 + (lh - lhs - DP(4)) * 0.5f, tw, lhs + DP(4) };
    gfx_rrect(chip, chip.h * 0.5f, T.accent_soft);
    font_draw_center(FONT_REGULAR, ui.m.font_small, chip, tag, T.accent);
  }
  font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font_small, x, y0 + lh + DP(2), path, right - x, T.text2);
  if (ui_icon_btn(id, rm, IC_CLOSE, T.text2, "Remove from library")) {
    char p[FM_PATH_MAX];
    fm_strlcpy(p, path, sizeof p);
    lib_folder_set(p, false);
    ui_toast("Removed %s from the library", fm_path_base(p));
  }
}

/* ---- tiles ------------------------------------------------------------------------ */

static void video_tile(FmRect t, u32 id, int r) {
  Info in;
  info_for_path(row_path(r), &in);
  FmRect cell = rect_inset(t, DP(5));
  int f = ui_hit(id, cell);
  row_bg(rect_inset(t, DP(1)), f);
  FmRect art = { cell.x + DP(3), cell.y + DP(3), cell.w - DP(6), (cell.w - DP(6)) * 9.0f / 16.0f };
  draw_art(art, &in, DP(10));
  /* play badge in the corner, heart on the other */
  float pb = DP(26);
  FmRect play = { art.x + DP(8), art.y + art.h - pb - DP(8), pb, pb };
  gfx_circle(play.x + pb * 0.5f, play.y + pb * 0.5f, pb * 0.5f, FM_RGBA(0, 0, 0, 140));
  icon_draw(IC_PLAY, rect_inset(play, DP(6)), FM_HEX(0xFFFFFF));
  float hb = FM_MIN(ui.m.hit, DP(40));
  FmRect heart = { art.x + art.w - hb - DP(2), art.y + DP(2), hb, hb };
  bool fav = lib_is_fav(in.path);
  bool show = fav || ui.touch_mode || (f & UI_HOVER) || ui_pointer_in(heart);
  bool hit = false;
  if (show) {
    float hs = DP(30);
    gfx_circle(heart.x + hb * 0.5f, heart.y + hb * 0.5f, hs * 0.5f, FM_RGBA(0, 0, 0, 120));
    hit = ui_icon_btn(ui_idn(id, 1), heart, fav ? IC_HEART_FILL : IC_HEART,
                      fav ? FM_HEX(0xFF4D6D) : FM_HEX(0xFFFFFF), fav ? "Remove from favorites" : "Add to favorites");
    if (hit) lib_fav_toggle(in.path);
  }
  float fs = ui.m.font, fss = ui.m.font_small;
  float y = art.y + art.h + DP(8);
  font_draw_ellipsis(FONT_REGULAR, fs, art.x + DP(2), y, in.title, art.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, fss, art.x + DP(2), y + font_line_h(fs), in.sub, art.w - DP(4), T.text3);
  if (hit) return;
  if (f & UI_CLICK) open_path(in.path, false);
  else if ((f & UI_RCLICK) || (f & UI_LONG)) open_item_menu(in.path, ui.mx, ui.my);
}

static void album_tile(FmRect t, u32 id, int gi) {
  int n;
  const FmLibGroup *gr = &cur_groups(&n)[gi];
  const FmLibData *d = lib_data();
  const FmLibItem *first = &d->items[group_order()[gr->first]];
  Info in;
  info_for_path(lib_str(first->path), &in);
  FmRect cell = rect_inset(t, DP(5));
  int f = ui_hit(id, cell);
  row_bg(rect_inset(t, DP(1)), f);
  FmRect art = { cell.x + DP(3), cell.y + DP(3), cell.w - DP(6), cell.w - DP(6) };
  draw_art(art, &in, DP(12));
  float fs = ui.m.font, fss = ui.m.font_small;
  float y = art.y + art.h + DP(8);
  font_draw_ellipsis(FONT_BOLD, fs, art.x + DP(2), y, group_name(gr), art.w - DP(4), gr->name ? T.text : T.text2);
  char sub[AUD_META_TEXT + 32], c[32];
  fmt_n(gr->count, "song", "songs", c, sizeof c);
  if (first->artist) fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", lib_str(first->artist), c);
  else fm_strlcpy(sub, c, sizeof sub);
  font_draw_ellipsis(FONT_REGULAR, fss, art.x + DP(2), y + font_line_h(fs), sub, art.w - DP(4), T.text3);
  if (f & UI_CLICK) enter_group(gi);
  else if ((f & UI_RCLICK) || (f & UI_LONG)) open_group_menu(gi, ui.mx, ui.my);
}

/* ---- lists ------------------------------------------------------------------------ */

static FmScroll *cur_scroll(void) { return U.in_group ? &U.sub_scroll : &U.scroll[U.sec]; }

static void draw_list(FmRect body, u32 sid) {
  FmScroll *sc = cur_scroll();
  float rh = row_h(), pad = DP(4);
  float content = (float)U.nrows * rh + pad * 2;
  ui_scroll(sc, sid, body, content);
  int first = FM_MAX(0, (int)((sc->y - pad) / rh));
  int last = FM_MIN(U.nrows - 1, (int)((sc->y + body.h - pad) / rh) + 1);
  gfx_clip_push(body);
  for (int r = first; r <= last; r++) {
    FmRect row = { body.x + DP(6), body.y + pad + (float)r * rh - sc->y, body.w - DP(12), rh };
    u32 id = ui_idn(sid, (u32)U.rows[r] + 1);
    if (U.sec == LIB_SEC_FOLDERS) folder_row(row, id, U.rows[r]);
    else if (sec_has_groups() && !U.in_group) group_row(row, id, U.rows[r]);
    else media_row(row, id, r);
  }
  gfx_clip_pop();
  ui_scrollbar(sc, body, content);
}

static void draw_grid(FmRect body, u32 sid, bool video) {
  FmScroll *sc = cur_scroll();
  float pad = DP(6);
  float want = video ? DP(ui.touch_mode ? 172 : 196) : DP(ui.touch_mode ? 150 : 168);
  float avail = body.w - pad * 2;
  int cols = FM_MAX(2, (int)(avail / want));
  float tw = avail / (float)cols;
  float art_h = video ? (tw - DP(16)) * 9.0f / 16.0f : tw - DP(16);
  float th = art_h + DP(16) + font_line_h(ui.m.font) + font_line_h(ui.m.font_small) + DP(10);
  int rows = (U.nrows + cols - 1) / cols;
  float content = (float)rows * th + pad * 2;
  ui_scroll(sc, sid, body, content);
  int r0 = FM_MAX(0, (int)((sc->y - pad) / th));
  int r1 = FM_MIN(rows - 1, (int)((sc->y + body.h - pad) / th) + 1);
  gfx_clip_push(body);
  for (int r = r0; r <= r1; r++)
    for (int k = 0; k < cols; k++) {
      int i = r * cols + k;
      if (i >= U.nrows) break;
      FmRect t = { body.x + pad + (float)k * tw, body.y + pad + (float)r * th - sc->y, tw, th };
      u32 id = ui_idn(sid, (u32)U.rows[i] + 1);
      if (video) video_tile(t, id, i);
      else album_tile(t, id, U.rows[i]);
    }
  gfx_clip_pop();
  ui_scrollbar(sc, body, content);
}

static void empty_state(FmRect body) {
  FmIcon ic = kSec[U.sec].icon;
  const char *title = "Nothing here yet", *sub = NULL, *btn = NULL;
  int action = 0;
  bool scanning = lib_scanning();
  if (U.filter[0]) {
    ic = IC_SEARCH;
    title = "No matches";
    sub = "Try another word, or clear the search.";
  } else if (scanning && U.sec <= LIB_SEC_VIDEOS) {
    title = "Looking for your media\xE2\x80\xA6";
    sub = "Songs and videos appear here as they are found.";
  } else {
    switch (U.sec) {
      case LIB_SEC_VIDEOS:
        title = "No videos yet";
        sub = "Videos in your library folders show up here.";
        btn = "Manage folders"; action = 1;
        break;
      case LIB_SEC_FAVS:
        ic = IC_HEART;
        title = "No favorites yet";
        sub = "Tap the heart on a song or video to keep it here.";
        break;
      case LIB_SEC_RECENT:
        title = "Nothing played yet";
        sub = "Songs and videos you open are listed here.";
        break;
      case LIB_SEC_FOLDERS:
        title = "No library folders";
        sub = "Add a folder with music or videos.";
        btn = "Add folder"; action = 2;
        break;
      default:
        title = "No songs yet";
        sub = "Music in your library folders shows up here.";
        btn = "Manage folders"; action = 1;
        break;
    }
  }
  float is = DP(52), bh = DP(ui.touch_mode ? 44 : 36);
  float tl = font_line_h(ui.m.font_title);
  float sh = sub ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, body.w - DP(48), sub, T.text2, false) : 0;
  float h = is + DP(14) + tl + (sub ? sh + DP(4) : 0) + (btn ? bh + DP(18) : 0);
  float y = body.y + FM_MAX(DP(10), (body.h - h) * 0.42f);
  float cx = body.x + body.w * 0.5f;
  gfx_circle(cx, y + is * 0.5f, is * 0.62f, col_alpha(T.accent, 0.10f));
  if (scanning && !U.filter[0] && U.sec <= LIB_SEC_VIDEOS) ui_spinner(rect_center(FM_RECT(cx - is * 0.5f, y, is, is), is * 0.6f, is * 0.6f), T.accent);
  else icon_draw(ic, FM_RECT(cx - is * 0.3f, y + is * 0.2f, is * 0.6f, is * 0.6f), T.accent);
  y += is + DP(14);
  ui_label(FM_RECT(body.x + DP(12), y, body.w - DP(24), tl), title, FONT_BOLD, ui.m.font_title, T.text, UI_CENTER);
  y += tl;
  if (sub) {
    y += DP(4);
    float w = FM_MIN(body.w - DP(48), font_width(FONT_REGULAR, ui.m.font, sub, -1) + DP(2));
    font_draw_wrap(FONT_REGULAR, ui.m.font, cx - w * 0.5f, y, w, sub, T.text2, true);
    y += sh;
  }
  if (btn) {
    y += DP(18);
    float bw = font_width(FONT_BOLD, ui.m.font, btn, -1) + DP(60);
    FmRect b = { cx - bw * 0.5f, y, bw, bh };
    if (ui_button(ui_id("lib.empty.btn"), b, action == 2 ? IC_PLUS : IC_FOLDER, btn, UI_BTN_TONAL)) {
      if (action == 1) set_section(LIB_SEC_FOLDERS);
      else open_add_menu(b.x, b.y + b.h);
    }
  }
}

/* ---- section header ------------------------------------------------------------- */

static float header_h(void) { return DP(ui.touch_mode ? 56 : 50); }

static void draw_header(FmRect *body) {
  bool kinds = U.sec == LIB_SEC_FAVS || U.sec == LIB_SEC_RECENT;
  bool two = kinds && body->w < DP(470);
  FmRect h = rect_cut_top(body, header_h());
  FmRect seg_row = two ? rect_cut_top(body, DP(ui.touch_mode ? 46 : 40)) : FM_RECT(0, 0, 0, 0);
  FmRect in = rect_inset2(h, DP(10), 0);
  float bs = btn_size();
  u32 base = ui_id("lib.hdr");
  const char *title = kSec[U.sec].label;
  char cnt[64];
  int count = U.nrows;
  if (U.in_group) {
    if (ui_icon_btn(ui_idn(base, 1), rect_center(rect_cut_left(&in, bs), bs, bs), IC_BACK, T.text2, "Back")) {
      U.in_group = false;
      ui_redraw();
    }
    rect_cut_left(&in, DP(4));
    title = U.group_key[0] ? U.group_key : (U.sec == LIB_SEC_ARTISTS ? "Unknown artist" : "Unknown album");
  }
  if (U.sec == LIB_SEC_ARTISTS && !U.in_group) fmt_n(count, "artist", "artists", cnt, sizeof cnt);
  else if (U.sec == LIB_SEC_ALBUMS && !U.in_group) fmt_n(count, "album", "albums", cnt, sizeof cnt);
  else if (U.sec == LIB_SEC_VIDEOS) fmt_n(count, "video", "videos", cnt, sizeof cnt);
  else if (U.sec == LIB_SEC_FOLDERS) fmt_n(count, "folder", "folders", cnt, sizeof cnt);
  else if (kinds) fmt_n(count, "item", "items", cnt, sizeof cnt);
  else fmt_n(count, "song", "songs", cnt, sizeof cnt);
  /* actions on the right */
  bool songs = (U.sec == LIB_SEC_SONGS || U.in_group) && U.nrows > 0;
  if (songs || ((U.sec == LIB_SEC_FAVS || U.sec == LIB_SEC_RECENT) && U.kind[U.sec] != KIND_VIDEO && U.nrows > 0)) {
    bool any_audio = songs;
    for (int r = 0; r < U.nrows && !any_audio; r++) any_audio = fm_type_from_name(row_path(r)) == FT_AUDIO;
    if (any_audio) {
      const char *l = "Play all";
      float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(48);
      bool icon_only = in.w < DP(360);
      FmRect b = rect_center(rect_cut_right(&in, icon_only ? bs : bw), icon_only ? bs : bw, DP(ui.touch_mode ? 40 : 34));
      if (ui_button(ui_idn(base, 2), b, IC_PLAY, icon_only ? NULL : l, UI_BTN_FILLED)) play_all();
      rect_cut_right(&in, DP(6));
    }
  }
  if (U.sec == LIB_SEC_RECENT && lib_recent_count() > 0) {
    FmRect b = rect_center(rect_cut_right(&in, bs), bs, bs);
    if (ui_icon_btn(ui_idn(base, 3), b, IC_DELETE, T.text2, "Clear recent")) {
      lib_recent_clear();
      ui_toast("Recent list cleared");
    }
  }
  if (U.sec == LIB_SEC_FOLDERS) {
    const char *l = "Add folder";
    float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(48);
    FmRect b = rect_center(rect_cut_right(&in, bw), bw, DP(ui.touch_mode ? 40 : 34));
    if (ui_button(ui_idn(base, 4), b, IC_PLUS, l, UI_BTN_FILLED)) open_add_menu(b.x, b.y + b.h);
    rect_cut_right(&in, DP(6));
  }
  if (kinds) {
    static const char *const kKinds[] = { "All", "Music", "Videos" };
    FmRect sr;
    if (two) sr = rect_inset2(seg_row, DP(12), DP(4));
    else sr = rect_center(rect_cut_right(&in, DP(230)), DP(222), DP(ui.touch_mode ? 38 : 32));
    if (ui_segmented(ui_idn(base, 5), sr, kKinds, 3, &U.kind[U.sec])) ui_redraw();
    if (!two) rect_cut_right(&in, DP(8));
  }
  /* title + count */
  float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x + DP(4), y, title, in.w - DP(8), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(4), y + tl, cnt, in.w - DP(8), T.text2);
  ui_divider(body->x + DP(12), body->x + body->w - DP(12), body->y);
}

static void draw_search(FmRect *body) {
  FmRect r = rect_cut_top(body, DP(ui.touch_mode ? 54 : 46));
  FmRect in = rect_inset2(r, DP(10), DP(6));
  u32 id = ui_id("lib.search");
  FmRect close = rect_cut_right(&in, in.h);
  if (U.search_focus) { ui_focus(id); U.search_focus = false; }
  int res = ui_textfield(id, in, U.filter, sizeof U.filter, "Search titles, artists, albums", 0);
  if (res & UI_TF_CHANGED) ui_redraw();
  icon_draw(IC_SEARCH, FM_RECT(in.x + in.w - DP(26), in.y + (in.h - DP(16)) * 0.5f, DP(16), DP(16)), T.text3);
  if (ui_icon_btn(ui_idn(id, 1), close, IC_CLOSE, T.text2, "Close search") || (res & UI_TF_CANCEL)) {
    U.search_open = false;
    U.filter[0] = 0;
    ui_focus(0);
  }
}

static void draw_body(FmRect r) {
  float rad = ui.m.radius;
  gfx_shadow(r, rad, DP(12), T.shadow);
  gfx_rrect(r, rad, T.surface);
  if (T.panel_border.a) gfx_rrect_line(r, rad, DP(T.panel_border_w), T.panel_border);
  else gfx_rrect_line(r, rad, DP(1), T.border);
  if (U.sec == LIB_SEC_PLAYLISTS) {
    qui_playlists(r);
    return;
  }
  build_rows();
  FmRect body = r;
  draw_header(&body);
  if (U.search_open) draw_search(&body);
  rect_cut_bottom(&body, DP(4));
  u32 sid = ui_idn(ui_id("lib.list"), (u32)(U.sec * 2 + (U.in_group ? 1 : 0)));
  if (U.nrows == 0) {
    empty_state(body);
  } else if (U.sec == LIB_SEC_VIDEOS) {
    draw_grid(body, sid, true);
  } else if (U.sec == LIB_SEC_ALBUMS && !U.in_group) {
    draw_grid(body, sid, false);
  } else {
    draw_list(body, sid);
  }
  if (U.sec == LIB_SEC_FOLDERS && U.nrows > 0) {
    /* a short note under the folders */
    float y = r.y + header_h() + (float)U.nrows * row_h() + DP(16);
    if (y + DP(40) < r.y + r.h)
      font_draw_wrap(FONT_REGULAR, ui.m.font_small, r.x + DP(20), y, r.w - DP(40),
                     "Songs and videos in these folders and their subfolders are added automatically. "
                     "Hidden folders and folders with a .nomedia file are skipped.", T.text3, true);
  }
}

/* ---- navigation ---------------------------------------------------------------- */

static void draw_rail(FmRect r) {
  gfx_shadow(r, ui.m.radius, DP(10), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, ui.m.radius, T.surface);
  gfx_rrect_line(r, ui.m.radius, DP(1), T.border);
  FmRect in = rect_inset(r, DP(8));
  float rh = DP(ui.touch_mode ? 46 : 38), hh = DP(28);
  u32 base = ui_id("lib.rail");
  float y = in.y;
  static const char *const kHead[] = { "MUSIC", "VIDEOS", "COLLECTIONS", "SOURCES" };
  static const int kHeadAt[] = { LIB_SEC_SONGS, LIB_SEC_VIDEOS, LIB_SEC_FAVS, LIB_SEC_FOLDERS };
  int hi = 0;
  for (int s = 0; s < LIB_SEC_COUNT; s++) {
    if (hi < 4 && kHeadAt[hi] == s) {
      font_draw(FONT_BOLD, ui.m.font_small, in.x + DP(10), y + hh - font_line_h(ui.m.font_small) - DP(4),
                kHead[hi], -1, T.text3);
      y += hh;
      hi++;
    }
    FmRect row = { in.x, y, in.w, rh };
    y += rh;
    int f = ui_hit(ui_idn(base, (u32)s + 1), row);
    bool here = U.sec == s;
    if (here) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    FmIcon ic = s == LIB_SEC_FAVS && here ? IC_HEART_FILL : kSec[s].icon;
    icon_draw(ic, FM_RECT(row.x + DP(10), row.y + (rh - is) * 0.5f, is, is),
              here ? T.accent : s == LIB_SEC_FAVS ? T.danger : T.text2);
    char c[16] = "";
    int n = sec_count(s);
    if (n > 0) fm_snprintf(c, sizeof c, "%d", n);
    float cw = c[0] ? font_width(FONT_REGULAR, ui.m.font_small, c, -1) : 0;
    font_draw_ellipsis(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, row.x + DP(38),
                       row.y + (rh - font_line_h(ui.m.font)) * 0.5f, kSec[s].label, row.w - DP(52) - cw,
                       here ? T.accent : T.text);
    if (c[0])
      font_draw(FONT_REGULAR, ui.m.font_small, row.x + row.w - DP(10) - cw,
                row.y + (rh - font_line_h(ui.m.font_small)) * 0.5f, c, -1, here ? T.accent : T.text3);
    if (f & UI_CLICK) set_section(s);
  }
}

static void draw_chips(FmRect r) {
  u32 base = ui_id("lib.chips");
  float ch = DP(ui.touch_mode ? 38 : 32), gap = DP(8), pad = DP(2);
  float fs = ui.m.font_small * 1.08f, is = DP(16);
  float w[LIB_SEC_COUNT], total = pad;
  for (int s = 0; s < LIB_SEC_COUNT; s++) {
    w[s] = font_width(FONT_BOLD, fs, kSec[s].label, -1) + is + DP(30);
    total += w[s] + gap;
  }
  total += pad - gap;
  float max = FM_MAX(0.0f, total - r.w);
  /* horizontal drag and the wheel scroll the strip */
  bool dragging = ui.drag_owner == base;
  if (!dragging && ui.down && ui.drag_owner == 0 && ui.moved && ui_input_ok() &&
      rect_has(r, ui.press_x, ui.press_y) && fabsf(ui.mx - ui.press_x) > fabsf(ui.my - ui.press_y)) {
    ui.drag_owner = base;
    U.chip_x0 = U.chip_x;
    dragging = true;
  }
  if (dragging && ui.down) U.chip_x = U.chip_x0 - (ui.mx - ui.press_x);
  if (ui_input_ok() && rect_has(r, ui.mx, ui.my) && (ui.wheel != 0 || ui.wheel_x != 0)) {
    U.chip_x -= (ui.wheel_x != 0 ? -ui.wheel_x : ui.wheel) * DP(60);
    ui.wheel = ui.wheel_x = 0;
  }
  if (U.chip_reveal) {
    float x = pad;
    for (int s = 0; s < U.sec; s++) x += w[s] + gap;
    if (x - DP(24) < U.chip_x) U.chip_x = x - DP(24);
    if (x + w[U.sec] + DP(24) > U.chip_x + r.w) U.chip_x = x + w[U.sec] + DP(24) - r.w;
    U.chip_reveal = false;
  }
  U.chip_x = FM_CLAMP(U.chip_x, 0.0f, max);
  gfx_clip_push(r);
  float x = r.x + pad - U.chip_x;
  for (int s = 0; s < LIB_SEC_COUNT; s++) {
    FmRect c = { x, r.y + (r.h - ch) * 0.5f, w[s], ch };
    x += w[s] + gap;
    if (!gfx_visible(c)) continue;
    bool on = U.sec == s;
    int f = ui_hit(ui_idn(base, (u32)s + 1), c);
    if (on) gfx_rrect(c, ch * 0.5f, T.accent);
    else {
      gfx_rrect(c, ch * 0.5f, T.surface);
      gfx_rrect_line(c, ch * 0.5f, DP(1), T.border);
      if (f & UI_HOVER) gfx_rrect(c, ch * 0.5f, T.hover);
    }
    FmColor fg = on ? T.on_accent : T.text;
    FmIcon ic = s == LIB_SEC_FAVS && on ? IC_HEART_FILL : kSec[s].icon;
    icon_draw(ic, FM_RECT(c.x + DP(12), c.y + (ch - is) * 0.5f, is, is),
              on ? fg : s == LIB_SEC_FAVS ? T.danger : T.text2);
    font_draw(on ? FONT_BOLD : FONT_REGULAR, fs, c.x + DP(12) + is + DP(6), c.y + (ch - font_line_h(fs)) * 0.5f,
              kSec[s].label, -1, fg);
    if (f & UI_CLICK) set_section(s);
  }
  gfx_clip_pop();
}

/* ---- top bar ------------------------------------------------------------------- */

static void go_back(void) {
  if (U.search_open) { U.search_open = false; U.filter[0] = 0; ui_focus(0); return; }
  if (U.sec == LIB_SEC_PLAYLISTS && qui_playlists_back()) return;
  if (U.in_group) { U.in_group = false; return; }
  lib_ui_close();
}

static void draw_top(FmRect r, bool narrow) {
  u32 base = ui_id("lib.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text, U.in_group ? "Back" : "Back to files")) go_back();
  rect_cut_left(&in, DP(4));
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, FM_HEX(0xFFFFFF), 0.15f), T.accent);
    icon_draw(IC_LIBRARY, rect_inset(m, DP(5)), T.on_accent);
    rect_cut_left(&in, DP(8));
  }
  /* right: rescan, search */
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (lib_scanning()) {
    ui_spinner(rect_center(b, DP(20), DP(20)), T.accent);
    ui_tip_track(ui_idn(base, 2), ui_hit(ui_idn(base, 2), b), "Scanning");
  } else if (ui_icon_btn(ui_idn(base, 2), b, IC_REFRESH, T.text2, "Rescan (F5)")) {
    lib_rescan();
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_toggle_btn(ui_idn(base, 3), b, IC_SEARCH, U.search_open, "Search (Ctrl+F)")) {
    U.search_open = !U.search_open;
    U.search_focus = U.search_open;
    if (!U.search_open) { U.filter[0] = 0; ui_focus(0); }
  }
  /* title and a summary line */
  const FmLibData *d = lib_data();
  char sub[96], a[32], v[32];
  if (lib_scanning()) {
    fm_snprintf(sub, sizeof sub, "Scanning\xE2\x80\xA6 %d files found", FM_MAX(lib_scan_found(), d->n));
  } else {
    fmt_n(d->nsongs, "song", "songs", a, sizeof a);
    fmt_n(d->nvideos, "video", "videos", v, sizeof v);
    fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s  \xC2\xB7  %d favorites", a, v, lib_fav_count());
  }
  float tl = font_line_h(ui.m.font_title * (narrow ? 0.9f : 1.0f)), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title * (narrow ? 0.9f : 1.0f), in.x + DP(2), y, narrow ? "Library" : "Media library",
                     in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

/* ---- public ---------------------------------------------------------------------- */

void lib_ui_set_reveal(void (*fn)(const char *path)) { U.reveal = fn; }

int lib_ui_section(const char *name) {
  static const char *const kNames[] = { "songs", "artists", "albums", "videos", "favorites", "recent", "playlists",
                                       "folders" };
  if (!name) return -1;
  for (int i = 0; i < LIB_SEC_COUNT; i++)
    if (fm_stricmp(name, kNames[i]) == 0 || fm_stricmp(name, kSec[i].label) == 0) return i;
  if (!fm_stricmp(name, "favs")) return LIB_SEC_FAVS;
  return -1;
}

bool lib_ui_is_open(void) { return U.open; }

void lib_ui_open(int section, const char *hint_dir) {
  ID_MENU = ui_id("lib.menu");
  ID_ADD = ui_id("lib.addmenu");
  ID_GROUP = ui_id("lib.groupmenu");
  if (section >= 0 && section < LIB_SEC_COUNT) { U.sec = section; U.in_group = false; }
  fm_strlcpy(U.hint, hint_dir ? hint_dir : "", sizeof U.hint);
  U.open = true;
  U.chip_reveal = true;
  U.rows_ok = false;
  /* the index is built on first use, and refreshed when it is a minute old */
  u64 now = plat_now_ms();
  if (!lib_scanning() && (U.last_scan == 0 || now - U.last_scan > 60000)) {
    U.last_scan = now;
    lib_rescan();
  }
  ui_redraw();
}

void lib_ui_close(void) {
  U.open = false;
  U.search_open = false;
  U.filter[0] = 0;
  ui_focus(0);
  fm_free(U.rows);
  U.rows = NULL;
  U.nrows = U.caprows = 0;
  U.rows_ok = false;
  thumb_cancel_all();
  ui_redraw();
}

void lib_ui_frame(FmRect area) {
  menu_results();
  if (!U.open) return;
  theme_draw_bg(area);
  bool narrow = area.w / ui.scale < 760;
  FmRect r = area;
  FmRect top = rect_cut_top(&r, ui.m.bar_h);
  if (audio_mini_active()) {
    FmRect mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
    audio_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  }
  float gpx = DP(narrow ? 6 : 8);
  r = rect_inset2(r, gpx, 0);
  rect_cut_bottom(&r, gpx);
  if (narrow) {
    FmRect chips = rect_cut_top(&r, DP(ui.touch_mode ? 50 : 44));
    draw_chips(chips);
  } else {
    rect_cut_top(&r, DP(2));
    FmRect rail = rect_cut_left(&r, DP(220));
    rect_cut_left(&r, gpx);
    draw_rail(rail);
  }
  draw_body(r);
  draw_top(top, narrow);
  if (!U.open) return;
  /* keys */
  if (ui_input_ok() && !ui_menu_is_open()) {
    if (ui_key(SDLK_f, KMOD_CTRL)) { U.search_open = true; U.search_focus = true; }
    if (ui_key(SDLK_F5, 0) || ui_key(SDLK_r, KMOD_CTRL)) lib_rescan();
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) go_back();
    if (!ui.focus) {
      if (ui_key(SDLK_PAGEDOWN, 0)) { set_section((U.sec + 1) % LIB_SEC_COUNT); }
      if (ui_key(SDLK_PAGEUP, 0)) { set_section((U.sec + LIB_SEC_COUNT - 1) % LIB_SEC_COUNT); }
    }
  }
}

/* ---- demo (--demo-library) ------------------------------------------------------ */

void lib_demo(const char *dir, int section) {
  lib_test_reset(dir, true);
  lib_folder_set(dir, true);
  lib_rescan();
  lib_scan_wait();
  const FmLibData *d = lib_data();
  /* a few favorites and recents so every section has something to show */
  for (int i = 0; i < d->nsongs && i < 7; i += 2) lib_fav_set(lib_str(d->items[d->songs[i]].path), true);
  if (d->nvideos > 0) lib_fav_set(lib_str(d->items[d->videos[0]].path), true);
  for (int i = FM_MIN(d->nsongs, 4) - 1; i >= 0; i--) lib_note_played(lib_str(d->items[d->songs[i]].path));
  if (d->nvideos > 1) lib_note_played(lib_str(d->items[d->videos[1]].path));
  U.last_scan = plat_now_ms();
  lib_ui_open(section, dir);
}
