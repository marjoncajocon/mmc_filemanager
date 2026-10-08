/* fview_aud.c -- music player: playlist, gapless playback, visualizer, mini player.
**
** Design decisions:
**   - One decoder thread streams the playlist into a float ring buffer at
**     the device rate (about 0.75 s); the SDL audio callback copies from
**     the ring, applies the volume and keeps a copy of the last samples for
**     the visualizer. Nothing is decoded in the callback.
**   - Gapless: when a track ends the decoder opens the next one at once and
**     drops a marker at the ring position where it starts; the callback
**     switches the "now playing" track when it reaches the marker, so the
**     title changes exactly when the music does.
**   - Each track gets its own SDL_AudioStream (rate and channel
**     conversion), flushed at the end so the tail is not lost.
**   - The player outlives its viewer: closing the viewer only hides it and
**     audio_mini_* draw a compact bar. While hidden the bar refreshes once a
**     second through a timer, not every frame.
**   - Tags and the cover are read by the decoder thread when a track is
**     opened; the cover is downscaled there and uploaded on the main thread.
**   - The visualizer (fviz.c) gets the last 1024 samples heard; its FFT
**     runs on the main thread only while the full player is visible, and
**     redrawing stops once the bars have fallen after a pause. With the
**     visualizer off, the player refreshes a few times a second instead of
**     every frame.
**   - The equalizer (feq.c) runs in the callback on what is about to play,
**     so changes are heard at once; its filters are redesigned on the main
**     thread only when the settings change.
*/
#include "fview_int.h"
#include "flib.h"
#include "fplat.h"
#include "fconf.h"
#include "fdec_aud.h"
#include "fdec_img.h"
#include "fviz.h"
#include "feq.h"

#define RING_SEC 0.75
#define CHUNK 2048
#define COVER_PX 512

enum { REQ_NONE = 0, REQ_PLAY, REQ_SEEK };
enum { REP_OFF = 0, REP_ALL, REP_ONE };

typedef struct Marker { u64 at; int track; double sec; } Marker;

typedef struct TrackMeta {
  int idx;                    /* playlist index, -1 = unused */
  char title[AUD_META_TEXT], artist[AUD_META_TEXT], album[AUD_META_TEXT];
  FmImage cover;              /* waiting for upload */
  double dur;
  char codec[24];
  int rate, ch;
} TrackMeta;

static struct {
  bool active;                /* a playlist is loaded */
  bool viewer_open;
  /* playlist */
  FmArena arena;
  char **paths;
  int n;
  int *order;
  bool shuffle;
  int repeat;
  /* device */
  SDL_AudioDeviceID dev;
  int rate;
  /* ring (stereo float at device rate) */
  float *ring;
  u32 ring_frames;
  u64 r_read, r_write;
  Marker marks[8];
  int nmarks;
  int heard;                  /* playlist index being heard */
  u64 base_frame;
  double base_sec;
  bool dec_done;              /* decoder reached the end of the playlist */
  bool ended;
  /* decoder thread */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  bool quit;
  int req;
  int req_index;
  double req_seek;
  bool paused;
  float volume, vol_before_mute;
  TrackMeta meta[3];
  int fail_streak;
  char error[160];
  /* visualizer: written by the callback, copied out on the main thread */
  float viz[VIZ_FFT];
  int viz_pos;
  float viz_snap[VIZ_FFT];
  bool viz_open;              /* settings panel shown */
  FmEq eq;                    /* equalizer state (callback, under mx) */
  /* main thread */
  SDL_Texture *cover_tex;
  int cover_idx;
  int cover_w, cover_h;
  bool show_list;
  FmScroll list_scroll;
  bool seek_drag;
  float seek_t;
  bool info_open;
  FmViewChrome chrome;
} P;

/* ---- helpers ------------------------------------------------------------------- */

static void lock(void) { SDL_LockMutex(P.mx); }
static void unlock(void) { SDL_UnlockMutex(P.mx); }

static int order_pos(int idx) {
  for (int i = 0; i < P.n; i++)
    if (P.order[i] == idx) return i;
  return 0;
}

/* Next playlist index after `idx`; -1 at the end. Called with the lock held. */
static int next_index(int idx, bool user) {
  if (P.n <= 0) return -1;
  if (!user && P.repeat == REP_ONE) return idx;
  int p = order_pos(idx) + 1;
  if (p >= P.n) {
    if (P.repeat == REP_ALL || user) p = 0;
    else return -1;
  }
  return P.order[p];
}

static int prev_index(int idx) {
  if (P.n <= 0) return -1;
  int p = order_pos(idx) - 1;
  if (p < 0) p = P.n - 1;
  return P.order[p];
}

static void make_order(int first) {
  for (int i = 0; i < P.n; i++) P.order[i] = i;
  if (!P.shuffle) return;
  u32 seed;
  plat_random(&seed, sizeof seed);
  for (int i = P.n - 1; i > 0; i--) {
    seed = seed * 1664525u + 1013904223u;
    int j = (int)((seed >> 8) % (u32)(i + 1));
    int t = P.order[i]; P.order[i] = P.order[j]; P.order[j] = t;
  }
  /* the current track stays first */
  int p = order_pos(first);
  int t = P.order[0]; P.order[0] = P.order[p]; P.order[p] = t;
}

static TrackMeta *meta_for(int idx) {
  for (int i = 0; i < 3; i++)
    if (P.meta[i].idx == idx) return &P.meta[i];
  return NULL;
}

static const char *display_name(int idx, char *buf, size_t cap) {
  const char *b = fm_path_base(P.paths[idx]);
  fm_strlcpy(buf, b, cap);
  char *dot = strrchr(buf, '.');
  if (dot && dot != buf) *dot = 0;
  return buf;
}

/* ---- audio callback --------------------------------------------------------------- */

