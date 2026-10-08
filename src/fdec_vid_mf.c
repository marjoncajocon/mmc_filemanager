/* fdec_vid_mf.c -- video through Windows Media Foundation (Windows 7+).
**
** The OS backend for Windows (see fdec_vid_int.h): an IMFSourceReader
** demuxes and decodes MP4/MOV/M4V, MKV and WebM (Windows 10+), AVI, WMV/ASF,
** 3GP, MPEG-TS and whatever else the installed codecs cover (H.264,
** HEVC/VP9/AV1 when their Store extensions are present, WMV, MPEG-4 part 2,
** MJPEG; AAC, MP3, WMA, AC-3, FLAC, Opus ...).
**
** Design decisions:
**   - Nothing is linked: mfplat.dll and mfreadwrite.dll are opened at run
**     time, so the exe still starts on systems without Media Foundation
**     (Windows "N" editions, Server Core); open then just fails and the
**     viewer offers the system player. COM interfaces come from the mingw
**     headers, GUIDs are compiled in (initguid), so no extra import libs.
**   - Video is requested as NV12 first (what decoders produce, no
**     conversion), then I420 / YV12, then RGB32 through the reader's video
**     processor for odd decoders (Windows 7). Everything leaves as YUV420P
**     planes copied into buffers we own, cropped to the display aperture
**     (H.264 1080p is coded as 1088 lines).
**   - Audio is requested as float; the reader's resampler downmixes to
**     stereo when it can, otherwise we downmix here.
**   - Synchronous ReadSample on the caller's (worker) thread; COM is
**     initialised per call thread as multithreaded, and MFStartup /
**     MFShutdown are reference counted by Media Foundation itself.
*/
#include "fcore.h"

#if defined(FM_WIN) && !defined(__TINYC__)

#define COBJMACROS
#include "fwin.h"
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include "fdec_vid_int.h"
#include "fsdl.h"
#include "fplat.h"

/* ---- loading -------------------------------------------------------------- */

typedef HRESULT (WINAPI *MFStartupFn)(ULONG, DWORD);
typedef HRESULT (WINAPI *MFShutdownFn)(void);
typedef HRESULT (WINAPI *MFCreateAttributesFn)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *MFCreateMediaTypeFn)(IMFMediaType **);
typedef HRESULT (WINAPI *MFCreateReaderFn)(LPCWSTR, IMFAttributes *, IMFSourceReader **);

static struct {
  int state;                 /* 0 untried, 1 ok, -1 missing */
  MFStartupFn startup;
  MFShutdownFn shutdown;
  MFCreateAttributesFn create_attributes;
  MFCreateMediaTypeFn create_media_type;
  MFCreateReaderFn create_reader;
} mf;

static SDL_SpinLock g_mf_lock;

#define MF_VERSION_WIN7 0x00020070   /* MF_SDK_VERSION 2, MF_API_VERSION 0x70 */

static bool mf_load(void) {
  SDL_AtomicLock(&g_mf_lock);
  if (!mf.state) {
    HMODULE plat = LoadLibraryW(L"mfplat.dll");
    HMODULE rw = plat ? LoadLibraryW(L"mfreadwrite.dll") : NULL;
    if (rw) {
      mf.startup = (MFStartupFn)(void (*)(void))GetProcAddress(plat, "MFStartup");
      mf.shutdown = (MFShutdownFn)(void (*)(void))GetProcAddress(plat, "MFShutdown");
      mf.create_attributes = (MFCreateAttributesFn)(void (*)(void))GetProcAddress(plat, "MFCreateAttributes");
      mf.create_media_type = (MFCreateMediaTypeFn)(void (*)(void))GetProcAddress(plat, "MFCreateMediaType");
      mf.create_reader =
          (MFCreateReaderFn)(void (*)(void))GetProcAddress(rw, "MFCreateSourceReaderFromURL");
    }
    mf.state = (mf.startup && mf.shutdown && mf.create_attributes && mf.create_media_type &&
                mf.create_reader) ? 1 : -1;
  }
  SDL_AtomicUnlock(&g_mf_lock);
  return mf.state > 0;
}

