/* fcloud_ui.c -- cloud storage in the UI: the "Cloud storage" sheet, adding
** and signing in to accounts, the account menu, the settings section, and
** the panel operations on cloud folders.
**
** Design decisions:
**   - The sheets are this module's own modal dialogs (one at a time, state in
**     U), drawn from fapp's draw_dialogs; job questions wait while one is
**     open. Each sheet measures and draws with the same code, so its height
**     always matches.
**   - Adding an account adds it as a temporary one first and signs in on a
**     worker (OAuth opens the browser there); it becomes a saved account
**     only when the sign-in worked, so a mistyped password never leaves a
**     broken entry behind. Cancel stops the sign-in.
**   - Services whose adapter is still a stub (no list()) are shown with
**     "Coming soon" and cannot be picked.
**   - Opening a cloud file downloads it into PLACE_CACHE/cloud with a job
**     card (a copy of the same size is reused), then opens it like a local
**     file. Audio and video from services with CLOUD_STREAM play from the
**     stream link instead; video only when the link needs no extra headers
**     (the video player cannot send them), else it is downloaded too.
**   - Rename and new folder are small tasks; the panel refreshes when they
**     finish and selects the result.
*/
#include "fcloud_app.h"
#include "fpanel.h"
#include "fapp.h"
#include "fconf.h"
#include "fview.h"

enum { U_NONE, U_HOME, U_PICK, U_FORM, U_LINK, U_LABEL, U_REMOVE, U_DELETE };

typedef struct UState {
  int kind;
  FmScroll scroll;
  /* form */
  const FmCloud *svc;
  int serial;                  /* the account being signed in (temporary while adding) */
  bool editing;                /* signing in to an existing account */
  bool busy;                   /* the sign-in task runs */
  bool show_pw;
  char label[64], user[256], secret[512], server[512], client_id[256], client_secret[256];
  char endpoint[256], region[64], bucket[192];
  char link[1024];
  int link_svc;
  char err[256];
  /* delete */
  int panel;
  FmCloudEntry *ents;
  int nents;
  FmLoc loc;
} UState;

static UState U;
static FmPanel *g_panels;
static FmCloudHooks g_hooks;
static u32 ID_ACCT;
static int g_menu_serial;

enum { AM_OPEN = 1, AM_RENAME, AM_SIGNIN, AM_SIGNOUT, AM_REMOVE };

void cloud_ui_bind(FmPanel *panels, const FmCloudHooks *h) {
  g_panels = panels;
  if (h) g_hooks = *h;
  ID_ACCT = ui_id("cloud.acctmenu");
}

bool cloud_ui_is_open(void) { return U.kind != U_NONE; }

static void close_ui(void) {
  if (U.busy && U.serial > 0) cloud_task_cancel(CT_LOGIN, U.serial);
  if (!U.editing && U.serial > 0) {
    int i = cloud_acct_index(U.serial);
    if (i >= 0 && cloud_acct_is_temp(i)) cloud_acct_remove(i);
  }
  fm_free(U.ents);
  memset(U.secret, 0, sizeof U.secret);
  memset(U.client_secret, 0, sizeof U.client_secret);
  memset(&U, 0, sizeof U);
  ui_focus(0);
  ui_redraw();
}

static void open_ui(int kind) {
  if (U.kind != U_NONE) close_ui();
  U.kind = kind;
  ui_redraw();
}

static int active_panel(void) { return g_hooks.active ? g_hooks.active() : 0; }

static const FmCloud *svc_of(int serial) {
  const FmCloudAcct *a = cloud_acct_by_serial(serial);
  return a ? cloud_find(a->provider) : NULL;
}

FmIcon cloud_ui_icon(int serial) {
  const FmCloud *c = svc_of(serial);
  return (c && c->icon != IC_NONE && c->icon != IC_LINK) ? c->icon : IC_CLOUD;
}

static FmIcon svc_icon(const FmCloud *c) {
  return (c && c->icon != IC_NONE && c->icon != IC_LINK) ? c->icon : IC_CLOUD;
}

/* The adapter's line for the sheets, without a stub's "FLAGS|" prefix. */
static const char *about_of(const FmCloud *c) {
  if (!c || !c->about) return "";
  const char *bar = strstr(c->about, "CLOUD_") ? strrchr(c->about, '|') : NULL;
  return bar ? bar + 1 : c->about;
}

static bool needs_session(const FmCloud *c) { return c && (c->flags & CLOUD_OAUTH); }

/* ---- small layout helpers ------------------------------------------------- */

static float btn_h(void) { return DP(ui.touch_mode ? 46 : 38); }
static float fld_h(void) { return DP(ui.touch_mode ? 48 : 40); }
static float lh(void) { return font_line_h(ui.m.font); }
static float ls(void) { return font_line_h(ui.m.font_small); }
static float row_h(void) { return DP(ui.touch_mode ? 64 : 56); }

static float dlg_cw(float w_dp) {
  float w = (ui.portrait && ui.w < DP(560)) ? ui.w : FM_MIN(DP(w_dp), ui.w - DP(24));
  return w - DP(40);
}

/* Dialog height (dp) for content px under the title, at most the window. */
static float dlg_h(float content) {
  float h = (content + DP(40) + font_line_h(ui.m.font_title) + DP(12)) / ui.scale + 2;
  return FM_MIN(h, ui.h / ui.scale - 32);
}

static float btn_w(const char *l) { return font_width(FONT_BOLD, ui.m.font, l, -1) + DP(36); }

/* A row of buttons at the bottom right; the last is the primary one. */
static int buttons(u32 base, FmRect c, const char *const *labels, int n, int primary_style, int disabled_mask) {
  float bh = btn_h(), x = c.x + c.w, y = c.y + c.h - bh;
  float total = 0;
  for (int i = 0; i < n; i++) total += btn_w(labels[i]) + (i ? DP(8) : 0);
  bool stack = total > c.w;
  if (stack) y = c.y + c.h - (float)n * (bh + DP(8)) + DP(8);
  int res = -1;
  for (int i = n - 1; i >= 0; i--) {
    float bw = stack ? c.w : btn_w(labels[i]);
    if (!stack) x -= bw;
    FmRect r = stack ? FM_RECT(c.x, y, bw, bh) : FM_RECT(x, y, bw, bh);
    int st = i == n - 1 ? primary_style : (stack ? UI_BTN_TONAL : UI_BTN_TEXT);
    if (disabled_mask & (1 << i)) {
      gfx_rrect(r, bh * 0.5f, col_alpha(T.text3, 0.12f));
      font_draw_center(FONT_BOLD, ui.m.font, r, labels[i], T.text3);
    } else if (ui_button(ui_idn(base, (u32)i), r, IC_NONE, labels[i], st)) {
      res = i;
    }
    if (stack) y += bh + DP(8);
    else x -= DP(8);
  }
  return res;
}

static float buttons_h(float w, const char *const *labels, int n) {
  float total = 0;
  for (int i = 0; i < n; i++) total += btn_w(labels[i]) + (i ? DP(8) : 0);
  return total > w ? (float)n * (btn_h() + DP(8)) - DP(8) : btn_h();
}

static void open_url(const char *url) {
  if (SDL_OpenURL(url) != 0 && !plat_open_external(url)) ui_toast("No browser found");
}

/* A small heading in the sheets and the settings. */
static void heading(FmRect *r, const char *s, bool draw) {
  FmRect h = rect_cut_top(r, ls() + DP(6));
  if (draw) font_draw(FONT_BOLD, ui.m.font_small, h.x + DP(2), h.y, s, -1, T.text2);
}

static float wrap(FmRect *r, const char *s, FmColor c, bool draw) {
  float h = font_draw_wrap(FONT_REGULAR, ui.m.font_small, r->x + DP(2), r->y, r->w - DP(4), s, c, draw);
  rect_cut_top(r, h + DP(4));
  return h;
}

