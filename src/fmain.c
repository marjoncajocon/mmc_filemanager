/* fmain.c -- entry point: SDL setup, window, renderer and the event loop.
**
** Design decisions:
**   - The loop sleeps in SDL_WaitEventTimeout whenever nothing animates, so
**     an idle file manager uses no CPU; ui_wait_ms says when the next frame
**     is due (caret blink, toast fade).
**   - Rendering goes through SDL_Renderer (Direct3D 11, Metal, OpenGL / ES,
**     or software as the last resort).
**   - Android: SDL_main is called from SDLActivity; lifecycle events stop
**     drawing in the background and recreate textures on return.
*/
#include "fapp.h"
#include "ftray.h"
#include "fmedia.h"
#include "fsdl.h"
#include "ftest.h"
#include "fperf.h"

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

FmApp app;

static void fatal_box(const char *msg) {
  fprintf(stderr, "mmcfm: %s\n", msg);
#ifdef FM_WIN
  {
    /* SDL may not be loaded: go to user32 directly. */
    extern __declspec(dllimport) int __stdcall MessageBoxA(void *, const char *, const char *, unsigned);
    MessageBoxA(NULL, msg, "MMC File Manager", 0x10);
  }
#elif defined(FM_SDL_DYNAMIC)
  if (SDL_ShowSimpleMessageBox) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MMC File Manager", msg, NULL);
#else
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MMC File Manager", msg, NULL);
#endif
}

static bool create_renderer(void) {
#ifdef FM_WIN
  /* Direct3D 11 first: on the same GPU it needs about half the driver memory
  ** of SDL's default Direct3D 9 (82 vs 140-185 MB measured on Intel). SDL
  ** falls back to the usual order where it is missing (Windows 7 without the
  ** platform update), and SDL_RENDER_DRIVER in the environment still wins.
  ** Picking a driver by hint turns SDL's batching off, so turn it back on. */
  if (!getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
  SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
#endif
  app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_ACCELERATED);
  if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
  if (!app.ren) return false;
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(app.ren, &info) == 0) fm_log("renderer: %s", info.name);
  gfx_init(app.ren);
  return true;
}

/* ---- sleeping between events ---------------------------------------------- */

/* SDL 2 only blocks for real on Windows, X11 and Wayland. Elsewhere
** (Android, macOS) SDL_WaitEventTimeout polls every millisecond, which keeps
** the CPU awake and drains batteries while nothing changes on screen. There
** an event watch posts a semaphore for every event SDL queues (input, text,
** timers, app_wake, lifecycle), and the loop sleeps on that instead. */
#if !defined(FM_WIN) && !defined(FM_WEB) && !defined(FM_LINUX) && !defined(FM_BSD)
#  define FM_OWN_WAIT 1
static SDL_sem *g_wake_sem;

static int SDLCALL wake_watch(void *user, SDL_Event *e) {
  FM_UNUSED(user);
  /* SDL queues a sentinel on every poll; waking for it would spin */
  if (e->type != SDL_POLLSENTINEL) SDL_SemPost(g_wake_sem);
  return 1;
}
#endif

static void wait_init(void) {
#ifdef FM_OWN_WAIT
  g_wake_sem = SDL_CreateSemaphore(0);
  if (g_wake_sem) SDL_AddEventWatch(wake_watch, NULL);
#endif
}

static void wait_shutdown(void) {
#ifdef FM_OWN_WAIT
  if (!g_wake_sem) return;
  SDL_DelEventWatch(wake_watch, NULL);
  SDL_DestroySemaphore(g_wake_sem);
  g_wake_sem = NULL;
#endif
}

static bool wait_event(SDL_Event *e, int ms) {
#ifdef FM_OWN_WAIT
  if (g_wake_sem) {
    if (SDL_PollEvent(e)) return true;          /* pumps, then checks the queue */
    SDL_SemWaitTimeout(g_wake_sem, (Uint32)ms);
    while (SDL_SemTryWait(g_wake_sem) == 0) {}  /* one wake covers them all */
    return SDL_PollEvent(e) != 0;
  }
#endif
  return SDL_WaitEventTimeout(e, ms) != 0;
}

