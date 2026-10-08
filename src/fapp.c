/* fapp.c -- the application: two panels, the action bar, jobs, dialogs.
**
** Design decisions:
**   - X-plore's model: every operation goes from the active panel to the
**     other one. The middle action bar shows arrows that point at the
**     target, so "Copy" never needs a destination dialog.
**   - One modal dialog at a time, described by the D struct. Questions from
**     jobs (name conflicts, passwords) wait until no user dialog is open,
**     then take over; the worker sleeps until they are answered.
**   - Jobs, the sidebar and the audio bar take layout space instead of
**     floating over the panels, so nothing hides rows the user can click.
**   - Settings are saved ~0.8 s after the last change (a timer wakes the
**     loop) and at exit; --shot runs use defaults and never save.
*/
#include "fapp.h"
#include "fpanel.h"
#include "flayout.h"
#include "fops.h"
#include "fconf.h"
#include "fview.h"
#include "fthumb.h"
#include "ftitle.h"
#include "farc.h"
#include "flib.h"
#include "fonline.h"
#include "fphoto.h"                    /* photos */
#include "faudio_online.h"             /* audio */

/* ---- state -------------------------------------------------------------- */

enum {
  DLG_NONE, DLG_RENAME, DLG_NEWDIR, DLG_NEWFILE, DLG_DELETE, DLG_PROPS, DLG_COMPRESS,
  DLG_EXTRACT, DLG_CONFLICT, DLG_PASSWORD, DLG_SETTINGS, DLG_EXIT
};

typedef struct Dialog {
  int kind;
  int panel;                    /* panel the dialog acts on */
  char name[256];
  char orig[256];
  char err[200];
  char **paths;                 /* captured targets (delete, compress, extract, props) */
  int npaths;
  bool permanent;
  /* properties */
  FmJob *job;
  FmJobInfo info;
  bool have_info;
  FmEntry entry;                /* single item (copy; name points into `name`) */
  bool single;
  bool in_arc;
  char where[FM_PATH_MAX];
  /* compress */
  int fmt;                      /* index into kFmts */
  float level;
  char pw[128];
  bool show_pw;
  int zip_mode;                 /* 0 AES-256, 1 ZipCrypto */
  bool enc_names;
  /* extract */
  bool subfolder;
  bool from_arc;                /* extracting from inside the panel's archive */
  /* conflict / password */
  FmJob *qjob;
  FmAsk ask;
  bool apply_all;
  int pw_panel;                 /* listing password: panel index, -1 for a job */
  FmLoc pw_loc;
  /* settings */
  FmScroll scroll;
  float zoom;
  /* exit */
  bool jobs_warn;
} Dialog;

static FmPanel g_p[2];
static int g_active;
static FmLayout g_L;
static int g_single_tab;
static bool g_sidebar_open = true;
static Dialog D;

static bool g_shot;               /* --shot: defaults, never saved */
static bool g_force_touch;
static bool g_conf_dirty;
static u64 g_conf_dirty_t;
static SDL_atomic_t g_timer_armed;

static FmVolume g_vols[24];
static int g_nvols;
static bool g_vols_dirty = true;

static char g_space_path[FM_PATH_MAX];
static u64 g_space_total, g_space_free;
static bool g_space_dirty = true;

static char **g_clip;
static int g_nclip;
static bool g_clip_cut;
static FmLoc g_clip_loc;

static bool g_drag;
static int g_drag_from;
static int g_drag_n;

static int g_menu_panel;
static FmJob *g_job_menu;

typedef struct Place {
  FmIcon icon;
  char label[64];
  char path[FM_PATH_MAX];
  int section;                  /* 0 storage, 1 places, 2 bookmarks, 3 recent */
  u64 total, free_b;
} Place;
#define PLACES_MAX 40
static Place g_places[PLACES_MAX];
static int g_nplaces;
static bool g_places_dirty = true;
static FmScroll g_side_scroll;

static u32 ID_CTX, ID_PMENU, ID_PLACES, ID_JOBMENU;

static FmPanel *P(void) { return &g_p[g_active]; }
static FmPanel *O(void) { return &g_p[1 - g_active]; }

/* ---- formats for compress ----------------------------------------------- */

typedef struct FmtInfo { FmArcFmt fmt; const char *label; const char *ext; } FmtInfo;
static const FmtInfo kFmts[] = {
  { ARC_ZIP, "ZIP", ".zip" }, { ARC_7Z, "7Z", ".7z" }, { ARC_TAR, "TAR", ".tar" },
  { ARC_TAR_GZ, "TAR.GZ", ".tar.gz" }, { ARC_TAR_BZ2, "TAR.BZ2", ".tar.bz2" },
  { ARC_TAR_XZ, "TAR.XZ", ".tar.xz" }, { ARC_TAR_ZST, "TAR.ZST", ".tar.zst" },
};

/* ---- small helpers ------------------------------------------------------ */

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

static Uint32 SDLCALL conf_timer(Uint32 interval, void *ud) {
  FM_UNUSED(interval); FM_UNUSED(ud);
  SDL_AtomicSet(&g_timer_armed, 0);
  app_wake();
  return 0;
}

static void conf_dirty(void) {
  if (g_shot) return;
  g_conf_dirty = true;
  g_conf_dirty_t = SDL_GetTicks64();
  if (SDL_AtomicCAS(&g_timer_armed, 0, 1)) SDL_AddTimer(900, conf_timer, NULL);
}

static void conf_flush(bool force) {
  if (!g_conf_dirty || g_shot) return;
  if (!force && SDL_GetTicks64() - g_conf_dirty_t < 800) {
    if (SDL_AtomicCAS(&g_timer_armed, 0, 1)) SDL_AddTimer(400, conf_timer, NULL);
    return;
  }
  g_conf_dirty = false;
  conf_save();
}

static const char *plural(int n, const char *one, const char *many) { return n == 1 ? one : many; }

static void fmt_count(u64 v, char *out, size_t cap) {
  char tmp[32];
  fm_snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
  size_t n = strlen(tmp), o = 0;
  for (size_t i = 0; i < n && o + 2 < cap; i++) {
    if (i && (n - i) % 3 == 0) out[o++] = ',';
    out[o++] = tmp[i];
  }
  out[o] = 0;
}

static void fmt_duration(int s, char *out, size_t cap) {
  if (s < 60) fm_snprintf(out, cap, "%d s", s);
  else if (s < 3600) fm_snprintf(out, cap, "%d min %02d s", s / 60, s % 60);
  else fm_snprintf(out, cap, "%d h %02d min", s / 3600, (s / 60) % 60);
}

static void update_title(void) {
  char t[300], name[256];
  loc_title(&P()->list.loc, name, sizeof name);
  fm_snprintf(t, sizeof t, "%s - MMC File Manager", name);
  SDL_SetWindowTitle(app.win, t);
}

/* Folder of a panel on disk (an archive panel: the folder holding it). */
static void panel_disk_dir(const FmPanel *p, char *out, size_t cap) {
  fm_strlcpy(out, p->list.loc.path, cap);
  if (p->list.loc.in_arc) fm_path_parent(out);
}

static void refresh_folder(const char *dir) {
  if (!dir || !dir[0]) return;
  for (int i = 0; i < 2; i++) {
    FmPanel *p = &g_p[i];
    if (!p->list.loc.in_arc && same_path(p->list.loc.path, dir)) panel_refresh(p);
    else if (p->list.loc.in_arc) {
      /* archive rewritten under us: reopen it */
      char d[FM_PATH_MAX];
      panel_disk_dir(p, d, sizeof d);
      if (same_path(d, dir) && !plat_exists(p->list.loc.path)) panel_up(p);
    }
  }
  g_space_dirty = true;
}

static void apply_touch(void) {
  bool t;
#ifdef FM_MOBILE
  t = true;
#else
  t = false;
#endif
  if (conf.touch == 0) t = false;
  if (conf.touch == 1) t = true;
  if (g_force_touch) t = true;
  ui.touch_mode = t;
  ui_update_scale();
}

/* ---- places ------------------------------------------------------------- */

static FmIcon vol_icon(FmVolKind k) {
  switch (k) {
    case VOL_HOME: return IC_HOME;
    case VOL_REMOVABLE: case VOL_USB: return IC_USB;
    case VOL_NETWORK: return IC_NETWORK;
    case VOL_OPTICAL: return IC_DISC;
    case VOL_INTERNAL: return IC_PHONE;
    case VOL_SDCARD: return IC_SDCARD;
    case VOL_FOLDER: return IC_FOLDER;
    default: return IC_DRIVE;
  }
}

static void place_add(FmIcon ic, const char *label, const char *path, int section) {
  if (g_nplaces >= PLACES_MAX) return;
  for (int i = 0; i < g_nplaces; i++)
    if (g_places[i].section == section && same_path(g_places[i].path, path)) return;
  Place *p = &g_places[g_nplaces++];
  memset(p, 0, sizeof *p);
  p->icon = ic;
  fm_strlcpy(p->label, label, sizeof p->label);
  fm_strlcpy(p->path, path, sizeof p->path);
  p->section = section;
}

static void build_places(void) {
  if (g_vols_dirty) {
    g_nvols = plat_volumes(g_vols, FM_COUNT(g_vols));
    g_vols_dirty = false;
  }
  g_nplaces = 0;
  for (int i = 0; i < g_nvols; i++) {
    place_add(vol_icon(g_vols[i].kind), g_vols[i].name, g_vols[i].path, 0);
    if (g_nplaces > 0) {
      g_places[g_nplaces - 1].total = g_vols[i].total;
      g_places[g_nplaces - 1].free_b = g_vols[i].free;
    }
  }
  static const struct { FmPlace p; FmIcon ic; const char *label; } kPl[] = {
    { PLACE_DESKTOP, IC_DESKTOP, "Desktop" }, { PLACE_DOCUMENTS, IC_DOCUMENTS, "Documents" },
    { PLACE_DOWNLOADS, IC_DOWNLOAD, "Downloads" }, { PLACE_PICTURES, IC_PICTURES, "Pictures" },
    { PLACE_MUSIC, IC_MUSIC, "Music" }, { PLACE_VIDEOS, IC_VIDEO, "Videos" },
  };
  char path[FM_PATH_MAX];
  place_add(IC_LIBRARY, "Media library", "", 1);   /* flib: empty path = the library view */
  place_add(IC_PLAY_BADGE, "Online videos", "online:", 1);   /* online: the online videos view */
  place_add(IC_ALBUM, "Online photos", "photos:", 1);        /* photos: the online photos view */
  place_add(IC_RADIO, "Online audio", "audio:", 1);          /* audio: the online audio view */
  for (int i = 0; i < FM_COUNT(kPl); i++)
    if (plat_place(kPl[i].p, path, sizeof path) && plat_is_dir(path))
      place_add(kPl[i].ic, kPl[i].label, path, 1);
  for (int i = 0; i < conf.nbookmarks; i++) {
    const char *b = conf.bookmarks[i];
    place_add(IC_STAR_FILL, fm_path_base(b)[0] ? fm_path_base(b) : b, b, 2);
  }
  int nh = 0;
  for (int i = 0; i < conf.nhistory && nh < 6; i++) {
    const char *h = conf.history[i];
    if (same_path(h, P()->list.loc.path)) continue;
    place_add(IC_HISTORY, fm_path_base(h)[0] ? fm_path_base(h) : h, h, 3);
    nh++;
  }
  g_places_dirty = false;
}

/* ---- media library (flib) ------------------------------------------------- */

/* Opens over the panels; the active folder is offered as a library folder. */
static void open_library(int section) {
  lib_ui_open(section, panel_is_local(P()) ? P()->list.loc.path : NULL);
}

/* online: the online videos view, over the panels like the library */
static void open_online(void) {
  if (lib_ui_is_open()) lib_ui_close();
  if (photo_is_open()) photo_close();   /* photos */
  if (aonline_is_open()) aonline_close();   /* audio */
  online_open(NULL);
}

static void online_reveal_path(const char *path) {   /* online: "Show in folder" */
  online_close();
  panel_go_path(P(), path);
}

/* photos: the online photos view, over the panels like the online videos */
static void open_photos(void) {
  if (lib_ui_is_open()) lib_ui_close();
  if (online_is_open()) online_close();
  if (aonline_is_open()) aonline_close();   /* audio */
  photo_open(NULL);
}

static void photo_reveal_path(const char *path) {    /* photos: "Show in folder" */
  photo_close();
  panel_go_path(P(), path);
}

/* audio: the online audio view, over the panels like the other online views */
static void open_aonline(void) {
  if (lib_ui_is_open()) lib_ui_close();
  if (online_is_open()) online_close();
  if (photo_is_open()) photo_close();
  aonline_open(NULL);
}

static void aonline_reveal_path(const char *path) {  /* audio: "Show in folder" */
  aonline_close();
  panel_go_path(P(), path);
}

/* "Show in folder" from the library: back to the panels, file selected. */
static void lib_reveal(const char *path) {
  lib_ui_close();
  panel_go_path(P(), path);
}

static void go_place(int i, int panel) {
  if (i < 0 || i >= g_nplaces) return;
  FmPanel *p = &g_p[panel];
  if (!g_places[i].path[0]) { open_library(LIB_SEC_SONGS); return; }   /* flib */
  if (!strcmp(g_places[i].path, "online:")) { open_online(); return; }   /* online */
  if (!strcmp(g_places[i].path, "photos:")) { open_photos(); return; }   /* photos */
  if (!strcmp(g_places[i].path, "audio:")) { open_aonline(); return; }    /* audio */
  if (!plat_is_dir(g_places[i].path)) {
    ui_toast("%s is not available", g_places[i].label);
    return;
  }
  FmLoc l;
  loc_local(&l, g_places[i].path);
  panel_go(p, &l, true);
}

/* ---- viewer ------------------------------------------------------------- */

void app_open(const char *path, const char *const *siblings, int n, int index) {
  FmType t = fm_type_from_name(path);
  if (t == FT_AUDIO || t == FT_VIDEO) lib_note_played(path);   /* flib: recently played */
  const FmViewer *v = view_for(t, path);
  if (v && v->open && v->open(path, siblings, n, index)) {
    app.viewer = v;
    ui_redraw();
    return;
  }
  if (!plat_open_external(path)) ui_toast("No app found to open %s", fm_path_base(path));
}

void app_close_viewer(void) {
  const FmViewer *v = app.viewer;
  if (!v) return;
  app.viewer = NULL;
  if (v->close) v->close();
  ui_redraw();
}

void app_render_reset(void) {
  font_reset();
  thumb_reset();
  online_render_reset();               /* online */
  photo_render_reset();                /* photos */
}

/* ---- dialogs: opening --------------------------------------------------- */

static void dlg_free_paths(void) {
  if (D.paths) panel_free_paths(D.paths, D.npaths);
  D.paths = NULL;
  D.npaths = 0;
}

static void dlg_close(void) {
  if (D.kind == DLG_PROPS && D.job) ops_cancel(D.job);
  dlg_free_paths();
  memset(D.pw, 0, sizeof D.pw);
  memset(&D, 0, sizeof D);
  D.pw_panel = -1;
  ui_focus(0);
  ui_redraw();
}

static void dlg_open(int kind, int panel) {
  dlg_close();
  D.kind = kind;
  D.panel = panel;
  D.pw_panel = -1;
  ui_redraw();
}

static bool need_selection(FmPanel *p) {
  if (p->nsel > 0) return true;
  ui_toast("Select files or folders first");
  return false;
}

static void open_rename(void) {
  FmPanel *p = P();
  if (p->list.loc.in_arc) { ui_toast("Archives are read-only"); return; }
  int *items;
  int n = panel_selected(p, &items);
  if (n != 1) {
    fm_free(items);
    ui_toast(n == 0 ? "Select one item to rename" : "Select only one item to rename");
    return;
  }
  dlg_open(DLG_RENAME, g_active);
  fm_strlcpy(D.name, p->list.items[items[0]].name, sizeof D.name);
  fm_strlcpy(D.orig, D.name, sizeof D.orig);
  fm_free(items);
}

static void open_new(bool file) {
  FmPanel *p = P();
  if (!panel_is_local(p)) { ui_toast("Can't create items here"); return; }
  dlg_open(file ? DLG_NEWFILE : DLG_NEWDIR, g_active);
  const char *base = file ? "New file.txt" : "New folder";
  char tmp[FM_PATH_MAX];
  fm_path_join(tmp, sizeof tmp, p->list.loc.path, base);
  if (plat_exists(tmp) && ops_unique_name(p->list.loc.path, base, tmp, sizeof tmp))
    fm_strlcpy(D.name, fm_path_base(tmp), sizeof D.name);
  else
    fm_strlcpy(D.name, base, sizeof D.name);
}

static void start_delete(char **paths, int n, bool permanent) {
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  bool trash = conf.use_trash && !permanent && ops_trash_available();
  s.kind = trash ? JOB_TRASH : JOB_DELETE;
  s.srcs = (const char *const *)paths;
  s.nsrc = n;
  if (!ops_start(&s)) ui_toast("Too many jobs are running");
}

static void open_delete(bool permanent) {
  FmPanel *p = P();
  if (p->list.loc.in_arc) { ui_toast("Archives are read-only"); return; }
  if (!need_selection(p)) return;
  int n;
  char **paths = panel_selected_paths(p, &n);
  if (!conf.confirm_delete) {
    start_delete(paths, n, permanent);
    panel_free_paths(paths, n);
    panel_select_all(p, false);
    return;
  }
  dlg_open(DLG_DELETE, g_active);
  D.paths = paths;
  D.npaths = n;
  D.permanent = permanent || !conf.use_trash || !ops_trash_available();
}