/* A labelled text field; returns UI_TF_* bits. */
static int field(FmRect *r, u32 id, const char *label, char *buf, int cap, const char *hint, int flags, bool draw,
                 bool eye) {
  heading(r, label, draw);
  FmRect f = rect_cut_top(r, fld_h());
  rect_cut_top(r, DP(10));
  if (!draw) return 0;
  if (eye) {
    FmRect e = rect_cut_right(&f, f.h);
    if (ui_icon_btn(ui_idn(id, 1), e, U.show_pw ? IC_EYE_OFF : IC_EYE, T.text2, U.show_pw ? "Hide" : "Show"))
      U.show_pw = !U.show_pw;
    if (!U.show_pw) flags |= UI_TF_PASSWORD;
  }
  return ui_textfield(id, f, buf, cap, hint, flags);
}

/* One account as a row: icon, label, service and user, a menu button. */
static bool acct_row(FmRect *r, u32 id, int i, bool draw, bool menu) {
  FmRect row = rect_cut_top(r, row_h());
  if (!draw || !gfx_visible(row)) return false;
  const FmCloudAcct *a = cloud_acct_at(i);
  const FmCloud *c = cloud_find(a->provider);
  int serial = cloud_acct_serial(i);
  FmRect mb = rect_cut_right(&row, DP(ui.touch_mode ? 48 : 40));
  int f = ui_hit(id, row);
  if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
  if (f & UI_HELD) gfx_rrect(row, DP(12), T.press);
  float is = DP(38);
  FmRect ic = { row.x + DP(8), row.y + (row.h - is) * 0.5f, is, is };
  gfx_circle(ic.x + is * 0.5f, ic.y + is * 0.5f, is * 0.5f, T.accent_soft);
  icon_draw(cloud_ui_icon(serial), rect_inset(ic, DP(9)), T.accent);
  float x = ic.x + is + DP(12), w = row.x + row.w - x - DP(4);
  float y = row.y + (row.h - lh() - ls() - DP(2)) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, x, y, a->label, w, T.text);
  char sub[400];
  bool out = needs_session(c) && !a->session[0];
  fm_snprintf(sub, sizeof sub, "%s%s%s%s%s", c ? c->name : a->provider, a->user[0] ? "  \xC2\xB7  " : "", a->user,
              cloud_acct_is_temp(i) ? "  \xC2\xB7  not saved" : "", out ? "  \xC2\xB7  signed out" : "");
  font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + lh() + DP(2), sub, w, out ? T.warn : T.text2);
  if (menu && ui_icon_btn(ui_idn(id, 7), rect_center(mb, DP(36), DP(36)), IC_MORE, T.text2, "Account")) {
    g_menu_serial = serial;
    FmMenuItem m[6];
    int n = 0;
    memset(m, 0, sizeof m);
    m[n].id = AM_OPEN; m[n].icon = IC_FOLDER_OPEN; m[n++].label = "Open";
    m[n].id = AM_RENAME; m[n].icon = IC_RENAME; m[n++].label = "Rename";
    if (c && c->login) { m[n].id = AM_SIGNIN; m[n].icon = IC_KEY; m[n++].label = "Sign in again"; }
    if (a->session[0] || a->secret[0]) { m[n].id = AM_SIGNOUT; m[n].icon = IC_LOCK; m[n++].label = "Sign out"; }
    m[n].id = AM_REMOVE; m[n].icon = IC_DELETE; m[n].flags = UI_MI_DANGER;
    m[n++].label = cloud_acct_is_temp(i) ? "Close" : "Remove";
    ui_menu_open(ID_ACCT, mb.x, mb.y + mb.h, m, n);
  }
  return (f & UI_CLICK) != 0;
}

/* ---- going to an account -------------------------------------------------- */

void cloud_ui_go(int serial, int panel) {
  if (!g_panels || cloud_acct_index(serial) < 0) return;
  FmLoc l;
  cloud_loc_root(&l, serial);
  if (g_hooks.activate) g_hooks.activate(panel);
  panel_go(&g_panels[panel], &l, true);
}

/* ---- the "Cloud storage" sheet -------------------------------------------- */

static bool any_links(void) {
  for (int i = 0; i < cloud_count(); i++)
    if ((cloud_at(i)->flags & CLOUD_LINKS) && cloud_at(i)->open_link) return true;
  return false;
}

void cloud_ui_home(void) { open_ui(U_HOME); }

static float home_layout(FmRect *r, u32 base, bool draw, int *go) {
  float y0 = r->y;
  int n = cloud_acct_count();
  if (n == 0) {
    FmRect top = rect_cut_top(r, DP(64));
    if (draw) {
      gfx_circle(top.x + top.w * 0.5f, top.y + DP(30), DP(28), col_alpha(T.accent, 0.12f));
      icon_draw(IC_CLOUD, FM_RECT(top.x + top.w * 0.5f - DP(16), top.y + DP(14), DP(32), DP(32)), T.accent);
    }
    FmRect t = rect_cut_top(r, font_line_h(ui.m.font_title) + DP(6));
    if (draw) ui_label(t, "No cloud accounts yet", FONT_BOLD, ui.m.font_title, T.text, UI_CENTER);
    float h = font_draw_wrap(FONT_REGULAR, ui.m.font, r->x, r->y, r->w,
                             "Add Google Drive, Dropbox, OneDrive, MEGA, WebDAV or S3 storage and use it like a "
                             "folder: copy and move files between it and this device.", T.text2, draw);
    rect_cut_top(r, h + DP(8));
  }
  for (int i = 0; i < n; i++) {
    if (acct_row(r, ui_idn(base, 10 + (u32)i), i, draw, true) && go) *go = cloud_acct_serial(i);
    rect_cut_top(r, DP(2));
  }
  return r->y - y0;
}

static void draw_home(void) {
  const char *labels2[] = { "Close", "Open a link", "Add account" };
  const char *labels1[] = { "Close", "Add account" };
  bool links = any_links();
  const char *const *labels = links ? labels2 : labels1;
  int nl = links ? 3 : 2;
  float cw = dlg_cw(500);
  FmRect m = { 0, 0, cw, 100000 };
  float content = home_layout(&m, 0, false, NULL);
  float bh = buttons_h(cw, labels, nl);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.home"), "Cloud storage", 500, dlg_h(content + DP(14) + bh), &cancel);
  FmRect btns = rect_cut_bottom(&c, bh);
  rect_cut_bottom(&c, DP(14));
  ui_scroll(&U.scroll, ui_id("cloud.home.scroll"), c, content);
  gfx_clip_push(c);
  FmRect r = { c.x, c.y - U.scroll.y, c.w, content };
  int go = 0;
  home_layout(&r, ui_id("cloud.home.w"), true, &go);
  gfx_clip_pop();
  ui_scrollbar(&U.scroll, c, content);
  int b = buttons(ui_id("cloud.home.b"), btns, labels, nl, UI_BTN_FILLED, 0);
  ui_dialog_end();
  if (go) {
    int panel = active_panel();
    close_ui();
    cloud_ui_go(go, panel);
    return;
  }
  if (cancel || b == 0) close_ui();
  else if (b == nl - 1) open_ui(U_PICK);
  else if (links && b == 1) open_ui(U_LINK);
}

/* ---- picking a service ---------------------------------------------------- */

void cloud_ui_add(void) { open_ui(U_PICK); }

static void start_form(const FmCloud *c);

