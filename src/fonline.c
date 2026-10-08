/* fonline.c -- the "Online videos" view: search, gallery, details, playing.
**
** Design decisions:
**   - A sibling of the media library (flib_ui.c): a full-window screen the
**     app draws instead of its panels, a source rail on wide screens and a
**     chip strip on narrow ones, one rounded body card. The video player
**     opens over it, so closing the player lands on the same results at
**     the same scroll position.
**   - The view never blocks: searches, resolving a stream and downloads
**     are OnTasks (fonline_task.c) polled in online_pump. A new search
**     abandons the old one (it is reaped when its thread returns) instead
**     of waiting for a slow server.
**   - Idle costs nothing: the only continuous redraw is the skeleton
**     shimmer while a search runs (30 fps through a wake timer, not every
**     vsync), thumbnail fade-ins (220 ms each) and the usual ui_anim
**     transitions. Progress arrives through app_wake from the workers.
**   - The grid touches only the visible rows plus one screen ahead (for
**     the thumbnail prefetch); a card is a handful of shapes and two text
**     lines, so a frame is cheap at any result count.
**   - Errors are sorted into states the user can act on (no key, no
**     yt-dlp, quota, offline, nothing found), each with one clear button,
**     and the adapter's own message underneath.
*/
#include "fonline_int.h"
#include "fview.h"
#include "fview_int.h"
#include "ftitle.h"

enum { E_NONE, E_NOKEY, E_NOYTDLP, E_NOJS, E_QUOTA, E_OFFLINE, E_EMPTY, E_OTHER };
enum { PREP_RUN, PREP_FAIL };
enum { MI_PLAY = 1, MI_DOWNLOAD, MI_BROWSER, MI_COPY, MI_DETAILS };

#define KEY_URL "https://console.cloud.google.com/apis/library/youtube.googleapis.com"

static FmOnlineHooks g_hooks;

static struct {
  bool open;
  bool readonly;
  int src;
  char field[512];             /* the search box */
  char query[512];             /* what the shown results are for */
  FmVsrcItem *items;
  int n, cap;
  char next[256];
  OnTask *search;
  bool search_more;            /* the running search is the next page */
  bool more_err;
  bool searched;               /* results or an error are shown for query */
  int estate;
  char error[256];
  FmScroll scroll;
  float chip_x, chip_x0;
  bool chip_reveal;
  bool focus_search;
  /* overlays */
  bool detail_open;
  FmVsrcItem det;
  bool tray_open;
  FmScroll tray_scroll;
  OnTask *prep;
  bool prep_open;
  int prep_state;
  FmVsrcItem prep_item;
  const FmVsrc *prep_src;
  char prep_err[512];
  float prep_frac;
  char prep_status[160];
  /* abandoned tasks, joined once their threads return */
  OnTask *dead[16];
  int ndead;
  bool settings_focus;
  /* --demo-online */
  char demo_state[24];
  bool demo_done;
} S;

static u32 ID_MENU;
static int g_menu_item = -1;
static FmVsrcItem g_menu_copy;

/* ---- hooks and small helpers ----------------------------------------------------------- */

void online_set_hooks(const FmOnlineHooks *h) {
  if (h) g_hooks = *h;
  else memset(&g_hooks, 0, sizeof g_hooks);
}

void online_conf_dirty(void) { if (g_hooks.conf_dirty) g_hooks.conf_dirty(); }

void online_open_settings(void) {
  S.settings_focus = true;
  otools(true);
  if (g_hooks.open_settings) g_hooks.open_settings();
}

void online_reveal(const char *path) {
  if (g_hooks.reveal) { g_hooks.reveal(path); return; }
  if (!plat_share(path)) {
    char dir[FM_PATH_MAX];
    fm_strlcpy(dir, path, sizeof dir);
    fm_path_parent(dir);
    if (!plat_open_external(dir)) ui_toast("Saved in %s", dir);
  }
}

bool online_settings_focus(void) {
  bool f = S.settings_focus;
  S.settings_focus = false;
  return f;
}

/* plat_open_external treats its argument as a path; URLs go through SDL. */
static void open_url(const char *url) {
  if (!url || !url[0]) { ui_toast("This video has no web page"); return; }
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

static const FmVsrc *cur_src(void) { return vsrc_count() > 0 ? vsrc_at(FM_CLAMP(S.src, 0, vsrc_count() - 1)) : NULL; }

/* The box takes a page link instead of words ("Any site"). */
static bool src_url_only(const FmVsrc *s) { return s && (s->flags & VSRC_URL); }

static void drop_task(OnTask *t) {
  if (!t) return;
  t->cancel = 1;
  if (S.ndead < FM_COUNT(S.dead)) S.dead[S.ndead++] = t;
  else otask_free(t, true);          /* full: wait for this one */
}

static void reap(void) {
  for (int i = S.ndead - 1; i >= 0; i--)
    if (otask_done(S.dead[i])) {
      otask_free(S.dead[i], false);
      S.dead[i] = S.dead[--S.ndead];
    }
}

/* ---- formatting ---------------------------------------------------------------------------- */

void ofmt_views(i64 v, char *out, size_t cap) {
  if (v < 0) { out[0] = 0; return; }
  if (v == 1) { fm_strlcpy(out, "1 view", cap); return; }
  if (v < 1000) { fm_snprintf(out, cap, "%d views", (int)v); return; }
  static const char *const kUnit[] = { "K", "M", "B" };
  double d = (double)v;
  int u = -1;
  while (d >= 1000.0 && u < 2) { d /= 1000.0; u++; }
  if (d < 10 && (int)(d * 10) % 10) fm_snprintf(out, cap, "%.1f%s views", floor(d * 10) / 10, kUnit[u]);
  else fm_snprintf(out, cap, "%d%s views", (int)d, kUnit[u]);
}

static i64 days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  i64 era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void ofmt_age(const char *iso, char *out, size_t cap) {
  out[0] = 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  bool have_time = false;
  if (!iso || !iso[0]) return;
  if (strlen(iso) == 8 && iso[0] >= '0' && iso[0] <= '9') {          /* 20240131 (yt-dlp) */
    if (sscanf(iso, "%4d%2d%2d", &y, &mo, &d) != 3) return;
  } else {
    int n = sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi);
    if (n < 3) return;
    have_time = n >= 5;
  }
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return;
  i64 t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
  i64 s = plat_time_unix() - t;
  if (s < 0) s = 0;
  i64 days = s / 86400;
  struct { i64 n; const char *one, *many; } u;
  if (have_time && s < 3600) { u.n = s / 60; u.one = "minute"; u.many = "minutes"; }
  else if (have_time && s < 86400) { u.n = s / 3600; u.one = "hour"; u.many = "hours"; }
  else if (days < 1) { fm_strlcpy(out, "today", cap); return; }
  else if (days < 7) { u.n = days; u.one = "day"; u.many = "days"; }
  else if (days < 31) { u.n = days / 7; u.one = "week"; u.many = "weeks"; }
  else if (days < 365) { u.n = days / 30; u.one = "month"; u.many = "months"; }
  else { u.n = days / 365; u.one = "year"; u.many = "years"; }
  if (u.n < 1) { fm_strlcpy(out, "just now", cap); return; }
  fm_snprintf(out, cap, "%d %s ago", (int)u.n, u.n == 1 ? u.one : u.many);
}