static void open_props(bool folder) {
  FmPanel *p = P();
  dlg_open(DLG_PROPS, g_active);
  D.in_arc = p->list.loc.in_arc;
  loc_display(&p->list.loc, D.where, sizeof D.where);
  int *items;
  int n = folder ? 0 : panel_selected(p, &items);
  if (folder || n == 0) {
    if (!folder) fm_free(items);
    /* the folder itself */
    loc_title(&p->list.loc, D.name, sizeof D.name);
    memset(&D.entry, 0, sizeof D.entry);
    D.entry.flags = FM_ST_DIR;
    D.entry.type = FT_DIR;
    D.single = true;
    FmStat st;
    if (!D.in_arc && plat_stat(p->list.loc.path, &st)) {
      D.entry.mtime = st.mtime;
      D.entry.flags = st.flags;
    }
    char parent[FM_PATH_MAX];
    fm_strlcpy(parent, D.where, sizeof parent);
    if (fm_path_parent(parent)) fm_strlcpy(D.where, parent, sizeof D.where);
    if (!D.in_arc) {
      D.paths = (char **)fm_alloc(sizeof(char *));
      D.paths[0] = fm_strdup(p->list.loc.path);
      D.npaths = 1;
    }
  } else {
    D.single = n == 1;
    if (D.single) {
      D.entry = p->list.items[items[0]];
      fm_strlcpy(D.name, D.entry.name, sizeof D.name);
    } else {
      fm_snprintf(D.name, sizeof D.name, "%d items", n);
    }
    if (D.in_arc) {
      u64 bytes = 0;
      int files = 0, dirs = 0;
      for (int i = 0; i < n; i++) {
        const FmEntry *e = &p->list.items[items[i]];
        if (e->flags & FM_ST_DIR) dirs++;
        else { files++; bytes += e->size; }
      }
      D.info.bytes_done = bytes;
      D.info.files_done = (u64)files;
      D.info.dirs = (u64)dirs;
      D.have_info = true;
    } else {
      D.paths = panel_selected_paths(p, &D.npaths);
    }
    fm_free(items);
  }
  D.entry.name = D.name;
  if (D.npaths > 0) {
    FmJobSpec s;
    memset(&s, 0, sizeof s);
    s.kind = JOB_SIZE;
    s.srcs = (const char *const *)D.paths;
    s.nsrc = D.npaths;
    s.quiet = true;
    D.job = ops_start(&s);
  }
}

static int creatable_fmt(int from) {
  for (int i = 0; i < FM_COUNT(kFmts); i++) {
    int k = (from + i) % FM_COUNT(kFmts);
    if (arc_fmt_can_create(kFmts[k].fmt)) return k;
  }
  return -1;
}

static void open_compress(void) {
  FmPanel *p = P(), *o = O();
  if (p->list.loc.in_arc) { ui_toast("Extract the files first to compress them"); return; }
  if (!need_selection(p)) return;
  if (!panel_is_local(o)) { ui_toast("The other panel can't receive files"); return; }
  dlg_open(DLG_COMPRESS, g_active);
  D.paths = panel_selected_paths(p, &D.npaths);
  if (D.npaths == 1) {
    fm_strlcpy(D.name, fm_path_base(D.paths[0]), sizeof D.name);
    if (!plat_is_dir(D.paths[0])) {
      char *dot = strrchr(D.name, '.');
      if (dot && dot != D.name) *dot = 0;
    }
  } else {
    loc_title(&p->list.loc, D.name, sizeof D.name);
    if (fm_path_is_root(p->list.loc.path) || !D.name[0]) fm_strlcpy(D.name, "Archive", sizeof D.name);
  }
  D.fmt = creatable_fmt(0);
  D.level = 6;
}

static bool is_archive_entry(const FmEntry *e) {
  return !(e->flags & FM_ST_DIR) && (e->type == FT_ARCHIVE || e->type == FT_APK);
}

static void open_extract(void) {
  FmPanel *p = P(), *o = O();
  if (!panel_is_local(o)) { ui_toast("The other panel can't receive files"); return; }
  dlg_open(DLG_EXTRACT, g_active);
  if (p->list.loc.in_arc) {
    D.from_arc = true;
    D.paths = panel_selected_paths(p, &D.npaths);
    D.subfolder = false;
    return;
  }
  int *items;
  int n = panel_selected(p, &items);
  int ok = 0;
  for (int i = 0; i < n; i++) if (is_archive_entry(&p->list.items[items[i]])) ok++;
  if (ok == 0) {
    fm_free(items);
    dlg_close();
    ui_toast("Select an archive to extract");
    return;
  }
  D.paths = (char **)fm_alloc((size_t)ok * sizeof(char *));
  for (int i = 0; i < n; i++) {
    const FmEntry *e = &p->list.items[items[i]];
    char path[FM_PATH_MAX];
    if (is_archive_entry(e) && vfs_entry_path(&p->list, e, path, sizeof path))
      D.paths[D.npaths++] = fm_strdup(path);
  }
  fm_free(items);
  D.subfolder = true;
}

static void open_settings(void) {
  dlg_open(DLG_SETTINGS, g_active);
  D.zoom = conf.zoom;
}

static void open_exit(void) {
  dlg_open(DLG_EXIT, g_active);
  D.jobs_warn = ops_running(false) > 0 || online_downloads_active() > 0 || photo_downloads_active() > 0 ||
                aonline_downloads_active() > 0;   /* photos, audio */
}

/* ---- operations --------------------------------------------------------- */

static void do_transfer(bool move, FmPanel *src, FmPanel *dst) {
  if (dst->list.loc.in_arc) { ui_toast("Can't write into an archive; extract it first"); return; }
  if (dst->list.err != FM_OK) { ui_toast("The target folder is not available"); return; }
  if (!need_selection(src)) return;
  if (!src->list.loc.in_arc && same_path(src->list.loc.path, dst->list.loc.path) && move) {
    ui_toast("Both panels show the same folder");
    return;
  }
  int n;
  char **paths = panel_selected_paths(src, &n);
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = move ? JOB_MOVE : JOB_COPY;
  s.srcs = (const char *const *)paths;
  s.nsrc = n;
  s.dst = dst->list.loc.path;
  s.conflict = CONFLICT_ASK;
  if (src->list.loc.in_arc) {
    if (move) ui_toast("Archives are read-only: copying instead of moving");
    s.kind = JOB_COPY;
    s.arc = src->list.loc.path;
    s.arc_inner = src->list.loc.inner;
  }
  if (!ops_start(&s)) ui_toast("Too many jobs are running");
  else panel_select_all(src, false);
  panel_free_paths(paths, n);
}

static void clip_free(void) {
  if (g_clip) panel_free_paths(g_clip, g_nclip);
  g_clip = NULL;
  g_nclip = 0;
}

static void clip_set(bool cut) {
  FmPanel *p = P();
  if (!need_selection(p)) return;
  if (cut && p->list.loc.in_arc) { ui_toast("Archives are read-only"); return; }
  clip_free();
  g_clip = panel_selected_paths(p, &g_nclip);
  g_clip_cut = cut;
  g_clip_loc = p->list.loc;
  if (!p->list.loc.in_arc) {
    /* the system clipboard gets the paths, one per line */
    size_t len = 1;
    for (int i = 0; i < g_nclip; i++) len += strlen(g_clip[i]) + 1;
    char *t = (char *)fm_alloc(len);
    t[0] = 0;
    for (int i = 0; i < g_nclip; i++) {
      if (i) fm_strlcat(t, "\n", len);
      fm_strlcat(t, g_clip[i], len);
    }
    ui_clipboard_set(t);
    fm_free(t);
  }
  ui_toast("%d %s %s", g_nclip, plural(g_nclip, "item", "items"), cut ? "cut" : "copied");
}

static void clip_paste(void) {
  FmPanel *p = P();
  if (g_nclip == 0) { ui_toast("Nothing to paste"); return; }
  if (!panel_is_local(p)) { ui_toast("Can't paste here"); return; }
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = g_clip_cut ? JOB_MOVE : JOB_COPY;
  s.srcs = (const char *const *)g_clip;
  s.nsrc = g_nclip;
  s.dst = p->list.loc.path;
  s.conflict = CONFLICT_ASK;
  if (g_clip_loc.in_arc) {
    s.kind = JOB_COPY;
    s.arc = g_clip_loc.path;
    s.arc_inner = g_clip_loc.inner;
  }
  if (!ops_start(&s)) { ui_toast("Too many jobs are running"); return; }
  if (g_clip_cut) clip_free();
}

static void do_select_all(void) {
  FmPanel *p = P();
  bool all = p->nsel == p->nview && p->nview > 0;
  panel_select_all(p, !all);
  if (!all && ui.touch_mode) p->select_mode = true;
}

void app_open_job(FmPanel *p, int item) {
  const FmEntry *e = &p->list.items[item];
  const FmArcEntry *ae = p->list.arc && e->arc_index >= 0 ? arc_entry(p->list.arc, e->arc_index) : NULL;
  char out[FM_PATH_MAX];
  if (!ae || !vfs_cache_path(p->list.loc.path, ae, out, sizeof out)) {
    ui_toast("Can't open %s", e->name);
    return;
  }
  FmStat st;
  if (plat_stat(out, &st) && st.size == ae->size) {
    app_open(out, NULL, 0, 0);
    return;
  }
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = JOB_OPEN;
  s.arc = p->list.loc.path;
  s.arc_index = e->arc_index;
  s.dst = out;
  if (!ops_start(&s)) ui_toast("Too many jobs are running");
}

/* ---- job results -------------------------------------------------------- */

static void job_finished(FmJob *j) {
  FmJobInfo in;
  ops_info(j, &in);
  if (j == D.job) {
    D.info = in;
    D.have_info = true;
    D.job = NULL;
  } else if (in.kind == JOB_OPEN) {
    if (in.err == FM_OK && in.result[0]) app_open(in.result, NULL, 0, 0);
    else if (in.err != FM_ERR_CANCEL) ui_toast("Can't open: %s", in.errmsg[0] ? in.errmsg : fm_err_str(in.err));
  } else if (!in.quiet) {
    char what[64];
    int n = (int)in.files_done;
    if (in.err == FM_ERR_CANCEL) {
      ui_toast("Cancelled");
    } else if (in.nerrors > 0) {
      if (in.nerrors == 1) ui_toast("%s", in.errmsg);
      else ui_toast("%d problems. First: %s", in.nerrors, in.errmsg);
    } else {
      switch (in.kind) {
        case JOB_COPY: fm_strlcpy(what, "Copied", sizeof what); break;
        case JOB_MOVE: fm_strlcpy(what, "Moved", sizeof what); break;
        case JOB_DELETE: fm_strlcpy(what, "Deleted", sizeof what); break;
        case JOB_TRASH: fm_strlcpy(what, "Moved to the trash:", sizeof what); break;
        case JOB_EXTRACT: fm_strlcpy(what, "Extracted", sizeof what); break;
        default: what[0] = 0; break;
      }
      if (in.kind == JOB_COMPRESS) {
        if (in.result[0]) ui_toast("Created %s", fm_path_base(in.result));
        else if (in.nskipped) ui_toast("Skipped: the archive already exists");
      } else if (in.kind == JOB_EXTRACT || (in.kind == JOB_COPY && in.bytes_total && !in.files_total)) {
        ui_toast("Extracted%s", in.nskipped ? " (some files skipped)" : "");
      } else if (in.nskipped) {
        ui_toast("%s %d %s, %d skipped", what, n, plural(n, "item", "items"), in.nskipped);
      } else {
        ui_toast("%s %d %s", what, n, plural(n, "item", "items"));
      }
    }
  }
  refresh_folder(in.touched[0]);
  refresh_folder(in.touched[1]);
  if (in.kind == JOB_COMPRESS && in.result[0]) {
    for (int i = 0; i < 2; i++) {
      FmPanel *p = &g_p[i];
      char d[FM_PATH_MAX];
      fm_strlcpy(d, in.result, sizeof d);
      fm_path_parent(d);
      if (!p->list.loc.in_arc && same_path(p->list.loc.path, d))
        panel_select_name(p, fm_path_base(in.result));
    }
  }
  if (j == g_job_menu) g_job_menu = NULL;
  ops_free(j);
  g_vols_dirty = true;
  g_places_dirty = true;
}

/* ---- panel callbacks ---------------------------------------------------- */

void app_panel_activate(int idx) {
  if (g_active == idx) return;
  g_active = idx;
  conf.active = idx;
  if (g_L.mode == LAYOUT_SINGLE) g_single_tab = idx;
  g_space_dirty = true;
  update_title();
  conf_dirty();
  ui_redraw();
}

void app_panel_navigated(FmPanel *p) {
  if (!p->list.loc.in_arc && p->list.err == FM_OK) {
    fm_strlcpy(conf.path[p->idx], p->list.loc.path, FM_PATH_MAX);
    conf_add_history(p->list.loc.path);
  } else if (p->list.loc.in_arc) {
    panel_disk_dir(p, conf.path[p->idx], FM_PATH_MAX);
  }
  g_places_dirty = true;
  if (p->idx == g_active) {
    g_space_dirty = true;
    if (app.win) update_title();
  }
  conf_dirty();
}

void app_panel_drag_start(FmPanel *p) {
  if (ui.touch_mode) return;
  g_drag = true;
  g_drag_from = p->idx;
  g_drag_n = p->nsel;
}

void app_panel_password(FmPanel *p, const FmLoc *loc, bool retry) {
  dlg_open(DLG_PASSWORD, p->idx);
  D.pw_panel = p->idx;
  D.pw_loc = *loc;
  D.ask.kind = ASK_PASSWORD;
  fm_strlcpy(D.ask.src, loc->path, sizeof D.ask.src);
  D.ask.retry = retry;
}

/* ---- menus -------------------------------------------------------------- */

enum {
  CM_OPEN = 1, CM_OPEN_SYSTEM, CM_COPY_TO, CM_MOVE_TO, CM_COPY, CM_CUT, CM_PASTE, CM_RENAME,
  CM_DELETE, CM_COMPRESS, CM_EXTRACT, CM_PROPS, CM_COPY_PATH, CM_BOOKMARK, CM_SHOW,
  CM_SELECT_ALL, CM_LIB_FOLDER, CM_LIB_FAV,
  PM_SORT_NAME = 40, PM_SORT_SIZE, PM_SORT_DATE, PM_SORT_TYPE, PM_DESC, PM_DIRS_FIRST, PM_LIST,
  PM_GRID, PM_HIDDEN, PM_NEWDIR, PM_NEWFILE, PM_PASTE, PM_SELECT_ALL, PM_REFRESH, PM_PROPS,
  PM_SEARCH, PM_MIRROR, PM_PATH, PM_SELECT_MODE, PM_LIBRARY, PM_ONLINE, PM_PHOTOS /* photos */, PM_AUDIO /* audio */,
  JM_PAUSE = 80, JM_CANCEL
};

static void sep(FmMenuItem *m, int *n) {
  if (*n > 0 && !(m[*n - 1].flags & UI_MI_SEP)) {
    memset(&m[*n], 0, sizeof m[*n]);
    m[(*n)++].flags = UI_MI_SEP;
  }
}

static void item(FmMenuItem *m, int *n, int id, FmIcon ic, const char *label, const char *key,
                 int flags) {
  FmMenuItem *it = &m[(*n)++];
  it->id = id;
  it->icon = ic;
  it->label = label;
  it->shortcut = ui.touch_mode ? NULL : key;
  it->flags = flags;
}

void app_panel_context(FmPanel *p, float x, float y) {
  FmMenuItem m[24];
  int n = 0;
  bool arc = p->list.loc.in_arc;
  FmPanel *o = &g_p[1 - p->idx];
  bool o_ok = panel_is_local(o);
  int dis_o = o_ok ? 0 : UI_MI_DISABLED;
  int dis_arc = arc ? UI_MI_DISABLED : 0;
  bool one = p->nsel == 1;
  FmEntry *ce = panel_cursor_entry(p);
  bool is_arc = one && ce && ce->selected && is_archive_entry(ce);
  g_menu_panel = p->idx;
  item(m, &n, CM_OPEN, IC_OPEN_WITH, "Open", "Enter", one ? 0 : UI_MI_DISABLED);
  if (!arc && one) item(m, &n, CM_OPEN_SYSTEM, IC_SHARE, "Open with system app", NULL, 0);
  sep(m, &n);
  item(m, &n, CM_COPY_TO, IC_COPY, "Copy to other panel", "F5", dis_o);
  item(m, &n, CM_MOVE_TO, IC_MOVE, "Move to other panel", "F6", dis_o | dis_arc);
  item(m, &n, CM_COPY, IC_COPY, "Copy", "Ctrl+C", 0);
  item(m, &n, CM_CUT, IC_CUT, "Cut", "Ctrl+X", dis_arc);
  if (g_nclip > 0) item(m, &n, CM_PASTE, IC_PASTE, "Paste here", "Ctrl+V", dis_arc);
  sep(m, &n);
  item(m, &n, CM_RENAME, IC_RENAME, "Rename", "F2", (one && !arc) ? 0 : UI_MI_DISABLED);
  item(m, &n, CM_DELETE, IC_DELETE, "Delete", "Del", (arc ? UI_MI_DISABLED : 0) | UI_MI_DANGER);
  sep(m, &n);
  item(m, &n, CM_COMPRESS, IC_COMPRESS, "Compress to other panel", NULL, dis_o | dis_arc);
  if (is_arc || arc) item(m, &n, CM_EXTRACT, IC_EXTRACT, "Extract to other panel", NULL, dis_o);
  sep(m, &n);
  if (one && ce && (ce->flags & FM_ST_DIR) && !arc) {
    char path[FM_PATH_MAX];
    vfs_entry_path(&p->list, ce, path, sizeof path);
    item(m, &n, CM_BOOKMARK, conf_is_bookmark(path) ? IC_STAR_FILL : IC_STAR,
         conf_is_bookmark(path) ? "Remove bookmark" : "Bookmark", NULL, 0);
  }
  /* flib: folders become library sources, media files favorites */
  if (one && ce && !arc) {
    char lp[FM_PATH_MAX];
    vfs_entry_path(&p->list, ce, lp, sizeof lp);
    if (ce->flags & FM_ST_DIR)
      item(m, &n, CM_LIB_FOLDER, IC_LIBRARY, lib_has_folder(lp) ? "Remove from media library" :
           "Add to media library", NULL, 0);
    else if (lib_is_media(lp))
      item(m, &n, CM_LIB_FAV, lib_is_fav(lp) ? IC_HEART_FILL : IC_HEART,
           lib_is_fav(lp) ? "Remove from favorites" : "Add to favorites", NULL, 0);
  } else if (!arc && ce && !(ce->flags & FM_ST_DIR) && lib_is_media(ce->name)) {
    item(m, &n, CM_LIB_FAV, IC_HEART, "Add to favorites", NULL, 0);
  }
  if (!arc) item(m, &n, CM_COPY_PATH, IC_COPY, "Copy path", NULL, 0);
  if (!arc && one) item(m, &n, CM_SHOW, IC_FOLDER_OPEN, "Show in system file manager", NULL, 0);
  item(m, &n, CM_PROPS, IC_INFO, "Properties", "Alt+Enter", 0);
  ui_menu_open(ID_CTX, x, y, m, n);
}

