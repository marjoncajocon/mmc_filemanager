/* faudio_online.c -- the "Online audio" view: sources, search, browse,
** podcasts and albums, the library, states and downloads.
**
** Design decisions:
**   - A sibling of the online videos and photos views (fonline.c,
**     fphoto.c) and built the same way: a full-window screen over the
**     panels, a source rail on wide windows and a chip strip on narrow
**     ones, one rounded body card, a search pill with recent searches,
**     and states the user can act on (onboarding, a missing key with the
**     link that hands out a free one, offline, no results, errors with the
**     site's own message).
**   - An empty search box is not an empty page: sources that can browse
**     show their categories (genres, top stations, trending ...) as chips
**     and the items of the one picked, so radio plays with one tap.
**   - Pages share the body: a source's results, a podcast or album opened
**     (its header scrolls with the episodes, which page in as the list
**     nears its end), and the library's Favorites, Recently played and
**     Subscriptions. Each keeps its own list and scroll position, so going
**     into a podcast and back lands where the user was.
**   - Playing goes through the music player (aplay_list ->
**     audio_play_entries; the item menu's Play next / Add to queue ->
**     audio_queue_add): the view never holds audio state of its own; it
**     asks the player once per frame what is playing to mark that row, and
**     the mini player sits at the bottom like on the other screens.
**   - Nothing runs while idle: searches, categories, episodes and
**     downloads are AoTasks polled in aonline_pump; the only timed redraws
**     are the loading shimmer, artwork fade-ins and the playing bars, each
**     through a wake timer and only while they move.
*/
#include "faudio_online_int.h"
#include "fview_int.h"
#include "ftitle.h"

enum { AP_SOURCE, AP_DETAIL, AP_FAV, AP_RECENT, AP_SUBS };
enum { E_NONE, E_NOKEY, E_QUOTA, E_OFFLINE, E_EMPTY, E_OTHER };
enum { MI_PLAY = 1, MI_OPEN, MI_FAV, MI_SUB, MI_DOWNLOAD, MI_BROWSER, MI_COPY, MI_REMOVE, MI_QNEXT, MI_QADD, MI_PLADD };

static FmAonlineHooks g_hooks;

/* A page of items from a task: results, a container's children. */
typedef struct Feed {
  AoList list;
  char next[256];
  AoTask *task;
  bool more;                  /* the running task is a next page */
  bool more_err;
  bool searched;
  int estate;
  char error[256];
} Feed;

static struct {
  bool open;
  bool readonly;
  int src;
  int page;
  char field[512];            /* the search box */
  char query[512];            /* what the results are for ("" = browse) */
  Feed res;
  /* browse categories of the current source */
  AoTask *cats_task;
  FmAsrcCat cats[AO_MAX_CATS];
  int ncats, cat;
  char cats_for[16];
  float cat_x, cat_x0;
  bool cat_reveal;
  /* a podcast or album opened */
  FmAsrcItem det;
  Feed det_feed;
  int det_back;               /* the page to go back to */
  /* the library pages */
  AoList lib;
  int lib_page;
  u32 lib_ver;
  /* chrome */
  float chip_x, chip_x0;
  bool chip_reveal;
  FmScroll rail_scroll;
  bool focus_search;
  bool tray_open;
  FmScroll tray_scroll;
  bool settings_focus;
  /* the player, once per frame */
  char play_key[192];
  int play_state;
  /* --demo-audio-online */
  char demo_state[24];
  bool demo_done;
} S;

static u32 ID_MENU;
static SDL_atomic_t g_tick_cats;
static FmAsrcItem g_menu_item;
static bool g_menu_live;

/* ---- hooks and small helpers ---------------------------------------------------------- */

void aonline_set_hooks(const FmAonlineHooks *h) {
  if (h) g_hooks = *h;
  else memset(&g_hooks, 0, sizeof g_hooks);
}

void ao_conf_dirty(void) { if (g_hooks.conf_dirty) g_hooks.conf_dirty(); }

void ao_open_settings(void) {
  S.settings_focus = true;
  if (g_hooks.open_settings) g_hooks.open_settings();
}

void ao_reveal(const char *path) {
  if (g_hooks.reveal) { g_hooks.reveal(path); return; }
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, path, sizeof dir);
  fm_path_parent(dir);
  if (!plat_open_external(dir)) ui_toast("Saved in %s", dir);
}

bool aonline_settings_focus(void) {
  bool f = S.settings_focus;
  S.settings_focus = false;
  return f;
}

