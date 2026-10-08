/* fpanel.c -- one file panel: navigation, sorting, selection and drawing.
**
** Design decisions:
**   - Entering an archive lists it into a temporary listing first, so a
**     wrong password or a damaged file leaves the panel where it was. Local
**     folders that fail to list keep the location and show an error card,
**     so "Go up" and "Retry" make sense.
**   - History entries are short encoded strings ("L<path>" or
**     "A<path>\n<inner>"), not FmLoc structs: 64 entries of a few dozen
**     bytes instead of 128 KB per panel.
**   - Mouse: press selects (or keeps a multi-selection for dragging), the
**     release decides a plain click, double click opens. Touch: tap opens,
**     long press starts selection mode with checkboxes.
**   - Desktop type-to-filter writes straight into the filter (no focused
**     field), so the first key is never swallowed by a select-all.
*/
#include "fpanel.h"
#include "fconf.h"
#include "fops.h"
#include "fapp.h"
#include "fthumb.h"
#include "fview.h"

/* The keyboard cursor outline only shows once the keyboard is used. */
static bool g_kbd_cursor;

static float footer_h(void) { return DP(ui.touch_mode ? 30 : 26); }
static float search_h(void) { return DP(ui.touch_mode ? 54 : 44); }

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

/* ---- history ------------------------------------------------------------ */

static char *loc_encode(const FmLoc *l) {
  size_t n = strlen(l->path) + strlen(l->inner) + 3;
  char *s = (char *)fm_alloc(n);
  if (l->in_arc) fm_snprintf(s, n, "A%s\n%s", l->path, l->inner);
  else fm_snprintf(s, n, "L%s", l->path);
  return s;
}

static void loc_decode(const char *s, FmLoc *l) {
  memset(l, 0, sizeof *l);
  if (s[0] == 'A') {
    const char *nl = strchr(s + 1, '\n');
    size_t pl = nl ? (size_t)(nl - s - 1) : strlen(s + 1);
    if (pl >= sizeof l->path) pl = sizeof l->path - 1;
    memcpy(l->path, s + 1, pl);
    l->path[pl] = 0;
    if (nl) fm_strlcpy(l->inner, nl + 1, sizeof l->inner);
    l->in_arc = true;
  } else {
    fm_strlcpy(l->path, s + 1, sizeof l->path);
  }
}

static void hist_push(char **stack, int *n, char *item) {
  if (*n == PANEL_HIST) {
    fm_free(stack[0]);
    memmove(stack, stack + 1, (PANEL_HIST - 1) * sizeof *stack);
    (*n)--;
  }
  stack[(*n)++] = item;
}

static void hist_clear(char **stack, int *n) {
  for (int i = 0; i < *n; i++) fm_free(stack[i]);
  *n = 0;
}

/* ---- view: sort and filter ---------------------------------------------- */

static const FmEntry *g_sort_items;
static int g_sort_key;
static bool g_sort_desc, g_sort_dirs;

static int cmp_u64(u64 a, u64 b) { return a < b ? -1 : a > b ? 1 : 0; }

static int cmp_view(const void *pa, const void *pb) {
  const FmEntry *x = &g_sort_items[*(const int *)pa];
  const FmEntry *y = &g_sort_items[*(const int *)pb];
  if (g_sort_dirs) {
    bool dx = (x->flags & FM_ST_DIR) != 0, dy = (y->flags & FM_ST_DIR) != 0;
    if (dx != dy) return dx ? -1 : 1;
  }
  int c = 0;
  switch (g_sort_key) {
    case SORT_SIZE: c = cmp_u64(x->size, y->size); break;
    case SORT_DATE: c = x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0; break;
    case SORT_TYPE: c = fm_stricmp(fm_path_ext(x->name), fm_path_ext(y->name)); break;
    default: break;
  }
  if (c == 0) c = fm_natcmp(x->name, y->name);
  if (c == 0) c = strcmp(x->name, y->name);
  return g_sort_desc ? -c : c;
}

void panel_update_sel(FmPanel *p) {
  p->nsel = 0;
  p->sel_bytes = 0;
  for (int i = 0; i < p->list.count; i++) {
    const FmEntry *e = &p->list.items[i];
    if (!e->selected) continue;
    p->nsel++;
    if (!(e->flags & FM_ST_DIR)) p->sel_bytes += e->size;
  }
  if (p->nsel == 0 && p->select_mode && ui.touch_mode) p->select_mode = false;
}

void panel_resort(FmPanel *p) {
  FmListing *l = &p->list;
  int keep = (p->cursor >= 0 && p->cursor < p->nview) ? p->view[p->cursor] : -1;
  if (p->view_cap < l->count) {
    p->view_cap = l->count + 64;
    p->view = (int *)fm_realloc(p->view, (size_t)p->view_cap * sizeof(int));
  }
  p->nview = 0;
  p->ndirs = p->nfiles = 0;
  for (int i = 0; i < l->count; i++) {
    FmEntry *e = &l->items[i];
    if (p->filter[0] && !fm_stristr(e->name, p->filter)) {
      e->selected = 0;                 /* never act on what is hidden */
      continue;
    }
    p->view[p->nview++] = i;
    if (e->flags & FM_ST_DIR) p->ndirs++;
    else p->nfiles++;
  }
  g_sort_items = l->items;
  g_sort_key = conf.sort[p->idx];
  g_sort_desc = conf.sort_desc[p->idx];
  g_sort_dirs = conf.folders_first;
  if (p->nview > 1) qsort(p->view, (size_t)p->nview, sizeof(int), cmp_view);
  p->cursor = p->nview > 0 ? 0 : -1;
  if (keep >= 0)
    for (int i = 0; i < p->nview; i++)
      if (p->view[i] == keep) { p->cursor = i; break; }
  if (p->anchor >= p->nview) p->anchor = p->cursor;
  panel_update_sel(p);
}

/* ---- breadcrumb --------------------------------------------------------- */

static void build_segments(FmPanel *p) {
  const FmLoc *loc = &p->list.loc;
  loc_display(loc, p->display, sizeof p->display);
  p->nsegs = 0;
  const char *d = p->display;
  size_t n = strlen(d), i = 0;
  char home[FM_PATH_MAX];
  size_t hl = 0;
  if (plat_place(PLACE_HOME, home, sizeof home)) {
    hl = strlen(home);
    while (hl > 1 && fm_is_sep(home[hl - 1])) hl--;
#ifdef FM_WIN
    bool pre = fm_strnicmp(d, home, hl) == 0;
#else
    bool pre = strncmp(d, home, hl) == 0;
#endif
    if (!pre || (d[hl] && !fm_is_sep(d[hl])) || fm_path_is_root(home)) hl = 0;
  }
  if (hl > 0) {
#ifdef FM_ANDROID
    fm_strlcpy(p->seg_label[0], "Storage", sizeof p->seg_label[0]);
#else
    fm_strlcpy(p->seg_label[0], "Home", sizeof p->seg_label[0]);
#endif
    p->seg_end[p->nsegs++] = (u16)hl;
    i = hl;
  } else {
    /* the root: "/", "C:\" or "\\server\share\" */
    size_t r = 0;
#ifdef FM_WIN
    if (d[0] && d[1] == ':') r = fm_is_sep(d[2]) ? 3 : 2;
    else if (fm_is_sep(d[0]) && fm_is_sep(d[1])) {
      int seps = 0;
      r = 2;
      while (d[r] && seps < 2) { if (fm_is_sep(d[r])) seps++; r++; }
    } else if (fm_is_sep(d[0])) r = 1;
#else
    if (d[0] == '/') r = 1;
#endif
    if (r > 0) {
      char lab[48];
      size_t ll = r < sizeof lab ? r : sizeof lab - 1;
      memcpy(lab, d, ll);
      lab[ll] = 0;
      while (ll > 1 && fm_is_sep(lab[ll - 1]) && lab[0] != '/') lab[--ll] = 0;
      fm_strlcpy(p->seg_label[0], lab, sizeof p->seg_label[0]);
      p->seg_end[p->nsegs++] = (u16)r;
      i = r;
    }
  }
  while (i < n && p->nsegs < PANEL_SEGS) {
    while (i < n && fm_is_sep(d[i])) i++;
    if (i >= n) break;
    size_t s = i;
    while (i < n && !fm_is_sep(d[i])) i++;
    size_t ll = i - s;
    if (ll >= sizeof p->seg_label[0]) ll = sizeof p->seg_label[0] - 1;
    memcpy(p->seg_label[p->nsegs], d + s, ll);
    p->seg_label[p->nsegs][ll] = 0;
    p->seg_end[p->nsegs++] = (u16)i;
  }
  p->crumb_x = 1e9f;   /* clamped to "show the end" when drawn */
}

