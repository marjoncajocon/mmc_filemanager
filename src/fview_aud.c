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
**   - Entries (audio_play_entries, the online audio view) are URLs or files
**     with the caller's title, artist and artwork instead of tags. Their
**     strings are copied into the playlist arena. An entry without a URL is
**     resolved by the hooks' resolve() on the decoder thread when its turn
**     comes, so a list of a hundred tracks costs no request up front and a
**     stream URL that expires is fetched fresh each time it plays. The
**     stream reader (fnetstream) does the network buffering; the player
**     still keeps only its 0.75 s ring.
**   - Live stations: no seek bar, no gapless next (a station never ends;
**     when it stops sending that is an error with Retry), Next/Previous
**     move between the stations of the list, and the ICY "now playing"
**     title is polled once a second while the player or the bar is drawn.
**   - Buffering: the callback stops consuming when the ring runs dry and
**     waits for half a ring before it plays again, so a stalling stream
**     stutters once instead of crackling; the UI shows "Buffering" after
**     a short grace period (local files never get that far).
**   - A slow stream must never freeze the window: a new list replaces the
**     old one in place (the decoder picks it up when its open returns), and
**     stopping a playlist with URLs detaches the decoder instead of joining
**     it. The lock and condition live outside the player state for that
**     reason, and a detached decoder checks its generation each time it
**     takes the lock, then leaves without touching anything.
*/
#include "fview_int.h"
#include "flib.h"
#include "fplat.h"
#include "fconf.h"
#include "fdec_aud.h"
#include "fdec_img.h"
#include "fviz.h"
#include "feq.h"
#include "fonline_int.h"

#define RING_SEC 0.75
#define CHUNK 2048
#define COVER_PX 512
#define STARVE_SHOW_MS 350      /* a dry ring this long shows "Buffering" */

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
  bool live;
  char url[1024];             /* entries: what was opened (Copy link) */
} TrackMeta;

/* A playlist entry given by audio_play_entries (strings in the arena). */
typedef struct Ent {
  char *url, *title, *artist, *album, *art, *headers, *ref;
  bool live;
} Ent;

/* A decoder thread's identity: it is gone once g_tgen moves on. */
typedef struct DecCtx {
  u32 tgen;
  volatile int cancel;        /* for resolve() */
} DecCtx;

/* Outside P: a detached decoder may still take the lock after a stop. */
static SDL_mutex *g_mx;
static SDL_cond *g_cv;
static u32 g_tgen;            /* decoder generation */
static u32 g_gen;             /* playlist generation (in-place replacements) */
static FmAudioHooks g_hooks;

static struct {
  bool active;                /* a playlist is loaded */
  bool viewer_open;
  /* playlist */
  FmArena arena;
  char **paths;
  Ent *ents;                  /* entries mode, else NULL (files) */
  bool urls;                  /* a URL was played: stop detaches */
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
  bool starved;               /* the ring ran dry: waiting for half a ring */
  u64 starved_at;
  /* decoder thread */
  SDL_Thread *thr;
  DecCtx *ctx;
  int req;
  int req_index;
  double req_seek;
  bool opening;               /* the decoder is opening open_idx */
  int open_idx;
  FmAudio *cur_a;             /* the decoder's open track (now playing title) */
  int cur_idx;
  bool paused;
  float volume, vol_before_mute;
  TrackMeta meta[3];
  int fail_streak;
  char error[160];
  char serr[200];             /* a stream that failed: shown with Retry */
  int serr_idx;               /* -1 = none */
  /* visualizer: written by the callback, copied out on the main thread */
  float viz[VIZ_FFT];
  int viz_pos;
  float viz_snap[VIZ_FFT];
  bool viz_open;              /* settings panel shown */
  FmEq eq;                    /* equalizer state (callback, under the lock) */
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
  char icy[256];              /* live: the station's current title */
  int icy_idx;
  u64 icy_poll;
  int noted;                  /* last entry reported to hooks.played */
  u32 noted_gen;
} P;

/* ---- helpers ------------------------------------------------------------------- */

/* A wake-up in ms with at most one timer pending per `t` (view_wake_in has
** one flag for the whole app, so periodic users can pile up timer chains). */
static Uint32 SDLCALL tick_cb(Uint32 interval, void *t) {
  FM_UNUSED(interval);
  SDL_AtomicSet((SDL_atomic_t *)t, 0);
  app_wake();
  return 0;
}

static void tick(SDL_atomic_t *t, u32 ms) {
  if (SDL_AtomicCAS(t, 0, 1) && !SDL_AddTimer(ms ? ms : 1, tick_cb, t)) SDL_AtomicSet(t, 0);
}

static SDL_atomic_t g_tick_busy, g_tick_clock, g_tick_mini;

static void lock(void) { SDL_LockMutex(g_mx); }
static void unlock(void) { SDL_UnlockMutex(g_mx); }
static bool gone(const DecCtx *c) { return c->tgen != g_tgen; }

static bool is_url(const char *p) { return !fm_strnicmp(p, "http://", 7) || !fm_strnicmp(p, "https://", 8); }

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

static const Ent *ent_at(int idx) { return P.ents && idx >= 0 && idx < P.n ? &P.ents[idx] : NULL; }

static const char *display_name(int idx, char *buf, size_t cap) {
  const Ent *e = ent_at(idx);
  if (e && e->title[0]) {
    fm_strlcpy(buf, e->title, cap);
    return buf;
  }
  const char *b = fm_path_base(P.paths[idx]);
  fm_strlcpy(buf, b[0] ? b : "Stream", cap);
  char *dot = strrchr(buf, '.');
  if (dot && dot != buf) *dot = 0;
  return buf;
}

/* An entry failed (lock held): shown with Retry until the user acts. */
static void set_serr(int idx, const char *why) {
  char name[256];
  display_name(idx, name, sizeof name);
  fm_snprintf(P.serr, sizeof P.serr, "\xE2\x80\x9C%s\xE2\x80\x9D: %s", name, why);
  P.serr_idx = idx;
}

/* ---- audio callback --------------------------------------------------------------- */

