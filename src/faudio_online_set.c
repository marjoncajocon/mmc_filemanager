/* faudio_online_set.c -- the "Online audio" section of the settings dialog.
**
** Design decisions:
**   - Like fonline_set.c and fphoto_set.c: the app's settings dialog only
**     reserves the height and calls aonline_settings(); one layout routine
**     measures and draws, so the scroll height always matches what is drawn.
**   - Two keys (Jamendo's client ID, Freesound's API key), each a password
**     field with show/hide and paste and a link to the page that hands out
**     the free key. Pasting keeps the first word of the clipboard.
**   - FFmpeg: the player decodes MP3, FLAC, WAV and Vorbis itself; many
**     stations send AAC or Opus, which need FFmpeg's libraries. The section
**     says whether they were found (the same check the video player uses,
**     ff_available), so a silent AAC station is never a mystery.
**   - The download folder is shared with online videos and photos and is
**     set in that section; this one says so instead of repeating it.
*/
#include "faudio_online_int.h"
#include "fdec_vid.h"

static bool g_show[2];

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

static void key_row(FmRect *r, u32 base, int k, const char *name, const char *what, const char *url, char *buf,
                    int cap, bool draw) {
  FmRect lr = rect_cut_top(r, lh() + DP(6));
  if (draw) {
    char l[96];
    fm_snprintf(l, sizeof l, "%s %s", name, what);
    float x = font_draw(FONT_REGULAR, ui.m.font, lr.x + DP(2), lr.y, l, -1, T.text);
    if (buf[0]) {
      float is = DP(14);
      icon_draw(IC_CHECK, FM_RECT(x + DP(6), lr.y + (lh() - is) * 0.5f, is, is), T.success);
    }
  }
  if (link_right(ui_idn(base, 1), FM_RECT(lr.x, lr.y, lr.w, lh()), "Get a free key", draw)) ao_open_url(url);
  FmRect row = rect_cut_top(r, fh());
  rect_cut_top(r, DP(10));
  if (!draw) return;
  float bs = fh();
  FmRect paste = rect_cut_right(&row, bs);
  FmRect eye = rect_cut_right(&row, bs);
  rect_cut_right(&row, DP(4));
  char hint[96];
  fm_snprintf(hint, sizeof hint, "Paste your %s %s", name, what);
  int res = ui_textfield(ui_idn(base, 2), row, buf, cap, hint, g_show[k] ? 0 : UI_TF_PASSWORD);
  if (res & UI_TF_CHANGED) ao_conf_dirty();
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
      ao_conf_dirty();
      ui_toast("%s key pasted", name);
    }
  }
}

/* The FFmpeg card: found or not, and what it is for. */
static void ffmpeg_card(FmRect *r, bool draw) {
  bool ok = ff_available();
  float pad = DP(12), is = DP(18);
  float tw = r->w - pad * 2 - is - DP(10);
  const char *t = ok ? "AAC and Opus stations play" : "Some stations need FFmpeg";
  char s[400];
  if (ok)
    fm_snprintf(s, sizeof s, "%s is installed, so AAC, HE-AAC, Opus and WMA streams play as well as MP3, FLAC and "
                             "Vorbis.", ff_version_str());
  else
    fm_strlcpy(s, "MP3, FLAC, WAV and Vorbis play built in. Many stations and podcasts send AAC or Opus: those "
                  "need the FFmpeg libraries next to the app (the same ones the video player uses). Stations "
                  "that need it are marked and say so instead of staying silent.", sizeof s);
  float sh = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, s, T.text2, false);
  FmRect card = rect_cut_top(r, pad + lh() + DP(2) + sh + pad);
  if (draw) {
    gfx_rrect(card, DP(12), T.surface2);
    icon_draw(ok ? IC_CHECK : IC_INFO, FM_RECT(card.x + pad, card.y + pad + (lh() - is) * 0.5f, is, is),
              ok ? T.success : T.accent);
    float x = card.x + pad + is + DP(10);
    font_draw(FONT_BOLD, ui.m.font, x, card.y + pad, t, -1, T.text);
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, card.y + pad + lh() + DP(2), tw, s, T.text2, true);
  }
  rect_cut_top(r, DP(8));
}

static float layout(FmRect *r, u32 base, bool draw) {
  float y0 = r->y;
  FmRect h = rect_cut_top(r, ls() + DP(6));
  if (draw) font_draw(FONT_BOLD, ui.m.font_small, h.x + DP(2), h.y, "ONLINE AUDIO", -1, T.text2);
  note(r, "Radio Browser, Audius, the Internet Archive and podcasts need no key. Jamendo and Freesound give a free "
          "one.", T.text3, draw);
  rect_cut_top(r, DP(6));
  key_row(r, ui_idn(base, 10), 0, "Jamendo", "client ID", "https://devportal.jamendo.com/", conf.key_jamendo,
          sizeof conf.key_jamendo, draw);
  key_row(r, ui_idn(base, 20), 1, "Freesound", "API key", "https://freesound.org/apiv2/apply", conf.key_freesound,
          sizeof conf.key_freesound, draw);
  ffmpeg_card(r, draw);
  note(r, "Tracks download to the folder set under Online videos; licensed music gets a .txt with its credit. "
          "Favorites, recently played and subscriptions are kept on this device.", T.text3, draw);
  rect_cut_top(r, DP(14));
  return r->y - y0;
}

float aonline_settings_h(float w) {
  FmRect r = { 0, 0, w, 100000 };
  return layout(&r, 0, false);
}

void aonline_settings(FmRect *r, u32 base) { layout(r, base, true); }