static void SDLCALL audio_cb(void *u, Uint8 *stream, int len) {
  FM_UNUSED(u);
  float *out = (float *)stream;
  int frames = len / (int)(sizeof(float) * 2);
  SDL_LockMutex(P.mx);
  u64 avail = P.r_write - P.r_read;
  int n = (int)FM_MIN((u64)frames, avail);
  float vol = P.volume * P.volume;          /* perceptual curve */
  for (int i = 0; i < n; i++) {
    u32 at = (u32)((P.r_read + (u64)i) % P.ring_frames);
    out[i * 2] = P.ring[at * 2];
    out[i * 2 + 1] = P.ring[at * 2 + 1];
  }
  eq_process(&P.eq, out, n);
  for (int i = 0; i < n; i++) {
    float l = out[i * 2], r = out[i * 2 + 1];
    out[i * 2] = l * vol;
    out[i * 2 + 1] = r * vol;
    P.viz[P.viz_pos] = (l + r) * 0.5f;
    P.viz_pos = (P.viz_pos + 1) % VIZ_FFT;
  }
  if (n < frames) memset(out + n * 2, 0, sizeof(float) * 2 * (size_t)(frames - n));
  u64 end = P.r_read + (u64)n;
  while (P.nmarks > 0 && P.marks[0].at <= end) {
    P.heard = P.marks[0].track;
    P.base_frame = P.marks[0].at;
    P.base_sec = P.marks[0].sec;
    memmove(P.marks, P.marks + 1, sizeof(Marker) * (size_t)(P.nmarks - 1));
    P.nmarks--;
    app_wake();
  }
  P.r_read = end;
  if (P.dec_done && P.r_read == P.r_write && !P.ended) {
    P.ended = true;
    app_wake();
  }
  SDL_CondBroadcast(P.cv);
  SDL_UnlockMutex(P.mx);
}

/* ---- decoder thread ---------------------------------------------------------------- */

typedef struct Dec {
  FmAudio *a;
  SDL_AudioStream *st;
  int idx;
  int ch;
} Dec;

static void dec_close(Dec *d) {
  if (d->st) SDL_FreeAudioStream(d->st);
  if (d->a) aud_close(d->a);
  d->a = NULL;
  d->st = NULL;
  d->idx = -1;
}

/* Opens track idx (lock NOT held); fills its metadata slot. */
static bool dec_open(Dec *d, int idx) {
  dec_close(d);
  char path[FM_PATH_MAX];
  lock();
  if (idx < 0 || idx >= P.n) { unlock(); return false; }
  fm_strlcpy(path, P.paths[idx], sizeof path);
  unlock();
  FmErr err;
  d->a = aud_open(path, &err);
  if (!d->a) {
    lock();
    fm_snprintf(P.error, sizeof P.error, "Cannot play %s%s", fm_path_base(path),
                err == FM_ERR_UNSUPPORTED ? " (needs FFmpeg)" : "");
    unlock();
    app_wake();
    return false;
  }
  d->ch = aud_channels(d->a);
  d->st = SDL_NewAudioStream(AUDIO_F32SYS, (Uint8)d->ch, aud_rate(d->a), AUDIO_F32SYS, 2, P.rate);
  if (!d->st) { aud_close(d->a); d->a = NULL; return false; }
  d->idx = idx;
  /* tags and cover */
  FmAudMeta m;
  aud_meta(path, &m, true);
  FmImage cover;
  memset(&cover, 0, sizeof cover);
  if (m.cover) img_load_mem(m.cover, m.cover_len, COVER_PX, &cover);
  lock();
  TrackMeta *t = meta_for(idx);
  if (!t) {
    /* reuse a slot that is neither heard nor this one */
    for (int i = 0; i < 3 && !t; i++)
      if (P.meta[i].idx != P.heard) t = &P.meta[i];
    if (!t) t = &P.meta[0];
  }
  img_free(&t->cover);
  t->idx = idx;
  fm_strlcpy(t->title, m.title, sizeof t->title);
  fm_strlcpy(t->artist, m.artist, sizeof t->artist);
  fm_strlcpy(t->album, m.album, sizeof t->album);
  t->cover = cover;
  t->dur = aud_length(d->a) > 0 ? (double)aud_length(d->a) / aud_rate(d->a) : 0;
  fm_strlcpy(t->codec, aud_codec(d->a), sizeof t->codec);
  t->rate = aud_rate(d->a);
  t->ch = d->ch;
  unlock();
  aud_meta_free(&m);
  app_wake();
  return true;
}

/* Writes converted audio into the ring (lock held); returns frames written. */
static int ring_put(const float *src, int frames) {
  for (int i = 0; i < frames; i++) {
    u32 at = (u32)((P.r_write + (u64)i) % P.ring_frames);
    P.ring[at * 2] = src[i * 2];
    P.ring[at * 2 + 1] = src[i * 2 + 1];
  }
  P.r_write += (u64)frames;
  return frames;
}

static void add_marker(int track, double sec) {
  if (P.nmarks == FM_COUNT(P.marks)) {
    memmove(P.marks, P.marks + 1, sizeof(Marker) * (FM_COUNT(P.marks) - 1));
    P.nmarks--;
  }
  P.marks[P.nmarks].at = P.r_write;
  P.marks[P.nmarks].track = track;
  P.marks[P.nmarks].sec = sec;
  P.nmarks++;
}

/* Moves converted output from the stream to the ring as space allows (lock held). */
static bool drain_stream(Dec *d, float *tmp, int tmp_frames) {
  for (;;) {
    u64 space = P.ring_frames - (P.r_write - P.r_read);
    int avail = SDL_AudioStreamAvailable(d->st) / (int)(sizeof(float) * 2);
    if (avail <= 0) return true;
    int n = (int)FM_MIN((u64)FM_MIN(avail, tmp_frames), space);
    if (n <= 0) return false;
    int got = SDL_AudioStreamGet(d->st, tmp, n * (int)(sizeof(float) * 2)) / (int)(sizeof(float) * 2);
    if (got <= 0) return true;
    ring_put(tmp, got);
  }
}

