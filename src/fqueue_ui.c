/* fqueue_ui.c -- the Queue panel, the Playlists section and the
** "Add to playlist" picker.
**
** Design decisions:
**   - One reorderable list drives both the queue and a playlist being
**     edited: every movable row has a grip on its right. The grip drags at
**     once (mouse or finger); a mouse also drags by the row itself, and a
**     finger after a long press on the row (lift), since a plain finger
**     drag on a row scrolls the list. A long press let go without moving
**     opens the row's menu instead. Alt+Up / Alt+Down move the focused row
**     (the one under the pointer or last tapped), Delete removes it.
**   - Rows move under the dragged one at once, without animation; frames
**     are drawn only while a drag lasts (and while it scrolls the list at
**     an edge), so an idle queue costs nothing.
**   - The panel shows "Now playing" and "Up next": what was heard before
**     stays in the queue (Previous and repeat-all go back to it) but is not
**     listed, as in most players.
**   - Menus and dialogs are owned here and drawn by qui_overlay on top of
**     whatever screen asked for them (panels, library, online audio, the
**     player), so each caller only adds its menu items.
*/
#include "fqueue.h"
#include "fview_int.h"
#include "flib.h"
#include "fplat.h"
#include "fthumb.h"
#include "fconf.h"
#include "fonline_int.h"

/* ---- the reorderable list --------------------------------------------------------- */

typedef struct Rl {
  FmScroll scroll;
  int drag;                   /* row being dragged, -1 none */
  float grab_dy;              /* pointer y - row top when it was picked up */
  float drag_top;             /* the lifted row's top in content coordinates */
  bool lift;                  /* picked up by a long press: a release in place is a menu */
  float lift_x, lift_y;
  int focus;                  /* keyboard row, -1 */
  int n;                      /* rows last frame */
  bool pinned;                /* screenshots: held without a pointer */
} Rl;

typedef struct RlRes {
  int click, menu;            /* row or -1 */
  float mx, my;
  int from, to;               /* a finished move, from = -1 none */
  int remove;                 /* Delete on the focused row */
} RlRes;

typedef void (*RlRowFn)(void *u, int i, FmRect r, bool lifted);

static float row_h(void) { return FM_MAX(ui.m.row_h, DP(ui.touch_mode ? 62 : 52)); }
static float grip_w(void) { return FM_MAX(DP(34), ui.touch_mode ? ui.m.hit : 0.0f); }

static int rl_target(const Rl *l, float rh) {
  int t = (int)floorf((l->drag_top + rh * 0.5f) / rh);
  return FM_CLAMP(t, 0, l->n - 1);
}

/* Where row i is drawn while row `drag` hovers over `target`. */
static int rl_slot(int i, int drag, int target) {
  if (drag < 0 || i == drag) return i;
  if (drag < target && i > drag && i <= target) return i - 1;
  if (target < drag && i >= target && i < drag) return i + 1;
  return i;
}

/* Row i is picked up; the pointer holds it `grab_y` px (screen) from its top. */
static void rl_start(Rl *l, u32 owner, int i, float rh, float row_y, float grab_y) {
  l->drag = i;
  l->drag_top = (float)i * rh;
  l->grab_dy = grab_y - row_y;
  l->focus = i;
  l->lift = false;
  ui.drag_owner = owner;
  ui_animate();
}

static RlRes rl_draw(Rl *l, u32 id, FmRect view, int n, RlRowFn row, void *u) {
  RlRes res = { -1, -1, 0, 0, -1, -1, -1 };
  float rh = row_h();
  u32 owner = ui_idn(id, 0x7FFFFFF);
  l->n = n;
  if (l->drag >= n) l->drag = -1;
  if (l->focus >= n) l->focus = n - 1;
  float content = rh * (float)n + DP(8);
  ui_scroll(&l->scroll, id, view, content);
  int target = -1;
  if (l->drag >= 0 && !l->pinned) {
    if (ui.down) {
      /* follow the pointer; near an edge the list scrolls under it */
      float edge = DP(36), sp = 0;
      if (ui.my < view.y + edge) sp = -(view.y + edge - ui.my);
      else if (ui.my > view.y + view.h - edge) sp = ui.my - (view.y + view.h - edge);
      if (sp != 0) {
        l->scroll.y = FM_CLAMP(l->scroll.y + sp * ui.dt * 8.0f, 0.0f, l->scroll.max);
      }
      l->drag_top = ui.my - view.y + l->scroll.y - l->grab_dy;
      l->drag_top = FM_CLAMP(l->drag_top, -rh * 0.5f, rh * (float)(n - 1) + rh * 0.5f);
      ui.drag_owner = owner;
      ui_animate();
    } else {
      /* dropped: a long press let go where it was is the row's menu */
      int t = rl_target(l, rh);
      if (l->lift && fabsf(ui.mx - l->lift_x) < DP(8) && fabsf(ui.my - l->lift_y) < DP(8)) {
        res.menu = l->drag;
        res.mx = ui.mx;
        res.my = ui.my;
      } else if (t != l->drag) {
        res.from = l->drag;
        res.to = t;
      }
      l->focus = t;
      l->drag = -1;
      l->lift = false;
      ui_redraw();
    }
  }
  if (l->drag >= 0) target = rl_target(l, rh);
  gfx_clip_push(view);
  int first = FM_MAX(0, (int)((l->scroll.y - DP(4)) / rh) - 1);
  int last = FM_MIN(n - 1, (int)((l->scroll.y + view.h) / rh) + 1);
  /* rows next to a dragged one may come from outside the visible range */
  if (l->drag >= 0) { first = FM_MAX(0, first - 1); last = FM_MIN(n - 1, last + 1); }
  float gw = grip_w();
  for (int i = first; i <= last; i++) {
    if (i == l->drag) continue;
    int s = rl_slot(i, l->drag, target);
    FmRect r = { view.x, view.y + DP(4) + (float)s * rh - l->scroll.y, view.w, rh };
    if (!gfx_visible(r)) continue;
    u32 rid = ui_idn(id, (u32)i + 1);
    int f = l->drag >= 0 ? 0 : ui_hit(rid, rect_inset2(r, 0, DP(1)));
    FmRect bg = rect_inset2(r, DP(2), DP(2));
    float rad = FM_MIN(DP(10), bg.h * 0.5f);
    if (f & UI_HELD) gfx_rrect(bg, rad, T.press);
    else if (f & UI_HOVER) gfx_rrect(bg, rad, T.hover);
    else if (i == l->focus && !ui.touch_mode && l->drag < 0) gfx_rrect_line(bg, rad, DP(1), T.border);
    if ((f & UI_HOVER) && !ui.from_touch) l->focus = i;
    FmRect grip = rect_cut_right(&r, gw);
    FmRect content_r = r;
    row(u, i, content_r, false);
    int g = l->drag >= 0 ? 0 : ui_hit(ui_idn(rid, 1), grip);
    icon_draw(IC_DRAG, rect_center(grip, DP(20), DP(20)), (g & UI_HOVER) ? T.text : T.text3);
    if (g & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_SIZENS);
    if (l->drag >= 0) continue;
    if (g & UI_PRESS) {
      rl_start(l, owner, i, rh, r.y, ui.press_y);
      continue;
    }
    if ((f & UI_DRAG) && !ui.from_touch && !ui.touch_mode && ui.drag_owner == 0) {
      /* a mouse drags the row itself */
      rl_start(l, owner, i, rh, r.y, ui.press_y);
      continue;
    }
    if (f & UI_LONG) {
      rl_start(l, owner, i, rh, r.y, ui.my);
      l->lift = true;
      l->lift_x = ui.mx;
      l->lift_y = ui.my;
      continue;
    }
    if (f & UI_CLICK) { res.click = i; l->focus = i; }
    if (f & UI_RCLICK) { res.menu = i; res.mx = ui.mx; res.my = ui.my; l->focus = i; }
  }
  if (l->drag >= 0) {
    /* the lifted row, over the others */
    FmRect r = { view.x, view.y + DP(4) + l->drag_top - l->scroll.y, view.w, rh };
    FmRect bg = rect_inset2(r, DP(2), DP(2));
    float rad = FM_MIN(DP(10), bg.h * 0.5f);
    gfx_shadow(bg, rad, DP(14), T.shadow);
    gfx_rrect(bg, rad, T.surface2);
    gfx_rrect_line(bg, rad, DP(1.5f), T.accent);
    FmRect grip = rect_cut_right(&r, gw);
    row(u, l->drag, r, true);
    icon_draw(IC_DRAG, rect_center(grip, DP(20), DP(20)), T.accent);
  }
  gfx_clip_pop();
  ui_scrollbar(&l->scroll, view, content);
  /* keys on the focused row */
  if (l->focus >= 0 && l->drag < 0 && ui_input_ok() && !ui_menu_is_open() && !ui.focus) {
    if (ui_key(SDLK_UP, KMOD_ALT) && l->focus > 0) { res.from = l->focus; res.to = l->focus - 1; l->focus--; }
    else if (ui_key(SDLK_DOWN, KMOD_ALT) && l->focus < n - 1) { res.from = l->focus; res.to = l->focus + 1; l->focus++; }
    else if (ui_key(SDLK_DELETE, 0)) res.remove = l->focus;
    if (res.from >= 0) ui_scroll_to(&l->scroll, (float)l->focus * rh, (float)(l->focus + 1) * rh + DP(8), view.h);
  }
  return res;
}

