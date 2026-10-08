/* fview_vid.c -- video player: MPEG-1 built in, other formats through FFmpeg.
**
** Design decisions:
**   - A worker thread owns the decoder. Video frames go into a queue of at
**     most three YUV frames; audio goes into an SDL_AudioStream that the
**     audio callback drains. Memory is bounded by the queue whatever the
**     length of the file.
**   - The master clock is the audio actually played (frames handed to the
**     device since the last seek, plus the timestamp of the first block).
**     When audio stops flowing (no audio track, starved, at the end) the
**     clock runs on wall time, so a full video queue can never deadlock
**     against an empty audio buffer.
**   - Frames are uploaded with SDL_UpdateYUVTexture into an IYUV texture:
**     no colour conversion on the CPU. Late frames are dropped.
**   - Controls auto-hide while playing; the screen saver is held off only
**     while playing, and fullscreen is undone when the viewer closes.
*/
#include "fview_int.h"
#include "fplat.h"
#include "fconf.h"
#include "fdec_vid.h"

#define QN 3
#define AUDIO_AHEAD 1.0      /* seconds of decoded audio kept queued */

enum { VS_OPENING = 0, VS_READY, VS_ERROR };

typedef struct VFrame {
  double t;
  int w, h;
  u8 *plane[3];
  int stride[3];
  size_t cap;
} VFrame;

static struct {
  bool open;
  char path[FM_PATH_MAX];
  char title[256];
  /* worker */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  bool quit;
  int state;
  FmErr err;
  FmVidInfo info;
  bool started;            /* main thread set up audio; decoding may begin */
  bool eof;
  VFrame q[QN];
  int qhead, qcount;
  bool seek_req;
  double seek_to;
  /* audio */
  SDL_AudioDeviceID dev;
  int dev_rate;
  SDL_AudioStream *ast;
  u64 out_frames;          /* device frames played */
  u64 base_out;
  double base_t;
  bool base_set;
  u64 last_audio_ms;
  float volume;
  /* clock */
  double clock;
  bool clock_set;
  bool paused, ended;
  /* main thread */
  SDL_Texture *tex;
  int tw, th;
  double shown_t;
  bool have_frame;
  FmViewChrome chrome;
  bool fullscreen_set;
  bool cursor_hidden;
  bool saver_off;
  bool seek_drag;
  float seek_t;
  bool tap_pending, ignore_click;
  u64 tap_t;
  float tap_x;
  int skip_flash;          /* -1 / +1 */
  u64 skip_flash_t;
  bool info_open;
  u64 vol_shown_until;
} V;

/* ---- audio callback ------------------------------------------------------------- */

static void SDLCALL vid_audio_cb(void *u, Uint8 *stream, int len) {
  FM_UNUSED(u);
  SDL_LockMutex(V.mx);
  int got = V.ast ? SDL_AudioStreamGet(V.ast, stream, len) : 0;
  if (got < 0) got = 0;
  if (got < len) memset(stream + got, 0, (size_t)(len - got));
  int frames = got / (int)(sizeof(float) * 2);
  if (frames > 0) {
    V.out_frames += (u64)frames;
    V.last_audio_ms = SDL_GetTicks64();
  }
  float vol = V.volume * V.volume;
  SDL_UnlockMutex(V.mx);
  float *f = (float *)stream;
  for (int i = 0; i < frames * 2; i++) f[i] *= vol;
}

/* ---- worker ------------------------------------------------------------------- */

static void copy_frame(VFrame *d, const FmVidFrame *s) {
  int cw = (s->w + 1) / 2, ch = (s->h + 1) / 2;
  size_t need = (size_t)s->w * s->h + (size_t)cw * ch * 2;
  if (need > d->cap) {
    fm_free(d->plane[0]);
    d->plane[0] = (u8 *)fm_alloc(need);
    d->cap = need;
  }
  d->plane[1] = d->plane[0] + (size_t)s->w * s->h;
  d->plane[2] = d->plane[1] + (size_t)cw * ch;
  d->stride[0] = s->w;
  d->stride[1] = d->stride[2] = cw;
  for (int y = 0; y < s->h; y++) memcpy(d->plane[0] + (size_t)y * s->w, s->plane[0] + (size_t)y * s->stride[0], (size_t)s->w);
  for (int y = 0; y < ch; y++) {
    memcpy(d->plane[1] + (size_t)y * cw, s->plane[1] + (size_t)y * s->stride[1], (size_t)cw);
    memcpy(d->plane[2] + (size_t)y * cw, s->plane[2] + (size_t)y * s->stride[2], (size_t)cw);
  }
  d->w = s->w;
  d->h = s->h;
  d->t = s->t;
}