static int decoder(void *u) {
  FM_UNUSED(u);
  Dec d = { NULL, NULL, -1, 0 };
  float *src = (float *)fm_alloc(sizeof(float) * CHUNK * 2);
  float *tmp = (float *)fm_alloc(sizeof(float) * CHUNK * 8);
  int tmp_frames = CHUNK * 4;
  lock();
  while (!P.quit) {
    if (P.req != REQ_NONE) {
      int req = P.req, idx = P.req_index;
      double sec = P.req_seek;
      P.req = REQ_NONE;
      if (req == REQ_PLAY) {
        P.dec_done = false;
        unlock();
        bool ok = dec_open(&d, idx);
        lock();
        if (P.req != REQ_NONE) continue;          /* superseded meanwhile */
        P.r_write = P.r_read;
        P.nmarks = 0;
        add_marker(idx, 0);
        if (!ok) {
          int nx = next_index(idx, false);
          if (++P.fail_streak < P.n && nx >= 0 && nx != idx) { P.req = REQ_PLAY; P.req_index = nx; }
          else { P.dec_done = true; }
          continue;
        }
        P.fail_streak = 0;
      } else if (req == REQ_SEEK && d.a) {
        unlock();
        u64 fr = (u64)(sec * aud_rate(d.a));
        bool ok = aud_seek(d.a, fr);
        SDL_AudioStreamClear(d.st);
        lock();
        if (P.req != REQ_NONE) continue;
        P.r_write = P.r_read;
        P.nmarks = 0;
        add_marker(d.idx, ok ? sec : (double)aud_tell(d.a) / aud_rate(d.a));
        P.dec_done = false;
        P.ended = false;
      }
      continue;
    }
    if (!d.a || P.dec_done) {
      SDL_CondWait(P.cv, P.mx);
      continue;
    }
    if (!drain_stream(&d, tmp, tmp_frames) || P.ring_frames - (P.r_write - P.r_read) < (u64)CHUNK * 2) {
      /* the audio callback broadcasts after every read, so this sleeps until
      ** there is room; paused, nothing reads and the thread stays asleep
      ** (it used to wake 10 times a second, even paused in the mini player) */
      SDL_CondWaitTimeout(P.cv, P.mx, P.paused ? 2000 : 500);
      continue;
    }
    unlock();
    int got = aud_read(d.a, src, CHUNK / 2);
    if (got > 0) {
      if (d.ch == 2) SDL_AudioStreamPut(d.st, src, got * (int)(sizeof(float) * 2));
      else SDL_AudioStreamPut(d.st, src, got * (int)sizeof(float));
      lock();
      continue;
    }
    /* end of track: flush the converter, then go on gaplessly */
    SDL_AudioStreamFlush(d.st);
    lock();
    while (!P.quit && P.req == REQ_NONE && !drain_stream(&d, tmp, tmp_frames))
      SDL_CondWaitTimeout(P.cv, P.mx, 100);
    if (P.quit || P.req != REQ_NONE) continue;
    int nx = next_index(d.idx, false);
    if (nx < 0) {
      P.dec_done = true;
      continue;
    }
    unlock();
    bool ok = dec_open(&d, nx);
    lock();
    if (P.req != REQ_NONE) continue;
    add_marker(nx, 0);
    if (!ok) {
      int nn = next_index(nx, false);
      if (++P.fail_streak < P.n && nn >= 0 && nn != nx) { P.req = REQ_PLAY; P.req_index = nn; }
      else P.dec_done = true;
    } else {
      P.fail_streak = 0;
    }
  }
  unlock();
  dec_close(&d);
  fm_free(src);
  fm_free(tmp);
  return 0;
}

/* ---- control (main thread) -------------------------------------------------------------- */

static bool device_open(void) {
  if (P.dev) return true;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    fm_snprintf(P.error, sizeof P.error, "No audio output: %s", SDL_GetError());
    return false;
  }
  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof want);
  want.freq = 48000;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_cb;
  P.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!P.dev) {
    fm_snprintf(P.error, sizeof P.error, "No audio output: %s", SDL_GetError());
    return false;
  }
  P.rate = have.freq > 0 ? have.freq : 48000;
  P.ring_frames = (u32)(P.rate * RING_SEC);
  P.ring = (float *)fm_calloc((size_t)P.ring_frames * 2, sizeof(float));
  return true;
}

static void request_play(int idx) {
  lock();
  P.req = REQ_PLAY;
  P.req_index = idx;
  P.r_write = P.r_read;
  P.nmarks = 0;
  P.heard = idx;
  P.base_frame = P.r_read;
  P.base_sec = 0;
  P.ended = false;
  P.dec_done = false;
  P.fail_streak = 0;
  P.error[0] = 0;
  SDL_CondBroadcast(P.cv);
  unlock();
  if (P.paused) { P.paused = false; SDL_PauseAudioDevice(P.dev, 0); }
  ui_redraw();
}

static void request_seek(double sec) {
  lock();
  TrackMeta *t = meta_for(P.heard);
  double dur = t ? t->dur : 0;
  if (dur > 0) sec = FM_CLAMP(sec, 0.0, dur - 0.05);
  if (sec < 0) sec = 0;
  P.req = REQ_SEEK;
  P.req_seek = sec;
  P.r_write = P.r_read;
  P.nmarks = 0;
  P.base_frame = P.r_read;
  P.base_sec = sec;
  P.ended = false;
  SDL_CondBroadcast(P.cv);
  unlock();
}

static double position(void) {
  lock();
  double s = P.base_sec + (double)(P.r_read - P.base_frame) / FM_MAX(1, P.rate);
  unlock();
  return s;
}

static void set_paused(bool p) {
  if (!P.dev) return;
  lock();
  bool ended = P.ended;
  int heard = P.heard;
  unlock();
  if (!p && ended) {
    /* play after the end: start the playlist (or the track) again */
    request_play(P.repeat == REP_ONE ? heard : P.order[0]);
    return;
  }
  P.paused = p;
  SDL_PauseAudioDevice(P.dev, p ? 1 : 0);
  ui_redraw();
}

void audio_pause(void) {
  if (P.active && !P.paused) set_paused(true);
}

static void set_volume(float v) {
  P.volume = FM_CLAMP(v, 0.0f, 1.0f);
  conf.volume = P.volume;
}

static void skip(int dir) {
  lock();
  int heard = P.heard;
  unlock();
  if (dir < 0 && position() > 3.0) { request_seek(0); return; }
  int idx = dir > 0 ? next_index(heard, true) : prev_index(heard);
  if (idx >= 0) request_play(idx);
}