/* Location of breadcrumb segment k. */
static void segment_loc(FmPanel *p, int k, FmLoc *out) {
  const FmLoc *cur = &p->list.loc;
  size_t end = p->seg_end[k];
  char prefix[FM_PATH_MAX];
  size_t n = end < sizeof prefix ? end : sizeof prefix - 1;
  memcpy(prefix, p->display, n);
  prefix[n] = 0;
  size_t pl = strlen(cur->path);
  if (cur->in_arc && end >= pl) {
    memset(out, 0, sizeof *out);
    fm_strlcpy(out->path, cur->path, sizeof out->path);
    out->in_arc = true;
    if (end > pl + 1) {
      char inner[FM_PATH_MAX];
      fm_strlcpy(inner, p->display + pl + 1, FM_MIN(sizeof inner, end - pl));
      for (char *s = inner; *s; s++) if (*s == FM_SEP) *s = '/';
      fm_strlcat(inner, "/", sizeof inner);
      fm_strlcpy(out->inner, inner, sizeof out->inner);
    }
    return;
  }
  loc_local(out, prefix);
}

/* ---- navigation --------------------------------------------------------- */

static void after_nav(FmPanel *p) {
  p->scroll.y = 0;
  p->scroll.vel = 0;
  p->filter[0] = 0;
  p->search_open = false;
  p->editing_path = false;
  p->select_mode = false;
  p->cursor = -1;
  p->anchor = 0;
  p->pending_single = -1;
  build_segments(p);
  panel_resort(p);
  FmStat st;
  p->dir_mtime = (!p->list.loc.in_arc && plat_stat(p->list.loc.path, &st)) ? st.mtime : 0;
  p->nav_t = ui.now;
  ui_anim_set(ui_idn(ui_id("panel.nav"), (u32)p->idx), 1.0f);
  thumb_cancel_all();
  app_panel_navigated(p);
  ui_redraw();
}

static FmErr go_into(FmPanel *p, const FmLoc *loc, bool push) {
  FmLoc want = *loc;
  FmLoc old = p->list.loc;
  bool had = old.path[0] != 0;
  bool new_arc = want.in_arc && !(p->list.arc && same_path(p->list.loc.path, want.path));
  FmErr err;
  if (new_arc) {
    FmListing tmp;
    memset(&tmp, 0, sizeof tmp);
    err = vfs_list(&tmp, &want, conf.show_hidden);
    if (err != FM_OK) {
      vfs_free(&tmp);
      return err;
    }
    vfs_free(&p->list);
    p->list = tmp;
  } else {
    err = vfs_list(&p->list, &want, conf.show_hidden);
  }
  if (push && had && !loc_equal(&old, &want)) {
    hist_push(p->back, &p->nback, loc_encode(&old));
    hist_clear(p->fwd, &p->nfwd);
  }
  after_nav(p);
  return FM_OK;
}

bool panel_go(FmPanel *p, const FmLoc *loc, bool push_history) {
  FmErr e = go_into(p, loc, push_history);
  if (e == FM_OK) return true;
  if (e == FM_ERR_PASSWORD) {
    app_panel_password(p, loc, false);
  } else {
    char t[256];
    loc_title(loc, t, sizeof t);
    fm_log("cannot open %s: %s", loc->path, fm_err_str(e));
    ui_toast("Can't open %s: %s", t, fm_err_str(e));
  }
  return false;
}

bool panel_go_path(FmPanel *p, const char *path) {
  char tmp[FM_PATH_MAX];
  fm_strlcpy(tmp, path, sizeof tmp);
  size_t n = strlen(tmp);
  while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t')) tmp[--n] = 0;
  char *s = tmp;
  while (*s == ' ' || *s == '\t') s++;
  if (s[0] == '~' && (s[1] == 0 || fm_is_sep(s[1]))) {
    char home[FM_PATH_MAX], j[FM_PATH_MAX];
    if (plat_place(PLACE_HOME, home, sizeof home) && fm_path_join(j, sizeof j, home, s + 1)) {
      fm_strlcpy(tmp, j, sizeof tmp);
      s = tmp;
    }
  }
  FmLoc l;
  loc_local(&l, s);
  if (plat_is_dir(l.path)) return panel_go(p, &l, true);
  if (plat_exists(l.path)) {
    char name[256];
    fm_strlcpy(name, fm_path_base(l.path), sizeof name);
    if (fm_type_from_name(name) == FT_ARCHIVE) {
      FmLoc a = l;
      a.in_arc = true;
      if (panel_go(p, &a, true)) return true;
    }
    if (!fm_path_parent(l.path) || !panel_go(p, &l, true)) return false;
    panel_select_name(p, name);
    return true;
  }
  ui_toast("Not found: %s", s);
  return false;
}

bool panel_up(FmPanel *p) {
  FmLoc l = p->list.loc;
  char name[256];
  loc_title(&l, name, sizeof name);
  if (!loc_up(&l)) return false;
  if (!panel_go(p, &l, true)) return false;
  panel_select_name(p, name);
  p->select_mode = false;
  return true;
}

bool panel_back(FmPanel *p) {
  if (p->nback == 0) return false;
  char *s = p->back[--p->nback];
  FmLoc l;
  loc_decode(s, &l);
  fm_free(s);
  FmLoc cur = p->list.loc;
  if (go_into(p, &l, false) != FM_OK) {
    ui_toast("That folder is no longer available");
    return false;
  }
  hist_push(p->fwd, &p->nfwd, loc_encode(&cur));
  return true;
}