static double audio_queued(void) {
  if (!V.ast || V.dev_rate <= 0) return 0;
  return (double)SDL_AudioStreamAvailable(V.ast) / (sizeof(float) * 2) / V.dev_rate;
}

static int worker(void *u) {
  FM_UNUSED(u);
  FmErr err;
  FmVid *vid = vid_open(V.path, 0, &err);
  SDL_LockMutex(V.mx);
  if (!vid) {
    V.state = VS_ERROR;
    V.err = err;
    SDL_UnlockMutex(V.mx);
    app_wake();
    return 0;
  }
  V.info = *vid_info(vid);
  V.state = VS_READY;
  SDL_UnlockMutex(V.mx);
  app_wake();
  SDL_LockMutex(V.mx);
  while (!V.quit && !V.started) SDL_CondWait(V.cv, V.mx);
  while (!V.quit) {
    if (V.seek_req) {
      double t = V.seek_to;
      V.seek_req = false;
      SDL_UnlockMutex(V.mx);
      vid_seek(vid, t);
      SDL_LockMutex(V.mx);
      V.qhead = V.qcount = 0;
      if (V.ast) SDL_AudioStreamClear(V.ast);
      V.base_set = false;
      V.eof = false;
      V.ended = false;
      V.clock_set = false;
      continue;
    }
    if (V.eof) { SDL_CondWait(V.cv, V.mx); continue; }
    bool vfull = V.qcount >= QN;
    bool afull = V.info.has_audio && audio_queued() > AUDIO_AHEAD;
    if ((V.info.has_video && vfull) || (afull && (!V.info.has_video || V.qcount > 0))) {
      SDL_CondWaitTimeout(V.cv, V.mx, 20);
      continue;
    }
    SDL_UnlockMutex(V.mx);
    FmVidFrame vf;
    FmVidPcm pc;
    int ev = vid_decode(vid, &vf, &pc);
    SDL_LockMutex(V.mx);
    if (V.seek_req) continue;
    if (ev == VID_EV_VIDEO) {
      if (vf.w > 0 && vf.h > 0 && V.qcount < QN) {
        VFrame *d = &V.q[(V.qhead + V.qcount) % QN];
        copy_frame(d, &vf);
        V.qcount++;
        if (!V.clock_set || V.paused) { SDL_UnlockMutex(V.mx); app_wake(); SDL_LockMutex(V.mx); }
      }
    } else if (ev == VID_EV_AUDIO) {
      if (V.ast && pc.frames > 0 && pc.rate == V.info.rate && pc.channels == V.info.channels) {
        if (!V.base_set) {
          V.base_set = true;
          V.base_t = pc.t >= 0 ? pc.t : V.clock;
          V.base_out = V.out_frames + (u64)(audio_queued() * V.dev_rate);
        }
        SDL_AudioStreamPut(V.ast, pc.pcm, pc.frames * pc.channels * (int)sizeof(float));
      }
    } else {
      V.eof = true;
      if (V.ast) SDL_AudioStreamFlush(V.ast);
      SDL_UnlockMutex(V.mx);
      app_wake();
      SDL_LockMutex(V.mx);
    }
  }
  SDL_UnlockMutex(V.mx);
  vid_close(vid);
  return 0;
}

/* ---- control ------------------------------------------------------------------- */

static void screensaver(bool off) {
  if (off == V.saver_off) return;
  V.saver_off = off;
  if (off) SDL_DisableScreenSaver();
  else SDL_EnableScreenSaver();
}

static void set_paused(bool p) {
  if (V.ended && !p) {
    /* replay */
    SDL_LockMutex(V.mx);
    V.seek_req = true;
    V.seek_to = 0;
    V.ended = false;
    SDL_CondBroadcast(V.cv);
    SDL_UnlockMutex(V.mx);
  }
  V.paused = p;
  if (V.dev) SDL_PauseAudioDevice(V.dev, p ? 1 : 0);
  ui_redraw();
}

