/* fphoto.c -- the "Online photos" view: sources, search, states, selection.
**
** Design decisions:
**   - A sibling of the online videos view (fonline.c) and built the same
**     way: a full-window screen over the panels, a source rail on wide
**     windows and a chip strip on narrow ones, one rounded body card, a
**     search pill with recent searches, and states the user can act on.
**     The photo wall (fphoto_wall.c) replaces the video cards, the
**     lightbox (fphoto_box.c) the detail sheet.
**   - Three pages share the body: a source's results (or its curated page
**     when the search is empty and the source can browse), the album
**     covers, and one album. Each wall keeps its own scroll position, so
**     going to an album and back lands where the user was.
**   - Nothing blocks and nothing runs while idle: searches, full pictures
**     and downloads are PhTasks polled in photo_pump; the only timed
**     redraws are the loading shimmer (30 fps through a wake timer, only
**     while a page loads), thumbnail fade-ins and ui_anim transitions.
**   - Free sources work at once; keyed ones (Pexels, Unsplash, Pixabay)
**     explain where the free key comes from and link to the key page.
**   - Selection is per wall (each photo has a flag), so selecting works the
**     same on results and in albums; the bar above the wall offers
**     favourite, add to album, download and, in an album, remove.
*/
#include "fphoto_int.h"
#include "fview.h"
#include "fview_int.h"
#include "ftitle.h"

enum { E_NONE, E_NOKEY, E_QUOTA, E_OFFLINE, E_EMPTY, E_SIGNIN, E_OTHER };
enum { MI_OPEN = 1, MI_DOWNLOAD, MI_ALBUM, MI_FAV, MI_SELECT, MI_BROWSER, MI_COPY, MI_REMOVE };

static FmPhotoHooks g_hooks;

static struct {
  bool open;
  bool readonly;
  int src;
  int page;                    /* PG_* */
  int album;                   /* PG_ALBUM */
  char field[512];             /* the search box */
  char query[512];             /* what the shown results are for ("" = curated) */
  PhWall res;                  /* results of the current source */
  PhWall alb;                  /* the open album */
  char next[256];
  PhTask *search;
  bool search_more;
  bool more_err;
  bool searched;
  int estate;
  char error[256];
  float chip_x, chip_x0;
  bool chip_reveal;
  FmScroll rail_scroll;
  bool focus_search;
  bool tray_open;
  FmScroll tray_scroll;
  bool settings_focus;
  char demo_state[24];
  bool demo_done;
} S;

static u32 ID_MENU;
static int g_menu_item = -1;
static PhItem g_menu_copy;

/* ---- hooks and small helpers ------------------------------------------------------- */

void photo_set_hooks(const FmPhotoHooks *h) {
  if (h) g_hooks = *h;
  else memset(&g_hooks, 0, sizeof g_hooks);
}

void ph_conf_dirty(void) { if (g_hooks.conf_dirty) g_hooks.conf_dirty(); }

void ph_open_settings(void) {
  S.settings_focus = true;
  if (g_hooks.open_settings) g_hooks.open_settings();
}

void ph_reveal(const char *path) {
  if (g_hooks.reveal) { g_hooks.reveal(path); return; }
  if (!plat_share(path)) {
    char dir[FM_PATH_MAX];
    fm_strlcpy(dir, path, sizeof dir);
    fm_path_parent(dir);
    if (!plat_open_external(dir)) ui_toast("Saved in %s", dir);
  }
}

bool photo_settings_focus(void) {
  bool f = S.settings_focus;
  S.settings_focus = false;
  return f;
}

