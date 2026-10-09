/* fdec_vid_amc.c -- Android video backend: the decoders the OS ships, through
** the NDK media API (AMediaExtractor + AMediaCodec).
**
** Plays what the phone plays: MP4, MKV, WebM, 3GP, TS ... with H.264, HEVC,
** VP8, VP9, AV1 (when the device has it), AAC, Opus, Vorbis, MP3, FLAC.
**
** Design decisions:
**   - libmediandk.so is opened with dlopen, never linked, and only API 21
**     calls are used, so the APK starts on any OS and a missing library or
**     symbol means "open with the system player", not a crash. The NDK
**     headers are included for the types only (__typeof__ never links), and
**     format keys are string literals because AMEDIAFORMAT_KEY_* are
**     variables exported by the library.
**   - The file is opened with open() and handed over as an fd, so plain
**     paths work without a content URI.
**   - http(s) URLs (online videos) are read through our own FmNetStream
**     (bounded ring, chunked ranges, cancellable) handed to the extractor as
**     an AMediaDataSource (Android 9+). Not AMediaExtractor_setDataSource(url):
**     the system's HTTP source prefetches without limit and cannot be
**     interrupted, which froze the player when it was closed mid-open. The
**     data-source calls are looked up on their own, so older devices still
**     play files; their URLs fall to the built-in decoder.
**   - Video decodes to ByteBuffers (no Surface) in YUV420Flexible. Planar
**     (19) output is handed out in place: the output buffer is held until
**     the next decode call, so the Y plane is never copied. Semi-planar
**     (21) only needs its chroma split into two quarter-size planes.
**     For 0x7F420888 the layout is not reported, so it is probed once per
**     format from the chroma bytes: in NV12 neighbouring bytes are U,V
**     pairs and differ more than bytes two apart; in I420 the reverse.
**     Vendor (tiled) formats are refused with VID_EV_ERROR, never garbage.
**   - Audio arrives as PCM16 (or float/8/32-bit when the codec says so), is
**     converted to float and downmixed to at most stereo. If the decoder's
**     rate differs from the one announced at open (HE-AAC SBR), a linear
**     resampler keeps the announced rate, because the viewer opens its
**     audio device from FmVidInfo.
**   - One pull loop like ff_decode: drain whichever codec has output, else
**     feed one extractor sample, else wait a few ms on output. Every loop
**     is bounded and a codec that goes quiet for seconds ends the stream.
**   - Flushing before the first output loses the codec-specific data, so
**     after such a seek csd-0..2 are queued again by hand.
*/
#include "fdec_vid_int.h"
#include "fnetstream.h"
#include "fplat.h"

#ifdef FM_ANDROID

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

/* ---- libmediandk loader --------------------------------------------------- */

#define AMC_FUNCS(X) \
  X(AMediaExtractor_new) X(AMediaExtractor_delete) X(AMediaExtractor_setDataSourceFd) \
  X(AMediaExtractor_getTrackCount) X(AMediaExtractor_getTrackFormat) \
  X(AMediaExtractor_selectTrack) X(AMediaExtractor_readSampleData) \
  X(AMediaExtractor_getSampleTrackIndex) X(AMediaExtractor_getSampleTime) \
  X(AMediaExtractor_advance) X(AMediaExtractor_seekTo) \
  X(AMediaFormat_delete) X(AMediaFormat_getString) X(AMediaFormat_getInt32) \
  X(AMediaFormat_getInt64) X(AMediaFormat_getFloat) X(AMediaFormat_setInt32) \
  X(AMediaFormat_getBuffer) \
  X(AMediaCodec_createDecoderByType) X(AMediaCodec_createCodecByName) X(AMediaCodec_configure) X(AMediaCodec_start) \
  X(AMediaCodec_stop) X(AMediaCodec_delete) X(AMediaCodec_flush) \
  X(AMediaCodec_dequeueInputBuffer) X(AMediaCodec_getInputBuffer) \
  X(AMediaCodec_queueInputBuffer) X(AMediaCodec_dequeueOutputBuffer) \
  X(AMediaCodec_getOutputBuffer) X(AMediaCodec_releaseOutputBuffer) \
  X(AMediaCodec_getOutputFormat)

#define AMC_FIELD(f) __typeof__(f) *f;
static struct { AMC_FUNCS(AMC_FIELD) } nd;
static bool g_nd_ok;

/* optional (Android 9+): reading URLs through FmNetStream. Typed by hand:
** the headers make API 28 declarations unusable below minSdk 28. */
#define AMC_DS_FUNCS(X) \
  X(AMediaDataSource_new) X(AMediaDataSource_delete) X(AMediaDataSource_setUserdata) \
  X(AMediaDataSource_setReadAt) X(AMediaDataSource_setGetSize) X(AMediaDataSource_setClose) \
  X(AMediaExtractor_setDataSourceCustom)
static struct {
  AMediaDataSource *(*AMediaDataSource_new)(void);
  void (*AMediaDataSource_delete)(AMediaDataSource *);
  void (*AMediaDataSource_setUserdata)(AMediaDataSource *, void *);
  void (*AMediaDataSource_setReadAt)(AMediaDataSource *, AMediaDataSourceReadAt);
  void (*AMediaDataSource_setGetSize)(AMediaDataSource *, AMediaDataSourceGetSize);
  void (*AMediaDataSource_setClose)(AMediaDataSource *, AMediaDataSourceClose);
  media_status_t (*AMediaExtractor_setDataSourceCustom)(AMediaExtractor *, AMediaDataSource *);
} ds;
static bool g_ds_ok;
static pthread_once_t g_nd_once = PTHREAD_ONCE_INIT;

static void amc_load(void) {
  void *h = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) { fm_log("video: libmediandk.so not found"); return; }
  bool ok = true;
#define AMC_SYM(f) if (ok && !(*(void **)&nd.f = dlsym(h, #f))) { fm_log("video: no %s", #f); ok = false; }
  AMC_FUNCS(AMC_SYM)
#undef AMC_SYM
  if (!ok) { dlclose(h); memset(&nd, 0, sizeof nd); return; }
  bool dok = true;