static void SDLCALL audio_cb(void *u, Uint8 *stream, int len) {
  FM_UNUSED(u);
  float *out = (float *)stream;
  int frames = len / (int)(sizeof(float) * 2);
  SDL_LockMutex(g_mx);
  u64 avail = P.r_write - P.r_read;
  if (P.starved) {
    /* refill half the ring before playing on: one gap, not a crackle */
    if (avail >= P.ring_frames / 2 || P.dec_done) {
      P.starved = false;
      app_wake();
    } else {
      avail = 0;
    }
  }
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
  if (n < frames && !P.dec_done && !P.starved) {
    P.starved = true;
    P.starved_at = SDL_GetTicks64();
  }
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
  SDL_CondBroadcast(g_cv);
  SDL_UnlockMutex(g_mx);
}

/* ---- decoder thread ---------------------------------------------------------------- */

typedef struct Dec {
  FmAudio *a;
  SDL_AudioStream *st;
  int idx;
  int ch;
  bool live;                  /* endless: its end is an error, not the next track */
  bool url;
} Dec;

/* Lock NOT held. */
static void dec_close(Dec *d) {
  if (d->a) {
    lock();
    if (P.cur_a == d->a) P.cur_a = NULL;
    unlock();
  }
  if (d->st) SDL_FreeAudioStream(d->st);
  if (d->a) aud_close(d->a);
  d->a = NULL;
  d->st = NULL;
  d->idx = -1;
  d->live = d->url = false;
}

/* Why an entry could not be opened, in words. */
static const char *open_why(FmErr err, bool url) {
  if (err == FM_ERR_UNSUPPORTED) return "this format needs FFmpeg (AAC, Opus ...)";
  if (err == FM_ERR_FORMAT) return "not an audio format this player reads";
  if (err == FM_ERR_NOT_FOUND) return "the file is gone";
  return url ? "the stream did not answer" : "cannot be read";
}

