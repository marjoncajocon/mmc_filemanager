/* fdec_vid.c -- MPEG-1 through pl_mpeg, everything else through FFmpeg loaded
** at run time.
**
** Design decisions:
**   - FFmpeg headers are not vendored, so the few structs we touch are
**     declared here as prefixes, one variant per library major, copied from
**     the release headers of FFmpeg 4.0-4.4 (avformat 58), 5.0-5.1 (59),
**     6.0-6.1 (60), 7.0-7.1 (61) and 8.0-8.1 (62). Fields read:
**     AVFormatContext nb_streams/streams/duration, AVStream index/
**     time_base/avg_frame_rate/codecpar, AVCodecParameters codec_type/
**     codec_id, AVPacket stream_index, and the AVFrame head up to pts
**     (avutil 56-59 have key_frame before pict_type, avutil 60 does not).
**   - The library set must match (avformat M, avcodec M, avutil M-2) and
**     every stream we use is cross-checked (its index field, its codec
**     type) before decoding, so a layout surprise means "unsupported",
**     never a crash. 64-bit only: we have no 32-bit FFmpeg to verify.
**   - Audio sample rate and channel count come from AVOptions on the codec
**     context ("ar", "ch_layout" or "ac"), which avoids the deep and
**     version-dependent channel-layout fields; samples are converted to
**     float here, so swresample is not needed. swscale is optional and only
**     used for pixel formats other than YUV420P.
**   - One pull loop (vid_decode) interleaves video and audio exactly as the
**     demuxer delivers them; the caller does the clocking.
*/
#include "fdec_vid.h"
#include "fdec_vid_int.h"
#include "fplat.h"
#include "fsdl.h"

#include "pl_mpeg.h"

#if !defined(FM_ANDROID) && !defined(FM_WEB) && !defined(FM_IOS) && !defined(__TINYC__) && \
    (defined(FM_X64) || defined(FM_ARM64))
#  define FM_FFMPEG 1
#endif

#ifdef FM_FFMPEG
#  ifdef FM_WIN
#    include "fwin.h"
#  else
#    include <dlfcn.h>
#  endif
#  include <errno.h>
#endif

/* ---- FFmpeg declarations -------------------------------------------------- */

#ifdef FM_FFMPEG

typedef struct FfRational { int num, den; } FfRational;

/* AVPacket as allocated by av_packet_alloc; we only read the head. */
typedef struct FfPacket {
  void *buf;
  i64 pts, dts;
  u8 *data;
  int size, stream_index, flags;
} FfPacket;

typedef struct FfPacket58 {
  void *buf; i64 pts, dts; u8 *data; int size, stream_index, flags;
  void *side_data; int side_data_elems; i64 duration, pos, convergence_duration;
} FfPacket58;

typedef struct FfPacket59 {
  void *buf; i64 pts, dts; u8 *data; int size, stream_index, flags;
  void *side_data; int side_data_elems; i64 duration, pos;
  void *opaque; void *opaque_ref; FfRational time_base;
} FfPacket59;

/* AVFrame head, avutil 56..59; the fields up to `format` are shared with 60 */
typedef struct FfFrame {
  u8 *data[8];
  int linesize[8];
  u8 **extended_data;
  int width, height, nb_samples, format;
  int key_frame;
  int pict_type;
  FfRational sar;
  i64 pts;
} FfFrame;

/* AVFrame head, avutil 60 (FFmpeg 8): key_frame is gone */
typedef struct FfFrame60 {
  u8 *data[8];
  int linesize[8];
  u8 **extended_data;
  int width, height, nb_samples, format;
  int pict_type;
  FfRational sar;
  i64 pts;
} FfFrame60;

typedef struct FfCodecPar { int codec_type; int codec_id; } FfCodecPar;

typedef struct FfStream58 {
  int index, id;
  void *codec;
  void *priv_data;
  FfRational time_base;
  i64 start_time, duration, nb_frames;
  int disposition, discard;
  FfRational sar;
  void *metadata;
  FfRational avg_frame_rate;
  FfPacket58 attached_pic;
  void *side_data;
  int nb_side_data;
  int event_flags;
  FfRational r_frame_rate;
  char *recommended_encoder_configuration;
  FfCodecPar *codecpar;
} FfStream58;

typedef struct FfStream59 {
  int index, id;
  void *priv_data;
  FfRational time_base;
  i64 start_time, duration, nb_frames;
  int disposition, discard;
  FfRational sar;
  void *metadata;
  FfRational avg_frame_rate;
  FfPacket59 attached_pic;
  void *side_data;
  int nb_side_data;
  int event_flags;
  FfRational r_frame_rate;
  FfCodecPar *codecpar;
} FfStream59;

typedef struct FfStream60 {           /* also 61 and 62 */
  const void *av_class;
  int index, id;
  FfCodecPar *codecpar;
  void *priv_data;
  FfRational time_base;
  i64 start_time, duration, nb_frames;
  int disposition, discard;
  FfRational sar;
  void *metadata;
  FfRational avg_frame_rate;
} FfStream60;

typedef struct FfFormat58 {
  const void *av_class, *iformat, *oformat;
  void *priv_data, *pb;
  int ctx_flags;
  unsigned nb_streams;
  void **streams;
  char filename[1024];
  char *url;
  i64 start_time, duration;
} FfFormat58;

typedef struct FfFormat59 {           /* also 60 */
  const void *av_class, *iformat, *oformat;
  void *priv_data, *pb;
  int ctx_flags;
  unsigned nb_streams;
  void **streams;
  char *url;
  i64 start_time, duration;
} FfFormat59;

