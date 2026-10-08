/* fonline_set.c -- the "Online videos" section of the settings dialog.
**
** Design decisions:
**   - The app's settings dialog (fapp.c) only reserves the height and calls
**     online_settings(); everything about online videos stays here.
**   - One layout routine serves both measuring and drawing, so the dialog's
**     scroll height can never disagree with what is drawn.
**   - Tool detection searches PATH, so it runs when the section comes into
**     view after a pause (the dialog was opened), not on every frame.
*/
#include "fonline_int.h"

#define KEY_URL "https://console.cloud.google.com/apis/library/youtube.googleapis.com"

static bool g_show_key;
static u64 g_last_draw;

static float fh(void) { return DP(ui.touch_mode ? 48 : 40); }
static float lh(void) { return font_line_h(ui.m.font); }
static float ls(void) { return font_line_h(ui.m.font_small); }

static void open_link(const char *url) {
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

/* A text link drawn at the right end of r (accent, underlined on hover). */
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

static void heading(FmRect *r, const char *s, bool draw) {
  FmRect h = rect_cut_top(r, ls() + DP(6));
  if (draw) font_draw(FONT_BOLD, ui.m.font_small, h.x + DP(2), h.y, s, -1, T.text2);
}

static void label(FmRect *r, const char *s, bool draw) {
  FmRect h = rect_cut_top(r, lh() + DP(6));
  if (draw) font_draw(FONT_REGULAR, ui.m.font, h.x + DP(2), h.y, s, -1, T.text);
}

static void note(FmRect *r, const char *s, FmColor c, bool draw) {
  float h = font_draw_wrap(FONT_REGULAR, ui.m.font_small, r->x + DP(2), r->y, r->w - DP(4), s, c, draw);
  rect_cut_top(r, h + DP(4));
}

/* A status line for a helper program: icon, name, detail, an optional button. */
static bool tool_row(FmRect *r, u32 id, bool ok, const char *name, const char *detail, const char *btn, bool busy,
                     bool draw) {
  float h = FM_MAX(lh() + ls() + DP(10), DP(ui.touch_mode ? 52 : 44));
  FmRect row = rect_cut_top(r, h);
  if (!draw) return false;
  float is = DP(18);
  icon_draw(ok ? IC_CHECK : IC_WARN, FM_RECT(row.x + DP(2), row.y + (h - is) * 0.5f, is, is), ok ? T.success : T.warn);
  float bw = btn ? font_width(FONT_BOLD, ui.m.font, btn, -1) + DP(32) : 0;
  float x = row.x + is + DP(12), w = row.x + row.w - x - bw - DP(8);
  float y = row.y + (h - lh() - ls()) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, name, w, T.text);
  font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + lh(), detail, w, ok ? T.text2 : T.text3);
  if (!btn) return false;
  FmRect b = { row.x + row.w - bw, row.y + (h - DP(ui.touch_mode ? 40 : 34)) * 0.5f, bw, DP(ui.touch_mode ? 40 : 34) };
  return ui_button(id, b, IC_NONE, btn, busy ? UI_BTN_TEXT : UI_BTN_TONAL) && !busy;
}

/* Text field bound to a conf string; returns true when it changed. */
static bool field(FmRect *r, u32 id, char *buf, int cap, const char *hint, bool draw) {
  FmRect f = rect_cut_top(r, fh());
  if (!draw) return false;
  int res = ui_textfield(id, f, buf, cap, hint, 0);
  return (res & UI_TF_CHANGED) != 0;
}