void app_panel_menu(FmPanel *p, float x, float y) {
  FmMenuItem m[28];
  int n = 0;
  int s = conf.sort[p->idx];
  bool arc = p->list.loc.in_arc;
  g_menu_panel = p->idx;
  item(m, &n, PM_SORT_NAME, IC_SORT, "Sort by name", NULL, s == SORT_NAME ? UI_MI_CHECKED : 0);
  item(m, &n, PM_SORT_SIZE, IC_SORT, "Sort by size", NULL, s == SORT_SIZE ? UI_MI_CHECKED : 0);
  item(m, &n, PM_SORT_DATE, IC_SORT, "Sort by date", NULL, s == SORT_DATE ? UI_MI_CHECKED : 0);
  item(m, &n, PM_SORT_TYPE, IC_SORT, "Sort by type", NULL, s == SORT_TYPE ? UI_MI_CHECKED : 0);
  item(m, &n, PM_DESC, IC_ARROW_DOWN, "Descending", NULL, conf.sort_desc[p->idx] ? UI_MI_CHECKED : 0);
  item(m, &n, PM_DIRS_FIRST, IC_FOLDER, "Folders first", NULL, conf.folders_first ? UI_MI_CHECKED : 0);
  sep(m, &n);
  item(m, &n, PM_LIST, IC_LIST, "List view", NULL, conf.view[p->idx] == VIEW_LIST ? UI_MI_CHECKED : 0);
  item(m, &n, PM_GRID, IC_GRID, "Grid view", NULL, conf.view[p->idx] == VIEW_GRID ? UI_MI_CHECKED : 0);
  item(m, &n, PM_HIDDEN, IC_EYE, "Show hidden files", "Ctrl+H", conf.show_hidden ? UI_MI_CHECKED : 0);
  sep(m, &n);
  item(m, &n, PM_NEWDIR, IC_NEW_FOLDER, "New folder", "F7", arc ? UI_MI_DISABLED : 0);
  item(m, &n, PM_NEWFILE, IC_NEW_FILE, "New file", "Shift+F4", arc ? UI_MI_DISABLED : 0);
  if (g_nclip > 0) item(m, &n, PM_PASTE, IC_PASTE, "Paste", "Ctrl+V", arc ? UI_MI_DISABLED : 0);
  item(m, &n, PM_SELECT_ALL, IC_SELECT_ALL, "Select all", "Ctrl+A", p->nview ? 0 : UI_MI_DISABLED);
  if (ui.touch_mode)
    item(m, &n, PM_SELECT_MODE, IC_CHECK, "Select items", NULL, p->select_mode ? UI_MI_CHECKED : 0);
  item(m, &n, PM_SEARCH, IC_SEARCH, "Filter", "Ctrl+F", 0);
  item(m, &n, PM_LIBRARY, IC_LIBRARY, "Media library", NULL, 0);   /* flib */
  item(m, &n, PM_ONLINE, IC_PLAY_BADGE, "Online videos", NULL, 0);   /* online */
  item(m, &n, PM_PHOTOS, IC_ALBUM, "Online photos", NULL, 0);        /* photos */
  item(m, &n, PM_AUDIO, IC_RADIO, "Online audio", NULL, 0);          /* audio */
  sep(m, &n);
  item(m, &n, PM_MIRROR, IC_SWAP, "Same folder in other panel", NULL, 0);
  item(m, &n, PM_PATH, IC_RENAME, "Go to path", "Ctrl+L", 0);
  item(m, &n, PM_REFRESH, IC_REFRESH, "Refresh", "Ctrl+R", 0);
  item(m, &n, PM_PROPS, IC_INFO, "Folder properties", NULL, 0);
  ui_menu_open(ID_PMENU, x, y, m, n);
}

void app_panel_places(FmPanel *p, float x, float y) {
  build_places();
  FmMenuItem m[PLACES_MAX + 4];
  int n = 0, last = -1;
  g_menu_panel = p->idx;
  for (int i = 0; i < g_nplaces && n < FM_COUNT(m) - 1; i++) {
    if (last >= 0 && g_places[i].section != last) sep(m, &n);
    last = g_places[i].section;
    item(m, &n, 100 + i, g_places[i].icon, g_places[i].label, NULL, 0);
  }
  ui_menu_open(ID_PLACES, x, y, m, n);
}

static void entry_path(FmPanel *p, const FmEntry *e, char *out, size_t cap) {
  if (!vfs_entry_path(&p->list, e, out, cap)) fm_strlcpy(out, e->name, cap);
}

static void ctx_action(int id) {
  FmPanel *p = &g_p[g_menu_panel];
  app_panel_activate(g_menu_panel);
  FmEntry *ce = panel_cursor_entry(p);
  char path[FM_PATH_MAX];
  switch (id) {
    case CM_OPEN:
      if (ce) panel_open_item(p, p->view[p->cursor]);
      break;
    case CM_OPEN_SYSTEM:
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        if (!plat_open_external(path)) ui_toast("No app found to open %s", ce->name);
      }
      break;
    case CM_COPY_TO: do_transfer(false, p, &g_p[1 - p->idx]); break;
    case CM_MOVE_TO: do_transfer(true, p, &g_p[1 - p->idx]); break;
    case CM_COPY: clip_set(false); break;
    case CM_CUT: clip_set(true); break;
    case CM_PASTE: clip_paste(); break;
    case CM_RENAME: open_rename(); break;
    case CM_DELETE: open_delete(false); break;
    case CM_COMPRESS: open_compress(); break;
    case CM_EXTRACT: open_extract(); break;
    case CM_PROPS: open_props(false); break;
    case CM_COPY_PATH:
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        ui_clipboard_set(path);
        ui_toast("Path copied");
      }
      break;
    case CM_BOOKMARK:
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        conf_toggle_bookmark(path);
        g_places_dirty = true;
        conf_dirty();
      }
      break;
    case CM_SHOW:
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        if (!plat_share(path)) ui_toast("Not available on this system");
      }
      break;
    case CM_LIB_FOLDER:   /* flib */
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        bool on = !lib_has_folder(path);
        lib_folder_set(path, on);
        ui_toast(on ? "Added to the media library" : "Removed from the media library");
      }
      break;
    case CM_LIB_FAV:      /* flib: every selected media file follows the one under the cursor */
      if (ce) {
        entry_path(p, ce, path, sizeof path);
        bool on = p->nsel > 1 || !lib_is_fav(path);
        int ns = 0;
        char **sel = panel_selected_paths(p, &ns);
        for (int i = 0; i < ns; i++)
          if (lib_is_media(sel[i])) lib_fav_set(sel[i], on);
        if (ns == 0) lib_fav_set(path, on);
        panel_free_paths(sel, ns);
        ui_toast(on ? "Added to favorites" : "Removed from favorites");
      }
      break;
    default: break;
  }
}

static void set_sort(FmPanel *p, int s) {
  if (conf.sort[p->idx] == s) conf.sort_desc[p->idx] = !conf.sort_desc[p->idx];
  else { conf.sort[p->idx] = s; conf.sort_desc[p->idx] = s == SORT_DATE || s == SORT_SIZE; }
  panel_resort(p);
  conf_dirty();
}

static void toggle_hidden(void) {
  conf.show_hidden = !conf.show_hidden;
  panel_refresh(&g_p[0]);
  panel_refresh(&g_p[1]);
  ui_toast(conf.show_hidden ? "Showing hidden files" : "Hiding hidden files");
  conf_dirty();
}

static void pmenu_action(int id) {
  FmPanel *p = &g_p[g_menu_panel];
  app_panel_activate(g_menu_panel);
  switch (id) {
    case PM_SORT_NAME: set_sort(p, SORT_NAME); break;
    case PM_SORT_SIZE: set_sort(p, SORT_SIZE); break;
    case PM_SORT_DATE: set_sort(p, SORT_DATE); break;
    case PM_SORT_TYPE: set_sort(p, SORT_TYPE); break;
    case PM_DESC:
      conf.sort_desc[p->idx] = !conf.sort_desc[p->idx];
      panel_resort(p);
      conf_dirty();
      break;
    case PM_DIRS_FIRST:
      conf.folders_first = !conf.folders_first;
      panel_resort(&g_p[0]);
      panel_resort(&g_p[1]);
      conf_dirty();
      break;
    case PM_LIST: conf.view[p->idx] = VIEW_LIST; conf_dirty(); break;
    case PM_GRID: conf.view[p->idx] = VIEW_GRID; conf_dirty(); break;
    case PM_HIDDEN: toggle_hidden(); break;
    case PM_NEWDIR: open_new(false); break;
    case PM_NEWFILE: open_new(true); break;
    case PM_PASTE: clip_paste(); break;
    case PM_SELECT_ALL: do_select_all(); break;
    case PM_SELECT_MODE:
      p->select_mode = !p->select_mode;
      if (!p->select_mode) panel_select_all(p, false);
      break;
    case PM_SEARCH: panel_open_search(p); break;
    case PM_LIBRARY: open_library(-1); break;   /* flib */
    case PM_ONLINE: open_online(); break;       /* online */
    case PM_PHOTOS: open_photos(); break;       /* photos */
    case PM_AUDIO: open_aonline(); break;       /* audio */
    case PM_MIRROR: {
      FmLoc l = p->list.loc;
      panel_go(&g_p[1 - p->idx], &l, true);
      break;
    }
    case PM_PATH: panel_edit_path(p); break;
    case PM_REFRESH: panel_refresh(p); break;
    case PM_PROPS: open_props(true); break;
    default: break;
  }
  ui_redraw();
}

static void job_menu_action(int id) {
  if (!g_job_menu) return;
  for (int i = 0; i < ops_count(); i++) {
    if (ops_at(i) != g_job_menu) continue;
    FmJobInfo in;
    ops_info(g_job_menu, &in);
    if (id == JM_PAUSE) ops_pause(g_job_menu, !in.paused);
    if (id == JM_CANCEL) ops_cancel(g_job_menu);
  }
  g_job_menu = NULL;
}

static void menu_results(void) {
  int r;
  if ((r = ui_menu_result(ID_CTX)) >= 0) ctx_action(r);
  if ((r = ui_menu_result(ID_PMENU)) >= 0) pmenu_action(r);
  if ((r = ui_menu_result(ID_PLACES)) >= 0) go_place(r - 100, g_menu_panel);
  if ((r = ui_menu_result(ID_JOBMENU)) >= 0) job_menu_action(r);
}

/* ---- dialog helpers ----------------------------------------------------- */

static float btn_h(void) { return DP(ui.touch_mode ? 46 : 38); }

/* Width of the dialog content for a w_dp dialog (matches ui_dialog_begin). */
static float dlg_cw(float w_dp) {
  float w = (ui.portrait && ui.w < DP(560)) ? ui.w : FM_MIN(DP(w_dp), ui.w - DP(24));
  return w - DP(40);
}

/* Dialog height in dp for `content_px` of content below the title. */
static float dlg_h(float content_px) {
  return (content_px + DP(40) + font_line_h(ui.m.font_title) + DP(12)) / ui.scale + 2;
}

static float btn_w(const char *l) { return font_width(FONT_BOLD, ui.m.font, l, -1) + DP(36); }

static bool btns_fit(float w, const char *const *labels, int n) {
  float t = 0;
  for (int i = 0; i < n; i++) t += btn_w(labels[i]) + (i ? DP(8) : 0);
  return t <= w;
}

static float btns_h(float w, const char *const *labels, int n) {
  return btns_fit(w, labels, n) ? btn_h() : (float)n * btn_h() + (float)(n - 1) * DP(8);
}

/* Buttons at the bottom of c; stacked full width when they do not fit in a
** row (the primary, last label, then goes on top). */
static int dlg_buttons(FmRect c, const char *const *labels, int n, bool danger) {
  u32 base = ui_id("dlg.buttons");
  int res = -1;
  float bh = btn_h();
  if (btns_fit(c.w, labels, n)) {
    float x = c.x + c.w, y = c.y + c.h - bh;
    for (int i = n - 1; i >= 0; i--) {
      float bw = btn_w(labels[i]);
      x -= bw;
      int st = i == n - 1 ? (danger ? UI_BTN_DANGER : UI_BTN_FILLED) : UI_BTN_TEXT;
      if (ui_button(ui_idn(base, (u32)i), FM_RECT(x, y, bw, bh), IC_NONE, labels[i], st)) res = i;
      x -= DP(8);
    }
  } else {
    float y = c.y + c.h - btns_h(c.w, labels, n);
    for (int i = n - 1; i >= 0; i--) {
      int st = i == n - 1 ? (danger ? UI_BTN_DANGER : UI_BTN_FILLED) : UI_BTN_TONAL;
      if (ui_button(ui_idn(base, (u32)i), FM_RECT(c.x, y, c.w, bh), IC_NONE, labels[i], st)) res = i;
      y += bh + DP(8);
    }
  }
  if (res < 0 && n > 0 && !ui.focus && (ui_key(SDLK_RETURN, 0) || ui_key(SDLK_KP_ENTER, 0)))
    res = n - 1;
  return res;
}

static float field_h(void) { return DP(ui.touch_mode ? 48 : 40); }

/* ---- theme picker ------------------------------------------------------- */

static FmColor argb_col(u32 c) {
  return FM_RGBA((c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) & 255);
}

static FmColor theme_accent_of(int theme, bool dark) {
  return argb_col(theme_variant(theme, dark)->accent);
}

#define THEME_CARD_H 64

static int theme_grid_cols(float w) { return FM_MAX(2, (int)(w / DP(150))); }

static float theme_grid_h(float w) {
  int cols = theme_grid_cols(w);
  int rows = (THEME_COUNT + cols - 1) / cols;
  return (float)rows * DP(THEME_CARD_H + 8) - DP(8);
}

/* A grid of small previews, each painted in its theme's own colours. */
static void theme_grid(FmRect *r, u32 base, float h) {
  FmRect g = rect_cut_top(r, h);
  int cols = theme_grid_cols(g.w);
  float gap = DP(8), cw = (g.w - gap * (float)(cols - 1)) / (float)cols, ch = DP(THEME_CARD_H);
  for (int t = 0; t < THEME_COUNT; t++) {
    FmRect c = { g.x + (float)(t % cols) * (cw + gap), g.y + (float)(t / cols) * (ch + gap), cw, ch };
    if (!gfx_visible(c)) continue;
    bool cur = t == conf.theme;
    const FmThemeVariant *v = theme_variant(t, cur ? T.dark : conf.dark);
    int f = ui_hit(ui_idn(base, (u32)t), c);
    FmColor panel = argb_col(v->panel), tx = argb_col(v->text | 0xFF000000u);
    float rad = DP(10);
    if (v->grad.top) gfx_rrect_vgrad(c, rad, argb_col(v->grad.top), argb_col(v->grad.bottom));
    else gfx_rrect(c, rad, argb_col(v->bg));
    /* two mini panels with a selected row, like the real screen */
    FmRect in = rect_inset(c, DP(7));
    FmRect lbl = rect_cut_bottom(&in, font_line_h(ui.m.font_small));
    rect_cut_bottom(&in, DP(3));
    float pw = (in.w - DP(4)) * 0.5f;
    for (int i = 0; i < 2; i++) {
      FmRect pr = { in.x + (float)i * (pw + DP(4)), in.y, pw, in.h };
      gfx_rrect(pr, DP(4), panel);
      if (v->border >> 24) gfx_rrect_line(pr, DP(4), DP(1), argb_col(v->border));
      FmRect row = { pr.x + DP(3), pr.y + DP(4), pr.w - DP(6), DP(5) };
      gfx_rrect(row, DP(2), i == 0 ? argb_col(v->sel | 0x60000000u) : col_alpha(tx, 0.18f));
      row.y += DP(8);
      gfx_rrect(row, DP(2), col_alpha(tx, 0.18f));
    }
    gfx_circle(in.x + in.w - DP(6), in.y + in.h - DP(6), DP(4), argb_col(v->accent));
    font_draw_ellipsis(cur ? FONT_BOLD : FONT_REGULAR, ui.m.font_small, lbl.x, lbl.y,
                       theme_info(t)->name, lbl.w, tx);
    if (cur) gfx_rrect_line(rect_inset(c, -DP(3)), rad + DP(3), DP(2), T.accent);
    else if (f & UI_HOVER) gfx_rrect_line(rect_inset(c, -DP(2)), rad + DP(2), DP(1.5f), T.border);
    if ((f & UI_CLICK) && !cur) {
      conf.theme = t;
      if (!theme_has_mode(t, conf.dark)) conf.dark = !conf.dark;
      conf.accent = -1;            /* a new theme brings its own accent */
      theme_apply(conf.theme, conf.dark, conf.accent);
      conf_dirty();
    }
  }
}

static void small_label(FmRect *c, const char *s) {
  FmRect r = rect_cut_top(c, font_line_h(ui.m.font_small) + DP(6));
  font_draw(FONT_BOLD, ui.m.font_small, r.x + DP(2), r.y, s, -1, T.text2);
}

static void text_line(FmRect *c, const char *s, FmColor col) {
  FmRect r = rect_cut_top(c, font_line_h(ui.m.font) + DP(4));
  font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font, r.x, r.y, s, r.w, col);
}

/* ---- dialogs: rename / new ---------------------------------------------- */

static void select_in_panels(const char *dir, const char *name) {
  for (int i = 0; i < 2; i++)
    if (!g_p[i].list.loc.in_arc && same_path(g_p[i].list.loc.path, dir)) {
      panel_refresh(&g_p[i]);
      if (i == D.panel) panel_select_name(&g_p[i], name);
    }
}

