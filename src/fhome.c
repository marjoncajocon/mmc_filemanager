/* fhome.c -- the home screen (see fhome.h).
**
** Design decisions:
**   - It only draws and reports the tile picked; fapp.c opens the place, so
**     the views keep one way in (the same as the sidebar's places).
**   - Two columns on a phone, three when wide; each tile is a coloured card
**     with a big icon, the name and one line saying what is inside. The grid
**     scrolls when the window is short (a phone held sideways).
**   - The music / background video mini bar stays at the bottom, as on the
**     other screens.
**   - Favorites, under the tiles: what was starred or hearted anywhere (media
**     library songs and videos, folder bookmarks, online audio, online
**     photos), newest first. None of those lists keeps a time, so this file
**     keeps its own log (home-favs.txt: time, kind, key): a favourite it has
**     not seen yet is stamped "now" (just added: it comes first), and so is
**     one opened from here or opened as a file anywhere (home_fav_touch).
**     Entries whose favourite was removed leave the log, so adding it again
**     brings it to the top again.
*/
#include "fhome.h"
#include "fapp.h"
#include "fconf.h"
#include "ffont.h"
#include "fgfx.h"
#include "ficon.h"
#include "ftheme.h"
#include "ftitle.h"
#include "fview.h"
#include "flib.h"
#include "fplat.h"
#include "fthumb.h"
#include "fonline_int.h"
#include "fphoto.h"
#include "faudio_online.h"
#include "fwiki.h"
#include <time.h>

typedef struct Tile {
  int act;
  FmIcon icon;
  const char *title, *sub;
  u32 c0, c1;                          /* the card's gradient, top to bottom */
} Tile;

static const Tile kTiles[] = {
  { HOME_FILES, IC_FOLDER, "File manager", "Your files and folders, two panels", 0x4F8CFF, 0x3A6FD8 },
  { HOME_VIDEOS, IC_PLAY_BADGE, "Online videos", "YouTube, Dailymotion, Bilibili and more", 0xF0564A, 0xC9302C },
  { HOME_AUDIO, IC_RADIO, "Online audio", "Music, radio stations and podcasts", 0x9B6BF2, 0x6E44C9 },
  { HOME_PHOTOS, IC_ALBUM, "Online photos", "Free photos to view and download", 0xF2B53C, 0xD9822A },
  { HOME_LIBRARY, IC_LIBRARY, "Media library", "Your songs, videos and playlists", 0x2EC4A0, 0x16967A },
  { HOME_CLOUD, IC_CLOUD, "Cloud storage", "Google Drive, MEGA, Dropbox, WebDAV", 0x5AB4F0, 0x2F86C9 },
  { HOME_WIKI, IC_TEXT, "Online reading", "Wikipedia: articles, pictures, videos (more sites to come)", 0x7A8394, 0x4B5263 },
};

static bool g_open;
static FmScroll g_scroll;

/* ---- favorites: the log and the strip --------------------------------------------- */

enum { FK_FILE = 'f', FK_DIR = 'd', FK_AUDIO = 'a', FK_PHOTO = 'p', FK_WIKI = 'w' };
#define HF_LOG   400                   /* log entries kept */
#define HF_SCAN  40                    /* newest favourites read per list */
#define HF_SHOW  12                    /* cards on the home screen */
#define HF_KEY   600

typedef struct HfLog { char kind; char key[HF_KEY]; i64 t; } HfLog;
typedef struct HFav {
  char kind;
  char key[HF_KEY];
  char title[200], sub[120], thumb[512];
  FmIcon icon;
  i64 t;
} HFav;

static struct {
  HfLog *log;
  int nlog;
  bool loaded;
  i64 last;                            /* the newest stamp handed out */
  HFav *cand, fav[HF_SHOW];
  int ncand, nfav;
  u64 next;                            /* when the strip is rebuilt */
  char pick[FM_PATH_MAX];              /* HOME_FOLDER: the bookmark picked */
} H;

static i64 stamp(void) {
  i64 t = (i64)time(NULL) * 1000;
  if (t <= H.last) t = H.last + 1;
  return H.last = t;
}

static bool log_path(char *out, size_t cap) {
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "home-favs.txt");
}