void ph_open_url(const char *url) {
  if (!url || !url[0]) { ui_toast("This photo has no web page"); return; }
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

void ph_copy_link(const PhItem *p) {
  const char *l = p->it.page[0] ? p->it.page : p->it.full;
  if (!l[0]) { ui_toast("This photo has no link"); return; }
  ui_clipboard_set(l);
  ui_toast("Link copied");
}

static const FmPsrc *cur_src(void) { return psrc_count() > 0 ? psrc_at(FM_CLAMP(S.src, 0, psrc_count() - 1)) : NULL; }

static PhWall *cur_wall(void) { return S.page == PG_ALBUM ? &S.alb : &S.res; }

int ph_page(void) { return S.page; }
int ph_cur_album(void) { return S.album; }

/* The settings field holding a keyed source's key, or NULL. */
static char *key_field(const FmPsrc *s, size_t *cap) {
  if (!s) return NULL;
  if (fm_stristr(s->key, "pexels")) { *cap = sizeof conf.key_pexels; return conf.key_pexels; }
  if (fm_stristr(s->key, "unsplash")) { *cap = sizeof conf.key_unsplash; return conf.key_unsplash; }
  if (fm_stristr(s->key, "pixabay")) { *cap = sizeof conf.key_pixabay; return conf.key_pixabay; }
  return NULL;
}

static const char *key_url(const FmPsrc *s) {
  if (!s) return "";
  if (fm_stristr(s->key, "pexels")) return "https://www.pexels.com/api/";
  if (fm_stristr(s->key, "unsplash")) return "https://unsplash.com/developers";
  if (fm_stristr(s->key, "pixabay")) return "https://pixabay.com/api/docs/";
  return "";
}

static bool missing_key(const FmPsrc *s) {
  if (!s || !(s->flags & PSRC_NEEDKEY)) return false;
  size_t cap;
  char *k = key_field(s, &cap);
  return !k || !k[0];
}

static bool can_browse(const FmPsrc *s) { return s && (s->flags & PSRC_BROWSE) && s->search; }
static bool can_search(const FmPsrc *s) { return s && (s->flags & PSRC_SEARCH) && s->search; }

static void join_dot(char *out, size_t cap, const char *part) {
  if (!part || !part[0]) return;
  if (out[0]) fm_strlcat(out, "  \xC2\xB7  ", cap);
  fm_strlcat(out, part, cap);
}

void ph_meta(const PhItem *p, char *out, size_t cap, bool with_source) {
  char by[160];
  out[0] = 0;
  if (p->it.author[0]) {
    fm_snprintf(by, sizeof by, "by %s", p->it.author);
    join_dot(out, cap, by);
  }
  join_dot(out, cap, p->it.license);
  if (with_source) join_dot(out, cap, ph_src_name(p->src));
}

int ph_text_lines(int face, float size, float x, float y, float w, const char *s, int lines, FmColor c, bool draw) {
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

/* ---- searching ------------------------------------------------------------------------ */

static void clear_results(void) {
  wall_clear(&S.res);
  S.next[0] = 0;
  S.more_err = false;
  S.searched = false;
  S.estate = E_NONE;
  S.error[0] = 0;
  othumb_forget_queue();
}

static int classify(const char *e, FmErr err) {
  if (fm_stristr(e, "rate limit") || fm_stristr(e, "too many requests") || fm_stristr(e, "429") ||
      fm_stristr(e, "quota"))
    return E_QUOTA;
  if (fm_stristr(e, "api key") || fm_stristr(e, "apikey") || fm_stristr(e, "needs a key") ||
      fm_stristr(e, "invalid key") || fm_stristr(e, "401") || fm_stristr(e, "unauthorized") ||
      fm_stristr(e, "access key") || fm_stristr(e, "client id"))
    return E_NOKEY;
  if (fm_stristr(e, "sign in") || fm_stristr(e, "sign-in") || fm_stristr(e, "oauth")) return E_SIGNIN;
  if (fm_stristr(e, "no photos") || fm_stristr(e, "no results") || fm_stristr(e, "nothing found")) return E_EMPTY;
  if (err == FM_ERR_UNSUPPORTED && fm_stristr(e, "network")) return E_OFFLINE;
  if (fm_stristr(e, "offline") || fm_stristr(e, "internet") || fm_stristr(e, "connect") ||
      fm_stristr(e, "resolve") || fm_stristr(e, "timed out") || fm_stristr(e, "timeout") ||
      fm_stristr(e, "unreachable") || fm_stristr(e, "network") || fm_stristr(e, "not available"))
    return E_OFFLINE;
  return E_OTHER;
}

static void search_start(bool more) {
  const FmPsrc *s = cur_src();
  if (!s || !s->search) return;
  if (!S.query[0] && !can_browse(s)) return;
  if (S.search) { ptask_drop(S.search); S.search = NULL; }
  if (!more) clear_results();
  if (missing_key(s)) {
    S.searched = true;
    S.estate = E_NOKEY;
    ui_redraw();
    return;
  }
  PhTask *t = ptask_new(PT_SEARCH, s);
  fm_strlcpy(t->query, S.query, sizeof t->query);
  if (more) fm_strlcpy(t->token, S.next, sizeof t->token);
  S.search = t;
  S.search_more = more;
  S.more_err = false;
  ptask_run(t);
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
  precent_add(b);
  ui_focus(0);
  S.page = PG_SOURCE;
  search_start(false);
}

/* The curated page of a source that has one, when nothing is searched. */
static void browse_if_idle(void) {
  const FmPsrc *s = cur_src();
  if (!S.query[0] && !S.search && !S.searched && S.res.n == 0 && can_browse(s)) search_start(false);
}

static void take_search(void) {
  PhTask *t = S.search;
  if (!t || !ptask_done(t)) return;
  S.search = NULL;
  const FmPsrc *s = t->src;
  if (t->err == FM_OK) {
    int start = S.search_more ? 0 : S.res.n;
    for (int i = 0; i < t->page.count; i++) {
      const FmPsrcItem *it = &t->page.items[i];
      /* a next page may repeat photos of this one: skip ids already shown */
      bool dup = false;
      for (int k = start; k < S.res.n && !dup; k++) dup = it->id[0] && strcmp(S.res.items[k].it.id, it->id) == 0;
      if (dup) continue;
      ph_item_init(wall_push(&S.res), it, s ? s->key : "");
    }
    fm_strlcpy(S.next, t->page.next, sizeof S.next);
    S.searched = true;
    S.estate = S.res.n == 0 ? E_EMPTY : E_NONE;
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
  ptask_free(t, false);
  ui_redraw();
}

void ph_need_more(PhWall *w) {
  if (w == &S.res && !S.search && !S.more_err && S.next[0] && S.res.n > 0) search_start(true);
}

/* ---- pages -------------------------------------------------------------------------------- */

static void rebuild_album(bool keep_scroll) {
  PhWall old = S.alb;
  PhWall nw;
  wall_init(&nw, "ph.wall.alb");
  int n = palb_size(S.album);
  for (int i = 0; i < n; i++) {
    PhItem *p = wall_push(&nw);
    palb_get(S.album, i, p);
    if (old.n) {
      int k = wall_find(&old, p->src, p->it.id);
      if (k >= 0) {
        p->sel = old.items[k].sel;
        p->tpx = old.items[k].tpx;
        if (old.items[k].ar > 0) p->ar = old.items[k].ar;
      }
    }
  }
  if (keep_scroll) {
    nw.scroll = old.scroll;
    nw.anchor = -1;
  }
  nw.id = old.id ? old.id : nw.id;
  wall_free(&old);
  S.alb = nw;
  S.alb.lay_n = -1;
}

void ph_album_changed(int a) {
  if (S.page == PG_ALBUM && a == S.album) rebuild_album(true);
}

void ph_album_removed(int a) {
  if (S.page != PG_ALBUM) return;
  if (S.album == a) ph_show_albums();
  else if (S.album > a) S.album--;
}

void ph_show_album(int a) {
  if (a < 0 || a >= palb_count()) return;
  if (box_is_open()) box_close();
  bool same = S.page == PG_ALBUM && S.album == a;
  S.page = PG_ALBUM;
  S.album = a;
  if (!same) {
    wall_free(&S.alb);
    wall_init(&S.alb, "ph.wall.alb");
  }
  rebuild_album(same);
  S.chip_reveal = true;
  ui_focus(0);
  ui_redraw();
}

void ph_show_albums(void) {
  if (box_is_open()) box_close();
  wall_select_all(&S.alb, false);
  S.page = PG_ALBUMS;
  S.chip_reveal = true;
  ui_focus(0);
  ui_redraw();
}

void ph_show_source(void) {
  S.page = PG_SOURCE;
  S.chip_reveal = true;
  browse_if_idle();
  ui_redraw();
}

static void set_source(int i) {
  if (i < 0 || i >= psrc_count()) return;
  if (i == S.src && S.page == PG_SOURCE) return;
  bool changed = i != S.src;
  S.page = PG_SOURCE;
  S.chip_reveal = true;
  if (!changed) { browse_if_idle(); ui_redraw(); return; }
  S.src = i;
  const FmPsrc *s = cur_src();
  fm_strlcpy(conf.photo_source, s->key, sizeof conf.photo_source);
  ph_conf_dirty();
  if (S.search) { ptask_drop(S.search); S.search = NULL; }
  clear_results();
  /* the same words on the new site, or its curated page */
  if (S.query[0] && can_search(s)) search_start(false);
  else {
    if (!can_search(s)) { S.query[0] = 0; S.field[0] = 0; }
    browse_if_idle();
  }
  ui_redraw();
}

/* ---- the tile menu -------------------------------------------------------------------------- */

static void open_menu(int i, float x, float y) {
  PhWall *w = cur_wall();
  if (i < 0 || i >= w->n) return;
  g_menu_item = i;
  g_menu_copy = w->items[i];
  bool fav = palb_is_fav(&g_menu_copy);
  FmMenuItem m[10];
  int n = 0;
  memset(m, 0, sizeof m);
  m[n].id = MI_OPEN; m[n].icon = IC_FULLSCREEN; m[n++].label = "Open";
  m[n].id = MI_FAV; m[n].icon = fav ? IC_HEART_FILL : IC_HEART; m[n++].label = fav ? "Remove from Favorites" : "Favorite";
  m[n].id = MI_ALBUM; m[n].icon = IC_ALBUM_ADD; m[n++].label = "Add to album";
  m[n].id = MI_DOWNLOAD; m[n].icon = IC_DOWNLOAD; m[n++].label = "Download";
  m[n].id = MI_SELECT; m[n].icon = IC_CHECK; m[n++].label = "Select";
  m[n++].flags = UI_MI_SEP;
  m[n].id = MI_BROWSER; m[n].icon = IC_OPEN_WITH; m[n++].label = "Open in browser";
  m[n].id = MI_COPY; m[n].icon = IC_LINK; m[n++].label = "Copy link";
  if (S.page == PG_ALBUM && S.album != PALB_FAV) {
    m[n].id = MI_REMOVE; m[n].icon = IC_DELETE; m[n].label = "Remove from album"; m[n++].flags = UI_MI_DANGER;
  }
  ui_menu_open(ID_MENU, x, y, m, n);
}

static void menu_results(void) {
  int r = ui_menu_result(ID_MENU);
  if (r < 0 || g_menu_item < 0) return;
  PhItem *p = &g_menu_copy;
  PhWall *w = cur_wall();
  int i = wall_find(w, p->src, p->it.id);
  switch (r) {
    case MI_OPEN: if (i >= 0) box_open(w, i); break;
    case MI_FAV: palb_fav_toggle(p); break;
    case MI_ALBUM: palb_pick_open(p, 1); break;
    case MI_DOWNLOAD: pdl_start(p, false); break;
    case MI_SELECT: if (i >= 0) { w->items[i].sel = true; w->anchor = i; } break;
    case MI_BROWSER: ph_open_url(p->it.page); break;
    case MI_COPY: ph_copy_link(p); break;
    case MI_REMOVE:
      palb_remove(S.album, p->src, p->it.id);
      ui_toast("Removed from \xE2\x80\x9C%s\xE2\x80\x9D", palb_name(S.album));
      break;
    default: break;
  }
  g_menu_item = -1;
  ui_redraw();
}

/* ---- chips and generic states ---------------------------------------------------------------- */

bool ph_chip(u32 id, FmRect c, FmIcon ic, const char *label, bool on) {
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

float ph_chip_w(FmIcon ic, const char *label, float max) {
  float fs = ui.m.font_small * 1.06f;
  return FM_MIN(max, font_width(FONT_BOLD, fs, label, -1) + DP(28) + (ic ? DP(21) : 0));
}

int ph_state_view(FmRect body, FmIcon ic, FmColor icol, const char *title, const char *sub, const char *detail,
                  const char *const *labels, const FmIcon *icons, int n) {
  float is = DP(56), bh = DP(ui.touch_mode ? 44 : 38);
  float tw = FM_MIN(body.w - DP(48), DP(520));
  float tl = font_line_h(ui.m.font_title);
  float sh = sub ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, tw, sub, T.text2, false) : 0;
  float dh = detail && detail[0] ? font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, detail, T.text3, false) : 0;
  float bw[4], total = 0;
  n = FM_MIN(n, 4);
  for (int i = 0; i < n; i++) {
    bw[i] = font_width(FONT_BOLD, ui.m.font, labels[i], -1) + (icons && icons[i] ? DP(56) : DP(36));
    total += bw[i] + (i ? DP(10) : 0);
  }
  bool stack = total > body.w - DP(32);
  float btns = n ? (stack ? (float)n * (bh + DP(8)) - DP(8) : bh) : 0;
  float h = is + DP(18) + tl + (sub ? sh + DP(6) : 0) + (dh > 0 ? dh + DP(8) : 0) + (n ? btns + DP(22) : 0);
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
  int res = -1;
  if (n) {
    y += DP(22);
    float x = cx - total * 0.5f;
    for (int i = 0; i < n; i++) {
      float sw = FM_MIN(tw, DP(320));
      FmRect b = stack ? FM_RECT(cx - sw * 0.5f, y + (float)i * (bh + DP(8)), sw, bh) : FM_RECT(x, y, bw[i], bh);
      if (ui_button(ui_idn(ui_id("ph.state.btn"), (u32)i), b, icons ? icons[i] : IC_NONE, labels[i],
                    i == 0 ? UI_BTN_FILLED : UI_BTN_TONAL))
        res = i;
      x += bw[i] + DP(10);
    }
  }
  return res;
}

/* ---- the source page: states and onboarding -------------------------------------------------- */

static void error_state(FmRect body, const FmPsrc *s) {
  char title[200], sub[400];
  const char *l[3];
  FmIcon ic[3];
  int n = 0, r;
  switch (S.estate) {
    case E_NOKEY:
      fm_snprintf(title, sizeof title, "Add a free %s key in Settings", s->name);
      fm_snprintf(sub, sizeof sub, missing_key(s) ? "%s asks for a free API key. It takes a minute to get one; "
                                                    "the free sources work without one meanwhile."
                                                  : "%s did not accept the key. Check it in Settings, or get a new "
                                                    "one.", s->name);
      l[n] = "Open Settings"; ic[n++] = IC_SETTINGS;
      l[n] = "Get a free key"; ic[n++] = IC_OPEN_WITH;
      r = ph_state_view(body, IC_KEY, T.accent, title, sub, missing_key(s) ? NULL : S.error, l, ic, n);
      if (r == 0) ph_open_settings();
      if (r == 1) ph_open_url(key_url(s));
      break;
    case E_QUOTA:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      r = ph_state_view(body, IC_WARN, T.warn, "Slow down a little",
                        "The site limits how many searches come in a minute or an hour. Try again in a moment; the "
                        "other sources keep working meanwhile.", S.error, l, ic, n);
      if (r == 0) search_start(false);
      break;
    case E_OFFLINE:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      if (palb_count() > 0) { l[n] = "Your albums"; ic[n++] = IC_ALBUM; }
      r = ph_state_view(body, IC_NETWORK, T.text2, "You're offline",
                        "Check the internet connection, then try again. Albums open offline with what is cached.",
                        S.error, l, ic, n);
      if (r == 0) search_start(false);
      if (r == 1) ph_show_albums();
      break;
    case E_EMPTY:
      fm_snprintf(title, sizeof title, "No photos for \xE2\x80\x9C%s\xE2\x80\x9D", S.query);
      l[n] = "New search"; ic[n++] = IC_SEARCH;
      r = ph_state_view(body, IC_SEARCH, T.accent, S.query[0] ? title : "Nothing here right now",
                        "Try other words, or another source.", NULL, l, ic, n);
      if (r == 0) { S.field[0] = 0; S.query[0] = 0; clear_results(); S.focus_search = true; }
      break;
    case E_SIGNIN:
      l[n] = "Open the website"; ic[n++] = IC_OPEN_WITH;
      r = ph_state_view(body, IC_PERSON, T.accent, "This source needs a sign-in",
                        "Signing in is not built into this app yet. The other sources need no account.", S.error, l,
                        ic, n);
      if (r == 0) ph_open_url(key_url(s));
      break;
    default:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      r = ph_state_view(body, IC_WARN, T.danger, "Something went wrong", "The search did not work this time.", S.error,
                        l, ic, n);
      if (r == 0) search_start(false);
      break;
  }
}

static const char *const kTry[] = { "Mountains", "Northern lights", "Cats", "Architecture", "Ocean", "Flowers" };

/* The card telling where a keyed source's free key comes from. */
static float key_card(FmRect r, const FmPsrc *s, bool draw) {
  float pad = DP(16), is = DP(36);
  float tx = pad + is + DP(14), tw = r.w - tx - pad;
  char text[300];
  fm_snprintf(text, sizeof text, "%s gives out a free API key in a minute. Paste it in Settings; the free sources "
                                 "(no key needed) work meanwhile.", s->name);
  float tl = font_line_h(ui.m.font);
  float th = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, text, T.text2, false);
  float bh = DP(ui.touch_mode ? 40 : 34);
  const char *l0 = "Open Settings", *l1 = "Get a free key";
  float b0 = font_width(FONT_BOLD, ui.m.font, l0, -1) + DP(52), b1 = font_width(FONT_BOLD, ui.m.font, l1, -1) + DP(52);
  bool two_rows = b0 + b1 + DP(8) > tw;
  float h = pad + tl + DP(4) + th + DP(12) + (two_rows ? bh * 2 + DP(8) : bh) + pad;
  if (!draw) return h;
  FmRect c = { r.x, r.y, r.w, h };
  gfx_rrect(c, DP(14), T.surface2);
  gfx_rrect_line(c, DP(14), DP(1), T.border);
  FmRect ib = { c.x + pad, c.y + pad, is, is };
  gfx_rrect(ib, DP(10), T.accent_soft);
  icon_draw(IC_KEY, rect_inset(ib, DP(8)), T.accent);
  float x = c.x + tx, y = c.y + pad;
  char title[96];
  fm_snprintf(title, sizeof title, "Add a free %s key in Settings", s->name);
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, title, tw, T.text);
  y += tl + DP(4);
  font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, y, tw, text, T.text2, true);
  y += th + DP(12);
  u32 id = ui_id("ph.keycard");
  if (ui_button(ui_idn(id, 0), FM_RECT(x, y, FM_MIN(b0, tw), bh), IC_SETTINGS, l0, UI_BTN_FILLED)) ph_open_settings();
  if (two_rows) y += bh + DP(8);
  else x += b0 + DP(8);
  if (ui_button(ui_idn(id, 1), FM_RECT(x, y, FM_MIN(b1, tw), bh), IC_OPEN_WITH, l1, UI_BTN_TONAL)) ph_open_url(key_url(s));
  return h;
}