/* ---- row pieces -------------------------------------------------------------------- */

/* Artwork: an online cover, a file's own cover (through the thumbnails),
** else a gradient tile with a note. */
static void art_tile(FmRect r, const char *art_url, const char *path, bool file, bool live, const char *seed) {
  float rad = DP(8);
  if (art_url && art_url[0]) {
    float fade = 1;
    SDL_Texture *tex = othumb_get_ex(art_url, (int)r.w, live ? 0.0f : 1.0f, NULL, &fade);
    if (tex && fade >= 1) {
      if (live) {
        int tw = 1, th = 1;
        SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
        gfx_rrect(r, rad, FM_HEX(0xF6F7FA));
        float k = FM_MIN(r.w * 0.8f / (float)tw, r.h * 0.8f / (float)th);
        gfx_tex(tex, NULL, rect_center(r, (float)tw * k, (float)th * k), FM_HEX(0xFFFFFF));
      } else {
        gfx_tex_rounded(tex, r, rad, FM_HEX(0xFFFFFF));
      }
      return;
    }
    if (tex) ui_animate();
  } else if (file && path && path[0] && conf.thumbnails) {
    int k = lib_find(path);
    if (k >= 0) {
      const FmLibItem *it = &lib_data()->items[k];
      SDL_Texture *tex = thumb_get(path, it->mtime, it->size, (int)r.w);
      int tw = 0, th = 0;
      if (tex && SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) {
        gfx_rrect(r, rad, T.surface3);
        float s = FM_MIN(r.w / (float)tw, r.h / (float)th);
        FmRect d = rect_center(r, (float)tw * s, (float)th * s);
        gfx_tex_rounded(tex, d, FM_MIN(rad, FM_MIN(d.w, d.h) * 0.5f), FM_HEX(0xFFFFFF));
        return;
      }
    }
  }
  u32 h = ui_id(seed && seed[0] ? seed : "x");
  FmColor a = kAccents[h % UI_ACCENTS], b = kAccents[(h / UI_ACCENTS + 3) % UI_ACCENTS];
  gfx_rrect_vgrad(r, rad, a, col_mix(b, FM_HEX(0x101018), 0.35f));
  float is = r.w * 0.46f;
  icon_draw(live ? IC_RADIO : IC_MUSIC, rect_center(r, is, is), FM_RGBA(255, 255, 255, 220));
}

static void fmt_len(double sec, char *out, size_t cap) {
  if (sec <= 0) { out[0] = 0; return; }
  view_fmt_time(sec, out, cap);
}

/* Title over a second line, art on the left, a time on the right. */
static void track_row(FmRect r, const char *title, const char *sub, const char *art_url, const char *path, bool file,
                      bool live, double dur, bool accent, bool dim) {
  float is = FM_MIN(r.h - DP(14), DP(42));
  FmRect art = { r.x + DP(8), r.y + (r.h - is) * 0.5f, is, is };
  art_tile(art, art_url, path, file, live, title);
  float x = art.x + is + DP(12);
  char t[32];
  if (live) fm_strlcpy(t, "LIVE", sizeof t);
  else fmt_len(dur, t, sizeof t);
  float tw = t[0] ? font_width(FONT_REGULAR, ui.m.font_small, t, -1) + DP(10) : 0;
  float right = r.x + r.w - tw;
  float lh = font_line_h(ui.m.font), lhs = font_line_h(ui.m.font_small);
  bool two = sub && sub[0];
  float y0 = r.y + (r.h - lh - (two ? lhs + DP(2) : 0)) * 0.5f;
  FmColor tc = accent ? T.accent : dim ? T.text2 : T.text;
  font_draw_ellipsis(accent ? FONT_BOLD : FONT_REGULAR, ui.m.font, x, y0, title, right - x - DP(4), tc);
  if (two) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y0 + lh + DP(2), sub, right - x - DP(4), T.text2);
  if (t[0])
    font_draw(live ? FONT_BOLD : FONT_REGULAR, ui.m.font_small, right, r.y + (r.h - lhs) * 0.5f, t, -1,
              live ? T.danger : T.text3);
}