static float pick_layout(FmRect *r, u32 base, bool draw, const FmCloud **pick) {
  float y0 = r->y;
  for (int i = 0; i < cloud_count(); i++) {
    const FmCloud *c = cloud_at(i);
    bool ok = cloud_available(c);
    /* the first sentence here; the sign-in form shows all of it */
    char about[200];
    fm_strlcpy(about, about_of(c), sizeof about);
    char *dot = strstr(about, ". ");
    if (dot) dot[1] = 0;
    float tw = r->w - DP(70) - (ok ? 0 : DP(100));
    float ah = font_draw_wrap(FONT_REGULAR, ui.m.font_small, 0, 0, tw, about, T.text2, false);
    float h = FM_MAX(row_h(), lh() + ah + DP(18));
    FmRect row = rect_cut_top(r, h);
    rect_cut_top(r, DP(2));
    if (!draw || !gfx_visible(row)) continue;
    int f = ok ? ui_hit(ui_idn(base, (u32)i), row) : 0;
    if (f & UI_HOVER) gfx_rrect(row, DP(12), T.hover);
    if (f & UI_HELD) gfx_rrect(row, DP(12), T.press);
    float is = DP(38);
    FmRect ic = { row.x + DP(8), row.y + DP(9), is, is };
    gfx_circle(ic.x + is * 0.5f, ic.y + is * 0.5f, is * 0.5f, ok ? T.accent_soft : T.surface3);
    icon_draw(svc_icon(c), rect_inset(ic, DP(9)), ok ? T.accent : T.text3);
    float x = ic.x + is + DP(12);
    font_draw(FONT_BOLD, ui.m.font, x, row.y + DP(8), c->name, -1, ok ? T.text : T.text3);
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, x, row.y + DP(8) + lh(), tw, about, ok ? T.text2 : T.text3, true);
    if (!ok) {
      const char *cs = "Coming soon";
      float w = font_width(FONT_BOLD, ui.m.font_small, cs, -1) + DP(16);
      FmRect chip = { row.x + row.w - w - DP(8), row.y + DP(10), w, ls() + DP(6) };
      gfx_rrect(chip, chip.h * 0.5f, T.surface3);
      font_draw_center(FONT_BOLD, ui.m.font_small, chip, cs, T.text3);
    } else {
      icon_draw(IC_CHEVRON_RIGHT, FM_RECT(row.x + row.w - DP(26), row.y + (h - DP(16)) * 0.5f, DP(16), DP(16)),
                T.text3);
    }
    if ((f & UI_CLICK) && pick) *pick = c;
  }
  return r->y - y0;
}

static void draw_pick(void) {
  const char *labels[] = { "Back" };
  float cw = dlg_cw(520);
  FmRect m = { 0, 0, cw, 100000 };
  float content = pick_layout(&m, 0, false, NULL);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.pick"), "Add a cloud account", 520, dlg_h(content + DP(14) + btn_h()),
                             &cancel);
  FmRect btns = rect_cut_bottom(&c, btn_h());
  rect_cut_bottom(&c, DP(14));
  ui_scroll(&U.scroll, ui_id("cloud.pick.scroll"), c, content);
  gfx_clip_push(c);
  FmRect r = { c.x, c.y - U.scroll.y, c.w, content };
  const FmCloud *pick = NULL;
  pick_layout(&r, ui_id("cloud.pick.w"), true, &pick);
  gfx_clip_pop();
  ui_scrollbar(&U.scroll, c, content);
  int b = buttons(ui_id("cloud.pick.b"), btns, labels, 1, UI_BTN_TONAL, 0);
  ui_dialog_end();
  if (pick) start_form(pick);
  else if (cancel || b == 0) open_ui(U_HOME);
}

/* ---- the sign-in form ---------------------------------------------------------- */

typedef struct Help { const char *key, *text, *url; } Help;
static const Help kHelp[] = {
  { "gdrive", "Needs your own free OAuth client: in Google Cloud Console enable the Drive API, then create "
              "Credentials > OAuth client ID > Desktop app.", "https://console.cloud.google.com/apis/credentials" },
  { "dropbox", "Needs your own free app: in the Dropbox App Console create a Scoped access app with Full Dropbox "
               "and copy its App key.", "https://www.dropbox.com/developers/apps" },
  { "onedrive", "Needs your own free app: in the Azure portal add an App registration for personal Microsoft "
                "accounts, platform Mobile and desktop, and copy its Application (client) ID.",
    "https://portal.azure.com/#view/Microsoft_AAD_RegisteredApps/ApplicationsListBlade" },
};

static const Help *help_of(const FmCloud *c) {
  for (int i = 0; i < FM_COUNT(kHelp); i++)
    if (!strcmp(kHelp[i].key, c->key)) return &kHelp[i];
  return NULL;
}

static bool is_s3(const FmCloud *c) { return c && (c->flags & CLOUD_KEYS) && (c->flags & CLOUD_SERVER); }