static void onboarding(FmRect body, const FmPsrc *s) {
  bool nokey = missing_key(s);
  float cw = FM_MIN(body.w - DP(32), DP(560));
  float x = body.x + (body.w - cw) * 0.5f;
  float is = DP(64), tlh = font_line_h(ui.m.font_title * 1.2f);
  float ab = s->about ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw, s->about, T.text2, false) : 0;
  bool tries = !nokey && precent_count() == 0 && can_search(s);
  float h = is + DP(18) + tlh + DP(6) + ab + (tries ? DP(56) : 0);
  if (nokey) h += DP(20) + key_card(FM_RECT(x, 0, cw, 0), s, false);
  float y = body.y + FM_MAX(DP(16), (body.h - h) * 0.4f);
  float cx = body.x + body.w * 0.5f;
  FmRect tile = { cx - is * 0.5f, y, is, is };
  gfx_shadow(tile, DP(18), DP(14), col_alpha(T.accent, 0.35f));
  gfx_rrect_vgrad(tile, DP(18), col_mix(T.accent, PH_WHITE, 0.18f), T.accent);
  icon_draw(s->icon ? s->icon : IC_ALBUM, rect_inset(tile, DP(15)), T.on_accent);
  y += is + DP(18);
  char title[96];
  fm_snprintf(title, sizeof title, "Search %s", s->name);
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
      ws[i] = ph_chip_w(IC_SEARCH, kTry[i], DP(200));
      if (total + ws[i] + (n ? gap : 0) > body.w - DP(32)) break;
      total += ws[i] + (n ? gap : 0);
      n++;
    }
    float xx = cx - total * 0.5f;
    for (int i = 0; i < n; i++) {
      if (ph_chip(ui_idn(ui_id("ph.try"), (u32)i), FM_RECT(xx, y, ws[i], ch), IC_SEARCH, kTry[i], false)) {
        fm_strlcpy(S.field, kTry[i], sizeof S.field);
        submit(kTry[i]);
      }
      xx += ws[i] + gap;
    }
    y += ch;
  }
  if (nokey) {
    y += DP(20);
    key_card(FM_RECT(x, y, cw, 0), s, true);
  }
}

/* ---- search field and recent searches ---------------------------------------------------------- */

static float pill_h(void) { return DP(ui.touch_mode ? 52 : 46); }