typedef struct FfFormat61 {           /* also 62 */
  const void *av_class, *iformat, *oformat;
  void *priv_data, *pb;
  int ctx_flags;
  unsigned nb_streams;
  void **streams;
  unsigned nb_stream_groups;
  void **stream_groups;
  unsigned nb_chapters;
  void **chapters;
  char *url;
  i64 start_time, duration;
} FfFormat61;

#define FF_NOPTS ((i64)0x8000000000000000ull)
#define FF_TIME_BASE 1000000
#define FF_EOF (-(int)((u32)'E' | (u32)'O' << 8 | (u32)'F' << 16 | (u32)' ' << 24))
#define FF_EAGAIN (-(EAGAIN))
enum { FF_MEDIA_VIDEO = 0, FF_MEDIA_AUDIO = 1 };
enum { FF_PIX_YUV420P = 0, FF_PIX_YUVJ420P = 12 };
enum {
  FF_SMP_U8, FF_SMP_S16, FF_SMP_S32, FF_SMP_FLT, FF_SMP_DBL,
  FF_SMP_U8P, FF_SMP_S16P, FF_SMP_S32P, FF_SMP_FLTP, FF_SMP_DBLP, FF_SMP_S64, FF_SMP_S64P
};

typedef struct FfApi {
  int major;                         /* avformat major, 0 = unusable */
  char info[96];
  unsigned (*avformat_version)(void);
  unsigned (*avcodec_version)(void);
  unsigned (*avutil_version)(void);
  int (*avformat_open_input)(void **ps, const char *url, const void *fmt, void **options);
  int (*avformat_find_stream_info)(void *ic, void **options);
  int (*av_find_best_stream)(void *ic, int type, int wanted, int related, const void **dec, int flags);
  int (*av_read_frame)(void *s, FfPacket *pkt);
  int (*av_seek_frame)(void *s, int stream_index, i64 timestamp, int flags);
  void (*avformat_close_input)(void **s);
  void *(*avcodec_alloc_context3)(const void *codec);
  int (*avcodec_parameters_to_context)(void *ctx, const void *par);
  int (*avcodec_open2)(void *ctx, const void *codec, void **options);
  int (*avcodec_send_packet)(void *ctx, const FfPacket *pkt);
  int (*avcodec_receive_frame)(void *ctx, FfFrame *frame);
  void (*avcodec_flush_buffers)(void *ctx);
  void (*avcodec_free_context)(void **ctx);
  const char *(*avcodec_get_name)(int id);
  FfPacket *(*av_packet_alloc)(void);
  void (*av_packet_free)(FfPacket **pkt);
  void (*av_packet_unref)(FfPacket *pkt);
  FfFrame *(*av_frame_alloc)(void);
  void (*av_frame_free)(FfFrame **frame);
  void (*av_frame_unref)(FfFrame *frame);
  void (*av_log_set_level)(int level);
  int (*av_opt_set_int)(void *obj, const char *name, i64 val, int flags);
  int (*av_opt_get_int)(void *obj, const char *name, int flags, i64 *out);
  int (*av_opt_get_chlayout)(void *obj, const char *name, int flags, void *layout);
  void (*av_channel_layout_uninit)(void *layout);
  void *(*sws_getCachedContext)(void *ctx, int sw, int sh, int sf, int dw, int dh, int df, int flags,
                                void *srcf, void *dstf, const double *param);
  int (*sws_scale)(void *ctx, const u8 *const src[], const int ss[], int y, int h, u8 *const dst[],
                   const int ds[]);
  void (*sws_freeContext)(void *ctx);
} FfApi;

static FfApi ff;
static SDL_atomic_t g_ff_state;      /* 0 untried, 1 loading, 2 done */

/* ---- loading ---------------------------------------------------------------- */

typedef void *LibH;