static void split_server(const char *s) {
  char tmp[512];
  fm_strlcpy(tmp, s, sizeof tmp);
  char *p1 = strchr(tmp, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
  if (p1) *p1 = 0;
  if (p2) *p2 = 0;
  fm_strlcpy(U.endpoint, tmp, sizeof U.endpoint);
  fm_strlcpy(U.region, p1 ? p1 + 1 : "", sizeof U.region);
  fm_strlcpy(U.bucket, p2 ? p2 + 1 : "", sizeof U.bucket);
}

static void start_form(const FmCloud *c) {
  open_ui(U_FORM);
  U.svc = c;
  U.serial = 0;
  if (is_s3(c)) fm_strlcpy(U.endpoint, "https://s3.amazonaws.com", sizeof U.endpoint);
}

void cloud_ui_signin(int serial) {
  const FmCloudAcct *a = cloud_acct_by_serial(serial);
  const FmCloud *c = a ? cloud_find(a->provider) : NULL;
  if (!c) return;
  open_ui(U_FORM);
  U.svc = c;
  U.serial = serial;
  U.editing = true;
  fm_strlcpy(U.label, a->label, sizeof U.label);
  fm_strlcpy(U.user, a->user, sizeof U.user);
  fm_strlcpy(U.server, a->server, sizeof U.server);
  fm_strlcpy(U.client_id, a->client_id, sizeof U.client_id);
  fm_strlcpy(U.client_secret, a->client_secret, sizeof U.client_secret);
  if (is_s3(c)) split_server(a->server);
}

static void login_done(FmCloudTask *t) {
  if (U.kind != U_FORM || U.serial != t->serial) return;   /* the sheet was closed */
  U.busy = false;
  if (t->err != FM_OK) {
    fm_strlcpy(U.err, t->err == FM_ERR_CANCEL ? "Cancelled" : t->msg, sizeof U.err);
    ui_redraw();
    return;
  }
  int i = cloud_acct_index(t->serial);
  if (i < 0) return;
  bool added = !U.editing;
  if (added) cloud_acct_keep(i);
  else cloud_acct_save();
  cloud_vfs_forget(t->serial);
  cloud_quota_stale(t->serial);
  int serial = t->serial, panel = active_panel();
  U.serial = 0;                                  /* kept: close_ui must not remove it */
  U.editing = true;
  close_ui();
  ui_toast(added ? "Account added" : "Signed in");
  /* panels on this account show it again */
  for (int k = 0; k < 2 && g_panels; k++)
    if (g_panels[k].list.loc.in_cloud && g_panels[k].list.loc.cloud == serial) panel_refresh(&g_panels[k]);
  if (added) cloud_ui_go(serial, panel);
}

static bool form_fill(FmCloudAcct *a) {
  const FmCloud *c = U.svc;
  memset(a, 0, sizeof *a);
  fm_strlcpy(a->provider, c->key, sizeof a->provider);
  fm_strlcpy(a->label, U.label, sizeof a->label);
  fm_strlcpy(a->user, U.user, sizeof a->user);
  fm_strlcpy(a->secret, U.secret, sizeof a->secret);
  fm_strlcpy(a->client_id, U.client_id, sizeof a->client_id);
  fm_strlcpy(a->client_secret, U.client_secret, sizeof a->client_secret);
  if (is_s3(c)) fm_snprintf(a->server, sizeof a->server, "%s|%s|%s", U.endpoint, U.region, U.bucket);
  else fm_strlcpy(a->server, U.server, sizeof a->server);
  const char *miss = NULL;
  if ((c->flags & CLOUD_SERVER) && !is_s3(c) && !U.server[0]) miss = "Enter the server address";
  if (is_s3(c) && !U.bucket[0]) miss = "Enter the bucket name";
  if ((c->flags & (CLOUD_PASSWORD | CLOUD_KEYS)) && !U.user[0])
    miss = (c->flags & CLOUD_KEYS) ? "Enter the access key ID" : "Enter your e-mail or user name";
  if ((c->flags & (CLOUD_PASSWORD | CLOUD_KEYS)) && !U.secret[0] && !U.editing)
    miss = (c->flags & CLOUD_KEYS) ? "Enter the secret key" : "Enter your password";
  if ((c->flags & CLOUD_OAUTH) && !U.client_id[0]) miss = "Paste your client ID first";
  else if ((c->flags & CLOUD_OAUTH) && !U.client_secret[0] && !strcmp(c->key, "gdrive"))
    miss = "Google also needs the client secret";
  if (miss) fm_strlcpy(U.err, miss, sizeof U.err);
  return miss == NULL;
}

static void form_submit(void) {
  FmCloudAcct a;
  U.err[0] = 0;
  if (!form_fill(&a)) return;
  if (U.editing) {
    FmCloudAcct *live = cloud_acct_by_serial(U.serial);
    if (!live) { close_ui(); return; }
    cloud_acct_lock();
    if (U.label[0]) fm_strlcpy(live->label, a.label, sizeof live->label);
    fm_strlcpy(live->user, a.user, sizeof live->user);
    if (U.secret[0]) fm_strlcpy(live->secret, a.secret, sizeof live->secret);
    fm_strlcpy(live->server, a.server, sizeof live->server);
    fm_strlcpy(live->client_id, a.client_id, sizeof live->client_id);
    fm_strlcpy(live->client_secret, a.client_secret, sizeof live->client_secret);
    cloud_acct_unlock();
  } else {
    if (U.serial > 0) {                          /* a failed try before: start over */
      int i = cloud_acct_index(U.serial);
      if (i >= 0 && cloud_acct_is_temp(i)) cloud_acct_remove(i);
    }
    int i = cloud_acct_add_temp(&a);
    if (i < 0) { fm_strlcpy(U.err, "Too many accounts", sizeof U.err); return; }
    U.serial = cloud_acct_serial(i);
  }
  memset(&a, 0, sizeof a);
  if (!U.svc->login) {                           /* nothing to check: keep it as typed */
    FmCloudTask t;
    memset(&t, 0, sizeof t);
    t.serial = U.serial;
    U.busy = true;
    login_done(&t);
    return;
  }
  FmCloudTask *t = cloud_task_new(CT_LOGIN, U.serial);
  if (!t) return;
  t->done = login_done;
  U.busy = true;
  cloud_task_start(t);
}

static float form_layout(FmRect *r, u32 base, bool draw, bool *submit) {
  const FmCloud *c = U.svc;
  float y0 = r->y;
  int fl = U.busy ? UI_TF_READONLY : 0;
  int res = 0;
  wrap(r, about_of(c), T.text2, draw);
  rect_cut_top(r, DP(6));
  if (c->flags & CLOUD_OAUTH) {
    const Help *h = help_of(c);
    /* adapters that explain the client id in their own line need no second one */
    const char *ab = about_of(c);
    if (!fm_stristr(ab, "client id") && !fm_stristr(ab, "app key"))
      wrap(r, h ? h->text : "Needs your own OAuth client ID from the service's developer site.", T.text3, draw);
    if (h) {
      FmRect lr = rect_cut_top(r, lh() + DP(8));
      if (draw) {
        const char *t = "How to get a client ID";
        float w = font_width(FONT_BOLD, ui.m.font_small, t, -1) + DP(24);
        FmRect b = { lr.x, lr.y, w, lr.h };
        int f = ui_hit(ui_idn(base, 90), b);
        float x = font_draw(FONT_BOLD, ui.m.font_small, b.x + DP(2), b.y + DP(2), t, -1, T.accent);
        icon_draw(IC_OPEN_WITH, FM_RECT(x + DP(4), b.y + DP(3), DP(12), DP(12)), T.accent);
        if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
        if (f & UI_CLICK) open_url(h->url);
      }
    }
    rect_cut_top(r, DP(4));
  }
  res |= field(r, ui_idn(base, 1), "NAME IN THE APP (OPTIONAL)", U.label, sizeof U.label, c->name, fl, draw, false);
  if (is_s3(c)) {
    res |= field(r, ui_idn(base, 2), "ENDPOINT", U.endpoint, sizeof U.endpoint, "https://s3.amazonaws.com", fl, draw,
                 false);
    res |= field(r, ui_idn(base, 3), "REGION", U.region, sizeof U.region, "us-east-1", fl, draw, false);
    res |= field(r, ui_idn(base, 4), "BUCKET", U.bucket, sizeof U.bucket, "my-bucket", fl, draw, false);
  } else if (c->flags & CLOUD_SERVER) {
    res |= field(r, ui_idn(base, 5), "SERVER ADDRESS", U.server, sizeof U.server,
                 "https://cloud.example.com/remote.php/dav/files/me/", fl, draw, false);
  }
  if (c->flags & CLOUD_KEYS) {
    res |= field(r, ui_idn(base, 6), "ACCESS KEY ID", U.user, sizeof U.user, "AKIA...", fl, draw, false);
    res |= field(r, ui_idn(base, 7), "SECRET ACCESS KEY", U.secret, sizeof U.secret,
                 U.editing ? "Unchanged" : "Secret key", fl, draw, true);
  } else if (c->flags & CLOUD_PASSWORD) {
    res |= field(r, ui_idn(base, 8), "E-MAIL OR USER NAME", U.user, sizeof U.user, "you@example.com", fl, draw,
                 false);
    res |= field(r, ui_idn(base, 9), "PASSWORD", U.secret, sizeof U.secret, U.editing ? "Your password" : "Password",
                 fl, draw, true);
  }
  if (c->flags & CLOUD_OAUTH) {
    res |= field(r, ui_idn(base, 10), "CLIENT ID", U.client_id, sizeof U.client_id, "Paste the client ID", fl, draw,
                 false);
    bool need = !strcmp(c->key, "gdrive");     /* Google's desktop clients come with one */
    res |= field(r, ui_idn(base, 11), need ? "CLIENT SECRET" : "CLIENT SECRET (WHEN YOUR CLIENT HAS ONE)",
                 U.client_secret, sizeof U.client_secret, need ? "Paste the client secret" : "Optional", fl, draw, true);
  }
  /* status */
  if (U.busy) {
    FmRect sr = rect_cut_top(r, FM_MAX(lh(), DP(20)) + DP(6));
    if (draw) {
      ui_spinner(FM_RECT(sr.x + DP(2), sr.y + DP(1), DP(18), DP(18)), T.accent);
      font_draw(FONT_REGULAR, ui.m.font, sr.x + DP(28), sr.y, "Signing in\xE2\x80\xA6", -1, T.text);
    }
    if (c->flags & CLOUD_OAUTH)
      wrap(r, "A browser window opened. Finish signing in there, then come back.", T.text2, draw);
  } else if (U.err[0]) {
    FmRect er = rect_cut_top(r, DP(2));
    FM_UNUSED(er);
    float h = font_draw_wrap(FONT_REGULAR, ui.m.font_small, r->x + DP(24), r->y, r->w - DP(26), U.err, T.danger, draw);
    if (draw) icon_draw(IC_WARN, FM_RECT(r->x + DP(2), r->y, DP(16), DP(16)), T.danger);
    rect_cut_top(r, FM_MAX(h, DP(16)) + DP(4));
  }
  if (res & UI_TF_CHANGED) U.err[0] = 0;
  if (submit && (res & UI_TF_SUBMIT)) *submit = true;
  return r->y - y0;
}

static void draw_form(void) {
  const FmCloud *c = U.svc;
  char title[128];
  if (U.editing) fm_snprintf(title, sizeof title, "Sign in to %s", U.label[0] ? U.label : c->name);
  else fm_snprintf(title, sizeof title, "Add %s", c->name);
  const char *labels[] = { U.busy ? "Stop" : U.editing ? "Cancel" : "Back", c->login ? "Sign in" : "Add" };
  float cw = dlg_cw(500);
  FmRect m = { 0, 0, cw - DP(6), 100000 };
  float content = form_layout(&m, 0, false, NULL);
  float bh = buttons_h(cw, labels, 2);
  bool cancel;
  FmRect cr = ui_dialog_begin(ui_id("cloud.form"), title, 500, dlg_h(content + DP(14) + bh), &cancel);
  FmRect btns = rect_cut_bottom(&cr, bh);
  rect_cut_bottom(&cr, DP(14));
  ui_scroll(&U.scroll, ui_id("cloud.form.scroll"), cr, content);
  gfx_clip_push(cr);
  FmRect r = { cr.x, cr.y - U.scroll.y, cr.w - DP(6), content };
  bool submit = false;
  form_layout(&r, ui_id("cloud.form.w"), true, &submit);
  gfx_clip_pop();
  ui_scrollbar(&U.scroll, cr, content);
  int b = buttons(ui_id("cloud.form.b"), btns, labels, 2, UI_BTN_FILLED, U.busy ? 2 : 0);
  ui_dialog_end();
  if (b == 0 && U.busy) {
    cloud_task_cancel(CT_LOGIN, U.serial);
    U.busy = false;
    fm_strlcpy(U.err, "Cancelled", sizeof U.err);
    return;
  }
  if (cancel || b == 0) {
    if (U.editing) close_ui();
    else open_ui(U_PICK);
    return;
  }
  if ((b == 1 || submit) && !U.busy) form_submit();
}

/* ---- opening a public link -------------------------------------------------------- */

static int link_svcs(const FmCloud **out, int max) {
  int n = 0;
  for (int i = 0; i < cloud_count() && n < max; i++)
    if ((cloud_at(i)->flags & CLOUD_LINKS) && cloud_at(i)->open_link) out[n++] = cloud_at(i);
  return n;
}

static void link_done(FmCloudTask *t) {
  if (U.kind != U_LINK) return;
  U.busy = false;
  if (t->err != FM_OK) {
    fm_strlcpy(U.err, t->msg, sizeof U.err);
    return;
  }
  FmCloudAcct a = t->acct;
  fm_strlcpy(a.provider, cloud_find(t->acct.provider) ? t->acct.provider : U.svc->key, sizeof a.provider);
  if (!a.label[0]) fm_strlcpy(a.label, t->entry.name[0] ? t->entry.name : "Shared link", sizeof a.label);
  int i = cloud_acct_add_temp(&a);
  memset(&a, 0, sizeof a);
  if (i < 0) { fm_strlcpy(U.err, "Too many accounts", sizeof U.err); return; }
  int serial = cloud_acct_serial(i), panel = active_panel();
  FmCloudEntry root = t->entry;
  close_ui();
  FmLoc l;
  cloud_loc_root(&l, serial);
  if (root.id[0] && root.dir) cloud_loc_child(&l, root.id, root.name);
  if (g_hooks.activate) g_hooks.activate(panel);
  if (!g_panels) return;
  panel_go(&g_panels[panel], &l, true);
  if (root.id[0] && !root.dir) {
    /* a link to one file: download it like an opened file */
    char out[FM_PATH_MAX];
    if (!cloud_cache_path(serial, &root, out, sizeof out)) return;
    FmCloudJobSpec s;
    memset(&s, 0, sizeof s);
    s.op = CJ_OPEN;
    s.src_loc = &l;
    s.entries = &root;
    s.nentries = 1;
    s.open_path = out;
    if (cloud_job_run(&s, false, NULL) != FM_OK) ui_toast("Too many jobs are running");
  }
}

static void draw_link(void) {
  const FmCloud *svcs[8];
  int ns = link_svcs(svcs, 8);
  if (ns == 0) { close_ui(); return; }
  U.link_svc = FM_CLAMP(U.link_svc, 0, ns - 1);
  U.svc = svcs[U.link_svc];
  const char *labels[] = { U.busy ? "Stop" : "Back", "Open" };
  float cw = dlg_cw(500);
  float ch = (ns > 1 ? DP(44) : 0) + ls() + DP(6) + fld_h() + DP(10) + (U.busy || U.err[0] ? lh() * 2 + DP(8) : 0) +
             DP(14) + buttons_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.link"), "Open a public link", 500, dlg_h(ch), &cancel);
  if (ns > 1) {
    const char *names[8];
    for (int i = 0; i < ns; i++) names[i] = svcs[i]->name;
    ui_segmented(ui_id("cloud.link.svc"), rect_cut_top(&c, DP(34)), names, ns, &U.link_svc);
    rect_cut_top(&c, DP(10));
  }
  heading(&c, "LINK", true);
  int r = ui_textfield(ui_id("cloud.link.tf"), rect_cut_top(&c, fld_h()), U.link, sizeof U.link,
                       "https://mega.nz/folder/...", UI_TF_FOCUS | (U.busy ? UI_TF_READONLY : 0));
  if (r & UI_TF_CHANGED) U.err[0] = 0;
  rect_cut_top(&c, DP(10));
  if (U.busy) {
    ui_spinner(FM_RECT(c.x + DP(2), c.y, DP(18), DP(18)), T.accent);
    font_draw(FONT_REGULAR, ui.m.font, c.x + DP(28), c.y, "Opening\xE2\x80\xA6", -1, T.text);
  } else if (U.err[0]) {
    font_draw_wrap(FONT_REGULAR, ui.m.font_small, c.x, c.y, c.w, U.err, T.danger, true);
  }
  int b = buttons(ui_id("cloud.link.b"), c, labels, 2, UI_BTN_FILLED, U.busy ? 2 : 0);
  ui_dialog_end();
  if (b == 0 && U.busy) {
    cloud_task_cancel(CT_LINK, 0);
    U.busy = false;
    return;
  }
  if (cancel || b == 0) { open_ui(U_HOME); return; }
  if ((b == 1 || (r & UI_TF_SUBMIT)) && !U.busy) {
    if (!U.link[0]) { fm_strlcpy(U.err, "Paste a link first", sizeof U.err); return; }
    FmCloudTask *t = cloud_task_new(CT_LINK, 0);
    fm_strlcpy(t->acct.provider, U.svc->key, sizeof t->acct.provider);
    fm_strlcpy(t->arg, U.link, sizeof t->arg);
    t->done = link_done;
    U.busy = true;
    cloud_task_start(t);
  }
}

/* ---- account label, removing --------------------------------------------------------- */

static void draw_label(void) {
  const char *labels[] = { "Cancel", "Rename" };
  float cw = dlg_cw(440);
  float ch = ls() + DP(6) + fld_h() + DP(16) + buttons_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.label"), "Rename account", 440, dlg_h(ch), &cancel);
  heading(&c, "NAME IN THE APP", true);
  int r = ui_textfield(ui_id("cloud.label.tf"), rect_cut_top(&c, fld_h()), U.label, sizeof U.label, "Name",
                       UI_TF_FOCUS);
  int b = buttons(ui_id("cloud.label.b"), c, labels, 2, UI_BTN_FILLED, 0);
  ui_dialog_end();
  if (cancel || b == 0 || (r & UI_TF_CANCEL)) { close_ui(); return; }
  if (b == 1 || (r & UI_TF_SUBMIT)) {
    int i = cloud_acct_index(U.serial);
    if (i >= 0 && U.label[0]) {
      cloud_acct_lock();
      fm_strlcpy(cloud_acct_at(i)->label, U.label, sizeof cloud_acct_at(i)->label);
      cloud_acct_unlock();
      cloud_acct_save();                         /* temporary ones are skipped, the places rebuild */
      for (int k = 0; k < 2 && g_panels; k++)
        if (g_panels[k].list.loc.in_cloud && g_panels[k].list.loc.cloud == U.serial) panel_refresh(&g_panels[k]);
    }
    U.serial = 0;
    U.editing = true;
    close_ui();
  }
}

static void leave_account(int serial) {
  for (int k = 0; k < 2 && g_panels; k++) {
    FmPanel *p = &g_panels[k];
    if (!p->list.loc.in_cloud || p->list.loc.cloud != serial) continue;
    char home[FM_PATH_MAX];
    if (!plat_place(PLACE_HOME, home, sizeof home)) fm_strlcpy(home, FM_SEP_STR, sizeof home);
    if (conf.path[k][0] && plat_is_dir(conf.path[k])) fm_strlcpy(home, conf.path[k], sizeof home);
    FmLoc l;
    loc_local(&l, home);
    panel_go(p, &l, false);
  }
}

static void draw_remove(void) {
  int i = cloud_acct_index(U.serial);
  if (i < 0) { close_ui(); return; }
  const FmCloudAcct *a = cloud_acct_at(i);
  char title[128];
  fm_snprintf(title, sizeof title, "Remove \"%s\"?", a->label);
  const char *labels[] = { "Cancel", "Remove" };
  float cw = dlg_cw(440);
  const char *msg = "Its files stay in the cloud. This removes the account and its sign-in from this app.";
  float mh = font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw, msg, T.text, false);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.remove"), title, 440, dlg_h(mh + DP(16) + buttons_h(cw, labels, 2)),
                             &cancel);
  font_draw_wrap(FONT_REGULAR, ui.m.font, c.x, c.y, c.w, msg, T.text2, true);
  int b = buttons(ui_id("cloud.remove.b"), c, labels, 2, UI_BTN_DANGER, 0);
  ui_dialog_end();
  if (cancel || b == 0) { close_ui(); return; }
  if (b == 1) {
    int serial = U.serial;
    U.serial = 0;
    U.editing = true;
    close_ui();
    leave_account(serial);
    cloud_vfs_forget(serial);
    cloud_acct_remove(cloud_acct_index(serial));
    ui_toast("Account removed");
  }
}