/* IMF2DBuffer gives the real row pitch; declared here so no uuid lib is needed. */
DEFINE_GUID(FM_IID_IMF2DBuffer, 0x7dc9d5f9, 0x9ed9, 0x44ec, 0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb);

/* ---- state ---------------------------------------------------------------- */

enum { PIX_NONE, PIX_NV12, PIX_I420, PIX_YV12, PIX_RGB32 };

typedef struct MfVid {
  IMFSourceReader *rd;
  bool com_init, started;
  DWORD vs, as;              /* selected streams (reader aliases), (DWORD)-1 = none */
  DWORD vi, ai;              /* their real indexes, as ReadSample reports them */
  int pix;
  int fw, fh;                /* frame size of the buffers (may include padding rows) */
  int dx, dy, dw, dh;        /* display aperture */
  LONG def_stride;           /* MF_MT_DEFAULT_STRIDE, 0 = unknown */
  int ach, arate;            /* channels the reader delivers */
  bool vdone, adone;
  u8 *plane[3];
  int pstride[3];
  int pw, ph;                /* allocated plane size */
  float *pcm;
  int pcm_cap;
  /* for sources that cannot seek (MPEG-TS): reopen and skip ahead */
  char *path;
  int flags;
  double skip_until;
  bool can_seek;
} MfVid;

#define NO_STREAM ((DWORD)-1)

static void sr(void *p) { if (p) IUnknown_Release((IUnknown *)p); }

/* ---- media types ---------------------------------------------------------- */

static UINT32 attr_u32(IMFAttributes *a, REFGUID k, UINT32 def) {
  UINT32 v;
  return SUCCEEDED(IMFAttributes_GetUINT32(a, k, &v)) ? v : def;
}

static bool attr_pair(IMFAttributes *a, REFGUID k, UINT32 *hi, UINT32 *lo) {
  UINT64 v;
  if (FAILED(IMFAttributes_GetUINT64(a, k, &v))) return false;
  *hi = (UINT32)(v >> 32);
  *lo = (UINT32)v;
  return true;
}

/* short codec name from the stream's native subtype */
static void codec_name(IMFSourceReader *rd, DWORD stream, char *out, size_t cap) {
  IMFMediaType *t = NULL;
  GUID g;
  out[0] = 0;
  if (FAILED(IMFSourceReader_GetNativeMediaType(rd, stream, 0, &t))) return;
  if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &g))) {
    static const struct { DWORD id; const char *name; } kNames[] = {
      { 0x34363248, "h264" }, { 0x3F40F4F0, "h264" } /* H264_ES */, { 0x43564548, "hevc" }, { 0x31435648, "hevc" },
      { 0x30395056, "vp9" }, { 0x30385056, "vp8" }, { 0x31305641, "av1" },
      { 0x33564d57, "wmv3" }, { 0x31564d57, "wmv1" }, { 0x32564d57, "wmv2" },
      { 0x31435657, "vc1" }, { 0x5334504d, "mpeg4" }, { 0x5634504d, "mpeg4" },
      { 0x47504a4d, "mjpeg" }, { 0xe06d8026, "mpeg2" }, { 0x8D2FD10B, "vorbis" }, { 0x3247504d, "mpeg2" }, { 0x3147504d, "mpeg1" },
      { 0x1610, "aac" }, { 0xFF, "aac" }, { 0x55, "mp3" }, { 0x50, "mpeg audio" },
      { 0x161, "wma" }, { 0x162, "wma pro" }, { 0x163, "wma lossless" }, { 0x2000, "ac3" },
      { 0xF1AC, "flac" }, { 0x704F, "opus" }, { 0x1, "pcm" }, { 0x3, "pcm float" },
      { 0x6C61, "alac" },
    };
    for (int i = 0; i < FM_COUNT(kNames); i++)
      if (g.Data1 == kNames[i].id) { fm_strlcpy(out, kNames[i].name, cap); break; }
    if (!out[0]) {
      /* unknown: show the FourCC when it is printable */
      char cc[5] = { (char)g.Data1, (char)(g.Data1 >> 8), (char)(g.Data1 >> 16), (char)(g.Data1 >> 24), 0 };
      bool ok = true;
      for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) ok = false;
      fm_strlcpy(out, ok ? cc : "?", cap);
    }
  }
  sr(t);
}