void ao_open_url(const char *url) {
  if (!url || !url[0]) { ui_toast("No web page for this one"); return; }
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

static const FmAsrc *cur_src(void) { return asrc_count() > 0 ? asrc_at(FM_CLAMP(S.src, 0, asrc_count() - 1)) : NULL; }

static bool can_search(const FmAsrc *s) { return s && (s->flags & ASRC_SEARCH) && s->search; }
static bool can_browse(const FmAsrc *s) { return s && (s->flags & ASRC_BROWSE) && s->browse; }

static char *key_field(const FmAsrc *s, size_t *cap) {
  if (!s) return NULL;
  if (!strcmp(s->key, "jamendo")) { *cap = sizeof conf.key_jamendo; return conf.key_jamendo; }
  if (!strcmp(s->key, "freesound")) { *cap = sizeof conf.key_freesound; return conf.key_freesound; }
  return NULL;
}

static const char *key_url(const FmAsrc *s) {
  if (s && !strcmp(s->key, "jamendo")) return "https://devportal.jamendo.com/";
  if (s && !strcmp(s->key, "freesound")) return "https://freesound.org/apiv2/apply";
  return "";
}

static bool missing_key(const FmAsrc *s) {
  if (!s || !(s->flags & ASRC_NEEDKEY)) return false;
  size_t cap;
  char *k = key_field(s, &cap);
  return !k || !k[0];
}

int ao_text_lines(int face, float size, float x, float y, float w, const char *s, int lines, FmColor c, bool draw) {
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

bool ao_chip(u32 id, FmRect c, FmIcon ic, const char *label, bool on) {
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

float ao_chip_w(FmIcon ic, const char *label, float max) {
  float fs = ui.m.font_small * 1.06f;
  return FM_MIN(max, font_width(FONT_BOLD, fs, label, -1) + DP(28) + (ic ? DP(21) : 0));
}

static int classify(const char *e, FmErr err) {
  if (fm_stristr(e, "rate limit") || fm_stristr(e, "too many requests") || fm_stristr(e, "429") ||
      fm_stristr(e, "quota"))
    return E_QUOTA;
  if (fm_stristr(e, "api key") || fm_stristr(e, "client_id") || fm_stristr(e, "client id") ||
      fm_stristr(e, "needs a key") || fm_stristr(e, "free key") || fm_stristr(e, "invalid key") ||
      fm_stristr(e, "401") || fm_stristr(e, "unauthorized") || fm_stristr(e, "token"))
    return E_NOKEY;
  if (fm_stristr(e, "no results") || fm_stristr(e, "nothing found")) return E_EMPTY;
  if (fm_stristr(e, "offline") || fm_stristr(e, "internet") || fm_stristr(e, "connect") ||
      fm_stristr(e, "resolve") || fm_stristr(e, "timed out") || fm_stristr(e, "timeout") ||
      fm_stristr(e, "unreachable") || fm_stristr(e, "network") || err == FM_ERR_IO)
    return E_OFFLINE;
  return E_OTHER;
}

/* ---- feeds: results and children ------------------------------------------------------- */

static void feed_clear(Feed *f) {
  if (f->task) { atask_drop(f->task); f->task = NULL; }
  aol_clear(&f->list);
  f->next[0] = 0;
  f->more = f->more_err = f->searched = false;
  f->estate = E_NONE;
  f->error[0] = 0;
}

/* Takes a finished page into the feed; true when it did. */
static bool feed_take(Feed *f) {
  AoTask *t = f->task;
  if (!t || !atask_done(t)) return false;
  f->task = NULL;
  if (t->err == FM_OK) {
    int start = f->more ? 0 : f->list.n;
    for (int i = 0; i < t->page.count; i++) {
      const FmAsrcItem *it = &t->page.items[i];
      /* a next page may repeat items of this one */
      bool dup = false;
      for (int k = start; k < f->list.n && !dup; k++) dup = aitem_same(&f->list.items[k], it);
      if (!dup) *aol_push(&f->list) = *it;
    }
    fm_strlcpy(f->next, t->page.next, sizeof f->next);
    f->searched = true;
    f->estate = f->list.n == 0 ? E_EMPTY : E_NONE;
  } else if (t->err != FM_ERR_CANCEL) {
    if (f->more) {
      f->more_err = true;
      fm_strlcpy(f->error, t->page.error, sizeof f->error);
    } else {
      f->searched = true;
      fm_strlcpy(f->error, t->page.error[0] ? t->page.error : t->errtext, sizeof f->error);
      f->estate = classify(f->error, t->err);
    }
  }
  atask_free(t, false);
  ui_redraw();
  return true;
}

static bool feed_loading_first(const Feed *f) { return f->task && !f->more; }

static void search_start(bool more) {
  const FmAsrc *s = cur_src();
  Feed *f = &S.res;
  if (!s) return;
  bool browse = !S.query[0];
  if (browse && !can_browse(s)) return;
  if (!browse && !can_search(s)) return;
  if (more && !f->next[0]) return;
  if (!more) feed_clear(f);
  else if (f->task) return;
  if (missing_key(s)) {
    f->searched = true;
    f->estate = E_NOKEY;
    ui_redraw();
    return;
  }
  /* browsing waits for the categories when the source has them */
  if (browse && s->categories && strcmp(S.cats_for, s->key) != 0) {
    if (!S.cats_task) {
      S.cats_task = atask_new(AT_CATS, s);
      atask_run(S.cats_task);
    }
    ui_redraw();
    return;
  }
  AoTask *t = atask_new(browse ? AT_BROWSE : AT_SEARCH, s);
  fm_strlcpy(t->query, S.query, sizeof t->query);
  if (browse && S.ncats > 0) fm_strlcpy(t->cat, S.cats[FM_CLAMP(S.cat, 0, S.ncats - 1)].id, sizeof t->cat);
  if (more) fm_strlcpy(t->token, f->next, sizeof t->token);
  f->task = t;
  f->more = more;
  f->more_err = false;
  atask_run(t);
  othumb_forget_queue();
  ui_redraw();
}

static void take_cats(void) {
  AoTask *t = S.cats_task;
  if (!t || !atask_done(t)) return;
  S.cats_task = NULL;
  const FmAsrc *s = cur_src();
  if (s && t->src == s) {
    S.ncats = t->ncats;
    memcpy(S.cats, t->cats, sizeof S.cats[0] * (size_t)S.ncats);
    fm_strlcpy(S.cats_for, s->key, sizeof S.cats_for);
    S.cat = 0;
    S.cat_x = 0;
    if (!S.query[0] && S.page == AP_SOURCE) search_start(false);
  }
  atask_free(t, false);
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
  arecent_add(b);
  ui_focus(0);
  S.page = AP_SOURCE;
  search_start(false);
}

/* The browse page of a source that has one, when nothing is searched. */
static void browse_if_idle(void) {
  const FmAsrc *s = cur_src();
  Feed *f = &S.res;
  if (!S.query[0] && !f->task && !f->searched && f->list.n == 0 && can_browse(s)) search_start(false);
}

static void set_cat(int i) {
  if (i < 0 || i >= S.ncats || (i == S.cat && (S.res.list.n || S.res.task))) return;
  S.cat = i;
  S.cat_reveal = true;
  search_start(false);
}

/* ---- pages ------------------------------------------------------------------------------- */

static void children_start(bool more) {
  Feed *f = &S.det_feed;
  const FmAsrc *s = asrc_find(S.det.source);
  if (more && (!f->next[0] || f->task)) return;
  if (!more) feed_clear(f);
  if (!s || !s->children) {
    f->searched = true;
    f->estate = E_OTHER;
    fm_strlcpy(f->error, "This source cannot list what is inside", sizeof f->error);
    return;
  }
  AoTask *t = atask_new(AT_CHILDREN, s);
  t->item = S.det;
  if (more) fm_strlcpy(t->token, f->next, sizeof t->token);
  f->task = t;
  f->more = more;
  f->more_err = false;
  atask_run(t);
  ui_redraw();
}

static void open_detail(const FmAsrcItem *it) {
  if (!aitem_container(it)) return;
  bool same = S.page == AP_DETAIL && aitem_same(&S.det, it);
  if (S.page != AP_DETAIL) S.det_back = S.page;
  S.page = AP_DETAIL;
  if (same) return;
  S.det = *it;
  children_start(false);
  ui_focus(0);
  ui_redraw();
}

static void show_lib(int page) {
  S.page = page;
  S.lib_page = -1;                 /* rebuilt on the next frame */
  S.chip_reveal = true;
  ui_focus(0);
  ui_redraw();
}

static void show_source(void) {
  S.page = AP_SOURCE;
  S.chip_reveal = true;
  browse_if_idle();
  ui_redraw();
}

static void set_source(int i) {
  if (i < 0 || i >= asrc_count()) return;
  if (i == S.src && S.page == AP_SOURCE) return;
  bool changed = i != S.src;
  S.page = AP_SOURCE;
  S.chip_reveal = true;
  if (!changed) { browse_if_idle(); ui_redraw(); return; }
  S.src = i;
  const FmAsrc *s = cur_src();
  fm_strlcpy(conf.audio_source, s->key, sizeof conf.audio_source);
  ao_conf_dirty();
  feed_clear(&S.res);
  if (S.cats_task) { atask_drop(S.cats_task); S.cats_task = NULL; }
  S.ncats = 0;
  S.cats_for[0] = 0;
  /* the same words on the new site, or its browse page */
  if (S.query[0] && can_search(s)) search_start(false);
  else {
    S.query[0] = 0;
    S.field[0] = 0;
    browse_if_idle();
  }
  ui_redraw();
}

static void go_back(void) {
  if (ui.focus == ui_id("ao.search")) { ui_focus(0); return; }
  if (S.page == AP_DETAIL) {
    S.page = S.det_back == AP_DETAIL ? AP_SOURCE : S.det_back;
    S.lib_page = -1;
    ui_redraw();
    return;
  }
  if (S.page != AP_SOURCE) { show_source(); return; }
  if (S.query[0] && can_browse(cur_src())) {
    /* out of a search, back to the browse page */
    S.query[0] = 0;
    S.field[0] = 0;
    feed_clear(&S.res);
    browse_if_idle();
    return;
  }
  aonline_close();
}

/* The library page's list follows the library file. */
static void sync_lib(void) {
  if (S.page != AP_FAV && S.page != AP_RECENT && S.page != AP_SUBS) return;
  if (S.lib_page == S.page && S.lib_ver == alib_version()) return;
  bool same = S.lib_page == S.page;
  float y = S.lib.scroll.y;
  int lib = S.page == AP_FAV ? ALIB_FAV : S.page == AP_RECENT ? ALIB_RECENT : ALIB_SUBS;
  aol_clear(&S.lib);
  for (int i = 0; i < alib_count(lib); i++) {
    FmAsrcItem *it = aol_push(&S.lib);
    if (!alib_get(lib, i, it)) S.lib.n--;
  }
  if (same) S.lib.scroll.y = y;
  S.lib_page = S.page;
  S.lib_ver = alib_version();
}

/* ---- the item menu -------------------------------------------------------------------------- */

static void open_menu(const FmAsrcItem *it, float x, float y) {
  g_menu_item = *it;
  g_menu_live = true;
  bool cont = aitem_container(it);
  FmMenuItem m[14];
  int n = 0;
  memset(m, 0, sizeof m);
  if (cont) { m[n].id = MI_OPEN; m[n].icon = IC_LIST; m[n++].label = it->kind == AITEM_PODCAST ? "Episodes" : "Tracks"; }
  else { m[n].id = MI_PLAY; m[n].icon = IC_PLAY; m[n++].label = "Play"; }
  if (aitem_playable(it)) {
    /* the music player's queue and playlists keep it (re-resolved when it plays) */
    m[n].id = MI_QNEXT; m[n].icon = IC_PLAY_NEXT; m[n++].label = "Play next";
    m[n].id = MI_QADD; m[n].icon = IC_QUEUE; m[n++].label = "Add to queue";
    m[n].id = MI_PLADD; m[n].icon = IC_PLAYLIST_ADD; m[n++].label = "Add to playlist\xE2\x80\xA6";
    m[n++].flags = UI_MI_SEP;
  }
  if (it->kind == AITEM_PODCAST) {
    bool sub = alib_has(ALIB_SUBS, it);
    m[n].id = MI_SUB; m[n].icon = IC_RSS; m[n++].label = sub ? "Unsubscribe" : "Subscribe";
  } else {
    bool fav = alib_has(ALIB_FAV, it);
    m[n].id = MI_FAV; m[n].icon = fav ? IC_HEART_FILL : IC_HEART; m[n++].label = fav ? "Remove from Favorites" : "Favorite";
  }
  if (it->kind == AITEM_TRACK) { m[n].id = MI_DOWNLOAD; m[n].icon = IC_DOWNLOAD; m[n++].label = "Download"; }
  m[n++].flags = UI_MI_SEP;
  m[n].id = MI_BROWSER; m[n].icon = IC_OPEN_WITH; m[n++].label = "Open in browser";
  m[n].id = MI_COPY; m[n].icon = IC_LINK; m[n++].label = "Copy link";
  if (S.page == AP_RECENT) {
    m[n].id = MI_REMOVE; m[n].icon = IC_DELETE; m[n].label = "Remove from Recently played"; m[n++].flags = UI_MI_DANGER;
  }
  ui_menu_open(ID_MENU, x, y, m, n);
}

static AoList *page_list(void) {
  if (S.page == AP_DETAIL) return &S.det_feed.list;
  if (S.page == AP_SOURCE) return &S.res.list;
  return &S.lib;
}

static void play_in(AoList *l, int i) {
  if (i < 0 || i >= l->n) return;
  if (aitem_container(&l->items[i])) { open_detail(&l->items[i]); return; }
  aplay_list(l->items, l->n, i);
}

static void menu_results(void) {
  int r = ui_menu_result(ID_MENU);
  if (r < 0 || !g_menu_live) return;
  g_menu_live = false;
  FmAsrcItem *it = &g_menu_item;
  switch (r) {
    case MI_PLAY: {
      AoList *l = page_list();
      int i = aol_find(l, it->source, it->id);
      if (i >= 0) play_in(l, i);
      else aplay_list(it, 1, 0);
      break;
    }
    case MI_OPEN: open_detail(it); break;
    case MI_FAV: alib_toggle(ALIB_FAV, it); break;
    case MI_SUB: alib_toggle(ALIB_SUBS, it); break;
    case MI_DOWNLOAD: adl_start(it); break;
    case MI_BROWSER: ao_open_url(it->page); break;
    case MI_COPY: {
      const char *l = it->page[0] ? it->page : it->url;
      if (!l[0]) ui_toast("No link for this one");
      else { ui_clipboard_set(l); ui_toast("Link copied"); }
      break;
    }
    case MI_REMOVE: alib_remove(ALIB_RECENT, it); break;
    case MI_QNEXT: aplay_queue(it, true); break;
    case MI_QADD: aplay_queue(it, false); break;
    case MI_PLADD: aplay_pick_playlist(it, ui.mx, ui.my); break;
    default: break;
  }
  ui_redraw();
}

/* What a list asked for this frame. */
static void list_actions(AoList *l, const AoListRes *r) {
  if (r->toggle) audio_show_player();
  if (r->play >= 0) play_in(l, r->play);
  if (r->open >= 0 && r->open < l->n) open_detail(&l->items[r->open]);
  if (r->fav >= 0 && r->fav < l->n) {
    const FmAsrcItem *it = &l->items[r->fav];
    alib_toggle(it->kind == AITEM_PODCAST ? ALIB_SUBS : ALIB_FAV, it);
  }
  if (r->dl >= 0 && r->dl < l->n) adl_start(&l->items[r->dl]);
  if (r->menu >= 0 && r->menu < l->n) open_menu(&l->items[r->menu], r->menu_x, r->menu_y);
}

/* ---- generic states -------------------------------------------------------------------------- */

static int state_view(FmRect body, FmIcon ic, FmColor icol, const char *title, const char *sub, const char *detail,
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
      if (ui_button(ui_idn(ui_id("ao.state.btn"), (u32)i), b, icons ? icons[i] : IC_NONE, labels[i],
                    i == 0 ? UI_BTN_FILLED : UI_BTN_TONAL))
        res = i;
      x += bw[i] + DP(10);
    }
  }
  return res;
}

/* The card telling where a keyed source's free key comes from. */
static float key_card(FmRect r, const FmAsrc *s, bool draw) {
  float pad = DP(16), is = DP(36);
  float tx = pad + is + DP(14), tw = r.w - tx - pad;
  char text[300];
  fm_snprintf(text, sizeof text, "%s gives out a free %s in a minute. Paste it in Settings; Radio, Audius, the "
                                 "Internet Archive and Podcasts work without one meanwhile.",
              s->name, !strcmp(s->key, "jamendo") ? "client ID" : "API key");
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
  u32 id = ui_id("ao.keycard");
  if (ui_button(ui_idn(id, 0), FM_RECT(x, y, FM_MIN(b0, tw), bh), IC_SETTINGS, l0, UI_BTN_FILLED)) ao_open_settings();
  if (two_rows) y += bh + DP(8);
  else x += b0 + DP(8);
  if (ui_button(ui_idn(id, 1), FM_RECT(x, y, FM_MIN(b1, tw), bh), IC_OPEN_WITH, l1, UI_BTN_TONAL)) ao_open_url(key_url(s));
  return h;
}

static void error_state(FmRect body, const FmAsrc *s, Feed *f, bool detail) {
  char title[200], sub[400];
  const char *l[3];
  FmIcon ic[3];
  int n = 0, r;
  switch (f->estate) {
    case E_NOKEY:
      fm_snprintf(title, sizeof title, "Add a free %s key in Settings", s->name);
      fm_snprintf(sub, sizeof sub, missing_key(s) ? "%s asks for a free key. It takes a minute to get one; Radio, "
                                                    "Audius and Podcasts work without one meanwhile."
                                                  : "%s did not accept the key. Check it in Settings, or get a new "
                                                    "one.", s->name);
      l[n] = "Open Settings"; ic[n++] = IC_SETTINGS;
      l[n] = "Get a free key"; ic[n++] = IC_OPEN_WITH;
      r = state_view(body, IC_KEY, T.accent, title, sub, missing_key(s) ? NULL : f->error, l, ic, n);
      if (r == 0) ao_open_settings();
      if (r == 1) ao_open_url(key_url(s));
      return;
    case E_QUOTA:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      r = state_view(body, IC_WARN, T.warn, "Slow down a little",
                     "The site limits how many requests come in a minute. Try again in a moment; the other "
                     "sources keep working meanwhile.", f->error, l, ic, n);
      break;
    case E_OFFLINE:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      if (alib_count(ALIB_FAV) > 0) { l[n] = "Favorites"; ic[n++] = IC_HEART; }
      r = state_view(body, IC_NETWORK, T.text2, "You're offline",
                     "Check the internet connection, then try again. Downloaded tracks still play from the "
                     "Downloads list.", f->error, l, ic, n);
      if (r == 1) { show_lib(AP_FAV); return; }
      break;
    case E_EMPTY:
      if (detail) {
        state_view(body, IC_MUSIC, T.accent, "Nothing to play here",
                       "This one lists no playable episodes or tracks right now.", NULL, NULL, NULL, 0);
        return;
      }
      fm_snprintf(title, sizeof title, "No results for \xE2\x80\x9C%s\xE2\x80\x9D", S.query);
      l[n] = "New search"; ic[n++] = IC_SEARCH;
      r = state_view(body, IC_SEARCH, T.accent, S.query[0] ? title : "Nothing here right now",
                     "Try other words, another category, or another source.", NULL, l, ic, n);
      if (r == 0) {
        S.field[0] = 0;
        S.query[0] = 0;
        feed_clear(&S.res);
        S.focus_search = true;
        browse_if_idle();
      }
      return;
    default:
      l[n] = "Try again"; ic[n++] = IC_REFRESH;
      r = state_view(body, IC_WARN, T.danger, "Something went wrong",
                     detail ? "The list did not load this time." : "The search did not work this time.", f->error, l,
                     ic, n);
      break;
  }
  if (r == 0) {
    if (detail) children_start(false);
    else search_start(false);
  }
}

static const char *const kTry[] = { "Jazz", "Lo-fi", "News", "Classical", "Ambient", "Comedy" };

static void onboarding(FmRect body, const FmAsrc *s) {
  bool nokey = missing_key(s);
  float cw = FM_MIN(body.w - DP(32), DP(560));
  float x = body.x + (body.w - cw) * 0.5f;
  float is = DP(64), tlh = font_line_h(ui.m.font_title * 1.2f);
  float ab = s->about ? font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw, s->about, T.text2, false) : 0;
  bool tries = !nokey && can_search(s);
  const char *hint = "Radio and Audius play at once, no account or key needed.";
  float hh = font_line_h(ui.m.font_small);
  float h = is + DP(18) + tlh + DP(6) + ab + DP(10) + hh + (tries ? DP(56) : 0);
  if (nokey) h += DP(20) + key_card(FM_RECT(x, 0, cw, 0), s, false);
  float y = body.y + FM_MAX(DP(16), (body.h - h) * 0.4f);
  float cx = body.x + body.w * 0.5f;
  FmRect tile = { cx - is * 0.5f, y, is, is };
  gfx_shadow(tile, DP(18), DP(14), col_alpha(T.accent, 0.35f));
  gfx_rrect_vgrad(tile, DP(18), col_mix(T.accent, AO_WHITE, 0.18f), T.accent);
  icon_draw(ao_src_icon(s), rect_inset(tile, DP(15)), T.on_accent);
  y += is + DP(18);
  char title[96];
  fm_snprintf(title, sizeof title, can_search(s) ? "Search %s" : "%s", s->name);
  ui_label(FM_RECT(body.x + DP(8), y, body.w - DP(16), tlh), title, FONT_BOLD, ui.m.font_title * 1.2f, T.text, UI_CENTER);
  y += tlh + DP(6);
  if (s->about) {
    float w = FM_MIN(cw, font_width(FONT_REGULAR, ui.m.font, s->about, -1) + DP(2));
    font_draw_wrap(FONT_REGULAR, ui.m.font, cx - w * 0.5f, y, w, s->about, T.text2, true);
    y += ab;
  }
  y += DP(10);
  ui_label(FM_RECT(body.x + DP(8), y, body.w - DP(16), hh), hint, FONT_REGULAR, ui.m.font_small, T.text3, UI_CENTER);
  y += hh;
  if (tries) {
    y += DP(20);
    float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8), total = 0;
    int n = 0;
    float ws[FM_COUNT(kTry)];
    for (int i = 0; i < FM_COUNT(kTry); i++) {
      ws[i] = ao_chip_w(IC_SEARCH, kTry[i], DP(200));
      if (total + ws[i] + (n ? gap : 0) > body.w - DP(32)) break;
      total += ws[i] + (n ? gap : 0);
      n++;
    }
    float xx = cx - total * 0.5f;
    for (int i = 0; i < n; i++) {
      if (ao_chip(ui_idn(ui_id("ao.try"), (u32)i), FM_RECT(xx, y, ws[i], ch), IC_SEARCH, kTry[i], false)) {
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

/* ---- search field, recent searches and categories ------------------------------------------- */

static float pill_h(void) { return DP(ui.touch_mode ? 52 : 46); }

static void draw_search(FmRect pill, const FmAsrc *s) {
  u32 id = ui_id("ao.search");
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
  bool clicked_go;
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
  fm_snprintf(hint, sizeof hint, !strcmp(s->key, "radio") ? "Search stations, genres, countries" : "Search %s",
              s->name);
  gfx_clip_push(clip);
  int res = ui_textfield(id, tf, S.field, sizeof S.field, hint, 0);
  gfx_clip_pop();
  if (res & UI_TF_CHANGED) ui_redraw();
  if ((res & UI_TF_SUBMIT) || clicked_go) {
    if (S.field[0]) submit(S.field);
    else if (S.query[0]) {
      /* an emptied box goes back to the browse page */
      S.query[0] = 0;
      feed_clear(&S.res);
      browse_if_idle();
    } else S.focus_search = true;
  }
}

static float draw_recents(FmRect r, bool draw) {
  int n = arecent_count();
  if (n == 0) return 0;
  float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8);
  float x = r.x, y = r.y;
  int rows = 1;
  u32 base = ui_id("ao.recent");
  for (int i = 0; i <= n; i++) {
    bool clear = i == n;
    const char *l = clear ? "Clear" : arecent_at(i);
    float w = ao_chip_w(clear ? IC_DELETE : IC_HISTORY, l, DP(240));
    if (x + w > r.x + r.w && x > r.x) {
      if (rows == 2) break;
      rows++;
      x = r.x;
      y += ch + gap;
    }
    if (draw) {
      FmRect c = { x, y, w, ch };
      if (ao_chip(ui_idn(base, (u32)i + 1), c, clear ? IC_DELETE : IC_HISTORY, l, false)) {
        if (clear) arecent_clear();
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

static FmIcon cat_icon(const char *name) {
  if (fm_stristr(name, "trend") || fm_stristr(name, "top") || fm_stristr(name, "popular")) return IC_TRENDING;
  if (fm_stristr(name, "near") || fm_stristr(name, "countr") || fm_stristr(name, "local")) return IC_NETWORK;
  if (fm_stristr(name, "new") || fm_stristr(name, "recent")) return IC_HISTORY;
  return IC_NONE;
}

/* A horizontal strip of chips that scrolls by drag and wheel. */
static void draw_cats(FmRect r) {
  u32 base = ui_id("ao.cats");
  float ch = DP(ui.touch_mode ? 36 : 32), gap = DP(8);
  float w[AO_MAX_CATS], total = 0;
  for (int i = 0; i < S.ncats; i++) {
    w[i] = ao_chip_w(cat_icon(S.cats[i].name), S.cats[i].name, DP(220));
    total += w[i] + gap;
  }
  total -= gap;
  float max = FM_MAX(0.0f, total - r.w);
  bool dragging = ui.drag_owner == base;
  if (!dragging && ui.down && ui.drag_owner == 0 && ui.moved && ui_input_ok() && rect_has(r, ui.press_x, ui.press_y) &&
      fabsf(ui.mx - ui.press_x) > fabsf(ui.my - ui.press_y)) {
    ui.drag_owner = base;
    S.cat_x0 = S.cat_x;
    dragging = true;
  }
  if (dragging && ui.down) S.cat_x = S.cat_x0 - (ui.mx - ui.press_x);
  if (ui_input_ok() && rect_has(r, ui.mx, ui.my) && (ui.wheel != 0 || ui.wheel_x != 0)) {
    S.cat_x -= (ui.wheel_x != 0 ? -ui.wheel_x : ui.wheel) * DP(60);
    ui.wheel = ui.wheel_x = 0;
  }
  if (S.cat_reveal && S.cat < S.ncats) {
    float x = 0;
    for (int i = 0; i < S.cat; i++) x += w[i] + gap;
    if (x - DP(24) < S.cat_x) S.cat_x = x - DP(24);
    if (x + w[S.cat] + DP(24) > S.cat_x + r.w) S.cat_x = x + w[S.cat] + DP(24) - r.w;
    S.cat_reveal = false;
  }
  S.cat_x = FM_CLAMP(S.cat_x, 0.0f, max);
  gfx_clip_push(r);
  float x = r.x - S.cat_x;
  for (int i = 0; i < S.ncats; i++) {
    FmRect c = { x, r.y + (r.h - ch) * 0.5f, w[i], ch };
    x += w[i] + gap;
    if (!gfx_visible(c)) continue;
    if (ao_chip(ui_idn(base, (u32)i + 1), c, cat_icon(S.cats[i].name), S.cats[i].name, i == S.cat) && !dragging)
      set_cat(i);
  }
  gfx_clip_pop();
  /* soft edges where the strip continues */
  if (S.cat_x > 1) gfx_rect(FM_RECT(r.x, r.y, DP(1), r.h), T.divider);
}

/* ---- the body ---------------------------------------------------------------------------------- */

static void feed_footer(FmRect view, const AoListRes *wr, Feed *f, bool detail) {
  if (f->list.n == 0 || (f->task && f->more)) return;
  FmRect fr = { view.x, wr->footer_y, view.w, DP(60) };
  gfx_clip_push(view);
  if (gfx_visible(fr)) {
    if (f->more_err) {
      const char *l = "Couldn't load more. Try again";
      float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(56);
      if (ui_button(ui_id("ao.more.retry"), rect_center(fr, bw, DP(ui.touch_mode ? 44 : 36)), IC_REFRESH, l,
                    UI_BTN_TONAL)) {
        if (detail) children_start(true);
        else search_start(true);
      }
    } else if (!f->next[0]) {
      char t[64];
      const char *what = detail ? (S.det.kind == AITEM_PODCAST ? "episodes" : "tracks")
                                : ao_layout_for(&f->list) == AL_STATIONS ? "stations" : "results";
      fm_snprintf(t, sizeof t, "%d %s", f->list.n, what);
      font_draw_center(FONT_REGULAR, ui.m.font_small, fr, t, T.text3);
    }
  }
  gfx_clip_pop();
}

static int guess_layout(const FmAsrc *s) {
  if (s && !strcmp(s->key, "radio")) return AL_STATIONS;
  if (s && !strcmp(s->key, "podcasts")) return AL_COVERS;
  return AL_ROWS;
}

static void feed_list(Feed *f, FmRect view, int layout, const char *src_key, bool detail, float header,
                      AoListRes *out) {
  AoListOpts o;
  memset(&o, 0, sizeof o);
  bool first = feed_loading_first(f), more = f->task && f->more;
  o.layout = layout;
  o.header = header;
  o.play_key = S.play_key;
  o.play_state = S.play_state;
  o.episodes = detail && S.det.kind == AITEM_PODCAST;
  if (first) {
    o.skeleton = layout == AL_ROWS ? 12 : layout == AL_STATIONS ? 18 : 16;
    o.fixed = !detail;
  } else if (more) {
    o.skeleton = layout == AL_ROWS ? 3 : 4;
  }
  o.footer = f->list.n > 0 && !more ? DP(60) : DP(10);
  FM_UNUSED(src_key);
  AoList empty;
  aol_init(&empty, "ao.empty");
  AoListRes r = ao_list_draw(first && !detail ? &empty : &f->list, view, &o);
  if (out) *out = r;
  if (first) return;
  feed_footer(view, &r, f, detail);
  if (r.near_end && f->next[0] && !f->task && !f->more_err) {
    if (detail) children_start(true);
    else search_start(true);
  }
  list_actions(&f->list, &r);
}

static void source_page(FmRect body, const FmAsrc *s) {
  float side = body.w < DP(520) ? DP(12) : DP(18);
  float ph = pill_h();
  FmRect top = rect_cut_top(&body, ph + DP(ui.touch_mode ? 24 : 22));
  float pw = FM_MIN(top.w - side * 2, DP(760));
  FmRect pill = { top.x + (top.w - pw) * 0.5f, top.y + DP(12), pw, ph };
  if (can_search(s)) draw_search(pill, s);
  else {
    float tl = font_line_h(ui.m.font_title);
    font_draw_ellipsis(FONT_BOLD, ui.m.font_title, pill.x + DP(6), pill.y + (ph - tl) * 0.5f, s->name, pill.w, T.text);
  }
  bool focused = ui.focus == ui_id("ao.search");
  bool browse = !S.query[0] && can_browse(s) && !missing_key(s);
  if (focused && !S.field[0] && arecent_count() > 0 && can_search(s)) {
    FmRect rr = { pill.x + DP(2), body.y, pw - DP(4), 0 };
    float h = draw_recents(rr, false);
    rect_cut_top(&body, h + DP(12));
    draw_recents(FM_RECT(rr.x, rr.y, rr.w, h), true);
  } else if (browse && (S.ncats > 0 || S.cats_task)) {
    FmRect cr = rect_cut_top(&body, DP(ui.touch_mode ? 46 : 42));
    cr = rect_inset2(cr, side, 0);
    if (S.ncats > 0) draw_cats(FM_RECT(cr.x, cr.y, cr.w, cr.h - DP(8)));
    else {
      for (int k = 0; k < 6; k++) {
        float w = ao_shimmer(k), cw = DP(70 + (k % 3) * 18), ch = DP(ui.touch_mode ? 36 : 32);
        FmRect c = { cr.x + (float)k * DP(110), cr.y + (cr.h - DP(8) - ch) * 0.5f, cw, ch };
        if (c.x + c.w > cr.x + cr.w) break;
        gfx_rrect(c, ch * 0.5f, col_mix(T.surface2, T.text, 0.04f + 0.03f * w));
      }
      ao_tick(&g_tick_cats, 33);
    }
  }
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  Feed *f = &S.res;
  int layout = f->list.n > 0 ? ao_layout_for(&f->list) : guess_layout(s);
  if (missing_key(s) && !S.query[0]) {
    onboarding(body, s);              /* the welcome with the free-key card */
  } else if (feed_loading_first(f) || (S.cats_task && browse)) {
    AoList empty;
    aol_init(&empty, "ao.empty");
    AoListOpts o;
    memset(&o, 0, sizeof o);
    o.layout = guess_layout(s);
    o.skeleton = o.layout == AL_ROWS ? 12 : 18;
    o.fixed = true;
    ao_list_draw(&empty, body, &o);
  } else if (f->searched && f->estate != E_NONE && f->list.n == 0) {
    error_state(body, s, f, false);
  } else if (f->list.n > 0) {
    feed_list(f, body, layout, s->key, false, 0, NULL);
  } else if (missing_key(s) && f->searched) {
    error_state(body, s, f, false);
  } else {
    onboarding(body, s);
  }
}

/* The podcast / album header: cover, titles and the actions. */
static float detail_header_h(float w) {
  float cs = DP(w < DP(460) ? 104 : 140);
  return DP(16) + cs + DP(18);
}

static void detail_header(FmRect r, float w) {
  const FmAsrcItem *it = &S.det;
  bool narrow = w < DP(460);
  float cs = DP(narrow ? 104 : 140);
  float pad = DP(narrow ? 12 : 18);
  FmRect cover = { r.x + pad, r.y + DP(16), cs, cs };
  gfx_shadow(cover, DP(16), DP(16), T.shadow);
  ao_art(cover, it, DP(16));
  float x = cover.x + cs + DP(narrow ? 14 : 20), tw = r.x + r.w - x - pad;
  float y = cover.y + DP(2);
  const char *kind = it->kind == AITEM_PODCAST ? "PODCAST" : "ALBUM";
  char head[96];
  fm_snprintf(head, sizeof head, "%s  \xC2\xB7  %s", kind, ao_src_name(it->source));
  font_draw_ellipsis(FONT_BOLD, ui.m.font_small * 0.92f, x, y, head, tw, T.accent);
  y += font_line_h(ui.m.font_small) + DP(2);
  float fs = narrow ? ui.m.font_title : ui.m.font_title * 1.25f;
  int lines = ao_text_lines(FONT_BOLD, fs, x, y, tw, it->title[0] ? it->title : "Untitled", narrow ? 2 : 2, T.text, true);
  y += font_line_h(fs) * (float)lines + DP(2);
  char meta[300];
  meta[0] = 0;
  if (it->artist[0]) fm_strlcpy(meta, it->artist, sizeof meta);
  Feed *f = &S.det_feed;
  if (f->list.n > 0) {
    char c[48];
    fm_snprintf(c, sizeof c, "%d%s %s", f->list.n, f->next[0] ? "+" : "",
                it->kind == AITEM_PODCAST ? (f->list.n == 1 ? "episode" : "episodes") : (f->list.n == 1 ? "track" : "tracks"));
    if (meta[0]) fm_strlcat(meta, "  \xC2\xB7  ", sizeof meta);
    fm_strlcat(meta, c, sizeof meta);
  }
  if (it->license[0]) {
    if (meta[0]) fm_strlcat(meta, "  \xC2\xB7  ", sizeof meta);
    fm_strlcat(meta, it->license, sizeof meta);
  }
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y, meta, tw, T.text2);
  /* actions along the cover's bottom */
  float bh = DP(ui.touch_mode ? 40 : 34);
  float by = cover.y + cs - bh;
  u32 id = ui_id("ao.dethead");
  bool has = false;
  for (int i = 0; i < f->list.n && !has; i++) has = aitem_playable(&f->list.items[i]);
  const char *lp = narrow ? "Play" : "Play all";
  float pw = font_width(FONT_BOLD, ui.m.font, lp, -1) + DP(52);
  float bx = x;
  if (has && ui_button(ui_idn(id, 1), FM_RECT(bx, by, pw, bh), IC_PLAY, lp, UI_BTN_FILLED)) {
    int first = 0;
    while (first < f->list.n && !aitem_playable(&f->list.items[first])) first++;
    play_in(&f->list, first);
  }
  if (has) bx += pw + DP(8);
  bool pod = it->kind == AITEM_PODCAST;
  bool on = alib_has(pod ? ALIB_SUBS : ALIB_FAV, it);
  const char *ls = pod ? (on ? "Subscribed" : "Subscribe") : (on ? "Saved" : "Save");
  float sw = font_width(FONT_BOLD, ui.m.font, ls, -1) + DP(52);
  if (bx + sw <= x + tw) {
    if (ui_button(ui_idn(id, 2), FM_RECT(bx, by, sw, bh), on ? IC_CHECK : pod ? IC_RSS : IC_HEART, ls,
                  on ? UI_BTN_TONAL : UI_BTN_OUTLINE))
      alib_toggle(pod ? ALIB_SUBS : ALIB_FAV, it);
    bx += sw + DP(4);
  } else if (ui_icon_btn(ui_idn(id, 2), FM_RECT(bx, by, bh, bh), on ? IC_CHECK : pod ? IC_RSS : IC_HEART,
                         on ? T.accent : T.text2, ls)) {
    alib_toggle(pod ? ALIB_SUBS : ALIB_FAV, it);
    bx += bh;
  }
  if (it->page[0] && bx + bh <= x + tw &&
      ui_icon_btn(ui_idn(id, 3), FM_RECT(bx, by, bh, bh), IC_OPEN_WITH, T.text2, "Open in browser"))
    ao_open_url(it->page);
}

static void detail_page(FmRect body) {
  /* back row */
  float side = body.w < DP(520) ? DP(12) : DP(18);
  FmRect head = rect_cut_top(&body, DP(ui.touch_mode ? 52 : 46));
  FmRect in = rect_inset2(head, side - DP(6), 0);
  float bs = DP(ui.touch_mode ? 44 : 38);
  static const char *const kBack[] = { "Back to results", "", "Favorites", "Recently played", "Subscriptions" };
  const char *bl = S.det_back >= 0 && S.det_back < 5 && kBack[S.det_back][0] ? kBack[S.det_back] : "Back";
  FmRect bb = rect_center(rect_cut_left(&in, bs), bs, bs);
  if (ui_icon_btn(ui_id("ao.det.back"), bb, IC_BACK, T.text, bl)) { go_back(); return; }
  rect_cut_left(&in, DP(4));
  font_draw_ellipsis(FONT_REGULAR, ui.m.font, in.x, in.y + (in.h - font_line_h(ui.m.font)) * 0.5f, bl, in.w, T.text2);
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  Feed *f = &S.det_feed;
  float hh = detail_header_h(body.w);
  if (f->searched && f->estate != E_NONE && f->list.n == 0) {
    FmRect hr = rect_cut_top(&body, hh);
    detail_header(hr, body.w);
    ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
    error_state(body, asrc_find(S.det.source) ? asrc_find(S.det.source) : cur_src(), f, true);
    return;
  }
  AoListRes r;
  feed_list(f, body, AL_ROWS, S.det.source, true, hh, &r);
  gfx_clip_push(body);
  FmRect hr = { body.x, r.header_y - DP(6), body.w, hh };
  if (gfx_visible(hr)) detail_header(hr, body.w);
  gfx_clip_pop();
}

static void lib_page(FmRect body) {
  sync_lib();
  float side = body.w < DP(520) ? DP(12) : DP(18);
  FmRect head = rect_cut_top(&body, pill_h() + DP(ui.touch_mode ? 24 : 22));
  FmRect in = rect_inset2(head, side, 0);
  static const char *const kTitle[] = { "", "", "Favorites", "Recently played", "Subscriptions" };
  static const FmIcon kIc[] = { IC_NONE, IC_NONE, IC_HEART_FILL, IC_HISTORY, IC_RSS };
  int p = FM_CLAMP(S.page, AP_FAV, AP_SUBS);
  if (p == AP_RECENT && S.lib.n > 0) {
    const char *l = "Clear";
    float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(48);
    FmRect b = rect_cut_right(&in, bw);
    if (ui_button(ui_id("ao.lib.clear"), rect_center(b, bw, DP(ui.touch_mode ? 40 : 34)), IC_DELETE, l, UI_BTN_TEXT)) {
      while (alib_count(ALIB_RECENT) > 0) {
        FmAsrcItem it;
        if (!alib_get(ALIB_RECENT, 0, &it)) break;
        alib_remove(ALIB_RECENT, &it);
      }
    }
  }
  float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
  float ty = in.y + (in.h - tl - sl) * 0.5f;
  float is = DP(22);
  icon_draw(kIc[p], FM_RECT(in.x, ty + (tl - is) * 0.5f, is, is), T.accent);
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x + is + DP(10), ty, kTitle[p], in.w - is - DP(10), T.text);
  char sub[64];
  fm_snprintf(sub, sizeof sub, "%d  \xC2\xB7  on this device", S.lib.n);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + is + DP(10), ty + tl, sub, in.w - is - DP(10), T.text2);
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  rect_cut_top(&body, DP(1));
  if (S.lib.n == 0) {
    static const char *const kL[] = { "Browse radio" };
    static const FmIcon kI[] = { IC_RADIO };
    const char *t = p == AP_FAV ? "No favorites yet" : p == AP_RECENT ? "Nothing played yet" : "No subscriptions yet";
    const char *s = p == AP_FAV ? "Tap the heart on a station or a track to keep it here."
                  : p == AP_RECENT ? "Stations, tracks and episodes you play show up here."
                                   : "Open a podcast and tap Subscribe to follow it.";
    int r = state_view(body, kIc[p], T.accent, t, s, "Kept on this device only.", kL, kI, 1);
    if (r == 0) {
      for (int i = 0; i < asrc_count(); i++)
        if (!strcmp(asrc_at(i)->key, p == AP_SUBS ? "podcasts" : "radio")) set_source(i);
      show_source();
    }
    return;
  }
  AoListOpts o;
  memset(&o, 0, sizeof o);
  o.layout = p == AP_SUBS ? AL_COVERS : AL_ROWS;
  o.footer = DP(16);
  o.play_key = S.play_key;
  o.play_state = S.play_state;
  AoListRes r = ao_list_draw(&S.lib, body, &o);
  list_actions(&S.lib, &r);
}

static void draw_body(FmRect r) {
  const FmAsrc *s = cur_src();
  float rad = ui.m.radius;
  gfx_shadow(r, rad, DP(12), T.shadow);
  gfx_rrect(r, rad, T.surface);
  if (T.panel_border.a) gfx_rrect_line(r, rad, DP(T.panel_border_w), T.panel_border);
  else gfx_rrect_line(r, rad, DP(1), T.border);
  if (!s) {
    state_view(r, IC_MUSIC, T.text2, "No audio sources", "This build has no online audio sources.", NULL, NULL, NULL, 0);
    return;
  }
  gfx_clip_push(r);
  switch (S.page) {
    case AP_DETAIL: detail_page(r); break;
    case AP_FAV: case AP_RECENT: case AP_SUBS: lib_page(r); break;
    default: source_page(r, s); break;
  }
  gfx_clip_pop();
}

/* ---- rail, chips, top bar ----------------------------------------------------------------------- */

static void tray_toggle(void) {
  S.tray_open = !S.tray_open;
  ui_redraw();
}

static void rail_heading(float x, float *y, float hh, const char *t) {
  font_draw(FONT_BOLD, ui.m.font_small, x + DP(10), *y + hh - font_line_h(ui.m.font_small) - DP(4), t, -1, T.text3);
  *y += hh;
}

static void rail_count(FmRect row, int n, bool strong) {
  char c[16];
  fm_snprintf(c, sizeof c, "%d", n);
  float cw = FM_MAX(DP(22), font_width(FONT_BOLD, ui.m.font_small, c, -1) + DP(12));
  FmRect b = { row.x + row.w - cw - DP(10), row.y + (row.h - DP(22)) * 0.5f, cw, DP(22) };
  gfx_rrect(b, DP(11), strong ? T.accent : T.surface3);
  font_draw_center(FONT_BOLD, ui.m.font_small, b, c, strong ? T.on_accent : T.text2);
}

static const struct { int page, lib; FmIcon ic; const char *label; } kLib[3] = {
  { AP_FAV, ALIB_FAV, IC_HEART, "Favorites" },
  { AP_RECENT, ALIB_RECENT, IC_HISTORY, "Recently played" },
  { AP_SUBS, ALIB_SUBS, IC_RSS, "Subscriptions" },
};

static void draw_rail(FmRect r) {
  gfx_shadow(r, ui.m.radius, DP(10), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, ui.m.radius, T.surface);
  gfx_rrect_line(r, ui.m.radius, DP(1), T.border);
  FmRect in = rect_inset(r, DP(8));
  float rh = DP(ui.touch_mode ? 58 : 52), hh = DP(28), mh = DP(ui.touch_mode ? 46 : 40);
  int ns = asrc_count();
  float content = hh + (float)ns * (rh + DP(2)) + DP(6) + hh + mh * 3 + DP(6) + hh + mh * 2 + DP(4);
  u32 base = ui_id("ao.rail");
  ui_scroll(&S.rail_scroll, base, in, content);
  gfx_clip_push(in);
  float y = in.y - S.rail_scroll.y;
  rail_heading(in.x, &y, hh, "SOURCES");
  for (int i = 0; i < ns; i++) {
    const FmAsrc *s = asrc_at(i);
    FmRect row = { in.x, y, in.w, rh };
    y += rh + DP(2);
    if (!gfx_visible(row)) continue;
    int f = ui_hit(ui_idn(base, (u32)i + 1), row);
    bool here = i == S.src && S.page == AP_SOURCE;
    if (here) gfx_rrect(row, DP(12), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
    float ts = DP(34);
    FmRect tile = { row.x + DP(8), row.y + (rh - ts) * 0.5f, ts, ts };
    if (here) gfx_rrect_vgrad(tile, DP(10), col_mix(T.accent, AO_WHITE, 0.15f), T.accent);
    else gfx_rrect(tile, DP(10), T.surface2);
    icon_draw(ao_src_icon(s), rect_inset(tile, DP(8)), here ? T.on_accent : T.text2);
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
    const char *sub = key ? "Needs a free key" : (s->flags & ASRC_NEEDKEY) ? "Key added" : "No key needed";
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, ty + lh, sub, tw, key ? T.text3 : T.text3);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) set_source(i);
  }
  y += DP(6);
  rail_heading(in.x, &y, hh, "LIBRARY");
  for (int i = 0; i < 3; i++) {
    FmRect row = { in.x, y, in.w, mh };
    y += mh;
    int f = ui_hit(ui_idn(base, 200 + (u32)i), row);
    bool here = S.page == kLib[i].page || (S.page == AP_DETAIL && S.det_back == kLib[i].page);
    if (here) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    int cnt = alib_count(kLib[i].lib);
    FmIcon ic = i == 0 && cnt ? IC_HEART_FILL : kLib[i].ic;
    icon_draw(ic, FM_RECT(row.x + DP(16), row.y + (mh - is) * 0.5f, is, is), here || i == 0 ? T.accent : T.text2);
    font_draw(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, row.x + DP(50), row.y + (mh - font_line_h(ui.m.font)) * 0.5f,
              kLib[i].label, -1, here ? T.accent : T.text);
    if (cnt > 0) rail_count(row, cnt, false);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) show_lib(kLib[i].page);
  }
  y += DP(6);
  rail_heading(in.x, &y, hh, "MORE");
  static const struct { FmIcon ic; const char *label; } kMore[] = { { IC_DOWNLOAD, "Downloads" },
                                                                    { IC_SETTINGS, "Settings" } };
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
    if (i == 0 && adl_count() > 0) {
      int act = adl_active();
      rail_count(row, act ? act : adl_count(), act > 0);
    }
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) {
      if (i == 0) tray_toggle();
      else ao_open_settings();
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&S.rail_scroll, in, content);
}

