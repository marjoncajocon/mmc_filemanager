/* fphoto_set.c -- the "Online photos" section of the settings dialog.
**
** Design decisions:
**   - Like fonline_set.c: the app's settings dialog only reserves the
**     height and calls photo_settings(); one layout routine measures and
**     draws, so the scroll height always matches what is drawn.
**   - Three keys, each a password field with show/hide and paste, and a
**     link to the page that hands out the free key. Pasting keeps the
**     first word of the clipboard: keys have no spaces, copied text often
**     has a trailing line break.
**   - The download folder and safe search are shared with online videos
**     and live in that section; this one says so instead of repeating them.
*/
#include "fphoto_int.h"

static bool g_show[3];

static float fh(void) { return DP(ui.touch_mode ? 48 : 40); }
static float lh(void) { return font_line_h(ui.m.font); }
static float ls(void) { return font_line_h(ui.m.font_small); }

static bool link_right(u32 id, FmRect r, const char *label, bool draw) {
  float w = font_width(FONT_BOLD, ui.m.font_small, label, -1) + DP(20);
  if (!draw) return false;
  FmRect b = { r.x + r.w - w, r.y, w, r.h };
  int f = ui_hit(id, b);
  float y = b.y + (b.h - ls()) * 0.5f;
  float x = font_draw(FONT_BOLD, ui.m.font_small, b.x + DP(2), y, label, -1, T.accent);
  icon_draw(IC_OPEN_WITH, FM_RECT(x + DP(4), y + (ls() - DP(12)) * 0.5f, DP(12), DP(12)), T.accent);
  if (f & UI_HOVER) {
    gfx_rect(FM_RECT(b.x + DP(2), y + ls() - DP(2), x - b.x - DP(2), DP(1)), T.accent);
    ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  }
  return (f & UI_CLICK) != 0;
}

static void note(FmRect *r, const char *s, FmColor c, bool draw) {
  float h = font_draw_wrap(FONT_REGULAR, ui.m.font_small, r->x + DP(2), r->y, r->w - DP(4), s, c, draw);
  rect_cut_top(r, h + DP(4));
}

static void key_row(FmRect *r, u32 base, int k, const char *name, const char *url, char *buf, int cap, bool draw) {
  FmRect lr = rect_cut_top(r, lh() + DP(6));
  if (draw) {
    float x = font_draw(FONT_REGULAR, ui.m.font, lr.x + DP(2), lr.y, name, -1, T.text);
    if (buf[0]) {
      float is = DP(14);
      icon_draw(IC_CHECK, FM_RECT(x + DP(6), lr.y + (lh() - is) * 0.5f, is, is), T.success);
    }
  }
  if (link_right(ui_idn(base, 1), FM_RECT(lr.x, lr.y, lr.w, lh()), "Get a free key", draw)) ph_open_url(url);
  FmRect row = rect_cut_top(r, fh());
  rect_cut_top(r, DP(10));
  if (!draw) return;
  float bs = fh();
  FmRect paste = rect_cut_right(&row, bs);
  FmRect eye = rect_cut_right(&row, bs);
  rect_cut_right(&row, DP(4));
  char hint[96];
  fm_snprintf(hint, sizeof hint, "Paste your %s key", name);
  int res = ui_textfield(ui_idn(base, 2), row, buf, cap, hint, g_show[k] ? 0 : UI_TF_PASSWORD);
  if (res & UI_TF_CHANGED) ph_conf_dirty();
  if (ui_icon_btn(ui_idn(base, 3), eye, g_show[k] ? IC_EYE_OFF : IC_EYE, T.text2, g_show[k] ? "Hide" : "Show"))
    g_show[k] = !g_show[k];
  if (ui_icon_btn(ui_idn(base, 4), paste, IC_PASTE, T.text2, "Paste")) {
    char *c = ui_clipboard_get();
    if (c) {
      char *p = c;
      while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
      size_t n = strcspn(p, " \r\n\t");
      p[n] = 0;
      fm_strlcpy(buf, p, (size_t)cap);
      fm_free(c);
      ph_conf_dirty();
      ui_toast("%s key pasted", name);
    }
  }
}

static float layout(FmRect *r, u32 base, bool draw) {
  float y0 = r->y;
  FmRect h = rect_cut_top(r, ls() + DP(6));
  if (draw) font_draw(FONT_BOLD, ui.m.font_small, h.x + DP(2), h.y, "ONLINE PHOTOS", -1, T.text2);
  note(r, "Openverse, Wikimedia Commons, NASA and the Art Institute of Chicago need no key. Pexels, Unsplash and "
          "Pixabay give a free one.", T.text3, draw);
  rect_cut_top(r, DP(6));
  key_row(r, ui_idn(base, 10), 0, "Pexels", "https://www.pexels.com/api/", conf.key_pexels, sizeof conf.key_pexels,
          draw);
  key_row(r, ui_idn(base, 20), 1, "Unsplash", "https://unsplash.com/developers", conf.key_unsplash,
          sizeof conf.key_unsplash, draw);
  key_row(r, ui_idn(base, 30), 2, "Pixabay", "https://pixabay.com/api/docs/", conf.key_pixabay, sizeof conf.key_pixabay,
          draw);
  /* licences */
  {
    float pad = DP(12), is = DP(18);
    float tw = r->w - pad * 2 - is - DP(10);
    const char *t = "Licences and credit";
    const char *s = "Every photo keeps its licence: public domain, a Creative Commons licence, or the site's own "
                    "(Pexels, Unsplash and Pixabay allow free use). Credit the author where the licence asks for it. "
                    "Each download gets a .txt with the title, author, licence and source page.";
    float sh = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, s, T.text2, false);
    FmRect card = rect_cut_top(r, pad + lh() + DP(2) + sh + pad);
    if (draw) {
      gfx_rrect(card, DP(12), T.surface2);
      icon_draw(IC_INFO, FM_RECT(card.x + pad, card.y + pad + (lh() - is) * 0.5f, is, is), T.accent);
      float x = card.x + pad + is + DP(10);
      font_draw(FONT_BOLD, ui.m.font, x, card.y + pad, t, -1, T.text);
      font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, card.y + pad + lh() + DP(2), tw, s, T.text2, true);
    }
    rect_cut_top(r, DP(8));
  }
  note(r, "Photos download to the folder set under Online videos, and Safe search there applies here too. Albums "
          "and favorites are kept on this device.", T.text3, draw);
  rect_cut_top(r, DP(14));
  return r->y - y0;
}

float photo_settings_h(float w) {
  FmRect r = { 0, 0, w, 100000 };
  return layout(&r, 0, false);
}

void photo_settings(FmRect *r, u32 base) { layout(r, base, true); }