void audio_stop(void) {
  if (!P.active && !P.thr && !P.dev) return;
  if (P.thr) {
    lock();
    P.quit = true;
    SDL_CondBroadcast(P.cv);
    unlock();
    SDL_WaitThread(P.thr, NULL);
  }
  if (P.dev) SDL_CloseAudioDevice(P.dev);
  if (P.cover_tex) SDL_DestroyTexture(P.cover_tex);
  for (int i = 0; i < 3; i++) img_free(&P.meta[i].cover);
  if (P.cv) SDL_DestroyCond(P.cv);
  if (P.mx) SDL_DestroyMutex(P.mx);
  fm_free(P.ring);
  fm_free(P.order);
  arena_free(&P.arena);
  float vol = P.volume;
  memset(&P, 0, sizeof P);
  P.volume = vol;
  ui_redraw();
}

/* ---- open / close ------------------------------------------------------------------- */

static bool aud_view_open(const char *path, const char *const *list, int n, int index) {
  if (P.active && P.heard >= 0 && P.heard < P.n && strcmp(P.paths[P.heard], path) == 0) {
    P.viewer_open = true;            /* back from the mini player */
    view_chrome_poke(&P.chrome);
    return true;
  }
  /* copy the new list before the old one goes (it may be the same memory) */
  FmArena arena;
  arena_init(&arena, 16 * 1024);
  char **paths;
  int cnt;
  if (list && n > 0 && index >= 0 && index < n) {
    paths = (char **)arena_alloc(&arena, sizeof(char *) * (size_t)n);
    for (int i = 0; i < n; i++) paths[i] = arena_strdup(&arena, list[i]);
    cnt = n;
  } else {
    paths = (char **)arena_alloc(&arena, sizeof(char *));
    paths[0] = arena_strdup(&arena, path);
    cnt = 1;
    index = 0;
  }
  bool shuffle = P.shuffle;
  int repeat = P.repeat;
  float vol = P.volume;
  audio_stop();
  P.shuffle = shuffle;
  P.repeat = repeat;
  P.volume = vol > 0 ? vol : (conf.volume > 0 ? conf.volume : 0.8f);
  P.arena = arena;
  P.paths = paths;
  P.n = cnt;
  P.order = (int *)fm_alloc(sizeof(int) * (size_t)cnt);
  make_order(index);
  for (int i = 0; i < 3; i++) P.meta[i].idx = -1;
  P.cover_idx = -1;
  viz_reset();
  P.heard = index;
  P.mx = SDL_CreateMutex();
  P.cv = SDL_CreateCond();
  P.active = true;
  P.viewer_open = true;
  view_chrome_poke(&P.chrome);
  if (!P.mx || !P.cv || !device_open()) return true;     /* the player shows the error */
  eq_follow(&P.eq, P.rate, P.mx);
  P.thr = fm_thread_create(decoder, "audio", NULL);
  request_play(index);
  SDL_PauseAudioDevice(P.dev, 0);
  return true;
}

static void aud_view_close(void) {
  P.viewer_open = false;
  P.info_open = false;
  if (P.active && !P.dev) audio_stop();       /* nothing could play: forget it */
  ui_redraw();
}

/* ---- per-frame state ------------------------------------------------------------------ */

typedef struct NowPlaying {
  int idx;
  char title[AUD_META_TEXT], artist[AUD_META_TEXT * 2];
  double dur, pos;
  bool ended;
  char codec[24];
  int rate, ch;
  char error[160];
} NowPlaying;

static void now_playing(NowPlaying *np) {
  memset(np, 0, sizeof *np);
  lock();
  np->idx = P.heard;
  np->ended = P.ended;
  fm_strlcpy(np->error, P.error, sizeof np->error);
  if (P.dev) P.error[0] = 0;
  TrackMeta *t = meta_for(P.heard);
  if (t) {
    fm_strlcpy(np->title, t->title, sizeof np->title);
    if (t->artist[0] && t->album[0]) fm_snprintf(np->artist, sizeof np->artist, "%s \xC2\xB7 %s", t->artist, t->album);
    else fm_strlcpy(np->artist, t->artist[0] ? t->artist : t->album, sizeof np->artist);
    np->dur = t->dur;
    fm_strlcpy(np->codec, t->codec, sizeof np->codec);
    np->rate = t->rate;
    np->ch = t->ch;
    /* cover upload */
    if (t->cover.px && P.cover_idx != P.heard) {
      if (P.cover_tex) SDL_DestroyTexture(P.cover_tex);
      P.cover_tex = view_tex_rgba(t->cover.px, t->cover.w, t->cover.h);
      P.cover_w = t->cover.w;
      P.cover_h = t->cover.h;
      P.cover_idx = P.heard;
    } else if (!t->cover.px && P.cover_idx != P.heard) {
      if (P.cover_tex) SDL_DestroyTexture(P.cover_tex);
      P.cover_tex = NULL;
      P.cover_idx = P.heard;
    }
  }
  double pos = P.base_sec + (double)(P.r_read - P.base_frame) / FM_MAX(1, P.rate);
  unlock();
  if (np->error[0] && P.dev) ui_toast("%s", np->error);
  np->pos = pos;
  if (np->dur > 0 && np->pos > np->dur) np->pos = np->dur;
  if (!np->title[0] && np->idx >= 0 && np->idx < P.n) display_name(np->idx, np->title, sizeof np->title);
}

/* ---- visualizer --------------------------------------------------------------------- */

/* Feeds fviz the samples being heard; returns true while it still moves. */
static bool viz_feed(bool playing) {
  if (playing) {
    lock();
    for (int i = 0; i < VIZ_FFT; i++) P.viz_snap[i] = P.viz[(P.viz_pos + i) % VIZ_FFT];
    unlock();
  }
  return viz_update(&conf.viz, playing ? P.viz_snap : NULL, P.rate, ui.dt);
}

static void viz_tap(void) {
  viz_cycle(&conf.viz, &conf.viz_saved);
  ui_toast("Visualizer: %s%s", viz_style_name(conf.viz.style), conf.viz.custom ? " (custom)" : "");
}