static bool set_video_type(MfVid *m, const GUID *sub) {
  IMFMediaType *t = NULL;
  if (FAILED(mf.create_media_type(&t))) return false;
  IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
  HRESULT hr = IMFSourceReader_SetCurrentMediaType(m->rd, m->vs, NULL, t);
  sr(t);
  return SUCCEEDED(hr);
}

/* Reads the negotiated video type: pixel format, frame size, aperture. */
static bool read_video_type(MfVid *m, FmVidInfo *info) {
  IMFMediaType *t = NULL;
  if (FAILED(IMFSourceReader_GetCurrentMediaType(m->rd, m->vs, &t))) return false;
  GUID g;
  m->pix = PIX_NONE;
  if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &g))) {
    if (IsEqualGUID(&g, &MFVideoFormat_NV12)) m->pix = PIX_NV12;
    else if (IsEqualGUID(&g, &MFVideoFormat_I420) || IsEqualGUID(&g, &MFVideoFormat_IYUV)) m->pix = PIX_I420;
    else if (IsEqualGUID(&g, &MFVideoFormat_YV12)) m->pix = PIX_YV12;
    else if (IsEqualGUID(&g, &MFVideoFormat_RGB32)) m->pix = PIX_RGB32;
  }
  UINT32 w = 0, h = 0;
  attr_pair((IMFAttributes *)t, &MF_MT_FRAME_SIZE, &w, &h);
  m->fw = (int)w;
  m->fh = (int)h;
  m->dx = m->dy = 0;
  m->dw = m->fw;
  m->dh = m->fh;
  MFVideoArea area;
  UINT32 got = 0;
  if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, &got)) &&
      got == sizeof area && area.Area.cx > 0 && area.Area.cy > 0) {
    m->dx = area.OffsetX.value;
    m->dy = area.OffsetY.value;
    m->dw = (int)area.Area.cx;
    m->dh = (int)area.Area.cy;
    if (m->dx < 0 || m->dy < 0 || m->dx + m->dw > m->fw || m->dy + m->dh > m->fh) {
      m->dx = m->dy = 0;
      m->dw = m->fw;
      m->dh = m->fh;
    }
  }
  m->dx &= ~1;               /* keep chroma aligned */
  m->dy &= ~1;
  m->def_stride = (LONG)attr_u32((IMFAttributes *)t, &MF_MT_DEFAULT_STRIDE, 0);
  if (info) {
    info->w = m->dw;
    info->h = m->dh;
    UINT32 n, d;
    if (attr_pair((IMFAttributes *)t, &MF_MT_PIXEL_ASPECT_RATIO, &n, &d) && n && d)
      info->sar = (double)n / (double)d;
    if (attr_pair((IMFAttributes *)t, &MF_MT_FRAME_RATE, &n, &d) && n && d)
      info->fps = (double)n / (double)d;
  }
  sr(t);
  return m->pix != PIX_NONE && m->fw > 0 && m->fh > 0;
}

static bool setup_video(MfVid *m, FmVidInfo *info) {
  static const GUID *const kTry[] = { &MFVideoFormat_NV12, &MFVideoFormat_I420, &MFVideoFormat_IYUV,
                                      &MFVideoFormat_YV12, &MFVideoFormat_RGB32 };
  for (int i = 0; i < FM_COUNT(kTry); i++)
    if (set_video_type(m, kTry[i]) && read_video_type(m, info)) return true;
  return false;
}