/* Sources, then the library, as one scrolling strip (narrow windows). */
static void draw_chips(FmRect r) {
  u32 base = ui_id("ao.chips");
  int ns = FM_MIN(asrc_count(), 12);
  int n = ns + 3;
  float ch = DP(ui.touch_mode ? 38 : 32), gap = DP(8), pad = DP(2), sep = DP(14);
  float fs = ui.m.font_small * 1.08f, is = DP(16);
  float w[16], total = pad;
  for (int i = 0; i < n; i++) {
    const char *l = i < ns ? asrc_at(i)->name : kLib[i - ns].label;
    w[i] = font_width(FONT_BOLD, fs, l, -1) + is + DP(30) + (i < ns && missing_key(asrc_at(i)) ? DP(18) : 0);
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
  int lp = S.page == AP_DETAIL ? S.det_back : S.page;
  int on_i = lp == AP_FAV ? ns : lp == AP_RECENT ? ns + 1 : lp == AP_SUBS ? ns + 2 : S.src;
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
    FmIcon ic = i < ns ? ao_src_icon(asrc_at(i)) : kLib[i - ns].ic;
    const char *l = i < ns ? asrc_at(i)->name : kLib[i - ns].label;
    icon_draw(ic, FM_RECT(c.x + DP(12), c.y + (ch - is) * 0.5f, is, is), on ? fg : (i >= ns ? T.accent : T.text2));
    float tx = font_draw(on ? FONT_BOLD : FONT_REGULAR, fs, c.x + DP(12) + is + DP(6), c.y + (ch - font_line_h(fs)) * 0.5f,
                         l, -1, fg);
    if (i < ns && missing_key(asrc_at(i))) {
      float ks = DP(12);
      icon_draw(IC_KEY, FM_RECT(tx + DP(5), c.y + (ch - ks) * 0.5f, ks, ks), on ? fg : T.text3);
    }
    if ((f & UI_CLICK) && !dragging) {
      if (i < ns) set_source(i);
      else show_lib(kLib[i - ns].page);
    }
  }
  gfx_clip_pop();
}