static LibH lib_open(const char *path) {
#ifdef FM_WIN
  wchar_t w[FM_PATH_MAX];
  if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, FM_PATH_MAX)) return NULL;
  bool full = strchr(path, '\\') || strchr(path, '/');
  return (LibH)LoadLibraryExW(w, NULL, full ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
#else
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void lib_close(LibH h) {
  if (!h) return;
#ifdef FM_WIN
  FreeLibrary((HMODULE)h);
#else
  dlclose(h);
#endif
}

static void *lib_sym(LibH h, const char *name) {
  if (!h) return NULL;
#ifdef FM_WIN
  return (void *)GetProcAddress((HMODULE)h, name);
#else
  return dlsym(h, name);
#endif
}

/* Tries the exe folder (and platform folders), then the system search. */
static LibH lib_find(const char *name) {
  char p[FM_PATH_MAX];
  char *base = SDL_GetBasePath();
  LibH h = NULL;
  if (base) {
    static const char *const kSub[] = {
#if defined(FM_LINUX) || defined(FM_BSD)
      "lib/",
#elif defined(FM_MACOS)
      "../Frameworks/",
#endif
      "",
    };
    for (int i = 0; i < FM_COUNT(kSub) && !h; i++) {
      fm_snprintf(p, sizeof p, "%s%s%s", base, kSub[i], name);
      h = lib_open(p);
    }
    SDL_free(base);
  }
#if defined(FM_MACOS)
  static const char *const kDirs[] = { "/opt/homebrew/lib/", "/usr/local/lib/", "/opt/local/lib/" };
  for (int i = 0; i < FM_COUNT(kDirs) && !h; i++) {
    fm_snprintf(p, sizeof p, "%s%s", kDirs[i], name);
    h = lib_open(p);
  }
#endif
  if (!h) h = lib_open(name);
  return h;
}

static void lib_name(char *out, size_t cap, const char *lib, int major) {
#if defined(FM_WIN)
  fm_snprintf(out, cap, "%s-%d.dll", lib, major);
#elif defined(FM_MACOS)
  fm_snprintf(out, cap, "lib%s.%d.dylib", lib, major);
#else
  fm_snprintf(out, cap, "lib%s.so.%d", lib, major);
#endif
}

#define FF_SYM(h, name) (*(void **)&ff.name = lib_sym(h, #name), ff.name != NULL)

static bool ff_try(int M, LibH *hu, LibH *hc, LibH *hf, LibH *hs) {
  char n[64];
  lib_name(n, sizeof n, "avutil", M - 2);
  if (!(*hu = lib_find(n))) return false;
  lib_name(n, sizeof n, "avcodec", M);
  if (!(*hc = lib_find(n))) return false;
  lib_name(n, sizeof n, "avformat", M);
  if (!(*hf = lib_find(n))) return false;
  lib_name(n, sizeof n, "swscale", M - 53);
  *hs = lib_find(n);
  LibH u = *hu, c = *hc, f = *hf, s = *hs;
  bool ok = FF_SYM(f, avformat_version) && FF_SYM(c, avcodec_version) && FF_SYM(u, avutil_version) &&
            FF_SYM(f, avformat_open_input) && FF_SYM(f, avformat_find_stream_info) &&
            FF_SYM(f, av_find_best_stream) && FF_SYM(f, av_read_frame) && FF_SYM(f, av_seek_frame) &&
            FF_SYM(f, avformat_close_input) && FF_SYM(c, avcodec_alloc_context3) &&
            FF_SYM(c, avcodec_parameters_to_context) && FF_SYM(c, avcodec_open2) &&
            FF_SYM(c, avcodec_send_packet) && FF_SYM(c, avcodec_receive_frame) &&
            FF_SYM(c, avcodec_flush_buffers) && FF_SYM(c, avcodec_free_context) &&
            FF_SYM(c, avcodec_get_name) && FF_SYM(c, av_packet_alloc) && FF_SYM(c, av_packet_free) &&
            FF_SYM(c, av_packet_unref) && FF_SYM(u, av_frame_alloc) && FF_SYM(u, av_frame_free) &&
            FF_SYM(u, av_frame_unref) && FF_SYM(u, av_log_set_level) && FF_SYM(u, av_opt_set_int) &&
            FF_SYM(u, av_opt_get_int);
  if (!ok) return false;
  FF_SYM(u, av_opt_get_chlayout);
  FF_SYM(u, av_channel_layout_uninit);
  if (!ff.av_opt_get_chlayout || !ff.av_channel_layout_uninit) {
    ff.av_opt_get_chlayout = NULL;
    ff.av_channel_layout_uninit = NULL;
  }
  if (s && !(FF_SYM(s, sws_getCachedContext) && FF_SYM(s, sws_scale) && FF_SYM(s, sws_freeContext))) {
    ff.sws_getCachedContext = NULL;
    ff.sws_scale = NULL;
    ff.sws_freeContext = NULL;
  }
  /* run-time versions must be the set we declared structs for */
  unsigned vf = ff.avformat_version() >> 16, vc = ff.avcodec_version() >> 16, vu = ff.avutil_version() >> 16;
  if ((int)vf != M || (int)vc != M || (int)vu != M - 2) return false;
  return true;
}

static void ff_load(void) {
  static const int kMajors[] = { 62, 61, 60, 59, 58 };
  for (int i = 0; i < FM_COUNT(kMajors); i++) {
    LibH hu = NULL, hc = NULL, hf = NULL, hs = NULL;
    FfApi keep = ff;
    if (ff_try(kMajors[i], &hu, &hc, &hf, &hs)) {
      ff.major = kMajors[i];
      unsigned v = ff.avformat_version();
      fm_snprintf(ff.info, sizeof ff.info, "FFmpeg %d (avformat %u.%u.%u)%s", ff.major - 54, v >> 16,
                  (v >> 8) & 255, v & 255, ff.sws_scale ? "" : ", no swscale");
      ff.av_log_set_level(-8);           /* AV_LOG_QUIET */
      fm_log("video: %s", ff.info);
      return;                            /* libraries stay loaded for the process */
    }
    lib_close(hs); lib_close(hf); lib_close(hc); lib_close(hu);
    ff = keep;
  }
  fm_snprintf(ff.info, sizeof ff.info, "FFmpeg 4-8 libraries not found");
}

bool ff_available(void) {
  if (SDL_AtomicGet(&g_ff_state) != 2) {
    if (SDL_AtomicCAS(&g_ff_state, 0, 1)) {
      ff_load();
      SDL_AtomicSet(&g_ff_state, 2);
    } else {
      while (SDL_AtomicGet(&g_ff_state) != 2) SDL_Delay(1);
    }
  }
  return ff.major != 0;
}

const char *ff_version_str(void) {
  ff_available();
  return ff.info;
}

/* Stream fields by layout version; false when the stream fails the checks. */
static bool ff_stream(void *fmt, int idx, int type, FfCodecPar **par, FfRational *tb, FfRational *fps) {
  unsigned nb;
  void **streams;
  if (ff.major == 58) { FfFormat58 *f = (FfFormat58 *)fmt; nb = f->nb_streams; streams = f->streams; }
  else if (ff.major >= 61) { FfFormat61 *f = (FfFormat61 *)fmt; nb = f->nb_streams; streams = f->streams; }
  else { FfFormat59 *f = (FfFormat59 *)fmt; nb = f->nb_streams; streams = f->streams; }
  if (!streams || idx < 0 || (unsigned)idx >= nb || nb > 4096) return false;
  void *st = streams[idx];
  if (!st) return false;
  int sidx;
  if (ff.major == 58) {
    FfStream58 *s = (FfStream58 *)st;
    sidx = s->index; *par = s->codecpar; *tb = s->time_base; *fps = s->avg_frame_rate;
  } else if (ff.major == 59) {
    FfStream59 *s = (FfStream59 *)st;
    sidx = s->index; *par = s->codecpar; *tb = s->time_base; *fps = s->avg_frame_rate;
  } else {
    FfStream60 *s = (FfStream60 *)st;
    sidx = s->index; *par = s->codecpar; *tb = s->time_base; *fps = s->avg_frame_rate;
  }
  if (sidx != idx || !*par || (*par)->codec_type != type) return false;
  if (tb->num <= 0 || tb->den <= 0) return false;
  return true;
}

static i64 frame_pts(const FfFrame *f) {
  return ff.major >= 62 ? ((const FfFrame60 *)f)->pts : f->pts;
}

static FfRational frame_sar(const FfFrame *f) {
  return ff.major >= 62 ? ((const FfFrame60 *)f)->sar : f->sar;
}

static i64 ff_duration(void *fmt) {
  if (ff.major == 58) return ((FfFormat58 *)fmt)->duration;
  if (ff.major >= 61) return ((FfFormat61 *)fmt)->duration;
  return ((FfFormat59 *)fmt)->duration;
}

#else  /* !FM_FFMPEG */

bool ff_available(void) { return false; }
const char *ff_version_str(void) { return "FFmpeg is not supported on this platform"; }

#endif

/* ---- the decoder object ---------------------------------------------------- */

struct FmVid {
  FmVidInfo info;
  int flags;
  /* pl_mpeg */
  plm_t *plm;
  double pl_vt, pl_at;
  bool pl_vdone, pl_adone, pl_seeking;
  plm_frame_t *pl_pending;
#ifdef FM_FFMPEG
  void *fmt;
  void *vctx, *actx;
  int vidx, aidx;
  FfRational vtb, atb;
  FfPacket *pkt;
  FfFrame *frm;
  bool eof_read, vflushed, aflushed, vdone, adone;
  double last_vt;
  void *sws;
  u8 *yuv[3];
  int yuv_stride[3];
  int yuv_w, yuv_h;
#endif
  double skip_until;
  float *pcm;
  int pcm_cap;
  /* OS backend (Media Foundation / MediaCodec) */
  const FmVidBackend *os;
  void *os_st;
  /* seek fallback for streams without an index (MPEG-TS with sparse key
  ** frames): if a seek runs into the end with no picture, restart at 0 and
  ** drop everything before the target */
  double seek_t;
  bool seek_watch;
  double drop_until;
};

#define SEEK_REPLAY_MAX 120.0           /* seconds decoded forward at most */

void vid_nv12_split(const u8 *uv, int uv_stride, int cw, int ch, u8 *u, int ustride, u8 *v,
                    int vstride) {
  for (int y = 0; y < ch; y++) {
    const u8 *s = uv + (size_t)y * uv_stride;
    u8 *du = u + (size_t)y * ustride, *dv = v + (size_t)y * vstride;
    for (int x = 0; x < cw; x++) {
      du[x] = s[2 * x];
      dv[x] = s[2 * x + 1];
    }
  }
}

const FmVidInfo *vid_info(const FmVid *v) { return &v->info; }

static float *pcm_buf(FmVid *v, int frames, int ch) {
  int need = frames * ch;
  if (need > v->pcm_cap) {
    fm_free(v->pcm);
    v->pcm_cap = need;
    v->pcm = (float *)fm_alloc((size_t)need * sizeof(float));
  }
  return v->pcm;
}

/* ---- pl_mpeg backend -------------------------------------------------------- */

bool vid_is_mpeg1(const char *path) {
  enum { HEAD = 64 * 1024 };
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  u8 *b = (u8 *)fm_alloc(HEAD);
  size_t n = fread(b, 1, HEAD, f);
  fclose(f);
  /* MPEG-1 pack header: 00 00 01 BA then '0010' marker bits */
  bool ok = n >= 5 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xBA && (b[4] & 0xF0) == 0x20;
  /* ... but the video inside may still be MPEG-2 (a sequence extension,
  ** 00 00 01 B5, follows its sequence header); pl_mpeg cannot decode that */
  for (size_t i = 0; ok && i + 4 <= n; i++)
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xB5) ok = false;
  fm_free(b);
  return ok;
}