void ofmt_dur(double sec, char *out, size_t cap) {
  int s = (int)(sec + 0.5);
  if (s >= 3600) fm_snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else fm_snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

static void join_dot(char *out, size_t cap, const char *part) {
  if (!part[0]) return;
  if (out[0]) fm_strlcat(out, "  \xC2\xB7  ", cap);
  fm_strlcat(out, part, cap);
}

void ofmt_meta(const FmVsrcItem *it, char *out, size_t cap) {
  char v[48], a[48];
  out[0] = 0;
  ofmt_views(it->views, v, sizeof v);
  ofmt_age(it->published, a, sizeof a);
  join_dot(out, cap, it->channel);
  if (it->live) join_dot(out, cap, it->views > 0 ? "watching now" : "Live now");
  else join_dot(out, cap, v);
  join_dot(out, cap, a);
}

/* Text in up to `lines` lines, breaking at spaces; the last line ends with
** an ellipsis. Returns the lines used. */
static int text_lines(int face, float size, float x, float y, float w, const char *s, int lines, FmColor c,
                      bool draw) {
  float lh = font_line_h(size);
  int used = 0;
  while (*s && used < lines) {
    while (*s == ' ') s++;
    if (!*s) break;
    float ly = y + (float)used * lh;
    int len = (int)strlen(s);
    if (used == lines - 1) {
      if (draw) font_draw_ellipsis(face, size, x, ly, s, w, c);
      used++;
      break;
    }
    int fit = font_fit(face, size, s, len, w);
    if (fit >= len) {
      if (draw) font_draw(face, size, x, ly, s, len, c);
      used++;
      break;
    }
    int brk = fit;
    while (brk > 0 && s[brk] != ' ') brk--;
    if (brk == 0) {
      u32 cp;
      brk = fit > 0 ? fit : utf8_decode(s, &cp);
      if (brk <= 0) brk = 1;
    }
    if (draw) font_draw(face, size, x, ly, s, brk, c);
    s += brk;
    used++;
  }
  return used;
}

/* Channel initial on a soft colour picked from its name. */
static void avatar(FmRect b, const char *name) {
  u32 h = 2166136261u;
  for (const char *p = name; *p; p++) { h ^= (u8)*p; h *= 16777619u; }
  FmColor base = kAccents[h % UI_ACCENTS];
  float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
  gfx_circle(cx, cy, b.w * 0.5f, col_mix(T.surface2, base, T.dark ? 0.35f : 0.22f));
  u32 cp = '?';
  if (name[0]) utf8_decode(name, &cp);
  if (cp >= 'a' && cp <= 'z') cp -= 32;
  char s[8];
  int n = utf8_encode(cp, s);
  s[n] = 0;
  font_draw_center(FONT_BOLD, b.h * 0.42f, b, s, col_mix(base, T.text, T.dark ? 0.15f : 0.35f));
}

/* ---- searching -------------------------------------------------------------------------------- */

static void clear_results(void) {
  fm_free(S.items);
  S.items = NULL;
  S.n = S.cap = 0;
  S.next[0] = 0;
  S.more_err = false;
  S.searched = false;
  S.estate = E_NONE;
  S.error[0] = 0;
  memset(&S.scroll, 0, sizeof S.scroll);
  othumb_forget_queue();
}

static int classify(const char *e, FmErr err) {
  if (err == FM_ERR_UNSUPPORTED && fm_stristr(e, "network")) return E_OFFLINE;
  if (fm_stristr(e, "quota") || fm_stristr(e, "rate limit") || fm_stristr(e, "too many requests")) return E_QUOTA;
  if (fm_stristr(e, "api key") || fm_stristr(e, "apikey") || fm_stristr(e, "key not valid") ||
      fm_stristr(e, "keyInvalid") || fm_stristr(e, "needs a key"))
    return E_NOKEY;
  if (fm_stristr(e, "yt-dlp") && !otools(false)->ytdlp) return E_NOYTDLP;
  if (fm_stristr(e, "javascript runtime")) return E_NOJS;
  if (fm_stristr(e, "no videos found") || fm_stristr(e, "no video found")) return E_EMPTY;
  if (fm_stristr(e, "offline") || fm_stristr(e, "internet") || fm_stristr(e, "connect") ||
      fm_stristr(e, "resolve host") || fm_stristr(e, "name not resolved") || fm_stristr(e, "timed out") ||
      fm_stristr(e, "timeout") || fm_stristr(e, "unreachable") || fm_stristr(e, "network"))
    return E_OFFLINE;
  return E_OTHER;
}

/* What stops a search before it starts. */
static int precheck(const FmVsrc *s) {
  const OnTools *tl = otools(false);
  if ((s->flags & VSRC_NEEDKEY) && !conf.yt_api_key[0] && !tl->ytdlp) return E_NOKEY;
  if (src_url_only(s) && (s->flags & VSRC_YTDLP) && !tl->ytdlp) return E_NOYTDLP;
  return E_NONE;
}

static void search_start(bool more) {
  const FmVsrc *s = cur_src();
  if (!s || !s->search || !S.query[0]) return;
  if (S.search) { drop_task(S.search); S.search = NULL; }
  if (!more) clear_results();
  int pre = precheck(s);
  if (pre != E_NONE) {
    S.searched = true;
    S.estate = pre;
    ui_redraw();
    return;
  }
  OnTask *t = otask_new(OT_SEARCH, s);
  fm_strlcpy(t->query, S.query, sizeof t->query);
  if (more) fm_strlcpy(t->token, S.next, sizeof t->token);
  S.search = t;
  S.search_more = more;
  S.more_err = false;
  otask_run(t);
  ui_redraw();
}

static void submit(const char *q) {
  char buf[512];
  fm_strlcpy(buf, q, sizeof buf);
  char *b = buf;
  while (*b == ' ') b++;
  size_t n = strlen(b);
  while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
  if (!b[0]) return;
  fm_strlcpy(S.query, b, sizeof S.query);
  if (S.field != b) fm_strlcpy(S.field, b, sizeof S.field);
  if (!src_url_only(cur_src())) orecent_add(b);
  ui_focus(0);
  search_start(false);
}

static void open_detail(const FmVsrcItem *it) {
  S.det = *it;
  S.detail_open = true;
  ui_redraw();
}

static void take_search(void) {
  OnTask *t = S.search;
  if (!t || !otask_done(t)) return;
  S.search = NULL;
  if (t->err == FM_OK) {
    int add = t->page.count;
    if (S.n + add > S.cap) {
      S.cap = FM_MAX(S.cap * 2, S.n + add + 16);
      S.items = (FmVsrcItem *)fm_realloc(S.items, sizeof *S.items * (size_t)S.cap);
    }
    /* the next page may repeat items of this one: skip ids already shown */
    for (int i = 0; i < add; i++) {
      const FmVsrcItem *it = &t->page.items[i];
      bool dup = false;
      for (int k = S.search_more ? 0 : S.n; k < S.n && !dup; k++) dup = it->id[0] && strcmp(S.items[k].id, it->id) == 0;
      if (!dup) S.items[S.n++] = *it;
    }
    fm_strlcpy(S.next, t->page.next, sizeof S.next);
    S.searched = true;
    S.estate = S.n == 0 ? E_EMPTY : E_NONE;
    /* a pasted link gives one video: show it straight away */
    if (!S.search_more && S.n == 1 && src_url_only(cur_src())) open_detail(&S.items[0]);
  } else if (t->err != FM_ERR_CANCEL) {
    if (S.search_more) {
      S.more_err = true;
      fm_strlcpy(S.error, t->page.error, sizeof S.error);
    } else {
      S.searched = true;
      fm_strlcpy(S.error, t->page.error[0] ? t->page.error : t->errtext, sizeof S.error);
      S.estate = classify(S.error, t->err);
    }
  }
  otask_free(t, false);
  ui_redraw();
}

/* ---- playing ------------------------------------------------------------------------------------ */

static void prep_fail(const char *msg) {
  S.prep_state = PREP_FAIL;
  fm_strlcpy(S.prep_err, msg, sizeof S.prep_err);
  ui_redraw();
}

static void play_item(const FmVsrc *s, const FmVsrcItem *it) {
  if (!s) return;
  if (S.prep) { drop_task(S.prep); S.prep = NULL; }
  S.detail_open = false;
  S.prep_open = true;
  S.prep_state = PREP_RUN;
  S.prep_item = *it;
  S.prep_src = s;
  S.prep_err[0] = 0;
  S.prep_frac = -1;
  fm_strlcpy(S.prep_status, "Opening\xE2\x80\xA6", sizeof S.prep_status);
  if ((s->flags & VSRC_YTDLP) && !otools(false)->ytdlp) {
    prep_fail("Playing from this site needs yt-dlp, a free helper program. Get it with one click, or watch "
              "the video in your browser.");
    return;
  }
  OnTask *t = otask_new(OT_RESOLVE, s);
  t->item = *it;
  S.prep = t;
  if (!otask_run(t)) { prep_fail(t->errtext); S.prep = NULL; otask_free(t, false); }
  ui_redraw();
}

static void take_prep(void) {
  OnTask *t = S.prep;
  if (!t) return;
  otask_progress(t, &S.prep_frac, S.prep_status, sizeof S.prep_status);
  if (!otask_done(t)) return;
  S.prep = NULL;
  if (t->err == FM_OK) {
    /* streams and cache files alike: the player gets the quality list and
    ** handles expired links and the cache fallback itself */
    if (video_open_online(S.prep_src, &t->conf, &S.prep_item, &t->stream, t->t0)) {
      S.prep_open = false;
    } else {
      prep_fail("The player could not open the stream.");
    }
  } else if (t->err == FM_ERR_CANCEL) {
    S.prep_open = false;
  } else {
    prep_fail(t->errtext[0] ? t->errtext : fm_err_str(t->err));
  }
  otask_free(t, false);
  ui_redraw();
}

static void download_item(const FmVsrc *s, const FmVsrcItem *it) {
  if (!s) return;
  if ((s->flags & VSRC_YTDLP) && !otools(false)->ytdlp && !s->download) {
    ui_toast("Downloads from %s need yt-dlp (Settings)", s->name);
    return;
  }
  odl_start(s, it);
}

/* ---- the card menu ------------------------------------------------------------------------------ */

static void open_menu(int i, float x, float y) {
  g_menu_item = i;
  g_menu_copy = S.items[i];
  FmMenuItem m[6];
  int n = 0;
  memset(m, 0, sizeof m);
  m[n].id = MI_PLAY; m[n].icon = IC_PLAY; m[n++].label = "Play";
  m[n].id = MI_DETAILS; m[n].icon = IC_INFO; m[n++].label = "Details";
  m[n].id = MI_DOWNLOAD; m[n].icon = IC_DOWNLOAD; m[n++].label = "Download";
  m[n++].flags = UI_MI_SEP;
  m[n].id = MI_BROWSER; m[n].icon = IC_OPEN_WITH; m[n++].label = "Open in browser";
  m[n].id = MI_COPY; m[n].icon = IC_LINK; m[n++].label = "Copy link";
  ui_menu_open(ID_MENU, x, y, m, n);
}

static void menu_results(void) {
  int r = ui_menu_result(ID_MENU);
  if (r < 0 || g_menu_item < 0) return;
  const FmVsrcItem *it = &g_menu_copy;
  switch (r) {
    case MI_PLAY: play_item(cur_src(), it); break;
    case MI_DETAILS: open_detail(it); break;
    case MI_DOWNLOAD: download_item(cur_src(), it); break;
    case MI_BROWSER: open_url(it->page); break;
    case MI_COPY: ui_clipboard_set(it->page); ui_toast("Link copied"); break;
    default: break;
  }
  g_menu_item = -1;
}

/* ---- thumbnails and badges ---------------------------------------------------------------------- */

#define WHITE FM_RGBA(255, 255, 255, 255)

static void thumb_box(FmRect r, const FmVsrcItem *it, float rad) {
  float fade = 1;
  /* whole pixels: crisp pictures, and texture coordinates that stay inside
  ** 0..1 (SDL drops a whole batch for a uv of -0.0000001) */
  float x1 = floorf(r.x + r.w + 0.5f), y1 = floorf(r.y + r.h + 0.5f);
  r.x = floorf(r.x + 0.5f);
  r.y = floorf(r.y + 0.5f);
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  SDL_Texture *tex = it->thumb[0] ? othumb_get(it->thumb, (int)r.w, &fade) : NULL;
  if (!tex || fade < 1) {
    gfx_rrect(r, rad, col_mix(T.surface2, T.text, T.dark ? 0.06f : 0.05f));
    float is = FM_MIN(r.w, r.h) * 0.28f;
    icon_draw(IC_PLAY_BADGE, rect_center(r, is, is), col_alpha(T.text3, 0.7f));
  }
  if (tex) {
    gfx_tex_rounded(tex, r, rad, col_alpha(WHITE, fade));
    if (fade < 1) ui_animate();
  }
}

static void badge(FmRect thumb, const FmVsrcItem *it, float scale) {
  float fs = ui.m.font_small * 0.92f * scale;
  float h = font_line_h(fs) + DP(4) * scale, m = DP(8) * scale;
  if (it->live) {
    const char *l = "LIVE";
    float w = font_width(FONT_BOLD, fs, l, -1) + DP(22) * scale;
    FmRect b = { thumb.x + thumb.w - w - m, thumb.y + thumb.h - h - m, w, h };
    gfx_rrect(b, DP(5) * scale, T.danger);
    gfx_circle(b.x + DP(8) * scale, b.y + h * 0.5f, DP(2.6f) * scale, WHITE);
    font_draw(FONT_BOLD, fs, b.x + DP(14) * scale, b.y + (h - font_line_h(fs)) * 0.5f, l, -1, WHITE);
  } else if (it->duration > 0) {
    char d[24];
    ofmt_dur(it->duration, d, sizeof d);
    float w = font_width(FONT_BOLD, fs, d, -1) + DP(12) * scale;
    FmRect b = { thumb.x + thumb.w - w - m, thumb.y + thumb.h - h - m, w, h };
    gfx_rrect(b, DP(5) * scale, FM_RGBA(0, 0, 0, 200));
    font_draw_center(FONT_BOLD, fs, b, d, WHITE);
  }
}

/* The round play button in the middle of a thumbnail; returns true when clicked. */
static bool play_disc(u32 id, FmRect thumb, float alpha, float size) {
  float s = size;
  FmRect b = { thumb.x + (thumb.w - s) * 0.5f, thumb.y + (thumb.h - s) * 0.5f, s, s };
  int f = ui_hit(id, b);
  float hv = ui_anim(id, (f & UI_HOVER) ? 1.0f : 0.0f, 16.0f);
  float cx = b.x + s * 0.5f, cy = b.y + s * 0.5f;
  gfx_circle(cx, cy, s * 0.5f, col_alpha(col_mix(FM_RGBA(0, 0, 0, 255), T.accent, hv), (0.58f + 0.4f * hv) * alpha));
  gfx_ring(cx, cy, s * 0.5f - DP(0.75f), DP(1.5f), col_alpha(WHITE, 0.55f * alpha * (1 - hv)));
  float is = s * 0.44f;
  icon_draw(IC_PLAY, FM_RECT(cx - is * 0.44f, cy - is * 0.5f, is, is),
            col_alpha(hv > 0.5f ? T.on_accent : WHITE, alpha));
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

/* ---- grid --------------------------------------------------------------------------------------- */

typedef struct Grid {
  int cols;
  float pad, gap, rgap;
  float cw, th, ch;            /* card width, thumbnail height, card height */
  float text_x, title_lh;
  bool avatar;
} Grid;

static Grid grid_layout(FmRect view) {
  Grid g;
  g.pad = DP(view.w < DP(520) ? 12 : 18);
  g.gap = DP(16);
  g.rgap = DP(ui.touch_mode ? 10 : 6);
  float avail = view.w - g.pad * 2;
  float want = DP(ui.touch_mode ? 270 : 250);
  if (ui.portrait && avail < DP(620)) g.cols = 1;
  else g.cols = FM_CLAMP((int)((avail + g.gap) / (want + g.gap)), 1, 6);
  g.cw = (avail - g.gap * (float)(g.cols - 1)) / (float)g.cols;
  g.th = floorf(g.cw * 9.0f / 16.0f);
  g.avatar = g.cw >= DP(200);
  g.title_lh = font_line_h(ui.m.font);
  g.text_x = g.avatar ? DP(36) + DP(12) : 0;
  g.ch = g.th + DP(12) + g.title_lh * 2 + DP(2) + font_line_h(ui.m.font_small) + DP(14);
  return g;
}

static float wave(int k) {
  return 0.5f + 0.5f * sinf((float)(ui.now % 100000) / 1600.0f * 6.2831853f - (float)k * 0.45f);
}

static void skeleton_card(FmRect c, const Grid *g, int k) {
  float w = wave(k);
  FmColor a = col_mix(T.surface2, T.text, (T.dark ? 0.05f : 0.04f) + 0.04f * w);
  FmColor b = col_mix(T.surface2, T.text, (T.dark ? 0.04f : 0.03f) + 0.03f * w);
  gfx_rrect(FM_RECT(c.x, c.y, c.w, g->th), DP(12), a);
  float y = c.y + g->th + DP(12);
  float x = c.x + g->text_x;
  if (g->avatar) gfx_circle(c.x + DP(18), y + DP(18), DP(18), b);
  float bh = DP(11), tw = c.w - g->text_x - DP(8);
  gfx_rrect(FM_RECT(x, y + DP(4), tw * 0.92f, bh), bh * 0.5f, b);
  gfx_rrect(FM_RECT(x, y + DP(4) + g->title_lh, tw * 0.6f, bh), bh * 0.5f, b);
  gfx_rrect(FM_RECT(x, y + g->title_lh * 2 + DP(6), tw * 0.42f, DP(9)), DP(4.5f), b);
}

static void card(FmRect c, u32 id, int i, const Grid *g) {
  const FmVsrcItem *it = &S.items[i];
  FmRect hit = rect_inset(c, -DP(6));
  int f = ui_hit(id, hit);
  float hv = ui.touch_mode ? 0 : ui_anim(ui_idn(id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 14.0f);
  if (hv > 0.01f) gfx_rrect(hit, DP(16), col_alpha(T.hover, hv));
  if (f & UI_HELD) gfx_rrect(hit, DP(16), T.press);
  FmRect thumb = { c.x, c.y, c.w, g->th };
  float rad = DP(12);
  thumb_box(thumb, it, rad);
  badge(thumb, it, 1);
  bool clicked_play = false;
  float pa = ui.touch_mode ? 1.0f : hv;
  if (pa > 0.01f) clicked_play = play_disc(ui_idn(id, 2), thumb, pa, DP(ui.touch_mode ? 52 : 56));
  /* text */
  float y = thumb.y + thumb.h + DP(12);
  if (g->avatar) avatar(FM_RECT(c.x, y, DP(36), DP(36)), it->channel);
  float x = c.x + g->text_x, w = c.w - g->text_x - DP(4);
  int lines = text_lines(FONT_BOLD, ui.m.font, x, y, w, it->title[0] ? it->title : "Untitled", 2, T.text, true);
  char meta[400];
  ofmt_meta(it, meta, sizeof meta);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + g->title_lh * (float)lines + DP(2), meta, w, T.text2);
  if (clicked_play) { play_item(cur_src(), it); return; }
  if (f & UI_CLICK) open_detail(it);
  else if ((f & UI_RCLICK) || (f & UI_LONG)) open_menu(i, ui.mx, ui.my);
}

static void draw_grid(FmRect view, bool loading_first) {
  Grid g = grid_layout(view);
  u32 sid = ui_id("on.grid");
  bool more = S.search && S.search_more;
  int skel = 0;
  if (loading_first) skel = g.cols * ((int)(view.h / (g.ch + g.rgap)) + 1);
  else if (more) skel = g.cols * (g.cols == 1 ? 2 : 1);
  int total = S.n + skel;
  int rows = (total + g.cols - 1) / g.cols;
  float footer = (!loading_first && S.n > 0 && !more) ? DP(64) : DP(12);
  float content = g.pad + (float)rows * (g.ch + g.rgap) + footer;
  if (loading_first) content = view.h;          /* placeholders do not scroll */
  ui_scroll(&S.scroll, sid, view, content);
  float sy = S.scroll.y;
  float rh = g.ch + g.rgap;
  int r0 = FM_MAX(0, (int)((sy - g.pad) / rh));
  int r1 = FM_MIN(rows - 1, (int)((sy + view.h - g.pad) / rh));
  gfx_clip_push(view);
  for (int r = r0; r <= r1; r++)
    for (int k = 0; k < g.cols; k++) {
      int i = r * g.cols + k;
      if (i >= total) break;
      FmRect c = { view.x + g.pad + (float)k * (g.cw + g.gap), view.y + g.pad + (float)r * rh - sy, g.cw, g.ch };
      if (i < S.n) card(c, ui_idn(sid, (u32)i + 1), i, &g);
      else skeleton_card(c, &g, i - S.n + r);
    }
  /* footer: end of results, or a retry for a failed page */
  if (!loading_first && S.n > 0 && !more) {
    float fy = view.y + g.pad + (float)rows * rh - sy;
    FmRect fr = { view.x, fy, view.w, footer };
    if (gfx_visible(fr)) {
      if (S.more_err) {
        const char *l = "Couldn't load more. Try again";
        float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(56);
        if (ui_button(ui_id("on.more.retry"), rect_center(fr, bw, DP(ui.touch_mode ? 44 : 36)), IC_REFRESH, l,
                      UI_BTN_TONAL))
          search_start(true);
      } else if (!S.next[0]) {
        char t[64];
        fm_snprintf(t, sizeof t, "%d %s", S.n, S.n == 1 ? "video" : "videos");
        font_draw_center(FONT_REGULAR, ui.m.font_small, fr, t, T.text3);
      }
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&S.scroll, view, content);
  /* thumbnails for the next screen, so they are there when it scrolls in */
  int p0 = (r1 + 1) * g.cols, p1 = FM_MIN(S.n, (r1 + 1 + (int)(view.h / rh) + 1) * g.cols);
  for (int i = p0; i < p1; i++)
    if (S.items[i].thumb[0]) othumb_prefetch(S.items[i].thumb, (int)g.cw);
  /* infinite scroll: the next page once the end is a screen and a half away */
  if (!S.search && !S.more_err && S.next[0] && S.n > 0 && sy + view.h * 2.5f >= content) search_start(true);
  if (skel > 0) view_wake_in(33);          /* gentle shimmer at 30 fps, only while loading */
}

/* ---- empty, error and first-run states ------------------------------------------------------------ */

typedef struct Action { const char *label; FmIcon icon; int style; int code; } Action;
enum { ACT_SETTINGS = 1, ACT_KEY_HELP, ACT_YTDLP, ACT_RETRY, ACT_CLEAR, ACT_BROWSER, ACT_DENO };

static int install_line(FmRect r) {
  float frac;
  char st[160];
  if (!oinst_running(&frac, st, sizeof st)) return 0;
  FmRect pr = { r.x, r.y + DP(6), r.w, DP(6) };
  ui_progress(pr, frac);
  ui_label(FM_RECT(r.x, r.y + DP(16), r.w, font_line_h(ui.m.font_small)), st, FONT_REGULAR, ui.m.font_small, T.text2,
           UI_CENTER);
  return 1;
}

static void run_action(int code) {
  switch (code) {
    case ACT_SETTINGS: online_open_settings(); break;
    case ACT_KEY_HELP: open_url(KEY_URL); break;
    case ACT_YTDLP: oinst_start(); break;
    case ACT_RETRY: otools(true); search_start(false); break;
    case ACT_CLEAR: S.field[0] = 0; S.query[0] = 0; clear_results(); S.focus_search = true; break;
    case ACT_DENO: open_url("https://deno.com"); break;
    default: break;
  }
  ui_redraw();
}

/* Centred icon, title, text, the adapter's message and up to three buttons. */
static void state_view(FmRect body, FmIcon ic, FmColor icol, const char *title, const char *sub, const char *detail,
                       const Action *acts, int nact) {
  float is = DP(56), bh = DP(ui.touch_mode ? 44 : 38);
  float tw = FM_MIN(body.w - DP(48), DP(520));
  float tl = font_line_h(ui.m.font_title);
  float sh = sub ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, tw, sub, T.text2, false) : 0;
  float dh = detail && detail[0] ? font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, detail, T.text3, false) : 0;
  bool inst = oinst_running(NULL, NULL, 0);
  float h = is + DP(18) + tl + (sub ? sh + DP(6) : 0) + (dh > 0 ? dh + DP(8) : 0) + (nact ? bh + DP(22) : 0) +
            (inst ? DP(40) : 0);
  float y = body.y + FM_MAX(DP(16), (body.h - h) * 0.42f);
  float cx = body.x + body.w * 0.5f;
  gfx_circle(cx, y + is * 0.5f, is * 0.66f, col_alpha(icol, 0.12f));
  icon_draw(ic, FM_RECT(cx - is * 0.3f, y + is * 0.2f, is * 0.6f, is * 0.6f), icol);
  y += is + DP(18);
  ui_label(FM_RECT(body.x + DP(12), y, body.w - DP(24), tl), title, FONT_BOLD, ui.m.font_title, T.text, UI_CENTER);
  y += tl;
  if (sub) {
    y += DP(6);
    float w = FM_MIN(tw, font_width(FONT_REGULAR, ui.m.font, sub, -1) + DP(2));
    font_draw_wrap(FONT_REGULAR, ui.m.font, cx - w * 0.5f, y, w, sub, T.text2, true);
    y += sh;
  }
  if (dh > 0) {
    y += DP(8);
    float w = FM_MIN(tw, font_width(FONT_REGULAR, ui.m.font_small, detail, -1) + DP(2));
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, cx - w * 0.5f, y, w, detail, T.text3, true);
    y += dh;
  }
  if (nact) {
    y += DP(22);
    float bw[4], total = 0;
    for (int i = 0; i < nact; i++) {
      bw[i] = font_width(FONT_BOLD, ui.m.font, acts[i].label, -1) + (acts[i].icon ? DP(56) : DP(36));
      total += bw[i] + (i ? DP(10) : 0);
    }
    bool stack = total > body.w - DP(32);
    float x = cx - total * 0.5f;
    for (int i = 0; i < nact; i++) {
      FmRect b = stack ? FM_RECT(cx - FM_MIN(tw, DP(320)) * 0.5f, y + (float)i * (bh + DP(8)), FM_MIN(tw, DP(320)), bh)
                       : FM_RECT(x, y, bw[i], bh);
      bool busy = acts[i].code == ACT_YTDLP && inst;
      if (ui_button(ui_idn(ui_id("on.state.btn"), (u32)i), b, acts[i].icon, busy ? "Getting yt-dlp\xE2\x80\xA6" : acts[i].label,
                    acts[i].style) && !busy)
        run_action(acts[i].code);
      x += bw[i] + DP(10);
    }
    y += stack ? (float)nact * (bh + DP(8)) : bh;
  }
  if (inst) install_line(FM_RECT(cx - DP(140), y + DP(8), DP(280), DP(32)));
}