/* ---- menus and dialogs ---------------------------------------------------------------- */

enum {
  QM_PLAY = 1, QM_NEXT, QM_UP, QM_DOWN, QM_ADDPL, QM_REMOVE,        /* a queue row */
  PM_PLAY_FROM = 20, PM_NEXT, PM_QUEUE, PM_UP, PM_DOWN, PM_REMOVE,    /* a playlist's track */
  LM_PLAY = 40, LM_SHUFFLE, LM_NEXT, LM_QUEUE, LM_RENAME, LM_EXPORT, LM_DELETE,   /* a playlist */
  PK_NEW = 60, PK_LIST = 100                                           /* the picker */
};
enum { DK_NONE, DK_NEW, DK_SAVEQ, DK_RENAME, DK_DELETE };

static struct {
  bool init;
  u32 id_qmenu, id_pmenu, id_lmenu, id_pick;
  int menu_pos;               /* queue position / track index the menu is for */
  int menu_pl;                /* playlist the menu is for */
  /* the picker's tracks (deep copies) */
  FmAudioEntry *pend;
  int npend;
  /* dialog */
  int dlg;
  int dlg_pl;
  char name[PL_NAME_MAX];
  char err[120];
  /* views */
  Rl queue, tracks;
  FmScroll lists_scroll;
  int open_pl;                /* the library's open playlist, -1 = the list of them */
  char open_name[PL_NAME_MAX];
  int qdrag_cur;              /* queue position heard when a drag began */
} S;

static void ids(void) {
  if (S.init) return;
  S.init = true;
  S.queue.drag = S.tracks.drag = -1;
  S.queue.focus = S.tracks.focus = -1;
  S.open_pl = -1;
  S.id_qmenu = ui_id("q.menu");
  S.id_pmenu = ui_id("q.pmenu");
  S.id_lmenu = ui_id("q.lmenu");
  S.id_pick = ui_id("q.pick");
}

static void pend_clear(void) {
  qents_free(S.pend, S.npend);
  S.pend = NULL;
  S.npend = 0;
}

static void open_dialog(int kind, int pl, const char *name) {
  S.dlg = kind;
  S.dlg_pl = pl;
  fm_strlcpy(S.name, name ? name : "", sizeof S.name);
  S.err[0] = 0;
  ui_redraw();
}

static const char *plural(int n, const char *one, const char *many, char *buf, size_t cap) {
  fm_snprintf(buf, cap, "%d %s", n, n == 1 ? one : many);
  return buf;
}

static void toast_added(int took, int p) {
  char b[32];
  if (took > 0) ui_toast("Added %s to \xE2\x80\x9C%s\xE2\x80\x9D", plural(took, "track", "tracks", b, sizeof b), pl_name(p));
  else ui_toast("Nothing there a playlist can keep");
}

void qui_pick_playlist(const FmAudioEntry *e, int n, float x, float y) {
  ids();
  if (!e || n <= 0) return;
  pend_clear();
  S.pend = qents_dup(e, n);
  S.npend = n;
  FmMenuItem m[34];
  int k = 0;
  memset(m, 0, sizeof m);
  m[k].id = PK_NEW; m[k].icon = IC_PLUS; m[k++].label = "New playlist\xE2\x80\xA6";
  if (pl_count() > 0) m[k++].flags = UI_MI_SEP;
  for (int i = 0; i < pl_count() && k < FM_COUNT(m); i++) {
    m[k].id = PK_LIST + i; m[k].icon = IC_QUEUE; m[k++].label = pl_name(i);
  }
  ui_menu_open(S.id_pick, x, y, m, k);
}

void qui_pick_playlist_paths(const char *const *paths, int n, float x, float y) {
  int k = 0;
  FmAudioEntry *e = qents_from_paths(paths, n, 5000, &k);
  if (k == 0) ui_toast("No music there");
  else qui_pick_playlist(e, k, x, y);
  qents_free(e, k);
}

void qui_queue_paths(const char *const *paths, int n, bool next) {
  ids();
  int k = 0;
  FmAudioEntry *e = qents_from_paths(paths, n, 5000, &k);
  char b[32];
  if (k == 0) ui_toast("No music there");
  else if (audio_queue_add(e, k, next ? AQ_NEXT : AQ_END))
    ui_toast(next ? "%s to play next" : "Added %s to the queue", plural(k, "track", "tracks", b, sizeof b));
  qents_free(e, k);
}

void qui_save_queue(void) {
  ids();
  if (audio_queue_len() == 0) { ui_toast("The queue is empty"); return; }
  char name[PL_NAME_MAX];
  pl_unique_name(NULL, name, sizeof name);
  open_dialog(DK_SAVEQ, -1, name);
}

static void play_playlist(int p, int from, bool shuffle) {
  int n = 0;
  FmAudioEntry *e = pl_entries(p, &n);
  bool ok = n > 0 && (shuffle ? audio_play_shuffled(e, n) : audio_play_entries(e, n, FM_CLAMP(from, 0, n - 1)));
  fm_free(e);
  if (n == 0) ui_toast("This playlist is empty");
  else if (!ok) ui_toast("No audio output on this device");
  else audio_show_player();
}

static void queue_playlist(int p, bool next) {
  int n = 0;
  FmAudioEntry *e = pl_entries(p, &n);
  char b[32];
  if (n > 0 && audio_queue_add(e, n, next ? AQ_NEXT : AQ_END))
    ui_toast(next ? "%s to play next" : "Added %s to the queue", plural(n, "track", "tracks", b, sizeof b));
  fm_free(e);
}

static void open_list_menu(int p, float x, float y) {
  S.menu_pl = p;
  FmMenuItem m[10];
  int k = 0;
  memset(m, 0, sizeof m);
  m[k].id = LM_PLAY; m[k].icon = IC_PLAY; m[k++].label = "Play";
  m[k].id = LM_SHUFFLE; m[k].icon = IC_SHUFFLE; m[k++].label = "Shuffle play";
  m[k].id = LM_NEXT; m[k].icon = IC_PLAY_NEXT; m[k++].label = "Play next";
  m[k].id = LM_QUEUE; m[k].icon = IC_QUEUE; m[k++].label = "Add to queue";
  m[k++].flags = UI_MI_SEP;
  m[k].id = LM_RENAME; m[k].icon = IC_RENAME; m[k++].label = "Rename";
  if (pl_local_only(p)) { m[k].id = LM_EXPORT; m[k].icon = IC_SHARE; m[k++].label = "Export as .m3u8"; }
  m[k].id = LM_DELETE; m[k].icon = IC_DELETE; m[k].label = "Delete playlist"; m[k++].flags = UI_MI_DANGER;
  ui_menu_open(S.id_lmenu, x, y, m, k);
}