static void downloads_btn(FmRect b, u32 id) {
  title_nodrag(b);
  int act = adl_active();
  if (ui_toggle_btn(id, b, IC_DOWNLOAD, S.tray_open, "Downloads (Ctrl+J)")) tray_toggle();
  if (act > 0) {
    float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, rr = FM_MIN(b.w, b.h) * 0.5f - DP(3);
    float fr = adl_frac();
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
  u32 base = ui_id("ao.top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text, "Back to files")) aonline_close();
  rect_cut_left(&in, DP(4));
  if (!narrow) {
    FmRect m = rect_center(rect_cut_left(&in, DP(34)), DP(28), DP(28));
    gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, AO_WHITE, 0.15f), T.accent);
    icon_draw(IC_RADIO, rect_inset(m, DP(5)), T.on_accent);
    rect_cut_left(&in, DP(8));
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_SETTINGS, T.text2, "Online audio settings")) ao_open_settings();
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  downloads_btn(b, ui_idn(base, 3));
  const FmAsrc *s = cur_src();
  char sub[300], title[200];
  sub[0] = 0;
  const char *lead = narrow || !s ? "Online audio" : s->name;
  fm_strlcpy(title, "Online audio", sizeof title);
  Feed *f = &S.res;
  switch (S.page) {
    case AP_DETAIL:
      if (narrow) fm_strlcpy(title, S.det.title, sizeof title);
      fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", S.det.kind == AITEM_PODCAST ? "Podcast" : "Album",
                  narrow ? ao_src_name(S.det.source) : S.det.title);
      break;
    case AP_FAV: case AP_RECENT: case AP_SUBS:
      if (narrow) fm_strlcpy(title, kLib[S.page - AP_FAV].label, sizeof title);
      fm_snprintf(sub, sizeof sub, "Library  \xC2\xB7  on this device");
      break;
    default:
      if (narrow && s) fm_strlcpy(title, s->name, sizeof title);
      if (f->task && !f->more)
        fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", lead, S.query[0] ? "Searching\xE2\x80\xA6" : "Loading\xE2\x80\xA6");
      else if (f->list.n > 0 && S.query[0])
        fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %d%s results", lead, f->list.n, f->next[0] ? "+" : "");
      else if (f->list.n > 0 && S.ncats > 0)
        fm_snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", lead, S.cats[FM_CLAMP(S.cat, 0, S.ncats - 1)].name);
      else fm_strlcpy(sub, lead, sizeof sub);
      break;
  }
  float fs = ui.m.font_title * (narrow ? 0.9f : 1.0f);
  float tl = font_line_h(fs), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, fs, in.x + DP(2), y, title, in.w - DP(4), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + tl, sub, in.w - DP(4), T.text2);
}