static bool setup_audio(MfVid *m, FmVidInfo *info) {
  for (int pass = 0; pass < 2; pass++) {
    IMFMediaType *t = NULL;
    if (FAILED(mf.create_media_type(&t))) return false;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    if (pass == 0) IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, 2);   /* ask for a downmix */
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(m->rd, m->as, NULL, t);
    sr(t);
    if (FAILED(hr)) continue;
    IMFMediaType *cur = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(m->rd, m->as, &cur))) return false;
    GUID g;
    bool flt = SUCCEEDED(IMFMediaType_GetGUID(cur, &MF_MT_SUBTYPE, &g)) && IsEqualGUID(&g, &MFAudioFormat_Float);
    m->ach = (int)attr_u32((IMFAttributes *)cur, &MF_MT_AUDIO_NUM_CHANNELS, 0);
    m->arate = (int)attr_u32((IMFAttributes *)cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    sr(cur);
    if (flt && m->ach > 0 && m->arate > 0) {
      info->rate = m->arate;
      info->channels = m->ach > 2 ? 2 : m->ach;
      return true;
    }
  }
  return false;
}

/* ---- open / close --------------------------------------------------------- */

/* The first video and first audio stream, by their real index. */
static void real_indexes(MfVid *m) {
  m->vi = m->ai = NO_STREAM;
  for (DWORD i = 0; i < 64 && (m->vi == NO_STREAM || m->ai == NO_STREAM); i++) {
    IMFMediaType *t = NULL;
    HRESULT hr = IMFSourceReader_GetNativeMediaType(m->rd, i, 0, &t);
    if (hr == MF_E_INVALIDSTREAMNUMBER) break;
    if (FAILED(hr)) continue;
    GUID g;
    if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_MAJOR_TYPE, &g))) {
      if (IsEqualGUID(&g, &MFMediaType_Video) && m->vi == NO_STREAM) m->vi = i;
      else if (IsEqualGUID(&g, &MFMediaType_Audio) && m->ai == NO_STREAM) m->ai = i;
    }
    sr(t);
  }
}

static void mf_close(void *st) {
  MfVid *m = (MfVid *)st;
  if (!m) return;
  sr(m->rd);
  if (m->started) mf.shutdown();
  if (m->com_init) CoUninitialize();
  for (int i = 0; i < 3; i++) fm_free(m->plane[i]);
  fm_free(m->pcm);
  fm_free(m->path);
  fm_free(m);
}

static IMFSourceReader *make_reader(const wchar_t *url, bool advanced) {
  IMFAttributes *a = NULL;
  IMFSourceReader *rd = NULL;
  if (FAILED(mf.create_attributes(&a, 2))) return NULL;
  IMFAttributes_SetUINT32(a, advanced ? &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
                                      : &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  if (FAILED(mf.create_reader(url, a, &rd))) rd = NULL;
  sr(a);
  return rd;
}

static void *mf_open(const char *path, int flags, FmVidInfo *info) {
  if (!mf_load()) return NULL;
  MfVid *m = (MfVid *)fm_calloc(1, sizeof *m);
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  m->com_init = SUCCEEDED(hr);           /* RPC_E_CHANGED_MODE: COM is already up */
  if (FAILED(mf.startup(MF_VERSION_WIN7, 0))) { mf_close(m); return NULL; }
  m->started = true;

  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (n <= 0) { mf_close(m); return NULL; }
  wchar_t *url = (wchar_t *)fm_alloc((size_t)n * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, path, -1, url, n);
  /* Windows 8+: the advanced processor converts to NV12/I420; Windows 7 only
  ** knows the basic one (RGB32 output) */
  m->rd = make_reader(url, true);
  if (!m->rd) m->rd = make_reader(url, false);
  fm_free(url);
  if (!m->rd) { mf_close(m); return NULL; }

  IMFSourceReader_SetStreamSelection(m->rd, MF_SOURCE_READER_ALL_STREAMS, FALSE);
  m->vs = m->as = NO_STREAM;
  if (!(flags & VID_OPEN_AUDIO_ONLY)) {
    m->vs = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    if (FAILED(IMFSourceReader_SetStreamSelection(m->rd, m->vs, TRUE)) || !setup_video(m, info)) {
      IMFSourceReader_SetStreamSelection(m->rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
      m->vs = NO_STREAM;
    }
  }
  if (!(flags & VID_OPEN_NO_AUDIO)) {
    m->as = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    if (FAILED(IMFSourceReader_SetStreamSelection(m->rd, m->as, TRUE)) || !setup_audio(m, info)) {
      IMFSourceReader_SetStreamSelection(m->rd, MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
      m->as = NO_STREAM;
    }
  }
  if (m->vs == NO_STREAM && m->as == NO_STREAM) { mf_close(m); return NULL; }
  real_indexes(m);
  info->has_video = m->vs != NO_STREAM;
  info->has_audio = m->as != NO_STREAM;
  m->vdone = !info->has_video;
  m->adone = !info->has_audio;
  if (!info->sar) info->sar = 1.0;

  PROPVARIANT pv;
  PropVariantInit(&pv);
  if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(m->rd, MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv)) &&
      pv.vt == VT_UI8)
    info->duration = (double)pv.uhVal.QuadPart / 1e7;
  PropVariantClear(&pv);

  /* MPEG-TS sources report no seeking: mf_seek then reopens and skips */
  PropVariantInit(&pv);
  m->can_seek = SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
                    m->rd, MF_SOURCE_READER_MEDIASOURCE, &MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &pv)) &&
                pv.vt == VT_UI4 && (pv.ulVal & MFMEDIASOURCE_CAN_SEEK);
  PropVariantClear(&pv);
  m->path = fm_strdup(path);
  m->flags = flags;
  fm_strlcpy(info->backend, "Media Foundation", sizeof info->backend);
  if (info->has_video) codec_name(m->rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, info->vcodec, sizeof info->vcodec);
  if (info->has_audio) codec_name(m->rd, MF_SOURCE_READER_FIRST_AUDIO_STREAM, info->acodec, sizeof info->acodec);
  return m;
}