/* --demo-audio FILE [--demo-viz N] [--demo-viz-panel] [--demo-eq N]: opens the
** player (or the video viewer) with a style, the panel or an equalizer preset,
** for screenshots. Checked from audio_mini_active, which the app asks every
** frame, so the hook needs no code in fapp.c. */
static void demo_hook(void) {
  static bool done;
  if (done) return;
  done = true;
  const char *file = NULL;
  bool panel = false;
  for (int i = 1; i < app.argc; i++) {
    if (!strcmp(app.argv[i], "--demo-audio") && i + 1 < app.argc) file = app.argv[i + 1];
    if (!strcmp(app.argv[i], "--demo-viz") && i + 1 < app.argc) viz_preset(&conf.viz, atoi(app.argv[i + 1]));
    if (!strcmp(app.argv[i], "--demo-viz-panel")) panel = true;
    if (!strcmp(app.argv[i], "--demo-eq") && i + 1 < app.argc) {
      eq_preset(&conf.eq, atoi(app.argv[i + 1]));
      conf.eq.on = true;
      viz_panel_tab(1);
      panel = true;
    }
  }
  if (!file) return;
  app_open(file, NULL, 0, 0);
  P.viz_open = panel;
}

/* ---- drawing pieces ----------------------------------------------------------------- */

static void cover_card(FmRect r, int idx, float radius) {
  gfx_shadow(r, radius, DP(18), T.shadow);
  if (P.cover_tex && P.cover_idx == idx) {
    /* crop to square */
    float cw = (float)P.cover_w, ch = (float)P.cover_h, s = FM_MIN(cw, ch);
    FmRect src = { (cw - s) * 0.5f, (ch - s) * 0.5f, s, s };
    gfx_rrect(r, radius, T.surface2);
    if (fabsf(cw - ch) < 2) gfx_tex_rounded(P.cover_tex, r, radius, FM_HEX(0xFFFFFF));
    else {
      gfx_clip_push(r);
      gfx_tex(P.cover_tex, &src, r, FM_HEX(0xFFFFFF));
      gfx_clip_pop();
    }
    return;
  }
  /* generated gradient from the file name */
  u32 h = ui_id(idx >= 0 && idx < P.n ? fm_path_base(P.paths[idx]) : "x");
  FmColor a = kAccents[h % UI_ACCENTS], b = kAccents[(h / UI_ACCENTS + 3) % UI_ACCENTS];
  gfx_rrect_vgrad(r, radius, a, col_mix(b, FM_HEX(0x101018), 0.35f));
  float is = r.w * 0.38f;
  icon_draw(IC_MUSIC, rect_center(r, is, is), FM_RGBA(255, 255, 255, 210));
}

/* Seek bar with drag; returns true while the user drags. */
static void seek_bar(FmRect r, NowPlaying *np) {
  u32 id = ui_id("aud.seek");
  FmRect hit = rect_inset2(r, 0, -DP(8));
  int f = np->dur > 0 ? ui_hit(id, hit) : 0;
  float t = np->dur > 0 ? (float)(np->pos / np->dur) : 0;
  if (f & UI_HELD) {
    ui.drag_owner = id;
    P.seek_drag = true;
    P.seek_t = FM_CLAMP((ui.mx - r.x) / r.w, 0.0f, 1.0f);
  }
  if (P.seek_drag) {
    t = P.seek_t;
    if (!ui.down) {
      P.seek_drag = false;
      request_seek(P.seek_t * np->dur);
      np->pos = P.seek_t * np->dur;
    }
  }
  t = FM_CLAMP(t, 0.0f, 1.0f);
  float th = DP(4), cy = r.y + r.h * 0.5f;
  gfx_rrect(FM_RECT(r.x, cy - th * 0.5f, r.w, th), th * 0.5f, T.surface3);
  gfx_rrect(FM_RECT(r.x, cy - th * 0.5f, r.w * t, th), th * 0.5f, T.accent);
  float kr = DP((f & (UI_HELD | UI_HOVER)) || P.seek_drag ? 8 : 6);
  if (np->dur > 0) gfx_circle(r.x + r.w * t, cy, kr, T.accent);
  if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
}

static bool round_btn(u32 id, FmRect r, FmIcon ic, bool filled, bool on, const char *tip) {
  if (filled) {
    float s = FM_MIN(r.w, r.h);
    gfx_shadow(rect_center(r, s, s), s * 0.5f, DP(10), col_alpha(T.accent, 0.35f));
    gfx_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, s * 0.5f, T.accent);
    int f = ui_hit(id, r);
    if (f & UI_HELD) gfx_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, s * 0.5f, T.press);
    float is = s * 0.42f;
    icon_draw(ic, rect_center(r, is, is), T.on_accent);
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
    return (f & UI_CLICK) != 0;
  }
  if (on) {
    float s = FM_MIN(r.w, r.h) - DP(6);
    gfx_circle(r.x + r.w * 0.5f, r.y + r.h * 0.5f, s * 0.5f, T.accent_soft);
  }
  return ui_icon_btn(id, r, ic, on ? T.accent : T.text, tip);
}

static void controls(FmRect r, bool playing) {
  float big = FM_MIN(r.h, DP(ui.touch_mode ? 72 : 60));
  float sm = FM_MAX(ui.m.hit, DP(44));
  float gap = FM_MIN(DP(18), (r.w - big - sm * 4) / 4);
  float total = big + sm * 4 + gap * 4;
  float x = r.x + (r.w - total) * 0.5f, cy = r.y + r.h * 0.5f;
  FmRect b;
  b = FM_RECT(x, cy - sm * 0.5f, sm, sm);
  if (round_btn(ui_id("aud.shuf"), b, IC_SHUFFLE, false, P.shuffle, "Shuffle")) {
    lock();
    P.shuffle = !P.shuffle;
    make_order(P.heard);
    unlock();
    ui_toast(P.shuffle ? "Shuffle on" : "Shuffle off");
  }
  x += sm + gap;
  if (round_btn(ui_id("aud.prev"), FM_RECT(x, cy - sm * 0.5f, sm, sm), IC_PREV, false, false, "Previous (P)")) skip(-1);
  x += sm + gap;
  if (round_btn(ui_id("aud.play"), FM_RECT(x, cy - big * 0.5f, big, big), playing ? IC_PAUSE : IC_PLAY, true, false,
                "Play / pause (Space)"))
    set_paused(playing);
  x += big + gap;
  if (round_btn(ui_id("aud.next"), FM_RECT(x, cy - sm * 0.5f, sm, sm), IC_NEXT, false, false, "Next (N)")) skip(1);
  x += sm + gap;
  FmIcon rep = P.repeat == REP_ONE ? IC_REPEAT_ONE : IC_REPEAT;
  if (round_btn(ui_id("aud.rep"), FM_RECT(x, cy - sm * 0.5f, sm, sm), rep, false, P.repeat != REP_OFF, "Repeat")) {
    lock();
    P.repeat = (P.repeat + 1) % 3;
    unlock();
    static const char *const kRep[] = { "Repeat off", "Repeat all", "Repeat one" };
    ui_toast("%s", kRep[P.repeat]);
  }
}