/* ---- downloads tray ----------------------------------------------------------------------------- */

static void draw_tray(void) {
  int n = adl_count();
  float rh = DP(ui.touch_mode ? 76 : 68);
  float bh = DP(ui.touch_mode ? 44 : 36);
  float list_h = n ? FM_MIN((float)n, 5.5f) * rh : DP(120);
  bool sheet = ui.portrait && ui.w < DP(560);
  float cw = (sheet ? ui.w : FM_MIN(DP(560), ui.w - DP(24))) - DP(40);
  float h = font_line_h(ui.m.font_title) + DP(12) + list_h + DP(14) + bh;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("ao.tray"), "Downloads", 560, (h + DP(40)) / ui.scale + 2, &cancel);
  FmRect list = rect_cut_top(&c, list_h);
  char dir[FM_PATH_MAX];
  odl_dir(dir, sizeof dir);
  if (n == 0) {
    ui_label(FM_RECT(list.x, list.y + DP(30), list.w, font_line_h(ui.m.font)), "Tracks you download appear here.",
             FONT_REGULAR, ui.m.font, T.text2, UI_CENTER);
    char t[FM_PATH_MAX + 64];
    fm_snprintf(t, sizeof t, "Saved to %s. Live stations cannot be downloaded.", dir);
    ui_label(FM_RECT(list.x, list.y + DP(30) + font_line_h(ui.m.font) + DP(4), list.w, font_line_h(ui.m.font_small)), t,
             FONT_REGULAR, ui.m.font_small, T.text3, UI_CENTER);
  } else {
    u32 sid = ui_id("ao.tray.list");
    float content = (float)n * rh;
    ui_scroll(&S.tray_scroll, sid, list, content);
    gfx_clip_push(list);
    int remove = -1, cancel_i = -1, retry = -1;
    for (int i = 0; i < n; i++) {
      FmRect row = { list.x, list.y + (float)i * rh - S.tray_scroll.y, list.w, rh };
      if (!gfx_visible(row)) continue;
      AoDl *d = adl_at(i);
      u32 id = ui_idn(sid, (u32)i + 1);
      float th = rh - DP(20);
      FmRect thumb = { row.x, row.y + DP(10), th, th };
      ao_art(thumb, &d->item, DP(8));
      float bs = FM_MIN(ui.m.hit, DP(40));
      FmRect act = { row.x + row.w, row.y + (rh - bs) * 0.5f, bs, bs };
      float x = thumb.x + th + DP(12);
      if (d->state == AD_RUNNING || d->state == AD_QUEUED) {
        act.x -= bs;
        if (ui_icon_btn(ui_idn(id, 1), act, IC_CLOSE, T.text2, "Cancel")) cancel_i = i;
      } else {
        act.x -= bs;
        if (ui_icon_btn(ui_idn(id, 2), act, IC_CLOSE, T.text3, "Remove from the list")) remove = i;
        if (d->state == AD_DONE) {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 3), act, IC_FOLDER_OPEN, T.text2, "Show in folder")) ao_reveal(d->path);
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 4), act, IC_PLAY, T.accent, "Play")) app_open(d->path, NULL, 0, 0);
        } else {
          act.x -= bs;
          if (ui_icon_btn(ui_idn(id, 5), act, IC_REFRESH, T.text2, "Try again")) retry = i;
        }
      }
      float w = act.x - DP(8) - x;
      float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
      float y = row.y + DP(10);
      font_draw_ellipsis(FONT_REGULAR, ui.m.font, x, y, d->item.title[0] ? d->item.title : "Track", w, T.text);
      y += lh + DP(2);
      char st[300];
      FmColor sc = T.text2;
      switch (d->state) {
        case AD_QUEUED: fm_strlcpy(st, "Waiting\xE2\x80\xA6", sizeof st); break;
        case AD_RUNNING:
          if (d->frac >= 0) fm_snprintf(st, sizeof st, "%d%%  \xC2\xB7  %s", (int)(d->frac * 100), ao_src_name(d->item.source));
          else fm_snprintf(st, sizeof st, "Downloading  \xC2\xB7  %s", ao_src_name(d->item.source));
          break;
        case AD_DONE: fm_snprintf(st, sizeof st, "Saved  \xC2\xB7  %s", fm_path_base(d->path)); sc = T.success; break;
        case AD_CANCELLED: fm_strlcpy(st, "Cancelled", sizeof st); sc = T.text3; break;
        default: fm_snprintf(st, sizeof st, "Failed: %s", d->err); sc = T.danger; break;
      }
      font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y, st, w, sc);
      y += ls + DP(6);
      if (d->state == AD_RUNNING) ui_progress(FM_RECT(x, y, w, DP(5)), d->frac);
      if (i < n - 1) ui_divider(x, row.x + row.w, row.y + rh - DP(0.5f));
    }
    gfx_clip_pop();
    ui_scrollbar(&S.tray_scroll, list, content);
    if (cancel_i >= 0) adl_cancel(cancel_i);
    if (remove >= 0) adl_remove(remove);
    if (retry >= 0) adl_retry(retry);
  }
  rect_cut_top(&c, DP(14));
  FmRect br = rect_cut_top(&c, bh);
  u32 bid = ui_id("ao.tray.btn");
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
  for (int i = 0; i < n; i++) fin |= adl_at(i)->state != AD_RUNNING && adl_at(i)->state != AD_QUEUED;
  if (fin) {
    const char *lc = "Clear";
    float cwb = font_width(FONT_BOLD, ui.m.font, lc, -1) + DP(32);
    if (ui_button(ui_idn(bid, 2), FM_RECT(x, br.y, cwb, bh), IC_NONE, lc, UI_BTN_TEXT)) adl_clear_finished();
  }
  ui_dialog_end();
  if (cancel || done) S.tray_open = false;
}