static void seek_to(double t) {
  double d = V.info.duration;
  if (d > 0) t = FM_CLAMP(t, 0.0, FM_MAX(0.0, d - 0.1));
  if (t < 0) t = 0;
  SDL_LockMutex(V.mx);
  V.seek_req = true;
  V.seek_to = t;
  V.qhead = V.qcount = 0;
  if (V.ast) SDL_AudioStreamClear(V.ast);
  V.base_set = false;
  V.eof = false;
  V.ended = false;
  V.clock = t;
  V.clock_set = false;
  SDL_CondBroadcast(V.cv);
  SDL_UnlockMutex(V.mx);
  ui_redraw();
}

static void set_volume(float v) {
  V.volume = FM_CLAMP(v, 0.0f, 1.0f);
  conf.volume = V.volume;
  V.vol_shown_until = ui.now + 900;
}

static void toggle_fullscreen(void) {
#ifndef FM_MOBILE
  if (!app.win) return;
  bool fs = (SDL_GetWindowFlags(app.win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_SetWindowFullscreen(app.win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  V.fullscreen_set = !fs;
#endif
}

/* Main thread, once the decoder is open: audio device and stream. */
static void start_playback(void) {
  if (V.info.has_audio && V.info.rate > 0) {
    if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
      SDL_AudioSpec want, have;
      memset(&want, 0, sizeof want);
      want.freq = V.info.rate;
      want.format = AUDIO_F32SYS;
      want.channels = 2;
      want.samples = 1024;
      want.callback = vid_audio_cb;
      V.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
      if (V.dev) {
        V.dev_rate = have.freq;
        SDL_LockMutex(V.mx);
        V.ast = SDL_NewAudioStream(AUDIO_F32SYS, (Uint8)V.info.channels, V.info.rate, AUDIO_F32SYS, 2, V.dev_rate);
        SDL_UnlockMutex(V.mx);
        if (V.ast) SDL_PauseAudioDevice(V.dev, 0);
      }
    }
  }
  SDL_LockMutex(V.mx);
  V.started = true;
  SDL_CondBroadcast(V.cv);
  SDL_UnlockMutex(V.mx);
}

/* ---- open / close ------------------------------------------------------------------ */

static void vid_view_close(void) {
  if (!V.open) return;
  if (V.thr) {
    SDL_LockMutex(V.mx);
    V.quit = true;
    SDL_CondBroadcast(V.cv);
    SDL_UnlockMutex(V.mx);
    SDL_WaitThread(V.thr, NULL);
  }
  if (V.dev) SDL_CloseAudioDevice(V.dev);
  if (V.ast) SDL_FreeAudioStream(V.ast);
  for (int i = 0; i < QN; i++) fm_free(V.q[i].plane[0]);
  if (V.tex) SDL_DestroyTexture(V.tex);
  screensaver(false);
  if (V.cursor_hidden) SDL_ShowCursor(SDL_ENABLE);
#ifndef FM_MOBILE
  if (V.fullscreen_set && app.win) SDL_SetWindowFullscreen(app.win, 0);
#endif
  if (V.cv) SDL_DestroyCond(V.cv);
  if (V.mx) SDL_DestroyMutex(V.mx);
  memset(&V, 0, sizeof V);
}

static bool vid_view_open(const char *path, const char *const *list, int n, int index) {
  FM_UNUSED(list); FM_UNUSED(n); FM_UNUSED(index);
  vid_view_close();
  memset(&V, 0, sizeof V);
  V.open = true;
  fm_strlcpy(V.path, path, sizeof V.path);
  fm_strlcpy(V.title, fm_path_base(path), sizeof V.title);
  V.volume = conf.volume > 0 ? conf.volume : 0.8f;
  V.mx = SDL_CreateMutex();
  V.cv = SDL_CreateCond();
  audio_pause();
  if (V.mx && V.cv) V.thr = fm_thread_create(worker, "video", NULL);
  if (!V.thr) { V.state = VS_ERROR; V.err = FM_ERR_NOMEM; }
  view_chrome_poke(&V.chrome);
  return true;
}

/* ---- frame ---------------------------------------------------------------------- */

static void update_clock(void) {
  if (V.paused || !V.clock_set) return;
  SDL_LockMutex(V.mx);
  bool flowing = V.dev && V.base_set && SDL_GetTicks64() - V.last_audio_ms < 150;
  double at = V.base_t + ((double)V.out_frames - (double)V.base_out) / FM_MAX(1, V.dev_rate);
  SDL_UnlockMutex(V.mx);
  if (flowing && V.out_frames >= V.base_out) V.clock = at;
  else V.clock += ui.dt;
}

static void present_frame(void) {
  SDL_LockMutex(V.mx);
  if (!V.clock_set && V.qcount > 0) {
    double t = V.q[V.qhead].t;
    if (V.base_set && V.base_t < t) t = V.base_t;
    V.clock = t;
    V.clock_set = true;
  } else if (!V.clock_set && !V.info.has_video && V.base_set) {
    V.clock = V.base_t;
    V.clock_set = true;
  }
  /* drop frames that are already late */
  while (V.qcount > 1 && V.q[(V.qhead + 1) % QN].t <= V.clock) {
    V.qhead = (V.qhead + 1) % QN;
    V.qcount--;
  }
  VFrame *f = NULL;
  if (V.qcount > 0 && (V.q[V.qhead].t <= V.clock + 0.008 || !V.have_frame)) f = &V.q[V.qhead];
  SDL_UnlockMutex(V.mx);
  if (!f) return;
  /* the worker never writes the head slot while it is queued */
  if (!V.tex || V.tw != f->w || V.th != f->h) {
    if (V.tex) SDL_DestroyTexture(V.tex);
    V.tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->w, f->h);
    V.tw = f->w;
    V.th = f->h;
    if (V.tex) SDL_SetTextureScaleMode(V.tex, SDL_ScaleModeLinear);
  }
  if (V.tex) {
    SDL_UpdateYUVTexture(V.tex, NULL, f->plane[0], f->stride[0], f->plane[1], f->stride[1], f->plane[2], f->stride[2]);
    V.have_frame = true;
    V.shown_t = f->t;
  }
  SDL_LockMutex(V.mx);
  V.qhead = (V.qhead + 1) % QN;
  V.qcount--;
  SDL_CondBroadcast(V.cv);
  SDL_UnlockMutex(V.mx);
}

static void info_card(void) {
  char dims[48], dur[32], fps[32], a[64];
  fm_snprintf(dims, sizeof dims, "%d \xC3\x97 %d", V.tw, V.th);
  view_fmt_time(V.info.duration, dur, sizeof dur);
  fm_snprintf(fps, sizeof fps, "%.3g fps", V.info.fps);
  if (V.info.has_audio) fm_snprintf(a, sizeof a, "%s, %d Hz", V.info.acodec, V.info.rate);
  else fm_strlcpy(a, "none", sizeof a);
  const char *keys[] = { "Name", "Video", "Size", "Frame rate", "Length", "Audio", "Decoder" };
  const char *vals[] = { V.title, V.info.vcodec, dims, V.info.fps > 0 ? fps : "unknown",
                         V.info.duration > 0 ? dur : "unknown", a, V.info.backend };
  view_info_dialog(ui_id("vid.info"), "Video info", keys, vals, FM_COUNT(keys), &V.info_open);
}

static void vid_seek_bar(FmRect r) {
  u32 id = ui_id("vid.seek");
  double dur = V.info.duration;
  int f = dur > 0 ? ui_hit(id, rect_inset2(r, 0, -DP(8))) : 0;
  float t = dur > 0 ? (float)(V.clock / dur) : 0;
  if (f & UI_HELD) {
    ui.drag_owner = id;
    V.seek_drag = true;
    V.seek_t = FM_CLAMP((ui.mx - r.x) / r.w, 0.0f, 1.0f);
    view_chrome_poke(&V.chrome);
  }
  if (V.seek_drag) {
    t = V.seek_t;
    if (!ui.down) { V.seek_drag = false; seek_to(V.seek_t * dur); }
  }
  t = FM_CLAMP(t, 0.0f, 1.0f);
  float th = DP(4), cy = r.y + r.h * 0.5f;
  gfx_rrect(FM_RECT(r.x, cy - th * 0.5f, r.w, th), th * 0.5f, FM_RGBA(255, 255, 255, 70));
  gfx_rrect(FM_RECT(r.x, cy - th * 0.5f, r.w * t, th), th * 0.5f, T.accent);
  if (dur > 0) gfx_circle(r.x + r.w * t, cy, DP((f & (UI_HELD | UI_HOVER)) || V.seek_drag ? 8 : 6), T.accent);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
}

enum { VM_OPEN = 1, VM_SHARE, VM_INFO };

static void vid_view_frame(FmRect area) {
  gfx_rect(area, VIEW_BG);
  int state;
  SDL_LockMutex(V.mx);
  state = V.state;
  SDL_UnlockMutex(V.mx);
  if (state == VS_READY && !V.started) start_playback();
  bool ready = state == VS_READY && V.started;

  if (ready) {
    update_clock();
    present_frame();
    SDL_LockMutex(V.mx);
    bool done = V.eof && V.qcount == 0 && (!V.ast || SDL_AudioStreamAvailable(V.ast) == 0);
    SDL_UnlockMutex(V.mx);
    if (done && !V.ended && V.clock_set) {
      V.ended = true;
      V.paused = true;
      if (V.dev) SDL_PauseAudioDevice(V.dev, 1);
      view_chrome_poke(&V.chrome);
    }
  }
  bool playing = ready && !V.paused && !V.ended;
  screensaver(playing);
  float chrome = view_chrome(&V.chrome, ui_id("vid.chrome"), playing && !V.seek_drag);

  /* picture, letterboxed with the pixel aspect */
  FmRect pic = area;
  if (V.tex && V.tw > 0 && V.th > 0) {
    double sar = V.info.sar > 0.1 && V.info.sar < 10 ? V.info.sar : 1.0;
    double dw = V.tw * sar, dh = V.th;
    double sc = FM_MIN(area.w / dw, area.h / dh);
    float w = (float)(dw * sc), h = (float)(dh * sc);
    pic = FM_RECT(area.x + (area.w - w) * 0.5f, area.y + (area.h - h) * 0.5f, w, h);
    gfx_tex(V.tex, NULL, pic, FM_HEX(0xFFFFFF));
  } else if (state == VS_OPENING || (ready && V.info.has_video && !V.have_frame)) {
    ui_spinner(rect_center(area, DP(40), DP(40)), VIEW_FG2);
  } else if (ready && !V.info.has_video) {
    float is = DP(96);
    icon_draw(IC_MUSIC, rect_center(area, is, is), VIEW_FG2);
  }
  if (state == VS_ERROR) {
    bool need_ff = V.err == FM_ERR_UNSUPPORTED;
    view_message(area, IC_VIDEO, need_ff ? "This video needs FFmpeg" : "Cannot play this video",
                 need_ff ? "Only MPEG-1 is built in. Install FFmpeg 4 to 8 (or put its libraries next to the "
                           "app) to play this format, or open it with the system player."
                         : "The file could not be decoded.",
                 VIEW_FG, VIEW_FG2);
    float bw = DP(240), bh = DP(ui.touch_mode ? 46 : 38);
    if (ui_button(ui_id("vid.ext"), FM_RECT(area.x + (area.w - bw) * 0.5f, area.y + area.h * 0.5f + DP(120), bw, bh),
                  IC_OPEN_WITH, "Open with system player", UI_BTN_TONAL))
      plat_open_external(V.path);
  }

  /* keys */
  if (ready) {
    if (ui_key(SDLK_SPACE, 0) || ui_key(SDLK_k, 0) || ui_key(SDLK_AUDIOPLAY, 0)) set_paused(!V.paused);
    if (ui_key(SDLK_RIGHT, 0)) { seek_to(V.clock + 10); view_chrome_poke(&V.chrome); }
    if (ui_key(SDLK_LEFT, 0)) { seek_to(V.clock - 10); view_chrome_poke(&V.chrome); }
    if (ui_key(SDLK_UP, 0)) set_volume(V.volume + 0.05f);
    if (ui_key(SDLK_DOWN, 0)) set_volume(V.volume - 0.05f);
    if (ui_key(SDLK_m, 0)) set_volume(V.volume > 0 ? 0 : 0.8f);
  }
  if (ui_key(SDLK_f, 0) || ui_key(SDLK_F11, 0)) toggle_fullscreen();

  /* taps: touch toggles bars, double tap on the sides skips 10 s;
  ** mouse click plays/pauses, double click toggles fullscreen */
  u32 vid = ui_id("vid.area");
  FmRect hv = area;
  if (chrome > 0.5f) { rect_cut_top(&hv, ui.m.bar_h); rect_cut_bottom(&hv, DP(ui.touch_mode ? 120 : 96)); }
  int f = ui_hit(vid, hv);
  bool touchish = ui.from_touch || ui.touch_mode;
  if ((f & UI_DCLICK) && ready) {
    V.tap_pending = false;
    V.ignore_click = true;
    if (touchish) {
      float rel = (ui.mx - area.x) / area.w;
      if (rel < 0.35f) { seek_to(V.clock - 10); V.skip_flash = -1; V.skip_flash_t = ui.now; }
      else if (rel > 0.65f) { seek_to(V.clock + 10); V.skip_flash = 1; V.skip_flash_t = ui.now; }
      else set_paused(!V.paused);
    } else {
      toggle_fullscreen();
    }
  }
  if (f & UI_CLICK) {
    if (V.ignore_click) V.ignore_click = false;
    else { V.tap_pending = true; V.tap_t = ui.now; V.tap_x = ui.mx; view_wake_in(300); }
  }
  if (V.tap_pending && ui.now - V.tap_t >= 280) {
    V.tap_pending = false;
    if (touchish || !ready) view_chrome_toggle(&V.chrome);
    else set_paused(!V.paused);
  }

  /* skip ripple */
  if (V.skip_flash && ui.now - V.skip_flash_t < 600) {
    float a = 1.0f - (float)(ui.now - V.skip_flash_t) / 600.0f;
    float cx = V.skip_flash < 0 ? area.x + area.w * 0.18f : area.x + area.w * 0.82f;
    float cy = area.y + area.h * 0.5f;
    gfx_circle(cx, cy, DP(56), col_alpha(VIEW_SCRIM, a));
    font_draw_center(FONT_BOLD, ui.m.font, FM_RECT(cx - DP(50), cy - DP(14), DP(100), DP(28)),
                     V.skip_flash < 0 ? "-10 s" : "+10 s", col_alpha(VIEW_FG, a));
    ui_animate();
  } else {
    V.skip_flash = 0;
  }

  /* cursor hides with the bars on desktop */
  bool hide_cursor = !ui.touch_mode && playing && chrome < 0.02f;
  if (hide_cursor != V.cursor_hidden) {
    SDL_ShowCursor(hide_cursor ? SDL_DISABLE : SDL_ENABLE);
    V.cursor_hidden = hide_cursor;
  }

  /* top bar */
  char sub[96];
  sub[0] = 0;
  if (ready && V.tw > 0) fm_snprintf(sub, sizeof sub, "%d \xC3\x97 %d  \xC2\xB7  %s", V.tw, V.th, V.info.backend);
  FmRect act;
  if (view_topbar(area, VIEW_BAR_MEDIA, chrome, V.title, sub, 2, &act)) { app_close_viewer(); return; }
  u32 mid = ui_id("vid.menu");
  if (view_bar_btn(&act, ui_id("vid.more"), IC_MORE, "More", VIEW_BAR_MEDIA, chrome, false)) {
    FmMenuItem items[] = {
      { VM_INFO, IC_INFO, "Video info", NULL, ready ? 0 : UI_MI_DISABLED },
      { VM_SHARE, IC_SHARE, "Share", NULL, 0 },
      { VM_OPEN, IC_OPEN_WITH, "Open with system player", NULL, 0 },
    };
    ui_menu_open(mid, act.x + act.w, area.y + ui.m.bar_h, items, FM_COUNT(items));
  }
#ifndef FM_MOBILE
  if (view_bar_btn(&act, ui_id("vid.fs"), IC_FULLSCREEN, "Fullscreen (F)", VIEW_BAR_MEDIA, chrome, V.fullscreen_set))
    toggle_fullscreen();
#endif
  switch (ui_menu_result(mid)) {
    case VM_INFO: V.info_open = true; break;
    case VM_SHARE: plat_share(V.path); break;
    case VM_OPEN: set_paused(true); plat_open_external(V.path); break;
    default: break;
  }

  /* bottom controls */
  if (ready && chrome > 0.01f) {
    float bh = DP(ui.touch_mode ? 120 : 96);
    FmRect bot = { area.x, area.y + area.h - bh, area.w, bh };
    view_bottom_scrim(FM_RECT(bot.x, bot.y - DP(30), bot.w, bh + DP(30)), chrome);
    FmRect c = rect_inset2(bot, DP(16), DP(8));
    FmRect seek = rect_cut_top(&c, DP(28));
    FmColor fg = col_alpha(VIEW_FG, chrome), fg2 = col_alpha(VIEW_FG2, chrome);
    vid_seek_bar(seek);
    float s = FM_MAX(ui.m.hit, DP(40));
    FmRect row = rect_center(c, c.w, FM_MIN(c.h, s));
    FmRect b = rect_cut_left(&row, s);
    if (ui_icon_btn(ui_id("vid.play"), b, V.paused || V.ended ? IC_PLAY : IC_PAUSE, fg, "Play / pause (Space)"))
      set_paused(!V.paused);
    if (ui_icon_btn(ui_id("vid.back10"), rect_cut_left(&row, s), IC_ARROW_LEFT, fg, "Back 10 s (Left)")) seek_to(V.clock - 10);
    if (ui_icon_btn(ui_id("vid.fwd10"), rect_cut_left(&row, s), IC_ARROW_RIGHT, fg, "Forward 10 s (Right)")) seek_to(V.clock + 10);
    char tb[64], a[24], d[24];
    view_fmt_time(V.seek_drag ? V.seek_t * V.info.duration : V.clock, a, sizeof a);
    view_fmt_time(V.info.duration, d, sizeof d);
    if (V.info.duration > 0) fm_snprintf(tb, sizeof tb, "%s / %s", a, d);
    else fm_strlcpy(tb, a, sizeof tb);
    rect_cut_left(&row, DP(8));
    float tw = font_width(FONT_REGULAR, ui.m.font_small, tb, -1);
    ui_label(rect_cut_left(&row, tw + DP(4)), tb, FONT_REGULAR, ui.m.font_small, fg2, UI_LEFT);
    if (V.info.has_audio) {
      FmRect vb = rect_cut_right(&row, s);
      if (ui_icon_btn(ui_id("vid.mute"), vb, V.volume <= 0.001f ? IC_MUTE : IC_VOLUME, fg, "Mute (M)"))
        set_volume(V.volume > 0.001f ? 0 : 0.8f);
      if (!ui.touch_mode && row.w > DP(140)) {
        FmRect sl = rect_cut_right(&row, DP(110));
        float v = V.volume;
        if (ui_slider(ui_id("vid.vol"), rect_inset2(sl, 0, FM_MAX(0.0f, (sl.h - DP(24)) * 0.5f)), &v, 0, 1)) set_volume(v);
      }
    }
  }
  /* big play button when paused on touch screens */
  if (ready && (V.paused || V.ended) && ui.touch_mode && chrome > 0.01f) {
    float bs = DP(72);
    FmRect b = rect_center(area, bs, bs);
    gfx_circle(b.x + bs * 0.5f, b.y + bs * 0.5f, bs * 0.5f, col_alpha(VIEW_SCRIM, chrome));
    if (ui_icon_btn(ui_id("vid.bigplay"), b, IC_PLAY, col_alpha(VIEW_FG, chrome), "Play")) set_paused(false);
  }
  /* volume badge */
  if (ui.now < V.vol_shown_until) {
    char vb[24];
    fm_snprintf(vb, sizeof vb, "Volume %d%%", (int)(V.volume * 100 + 0.5f));
    float w = font_width(FONT_BOLD, ui.m.font_small, vb, -1) + DP(24), h = DP(30);
    FmRect r = { area.x + (area.w - w) * 0.5f, area.y + ui.m.bar_h + DP(16), w, h };
    gfx_rrect(r, h * 0.5f, VIEW_SCRIM);
    font_draw_center(FONT_BOLD, ui.m.font_small, r, vb, VIEW_FG);
    view_wake_in((u32)(V.vol_shown_until - ui.now) + 10);
  }
  if (V.info_open && ready) info_card();

  if (playing) ui_animate();
  if (view_key_back()) {
#ifndef FM_MOBILE
    if (app.win && (SDL_GetWindowFlags(app.win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) {
      toggle_fullscreen();
      return;
    }
#endif
    app_close_viewer();
  }
}

const FmViewer g_view_video = { "video", vid_view_open, vid_view_frame, vid_view_close };