static void export_m3u(int p) {
  char dir[FM_PATH_MAX], path[FM_PATH_MAX], file[PL_NAME_MAX + 8];
  if (!plat_place(PLACE_MUSIC, dir, sizeof dir) && !plat_place(PLACE_DOCUMENTS, dir, sizeof dir)) return;
  plat_mkdirs(dir);
  /* a file name from the playlist's name */
  fm_strlcpy(file, pl_name(p), sizeof file);
  for (char *c = file; *c; c++)
    if (strchr("\\/:*?\"<>|", *c)) *c = '_';
  fm_strlcat(file, ".m3u8", sizeof file);
  if (!fm_path_join(path, sizeof path, dir, file)) return;
  if (pl_export_m3u(p, path)) ui_toast("Saved %s", path);
  else ui_toast("Couldn't write %s", path);
}

static void menu_results(void) {
  int r = ui_menu_result(S.id_qmenu);
  int pos = S.menu_pos;
  switch (r) {
    case QM_PLAY: audio_queue_jump(pos); break;
    case QM_NEXT: audio_queue_move(pos, audio_queue_pos() + (pos > audio_queue_pos() ? 1 : 0)); break;
    case QM_UP: if (pos - 1 > audio_queue_pos()) audio_queue_move(pos, pos - 1); break;
    case QM_DOWN: if (pos + 1 < audio_queue_len()) audio_queue_move(pos, pos + 1); break;
    case QM_REMOVE: audio_queue_remove(pos); break;
    case QM_ADDPL: {
      int n = 0;
      FmAudioEntry *all = audio_queue_entries(&n);
      if (pos >= 0 && pos < n) qui_pick_playlist(&all[pos], 1, ui.mx, ui.my);
      fm_free(all);
      break;
    }
    default: break;
  }
  r = ui_menu_result(S.id_pmenu);
  int p = S.open_pl, k = S.menu_pos;
  switch (r) {
    case PM_PLAY_FROM: play_playlist(p, k, false); break;
    case PM_NEXT: case PM_QUEUE: {
      const FmQItem *it = pl_item(p, k);
      if (it) {
        FmAudioEntry e = qitem_entry(it);
        if (audio_queue_add(&e, 1, r == PM_NEXT ? AQ_NEXT : AQ_END))
          ui_toast(r == PM_NEXT ? "Plays next" : "Added to the queue");
      }
      break;
    }
    case PM_UP: pl_move(p, k, k - 1); break;
    case PM_DOWN: pl_move(p, k, k + 1); break;
    case PM_REMOVE: pl_remove(p, k); break;
    default: break;
  }
  r = ui_menu_result(S.id_lmenu);
  p = S.menu_pl;
  switch (r) {
    case LM_PLAY: play_playlist(p, 0, false); break;
    case LM_SHUFFLE: play_playlist(p, 0, true); break;
    case LM_NEXT: queue_playlist(p, true); break;
    case LM_QUEUE: queue_playlist(p, false); break;
    case LM_RENAME: open_dialog(DK_RENAME, p, pl_name(p)); break;
    case LM_EXPORT: export_m3u(p); break;
    case LM_DELETE: open_dialog(DK_DELETE, p, pl_name(p)); break;
    default: break;
  }
  r = ui_menu_result(S.id_pick);
  if (r == PK_NEW) {
    char name[PL_NAME_MAX];
    pl_unique_name(NULL, name, sizeof name);
    open_dialog(DK_NEW, -1, name);
  } else if (r >= PK_LIST && r - PK_LIST < pl_count()) {
    toast_added(pl_add(r - PK_LIST, S.pend, S.npend), r - PK_LIST);
    pend_clear();
  } else if (r >= 0) {
    pend_clear();
  }
}

static float field_h(void) { return DP(ui.touch_mode ? 48 : 40); }
static float btn_h(void) { return DP(ui.touch_mode ? 46 : 38); }

static void dialog_name(void) {
  const char *title = S.dlg == DK_RENAME ? "Rename playlist" : S.dlg == DK_SAVEQ ? "Save queue as playlist" :
                      "New playlist";
  const char *labels[] = { "Cancel", S.dlg == DK_RENAME ? "Rename" : S.dlg == DK_SAVEQ ? "Save" : "Create" };
  float ch = font_line_h(ui.m.font_small) * 2 + DP(14) + field_h() + DP(10) + btn_h();
  float h_dp = (ch + DP(40) + font_line_h(ui.m.font_title) + DP(12)) / ui.scale + 2;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id(title), title, 420, h_dp, &cancel);
  char sub[96], b[32];
  if (S.dlg == DK_SAVEQ) fm_snprintf(sub, sizeof sub, "%s from the queue", plural(audio_queue_len(), "track", "tracks", b, sizeof b));
  else if (S.dlg == DK_NEW && S.npend > 0) fm_snprintf(sub, sizeof sub, "With %s", plural(S.npend, "track", "tracks", b, sizeof b));
  else fm_strlcpy(sub, "Name", sizeof sub);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(2), c.y, sub, c.w, T.text2);
  rect_cut_top(&c, font_line_h(ui.m.font_small) + DP(6));
  FmRect f = rect_cut_top(&c, field_h());
  int r = ui_textfield(ui_id("q.dlg.name"), f, S.name, sizeof S.name, "Playlist name", UI_TF_FOCUS);
  if (r & UI_TF_CHANGED) S.err[0] = 0;
  rect_cut_top(&c, DP(8));
  if (S.err[0]) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(2), c.y, S.err, c.w, T.danger);
  int bi = ui_dialog_buttons(c, labels, 2);
  ui_dialog_end();
  if (cancel || bi == 0 || (r & UI_TF_CANCEL)) {
    S.dlg = DK_NONE;
    pend_clear();
    ui_focus(0);
    return;
  }
  if (bi != 1 && !(r & UI_TF_SUBMIT)) return;
  int other = pl_find(S.name);
  bool empty = true;
  for (const char *s = S.name; *s; s++) if (*s != ' ') empty = false;
  if (empty) { fm_strlcpy(S.err, "Give it a name", sizeof S.err); return; }
  if (S.dlg == DK_RENAME) {
    if (other >= 0 && other != S.dlg_pl) { fm_strlcpy(S.err, "There is a playlist with that name", sizeof S.err); return; }
    pl_rename(S.dlg_pl, S.name);
  } else {
    if (other >= 0) { fm_strlcpy(S.err, "There is a playlist with that name", sizeof S.err); return; }
    int p = pl_create(S.name);
    if (p < 0) return;
    if (S.dlg == DK_SAVEQ) {
      int n = 0;
      FmAudioEntry *all = audio_queue_entries(&n);
      int took = pl_add(p, all, n);
      fm_free(all);
      char b2[32];
      ui_toast("Saved \xE2\x80\x9C%s\xE2\x80\x9D with %s", pl_name(p), plural(took, "track", "tracks", b2, sizeof b2));
    } else if (S.npend > 0) {
      toast_added(pl_add(p, S.pend, S.npend), p);
    } else {
      ui_toast("Created \xE2\x80\x9C%s\xE2\x80\x9D", pl_name(p));
    }
    pend_clear();
    if (S.open_pl >= 0) S.open_pl++;     /* the new one went on top */
  }
  S.dlg = DK_NONE;
  ui_focus(0);
}

