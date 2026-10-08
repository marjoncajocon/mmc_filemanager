/* fphoto_album.c -- online photos: Favorites and local albums.
**
** Design decisions:
**   - Albums are local, like Google Photos albums, but they keep the
**     photo's metadata (source, id, title, author, licence, the thumbnail,
**     full picture and page links, its size and colour), not the picture:
**     the thumbnails and full pictures live in the caches, so a cached
**     album opens offline, and "Download album" saves everything.
**   - One plain text file next to the settings, PLACE_CONFIG/photo-albums.txt,
**     tab separated, one line per album and per photo, newest first:
**
**       # mmcfm online photos: favorites and albums
**       favorites
**       photo<TAB>source<TAB>id<TAB>width<TAB>height<TAB>rrggbb<TAB>title<TAB>author<TAB>licence
**            <TAB>thumb<TAB>full<TAB>page<TAB>original              (all on one line)
**       album<TAB>Trips<TAB>created (unix seconds)
**       photo<TAB>...
**
**     Tabs and line breaks inside a field become spaces when saved.
**   - In memory a photo is one allocation: its strings packed back to back
**     with 16-bit offsets (a few hundred bytes), not a 2.7 KB FmPsrcItem.
**     Pages turn them into wall items only while an album is open.
**   - Album 0 is always Favorites; the others are kept in creation order
**     and shown newest first. Changes are saved on the next frame, so
**     adding fifty photos writes the file once.
*/
#include "fphoto_int.h"
#include "fview_int.h"

enum { RF_SRC, RF_ID, RF_TITLE, RF_AUTHOR, RF_LICENSE, RF_THUMB, RF_FULL, RF_PAGE, RF_ORIG, RF_COUNT };

typedef struct PhRef {
  char *blob;
  u16 off[RF_COUNT];
  int w, h;
  u32 color;
} PhRef;

typedef struct PhAlbum {
  char name[96];
  PhRef *refs;                /* newest first */
  int n, cap;
  i64 created;
} PhAlbum;

static PhAlbum *g_alb;
static int g_nalb, g_calb;
static bool g_ro, g_dirty;

/* ---- photos in memory ------------------------------------------------------- */

static const char *rf(const PhRef *r, int k) { return r->blob + r->off[k]; }

static void ref_free(PhRef *r) {
  fm_free(r->blob);
  r->blob = NULL;
}

static PhRef ref_make(const char *const *f, int w, int h, u32 color) {
  PhRef r;
  memset(&r, 0, sizeof r);
  size_t len[RF_COUNT], total = 0;
  for (int k = 0; k < RF_COUNT; k++) {
    len[k] = f[k] ? strlen(f[k]) : 0;
    if (len[k] > 2000) len[k] = 2000;
    total += len[k] + 1;
  }
  r.blob = (char *)fm_alloc(total);
  size_t at = 0;
  for (int k = 0; k < RF_COUNT; k++) {
    r.off[k] = (u16)at;
    if (len[k]) memcpy(r.blob + at, f[k], len[k]);
    /* one line in the file: no tabs or breaks inside a field */
    for (size_t i = 0; i < len[k]; i++) {
      char c = r.blob[at + i];
      if (c == '\t' || c == '\n' || c == '\r') r.blob[at + i] = ' ';
    }
    r.blob[at + len[k]] = 0;
    at += len[k] + 1;
  }
  r.w = w;
  r.h = h;
  r.color = color;
  return r;
}

static PhRef ref_from_item(const PhItem *p) {
  const char *f[RF_COUNT] = { p->src,      p->it.id,   p->it.title, p->it.author,  p->it.license,
                              p->it.thumb, p->it.full, p->it.page,  p->it.original };
  int w = p->it.width, h = p->it.height;
  /* a learned shape is worth keeping: the album opens laid out right */
  if ((w <= 0 || h <= 0) && p->ar > 0) { w = (int)(p->ar * 1000.0f + 0.5f); h = 1000; }
  return ref_make(f, w, h, p->it.color);
}