static void error_state(FmRect body, const FmVsrc *s) {
  char sub[400], title[200];
  Action a[3];
  int n = 0;
  const OnTools *tl = otools(false);
  switch (S.estate) {
    case E_NOKEY:
      fm_snprintf(title, sizeof title, "Add your %s API key in Settings", s->name);
      fm_strlcpy(sub, "The YouTube Data API is free: 10,000 units a day, about 100 searches. The key takes a "
                      "minute to create.", sizeof sub);
      if (!tl->ytdlp) fm_strlcat(sub, " Or get yt-dlp to search without a key.", sizeof sub);
      a[n++] = (Action){ "Open Settings", IC_SETTINGS, UI_BTN_FILLED, ACT_SETTINGS };
      a[n++] = (Action){ "How to get a key", IC_OPEN_WITH, UI_BTN_TEXT, ACT_KEY_HELP };
      if (!tl->ytdlp) a[n++] = (Action){ "Get yt-dlp", IC_DOWNLOAD, UI_BTN_TONAL, ACT_YTDLP };
      state_view(body, IC_KEY, T.accent, title, sub, S.error, a, n);
      break;
    case E_NOYTDLP:
      fm_snprintf(sub, sizeof sub, "%s works through yt-dlp, a free helper that finds and fetches the video. "
                                   "It is about 18 MB and is kept next to the settings.", s->name);
      a[n++] = (Action){ "Get yt-dlp", IC_DOWNLOAD, UI_BTN_FILLED, ACT_YTDLP };
      a[n++] = (Action){ "Settings", IC_SETTINGS, UI_BTN_TEXT, ACT_SETTINGS };
      state_view(body, IC_DOWNLOAD, T.accent, "yt-dlp is needed", sub, NULL, a, n);
      break;
    case E_NOJS:
      a[n++] = (Action){ "Get Deno", IC_OPEN_WITH, UI_BTN_FILLED, ACT_DENO };
      a[n++] = (Action){ "Try again", IC_REFRESH, UI_BTN_TEXT, ACT_RETRY };
      state_view(body, IC_CODE, T.accent, "A JavaScript runtime is needed",
                 "YouTube only gives yt-dlp its videos with Deno or Node.js installed. Install Deno, then try "
                 "again (Settings shows what was found).", S.error, a, n);
      break;
    case E_QUOTA:
      a[n++] = (Action){ "Try again", IC_REFRESH, UI_BTN_TONAL, ACT_RETRY };
      state_view(body, IC_WARN, T.warn, "Today's quota is used up",
                 "The free YouTube quota (10,000 units, about 100 searches a day) resets at midnight Pacific "
                 "time. Other sources keep working meanwhile.", S.error, a, n);
      break;
    case E_OFFLINE:
      a[n++] = (Action){ "Try again", IC_REFRESH, UI_BTN_FILLED, ACT_RETRY };
      state_view(body, IC_NETWORK, T.text2, "You're offline", "Check the internet connection, then try again.",
                 S.error, a, n);
      break;
    case E_EMPTY:
      fm_snprintf(title, sizeof title, "No results for \xE2\x80\x9C%s\xE2\x80\x9D", S.query);
      a[n++] = (Action){ "New search", IC_SEARCH, UI_BTN_TONAL, ACT_CLEAR };
      state_view(body, IC_SEARCH, T.accent, title,
                 src_url_only(s) ? "No video was found at that link." : "Try other words, or another source.", NULL, a,
                 n);
      break;
    default:
      a[n++] = (Action){ "Try again", IC_REFRESH, UI_BTN_FILLED, ACT_RETRY };
      state_view(body, IC_WARN, T.danger, "Something went wrong", "The search did not work this time.", S.error, a, n);
      break;
  }
}