/* ---- frames --------------------------------------------------------------- */

static void ensure_planes(MfVid *m, int w, int h) {
  if (m->plane[0] && m->pw == w && m->ph == h) return;
  for (int i = 0; i < 3; i++) fm_free(m->plane[i]);
  int cw = (w + 1) / 2, ch = (h + 1) / 2;
  m->plane[0] = (u8 *)fm_alloc((size_t)w * h);
  m->plane[1] = (u8 *)fm_alloc((size_t)cw * ch);
  m->plane[2] = (u8 *)fm_alloc((size_t)cw * ch);
  m->pstride[0] = w;
  m->pstride[1] = m->pstride[2] = cw;
  m->pw = w;
  m->ph = h;
}

static void copy_rows(u8 *dst, int dstride, const u8 *src, int sstride, int w, int h) {
  for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * dstride, src + (size_t)y * sstride, (size_t)w);
}

/* BT.601 limited range, for the rare RGB32 path (Windows 7 processors) */
static void rgb32_to_i420(MfVid *m, const u8 *src, int stride, bool bottom_up) {
  int w = m->dw, h = m->dh;
  for (int y = 0; y < h; y++) {
    int sy = bottom_up ? m->fh - 1 - (m->dy + y) : m->dy + y;
    const u8 *s = src + (size_t)sy * stride + (size_t)m->dx * 4;
    u8 *Y = m->plane[0] + (size_t)y * m->pstride[0];
    for (int x = 0; x < w; x++) {
      int b = s[4 * x], g = s[4 * x + 1], r = s[4 * x + 2];
      Y[x] = (u8)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
      if (!(y & 1) && !(x & 1)) {
        u8 *U = m->plane[1] + (size_t)(y / 2) * m->pstride[1];
        u8 *V = m->plane[2] + (size_t)(y / 2) * m->pstride[2];
        U[x / 2] = (u8)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
        V[x / 2] = (u8)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
      }
    }
  }
}