static void volume_row(FmRect r) {
  float h = r.h;
  FmRect ib = rect_cut_left(&r, h);
  FmIcon ic = P.volume <= 0.001f ? IC_MUTE : IC_VOLUME;
  if (ui_icon_btn(ui_id("aud.mute"), ib, ic, T.text2, "Mute")) {
    if (P.volume > 0.001f) { P.vol_before_mute = P.volume; set_volume(0); }
    else set_volume(P.vol_before_mute > 0.01f ? P.vol_before_mute : 0.8f);
  }
  rect_cut_left(&r, DP(6));
  float v = P.volume;
  if (ui_slider(ui_id("aud.vol"), rect_inset2(r, 0, FM_MAX(0.0f, (r.h - DP(28)) * 0.5f)), &v, 0, 1)) set_volume(v);
}

static void playlist(FmRect r, int heard) {
  float rh = ui.m.row_h * (ui.touch_mode ? 0.9f : 1.0f);
  u32 sid = ui_id("aud.list");
  ui_scroll(&P.list_scroll, sid, r, rh * P.n);
  gfx_clip_push(r);
  int first = (int)(P.list_scroll.y / rh);
  for (int i = FM_MAX(0, first); i < P.n; i++) {
    FmRect row = { r.x, r.y + i * rh - P.list_scroll.y, r.w, rh };
    if (row.y > r.y + r.h) break;
    int f = ui_hit(ui_idn(sid, (u32)i), row);
    bool cur = i == heard;
    if (cur) gfx_rrect(rect_inset(row, DP(3)), DP(10), T.sel);
    else if (f & UI_HOVER) gfx_rrect(rect_inset(row, DP(3)), DP(10), T.hover);
    if (f & UI_CLICK) request_play(i);
    FmRect c = rect_inset2(row, DP(12), 0);
    FmRect num = rect_cut_left(&c, DP(34));
    if (cur) icon_draw(IC_MUSIC, rect_center(num, DP(18), DP(18)), T.accent);
    else {
      char nb[16];
      fm_snprintf(nb, sizeof nb, "%d", i + 1);
      ui_label(num, nb, FONT_REGULAR, ui.m.font_small, T.text3, UI_LEFT);
    }
    char name[256];
    TrackMeta *t = NULL;
    lock();
    t = meta_for(i);
    if (t && t->title[0]) fm_strlcpy(name, t->title, sizeof name);
    else display_name(i, name, sizeof name);
    unlock();
    ui_label(c, name, cur ? FONT_BOLD : FONT_REGULAR, ui.m.font, cur ? T.accent : T.text, UI_LEFT);
  }
  gfx_clip_pop();
  ui_scrollbar(&P.list_scroll, r, rh * P.n);
}

static void info_card(NowPlaying *np) {
  char dur[32], fmt[64], size[32];
  view_fmt_time(np->dur, dur, sizeof dur);
  fm_snprintf(fmt, sizeof fmt, "%s, %d Hz, %s", np->codec, np->rate, np->ch >= 2 ? "stereo" : "mono");
  FmStat st;
  const char *path = P.paths[np->idx];
  fm_fmt_size(plat_stat(path, &st) ? st.size : 0, size, sizeof size);
  const char *keys[] = { "Title", "Artist", "Length", "Format", "Size", "Path" };
  const char *vals[] = { np->title, np->artist, np->dur > 0 ? dur : "unknown", fmt, size, path };
  view_info_dialog(ui_id("aud.info"), "Track info", keys, vals, FM_COUNT(keys), &P.info_open);
}

/* ---- the full player ------------------------------------------------------------------ */

enum { AM_OPEN = 1, AM_SHARE, AM_INFO, AM_STOP, AM_VIZ, AM_EQ };

