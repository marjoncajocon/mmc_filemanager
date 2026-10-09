/* ftray.c -- the notification area icon (see ftray.h).
**
** Design decisions:
**   - Shell_NotifyIconW from shell32.dll, loaded at run time like WinHTTP:
**     nothing new is linked and the tcc build keeps working. The structure
**     is the Windows 2000 layout (NOTIFYICONDATAW_V2), which every Windows
**     since accepts.
**   - The icon's messages go to a hidden top-level window of our own (a
**     message-only window would miss "TaskbarCreated", sent when Explorer
**     restarts, after which the icon is added again). SDL's event pump
**     dispatches every message of the thread, so its window procedure runs
**     on the main thread; it only records what was picked and wakes the app,
**     tray_pump does the work inside the normal frame.
**   - There is no .ico in the exe, so the logo (the title bar's: a blue
**     rounded square with a white folder) is drawn into 32x32 pixels once and
**     used for the tray and as the window's icon (taskbar, Alt+Tab).
**   - A hidden window draws nothing: the frame loop treats it like an app in
**     the background (no rendering, the CPU sleeps), while sound, downloads
**     and file jobs go on.
*/
#include "ftray.h"
#include "fapp.h"
#include "fconf.h"
#include "fsdl.h"
#include "fui.h"
#include "fview.h"

/* ---- the logo as pixels ------------------------------------------------------ */

/* tools/make_icons.py draws it, the same logo as the exe, .app and launcher */
#if !defined(FM_MOBILE) && !defined(FM_WEB)
#include "flogo.h"
#endif

void tray_set_window_icon(void) {
#if !defined(FM_MOBILE) && !defined(FM_WEB)
  if (!app.win) return;
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)kLogo64, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_ARGB8888);
  if (s) {
    SDL_SetWindowIcon(app.win, s);
    SDL_FreeSurface(s);
  }
#endif
}

/* ---- Windows ---------------------------------------------------------------- */
#if defined(FM_WIN)

#include "fwin.h"

typedef struct {
  DWORD cbSize;
  HWND hWnd;
  UINT uID, uFlags, uCallbackMessage;
  HICON hIcon;
  WCHAR szTip[128];
  DWORD dwState, dwStateMask;
  WCHAR szInfo[256];
  UINT uVersion;
  WCHAR szInfoTitle[64];
  DWORD dwInfoFlags;
} TrayNid;                                       /* NOTIFYICONDATAW_V2 */

enum { NIM_ADD_ = 0, NIM_MODIFY_ = 1, NIM_DELETE_ = 2, NIF_MESSAGE_ = 1, NIF_ICON_ = 2, NIF_TIP_ = 4 };
enum { TRAY_MSG = WM_APP + 77, TRAY_ID = 1 };
enum { CMD_SHOW = 1, CMD_PLAY, CMD_NEXT, CMD_QUIT };

typedef BOOL(WINAPI *NotifyFn)(DWORD, void *);

static NotifyFn g_notify;
static HWND g_hwnd;
static HICON g_icon;
static UINT g_taskbar_created;
static bool g_added, g_hidden;
static volatile int g_cmd;                       /* picked on the icon, for tray_pump */
static bool g_quitting;                          /* Quit was picked: the close button must not hide */

static bool load(void) {
  static int state;
  if (!state) {
    HMODULE m = LoadLibraryW(L"shell32.dll");
    g_notify = m ? (NotifyFn)(void (*)(void))GetProcAddress(m, "Shell_NotifyIconW") : NULL;
    state = g_notify ? 1 : -1;
  }
  return state > 0;
}

bool tray_available(void) { return load(); }

static void nid_init(TrayNid *n) {
  memset(n, 0, sizeof *n);
  n->cbSize = sizeof *n;
  n->hWnd = g_hwnd;
  n->uID = TRAY_ID;
}

static void icon_add(void) {
  TrayNid n;
  nid_init(&n);
  n.uFlags = NIF_MESSAGE_ | NIF_ICON_ | NIF_TIP_;
  n.uCallbackMessage = TRAY_MSG;
  n.hIcon = g_icon;
  wcsncpy(n.szTip, L"MMC File Manager", 127);
  g_added = g_notify(NIM_ADD_, &n) != 0;
  fm_log("tray: icon %s", g_added ? "added" : "could not be added");
}

static void icon_remove(void) {
  if (!g_added) return;
  TrayNid n;
  nid_init(&n);
  g_notify(NIM_DELETE_, &n);
  g_added = false;
}

