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
};

static bool g_open;
static FmScroll g_scroll;

bool home_is_open(void) { return g_open; }

void home_open(void) {
  g_open = true;
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
  float th = FM_CLAMP(tw * 0.62f, DP(compact ? 150 : 170), DP(230));
  int rows = (n + cols - 1) / cols;
  float head = DP(compact ? 64 : 84);
  float content = head + rows * th + (rows - 1) * gap + pad;

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
  gfx_clip_pop();
  ui_scrollbar(&g_scroll, view, content);
  int t = draw_top(top);
  if (t != HOME_NONE) act = t;
  /* keys: 1..6 open the tiles */
  if (ui_input_ok() && !ui.focus && !ui_menu_is_open())
    for (int i = 0; i < n; i++)
      if (ui_key((SDL_Keycode)(SDLK_1 + i), 0)) act = kTiles[i].act;
  return act;
}