static void log_load(void) {
  if (H.loaded) return;
  H.loaded = true;
  H.log = (HfLog *)fm_calloc(HF_LOG, sizeof *H.log);
  char path[FM_PATH_MAX], line[HF_KEY + 64];
  FILE *f = log_path(path, sizeof path) ? fm_fopen(path, "rb") : NULL;
  if (!f) return;
  while (H.nlog < HF_LOG && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
    if (!a || !b || b != a + 2 || !b[1]) continue;
    HfLog *e = &H.log[H.nlog++];
    e->t = 0;
    for (const char *d = line; *d >= '0' && *d <= '9'; d++) e->t = e->t * 10 + (*d - '0');   /* no strtoll in tcc */
    e->kind = a[1];
    fm_strlcpy(e->key, b + 1, sizeof e->key);
    if (e->t > H.last) H.last = e->t;
  }
  fclose(f);
}

static void log_save(void) {
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX];
  if (!log_path(path, sizeof path)) return;
  fm_snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  for (int i = 0; i < H.nlog; i++) fprintf(f, "%lld\t%c\t%s\n", (long long)H.log[i].t, H.log[i].kind, H.log[i].key);
  bool ok = fclose(f) == 0;
  if (ok) {
    plat_remove_file(path);
    plat_rename(tmp, path);
  }
}

static int log_find(char kind, const char *key) {
  for (int i = 0; i < H.nlog; i++)
    if (H.log[i].kind == kind && !strcmp(H.log[i].key, key)) return i;
  return -1;
}

/* Stamps (kind, key) now; false when it was already the newest. */
static void log_put(char kind, const char *key, i64 t) {
  int i = log_find(kind, key);
  if (i < 0) {
    if (H.nlog == HF_LOG) {                          /* full: the oldest goes */
      int old = 0;
      for (int k = 1; k < H.nlog; k++)
        if (H.log[k].t < H.log[old].t) old = k;
      H.log[old] = H.log[--H.nlog];
    }
    i = H.nlog++;
    H.log[i].kind = kind;
    fm_strlcpy(H.log[i].key, key, sizeof H.log[i].key);
  }
  H.log[i].t = t;
}

void home_fav_touch(char kind, const char *key) {
  if (!key || !key[0]) return;
  log_load();
  log_put(kind, key, stamp());
  log_save();
  H.next = 0;
}

static bool still_fav(const HfLog *e) {
  switch (e->kind) {
    case FK_FILE: return lib_is_fav(e->key);
    case FK_DIR: return conf_is_bookmark(e->key);
    case FK_AUDIO: return aonline_fav_has(e->key);
    case FK_WIKI: {
      char lang[16];
      const char *bar = strchr(e->key, '|');
      if (!bar || (size_t)(bar - e->key) >= sizeof lang) return false;
      memcpy(lang, e->key, (size_t)(bar - e->key));
      lang[bar - e->key] = 0;
      return wiki_saved_has(lang, bar + 1);
    }
    case FK_PHOTO: {
      char src[64];
      const char *bar = strchr(e->key, '|');
      if (!bar || (size_t)(bar - e->key) >= sizeof src) return false;
      memcpy(src, e->key, (size_t)(bar - e->key));
      src[bar - e->key] = 0;
      return photo_fav_has(src, bar + 1);
    }
    default: return false;
  }
}

static HFav *cand_add(char kind, const char *key) {
  if (H.ncand >= HF_SCAN * 4) return NULL;
  HFav *f = &H.cand[H.ncand++];
  memset(f, 0, sizeof *f);
  f->kind = kind;
  fm_strlcpy(f->key, key, sizeof f->key);
  return f;
}