static void acct_menu_action(int id) {
  int serial = g_menu_serial, i = cloud_acct_index(serial);
  if (i < 0) return;
  switch (id) {
    case AM_OPEN: {
      int panel = active_panel();
      close_ui();
      if (g_hooks.close_dialog) g_hooks.close_dialog();
      cloud_ui_go(serial, panel);
      break;
    }
    case AM_RENAME:
      open_ui(U_LABEL);
      U.serial = serial;
      U.editing = true;
      fm_strlcpy(U.label, cloud_acct_at(i)->label, sizeof U.label);
      break;
    case AM_SIGNIN: cloud_ui_signin(serial); break;
    case AM_SIGNOUT:
      cloud_acct_lock();
      cloud_acct_at(i)->session[0] = 0;
      memset(cloud_acct_at(i)->secret, 0, sizeof cloud_acct_at(i)->secret);
      cloud_acct_unlock();
      if (!cloud_acct_is_temp(i)) cloud_acct_save();
      cloud_vfs_forget(serial);
      ui_toast("Signed out");
      for (int k = 0; k < 2 && g_panels; k++)
        if (g_panels[k].list.loc.in_cloud && g_panels[k].list.loc.cloud == serial) panel_refresh(&g_panels[k]);
      break;
    case AM_REMOVE:
      if (cloud_acct_is_temp(i)) {
        leave_account(serial);
        cloud_vfs_forget(serial);
        cloud_acct_remove(i);
      } else {
        open_ui(U_REMOVE);
        U.serial = serial;
        U.editing = true;
      }
      break;
    default: break;
  }
}