static float layout(FmRect *r, u32 base, bool draw) {
  float y0 = r->y;
  const OnTools *tl = otools(false);
  heading(r, "ONLINE VIDEOS", draw);

  /* YouTube API key */
  {
    FmRect lr = rect_cut_top(r, lh() + DP(6));
    if (draw) font_draw(FONT_REGULAR, ui.m.font, lr.x + DP(2), lr.y, "YouTube API key", -1, T.text);
    if (link_right(ui_idn(base, 1), FM_RECT(lr.x, lr.y, lr.w, lh()), "Get a free key", draw)) open_link(KEY_URL);
    FmRect row = rect_cut_top(r, fh());
    if (draw) {
      float bs = fh();
      FmRect paste = rect_cut_right(&row, bs);
      FmRect eye = rect_cut_right(&row, bs);
      rect_cut_right(&row, DP(4));
      int res = ui_textfield(ui_idn(base, 2), row, conf.yt_api_key, sizeof conf.yt_api_key, "Paste your key here",
                             g_show_key ? 0 : UI_TF_PASSWORD);
      if (res & UI_TF_CHANGED) online_conf_dirty();
      if (ui_icon_btn(ui_idn(base, 3), eye, g_show_key ? IC_EYE_OFF : IC_EYE, T.text2, g_show_key ? "Hide" : "Show"))
        g_show_key = !g_show_key;
      if (ui_icon_btn(ui_idn(base, 4), paste, IC_PASTE, T.text2, "Paste")) {
        char *c = ui_clipboard_get();
        if (c) {
          /* keys have no spaces: keep the first word of what was copied */
          char *p = c;
          while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
          size_t n = strcspn(p, " \r\n\t");
          p[n] = 0;
          fm_strlcpy(conf.yt_api_key, p, sizeof conf.yt_api_key);
          fm_free(c);
          online_conf_dirty();
          ui_toast("Key pasted");
        }
      }
    }
    rect_cut_top(r, DP(4));
    note(r, "Free: 10,000 units a day, about 100 searches. Without a key, YouTube search uses yt-dlp.", T.text3,
         draw);
    rect_cut_top(r, DP(8));
  }

  /* yt-dlp */
  {
    float frac = -1;
    char st[160];
    bool busy = oinst_running(&frac, st, sizeof st);
    const char *detail = tl->ytdlp ? tl->ytdlp_path : "Not found. Needed for YouTube playback, Dailymotion and other sites.";
    if (tool_row(r, ui_idn(base, 10), tl->ytdlp, "yt-dlp", detail,
                 busy ? "Getting\xE2\x80\xA6" : tl->ytdlp ? "Update" : "Get yt-dlp", busy, draw))
      oinst_start();
    FmRect pr = busy ? rect_cut_top(r, DP(26)) : rect_cut_top(r, DP(4));
    if (draw && busy) {
      ui_progress(FM_RECT(pr.x + DP(30), pr.y + DP(2), pr.w - DP(30), DP(5)), frac);
      font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, pr.x + DP(30), pr.y + DP(9), st, pr.w - DP(30), T.text2);
    }
  }

  /* JavaScript runtime */
  {
    char detail[FM_PATH_MAX + 40];
    if (tl->js) {
      const char *colon = strchr(tl->js_rt, ':');
      bool node = strncmp(tl->js_rt, "node", 4) == 0;
      /* "node:C:/x/node.exe": the first colon separates the kind */
      fm_snprintf(detail, sizeof detail, "%s  \xC2\xB7  %s", node ? "Node.js" : "Deno", colon ? colon + 1 : tl->js_rt);
    } else {
      fm_strlcpy(detail, "Not found. YouTube needs Deno or Node.js for yt-dlp.", sizeof detail);
    }
    if (tool_row(r, ui_idn(base, 20), tl->js, "JavaScript runtime", detail, tl->js ? NULL : "Get Deno", false, draw))
      open_link("https://deno.com");
    rect_cut_top(r, DP(6));
  }

  /* FFmpeg */
  {
    label(r, "FFmpeg folder (optional)", draw);
    if (field(r, ui_idn(base, 30), conf.ffmpeg_dir, sizeof conf.ffmpeg_dir, "Folder with ffmpeg, for merged downloads",
              draw)) {
      online_conf_dirty();
      otools(true);
    }
    rect_cut_top(r, DP(4));
    if (conf.ffmpeg_dir[0])
      note(r, tl->ffmpeg ? "ffmpeg found: downloads get the best picture and sound in one file."
                         : "No ffmpeg in that folder.", tl->ffmpeg ? T.success : T.warn, draw);
    else
      note(r, "Without it, downloads use formats that come as one file.", T.text3, draw);
    rect_cut_top(r, DP(8));
  }

  /* download folder */
  {
    char def[FM_PATH_MAX], hint[FM_PATH_MAX + 16];
    if (!plat_place(PLACE_DOWNLOADS, def, sizeof def)) fm_strlcpy(def, "Downloads", sizeof def);
    fm_snprintf(hint, sizeof hint, "Default: %s", def);
    label(r, "Download folder", draw);
    if (field(r, ui_idn(base, 40), conf.online_dl_dir, sizeof conf.online_dl_dir, hint, draw)) online_conf_dirty();
    rect_cut_top(r, DP(12));
  }

  /* quality, safe search */
  {
    label(r, "Video quality", draw);
    FmRect sg = rect_cut_top(r, DP(ui.touch_mode ? 40 : 34));
    if (draw) {
      static const char *const kQ[] = { "360p", "480p", "720p", "1080p" };
      static const int kH[] = { 360, 480, 720, 1080 };
      int sel = 2;
      for (int i = 0; i < 4; i++)
        if (conf.online_height >= kH[i]) sel = i;
      if (ui_segmented(ui_idn(base, 50), sg, kQ, 4, &sel)) {
        conf.online_height = kH[sel];
        online_conf_dirty();
      }
    }
    rect_cut_top(r, DP(6));
    FmRect sw = rect_cut_top(r, DP(ui.touch_mode ? 50 : 42));
    if (draw && ui_switch(ui_idn(base, 60), sw, "Safe search", &conf.online_safe)) online_conf_dirty();
    rect_cut_top(r, DP(14));
  }
  return r->y - y0;
}

float online_settings_h(float w) {
  FmRect r = { 0, 0, w, 100000 };
  return layout(&r, 0, false);
}

void online_settings(FmRect *r, u32 base) {
  if (ui.now - g_last_draw > 1000) otools(true);   /* the dialog just opened */
  g_last_draw = ui.now;
  layout(r, base, true);
}