static void collect(void) {
  if (!H.cand) H.cand = (HFav *)fm_calloc(HF_SCAN * 4, sizeof *H.cand);
  H.ncand = 0;
  /* media library: songs, videos, any file */
  for (int i = 0; i < lib_fav_count() && i < HF_SCAN; i++) {
    const char *path = lib_fav_at(i);
    HFav *f = cand_add(FK_FILE, path);
    if (!f) break;
    fm_strlcpy(f->title, fm_path_base(path), sizeof f->title);
    FmType ty = fm_type_from_name(path);
    f->icon = ty == FT_AUDIO ? IC_MUSIC : ty == FT_VIDEO ? IC_VIDEO : ty == FT_IMAGE ? IC_IMAGE : IC_FILE;
    fm_strlcpy(f->sub, ty == FT_AUDIO ? "Song" : ty == FT_VIDEO ? "Video" : ty == FT_IMAGE ? "Picture" : "File",
               sizeof f->sub);
  }
  /* folder bookmarks (the list is oldest first) */
  for (int i = conf.nbookmarks - 1, k = 0; i >= 0 && k < HF_SCAN; i--, k++) {
    HFav *f = cand_add(FK_DIR, conf.bookmarks[i]);
    if (!f) break;
    const char *b = fm_path_base(conf.bookmarks[i]);
    fm_strlcpy(f->title, b && b[0] ? b : conf.bookmarks[i], sizeof f->title);
    fm_strlcpy(f->sub, "Folder", sizeof f->sub);
    f->icon = IC_FOLDER;
  }
  /* online audio */
  FmAsrcItem ai;
  char key[HF_KEY];
  for (int i = 0; i < aonline_fav_count() && i < HF_SCAN; i++) {
    if (!aonline_fav_get(i, &ai, key, sizeof key)) continue;
    HFav *f = cand_add(FK_AUDIO, key);
    if (!f) break;
    fm_strlcpy(f->title, ai.title, sizeof f->title);
    const char *k = ai.kind == AITEM_STATION ? "Station" : ai.kind == AITEM_PODCAST ? "Podcast"
                    : ai.kind == AITEM_ALBUM ? "Album" : "Track";
    if (ai.artist[0]) fm_snprintf(f->sub, sizeof f->sub, "%s \xC2\xB7 %s", k, ai.artist);
    else fm_strlcpy(f->sub, k, sizeof f->sub);
    fm_strlcpy(f->thumb, ai.art, sizeof f->thumb);
    f->icon = ai.kind == AITEM_STATION ? IC_RADIO : ai.kind == AITEM_PODCAST ? IC_PODCAST : IC_MUSIC;
  }
  /* saved Wikipedia articles */
  for (int i = 0; i < wiki_saved_count() && i < HF_SCAN; i++) {
    char wl[16], wt[256], wd[256], wth[512];
    if (!wiki_saved_get(i, wl, sizeof wl, wt, sizeof wt, wd, sizeof wd, wth, sizeof wth)) continue;
    fm_snprintf(key, sizeof key, "%s|%s", wl, wt);
    HFav *f = cand_add(FK_WIKI, key);
    if (!f) break;
    fm_strlcpy(f->title, wt, sizeof f->title);
    if (wd[0]) fm_snprintf(f->sub, sizeof f->sub, "Wikipedia \xC2\xB7 %s", wd);
    else fm_strlcpy(f->sub, "Wikipedia", sizeof f->sub);
    fm_strlcpy(f->thumb, wth, sizeof f->thumb);
    f->icon = IC_TEXT;
  }
  /* online photos */
  FmPsrcItem pi;
  char src[32];
  for (int i = 0; i < photo_fav_count() && i < HF_SCAN; i++) {
    if (!photo_fav_get(i, &pi, src, sizeof src)) continue;
    fm_snprintf(key, sizeof key, "%s|%s", src, pi.id);
    HFav *f = cand_add(FK_PHOTO, key);
    if (!f) break;
    fm_strlcpy(f->title, pi.title[0] ? pi.title : pi.id, sizeof f->title);
    if (pi.author[0]) fm_snprintf(f->sub, sizeof f->sub, "Photo \xC2\xB7 %s", pi.author);
    else fm_strlcpy(f->sub, "Photo", sizeof f->sub);
    fm_strlcpy(f->thumb, pi.thumb, sizeof f->thumb);
    f->icon = IC_IMAGE;
  }
}

static int fav_cmp(const void *a, const void *b) {
  i64 x = ((const HFav *)a)->t, y = ((const HFav *)b)->t;
  return x < y ? 1 : x > y ? -1 : 0;
}