static void dialog_delete(void) {
  const char *labels[] = { "Cancel", "Delete" };
  char t[PL_NAME_MAX + 40];
  fm_snprintf(t, sizeof t, "Delete \xE2\x80\x9C%s\xE2\x80\x9D?", pl_name(S.dlg_pl));
  float ch = font_line_h(ui.m.font) * 2 + DP(18) + btn_h();
  float h_dp = (ch + DP(40) + font_line_h(ui.m.font_title) + DP(12)) / ui.scale + 2;
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("q.dlg.del"), t, 420, h_dp, &cancel);
  font_draw_wrap(FONT_REGULAR, ui.m.font, c.x, c.y, c.w, "The songs stay where they are; only the list goes.", T.text2, true);
  int bi = ui_dialog_buttons(c, labels, 2);
  ui_dialog_end();
  if (cancel || bi == 0) { S.dlg = DK_NONE; return; }
  if (bi == 1) {
    int p = S.dlg_pl;
    pl_delete(p);
    if (S.open_pl == p) S.open_pl = -1;
    else if (S.open_pl > p) S.open_pl--;
    S.dlg = DK_NONE;
  }
}

void qui_overlay(void) {
  ids();
  menu_results();
  if (S.dlg == DK_NEW || S.dlg == DK_SAVEQ || S.dlg == DK_RENAME) dialog_name();
  else if (S.dlg == DK_DELETE) dialog_delete();
}

/* ---- the queue panel ------------------------------------------------------------------- */

typedef struct QRowCtx { int base; } QRowCtx;

static void queue_row(void *u, int i, FmRect r, bool lifted) {
  FM_UNUSED(lifted);
  QRowCtx *c = (QRowCtx *)u;
  FmAudioQInfo q;
  if (!audio_queue_get(c->base + i, &q)) return;
  /* the More button sits between the text and the grip */
  float mb = FM_MIN(r.h, ui.m.hit);
  FmRect more = rect_cut_right(&r, mb);
  track_row(r, q.title, q.artist, q.art_url, q.path, q.file, q.live, q.dur, false, false);
  if (!lifted && ui_icon_btn(ui_idn(ui_id("q.rowmore"), (u32)(c->base + i)), more, IC_MORE, T.text2, "More")) {
    S.menu_pos = c->base + i;
    FmMenuItem m[8];
    int k = 0;
    memset(m, 0, sizeof m);
    m[k].id = QM_PLAY; m[k].icon = IC_PLAY; m[k++].label = "Play now";
    m[k].id = QM_NEXT; m[k].icon = IC_PLAY_NEXT; m[k++].label = "Play next";
    m[k].id = QM_UP; m[k].icon = IC_ARROW_UP; m[k].label = "Move up"; m[k].shortcut = ui.touch_mode ? NULL : "Alt+Up";
    m[k++].flags = i == 0 ? UI_MI_DISABLED : 0;
    m[k].id = QM_DOWN; m[k].icon = IC_ARROW_DOWN; m[k].label = "Move down"; m[k].shortcut = ui.touch_mode ? NULL : "Alt+Down";
    m[k++].flags = c->base + i + 1 >= audio_queue_len() ? UI_MI_DISABLED : 0;
    m[k].id = QM_ADDPL; m[k].icon = IC_PLAYLIST_ADD; m[k++].label = "Add to playlist\xE2\x80\xA6";
    m[k++].flags = UI_MI_SEP;
    m[k].id = QM_REMOVE; m[k].icon = IC_CLOSE; m[k].label = "Remove from queue"; m[k].shortcut = ui.touch_mode ? NULL : "Del";
    m[k++].flags = UI_MI_DANGER;
    ui_menu_open(S.id_qmenu, more.x + more.w, more.y + more.h, m, k);
  }
}

static void section_label(FmRect *r, const char *s, const char *right) {
  FmRect l = rect_cut_top(r, DP(30));
  float fs = ui.m.font_small;
  font_draw(FONT_BOLD, fs, l.x + DP(12), l.y + l.h - font_line_h(fs) - DP(4), s, -1, T.text3);
  if (right && right[0])
    font_draw(FONT_REGULAR, fs, l.x + l.w - DP(12) - font_width(FONT_REGULAR, fs, right, -1),
              l.y + l.h - font_line_h(fs) - DP(4), right, -1, T.text3);
}