static bool do_rename(void) {
  FmPanel *p = &g_p[D.panel];
  const char *dir = p->list.loc.path;
  if (!strcmp(D.name, D.orig)) return true;
  if (!ops_valid_name(D.name)) {
    fm_strlcpy(D.err, "That name is not allowed", sizeof D.err);
    return false;
  }
  char a[FM_PATH_MAX], b[FM_PATH_MAX];
  if (!fm_path_join(a, sizeof a, dir, D.orig) || !fm_path_join(b, sizeof b, dir, D.name)) {
    fm_strlcpy(D.err, "The name is too long", sizeof D.err);
    return false;
  }
  FmErr e;
  if (fm_stricmp(D.name, D.orig) == 0) {
    /* only the case changes: go through a temporary name (case-insensitive file systems) */
    char tmp[FM_PATH_MAX], tn[300];
    fm_snprintf(tn, sizeof tn, ".%s.mmcfm-rename", D.orig);
    fm_path_join(tmp, sizeof tmp, dir, tn);
    e = plat_rename(a, tmp);
    if (e == FM_OK) {
      e = plat_rename(tmp, b);
      if (e != FM_OK) plat_rename(tmp, a);
    }
  } else if (plat_exists(b)) {
    fm_strlcpy(D.err, "An item with this name already exists", sizeof D.err);
    return false;
  } else {
    e = plat_rename(a, b);
  }
  if (e != FM_OK) {
    fm_snprintf(D.err, sizeof D.err, "Can't rename: %s", fm_err_str(e));
    return false;
  }
  char d[FM_PATH_MAX], n[256];
  fm_strlcpy(d, dir, sizeof d);
  fm_strlcpy(n, D.name, sizeof n);
  select_in_panels(d, n);
  return true;
}

static bool do_create(bool file) {
  FmPanel *p = &g_p[D.panel];
  if (!ops_valid_name(D.name)) {
    fm_strlcpy(D.err, "That name is not allowed", sizeof D.err);
    return false;
  }
  char path[FM_PATH_MAX];
  if (!fm_path_join(path, sizeof path, p->list.loc.path, D.name)) {
    fm_strlcpy(D.err, "The name is too long", sizeof D.err);
    return false;
  }
  if (plat_exists(path)) {
    fm_strlcpy(D.err, "An item with this name already exists", sizeof D.err);
    return false;
  }
  FmErr e = FM_OK;
  if (file) {
    FILE *f = fm_fopen(path, "wb");
    if (!f) e = FM_ERR_ACCESS;
    else fclose(f);
  } else {
    e = plat_mkdir(path);
  }
  if (e != FM_OK) {
    fm_snprintf(D.err, sizeof D.err, "Can't create: %s", fm_err_str(e));
    return false;
  }
  char d[FM_PATH_MAX], n[256];
  fm_strlcpy(d, p->list.loc.path, sizeof d);
  fm_strlcpy(n, D.name, sizeof n);
  select_in_panels(d, n);
  return true;
}

static void dlg_name(void) {
  const char *title = D.kind == DLG_RENAME ? "Rename" : D.kind == DLG_NEWDIR ? "New folder" : "New file";
  const char *ok = D.kind == DLG_RENAME ? "Rename" : "Create";
  const char *labels[] = { "Cancel", ok };
  float cw = dlg_cw(440);
  float ch = font_line_h(ui.m.font_small) + DP(6) + field_h() + DP(8) + font_line_h(ui.m.font_small) +
             DP(16) + btns_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id(title), title, 440, dlg_h(ch), &cancel);
  small_label(&c, D.kind == DLG_NEWDIR ? "Folder name" : "Name");
  FmRect f = rect_cut_top(&c, field_h());
  int flags = UI_TF_FOCUS | (D.kind != DLG_NEWDIR ? UI_TF_SELECT_STEM : 0);
  int r = ui_textfield(ui_id("dlg.name.tf"), f, D.name, sizeof D.name, "Name", flags);
  if (r & UI_TF_CHANGED) D.err[0] = 0;
  rect_cut_top(&c, DP(8));
  if (D.err[0]) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x + DP(2), c.y, D.err, c.w, T.danger);
  int b = dlg_buttons(c, labels, 2, false);
  ui_dialog_end();
  if (cancel || b == 0 || (r & UI_TF_CANCEL)) { dlg_close(); return; }
  if (b == 1 || (r & UI_TF_SUBMIT)) {
    bool ok2 = D.kind == DLG_RENAME ? do_rename() : do_create(D.kind == DLG_NEWFILE);
    if (ok2) dlg_close();
  }
}

/* ---- dialogs: delete ---------------------------------------------------- */

static void dlg_delete(void) {
  char title[300];
  if (D.npaths == 1) fm_snprintf(title, sizeof title, "Delete \"%s\"?", fm_path_base(D.paths[0]));
  else fm_snprintf(title, sizeof title, "Delete %d items?", D.npaths);
  const char *labels[] = { "Cancel", D.permanent ? "Delete" : "Move to Trash" };
  float cw = dlg_cw(460);
  int shown = FM_MIN(D.npaths, 4);
  float lh = font_line_h(ui.m.font) + DP(10);
  bool can_trash = conf.use_trash && ops_trash_available();
  float ch = (float)shown * lh + (D.npaths > shown ? lh : 0) + DP(10) + font_line_h(ui.m.font) * 2 +
             DP(8) + (can_trash ? DP(40) : 0) + DP(14) + btns_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.delete"), title, 460, dlg_h(ch), &cancel);
  for (int i = 0; i < shown; i++) {
    FmRect r = rect_cut_top(&c, lh);
    FmType t = plat_is_dir(D.paths[i]) ? FT_DIR : fm_type_from_name(D.paths[i]);
    float is = DP(22);
    icon_file(t, FM_RECT(r.x, r.y + (r.h - is) * 0.5f, is, is), T.dark);
    font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font, r.x + is + DP(10),
                           r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, fm_path_base(D.paths[i]),
                           r.w - is - DP(10), T.text);
  }
  if (D.npaths > shown) {
    char more[64];
    fm_snprintf(more, sizeof more, "and %d more", D.npaths - shown);
    FmRect r = rect_cut_top(&c, lh);
    font_draw(FONT_REGULAR, ui.m.font, r.x + DP(32), r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, more,
              -1, T.text2);
  }
  rect_cut_top(&c, DP(10));
  if (D.permanent) {
    FmRect r = rect_cut_top(&c, font_line_h(ui.m.font) * 2 + DP(8));
    icon_draw(IC_WARN, FM_RECT(r.x, r.y + DP(1), DP(18), DP(18)), T.danger);
    font_draw_wrap(FONT_REGULAR, ui.m.font, r.x + DP(26), r.y, r.w - DP(26),
                   "They will be deleted permanently. This can't be undone.", T.danger, true);
  } else {
    FmRect r = rect_cut_top(&c, font_line_h(ui.m.font) * 2 + DP(8));
    icon_draw(IC_DELETE, FM_RECT(r.x, r.y + DP(1), DP(18), DP(18)), T.text2);
    font_draw_wrap(FONT_REGULAR, ui.m.font, r.x + DP(26), r.y, r.w - DP(26),
                   "They will be moved to the Trash, where you can restore them.", T.text2, true);
  }
  if (can_trash) {
    FmRect r = rect_cut_top(&c, DP(40));
    ui_check(ui_id("dlg.delete.perm"), r, "Delete permanently", &D.permanent);
  }
  int b = dlg_buttons(c, labels, 2, true);
  ui_dialog_end();
  if (cancel || b == 0) { dlg_close(); return; }
  if (b == 1) {
    start_delete(D.paths, D.npaths, D.permanent);
    panel_select_all(&g_p[D.panel], false);
    dlg_close();
  }
}

/* ---- dialogs: properties ------------------------------------------------ */

static void prop_row(FmRect *c, const char *k, const char *v, float kw) {
  float lh = font_line_h(ui.m.font) + DP(8);
  FmRect r = rect_cut_top(c, lh);
  font_draw(FONT_REGULAR, ui.m.font_small, r.x, r.y + (lh - font_line_h(ui.m.font_small)) * 0.5f, k, -1,
            T.text2);
  font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font, r.x + kw, r.y + (lh - font_line_h(ui.m.font)) * 0.5f, v,
                     r.w - kw, T.text);
}

static void mode_str(u32 m, char *out) {
  const char *x = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) out[i] = (m & (1u << (8 - i))) ? x[i] : '-';
  out[9] = 0;
}

static void dlg_props(void) {
  if (D.job) {
    ops_info(D.job, &D.info);
    D.have_info = true;
  }
  const FmEntry *e = &D.entry;
  bool dir = D.single && (e->flags & FM_ST_DIR);
  bool arc = D.info.arc_fmt != ARC_NONE;
  int rows = 3 + (dir || !D.single ? 1 : 0) + (D.single && e->mtime ? 1 : 0) +
             (D.single && !D.in_arc && e->flags & (FM_ST_LINK | FM_ST_BROKEN) ? 1 : 0) +
             (arc ? 3 : 0);
  const char *labels[] = { "Close" };
  float cw = dlg_cw(480);
  float lh = font_line_h(ui.m.font) + DP(8);
  float ch = DP(56) + DP(14) + (float)rows * lh + DP(14) + btns_h(cw, labels, 1);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.props"), "Properties", 480, dlg_h(ch), &cancel);
  /* head: big icon, name, type */
  FmRect h = rect_cut_top(&c, DP(56));
  if (D.single) icon_file(e->type, FM_RECT(h.x, h.y, DP(52), DP(52)), T.dark);
  else icon_file(FT_FILE, FM_RECT(h.x, h.y, DP(52), DP(52)), T.dark);
  float tx = h.x + DP(64);
  font_draw_mid_ellipsis(FONT_BOLD, ui.m.font_title, tx, h.y + DP(4), D.name, h.w - DP(64), T.text);
  char type[96];
  if (!D.single) fm_strlcpy(type, "Multiple items", sizeof type);
  else if (dir) fm_strlcpy(type, (e->flags & FM_ST_LINK) ? "Folder (link)" : "Folder", sizeof type);
  else {
    const char *ext = fm_path_ext(D.name);
    if (ext[0]) fm_snprintf(type, sizeof type, "%s (%s)", fm_type_label((FmType)e->type), ext + 1);
    else fm_strlcpy(type, fm_type_label((FmType)e->type), sizeof type);
  }
  font_draw(FONT_REGULAR, ui.m.font_small, tx, h.y + DP(8) + font_line_h(ui.m.font_title), type, -1,
            T.text2);
  rect_cut_top(&c, DP(14));
  float kw = FM_MIN(DP(110), c.w * 0.32f);
  char v[FM_PATH_MAX + 64], a[32], b[32], cnt[32];
  prop_row(&c, "Location", D.where, kw);
  /* size: live while the size job runs */
  bool running = D.job != NULL;
  u64 bytes = D.have_info ? D.info.bytes_done : e->size;
  if (D.single && !dir && !D.have_info) bytes = e->size;
  fmt_count(bytes, cnt, sizeof cnt);
  fm_snprintf(v, sizeof v, "%s (%s bytes)%s", fm_fmt_size(bytes, a, sizeof a), cnt,
              running ? "  ..." : "");
  prop_row(&c, "Size", v, kw);
  if (running) {
    FmRect sp = { c.x + c.w - DP(18), c.y - lh + (lh - DP(16)) * 0.5f, DP(16), DP(16) };
    ui_spinner(sp, T.accent);
  }
  if (dir || !D.single) {
    char f1[32], f2[32];
    fmt_count(D.info.files_done, f1, sizeof f1);
    fmt_count(D.info.dirs, f2, sizeof f2);
    fm_snprintf(v, sizeof v, "%s %s, %s %s", f1, D.info.files_done == 1 ? "file" : "files", f2,
                D.info.dirs == 1 ? "folder" : "folders");
    prop_row(&c, "Contains", v, kw);
  }
  if (D.single && e->mtime) prop_row(&c, "Modified", fm_fmt_time(e->mtime, b, sizeof b), kw);
  if (D.single && !D.in_arc) {
    FmStat st;
    char full[FM_PATH_MAX];
    bool have = D.npaths > 0 && plat_stat(D.paths[0], &st);
    if (!D.npaths) fm_strlcpy(full, D.where, sizeof full);
    if (have && st.mode) {
      char ms[12];
      mode_str(st.mode, ms);
      fm_snprintf(v, sizeof v, "%s (%03o)%s", ms, (unsigned)(st.mode & 0777),
                  (st.flags & FM_ST_READONLY) ? ", read-only" : "");
    } else {
      fm_snprintf(v, sizeof v, "%s%s", have && (st.flags & FM_ST_READONLY) ? "Read-only" : "Read and write",
                  have && (st.flags & FM_ST_HIDDEN) ? ", hidden" : "");
    }
    prop_row(&c, "Access", v, kw);
    if (e->flags & (FM_ST_LINK | FM_ST_BROKEN))
      prop_row(&c, "Link", (e->flags & FM_ST_BROKEN) ? "Broken (the target is missing)" : "Symbolic link", kw);
  } else if (D.in_arc && D.single) {
    prop_row(&c, "Encrypted", e->encrypted ? "Yes" : "No", kw);
  } else {
    prop_row(&c, "Access", "", kw);
  }
  if (arc) {
    prop_row(&c, "Format", arc_fmt_name((FmArcFmt)D.info.arc_fmt), kw);
    if (D.info.arc_entries >= 0) {
      fmt_count((u64)D.info.arc_entries, cnt, sizeof cnt);
      fm_snprintf(v, sizeof v, "%s entries, %s unpacked", cnt, fm_fmt_size(D.info.arc_unpacked, a, sizeof a));
    } else {
      fm_strlcpy(v, D.info.arc_encrypted ? "Hidden (encrypted file list)" : "Unreadable", sizeof v);
    }
    prop_row(&c, "Contents", v, kw);
    prop_row(&c, "Encrypted", D.info.arc_encrypted ? "Yes" : "No", kw);
  }
  int bt = dlg_buttons(c, labels, 1, false);
  ui_dialog_end();
  if (cancel || bt == 0) dlg_close();
}

/* ---- dialogs: compress / extract ---------------------------------------- */

static void start_compress(void) {
  FmPanel *o = &g_p[1 - D.panel], *p = &g_p[D.panel];
  if (D.fmt < 0) return;
  const FmtInfo *fi = &kFmts[D.fmt];
  if (!ops_valid_name(D.name)) {
    fm_strlcpy(D.err, "That name is not allowed", sizeof D.err);
    return;
  }
  char file[300], out[FM_PATH_MAX];
  fm_snprintf(file, sizeof file, "%s%s", D.name, fi->ext);
  if (!fm_path_join(out, sizeof out, o->list.loc.path, file)) {
    fm_strlcpy(D.err, "The name is too long", sizeof D.err);
    return;
  }
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.kind = JOB_COMPRESS;
  s.srcs = (const char *const *)D.paths;
  s.nsrc = D.npaths;
  s.dst = out;
  s.base_dir = p->list.loc.path;
  s.conflict = CONFLICT_ASK;
  s.arc_opts.fmt = fi->fmt;
  s.arc_opts.level = fi->fmt == ARC_TAR ? 0 : (int)(D.level + 0.5f);
  if (arc_fmt_can_encrypt(fi->fmt) && D.pw[0]) {
    s.arc_opts.password = D.pw;
    s.arc_opts.zip_aes = D.zip_mode == 0;
    s.arc_opts.encrypt_names = fi->fmt == ARC_7Z && D.enc_names;
  }
  s.arc_opts.solid = fi->fmt == ARC_7Z;
  if (!ops_start(&s)) { ui_toast("Too many jobs are running"); return; }
  panel_select_all(p, false);
  dlg_close();
}

static const char *level_name(int l) {
  if (l <= 0) return "Store";
  if (l <= 2) return "Fastest";
  if (l <= 4) return "Fast";
  if (l <= 6) return "Normal";
  if (l <= 8) return "Maximum";
  return "Ultra";
}