static void ref_to_item(const PhRef *r, PhItem *out) {
  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, rf(r, RF_ID), sizeof it.id);
  fm_strlcpy(it.title, rf(r, RF_TITLE), sizeof it.title);
  fm_strlcpy(it.author, rf(r, RF_AUTHOR), sizeof it.author);
  fm_strlcpy(it.license, rf(r, RF_LICENSE), sizeof it.license);
  fm_strlcpy(it.thumb, rf(r, RF_THUMB), sizeof it.thumb);
  fm_strlcpy(it.full, rf(r, RF_FULL), sizeof it.full);
  fm_strlcpy(it.page, rf(r, RF_PAGE), sizeof it.page);
  fm_strlcpy(it.original, rf(r, RF_ORIG), sizeof it.original);
  fm_strlcpy(it.source, rf(r, RF_SRC), sizeof it.source);
  it.width = r->w;
  it.height = r->h;
  it.color = r->color;
  ph_item_init(out, &it, rf(r, RF_SRC));
}

static PhAlbum *album_new(const char *name, i64 created) {
  if (g_nalb == g_calb) {
    g_calb = g_calb ? g_calb * 2 : 8;
    g_alb = (PhAlbum *)fm_realloc(g_alb, sizeof *g_alb * (size_t)g_calb);
  }
  PhAlbum *a = &g_alb[g_nalb++];
  memset(a, 0, sizeof *a);
  fm_strlcpy(a->name, name, sizeof a->name);
  for (char *c = a->name; *c; c++)
    if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
  a->created = created;
  return a;
}

static void album_push(PhAlbum *a, PhRef r, bool front) {
  if (a->n == a->cap) {
    a->cap = a->cap ? a->cap * 2 : 16;
    a->refs = (PhRef *)fm_realloc(a->refs, sizeof *a->refs * (size_t)a->cap);
  }
  if (front) {
    memmove(&a->refs[1], &a->refs[0], sizeof a->refs[0] * (size_t)a->n);
    a->refs[0] = r;
  } else {
    a->refs[a->n] = r;
  }
  a->n++;
}

static void album_free(PhAlbum *a) {
  for (int i = 0; i < a->n; i++) ref_free(&a->refs[i]);
  fm_free(a->refs);
  a->refs = NULL;
  a->n = a->cap = 0;
}

/* ---- the file --------------------------------------------------------------------- */

static char g_test_file[FM_PATH_MAX];

void palb_test_file(const char *path) { fm_strlcpy(g_test_file, path ? path : "", sizeof g_test_file); }

static bool albums_file(char *out, size_t cap) {
  if (g_test_file[0]) { fm_strlcpy(out, g_test_file, cap); return true; }
  char dir[FM_PATH_MAX];
  return plat_place(PLACE_CONFIG, dir, sizeof dir) && fm_path_join(out, cap, dir, "photo-albums.txt");
}

static void save_now(void) {
  g_dirty = false;
  if (g_ro) return;
  char path[FM_PATH_MAX], tmp[FM_PATH_MAX], dir[FM_PATH_MAX];
  if (!albums_file(path, sizeof path)) return;
  fm_strlcpy(dir, path, sizeof dir);
  fm_path_parent(dir);
  plat_mkdirs(dir);
  fm_snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fm_fopen(tmp, "wb");
  if (!f) return;
  fputs("# mmcfm online photos: favorites and albums, newest first (tab separated)\n"
        "# photo: source id width height rrggbb title author licence thumb full page original\n", f);
  for (int a = 0; a < g_nalb; a++) {
    if (a == PALB_FAV) fputs("favorites\n", f);
    else fprintf(f, "album\t%s\t%lld\n", g_alb[a].name, (long long)g_alb[a].created);
    for (int i = 0; i < g_alb[a].n; i++) {
      const PhRef *r = &g_alb[a].refs[i];
      fprintf(f, "photo\t%s\t%s\t%d\t%d\t%06x\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", rf(r, RF_SRC), rf(r, RF_ID), r->w,
              r->h, (unsigned)(r->color & 0xFFFFFF), rf(r, RF_TITLE), rf(r, RF_AUTHOR), rf(r, RF_LICENSE),
              rf(r, RF_THUMB), rf(r, RF_FULL), rf(r, RF_PAGE), rf(r, RF_ORIG));
    }
  }
  bool ok = fclose(f) == 0;
  if (!ok) { plat_remove_file(tmp); return; }
  plat_remove_file(path);
  if (plat_rename(tmp, path) != FM_OK) plat_remove_file(tmp);
}

/* Splits a line at tabs in place; returns the field count. */
static int split_tabs(char *s, char **f, int max) {
  int n = 0;
  while (n < max) {
    f[n++] = s;
    char *t = strchr(s, '\t');
    if (!t) break;
    *t = 0;
    s = t + 1;
  }
  return n;
}