/* A small card with an icon, a title, a line of text and buttons (onboarding). */
static float info_card(FmRect r, u32 id, FmIcon ic, const char *title, const char *text, const Action *a, int n,
                       bool draw) {
  float pad = DP(16), is = DP(36);
  float tx = pad + is + DP(14), tw = r.w - tx - pad;
  float tl = font_line_h(ui.m.font);
  float th = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, text, T.text2, false);
  float bh = DP(ui.touch_mode ? 40 : 34);
  bool inst = oinst_running(NULL, NULL, 0);
  /* buttons flow onto another row when the card is narrow */
  float bw[4], bx = 0;
  int brows = n ? 1 : 0;
  n = FM_MIN(n, 4);
  for (int i = 0; i < n; i++) {
    bool busy = a[i].code == ACT_YTDLP && inst;
    const char *l = busy ? "Getting yt-dlp\xE2\x80\xA6" : a[i].label;
    bw[i] = FM_MIN(tw, font_width(FONT_BOLD, ui.m.font, l, -1) + (a[i].icon ? DP(52) : DP(32)));
    if (i > 0 && bx + bw[i] > tw) { brows++; bx = 0; }
    bx += bw[i] + DP(8);
  }
  float h = pad + tl + DP(4) + th + (n ? DP(12) + (float)brows * bh + (float)(brows - 1) * DP(8) : 0) +
            (inst ? DP(34) : 0) + pad;
  if (!draw) return h;
  FmRect c = { r.x, r.y, r.w, h };
  gfx_rrect(c, DP(14), T.surface2);
  gfx_rrect_line(c, DP(14), DP(1), T.border);
  FmRect ib = { c.x + pad, c.y + pad, is, is };
  gfx_rrect(ib, DP(10), T.accent_soft);
  icon_draw(ic, rect_inset(ib, DP(8)), T.accent);
  float x = c.x + tx, y = c.y + pad;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, title, tw, T.text);
  y += tl + DP(4);
  font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, y, tw, text, T.text2, true);
  y += th + DP(12);
  for (int i = 0; i < n; i++) {
    bool busy = a[i].code == ACT_YTDLP && inst;
    const char *l = busy ? "Getting yt-dlp\xE2\x80\xA6" : a[i].label;
    if (i > 0 && x + bw[i] > c.x + tx + tw) { x = c.x + tx; y += bh + DP(8); }
    if (ui_button(ui_idn(id, (u32)i), FM_RECT(x, y, bw[i], bh), a[i].icon, l, a[i].style) && !busy)
      run_action(a[i].code);
    x += bw[i] + DP(8);
  }
  if (inst) install_line(FM_RECT(c.x + tx, y + bh + DP(2), tw, DP(30)));
  return h;
}

static const char *const kTry[] = { "Nature documentary", "Lo-fi music", "Space", "Cooking", "Travel" };

static bool chip(u32 id, FmRect c, FmIcon ic, const char *label, bool on) {
  int f = ui_hit(id, c);
  float rad = c.h * 0.5f;
  if (on) gfx_rrect(c, rad, T.accent);
  else {
    gfx_rrect(c, rad, T.surface);
    gfx_rrect_line(c, rad, DP(1), T.border);
    if (f & UI_HOVER) gfx_rrect(c, rad, T.hover);
  }
  if (f & UI_HELD) gfx_rrect(c, rad, T.press);
  float is = DP(15), x = c.x + DP(12);
  FmColor fg = on ? T.on_accent : T.text;
  if (ic) {
    icon_draw(ic, FM_RECT(x, c.y + (c.h - is) * 0.5f, is, is), on ? fg : T.text2);
    x += is + DP(6);
  }
  float fs = ui.m.font_small * 1.06f;
  font_draw_ellipsis(on ? FONT_BOLD : FONT_REGULAR, fs, x, c.y + (c.h - font_line_h(fs)) * 0.5f, label,
                     c.x + c.w - DP(12) - x, fg);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  return (f & UI_CLICK) != 0;
}

static float chip_w(FmIcon ic, const char *label, float max) {
  float fs = ui.m.font_small * 1.06f;
  return FM_MIN(max, font_width(FONT_REGULAR, fs, label, -1) + DP(28) + (ic ? DP(21) : 0));
}