static void draw_search(FmRect pill, const FmPsrc *s) {
  u32 id = ui_id("ph.search");
  if (S.focus_search) { ui_focus(id); S.focus_search = false; }
  bool focused = ui.focus == id;
  float t = ui_anim(ui_idn(id, 9), focused ? 1.0f : 0.0f, 16.0f);
  float rad = pill.h * 0.5f;
  if (t > 0.01f) gfx_shadow(pill, rad, DP(10), col_alpha(T.accent, 0.18f * t));
  gfx_rrect(pill, rad, T.surface2);
  gfx_rrect_line(pill, rad, DP(1) + DP(0.8f) * t, col_mix(T.border, T.accent, t));
  float is = DP(18);
  icon_draw(IC_SEARCH, FM_RECT(pill.x + DP(18), pill.y + (pill.h - is) * 0.5f, is, is), col_mix(T.text3, T.accent, t));
  float bs = pill.h - DP(10);
  FmRect go = { pill.x + pill.w - bs - DP(5), pill.y + DP(5), bs, bs };
  float right = go.x - DP(4);
  bool clicked_go = false;
  {
    int f = ui_hit(ui_idn(id, 1), go);
    gfx_circle(go.x + bs * 0.5f, go.y + bs * 0.5f, bs * 0.5f, S.field[0] ? T.accent : col_alpha(T.accent, 0.45f));
    if (f & UI_HOVER) {
      gfx_circle(go.x + bs * 0.5f, go.y + bs * 0.5f, bs * 0.5f, T.hover);
      ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    }
    icon_draw(IC_ARROW_RIGHT, rect_center(go, DP(18), DP(18)), T.on_accent);
    ui_tip_track(ui_idn(id, 1), f, "Search (Enter)");
    clicked_go = (f & UI_CLICK) != 0;
  }
  if (S.field[0]) {
    FmRect cl = { right - bs, go.y, bs, bs };
    if (ui_icon_btn(ui_idn(id, 2), cl, IC_CLOSE, T.text2, "Clear")) {
      S.field[0] = 0;
      ui_focus(id);
    }
    right = cl.x;
  }
  float x0 = pill.x + DP(46);
  FmRect clip = { x0 - DP(2), pill.y + DP(2), right - x0 + DP(2), pill.h - DP(4) };
  FmRect tf = { x0 - DP(12), pill.y - DP(8), right - x0 + DP(24), pill.h + DP(16) };
  char hint[96];
  fm_snprintf(hint, sizeof hint, "Search %s", s->name);
  gfx_clip_push(clip);
  int res = ui_textfield(id, tf, S.field, sizeof S.field, hint, 0);
  gfx_clip_pop();
  if (res & UI_TF_CHANGED) ui_redraw();
  if ((res & UI_TF_SUBMIT) || clicked_go) {
    if (S.field[0]) submit(S.field);
    else if (S.query[0]) {
      /* an emptied box goes back to the curated page */
      S.query[0] = 0;
      if (S.search) { ptask_drop(S.search); S.search = NULL; }
      clear_results();
      browse_if_idle();
    } else S.focus_search = true;
  }
}