/* MMCFM_IDLE_LOG=1: every 5 s, the frames drawn and the loop's wakeups, and
** who asked for them (ui_animate / ui_redraw / app_wake callers, counted),
** to find what keeps an idle screen drawing (battery). */
static void idle_log(bool drawing) {
  static int on = -1, frames, wakes, nwhy, window_ms = 5000;
  static u32 seen_seq;
  static u64 t0;
  static struct { char s[120]; int n; } why[6];
  if (on < 0) {
    const char *v = getenv("MMCFM_IDLE_LOG");    /* 1, or the window in ms (e.g. 1000) */
    on = v != NULL;
    if (v && atoi(v) >= 200) window_ms = atoi(v);
  }
  if (!on) return;
  u64 now = SDL_GetTicks64();
  if (!t0) t0 = now;
  wakes++;
  if (drawing) {
    char w[120];
    frames++;
    if (ui_busy_frame()) fm_strlcpy(w, "busy (loading animation)", sizeof w);
    else if (ui.anim && g_ui_anim_file) fm_snprintf(w, sizeof w, "animate %s:%d", g_ui_anim_file, g_ui_anim_line);
    else if (ui.redraw > 0 && g_ui_redraw_file && g_ui_redraw_seq != seen_seq)
      fm_snprintf(w, sizeof w, "redraw %s:%d", g_ui_redraw_file, g_ui_redraw_line);
    else if (ui.down || ui.nfingers) fm_strlcpy(w, "pointer down", sizeof w);
    else fm_strlcpy(w, ui.focus ? "input or caret blink" : "input", sizeof w);
    int i = 0;
    while (i < nwhy && strcmp(why[i].s, w)) i++;
    if (i == nwhy && nwhy < (int)FM_COUNT(why)) fm_strlcpy(why[nwhy++].s, w, sizeof why[0].s);
    if (i < nwhy) why[i].n++;
    seen_seq = g_ui_redraw_seq;
  }
  if (now - t0 >= (u64)window_ms) {
    char list[800] = "";
    size_t n = 0;
    for (int i = 0; i < nwhy && n < sizeof list; i++)
      n += (size_t)fm_snprintf(list + n, sizeof list - n, "%s%s x%d", i ? ", " : "", why[i].s, why[i].n);
    u32 pr, sk;
    gfx_present_counts(&pr, &sk);
    static u32 pr0, sk0;
    fm_log("idle-log: %d frames (%u presented, %u the same picture), %d wakeups in %.1f s%s%s", frames, pr - pr0, sk - sk0,
           wakes, (now - t0) / 1000.0, nwhy ? "; " : "", list);
    pr0 = pr;
    sk0 = sk;
    frames = wakes = nwhy = 0;
    memset(why, 0, sizeof why);
    t0 = now;
  }
}

/* The display's frame time in ms (60 Hz when unknown); looked up once a second. */
static double display_period(void) {
  static double period = 1000.0 / 60.0;
  static u64 asked;
  u64 now = SDL_GetTicks64();
  if (!asked || now - asked > 1000) {
    asked = now;
    SDL_DisplayMode m;
    int d = app.win ? SDL_GetWindowDisplayIndex(app.win) : 0;
    if (SDL_GetCurrentDisplayMode(d < 0 ? 0 : d, &m) == 0 && m.refresh_rate >= 20 && m.refresh_rate <= 500)
      period = 1000.0 / m.refresh_rate;
  }
  return period;
}

/* A frame not presented (the same picture) does not wait for vsync, and a
** present may return early while the GPU queue has room: the next frame
** waits until a display frame has passed since this one began, so whatever
** keeps asking for frames runs at the display's rate at most, never flat
** out. Input still wakes the loop at once. */
static u64 g_pace_until;

/* MMCFM_TEST_TAP=x,y,ms[;x,y,ms...]: a press and its release queued together
** at x,y (window pixels) ms after start, the worst case for a capped frame
** rate; several taps run in order. */
static void test_tap(void) {
  static const char *next;
  static int state = -1, tx, ty, at;
  if (state < 0) {
    next = getenv("MMCFM_TEST_TAP");
    state = 1;
  }
  if (state) {                                     /* parse the next tap */
    if (!next || sscanf(next, "%d,%d,%d", &tx, &ty, &at) != 3) { next = NULL; return; }
    next = strchr(next, ';');
    if (next) next++;
    state = 0;
  }
  if (SDL_GetTicks64() < (u64)at) return;
  state = 1;
  SDL_Event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = SDL_MOUSEMOTION;
  ev.motion.windowID = SDL_GetWindowID(app.win);
  ev.motion.x = tx;
  ev.motion.y = ty;
  SDL_PushEvent(&ev);
  for (int k = 0; k < 2; k++) {
    memset(&ev, 0, sizeof ev);
    ev.type = k ? SDL_MOUSEBUTTONUP : SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = SDL_GetWindowID(app.win);
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.state = k ? SDL_RELEASED : SDL_PRESSED;
    ev.button.clicks = 1;
    ev.button.x = tx;
    ev.button.y = ty;
    SDL_PushEvent(&ev);
  }
  fm_log("test tap at %d,%d", tx, ty);
}