static void onboarding(FmRect body, const FmVsrc *s) {
  const OnTools *tl = otools(false);
  bool nokey = (s->flags & VSRC_NEEDKEY) && !conf.yt_api_key[0];   /* YouTube searches without one */
  bool noyt = (s->flags & VSRC_YTDLP) && !tl->ytdlp;
  float cw = FM_MIN(body.w - DP(32), DP(560));
  float x = body.x + (body.w - cw) * 0.5f;
  /* measure, to centre the block */
  char ktext[300], ytext[300];
  fm_strlcpy(ktext, "The YouTube Data API key is free: 10,000 units a day, about 100 searches.", sizeof ktext);
  fm_strlcat(ktext, tl->ytdlp ? " Until then, search runs through yt-dlp (slower)." : " Without a key, search needs yt-dlp.",
             sizeof ktext);
  fm_snprintf(ytext, sizeof ytext, "Playing and downloading from %s works through yt-dlp, a free helper "
                                   "(about 18 MB). One click gets it.", s->name);
  Action ka[2] = { { "Open Settings", IC_SETTINGS, UI_BTN_FILLED, ACT_SETTINGS },
                   { "How to get a key", IC_OPEN_WITH, UI_BTN_TEXT, ACT_KEY_HELP } };
  Action ya[1] = { { "Get yt-dlp", IC_DOWNLOAD, UI_BTN_FILLED, ACT_YTDLP } };
  float is = DP(64), tlh = font_line_h(ui.m.font_title * 1.2f);
  float ab = s->about ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw, s->about, T.text2, false) : 0;
  bool tries = !src_url_only(s) && orecent_count() == 0;
  float h = is + DP(18) + tlh + DP(6) + ab + (tries ? DP(56) : 0);
  if (nokey) h += DP(20) + info_card(FM_RECT(x, 0, cw, 0), 0, IC_KEY, "", ktext, ka, 2, false);
  if (noyt) h += DP(14) + info_card(FM_RECT(x, 0, cw, 0), 0, IC_DOWNLOAD, "", ytext, ya, 1, false);
  float y = body.y + FM_MAX(DP(16), (body.h - h) * 0.4f);
  float cx = body.x + body.w * 0.5f;
  FmRect tile = { cx - is * 0.5f, y, is, is };
  gfx_shadow(tile, DP(18), DP(14), col_alpha(T.accent, 0.35f));
  gfx_rrect_vgrad(tile, DP(18), col_mix(T.accent, WHITE, 0.18f), T.accent);
  icon_draw(s->icon ? s->icon : IC_PLAY_BADGE, rect_inset(tile, DP(15)), T.on_accent);
  y += is + DP(18);
  char title[96];
  fm_snprintf(title, sizeof title, src_url_only(s) ? "Play a video from a link" : "Search %s", s->name);
  ui_label(FM_RECT(body.x + DP(8), y, body.w - DP(16), tlh), title, FONT_BOLD, ui.m.font_title * 1.2f, T.text, UI_CENTER);
  y += tlh + DP(6);
  if (s->about) {
    float w = FM_MIN(cw, font_width(FONT_REGULAR, ui.m.font, s->about, -1) + DP(2));
    font_draw_wrap(FONT_REGULAR, ui.m.font, cx - w * 0.5f, y, w, s->about, T.text2, true);
    y += ab;
  }
  if (tries) {
    y += DP(20);
    float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8), total = 0;
    int n = 0;
    float ws[FM_COUNT(kTry)];
    for (int i = 0; i < FM_COUNT(kTry); i++) {
      ws[i] = chip_w(IC_SEARCH, kTry[i], DP(200));
      if (total + ws[i] > body.w - DP(32)) break;
      total += ws[i] + (n ? gap : 0);
      n++;
    }
    float xx = cx - total * 0.5f;
    for (int i = 0; i < n; i++) {
      if (chip(ui_idn(ui_id("on.try"), (u32)i), FM_RECT(xx, y, ws[i], ch), IC_SEARCH, kTry[i], false)) {
        fm_strlcpy(S.field, kTry[i], sizeof S.field);
        submit(kTry[i]);
      }
      xx += ws[i] + gap;
    }
    y += ch;
  }
  if (nokey) {
    y += DP(20);
    char kt[96];
    fm_snprintf(kt, sizeof kt, "Add your %s API key in Settings", s->name);
    y += info_card(FM_RECT(x, y, cw, 0), ui_id("on.card.key"), IC_KEY, kt, ktext, ka, 2, true);
  }
  if (noyt) {
    y += DP(14);
    info_card(FM_RECT(x, y, cw, 0), ui_id("on.card.yt"), IC_DOWNLOAD, "Get yt-dlp to play videos", ytext, ya, 1, true);
  }
}

/* ---- search field and recent searches ------------------------------------------------------------- */

static float pill_h(void) { return DP(ui.touch_mode ? 52 : 46); }

static void draw_search(FmRect pill, const FmVsrc *s) {
  u32 id = ui_id("on.search");
  if (S.focus_search) { ui_focus(id); S.focus_search = false; }
  bool focused = ui.focus == id;
  float t = ui_anim(ui_idn(id, 9), focused ? 1.0f : 0.0f, 16.0f);
  float rad = pill.h * 0.5f;
  if (t > 0.01f) gfx_shadow(pill, rad, DP(10), col_alpha(T.accent, 0.18f * t));
  gfx_rrect(pill, rad, T.surface2);
  gfx_rrect_line(pill, rad, DP(1) + DP(0.8f) * t, col_mix(T.border, T.accent, t));
  float is = DP(18);
  bool url = src_url_only(s);
  icon_draw(url ? IC_LINK : IC_SEARCH, FM_RECT(pill.x + DP(18), pill.y + (pill.h - is) * 0.5f, is, is),
            col_mix(T.text3, T.accent, t));
  /* right side: the round search button, then clear (or paste for links) */
  float bs = pill.h - DP(10);
  FmRect go = { pill.x + pill.w - bs - DP(5), pill.y + DP(5), bs, bs };
  float right = go.x - DP(4);
  bool clicked_go = false;
  {
    int f = ui_hit(ui_idn(id, 1), go);
    gfx_circle(go.x + bs * 0.5f, go.y + bs * 0.5f, bs * 0.5f, S.field[0] ? T.accent : col_alpha(T.accent, 0.45f));
    if (f & UI_HOVER) { gfx_circle(go.x + bs * 0.5f, go.y + bs * 0.5f, bs * 0.5f, T.hover); ui_set_cursor(SDL_SYSTEM_CURSOR_HAND); }
    icon_draw(url ? IC_PLAY : IC_ARROW_RIGHT, rect_center(go, DP(18), DP(18)), T.on_accent);
    ui_tip_track(ui_idn(id, 1), f, url ? "Open the link (Enter)" : "Search (Enter)");
    clicked_go = (f & UI_CLICK) != 0;
  }
  if (S.field[0]) {
    FmRect cl = { right - bs, go.y, bs, bs };
    if (ui_icon_btn(ui_idn(id, 2), cl, IC_CLOSE, T.text2, "Clear")) {
      S.field[0] = 0;
      ui_focus(id);
    }
    right = cl.x;
  } else if (url) {
    const char *l = "Paste";
    float pw = font_width(FONT_BOLD, ui.m.font_small, l, -1) + DP(24);
    FmRect pb = { right - pw, go.y + DP(4), pw, bs - DP(8) };
    if (ui_button(ui_idn(id, 3), pb, IC_NONE, l, UI_BTN_TEXT)) {
      char *c = ui_clipboard_get();
      if (c) { fm_strlcpy(S.field, c, sizeof S.field); fm_free(c); submit(S.field); }
    }
    right = pb.x;
  }
  /* the shared text field, its own frame clipped away so the pill shows */
  float x0 = pill.x + DP(46);
  FmRect clip = { x0 - DP(2), pill.y + DP(2), right - x0 + DP(2), pill.h - DP(4) };
  FmRect tf = { x0 - DP(12), pill.y - DP(8), right - x0 + DP(24), pill.h + DP(16) };
  char hint[96];
  if (url) fm_strlcpy(hint, "Paste a video link", sizeof hint);
  else fm_snprintf(hint, sizeof hint, "Search %s", s->name);
  gfx_clip_push(clip);
  int res = ui_textfield(id, tf, S.field, sizeof S.field, hint, 0);
  gfx_clip_pop();
  if (res & UI_TF_CHANGED) ui_redraw();
  if ((res & UI_TF_SUBMIT) || clicked_go) {
    if (S.field[0]) submit(S.field);
    else S.focus_search = true;
  }
}

/* Recent searches as wrapping chips, at most two rows. Returns the height. */
static float draw_recents(FmRect r, bool draw) {
  int n = orecent_count();
  if (n == 0) return 0;
  float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8);
  float x = r.x, y = r.y;
  int rows = 1;
  u32 base = ui_id("on.recent");
  for (int i = 0; i <= n; i++) {
    bool clear = i == n;
    const char *l = clear ? "Clear" : orecent_at(i);
    float w = chip_w(clear ? IC_DELETE : IC_HISTORY, l, DP(240));
    if (x + w > r.x + r.w && x > r.x) {
      if (rows == 2) break;
      rows++;
      x = r.x;
      y += ch + gap;
    }
    if (draw) {
      FmRect c = { x, y, w, ch };
      if (chip(ui_idn(base, (u32)i + 1), c, clear ? IC_DELETE : IC_HISTORY, l, false)) {
        if (clear) orecent_clear();
        else {
          char q[256];
          fm_strlcpy(q, l, sizeof q);
          fm_strlcpy(S.field, q, sizeof S.field);
          submit(q);
        }
        return 0;
      }
    }
    x += w + gap;
  }
  return (float)rows * ch + (float)(rows - 1) * gap;
}

/* ---- body, rail, chips, top bar --------------------------------------------------------------------- */

static void draw_body(FmRect r) {
  const FmVsrc *s = cur_src();
  float rad = ui.m.radius;
  gfx_shadow(r, rad, DP(12), T.shadow);
  gfx_rrect(r, rad, T.surface);
  if (T.panel_border.a) gfx_rrect_line(r, rad, DP(T.panel_border_w), T.panel_border);
  else gfx_rrect_line(r, rad, DP(1), T.border);
  if (!s) {
    state_view(r, IC_PLAY_BADGE, T.text2, "No video sources", "This build has no online video sources.", NULL, NULL, 0);
    return;
  }
  FmRect body = r;
  float side = body.w < DP(520) ? DP(12) : DP(18);
  float ph = pill_h();
  FmRect top = rect_cut_top(&body, ph + DP(ui.touch_mode ? 24 : 22));
  float pw = FM_MIN(top.w - side * 2, DP(760));
  draw_search(FM_RECT(top.x + (top.w - pw) * 0.5f, top.y + DP(12), pw, ph), s);
  if (!S.field[0] && !src_url_only(s) && orecent_count() > 0) {
    FmRect rr = { top.x + (top.w - pw) * 0.5f + DP(2), body.y, pw - DP(4), 0 };
    float h = draw_recents(rr, false);
    rect_cut_top(&body, h + DP(12));
    draw_recents(FM_RECT(rr.x, rr.y, rr.w, h), true);
  }
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  bool loading_first = S.search && !S.search_more;
  if (loading_first) draw_grid(body, true);
  else if (S.searched && S.estate != E_NONE && S.n == 0) error_state(body, s);
  else if (S.n > 0) draw_grid(body, false);
  else onboarding(body, s);
}

static void set_source(int i) {
  if (i == S.src || i < 0 || i >= vsrc_count()) return;
  const FmVsrc *old = cur_src();
  S.src = i;
  const FmVsrc *s = cur_src();
  fm_strlcpy(conf.online_source, s->key, sizeof conf.online_source);
  online_conf_dirty();
  S.chip_reveal = true;
  if (S.search) { drop_task(S.search); S.search = NULL; }
  clear_results();
  /* the same words on the new site; a link box starts empty */
  bool was_url = src_url_only(old), is_url = src_url_only(s);
  if (was_url != is_url) { S.field[0] = 0; S.query[0] = 0; }
  else if (S.query[0] && s->search) search_start(false);
  ui_redraw();
}

static void tray_toggle(void) {
  S.tray_open = !S.tray_open;
  ui_redraw();
}