void qui_queue_panel(FmRect area) {
  ids();
  gfx_rrect(area, ui.m.radius, T.surface);
  FmRect r = rect_inset2(area, DP(6), DP(4));
  int len = audio_queue_len(), cur = audio_queue_pos();
  /* header: title and count, save and clear */
  FmRect hdr = rect_cut_top(&r, DP(48));
  float bs = FM_MIN(hdr.h, DP(ui.touch_mode ? 46 : 38));
  FmRect in = rect_inset2(hdr, DP(10), 0);
  u32 base = ui_id("q.hdr");
  int upnext = cur >= 0 ? len - cur - 1 : 0;
  if (upnext > 0 && ui_icon_btn(ui_idn(base, 1), rect_center(rect_cut_right(&in, bs), bs, bs), IC_DELETE, T.text2,
                                "Clear up next")) {
    audio_queue_clear();
    ui_toast("Up next cleared");
  }
  if (len > 0 && ui_icon_btn(ui_idn(base, 2), rect_center(rect_cut_right(&in, bs), bs, bs), IC_PLAYLIST_ADD, T.text2,
                             "Save queue as playlist"))
    qui_save_queue();
  double total = 0;
  for (int i = 0; i < len; i++) {
    FmAudioQInfo q;
    if (audio_queue_get(i, &q)) total += q.dur;
  }
  char cnt[64], b[32], tl[32];
  if (total > 0) {
    view_fmt_time(total, tl, sizeof tl);
    fm_snprintf(cnt, sizeof cnt, "%s  \xC2\xB7  %s", plural(len, "track", "tracks", b, sizeof b), tl);
  } else {
    plural(len, "track", "tracks", cnt, sizeof cnt);
  }
  float th = font_line_h(ui.m.font), sh = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - th - sh) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, in.x + DP(2), y, "Queue", in.w, T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(2), y + th, cnt, in.w, T.text2);
  ui_divider(r.x + DP(8), r.x + r.w - DP(8), r.y);
  if (cur < 0) return;
  /* now playing */
  section_label(&r, "NOW PLAYING", NULL);
  FmAudioQInfo q;
  FmRect np = rect_cut_top(&r, row_h());
  if (audio_queue_get(cur, &q)) {
    FmRect bg = rect_inset2(np, DP(2), DP(2));
    gfx_rrect(bg, FM_MIN(DP(10), bg.h * 0.5f), T.accent_soft);
    u32 nid = ui_id("q.now");
    int f = ui_hit(nid, np);
    FmRect c = np;
    rect_cut_right(&c, grip_w());
    track_row(c, q.title, q.artist, q.art_url, q.path, q.file, q.live, q.dur, true, false);
    float is = DP(18);
    icon_draw(IC_WAVEFORM, FM_RECT(np.x + np.w - grip_w() + (grip_w() - is) * 0.5f, np.y + (np.h - is) * 0.5f, is, is),
              T.accent);
    if ((f & UI_RCLICK) || (f & UI_LONG)) {
      S.menu_pos = cur;
      FmMenuItem m[3];
      memset(m, 0, sizeof m);
      m[0].id = QM_ADDPL; m[0].icon = IC_PLAYLIST_ADD; m[0].label = "Add to playlist\xE2\x80\xA6";
      m[1].id = QM_REMOVE; m[1].icon = IC_CLOSE; m[1].label = "Remove from queue"; m[1].flags = UI_MI_DANGER;
      ui_menu_open(S.id_qmenu, ui.mx, ui.my, m, 2);
    }
  }
  /* up next */
  char right[48];
  right[0] = 0;
  if (upnext > 0) plural(upnext, "track", "tracks", right, sizeof right);
  section_label(&r, "UP NEXT", right);
  if (upnext == 0) {
    FmRect t = rect_inset2(rect_cut_top(&r, DP(80)), DP(14), DP(6));
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, t.x, t.y, t.w,
                   "Nothing up next. Use \xE2\x80\x9C" "Add to queue\xE2\x80\x9D or \xE2\x80\x9CPlay next\xE2\x80\x9D on songs "
                   "in your files, the media library or online audio.", T.text3, true);
    return;
  }
  /* positions shift when the track changes during a drag: keep the start's */
  int qbase = S.queue.drag >= 0 ? S.qdrag_cur + 1 : cur + 1;
  QRowCtx ctx = { qbase };
  RlRes res = rl_draw(&S.queue, ui_id("q.list"), r, len - qbase, queue_row, &ctx);
  if (S.queue.drag < 0) S.qdrag_cur = cur;
  if (res.from >= 0) audio_queue_move(qbase + res.from, qbase + res.to);
  if (res.click >= 0) audio_queue_jump(qbase + res.click);
  if (res.remove >= 0) audio_queue_remove(qbase + res.remove);
  if (res.menu >= 0) {
    S.menu_pos = qbase + res.menu;
    FmMenuItem m[6];
    int k = 0;
    memset(m, 0, sizeof m);
    m[k].id = QM_PLAY; m[k].icon = IC_PLAY; m[k++].label = "Play now";
    m[k].id = QM_NEXT; m[k].icon = IC_PLAY_NEXT; m[k++].label = "Play next";
    m[k].id = QM_UP; m[k].icon = IC_ARROW_UP; m[k].label = "Move up"; m[k++].flags = res.menu == 0 ? UI_MI_DISABLED : 0;
    m[k].id = QM_DOWN; m[k].icon = IC_ARROW_DOWN; m[k].label = "Move down";
    m[k++].flags = qbase + res.menu + 1 >= len ? UI_MI_DISABLED : 0;
    m[k].id = QM_ADDPL; m[k].icon = IC_PLAYLIST_ADD; m[k++].label = "Add to playlist\xE2\x80\xA6";
    m[k].id = QM_REMOVE; m[k].icon = IC_CLOSE; m[k].label = "Remove from queue"; m[k++].flags = UI_MI_DANGER;
    ui_menu_open(S.id_qmenu, res.mx, res.my, m, k);
  }
}

/* ---- the Playlists section --------------------------------------------------------------- */

void qui_playlists_reset(void) {
  ids();
  S.open_pl = -1;
  S.tracks.drag = -1;
  S.tracks.focus = -1;
}

bool qui_playlists_back(void) {
  ids();
  if (S.open_pl < 0) return false;
  S.open_pl = -1;
  ui_redraw();
  return true;
}

static void list_tile(FmRect b, const char *name) {
  u32 h = ui_id(name);
  FmColor a = kAccents[h % UI_ACCENTS];
  gfx_rrect_vgrad(b, DP(10), col_mix(a, FM_HEX(0xFFFFFF), 0.15f), col_mix(a, FM_HEX(0x101018), 0.30f));
  float is = b.w * 0.5f;
  icon_draw(IC_QUEUE, rect_center(b, is, is), FM_RGBA(255, 255, 255, 230));
}

static void list_sub(int p, char *out, size_t cap) {
  char b[32], t[32];
  double d = pl_duration(p);
  plural(pl_len(p), "track", "tracks", b, sizeof b);
  if (d > 0) {
    view_fmt_time(d, t, sizeof t);
    fm_snprintf(out, cap, "%s  \xC2\xB7  %s", b, t);
  } else {
    fm_strlcpy(out, b, cap);
  }
}

static void new_playlist(void) {
  pend_clear();
  char name[PL_NAME_MAX];
  pl_unique_name(NULL, name, sizeof name);
  open_dialog(DK_NEW, -1, name);
}

static float hdr_h(void) { return DP(ui.touch_mode ? 56 : 50); }