static void aud_view_frame(FmRect area) {
  gfx_rect(area, T.bg);
  if (!P.active) { app_close_viewer(); return; }
  NowPlaying np;
  now_playing(&np);
  bool playing = P.dev && !P.paused && !np.ended;
  if (P.dev) eq_follow(&P.eq, P.rate, P.mx);

  /* keys */
  if (ui_key(SDLK_SPACE, 0) || ui_key(SDLK_AUDIOPLAY, 0) || ui_key(SDLK_k, 0)) set_paused(playing);
  if (ui_key(SDLK_RIGHT, 0)) request_seek(np.pos + 5);
  if (ui_key(SDLK_LEFT, 0)) request_seek(np.pos - 5);
  if (ui_key(SDLK_UP, 0)) set_volume(P.volume + 0.05f);
  if (ui_key(SDLK_DOWN, 0)) set_volume(P.volume - 0.05f);
  if (ui_key(SDLK_n, 0) || ui_key(SDLK_AUDIONEXT, 0)) skip(1);
  if (ui_key(SDLK_p, 0) || ui_key(SDLK_AUDIOPREV, 0)) skip(-1);

  bool wide = !ui.portrait && area.w >= DP(720);
  char sub[48];
  if (P.n > 1) fm_snprintf(sub, sizeof sub, "%d / %d", np.idx + 1, P.n);
  else sub[0] = 0;
  FmRect act;
  if (view_topbar(area, VIEW_BAR_THEME, 1.0f, "Now playing", sub, wide || P.n < 2 ? 2 : 3, &act)) {
    app_close_viewer();
    return;
  }
  u32 mid = ui_id("aud.menu");
  if (view_bar_btn(&act, ui_id("aud.more"), IC_MORE, "More", VIEW_BAR_THEME, 1, false)) {
    FmMenuItem items[] = {
      { AM_INFO, IC_INFO, "Track info", NULL, 0 },
      { AM_VIZ, IC_EQUALIZER, "Visualizer", NULL, 0 },
      { AM_EQ, IC_EQUALIZER, "Equalizer", NULL, 0 },
      { AM_SHARE, IC_SHARE, "Share", NULL, 0 },
      { AM_OPEN, IC_OPEN_WITH, "Open with system app", NULL, 0 },
      { 0, IC_NONE, NULL, NULL, UI_MI_SEP },
      { AM_STOP, IC_STOP, "Stop and close", NULL, UI_MI_DANGER },
    };
    ui_menu_open(mid, act.x + act.w, area.y + ui.m.bar_h, items, FM_COUNT(items));
  }
  if (!wide && P.n > 1 && view_bar_btn(&act, ui_id("aud.listbtn"), IC_LIST, "Playlist", VIEW_BAR_THEME, 1, P.show_list)) {
    P.show_list = !P.show_list;
    P.viz_open = false;
  }
  /* favorite: the media library's heart for the track being heard */
  if (np.idx >= 0) {
    bool fav = lib_is_fav(P.paths[np.idx]);
    if (view_bar_btn(&act, ui_id("aud.favbtn"), fav ? IC_HEART_FILL : IC_HEART,
                     fav ? "Remove from favorites" : "Add to favorites", VIEW_BAR_THEME, 1, fav))
      lib_fav_toggle(P.paths[np.idx]);
  }
  if (view_bar_btn(&act, ui_id("aud.vizbtn"), IC_EQUALIZER, "Visualizer & equalizer", VIEW_BAR_THEME, 1,
                   P.viz_open)) {
    P.viz_open = !P.viz_open;
    P.show_list = false;
  }
  int mres = ui_menu_result(mid);
  switch (mres) {
    case AM_OPEN: set_paused(true); plat_open_external(P.paths[np.idx]); break;
    case AM_SHARE: plat_share(P.paths[np.idx]); break;
    case AM_INFO: P.info_open = true; break;
    case AM_VIZ: case AM_EQ:
      P.viz_open = true;
      P.show_list = false;
      viz_panel_tab(mres == AM_EQ);
      break;
    case AM_STOP: audio_stop(); app_close_viewer(); return;
    default: break;
  }

  FmRect body = area;
  rect_cut_top(&body, ui.m.bar_h);
  if (!P.dev) {
    view_message(body, IC_MUTE, "Cannot play audio", np.error[0] ? np.error : "No audio output device.", T.text,
                 T.text2);
    float bw = DP(220), bh = DP(ui.touch_mode ? 46 : 38);
    if (np.idx >= 0 && np.idx < P.n &&
        ui_button(ui_id("aud.ext"), FM_RECT(body.x + (body.w - bw) * 0.5f, body.y + body.h * 0.5f + DP(110), bw, bh),
                  IC_OPEN_WITH, "Open with system app", UI_BTN_TONAL))
      plat_open_external(P.paths[np.idx]);
    if (view_key_back()) app_close_viewer();
    return;
  }
  body = rect_inset(body, DP(ui.touch_mode ? 20 : 24));
  FmRect left = body, right = { 0, 0, 0, 0 };
  /* wide: the visualizer settings take the right column (over the playlist),
  ** so the player stays in view as a live preview; narrow: the cover area */
  bool side = wide && (P.n > 1 || P.viz_open);
  if (side) {
    right = rect_cut_right(&left, FM_MIN(DP(420), body.w * 0.45f));
    rect_cut_right(&left, DP(24));
  }
  bool panel_left = P.viz_open && !side;
  bool list_left = !panel_left && P.show_list && !wide && P.n > 1;
  int style = conf.viz.style;
  bool ring = viz_around_cover(style) && !panel_left && !list_left;

  /* control stack from the bottom */
  float ch = DP(ui.touch_mode ? 84 : 72);
  FmRect vol = rect_cut_bottom(&left, DP(40));
  vol = rect_center(vol, FM_MIN(vol.w, DP(360)), vol.h);
  FmRect ctl = rect_cut_bottom(&left, ch);
  FmRect times = rect_cut_bottom(&left, DP(22));
  FmRect seek = rect_cut_bottom(&left, DP(24));
  rect_cut_bottom(&left, DP(10));
  float th = font_line_h(ui.m.font_big), ah = font_line_h(ui.m.font);
  /* a ring needs room round the cover (phone landscape has none): use the strip */
  if (ring && FM_MIN(left.w, left.h - th - ah - DP(20)) < DP(150)) ring = false;
  /* the strip grows with the room; ring styles and "off" give it to the cover */
  float vh = 0;
  if (style != VIZ_OFF && !ring) vh = FM_CLAMP((left.h - th - ah) * 0.2f, DP(44), DP(120));
  /* the panel over the cover gets the titles' room too, and the strip shrinks */
  if (panel_left) vh = FM_MIN(vh, DP(64));
  FmRect vizr = rect_cut_bottom(&left, vh);
  if (vh > 0) rect_cut_bottom(&left, DP(8));
  FmRect titles = rect_cut_bottom(&left, panel_left ? 0 : th + ah + DP(10));
  rect_cut_bottom(&left, panel_left ? 0 : DP(10));

  /* visualizer state first: the cover may bounce with the beat */
  bool live = style != VIZ_OFF || P.viz_open;
  bool moving = live && viz_feed(playing);
  if (panel_left) {
    if (!viz_panel(rect_center(left, FM_MIN(left.w, DP(560)), left.h))) P.viz_open = false;
  } else if (list_left) {
    gfx_rrect(left, ui.m.radius, T.surface);
    playlist(rect_inset(left, DP(6)), np.idx);
  } else {
    float cs = FM_MIN(left.w, left.h);
    cs = FM_MIN(cs, DP(ring ? 480 : 420));
    FmRect sq = rect_center(left, cs, cs);
    if (ring && cs > DP(96)) {
      float in = cs * 0.6f * (style == VIZ_PULSE ? 1.0f + 0.04f * viz_beat() : 1.0f);
      viz_draw(&conf.viz, sq, in * 0.5f);
      cover_card(rect_center(sq, in, in), np.idx, in * 0.5f);
      if (ui_hit(ui_id("aud.vizring"), sq) & UI_CLICK) viz_tap();
    } else if (cs > DP(48)) {
      cover_card(sq, np.idx, DP(20));
    }
  }
  /* title and artist */
  if (!panel_left) {
    float tw = font_width(FONT_BOLD, ui.m.font_big, np.title, -1);
    float tx = titles.x + FM_MAX(0.0f, (titles.w - tw) * 0.5f);
    font_draw_ellipsis(FONT_BOLD, ui.m.font_big, tx, titles.y, np.title, titles.w, T.text);
    const char *a = np.artist;
    float aw = font_width(FONT_REGULAR, ui.m.font, a, -1);
    float ax = titles.x + FM_MAX(0.0f, (titles.w - aw) * 0.5f);
    font_draw_ellipsis(FONT_REGULAR, ui.m.font, ax, titles.y + th + DP(4), a, titles.w, T.text2);
  }
  /* visualizer strip; a tap cycles the styles */
  if (vh > 0) {
    FmRect vr = rect_center(vizr, FM_MIN(vizr.w, DP(560)), vizr.h);
    viz_draw(&conf.viz, vr, 0);
    int f = ui_hit(ui_id("aud.viz"), vr);
    if (f & UI_CLICK) viz_tap();
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  }
  /* seek + times */
  FmRect sr = rect_center(seek, FM_MIN(seek.w, DP(560)), seek.h);
  seek_bar(sr, &np);
  char a[32], b[32];
  double shown = P.seek_drag ? P.seek_t * np.dur : np.pos;
  view_fmt_time(shown, a, sizeof a);
  if (np.dur > 0) {
    char r2[32];
    view_fmt_time(np.dur - shown, r2, sizeof r2);
    fm_snprintf(b, sizeof b, "-%s", r2);
  } else {
    fm_strlcpy(b, "--:--", sizeof b);
  }
  FmRect tr = { sr.x, times.y, sr.w, times.h };
  ui_label(tr, a, FONT_REGULAR, ui.m.font_small, T.text2, UI_LEFT);
  ui_label(tr, b, FONT_REGULAR, ui.m.font_small, T.text2, UI_RIGHT);
  controls(ctl, playing);
  volume_row(vol);

  if (side && P.viz_open) {
    if (!viz_panel(right)) P.viz_open = false;
  } else if (side) {
    gfx_rrect(right, ui.m.radius, T.surface);
    FmRect hdr = rect_cut_top(&right, DP(44));
    char hb[48];
    fm_snprintf(hb, sizeof hb, "Playlist  \xC2\xB7  %d", P.n);
    ui_label(rect_inset2(hdr, DP(16), 0), hb, FONT_BOLD, ui.m.font, T.text, UI_LEFT);
    playlist(rect_inset2(right, DP(6), DP(2)), np.idx);
  }

  if (P.info_open) info_card(&np);
  if ((live && (playing || moving)) || P.seek_drag) ui_animate();
  else if (playing) view_wake_in(250);        /* no visualizer: just the clock */
  if (np.ended && P.dev && !P.paused) { P.paused = true; SDL_PauseAudioDevice(P.dev, 1); }
  if (view_key_back()) {
    if (P.viz_open) P.viz_open = false;
    else app_close_viewer();
  }
}