void palb_load(bool readonly) {
  palb_shutdown();
  g_ro = readonly;
  album_new("Favorites", 0);
  char path[FM_PATH_MAX];
  if (readonly || !albums_file(path, sizeof path)) return;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  size_t cap = 8192;
  char *line = (char *)fm_alloc(cap);
  PhAlbum *cur = &g_alb[PALB_FAV];
  while (fgets(line, (int)cap, f)) {
    size_t n = strlen(line);
    if (n == cap - 1 && line[n - 1] != '\n') {
      /* longer than any real line: skip the rest of it */
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') {}
      continue;
    }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!line[0] || line[0] == '#') continue;
    char *fl[16];
    int nf = split_tabs(line, fl, 16);
    if (!strcmp(fl[0], "favorites")) {
      cur = &g_alb[PALB_FAV];
    } else if (!strcmp(fl[0], "album") && nf >= 2 && fl[1][0]) {
      cur = album_new(fl[1], nf >= 3 ? (i64)strtod(fl[2], NULL) : 0);
    } else if (!strcmp(fl[0], "photo") && nf >= 12 && fl[2][0]) {
      const char *v[RF_COUNT] = { fl[1], fl[2], fl[6], fl[7], fl[8], fl[9], fl[10], fl[11], nf >= 13 ? fl[12] : "" };
      album_push(cur, ref_make(v, atoi(fl[3]), atoi(fl[4]), (u32)strtoul(fl[5], NULL, 16)), false);
    }
  }
  fm_free(line);
  fclose(f);
}

void palb_pump(void) {
  if (g_dirty) save_now();
}

void palb_shutdown(void) {
  if (g_dirty) save_now();
  for (int a = 0; a < g_nalb; a++) album_free(&g_alb[a]);
  fm_free(g_alb);
  g_alb = NULL;
  g_nalb = g_calb = 0;
}

/* ---- queries and changes ----------------------------------------------------------- */

static void changed(int a) {
  g_dirty = true;
  ph_album_changed(a);
  ui_redraw();
}

int palb_count(void) { return g_nalb; }
const char *palb_name(int a) { return a >= 0 && a < g_nalb ? g_alb[a].name : ""; }
int palb_size(int a) { return a >= 0 && a < g_nalb ? g_alb[a].n : 0; }

int palb_find(int a, const char *src, const char *id) {
  if (a < 0 || a >= g_nalb) return -1;
  const PhAlbum *al = &g_alb[a];
  for (int i = 0; i < al->n; i++)
    if (!strcmp(rf(&al->refs[i], RF_ID), id) && !strcmp(rf(&al->refs[i], RF_SRC), src)) return i;
  return -1;
}

int palb_find_name(const char *name) {
  for (int a = 1; a < g_nalb; a++)
    if (fm_stricmp(g_alb[a].name, name) == 0) return a;
  return -1;
}

bool palb_add(int a, const PhItem *p) {
  if (a < 0 || a >= g_nalb || !p->it.id[0] || palb_find(a, p->src, p->it.id) >= 0) return false;
  album_push(&g_alb[a], ref_from_item(p), true);
  changed(a);
  return true;
}

void palb_remove(int a, const char *src, const char *id) {
  int i = palb_find(a, src, id);
  if (i < 0) return;
  PhAlbum *al = &g_alb[a];
  ref_free(&al->refs[i]);
  memmove(&al->refs[i], &al->refs[i + 1], sizeof al->refs[0] * (size_t)(al->n - i - 1));
  al->n--;
  changed(a);
}

void palb_get(int a, int i, PhItem *out) {
  if (a < 0 || a >= g_nalb || i < 0 || i >= g_alb[a].n) { memset(out, 0, sizeof *out); return; }
  ref_to_item(&g_alb[a].refs[i], out);
}

const char *palb_cover(int a) { return a >= 0 && a < g_nalb && g_alb[a].n ? rf(&g_alb[a].refs[0], RF_THUMB) : ""; }

static const char *cover_headers(int a) {
  if (a < 0 || a >= g_nalb || !g_alb[a].n) return NULL;
  const FmPsrc *s = psrc_find(rf(&g_alb[a].refs[0], RF_SRC));
  return s && s->img_headers && s->img_headers[0] ? s->img_headers : NULL;
}

static u32 cover_color(int a) { return a >= 0 && a < g_nalb && g_alb[a].n ? g_alb[a].refs[0].color : 0; }