#define AMC_DS_SYM(f) if (dok && !(*(void **)&ds.f = dlsym(h, #f))) dok = false;
  AMC_DS_FUNCS(AMC_DS_SYM)
#undef AMC_DS_SYM
  g_ds_ok = dok;
  g_nd_ok = true;                      /* the library stays loaded for the process */
}

/* ---- state ---------------------------------------------------------------- */

enum {
  COLOR_PLANAR = 19, COLOR_SEMI = 21, COLOR_FLEX = 0x7F420888,
  AMC_WAIT_US = 4000,                  /* output wait when nothing can be fed */
  AMC_IDLE_MAX = 750,                  /* ~3 s of silence from the codecs */
  AMC_GUARD = 200000,                  /* iterations per decode call */
};
enum { LAY_UNKNOWN, LAY_PLANAR, LAY_SEMI };

typedef struct AmcTrack {
  AMediaCodec *codec;
  AMediaFormat *fmt;                   /* track format: csd for re-feeding */
  int idx;                             /* extractor track */
  int queued;                          /* inputs since start or flush */
  int csd_next;                        /* next csd-N to re-queue, -1 none */
  bool in_eos, out_eos, got_out, got_fmt, dead;
} AmcTrack;

/* A URL as the extractor's data source. The extractor may call from its own
** threads, so reads are serialised. */
#define NET_WINS 4

typedef struct AmcNet {
  FmNetStream *ns;                     /* the first window's connection: size, live, abort */
  char *url;                           /* for the other windows' connections */
  pthread_mutex_t mx;
  /* windows of bytes read (see net_read_at), each with its own connection
  ** (opened on first use; w[0].ns == ns) */
  struct { u8 *buf; i64 pos; size_t len; u64 used; FmNetStream *ns; i64 run; bool wide; } w[NET_WINS];
  u64 tick;
} AmcNet;

typedef struct Amc {
  int fd;
  AmcNet *net;                         /* URLs: our stream reader */
  AMediaDataSource *src;
  /* HLS (fnetstream joins the segments): seeking swaps in an extractor that
  ** reads from "<url>#t=<s>"; the codecs stay. Older TS counts from 0 again
  ** after such a start, so its timestamps get hls_start added. */
  char *url;
  bool hls, t_checked;
  double hls_start;
  int64_t t_off_us;
  AMediaExtractor *ex;
  AmcTrack v, a;
  FmVidInfo *info;
  bool ex_eos, progress;
  int early;                           /* frames dropped for an impossible layout */
  double skip_until;
  /* Android's AAC reader gives every sample of an unsized (live) stream the
  ** same time; such a repeat is counted on from the samples decoded */
  double a_raw;                        /* the decoder's last time, -1 = none yet */
  double a_next;                       /* where the sound heard so far ends */
  ssize_t held;                        /* video output buffer handed out, -1 none */
  /* video output layout */
  int color, bw, bh, stride, slice, cl, ct, cr, cb, layout;
  u8 *uv;                              /* split chroma (semi-planar) */
  size_t uv_cap;
  /* audio output */
  int a_rate, a_ch, a_enc;
  float *pcm, *rs;
  int pcm_cap, rs_cap;
  double rs_pos;
  float rs_last[2];
  bool rs_have;
} Amc;

static void amc_close(void *p);

/* ---- network data source ---------------------------------------------------- */

/* The extractor reads in 2 KB pieces and keeps stepping back a few hundred
** bytes (MP4 box parsing; measured on the YouTube DASH index: 352 reads,
** most a seek), and while it opens it swings between the start and points
** far ahead (measured on Dailymotion HLS: 1527 <-> 958186, a dozen times).
** Each miss was a new HTTP request on FmNetStream (with HLS a step back
** re-fetches a segment): a 720p open took 11-34 s on a phone. So reads go
** through NET_WINS windows of bytes already read (the least recently used
** one is reused): steps back and swings cost nothing, and the stream is
** read ahead in NET_STEP pieces. 1 MB per stream.
** While playing, MP4 fragments are read at two places at once (the data and
** a cursor far behind it; measured on YouTube DASH: 146 KB <-> 872 KB, both
** moving on). With one connection each swing was two reconnects of ~0.4 s
** and the picture stalled every few seconds ("Buffering"). So each window
** keeps its own connection, reading on from where that window ends; the
** extra ones keep a small read-ahead (NET_SIDE_RING). */
#define NET_WIN (256u * 1024u)
#define NET_STEP (64u * 1024u)
#define NET_NEAR (256 * 1024)          /* a gap this small is read through, not sought */
#define NET_SIDE_RING (512u * 1024u)
#define NET_BACK (64 * 1024)            /* a new window starts this far before the asked byte */
#define NET_RUN (1024 * 1024)           /* read straight on this far: the data cursor, read ahead fully */

/* MMCFM_AMC_TRACE=1 logs every extractor read (device debugging) */
static bool amc_trace(void) {
  static int t = -1;
  if (t < 0) t = getenv("MMCFM_AMC_TRACE") != NULL;
  return t > 0;
}