static void frame(void) {
  SDL_Event e;
  test_tap();
  int wait = ui_wait_ms();
  bool got;
  u64 tnow = SDL_GetTicks64();
  if (wait == 0 && g_pace_until > tnow) wait = (int)(g_pace_until - tnow);
  /* A press and its release in one batch (a quick tap while a busy frame,
  ** such as a playing video, kept the loop away) would reach the widgets in
  ** the same frame, and press-then-release clicks were lost: the release
  ** waits for the next frame, which comes at once. */
  static SDL_Event held_up;
  static bool have_held;
  static bool pressed_now;               /* a press not drawn yet (frames can wait for the cap) */
  if (have_held) {
    if (pressed_now && g_pace_until > tnow) SDL_Delay((Uint32)FM_MIN(g_pace_until - tnow, 100));
    have_held = false;
    e = held_up;
    got = true;
  } else if (app.background) {
    /* Idle and in the background the thread sleeps in the event wait, so
    ** the CPU stays asleep until input, a timer or app_wake() arrives. */
    got = wait_event(&e, 1000);
  } else if (wait == 0) {
    got = SDL_PollEvent(&e) != 0;
  } else {
    got = wait_event(&e, wait < 0 ? 1000 : wait);
  }
  while (got) {
    if (e.type == SDL_MOUSEBUTTONUP && pressed_now) {
      held_up = e;
      have_held = true;
      ui_redraw();
      break;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN) pressed_now = true;
    switch (e.type) {
      case SDL_QUIT:
        if (tray_take_close()) break;    /* the close button hides to the tray when that is on */
        if (app_can_quit()) app.quit = true;
        break;
      case SDL_APP_TERMINATING:
        app.quit = true;
        break;
      case SDL_APP_WILLENTERBACKGROUND:
      case SDL_APP_DIDENTERBACKGROUND:
        app.background = true;
        break;
      case SDL_APP_DIDENTERFOREGROUND:
        app.background = false;
        gfx_invalidate();
        ui_redraw();
        break;
      case SDL_RENDER_TARGETS_RESET:
      case SDL_RENDER_DEVICE_RESET:
        app_render_reset();
        gfx_invalidate();
        ui_redraw();
        break;
      case SDL_WINDOWEVENT:
        /* the window's pixels may be gone or stale: the next frame is presented
        ** even when it is the same picture */
        if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
            e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
          gfx_invalidate();
          ui_redraw();
        }
        break;
      default:
        break;
    }
    ui_event(&e);
    app_event(&e);
    got = SDL_PollEvent(&e) != 0;
  }
  tray_pump();                           /* the tray icon's clicks, also while hidden */
  media_pump();                          /* the notification's buttons (Android) */
  if (app.background) {                  /* nothing is drawn while hidden; music goes on */
    media_bg_pump();
    return;
  }
  /* the maximum frame rate holds for input too: a frame due early waits for
  ** its turn while events keep arriving (they all reach the widgets) */
  if (ui_needs_frame() && g_pace_until > SDL_GetTicks64()) return;
  idle_log(ui_needs_frame());
  if (!ui_needs_frame()) return;
  int w, h;
  SDL_GetRendererOutputSize(app.ren, &w, &h);
  if ((float)w != ui.w || (float)h != ui.h) ui_update_scale();
  u64 t0 = SDL_GetTicks64();
  gfx_begin(w, h, T.bg);
  ui_begin();
  app_frame();
  ui_end();
  gfx_end();
  pressed_now = false;
  /* presented or not: at most one frame per display frame, and per the maximum frame rate */
  double period = display_period();
  if (ui.max_fps > 0) period = FM_MAX(period, 1000.0 / ui.max_fps);
  g_pace_until = t0 + (u64)(period + 0.5);
}

#ifdef __EMSCRIPTEN__
static void web_frame(void) { frame(); }
#endif

/* ---- screenshots (--shot file.bmp) --------------------------------------- */

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

/* Renders a few frames, then saves the window as a 32-bit BMP. */
static int take_shot(const char *path, int frames) {
  for (int i = 0; i < frames; i++) {
    ui_redraw();
    frame();
    SDL_Delay(16);
  }
  /* draw once more and read back before presenting */
  int w, h;
  SDL_GetRendererOutputSize(app.ren, &w, &h);
  gfx_begin(w, h, T.bg);
  ui_begin();
  app_frame();
  ui_end();
  gfx_render_now();                      /* recorded frames are sent now, to be read back */
  u8 *px = (u8 *)fm_alloc((size_t)w * h * 4);
  int ok = SDL_RenderReadPixels(app.ren, NULL, SDL_PIXELFORMAT_ARGB8888, px, w * 4) == 0;
  SDL_RenderPresent(app.ren);
  FILE *f = ok ? fm_fopen(path, "wb") : NULL;
  if (f) {
    u8 hdr[54] = { 'B', 'M' };
    put32(hdr + 2, 54 + (u32)(w * h * 4));
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, (u32)w);
    put32(hdr + 22, (u32)-h);   /* top-down */
    put16(hdr + 26, 1);
    put16(hdr + 28, 32);
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
  }
  fm_free(px);
  fm_log("shot %s: %dx%d %s", path, w, h, f ? "saved" : "FAILED");
  return f ? 0 : 1;
}