int palb_create(const char *name) {
  char nm[96];
  fm_strlcpy(nm, name, sizeof nm);
  char *b = nm;
  while (*b == ' ') b++;
  size_t n = strlen(b);
  while (n > 0 && b[n - 1] == ' ') b[--n] = 0;
  if (!b[0]) b = "Untitled album";
  char unique[96];
  fm_strlcpy(unique, b, sizeof unique);
  for (int k = 2; palb_find_name(unique) >= 0 || fm_stricmp(unique, "Favorites") == 0; k++)
    fm_snprintf(unique, sizeof unique, "%.80s (%d)", b, k);
  album_new(unique, plat_time_unix());
  g_dirty = true;
  ui_redraw();
  return g_nalb - 1;
}

static void rename_album(int a, const char *name) {
  if (a <= PALB_FAV || a >= g_nalb) return;
  char nm[96];
  fm_strlcpy(nm, name, sizeof nm);
  char *b = nm;
  while (*b == ' ') b++;
  size_t n = strlen(b);
  while (n > 0 && b[n - 1] == ' ') b[--n] = 0;
  if (!b[0] || !strcmp(b, g_alb[a].name)) return;
  int other = palb_find_name(b);
  if ((other >= 0 && other != a) || fm_stricmp(b, "Favorites") == 0) {
    ui_toast("There is already an album called \xE2\x80\x9C%s\xE2\x80\x9D", b);
    return;
  }
  fm_strlcpy(g_alb[a].name, b, sizeof g_alb[a].name);
  for (char *c = g_alb[a].name; *c; c++)
    if (*c == '\t') *c = ' ';
  changed(a);
}

static void delete_album(int a) {
  if (a <= PALB_FAV || a >= g_nalb) return;
  char name[96];
  fm_strlcpy(name, g_alb[a].name, sizeof name);
  album_free(&g_alb[a]);
  memmove(&g_alb[a], &g_alb[a + 1], sizeof g_alb[0] * (size_t)(g_nalb - a - 1));
  g_nalb--;
  g_dirty = true;
  ph_album_removed(a);
  ui_toast("Deleted \xE2\x80\x9C%s\xE2\x80\x9D", name);
  ui_redraw();
}

bool palb_is_fav(const PhItem *p) { return p->it.id[0] && palb_find(PALB_FAV, p->src, p->it.id) >= 0; }

void palb_fav_toggle(const PhItem *p) {
  if (palb_is_fav(p)) {
    palb_remove(PALB_FAV, p->src, p->it.id);
    ui_toast("Removed from Favorites");
  } else if (palb_add(PALB_FAV, p)) {
    ui_toast("Added to Favorites");
  }
}

void palb_download(int a) {
  int n = palb_size(a);
  if (n <= 0) { ui_toast("This album is empty"); return; }
  PhItem *items = (PhItem *)fm_alloc(sizeof *items * (size_t)n);
  for (int i = 0; i < n; i++) palb_get(a, i, &items[i]);
  pdl_batch(items, n);
  fm_free(items);
}

/* ---- covers page ------------------------------------------------------------------------ */

static FmScroll g_cov_scroll;

/* Albums in the order shown: Favorites, then the newest first. */
static int shown_album(int k) { return k == 0 ? PALB_FAV : g_nalb - k; }

static void cover_art(FmRect r, int a, float rad) {
  const char *url = palb_cover(a);
  float fade = 1;
  SDL_Texture *tex = url[0] ? othumb_get_ex(url, (int)r.w, 1.0f, cover_headers(a), &fade) : NULL;
  if (!tex || fade < 1) {
    u32 c = cover_color(a);
    if (c) gfx_rrect(r, rad, col_mix(T.surface2, FM_HEX(c), T.dark ? 0.75f : 0.85f));
    else {
      gfx_rrect(r, rad, col_mix(T.surface2, T.accent, a == PALB_FAV ? 0.12f : 0.06f));
      if (!url[0]) {
        float is = r.w * 0.34f;
        icon_draw(a == PALB_FAV ? IC_HEART : IC_ALBUM, rect_center(r, is, is),
                  a == PALB_FAV ? col_alpha(T.accent, 0.8f) : T.text3);
      }
    }
  }
  if (tex) {
    gfx_tex_rounded(tex, r, rad, col_alpha(PH_WHITE, fade));
    if (fade < 1) ui_animate();
  }
}