static bool emit_video(MfVid *m, IMFSample *s, LONGLONG t100, FmVidFrame *vf) {
  IMFMediaBuffer *buf = NULL;
  if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return false;
  IMF2DBuffer *b2 = NULL;
  BYTE *data = NULL;
  LONG pitch = 0;
  DWORD len = 0;
  bool locked2d = false;
  if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &FM_IID_IMF2DBuffer, (void **)&b2)) &&
      SUCCEEDED(IMF2DBuffer_Lock2D(b2, &data, &pitch)))
    locked2d = true;
  else if (FAILED(IMFMediaBuffer_Lock(buf, &data, NULL, &len))) {
    sr(b2);
    sr(buf);
    return false;
  }
  bool bottom_up = false;
  if (!locked2d) {
    pitch = m->def_stride ? m->def_stride : (m->pix == PIX_RGB32 ? m->fw * 4 : m->fw);
  }
  if (pitch < 0) {                       /* bottom-up RGB */
    bottom_up = !locked2d;
    if (locked2d) {                      /* Lock2D already points at the top row */
      pitch = -pitch;
      data -= (size_t)pitch * (m->fh - 1);
      bottom_up = true;
    } else {
      pitch = -pitch;
    }
  }
  int w = m->dw & ~1, h = m->dh & ~1;
  bool ok = w > 0 && h > 0 && pitch > 0;
  if (ok) {
    size_t need = m->pix == PIX_RGB32 ? (size_t)pitch * m->fh : (size_t)pitch * m->fh * 3 / 2;
    if (!locked2d && len && len < need) ok = false;      /* a short buffer: skip, never overread */
  }
  if (ok) {
    ensure_planes(m, w, h);
    int cw = w / 2, ch = h / 2, cx = m->dx / 2, cy = m->dy / 2;
    const u8 *Y = data + (size_t)m->dy * pitch + m->dx;
    if (m->pix == PIX_NV12) {
      const u8 *UV = data + (size_t)pitch * m->fh;
      copy_rows(m->plane[0], m->pstride[0], Y, pitch, w, h);
      vid_nv12_split(UV + (size_t)cy * pitch + (size_t)cx * 2, pitch, cw, ch, m->plane[1], m->pstride[1],
                     m->plane[2], m->pstride[2]);
    } else if (m->pix == PIX_I420 || m->pix == PIX_YV12) {
      int cp = pitch / 2;
      const u8 *P1 = data + (size_t)pitch * m->fh;
      const u8 *P2 = P1 + (size_t)cp * (m->fh / 2);
      const u8 *U = m->pix == PIX_I420 ? P1 : P2, *V = m->pix == PIX_I420 ? P2 : P1;
      copy_rows(m->plane[0], m->pstride[0], Y, pitch, w, h);
      copy_rows(m->plane[1], m->pstride[1], U + (size_t)cy * cp + cx, cp, cw, ch);
      copy_rows(m->plane[2], m->pstride[2], V + (size_t)cy * cp + cx, cp, cw, ch);
    } else {
      int save_w = m->dw, save_h = m->dh;
      m->dw = w;
      m->dh = h;
      rgb32_to_i420(m, data, pitch, bottom_up);
      m->dw = save_w;
      m->dh = save_h;
    }
    vf->t = (double)t100 / 1e7;
    vf->w = w;
    vf->h = h;
    for (int i = 0; i < 3; i++) {
      vf->plane[i] = m->plane[i];
      vf->stride[i] = m->pstride[i];
    }
  }
  if (locked2d) IMF2DBuffer_Unlock2D(b2);
  else IMFMediaBuffer_Unlock(buf);
  sr(b2);
  sr(buf);
  return ok;
}