static void rebuild(void) {
  log_load();
  collect();
  bool dirty = false;
  /* new favourites: stamped now, keeping each list's own newest-first order */
  int fresh = 0;
  for (int i = 0; i < H.ncand; i++)
    if (log_find(H.cand[i].kind, H.cand[i].key) < 0) fresh++;
  if (fresh) {
    i64 base = stamp() + fresh;
    H.last = base;
    for (int i = 0, k = 0; i < H.ncand; i++)
      if (log_find(H.cand[i].kind, H.cand[i].key) < 0) log_put(H.cand[i].kind, H.cand[i].key, base - k++);
    dirty = true;
  }
  /* removed favourites leave the log (adding one again makes it new again) */
  for (int i = 0; i < H.nlog;) {
    if (!still_fav(&H.log[i])) {
      H.log[i] = H.log[--H.nlog];
      dirty = true;
    } else {
      i++;
    }
  }
  if (dirty) log_save();
  for (int i = 0; i < H.ncand; i++) {
    int k = log_find(H.cand[i].kind, H.cand[i].key);
    H.cand[i].t = k >= 0 ? H.log[k].t : 0;
  }
  qsort(H.cand, (size_t)H.ncand, sizeof *H.cand, fav_cmp);
  H.nfav = FM_MIN(H.ncand, HF_SHOW);
  memcpy(H.fav, H.cand, (size_t)H.nfav * sizeof *H.fav);
}

const char *home_picked_folder(void) { return H.pick; }

/* A card picked: everything but a folder opens here; a folder is fapp's
** (the panels), so it comes back as HOME_FOLDER. */
static int fav_open(const HFav *f) {
  HFav c = *f;                           /* the touch below rebuilds the strip */
  home_fav_touch(c.kind, c.key);
  switch (c.kind) {
    case FK_FILE:
      if (!plat_exists(c.key)) { ui_toast("%s is not there any more", c.title); return HOME_NONE; }
      app_open(c.key, NULL, 0, 0);
      return HOME_NONE;
    case FK_DIR:
      fm_strlcpy(H.pick, c.key, sizeof H.pick);
      return HOME_FOLDER;
    case FK_AUDIO:
      aonline_fav_open(c.key);
      return HOME_NONE;
    case FK_WIKI: {
      char lang[16];
      const char *bar = strchr(c.key, '|');
      if (!bar || (size_t)(bar - c.key) >= sizeof lang) return HOME_NONE;
      memcpy(lang, c.key, (size_t)(bar - c.key));
      lang[bar - c.key] = 0;
      wiki_open_article(lang, bar + 1);
      return HOME_NONE;
    }
    case FK_PHOTO: {
      char src[64];
      const char *bar = strchr(c.key, '|');
      if (!bar || (size_t)(bar - c.key) >= sizeof src) return HOME_NONE;
      memcpy(src, c.key, (size_t)(bar - c.key));
      src[bar - c.key] = 0;
      photo_fav_open(src, bar + 1);
      return HOME_NONE;
    }
    default: return HOME_NONE;
  }
}

static void fav_art(FmRect r, const HFav *f) {
  float rad = DP(10);
  if (f->thumb[0]) {
    float fade = 1;
    SDL_Texture *tex = othumb_get_ex(f->thumb, (int)r.w * 2, 1.0f, NULL, &fade);
    if (tex) {
      gfx_tex_rounded(tex, r, rad, FM_RGBA(255, 255, 255, (u8)(255 * fade)));
      if (fade < 1) ui_animate();
      return;
    }
  } else if (f->kind == FK_FILE && conf.thumbnails) {
    int k = lib_find(f->key);
    if (k >= 0) {
      const FmLibItem *it = &lib_data()->items[k];
      SDL_Texture *tex = thumb_get(f->key, it->mtime, it->size, (int)r.w * 2);
      int tw = 0, th = 0;
      if (tex && SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) {
        gfx_rrect(r, rad, T.surface3);
        float s = FM_MAX(r.w / (float)tw, r.h / (float)th);
        gfx_clip_push(r);
        gfx_tex_rounded(tex, rect_center(r, (float)tw * s, (float)th * s), rad, FM_HEX(0xFFFFFF));
        gfx_clip_pop();
        return;
      }
    }
  }
  u32 h = ui_id(f->key);
  static const u32 kC[] = { 0x4F8CFF, 0xF0564A, 0x9B6BF2, 0xF2B53C, 0x2EC4A0, 0x5AB4F0 };
  FmColor a = FM_HEX(kC[h % FM_COUNT(kC)]);
  gfx_rrect_vgrad(r, rad, a, col_mix(a, FM_HEX(0x101018), 0.35f));
  float is = r.w * 0.5f;
  icon_draw(f->icon, rect_center(r, is, is), FM_HEX(0xFFFFFF));
}