void palb_page(FmRect body) {
  float pad = wall_pad(body.w) + DP(4);
  FmRect in = rect_inset2(body, pad, 0);
  /* heading: the title and "New album" */
  FmRect head = rect_cut_top(&in, DP(ui.touch_mode ? 60 : 54));
  float bh = DP(ui.touch_mode ? 40 : 34);
  const char *nl = "New album";
  float nw = font_width(FONT_BOLD, ui.m.font, nl, -1) + DP(52);
  if (ui_button(ui_id("ph.alb.new"), FM_RECT(head.x + head.w - nw, head.y + (head.h - bh) * 0.5f, nw, bh), IC_PLUS, nl,
                UI_BTN_TONAL))
    palb_new_open();
  char sub[64];
  int nal = g_nalb - 1;
  fm_snprintf(sub, sizeof sub, "%d %s  \xC2\xB7  kept on this device", nal, nal == 1 ? "album" : "albums");
  float tl = font_line_h(ui.m.font_title), sl = font_line_h(ui.m.font_small);
  float ty = head.y + (head.h - tl - sl) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font_title, head.x, ty, "Albums", head.w - nw - DP(12), T.text);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, head.x, ty + tl, sub, head.w - nw - DP(12), T.text2);
  ui_divider(body.x + DP(12), body.x + body.w - DP(12), head.y + head.h);
  rect_cut_top(&in, DP(1));
  FmRect view = { body.x, in.y, body.w, body.y + body.h - in.y - DP(4) };
  /* the grid */
  float gap = DP(ui.touch_mode ? 12 : 16);
  float W = view.w - pad * 2;
  float want = DP(ui.touch_mode ? 160 : 176);
  int cols = FM_CLAMP((int)((W + gap) / (want + gap)), 2, 8);
  float s = floorf((W - gap * (float)(cols - 1)) / (float)cols);
  float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
  float ch = s + DP(10) + lh + ls + DP(14);
  int n = g_nalb;
  int rows = (n + cols - 1) / cols;
  float content = DP(14) + (float)rows * ch + DP(12);
  u32 sid = ui_id("ph.alb.grid");
  ui_scroll(&g_cov_scroll, sid, view, content);
  float sy = g_cov_scroll.y;
  gfx_clip_push(view);
  float rad = DP(14);
  for (int k = 0; k < n; k++) {
    int r = k / cols, c = k % cols;
    FmRect card = { view.x + pad + (float)c * (s + gap), view.y + DP(14) + (float)r * ch - sy, s, ch };
    if (!gfx_visible(card)) continue;
    int a = shown_album(k);
    u32 id = ui_idn(sid, (u32)a + 1);
    FmRect hit = rect_inset(card, -DP(6));
    int f = ui_hit(id, hit);
    float hv = ui.touch_mode ? 0 : ui_anim(ui_idn(id, 1), (f & UI_HOVER) ? 1.0f : 0.0f, 14.0f);
    if (hv > 0.01f) gfx_rrect(hit, DP(18), col_alpha(T.hover, hv));
    FmRect cov = { card.x, card.y, s, s };
    cover_art(cov, a, rad);
    if (f & UI_HELD) gfx_rrect(cov, rad, FM_RGBA(0, 0, 0, 40));
    if (a == PALB_FAV && g_alb[a].n) {
      float d = DP(30);
      FmRect hb = { cov.x + DP(10), cov.y + cov.h - d - DP(10), d, d };
      gfx_circle(hb.x + d * 0.5f, hb.y + d * 0.5f, d * 0.5f, FM_RGBA(0, 0, 0, 110));
      icon_draw(IC_HEART_FILL, rect_inset(hb, DP(7)), PH_WHITE);
    }
    float ty2 = cov.y + s + DP(10);
    font_draw_ellipsis(FONT_BOLD, ui.m.font, card.x + DP(2), ty2, g_alb[a].name, s - DP(4), T.text);
    char cnt[48];
    fm_snprintf(cnt, sizeof cnt, "%d %s", g_alb[a].n, g_alb[a].n == 1 ? "photo" : "photos");
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, card.x + DP(2), ty2 + lh, cnt, s - DP(4), T.text2);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) ph_show_album(a);
    else if ((f & UI_RCLICK) || (f & UI_LONG)) palb_album_menu(a, ui.mx, ui.my);
  }
  gfx_clip_pop();
  ui_scrollbar(&g_cov_scroll, view, content);
}