static bool emit_audio(MfVid *m, IMFSample *s, LONGLONG t100, FmVidPcm *pc) {
  IMFMediaBuffer *buf = NULL;
  if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return false;
  BYTE *data = NULL;
  DWORD len = 0;
  if (FAILED(IMFMediaBuffer_Lock(buf, &data, NULL, &len))) { sr(buf); return false; }
  int ch = m->ach, out_ch = ch > 2 ? 2 : ch;
  int frames = (int)(len / (sizeof(float) * (size_t)ch));
  if (frames > 0) {
    int need = frames * out_ch;
    if (need > m->pcm_cap) {
      fm_free(m->pcm);
      m->pcm_cap = need;
      m->pcm = (float *)fm_alloc((size_t)need * sizeof(float));
    }
    const float *src = (const float *)data;
    if (ch <= 2) {
      memcpy(m->pcm, src, (size_t)need * sizeof(float));
    } else {
      /* L = FL + 0.7 C + 0.5 rest-left, R likewise (WAVE channel order) */
      for (int i = 0; i < frames; i++) {
        const float *f = src + (size_t)i * ch;
        float c = ch > 2 ? f[2] * 0.7071f : 0;
        float l = f[0] + c, r = f[1] + c;
        for (int k = 4; k < ch; k++) { if (k & 1) r += f[k] * 0.5f; else l += f[k] * 0.5f; }
        m->pcm[2 * i] = l * 0.6f;
        m->pcm[2 * i + 1] = r * 0.6f;
      }
    }
    pc->t = (double)t100 / 1e7;
    pc->frames = frames;
    pc->channels = out_ch;
    pc->rate = m->arate;
    pc->pcm = m->pcm;
  }
  IMFMediaBuffer_Unlock(buf);
  sr(buf);
  return frames > 0;
}

static int mf_decode(void *st, FmVidFrame *vf, FmVidPcm *pc) {
  MfVid *m = (MfVid *)st;
  for (int guard = 0; guard < 4096; guard++) {
    if (m->vdone && m->adone) return VID_EV_END;
    DWORD idx = 0, fl = 0;
    LONGLONG t = 0;
    IMFSample *s = NULL;
    HRESULT hr = IMFSourceReader_ReadSample(m->rd, MF_SOURCE_READER_ANY_STREAM, 0, &idx, &fl, &t, &s);
    if (FAILED(hr)) return VID_EV_ERROR;
    bool is_v = m->vs != NO_STREAM && !m->vdone && idx == m->vi;
    bool is_a = m->as != NO_STREAM && !m->adone && idx == m->ai;
    if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      if (is_v && !read_video_type(m, NULL)) { sr(s); return VID_EV_ERROR; }
    }
    if (fl & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      if (is_v) m->vdone = true;
      if (is_a) m->adone = true;
      if (!is_v && !is_a) { m->vdone = m->adone = true; }
    }
    if (!s) continue;                    /* gap / tick / end */
    if (m->skip_until > 0 && (double)t / 1e7 < m->skip_until - 0.02) { sr(s); continue; }
    if (is_v) m->skip_until = 0;         /* reached the target */
    bool got = false;
    if (is_v) got = emit_video(m, s, t, vf);
    else if (is_a) got = emit_audio(m, s, t, pc);
    sr(s);
    if (got) return is_v ? VID_EV_VIDEO : VID_EV_AUDIO;
  }
  return VID_EV_ERROR;
}

static const GUID kTimeFormat100ns;     /* GUID_NULL: 100 ns units */

static bool mf_seek(void *st, double t) {
  MfVid *m = (MfVid *)st;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  pv.vt = VT_I8;
  pv.hVal.QuadPart = (LONGLONG)(t * 1e7);
  HRESULT hr = m->can_seek ? IMFSourceReader_SetCurrentPosition(m->rd, &kTimeFormat100ns, &pv) : E_FAIL;
  PropVariantClear(&pv);
  m->skip_until = 0;
  if (FAILED(hr)) {
    /* not seekable (MPEG-TS and some streams): start over, then drop
    ** everything before t; decoding is fast, drawing is skipped */
    FmVidInfo tmp;
    memset(&tmp, 0, sizeof tmp);
    MfVid *n = m->path ? (MfVid *)mf_open(m->path, m->flags, &tmp) : NULL;
    if (!n) return false;
    IMFSourceReader *old = m->rd;
    m->rd = n->rd;
    n->rd = old;
    m->vi = n->vi;
    m->ai = n->ai;
    m->pix = n->pix;
    mf_close(n);
    m->skip_until = t;
  }
  m->vdone = m->vs == NO_STREAM;
  m->adone = m->as == NO_STREAM;
  return true;
}

const FmVidBackend g_vid_mf = { "Media Foundation", mf_open, mf_decode, mf_seek, mf_close };

#endif