int main(int argc, char **argv) {
  app.argc = argc;
  app.argv = argv;

  char err[512];
  if (!fsdl_load(err, sizeof err)) {
    fatal_box(err);
    return 1;
  }

  /* --shot out.bmp [--size WxH] [--frames N]: render and save, for docs and
  ** visual checks; the app itself reads other options from app.argv. */
  const char *shot = NULL;
  int shot_w = 1180, shot_h = 740, shot_frames = 6;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--selftest") == 0) {
#ifdef FM_ANDROID
      /* no console here: the report goes to <external files>/selftest.txt,
      ** read back with adb (see FmActivity.getArguments) */
      const char *ext = SDL_AndroidGetExternalStoragePath();
      char out[FM_PATH_MAX];
      if (ext && fm_path_join(out, sizeof out, ext, "selftest.txt")) freopen(out, "w", stdout);
#endif
      return test_run_all();
    }
    if (strcmp(argv[i], "--version") == 0) {
      printf("mmcfm %s\n%s\n", FM_VERSION, FM_AUTHOR_LINE);
      return 0;
    }
    if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
    else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) sscanf(argv[++i], "%dx%d", &shot_w, &shot_h);
    else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) shot_frames = atoi(argv[++i]);
  }
  /* --perf: the measured tour of the screens (fperf.c) */
  bool perf = !shot && perf_wanted(argc, argv);
  if (perf) {
    if (!perf_prepare(&app.argc, &app.argv)) return 1;
    perf_mark("start");
  }

  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
  /* Android: leaving the screen must not freeze the main loop nor pause the
  ** sound; music and a background video keep playing (fmedia.c) */
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
  SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO, "0");
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  SDL_SetHint(SDL_HINT_APP_NAME, "MMC File Manager");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    fm_snprintf(err, sizeof err, "Could not start SDL: %s", SDL_GetError());
    fatal_box(err);
    return 1;
  }
  if (perf) perf_mark("sdl");

  Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef FM_MOBILE
  flags |= SDL_WINDOW_FULLSCREEN;
#endif
  if (shot || perf) flags &= ~(Uint32)(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
  app.win = SDL_CreateWindow("MMC File Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             shot_w, shot_h, flags | SDL_WINDOW_HIDDEN);
  if (!app.win) {
    fm_snprintf(err, sizeof err, "Could not create the window: %s", SDL_GetError());
    fatal_box(err);
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(app.win, 360, 400);
  if (!create_renderer()) {
    fm_snprintf(err, sizeof err, "Could not create a renderer: %s", SDL_GetError());
    fatal_box(err);
    SDL_DestroyWindow(app.win);
    SDL_Quit();
    return 1;
  }
  app.ev_wake = SDL_RegisterEvents(1);
  wait_init();
  if (perf) perf_mark("window");

  if (!font_init()) {
    fatal_box("The built-in fonts could not be loaded.");
    return 1;
  }
  if (perf) perf_mark("fonts");
  ui_init(app.win);
  app_init();           /* restores window size, theme, panels */
  if (perf) perf_mark("app_init");
  if (shot || perf) {
    SDL_SetWindowSize(app.win, shot_w, shot_h);   /* app_init may restore another size */
  }
  SDL_ShowWindow(app.win);
  ui_update_scale();

  if (shot) {
    int rc = take_shot(shot, shot_frames);
    app_shutdown();
    SDL_Quit();
    return rc;
  }

  int rc = 0;
#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(web_frame, 0, 1);
#else
  if (perf) rc = perf_run(frame);
  else while (!app.quit) frame();
#endif

  app_shutdown();
  wait_shutdown();
  ui_shutdown();
  font_shutdown();
  gfx_shutdown();
  SDL_DestroyRenderer(app.ren);
  SDL_DestroyWindow(app.win);
  SDL_Quit();
  return rc;
}

void app_wake_at(const char *file, int line, int frames) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = app.ev_wake;
  e.user.code = line;
  e.user.data1 = (void *)file;
  e.user.data2 = (void *)(intptr_t)frames;
  SDL_PushEvent(&e);
}