/* ---- album menu ------------------------------------------------------------------------- */

enum { AM_OPEN = 1, AM_DOWNLOAD, AM_RENAME, AM_DELETE };
static u32 ID_AMENU;
static int g_menu_album = -1;

void palb_album_menu(int a, float x, float y) {
  if (!ID_AMENU) ID_AMENU = ui_id("ph.alb.menu");
  g_menu_album = a;
  FmMenuItem m[6];
  int n = 0;
  memset(m, 0, sizeof m);
  m[n].id = AM_OPEN; m[n].icon = IC_FOLDER_OPEN; m[n++].label = "Open";
  m[n].id = AM_DOWNLOAD; m[n].icon = IC_DOWNLOAD; m[n].label = "Download album";
  m[n++].flags = palb_size(a) ? 0 : UI_MI_DISABLED;
  if (a != PALB_FAV) {
    m[n++].flags = UI_MI_SEP;
    m[n].id = AM_RENAME; m[n].icon = IC_RENAME; m[n++].label = "Rename";
    m[n].id = AM_DELETE; m[n].icon = IC_DELETE; m[n].label = "Delete album"; m[n++].flags = UI_MI_DANGER;
  }
  ui_menu_open(ID_AMENU, x, y, m, n);
}

void palb_menu_results(void) {
  if (!ID_AMENU) return;
  int r = ui_menu_result(ID_AMENU);
  if (r < 0 || g_menu_album < 0) return;
  int a = g_menu_album;
  g_menu_album = -1;
  switch (r) {
    case AM_OPEN: ph_show_album(a); break;
    case AM_DOWNLOAD: palb_download(a); break;
    case AM_RENAME: palb_rename_open(a); break;
    case AM_DELETE: palb_delete_open(a); break;
    default: break;
  }
}

/* ---- dialogs ------------------------------------------------------------------------------ */

enum { DL_NONE, DL_PICK, DL_NEW, DL_RENAME, DL_DELETE };

static struct {
  int kind;
  int album;
  PhItem *pend;               /* photos waiting for an album (pick / new) */
  int npend;
  char name[96];
  FmScroll scroll;
} D;

static void set_pending(const PhItem *items, int n) {
  fm_free(D.pend);
  D.pend = NULL;
  D.npend = 0;
  if (n <= 0) return;
  D.pend = (PhItem *)fm_alloc(sizeof *D.pend * (size_t)n);
  memcpy(D.pend, items, sizeof *D.pend * (size_t)n);
  D.npend = n;
}

static void dlg_close(void) {
  D.kind = DL_NONE;
  set_pending(NULL, 0);
  ui_focus(0);
  ui_redraw();
}

void palb_pick_open(const PhItem *items, int n) {
  if (n <= 0) return;
  set_pending(items, n);
  D.kind = DL_PICK;
  memset(&D.scroll, 0, sizeof D.scroll);
  ui_redraw();
}

void palb_new_open(void) {
  D.kind = DL_NEW;
  D.name[0] = 0;
  ui_redraw();
}

void palb_rename_open(int a) {
  if (a <= PALB_FAV || a >= g_nalb) return;
  D.kind = DL_RENAME;
  D.album = a;
  fm_strlcpy(D.name, g_alb[a].name, sizeof D.name);
  ui_redraw();
}

void palb_delete_open(int a) {
  if (a <= PALB_FAV || a >= g_nalb) return;
  D.kind = DL_DELETE;
  D.album = a;
  ui_redraw();
}

/* Adds the pending photos to album a, with one toast. */
static void add_pending(int a) {
  int added = 0;
  for (int i = 0; i < D.npend; i++) added += palb_add(a, &D.pend[i]);
  if (added == 0) ui_toast("Already in \xE2\x80\x9C%s\xE2\x80\x9D", g_alb[a].name);
  else if (added == 1) ui_toast("Added to \xE2\x80\x9C%s\xE2\x80\x9D", g_alb[a].name);
  else ui_toast("Added %d photos to \xE2\x80\x9C%s\xE2\x80\x9D", added, g_alb[a].name);
}

static float btn_h(void) { return DP(ui.touch_mode ? 46 : 38); }

static float dlg_w(float w_dp) {
  bool sheet = ui.portrait && ui.w < DP(560);
  return (sheet ? ui.w : FM_MIN(DP(w_dp), ui.w - DP(24))) - DP(40);
}