static void dlg_compress(void) {
  const char *labels[] = { "Cancel", "Compress" };
  const char *close_l[] = { "Close" };
  float cw = dlg_cw(500);
  bool any = D.fmt >= 0;
  const FmtInfo *fi = any ? &kFmts[D.fmt] : NULL;
  bool enc = any && arc_fmt_can_encrypt(fi->fmt);
  bool lvl = any && fi->fmt != ARC_TAR;
  float sl = font_line_h(ui.m.font_small) + DP(6);
  float chip_h = DP(ui.touch_mode ? 40 : 32);
  /* format chips wrap: estimate rows */
  int nf = 0;
  for (int i = 0; i < FM_COUNT(kFmts); i++) if (arc_fmt_can_create(kFmts[i].fmt)) nf++;
  float row_w = 0;
  int chip_rows = nf ? 1 : 0;
  for (int i = 0; i < FM_COUNT(kFmts); i++) {
    if (!arc_fmt_can_create(kFmts[i].fmt)) continue;
    float w = font_width(FONT_BOLD, ui.m.font_small, kFmts[i].label, -1) + DP(28);
    if (row_w + w > cw && row_w > 0) { chip_rows++; row_w = 0; }
    row_w += w + DP(8);
  }
  float ch;
  if (!any) {
    ch = font_line_h(ui.m.font) * 3 + DP(20) + btn_h();
  } else {
    ch = sl + field_h() + DP(12) + sl + (float)chip_rows * (chip_h + DP(8)) + DP(6) +
         (lvl ? sl + DP(36) + DP(6) : 0) +
         (enc ? sl + field_h() + DP(10) + (fi->fmt == ARC_ZIP || fi->fmt == ARC_7Z ? DP(40) : 0) : 0) +
         font_line_h(ui.m.font_small) + DP(16) + font_line_h(ui.m.font_small) + DP(8) +
         btns_h(cw, labels, 2);
  }
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.compress"), "Compress", 500, dlg_h(ch), &cancel);
  if (!any) {
    font_draw_wrap(FONT_REGULAR, ui.m.font, c.x, c.y, c.w,
                   "Creating archives is not available in this build.", T.text2, true);
    int b = dlg_buttons(c, close_l, 1, false);
    ui_dialog_end();
    if (cancel || b == 0) dlg_close();
    return;
  }
  small_label(&c, "Archive name");
  FmRect f = rect_cut_top(&c, field_h());
  float ew = font_width(FONT_REGULAR, ui.m.font, fi->ext, -1) + DP(14);
  FmRect extr = rect_cut_right(&f, ew);
  int r = ui_textfield(ui_id("dlg.compress.name"), f, D.name, sizeof D.name, "Name", UI_TF_FOCUS);
  if (r & UI_TF_CHANGED) D.err[0] = 0;
  font_draw(FONT_REGULAR, ui.m.font, extr.x + DP(8), extr.y + (extr.h - font_line_h(ui.m.font)) * 0.5f,
            fi->ext, -1, T.text2);
  rect_cut_top(&c, DP(12));
  small_label(&c, "Format");
  {
    float x = c.x, y = c.y;
    for (int i = 0; i < FM_COUNT(kFmts); i++) {
      if (!arc_fmt_can_create(kFmts[i].fmt)) continue;
      float w = font_width(FONT_BOLD, ui.m.font_small, kFmts[i].label, -1) + DP(28);
      if (x + w > c.x + c.w && x > c.x) { x = c.x; y += chip_h + DP(8); }
      FmRect chip = { x, y, w, chip_h };
      bool on = i == D.fmt;
      int hf = ui_hit(ui_idn(ui_id("dlg.compress.fmt"), (u32)i), chip);
      gfx_rrect(chip, chip_h * 0.5f, on ? T.accent : T.surface2);
      if (!on) gfx_rrect_line(chip, chip_h * 0.5f, DP(1), T.border);
      if (hf & UI_HOVER) gfx_rrect(chip, chip_h * 0.5f, T.hover);
      font_draw_center(FONT_BOLD, ui.m.font_small, chip, kFmts[i].label, on ? T.on_accent : T.text);
      if (hf & UI_CLICK) D.fmt = i;
      x += w + DP(8);
    }
    rect_cut_top(&c, (float)chip_rows * (chip_h + DP(8)) + DP(6));
  }
  if (lvl) {
    char lv[64];
    fm_snprintf(lv, sizeof lv, "Compression: %s (%d)", level_name((int)(D.level + 0.5f)),
                (int)(D.level + 0.5f));
    small_label(&c, lv);
    FmRect sr = rect_cut_top(&c, DP(36));
    ui_slider(ui_id("dlg.compress.level"), sr, &D.level, 0, 9);
    D.level = floorf(D.level + 0.5f);
    rect_cut_top(&c, DP(6));
  }
  if (enc) {
    small_label(&c, "Password (optional)");
    FmRect pf = rect_cut_top(&c, field_h());
    FmRect eye = rect_cut_right(&pf, pf.h);
    ui_textfield(ui_id("dlg.compress.pw"), pf, D.pw, sizeof D.pw, "No password",
                 D.show_pw ? 0 : UI_TF_PASSWORD);
    if (ui_icon_btn(ui_id("dlg.compress.eye"), eye, D.show_pw ? IC_EYE_OFF : IC_EYE, T.text2,
                    D.show_pw ? "Hide password" : "Show password"))
      D.show_pw = !D.show_pw;
    rect_cut_top(&c, DP(10));
    if (fi->fmt == ARC_ZIP) {
      static const char *const kz[] = { "AES-256", "ZipCrypto (legacy)" };
      FmRect sg = rect_cut_top(&c, DP(32));
      sg.w = FM_MIN(sg.w, DP(320));
      ui_segmented(ui_id("dlg.compress.zipmode"), sg, kz, 2, &D.zip_mode);
      rect_cut_top(&c, DP(8));
    } else if (fi->fmt == ARC_7Z) {
      FmRect sw = rect_cut_top(&c, DP(40));
      ui_switch(ui_id("dlg.compress.encnames"), sw, "Encrypt file names", &D.enc_names);
    }
  }
  {
    char into[FM_PATH_MAX + 16];
    fm_snprintf(into, sizeof into, "Into %s", g_p[1 - D.panel].display);
    FmRect ir = rect_cut_top(&c, font_line_h(ui.m.font_small) + DP(8));
    icon_draw(IC_FOLDER, FM_RECT(ir.x, ir.y, DP(14), DP(14)), T.text2);
    font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font_small, ir.x + DP(20), ir.y, into, ir.w - DP(20), T.text2);
  }
  if (D.err[0]) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, c.y, D.err, c.w, T.danger);
  int b = dlg_buttons(c, labels, 2, false);
  ui_dialog_end();
  if (cancel || b == 0) { dlg_close(); return; }
  if (b == 1 || (r & UI_TF_SUBMIT)) start_compress();
}

static void start_extract(void) {
  FmPanel *p = &g_p[D.panel], *o = &g_p[1 - D.panel];
  FmJobSpec s;
  memset(&s, 0, sizeof s);
  s.conflict = CONFLICT_ASK;
  char dst[FM_PATH_MAX];
  if (D.from_arc) {
    fm_strlcpy(dst, o->list.loc.path, sizeof dst);
    if (D.subfolder) {
      char stem[256];
      fm_strlcpy(stem, fm_path_base(p->list.loc.path), sizeof stem);
      char *dot = strchr(stem + 1, '.');
      if (dot) *dot = 0;
      fm_path_join(dst, sizeof dst, o->list.loc.path, stem);
      plat_mkdirs(dst);
    }
    s.kind = JOB_EXTRACT;
    s.arc = p->list.loc.path;
    s.arc_inner = p->list.loc.inner;
    s.srcs = (const char *const *)D.paths;
    s.nsrc = D.npaths;
    s.dst = dst;
    if (!ops_start(&s)) ui_toast("Too many jobs are running");
  } else {
    for (int i = 0; i < D.npaths; i++) {
      fm_strlcpy(dst, o->list.loc.path, sizeof dst);
      if (D.subfolder) {
        char stem[256];
        fm_strlcpy(stem, fm_path_base(D.paths[i]), sizeof stem);
        char *dot = strchr(stem + 1, '.');
        if (dot) *dot = 0;
        char want[FM_PATH_MAX];
        fm_path_join(want, sizeof want, o->list.loc.path, stem);
        fm_strlcpy(dst, want, sizeof dst);
        plat_mkdirs(dst);
      }
      s.kind = JOB_EXTRACT;
      s.srcs = (const char *const *)&D.paths[i];
      s.nsrc = 1;
      s.dst = dst;
      if (!ops_start(&s)) { ui_toast("Too many jobs are running"); break; }
    }
  }
  panel_select_all(p, false);
  dlg_close();
}

static void dlg_extract(void) {
  const char *labels[] = { "Cancel", "Extract" };
  float cw = dlg_cw(480);
  float lh = font_line_h(ui.m.font) + DP(6);
  float ch = (font_line_h(ui.m.font_small) + DP(6) + lh) * 2 + DP(10) + DP(44) + DP(14) +
             btns_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.extract"), "Extract", 480, dlg_h(ch), &cancel);
  FmPanel *p = &g_p[D.panel], *o = &g_p[1 - D.panel];
  char from[FM_PATH_MAX + 64];
  if (D.from_arc) {
    if (D.npaths == 0) fm_snprintf(from, sizeof from, "Everything in %s", p->display);
    else if (D.npaths == 1) fm_snprintf(from, sizeof from, "\"%s\" from %s", D.paths[0],
                                         fm_path_base(p->list.loc.path));
    else fm_snprintf(from, sizeof from, "%d items from %s", D.npaths, fm_path_base(p->list.loc.path));
  } else if (D.npaths == 1) {
    fm_strlcpy(from, fm_path_base(D.paths[0]), sizeof from);
  } else {
    fm_snprintf(from, sizeof from, "%d archives", D.npaths);
  }
  small_label(&c, "From");
  text_line(&c, from, T.text);
  rect_cut_top(&c, DP(4));
  small_label(&c, "To");
  text_line(&c, o->display, T.text);
  rect_cut_top(&c, DP(10));
  const char *sub = D.npaths > 1 && !D.from_arc ? "A new folder for each archive" : "Into a new folder";
  FmRect sw = rect_cut_top(&c, DP(44));
  ui_switch(ui_id("dlg.extract.sub"), sw, sub, &D.subfolder);
  int b = dlg_buttons(c, labels, 2, false);
  ui_dialog_end();
  if (cancel || b == 0) { dlg_close(); return; }
  if (b == 1) start_extract();
}

/* ---- dialogs: job questions --------------------------------------------- */

static void conflict_answer(FmConflict c) {
  if (D.qjob) ops_answer_conflict(D.qjob, c, D.apply_all);
  dlg_close();
}

static void info_card(FmRect r, const char *title, const FmStat *st, bool known, bool newer) {
  gfx_rrect(r, DP(12), T.surface2);
  float pad = DP(12);
  float y = r.y + pad;
  font_draw(FONT_BOLD, ui.m.font_small, r.x + pad, y, title, -1, T.text2);
  if (newer) {
    float tw = font_width(FONT_BOLD, ui.m.font_small, "Newer", -1) + DP(14);
    FmRect b = { r.x + r.w - pad - tw, y - DP(2), tw, font_line_h(ui.m.font_small) + DP(4) };
    gfx_rrect(b, b.h * 0.5f, T.accent_soft);
    font_draw_center(FONT_BOLD, ui.m.font_small, b, "Newer", T.accent);
  }
  y += font_line_h(ui.m.font_small) + DP(6);
  char a[32], b2[32], line[96];
  if (known) {
    if (st->flags & FM_ST_DIR) fm_strlcpy(line, "Folder", sizeof line);
    else fm_strlcpy(line, fm_fmt_size(st->size, a, sizeof a), sizeof line);
    font_draw(FONT_BOLD, ui.m.font, r.x + pad, y, line, -1, T.text);
    y += font_line_h(ui.m.font) + DP(2);
    font_draw(FONT_REGULAR, ui.m.font_small, r.x + pad, y, fm_fmt_time(st->mtime, b2, sizeof b2), -1,
              T.text2);
  } else {
    font_draw(FONT_REGULAR, ui.m.font, r.x + pad, y, "From the archive", -1, T.text);
  }
}

static void dlg_conflict(void) {
  const char *labels[] = { "Cancel", "Skip", "Keep both", "Replace" };
  float cw = dlg_cw(500);
  float card_h = DP(84);
  bool cards_row = cw >= DP(360);
  float ch = font_line_h(ui.m.font) * 2 + DP(14) + (cards_row ? card_h : card_h * 2 + DP(10)) + DP(12) +
             DP(40) + DP(12) + btns_h(cw, labels, 4);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.conflict"), "This name is taken", 500, dlg_h(ch), &cancel);
  char dir[FM_PATH_MAX], msg[FM_PATH_MAX + 128];
  fm_strlcpy(dir, D.ask.dst, sizeof dir);
  fm_path_parent(dir);
  fm_snprintf(msg, sizeof msg, "\"%s\" already exists in \"%s\".", fm_path_base(D.ask.dst),
              fm_path_base(dir)[0] ? fm_path_base(dir) : dir);
  FmRect mr = rect_cut_top(&c, font_line_h(ui.m.font) * 2 + DP(14));
  font_draw_wrap(FONT_REGULAR, ui.m.font, mr.x, mr.y, mr.w, msg, T.text, true);
  bool src_newer = D.ask.src_known && D.ask.src_st.mtime > D.ask.dst_st.mtime;
  bool dst_newer = D.ask.src_known && D.ask.dst_st.mtime > D.ask.src_st.mtime;
  if (cards_row) {
    FmRect cr = rect_cut_top(&c, card_h);
    float w = (cr.w - DP(10)) * 0.5f;
    info_card(FM_RECT(cr.x, cr.y, w, cr.h), "EXISTING", &D.ask.dst_st, true, dst_newer);
    info_card(FM_RECT(cr.x + w + DP(10), cr.y, w, cr.h), "NEW", &D.ask.src_st, D.ask.src_known, src_newer);
  } else {
    info_card(rect_cut_top(&c, card_h), "EXISTING", &D.ask.dst_st, true, dst_newer);
    rect_cut_top(&c, DP(10));
    info_card(rect_cut_top(&c, card_h), "NEW", &D.ask.src_st, D.ask.src_known, src_newer);
  }
  rect_cut_top(&c, DP(12));
  ui_check(ui_id("dlg.conflict.all"), rect_cut_top(&c, DP(40)), "Do this for all conflicts", &D.apply_all);
  int b = dlg_buttons(c, labels, 4, false);
  ui_dialog_end();
  if (cancel || b == 0) conflict_answer(CONFLICT_CANCEL);
  else if (b == 1) conflict_answer(CONFLICT_SKIP);
  else if (b == 2) conflict_answer(CONFLICT_KEEP_BOTH);
  else if (b == 3) conflict_answer(CONFLICT_OVERWRITE);
}

static void dlg_password(void) {
  const char *labels[] = { "Cancel", "OK" };
  float cw = dlg_cw(440);
  float ch = font_line_h(ui.m.font) * 2 + DP(10) + (D.ask.retry ? font_line_h(ui.m.font) + DP(8) : 0) +
             field_h() + DP(16) + btns_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.password"), "Password required", 440, dlg_h(ch), &cancel);
  char msg[FM_PATH_MAX + 64];
  fm_snprintf(msg, sizeof msg, "\"%s\" is encrypted. Enter its password.", fm_path_base(D.ask.src));
  FmRect mr = rect_cut_top(&c, font_line_h(ui.m.font) * 2 + DP(10));
  font_draw_wrap(FONT_REGULAR, ui.m.font, mr.x, mr.y, mr.w, msg, T.text2, true);
  if (D.ask.retry) {
    FmRect er = rect_cut_top(&c, font_line_h(ui.m.font) + DP(8));
    icon_draw(IC_WARN, FM_RECT(er.x, er.y + DP(1), DP(16), DP(16)), T.danger);
    font_draw(FONT_REGULAR, ui.m.font, er.x + DP(22), er.y, "Wrong password. Try again.", -1, T.danger);
  }
  FmRect f = rect_cut_top(&c, field_h());
  FmRect eye = rect_cut_right(&f, f.h);
  int r = ui_textfield(ui_id("dlg.password.tf"), f, D.pw, sizeof D.pw, "Password",
                       UI_TF_FOCUS | (D.show_pw ? 0 : UI_TF_PASSWORD));
  if (ui_icon_btn(ui_id("dlg.password.eye"), eye, D.show_pw ? IC_EYE_OFF : IC_EYE, T.text2,
                  D.show_pw ? "Hide password" : "Show password"))
    D.show_pw = !D.show_pw;
  int b = dlg_buttons(c, labels, 2, false);
  ui_dialog_end();
  bool ok = b == 1 || (r & UI_TF_SUBMIT);
  if (!(cancel || b == 0 || (r & UI_TF_CANCEL) || ok)) return;
  if (D.pw_panel >= 0) {
    int pi = D.pw_panel;
    FmLoc loc = D.pw_loc;
    char pw[128];
    fm_strlcpy(pw, D.pw, sizeof pw);
    dlg_close();
    if (ok) {
      ops_password_remember(loc.path, pw);
      memset(pw, 0, sizeof pw);
      /* a wrong password makes panel_go ask again: show that as a retry */
      if (!panel_go(&g_p[pi], &loc, true) && D.kind == DLG_PASSWORD) D.ask.retry = true;
    }
    return;
  }
  if (D.qjob) ops_answer_password(D.qjob, ok ? D.pw : NULL);
  dlg_close();
}

/* ---- dialogs: settings and exit ----------------------------------------- */