static void menu_show(void) {
  HMENU m = CreatePopupMenu();
  if (!m) return;
  int st = audio_state(NULL, 0);
  bool playing = st == AUDIO_PLAYING || st == AUDIO_BUFFERING;
  AppendMenuW(m, MF_STRING, CMD_SHOW, g_hidden ? L"Show MMC File Manager" : L"Hide to the tray");
  if (st != AUDIO_IDLE) {
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_PLAY, playing ? L"Pause" : L"Play");
    AppendMenuW(m, MF_STRING, CMD_NEXT, L"Next track");
  }
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, CMD_QUIT, L"Quit");
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(g_hwnd);                    /* else the menu does not close on a click elsewhere */
  int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, NULL);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  if (cmd) {
    g_cmd = cmd;
    app_wake();
  }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == TRAY_MSG) {
    UINT ev = (UINT)lp;
    if (ev == WM_LBUTTONUP) { g_cmd = CMD_SHOW; app_wake(); }
    else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) menu_show();
    return 0;
  }
  if (g_taskbar_created && msg == g_taskbar_created) {   /* Explorer restarted */
    g_added = false;
    if (conf.tray) icon_add();
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static bool window_make(void) {
  if (g_hwnd) return true;
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSEXW wc;
  memset(&wc, 0, sizeof wc);
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.lpszClassName = L"mmcfm-tray";
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"mmcfm-tray", L"mmcfm tray", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
  if (!g_hwnd) return false;
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  /* the exe's own icon at the tray's size (zig builds embed icons/mmcfm.ico);
  ** else (tcc) the logo pixels as an HICON */
  g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                             GetSystemMetrics(SM_CYSMICON), 0);
  if (g_icon) return true;
  enum { LOGO = 32 };
  /* a 32-bit colour bitmap and an (unused) mask */
  BITMAPV5HEADER bi;
  memset(&bi, 0, sizeof bi);
  bi.bV5Size = sizeof bi;
  bi.bV5Width = LOGO;
  bi.bV5Height = -LOGO;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void *bits = NULL;
  HDC dc = GetDC(NULL);
  HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
  ReleaseDC(NULL, dc);
  HBITMAP mask = CreateBitmap(LOGO, LOGO, 1, 1, NULL);
  if (color && bits) {
    /* premultiplied, as Windows expects for 32-bit icons */
    u32 *px = (u32 *)bits;
    for (int i = 0; i < LOGO * LOGO; i++) {
      u32 c = kLogo32[i], a = c >> 24;
      u32 r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
      px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    g_icon = CreateIconIndirect(&ii);
  }
  if (color) DeleteObject(color);
  if (mask) DeleteObject(mask);
  if (!g_icon) g_icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  return true;
}

void tray_apply(void) {
  if (!load()) return;
  if (conf.tray) {
    if (window_make() && !g_added) icon_add();
  } else {
    if (g_hidden) tray_show_window();           /* never leave it hidden without the icon */
    icon_remove();
  }
}

void tray_shutdown(void) {
  icon_remove();
  if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
  if (g_icon) { DestroyIcon(g_icon); g_icon = NULL; }
}

bool tray_window_hidden(void) { return g_hidden; }

void tray_hide_window(void) {
  if (!app.win || !g_added) return;             /* without the icon there would be no way back */
  g_hidden = true;
  SDL_HideWindow(app.win);
  fm_log("tray: window hidden (shown %d)", (int)((SDL_GetWindowFlags(app.win) & SDL_WINDOW_SHOWN) != 0));
  app.background = true;                        /* nothing to draw: the loop sleeps */
}

void tray_show_window(void) {
  if (!app.win) return;
  g_hidden = false;
  app.background = false;
  SDL_ShowWindow(app.win);
  if (SDL_GetWindowFlags(app.win) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(app.win);
  SDL_RaiseWindow(app.win);
  ui_redraw();
  fm_log("tray: window shown (shown %d)", (int)((SDL_GetWindowFlags(app.win) & SDL_WINDOW_SHOWN) != 0));
}

bool tray_take_close(void) {
  if (g_quitting) { g_quitting = false; return false; }
  if (!conf.tray || !conf.tray_close || !g_added) return false;
  tray_hide_window();
  return true;
}

bool tray_take_minimize(void) {
  if (!conf.tray || !conf.tray_min || !g_added || g_hidden) return false;
  tray_hide_window();
  return true;
}

void tray_pump(void) {
  /* MMCFM_TRAY_DEMO=1: minimize at 2 s (to the tray), "click" the icon at 4 s */
  static int demo = -1;
  static u64 t0;
  if (demo < 0) { demo = getenv("MMCFM_TRAY_DEMO") ? 0 : 3; t0 = SDL_GetTicks64(); }
  if (demo == 0 && SDL_GetTicks64() - t0 > 2000) { demo = 1; fm_log("tray demo: minimize"); SDL_MinimizeWindow(app.win); }
  if (demo == 1 && SDL_GetTicks64() - t0 > 4000) { demo = 2; fm_log("tray demo: icon clicked"); g_cmd = CMD_SHOW; }
  if (demo < 3) app_wake();
  int cmd = g_cmd;
  if (!cmd) return;
  g_cmd = 0;
  switch (cmd) {
    case CMD_SHOW:
      if (g_hidden) tray_show_window();
      else tray_hide_window();
      break;
    case CMD_PLAY: audio_toggle_play(); break;
    case CMD_NEXT: audio_next_track(); break;
    case CMD_QUIT: {
      SDL_Event e;
      memset(&e, 0, sizeof e);
      e.type = SDL_QUIT;                         /* the normal quit path (it asks when jobs run) */
      g_quitting = true;
      if (g_hidden) tray_show_window();
      SDL_PushEvent(&e);
      break;
    }
    default: break;
  }
}

#else /* not Windows */

bool tray_available(void) { return false; }
void tray_apply(void) {}
void tray_shutdown(void) {}
bool tray_window_hidden(void) { return false; }
void tray_hide_window(void) {}
void tray_show_window(void) {}
void tray_pump(void) {}
bool tray_take_close(void) { return false; }
bool tray_take_minimize(void) { return false; }

#endif