static bool fav_card(u32 id, FmRect r, const HFav *f) {
  int fl = ui_hit(id, r);
  bool hot = (fl & UI_HOVER) != 0, down = (fl & UI_HELD) != 0;
  FmRect c = down ? rect_inset(r, DP(1)) : r;
  gfx_rrect(c, DP(14), hot ? T.surface2 : T.surface);
  gfx_rrect_line(c, DP(14), DP(1), T.border);
  float as = c.h - DP(16);
  FmRect art = { c.x + DP(8), c.y + DP(8), as, as };
  fav_art(art, f);
  float x = art.x + art.w + DP(10), w = c.x + c.w - x - DP(10);
  float tl = font_line_h(ui.m.font), sl = font_line_h(ui.m.font_small);
  float y = c.y + (c.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, f->title, w, T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + tl, f->sub, w, T.text2);
  if (hot) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (fl & UI_CLICK) != 0;
}

bool home_is_open(void) { return g_open; }

void home_open(void) {
  g_open = true;
  H.next = 0;                          /* favourites added elsewhere show at once */
  ui_redraw();
}

void home_close(void) {
  g_open = false;
  ui_redraw();
}

static int draw_top(FmRect r) {
  u32 base = ui_id("home.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  int act = HOME_NONE;
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect m = rect_center(rect_cut_left(&in, bs), DP(28), DP(28));
  gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, FM_HEX(0xFFFFFF), 0.15f), T.accent);
  icon_draw(IC_FOLDER, rect_inset(m, DP(6)), T.on_accent);
  rect_cut_left(&in, DP(6));
  FmRect b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_SETTINGS, T.text2, "Settings")) act = HOME_SETTINGS;
  if (theme_has_both(conf.theme)) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 2), b, T.dark ? IC_SUN : IC_MOON, T.text2, T.dark ? "Light mode" : "Dark mode"))
      act = HOME_THEME;
  }
  float y = in.y + (in.h - font_line_h(ui.m.font_title)) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x + DP(4), y, "MMC File Manager", in.w - DP(8), T.text);
  return act;
}