static ssize_t net_read_at(void *u, off64_t off, void *buf, size_t size) {
  AmcNet *n = (AmcNet *)u;
  if (off < 0) return -1;
  u64 t_in = plat_now_ms();
  pthread_mutex_lock(&n->mx);
  i64 sz = ns_size(n->ns);
  if (sz >= 0 && off >= sz) {
    pthread_mutex_unlock(&n->mx);
    return 0;                          /* end of stream */
  }
  size_t done = 0;
  bool fail = false;
  while (done < size) {
    i64 p = (i64)off + (i64)done;
    int hit = -1;
    for (int i = 0; i < NET_WINS && hit < 0; i++)
      if (n->w[i].len && p >= n->w[i].pos && p < n->w[i].pos + (i64)n->w[i].len) hit = i;
    if (hit >= 0) {                                           /* already read */
      i64 end = n->w[hit].pos + (i64)n->w[hit].len;
      size_t take = FM_MIN(size - done, (size_t)(end - p));
      memcpy((u8 *)buf + done, n->w[hit].buf + (p - n->w[hit].pos), take);
      n->w[hit].used = ++n->tick;
      done += take;
      continue;
    }
    /* a peek far ahead (the next fragment's header) that a connection has
    ** already read ahead: served from its buffer, the cursor stays put */
    size_t pk = 0;
    for (int i = 0; i < NET_WINS && !pk; i++)
      if (n->w[i].ns) pk = ns_peek_at(n->w[i].ns, p, (u8 *)buf + done, size - done);
    if (pk) { done += pk; continue; }
    /* a window whose connection reads on to p */
    int k = -1;
    i64 lend = 0;
    for (int i = 0; i < NET_WINS && k < 0; i++) {
      i64 e = n->w[i].pos + (i64)n->w[i].len;
      if (n->w[i].ns && n->w[i].len && p >= e && p - e <= NET_NEAR) { k = i; lend = e; }
    }
    if (k < 0) {                                              /* elsewhere: reuse the oldest window */
      k = 0;
      for (int i = 1; i < NET_WINS; i++)
        if (n->w[i].used < n->w[k].used) k = i;
      if (!n->w[k].ns) {
        char why[160];
        n->w[k].ns = ns_open(n->url, NULL, why, sizeof why);
        if (!n->w[k].ns) { fm_log("video: %s", why); vid_note_net_error(why); fail = true; break; }
        ns_limit_ring(n->w[k].ns, NET_SIDE_RING);
      }
      /* the extractor steps back a little right after a jump: start early */
      lend = p > NET_BACK ? p - NET_BACK : 0;
      n->w[k].pos = lend;
      n->w[k].len = 0;
      n->w[k].run = 0;
    }
    FmNetStream *ns = n->w[k].ns;
    if (amc_trace() && ns_tell(ns) != lend) {
      char wl[256];
      size_t o = 0;
      for (int i = 0; i < NET_WINS; i++)
        if (n->w[i].ns)
          o += (size_t)fm_snprintf(wl + o, sizeof wl - o, " [%d %lld+%zu ahead %zu%s]", i, (long long)n->w[i].pos,
                                   n->w[i].len, ns_buffered(n->w[i].ns), n->w[i].wide ? " wide" : "");
      fm_log("amc miss %lld: window %d moves %lld -> %lld;%s", (long long)p, k, (long long)ns_tell(ns), (long long)lend, wl);
    }
    if (ns_tell(ns) != lend && !ns_seek(ns, lend)) { n->w[k].len = 0; fail = true; break; }
    if (n->w[k].len + NET_STEP > NET_WIN) {                   /* slide: drop its oldest bytes */
      size_t drop = n->w[k].len + NET_STEP - NET_WIN;
      memmove(n->w[k].buf, n->w[k].buf + drop, n->w[k].len - drop);
      n->w[k].len -= drop;
      n->w[k].pos += (i64)drop;
    }
    size_t got = ns_read(ns, n->w[k].buf + n->w[k].len, NET_STEP);
    n->w[k].used = ++n->tick;
    if (!got) break;                                          /* end of stream (or aborted) */
    n->w[k].len += got;
    n->w[k].run += (i64)got;
    if (!n->w[k].wide && n->w[k].run >= NET_RUN) {            /* this one carries the data now */
      n->w[k].wide = true;
      ns_limit_ring(ns, 4u * 1024u * 1024u);
    }
  }
  pthread_mutex_unlock(&n->mx);
  ssize_t r = fail && !done ? -1 : (ssize_t)done;
  if (amc_trace())
    fm_log("amc read %lld +%zu -> %zd at %llu ms took %llu", (long long)off, size, r, (unsigned long long)plat_now_ms(),
           (unsigned long long)(plat_now_ms() - t_in));
  return r;
}

static ssize_t net_get_size(void *u) {
  i64 sz = ns_size(((AmcNet *)u)->ns);
  return sz >= 0 ? (ssize_t)sz : -1;
}

static void net_ds_close(void *u) { FM_UNUSED(u); }   /* amc_close frees it */

/* From any thread: blocked reads on every connection return now. */
static void net_abort(AmcNet *n) {
  for (int i = 0; i < NET_WINS; i++)
    if (n->w[i].ns) ns_abort(n->w[i].ns);
}

static void net_free(AmcNet *n) {
  if (!n) return;
  for (int i = 0; i < NET_WINS; i++) {
    if (n->w[i].ns) ns_close(n->w[i].ns);
    fm_free(n->w[i].buf);
  }
  pthread_mutex_destroy(&n->mx);
  fm_free(n->url);
  fm_free(n);
}

/* A new extractor reading url through FmNetStream; *net and *src get what
** it reads from (freed by the caller after the extractor). NULL on failure. */
static AMediaExtractor *net_extractor(const char *url, AmcNet **net, AMediaDataSource **src) {
  *net = NULL;
  *src = NULL;
  char why[160];
  FmNetStream *ns = ns_open(url, NULL, why, sizeof why);
  if (!ns) { fm_log("video: %s", why); vid_note_net_error(why); return NULL; }
  AmcNet *n = (AmcNet *)fm_calloc(1, sizeof *n);
  n->ns = ns;
  for (int i = 0; i < NET_WINS; i++) n->w[i].buf = (u8 *)fm_alloc(NET_WIN);
  n->w[0].ns = ns;
  n->url = fm_strdup(url);
  pthread_mutex_init(&n->mx, NULL);
  AMediaExtractor *ex = nd.AMediaExtractor_new();
  AMediaDataSource *ds_ = ex ? ds.AMediaDataSource_new() : NULL;
  media_status_t ms = AMEDIA_ERROR_UNKNOWN;
  if (ds_) {
    ds.AMediaDataSource_setUserdata(ds_, n);
    ds.AMediaDataSource_setReadAt(ds_, net_read_at);
    ds.AMediaDataSource_setGetSize(ds_, net_get_size);
    ds.AMediaDataSource_setClose(ds_, net_ds_close);
    ms = ds.AMediaExtractor_setDataSourceCustom(ex, ds_);
  }
  if (ms != AMEDIA_OK) {
    fm_log("video: MediaCodec could not read the stream (%d)", (int)ms);
    if (ex) nd.AMediaExtractor_delete(ex);
    if (ds_) ds.AMediaDataSource_delete(ds_);
    net_free(n);
    return NULL;
  }
  *net = n;
  *src = ds_;
  return ex;
}