static void pl_video_cb(plm_t *p, plm_frame_t *frame, void *user) {
  FmVid *v = (FmVid *)user;
  FM_UNUSED(p);
  if (v->pl_seeking) v->pl_pending = frame;
}

static void pl_audio_cb(plm_t *p, plm_samples_t *s, void *user) {
  FM_UNUSED(p); FM_UNUSED(s); FM_UNUSED(user);
}

static bool pl_open(FmVid *v, const char *path) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  v->plm = plm_create_with_file(f, 1);
  if (!v->plm) return false;
  if (!plm_has_headers(v->plm)) { plm_destroy(v->plm); v->plm = NULL; return false; }
  plm_set_loop(v->plm, 0);
  FmVidInfo *in = &v->info;
  in->has_video = !(v->flags & VID_OPEN_AUDIO_ONLY) && plm_get_num_video_streams(v->plm) > 0;
  in->has_audio = !(v->flags & VID_OPEN_NO_AUDIO) && plm_get_num_audio_streams(v->plm) > 0;
  plm_set_video_enabled(v->plm, in->has_video);
  plm_set_audio_enabled(v->plm, in->has_audio);
  plm_set_video_decode_callback(v->plm, pl_video_cb, v);
  plm_set_audio_decode_callback(v->plm, pl_audio_cb, v);
  if (in->has_video) {
    in->w = plm_get_width(v->plm);
    in->h = plm_get_height(v->plm);
    in->fps = plm_get_framerate(v->plm);
    double par = plm_get_pixel_aspect_ratio(v->plm);
    in->sar = par > 0.1 && par < 10 ? par : 1.0;
    fm_strlcpy(in->vcodec, "MPEG-1", sizeof in->vcodec);
  }
  if (in->has_audio) {
    in->rate = plm_get_samplerate(v->plm);
    in->channels = 2;
    fm_strlcpy(in->acodec, "MP2", sizeof in->acodec);
  }
  in->duration = plm_get_duration(v->plm);
  fm_strlcpy(in->backend, "MPEG-1", sizeof in->backend);
  return in->has_video || in->has_audio;
}