/* One card; true when clicked. */
static bool tile(u32 id, FmRect r, const Tile *t, bool compact) {
  int f = ui_hit(id, r);
  bool hot = (f & UI_HOVER) != 0, down = (f & UI_HELD) != 0;
  FmRect c = down ? rect_inset(r, DP(2)) : r;
  float rad = DP(18);
  gfx_shadow(c, rad, DP(hot ? 16 : 10), col_alpha(T.shadow, hot ? 0.9f : 0.6f));
  FmColor top = FM_HEX(t->c0), bot = FM_HEX(t->c1);
  if (hot) { top = col_mix(top, FM_HEX(0xFFFFFF), 0.08f); bot = col_mix(bot, FM_HEX(0xFFFFFF), 0.08f); }
  gfx_rrect_vgrad(c, rad, top, bot);
  FmColor white = FM_HEX(0xFFFFFF);
  FmRect in = rect_inset(c, DP(compact ? 14 : 18));
  /* the icon in a soft round badge, top left */
  float isz = DP(compact ? 44 : 56);
  FmRect badge = { in.x, in.y, isz, isz };
  gfx_circle(badge.x + isz * 0.5f, badge.y + isz * 0.5f, isz * 0.5f, col_alpha(white, 0.18f));
  icon_draw(t->icon, rect_inset(badge, isz * 0.24f), white);
  /* name and what is inside, at the bottom */
  float ts = ui.m.font_title * (compact ? 0.95f : 1.1f), ss = ui.m.font_small;
  float sub_h = font_draw_wrap(FONT_REGULAR, ss, in.x, 0, in.w, t->sub, white, false);
  float ty = in.y + in.h - sub_h - font_line_h(ts) - DP(2);
  font_draw_ellipsis(FONT_BOLD, ts, in.x, ty, t->title, in.w, white);
  font_draw_wrap(FONT_REGULAR, ss, in.x, ty + font_line_h(ts) + DP(2), in.w, t->sub, col_alpha(white, 0.85f), true);
  if (hot) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

int home_frame(FmRect area) {
  if (!g_open) return HOME_NONE;
  theme_draw_bg(area);
  FmRect r = area;
  FmRect top = rect_cut_top(&r, ui.m.bar_h);
  if (video_mini_active()) {
    FmRect mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
    video_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  } else if (audio_mini_active()) {
    FmRect mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
    audio_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  }

  int n = (int)FM_COUNT(kTiles);
  float wdp = r.w / ui.scale;
  int cols = wdp >= 900 ? 3 : 2;
  bool compact = wdp < 520;
  float gap = DP(compact ? 12 : 18), pad = DP(compact ? 14 : 28);
  float maxw = DP(1100);
  FmRect view = r;
  FmRect grid = rect_inset2(r, pad, 0);
  if (grid.w > maxw) { grid.x += (grid.w - maxw) * 0.5f; grid.w = maxw; }
  float tw = (grid.w - gap * (cols - 1)) / cols;
  if (SDL_GetTicks64() >= H.next) {
    rebuild();
    H.next = SDL_GetTicks64() + 1000;
  }
  /* with favourites the tiles get shorter, so the strip shows without scrolling */
  float th = H.nfav ? FM_CLAMP(tw * 0.40f, DP(compact ? 118 : 128), DP(170))
                    : FM_CLAMP(tw * 0.62f, DP(compact ? 150 : 170), DP(230));
  int rows = (n + cols - 1) / cols;
  float head = DP(compact ? 64 : 84);
  int fcols = wdp >= 900 ? 3 : wdp >= 560 ? 2 : 1;
  float fh = DP(ui.touch_mode ? 72 : 64), fgap = DP(10);
  float fhead = DP(compact ? 56 : 64);
  int frows = H.nfav ? (H.nfav + fcols - 1) / fcols : 0;
  float fbody = H.nfav ? frows * fh + (frows - 1) * fgap : DP(48);
  float content = head + rows * th + (rows - 1) * gap + fhead + fbody + pad;

  ui_scroll(&g_scroll, ui_id("home.scroll"), view, content);
  gfx_clip_push(view);
  float y0 = view.y - g_scroll.y;
  /* greeting */
  float gy = y0 + (head - font_line_h(ui.m.font_title * 1.3f) - font_line_h(ui.m.font_small)) * 0.5f;
  font_draw(FONT_BOLD, ui.m.font_title * 1.3f, grid.x, gy, "What would you like to open?", -1, T.text);
  font_draw(FONT_REGULAR, ui.m.font_small, grid.x, gy + font_line_h(ui.m.font_title * 1.3f),
            "Developed by MMC Solo Dev (Marjon Cajocon)", -1, T.text2);
  int act = HOME_NONE;
  u32 base = ui_id("home.tile");
  for (int i = 0; i < n; i++) {
    FmRect c = { grid.x + (i % cols) * (tw + gap), y0 + head + (i / cols) * (th + gap), tw, th };
    if (!gfx_visible(c)) continue;
    if (tile(ui_idn(base, (u32)i), c, &kTiles[i], compact)) act = kTiles[i].act;
  }
  /* favourites: newest first */
  float fy = y0 + head + rows * th + (rows - 1) * gap;
  float hy = fy + fhead - font_line_h(ui.m.font_title) - DP(8);
  float hx = font_draw(FONT_BOLD, ui.m.font_title, grid.x, hy, "Favorites", -1, T.text);
  font_draw(FONT_REGULAR, ui.m.font_small, hx + DP(10), hy + font_line_h(ui.m.font_title) - font_line_h(ui.m.font_small),
            "the latest added or opened first", -1, T.text2);
  fy += fhead;
  if (!H.nfav) {
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, grid.x, fy + DP(6), grid.w,
                   "Star a folder or a Wikipedia article, or heart a song, a video, a station or a photo: it shows up here.", T.text2, true);
  }
  float fw = (grid.w - fgap * (fcols - 1)) / fcols;
  u32 fbase = ui_id("home.fav");
  for (int i = 0; i < H.nfav; i++) {
    FmRect c = { grid.x + (i % fcols) * (fw + fgap), fy + (i / fcols) * (fh + fgap), fw, fh };
    if (!gfx_visible(c)) continue;
    if (fav_card(ui_idn(fbase, (u32)i), c, &H.fav[i])) {
      int a = fav_open(&H.fav[i]);
      if (a != HOME_NONE) act = a;
      break;
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&g_scroll, view, content);
  int t = draw_top(top);
  if (t != HOME_NONE) act = t;
  /* keys: 1..7 open the tiles */
  if (ui_input_ok() && !ui.focus && !ui_menu_is_open())
    for (int i = 0; i < n; i++)
      if (ui_key((SDL_Keycode)(SDLK_1 + i), 0)) act = kTiles[i].act;
  return act;
}