/* ---- deleting items --------------------------------------------------------------------- */

void cloud_ui_delete(FmPanel *p) {
  int *items;
  int n = panel_selected(p, &items);
  if (n == 0) {
    fm_free(items);
    ui_toast("Select files or folders first");
    return;
  }
  open_ui(U_DELETE);
  U.panel = p->idx;
  U.loc = p->list.loc;
  U.ents = (FmCloudEntry *)fm_alloc((size_t)n * sizeof *U.ents);
  for (int i = 0; i < n; i++) {
    const FmCloudEntry *e = cloud_vfs_entry(&p->list, &p->list.items[items[i]]);
    if (e) U.ents[U.nents++] = *e;
  }
  fm_free(items);
}

static void draw_delete(void) {
  char title[300], where[128];
  if (U.nents == 1) fm_snprintf(title, sizeof title, "Delete \"%s\"?", U.ents[0].name);
  else fm_snprintf(title, sizeof title, "Delete %d items?", U.nents);
  const char *labels[] = { "Cancel", "Delete" };
  float cw = dlg_cw(460);
  int shown = FM_MIN(U.nents, 4);
  float rh = lh() + DP(10);
  loc_title(&U.loc, where, sizeof where);
  char msg[300];
  fm_snprintf(msg, sizeof msg, "They will be deleted from %s, into its trash when the service has one.", where);
  float mh = font_draw_wrap(FONT_REGULAR, ui.m.font, 0, 0, cw - DP(26), msg, T.text2, false);
  float ch = (float)shown * rh + (U.nents > shown ? rh : 0) + DP(10) + mh + DP(16) + buttons_h(cw, labels, 2);
  bool cancel;
  FmRect c = ui_dialog_begin(ui_id("cloud.delete"), title, 460, dlg_h(ch), &cancel);
  for (int i = 0; i < shown; i++) {
    FmRect r = rect_cut_top(&c, rh);
    float is = DP(22);
    icon_file(U.ents[i].dir ? FT_DIR : fm_type_from_name(U.ents[i].name), FM_RECT(r.x, r.y + (r.h - is) * 0.5f, is, is),
              T.dark);
    font_draw_mid_ellipsis(FONT_REGULAR, ui.m.font, r.x + is + DP(10), r.y + (r.h - lh()) * 0.5f, U.ents[i].name,
                           r.w - is - DP(10), T.text);
  }
  if (U.nents > shown) {
    char more[64];
    fm_snprintf(more, sizeof more, "and %d more", U.nents - shown);
    FmRect r = rect_cut_top(&c, rh);
    font_draw(FONT_REGULAR, ui.m.font, r.x + DP(32), r.y + (r.h - lh()) * 0.5f, more, -1, T.text2);
  }
  rect_cut_top(&c, DP(10));
  icon_draw(IC_DELETE, FM_RECT(c.x, c.y + DP(1), DP(18), DP(18)), T.text2);
  font_draw_wrap(FONT_REGULAR, ui.m.font, c.x + DP(26), c.y, c.w - DP(26), msg, T.text2, true);
  int b = buttons(ui_id("cloud.delete.b"), c, labels, 2, UI_BTN_DANGER, 0);
  ui_dialog_end();
  if (cancel || b == 0) { close_ui(); return; }
  if (b == 1) {
    FmCloudJobSpec s;
    memset(&s, 0, sizeof s);
    s.op = CJ_DELETE;
    s.src_loc = &U.loc;
    s.entries = U.ents;
    s.nentries = U.nents;
    if (cloud_job_run(&s, false, NULL) != FM_OK) ui_toast("Too many jobs are running");
    else if (g_panels) panel_select_all(&g_panels[U.panel], false);
    close_ui();
  }
}

/* ---- frame ---------------------------------------------------------------------------- */