static int pl_emit_video(FmVid *v, plm_frame_t *f, FmVidFrame *vf) {
  FM_UNUSED(v);
  vf->t = f->time;
  vf->w = (int)f->width;
  vf->h = (int)f->height;
  vf->plane[0] = f->y.data;  vf->stride[0] = (int)f->y.width;
  vf->plane[1] = f->cb.data; vf->stride[1] = (int)f->cb.width;
  vf->plane[2] = f->cr.data; vf->stride[2] = (int)f->cr.width;
  return VID_EV_VIDEO;
}

static int pl_decode(FmVid *v, FmVidFrame *vf, FmVidPcm *pc) {
  if (v->pl_pending) {
    plm_frame_t *f = v->pl_pending;
    v->pl_pending = NULL;
    v->pl_vt = f->time;
    return pl_emit_video(v, f, vf);
  }
  for (;;) {
    bool can_v = v->info.has_video && !v->pl_vdone;
    bool can_a = v->info.has_audio && !v->pl_adone;
    if (!can_v && !can_a) return VID_EV_END;
    if (can_a && (!can_v || v->pl_at <= v->pl_vt)) {
      plm_samples_t *s = plm_decode_audio(v->plm);
      if (!s) { v->pl_adone = true; continue; }
      v->pl_at = s->time;
      float *out = pcm_buf(v, (int)s->count, 2);
      memcpy(out, s->interleaved, sizeof(float) * 2 * s->count);
      pc->t = s->time;
      pc->frames = (int)s->count;
      pc->channels = 2;
      pc->rate = v->info.rate;
      pc->pcm = out;
      return VID_EV_AUDIO;
    }
    plm_frame_t *f = plm_decode_video(v->plm);
    if (!f) { v->pl_vdone = true; continue; }
    v->pl_vt = f->time;
    return pl_emit_video(v, f, vf);
  }
}

static bool pl_seek(FmVid *v, double t) {
  v->pl_seeking = true;
  v->pl_pending = NULL;
  int ok;
  if (v->info.has_video) {
    ok = plm_seek(v->plm, t, 0);
  } else {
    plm_rewind(v->plm);
    ok = 1;
    /* audio only: decode forward to t (MP2 frames are cheap) */
    plm_samples_t *s;
    while ((s = plm_decode_audio(v->plm)) && s->time + 1152.0 / FM_MAX(1, v->info.rate) < t) {}
  }
  v->pl_seeking = false;
  v->pl_vdone = v->pl_adone = false;
  v->pl_vt = v->pl_at = t;
  return ok != 0;
}

/* ---- FFmpeg backend ------------------------------------------------------------ */

#ifdef FM_FFMPEG

static int ff_threads(void) {
  int n = plat_cpu_count();
  return FM_CLAMP(n, 1, 4);
}

static void *ff_open_codec(void *fmt, int idx, int type, FfRational *tb, FfRational *fps, char *name,
                           size_t name_cap) {
  FfCodecPar *par;
  if (!ff_stream(fmt, idx, type, &par, tb, fps)) return NULL;
  const void *dec = NULL;
  /* av_find_best_stream returns the decoder for this exact stream */
  if (ff.av_find_best_stream(fmt, type, idx, -1, &dec, 0) != idx || !dec) return NULL;
  void *ctx = ff.avcodec_alloc_context3(dec);
  if (!ctx) return NULL;
  if (ff.avcodec_parameters_to_context(ctx, par) < 0) { ff.avcodec_free_context(&ctx); return NULL; }
  ff.av_opt_set_int(ctx, "threads", type == FF_MEDIA_VIDEO ? ff_threads() : 1, 0);
  if (ff.avcodec_open2(ctx, dec, NULL) < 0) { ff.avcodec_free_context(&ctx); return NULL; }
  const char *cn = ff.avcodec_get_name(par->codec_id);
  fm_strlcpy(name, cn ? cn : "", name_cap);
  return ctx;
}

static int ff_channels(void *ctx) {
  if (ff.av_opt_get_chlayout) {
    /* AVChannelLayout: { int order; int nb_channels; union; void *opaque } */
    union { u8 b[64]; struct { int order, nb_channels; } h; } lay;
    memset(&lay, 0, sizeof lay);
    if (ff.av_opt_get_chlayout(ctx, "ch_layout", 0, &lay) >= 0) {
      int n = lay.h.nb_channels;
      ff.av_channel_layout_uninit(&lay);
      if (n > 0 && n <= 64) return n;
    }
  }
  i64 v = 0;
  if (ff.av_opt_get_int(ctx, "ac", 0, &v) >= 0 && v > 0 && v <= 64) return (int)v;
  return 0;
}