static float draw_recents(FmRect r, bool draw) {
  int n = precent_count();
  if (n == 0) return 0;
  float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8);
  float x = r.x, y = r.y;
  int rows = 1;
  u32 base = ui_id("ph.recent");
  for (int i = 0; i <= n; i++) {
    bool clear = i == n;
    const char *l = clear ? "Clear" : precent_at(i);
    float w = ph_chip_w(clear ? IC_DELETE : IC_HISTORY, l, DP(240));
    if (x + w > r.x + r.w && x > r.x) {
      if (rows == 2) break;
      rows++;
      x = r.x;
      y += ch + gap;
    }
    if (draw) {
      FmRect c = { x, y, w, ch };
      if (ph_chip(ui_idn(base, (u32)i + 1), c, clear ? IC_DELETE : IC_HISTORY, l, false)) {
        if (clear) precent_clear();
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

/* ---- selection bar --------------------------------------------------------------------------------- */

static PhItem *selected_items(PhWall *w, int *n) {
  int k = wall_nsel(w);
  *n = k;
  if (!k) return NULL;
  PhItem *out = (PhItem *)fm_alloc(sizeof *out * (size_t)k);
  int j = 0;
  for (int i = 0; i < w->n; i++)
    if (w->items[i].sel) out[j++] = w->items[i];
  return out;
}

static void sel_action(int act) {
  PhWall *w = cur_wall();
  int n;
  PhItem *items = selected_items(w, &n);
  if (!items) return;
  switch (act) {
    case 0: {                                  /* favourite all (or unfavourite when all are) */
      bool all = true;
      for (int i = 0; i < n && all; i++) all = palb_is_fav(&items[i]);
      int changed = 0;
      for (int i = 0; i < n; i++) {
        if (all) { palb_remove(PALB_FAV, items[i].src, items[i].it.id); changed++; }
        else changed += palb_add(PALB_FAV, &items[i]);
      }
      if (all) ui_toast("Removed %d from Favorites", changed);
      else ui_toast("Added %d to Favorites", changed);
      break;
    }
    case 1: palb_pick_open(items, n); break;
    case 2: pdl_batch(items, n); break;
    case 3:
      for (int i = 0; i < n; i++) palb_remove(S.album, items[i].src, items[i].it.id);
      ui_toast("Removed %d from \xE2\x80\x9C%s\xE2\x80\x9D", n, palb_name(S.album));
      break;
    default: break;
  }
  fm_free(items);
  if (act != 1) wall_select_all(cur_wall(), false);
}

static void selection_bar(FmRect r) {
  PhWall *w = cur_wall();
  int n = wall_nsel(w);
  u32 id = ui_id("ph.selbar");
  float h = pill_h();
  FmRect bar = { r.x, r.y + (r.h - h) * 0.5f, r.w, h };
  /* opaque: a translucent pill shows a seam where its two halves meet */
  gfx_rrect(bar, h * 0.5f, col_mix(T.surface, T.accent, T.dark ? 0.16f : 0.12f));
  FmRect in = rect_inset2(bar, DP(5), DP(5));
  float bs = in.h;
  if (ui_icon_btn(ui_idn(id, 1), rect_cut_left(&in, bs), IC_CLOSE, T.text, "Clear the selection (Esc)"))
    wall_select_all(w, false);
  rect_cut_left(&in, DP(6));
  bool narrow = bar.w < DP(620);
  bool album = S.page == PG_ALBUM && S.album != PALB_FAV;
  struct { FmIcon ic; const char *label; int act; } kAct[4] = {
    { IC_DOWNLOAD, "Download", 2 }, { IC_ALBUM_ADD, "Add to album", 1 },
    { IC_HEART, "Favorite", 0 },    { IC_DELETE, "Remove", 3 },
  };
  int na = album ? 4 : 3;
  for (int i = 0; i < na; i++) {
    if (narrow) {
      FmRect b = rect_cut_right(&in, bs);
      if (ui_icon_btn(ui_idn(id, 10 + (u32)i), b, kAct[i].ic, kAct[i].act == 3 ? T.danger : T.text, kAct[i].label))
        sel_action(kAct[i].act);
    } else {
      float bw = font_width(FONT_BOLD, ui.m.font, kAct[i].label, -1) + DP(54);
      FmRect b = rect_cut_right(&in, bw);
      if (ui_button(ui_idn(id, 10 + (u32)i), b, kAct[i].ic, kAct[i].label, i == 0 ? UI_BTN_FILLED : UI_BTN_TEXT))
        sel_action(kAct[i].act);
      rect_cut_right(&in, DP(4));
    }
  }
  /* the count, and select all when there is room */
  char t[48];
  fm_snprintf(t, sizeof t, "%d selected", n);
  float fs = ui.m.font;
  float tx = in.x + DP(2);
  float end = font_draw_ellipsis(FONT_BOLD, fs, tx, in.y + (in.h - font_line_h(fs)) * 0.5f, t, in.w - DP(4), T.text);
  const char *sa = n == w->n ? "None" : "All";
  float sw = font_width(FONT_BOLD, ui.m.font_small, sa, -1) + DP(24);
  if (tx + end + DP(8) + sw <= in.x + in.w) {
    FmRect sb = { tx + end + DP(8), in.y + DP(4), sw, in.h - DP(8) };
    if (ui_button(ui_idn(id, 3), sb, IC_NONE, sa, UI_BTN_TEXT)) wall_select_all(w, n != w->n);
  }
}

/* ---- the body ------------------------------------------------------------------------------------- */

static void results_footer(FmRect view, const PhWallRes *wr) {
  bool more = S.search && S.search_more;
  if (S.res.n == 0 || more) return;
  FmRect fr = { view.x, wr->footer_y, view.w, DP(64) };
  gfx_clip_push(view);
  if (gfx_visible(fr)) {
    if (S.more_err) {
      const char *l = "Couldn't load more. Try again";
      float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(56);
      if (ui_button(ui_id("ph.more.retry"), rect_center(fr, bw, DP(ui.touch_mode ? 44 : 36)), IC_REFRESH, l, UI_BTN_TONAL))
        search_start(true);
    } else if (!S.next[0]) {
      char t[64];
      fm_snprintf(t, sizeof t, "%d %s", S.res.n, S.res.n == 1 ? "photo" : "photos");
      font_draw_center(FONT_REGULAR, ui.m.font_small, fr, t, T.text3);
    }
  }
  gfx_clip_pop();
}

static void draw_wall(PhWall *w, FmRect view, bool results) {
  PhWallOpts o;
  memset(&o, 0, sizeof o);
  bool loading_first = results && S.search && !S.search_more;
  bool more = results && S.search && S.search_more;
  if (loading_first) {
    o.skeleton = 24;
    o.fixed = true;
  } else if (more) {
    o.skeleton = view.w < DP(600) ? 6 : 8;
  }
  o.footer = results && !loading_first && S.res.n > 0 && !more ? DP(64) : DP(10);
  PhWallRes wr = wall_draw(loading_first ? &S.res : w, view, &o);
  if (loading_first) return;
  if (results) {
    results_footer(view, &wr);
    if (wr.near_end) ph_need_more(&S.res);
  }
  if (wr.open >= 0) box_open(w, wr.open);
  else if (wr.menu >= 0) open_menu(wr.menu, wr.menu_x, wr.menu_y);
}

static void source_page(FmRect body, const FmPsrc *s) {
  float side = body.w < DP(520) ? DP(12) : DP(18);
  float ph = pill_h();
  int nsel = wall_nsel(&S.res);
  FmRect top = rect_cut_top(&body, ph + DP(ui.touch_mode ? 24 : 22));
  float pw = FM_MIN(top.w - side * 2, DP(760));
  FmRect pill = { top.x + (top.w - pw) * 0.5f, top.y + DP(12), pw, ph };
  if (nsel > 0) selection_bar(FM_RECT(top.x + side, pill.y, top.w - side * 2, ph));
  else if (can_search(s)) draw_search(pill, s);
  else {
    float tl = font_line_h(ui.m.font_title);
    font_draw_ellipsis(FONT_BOLD, ui.m.font_title, pill.x + DP(6), pill.y + (ph - tl) * 0.5f, s->name, pill.w, T.text);
  }
  if (nsel == 0 && !S.field[0] && precent_count() > 0 && can_search(s)) {
    FmRect rr = { pill.x + DP(2), body.y, pw - DP(4), 0 };
    float h = draw_recents(rr, false);
    rect_cut_top(&body, h + DP(12));
    draw_recents(FM_RECT(rr.x, rr.y, rr.w, h), true);
  }
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  bool loading_first = S.search && !S.search_more;
  if (loading_first) draw_wall(&S.res, body, true);
  else if (S.searched && S.estate != E_NONE && S.res.n == 0) error_state(body, s);
  else if (S.res.n > 0) draw_wall(&S.res, body, true);
  else if (missing_key(s) && S.searched) error_state(body, s);
  else onboarding(body, s);
}

static void album_page(FmRect body) {
  int a = S.album;
  PhWall *w = &S.alb;
  int nsel = wall_nsel(w);
  float side = body.w < DP(520) ? DP(12) : DP(18);
  FmRect head = rect_cut_top(&body, pill_h() + DP(ui.touch_mode ? 24 : 22));
  FmRect in = rect_inset2(head, side - DP(6), 0);
  if (nsel > 0) {
    selection_bar(FM_RECT(head.x + side, head.y + DP(12), head.w - side * 2, pill_h()));
  } else {
    u32 id = ui_id("ph.albhead");
    float bs = DP(ui.touch_mode ? 44 : 38);
    if (ui_icon_btn(ui_idn(id, 1), rect_center(rect_cut_left(&in, bs), bs, bs), IC_BACK, T.text, "All albums"))
      ph_show_albums();
    rect_cut_left(&in, DP(6));
    bool narrow = in.w < DP(460);
    if (a == PALB_FAV) rect_cut_right(&in, DP(8));
    else {
      FmRect b = rect_center(rect_cut_right(&in, bs), bs, bs);
      if (ui_icon_btn(ui_idn(id, 2), b, IC_MORE, T.text2, "More")) palb_album_menu(a, b.x + b.w, b.y + b.h);
    }
    if (palb_size(a) > 0) {
      const char *l = narrow ? "Download" : "Download album";
      float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(52);
      float bh = DP(ui.touch_mode ? 40 : 34);
      FmRect b = rect_cut_right(&in, bw);
      if (ui_button(ui_idn(id, 3), rect_center(b, bw, bh), IC_DOWNLOAD, l, UI_BTN_TONAL)) palb_download(a);
      rect_cut_right(&in, DP(8));
    }
    char sub[64];
    fm_snprintf(sub, sizeof sub, "%d %s", palb_size(a), palb_size(a) == 1 ? "photo" : "photos");
    float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
    float ty = in.y + (in.h - tl - sl) * 0.5f;
    if (a == PALB_FAV) {
      icon_draw(IC_HEART_FILL, FM_RECT(in.x, ty + (tl - DP(20)) * 0.5f, DP(20), DP(20)), T.accent);
      in.x += DP(26);
      in.w -= DP(26);
    }
    font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x, ty, palb_name(a), in.w, T.text);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x - (a == PALB_FAV ? DP(26) : 0), ty + tl, sub, in.w, T.text2);
  }
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  if (w->n == 0) {
    static const char *const kL[] = { "Find photos" };
    static const FmIcon kI[] = { IC_SEARCH };
    bool fav = a == PALB_FAV;
    int r = ph_state_view(body, fav ? IC_HEART : IC_ALBUM, T.accent, fav ? "No favorites yet" : "This album is empty",
                          fav ? "Tap the heart on a photo to keep it here. Favorites stay on this device."
                              : "Select photos in a search (long-press or the round check), then Add to album.",
                          NULL, kL, kI, 1);
    if (r == 0) ph_show_source();
    return;
  }
  draw_wall(w, body, false);
}

static void draw_body(FmRect r) {
  const FmPsrc *s = cur_src();
  float rad = ui.m.radius;
  gfx_shadow(r, rad, DP(12), T.shadow);
  gfx_rrect(r, rad, T.surface);
  if (T.panel_border.a) gfx_rrect_line(r, rad, DP(T.panel_border_w), T.panel_border);
  else gfx_rrect_line(r, rad, DP(1), T.border);
  if (S.page == PG_ALBUMS) { palb_page(r); return; }
  if (S.page == PG_ALBUM) { album_page(r); return; }
  if (!s) {
    ph_state_view(r, IC_ALBUM, T.text2, "No photo sources", "This build has no online photo sources.", NULL, NULL, NULL, 0);
    return;
  }
  source_page(r, s);
}

/* ---- rail, chips, top bar --------------------------------------------------------------------------- */

static void tray_toggle(void) {
  S.tray_open = !S.tray_open;
  ui_redraw();
}

static void rail_heading(float x, float *y, float hh, const char *t) {
  font_draw(FONT_BOLD, ui.m.font_small, x + DP(10), *y + hh - font_line_h(ui.m.font_small) - DP(4), t, -1, T.text3);
  *y += hh;
}

/* A small count at the right end of a rail row. */
static void rail_count(FmRect row, int n, bool strong) {
  char c[16];
  fm_snprintf(c, sizeof c, "%d", n);
  float cw = FM_MAX(DP(22), font_width(FONT_BOLD, ui.m.font_small, c, -1) + DP(12));
  FmRect b = { row.x + row.w - cw - DP(10), row.y + (row.h - DP(22)) * 0.5f, cw, DP(22) };
  gfx_rrect(b, DP(11), strong ? T.accent : T.surface3);
  font_draw_center(FONT_BOLD, ui.m.font_small, b, c, strong ? T.on_accent : T.text2);
}

static void draw_rail(FmRect r) {
  gfx_shadow(r, ui.m.radius, DP(10), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, ui.m.radius, T.surface);
  gfx_rrect_line(r, ui.m.radius, DP(1), T.border);
  FmRect in = rect_inset(r, DP(8));
  float rh = DP(ui.touch_mode ? 58 : 52), hh = DP(28), mh = DP(ui.touch_mode ? 46 : 40);
  int ns = psrc_count();
  float content = hh + (float)ns * (rh + DP(2)) + DP(6) + hh + mh * 2 + DP(6) + hh + mh * 2 + DP(4);
  u32 base = ui_id("ph.rail");
  ui_scroll(&S.rail_scroll, base, in, content);
  gfx_clip_push(in);
  float y = in.y - S.rail_scroll.y;
  rail_heading(in.x, &y, hh, "SOURCES");
  for (int i = 0; i < ns; i++) {
    const FmPsrc *s = psrc_at(i);
    FmRect row = { in.x, y, in.w, rh };
    y += rh + DP(2);
    if (!gfx_visible(row)) continue;
    int f = ui_hit(ui_idn(base, (u32)i + 1), row);
    bool here = i == S.src && S.page == PG_SOURCE;
    if (here) gfx_rrect(row, DP(12), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
    float ts = DP(34);
    FmRect tile = { row.x + DP(8), row.y + (rh - ts) * 0.5f, ts, ts };
    if (here) gfx_rrect_vgrad(tile, DP(10), col_mix(T.accent, PH_WHITE, 0.15f), T.accent);
    else gfx_rrect(tile, DP(10), T.surface2);
    icon_draw(s->icon ? s->icon : IC_IMAGE, rect_inset(tile, DP(8)), here ? T.on_accent : T.text2);
    float tx = tile.x + ts + DP(10), tw = row.x + row.w - tx - DP(8);
    bool key = missing_key(s);
    if (key) {
      float ks = DP(14);
      icon_draw(IC_KEY, FM_RECT(row.x + row.w - ks - DP(10), row.y + (rh - ks) * 0.5f, ks, ks), T.text3);
      tw -= ks + DP(6);
    }
    float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
    float ty = row.y + (rh - lh - ls) * 0.5f;
    font_draw_ellipsis(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, tx, ty, s->name, tw, here ? T.accent : T.text);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, ty + lh, key ? "Needs a free key" : s->about ? s->about : "", tw,
                       T.text3);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) set_source(i);
  }
  y += DP(6);
  rail_heading(in.x, &y, hh, "LIBRARY");
  struct { FmIcon ic; const char *label; } kLib[] = { { IC_HEART, "Favorites" }, { IC_ALBUM, "Albums" } };
  for (int i = 0; i < 2; i++) {
    FmRect row = { in.x, y, in.w, mh };
    y += mh;
    int f = ui_hit(ui_idn(base, 200 + (u32)i), row);
    bool here = i == 0 ? (S.page == PG_ALBUM && S.album == PALB_FAV)
                       : (S.page == PG_ALBUMS || (S.page == PG_ALBUM && S.album != PALB_FAV));
    if (here) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    icon_draw(i == 0 && palb_size(PALB_FAV) ? IC_HEART_FILL : kLib[i].ic,
              FM_RECT(row.x + DP(16), row.y + (mh - is) * 0.5f, is, is), here || i == 0 ? T.accent : T.text2);
    font_draw(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, row.x + DP(50), row.y + (mh - font_line_h(ui.m.font)) * 0.5f,
              kLib[i].label, -1, here ? T.accent : T.text);
    int cnt = i == 0 ? palb_size(PALB_FAV) : palb_count() - 1;
    if (cnt > 0) rail_count(row, cnt, false);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) {
      if (i == 0) ph_show_album(PALB_FAV);
      else ph_show_albums();
    }
  }
  y += DP(6);
  rail_heading(in.x, &y, hh, "MORE");
  struct { FmIcon ic; const char *label; } kMore[] = { { IC_DOWNLOAD, "Downloads" }, { IC_SETTINGS, "Settings" } };
  for (int i = 0; i < 2; i++) {
    FmRect row = { in.x, y, in.w, mh };
    y += mh;
    int f = ui_hit(ui_idn(base, 100 + (u32)i), row);
    if (i == 0 && S.tray_open) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    icon_draw(kMore[i].ic, FM_RECT(row.x + DP(16), row.y + (mh - is) * 0.5f, is, is), T.text2);
    font_draw(FONT_REGULAR, ui.m.font, row.x + DP(50), row.y + (mh - font_line_h(ui.m.font)) * 0.5f, kMore[i].label, -1,
              T.text);
    if (i == 0 && pdl_count() > 0) {
      int act = pdl_active();
      rail_count(row, act ? act : pdl_count(), act > 0);
    }
    if (f & UI_CLICK) {
      if (i == 0) tray_toggle();
      else ph_open_settings();
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&S.rail_scroll, in, content);
}