static void dlg_settings(void) {
  const char *labels[] = { "Done" };
  bool cancel;
  float h = FM_MIN(640.0f, ui.h / ui.scale - 32);
  FmRect c = ui_dialog_begin(ui_id("dlg.settings"), "Settings", 520, h, &cancel);
  FmRect btns = rect_cut_bottom(&c, btn_h());
  rect_cut_bottom(&c, DP(10));
  float rh = DP(ui.touch_mode ? 50 : 42), sl = font_line_h(ui.m.font_small) + DP(14);
  float seg_h = DP(ui.touch_mode ? 40 : 34);
  float grid_h = theme_grid_h(c.w - DP(6));
  float content = sl * 5 + grid_h + DP(10) + (seg_h + DP(10)) * 3 + DP(46) + rh * 3 + rh * 5 +
                  DP(50) + DP(90);
  float online_h = online_settings_h(c.w - DP(6));   /* online: its section */
  static bool online_jump;
  if (online_settings_focus()) online_jump = true;
  content += online_h;
  content += photo_settings_h(c.w - DP(6));          /* photos: its section */
  static bool photo_jump;
  if (photo_settings_focus()) photo_jump = true;
  content += aonline_settings_h(c.w - DP(6));        /* audio: its section */
  static bool audio_jump;
  if (aonline_settings_focus()) audio_jump = true;
  u32 sid = ui_id("dlg.settings.scroll");
  ui_scroll(&D.scroll, sid, c, content);
  gfx_clip_push(c);
  FmRect r = { c.x, c.y - D.scroll.y, c.w - DP(6), content };
  u32 base = ui_id("dlg.settings.w");
  int k = 0;

  small_label(&r, "THEME");
  theme_grid(&r, ui_idn(base, 300), grid_h);
  rect_cut_top(&r, DP(10));
  small_label(&r, "APPEARANCE");
  {
    static const char *const kMode[] = { "Dark", "Light" };
    int t = T.dark ? 0 : 1;
    FmRect sg = rect_cut_top(&r, seg_h);
    if (theme_has_both(conf.theme)) {
      if (ui_segmented(ui_idn(base, (u32)k), sg, kMode, 2, &t)) {
        conf.dark = t == 0;
        theme_apply(conf.theme, conf.dark, conf.accent);
        conf_dirty();
      }
    } else {
      font_draw(FONT_REGULAR, ui.m.font_small, sg.x + DP(2),
                sg.y + (sg.h - font_line_h(ui.m.font_small)) * 0.5f,
                T.dark ? "This theme is dark only" : "This theme is light only", -1, T.text2);
    }
    k++;
    rect_cut_top(&r, DP(10));
    /* accent: the theme's own first, then the fixed swatches */
    FmRect ar = rect_cut_top(&r, DP(36));
    float cs = DP(28), step = FM_MIN(cs + DP(12), (ar.w - DP(12)) / (float)(UI_ACCENTS + 1));
    for (int i = -1; i < UI_ACCENTS; i++) {
      FmRect b = { ar.x + DP(6) + (float)(i + 1) * step, ar.y + (ar.h - cs) * 0.5f, cs, cs };
      int f = ui_hit(ui_idn(base, 100 + (u32)(i + 1)), rect_inset(b, -DP(4)));
      FmColor col = i < 0 ? theme_accent_of(conf.theme, T.dark) : kAccents[i];
      float cx = b.x + cs * 0.5f, cy = b.y + cs * 0.5f;
      gfx_circle(cx, cy, cs * 0.5f, col);
      if (i < 0 && i != conf.accent) gfx_ring(cx, cy, cs * 0.5f - DP(5), DP(2), col_alpha(T.surface, 0.8f));
      if (i == conf.accent) {
        gfx_ring(cx, cy, cs * 0.5f + DP(4), DP(2), col);
        icon_draw(IC_CHECK, rect_inset(b, DP(6)), i < 0 ? T.on_accent : FM_HEX(0xFFFFFF));
      } else if (f & UI_HOVER) {
        gfx_ring(cx, cy, cs * 0.5f + DP(3), DP(1.5f), T.border);
      }
      if (f & UI_CLICK) {
        conf.accent = i;
        theme_apply(conf.theme, conf.dark, conf.accent);
        conf_dirty();
      }
    }
    rect_cut_top(&r, DP(10));
  }
  {
    char z[64];
    fm_snprintf(z, sizeof z, "UI SIZE  %d%%", (int)(D.zoom * 100 + 0.5f));
    small_label(&r, z);
    FmRect sr = rect_cut_top(&r, seg_h);
    u32 zid = ui_idn(base, 200);
    ui_slider(zid, sr, &D.zoom, 0.75f, 2.0f);
    D.zoom = floorf(D.zoom * 20 + 0.5f) / 20;
    bool held = ui.active == zid && ui.down;
    if (!held && fabsf(D.zoom - conf.zoom) > 0.001f) {
      conf.zoom = D.zoom;
      ui.zoom = conf.zoom;
      ui_update_scale();
      conf_dirty();
    }
    rect_cut_top(&r, DP(10));
  }
  small_label(&r, "LAYOUT");
  {
    static const char *const kLay[] = { "Auto", "Side", "Stacked", "Single" };
    int l = conf.layout;
    if (ui_segmented(ui_idn(base, (u32)k++), rect_cut_top(&r, seg_h), kLay, 4, &l)) {
      conf.layout = l;
      conf_dirty();
    }
    rect_cut_top(&r, DP(10));
    static const char *const kTouch[] = { "Touch: auto", "On", "Off" };
    int t = conf.touch < 0 ? 0 : conf.touch == 1 ? 1 : 2;
    if (ui_segmented(ui_idn(base, (u32)k++), rect_cut_top(&r, seg_h), kTouch, 3, &t)) {
      conf.touch = t == 0 ? -1 : t == 1 ? 1 : 0;
      apply_touch();
      conf_dirty();
    }
    rect_cut_top(&r, DP(10));
  }
#if !defined(FM_MOBILE) && !defined(FM_WEB)
  if (ui_switch(ui_idn(base, 310), rect_cut_top(&r, rh), "System title bar", &conf.system_title)) {
    title_apply();
    conf_dirty();
  }
  rect_cut_top(&r, DP(10));
#endif
  small_label(&r, "FILES");
  {
    bool v = conf.show_hidden;
    if (ui_switch(ui_idn(base, 300), rect_cut_top(&r, rh), "Show hidden files", &v)) toggle_hidden();
    if (ui_switch(ui_idn(base, 301), rect_cut_top(&r, rh), "Folders first", &conf.folders_first)) {
      panel_resort(&g_p[0]);
      panel_resort(&g_p[1]);
      conf_dirty();
    }
    if (ui_switch(ui_idn(base, 302), rect_cut_top(&r, rh), "Thumbnails", &conf.thumbnails)) {
      if (!conf.thumbnails) thumb_cancel_all();
      conf_dirty();
    }
    if (ui_switch(ui_idn(base, 303), rect_cut_top(&r, rh), "Ask before deleting", &conf.confirm_delete))
      conf_dirty();
    bool tr = conf.use_trash;
    if (ops_trash_available()) {
      if (ui_switch(ui_idn(base, 304), rect_cut_top(&r, rh), "Delete to the Trash", &tr)) {
        conf.use_trash = tr;
        conf_dirty();
      }
    } else {
      FmRect tr_r = rect_cut_top(&r, rh);
      font_draw(FONT_REGULAR, ui.m.font, tr_r.x, tr_r.y + (rh - font_line_h(ui.m.font)) * 0.5f,
                "Trash: not available on this system", -1, T.text3);
    }
    rect_cut_top(&r, DP(14));
  }
  /* online: online videos (fonline_set.c); opened from that view it scrolls here */
  if (online_jump) {
    D.scroll.y = r.y - c.y + D.scroll.y;
    online_jump = false;
    ui_redraw();
  }
  online_settings(&r, ui_idn(base, 400));
  /* photos: online photos (fphoto_set.c); opened from that view it scrolls here */
  if (photo_jump) {
    D.scroll.y = r.y - c.y + D.scroll.y;
    photo_jump = false;
    ui_redraw();
  }
  photo_settings(&r, ui_idn(base, 500));
  /* audio: online audio (faudio_online_set.c); opened from that view it scrolls here */
  if (audio_jump) {
    D.scroll.y = r.y - c.y + D.scroll.y;
    audio_jump = false;
    ui_redraw();
  }
  aonline_settings(&r, ui_idn(base, 600));
  small_label(&r, "ABOUT");
  {
    char ver[96];
    fm_snprintf(ver, sizeof ver, "MMC File Manager %s", FM_VERSION);
    FmRect a = rect_cut_top(&r, font_line_h(ui.m.font) + DP(4));
    font_draw(FONT_BOLD, ui.m.font, a.x, a.y, ver, -1, T.text);
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, r.x, r.y, r.w,
                   "MMC File Manager is free software: you can use, study, share and improve it.",
                   T.text2, true);
  }
  gfx_clip_pop();
  ui_scrollbar(&D.scroll, c, content);
  int b = dlg_buttons(btns, labels, 1, false);
  ui_dialog_end();
  if (cancel || b == 0) dlg_close();
}

static void dlg_exit(void) {
  const char *labels[] = { "Cancel", "Exit" };
  float cw = dlg_cw(420);
  float ch = font_line_h(ui.m.font) * (D.jobs_warn ? 3 : 1) + DP(20) + btns_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("dlg.exit"), "Exit MMC File Manager?", 420, dlg_h(ch), &cancel);
  if (D.jobs_warn) {
    int n = ops_running(false) + online_downloads_active() + photo_downloads_active() +
            aonline_downloads_active();   /* photos, audio */
    char msg[160];
    fm_snprintf(msg, sizeof msg, "%d %s still running. %s will be cancelled.", n,
                plural(n, "job or download is", "jobs or downloads are"), plural(n, "It", "They"));
    font_draw_wrap(FONT_REGULAR, ui.m.font, c.x, c.y, c.w, msg, T.warn, true);
  } else {
    font_draw(FONT_REGULAR, ui.m.font, c.x, c.y, "Your folders will be here next time.", -1, T.text2);
  }
  int b = dlg_buttons(c, labels, 2, D.jobs_warn);
  ui_dialog_end();
  if (cancel || b == 0) { dlg_close(); return; }
  if (b == 1) {
    ops_cancel_all();
    dlg_close();
    app.quit = true;
  }
}

static void draw_dialogs(void) {
  if (D.kind == DLG_NONE) {
    FmAsk a;
    FmJob *j = ops_question(&a);
    if (j) {
      dlg_open(a.kind == ASK_PASSWORD ? DLG_PASSWORD : DLG_CONFLICT, g_active);
      D.qjob = j;
      D.ask = a;
    }
  }
  switch (D.kind) {
    case DLG_RENAME: case DLG_NEWDIR: case DLG_NEWFILE: dlg_name(); break;
    case DLG_DELETE: dlg_delete(); break;
    case DLG_PROPS: dlg_props(); break;
    case DLG_COMPRESS: dlg_compress(); break;
    case DLG_EXTRACT: dlg_extract(); break;
    case DLG_CONFLICT: dlg_conflict(); break;
    case DLG_PASSWORD: dlg_password(); break;
    case DLG_SETTINGS: dlg_settings(); break;
    case DLG_EXIT: dlg_exit(); break;
    default: break;
  }
}

/* ---- top bar, sidebar, tabs --------------------------------------------- */

static void draw_top(FmRect r) {
  u32 base = ui_id("top");
  float bs = DP(ui.touch_mode ? 46 : 38);
  title_bar(r);
  FmRect in = r;
  title_buttons(&in);
  in = rect_inset2(in, DP(8), 0);
  if (g_L.wide && g_L.mode == LAYOUT_SIDE) {
    FmRect b = rect_center(rect_cut_left(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_toggle_btn(ui_idn(base, 1), b, IC_PANELS, g_sidebar_open, "Places sidebar"))
      g_sidebar_open = !g_sidebar_open;
    rect_cut_left(&in, DP(4));
  } else {
    /* app mark: a small accent tile with a folder */
    FmRect m = rect_center(rect_cut_left(&in, bs), DP(28), DP(28));
    gfx_rrect_vgrad(m, DP(8), col_mix(T.accent, FM_HEX(0xFFFFFF), 0.15f), T.accent);
    icon_draw(IC_FOLDER, rect_inset(m, DP(6)), T.on_accent);
    rect_cut_left(&in, DP(6));
  }
  /* right side buttons */
  FmRect b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 2), b, IC_SETTINGS, T.text2, "Settings")) open_settings();
  if (theme_has_both(conf.theme)) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 3), b, T.dark ? IC_SUN : IC_MOON, T.text2,
                    T.dark ? "Light mode" : "Dark mode")) {
      conf.dark = !T.dark;
      theme_apply(conf.theme, conf.dark, conf.accent);
      conf_dirty();
    }
  }
  if (!g_L.sidebar) {
    b = rect_center(rect_cut_right(&in, bs), bs, bs);
    title_nodrag(b);
    if (ui_icon_btn(ui_idn(base, 4), b, IC_DRIVE, T.text2, "Places")) app_panel_places(P(), b.x, b.y + b.h);
  }
  b = rect_center(rect_cut_right(&in, bs), bs, bs);
  title_nodrag(b);
  if (ui_icon_btn(ui_idn(base, 5), b, IC_SEARCH, T.text2, "Filter (Ctrl+F)")) panel_open_search(P());
  /* title, or the active folder on narrow screens */
  if (g_L.narrow) {
    char t[256];
    loc_title(&P()->list.loc, t, sizeof t);
    float y = in.y + (in.h - font_line_h(ui.m.font) - font_line_h(ui.m.font_small)) * 0.5f;
    font_draw_ellipsis(FONT_BOLD, ui.m.font, in.x + DP(4), y, t, in.w - DP(8), T.text);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, in.x + DP(4), y + font_line_h(ui.m.font),
                       P()->display, in.w - DP(8), T.text2);
  } else {
    float y = in.y + (in.h - font_line_h(ui.m.font_title)) * 0.5f;
    float x = font_draw(FONT_BOLD, ui.m.font_title, in.x + DP(4), y, "MMC File Manager", -1, T.text);
    (void)x;
  }
}

static void draw_sidebar(FmRect r) {
  if (g_places_dirty) build_places();
  gfx_shadow(r, ui.m.radius, DP(10), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, ui.m.radius, T.surface);
  gfx_rrect_line(r, ui.m.radius, DP(1), T.border);
  FmRect in = rect_inset(r, DP(8));
  float rh = DP(ui.touch_mode ? 46 : 36), hh = DP(28);
  float content = 0;
  int last = -1;
  for (int i = 0; i < g_nplaces; i++) {
    if (g_places[i].section != last) { content += hh; last = g_places[i].section; }
    content += rh + (g_places[i].section == 0 && g_places[i].total ? DP(6) : 0);
  }
  u32 sid = ui_id("side.scroll");
  ui_scroll(&g_side_scroll, sid, in, content);
  gfx_clip_push(in);
  static const char *const kSec[] = { "STORAGE", "PLACES", "BOOKMARKS", "RECENT" };
  float y = in.y - g_side_scroll.y;
  last = -1;
  const char *cur = P()->list.loc.path;
  for (int i = 0; i < g_nplaces; i++) {
    Place *pl = &g_places[i];
    if (pl->section != last) {
      last = pl->section;
      font_draw(FONT_BOLD, ui.m.font_small, in.x + DP(10), y + hh - font_line_h(ui.m.font_small) - DP(4),
                kSec[pl->section], -1, T.text3);
      y += hh;
    }
    float h = rh + (pl->section == 0 && pl->total ? DP(6) : 0);
    FmRect row = { in.x, y, in.w, h };
    y += h;
    if (!gfx_visible(row)) continue;
    int f = ui_hit(ui_idn(sid, (u32)i + 1), row);
    bool here = !P()->list.loc.in_arc && same_path(cur, pl->path);
    if (here) gfx_rrect(row, DP(10), T.accent_soft);
    else if (f & UI_HOVER) gfx_rrect(row, DP(10), T.hover);
    float is = DP(18);
    icon_draw(pl->icon, FM_RECT(row.x + DP(10), row.y + (rh - is) * 0.5f, is, is),
              here ? T.accent : pl->section == 2 ? T.warn : T.text2);
    font_draw_ellipsis(here ? FONT_BOLD : FONT_REGULAR, ui.m.font, row.x + DP(38),
                       row.y + (rh - font_line_h(ui.m.font)) * 0.5f, pl->label, row.w - DP(46),
                       here ? T.accent : T.text);
    if (pl->section == 0 && pl->total) {
      float used = 1.0f - (float)((double)pl->free_b / (double)pl->total);
      FmRect bar = { row.x + DP(38), row.y + rh - DP(4), row.w - DP(50), DP(4) };
      gfx_rrect(bar, DP(2), T.surface3);
      gfx_rrect(FM_RECT(bar.x, bar.y, FM_MAX(bar.w * used, DP(4)), bar.h), DP(2),
                used > 0.9f ? T.danger : T.accent);
    }
    if (f & UI_CLICK) go_place(i, g_active);
  }
  gfx_clip_pop();
  ui_scrollbar(&g_side_scroll, in, content);
}

static void draw_tabs(FmRect r) {
  char t0[64], t1[64];
  loc_title(&g_p[0].list.loc, t0, sizeof t0);
  loc_title(&g_p[1].list.loc, t1, sizeof t1);
  char l0[80], l1[80];
  fm_snprintf(l0, sizeof l0, "Left: %s", t0);
  fm_snprintf(l1, sizeof l1, "Right: %s", t1);
  const char *labels[] = { l0, l1 };
  int sel = g_single_tab;
  if (ui_segmented(ui_id("tabs"), r, labels, 2, &sel)) {
    g_single_tab = sel;
    app_panel_activate(sel);
  }
}

/* ---- action bar --------------------------------------------------------- */

enum { ACT_COPY, ACT_MOVE, ACT_DELETE, ACT_RENAME, ACT_NEWDIR, ACT_EXTRACT, ACT_COMPRESS, ACT_SELALL };

typedef struct Act {
  int id;
  FmIcon icon;
  const char *label;
  const char *tip;
  bool on;
  int style;                    /* 0 plain, 1 filled, 2 tonal */
} Act;

static FmIcon target_arrow(void) {
  if (g_L.mode == LAYOUT_STACK) return g_active == 0 ? IC_ARROW_DOWN : IC_ARROW_UP;
  return g_active == 0 ? IC_ARROW_RIGHT : IC_ARROW_LEFT;
}

static void run_action(int id) {
  switch (id) {
    case ACT_COPY: do_transfer(false, P(), O()); break;
    case ACT_MOVE: do_transfer(true, P(), O()); break;
    case ACT_DELETE: open_delete(false); break;
    case ACT_RENAME: open_rename(); break;
    case ACT_NEWDIR: open_new(false); break;
    case ACT_EXTRACT: open_extract(); break;
    case ACT_COMPRESS: open_compress(); break;
    case ACT_SELALL: do_select_all(); break;
    default: break;
  }
}

static int build_actions(Act *a) {
  FmPanel *p = P(), *o = O();
  bool sel = p->nsel > 0, arc = p->list.loc.in_arc, o_ok = panel_is_local(o);
  bool sel_arcs = false;
  if (!arc && p->nsel > 0) {
    sel_arcs = true;
    for (int i = 0; i < p->list.count; i++) {
      const FmEntry *e = &p->list.items[i];
      if (e->selected && !is_archive_entry(e)) { sel_arcs = false; break; }
    }
  }
  bool can_create = creatable_fmt(0) >= 0;
  int n = 0;
  Act k[8] = {
    { ACT_COPY, target_arrow(), "Copy", "Copy to the other panel (F5)", sel && o_ok, 1 },
    { ACT_MOVE, IC_MOVE, "Move", "Move to the other panel (F6)", sel && o_ok && !arc, 2 },
    { ACT_DELETE, IC_DELETE, "Delete", "Delete (Del)", sel && !arc, 0 },
    { ACT_RENAME, IC_RENAME, "Rename", "Rename (F2)", p->nsel == 1 && !arc, 0 },
    { ACT_NEWDIR, IC_NEW_FOLDER, "New", "New folder (F7)", panel_is_local(p), 0 },
    { ACT_EXTRACT, IC_EXTRACT, "Extract", "Extract to the other panel", o_ok && (arc || sel_arcs), 0 },
    { ACT_COMPRESS, IC_COMPRESS, "Zip", "Compress into the other panel", sel && !arc && o_ok && can_create, 0 },
    { ACT_SELALL, IC_SELECT_ALL, "All", "Select all (Ctrl+A)", p->nview > 0, 0 },
  };
  for (int i = 0; i < 8; i++) a[n++] = k[i];
  return n;
}