static bool ff_open(FmVid *v, const char *path) {
  if (!ff_available()) return false;
  void *fmt = NULL;
  if (ff.avformat_open_input(&fmt, path, NULL, NULL) < 0 || !fmt) return false;
  v->fmt = fmt;
  if (ff.avformat_find_stream_info(fmt, NULL) < 0) return false;
  FmVidInfo *in = &v->info;
  v->vidx = v->aidx = -1;
  FfRational fps = { 0, 1 }, dummy;
  if (!(v->flags & VID_OPEN_AUDIO_ONLY)) {
    int i = ff.av_find_best_stream(fmt, FF_MEDIA_VIDEO, -1, -1, NULL, 0);
    if (i >= 0) {
      v->vctx = ff_open_codec(fmt, i, FF_MEDIA_VIDEO, &v->vtb, &fps, in->vcodec, sizeof in->vcodec);
      if (v->vctx) v->vidx = i;
    }
  }
  if (!(v->flags & VID_OPEN_NO_AUDIO)) {
    int i = ff.av_find_best_stream(fmt, FF_MEDIA_AUDIO, -1, v->vidx, NULL, 0);
    if (i >= 0) {
      v->actx = ff_open_codec(fmt, i, FF_MEDIA_AUDIO, &v->atb, &dummy, in->acodec, sizeof in->acodec);
      if (v->actx) {
        i64 rate = 0;
        int ch = ff_channels(v->actx);
        if (ff.av_opt_get_int(v->actx, "ar", 0, &rate) >= 0 && rate > 0 && rate <= 768000 && ch > 0) {
          v->aidx = i;
          in->rate = (int)rate;
          in->channels = ch >= 2 ? 2 : 1;
        } else {
          ff.avcodec_free_context(&v->actx);
        }
      }
    }
  }
  in->has_video = v->vctx != NULL;
  in->has_audio = v->actx != NULL;
  if (!in->has_video && !in->has_audio) return false;
  in->fps = fps.den > 0 && fps.num > 0 ? (double)fps.num / fps.den : 0;
  if (in->fps > 1000) in->fps = 0;
  i64 d = ff_duration(fmt);
  in->duration = d > 0 && d != FF_NOPTS ? (double)d / FF_TIME_BASE : 0;
  in->sar = 1.0;
  fm_strlcpy(in->backend, ff.info, sizeof in->backend);
  {
    char b[24];
    fm_snprintf(b, sizeof b, "FFmpeg %d", ff.major - 54);
    fm_strlcpy(in->backend, b, sizeof in->backend);
  }
  v->pkt = ff.av_packet_alloc();
  v->frm = ff.av_frame_alloc();
  return v->pkt && v->frm;
}

static void ff_close(FmVid *v) {
  if (!ff.major) return;
  if (v->frm) ff.av_frame_free(&v->frm);
  if (v->pkt) ff.av_packet_free(&v->pkt);
  if (v->vctx) ff.avcodec_free_context(&v->vctx);
  if (v->actx) ff.avcodec_free_context(&v->actx);
  if (v->fmt) ff.avformat_close_input(&v->fmt);
  if (v->sws && ff.sws_freeContext) ff.sws_freeContext(v->sws);
  fm_free(v->yuv[0]);
}

static double ts(i64 pts, FfRational tb) { return (double)pts * tb.num / tb.den; }

static bool ff_emit_video(FmVid *v, FmVidFrame *vf) {
  FfFrame *f = v->frm;
  if (f->width <= 0 || f->height <= 0 || f->width > 16384 || f->height > 16384) return false;
  i64 pts = frame_pts(f);
  double t = pts != FF_NOPTS ? ts(pts, v->vtb) : v->last_vt + (v->info.fps > 0 ? 1.0 / v->info.fps : 0.04);
  v->last_vt = t;
  if (t < v->skip_until - 0.001) return false;
  v->skip_until = 0;
  vf->t = t;
  vf->w = f->width;
  vf->h = f->height;
  FfRational sar = frame_sar(f);
  if (sar.num > 0 && sar.den > 0) v->info.sar = (double)sar.num / sar.den;
  v->info.w = f->width;
  v->info.h = f->height;
  if (f->format == FF_PIX_YUV420P || f->format == FF_PIX_YUVJ420P) {
    for (int i = 0; i < 3; i++) { vf->plane[i] = f->data[i]; vf->stride[i] = f->linesize[i]; }
    return f->data[0] && f->data[1] && f->data[2] && f->linesize[0] > 0;
  }
  if (!ff.sws_scale) return false;
  int w = f->width, h = f->height;
  if (v->yuv_w != w || v->yuv_h != h) {
    fm_free(v->yuv[0]);
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    v->yuv_stride[0] = (w + 31) & ~31;
    v->yuv_stride[1] = v->yuv_stride[2] = (cw + 31) & ~31;
    size_t ys = (size_t)v->yuv_stride[0] * h, cs = (size_t)v->yuv_stride[1] * ch;
    v->yuv[0] = (u8 *)fm_alloc(ys + 2 * cs + 64);
    v->yuv[1] = v->yuv[0] + ys;
    v->yuv[2] = v->yuv[1] + cs;
    v->yuv_w = w;
    v->yuv_h = h;
  }
  v->sws = ff.sws_getCachedContext(v->sws, w, h, f->format, w, h, FF_PIX_YUV420P, 2 /* SWS_BILINEAR */,
                                   NULL, NULL, NULL);
  if (!v->sws) return false;
  ff.sws_scale(v->sws, (const u8 *const *)f->data, f->linesize, 0, h, v->yuv, v->yuv_stride);
  for (int i = 0; i < 3; i++) { vf->plane[i] = v->yuv[i]; vf->stride[i] = v->yuv_stride[i]; }
  return true;
}