/* Opens track idx (lock NOT held); fills its metadata slot. */
static bool dec_open(DecCtx *c, Dec *d, int idx) {
  dec_close(d);
  lock();
  if (gone(c) || idx < 0 || idx >= P.n) { unlock(); return false; }
  u32 gen = g_gen;
  const Ent *e = ent_at(idx);
  char *path = fm_strdup(P.paths[idx]);
  char *headers = e && e->headers ? fm_strdup(e->headers) : NULL;
  char *ref = e && e->ref ? fm_strdup(e->ref) : NULL;
  bool ent = e != NULL, live = e && e->live;
  P.opening = true;
  P.open_idx = idx;
  unlock();
  app_wake();

  char why[200];
  why[0] = 0;
  if (ent && !path[0]) {
    /* resolved now, not at search time: stream links may expire */
    FmAudioStream *st = (FmAudioStream *)fm_calloc(1, sizeof *st);
    if (ref && g_hooks.resolve && g_hooks.resolve(ref, st, &c->cancel) && st->url[0]) {
      fm_free(path);
      path = fm_strdup(st->url);
      fm_free(headers);
      headers = st->headers[0] ? fm_strdup(st->headers) : NULL;
      live = live || st->live;
    } else {
      fm_strlcpy(why, st->err[0] ? st->err : "no stream for this item", sizeof why);
    }
    fm_free(st);
  }
  FmErr err = FM_ERR_IO;
  if (!why[0]) d->a = aud_open_msg(path, headers && headers[0] ? headers : NULL, &err, why, sizeof why);
  bool ok = d->a != NULL;
  if (ok) {
    d->ch = aud_channels(d->a);
    d->st = SDL_NewAudioStream(AUDIO_F32SYS, (Uint8)d->ch, aud_rate(d->a), AUDIO_F32SYS, 2, P.rate);
    if (!d->st) { aud_close(d->a); d->a = NULL; ok = false; err = FM_ERR_NOMEM; }
  }
  /* tags and cover: files only (entries bring their own, art by URL) */
  FmAudMeta m;
  memset(&m, 0, sizeof m);
  FmImage cover;
  memset(&cover, 0, sizeof cover);
  if (ok && !ent) {
    aud_meta(path, &m, true);
    if (m.cover) img_load_mem(m.cover, m.cover_len, COVER_PX, &cover);
  }
  lock();
  P.opening = false;
  if (gone(c) || gen != g_gen) {
    /* stopped or replaced meanwhile: the new request follows */
    unlock();
    img_free(&cover);
    aud_meta_free(&m);
    if (d->st) SDL_FreeAudioStream(d->st);
    if (d->a) aud_close(d->a);
    d->a = NULL;
    d->st = NULL;
    fm_free(path);
    fm_free(headers);
    fm_free(ref);
    return false;
  }
  if (!ok) {
    if (ent) set_serr(idx, why[0] ? why : open_why(err, is_url(path)));
    else fm_snprintf(P.error, sizeof P.error, "Cannot play %s%s", fm_path_base(path),
                     err == FM_ERR_UNSUPPORTED ? " (needs FFmpeg)" : "");
    unlock();
    app_wake();
    fm_free(path);
    fm_free(headers);
    fm_free(ref);
    return false;
  }
  d->idx = idx;
  d->url = is_url(path);
  d->live = live || aud_is_live(d->a);
  P.cur_a = d->a;
  P.cur_idx = idx;
  TrackMeta *t = meta_for(idx);
  if (!t) {
    /* reuse a slot that is neither heard nor this one */
    for (int i = 0; i < 3 && !t; i++)
      if (P.meta[i].idx != P.heard) t = &P.meta[i];
    if (!t) t = &P.meta[0];
  }
  img_free(&t->cover);
  t->idx = idx;
  if (ent) {
    fm_strlcpy(t->title, e->title, sizeof t->title);
    fm_strlcpy(t->artist, e->artist, sizeof t->artist);
    fm_strlcpy(t->album, e->album, sizeof t->album);
    fm_strlcpy(t->url, path, sizeof t->url);
  } else {
    fm_strlcpy(t->title, m.title, sizeof t->title);
    fm_strlcpy(t->artist, m.artist, sizeof t->artist);
    fm_strlcpy(t->album, m.album, sizeof t->album);
    t->url[0] = 0;
  }
  t->cover = cover;
  t->live = d->live;
  t->dur = !d->live && aud_length(d->a) > 0 ? (double)aud_length(d->a) / aud_rate(d->a) : 0;
  fm_strlcpy(t->codec, aud_codec(d->a), sizeof t->codec);
  t->rate = aud_rate(d->a);
  t->ch = d->ch;
  if (P.serr_idx == idx) P.serr_idx = -1;      /* it plays now */
  unlock();
  aud_meta_free(&m);
  fm_free(path);
  fm_free(headers);
  fm_free(ref);
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

/* Track idx could not be opened (lock held): a station stays put with its
** Retry; anything else goes on with the next track. */
static void open_failed(int idx) {
  const Ent *e = ent_at(idx);
  if (e && e->live) { P.dec_done = true; return; }
  int nx = next_index(idx, false);
  if (++P.fail_streak < P.n && nx >= 0 && nx != idx) { P.req = REQ_PLAY; P.req_index = nx; }
  else P.dec_done = true;
}

static int decoder(void *u) {
  DecCtx *c = (DecCtx *)u;
  Dec d = { NULL, NULL, -1, 0, false, false };
  float *src = (float *)fm_alloc(sizeof(float) * CHUNK * 2);
  float *tmp = (float *)fm_alloc(sizeof(float) * CHUNK * 8);
  int tmp_frames = CHUNK * 4;
  lock();
  while (!gone(c)) {
    if (P.req != REQ_NONE) {
      int req = P.req, idx = P.req_index;
      double sec = P.req_seek;
      P.req = REQ_NONE;
      if (req == REQ_PLAY) {
        P.dec_done = false;
        unlock();
        bool ok = dec_open(c, &d, idx);
        lock();
        if (gone(c)) break;
        if (P.req != REQ_NONE) continue;          /* superseded meanwhile */
        P.r_write = P.r_read;
        P.nmarks = 0;
        add_marker(idx, 0);
        if (!ok) {
          open_failed(idx);
          continue;
        }
        P.fail_streak = 0;
      } else if (req == REQ_SEEK && d.a && !d.live) {
        unlock();
        u64 fr = (u64)(sec * aud_rate(d.a));
        bool ok = aud_seek(d.a, fr);
        SDL_AudioStreamClear(d.st);
        lock();
        if (gone(c)) break;
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
      SDL_CondWait(g_cv, g_mx);
      continue;
    }
    if (!drain_stream(&d, tmp, tmp_frames) || P.ring_frames - (P.r_write - P.r_read) < (u64)CHUNK * 2) {
      /* the audio callback broadcasts after every read, so this sleeps until
      ** there is room; paused, nothing reads and the thread stays asleep
      ** (it used to wake 10 times a second, even paused in the mini player) */
      SDL_CondWaitTimeout(g_cv, g_mx, P.paused ? 2000 : 500);
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
    while (!gone(c) && P.req == REQ_NONE && !drain_stream(&d, tmp, tmp_frames))
      SDL_CondWaitTimeout(g_cv, g_mx, 100);
    if (gone(c)) break;
    if (P.req != REQ_NONE) continue;
    if (d.url) {
      /* a stream that stopped early broke off: say so, then go on */
      u64 len = aud_length(d.a), at = aud_tell(d.a);
      bool broke = d.live || (len > 0 && at + (u64)aud_rate(d.a) * 3 < (u64)((double)len * 0.97));
      if (broke) {
        set_serr(d.idx, d.live ? "the station stopped sending" : "the stream broke off");
        app_wake();
        if (d.live) {
          P.dec_done = true;
          continue;
        }
      }
    }
    int nx = d.live ? -1 : next_index(d.idx, false);
    if (nx < 0) {
      P.dec_done = true;
      continue;
    }
    unlock();
    bool ok = dec_open(c, &d, nx);
    lock();
    if (gone(c)) break;
    if (P.req != REQ_NONE) continue;
    add_marker(nx, 0);
    if (!ok) open_failed(nx);
    else P.fail_streak = 0;
  }
  unlock();
  dec_close(&d);
  fm_free(src);
  fm_free(tmp);
  fm_free(c);
  return 0;
}

/* ---- control (main thread) -------------------------------------------------------------- */

static bool sync_init(void) {
  if (!g_mx) g_mx = SDL_CreateMutex();
  if (!g_cv) g_cv = SDL_CreateCond();
  return g_mx && g_cv;
}

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
  P.serr_idx = -1;
  SDL_CondBroadcast(g_cv);
  unlock();
  P.icy[0] = 0;
  if (P.paused) { P.paused = false; SDL_PauseAudioDevice(P.dev, 0); }
  ui_redraw();
}

static bool heard_live(void) {
  const TrackMeta *t = meta_for(P.heard);
  const Ent *e = ent_at(P.heard);
  return (t && t->live) || (e && e->live);
}

static void request_seek(double sec) {
  lock();
  if (heard_live()) { unlock(); return; }
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
  SDL_CondBroadcast(g_cv);
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
  bool failed = P.serr_idx >= 0 && P.serr_idx == heard && P.dec_done;
  unlock();
  if (!p && (ended || failed)) {
    /* play after the end: start the playlist (or the track) again; a
    ** station that failed is simply tried again */
    request_play(failed || P.repeat == REP_ONE || heard_live() ? heard : P.order[0]);
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
  bool live = heard_live();
  unlock();
  if (dir < 0 && !live && position() > 3.0) { request_seek(0); return; }
  int idx = dir > 0 ? next_index(heard, true) : prev_index(heard);
  if (idx >= 0) request_play(idx);
}

static void retry(void) {
  int idx = P.serr_idx;
  if (idx >= 0 && idx < P.n) request_play(idx);
}

void audio_stop(void) {
  if (!P.active && !P.thr && !P.dev) return;
  if (P.dev) SDL_PauseAudioDevice(P.dev, 1);
  if (P.thr) {
    lock();
    g_tgen++;
    if (P.ctx) P.ctx->cancel = 1;
    SDL_CondBroadcast(g_cv);
    unlock();
    /* a decoder inside a slow network call must not freeze the window:
    ** it finds itself gone when the call returns and cleans up alone */
    if (P.urls) SDL_DetachThread(P.thr);
    else SDL_WaitThread(P.thr, NULL);
  }
  if (P.dev) SDL_CloseAudioDevice(P.dev);
  if (P.cover_tex) SDL_DestroyTexture(P.cover_tex);
  for (int i = 0; i < 3; i++) img_free(&P.meta[i].cover);
  fm_free(P.ring);
  fm_free(P.order);
  arena_free(&P.arena);
  float vol = P.volume;
  memset(&P, 0, sizeof P);
  P.volume = vol;
  P.serr_idx = -1;
  ui_redraw();
}

/* ---- open / close ------------------------------------------------------------------- */

/* Starts a new playlist from scratch (whatever played is stopped). */
static void start_list(FmArena arena, char **paths, Ent *ents, int cnt, int index, bool urls, bool viewer) {
  bool shuffle = P.shuffle;
  int repeat = P.repeat;
  float vol = P.volume;
  audio_stop();
  P.shuffle = shuffle;
  P.repeat = repeat;
  P.volume = vol > 0 ? vol : (conf.volume > 0 ? conf.volume : 0.8f);
  P.arena = arena;
  P.paths = paths;
  P.ents = ents;
  P.urls = urls;
  P.n = cnt;
  P.order = (int *)fm_alloc(sizeof(int) * (size_t)cnt);
  make_order(index);
  for (int i = 0; i < 3; i++) P.meta[i].idx = -1;
  P.cover_idx = -1;
  P.serr_idx = -1;
  P.noted = -1;
  P.icy_idx = -1;
  viz_reset();
  P.heard = index;
  video_bg_stop();                     /* one sound at a time */
  P.active = true;
  P.viewer_open = viewer;
  view_chrome_poke(&P.chrome);
  if (!sync_init() || !device_open()) return;      /* the player shows the error */
  eq_follow(&P.eq, P.rate, g_mx);
  P.ctx = (DecCtx *)fm_calloc(1, sizeof *P.ctx);
  P.ctx->tgen = g_tgen;
  P.thr = fm_thread_create(decoder, "audio", P.ctx);
  if (!P.thr) {
    fm_free(P.ctx);
    P.ctx = NULL;
  }
  request_play(index);
  SDL_PauseAudioDevice(P.dev, 0);
}

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
  start_list(arena, paths, NULL, cnt, index, is_url(path), true);
  return true;
}

static void aud_view_close(void) {
  P.viewer_open = false;
  P.info_open = false;
  if (P.active && !P.dev) audio_stop();       /* nothing could play: forget it */
  ui_redraw();
}

void audio_set_hooks(const FmAudioHooks *h) {
  if (h) g_hooks = *h;
  else memset(&g_hooks, 0, sizeof g_hooks);
}

static char *dup_or(FmArena *a, const char *s, const char *def) {
  return arena_strdup(a, s && s[0] ? s : def);
}

bool audio_play_entries(const FmAudioEntry *list, int n, int index) {
  if (!list || n <= 0 || index < 0 || index >= n) return false;
  FmArena arena;
  arena_init(&arena, 16 * 1024);
  char **paths = (char **)arena_alloc(&arena, sizeof(char *) * (size_t)n);
  Ent *ents = (Ent *)arena_alloc(&arena, sizeof(Ent) * (size_t)n);
  bool urls = false;
  for (int i = 0; i < n; i++) {
    const FmAudioEntry *s = &list[i];
    Ent *e = &ents[i];
    e->url = dup_or(&arena, s->url, "");
    e->title = dup_or(&arena, s->title, "");
    e->artist = dup_or(&arena, s->artist, "");
    e->album = dup_or(&arena, s->album, "");
    e->art = dup_or(&arena, s->art_url, "");
    e->headers = s->headers && s->headers[0] ? arena_strdup(&arena, s->headers) : NULL;
    e->ref = s->ref && s->ref[0] ? arena_strdup(&arena, s->ref) : NULL;
    e->live = s->live;
    paths[i] = e->url;
    urls = urls || !e->url[0] || is_url(e->url);
  }
  if (P.active && P.thr && P.dev) {
    /* replace in place: the decoder may be stuck connecting to the old
    ** stream, and waiting for it would freeze the window */
    int *order = (int *)fm_alloc(sizeof(int) * (size_t)n);
    FmArena old = P.arena;
    int *old_order = P.order;
    lock();
    g_gen++;
    P.arena = arena;
    P.paths = paths;
    P.ents = ents;
    P.urls = P.urls || urls;
    P.n = n;
    P.order = order;
    make_order(index);
    for (int i = 0; i < 3; i++) {
      img_free(&P.meta[i].cover);
      P.meta[i].idx = -1;
    }
    unlock();
    arena_free(&old);
    fm_free(old_order);
    P.cover_idx = -1;
    P.noted = -1;
    P.icy_idx = -1;
    P.list_scroll.y = 0;
    viz_reset();
    request_play(index);
    return true;
  }
  start_list(arena, paths, ents, n, index, urls, false);
  return P.active && P.dev;
}

void audio_show_player(void) {
  if (!P.active) return;
  if (app.viewer != &g_view_audio) {
    if (app.viewer && app.viewer->close) app.viewer->close();
    app.viewer = &g_view_audio;
  }
  P.viewer_open = true;
  view_chrome_poke(&P.chrome);
  ui_redraw();
}

static bool buffering_locked(void) {
  if (P.paused || P.ended) return false;
  if (P.opening && P.open_idx == P.heard) return true;
  if (P.req == REQ_PLAY) return true;
  return P.starved && !P.dec_done && SDL_GetTicks64() - P.starved_at >= STARVE_SHOW_MS;
}

int audio_state(char *ref, size_t cap) {
  if (ref && cap) ref[0] = 0;
  if (!P.active || !g_mx) return AUDIO_IDLE;
  lock();
  int st;
  const Ent *e = ent_at(P.heard);
  if (ref && cap && e && e->ref) fm_strlcpy(ref, e->ref, cap);
  if (!P.dev) st = AUDIO_FAILED;
  else if (P.serr_idx >= 0 && P.serr_idx == P.heard && (P.dec_done || P.opening)) st = AUDIO_FAILED;
  else if (buffering_locked()) st = AUDIO_BUFFERING;
  else if (P.paused || P.ended) st = AUDIO_PAUSED;
  else st = AUDIO_PLAYING;
  unlock();
  return st;
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
  bool entry, live, buffering, connecting;
  char icy[256];              /* live: the station's current title */
  char serr[200];             /* a failed entry, with Retry */
  bool serr_here;             /* ... and it is the one heard */
  char url[1024];
} NowPlaying;

static void now_playing(NowPlaying *np) {
  memset(np, 0, sizeof *np);
  lock();
  np->idx = P.heard;
  np->ended = P.ended;
  np->entry = P.ents != NULL;
  fm_strlcpy(np->error, P.error, sizeof np->error);
  if (P.dev) P.error[0] = 0;
  const Ent *e = ent_at(P.heard);
  TrackMeta *t = meta_for(P.heard);
  np->live = (t && t->live) || (e && e->live);
  np->buffering = buffering_locked();
  np->connecting = (P.opening && P.open_idx == P.heard) || P.req == REQ_PLAY;
  if (P.serr_idx >= 0) {
    fm_strlcpy(np->serr, P.serr, sizeof np->serr);
    np->serr_here = P.serr_idx == P.heard && (P.dec_done || P.opening);
    if (np->serr_here) np->buffering = false;
  }
  if (t) {
    fm_strlcpy(np->title, t->title, sizeof np->title);
    if (t->artist[0] && t->album[0] && !np->live)
      fm_snprintf(np->artist, sizeof np->artist, "%s \xC2\xB7 %s", t->artist, t->album);
    else fm_strlcpy(np->artist, t->artist[0] ? t->artist : t->album, sizeof np->artist);
    np->dur = t->dur;
    fm_strlcpy(np->codec, t->codec, sizeof np->codec);
    np->rate = t->rate;
    np->ch = t->ch;
    fm_strlcpy(np->url, t->url, sizeof np->url);
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
  } else if (e) {
    fm_strlcpy(np->title, e->title, sizeof np->title);
    fm_strlcpy(np->artist, e->artist, sizeof np->artist);
  }
  /* live: the ICY title, polled once a second and only for the station heard */
  if (P.icy_idx != P.heard) {
    P.icy[0] = 0;
    P.icy_idx = P.heard;
    P.icy_poll = 0;
  }
  if (np->live && P.cur_a && P.cur_idx == P.heard && ui.now - P.icy_poll >= 1000) {
    char now[256];
    aud_now_playing(P.cur_a, now, sizeof now);
    P.icy_poll = ui.now;
    if (strcmp(now, P.icy) != 0) fm_strlcpy(P.icy, now, sizeof P.icy);
  }
  fm_strlcpy(np->icy, P.icy, sizeof np->icy);
  double pos = P.base_sec + (double)(P.r_read - P.base_frame) / FM_MAX(1, P.rate);
  /* "Recently played": once per entry that really starts */
  const char *note = NULL;
  if (e && e->ref && t && (P.noted != P.heard || P.noted_gen != g_gen)) {
    P.noted = P.heard;
    P.noted_gen = g_gen;
    note = e->ref;
  }
  unlock();
  if (note && g_hooks.played) g_hooks.played(note);
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

/* Generated gradient with a glyph: no cover, or while artwork loads. */
static void cover_fallback(FmRect r, int idx, float radius) {
  char name[256];
  u32 h = ui_id(idx >= 0 && idx < P.n ? display_name(idx, name, sizeof name) : "x");
  FmColor a = kAccents[h % UI_ACCENTS], b = kAccents[(h / UI_ACCENTS + 3) % UI_ACCENTS];
  gfx_rrect_vgrad(r, radius, a, col_mix(b, FM_HEX(0x101018), 0.35f));
  const Ent *e = ent_at(idx);
  float is = r.w * 0.38f;
  icon_draw(e && e->live ? IC_RADIO : IC_MUSIC, rect_center(r, is, is), FM_RGBA(255, 255, 255, 210));
}

/* Entry artwork by URL through the online thumbnail loader. Station logos
** are fitted on a light tile (many are wide or transparent); covers fill. */
static bool cover_art(FmRect r, int idx, float radius) {
  const Ent *e = ent_at(idx);
  if (!e || !e->art[0]) return false;
  float fade = 1;
  int px = (int)FM_MAX(r.w, r.h);
  SDL_Texture *tex = othumb_get_ex(e->art, px, e->live ? 0.0f : 1.0f, NULL, &fade);
  if (!tex) {
    cover_fallback(r, idx, radius);
    return true;
  }
  if (fade < 1) {
    cover_fallback(r, idx, radius);
    ui_animate();
  }
  if (e->live) {
    int tw = 1, th = 1;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    gfx_rrect(r, radius, col_alpha(FM_HEX(0xF6F7FA), fade));
    float k = FM_MIN(r.w * 0.8f / (float)tw, r.h * 0.8f / (float)th);
    FmRect d = rect_center(r, (float)tw * k, (float)th * k);
    gfx_tex(tex, NULL, d, col_alpha(FM_HEX(0xFFFFFF), fade));
  } else {
    gfx_tex_rounded(tex, r, radius, col_alpha(FM_HEX(0xFFFFFF), fade));
  }
  return true;
}

static void cover_card(FmRect r, int idx, float radius) {
  gfx_shadow(r, radius, DP(18), T.shadow);
  if (cover_art(r, idx, radius)) return;
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
  cover_fallback(r, idx, radius);
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

/* The red "LIVE" pill; returns its width. */
static float live_pill(float x, float cy, float fs) {
  const char *l = "LIVE";
  float h = font_line_h(fs) + DP(6), w = font_width(FONT_BOLD, fs, l, -1) + DP(26);
  FmRect b = { x, cy - h * 0.5f, w, h };
  gfx_rrect(b, h * 0.5f, T.danger);
  gfx_circle(b.x + DP(10), cy, DP(3), FM_RGBA(255, 255, 255, 255));
  font_draw(FONT_BOLD, fs, b.x + DP(17), cy - font_line_h(fs) * 0.5f, l, -1, FM_RGBA(255, 255, 255, 255));
  return w;
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

/* A turning arc round a button while a stream connects or buffers. */
static void busy_ring(FmRect b, FmColor c) {
  float s = FM_MIN(b.w, b.h);
  float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
  float a = (float)(ui.now % 1000) / 1000.0f * 6.2831853f;
  gfx_arc(cx, cy, s * 0.5f + DP(4), DP(2.5f), a, a + 1.9f, c);
  tick(&g_tick_busy, 33);
}

static void controls(FmRect r, bool playing, bool busy) {
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
  FmRect pb = FM_RECT(x, cy - big * 0.5f, big, big);
  if (busy) busy_ring(pb, T.accent);
  if (round_btn(ui_id("aud.play"), pb, playing ? IC_PAUSE : IC_PLAY, true, false, "Play / pause (Space)"))
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
    const Ent *e = ent_at(i);
    if (cur) icon_draw(e && e->live ? IC_RADIO : IC_MUSIC, rect_center(num, DP(18), DP(18)), T.accent);
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
  if (np->entry) {
    const Ent *e = ent_at(np->idx);
    const char *keys[] = { "Title", "Artist", "Album", "Length", "Format", "Stream" };
    const char *vals[] = { np->title, e && e->artist[0] ? e->artist : "unknown", e && e->album[0] ? e->album : "-",
                           np->live ? "live" : np->dur > 0 ? dur : "unknown", np->codec[0] ? fmt : "-",
                           np->url[0] ? np->url : "not opened yet" };
    view_info_dialog(ui_id("aud.info"), np->live ? "Station info" : "Track info", keys, vals, FM_COUNT(keys),
                     &P.info_open);
    return;
  }
  FmStat st;
  const char *path = P.paths[np->idx];
  fm_fmt_size(plat_stat(path, &st) ? st.size : 0, size, sizeof size);
  const char *keys[] = { "Title", "Artist", "Length", "Format", "Size", "Path" };
  const char *vals[] = { np->title, np->artist, np->dur > 0 ? dur : "unknown", fmt, size, path };
  view_info_dialog(ui_id("aud.info"), "Track info", keys, vals, FM_COUNT(keys), &P.info_open);
}

/* The failed stream's message with Retry and dismiss. */
static void error_banner(FmRect r, const NowPlaying *np) {
  u32 id = ui_id("aud.serr");
  gfx_rrect(r, DP(12), col_mix(T.surface, T.danger, T.dark ? 0.22f : 0.12f));
  FmRect in = rect_inset2(r, DP(12), DP(6));
  float is = DP(18);
  icon_draw(IC_WARN, FM_RECT(in.x, in.y + (in.h - is) * 0.5f, is, is), T.danger);
  in.x += is + DP(10);
  in.w -= is + DP(10);
  float bs = in.h;
  if (ui_icon_btn(ui_idn(id, 1), rect_cut_right(&in, bs), IC_CLOSE, T.text2, "Dismiss")) {
    lock();
    P.serr_idx = -1;
    unlock();
  }
  const char *l = "Retry";
  float bw = font_width(FONT_BOLD, ui.m.font, l, -1) + DP(50);
  FmRect b = rect_cut_right(&in, bw);
  if (ui_button(ui_idn(id, 2), rect_center(b, bw, FM_MIN(b.h, DP(34))), IC_REFRESH, l, UI_BTN_TONAL)) retry();
  rect_cut_right(&in, DP(6));
  char t[260];
  fm_snprintf(t, sizeof t, "Couldn't play %s", np->serr);
  float fs = ui.m.font_small;
  font_draw_ellipsis(FONT_REGULAR, fs, in.x, in.y + (in.h - font_line_h(fs)) * 0.5f, t, in.w, T.text);
}

/* ---- the full player ------------------------------------------------------------------ */

enum { AM_OPEN = 1, AM_SHARE, AM_INFO, AM_STOP, AM_VIZ, AM_EQ, AM_COPY };

static bool heart_state(int idx, bool *fav) {
  const Ent *e = ent_at(idx);
  if (e) {
    if (!e->ref || !g_hooks.is_fav || !g_hooks.fav_toggle) return false;
    *fav = g_hooks.is_fav(e->ref);
    return true;
  }
  if (idx < 0 || idx >= P.n) return false;
  *fav = lib_is_fav(P.paths[idx]);
  return true;
}

static void heart_toggle(int idx) {
  const Ent *e = ent_at(idx);
  if (e) {
    if (e->ref && g_hooks.fav_toggle) g_hooks.fav_toggle(e->ref);
  } else if (idx >= 0 && idx < P.n) {
    lib_fav_toggle(P.paths[idx]);
  }
}

static void aud_view_frame(FmRect area) {
  gfx_rect(area, T.bg);
  if (!P.active) { app_close_viewer(); return; }
  NowPlaying np;
  now_playing(&np);
  bool playing = P.dev && !P.paused && !np.ended && !np.serr_here;
  bool busy = playing && np.buffering;
  if (P.dev) eq_follow(&P.eq, P.rate, g_mx);

  /* keys */
  if (ui_key(SDLK_SPACE, 0) || ui_key(SDLK_AUDIOPLAY, 0) || ui_key(SDLK_k, 0)) set_paused(playing);
  if (!np.live && ui_key(SDLK_RIGHT, 0)) request_seek(np.pos + 5);
  if (!np.live && ui_key(SDLK_LEFT, 0)) request_seek(np.pos - 5);
  if (ui_key(SDLK_UP, 0)) set_volume(P.volume + 0.05f);
  if (ui_key(SDLK_DOWN, 0)) set_volume(P.volume - 0.05f);
  if (ui_key(SDLK_n, 0) || ui_key(SDLK_AUDIONEXT, 0)) skip(1);
  if (ui_key(SDLK_p, 0) || ui_key(SDLK_AUDIOPREV, 0)) skip(-1);

  bool wide = !ui.portrait && area.w >= DP(720);
  char sub[48];
  if (P.n > 1) fm_snprintf(sub, sizeof sub, "%d / %d", np.idx + 1, P.n);
  else sub[0] = 0;
  bool fav = false, has_heart = np.idx >= 0 && heart_state(np.idx, &fav);
  FmRect act;
  /* more, playlist (narrow), heart, visualizer: room for each one drawn */
  int nact = 2 + (!wide && P.n > 1 ? 1 : 0) + (has_heart ? 1 : 0);
  if (view_topbar(area, VIEW_BAR_THEME, 1.0f, np.live ? "Live radio" : "Now playing", sub, nact, &act)) {
    app_close_viewer();
    return;
  }
  u32 mid = ui_id("aud.menu");
  if (view_bar_btn(&act, ui_id("aud.more"), IC_MORE, "More", VIEW_BAR_THEME, 1, false)) {
    FmMenuItem items[8];
    int k = 0;
    memset(items, 0, sizeof items);
    items[k].id = AM_INFO; items[k].icon = IC_INFO; items[k++].label = np.live ? "Station info" : "Track info";
    items[k].id = AM_VIZ; items[k].icon = IC_EQUALIZER; items[k++].label = "Visualizer";
    items[k].id = AM_EQ; items[k].icon = IC_EQUALIZER; items[k++].label = "Equalizer";
    if (np.entry) {
      items[k].id = AM_COPY; items[k].icon = IC_LINK; items[k++].label = "Copy stream link";
    } else {
      items[k].id = AM_SHARE; items[k].icon = IC_SHARE; items[k++].label = "Share";
      items[k].id = AM_OPEN; items[k].icon = IC_OPEN_WITH; items[k++].label = "Open with system app";
    }
    items[k++].flags = UI_MI_SEP;
    items[k].id = AM_STOP; items[k].icon = IC_STOP; items[k].label = "Stop and close"; items[k++].flags = UI_MI_DANGER;
    ui_menu_open(mid, act.x + act.w, area.y + ui.m.bar_h, items, k);
  }
  if (!wide && P.n > 1 && view_bar_btn(&act, ui_id("aud.listbtn"), IC_LIST, "Playlist", VIEW_BAR_THEME, 1, P.show_list)) {
    P.show_list = !P.show_list;
    P.viz_open = false;
  }
  /* favorite: the media library's heart for files, the caller's for entries */
  if (has_heart && view_bar_btn(&act, ui_id("aud.favbtn"), fav ? IC_HEART_FILL : IC_HEART,
                                fav ? "Remove from favorites" : "Add to favorites", VIEW_BAR_THEME, 1, fav))
    heart_toggle(np.idx);
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
    case AM_COPY:
      if (np.url[0]) { ui_clipboard_set(np.url); ui_toast("Link copied"); }
      else ui_toast("Not connected yet");
      break;
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
    if (!np.entry && np.idx >= 0 && np.idx < P.n &&
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
  FmRect errr = { 0, 0, 0, 0 };
  if (np.serr[0] && P.serr_idx >= 0) {
    errr = rect_cut_bottom(&left, DP(ui.touch_mode ? 52 : 46));
    errr = rect_center(errr, FM_MIN(errr.w, DP(560)), errr.h);
    rect_cut_bottom(&left, DP(10));
  }
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
  bool moving = live && viz_feed(playing && !busy);
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
  /* title and artist; a station shows what it plays now under its name */
  if (!panel_left) {
    float tw = font_width(FONT_BOLD, ui.m.font_big, np.title, -1);
    float tx = titles.x + FM_MAX(0.0f, (titles.w - tw) * 0.5f);
    font_draw_ellipsis(FONT_BOLD, ui.m.font_big, tx, titles.y, np.title, titles.w, T.text);
    const char *a = np.live && np.icy[0] ? np.icy : np.artist;
    FmColor ac = np.live && np.icy[0] ? T.accent : T.text2;
    float is = np.live && np.icy[0] ? DP(16) : 0;
    float aw = font_width(FONT_REGULAR, ui.m.font, a, -1) + (is > 0 ? is + DP(6) : 0);
    float ax = titles.x + FM_MAX(0.0f, (titles.w - aw) * 0.5f);
    float ay = titles.y + th + DP(4);
    if (is > 0) {
      icon_draw(IC_MUSIC, FM_RECT(ax, ay + (ah - is) * 0.5f, is, is), ac);
      ax += is + DP(6);
    }
    font_draw_ellipsis(FONT_REGULAR, ui.m.font, ax, ay, a, titles.x + titles.w - ax, ac);
  }
  /* visualizer strip; a tap cycles the styles */
  if (vh > 0) {
    FmRect vr = rect_center(vizr, FM_MIN(vizr.w, DP(560)), vizr.h);
    viz_draw(&conf.viz, vr, 0);
    int f = ui_hit(ui_id("aud.viz"), vr);
    if (f & UI_CLICK) viz_tap();
    if (f & UI_HOVER) ui_set_cursor(SDL_SYSTEM_CURSOR_HAND);
  }
  if (errr.w > 0) error_banner(errr, &np);
  /* seek + times; a station has no seek bar: LIVE, the format and the clock */
  FmRect sr = rect_center(seek, FM_MIN(seek.w, DP(560)), seek.h);
  FmRect tr = { sr.x, times.y, sr.w, times.h };
  if (np.live) {
    float fs = ui.m.font_small;
    float pw = font_width(FONT_BOLD, fs, "LIVE", -1) + DP(26);
    live_pill(sr.x + (sr.w - pw) * 0.5f, sr.y + sr.h * 0.5f, fs);
  } else {
    seek_bar(sr, &np);
  }
  char a[32], b[96];
  if (busy || (np.connecting && !np.serr_here)) {
    const char *l = np.connecting ? "Connecting\xE2\x80\xA6" : "Buffering\xE2\x80\xA6";
    ui_label(tr, l, FONT_REGULAR, ui.m.font_small, T.accent, UI_CENTER);
  } else if (np.live) {
    view_fmt_time(np.pos, a, sizeof a);
    if (np.codec[0]) fm_snprintf(b, sizeof b, "%s  \xC2\xB7  %s", np.codec, np.ch >= 2 ? "stereo" : "mono");
    else b[0] = 0;
    ui_label(tr, a, FONT_REGULAR, ui.m.font_small, T.text2, UI_LEFT);
    ui_label(tr, b, FONT_REGULAR, ui.m.font_small, T.text2, UI_RIGHT);
  } else {
    double shown = P.seek_drag ? P.seek_t * np.dur : np.pos;
    view_fmt_time(shown, a, sizeof a);
    if (np.dur > 0) {
      char r2[32];
      view_fmt_time(np.dur - shown, r2, sizeof r2);
      fm_snprintf(b, sizeof b, "-%s", r2);
    } else {
      fm_strlcpy(b, "--:--", sizeof b);
    }
    ui_label(tr, a, FONT_REGULAR, ui.m.font_small, T.text2, UI_LEFT);
    ui_label(tr, b, FONT_REGULAR, ui.m.font_small, T.text2, UI_RIGHT);
  }
  controls(ctl, playing, busy || (np.connecting && !np.serr_here));
  volume_row(vol);

  if (side && P.viz_open) {
    if (!viz_panel(right)) P.viz_open = false;
  } else if (side) {
    gfx_rrect(right, ui.m.radius, T.surface);
    FmRect hdr = rect_cut_top(&right, DP(44));
    char hb[48];
    fm_snprintf(hb, sizeof hb, "%s  \xC2\xB7  %d", np.live ? "Stations" : "Playlist", P.n);
    ui_label(rect_inset2(hdr, DP(16), 0), hb, FONT_BOLD, ui.m.font, T.text, UI_LEFT);
    playlist(rect_inset2(right, DP(6), DP(2)), np.idx);
  }

  if (P.info_open) info_card(&np);
  if ((live && (playing || moving)) || P.seek_drag) ui_animate();
  else if (playing) tick(&g_tick_clock, np.live ? 1000 : 250);   /* no visualizer: the clock, the station title */
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
  eq_follow(&P.eq, P.rate, g_mx);
  if (np.ended && !P.paused) { P.paused = true; SDL_PauseAudioDevice(P.dev, 1); }
  bool playing = !P.paused && !np.ended && !np.serr_here;
  bool busy = playing && (np.buffering || np.connecting);
  gfx_rect(r, T.surface2);
  ui_divider(r.x, r.x + r.w, r.y);
  /* progress line along the top */
  if (np.dur > 0 && !np.live)
    gfx_rect(FM_RECT(r.x, r.y, r.w * (float)FM_CLAMP(np.pos / np.dur, 0.0, 1.0), DP(2)), T.accent);
  FmRect c = rect_inset2(r, DP(8), DP(6));
  float h = c.h;
  u32 id = ui_id("mini.body");
  bool close = ui_icon_btn(ui_id("mini.close"), rect_cut_right(&c, FM_MAX(h, ui.m.hit)), IC_CLOSE, T.text2, "Stop");
  bool next = P.n > 1 && ui_icon_btn(ui_id("mini.next"), rect_cut_right(&c, FM_MAX(h, ui.m.hit)), IC_NEXT, T.text, "Next");
  FmRect pb = rect_cut_right(&c, FM_MAX(h, ui.m.hit));
  float ps = FM_MIN(pb.h, DP(40));
  gfx_circle(pb.x + pb.w * 0.5f, pb.y + pb.h * 0.5f, ps * 0.5f, T.accent);
  if (busy) busy_ring(rect_center(pb, ps - DP(4), ps - DP(4)), T.accent);
  bool pp = ui_icon_btn(ui_id("mini.play"), pb, playing ? IC_PAUSE : IC_PLAY, T.on_accent, playing ? "Pause" : "Play");
  bool again = np.serr_here &&
               ui_icon_btn(ui_id("mini.retry"), rect_cut_right(&c, FM_MAX(h, ui.m.hit)), IC_REFRESH, T.danger, "Retry");
  FmRect cov = rect_cut_left(&c, h);
  cover_card(rect_inset(cov, DP(2)), np.idx, DP(8));
  rect_cut_left(&c, DP(10));
  int f = ui_hit(id, c);
  /* second line: the error, the station's current title, or the artist */
  const char *line2 = np.artist;
  FmColor c2 = T.text2;
  char buf[260];
  if (np.serr_here) {
    fm_snprintf(buf, sizeof buf, "Couldn't play %s", np.serr);
    const char *colon = strstr(buf, "\xE2\x80\x9D: ");
    if (colon) { fm_snprintf(buf, sizeof buf, "Couldn't play: %s", colon + 5); }
    line2 = buf;
    c2 = T.danger;
  } else if (busy) {
    line2 = np.connecting ? "Connecting\xE2\x80\xA6" : "Buffering\xE2\x80\xA6";
    c2 = T.accent;
  } else if (np.live && np.icy[0]) {
    line2 = np.icy;
  }
  float lh = font_line_h(ui.m.font), sh = line2[0] || np.live ? font_line_h(ui.m.font_small) : 0;
  float y = c.y + (c.h - lh - sh) * 0.5f;
  font_draw_ellipsis(FONT_BOLD, ui.m.font, c.x, y, np.title, c.w, T.text);
  if (sh > 0) {
    float x = c.x;
    if (np.live && !np.serr_here) {
      gfx_circle(x + DP(4), y + lh + sh * 0.5f, DP(3.5f), T.danger);
      x += DP(13);
      if (!line2[0]) line2 = "Live";
    }
    font_draw_ellipsis(FONT_REGULAR, ui.m.font_small, x, y + lh, line2, c.x + c.w - x, c2);
  }
  if (close) { audio_stop(); return; }
  if (next) skip(1);
  if (again) retry();
  else if (pp) set_paused(playing);
  if ((f & UI_CLICK) && np.idx >= 0 && np.idx < P.n) {
    if (np.entry) audio_show_player();
    else app_open(P.paths[np.idx], (const char *const *)P.paths, P.n, np.idx);
    return;
  }
  /* refresh the progress line (and a station's title) once a second, without animating */
  if (playing) tick(&g_tick_mini, 1000);
}

const FmViewer g_view_audio = { "audio", aud_view_open, aud_view_frame, aud_view_close };