/* Sources, then Favorites and Albums, as one scrolling strip. */
static void draw_chips(FmRect r) {
  u32 base = ui_id("ph.chips");
  int ns = FM_MIN(psrc_count(), 16);
  int n = ns + 2;
  float ch = DP(ui.touch_mode ? 38 : 32), gap = DP(8), pad = DP(2), sep = DP(14);
  float fs = ui.m.font_small * 1.08f, is = DP(16);
  float w[18], total = pad;
  for (int i = 0; i < n; i++) {
    const char *l = i < ns ? psrc_at(i)->name : i == ns ? "Favorites" : "Albums";
    w[i] = font_width(FONT_BOLD, fs, l, -1) + is + DP(30);
    total += w[i] + gap + (i == ns ? sep : 0);
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
  int on_i = S.page == PG_SOURCE ? S.src : (S.page == PG_ALBUM && S.album == PALB_FAV) ? ns : ns + 1;
  if (S.chip_reveal && on_i < n) {
    float x = pad;
    for (int i = 0; i < on_i; i++) x += w[i] + gap + (i == ns ? sep : 0);
    if (on_i >= ns) x += sep;
    if (x - DP(24) < S.chip_x) S.chip_x = x - DP(24);
    if (x + w[on_i] + DP(24) > S.chip_x + r.w) S.chip_x = x + w[on_i] + DP(24) - r.w;
    S.chip_reveal = false;
  }
  S.chip_x = FM_CLAMP(S.chip_x, 0.0f, max);
  gfx_clip_push(r);
  float x = r.x + pad - S.chip_x;
  for (int i = 0; i < n; i++) {
    if (i == ns) {
      /* a thin rule between the sources and the library */
      gfx_rect(FM_RECT(x + sep * 0.5f - DP(4) - DP(0.5f), r.y + (r.h - ch * 0.6f) * 0.5f, DP(1), ch * 0.6f), T.divider);
      x += sep;
    }
    FmRect c = { x, r.y + (r.h - ch) * 0.5f, w[i], ch };
    x += w[i] + gap;
    if (!gfx_visible(c)) continue;
    bool on = i == on_i;
    int f = ui_hit(ui_idn(base, (u32)i + 1), c);
    if (on) gfx_rrect(c, ch * 0.5f, T.accent);
    else {
      gfx_rrect(c, ch * 0.5f, T.surface);
      gfx_rrect_line(c, ch * 0.5f, DP(1), T.border);
      if (f & UI_HOVER) gfx_rrect(c, ch * 0.5f, T.hover);
    }
    FmColor fg = on ? T.on_accent : T.text;
    FmIcon ic = i < ns ? (psrc_at(i)->icon ? psrc_at(i)->icon : IC_IMAGE) : i == ns ? IC_HEART : IC_ALBUM;
    const char *l = i < ns ? psrc_at(i)->name : i == ns ? "Favorites" : "Albums";
    icon_draw(ic, FM_RECT(c.x + DP(12), c.y + (ch - is) * 0.5f, is, is), on ? fg : (i >= ns ? T.accent : T.text2));
    font_draw(on ? FONT_BOLD : FONT_REGULAR, fs, c.x + DP(12) + is + DP(6), c.y + (ch - font_line_h(fs)) * 0.5f, l, -1, fg);
    if (f & UI_CLICK) {
      if (i < ns) set_source(i);
      else if (i == ns) ph_show_album(PALB_FAV);
      else ph_show_albums();
    }
  }
  gfx_clip_pop();
}

static void downloads_btn(FmRect b, u32 id) {
  title_nodrag(b);
  int act = pdl_active();
  if (ui_toggle_btn(id, b, IC_DOWNLOAD, S.tray_open, "Downloads (Ctrl+J)")) tray_toggle();
  if (act > 0) {
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, rr = FM_MIN(b.w, b.h) * 0.5f - DP(3);
    float fr = pdl_frac();
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
  u32 base = ui_id("ph.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(box_is_open() ? FM_RECT(0, 0, 0, 0) : r);   /* the lightbox owns the top edge */
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text, "Back to files")) photo_close();
  rect_cut_left(&in, DP(4));
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, PH_WHITE, 0.15f), T.accent);
    icon_draw(IC_ALBUM, rect_inset(m, DP(5)), T.on_accent);
    rect_cut_left(&in, DP(8));
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_SETTINGS, T.text2, "Online photo settings")) ph_open_settings();
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  downloads_btn(b, ui_idn(base, 3));
  const FmPsrc *s = cur_src();
  char sub[200], title[120];
  sub[0] = 0;
  const char *lead = "Online photos";
  if (S.page == PG_SOURCE) {
    fm_strlcpy(title, narrow && s ? s->name : "Online photos", sizeof title);
    lead = narrow || !s ? "Online photos" : s->name;
    if (S.search && !S.search_more)
      fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", lead, S.query[0] ? "Searching\xE2\x80\xA6" : "Loading\xE2\x80\xA6");
    else if (S.res.n > 0)
      fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s%d%s %s", lead, S.query[0] ? "" : "Curated  \xC2\xB7  ", S.res.n,
                  S.next[0] ? "+" : "", S.res.n == 1 ? "photo" : "photos");
    else fm_strlcpy(sub, lead, sizeof sub);
  } else {
    fm_strlcpy(title, narrow ? (S.page == PG_ALBUM ? palb_name(S.album) : "Albums") : "Online photos", sizeof title);
    if (S.page == PG_ALBUMS) fm_snprintf(sub, sizeof sub, "Albums  \xC2\xB7  on this device");
    else fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d %s", narrow ? "Album" : palb_name(S.album), palb_size(S.album),
                     palb_size(S.album) == 1 ? "photo" : "photos");
  }
  float fs = ui.m.font_title * (narrow ? 0.9f : 1.0f);
  float tl = font_line_h(fs), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, fs, in.x + DP(2), y, title, in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