static void draw_rail(FmRect r) {
  gfx_shadow(r, ui.m.radius, DP(10), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, ui.m.radius, T.surface);
  gfx_rrect_line(r, ui.m.radius, DP(1), T.border);
  FmRect in = rect_inset(r, DP(8));
  float rh = DP(ui.touch_mode ? 58 : 52), hh = DP(28);
  u32 base = ui_id("on.rail");
  float y = in.y;
  font_draw(FONT_BOLD, ui.m.font_small, in.x + DP(10), y + hh - font_line_h(ui.m.font_small) - DP(4), "SOURCES", -1,
            T.text3);
  y += hh;
  for (int i = 0; i < vsrc_count(); i++) {
    const FmVsrc *s = vsrc_at(i);
    FmRect row = { in.x, y, in.w, rh };
    y += rh + DP(2);
    int f = ui_hit(ui_idn(base, (u32)i + 1), row);
    bool here = i == S.src;
    if (here) gfx_rrect(row, DP(12), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
    float ts = DP(34);
    FmRect tile = { row.x + DP(8), row.y + (rh - ts) * 0.5f, ts, ts };
    if (here) gfx_rrect_vgrad(tile, DP(10), col_mix(T.accent, WHITE, 0.15f), T.accent);
    else gfx_rrect(tile, DP(10), T.surface2);
    icon_draw(s->icon ? s->icon : IC_PLAY_BADGE, rect_inset(tile, DP(8)), here ? T.on_accent : T.text2);
    float tx = tile.x + ts + DP(10), tw = row.x + row.w - tx - DP(8);
    float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
    float ty = row.y + (rh - lh - ls) * 0.5f;
    font_draw_ellipsis(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, tx, ty, s->name, tw, here ? T.accent : T.text);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, ty + lh, s->about ? s->about : "", tw, T.text3);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) set_source(i);
  }
  /* more: downloads and settings */
  y += DP(6);
  font_draw(FONT_BOLD, ui.m.font_small, in.x + DP(10), y + hh - font_line_h(ui.m.font_small) - DP(4), "MORE", -1,
            T.text3);
  y += hh;
  float mh = DP(ui.touch_mode ? 46 : 38);
  struct { FmIcon ic; const char *label; } kMore[] = { { IC_DOWNLOAD, "Downloads" }, { IC_SETTINGS, "Settings" } };
  for (int i = 0; i < 2; i++) {
    FmRect row = { in.x, y, in.w, mh };
    y += mh;
    int f = ui_hit(ui_idn(base, 100 + (u32)i), row);
    if ((i == 0 && S.tray_open)) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    icon_draw(kMore[i].ic, FM_RECT(row.x + DP(16), row.y + (mh - is) * 0.5f, is, is), T.text2);
    font_draw(FONT_REGULAR, ui.m.font, row.x + DP(50), row.y + (mh - font_line_h(ui.m.font)) * 0.5f, kMore[i].label, -1,
              T.text);
    if (i == 0 && odl_count() > 0) {
      char c[16];
      int act = odl_active();
      fm_snprintf(c, sizeof c, "%d", act ? act : odl_count());
      float cw = FM_MAX(DP(22), font_width(FONT_BOLD, ui.m.font_small, c, -1) + DP(12));
      FmRect b = { row.x + row.w - cw - DP(10), row.y + (mh - DP(22)) * 0.5f, cw, DP(22) };
      gfx_rrect(b, DP(11), act ? T.accent : T.surface3);
      font_draw_center(FONT_BOLD, ui.m.font_small, b, c, act ? T.on_accent : T.text2);
    }
    if (f & UI_CLICK) {
      if (i == 0) tray_toggle();
      else online_open_settings();
    }
  }
}

static void draw_src_chips(FmRect r) {
  u32 base = ui_id("on.chips");
  int n = vsrc_count();
  float ch = DP(ui.touch_mode ? 38 : 32), gap = DP(8), pad = DP(2);
  float fs = ui.m.font_small * 1.08f, is = DP(16);
  float w[16], total = pad;
  n = FM_MIN(n, 16);
  for (int i = 0; i < n; i++) {
    w[i] = font_width(FONT_BOLD, fs, vsrc_at(i)->name, -1) + is + DP(30);
    total += w[i] + gap;
  }
  total += pad - gap;
  float max = FM_MAX(0.0f, total - r.w);
  bool dragging = ui.drag_owner == base;
  if (!dragging && ui.down && ui.drag_owner == 0 && ui.moved && ui_input_ok() && rect_has(r, ui.press_x, ui.press_y) &&
      fabsf(ui.mx - ui.press_x) > fabsf(ui.my - ui.press_y)) {
    ui.drag_owner = base;
    S.chip_x0 = S.chip_x;
    dragging = true;
  }
  if (dragging && ui.down) S.chip_x = S.chip_x0 - (ui.mx - ui.press_x);
  if (ui_input_ok() && rect_has(r, ui.mx, ui.my) && (ui.wheel != 0 || ui.wheel_x != 0)) {
    S.chip_x -= (ui.wheel_x != 0 ? -ui.wheel_x : ui.wheel) * DP(60);
    ui.wheel = ui.wheel_x = 0;
  }
  if (S.chip_reveal && S.src < n) {
    float x = pad;
    for (int i = 0; i < S.src; i++) x += w[i] + gap;
    if (x - DP(24) < S.chip_x) S.chip_x = x - DP(24);
    if (x + w[S.src] + DP(24) > S.chip_x + r.w) S.chip_x = x + w[S.src] + DP(24) - r.w;
    S.chip_reveal = false;
  }
  S.chip_x = FM_CLAMP(S.chip_x, 0.0f, max);
  gfx_clip_push(r);
  float x = r.x + pad - S.chip_x;
  for (int i = 0; i < n; i++) {
    const FmVsrc *s = vsrc_at(i);
    FmRect c = { x, r.y + (r.h - ch) * 0.5f, w[i], ch };
    x += w[i] + gap;
    if (!gfx_visible(c)) continue;
    bool on = i == S.src;
    int f = ui_hit(ui_idn(base, (u32)i + 1), c);
    if (on) gfx_rrect(c, ch * 0.5f, T.accent);
    else {
      gfx_rrect(c, ch * 0.5f, T.surface);
      gfx_rrect_line(c, ch * 0.5f, DP(1), T.border);
      if (f & UI_HOVER) gfx_rrect(c, ch * 0.5f, T.hover);
    }
    FmColor fg = on ? T.on_accent : T.text;
    icon_draw(s->icon ? s->icon : IC_PLAY_BADGE, FM_RECT(c.x + DP(12), c.y + (ch - is) * 0.5f, is, is), on ? fg : T.text2);
    font_draw(on ? FONT_BOLD : FONT_REGULAR, fs, c.x + DP(12) + is + DP(6), c.y + (ch - font_line_h(fs)) * 0.5f, s->name,
              -1, fg);
    if (f & UI_CLICK) set_source(i);
  }
  gfx_clip_pop();
}

static void go_back(void) {
  if (ui.focus == ui_id("on.search")) { ui_focus(0); return; }
  online_close();
}

/* Downloads button: a ring shows the progress, a dot the count. */
static void downloads_btn(FmRect b, u32 id) {
  title_nodrag(b);
  int act = odl_active();
  if (ui_toggle_btn(id, b, IC_DOWNLOAD, S.tray_open, "Downloads")) tray_toggle();
  if (act > 0) {
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, rr = FM_MIN(b.w, b.h) * 0.5f - DP(3);
    float fr = odl_frac();
    gfx_ring(cx, cy, rr, DP(2), col_alpha(T.accent, 0.25f));
    if (fr >= 0) gfx_arc(cx, cy, rr, DP(2), -1.5707963f, -1.5707963f + 6.2831853f * fr, T.accent);
    char c[8];
    fm_snprintf(c, sizeof c, "%d", act);
    float d = DP(16);
    FmRect dot = { b.x + b.w - d - DP(1), b.y + DP(1), d, d };
    gfx_circle(dot.x + d * 0.5f, dot.y + d * 0.5f, d * 0.5f, T.accent);
    font_draw_center(FONT_BOLD, ui.m.font_small * 0.8f, dot, c, T.on_accent);
  }
}

static void draw_top(FmRect r, bool narrow) {
  u32 base = ui_id("on.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text, "Back to files")) online_close();
  rect_cut_left(&in, DP(4));
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, WHITE, 0.15f), T.accent);
    icon_draw(IC_PLAY_BADGE, rect_inset(m, DP(5)), T.on_accent);
    rect_cut_left(&in, DP(8));
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_SETTINGS, T.text2, "Online video settings")) online_open_settings();
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  downloads_btn(b, ui_idn(base, 3));
  const FmVsrc *s = cur_src();
  char sub[160];
  sub[0] = 0;
  /* narrow: the source is the title, the screen name moves to the second line */
  const char *title = narrow && s ? s->name : "Online videos";
  const char *lead = narrow || !s ? "Online videos" : s->name;
  if (S.search && !S.search_more) fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  Searching\xE2\x80\xA6", lead);
  else if (S.n > 0) fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d%s %s", lead, S.n, S.next[0] ? "+" : "",
                                S.n == 1 ? "video" : "videos");
  else fm_strlcpy(sub, lead, sizeof sub);
  float fs = ui.m.font_title * (narrow ? 0.9f : 1.0f);
  float tl = font_line_h(fs), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, fs, in.x + DP(2), y, title, in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

/* ---- detail sheet ----------------------------------------------------------------------------------- */

static float dlg_content_w(float w_dp) {
  bool sheet = ui.portrait && ui.w < DP(560);
  float w = sheet ? ui.w : FM_MIN(DP(w_dp), ui.w - DP(24));
  return w - DP(40);
}