static void act_button(u32 id, FmRect cell, const Act *a, bool label, FmIcon arrow) {
  float lh = label ? font_line_h(ui.m.font_small) + DP(2) : 0;
  float cs = FM_MIN(FM_MIN(cell.w - DP(8), cell.h - lh - DP(6)), DP(ui.touch_mode ? 48 : 42));
  cs = FM_MAX(cs, DP(28));
  FmRect circ = { cell.x + (cell.w - cs) * 0.5f, cell.y + (cell.h - cs - lh) * 0.5f, cs, cs };
  FmColor ic = a->on ? T.text : T.text3;
  if (a->style == 1) {
    gfx_circle(circ.x + cs * 0.5f, circ.y + cs * 0.5f, cs * 0.5f, a->on ? T.accent : col_alpha(T.accent, 0.3f));
    ic = T.on_accent;
  } else if (a->style == 2) {
    gfx_circle(circ.x + cs * 0.5f, circ.y + cs * 0.5f, cs * 0.5f, a->on ? T.accent_soft : T.surface3);
    ic = a->on ? T.accent : T.text3;
  }
  if (a->on) {
    if (ui_icon_btn(id, circ, a->icon, ic, a->tip)) run_action(a->id);
  } else {
    float is = FM_MIN(ui.m.icon, cs * 0.7f);
    icon_draw(a->icon, rect_center(circ, is, is), ic);
  }
  if (arrow != IC_NONE) {
    /* the move button carries the direction as a small badge */
    float bs = cs * 0.42f;
    FmRect bb = { circ.x + cs - bs * 0.8f, circ.y + cs - bs * 0.8f, bs, bs };
    gfx_circle(bb.x + bs * 0.5f, bb.y + bs * 0.5f, bs * 0.5f + DP(1.5f), T.bg);
    gfx_circle(bb.x + bs * 0.5f, bb.y + bs * 0.5f, bs * 0.5f, a->on ? T.accent : T.surface3);
    icon_draw(arrow, rect_inset(bb, bs * 0.18f), a->on ? T.on_accent : T.text3);
  }
  if (label) {
    float tw = font_width(FONT_REGULAR, ui.m.font_small, a->label, -1);
    font_draw(FONT_REGULAR, ui.m.font_small, cell.x + (cell.w - tw) * 0.5f, circ.y + cs + DP(3), a->label,
              -1, a->on ? T.text2 : T.text3);
  }
}

static void draw_action_bar(FmRect r, bool vertical) {
  u32 base = ui_id("act");
  /* the bar's own empty space is the splitter */
  u32 split_id = ui_idn(base, 999);
  int sf = g_L.mode == LAYOUT_SINGLE ? 0 : ui_hit(split_id, r);
  if (sf & UI_HOVER) ui_set_cursor(vertical ? SDL_SYSTEM_CURSOR_SIZEWE : SDL_SYSTEM_CURSOR_SIZENS);
  /* only an actual drag claims the pointer: claiming it on press made every
  ** tap on the buttons above a splitter grab, so they never got their click */
  if ((sf & UI_DRAG) && (ui.active == split_id || ui.drag_owner == split_id)) {
    ui.drag_owner = split_id;
    float s = layout_split_at(&g_L, ui.mx, ui.my);
    if (fabsf(s - conf.split) > 0.0005f) {
      conf.split = s;
      conf_dirty();
    }
  }
  FmRect bg = vertical ? rect_inset2(r, DP(6), 0) : rect_inset2(r, 0, DP(5));
  float rad = vertical ? bg.w * 0.5f : bg.h * 0.5f;
  if (sf & (UI_HELD | UI_HOVER)) gfx_rrect(bg, rad, T.hover);
  /* grip */
  if (vertical)
    gfx_rrect(FM_RECT(bg.x + bg.w * 0.5f - DP(2), bg.y + bg.h - DP(22), DP(4), DP(14)), DP(2), T.text3);
  else if (g_L.mode != LAYOUT_SINGLE)
    gfx_rrect(FM_RECT(bg.x + bg.w - DP(20), bg.y + bg.h * 0.5f - DP(7), DP(4), DP(14)), DP(2), T.text3);

  Act a[8];
  int n = build_actions(a);
  FmIcon move_arrow = target_arrow();
  if (vertical) {
    FmRect col = rect_inset2(bg, 0, DP(8));
    col.h -= DP(26);
    float full = DP(ui.touch_mode ? 74 : 66), compact = DP(ui.touch_mode ? 52 : 46);
    bool label = (float)n * full <= col.h;
    float each = label ? full : compact;
    while (n > 2 && (float)n * each > col.h) n--;
    float y = col.y + FM_MAX(0.0f, (col.h - (float)n * each) * 0.5f);
    for (int i = 0; i < n; i++) {
      FmRect cell = { col.x, y, col.w, each };
      act_button(ui_idn(base, (u32)a[i].id), cell, &a[i], label, a[i].id == ACT_MOVE ? move_arrow : IC_NONE);
      y += each;
    }
  } else {
    FmRect row = rect_inset2(bg, DP(8), 0);
    if (g_L.mode != LAYOUT_SINGLE) row.w -= DP(24);
    float each = row.w / (float)n;
    float min_w = DP(ui.touch_mode ? 52 : 44);
    while (n > 2 && each < min_w) { n--; each = row.w / (float)n; }
    bool label = each >= DP(56) && row.h >= DP(50);
    for (int i = 0; i < n; i++) {
      FmRect cell = { row.x + (float)i * each, row.y, each, row.h };
      act_button(ui_idn(base, (u32)a[i].id), cell, &a[i], label, a[i].id == ACT_MOVE ? move_arrow : IC_NONE);
    }
  }
}

/* ---- jobs strip --------------------------------------------------------- */

static float job_row_h(void) { return DP(ui.touch_mode ? 74 : 62); }

static int visible_jobs(FmJob **out, int max, int *total) {
  int n = 0, t = 0;
  for (int i = 0; i < ops_count(); i++) {
    FmJob *j = ops_at(i);
    if (ops_is_quiet(j)) continue;
    t++;
    if (n < max) out[n++] = j;
  }
  *total = t;
  return n;
}

static float jobs_target_h(void) {
  FmJob *js[2];
  int total;
  int n = visible_jobs(js, 2, &total);
  if (n == 0) return 0;
  return (float)n * (job_row_h() + DP(6)) + (total > n ? DP(22) : 0) + DP(4);
}

static FmIcon job_icon(FmJobKind k) {
  switch (k) {
    case JOB_COPY: return IC_COPY;
    case JOB_MOVE: return IC_MOVE;
    case JOB_DELETE: case JOB_TRASH: return IC_DELETE;
    case JOB_EXTRACT: return IC_EXTRACT;
    case JOB_COMPRESS: return IC_COMPRESS;
    case JOB_OPEN: return IC_OPEN_WITH;
    default: return IC_INFO;
  }
}

static void draw_job(FmJob *j, FmRect r) {
  FmJobInfo in;
  ops_info(j, &in);
  u32 base = ui_idn(ui_id("job"), (u32)in.id);
  gfx_shadow(r, DP(14), DP(8), col_alpha(T.shadow, 0.6f));
  gfx_rrect(r, DP(14), T.surface);
  gfx_rrect_line(r, DP(14), DP(1), T.border);
  FmRect c = rect_inset2(r, DP(12), DP(8));
  float bs = DP(ui.touch_mode ? 40 : 32);
  FmRect cancel = rect_center(rect_cut_right(&c, bs), bs, bs);
  FmRect pause = rect_center(rect_cut_right(&c, bs), bs, bs);
  if (ui_icon_btn(ui_idn(base, 1), cancel, IC_CLOSE, T.text2, "Cancel")) ops_cancel(j);
  if (!in.done && ui_icon_btn(ui_idn(base, 2), pause, in.paused ? IC_PLAY : IC_PAUSE, T.text2,
                              in.paused ? "Resume" : "Pause"))
    ops_pause(j, !in.paused);
  float is = DP(34);
  FmRect icr = { c.x, c.y + DP(2), is, is };
  gfx_circle(icr.x + is * 0.5f, icr.y + is * 0.5f, is * 0.5f, T.accent_soft);
  icon_draw(job_icon(in.kind), rect_inset(icr, DP(8)), T.accent);
  float x = c.x + is + DP(12), w = c.w - is - DP(16);
  float fs = ui.m.font_small;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, c.y, in.title, w, T.text);
  float y2 = c.y + font_line_h(ui.m.font) + DP(1);
  char stat[160], a[32], b[32], d[32];
  if (in.asking) fm_strlcpy(stat, "Waiting for your answer", sizeof stat);
  else if (in.paused) fm_strlcpy(stat, "Paused", sizeof stat);
  else if (in.scanning) {
    fmt_count(in.files_total, a, sizeof a);
    fm_snprintf(stat, sizeof stat, "Preparing... %s %s", a, in.files_total == 1 ? "file" : "files");
  } else {
    int pct = in.fraction >= 0 ? (int)(in.fraction * 100) : 0;
    if (in.bytes_total > 0) {
      fm_snprintf(stat, sizeof stat, "%d%%  \xC2\xB7  %s of %s", pct, fm_fmt_size(in.bytes_done, a, sizeof a),
                  fm_fmt_size(in.bytes_total, b, sizeof b));
      if (in.speed > 1024) {
        char sp[48];
        fm_snprintf(sp, sizeof sp, "  \xC2\xB7  %s/s", fm_fmt_size((u64)in.speed, d, sizeof d));
        fm_strlcat(stat, sp, sizeof stat);
      }
      if (in.eta_s >= 0) {
        char et[48], du[32];
        fmt_duration(in.eta_s, du, sizeof du);
        fm_snprintf(et, sizeof et, "  \xC2\xB7  %s left", du);
        fm_strlcat(stat, et, sizeof stat);
      }
    } else {
      fm_snprintf(stat, sizeof stat, "%d%%  \xC2\xB7  %llu of %llu", pct, (unsigned long long)in.files_done,
                  (unsigned long long)in.files_total);
    }
  }
  float sw = font_width(FONT_REGULAR, fs, stat, -1);
  bool two = w > sw + DP(160);
  if (two) {
    font_draw_ellipsis(FONT_REGULAR, fs, x, y2, in.current, w - sw - DP(16), T.text2);
    font_draw(FONT_REGULAR, fs, x + w - sw, y2, stat, -1, in.paused || in.asking ? T.warn : T.text2);
  } else {
    font_draw_ellipsis(FONT_REGULAR, fs, x, y2, stat, w, in.paused || in.asking ? T.warn : T.text2);
  }
  FmRect bar = { x, c.y + c.h - DP(5), w, DP(5) };
  ui_progress(bar, in.scanning ? -1.0f : FM_MAX(in.fraction, 0.0f));
  /* touch: long press for pause / cancel */
  int f = ui_hit(ui_idn(base, 3), FM_RECT(r.x, r.y, cancel.x - r.x - bs, r.h));
  if ((f & (UI_LONG | UI_RCLICK))) {
    FmMenuItem m[2];
    memset(m, 0, sizeof m);
    m[0].id = JM_PAUSE; m[0].icon = in.paused ? IC_PLAY : IC_PAUSE; m[0].label = in.paused ? "Resume" : "Pause";
    m[1].id = JM_CANCEL; m[1].icon = IC_CLOSE; m[1].label = "Cancel"; m[1].flags = UI_MI_DANGER;
    g_job_menu = j;
    ui_menu_open(ID_JOBMENU, ui.mx, ui.my, m, 2);
  }
}

static void draw_jobs(FmRect r) {
  FmJob *js[2];
  int total;
  int n = visible_jobs(js, 2, &total);
  gfx_clip_push(r);
  float y = r.y + DP(2);
  for (int i = 0; i < n; i++) {
    draw_job(js[i], FM_RECT(r.x, y, r.w, job_row_h()));
    y += job_row_h() + DP(6);
  }
  if (total > n) {
    char more[64];
    fm_snprintf(more, sizeof more, "+ %d more %s waiting", total - n, plural(total - n, "job", "jobs"));
    font_draw(FONT_REGULAR, ui.m.font_small, r.x + DP(14), y, more, -1, T.text2);
  }
  gfx_clip_pop();
}

/* ---- status bar --------------------------------------------------------- */

static void draw_status(FmRect r) {
  FmPanel *p = P();
  float fs = ui.m.font_small;
  FmRect in = rect_inset2(r, DP(14), 0);
  float ty = in.y + (in.h - font_line_h(fs)) * 0.5f;
  if (g_space_dirty) {
    panel_disk_dir(p, g_space_path, sizeof g_space_path);
    if (!plat_disk_space(g_space_path, &g_space_total, &g_space_free)) g_space_total = 0;
    g_space_dirty = false;
  }
  char buf[200], a[32], b[32];
  float x = in.x;
  if (g_space_total > 0) {
    icon_draw(IC_DRIVE, FM_RECT(x, in.y + (in.h - DP(14)) * 0.5f, DP(14), DP(14)), T.text2);
    x += DP(20);
    fm_snprintf(buf, sizeof buf, "%s free of %s", fm_fmt_size(g_space_free, a, sizeof a),
                fm_fmt_size(g_space_total, b, sizeof b));
    x = font_draw(FONT_REGULAR, fs, x, ty, buf, -1, T.text2) + DP(8);
    float used = 1.0f - (float)((double)g_space_free / (double)g_space_total);
    FmRect bar = { x, in.y + in.h * 0.5f - DP(2), DP(60), DP(4) };
    if (bar.x + bar.w < in.x + in.w * 0.5f) {
      gfx_rrect(bar, DP(2), T.surface3);
      gfx_rrect(FM_RECT(bar.x, bar.y, FM_MAX(bar.w * used, DP(4)), bar.h), DP(2),
                used > 0.9f ? T.danger : T.accent);
      x = bar.x + bar.w + DP(12);
    }
  }
  /* right: selection, clipboard, running jobs */
  float rx = in.x + in.w;
  int running = ops_running(false);
  if (running > 0) {
    fm_snprintf(buf, sizeof buf, "%d %s", running, plural(running, "job", "jobs"));
    float w = font_width(FONT_BOLD, fs, buf, -1);
    rx -= w;
    font_draw(FONT_BOLD, fs, rx, ty, buf, -1, T.accent);
    rx -= DP(16);
  }
  if (g_nclip > 0) {
    fm_snprintf(buf, sizeof buf, "%d in clipboard", g_nclip);
    float w = font_width(FONT_REGULAR, fs, buf, -1);
    if (rx - w > x + DP(40)) {
      rx -= w;
      font_draw(FONT_REGULAR, fs, rx, ty, buf, -1, T.text2);
      icon_draw(g_clip_cut ? IC_CUT : IC_PASTE, FM_RECT(rx - DP(18), in.y + (in.h - DP(13)) * 0.5f, DP(13), DP(13)),
                T.text2);
      rx -= DP(34);
    }
  }
  if (p->nsel > 0) fm_snprintf(buf, sizeof buf, "%d of %d selected", p->nsel, p->nview);
  else fm_snprintf(buf, sizeof buf, "%d %s", p->nview, plural(p->nview, "item", "items"));
  float w = font_width(FONT_REGULAR, fs, buf, -1);
  if (rx - w > x + DP(10)) font_draw(FONT_REGULAR, fs, rx - w, ty, buf, -1, T.text2);
}

/* ---- drag and drop ------------------------------------------------------ */

static void draw_drag(void) {
  if (!g_drag) return;
  FmPanel *to = &g_p[1 - g_drag_from];
  bool move = (ui.mod & KMOD_SHIFT) != 0;
  bool over = g_L.show[to->idx] && rect_has(to->rect, ui.mx, ui.my) && panel_is_local(to);
  if (move && g_p[g_drag_from].list.loc.in_arc) move = false;
  if (over) gfx_rrect_line(rect_inset(to->rect, -DP(3)), ui.m.radius + DP(3), DP(2.5f), T.accent);
  char t[96];
  fm_snprintf(t, sizeof t, "%s %d %s", move ? "Move" : "Copy", g_drag_n, plural(g_drag_n, "item", "items"));
  float w = font_width(FONT_BOLD, ui.m.font, t, -1) + DP(58);
  FmRect r = { ui.mx + DP(14), ui.my + DP(10), w, DP(40) };
  gfx_shadow(r, DP(12), DP(12), T.shadow);
  gfx_rrect(r, DP(12), over ? T.accent : T.surface2);
  icon_draw(move ? IC_MOVE : IC_COPY, FM_RECT(r.x + DP(12), r.y + DP(10), DP(20), DP(20)),
            over ? T.on_accent : T.text);
  font_draw(FONT_BOLD, ui.m.font, r.x + DP(42), r.y + (r.h - font_line_h(ui.m.font)) * 0.5f, t, -1,
            over ? T.on_accent : T.text);
  ui_set_cursor(over ? SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_NO);
  if (ui.released || !ui.down) {
    g_drag = false;
    if (over) {
      app_panel_activate(g_drag_from);
      do_transfer(move, &g_p[g_drag_from], to);
    }
  }
  ui_redraw();
}

/* ---- keyboard ----------------------------------------------------------- */

static void back_action(void) {
  FmPanel *p = P();
  if (g_drag) { g_drag = false; return; }
  if (p->editing_path) { p->editing_path = false; return; }
  if (p->select_mode || p->nsel > 0) {
    panel_select_all(p, false);
    p->select_mode = false;
    return;
  }
  if (p->filter[0] || p->search_open) {
    p->search_open = false;
    panel_set_filter(p, "");
    return;
  }
  if (panel_up(p)) return;
  open_exit();
}

static void global_keys(void) {
  if (!ui_input_ok()) return;
  if (ui_key(SDLK_TAB, 0) || ui_key(SDLK_TAB, KMOD_SHIFT)) {
    app_panel_activate(1 - g_active);
    if (g_L.mode == LAYOUT_SINGLE) g_single_tab = g_active;
  }
  if (ui_key(SDLK_F5, 0)) do_transfer(false, P(), O());
  if (ui_key(SDLK_F6, 0)) do_transfer(true, P(), O());
  if (ui_key(SDLK_F2, 0)) open_rename();
  if (ui_key(SDLK_F7, 0)) open_new(false);
  if (ui_key(SDLK_F4, KMOD_SHIFT)) open_new(true);
  if (ui_key(SDLK_F8, 0) || ui_key(SDLK_DELETE, 0)) open_delete(false);
  if (ui_key(SDLK_F8, KMOD_SHIFT) || ui_key(SDLK_DELETE, KMOD_SHIFT)) open_delete(true);
  if (ui_key(SDLK_c, KMOD_CTRL)) clip_set(false);
  if (ui_key(SDLK_x, KMOD_CTRL)) clip_set(true);
  if (ui_key(SDLK_v, KMOD_CTRL)) clip_paste();
  if (ui_key(SDLK_h, KMOD_CTRL)) toggle_hidden();
  if (ui_key(SDLK_r, KMOD_CTRL)) { panel_refresh(&g_p[0]); panel_refresh(&g_p[1]); }
  if (ui_key(SDLK_RETURN, KMOD_ALT)) open_props(false);
  if (ui_key(SDLK_COMMA, KMOD_CTRL)) open_settings();
  if (ui_key(SDLK_q, KMOD_CTRL)) { if (app_can_quit()) app.quit = true; }
  if (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0)) back_action();
}