/* ---- downloads tray ---------------------------------------------------------------------------------- */

static float dlg_content_w(float w_dp) {
  bool sheet = ui.portrait && ui.w < DP(560);
  float w = sheet ? ui.w : FM_MIN(DP(w_dp), ui.w - DP(24));
  return w - DP(40);
}

static void tray_thumb(FmRect r, const PhDl *d) {
  float fade = 1;
  const char *hdr = psrc_item_headers(&d->item);
  SDL_Texture *tex = d->item.thumb[0] ? othumb_get_ex(d->item.thumb, (int)r.w, 1.0f, hdr, &fade) : 0;
  if (!tex || fade < 1)
    gfx_rrect(r, DP(8), d->item.color ? col_mix(T.surface2, FM_HEX(d->item.color), 0.8f) : T.surface2);
  if (tex) {
    gfx_tex_rounded(tex, r, DP(8), col_alpha(PH_WHITE, fade));
    if (fade < 1) ui_animate();
  }
}

static void draw_tray(void) {
  int n = pdl_count();
  float rh = DP(ui.touch_mode ? 76 : 68);
  float bh = DP(ui.touch_mode ? 44 : 36);
  float list_h = n ? FM_MIN((float)n, 5.5f) * rh : DP(120);
  float cw = dlg_content_w(560);
  float h = font_line_h(ui.m.font_title) + DP(12) + list_h + DP(14) + bh;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("ph.tray"), "Downloads", 560, (h + DP(40)) / ui.scale + 2, &cancel);
  FmRect list = rect_cut_top(&c, list_h);
  char dir[FM_PATH_MAX];
  odl_dir(dir, sizeof dir);
  if (n == 0) {
    ui_label(FM_RECT(list.x, list.y + DP(30), list.w, font_line_h(ui.m.font)), "Photos you download appear here.",
             FONT_REGULAR, ui.m.font, T.text2, UI_CENTER);
    char t[FM_PATH_MAX + 16];
    fm_snprintf(t, sizeof t, "Saved to %s, each with a credits .txt", dir);
    ui_label(FM_RECT(list.x, list.y + DP(30) + font_line_h(ui.m.font) + DP(4), list.w, font_line_h(ui.m.font_small)), t,
             FONT_REGULAR, ui.m.font_small, T.text3, UI_CENTER);
  } else {
    u32 sid = ui_id("ph.tray.list");
    float content = (float)n * rh;
    ui_scroll(&S.tray_scroll, sid, list, content);
    gfx_clip_push(list);
    int remove = -1, cancel_i = -1, retry = -1;
    for (int i = 0; i < n; i++) {
      FmRect row = { list.x, list.y + (float)i * rh - S.tray_scroll.y, list.w, rh };
      if (!gfx_visible(row)) continue;
      PhDl *d = pdl_at(i);
      u32 id = ui_idn(sid, (u32)i + 1);
      float th = rh - DP(20);
      FmRect thumb = { row.x, row.y + DP(10), th, th };
      tray_thumb(thumb, d);
      float bs = FM_MIN(ui.m.hit, DP(40));
      FmRect act = { row.x + row.w, row.y + (rh - bs) * 0.5f, bs, bs };
      float x = thumb.x + th + DP(12);
      if (d->state == PD_RUNNING || d->state == PD_QUEUED) {
        act.x -= bs;
        if (ui_icon_btn(ui_idn(id, 1), act, IC_CLOSE, T.text2, "Cancel")) cancel_i = i;
      } else {
        act.x -= bs;
        if (ui_icon_btn(ui_idn(id, 2), act, IC_CLOSE, T.text3, "Remove from the list")) remove = i;
        if (d->state == PD_DONE) {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 3), act, IC_FOLDER_OPEN, T.text2, "Show in folder")) ph_reveal(d->path);
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 4), act, IC_IMAGE, T.accent, "Open")) {
            image_view_title(d->item.title[0] ? d->item.title : NULL);
            app_open(d->path, NULL, 0, 0);
            image_view_title(NULL);
          }
        } else {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 5), act, IC_REFRESH, T.text2, "Try again")) retry = i;
        }
      }
      float w = act.x - DP(8) - x;
      float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
      float y = row.y + DP(10);
      font_draw_ellipsis(FONT_REGULAR, ui.m.font, x, y, d->item.title[0] ? d->item.title : "Photo", w, T.text);
      y += lh + DP(2);
      char st[300];
      FmColor sc = T.text2;
      switch (d->state) {
        case PD_QUEUED: fm_strlcpy(st, "Waiting\xE2\x80\xA6", sizeof st); break;
        case PD_RUNNING:
          if (d->frac >= 0) fm_snprintf(st, sizeof st, "%d%%  \xC2\xB7  %s", (int)(d->frac * 100), ph_src_name(d->src));
          else fm_snprintf(st, sizeof st, "Downloading  \xC2\xB7  %s", ph_src_name(d->src));
          break;
        case PD_DONE: fm_snprintf(st, sizeof st, "Saved  \xC2\xB7  %s", fm_path_base(d->path)); sc = T.success; break;
        case PD_CANCELLED: fm_strlcpy(st, "Cancelled", sizeof st); sc = T.text3; break;
        default: fm_snprintf(st, sizeof st, "Failed: %s", d->err); sc = T.danger; break;
      }
      font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y, st, w, sc);
      y += ls + DP(6);
      if (d->state == PD_RUNNING) ui_progress(FM_RECT(x, y, w, DP(5)), d->frac);
      if (i < n - 1) ui_divider(x, row.x + row.w, row.y + rh - DP(0.5f));
    }
    gfx_clip_pop();
    ui_scrollbar(&S.tray_scroll, list, content);
    if (cancel_i >= 0) pdl_cancel(cancel_i);
    if (remove >= 0) pdl_remove(remove);
    if (retry >= 0) pdl_retry(retry);
  }
  rect_cut_top(&c, DP(14));
  FmRect br = rect_cut_top(&c, bh);
  u32 bid = ui_id("ph.tray.btn");
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
  for (int i = 0; i < n; i++) fin |= pdl_at(i)->state != PD_RUNNING && pdl_at(i)->state != PD_QUEUED;
  if (fin) {
    const char *lc = "Clear";
    float cwb = font_width(FONT_BOLD, ui.m.font, lc, -1) + DP(32);
    if (ui_button(ui_idn(bid, 2), FM_RECT(x, br.y, cwb, bh), IC_NONE, lc, UI_BTN_TEXT)) pdl_clear_finished();
  }
  ui_dialog_end();
  if (cancel || done) S.tray_open = false;
}

/* ---- public ------------------------------------------------------------------------------------------- */

void photo_init(bool readonly) {
  memset(&S, 0, sizeof S);
  S.readonly = readonly;
  ID_MENU = ui_id("ph.menu");
  wall_init(&S.res, "ph.wall.res");
  wall_init(&S.alb, "ph.wall.alb");
  precent_load(readonly);
  palb_load(readonly);
}

void photo_shutdown(void) {
  box_shutdown();
  if (S.search) ptask_free(S.search, true);
  S.search = NULL;
  ptask_shutdown();
  pdl_shutdown();
  palb_shutdown();
  wall_free(&S.res);
  wall_free(&S.alb);
}