/* Two buttons at the bottom right: returns 0 (secondary), 1 (primary) or -1. */
static int two_buttons(FmRect c, const char *no, const char *yes, int style) {
  float bh = btn_h();
  float y = c.y + c.h - bh;
  float yw = font_width(FONT_BOLD, ui.m.font, yes, -1) + DP(36);
  float nw = font_width(FONT_BOLD, ui.m.font, no, -1) + DP(32);
  u32 id = ui_id("ph.dlg.btn");
  int r = -1;
  if (ui_button(ui_idn(id, 1), FM_RECT(c.x + c.w - yw, y, yw, bh), IC_NONE, yes, style)) r = 1;
  if (ui_button(ui_idn(id, 0), FM_RECT(c.x + c.w - yw - DP(8) - nw, y, nw, bh), IC_NONE, no, UI_BTN_TEXT)) r = 0;
  return r;
}

static void dlg_pick(void) {
  float rh = DP(ui.touch_mode ? 64 : 58);
  int n = g_nalb + 1;                          /* "New album" first */
  float list_max = (ui.h - ui_keyboard_h()) * 0.62f;
  float list_h = FM_MIN((float)n * rh, FM_MAX(rh * 2, list_max));
  float sub_h = font_line_h(ui.m.font_small) + DP(10);
  float h = sub_h + list_h + DP(14) + btn_h();
  float title_h = font_line_h(ui.m.font_title) + DP(12);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("ph.dlg.pick"), "Add to album", 460, (h + title_h + DP(40)) / ui.scale + 2, &cancel);
  char sub[96];
  if (D.npend == 1) fm_snprintf(sub, sizeof sub, "%s", D.pend[0].it.title[0] ? D.pend[0].it.title : "1 photo");
  else fm_snprintf(sub, sizeof sub, "%d photos", D.npend);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, c.y, sub, c.w, T.text2);
  rect_cut_top(&c, sub_h);
  FmRect list = rect_cut_top(&c, list_h);
  u32 sid = ui_id("ph.dlg.pick.list");
  ui_scroll(&D.scroll, sid, list, (float)n * rh);
  gfx_clip_push(list);
  int chosen = -2;
  for (int k = 0; k < n; k++) {
    FmRect row = { list.x, list.y + (float)k * rh - D.scroll.y, list.w, rh };
    if (!gfx_visible(row)) continue;
    int a = k == 0 ? -1 : shown_album(k - 1);
    int f = ui_hit(ui_idn(sid, (u32)k + 1), row);
    if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
    if (f & UI_HELD) gfx_rrect(row, DP(12), T.press);
    float cs = rh - DP(14);
    FmRect cov = { row.x + DP(6), row.y + DP(7), cs, cs };
    float tx = cov.x + cs + DP(14), tw = row.x + row.w - tx - DP(40);
    if (a < 0) {
      gfx_rrect(cov, DP(10), T.accent_soft);
      icon_draw(IC_PLUS, rect_inset(cov, cs * 0.28f), T.accent);
      font_draw_ellipsis(FONT_BOLD, ui.m.font, tx, row.y + (rh - font_line_h(ui.m.font)) * 0.5f, "New album", tw, T.accent);
    } else {
      cover_art(cov, a, DP(10));
      float lh = font_line_h(ui.m.font), ls = font_line_h(ui.m.font_small);
      float ty = row.y + (rh - lh - ls) * 0.5f;
      font_draw_ellipsis(FONT_BOLD, ui.m.font, tx, ty, g_alb[a].name, tw, T.text);
      char cnt[48];
      fm_snprintf(cnt, sizeof cnt, "%d %s", g_alb[a].n, g_alb[a].n == 1 ? "photo" : "photos");
      font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, tx, ty + lh, cnt, tw, T.text2);
      /* a check when every photo is in it already */
      bool all = D.npend > 0;
      for (int i = 0; i < D.npend && all; i++) all = palb_find(a, D.pend[i].src, D.pend[i].it.id) >= 0;
      if (all) {
        float is = DP(20);
        icon_draw(IC_CHECK, FM_RECT(row.x + row.w - is - DP(12), row.y + (rh - is) * 0.5f, is, is), T.accent);
      }
    }
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    if (f & UI_CLICK) chosen = a;
  }
  gfx_clip_pop();
  ui_scrollbar(&D.scroll, list, (float)n * rh);
  rect_cut_top(&c, DP(14));
  const char *l = "Cancel";
  float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(36);
  bool close = ui_button(ui_id("ph.dlg.pick.cancel"), FM_RECT(c.x + c.w - bw, c.y, bw, btn_h()), IC_NONE, l,
                         UI_BTN_TEXT);
  ui_dialog_end();
  if (chosen == -1) {
    D.kind = DL_NEW;                         /* keeps the pending photos */
    D.name[0] = 0;
    ui_redraw();
    return;
  }
  if (chosen >= 0) {
    add_pending(chosen);
    dlg_close();
    return;
  }
  if (cancel || close) dlg_close();
}