bool amc_can_stream(void) {
  pthread_once(&g_nd_once, amc_load);
  return g_nd_ok && g_ds_ok;
}

/* ---- helpers -------------------------------------------------------------- */

static int fmt_i32(AMediaFormat *f, const char *k, int def) {
  int32_t v;
  return nd.AMediaFormat_getInt32(f, k, &v) ? (int)v : def;
}

static void codec_name(const char *mime, char *out, size_t cap) {
  static const char *const kMap[][2] = {
    { "video/avc", "h264" }, { "video/hevc", "hevc" }, { "video/x-vnd.on2.vp8", "vp8" },
    { "video/x-vnd.on2.vp9", "vp9" }, { "video/av01", "av01" }, { "video/mp4v-es", "mpeg4" },
    { "video/3gpp", "h263" }, { "video/mpeg2", "mpeg2" }, { "audio/mp4a-latm", "aac" },
    { "audio/opus", "opus" }, { "audio/vorbis", "vorbis" }, { "audio/mpeg", "mp3" },
    { "audio/flac", "flac" }, { "audio/3gpp", "amrnb" }, { "audio/amr-wb", "amrwb" },
    { "audio/raw", "pcm" }, { "audio/g711-alaw", "alaw" }, { "audio/g711-mlaw", "mulaw" },
    { "audio/ac3", "ac3" }, { "audio/eac3", "eac3" },
  };
  for (int i = 0; i < FM_COUNT(kMap); i++)
    if (!strcmp(mime, kMap[i][0])) { fm_strlcpy(out, kMap[i][1], cap); return; }
  const char *s = strchr(mime, '/');
  fm_strlcpy(out, s ? s + 1 : mime, cap);
}

/* Creates and starts a decoder for track i; false leaves t empty. */
static bool track_open(Amc *s, AmcTrack *t, size_t i, AMediaFormat *f, const char *mime, bool video) {
  /* MMCFM_AMC_CODEC=name: a test picks the picture decoder (the emulator's
  ** c2.goldfish one vs Android's own c2.android.avc.decoder) */
  const char *force = video ? getenv("MMCFM_AMC_CODEC") : NULL;
  AMediaCodec *c = force && force[0] ? nd.AMediaCodec_createCodecByName(force) : nd.AMediaCodec_createDecoderByType(mime);
  if (!c) return false;
  if (video) nd.AMediaFormat_setInt32(f, "color-format", COLOR_FLEX);
  if (nd.AMediaCodec_configure(c, f, NULL, NULL, 0) != AMEDIA_OK || nd.AMediaCodec_start(c) != AMEDIA_OK) {
    nd.AMediaCodec_delete(c);
    return false;
  }
  if (nd.AMediaExtractor_selectTrack(s->ex, i) != AMEDIA_OK) {
    nd.AMediaCodec_stop(c);
    nd.AMediaCodec_delete(c);
    return false;
  }
  t->codec = c;
  t->fmt = f;
  t->idx = (int)i;
  t->csd_next = -1;
  return true;
}

static void track_close(AmcTrack *t) {
  if (t->codec) { nd.AMediaCodec_stop(t->codec); nd.AMediaCodec_delete(t->codec); }
  if (t->fmt) nd.AMediaFormat_delete(t->fmt);
  t->codec = NULL;
  t->fmt = NULL;
}

/* A codec that fails mid-stream: video ends the file, audio just goes quiet. */
static void track_kill(AmcTrack *t) {
  t->in_eos = t->out_eos = t->dead = true;
}

static void release_held(Amc *s) {
  if (s->held >= 0 && s->v.codec) nd.AMediaCodec_releaseOutputBuffer(s->v.codec, (size_t)s->held, false);
  s->held = -1;
}

/* ---- open ----------------------------------------------------------------- */

static void *amc_open(const char *path, int flags, FmVidInfo *in) {
  pthread_once(&g_nd_once, amc_load);
  if (!g_nd_ok) return NULL;
  bool url = !fm_strnicmp(path, "http://", 7) || !fm_strnicmp(path, "https://", 8);
  if (url && !g_ds_ok) return NULL;
  int fd = -1;
  struct stat st;
  if (!url) {
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
  }
  Amc *s = (Amc *)fm_calloc(1, sizeof *s);
  s->fd = fd;
  s->held = -1;
  s->a_raw = -1;
  s->v.idx = s->a.idx = -1;
  s->info = in;
  s->ex = nd.AMediaExtractor_new();
  media_status_t ms = AMEDIA_ERROR_UNKNOWN;
  if (s->ex && url) {
    nd.AMediaExtractor_delete(s->ex);
    s->ex = net_extractor(path, &s->net, &s->src);
    if (s->ex) {
      ms = AMEDIA_OK;
      s->url = fm_strdup(path);
      s->hls = ns_is_hls(s->net->ns);
      s->hls_start = ns_hls_start(s->net->ns);
    }
  } else if (s->ex) {
    ms = nd.AMediaExtractor_setDataSourceFd(s->ex, fd, 0, (off64_t)st.st_size);
  }
  if (ms != AMEDIA_OK) {
    amc_close(s);
    return NULL;
  }
  size_t n = nd.AMediaExtractor_getTrackCount(s->ex);
  i64 dur = 0;
  double fps = 0;
  int sar_w = 0, sar_h = 0, ach = 0;
  for (size_t i = 0; i < n && i < 64; i++) {
    AMediaFormat *f = nd.AMediaExtractor_getTrackFormat(s->ex, i);
    if (!f) continue;
    const char *mime = NULL;
    bool keep = false;
    if (nd.AMediaFormat_getString(f, "mime", &mime) && mime) {
      bool video = !strncmp(mime, "video/", 6), audio = !strncmp(mime, "audio/", 6);
      int w = fmt_i32(f, "width", 0), h = fmt_i32(f, "height", 0);
      int rate = fmt_i32(f, "sample-rate", 0), ch = fmt_i32(f, "channel-count", 0);
      if (video && !s->v.codec && !(flags & VID_OPEN_AUDIO_ONLY) && w > 0 && h > 0 && w <= 16384 &&
          h <= 16384 && track_open(s, &s->v, i, f, mime, true)) {
        keep = true;
        in->w = w;
        in->h = h;
        codec_name(mime, in->vcodec, sizeof in->vcodec);
        float fr;
        fps = fmt_i32(f, "frame-rate", 0);
        if (fps <= 0 && nd.AMediaFormat_getFloat(f, "frame-rate", &fr) && fr > 0) fps = fr;
        sar_w = fmt_i32(f, "sar-width", 0);
        sar_h = fmt_i32(f, "sar-height", 0);
      } else if (audio && !s->a.codec && !(flags & VID_OPEN_NO_AUDIO) && rate > 0 && rate <= 768000 &&
                 ch > 0 && ch <= 64 && track_open(s, &s->a, i, f, mime, false)) {
        keep = true;
        in->rate = rate;
        ach = ch;
        codec_name(mime, in->acodec, sizeof in->acodec);
      }
      if (keep) {
        int64_t d;
        if (nd.AMediaFormat_getInt64(f, "durationUs", &d) && d > dur) dur = d;
      }
    }
    if (!keep) nd.AMediaFormat_delete(f);
  }
  if (!s->v.codec && !s->a.codec) { amc_close(s); return NULL; }
  in->has_video = s->v.codec != NULL;
  in->has_audio = s->a.codec != NULL;
  in->channels = ach >= 2 ? 2 : 1;
  if (!in->has_audio) in->rate = in->channels = 0;
  in->fps = fps > 0 && fps < 1000 ? fps : 0;
  in->sar = sar_w > 0 && sar_h > 0 ? (double)sar_w / sar_h : 1.0;
  if (in->sar < 0.1 || in->sar > 10) in->sar = 1.0;
  in->duration = dur > 0 ? (double)dur / 1e6 : 0;
  if (s->hls && in->duration <= 0) in->duration = ns_hls_duration(s->net->ns);
  fm_strlcpy(in->backend, "MediaCodec", sizeof in->backend);
  s->a_rate = in->rate;
  s->a_ch = ach;
  s->a_enc = 2;
  return s;
}