static void draw_detail(void) {
  const FmVsrc *s = cur_src();
  FmVsrcItem *it = &S.det;
  float cw = dlg_content_w(600);
  float vis_h = ui.h - ui_keyboard_h();
  float tw = cw, th = floorf(tw * 9.0f / 16.0f);
  float max_th = vis_h * (ui.portrait ? 0.34f : 0.42f);
  if (th > max_th) { th = max_th; tw = th * 16.0f / 9.0f; }
  float tfs = ui.m.font_title;
  float title_h = (float)text_lines(FONT_BOLD, tfs, 0, 0, cw, it->title, 3, T.text, false) * font_line_h(tfs);
  bool two_rows = cw < DP(500);
  float bh = DP(ui.touch_mode ? 46 : 40);
  float btns = two_rows ? bh * 2 + DP(10) : bh;
  float h = th + DP(16) + title_h + DP(10) + DP(40) + DP(18) + btns;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("on.detail"), NULL, 600, (h + DP(40)) / ui.scale + 2, &cancel);
  FmRect thumb = { c.x + (cw - tw) * 0.5f, c.y, tw, th };
  thumb_box(thumb, it, DP(14));
  badge(thumb, it, 1.15f);
  if (play_disc(ui_id("on.det.play2"), thumb, 1.0f, DP(68))) { play_item(s, it); ui_dialog_end(); return; }
  float y = thumb.y + th + DP(16);
  text_lines(FONT_BOLD, tfs, c.x, y, cw, it->title[0] ? it->title : "Untitled", 3, T.text, true);
  y += title_h + DP(10);
  /* channel and stats */
  FmRect av = { c.x, y + DP(2), DP(36), DP(36) };
  avatar(av, it->channel);
  char stats[200], v[48], a[48], d[24];
  stats[0] = 0;
  ofmt_views(it->views, v, sizeof v);
  ofmt_age(it->published, a, sizeof a);
  if (it->live) join_dot(stats, sizeof stats, "Live now");
  else join_dot(stats, sizeof stats, v);
  join_dot(stats, sizeof stats, a);
  if (it->duration > 0 && !it->live) { ofmt_dur(it->duration, d, sizeof d); join_dot(stats, sizeof stats, d); }
  float tx = av.x + av.w + DP(12);
  float sw = 0;
  if (s) {
    float fs = ui.m.font_small;
    sw = font_width(FONT_BOLD, fs, s->name, -1) + DP(40);
    FmRect sc = { c.x + cw - sw, y + DP(7), sw, DP(26) };
    gfx_rrect(sc, DP(13), T.accent_soft);
    icon_draw(s->icon ? s->icon : IC_PLAY_BADGE, FM_RECT(sc.x + DP(9), sc.y + DP(5), DP(16), DP(16)), T.accent);
    font_draw(FONT_BOLD, fs, sc.x + DP(30), sc.y + (sc.h - font_line_h(fs)) * 0.5f, s->name, -1, T.accent);
  }
  float lw = c.x + cw - sw - DP(10) - tx;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, tx, y, it->channel[0] ? it->channel : "Unknown channel", lw, T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, y + font_line_h(ui.m.font), stats, lw, T.text2);
  y += DP(40) + DP(18);
  /* actions */
  u32 bid = ui_id("on.det.btn");
  int act = -1;
  if (two_rows) {
    float hw = (cw - DP(10)) * 0.5f;
    if (ui_button(ui_idn(bid, 0), FM_RECT(c.x, y, hw, bh), IC_PLAY, "Play", UI_BTN_FILLED)) act = 0;
    if (ui_button(ui_idn(bid, 1), FM_RECT(c.x + hw + DP(10), y, hw, bh), IC_DOWNLOAD, "Download", UI_BTN_TONAL)) act = 1;
    y += bh + DP(10);
    if (ui_button(ui_idn(bid, 2), FM_RECT(c.x, y, hw, bh), IC_OPEN_WITH, "Browser", UI_BTN_OUTLINE)) act = 2;
    if (ui_button(ui_idn(bid, 3), FM_RECT(c.x + hw + DP(10), y, hw, bh), IC_LINK, "Copy link", UI_BTN_OUTLINE)) act = 3;
  } else {
    const char *l[4] = { "Play", "Download", "Open in browser", "Copy link" };
    FmIcon ic[4] = { IC_PLAY, IC_DOWNLOAD, IC_OPEN_WITH, IC_LINK };
    int st[4] = { UI_BTN_FILLED, UI_BTN_TONAL, UI_BTN_OUTLINE, UI_BTN_OUTLINE };
    float bw[4], tot = 0;
    for (int i = 0; i < 4; i++) { bw[i] = font_width(FONT_BOLD, ui.m.font, l[i], -1) + DP(54); tot += bw[i]; }
    float extra = (cw - tot - DP(30)) / 4;
    float x = c.x;
    for (int i = 0; i < 4; i++) {
      float w = bw[i] + FM_MAX(0.0f, extra);
      if (ui_button(ui_idn(bid, (u32)i), FM_RECT(x, y, w, bh), ic[i], l[i], st[i])) act = i;
      x += w + DP(10);
    }
  }
  ui_dialog_end();
  if (act == 0) { play_item(s, it); return; }
  switch (act) {
    case 1: download_item(s, it); S.detail_open = false; break;
    case 2: open_url(it->page); break;
    case 3: ui_clipboard_set(it->page); ui_toast("Link copied"); break;
    default: break;
  }
  if (cancel) S.detail_open = false;
}

/* ---- preparing a stream ---------------------------------------------------------------------------- */

static void draw_prep(void) {
  const FmVsrcItem *it = &S.prep_item;
  bool fail = S.prep_state == PREP_FAIL;
  float cw = dlg_content_w(460);
  float bh = DP(ui.touch_mode ? 46 : 38);
  float th = DP(72), tw = th * 16.0f / 9.0f;
  float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
  float msg_h = fail ? font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, cw, S.prep_err, T.text2, false) : 0;
  bool slow = S.prep_src && (S.prep_src->flags & VSRC_YTDLP);
  bool inst = oinst_running(NULL, NULL, 0);
  float body = fail ? DP(8) + lh + DP(6) + msg_h + (inst ? DP(36) : 0) : DP(16) + ls + DP(10) + DP(6) + DP(8) + (slow ? ls * 2 + DP(6) : 0);
  float h = th + body + DP(22) + bh;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("on.prep"), NULL, 460, (h + DP(40)) / ui.scale + 2, &cancel);
  FmRect thumb = { c.x, c.y, tw, th };
  thumb_box(thumb, it, DP(10));
  float x = thumb.x + tw + DP(14), w = c.x + c.w - x;
  float ty = c.y + DP(4);
  int nl = text_lines(FONT_BOLD, ui.m.font, x, ty, w, it->title, 2, T.text, true);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, ty + lh * (float)nl + DP(2), it->channel, w, T.text2);
  float y = c.y + th;
  if (fail) {
    y += DP(8);
    icon_draw(IC_WARN, FM_RECT(c.x, y + (lh - DP(18)) * 0.5f, DP(18), DP(18)), T.danger);
    font_draw(FONT_BOLD, ui.m.font, c.x + DP(26), y, "Could not play this video", -1, T.text);
    y += lh + DP(6);
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, c.x, y, cw, S.prep_err, T.text2, true);
    y += msg_h;
    if (inst) install_line(FM_RECT(c.x, y + DP(2), cw, DP(32)));
  } else {
    y += DP(16);
    float el = (float)(SDL_GetTicks64() - (S.prep ? S.prep->t0 : SDL_GetTicks64())) / 1000.0f;
    char e[32];
    fm_snprintf(e, sizeof e, "%d s", (int)el);
    float ew = font_width(FONT_REGULAR, ui.m.font_small, e, -1);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, y, S.prep_status[0] ? S.prep_status : "Preparing\xE2\x80\xA6",
                       cw - ew - DP(12), T.text2);
    font_draw(FONT_REGULAR, ui.m.font_small, c.x + cw - ew, y, e, -1, T.text3);
    y += ls + DP(10);
    ui_progress(FM_RECT(c.x, y, cw, DP(6)), S.prep_frac);
    y += DP(14);
    /* a stream opens in a few seconds; only sites without a stream this
    ** app can read (HLS) download first, and then progress shows */
    if (slow)
      font_draw_wrap(FONT_REGULAR, ui.m.font_small, c.x, y, cw,
                     S.prep_frac >= 0 ? "This site has no stream the player can read directly, so yt-dlp downloads "
                                        "the video into the cache first."
                                      : "yt-dlp is finding the stream; playback starts as soon as the first seconds "
                                        "arrive.",
                     T.text3, true);
    view_wake_in(1000);                 /* the seconds counter */
  }
  /* buttons */
  FmRect br = { c.x, c.y + c.h - bh, cw, bh };
  u32 bid = ui_id("on.prep.btn");
  int res = -1;
  if (fail) {
    bool need_yt = S.prep_src && (S.prep_src->flags & VSRC_YTDLP) && !otools(false)->ytdlp;
    const char *l[3];
    int n = 0;
    l[n++] = "Close";
    if (need_yt) l[n++] = inst ? "Getting yt-dlp\xE2\x80\xA6" : "Get yt-dlp";
    else l[n++] = "Try again";
    l[n++] = "Open in browser";
    float x = br.x + br.w;
    for (int i = n - 1; i >= 0; i--) {
      float bw = font_width(FONT_BOLD, ui.m.font, l[i], -1) + DP(32);
      x -= bw;
      int st = i == n - 1 ? UI_BTN_FILLED : i == 1 ? UI_BTN_TONAL : UI_BTN_TEXT;
      if (ui_button(ui_idn(bid, (u32)i), FM_RECT(x, br.y, bw, bh), IC_NONE, l[i], st)) res = i;
      x -= DP(8);
    }
    if (res == 1) {
      if (need_yt) { if (!inst) oinst_start(); }
      else play_item(S.prep_src, it);
    }
    if (res == 2) { open_url(it->page); S.prep_open = false; }
    if (res == 0) S.prep_open = false;
  } else {
    const char *l = "Cancel";
    float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(40);
    if (ui_button(ui_idn(bid, 9), FM_RECT(br.x + br.w - bw, br.y, bw, bh), IC_NONE, l, UI_BTN_TONAL)) res = 0;
    if (res == 0) {
      drop_task(S.prep);
      S.prep = NULL;
      S.prep_open = false;
    }
  }
  ui_dialog_end();
  if (cancel) {
    if (S.prep) { drop_task(S.prep); S.prep = NULL; }
    S.prep_open = false;
  }
  /* yt-dlp arrived while the card is up: try again by itself */
  if (S.prep_open && fail && S.prep_src && (S.prep_src->flags & VSRC_YTDLP) && otools(false)->ytdlp && res < 0 &&
      strstr(S.prep_err, "needs yt-dlp"))
    play_item(S.prep_src, it);
}

/* ---- downloads tray ------------------------------------------------------------------------------- */