static float smp(const u8 *p, int fmt, size_t i) {
  switch (fmt) {
    case FF_SMP_U8: case FF_SMP_U8P: return (p[i] - 128) / 128.0f;
    case FF_SMP_S16: case FF_SMP_S16P: return ((const i16 *)p)[i] / 32768.0f;
    case FF_SMP_S32: case FF_SMP_S32P: return (float)(((const i32 *)p)[i] / 2147483648.0);
    case FF_SMP_FLT: case FF_SMP_FLTP: return ((const float *)p)[i];
    case FF_SMP_DBL: case FF_SMP_DBLP: return (float)((const double *)p)[i];
    case FF_SMP_S64: case FF_SMP_S64P: return (float)(((const i64 *)p)[i] / 9223372036854775808.0);
    default: return 0;
  }
}

static bool ff_emit_audio(FmVid *v, FmVidPcm *pc) {
  FfFrame *f = v->frm;
  int n = f->nb_samples, fmt = f->format;
  int ch = ff_channels(v->actx);
  if (n <= 0 || ch <= 0 || fmt < 0 || fmt > FF_SMP_S64P || !f->extended_data) return false;
  i64 rate = 0;
  if (ff.av_opt_get_int(v->actx, "ar", 0, &rate) < 0 || rate <= 0) return false;
  i64 pts = frame_pts(f);
  double t = pts != FF_NOPTS ? ts(pts, v->atb) : -1;
  if (t >= 0 && t + (double)n / rate < v->skip_until) return false;
  bool planar = fmt >= FF_SMP_U8P && fmt != FF_SMP_S64;
  int oc = ch >= 2 ? 2 : 1;
  float *out = pcm_buf(v, n, oc);
  for (int i = 0; i < n; i++) {
    float l = 0, r = 0;
    int nl = 0, nr = 0;
    for (int c = 0; c < ch; c++) {
      float s = planar ? smp(f->extended_data[c], fmt, (size_t)i) : smp(f->extended_data[0], fmt, (size_t)i * ch + c);
      if (ch == 1) { l = s; nl = 1; break; }
      if (ch >= 6 && c == 3) continue;
      if (ch >= 3 && c == 2) { l += s * 0.7071f; r += s * 0.7071f; continue; }
      if (c & 1) { r += s; nr++; } else { l += s; nl++; }
    }
    if (oc == 1) out[i] = l;
    else {
      float k = 1.0f / (float)FM_MAX(1, FM_MAX(nl, nr));
      out[i * 2] = FM_CLAMP(l * k, -1.0f, 1.0f);
      out[i * 2 + 1] = FM_CLAMP(r * k, -1.0f, 1.0f);
    }
  }
  pc->t = t;
  pc->frames = n;
  pc->channels = oc;
  pc->rate = (int)rate;
  pc->pcm = out;
  return true;
}

static int ff_decode(FmVid *v, FmVidFrame *vf, FmVidPcm *pc) {
  ff.av_frame_unref(v->frm);
  for (int guard = 0; guard < 1000000; guard++) {
    if (v->vctx && !v->vdone) {
      int r = ff.avcodec_receive_frame(v->vctx, v->frm);
      if (r == 0) {
        if (ff_emit_video(v, vf)) return VID_EV_VIDEO;
        ff.av_frame_unref(v->frm);
        continue;
      }
      if (r != FF_EAGAIN || v->vflushed) v->vdone = true;
    }
    if (v->actx && !v->adone) {
      int r = ff.avcodec_receive_frame(v->actx, v->frm);
      if (r == 0) {
        if (ff_emit_audio(v, pc)) return VID_EV_AUDIO;
        ff.av_frame_unref(v->frm);
        continue;
      }
      if (r != FF_EAGAIN || v->aflushed) v->adone = true;
    }
    if ((!v->vctx || v->vdone) && (!v->actx || v->adone)) return VID_EV_END;
    if (v->eof_read) {
      if (v->vctx && !v->vflushed) { ff.avcodec_send_packet(v->vctx, NULL); v->vflushed = true; }
      if (v->actx && !v->aflushed) { ff.avcodec_send_packet(v->actx, NULL); v->aflushed = true; }
      continue;
    }
    int r = ff.av_read_frame(v->fmt, v->pkt);
    if (r < 0) {
      v->eof_read = true;
      continue;
    }
    if (v->pkt->stream_index == v->vidx && v->vctx) ff.avcodec_send_packet(v->vctx, v->pkt);
    else if (v->pkt->stream_index == v->aidx && v->actx) ff.avcodec_send_packet(v->actx, v->pkt);
    ff.av_packet_unref(v->pkt);
  }
  return VID_EV_ERROR;
}

static bool ff_seek(FmVid *v, double t) {
  ff.av_frame_unref(v->frm);
  i64 target = (i64)(t * FF_TIME_BASE);
  bool ok = ff.av_seek_frame(v->fmt, -1, target, 1 /* AVSEEK_FLAG_BACKWARD */) >= 0;
  if (v->vctx) ff.avcodec_flush_buffers(v->vctx);
  if (v->actx) ff.avcodec_flush_buffers(v->actx);
  v->eof_read = v->vflushed = v->aflushed = v->vdone = v->adone = false;
  v->skip_until = t;
  v->last_vt = t;
  return ok;
}

#endif /* FM_FFMPEG */

/* ---- public API -------------------------------------------------------------------- */