/* ---- public --------------------------------------------------------------------------------------- */

void aonline_init(bool readonly) {
  memset(&S, 0, sizeof S);
  S.readonly = readonly;
  S.lib_page = -1;
  ID_MENU = ui_id("ao.menu");
  aol_init(&S.res.list, "ao.list.res");
  aol_init(&S.det_feed.list, "ao.list.det");
  aol_init(&S.lib, "ao.list.lib");
  arecent_load(readonly);
  alib_load(readonly);
  aplay_init();
}

void aonline_shutdown(void) {
  feed_clear(&S.res);
  feed_clear(&S.det_feed);
  if (S.cats_task) atask_drop(S.cats_task);
  S.cats_task = NULL;
  atask_shutdown();
  adl_shutdown();
  alib_shutdown();
  aol_free(&S.res.list);
  aol_free(&S.det_feed.list);
  aol_free(&S.lib);
}

bool aonline_is_open(void) { return S.open; }
int aonline_downloads_active(void) { return adl_active(); }

void aonline_open(const char *source) {
  int idx = -1;
  const char *key = source && source[0] ? source : conf.audio_source;
  for (int i = 0; i < asrc_count(); i++)
    if (key && !strcmp(asrc_at(i)->key, key)) idx = i;
  if (idx < 0 && source)
    for (int i = 0; i < asrc_count(); i++)
      if (!fm_stricmp(asrc_at(i)->name, source)) idx = i;
  if (idx < 0 && !S.open && S.res.list.n == 0) idx = 0;
  if (idx >= 0 && idx != S.src) {
    feed_clear(&S.res);
    if (S.cats_task) { atask_drop(S.cats_task); S.cats_task = NULL; }
    S.ncats = 0;
    S.cats_for[0] = 0;
    S.src = idx;
  }
  S.open = true;
  S.chip_reveal = true;
  if (S.page == AP_SOURCE) browse_if_idle();
  ui_redraw();
}