static void dlg_name(bool rename) {
  float fh = DP(ui.touch_mode ? 48 : 40);
  float note_h = font_line_h(ui.m.font_small) + DP(6);
  float h = note_h + fh + DP(22) + btn_h();
  float title_h = font_line_h(ui.m.font_title) + DP(12);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id(rename ? "ph.dlg.rename" : "ph.dlg.new"), rename ? "Rename album" : "New album", 420,
                             (h + title_h + DP(40)) / ui.scale + 2, &cancel);
  char note[96];
  if (rename) fm_strlcpy(note, "Albums are kept on this device.", sizeof note);
  else if (D.npend > 1) fm_snprintf(note, sizeof note, "%d photos go into it.", D.npend);
  else if (D.npend == 1) fm_strlcpy(note, "The photo goes into it.", sizeof note);
  else fm_strlcpy(note, "Add photos from a search or the lightbox.", sizeof note);
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, c.y, note, c.w, T.text2);
  rect_cut_top(&c, note_h);
  FmRect fr = rect_cut_top(&c, fh);
  int res = ui_textfield(ui_id("ph.dlg.name"), fr, D.name, sizeof D.name, "Album name", UI_TF_FOCUS);
  int b = two_buttons(c, "Cancel", rename ? "Rename" : "Create", UI_BTN_FILLED);
  ui_dialog_end();
  if (b == 1 || (res & UI_TF_SUBMIT)) {
    if (rename) {
      rename_album(D.album, D.name);
    } else {
      int a = palb_create(D.name);
      if (D.npend > 0) add_pending(a);
      else ui_toast("Created \xE2\x80\x9C%s\xE2\x80\x9D", g_alb[a].name);
    }
    dlg_close();
    return;
  }
  if (cancel || b == 0) dlg_close();
}

static void dlg_delete(void) {
  int a = D.album;
  if (a <= PALB_FAV || a >= g_nalb) { dlg_close(); return; }
  float cw = dlg_w(420);
  const char *text = "The album and its list of photos go away. The photos stay on their websites and in your "
                     "downloads.";
  float th = font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw, text, T.text2, false);
  float h = th + DP(22) + btn_h();
  float title_h = font_line_h(ui.m.font_title) + DP(12);
  char title[140];
  fm_snprintf(title, sizeof title, "Delete \xE2\x80\x9C%s\xE2\x80\x9D?", g_alb[a].name);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("ph.dlg.delete"), title, 420, (h + title_h + DP(40)) / ui.scale + 2, &cancel);
  font_draw_wrap(FONT_REGULAR, ui.m.font, c.x, c.y, c.w, text, T.text2, true);
  int b = two_buttons(c, "Cancel", "Delete", UI_BTN_DANGER);
  ui_dialog_end();
  if (b == 1) { dlg_close(); delete_album(a); return; }
  if (cancel || b == 0) dlg_close();
}

bool palb_dialogs(void) {
  switch (D.kind) {
    case DL_PICK: dlg_pick(); return true;
    case DL_NEW: dlg_name(false); return true;
    case DL_RENAME: dlg_name(true); return true;
    case DL_DELETE: dlg_delete(); return true;
    default: return false;
  }
}

/* ---- screenshots ------------------------------------------------------------------------- */

void palb_demo(const PhItem *items, int n) {
  static const char *const kNames[] = { "Weekend in the mountains", "Wallpapers", "Ideas for the living room" };
  for (int i = 0; i < n && i < 7; i++) palb_add(PALB_FAV, &items[i]);
  for (int k = 0; k < FM_COUNT(kNames); k++) {
    int a = palb_find_name(kNames[k]);
    if (a < 0) a = palb_create(kNames[k]);
    for (int i = 0; i < n; i++)
      if ((i + k) % (k + 2) == 0) palb_add(a, &items[i]);
  }
  g_dirty = false;
}