FmVid *vid_open(const char *path, int flags, FmErr *err) {
  FmVid *v = (FmVid *)fm_calloc(1, sizeof *v);
  v->flags = flags;
  v->info.sar = 1.0;
  if (vid_is_mpeg1(path) && pl_open(v, path)) { *err = FM_OK; return v; }
  if (v->plm) { plm_destroy(v->plm); v->plm = NULL; }
  memset(&v->info, 0, sizeof v->info);
  v->info.sar = 1.0;
  *err = FM_ERR_UNSUPPORTED;
#ifdef FM_FFMPEG
  if (ff_open(v, path)) { *err = FM_OK; return v; }
  ff_close(v);
  memset(&v->info, 0, sizeof v->info);
  v->info.sar = 1.0;
  if (ff.major) *err = FM_ERR_FORMAT;
#endif
#ifdef FM_VID_OS
  /* the decoders the OS ships: most phone/camera/web formats */
  v->os_st = FM_VID_OS_BACKEND.open(path, flags, &v->info);
  if (v->os_st) {
    v->os = &FM_VID_OS_BACKEND;
    if (!v->info.sar) v->info.sar = 1.0;
    *err = FM_OK;
    return v;
  }
  if (*err == FM_ERR_UNSUPPORTED) *err = FM_ERR_FORMAT;
#endif
  fm_free(v);
  return NULL;
}

static int backend_decode(FmVid *v, FmVidFrame *vf, FmVidPcm *pc) {
  if (v->plm) return pl_decode(v, vf, pc);
#ifdef FM_FFMPEG
  if (v->fmt) return ff_decode(v, vf, pc);
#endif
  if (v->os) return v->os->decode(v->os_st, vf, pc);
  return VID_EV_ERROR;
}

static bool backend_seek(FmVid *v, double t);

int vid_decode(FmVid *v, FmVidFrame *vf, FmVidPcm *pc) {
  for (int guard = 0; guard < 100000; guard++) {
    int ev = backend_decode(v, vf, pc);
    if (v->seek_watch) {
      if (ev == VID_EV_VIDEO) v->seek_watch = false;
      else if (ev == VID_EV_END && v->info.has_video && v->seek_t > 0.5 && v->seek_t <= SEEK_REPLAY_MAX) {
        v->seek_watch = false;
        if (!backend_seek(v, 0)) return ev;
        v->drop_until = v->seek_t;
        continue;
      }
    }
    if (v->drop_until > 0) {
      double t = ev == VID_EV_VIDEO ? vf->t : ev == VID_EV_AUDIO ? pc->t : 1e300;
      if ((ev == VID_EV_VIDEO || ev == VID_EV_AUDIO) && t < v->drop_until - 0.02) continue;
      if (ev == VID_EV_VIDEO || ev <= 0) v->drop_until = 0;
    }
    return ev;
  }
  return VID_EV_ERROR;
}

bool vid_seek(FmVid *v, double t) {
  if (t < 0) t = 0;
  v->drop_until = 0;
  v->seek_t = t;
  v->seek_watch = true;
  return backend_seek(v, t);
}

static bool backend_seek(FmVid *v, double t) {
  if (v->plm) return pl_seek(v, t);
#ifdef FM_FFMPEG
  if (v->fmt) return ff_seek(v, t);
#endif
  if (v->os) return v->os->seek(v->os_st, t);
  return false;
}

void vid_close(FmVid *v) {
  if (!v) return;
  if (v->plm) plm_destroy(v->plm);
#ifdef FM_FFMPEG
  if (v->fmt || v->pkt || v->frm) ff_close(v);
#endif
  if (v->os) v->os->close(v->os_st);
  fm_free(v->pcm);
  fm_free(v);
}

FmErr vid_thumb(const char *path, int max_px, FmImage *out) {
  memset(out, 0, sizeof *out);
  FmErr err;
  FmVid *v = vid_open(path, VID_OPEN_NO_AUDIO, &err);
  if (!v) return err;
  if (!v->info.has_video) { vid_close(v); return FM_ERR_FORMAT; }
  /* a little into the clip: first frames are often black */
  double d = v->info.duration;
  if (d > 4) vid_seek(v, FM_MIN(d * 0.1, 10.0));
  FmVidFrame vf;
  FmVidPcm pc;
  int ev, guard = 0;
  while ((ev = vid_decode(v, &vf, &pc)) == VID_EV_AUDIO && guard++ < 1000) {}
  if (ev != VID_EV_VIDEO && d > 4) {          /* seek failed: try from the start */
    vid_seek(v, 0);
    while ((ev = vid_decode(v, &vf, &pc)) == VID_EV_AUDIO && guard++ < 2000) {}
  }
  if (ev != VID_EV_VIDEO || vf.w <= 0 || vf.h <= 0) { vid_close(v); return FM_ERR_FORMAT; }
  double sar = v->info.sar > 0.1 && v->info.sar < 10 ? v->info.sar : 1.0;
  int dw0 = (int)(vf.w * sar + 0.5), dh0 = vf.h;
  int dw, dh;
  img_fit(dw0, dh0, max_px, max_px, &dw, &dh);
  if (dw > vf.w) dw = vf.w;                    /* never upscale in x */
  out->w = dw;
  out->h = dh;
  out->px = (u8 *)fm_alloc((size_t)dw * dh * 4);
  img_yuv420_to_rgba(vf.plane[0], vf.stride[0], vf.plane[1], vf.stride[1], vf.plane[2], vf.stride[2],
                     vf.w, vf.h, out->px, dw, dh);
  vid_close(v);
  return FM_OK;
}