void aonline_close(void) {
  S.open = false;
  S.tray_open = false;
  ui_focus(0);
  /* the artwork goes back; results, the library and the query stay */
  othumb_reset();
  ui_redraw();
}

static void demo_after_results(void);

void aonline_pump(void) {
  atask_reap();
  take_cats();
  feed_take(&S.res);
  feed_take(&S.det_feed);
  adl_pump();
  alib_pump();
  if (S.demo_state[0] && !S.demo_done && S.res.searched && !S.res.task) {
    S.demo_done = true;
    demo_after_results();
  }
}

void aonline_frame(FmRect area) {
  menu_results();
  if (!S.open) return;
  S.play_state = aplay_state(S.play_key, sizeof S.play_key);
  theme_draw_bg(area);
  bool narrow = area.w / ui.scale < 760;
  FmRect r = area;
  FmRect top = rect_cut_top(&r, ui.m.bar_h);
  /* the mini player is cut now and drawn after the body: its once-a-second
  ** wake must not be armed before the playing row's quicker one (the wake
  ** timer keeps only the earliest, and a later one would add frames) */
  FmRect mini = { 0, 0, 0, 0 };
  if (audio_mini_active()) mini = rect_cut_bottom(&r, DP(ui.touch_mode ? 64 : 56));
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
  if (mini.h > 0) audio_mini_draw(rect_inset2(mini, DP(8), DP(4)));
  draw_top(top, narrow);
  if (!S.open) return;
  if (S.tray_open) draw_tray();
  if (!S.open) return;
  if (ui_input_ok() && !ui_menu_is_open()) {
    if (ui_key(SDLK_f, KMOD_CTRL) || ui_key(SDLK_l, KMOD_CTRL)) { show_source(); S.focus_search = true; }
    if (!ui.focus && ui_key(SDLK_SLASH, 0)) { show_source(); S.focus_search = true; }
    if (ui_key(SDLK_F5, 0) || ui_key(SDLK_r, KMOD_CTRL)) {
      if (S.page == AP_SOURCE) search_start(false);
      else if (S.page == AP_DETAIL) children_start(false);
    }
    if (ui_key(SDLK_j, KMOD_CTRL)) tray_toggle();
    if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0) || ui_key(SDLK_BACKSPACE, KMOD_ALT)) go_back();
    if (!ui.focus && asrc_count() > 1) {
      if (ui_key(SDLK_PAGEDOWN, KMOD_CTRL)) set_source((S.src + 1) % asrc_count());
      if (ui_key(SDLK_PAGEUP, KMOD_CTRL)) set_source((S.src + asrc_count() - 1) % asrc_count());
    }
  }
}