/* ---- init / shutdown ---------------------------------------------------- */

static const char *arg_value(const char *name) {
  for (int i = 1; i + 1 < app.argc; i++)
    if (strcmp(app.argv[i], name) == 0) return app.argv[i + 1];
  return NULL;
}

static bool arg_flag(const char *name) {
  for (int i = 1; i < app.argc; i++)
    if (strcmp(app.argv[i], name) == 0) return true;
  return false;
}

/* A plain path argument ("mmcfm ~/Downloads", "Open with"): opens in the left panel. */
static const char *arg_positional(void) {
  static const char *const kVal[] = { "--shot", "--size", "--frames", "--left", "--right",
                                      "--demo-dialog", "--layout", "--demo-library",
                                      "--demo-online", "--demo-online-state",
                                      "--demo-photos", "--demo-photos-state" /* photos */,
                                      "--demo-audio-online", "--demo-audio-online-state" /* audio */ };
  for (int i = 1; i < app.argc; i++) {
    const char *a = app.argv[i];
    bool val = false;
    for (int k = 0; k < FM_COUNT(kVal); k++)
      if (!strcmp(a, kVal[k])) val = true;
    if (val) { i++; continue; }
    if (a[0] != '-' && a[0]) return a;
  }
  return NULL;
}

static void default_path(int idx, char *out, size_t cap) {
  if (conf.path[idx][0] && plat_is_dir(conf.path[idx])) {
    fm_strlcpy(out, conf.path[idx], cap);
    return;
  }
  if (idx == 1 && (plat_place(PLACE_DOWNLOADS, out, cap) || plat_place(PLACE_DOCUMENTS, out, cap)))
    return;
  if (!plat_place(PLACE_HOME, out, cap)) fm_strlcpy(out, FM_SEP_STR, cap);
}

static void restore_window(void) {
#ifndef FM_MOBILE
  if (conf.win_w >= 400 && conf.win_h >= 300) SDL_SetWindowSize(app.win, conf.win_w, conf.win_h);
  if (conf.win_x != -1 || conf.win_y != -1) {
    /* only when the saved spot is still on a connected display */
    int nd = SDL_GetNumVideoDisplays();
    for (int i = 0; i < nd; i++) {
      SDL_Rect b;
      if (SDL_GetDisplayUsableBounds(i, &b) != 0) continue;
      if (conf.win_x >= b.x - 50 && conf.win_x < b.x + b.w - 100 && conf.win_y >= b.y - 10 &&
          conf.win_y < b.y + b.h - 100) {
        SDL_SetWindowPosition(app.win, conf.win_x, conf.win_y);
        break;
      }
    }
  }
  if (conf.win_max) SDL_MaximizeWindow(app.win);
#endif
}

static void track_window(void) {
#ifndef FM_MOBILE
  if (g_shot) return;
  Uint32 fl = SDL_GetWindowFlags(app.win);
  conf.win_max = (fl & SDL_WINDOW_MAXIMIZED) != 0;
  if (fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) return;
  SDL_GetWindowSize(app.win, &conf.win_w, &conf.win_h);
  SDL_GetWindowPosition(app.win, &conf.win_x, &conf.win_y);
#endif
}

/* Old cached archive entries are deleted in the background at start. */
static void prune_view_cache(void) {
  char cache[FM_PATH_MAX], view[FM_PATH_MAX], old[FM_PATH_MAX], name[64];
  if (!plat_place(PLACE_CACHE, cache, sizeof cache)) return;
  FmDir *d = plat_dir_open(cache, NULL);
  if (!d) return;
  const char *n;
  char *list[8];
  int nl = 0;
  while (plat_dir_next(d, &n, NULL) && nl < 8)
    if (strncmp(n, "view-old-", 9) == 0 && fm_path_join(old, sizeof old, cache, n)) list[nl++] = fm_strdup(old);
  plat_dir_close(d);
  if (fm_path_join(view, sizeof view, cache, "view") && plat_is_dir(view) && nl < 8) {
    fm_snprintf(name, sizeof name, "view-old-%llu", (unsigned long long)plat_time_unix());
    if (fm_path_join(old, sizeof old, cache, name) && plat_rename(view, old) == FM_OK) list[nl++] = fm_strdup(old);
  }
  if (nl > 0) {
    FmJobSpec s;
    memset(&s, 0, sizeof s);
    s.kind = JOB_DELETE;
    s.srcs = (const char *const *)list;
    s.nsrc = nl;
    s.quiet = true;
    ops_start(&s);
  }
  for (int i = 0; i < nl; i++) fm_free(list[i]);
}

static void demo_select(FmPanel *p) {
  int picked = 0;
  for (int i = 0; i < p->nview && picked < 3; i++) {
    FmEntry *e = &p->list.items[p->view[i]];
    if (i % 2 == 1 || (picked == 0 && i > 2)) { e->selected = 1; picked++; p->cursor = i; }
  }
  panel_update_sel(p);
}

static void select_first_file(FmPanel *p) {
  for (int i = 0; i < p->nview; i++) {
    if (!(p->list.items[p->view[i]].flags & FM_ST_DIR)) {
      panel_select_all(p, false);
      p->list.items[p->view[i]].selected = 1;
      p->cursor = i;
      panel_update_sel(p);
      return;
    }
  }
  if (p->nview > 0) {
    p->list.items[p->view[0]].selected = 1;
    p->cursor = 0;
    panel_update_sel(p);
  }
}

static void demo_dialog(const char *name) {
  FmPanel *p = P();
  if (!strcmp(name, "rename")) { select_first_file(p); open_rename(); }
  else if (!strcmp(name, "newfolder")) open_new(false);
  else if (!strcmp(name, "newfile")) open_new(true);
  else if (!strcmp(name, "delete")) { if (!p->nsel) demo_select(p); open_delete(false); }
  else if (!strcmp(name, "props")) { select_first_file(p); open_props(false); }
  else if (!strcmp(name, "folderprops")) open_props(true);
  else if (!strcmp(name, "compress")) { if (!p->nsel) demo_select(p); open_compress(); }
  else if (!strcmp(name, "extract")) {
    if (!p->list.loc.in_arc) {
      panel_select_all(p, false);
      for (int i = 0; i < p->nview; i++)
        if (is_archive_entry(&p->list.items[p->view[i]])) { p->list.items[p->view[i]].selected = 1; break; }
      panel_update_sel(p);
    }
    open_extract();
  }
  else if (!strcmp(name, "settings")) open_settings();
  else if (!strcmp(name, "exit")) open_exit();
  else if (!strcmp(name, "conflict") || !strcmp(name, "password")) {
    bool pw = !strcmp(name, "password");
    dlg_open(pw ? DLG_PASSWORD : DLG_CONFLICT, g_active);
    D.ask.kind = pw ? ASK_PASSWORD : ASK_CONFLICT;
    fm_path_join(D.ask.src, sizeof D.ask.src, P()->list.loc.path, pw ? "Holiday photos.zip" : "Report 2026.pdf");
    fm_path_join(D.ask.dst, sizeof D.ask.dst, O()->list.loc.path, "Report 2026.pdf");
    D.ask.src_known = true;
    D.ask.src_st.size = 2400000;
    D.ask.src_st.mtime = plat_time_unix() - 3600;
    D.ask.dst_st.size = 1830000;
    D.ask.dst_st.mtime = plat_time_unix() - 86400 * 9;
    D.ask.retry = pw;
  } else if (!strcmp(name, "places")) {
    app_panel_places(p, p->rect.x + p->rect.w - DP(60), p->rect.y + ui.m.bar_h);
  } else if (!strcmp(name, "menu")) {
    app_panel_menu(p, p->rect.x + p->rect.w - DP(30), p->rect.y + ui.m.bar_h);
  } else if (!strcmp(name, "context")) {
    select_first_file(p);
    app_panel_context(p, p->rect.x + p->rect.w * 0.4f, p->rect.y + ui.m.bar_h + DP(60));
  } else if (!strcmp(name, "search")) {
    panel_open_search(p);
  }
}

static const char *g_demo_dialog;
static int g_demo_frames;

void app_init(void) {
  ID_CTX = ui_id("menu.ctx");
  ID_PMENU = ui_id("menu.panel");
  ID_PLACES = ui_id("menu.places");
  ID_JOBMENU = ui_id("menu.job");
  memset(&D, 0, sizeof D);
  D.pw_panel = -1;
  g_shot = arg_flag("--shot");
  g_force_touch = arg_flag("--touch");
  if (g_shot) conf_defaults();
  else conf_load();
  if (arg_flag("--light")) conf.dark = false;
  if (arg_flag("--dark")) conf.dark = true;
  const char *th = arg_value("--theme");
  if (th && theme_find(th) >= 0) {
    conf.theme = theme_find(th);
    if (!theme_has_both(conf.theme) && !arg_flag("--light") && !arg_flag("--dark"))
      conf.dark = theme_variant(conf.theme, true)->dark;
  }
  if (arg_flag("--grid")) conf.view[0] = conf.view[1] = VIEW_GRID;
  const char *lay = arg_value("--layout");
  if (lay) conf.layout = !strcmp(lay, "side") ? LAYOUT_SIDE : !strcmp(lay, "stack") ? LAYOUT_STACK :
                         !strcmp(lay, "single") ? LAYOUT_SINGLE : LAYOUT_AUTO;
  theme_apply(conf.theme, conf.dark, conf.accent);
  ui.zoom = conf.zoom;
  apply_touch();
  thumb_init();
  lib_init(g_shot);                    /* flib: --shot runs never save */
  lib_ui_set_reveal(lib_reveal);
  online_init(g_shot);                 /* online */
  {
    FmOnlineHooks oh = { conf_dirty, open_settings, online_reveal_path };
    online_set_hooks(&oh);
  }
  photo_init(g_shot);                  /* photos */
  {
    FmPhotoHooks ph = { conf_dirty, open_settings, photo_reveal_path };
    photo_set_hooks(&ph);
  }
  aonline_init(g_shot);                /* audio */
  {
    FmAonlineHooks ah = { conf_dirty, open_settings, aonline_reveal_path };
    aonline_set_hooks(&ah);
  }
  if (!g_shot) restore_window();
  title_apply();

  char path[FM_PATH_MAX];
  for (int i = 0; i < 2; i++) {
    const char *arg = arg_value(i == 0 ? "--left" : "--right");
    if (!arg && i == 0) arg = arg_positional();
    default_path(i, path, sizeof path);
    if (arg && plat_is_dir(arg)) fm_strlcpy(path, arg, sizeof path);
    panel_init(&g_p[i], i, path);
    /* a file: open its folder with it selected, or browse into an archive */
    if (arg && !plat_is_dir(arg) && plat_exists(arg)) panel_go_path(&g_p[i], arg);
  }
  g_active = FM_CLAMP(conf.active, 0, 1);
  g_single_tab = g_active;
  update_title();
  if (!g_shot) prune_view_cache();
  if (arg_flag("--demo-select")) demo_select(P());
  if (arg_flag("--demo-job")) ops_demo();
  g_demo_dialog = arg_value("--demo-dialog");
  g_demo_frames = 0;
  /* flib: --demo-library SECTION shows the library of the --left folder */
  const char *dl = arg_value("--demo-library");
  if (dl) lib_demo(g_p[0].list.loc.path, FM_MAX(0, lib_ui_section(dl)));
  /* online: --demo-online SOURCE [QUERY] [--demo-online-state STATE] */
  const char *don = arg_value("--demo-online");
  if (don) {
    const char *q = NULL;
    for (int i = 1; i + 2 < app.argc; i++)
      if (!strcmp(app.argv[i], "--demo-online") && app.argv[i + 2][0] != '-') q = app.argv[i + 2];
    online_demo(don, q, arg_value("--demo-online-state"));
  }
  /* photos: --demo-photos SOURCE [QUERY] [--demo-photos-state STATE] */
  const char *dph = arg_value("--demo-photos");
  if (dph) {
    const char *q = NULL;
    for (int i = 1; i + 2 < app.argc; i++)
      if (!strcmp(app.argv[i], "--demo-photos") && app.argv[i + 2][0] != '-') q = app.argv[i + 2];
    photo_demo(dph, q, arg_value("--demo-photos-state"));
  }
  /* audio: --demo-audio-online SOURCE [QUERY] [--demo-audio-online-state STATE] */
  const char *dau = arg_value("--demo-audio-online");
  if (dau) {
    const char *q = NULL;
    for (int i = 1; i + 2 < app.argc; i++)
      if (!strcmp(app.argv[i], "--demo-audio-online") && app.argv[i + 2][0] != '-') q = app.argv[i + 2];
    aonline_demo(dau, q, arg_value("--demo-audio-online-state"));
  }
  ui_redraw();
}

void app_shutdown(void) {
  app_close_viewer();
  ops_shutdown();
  if (!g_shot) {
    track_window();
    for (int i = 0; i < 2; i++) {
      if (!g_p[i].list.loc.in_arc && g_p[i].list.err == FM_OK)
        fm_strlcpy(conf.path[i], g_p[i].list.loc.path, FM_PATH_MAX);
    }
    conf.active = g_active;
    conf_save();
    g_conf_dirty = false;
  }
  dlg_free_paths();
  clip_free();
  panel_free(&g_p[0]);
  panel_free(&g_p[1]);
  lib_shutdown();                      /* flib */
  aonline_shutdown();                  /* audio */
  photo_shutdown();                    /* photos */
  online_shutdown();                   /* online */
  thumb_shutdown();
}

bool app_can_quit(void) {
  /* file jobs and online downloads both ask first */
  if (ops_running(false) > 0 || online_downloads_active() > 0 || photo_downloads_active() > 0 ||
      aonline_downloads_active() > 0) {   /* photos, audio */
    open_exit();
    return false;
  }
  return true;
}

void app_event(const SDL_Event *e) {
  if (e->type == app.ev_wake) {
    ui_redraw();
    return;
  }
  switch (e->type) {
    case SDL_WINDOWEVENT:
      switch (e->window.event) {
        case SDL_WINDOWEVENT_MOVED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
          if (!g_shot) {
            track_window();
            conf_dirty();
          }
          break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
          for (int i = 0; i < 2; i++)
            if (panel_changed_outside(&g_p[i])) panel_refresh(&g_p[i]);
          g_vols_dirty = g_places_dirty = g_space_dirty = true;
          break;
        default: break;
      }
      break;
    case SDL_APP_DIDENTERFOREGROUND:
      /* back from the system settings (storage permission) */
      panel_refresh(&g_p[0]);
      panel_refresh(&g_p[1]);
      g_vols_dirty = g_places_dirty = true;
      break;
    case SDL_APP_WILLENTERBACKGROUND:
      conf_flush(true);
      break;
    default: break;
  }
}

/* ---- frame -------------------------------------------------------------- */

void app_frame(void) {
  thumb_pump();
  lib_pump();                          /* flib: scan results, saving */
  online_pump();                       /* online: searches, downloads, thumbnails */
  photo_pump();                        /* photos: searches, full pictures, downloads, albums */
  aonline_pump();                      /* audio: searches, episodes, downloads, the library */
  FmJob *fj;
  while ((fj = ops_take_finished()) != NULL) job_finished(fj);
  conf_flush(false);
  menu_results();

  if (app.viewer) {
    FmRect full = { 0, 0, ui.w, ui.h };
    int prev = ui_push_layer(UI_LAYER_SHEET);
    title_bar(FM_RECT(0, 0, 0, 0));    /* viewers own the top edge: no drag area */
    app.viewer->frame(full);
    if (app.viewer && (ui_key(SDLK_ESCAPE, 0) || ui_key(SDLK_AC_BACK, 0))) app_close_viewer();
    ui_pop_layer(prev);
    draw_dialogs();
    return;
  }
  /* flib: the media library takes the window, under the viewers */
  if (lib_ui_is_open()) {
    lib_ui_frame(FM_RECT(0, 0, ui.w, ui.h));
    draw_dialogs();
    title_outline();
    return;
  }
  /* online: the online videos view, also under the viewers */
  if (online_is_open()) {
    online_frame(FM_RECT(0, 0, ui.w, ui.h));
    draw_dialogs();
    title_outline();
    return;
  }
  /* photos: the online photos view, also under the viewers */
  if (photo_is_open()) {
    photo_frame(FM_RECT(0, 0, ui.w, ui.h));
    draw_dialogs();
    title_outline();
    return;
  }
  /* audio: the online audio view, also under the viewers */
  if (aonline_is_open()) {
    aonline_frame(FM_RECT(0, 0, ui.w, ui.h));
    draw_dialogs();
    title_outline();
    return;
  }

  float jh_target = jobs_target_h();
  float jh = ui_anim(ui_id("jobs.h"), jh_target, 14.0f);
  if (jh < 1.0f && jh_target == 0) jh = 0;
  layout_compute(&g_L, g_single_tab, g_sidebar_open, jh, audio_mini_active());
  if (g_L.mode == LAYOUT_SINGLE && g_active != g_single_tab) g_single_tab = g_active;

  theme_draw_bg(FM_RECT(0, 0, ui.w, ui.h));
  draw_top(g_L.top);
  if (g_L.sidebar) draw_sidebar(g_L.side);
  if (g_L.mode == LAYOUT_SINGLE) draw_tabs(g_L.tabs);
  for (int i = 0; i < 2; i++)
    if (g_L.show[i]) panel_frame(&g_p[i], g_L.panel[i], i == g_active);
  draw_action_bar(g_L.action, g_L.action_vertical);
  if (jh > 0.5f) draw_jobs(g_L.jobs);
  if (g_L.audio.h > 0) audio_mini_draw(rect_inset2(g_L.audio, DP(8), DP(4)));
  ui_divider(0, ui.w, g_L.status.y);
  draw_status(g_L.status);
  if (D.kind == DLG_NONE && !ui_menu_is_open()) global_keys();
  draw_drag();
  draw_dialogs();

  title_outline();
  if (g_demo_dialog && ++g_demo_frames == 2) {
    demo_dialog(g_demo_dialog);
    g_demo_dialog = NULL;
  }
}