bool photo_is_open(void) { return S.open; }
int photo_downloads_active(void) { return pdl_active(); }

void photo_render_reset(void) { box_reset(); }

void photo_open(const char *source) {
  int idx = -1;
  const char *key = source && source[0] ? source : conf.photo_source;
  for (int i = 0; i < psrc_count(); i++)
    if (key && strcmp(psrc_at(i)->key, key) == 0) idx = i;
  if (idx < 0 && source) {
    for (int i = 0; i < psrc_count(); i++)
      if (fm_stricmp(psrc_at(i)->name, source) == 0) idx = i;
  }
  if (idx >= 0 && idx != S.src) {
    if (S.search) { ptask_drop(S.search); S.search = NULL; }
    clear_results();
    S.src = idx;
  }
  S.open = true;
  S.chip_reveal = true;
  if (S.page == PG_SOURCE) browse_if_idle();
  if (!S.searched && S.res.n == 0 && !ui.touch_mode && !S.search) S.focus_search = true;
  ui_redraw();
}

void photo_close(void) {
  S.open = false;
  S.tray_open = false;
  box_close();
  ui_focus(0);
  /* the thumbnails go back; results, albums and the query stay for next time */
  othumb_reset();
  ui_redraw();
}

static void demo_after_results(void);

void photo_pump(void) {
  ptask_reap();
  take_search();
  box_pump();
  pdl_pump();
  palb_pump();
  if (S.demo_state[0] && !S.demo_done && S.searched && !S.search) {
    S.demo_done = true;
    demo_after_results();
  }
}

static void go_back(void) {
  if (wall_nsel(cur_wall()) > 0) { wall_select_all(cur_wall(), false); return; }
  if (ui.focus == ui_id("ph.search")) { ui_focus(0); return; }
  if (S.page == PG_ALBUM) { ph_show_albums(); return; }
  if (S.page == PG_ALBUMS) { ph_show_source(); return; }
  photo_close();
}

void photo_frame(FmRect area) {
  menu_results();
  palb_menu_results();
  box_menu_results();
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
    draw_chips(rect_cut_top(&r, DP(ui.touch_mode ? 50 : 44)));
  } else {
    rect_cut_top(&r, DP(2));
    FmRect rail = rect_cut_left(&r, DP(236));
    rect_cut_left(&r, gpx);
    draw_rail(rail);
  }
  draw_body(r);
  draw_top(top, narrow);
  if (!S.open) return;
  box_frame(area);
  if (!S.open) return;
  /* one dialog at a time: albums, then the tray */
  if (!palb_dialogs() && S.tray_open) draw_tray();
  if (!S.open) return;
  if (ui_input_ok() && !ui_menu_is_open() && !box_is_open()) {
    PhWall *w = cur_wall();
    if (ui_key(SDLK_f, KMOD_CTRL) || ui_key(SDLK_l, KMOD_CTRL)) { S.page = PG_SOURCE; S.focus_search = true; }
    if (!ui.focus && ui_key(SDLK_SLASH, 0)) { S.page = PG_SOURCE; S.focus_search = true; }
    if (ui_key(SDLK_F5, 0) || ui_key(SDLK_r, KMOD_CTRL)) {
      if (S.page == PG_SOURCE) search_start(false);
    }
    if (ui_key(SDLK_j, KMOD_CTRL)) tray_toggle();
    if (!ui.focus && ui_key(SDLK_a, KMOD_CTRL) && S.page != PG_ALBUMS) wall_select_all(w, true);
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) go_back();
    if (!ui.focus && psrc_count() > 1) {
      if (ui_key(SDLK_PAGEDOWN, KMOD_CTRL)) set_source((S.src + 1) % psrc_count());
      if (ui_key(SDLK_PAGEUP, KMOD_CTRL)) set_source((S.src + psrc_count() - 1) % psrc_count());
    }
  }
}

/* ---- demo ---------------------------------------------------------------------------------------------- */

static void env_conf(const char *env, char *dst, size_t cap) {
  const char *v = getenv(env);
  if (v && v[0]) fm_strlcpy(dst, v, cap);
}

static void demo_after_results(void) {
  char st[24];
  fm_strlcpy(st, S.demo_state, sizeof st);
  int k = 0;
  size_t len = strlen(st);
  while (len > 0 && st[len - 1] >= '0' && st[len - 1] <= '9') len--;
  if (st[len]) { k = atoi(st + len); st[len] = 0; }
  int n = S.res.n;
  k = FM_CLAMP(k, 0, FM_MAX(0, n - 1));
  if (n == 0) return;
  if (!strcmp(st, "lightbox") || !strcmp(st, "info")) {
    box_open(&S.res, k);
    if (!strcmp(st, "info")) box_set_info(true);
  } else if (!strcmp(st, "zoom")) {
    /* zoomed in 3x: the sharper picture is fetched once the full one is in */
    box_open(&S.res, k);
    box_demo_zoom(3.0f);
  } else if (!strcmp(st, "select")) {
    for (int i = 0; i < n && i < 9; i++) S.res.items[i].sel = i == 1 || i == 2 || i == 4 || i == 7;
  } else if (!strcmp(st, "albums") || !strcmp(st, "album") || !strcmp(st, "addalbum") || !strcmp(st, "favorites")) {
    palb_demo(S.res.items, n);
    if (!strcmp(st, "albums")) ph_show_albums();
    else if (!strcmp(st, "favorites")) ph_show_album(PALB_FAV);
    else if (!strcmp(st, "album")) ph_show_album(FM_MAX(1, palb_find_name("Wallpapers")));
    else palb_pick_open(&S.res.items[k], 1);
  } else if (!strcmp(st, "downloads")) {
    pdl_batch(S.res.items, FM_MIN(n, 4));
    S.tray_open = true;
  } else if (!strcmp(st, "e2e")) {
    /* the whole path at once: lightbox, favourite, an album, a download,
    ** then the image viewer once the full picture is in */
    PhItem *p = &S.res.items[k];
    box_open(&S.res, k);
    if (!palb_is_fav(p)) palb_fav_toggle(p);
    int a = palb_find_name("E2E test");
    if (a < 0) a = palb_create("E2E test");
    palb_add(a, p);
    pdl_start(p, false);
    box_view_full();
  } else if (!strcmp(st, "menu")) {
    open_menu(k, ui.w * 0.45f, ui.h * 0.35f);
  } else if (!strcmp(st, "scroll")) {
    S.res.scroll.y = 1e7f;
  }
  ui_redraw();
}

void photo_demo(const char *source, const char *query, const char *state) {
  /* keys for test runs */
  env_conf("MMCFM_PEXELS_KEY", conf.key_pexels, sizeof conf.key_pexels);
  env_conf("MMCFM_UNSPLASH_KEY", conf.key_unsplash, sizeof conf.key_unsplash);
  env_conf("MMCFM_PIXABAY_KEY", conf.key_pixabay, sizeof conf.key_pixabay);
  if (state && !strcmp(state, "downloads")) {
    char tmp[FM_PATH_MAX];
    if (plat_place(PLACE_TEMP, tmp, sizeof tmp))
      fm_path_join(conf.online_dl_dir, sizeof conf.online_dl_dir, tmp, "mmcfm-demo-photos");
  }
  if (state && !strcmp(state, "onboarding")) conf.photo_source[0] = 0;
  photo_open(source);
  fm_strlcpy(S.demo_state, state ? state : "", sizeof S.demo_state);
  if (query && query[0]) {
    fm_strlcpy(S.field, query, sizeof S.field);
    submit(query);
  }
  static const struct { const char *name; int st; const char *msg; } kForce[] = {
    { "nokey", E_NOKEY, "" },
    { "quota", E_QUOTA, "HTTP 429: Too Many Requests" },
    { "offline", E_OFFLINE, "WinHTTP: the server name or address could not be resolved" },
    { "empty", E_EMPTY, "" }, { "error", E_OTHER, "HTTP 500 from the server" },
  };
  for (int i = 0; state && i < FM_COUNT(kForce); i++)
    if (!strcmp(state, kForce[i].name)) {
      if (S.search) { ptask_drop(S.search); S.search = NULL; }
      clear_results();
      if (!S.query[0]) fm_strlcpy(S.query, query ? query : "lighthouse", sizeof S.query);
      S.searched = true;
      S.estate = kForce[i].st;
      fm_strlcpy(S.error, kForce[i].msg, sizeof S.error);
      S.demo_done = true;
    }
  if (state && !strcmp(state, "settings")) ph_open_settings();
  if (state && !strcmp(state, "albums") && (!query || !query[0])) ph_show_albums();
  ui_focus(0);
  S.focus_search = false;
}