static void amc_close(void *p) {
  Amc *s = (Amc *)p;
  if (!s) return;
  release_held(s);
  track_close(&s->v);
  track_close(&s->a);
  if (s->net) net_abort(s->net);       /* a read blocked between segments returns now */
  if (s->ex) nd.AMediaExtractor_delete(s->ex);
  if (s->src) ds.AMediaDataSource_delete(s->src);
  net_free(s->net);                    /* after the extractor: it reads until deleted */
  fm_free(s->url);
  if (s->fd >= 0) close(s->fd);
  fm_free(s->uv);
  fm_free(s->pcm);
  fm_free(s->rs);
  fm_free(s);
}

/* ---- feeding -------------------------------------------------------------- */

/* Re-queues csd-N after an early flush; false while waiting for a buffer. */
static bool feed_csd(AmcTrack *t) {
  while (t->csd_next >= 0 && t->csd_next < 3) {
    char key[8] = "csd-0";
    key[4] = (char)('0' + t->csd_next);
    void *data = NULL;
    size_t size = 0;
    if (!nd.AMediaFormat_getBuffer(t->fmt, key, &data, &size) || !data || !size) break;
    ssize_t bi = nd.AMediaCodec_dequeueInputBuffer(t->codec, 0);
    if (bi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return false;
    if (bi < 0) { track_kill(t); break; }
    size_t cap = 0;
    u8 *buf = nd.AMediaCodec_getInputBuffer(t->codec, (size_t)bi, &cap);
    size_t m = buf ? FM_MIN(size, cap) : 0;
    if (m) memcpy(buf, data, m);
    nd.AMediaCodec_queueInputBuffer(t->codec, (size_t)bi, 0, m, 0, AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
    t->csd_next++;
  }
  t->csd_next = -1;
  return true;
}

static bool feed_eos(AmcTrack *t) {
  if (!t->codec || t->in_eos) return false;
  ssize_t bi = nd.AMediaCodec_dequeueInputBuffer(t->codec, 0);
  if (bi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return false;
  if (bi < 0 || nd.AMediaCodec_queueInputBuffer(t->codec, (size_t)bi, 0, 0, 0,
                                                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK)
    track_kill(t);
  t->in_eos = true;
  return true;
}

/* Moves one step: one sample, one csd or one end-of-stream. False when all
** codecs are full and the caller should wait for output instead. */
static bool feed(Amc *s) {
  AmcTrack *ts[2] = { &s->v, &s->a };
  for (int i = 0; i < 2; i++) {
    if (ts[i]->codec && ts[i]->csd_next >= 0 && !ts[i]->in_eos && !feed_csd(ts[i])) return false;
  }
  if (s->ex_eos) {
    bool v = feed_eos(&s->v), a = feed_eos(&s->a);
    return v || a;
  }
  int ti = nd.AMediaExtractor_getSampleTrackIndex(s->ex);
  if (ti < 0) { s->ex_eos = true; return true; }
  AmcTrack *t = ti == s->v.idx && s->v.codec ? &s->v : ti == s->a.idx && s->a.codec ? &s->a : NULL;
  if (t && !t->in_eos) {
    ssize_t bi = nd.AMediaCodec_dequeueInputBuffer(t->codec, 0);
    if (bi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return false;
    if (bi < 0) {
      fm_log("video: MediaCodec input error %d", (int)bi);
      track_kill(t);
    } else {
      size_t cap = 0;
      u8 *buf = nd.AMediaCodec_getInputBuffer(t->codec, (size_t)bi, &cap);
      ssize_t n = buf && cap ? nd.AMediaExtractor_readSampleData(s->ex, buf, cap) : -1;
      int64_t us = nd.AMediaExtractor_getSampleTime(s->ex);
      if (getenv("MMCFM_AMC_TRACE") && t->queued < 8)
        fm_log("amc in: %s sample %d at %lld us, %d bytes, offset %lld us", t == &s->v ? "video" : "audio",
               (int)t->queued, (long long)us, (int)n, (long long)s->t_off_us);
      if (s->hls && !s->t_checked && us >= 0) {
        s->t_checked = true;
        if (s->hls_start > 5 && (double)us / 1e6 < s->hls_start - 5) s->t_off_us = (int64_t)(s->hls_start * 1e6);
        /* live: Android counts both the TS picture and the packed audio
        ** beside it from 0, each from its own start; back on the broadcast's
        ** clock (YouTube live: hours) they are in step again */
        double at = ns_hls_live_time(s->net->ns);
        if (at > 5 && (double)us / 1e6 < at - 5) s->t_off_us = (int64_t)(at * 1e6) - us;
      }
      if (us >= 0) us += s->t_off_us;
      /* an oversized or unreadable sample is skipped, its buffer goes back empty */
      if (nd.AMediaCodec_queueInputBuffer(t->codec, (size_t)bi, 0, n > 0 ? (size_t)n : 0,
                                          us > 0 ? (uint64_t)us : 0, 0) != AMEDIA_OK)
        track_kill(t);
      else
        t->queued++;
    }
  }
  if (!nd.AMediaExtractor_advance(s->ex)) s->ex_eos = true;
  return true;
}

/* ---- video output --------------------------------------------------------- */

static void video_format(Amc *s) {
  AMediaFormat *f = nd.AMediaCodec_getOutputFormat(s->v.codec);
  if (!f) return;
  s->color = fmt_i32(f, "color-format", 0);
  s->bw = fmt_i32(f, "width", s->info->w);
  s->bh = fmt_i32(f, "height", s->info->h);
  s->stride = fmt_i32(f, "stride", 0);
  s->slice = fmt_i32(f, "slice-height", 0);
  s->cl = fmt_i32(f, "crop-left", 0);
  s->ct = fmt_i32(f, "crop-top", 0);
  s->cr = fmt_i32(f, "crop-right", -1);
  s->cb = fmt_i32(f, "crop-bottom", -1);
  nd.AMediaFormat_delete(f);
  if (s->stride < s->bw) s->stride = s->bw;            /* some devices report 0 */
  if (s->slice < s->bh) s->slice = s->bh;
  if (s->cr < s->cl || s->cr >= s->bw) { s->cl = 0; s->cr = s->bw - 1; }
  if (s->cb < s->ct || s->cb >= s->bh) { s->ct = 0; s->cb = s->bh - 1; }
  s->cl &= ~1;                                         /* chroma sites stay aligned */
  s->ct &= ~1;
  s->layout = s->color == COLOR_PLANAR ? LAY_PLANAR : s->color == COLOR_SEMI ? LAY_SEMI : LAY_UNKNOWN;
  s->v.got_fmt = true;
}

/* NV12 or I420 for YUV420Flexible, from one row of chroma bytes. */
static int probe_layout(const u8 *uv, size_t len) {
  u32 d1 = 0, d2 = 0;
  if (len > 4096) len = 4096;
  for (size_t i = 0; i + 2 < len; i++) {
    d1 += (u32)abs((int)uv[i] - uv[i + 1]);
    d2 += (u32)abs((int)uv[i] - uv[i + 2]);
  }
  if (d1 + d2 < (u32)len * 2) return LAY_UNKNOWN;     /* flat chroma: either layout looks alike */
  return d1 > d2 ? LAY_SEMI : LAY_PLANAR;
}

/* Fills vf from output buffer `buf` of `size` bytes: 1 done, 0 drop it, -1 unusable. */
static int video_emit(Amc *s, const u8 *buf, size_t size, FmVidFrame *vf) {
  if (s->color != COLOR_PLANAR && s->color != COLOR_SEMI && s->color != COLOR_FLEX) {
    fm_log("video: MediaCodec color format 0x%x not supported", (unsigned)s->color);
    return -1;
  }
  /* A first format can claim a padded slice height the buffer does not have
  ** (seen with dav1d), and such a frame's chroma order is not to be trusted
  ** either: drop a few, then fall back to the plain size. */
  if ((size_t)s->stride * s->slice * 3 / 2 > size + (size_t)s->stride) {
    if (s->early++ < 8) return 0;
    if (s->slice > s->bh) s->slice = s->bh;
    if ((size_t)s->stride * s->slice * 3 / 2 > size + (size_t)s->stride && s->stride > s->bw) s->stride = s->bw;
  }
  int dw = s->cr - s->cl + 1, dh = s->cb - s->ct + 1;
  int cw = (dw + 1) / 2, ch = (dh + 1) / 2;
  size_t st = (size_t)s->stride, cs = (st + 1) / 2;
  size_t ysz = st * (size_t)s->slice;
  size_t need_y = ((size_t)s->ct + dh - 1) * st + s->cl + dw;
  size_t need_semi = ysz + ((size_t)s->ct / 2 + ch - 1) * st + ((size_t)s->cl / 2 + cw) * 2;
  size_t voff = ysz + cs * (((size_t)s->slice + 1) / 2);
  size_t need_pl = voff + ((size_t)s->ct / 2 + ch - 1) * cs + s->cl / 2 + cw;
  if (dw <= 0 || dh <= 0 || need_y > size) return -1;
  int lay = s->layout;
  if (lay == LAY_UNKNOWN) {
    if (need_semi <= size) {
      int p = probe_layout(buf + ysz + (size_t)(ch / 2) * st, (size_t)cw * 2);
      if (p != LAY_UNKNOWN) s->layout = p;
      lay = p == LAY_PLANAR && need_pl <= size ? LAY_PLANAR : LAY_SEMI;
    } else {
      lay = LAY_PLANAR;
    }
  }
  vf->plane[0] = buf + (size_t)s->ct * st + s->cl;
  vf->stride[0] = (int)st;
  if (lay == LAY_SEMI) {
    if (need_semi > size) return -1;
    size_t need = (size_t)cw * ch * 2;
    if (need > s->uv_cap) {
      fm_free(s->uv);
      s->uv = (u8 *)fm_alloc(need);
      s->uv_cap = need;
    }
    vid_nv12_split(buf + ysz + (size_t)(s->ct / 2) * st + (size_t)(s->cl / 2) * 2, (int)st, cw, ch, s->uv, cw,
                   s->uv + (size_t)cw * ch, cw);
    vf->plane[1] = s->uv;
    vf->plane[2] = s->uv + (size_t)cw * ch;
    vf->stride[1] = vf->stride[2] = cw;
  } else {
    if (need_pl > size) return -1;
    size_t co = (size_t)(s->ct / 2) * cs + s->cl / 2;
    vf->plane[1] = buf + ysz + co;
    vf->plane[2] = buf + voff + co;
    vf->stride[1] = vf->stride[2] = (int)cs;
  }
  vf->w = dw;
  vf->h = dh;
  s->info->w = dw;
  s->info->h = dh;
  return 1;
}

/* ---- audio output --------------------------------------------------------- */

static void audio_format(Amc *s) {
  AMediaFormat *f = nd.AMediaCodec_getOutputFormat(s->a.codec);
  if (!f) return;
  int r = fmt_i32(f, "sample-rate", s->a_rate), c = fmt_i32(f, "channel-count", s->a_ch);
  int e = fmt_i32(f, "pcm-encoding", 2);
  nd.AMediaFormat_delete(f);
  if (r > 0 && r <= 768000) s->a_rate = r;
  if (c > 0 && c <= 64) s->a_ch = c;
  s->a_enc = e == 3 || e == 4 || e == 22 ? e : 2;    /* 8-bit, float, 32-bit; else PCM16 */
  s->rs_have = false;
  s->a.got_fmt = true;
}

static float smp(const u8 *p, int enc, size_t i) {
  switch (enc) {
    case 3: return (p[i] - 128) / 128.0f;
    case 4: { float f; memcpy(&f, p + i * 4, 4); return f; }
    case 22: { i32 x; memcpy(&x, p + i * 4, 4); return (float)(x / 2147483648.0); }
    default: { i16 x; memcpy(&x, p + i * 2, 2); return x / 32768.0f; }
  }
}

static float *grow(float **b, int *cap, int need) {
  if (need > *cap) {
    fm_free(*b);
    *cap = need;
    *b = (float *)fm_alloc((size_t)need * sizeof(float));
  }
  return *b;
}

/* Linear resampler; position and last frame carry across blocks. */
static int resample(Amc *s, const float *in, int n, int oc, int src, int dst) {
  double step = (double)src / dst;
  int cap = (int)(n / step) + 4;
  float *out = grow(&s->rs, &s->rs_cap, cap * oc);
  if (!s->rs_have) {
    for (int c = 0; c < oc; c++) s->rs_last[c] = in[c];
    s->rs_pos = 0;
    s->rs_have = true;
  }
  double p = s->rs_pos;                /* -1 is rs_last, k is in[k] */
  int m = 0;
  while (p < n - 1 && m < cap) {
    int i = (int)(p + 1.0) - 1;
    float f = (float)(p - i);
    for (int c = 0; c < oc; c++) {
      float a = i < 0 ? s->rs_last[c] : in[i * oc + c], b = in[(i + 1) * oc + c];
      out[m * oc + c] = a + (b - a) * f;
    }
    m++;
    p += step;
  }
  s->rs_pos = p - n;
  for (int c = 0; c < oc; c++) s->rs_last[c] = in[(n - 1) * oc + c];
  return m;
}

static bool audio_emit(Amc *s, const u8 *buf, size_t size, i64 us, FmVidPcm *pc) {
  int sc = s->a_ch, enc = s->a_enc;
  int bps = enc == 3 ? 1 : enc == 2 ? 2 : 4;
  int n = (int)(size / ((size_t)bps * sc));
  if (n <= 0 || s->a_rate <= 0) return false;
  double t = us >= 0 ? us / 1e6 : -1;
  static int trace;
  if (getenv("MMCFM_AMC_TRACE") && trace++ < 12)
    fm_log("amc out: audio at %lld us, %d frames, raw %.6f next %.6f", (long long)us, n, s->a_raw, s->a_next);
  if (t >= 0 && t == s->a_raw) t = s->a_next;          /* a repeat: the time did not move */
  else s->a_raw = t;
  if (t >= 0) s->a_next = t + (double)n / s->a_rate;
  if (t >= 0 && t + (double)n / s->a_rate < s->skip_until) return false;
  int oc = s->info->channels;
  float *out = grow(&s->pcm, &s->pcm_cap, n * oc);
  for (int i = 0; i < n; i++) {
    float l = 0, r = 0;
    int nl = 0, nr = 0;
    for (int c = 0; c < sc; c++) {
      float x = smp(buf, enc, (size_t)i * sc + c);
      if (sc == 1) { l = r = x; nl = nr = 1; break; }
      if (sc >= 6 && c == 3) continue;                     /* LFE */
      if (sc >= 3 && c == 2) { l += x * 0.7071f; r += x * 0.7071f; continue; }
      if (c & 1) { r += x; nr++; } else { l += x; nl++; }
    }
    float k = 1.0f / (float)FM_MAX(1, FM_MAX(nl, nr));
    l = FM_CLAMP(l * k, -1.0f, 1.0f);
    r = FM_CLAMP(r * k, -1.0f, 1.0f);
    if (oc == 1) out[i] = (l + r) * 0.5f;
    else { out[i * 2] = l; out[i * 2 + 1] = r; }
  }
  const float *res = out;
  if (s->a_rate != s->info->rate) {
    n = resample(s, out, n, oc, s->a_rate, s->info->rate);
    res = s->rs;
    if (n <= 0) return false;
  }
  pc->t = t;
  pc->frames = n;
  pc->channels = oc;
  pc->rate = s->info->rate;
  pc->pcm = res;
  return true;
}

/* ---- the pull loop -------------------------------------------------------- */

/* One output step of track t: VID_EV_VIDEO/AUDIO when emitted, VID_EV_ERROR,
** or 0 when nothing came out (s->progress tells whether anything moved). */
static int drain(Amc *s, AmcTrack *t, int64_t wait_us, FmVidFrame *vf, FmVidPcm *pc) {
  bool video = t == &s->v;
  for (int k = 0; k < 8; k++) {
    AMediaCodecBufferInfo bi;
    ssize_t ix = nd.AMediaCodec_dequeueOutputBuffer(t->codec, &bi, k ? 0 : wait_us);
    if (ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
    s->progress = true;
    if (ix == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      if (video) { video_format(s); s->early = 0; } else audio_format(s);
      continue;
    }
    if (ix == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
    if (ix < 0) {
      fm_log("video: MediaCodec %s output error %d", video ? "video" : "audio", (int)ix);
      if (video) return VID_EV_ERROR;
      track_kill(t);
      return 0;
    }
    t->got_out = true;
    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) t->out_eos = true;
    size_t cap = 0;
    u8 *buf = nd.AMediaCodec_getOutputBuffer(t->codec, (size_t)ix, &cap);
    bool data = buf && bi.size > 0 && bi.offset >= 0 && (size_t)bi.offset + (size_t)bi.size <= cap &&
                !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
    if (data && video) {
      if (!t->got_fmt) video_format(s);
      double vt = bi.presentationTimeUs / 1e6;
      int ok = vt >= s->skip_until - 0.001 ? video_emit(s, buf + bi.offset, (size_t)bi.size, vf) : 0;
      if (ok) {
        if (ok < 0) {
          fm_log("video: MediaCodec frame %dx%d stride %d slice %d color 0x%x in %d bytes", s->bw, s->bh,
                 s->stride, s->slice, (unsigned)s->color, (int)bi.size);
          nd.AMediaCodec_releaseOutputBuffer(t->codec, (size_t)ix, false);
          return VID_EV_ERROR;
        }
        s->skip_until = 0;
        vf->t = vt;
        s->held = ix;                  /* planes point into it until the next call */
        return VID_EV_VIDEO;
      }
    }
    int ev = 0;
    if (data && !video) {
      if (!t->got_fmt) audio_format(s);
      if (audio_emit(s, buf + bi.offset, (size_t)bi.size, bi.presentationTimeUs, pc)) ev = VID_EV_AUDIO;
    }
    nd.AMediaCodec_releaseOutputBuffer(t->codec, (size_t)ix, false);
    if (ev) return ev;
    if (t->out_eos) return 0;
  }
  return 0;
}

static bool live(const AmcTrack *t) { return t->codec && !t->out_eos; }

static int amc_decode(void *p, FmVidFrame *vf, FmVidPcm *pc) {
  Amc *s = (Amc *)p;
  release_held(s);
  int idle = 0, r;
  for (int guard = 0; guard < AMC_GUARD; guard++) {
    if (s->v.dead) return VID_EV_ERROR;
    if (live(&s->v) && (r = drain(s, &s->v, 0, vf, pc)) != 0) return r;
    if (live(&s->a) && (r = drain(s, &s->a, 0, vf, pc)) != 0) return r;
    if (!live(&s->v) && !live(&s->a)) return VID_EV_END;
    if (feed(s)) { idle = 0; continue; }
    /* every codec is full: wait a little for output */
    s->progress = false;
    if ((r = drain(s, live(&s->v) ? &s->v : &s->a, AMC_WAIT_US, vf, pc)) != 0) return r;
    if (s->progress) idle = 0;
    else if (++idle > AMC_IDLE_MAX) {
      fm_log("video: MediaCodec stalled");
      return s->ex_eos ? VID_EV_END : VID_EV_ERROR;
    }
  }
  return VID_EV_ERROR;
}

static void track_flush(AmcTrack *t) {
  if (!t->codec || t->dead) return;
  if (t->queued > 0 || t->got_out) {
    nd.AMediaCodec_flush(t->codec);
    /* flushed before the first output: the codec forgot its csd */
    if (!t->got_out) t->csd_next = 0;
  }
  t->queued = 0;
  t->in_eos = t->out_eos = false;
}

/* HLS: a new extractor that starts at t's segment, same tracks, same codecs. */
static bool hls_reopen(Amc *s, double t) {
  size_t n0 = strcspn(s->url, "#");
  char *at = (char *)fm_alloc(n0 + 32);
  memcpy(at, s->url, n0);
  fm_snprintf(at + n0, 32, "#t=%.3f", t > 0 ? t : 0);
  AmcNet *net;
  AMediaDataSource *src;
  AMediaExtractor *ex = net_extractor(at, &net, &src);
  fm_free(at);
  if (!ex) return false;
  if ((s->v.idx >= 0 && nd.AMediaExtractor_selectTrack(ex, (size_t)s->v.idx) != AMEDIA_OK) ||
      (s->a.idx >= 0 && nd.AMediaExtractor_selectTrack(ex, (size_t)s->a.idx) != AMEDIA_OK)) {
    nd.AMediaExtractor_delete(ex);
    ds.AMediaDataSource_delete(src);
    net_free(net);
    return false;
  }
  nd.AMediaExtractor_delete(s->ex);
  ds.AMediaDataSource_delete(s->src);
  net_free(s->net);
  s->ex = ex;
  s->src = src;
  s->net = net;
  s->hls_start = ns_hls_start(net->ns);
  s->t_off_us = 0;
  s->t_checked = false;
  return true;
}

static bool amc_seek(void *p, double t) {
  Amc *s = (Amc *)p;
  release_held(s);
  if (s->hls && s->url) {              /* never byte-seek across segments not read yet */
    bool ok = hls_reopen(s, t);
    track_flush(&s->v);
    track_flush(&s->a);
    s->ex_eos = false;
    s->skip_until = ok ? t : 0;
    s->rs_have = false;
    s->a_raw = -1;
    return ok;
  }
  int64_t us = (int64_t)(t * 1e6);
  bool ok = nd.AMediaExtractor_seekTo(s->ex, us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) == AMEDIA_OK ||
            nd.AMediaExtractor_seekTo(s->ex, us, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) == AMEDIA_OK;
  /* no index (some TS): restart, and decode forward to t only when that is short */
  if (!ok) nd.AMediaExtractor_seekTo(s->ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
  track_flush(&s->v);
  track_flush(&s->a);
  s->ex_eos = false;
  s->skip_until = ok || t <= 30 ? t : 0;
  s->rs_have = false;
  s->a_raw = -1;
  return ok;
}

const FmVidBackend g_vid_amc = { "MediaCodec", amc_open, amc_decode, amc_seek, amc_close };

#endif /* FM_ANDROID */