bool cloud_ui_frame(void) {
  int r = ui_menu_result(ID_ACCT);
  if (r >= 0) acct_menu_action(r);
  switch (U.kind) {
    case U_HOME: draw_home(); break;
    case U_PICK: draw_pick(); break;
    case U_FORM: draw_form(); break;
    case U_LINK: draw_link(); break;
    case U_LABEL: draw_label(); break;
    case U_REMOVE: draw_remove(); break;
    case U_DELETE: draw_delete(); break;
    default: break;
  }
  return U.kind != U_NONE;
}

/* ---- settings ------------------------------------------------------------------------ */

static float settings_layout(FmRect *r, u32 base, bool draw) {
  float y0 = r->y;
  heading(r, "CLOUD STORAGE", draw);
  int n = cloud_acct_count();
  if (n == 0) wrap(r, "No accounts yet. Add one to browse it in the panels.", T.text3, draw);
  for (int i = 0; i < n; i++) {
    if (acct_row(r, ui_idn(base, 10 + (u32)i), i, draw, true)) {
      int serial = cloud_acct_serial(i);
      g_menu_serial = serial;
      acct_menu_action(AM_OPEN);
    }
  }
  rect_cut_top(r, DP(6));
  FmRect br = rect_cut_top(r, btn_h());
  if (draw) {
    float w = btn_w("Add account") + DP(20);
    if (ui_button(ui_idn(base, 1), FM_RECT(br.x, br.y, w, br.h), IC_PLUS, "Add account", UI_BTN_TONAL)) cloud_ui_add();
  }
  rect_cut_top(r, DP(8));
#ifdef FM_WIN
  wrap(r, "Accounts are kept in cloud.txt in the settings folder; passwords, keys and sign-ins are encrypted for "
          "your Windows user.", T.text3, draw);
#else
  wrap(r, "Accounts are kept in cloud.txt in the settings folder; passwords, keys and sign-ins are only scrambled "
          "there, not encrypted, so keep that folder private.", T.text3, draw);
#endif
  rect_cut_top(r, DP(14));
  return r->y - y0;
}

static bool g_settings_jump;

bool cloud_settings_focus(void) {
  bool j = g_settings_jump;
  g_settings_jump = false;
  return j;
}

float cloud_settings_h(float w) {
  FmRect r = { 0, 0, w, 100000 };
  return settings_layout(&r, 0, false);
}

void cloud_settings(FmRect *r, u32 base) { settings_layout(r, base, true); }

/* ---- panel operations --------------------------------------------------------------- */

bool cloud_panel_writable(const FmPanel *p) {
  if (!p->list.loc.in_cloud || p->list.err != FM_OK) return false;
  const FmCloud *c = svc_of(p->list.loc.cloud);
  return c && (c->flags & CLOUD_UPLOAD) && c->upload && c->mkdir;
}

static void stream_done(FmCloudTask *t) {
  bool video = fm_type_from_name(t->entry.name) == FT_VIDEO;
  if (t->err == FM_OK && t->url[0]) {
    if (video && !t->headers[0]) {
      if (video_open_stream(t->entry.name, t->url, NULL)) return;
    } else if (!video) {
      FmAudioEntry e;
      memset(&e, 0, sizeof e);
      e.url = t->url;
      e.title = t->entry.name;
      e.headers = t->headers[0] ? t->headers : NULL;
      if (audio_play_entries(&e, 1, 0)) {
        audio_show_player();
        return;
      }
    }
  }
  /* no stream: download it like any other file */
  char out[FM_PATH_MAX];
  FmLoc l;
  cloud_loc_root(&l, t->serial);
  if (!cloud_cache_path(t->serial, &t->entry, out, sizeof out)) return;
  FmCloudJobSpec s;
  memset(&s, 0, sizeof s);
  s.op = CJ_OPEN;
  s.src_loc = &l;
  s.entries = &t->entry;
  s.nentries = 1;
  s.open_path = out;
  if (cloud_job_run(&s, false, NULL) != FM_OK) ui_toast("Too many jobs are running");
}

void cloud_ui_open_item(FmPanel *p, int item) {
  const FmEntry *fe = &p->list.items[item];
  const FmCloudEntry *e = cloud_vfs_entry(&p->list, fe);
  if (!e) return;
  int serial = p->list.loc.cloud;
  const FmCloud *c = svc_of(serial);
  if (!c) return;
  FmType t = fm_type_from_name(e->name);
  char out[FM_PATH_MAX];
  if (!cloud_cache_path(serial, e, out, sizeof out)) { ui_toast("Can't open %s", e->name); return; }
  FmStat st;
  if (plat_stat(out, &st) && st.size == e->size) {   /* opened before: the cached copy */
    app_open(out, NULL, 0, 0);
    return;
  }
  if ((t == FT_AUDIO || t == FT_VIDEO) && (c->flags & CLOUD_STREAM) && c->stream_url) {
    FmCloudTask *k = cloud_task_new(CT_STREAM, serial);
    if (k) {
      k->entry = *e;
      k->done = stream_done;
      cloud_task_start(k);
      ui_toast("Opening %s\xE2\x80\xA6", e->name);
      return;
    }
  }
  FmCloudJobSpec s;
  memset(&s, 0, sizeof s);
  s.op = CJ_OPEN;
  s.src_loc = &p->list.loc;
  s.entries = e;
  s.nentries = 1;
  s.open_path = out;
  if (cloud_job_run(&s, false, NULL) != FM_OK) ui_toast("Too many jobs are running");
}

void cloud_ui_transfer(bool move, FmPanel *src, FmPanel *dst) {
  bool to_cloud = dst->list.loc.in_cloud;
  if (to_cloud && !cloud_panel_writable(dst)) {
    ui_toast(dst->list.err != FM_OK ? "The target folder is not available" : "This service can't receive files yet");
    return;
  }
  if (!to_cloud && (dst->list.loc.in_arc || dst->list.err != FM_OK)) {
    ui_toast(dst->list.loc.in_arc ? "Can't write into an archive; extract it first" : "The target folder is not available");
    return;
  }
  if (src->list.loc.in_arc) { ui_toast("Extract the files first, then copy them to the cloud"); return; }
  if (src->nsel == 0) { ui_toast("Select files or folders first"); return; }
  if (move && loc_equal(&src->list.loc, &dst->list.loc)) { ui_toast("Both panels show the same folder"); return; }
  FmCloudJobSpec s;
  memset(&s, 0, sizeof s);
  s.op = CJ_TRANSFER;
  s.move = move;
  s.conflict = CONFLICT_ASK;
  s.dst_loc = to_cloud ? &dst->list.loc : NULL;
  s.dst_dir = to_cloud ? NULL : dst->list.loc.path;
  char **paths = NULL;
  int np = 0;
  FmCloudEntry *ents = NULL;
  if (src->list.loc.in_cloud) {
    int *items;
    int n = panel_selected(src, &items);
    ents = (FmCloudEntry *)fm_alloc((size_t)(n > 0 ? n : 1) * sizeof *ents);
    int k = 0;
    for (int i = 0; i < n; i++) {
      const FmCloudEntry *e = cloud_vfs_entry(&src->list, &src->list.items[items[i]]);
      if (e) ents[k++] = *e;
    }
    fm_free(items);
    s.src_loc = &src->list.loc;
    s.entries = ents;
    s.nentries = k;
  } else {
    paths = panel_selected_paths(src, &np);
    s.paths = (const char *const *)paths;
    s.npaths = np;
  }
  FmErr e = cloud_job_run(&s, false, NULL);
  if (e == FM_OK) panel_select_all(src, false);
  else ui_toast(e == FM_ERR_IO ? "Too many jobs are running" : "That account is no longer here");
  fm_free(ents);
  if (paths) panel_free_paths(paths, np);
}