/* ---- mini player ---------------------------------------------------------------------- */

bool audio_mini_active(void) {
  demo_hook();
  return P.active && !P.viewer_open && P.dev;
}

void audio_mini_draw(FmRect r) {
  if (!audio_mini_active()) return;
  NowPlaying np;
  now_playing(&np);
  eq_follow(&P.eq, P.rate, P.mx);
  if (np.ended && !P.paused) { P.paused = true; SDL_PauseAudioDevice(P.dev, 1); }
  bool playing = !P.paused && !np.ended;
  gfx_rect(r, T.surface2);
  ui_divider(r.x, r.x + r.w, r.y);
  /* progress line along the top */
  if (np.dur > 0) gfx_rect(FM_RECT(r.x, r.y, r.w * (float)FM_CLAMP(np.pos / np.dur, 0.0, 1.0), DP(2)), T.accent);
  FmRect c = rect_inset2(r, DP(8), DP(6));
  float h = c.h;
  u32 id = ui_id("mini.body");
  bool close = ui_icon_btn(ui_id("mini.close"), rect_cut_right(&c, FM_MAX(h, ui.m.hit)), IC_CLOSE, T.text2, "Stop");
  bool next = P.n > 1 && ui_icon_btn(ui_id("mini.next"), rect_cut_right(&c, FM_MAX(h, ui.m.hit)), IC_NEXT, T.text, "Next");
  FmRect pb = rect_cut_right(&c, FM_MAX(h, ui.m.hit));
  float ps = FM_MIN(pb.h, DP(40));
  gfx_circle(pb.x + pb.w * 0.5f, pb.y + pb.h * 0.5f, ps * 0.5f, T.accent);
  bool pp = ui_icon_btn(ui_id("mini.play"), pb, playing ? IC_PAUSE : IC_PLAY, T.on_accent, playing ? "Pause" : "Play");
  FmRect cov = rect_cut_left(&c, h);
  cover_card(rect_inset(cov, DP(2)), np.idx, DP(8));
  rect_cut_left(&c, DP(10));
  int f = ui_hit(id, c);
  float lh = font_line_h(ui.m.font), sh = np.artist[0] ? font_line_h(ui.m.font_small) : 0;
  float y = c.y + (c.h - lh - sh) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, c.x, y, np.title, c.w, T.text);
  if (sh > 0) font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, c.x, y + lh, np.artist, c.w, T.text2);
  if (close) { audio_stop(); return; }
  if (next) skip(1);
  if (pp) set_paused(playing);
  if ((f & UI_CLICK) && np.idx >= 0 && np.idx < P.n) {
    app_open(P.paths[np.idx], (const char *const *)P.paths, P.n, np.idx);
    return;
  }
  /* refresh the progress line once a second, without animating */
  if (playing) view_wake_in(1000);
}

const FmViewer g_view_audio = { "audio", aud_view_open, aud_view_frame, aud_view_close };