static void lists_view(FmRect body) {
  u32 base = ui_id("q.lists");
  FmRect h = rect_cut_top(&body, hdr_h());
  FmRect in = rect_inset2(h, DP(10), 0);
  const char *l = "New playlist";
  float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(48);
  bool icon_only = in.w < DP(320);
  float bs = DP(ui.touch_mode ? 44 : 36);
  FmRect b = rect_center(rect_cut_right(&in, icon_only ? bs : bw), icon_only ? bs : bw, DP(ui.touch_mode ? 40 : 34));
  if (ui_button(ui_idn(base, 1), b, IC_PLUS, icon_only ? NULL : l, UI_BTN_FILLED)) new_playlist();
  char cnt[32];
  plural(pl_count(), "playlist", "playlists", cnt, sizeof cnt);
  float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x + DP(4), y, "Playlists", in.w - DP(8), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(4), y + tl, cnt, in.w - DP(8), T.text2);
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  int n = pl_count();
  if (n == 0) {
    float is = DP(52);
    float cx = body.x + body.w * 0.5f, yy = body.y + FM_MAX(DP(10), body.h * 0.25f);
    gfx_circle(cx, yy + is * 0.5f, is * 0.62f, col_alpha(T.accent, 0.10f));
    icon_draw(IC_QUEUE, FM_RECT(cx - is * 0.3f, yy + is * 0.2f, is * 0.6f, is * 0.6f), T.accent);
    yy += is + DP(14);
    ui_label(FM_RECT(body.x + DP(12), yy, body.w - DP(24), font_line_h(ui.m.font_title)), "No playlists yet", FONT_BOLD,
             ui.m.font_title, T.text, UI_CENTER);
    yy += font_line_h(ui.m.font_title) + DP(4);
    const char *sub = "Save the queue as a playlist, or use \xE2\x80\x9C" "Add to playlist\xE2\x80\x9D on songs and "
                      "online tracks. Open an .m3u file's menu in the files to import it.";
    float w = FM_MIN(body.w - DP(48), DP(460));
    font_draw_wrap(FONT_REGULAR, ui.m.font, cx - w * 0.5f, yy, w, sub, T.text2, true);
    return;
  }
  float rh = FM_MAX(ui.m.row_h, DP(ui.touch_mode ? 68 : 60)), pad = DP(4);
  float content = rh * (float)n + pad * 2;
  ui_scroll(&S.lists_scroll, base, body, content);
  gfx_clip_push(body);
  int first = FM_MAX(0, (int)((S.lists_scroll.y - pad) / rh));
  int last = FM_MIN(n - 1, (int)((S.lists_scroll.y + body.h) / rh) + 1);
  for (int i = first; i <= last; i++) {
    FmRect row = { body.x + DP(6), body.y + pad + (float)i * rh - S.lists_scroll.y, body.w - DP(12), rh };
    u32 id = ui_idn(base, (u32)i + 10);
    int f = ui_hit(id, rect_inset2(row, 0, DP(1)));
    FmRect bg = rect_inset2(row, 0, DP(1.5f));
    float rad = FM_MIN(DP(10), bg.h * 0.5f);
    if (f & UI_HELD) gfx_rrect(bg, rad, T.press);
    else if (f & UI_HOVER) gfx_rrect(bg, rad, T.hover);
    float is = FM_MIN(rh - DP(14), DP(48));
    list_tile(FM_RECT(row.x + DP(8), row.y + (rh - is) * 0.5f, is, is), pl_name(i));
    float hb = FM_MIN(rh, ui.m.hit);
    FmRect more = { row.x + row.w - hb, row.y + (rh - hb) * 0.5f, hb, hb };
    FmRect play = { more.x - hb, more.y, hb, hb };
    float x = row.x + DP(8) + is + DP(12), right = play.x - DP(4);
    char sub[96];
    list_sub(i, sub, sizeof sub);
    float lh = font_line_h(ui.m.font), lhs = font_line_h(ui.m.font_small);
    float y0 = row.y + (rh - lh - lhs - DP(2)) * 0.5f;
    font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y0, pl_name(i), right - x, T.text);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y0 + lh + DP(2), sub, right - x, T.text2);
    bool pb = pl_len(i) > 0 && ui_icon_btn(ui_idn(id, 1), play, IC_PLAY, T.accent, "Play");
    bool mb = ui_icon_btn(ui_idn(id, 2), more, IC_MORE, T.text2, "More");
    if (pb) play_playlist(i, 0, false);
    else if (mb) open_list_menu(i, more.x + more.w, more.y + more.h);
    else if (f & UI_CLICK) {
      S.open_pl = i;
      fm_strlcpy(S.open_name, pl_name(i), sizeof S.open_name);
      S.tracks.scroll.y = 0;
      S.tracks.drag = -1;
      S.tracks.focus = -1;
      ui_redraw();
    } else if ((f & UI_RCLICK) || (f & UI_LONG)) {
      open_list_menu(i, ui.mx, ui.my);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&S.lists_scroll, body, content);
}

typedef struct PRowCtx { int p; } PRowCtx;

static void pl_row(void *u, int i, FmRect r, bool lifted) {
  PRowCtx *c = (PRowCtx *)u;
  const FmQItem *it = pl_item(c->p, i);
  if (!it) return;
  float mb = FM_MIN(r.h, ui.m.hit);
  FmRect more = rect_cut_right(&r, mb);
  /* files without a title show their name */
  char name[256];
  if (it->title[0]) fm_strlcpy(name, it->title, sizeof name);
  else {
    fm_strlcpy(name, fm_path_base(it->url), sizeof name);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
  }
  const char *sub = it->artist[0] ? it->artist : it->album;
  bool missing = it->file && !lifted && !plat_exists(it->url);
  track_row(r, name, missing ? "Not found" : sub, it->art, it->url, it->file, it->live, it->dur, false, missing);
  if (!lifted && ui_icon_btn(ui_idn(ui_id("q.plmore"), (u32)i), more, IC_MORE, T.text2, "More")) {
    S.menu_pos = i;
    FmMenuItem m[8];
    int k = 0;
    memset(m, 0, sizeof m);
    m[k].id = PM_PLAY_FROM; m[k].icon = IC_PLAY; m[k++].label = "Play from here";
    m[k].id = PM_NEXT; m[k].icon = IC_PLAY_NEXT; m[k++].label = "Play next";
    m[k].id = PM_QUEUE; m[k].icon = IC_QUEUE; m[k++].label = "Add to queue";
    m[k].id = PM_UP; m[k].icon = IC_ARROW_UP; m[k].label = "Move up"; m[k].shortcut = ui.touch_mode ? NULL : "Alt+Up";
    m[k++].flags = i == 0 ? UI_MI_DISABLED : 0;
    m[k].id = PM_DOWN; m[k].icon = IC_ARROW_DOWN; m[k].label = "Move down"; m[k].shortcut = ui.touch_mode ? NULL : "Alt+Down";
    m[k++].flags = i + 1 >= pl_len(c->p) ? UI_MI_DISABLED : 0;
    m[k++].flags = UI_MI_SEP;
    m[k].id = PM_REMOVE; m[k].icon = IC_CLOSE; m[k].label = "Remove from playlist"; m[k].shortcut = ui.touch_mode ? NULL : "Del";
    m[k++].flags = UI_MI_DANGER;
    ui_menu_open(S.id_pmenu, more.x + more.w, more.y + more.h, m, k);
  }
}