void cloud_ui_upload_paths(FmPanel *dst, char **paths, int n, bool move) {
  if (!cloud_panel_writable(dst)) { ui_toast("Can't paste here"); return; }
  FmCloudJobSpec s;
  memset(&s, 0, sizeof s);
  s.op = CJ_TRANSFER;
  s.move = move;
  s.conflict = CONFLICT_ASK;
  s.paths = (const char *const *)paths;
  s.npaths = n;
  s.dst_loc = &dst->list.loc;
  if (cloud_job_run(&s, false, NULL) != FM_OK) ui_toast("Too many jobs are running");
}

static bool name_taken(const FmPanel *p, const char *name, const char *except) {
  for (int i = 0; i < p->list.count; i++) {
    const char *n = p->list.items[i].name;
    if (!fm_stricmp(n, name) && (!except || strcmp(n, except) != 0)) return true;
  }
  return false;
}

/* Refreshes the panels showing `key`, selecting `name` once it is listed. */
static void refresh_key(const char *key, const char *name) {
  cloud_vfs_invalidate(key);
  for (int k = 0; k < 2 && g_panels; k++) {
    FmPanel *p = &g_panels[k];
    if (!p->list.loc.in_cloud) continue;
    char pk[CLOUD_ID_MAX + 32];
    cloud_loc_key(&p->list.loc, pk, sizeof pk);
    if (strcmp(pk, key) != 0) continue;
    if (name) fm_strlcpy(p->select_after, name, sizeof p->select_after);
    panel_refresh(p);
  }
}

static void small_done(FmCloudTask *t) {
  if (t->err != FM_OK) {
    ui_toast("%s", t->msg);
    return;
  }
  refresh_key(t->key, t->name);
}

bool cloud_ui_rename(FmPanel *p, const char *orig, const char *name, char *err, size_t cap) {
  if (!strcmp(orig, name)) return true;
  const FmCloudEntry *e = NULL;
  for (int i = 0; i < p->list.count && !e; i++)
    if (!strcmp(p->list.items[i].name, orig)) e = cloud_vfs_entry(&p->list, &p->list.items[i]);
  if (!e) { fm_strlcpy(err, "That item is no longer here", cap); return false; }
  if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
    fm_strlcpy(err, "That name is not allowed", cap);
    return false;
  }
  if (name_taken(p, name, orig)) { fm_strlcpy(err, "An item with this name already exists", cap); return false; }
  FmCloudTask *t = cloud_task_new(CT_RENAME, p->list.loc.cloud);
  if (!t) { fm_strlcpy(err, "That account is no longer here", cap); return false; }
  t->entry = *e;
  fm_strlcpy(t->name, name, sizeof t->name);
  cloud_loc_key(&p->list.loc, t->key, sizeof t->key);
  t->done = small_done;
  cloud_task_start(t);
  return true;
}

bool cloud_ui_mkdir(FmPanel *p, const char *name, char *err, size_t cap) {
  if (!cloud_panel_writable(p)) { fm_strlcpy(err, "Can't create folders here", cap); return false; }
  if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
    fm_strlcpy(err, "That name is not allowed", cap);
    return false;
  }
  if (name_taken(p, name, NULL)) { fm_strlcpy(err, "An item with this name already exists", cap); return false; }
  FmCloudTask *t = cloud_task_new(CT_MKDIR, p->list.loc.cloud);
  if (!t) { fm_strlcpy(err, "That account is no longer here", cap); return false; }
  cloud_loc_id(&p->list.loc, t->arg, sizeof t->arg);
  fm_strlcpy(t->name, name, sizeof t->name);
  cloud_loc_key(&p->list.loc, t->key, sizeof t->key);
  t->done = small_done;
  cloud_task_start(t);
  return true;
}

void cloud_ui_touched(const char *key) {
  int serial = cloud_key_serial(key);
  if (!serial) return;
  /* folders below the target changed too: forget them all, refresh what shows */
  cloud_vfs_forget(serial);
  cloud_quota_stale(serial);
  for (int k = 0; k < 2 && g_panels; k++) {
    FmPanel *p = &g_panels[k];
    if (!p->list.loc.in_cloud || p->list.loc.cloud != serial) continue;
    char pk[CLOUD_ID_MAX + 32];
    cloud_loc_key(&p->list.loc, pk, sizeof pk);
    if (!strcmp(pk, key)) panel_refresh(p);
  }
}

bool cloud_ui_quota_text(const FmPanel *p, char *out, size_t cap, float *used) {
  if (!p->list.loc.in_cloud) return false;
  u64 u, t;
  if (!cloud_quota(p->list.loc.cloud, &u, &t)) return false;
  char a[32], b[32];
  if (t > 0) {
    fm_snprintf(out, cap, "%s of %s used", fm_fmt_size(u, a, sizeof a), fm_fmt_size(t, b, sizeof b));
    *used = (float)((double)u / (double)t);
  } else {
    fm_snprintf(out, cap, "%s used", fm_fmt_size(u, a, sizeof a));
    *used = -1;
  }
  return true;
}

/* ---- demo ------------------------------------------------------------------------------- */

void cloud_ui_demo(const char *state) {
  cloud_mock_enable(true);
  cloud_mock_reset(true);
  const char *st = state ? state : "panel";
  if (!strcmp(st, "loading")) cloud_mock_set_delay(60000);
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "mock", sizeof a.provider);
  fm_strlcpy(a.label, "Demo cloud", sizeof a.label);
  fm_strlcpy(a.user, "ann@example.com", sizeof a.user);
  if (strcmp(st, "signedout") != 0) fm_strlcpy(a.session, "mock-session:demo", sizeof a.session);
  int i = cloud_acct_add_temp(&a);
  if (i < 0) return;
  int serial = cloud_acct_serial(i);
  /* the demo panel lists inline, so the first frame already shows it */
  bool loading = !strcmp(st, "loading");
  cloud_set_sync(!loading);
  cloud_ui_go(serial, 1);
  FmPanel *p = g_panels ? &g_panels[1] : NULL;
  if (p && (!strcmp(st, "folder") || !strcmp(st, "delete"))) {
    const char *path[] = { "Photos", "Holiday 2026" };
    for (int k = 0; k < 2; k++)
      for (int j = 0; j < p->list.count; j++)
        if (!strcmp(p->list.items[j].name, path[k])) { panel_open_item(p, j); break; }
  }
  cloud_set_sync(false);
  if (p && !strcmp(st, "delete")) {
    for (int j = 0; j < p->nview && j < 4; j += 2) p->list.items[p->view[j]].selected = 1;
    panel_update_sel(p);
    cloud_ui_delete(p);
  }
  if (!strcmp(st, "home")) cloud_ui_home();
  else if (!strcmp(st, "add")) cloud_ui_add();
  else if (!strncmp(st, "form-", 5)) {
    const FmCloud *c = cloud_find(st + 5);
    if (c) start_form(c);
  } else if (!strcmp(st, "link")) {
    open_ui(U_LINK);
  } else if (!strcmp(st, "job")) {
    /* a real upload on a worker that meets "Welcome.txt": the conflict question */
    char dir[FM_PATH_MAX], f1[FM_PATH_MAX], f2[FM_PATH_MAX];
    if (plat_place(PLACE_TEMP, dir, sizeof dir) && fm_path_join(dir, sizeof dir, dir, "mmcfm-cloud-demo") &&
        plat_mkdirs(dir) == FM_OK && fm_path_join(f1, sizeof f1, dir, "Welcome.txt") &&
        fm_path_join(f2, sizeof f2, dir, "Notes from the trip.txt")) {
      FILE *f = fm_fopen(f1, "wb");
      if (f) { fputs("A newer welcome.\n", f); fclose(f); }
      f = fm_fopen(f2, "wb");
      if (f) { fputs("Day 1: the coast.\n", f); fclose(f); }
      char *paths[] = { f2, f1 };
      if (p) cloud_ui_upload_paths(p, paths, 2, false);
    }
  } else if (!strcmp(st, "settings") && g_hooks.open_settings) {
    g_hooks.open_settings();
    g_settings_jump = true;
  }
}