/* ---- demo ------------------------------------------------------------------------------------------- */

static void env_conf(const char *env, char *dst, size_t cap) {
  const char *v = getenv(env);
  if (v && v[0]) fm_strlcpy(dst, v, cap);
}

static int first_kind(const AoList *l, bool container) {
  for (int i = 0; i < l->n; i++)
    if (aitem_container(&l->items[i]) == container) return i;
  return -1;
}

static void demo_after_results(void) {
  char st[24];
  fm_strlcpy(st, S.demo_state, sizeof st);
  int k = -1;
  size_t len = strlen(st);
  while (len > 0 && st[len - 1] >= '0' && st[len - 1] <= '9') len--;
  if (st[len]) { k = atoi(st + len); st[len] = 0; }
  AoList *l = &S.res.list;
  if (l->n == 0) return;
  if (!strcmp(st, "play") || !strcmp(st, "player") || !strcmp(st, "e2e")) {
    int i = k >= 0 ? FM_CLAMP(k, 0, l->n - 1) : first_kind(l, false);
    if (i < 0) return;
    play_in(l, i);
    if (!strcmp(st, "player")) audio_show_player();
    if (!strcmp(st, "e2e")) {
      if (!alib_has(ALIB_FAV, &l->items[i])) alib_toggle(ALIB_FAV, &l->items[i]);
      if (l->items[i].kind == AITEM_TRACK) adl_start(&l->items[i]);
      S.tray_open = l->items[i].kind == AITEM_TRACK;
    }
  } else if (!strcmp(st, "detail")) {
    int i = k >= 0 ? FM_CLAMP(k, 0, l->n - 1) : first_kind(l, true);
    if (i >= 0) open_detail(&l->items[i]);
  } else if (!strcmp(st, "menu")) {
    int i = FM_CLAMP(k, 0, l->n - 1);
    open_menu(&l->items[i], ui.w * 0.5f, ui.h * 0.35f);
  } else if (!strcmp(st, "downloads")) {
    int got = 0;
    for (int i = 0; i < l->n && got < 3; i++)
      if (l->items[i].kind == AITEM_TRACK) { adl_start(&l->items[i]); got++; }
    S.tray_open = true;
  } else if (!strcmp(st, "favorites") || !strcmp(st, "recent") || !strcmp(st, "subs")) {
    /* in-memory library rows for the screenshot (--shot never writes) */
    for (int i = 0; i < l->n && i < 8; i++) {
      const FmAsrcItem *it = &l->items[i];
      if (it->kind == AITEM_PODCAST) alib_add(ALIB_SUBS, it);
      else alib_add(!strcmp(st, "recent") ? ALIB_RECENT : ALIB_FAV, it);
    }
    show_lib(!strcmp(st, "favorites") ? AP_FAV : !strcmp(st, "recent") ? AP_RECENT : AP_SUBS);
  } else if (!strcmp(st, "scroll")) {
    l->scroll.y = 1e7f;
  }
  ui_redraw();
}

void aonline_demo(const char *source, const char *query, const char *state) {
  env_conf("MMCFM_JAMENDO_KEY", conf.key_jamendo, sizeof conf.key_jamendo);
  env_conf("MMCFM_FREESOUND_KEY", conf.key_freesound, sizeof conf.key_freesound);
  if (state && (!strcmp(state, "downloads") || !strncmp(state, "e2e", 3)) && S.readonly) {
    char tmp[FM_PATH_MAX];
    if (plat_place(PLACE_TEMP, tmp, sizeof tmp))
      fm_path_join(conf.online_dl_dir, sizeof conf.online_dl_dir, tmp, "mmcfm-demo-audio");
  }
  aonline_open(source);
  fm_strlcpy(S.demo_state, state ? state : "", sizeof S.demo_state);
  if (query && query[0]) {
    fm_strlcpy(S.field, query, sizeof S.field);
    submit(query);
  }
  static const struct { const char *name; int st; const char *msg; } kForce[] = {
    { "nokey", E_NOKEY, "" },
    { "offline", E_OFFLINE, "Network error \xE2\x80\x94 check the internet connection (the server name could not be resolved)" },
    { "empty", E_EMPTY, "" },
    { "error", E_OTHER, "Radio Browser answered HTTP 503: the service is busy" },
  };
  for (int i = 0; state && i < FM_COUNT(kForce); i++)
    if (!strcmp(state, kForce[i].name)) {
      feed_clear(&S.res);
      if (S.cats_task) { atask_drop(S.cats_task); S.cats_task = NULL; }
      if (!S.query[0]) fm_strlcpy(S.query, query ? query : "jazz", sizeof S.query);
      S.res.searched = true;
      S.res.estate = kForce[i].st;
      fm_strlcpy(S.res.error, kForce[i].msg, sizeof S.res.error);
      S.demo_done = true;
    }
  if (state && !strcmp(state, "onboarding")) {
    feed_clear(&S.res);
    if (S.cats_task) { atask_drop(S.cats_task); S.cats_task = NULL; }
    S.cats_for[0] = 0;
    S.query[0] = 0;
    S.demo_done = true;
    S.res.searched = false;
    /* a source with nothing to browse shows the welcome */
    S.cats_for[0] = 0;
  }
  if (state && !strcmp(state, "settings")) ao_open_settings();
  /* with no query: the library as it is on disk (a restart shows what persisted) */
  if (state && (!strcmp(state, "favorites") || !strcmp(state, "recent") || !strcmp(state, "subs")) &&
      (!query || !query[0])) {
    show_lib(!strcmp(state, "favorites") ? AP_FAV : !strcmp(state, "recent") ? AP_RECENT : AP_SUBS);
    S.demo_done = true;
  }
  ui_focus(0);
  S.focus_search = false;
}