static void playlist_view(FmRect body) {
  int p = S.open_pl;
  u32 base = ui_id("q.pl");
  FmRect h = rect_cut_top(&body, hdr_h());
  FmRect in = rect_inset2(h, DP(10), 0);
  float bs = DP(ui.touch_mode ? 44 : 36);
  if (ui_icon_btn(ui_idn(base, 1), rect_center(rect_cut_left(&in, bs), bs, bs), IC_BACK, T.text2, "Back")) {
    qui_playlists_back();
    return;
  }
  rect_cut_left(&in, DP(4));
  FmRect mb = rect_center(rect_cut_right(&in, bs), bs, bs);
  if (ui_icon_btn(ui_idn(base, 2), mb, IC_MORE, T.text2, "More")) open_list_menu(p, mb.x + mb.w, mb.y + mb.h);
  int n = pl_len(p);
  if (n > 0) {
    bool narrow = in.w < DP(420);
    FmRect qb = rect_center(rect_cut_right(&in, bs), bs, bs);
    if (ui_icon_btn(ui_idn(base, 3), qb, IC_QUEUE, T.text2, "Add to queue")) queue_playlist(p, false);
    FmRect sb = rect_center(rect_cut_right(&in, bs), bs, bs);
    if (ui_icon_btn(ui_idn(base, 4), sb, IC_SHUFFLE, T.text2, "Shuffle play")) play_playlist(p, 0, true);
    rect_cut_right(&in, DP(4));
    const char *l = "Play";
    float bw = narrow ? bs : font_width(FONT_BOLD, ui.m.font, l, -1) + DP(48);
    FmRect b = rect_center(rect_cut_right(&in, bw), bw, DP(ui.touch_mode ? 40 : 34));
    if (ui_button(ui_idn(base, 5), b, IC_PLAY, narrow ? NULL : l, UI_BTN_FILLED)) play_playlist(p, 0, false);
    rect_cut_right(&in, DP(6));
  }
  char sub[96];
  list_sub(p, sub, sizeof sub);
  float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
  float y = in.y + (in.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, in.x + DP(4), y, pl_name(p), in.w - DP(8), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(4), y + tl, sub, in.w - DP(8), T.text2);
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), body.y);
  rect_cut_bottom(&body, DP(4));
  if (n == 0) {
    FmRect t = rect_inset2(rect_cut_top(&body, DP(120)), DP(24), DP(24));
    font_draw_wrap(FONT_REGULAR, ui.m.font, t.x, t.y, t.w,
                   "This playlist is empty. Add songs with \xE2\x80\x9C" "Add to playlist\xE2\x80\x9D from the files, the "
                   "media library, online audio or the player's menu.", T.text2, true);
    return;
  }
  PRowCtx ctx = { p };
  RlRes res = rl_draw(&S.tracks, ui_idn(base, 100 + (u32)p), rect_inset2(body, DP(6), 0), n, pl_row, &ctx);
  if (res.from >= 0) pl_move(p, res.from, res.to);
  if (res.remove >= 0) pl_remove(p, res.remove);
  if (res.click >= 0) play_playlist(p, res.click, false);
  if (res.menu >= 0) {
    S.menu_pos = res.menu;
    FmMenuItem m[7];
    int k = 0;
    memset(m, 0, sizeof m);
    m[k].id = PM_PLAY_FROM; m[k].icon = IC_PLAY; m[k++].label = "Play from here";
    m[k].id = PM_NEXT; m[k].icon = IC_PLAY_NEXT; m[k++].label = "Play next";
    m[k].id = PM_QUEUE; m[k].icon = IC_QUEUE; m[k++].label = "Add to queue";
    m[k].id = PM_UP; m[k].icon = IC_ARROW_UP; m[k].label = "Move up"; m[k++].flags = res.menu == 0 ? UI_MI_DISABLED : 0;
    m[k].id = PM_DOWN; m[k].icon = IC_ARROW_DOWN; m[k].label = "Move down"; m[k++].flags = res.menu + 1 >= n ? UI_MI_DISABLED : 0;
    m[k].id = PM_REMOVE; m[k].icon = IC_CLOSE; m[k].label = "Remove from playlist"; m[k++].flags = UI_MI_DANGER;
    ui_menu_open(S.id_pmenu, res.mx, res.my, m, k);
  }
}

void qui_playlists(FmRect body) {
  ids();
  /* the open playlist may have moved (one created or deleted): follow its name */
  if (S.open_pl >= 0 && (S.open_pl >= pl_count() || strcmp(pl_name(S.open_pl), S.open_name) != 0)) {
    int k = pl_find(S.open_name);
    if (k >= 0 && strcmp(pl_name(k), S.open_name) == 0) S.open_pl = k;
    else if (S.open_pl < pl_count()) fm_strlcpy(S.open_name, pl_name(S.open_pl), sizeof S.open_name);
    else S.open_pl = -1;
  }
  if (S.open_pl >= 0) playlist_view(body);
  else lists_view(body);
}

/* ---- screenshots -------------------------------------------------------------------- */

/* --demo-queue-state: "drag" lifts the second up-next row half way down,
** "menu" opens the first row's menu; "playlist" opens the first playlist. */
void qui_demo(const char *state) {
  ids();
  if (!state) return;
  if (!strcmp(state, "drag") && audio_queue_len() - audio_queue_pos() > 3) {
    S.queue.pinned = true;
    S.queue.drag = 1;
    S.queue.drag_top = row_h() * 2.45f;
    S.queue.grab_dy = row_h() * 0.5f;
    S.qdrag_cur = audio_queue_pos();
  } else if (!strcmp(state, "playlist") && pl_count() > 0) {
    S.open_pl = 0;
    fm_strlcpy(S.open_name, pl_name(0), sizeof S.open_name);
  }
}