bool panel_forward(FmPanel *p) {
  if (p->nfwd == 0) return false;
  char *s = p->fwd[--p->nfwd];
  FmLoc l;
  loc_decode(s, &l);
  fm_free(s);
  FmLoc cur = p->list.loc;
  if (go_into(p, &l, false) != FM_OK) {
    ui_toast("That folder is no longer available");
    return false;
  }
  hist_push(p->back, &p->nback, loc_encode(&cur));
  return true;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void panel_refresh(FmPanel *p) {
  FmListing *l = &p->list;
  /* remember the selection and cursor by name */
  FmArena ar;
  arena_init(&ar, 16 * 1024);
  char **names = (char **)fm_alloc((size_t)(p->nsel > 0 ? p->nsel : 1) * sizeof(char *));
  int nn = 0;
  for (int i = 0; i < l->count && nn < p->nsel; i++)
    if (l->items[i].selected) names[nn++] = arena_strdup(&ar, l->items[i].name);
  qsort(names, (size_t)nn, sizeof *names, cmp_str);
  char cur[512] = { 0 };
  FmEntry *ce = panel_cursor_entry(p);
  if (ce) fm_strlcpy(cur, ce->name, sizeof cur);
  float sy = p->scroll.y;
  bool sm = p->select_mode;

  FmLoc loc = l->loc;
  FmErr e = vfs_list(l, &loc, conf.show_hidden);
  if (e == FM_ERR_NOT_FOUND && !loc.in_arc) {
    /* the folder went away: show the nearest one that still exists */
    while (fm_path_parent(loc.path) && !plat_is_dir(loc.path)) {}
    vfs_list(l, &loc, conf.show_hidden);
    after_nav(p);
  } else {
    for (int i = 0; i < l->count; i++) {
      const char *key = l->items[i].name;
      if (nn && bsearch(&key, names, (size_t)nn, sizeof *names, cmp_str)) l->items[i].selected = 1;
    }
    p->cursor = -1;
    panel_resort(p);
    if (cur[0])
      for (int i = 0; i < p->nview; i++)
        if (!strcmp(l->items[p->view[i]].name, cur)) { p->cursor = i; break; }
    p->scroll.y = sy;
    p->select_mode = sm && p->nsel > 0;
    FmStat st;
    p->dir_mtime = (!loc.in_arc && plat_stat(loc.path, &st)) ? st.mtime : 0;
  }
  fm_free(names);
  arena_free(&ar);
  ui_redraw();
}

bool panel_changed_outside(FmPanel *p) {
  if (p->list.loc.in_arc) return false;
  FmStat st;
  if (!plat_stat(p->list.loc.path, &st)) return true;
  return st.mtime != p->dir_mtime;
}

void panel_set_filter(FmPanel *p, const char *f) {
  if (strcmp(p->filter, f) == 0) return;
  fm_strlcpy(p->filter, f, sizeof p->filter);
  panel_resort(p);
  p->scroll.y = 0;
  ui_redraw();
}

void panel_edit_path(FmPanel *p) {
  p->editing_path = true;
  p->focus_pending = true;
  fm_strlcpy(p->path_edit, p->display, sizeof p->path_edit);
  ui_redraw();
}

void panel_open_search(FmPanel *p) {
  p->search_open = true;
  p->focus_pending = true;
  ui_redraw();
}

/* ---- init --------------------------------------------------------------- */

void panel_init(FmPanel *p, int idx, const char *path) {
  memset(p, 0, sizeof *p);
  p->idx = idx;
  p->cursor = -1;
  p->pending_single = -1;
  p->cols = 1;
  FmLoc l;
  loc_local(&l, path);
  if (go_into(p, &l, false) != FM_OK) {
    char home[FM_PATH_MAX];
    if (!plat_place(PLACE_HOME, home, sizeof home)) fm_strlcpy(home, "/", sizeof home);
    loc_local(&l, home);
    go_into(p, &l, false);
  }
}

void panel_free(FmPanel *p) {
  vfs_free(&p->list);
  fm_free(p->view);
  hist_clear(p->back, &p->nback);
  hist_clear(p->fwd, &p->nfwd);
  memset(p, 0, sizeof *p);
}

/* ---- selection ---------------------------------------------------------- */

void panel_select_all(FmPanel *p, bool on) {
  for (int i = 0; i < p->list.count; i++) p->list.items[i].selected = 0;
  if (on)
    for (int i = 0; i < p->nview; i++) p->list.items[p->view[i]].selected = 1;
  panel_update_sel(p);
  ui_redraw();
}

static void select_only(FmPanel *p, int pos) {
  for (int i = 0; i < p->list.count; i++) p->list.items[i].selected = 0;
  if (pos >= 0 && pos < p->nview) p->list.items[p->view[pos]].selected = 1;
  panel_update_sel(p);
}

static void select_range(FmPanel *p, int a, int b, bool keep) {
  if (!keep)
    for (int i = 0; i < p->list.count; i++) p->list.items[i].selected = 0;
  if (a > b) { int t = a; a = b; b = t; }
  a = FM_MAX(a, 0);
  b = FM_MIN(b, p->nview - 1);
  for (int i = a; i <= b; i++) p->list.items[p->view[i]].selected = 1;
  panel_update_sel(p);
}

static void toggle(FmPanel *p, int pos) {
  if (pos < 0 || pos >= p->nview) return;
  FmEntry *e = &p->list.items[p->view[pos]];
  e->selected = !e->selected;
  panel_update_sel(p);
}

void panel_select_name(FmPanel *p, const char *name) {
  for (int i = 0; i < p->nview; i++) {
    if (strcmp(p->list.items[p->view[i]].name, name) == 0) {
      p->cursor = p->anchor = i;
      if (!ui.touch_mode) select_only(p, i);
      float rh = p->row_h > 0 ? p->row_h : ui.m.row_h;
      int cols = p->cols > 0 ? p->cols : 1;
      float top = (float)(i / cols) * (p->tile_h > 0 && cols > 1 ? p->tile_h : rh);
      float vh = p->body.h > 0 ? p->body.h : ui.h * 0.5f;
      p->scroll.y = FM_MAX(0.0f, top - vh * 0.4f);
      return;
    }
  }
}

FmEntry *panel_cursor_entry(FmPanel *p) {
  if (p->cursor < 0 || p->cursor >= p->nview) return NULL;
  return &p->list.items[p->view[p->cursor]];
}

int panel_selected(FmPanel *p, int **items) {
  int n = 0;
  *items = (int *)fm_alloc((size_t)(p->nsel > 0 ? p->nsel : 1) * sizeof(int));
  /* in view order, so operations run in the order the user sees */
  for (int i = 0; i < p->nview && n < p->nsel; i++)
    if (p->list.items[p->view[i]].selected) (*items)[n++] = p->view[i];
  return n;
}

char **panel_selected_paths(FmPanel *p, int *n) {
  int *items;
  int k = panel_selected(p, &items);
  char **out = (char **)fm_alloc((size_t)(k > 0 ? k : 1) * sizeof(char *));
  int m = 0;
  for (int i = 0; i < k; i++) {
    const FmEntry *e = &p->list.items[items[i]];
    char path[FM_PATH_MAX];
    if (p->list.loc.in_arc) out[m++] = fm_strdup(e->name);
    else if (vfs_entry_path(&p->list, e, path, sizeof path)) out[m++] = fm_strdup(path);
  }
  fm_free(items);
  *n = m;
  return out;
}

void panel_free_paths(char **paths, int n) {
  for (int i = 0; i < n; i++) fm_free(paths[i]);
  fm_free(paths);
}

bool panel_is_local(const FmPanel *p) {
  return !p->list.loc.in_arc && p->list.err == FM_OK;
}

/* ---- opening ------------------------------------------------------------ */

static int viewer_kind(int t) {
  switch (t) {
    case FT_IMAGE: case FT_GIF: case FT_SVG: return 1;
    case FT_AUDIO: return 2;
    case FT_VIDEO: return 3;
    case FT_TEXT: case FT_CODE: return 4;
    default: return 0;
  }
}

static void open_local_file(FmPanel *p, int item) {
  const FmEntry *e = &p->list.items[item];
  char path[FM_PATH_MAX];
  if (!vfs_entry_path(&p->list, e, path, sizeof path)) return;
  int kind = viewer_kind(e->type);
  if (kind == 0) {
    app_open(path, NULL, 0, 0);
    return;
  }
  /* siblings of the same kind, in view order, for next / previous */
  FmArena ar;
  arena_init(&ar, 32 * 1024);
  const char **sib = (const char **)fm_alloc((size_t)(p->nview > 0 ? p->nview : 1) * sizeof(char *));
  int n = 0, index = 0;
  for (int i = 0; i < p->nview; i++) {
    const FmEntry *o = &p->list.items[p->view[i]];
    if ((o->flags & FM_ST_DIR) || viewer_kind(o->type) != kind) continue;
    char sp[FM_PATH_MAX];
    if (!vfs_entry_path(&p->list, o, sp, sizeof sp)) continue;
    if (p->view[i] == item) index = n;
    sib[n++] = arena_strdup(&ar, sp);
  }
  app_open(path, sib, n, index);
  fm_free((void *)sib);
  arena_free(&ar);
}

void panel_open_item(FmPanel *p, int item) {
  if (item < 0 || item >= p->list.count) return;
  FmEntry *e = &p->list.items[item];
  FmLoc l = p->list.loc;
  if (e->flags & FM_ST_DIR) {
    if (l.in_arc) {
      fm_strlcat(l.inner, e->name, sizeof l.inner);
      fm_strlcat(l.inner, "/", sizeof l.inner);
    } else if (!fm_path_join(l.path, sizeof l.path, p->list.loc.path, e->name)) {
      return;
    }
    panel_go(p, &l, true);
    return;
  }
  if (!l.in_arc) {
    if (e->type == FT_ARCHIVE || e->type == FT_APK) {
      FmLoc a;
      memset(&a, 0, sizeof a);
      if (!vfs_entry_path(&p->list, e, a.path, sizeof a.path)) return;
      a.in_arc = true;
      FmErr err = go_into(p, &a, true);
      if (err == FM_OK) return;
      if (err == FM_ERR_PASSWORD) { app_panel_password(p, &a, false); return; }
      if (e->type == FT_ARCHIVE && err != FM_ERR_UNSUPPORTED && err != FM_ERR_FORMAT) {
        ui_toast("Can't open %s: %s", e->name, fm_err_str(err));
        return;
      }
      /* not something we can browse: let the system try */
    }
    open_local_file(p, item);
    return;
  }
  /* inside an archive: small plain entries open at once, others via a job */
  char pw[8];
  bool need_pw = e->encrypted && !ops_password_lookup(l.path, pw, sizeof pw);
  if (need_pw || e->size > 4u * 1024 * 1024 || e->arc_index < 0) {
    app_open_job(p, item);
    return;
  }
  char out[FM_PATH_MAX];
  FmErr err = vfs_materialize(&p->list, e, out, sizeof out);
  if (err != FM_OK) {
    if (err == FM_ERR_PASSWORD) app_open_job(p, item);
    else ui_toast("Can't open %s: %s", e->name, fm_err_str(err));
    return;
  }
  if (e->type == FT_ARCHIVE) {           /* nested archive: browse the cached copy */
    FmLoc a;
    memset(&a, 0, sizeof a);
    fm_strlcpy(a.path, out, sizeof a.path);
    a.in_arc = true;
    if (go_into(p, &a, true) == FM_OK) return;
  }
  app_open(out, NULL, 0, 0);
}

/* ---- keyboard ----------------------------------------------------------- */

static void ensure_visible(FmPanel *p, int pos) {
  if (pos < 0) return;
  int cols = p->cols > 0 ? p->cols : 1;
  float h = cols > 1 ? p->tile_h : p->row_h;
  if (h <= 0) return;
  float top = (float)(pos / cols) * h + DP(4);
  ui_scroll_to(&p->scroll, top - DP(4), top + h + DP(4), p->body.h);
}

static void move_cursor(FmPanel *p, int to, u16 mod) {
  if (p->nview == 0) return;
  g_kbd_cursor = true;
  to = FM_CLAMP(to, 0, p->nview - 1);
  bool shift = (mod & KMOD_SHIFT) != 0, ctrl = (mod & (KMOD_CTRL | KMOD_GUI)) != 0;
  if (p->cursor < 0) p->anchor = to;
  if (shift) select_range(p, p->anchor, to, false);
  else if (!ctrl && !p->select_mode) { select_only(p, to); p->anchor = to; }
  p->cursor = to;
  ensure_visible(p, to);
  ui_redraw();
}

static bool key_mod(SDL_Keycode k, u16 *mod) {
  static const u16 mods[] = { 0, KMOD_SHIFT, KMOD_CTRL, KMOD_CTRL | KMOD_SHIFT };
  for (int i = 0; i < FM_COUNT(mods); i++)
    if (ui_key(k, mods[i])) { *mod = mods[i]; return true; }
  return false;
}

static void panel_keys(FmPanel *p) {
  u16 mod = 0;
  int cols = p->cols > 0 ? p->cols : 1;
  int page = (int)(p->body.h / FM_MAX(1.0f, cols > 1 ? p->tile_h : p->row_h));
  if (page < 1) page = 1;
  int cur = p->cursor < 0 ? 0 : p->cursor;
  if (key_mod(SDLK_DOWN, &mod)) move_cursor(p, p->cursor < 0 ? 0 : cur + cols, mod);
  if (key_mod(SDLK_UP, &mod)) move_cursor(p, cur - cols, mod);
  if (cols > 1) {
    if (key_mod(SDLK_RIGHT, &mod)) move_cursor(p, cur + 1, mod);
    if (key_mod(SDLK_LEFT, &mod)) move_cursor(p, cur - 1, mod);
  }
  if (key_mod(SDLK_HOME, &mod)) move_cursor(p, 0, mod);
  if (key_mod(SDLK_END, &mod)) move_cursor(p, p->nview - 1, mod);
  if (key_mod(SDLK_PAGEDOWN, &mod)) move_cursor(p, cur + page * cols, mod);
  if (key_mod(SDLK_PAGEUP, &mod)) move_cursor(p, cur - page * cols, mod);
  if (ui_key(SDLK_RETURN, 0) || ui_key(SDLK_KP_ENTER, 0)) {
    FmEntry *e = panel_cursor_entry(p);
    if (e) panel_open_item(p, p->view[p->cursor]);
    return;
  }
  if (ui_key(SDLK_INSERT, 0) && p->cursor >= 0) {
    toggle(p, p->cursor);
    move_cursor(p, p->cursor + 1, KMOD_CTRL);
  }
  if (ui_key(SDLK_a, KMOD_CTRL)) panel_select_all(p, true);
  if (ui_key(SDLK_l, KMOD_CTRL)) panel_edit_path(p);
  if (ui_key(SDLK_f, KMOD_CTRL)) panel_open_search(p);
  if (ui_key(SDLK_LEFT, KMOD_ALT)) panel_back(p);
  if (ui_key(SDLK_RIGHT, KMOD_ALT)) panel_forward(p);
  if (ui_key(SDLK_UP, KMOD_ALT)) panel_up(p);
  if (ui_key(SDLK_BACKSPACE, 0)) {
    if (p->filter[0]) {
      char f[128];
      fm_strlcpy(f, p->filter, sizeof f);
      f[utf8_prev(f, (int)strlen(f))] = 0;
      panel_set_filter(p, f);
    } else {
      panel_up(p);
    }
    return;
  }
  /* type to filter */
  if (!ui.focus && ui.text_len > 0 && !p->search_open && ui_input_ok()) {
    if (!(ui.text_len == 1 && ui.text[0] == ' ' && !p->filter[0])) {
      char f[128];
      fm_strlcpy(f, p->filter, sizeof f);
      fm_strlcat(f, ui.text, sizeof f);
      panel_set_filter(p, f);
    } else if (p->cursor >= 0) {
      toggle(p, p->cursor);             /* space toggles like Insert */
    }
    ui.text_len = 0;
    ui.text[0] = 0;
  }
}

/* ---- drawing helpers ---------------------------------------------------- */

/* Thumbnail when there is one, else the file-type tile; plus small badges. */
static void draw_icon(FmPanel *p, const FmEntry *e, FmRect box) {
  bool drawn = false;
  if (conf.thumbnails && !p->list.loc.in_arc && !(e->flags & FM_ST_DIR) &&
      thumb_supported((FmType)e->type)) {
    char path[FM_PATH_MAX];
    if (vfs_entry_path(&p->list, e, path, sizeof path)) {
      SDL_Texture *t = thumb_get(path, e->mtime, e->size, (int)box.w);
      int tw = 0, th = 0;
      if (t && SDL_QueryTexture(t, NULL, NULL, &tw, &th) == 0 && tw > 0 && th > 0) {
        float s = FM_MIN(box.w / (float)tw, box.h / (float)th);
        FmRect d = rect_center(box, (float)tw * s, (float)th * s);
        /* a backdrop, so transparent or empty images still read as a tile */
        gfx_rect(d, T.surface3);
        gfx_tex(t, NULL, d, FM_HEX(0xFFFFFF));
        drawn = true;
      }
    }
  }
  if (!drawn) icon_file(e->type, box, T.dark);
  float bs = FM_MAX(DP(12), box.w * 0.36f);
  FmRect badge = { box.x + box.w - bs * 0.85f, box.y + box.h - bs * 0.95f, bs, bs };
  if (e->flags & (FM_ST_LINK | FM_ST_BROKEN)) {
    gfx_circle(badge.x + bs * 0.5f, badge.y + bs * 0.5f, bs * 0.5f, T.surface);
    icon_draw(IC_ARROW_RIGHT, rect_inset(badge, bs * 0.18f),
              (e->flags & FM_ST_BROKEN) ? T.danger : T.text2);
  } else if (e->encrypted) {
    gfx_circle(badge.x + bs * 0.5f, badge.y + bs * 0.5f, bs * 0.5f, T.surface);
    icon_draw(IC_LOCK, rect_inset(badge, bs * 0.16f), T.warn);
  }
}

static void secondary_text(const FmEntry *e, char *out, size_t cap, bool with_date) {
  char a[32], b[32];
  if (e->flags & FM_ST_DIR) {
    if (e->mtime && with_date) fm_snprintf(out, cap, "Folder  \xC2\xB7  %s", fm_fmt_time(e->mtime, b, sizeof b));
    else fm_strlcpy(out, "Folder", cap);
  } else if (e->mtime && with_date) {
    fm_snprintf(out, cap, "%s  \xC2\xB7  %s", fm_fmt_size(e->size, a, sizeof a),
                fm_fmt_time(e->mtime, b, sizeof b));
  } else {
    fm_strlcpy(out, fm_fmt_size(e->size, a, sizeof a), cap);
  }
}

static void draw_check(FmRect box, bool on) {
  if (on) {
    gfx_rrect(box, DP(6), T.accent);
    icon_draw(IC_CHECK, rect_inset(box, DP(3)), T.on_accent);
  } else {
    gfx_rrect_line(box, DP(6), DP(1.6f), T.text3);
  }
}

/* ---- rows and tiles ----------------------------------------------------- */

typedef struct RowCtx {
  FmPanel *p;
  bool active;
  int open_pos;          /* deferred: open this view position */
  bool row_hit;
  bool menu;             /* deferred: context menu at the pointer */
} RowCtx;

/* Pointer handling shared by list rows and grid tiles. */
static void item_input(RowCtx *c, int pos, int f) {
  FmPanel *p = c->p;
  FmEntry *e = &p->list.items[p->view[pos]];
  bool touch = ui.touch_mode;
  if (f & (UI_PRESS | UI_RCLICK)) {
    c->row_hit = true;
    app_panel_activate(p->idx);
  }
  if (touch || p->select_mode) {
    if (f & UI_LONG) {
      p->select_mode = true;
      p->cursor = p->anchor = pos;
      if (!e->selected) toggle(p, pos);
      ui_redraw();
    } else if (f & UI_CLICK) {
      p->cursor = pos;
      if (p->select_mode) {
        if (ui.mod & KMOD_SHIFT) select_range(p, p->anchor, pos, true);
        else toggle(p, pos);
        p->anchor = pos;
      } else {
        c->open_pos = pos;
      }
    }
    if ((f & UI_RCLICK) && !touch) {
      if (!e->selected) { select_only(p, pos); p->anchor = pos; }
      p->cursor = pos;
      c->menu = true;
    }
    return;
  }
  u16 m = ui.mod;
  bool ctrl = (m & (KMOD_CTRL | KMOD_GUI)) != 0, shift = (m & KMOD_SHIFT) != 0;
  if (f & UI_PRESS) {
    if (ctrl) {
      toggle(p, pos);
      p->anchor = pos;
    } else if (shift) {
      select_range(p, p->anchor < 0 ? pos : p->anchor, pos, false);
    } else {
      if (!e->selected) select_only(p, pos);
      else p->pending_single = pos;
      p->anchor = pos;
    }
    p->cursor = pos;
    if (f & UI_DCLICK) c->open_pos = pos;
    ui_redraw();
  }
  if ((f & UI_CLICK) && p->pending_single == pos && !ctrl && !shift) {
    select_only(p, pos);
    p->pending_single = -1;
  }
  if ((f & UI_DRAG) && !p->dragging && p->nsel > 0 && e->selected) {
    p->dragging = true;
    p->pending_single = -1;
    app_panel_drag_start(p);
  }
  if (f & UI_RCLICK) {
    if (!e->selected) { select_only(p, pos); p->anchor = pos; }
    p->cursor = pos;
    c->menu = true;
  }
}

static void row_bg(FmRect r, bool sel, bool cursor, bool active, int f) {
  float rad = FM_MIN(DP(T.row_radius), r.h * 0.5f);
  if (sel) gfx_rrect(r, rad, T.sel);
  else if (f & UI_HELD) gfx_rrect(r, rad, T.press);
  else if (f & UI_HOVER) gfx_rrect(r, rad, T.hover);
  if (cursor && active && g_kbd_cursor && !ui.touch_mode)
    gfx_rrect_line(r, rad, DP(1.2f), col_alpha(T.sel_line, sel ? 0.9f : 0.45f));
}

static void draw_list(RowCtx *c, FmRect body, float shift_y) {
  FmPanel *p = c->p;
  float rh = FM_MAX(ui.m.row_h, DP(ui.touch_mode ? FM_MAX(T.row_h * 1.15f, 52) : T.row_h));
  p->row_h = rh;
  p->cols = 1;
  p->tile_h = 0;
  float top_pad = DP(4);
  float content = (float)p->nview * rh + top_pad * 2;
  u32 sid = ui_idn(ui_id("panel.list"), (u32)p->idx);
  ui_scroll(&p->scroll, sid, body, content);
  bool wide = body.w >= DP(560) && !ui.touch_mode;
  float colw_size = DP(84), colw_date = DP(132);
  int first = (int)((p->scroll.y - top_pad - shift_y) / rh);
  int last = (int)((p->scroll.y + body.h - top_pad - shift_y) / rh) + 1;
  first = FM_MAX(first, 0);
  last = FM_MIN(last, p->nview - 1);
  gfx_clip_push(body);
  float fs = ui.m.font, fss = ui.m.font_small;
  float lh = font_line_h(fs), lhs = font_line_h(fss);
  bool one_line = rh < lh + lhs + DP(6);
  for (int i = first; i <= last; i++) {
    FmEntry *e = &p->list.items[p->view[i]];
    FmRect row = { body.x + DP(6), body.y + top_pad + (float)i * rh - p->scroll.y + shift_y,
                   body.w - DP(12), rh };
    int f = ui_hit(ui_idn(sid, (u32)p->view[i] + 1), rect_inset2(row, 0, DP(1)));
    item_input(c, i, f);
    row_bg(rect_inset2(row, 0, DP(1.5f)), e->selected, i == p->cursor, c->active, f);
    float x = row.x + DP(8);
    if (p->select_mode) {
      float cs = DP(20);
      draw_check(FM_RECT(x, row.y + (rh - cs) * 0.5f, cs, cs), e->selected);
      x += cs + DP(12);
    }
    float is = FM_MIN(rh * 0.68f, DP(40));
    draw_icon(p, e, FM_RECT(x, row.y + (rh - is) * 0.5f, is, is));
    x += is + DP(12);
    FmColor name_c = (e->flags & FM_ST_HIDDEN) ? T.text2 : T.text;
    FmColor sub_c = T.text2;
    if (e->selected && T.sel_text.a) { name_c = T.sel_text; sub_c = col_alpha(T.sel_text, 0.75f); }
    float right = row.x + row.w - DP(10);
    char sec[96];
    if (wide) {
      char a[32], b[32];
      float dx = right - colw_date;
      float ty = row.y + (rh - lhs) * 0.5f;
      if (e->mtime) font_draw(FONT_REGULAR, fss, dx, ty, fm_fmt_time(e->mtime, b, sizeof b), -1, sub_c);
      const char *sz = (e->flags & FM_ST_DIR) ? "Folder" : fm_fmt_size(e->size, a, sizeof a);
      float sw = font_width(FONT_REGULAR, fss, sz, -1);
      font_draw(FONT_REGULAR, fss, dx - DP(16) - sw, ty, sz, -1, sub_c);
      font_draw_mid_ellipsis(FONT_REGULAR, fs, x, row.y + (rh - lh) * 0.5f, e->name,
                             dx - colw_size - DP(16) - x, name_c);
    } else if (one_line) {
      /* dense themes: no room for a second line, so the size goes right */
      char a[32];
      const char *sz = (e->flags & FM_ST_DIR) ? "" : fm_fmt_size(e->size, a, sizeof a);
      float sw = *sz ? font_width(FONT_REGULAR, fss, sz, -1) : 0;
      if (*sz) font_draw(FONT_REGULAR, fss, right - sw, row.y + (rh - lhs) * 0.5f, sz, -1, sub_c);
      font_draw_mid_ellipsis(FONT_REGULAR, fs, x, row.y + (rh - lh) * 0.5f, e->name,
                             right - x - sw - (*sz ? DP(12) : 0), name_c);
    } else {
      float gap = DP(1);
      float y0 = row.y + (rh - lh - lhs - gap) * 0.5f;
      font_draw_mid_ellipsis(FONT_REGULAR, fs, x, y0, e->name, right - x, name_c);
      secondary_text(e, sec, sizeof sec, true);
      font_draw_ellipsis(FONT_REGULAR, fss, x, y0 + lh + gap, sec, right - x, sub_c);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&p->scroll, body, content);
}

static void draw_grid(RowCtx *c, FmRect body, float shift_y) {
  FmPanel *p = c->p;
  float want = DP(ui.touch_mode ? 116 : 108);
  float pad = DP(6);
  float avail = body.w - pad * 2;
  int cols = FM_MAX(1, (int)(avail / want));
  float tw = avail / (float)cols;
  float fs = ui.m.font_small;
  float th = tw * 0.72f + font_line_h(fs) * 2 + DP(14);
  p->cols = cols;
  p->tile_w = tw;
  p->tile_h = th;
  p->row_h = th;
  int rows = (p->nview + cols - 1) / cols;
  float content = (float)rows * th + pad * 2;
  u32 sid = ui_idn(ui_id("panel.grid"), (u32)p->idx);
  ui_scroll(&p->scroll, sid, body, content);
  int r0 = FM_MAX(0, (int)((p->scroll.y - pad - shift_y) / th));
  int r1 = FM_MIN(rows - 1, (int)((p->scroll.y + body.h - pad - shift_y) / th) + 1);
  gfx_clip_push(body);
  for (int r = r0; r <= r1; r++) {
    for (int k = 0; k < cols; k++) {
      int i = r * cols + k;
      if (i >= p->nview) break;
      FmEntry *e = &p->list.items[p->view[i]];
      FmRect t = { body.x + pad + (float)k * tw, body.y + pad + (float)r * th - p->scroll.y + shift_y,
                   tw, th };
      FmRect cell = rect_inset(t, DP(3));
      int f = ui_hit(ui_idn(sid, (u32)p->view[i] + 1), cell);
      item_input(c, i, f);
      row_bg(cell, e->selected, i == p->cursor, c->active, f);
      float is = tw * 0.62f;
      FmRect box = { t.x + (tw - is) * 0.5f, t.y + DP(8), is, is };
      draw_icon(p, e, box);
      if (p->select_mode) {
        float cs = DP(20);
        FmRect cb = { cell.x + DP(6), cell.y + DP(6), cs, cs };
        if (!e->selected) gfx_rrect(cb, DP(6), col_alpha(T.surface, 0.85f));
        draw_check(cb, e->selected);
      }
      float ty = box.y + is + DP(6);
      FmColor nc = (e->flags & FM_ST_HIDDEN) ? T.text2 : T.text;
      if (e->selected && T.sel_text.a) nc = T.sel_text;
      float nw = font_width(FONT_REGULAR, fs, e->name, -1);
      float maxw = cell.w - DP(10);
      if (nw <= maxw)
        font_draw(FONT_REGULAR, fs, cell.x + (cell.w - nw) * 0.5f, ty, e->name, -1, nc);
      else
        font_draw_mid_ellipsis(FONT_REGULAR, fs, cell.x + DP(5), ty, e->name, maxw, nc);
      char sec[64], a[32];
      if (e->flags & FM_ST_DIR) fm_strlcpy(sec, "Folder", sizeof sec);
      else fm_strlcpy(sec, fm_fmt_size(e->size, a, sizeof a), sizeof sec);
      float sw = font_width(FONT_REGULAR, fs, sec, -1);
      font_draw(FONT_REGULAR, fs, cell.x + (cell.w - FM_MIN(sw, maxw)) * 0.5f,
                ty + font_line_h(fs), sec, -1, T.text3);
    }
  }
  gfx_clip_pop();
  ui_scrollbar(&p->scroll, body, content);
}

/* ---- header ------------------------------------------------------------- */

static void draw_crumbs(FmPanel *p, FmRect area, bool active) {
  u32 base = ui_idn(ui_id("panel.crumb"), (u32)p->idx);
  if (p->editing_path) {
    u32 tid = ui_idn(base, 999);
    if (p->focus_pending) { ui_focus(tid); p->focus_pending = false; }
    FmRect fr = rect_inset2(area, 0, (area.h - DP(ui.touch_mode ? 40 : 32)) * 0.5f);
    int r = ui_textfield(tid, fr, p->path_edit, sizeof p->path_edit, "Type a path", 0);
    if (r & UI_TF_SUBMIT) {
      p->editing_path = false;
      ui_focus(0);
      panel_go_path(p, p->path_edit);
    } else if ((r & UI_TF_CANCEL) || ui.focus != tid) {
      p->editing_path = false;
    }
    return;
  }
  float fs = ui.m.font;
  float chev = DP(14), padx = DP(8);
  float w[PANEL_SEGS];
  float total = 0;
  for (int i = 0; i < p->nsegs; i++) {
    w[i] = font_width(i == p->nsegs - 1 ? FONT_BOLD : FONT_REGULAR, fs, p->seg_label[i], -1) + padx * 2;
    if (i == 0 && (strcmp(p->seg_label[0], "Home") == 0 || strcmp(p->seg_label[0], "Storage") == 0))
      w[i] += DP(18);
    total += w[i] + (i ? chev : 0);
  }
  float maxs = FM_MAX(0.0f, total - area.w);
  /* wheel and horizontal drag scroll the path when it is long */
  u32 did = ui_idn(base, 1000);
  if (maxs > 0 && ui_hover(area) && (ui.wheel != 0 || ui.wheel_x != 0)) {
    p->crumb_x -= (ui.wheel_x != 0 ? -ui.wheel_x : ui.wheel) * DP(48);
    ui.wheel = ui.wheel_x = 0;
  }
  if (maxs > 0 && ui_input_ok() && ui.down && ui.moved && rect_has(area, ui.press_x, ui.press_y) &&
      (ui.drag_owner == 0 || ui.drag_owner == did) &&
      fabsf(ui.mx - ui.press_x) > fabsf(ui.my - ui.press_y)) {
    if (ui.drag_owner != did) { ui.drag_owner = did; p->crumb_x0 = p->crumb_x; }
    p->crumb_x = p->crumb_x0 - (ui.mx - ui.press_x);
  }
  p->crumb_x = FM_CLAMP(p->crumb_x, 0.0f, maxs);
  gfx_clip_push(area);
  float x = area.x - p->crumb_x;
  float bh = FM_MIN(area.h - DP(8), DP(ui.touch_mode ? 40 : 30));
  float by = area.y + (area.h - bh) * 0.5f;
  int go = -1;
  for (int i = 0; i < p->nsegs; i++) {
    if (i) {
      icon_draw(IC_CHEVRON_RIGHT, FM_RECT(x + DP(1), area.y + (area.h - DP(12)) * 0.5f, DP(12), DP(12)),
                T.text3);
      x += chev;
    }
    FmRect b = { x, by, w[i], bh };
    bool last = i == p->nsegs - 1;
    if (gfx_visible(b)) {
      int f = ui_hit(ui_idn(base, (u32)i), b);
      if (f & UI_HOVER) { gfx_rrect(b, bh * 0.5f, T.hover); ui_set_cursor(SDL_SYSTEM_CURSOR_HAND); }
      if (f & UI_HELD) gfx_rrect(b, bh * 0.5f, T.press);
      if (f & UI_PRESS) app_panel_activate(p->idx);
      if ((f & UI_CLICK) && !last) go = i;
      if ((f & UI_CLICK) && last && !ui.touch_mode) panel_edit_path(p);
      float tx = b.x + padx;
      if (w[i] > font_width(FONT_REGULAR, fs, p->seg_label[i], -1) + padx * 2 + DP(4) && i == 0) {
        icon_draw(IC_HOME, FM_RECT(tx, b.y + (bh - DP(15)) * 0.5f, DP(15), DP(15)),
                  last ? T.text : T.text2);
        tx += DP(18);
      }
      FmColor c = last ? (active ? T.text : T.text) : T.text2;
      font_draw(last ? FONT_BOLD : FONT_REGULAR, fs, tx, b.y + (bh - font_line_h(fs)) * 0.5f,
                p->seg_label[i], -1, c);
    }
    x += w[i];
  }
  /* empty space after the path: click to type a path (desktop) */
  if (x < area.x + area.w && !ui.touch_mode) {
    FmRect rest = { x, area.y, area.x + area.w - x, area.h };
    int f = ui_hit(ui_idn(base, 1001), rest);
    if (f & UI_PRESS) app_panel_activate(p->idx);
    if (f & UI_CLICK) panel_edit_path(p);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_IBEAM);
  }
  gfx_clip_pop();
  /* fade the clipped edge so a scrolled path reads as "there is more" */
  if (p->crumb_x > 0.5f) {
    FmColor bg = active ? col_mix(T.surface2, T.accent, T.dark ? 0.10f : 0.07f) : T.surface2;
    for (int k = 0; k < 6; k++)
      gfx_rect(FM_RECT(area.x + (float)k * DP(3), area.y, DP(3), area.h), col_alpha(bg, 1.0f - (float)k / 6.0f));
  }
  if (go >= 0) {
    FmLoc l;
    segment_loc(p, go, &l);
    if (!loc_equal(&l, &p->list.loc)) panel_go(p, &l, true);
  }
}

static void draw_header(FmPanel *p, FmRect h, bool active) {
  u32 base = ui_idn(ui_id("panel.hdr"), (u32)p->idx);
  float bs = DP(ui.touch_mode ? 44 : 34);
  FmRect r = rect_inset2(h, DP(6), 0);
  bool narrow = r.w < DP(330);
  FmRect b;
  if (!narrow) {
    b = rect_cut_left(&r, bs);
    b = rect_center(b, bs, bs);
    if (p->nback > 0) {
      if (ui_icon_btn(ui_idn(base, 1), b, IC_BACK, T.text2, "Back (Alt+Left)")) {
        app_panel_activate(p->idx);
        panel_back(p);
      }
    } else {
      icon_draw(IC_BACK, rect_center(b, ui.m.icon, ui.m.icon), T.text3);
    }
  }
  b = rect_center(rect_cut_left(&r, bs), bs, bs);
  FmLoc up = p->list.loc;
  if (loc_up(&up)) {
    if (ui_icon_btn(ui_idn(base, 2), b, IC_UP, T.text2, "Up (Backspace)")) {
      app_panel_activate(p->idx);
      panel_up(p);
    }
  } else {
    icon_draw(IC_UP, rect_center(b, ui.m.icon, ui.m.icon), T.text3);
  }
  b = rect_center(rect_cut_right(&r, bs), bs, bs);
  if (ui_icon_btn(ui_idn(base, 3), b, IC_MORE, T.text2, "Menu")) {
    app_panel_activate(p->idx);
    app_panel_menu(p, b.x, b.y + b.h);
  }
  b = rect_center(rect_cut_right(&r, bs), bs, bs);
  if (ui_icon_btn(ui_idn(base, 4), b, IC_DRIVE, T.text2, "Places")) {
    app_panel_activate(p->idx);
    app_panel_places(p, b.x, b.y + b.h);
  }
  if (!narrow && !p->list.loc.in_arc) {
    b = rect_center(rect_cut_right(&r, bs), bs, bs);
    bool star = conf_is_bookmark(p->list.loc.path);
    if (ui_icon_btn(ui_idn(base, 5), b, star ? IC_STAR_FILL : IC_STAR, star ? T.warn : T.text2,
                    star ? "Remove bookmark" : "Bookmark this folder")) {
      conf_toggle_bookmark(p->list.loc.path);
      app_panel_navigated(p);
      ui_toast(star ? "Bookmark removed" : "Bookmarked");
    }
  }
  rect_cut_left(&r, DP(2));
  draw_crumbs(p, r, active);
}

static void draw_search(FmPanel *p, FmRect r) {
  u32 id = ui_idn(ui_id("panel.search"), (u32)p->idx);
  FmRect in = rect_inset2(r, DP(10), DP(6));
  FmRect close = rect_cut_right(&in, in.h);
  if (p->focus_pending) { ui_focus(id); p->focus_pending = false; }
  char f[128];
  fm_strlcpy(f, p->filter, sizeof f);
  int res = ui_textfield(id, rect_inset2(in, 0, 0), f, sizeof f, "Filter this folder", 0);
  if (res & UI_TF_CHANGED) panel_set_filter(p, f);
  icon_draw(IC_SEARCH, FM_RECT(in.x + in.w - DP(26), in.y + (in.h - DP(16)) * 0.5f, DP(16), DP(16)),
            T.text3);
  if (ui_icon_btn(ui_idn(id, 1), close, IC_CLOSE, T.text2, "Close search") || (res & UI_TF_CANCEL)) {
    p->search_open = false;
    panel_set_filter(p, "");
    ui_focus(0);
  }
}

static void fmt_count(int v, char *out, size_t cap) {
  char tmp[24];
  fm_snprintf(tmp, sizeof tmp, "%d", v);
  size_t n = strlen(tmp), o = 0;
  for (size_t i = 0; i < n && o + 2 < cap; i++) {
    if (i && (n - i) % 3 == 0) out[o++] = ',';
    out[o++] = tmp[i];
  }
  out[o] = 0;
}

static void draw_footer(FmPanel *p, FmRect r) {
  u32 base = ui_idn(ui_id("panel.foot"), (u32)p->idx);
  float fs = ui.m.font_small;
  FmRect in = rect_inset2(r, DP(12), 0);
  char buf[160], a[32];
  float ty = in.y + (in.h - font_line_h(fs)) * 0.5f;
  /* right side: view mode and sort */
  float bs = in.h;
  FmRect vb = rect_cut_right(&in, bs);
  bool grid = conf.view[p->idx] == VIEW_GRID;
  if (ui_icon_btn(ui_idn(base, 1), vb, grid ? IC_LIST : IC_GRID, T.text2,
                  grid ? "List view" : "Grid view")) {
    conf.view[p->idx] = grid ? VIEW_LIST : VIEW_GRID;
    app_panel_navigated(p);
  }
  static const char *const kSort[] = { "Name", "Size", "Date", "Type" };
  const char *sl = kSort[FM_CLAMP(conf.sort[p->idx], 0, 3)];
  float sw = font_width(FONT_REGULAR, fs, sl, -1) + DP(32);
  FmRect sb = rect_cut_right(&in, sw);
  int f = ui_hit(ui_idn(base, 2), sb);
  if (f & UI_HOVER) gfx_rrect(rect_inset2(sb, 0, DP(2)), DP(8), T.hover);
  icon_draw(conf.sort_desc[p->idx] ? IC_ARROW_DOWN : IC_ARROW_UP,
            FM_RECT(sb.x + DP(6), sb.y + (sb.h - DP(13)) * 0.5f, DP(13), DP(13)), T.text2);
  font_draw(FONT_REGULAR, fs, sb.x + DP(22), ty, sl, -1, T.text2);
  if (f & UI_CLICK) {
    app_panel_activate(p->idx);
    app_panel_menu(p, sb.x, sb.y - DP(4));
  }
  /* left: filter chip, then the counts */
  if (p->filter[0] && !p->search_open) {
    fm_snprintf(buf, sizeof buf, "\"%s\"", p->filter);
    float cw = FM_MIN(font_width(FONT_REGULAR, fs, buf, -1) + DP(34), in.w * 0.5f);
    FmRect chip = { in.x - DP(4), in.y + DP(3), cw, in.h - DP(6) };
    gfx_rrect(chip, chip.h * 0.5f, T.accent_soft);
    icon_draw(IC_FILTER, FM_RECT(chip.x + DP(7), chip.y + (chip.h - DP(12)) * 0.5f, DP(12), DP(12)),
              T.accent);
    font_draw_ellipsis(FONT_REGULAR, fs, chip.x + DP(22), ty, buf, cw - DP(28), T.accent);
    if (ui_hit(ui_idn(base, 3), chip) & UI_CLICK) panel_set_filter(p, "");
    in.x += cw + DP(4);
    in.w -= cw + DP(4);
  }
  if (p->nsel > 0) {
    fm_snprintf(buf, sizeof buf, "%d selected  \xC2\xB7  %s", p->nsel,
                fm_fmt_size(p->sel_bytes, a, sizeof a));
    font_draw_ellipsis(FONT_BOLD, fs, in.x, ty, buf, in.w - DP(4), T.accent);
  } else if (p->list.err == FM_OK) {
    char nd[24], nf[24];
    fmt_count(p->ndirs, nd, sizeof nd);
    fmt_count(p->nfiles, nf, sizeof nf);
    if (p->ndirs && p->nfiles)
      fm_snprintf(buf, sizeof buf, "%s folder%s, %s file%s  \xC2\xB7  %s", nd, p->ndirs == 1 ? "" : "s",
                  nf, p->nfiles == 1 ? "" : "s", fm_fmt_size(p->list.total_size, a, sizeof a));
    else if (p->ndirs)
      fm_snprintf(buf, sizeof buf, "%s folder%s", nd, p->ndirs == 1 ? "" : "s");
    else
      fm_snprintf(buf, sizeof buf, "%s file%s  \xC2\xB7  %s", nf, p->nfiles == 1 ? "" : "s",
                  fm_fmt_size(p->list.total_size, a, sizeof a));
    font_draw_ellipsis(FONT_REGULAR, fs, in.x, ty, buf, in.w - DP(4), T.text2);
  }
}

/* ---- empty and error states --------------------------------------------- */

static void draw_state(FmPanel *p, FmRect body) {
  u32 base = ui_idn(ui_id("panel.state"), (u32)p->idx);
  FmErr e = p->list.err;
  FmIcon ic = IC_FOLDER_OPEN;
  FmColor icc = T.text3;
  const char *title, *sub = NULL;
  char tbuf[200];
  if (e == FM_OK && p->filter[0]) {
    ic = IC_SEARCH;
    fm_snprintf(tbuf, sizeof tbuf, "Nothing matches \"%s\"", p->filter);
    title = tbuf;
  } else if (e == FM_OK) {
    title = "This folder is empty";
  } else {
    ic = e == FM_ERR_PASSWORD ? IC_LOCK : IC_WARN;
    icc = e == FM_ERR_PASSWORD ? T.text2 : T.warn;
    switch (e) {
      case FM_ERR_ACCESS: title = "Permission denied"; sub = "You don't have access to this folder."; break;
      case FM_ERR_NOT_FOUND: title = "Folder not found"; sub = "It may have been moved or deleted."; break;
      case FM_ERR_PASSWORD: title = "This archive is encrypted"; sub = "Enter the password to see the files."; break;
      case FM_ERR_FORMAT: title = "Can't read this archive"; sub = "The file is damaged or not an archive."; break;
      case FM_ERR_UNSUPPORTED: title = "Not supported"; sub = "This kind of archive can't be opened."; break;
      default: title = fm_err_str(e); break;
    }
  }
  /* Android 11+: without "all files" access the storage lists as empty */
  bool want_grant = (e == FM_ERR_ACCESS || (e == FM_OK && !p->filter[0])) && !plat_storage_granted();
  if (want_grant && e == FM_OK) {
    ic = IC_LOCK;
    title = "No access to your files";
  }
  if (want_grant) sub = "Allow access to all files to browse your storage.";
  float is = DP(52);
  float bh = DP(ui.touch_mode ? 44 : 36);
  float h = is + DP(14) + font_line_h(ui.m.font_title) + (sub ? font_line_h(ui.m.font) + DP(4) : 0) +
            (e != FM_OK || p->filter[0] || want_grant ? bh + DP(18) : 0);
  float y = body.y + FM_MAX(DP(10), (body.h - h) * 0.42f);
  float cx = body.x + body.w * 0.5f;
  gfx_circle(cx, y + is * 0.5f, is * 0.62f, col_alpha(icc, 0.10f));
  icon_draw(ic, FM_RECT(cx - is * 0.3f, y + is * 0.2f, is * 0.6f, is * 0.6f), icc);
  y += is + DP(14);
  ui_label(FM_RECT(body.x + DP(12), y, body.w - DP(24), font_line_h(ui.m.font_title)), title, FONT_BOLD,
           ui.m.font_title, T.text, UI_CENTER);
  y += font_line_h(ui.m.font_title);
  if (sub) {
    y += DP(4);
    ui_label(FM_RECT(body.x + DP(12), y, body.w - DP(24), font_line_h(ui.m.font)), sub, FONT_REGULAR,
             ui.m.font, T.text2, UI_CENTER);
    y += font_line_h(ui.m.font);
  }
  y += DP(18);
  if (e == FM_OK && p->filter[0]) {
    float bw = DP(130);
    if (ui_button(ui_idn(base, 1), FM_RECT(cx - bw * 0.5f, y, bw, bh), IC_CLOSE, "Clear filter",
                  UI_BTN_TONAL))
      panel_set_filter(p, "");
  } else if (e != FM_OK || want_grant) {
    float bw = DP(120), gap = DP(10);
    if (want_grant) {
      float gw = DP(150);
      if (ui_button(ui_idn(base, 2), FM_RECT(cx - gw * 0.5f, y, gw, bh), IC_UNLOCK, "Grant access",
                    UI_BTN_FILLED))
        plat_storage_request();
    } else if (e == FM_ERR_PASSWORD) {
      float gw = DP(170);
      if (ui_button(ui_idn(base, 3), FM_RECT(cx - gw * 0.5f, y, gw, bh), IC_KEY, "Enter password",
                    UI_BTN_FILLED))
        app_panel_password(p, &p->list.loc, false);
    } else {
      if (ui_button(ui_idn(base, 4), FM_RECT(cx - bw - gap * 0.5f, y, bw, bh), IC_UP, "Go up",
                    UI_BTN_TONAL))
        panel_up(p);
      if (ui_button(ui_idn(base, 5), FM_RECT(cx + gap * 0.5f, y, bw, bh), IC_REFRESH, "Retry",
                    UI_BTN_OUTLINE))
        panel_refresh(p);
    }
  }
}

/* ---- frame -------------------------------------------------------------- */

void panel_frame(FmPanel *p, FmRect r, bool active) {
  p->rect = r;
  float rad = ui.m.radius;
  gfx_shadow(r, rad, DP(active ? 14 : 10), active ? T.shadow : col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, rad, T.surface);

  /* header: tinted with the accent when this is the active panel */
  float t = ui_anim(ui_idn(ui_id("panel.act"), (u32)p->idx), active ? 1.0f : 0.0f, 12.0f);
  FmRect in = r;
  FmRect hdr = rect_cut_top(&in, ui.m.bar_h);
  FmColor hbg = col_mix(T.surface2, T.accent, (T.dark ? 0.10f : 0.07f) * t);
  gfx_rrect4(hdr, rad, rad, 0, 0, hbg);
  ui_divider(r.x, r.x + r.w, hdr.y + hdr.h - 1);
  if (T.panel_border.a) gfx_rrect_line(r, rad, DP(T.panel_border_w), T.panel_border);

  /* a press anywhere on the card makes it active */
  if (ui_input_ok() && ui.pressed && rect_has(r, ui.press_x, ui.press_y)) app_panel_activate(p->idx);

  if (active && ui_input_ok() && !ui.focus) panel_keys(p);
  if (active && ui_input_ok() && ui.focus && p->search_open) {
    if (ui_key(SDLK_DOWN, 0) && p->nview > 0) { ui_focus(0); move_cursor(p, 0, 0); }
  }

  if (ui.pressed) g_kbd_cursor = false;
  draw_header(p, hdr, active);
  if (t > 0.01f)
    gfx_rrect(FM_RECT(r.x + rad, r.y, r.w - rad * 2, DP(3)), DP(1.5f), col_alpha(T.accent, t));
  if (p->search_open) {
    FmRect s = rect_cut_top(&in, search_h());
    draw_search(p, s);
  }
  FmRect foot = rect_cut_bottom(&in, footer_h());
  ui_divider(r.x + DP(12), r.x + r.w - DP(12), foot.y);
  FmRect body = in;
  p->body = body;

  float nav = ui_anim(ui_idn(ui_id("panel.nav"), (u32)p->idx), 0.0f, 16.0f);
  float shift_y = DP(10) * nav;

  /* the empty area of the list is hit first, so rows drawn on top keep the press */
  int bgf = p->nview > 0 ? ui_hit(ui_idn(ui_id("panel.bg"), (u32)p->idx), body) : 0;

  RowCtx c;
  memset(&c, 0, sizeof c);
  c.p = p;
  c.active = active;
  c.open_pos = -1;
  if (p->nview == 0) {
    draw_state(p, body);
  } else if (conf.view[p->idx] == VIEW_GRID) {
    draw_grid(&c, body, shift_y);
  } else {
    draw_list(&c, body, shift_y);
  }
  /* empty area of the list: clears the selection (desktop) or opens the menu */
  {
    int f = bgf;
    if ((f & UI_PRESS) && !c.row_hit) {
      app_panel_activate(p->idx);
      if (!ui.touch_mode && !(ui.mod & (KMOD_CTRL | KMOD_SHIFT))) panel_select_all(p, false);
    }
    if ((f & UI_RCLICK) && !c.row_hit) {
      panel_select_all(p, false);
      app_panel_menu(p, ui.mx, ui.my);
    }
  }
  if (!ui.down) p->dragging = false;

  draw_footer(p, foot);

  if (active) gfx_rrect_line(r, rad, DP(1.5f), col_alpha(T.accent, 0.55f * t));
  else gfx_rrect_line(r, rad, DP(1), T.border);

  if (c.menu) app_panel_context(p, ui.mx, ui.my);
  if (c.open_pos >= 0 && c.open_pos < p->nview) panel_open_item(p, p->view[c.open_pos]);
}
