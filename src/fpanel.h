/* fpanel.h -- one file panel: location, listing, history, selection, drawing.
**
** The app owns two panels. A panel lists its location through fvfs, keeps
** a sorted and filtered view of the entries, and draws itself as a card:
** header (back, up, breadcrumb path, bookmark, places, menu), an optional
** search row, the list or grid, and a footer with the selection summary.
**
** Design decisions:
**   - The view is an array of indices into the listing, rebuilt only when
**     the folder, the sort or the filter changes; drawing touches only the
**     rows on screen, so 10 000 entries cost the same as 20.
**   - Selection lives in FmEntry.selected, so it survives re-sorting; a
**     refresh restores it by name.
**   - What a panel cannot decide alone (opening menus, starting a drag,
**     asking for a password) goes to the app through the app_panel_*
**     functions declared at the end, implemented in fapp.c.
*/
#ifndef FPANEL_H
#define FPANEL_H

#include "fui.h"
#include "fvfs.h"

#define PANEL_HIST 32
#define PANEL_SEGS 48

typedef struct FmPanel {
  int idx;                       /* 0 = left/top, 1 = right/bottom */
  FmListing list;
  int *view;                     /* list.items indices, sorted and filtered */
  int nview, view_cap;
  int ndirs, nfiles;             /* in the view */
  int cursor, anchor;            /* view positions (-1 none) */
  int nsel;
  u64 sel_bytes;
  bool select_mode;              /* touch: checkboxes, tap toggles */
  FmScroll scroll;
  char *back[PANEL_HIST];        /* history, encoded locations */
  int nback;
  char *fwd[PANEL_HIST];
  int nfwd;
  char filter[128];
  bool search_open;
  bool editing_path;
  char path_edit[FM_PATH_MAX];
  float crumb_x, crumb_x0;       /* breadcrumb horizontal scroll, at drag start */
  bool focus_pending;            /* give the path / search field focus next frame */
  char display[FM_PATH_MAX];     /* loc_display of the location */
  int nsegs;
  u16 seg_end[PANEL_SEGS];       /* breadcrumb segments: end offsets in display */
  char seg_label[PANEL_SEGS][48];
  i64 dir_mtime;                 /* to notice outside changes on focus */
  int pending_single;            /* view pos to select alone on click release */
  bool dragging;
  FmRect rect, body;             /* last frame */
  float row_h, tile_w, tile_h;   /* last frame geometry, for keyboard scrolling */
  int cols;                      /* grid columns (1 in list view) */
  u64 nav_t;                     /* time of the last navigation (fade-in) */
} FmPanel;

void panel_init(FmPanel *p, int idx, const char *path);
void panel_free(FmPanel *p);

/* Navigation. panel_go keeps the current folder in the back history. */
bool panel_go(FmPanel *p, const FmLoc *loc, bool push_history);
/* A local path: a folder opens; a file opens its folder with it selected. */
bool panel_go_path(FmPanel *p, const char *path);
bool panel_up(FmPanel *p);
bool panel_back(FmPanel *p);
bool panel_forward(FmPanel *p);
void panel_refresh(FmPanel *p);      /* relist, keeping selection and scroll */
void panel_resort(FmPanel *p);       /* after a sort, hidden or filter change */
bool panel_changed_outside(FmPanel *p);
void panel_set_filter(FmPanel *p, const char *f);
void panel_edit_path(FmPanel *p);    /* Ctrl+L */
void panel_open_search(FmPanel *p);

/* Selection. */
void panel_select_all(FmPanel *p, bool on);
void panel_select_name(FmPanel *p, const char *name);   /* alone, cursor on it */
void panel_update_sel(FmPanel *p);
FmEntry *panel_cursor_entry(FmPanel *p);
/* Selected entries as list.items indices; returns the count (caller fm_frees). */
int  panel_selected(FmPanel *p, int **items);
/* Local absolute paths, or names inside the archive (folders without '/').
** Free with panel_free_paths. */
char **panel_selected_paths(FmPanel *p, int *n);
void panel_free_paths(char **paths, int n);
bool panel_is_local(const FmPanel *p);   /* a readable local folder */

/* Opens an entry: folder, archive, or a file (viewer / system app). */
void panel_open_item(FmPanel *p, int item);

/* Draws and handles input; `active` = keyboard focus panel. */
void panel_frame(FmPanel *p, FmRect r, bool active);

/* ---- provided by fapp.c ------------------------------------------------- */

void app_panel_activate(int idx);
void app_panel_context(FmPanel *p, float x, float y);   /* right click / long press menu */
void app_panel_menu(FmPanel *p, float x, float y);      /* the header menu button */
void app_panel_places(FmPanel *p, float x, float y);
void app_panel_drag_start(FmPanel *p);
void app_panel_password(FmPanel *p, const FmLoc *loc, bool retry);
void app_panel_navigated(FmPanel *p);                   /* history, bookmarks, conf */
void app_open_job(FmPanel *p, int item);                /* big archive entry: open via a job */

#endif