static void draw_tray(void) {
  int n = odl_count();
  float rh = DP(ui.touch_mode ? 84 : 76);
  float bh = DP(ui.touch_mode ? 44 : 36);
  float list_h = n ? FM_MIN((float)n, 5.5f) * rh : DP(120);
  float cw = dlg_content_w(560);
  float h = font_line_h(ui.m.font_title) + DP(12) + list_h + DP(14) + bh;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("on.tray"), "Downloads", 560, (h + DP(40)) / ui.scale + 2, &cancel);
  FmRect list = rect_cut_top(&c, list_h);
  char dir[FM_PATH_MAX];
  odl_dir(dir, sizeof dir);
  if (n == 0) {
    ui_label(FM_RECT(list.x, list.y + DP(30), list.w, font_line_h(ui.m.font)), "Videos you download appear here.",
             FONT_REGULAR, ui.m.font, T.text2, UI_CENTER);
    char t[FM_PATH_MAX + 16];
    fm_snprintf(t, sizeof t, "Saved to %s", dir);
    ui_label(FM_RECT(list.x, list.y + DP(30) + font_line_h(ui.m.font) + DP(4), list.w, font_line_h(ui.m.font_small)), t,
             FONT_REGULAR, ui.m.font_small, T.text3, UI_CENTER);
  } else {
    u32 sid = ui_id("on.tray.list");
    float content = (float)n * rh;
    ui_scroll(&S.tray_scroll, sid, list, content);
    gfx_clip_push(list);
    int remove = -1, cancel_i = -1;
    for (int i = 0; i < n; i++) {
      FmRect row = { list.x, list.y + (float)i * rh - S.tray_scroll.y, list.w, rh };
      if (!gfx_visible(row)) continue;
      OnDl *d = odl_at(i);
      u32 id = ui_idn(sid, (u32)i + 1);
      float th = rh - DP(22), tw = th * 16.0f / 9.0f;
      FmRect thumb = { row.x, row.y + DP(11), tw, th };
      thumb_box(thumb, &d->item, DP(8));
      float bs = FM_MIN(ui.m.hit, DP(40));
      FmRect act = { row.x + row.w, row.y + (rh - bs) * 0.5f, 0, bs };
      float x = thumb.x + tw + DP(12);
      /* buttons from the right */
      if (d->state == DL_RUNNING) {
        act.x -= bs; act.w = bs;
        if (ui_icon_btn(ui_idn(id, 1), act, IC_CLOSE, T.text2, "Cancel")) cancel_i = i;
      } else {
        act.x -= bs; act.w = bs;
        if (ui_icon_btn(ui_idn(id, 2), act, IC_CLOSE, T.text3, "Remove from the list")) remove = i;
        if (d->state == DL_DONE) {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 3), act, IC_FOLDER_OPEN, T.text2, "Show in folder")) online_reveal(d->path);
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 4), act, IC_PLAY, T.accent, "Play")) {
            if (!video_open_stream(d->item.title, d->path, NULL)) ui_toast("Could not open %s", fm_path_base(d->path));
          }
        } else {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 5), act, IC_REFRESH, T.text2, "Try again")) {
            FmVsrcItem copy = d->item;
            const FmVsrc *src = d->src;
            odl_remove(i);
            odl_start(src, &copy);
            break;
          }
        }
      }
      float w = act.x - DP(8) - x;
      float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
      float y = row.y + DP(12);
      font_draw_ellipsis(FONT_REGULAR, ui.m.font, x, y, d->item.title, w, T.text);
      y += lh + DP(2);
      char st[300];
      FmColor sc = T.text2;
      switch (d->state) {
        case DL_RUNNING:
          if (d->frac >= 0 && !strchr(d->status, '%'))
            fm_snprintf(st, sizeof st, "%d%%  \xC2\xB7  %s", (int)(d->frac * 100), d->status);
          else fm_strlcpy(st, d->status[0] ? d->status : "Starting\xE2\x80\xA6", sizeof st);
          break;
        case DL_DONE: fm_snprintf(st, sizeof st, "Done  \xC2\xB7  %s", fm_path_base(d->path)); sc = T.success; break;
        case DL_CANCELLED: fm_strlcpy(st, "Cancelled", sizeof st); sc = T.text3; break;
        default: fm_snprintf(st, sizeof st, "Failed: %s", d->err); sc = T.danger; break;
      }
      font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y, st, w, sc);
      y += ls + DP(6);
      if (d->state == DL_RUNNING) ui_progress(FM_RECT(x, y, w, DP(5)), d->frac);
      if (i < n - 1) ui_divider(x, row.x + row.w, row.y + rh - DP(0.5f));
    }
    gfx_clip_pop();
    ui_scrollbar(&S.tray_scroll, list, content);
    if (cancel_i >= 0) odl_cancel(cancel_i);
    if (remove >= 0) odl_remove(remove);
  }
  rect_cut_top(&c, DP(14));
  FmRect br = rect_cut_top(&c, bh);
  u32 bid = ui_id("on.tray.btn");
  const char *ld = "Done";
  float bw = font_width(FONT_BOLD, ui.m.font, ld, -1) + DP(40);
  bool done = ui_button(ui_idn(bid, 0), FM_RECT(br.x + br.w - bw, br.y, bw, bh), IC_NONE, ld, UI_BTN_FILLED);
  float x = br.x;
  const char *lo = cw < DP(400) ? "Folder" : "Open folder";
  float ow = font_width(FONT_BOLD, ui.m.font, lo, -1) + DP(56);
  if (ui_button(ui_idn(bid, 1), FM_RECT(x, br.y, ow, bh), IC_FOLDER_OPEN, lo, UI_BTN_TONAL)) {
    plat_mkdirs(dir);
    if (!plat_open_external(dir)) ui_toast("Saved to %s", dir);
  }
  x += ow + DP(8);
  bool fin = false;
  for (int i = 0; i < n; i++) fin |= odl_at(i)->state != DL_RUNNING;
  if (fin) {
    const char *lc = "Clear";
    float cwb = font_width(FONT_BOLD, ui.m.font, lc, -1) + DP(32);
    if (ui_button(ui_idn(bid, 2), FM_RECT(x, br.y, cwb, bh), IC_NONE, lc, UI_BTN_TEXT)) odl_clear_finished();
  }
  ui_dialog_end();
  if (cancel || done) S.tray_open = false;
}

/* ---- public ------------------------------------------------------------------------------------------ */

void online_init(bool readonly) {
  memset(&S, 0, sizeof S);
  S.readonly = readonly;
  ID_MENU = ui_id("on.menu");
  othumb_init();
  orecent_load(readonly);
}

void online_shutdown(void) {
  if (S.search) otask_free(S.search, true);
  if (S.prep) otask_free(S.prep, true);
  for (int i = 0; i < S.ndead; i++) S.dead[i]->cancel = 1;
  for (int i = 0; i < S.ndead; i++) otask_free(S.dead[i], true);
  S.ndead = 0;
  S.search = S.prep = NULL;
  odl_shutdown();
  oinst_shutdown();
  othumb_shutdown();
  fm_free(S.items);
  S.items = NULL;
}

bool online_is_open(void) { return S.open; }
void online_render_reset(void) { othumb_reset(); }
int online_downloads_active(void) { return odl_active(); }

void online_open(const char *source) {
  int idx = -1;
  const char *key = source && source[0] ? source : conf.online_source;
  for (int i = 0; i < vsrc_count(); i++)
    if (key && strcmp(vsrc_at(i)->key, key) == 0) idx = i;
  if (idx < 0 && source) {
    for (int i = 0; i < vsrc_count(); i++)
      if (fm_stricmp(vsrc_at(i)->name, source) == 0) idx = i;
  }
  if (idx >= 0 && idx != S.src) {
    if (S.search) { drop_task(S.search); S.search = NULL; }
    clear_results();
    S.src = idx;
  }
  S.open = true;
  S.chip_reveal = true;
  otools(true);
  if (!S.searched && S.n == 0 && !ui.touch_mode) S.focus_search = true;
  ui_redraw();
}

void online_close(void) {
  S.open = false;
  S.detail_open = S.tray_open = false;
  if (S.prep) { drop_task(S.prep); S.prep = NULL; }
  S.prep_open = false;
  ui_focus(0);
  /* the textures go back; results and the query stay for next time */
  othumb_reset();
  ui_redraw();
}

void online_pump(void) {
  reap();
  take_search();
  take_prep();
  odl_pump();
  oinst_pump();
  othumb_pump();
  /* --demo-online STATE: once the first results are in */
  if (S.demo_state[0] && !S.demo_done && S.searched && !S.search) {
    S.demo_done = true;
    /* "play3": the item number follows the state name */
    char st[24];
    fm_strlcpy(st, S.demo_state, sizeof st);
    int k = 0;
    size_t len = strlen(st);
    while (len > 0 && st[len - 1] >= '0' && st[len - 1] <= '9') len--;
    if (st[len]) { k = atoi(st + len); st[len] = 0; }
    k = FM_CLAMP(k, 0, FM_MAX(0, S.n - 1));
    if (S.n > 0 && !strcmp(st, "detail")) open_detail(&S.items[k]);
    else if (S.n > 0 && (!strcmp(st, "prepare") || !strcmp(st, "play"))) play_item(cur_src(), &S.items[k]);
    else if (S.n > 0 && !strcmp(st, "scroll")) S.scroll.y = 1e7f;   /* to the end: the next page loads */
    else if (S.n > 0 && !strcmp(st, "downloads")) {
      download_item(cur_src(), &S.items[0]);
      if (S.n > 1) download_item(cur_src(), &S.items[1]);
      S.tray_open = true;
    }
  }
}

void online_frame(FmRect area) {
  menu_results();
  if (!S.open) return;
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
    if (vsrc_count() > 1) draw_src_chips(rect_cut_top(&r, DP(ui.touch_mode ? 50 : 44)));
  } else {
    rect_cut_top(&r, DP(2));
    FmRect rail = rect_cut_left(&r, DP(236));
    rect_cut_left(&r, gpx);
    draw_rail(rail);
  }
  draw_body(r);
  draw_top(top, narrow);
  if (!S.open) return;
  /* one overlay at a time: preparing, then details, then the tray */
  if (S.prep_open) draw_prep();
  else if (S.detail_open) draw_detail();
  else if (S.tray_open) draw_tray();
  if (!S.open) return;
  if (ui_input_ok() && !ui_menu_is_open()) {
    if (ui_key(SDLK_f, KMOD_CTRL) || ui_key(SDLK_l, KMOD_CTRL)) S.focus_search = true;
    if (!ui.focus && ui_key(SDLK_SLASH, 0)) S.focus_search = true;
    if (ui_key(SDLK_F5, 0) || ui_key(SDLK_r, KMOD_CTRL)) { otools(true); search_start(false); }
    if (ui_key(SDLK_j, KMOD_CTRL)) tray_toggle();
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) go_back();
    if (!ui.focus && vsrc_count() > 1) {
      if (ui_key(SDLK_PAGEDOWN, KMOD_CTRL)) set_source((S.src + 1) % vsrc_count());
      if (ui_key(SDLK_PAGEUP, KMOD_CTRL)) set_source((S.src + vsrc_count() - 1) % vsrc_count());
    }
  }
}

/* ---- demo ----------------------------------------------------------------------------------------------- */

static void env_conf(const char *env, char *dst, size_t cap) {
  const char *v = getenv(env);
  if (v && v[0]) fm_strlcpy(dst, v, cap);
}

void online_demo(const char *source, const char *query, const char *state) {
  /* tools for test runs on a machine where they are not installed */
  env_conf("MMCFM_YTDLP", conf.ytdlp_path, sizeof conf.ytdlp_path);
  env_conf("MMCFM_JS_RUNTIME", conf.js_runtime, sizeof conf.js_runtime);
  env_conf("MMCFM_FFMPEG_DIR", conf.ffmpeg_dir, sizeof conf.ffmpeg_dir);
  env_conf("MMCFM_YT_KEY", conf.yt_api_key, sizeof conf.yt_api_key);
  if (state && !strcmp(state, "downloads")) {
    char tmp[FM_PATH_MAX];
    if (plat_place(PLACE_TEMP, tmp, sizeof tmp)) fm_path_join(conf.online_dl_dir, sizeof conf.online_dl_dir, tmp, "mmcfm-demo-dl");
  }
  online_open(source);
  fm_strlcpy(S.demo_state, state ? state : "", sizeof S.demo_state);
  if (query && query[0]) {
    fm_strlcpy(S.field, query, sizeof S.field);
    submit(query);
  }
  /* forced states, for screenshots of each */
  static const struct { const char *name; int st; const char *msg; } kForce[] = {
    { "nokey", E_NOKEY, "" }, { "noytdlp", E_NOYTDLP, "" },
    { "quota", E_QUOTA, "The request cannot be completed because you have exceeded your quota." },
    { "offline", E_OFFLINE, "WinHTTP: the server name or address could not be resolved" },
    { "empty", E_EMPTY, "" }, { "error", E_OTHER, "HTTP 500 from the server" },
  };
  for (int i = 0; state && i < FM_COUNT(kForce); i++)
    if (!strcmp(state, kForce[i].name)) {
      if (S.search) { drop_task(S.search); S.search = NULL; }
      clear_results();
      if (!S.query[0]) fm_strlcpy(S.query, query ? query : "cats", sizeof S.query);
      S.searched = true;
      S.estate = kForce[i].st;
      fm_strlcpy(S.error, kForce[i].msg, sizeof S.error);
      S.demo_done = true;
    }
  if (state && !strcmp(state, "settings")) online_open_settings();
  ui_focus(0);
  S.focus_search = false;
}
