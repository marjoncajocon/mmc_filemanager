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
#include "fnetstream.h"
#include "fsdl.h"
#include "fplat.h"

/* ---- loading -------------------------------------------------------------- */

typedef HRESULT (WINAPI *MFStartupFn)(ULONG, DWORD);
typedef HRESULT (WINAPI *MFShutdownFn)(void);
typedef HRESULT (WINAPI *MFCreateAttributesFn)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *MFCreateMediaTypeFn)(IMFMediaType **);
typedef HRESULT (WINAPI *MFCreateReaderFn)(LPCWSTR, IMFAttributes *, IMFSourceReader **);
typedef HRESULT (WINAPI *MFCreateReaderBsFn)(IMFByteStream *, IMFAttributes *, IMFSourceReader **);
typedef HRESULT (WINAPI *MFCreateAsyncResultFn)(IUnknown *, IMFAsyncCallback *, IUnknown *, IMFAsyncResult **);
typedef HRESULT (WINAPI *MFInvokeCallbackFn)(IMFAsyncResult *);

static struct {
  int state;                 /* 0 untried, 1 ok, -1 missing */
  MFStartupFn startup;
  MFShutdownFn shutdown;
  MFCreateAttributesFn create_attributes;
  MFCreateMediaTypeFn create_media_type;
  MFCreateReaderFn create_reader;
  MFCreateReaderBsFn create_reader_bs;     /* streaming through our byte stream */
  MFCreateAsyncResultFn create_async;
  MFInvokeCallbackFn invoke;
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
      mf.create_reader_bs =
          (MFCreateReaderBsFn)(void (*)(void))GetProcAddress(rw, "MFCreateSourceReaderFromByteStream");
      mf.create_async = (MFCreateAsyncResultFn)(void (*)(void))GetProcAddress(plat, "MFCreateAsyncResult");
      mf.invoke = (MFInvokeCallbackFn)(void (*)(void))GetProcAddress(plat, "MFInvokeCallback");
    }
    mf.state = (mf.startup && mf.shutdown && mf.create_attributes && mf.create_media_type &&
                mf.create_reader) ? 1 : -1;
  }
  SDL_AtomicUnlock(&g_mf_lock);
  return mf.state > 0;
}

/* IMF2DBuffer gives the real row pitch; declared here so no uuid lib is needed. */
DEFINE_GUID(FM_IID_IMF2DBuffer, 0x7dc9d5f9, 0x9ed9, 0x44ec, 0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb);

/* ---- streaming: an IMFByteStream over FmNetStream ------------------------------ */

/* Media Foundation's own HTTP stack gets throttled by some hosts and cannot
** take our headers; reading through FmNetStream (ring buffer, range seeks)
** lets the source reader play while the data arrives.
** BeginRead must not block: it runs on Media Foundation's shared work-queue
** threads, and a network wait there starved the other reader of a
** video+audio pair. So each byte stream has its own reader thread; BeginRead
** queues the request and returns, the thread reads and invokes the callback. */

typedef struct NsBS {
  IMFByteStreamVtbl *lpVtbl;
  LONG ref;
  FmNetStream *ns;
  IMFAttributes *attrs;      /* content type + origin name: the URL has no extension */
  bool eof;
  /* async reads */
  SDL_Thread *thr;
  SDL_mutex *mx;
  SDL_cond *cv;
  bool quit;
  bool busy;                 /* the reader thread is inside a read or its callback */
  /* queued reads, served strictly in order: Media Foundation does overlap
  ** them, and reading two at once interleaved the bytes (corrupt stream) */
  struct { BYTE *buf; ULONG cb; IMFAsyncResult *res; QWORD pos; } q[8];
  int qhead, qcount;
  /* Media Foundation's view of the position: each read starts where the
  ** previous one ended *when it was issued*, even while earlier reads are
  ** still queued, and SetCurrentPosition may come in between. Reading from
  ** whatever FmNetStream happened to be at fed it the wrong bytes. */
  QWORD cursor;
  /* live streams cannot rewind, but Media Foundation's AAC/MP3 sources probe
  ** the first bytes and seek back (MF_E_BYTESTREAM_NOT_SEEKABLE otherwise):
  ** the last HIST (TS: HIST_TS) bytes read are kept so such seeks are served
  ** from memory */
  u8 *hist;
  size_t hist_len, hist_cap;
  QWORD hist_start;          /* stream position of hist[0] */
  QWORD live_pos;            /* how far the live stream has been read */
  /* live TS while the source opens (see HIST_TS): past what has arrived the
  ** probe is answered with null packets instead of waiting for the broadcast
  ** to produce 3 MiB (a minute and more at low bitrates); synth_from = where
  ** that started (0 = not yet). The real bytes come after its rewind. */
  bool probing;
  QWORD synth_from;
  double id3_t;              /* live packed audio: when its first sample plays (s), 0 = unknown */
} NsBS;

#define HIST (512u * 1024u)
/* the MPEG-TS source reads 3 MiB ahead while it opens, then goes back to
** the start (measured on YouTube live, 2026-10-09: any bitrate) */
#define HIST_TS (4096u * 1024u)
#define PROBE_REAL (192u * 1024u)    /* real bytes the probe sees at least (a first segment) */

typedef struct ReadRes {
  IUnknownVtbl *lpVtbl;
  LONG ref;
  ULONG got;
} ReadRes;

static HRESULT STDMETHODCALLTYPE rr_qi(IUnknown *u, REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown)) { *out = u; u->lpVtbl->AddRef(u); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE rr_addref(IUnknown *u) { return (ULONG)InterlockedIncrement(&((ReadRes *)u)->ref); }
static ULONG STDMETHODCALLTYPE rr_release(IUnknown *u) {
  ReadRes *r = (ReadRes *)u;
  LONG n = InterlockedDecrement(&r->ref);
  if (!n) fm_free(r);
  return (ULONG)n;
}
static IUnknownVtbl g_rr_vtbl = { rr_qi, rr_addref, rr_release };

#define BS(p) ((NsBS *)(p))

static HRESULT STDMETHODCALLTYPE bs_qi(IMFByteStream *p, REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMFByteStream)) {
    *out = p;
    InterlockedIncrement(&BS(p)->ref);
    return S_OK;
  }
  if (IsEqualIID(iid, &IID_IMFAttributes) && BS(p)->attrs)
    return IMFAttributes_QueryInterface(BS(p)->attrs, iid, out);
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bs_addref(IMFByteStream *p) { return (ULONG)InterlockedIncrement(&BS(p)->ref); }
static ULONG STDMETHODCALLTYPE bs_release(IMFByteStream *p) {
  NsBS *b = BS(p);
  LONG n = InterlockedDecrement(&b->ref);
  if (!n) {
    if (b->thr) {
      SDL_LockMutex(b->mx);
      b->quit = true;
      SDL_CondBroadcast(b->cv);
      SDL_UnlockMutex(b->mx);
      SDL_WaitThread(b->thr, NULL);
    }
    for (int i = 0; i < b->qcount; i++) IMFAsyncResult_Release(b->q[(b->qhead + i) % 8].res);
    if (b->cv) SDL_DestroyCond(b->cv);
    if (b->mx) SDL_DestroyMutex(b->mx);
    if (b->attrs) IMFAttributes_Release(b->attrs);
    ns_close(b->ns);
    fm_free(b->hist);
    fm_free(b);
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE bs_caps(IMFByteStream *p, DWORD *caps) {
  /* live streams seek inside the rewind window (see NsBS.hist) */
  *caps = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
  FM_UNUSED(p);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_getlen(IMFByteStream *p, QWORD *len) {
  i64 n = ns_size(BS(p)->ns);
  *len = n >= 0 ? (QWORD)n : (QWORD)-1;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_setlen(IMFByteStream *p, QWORD len) { FM_UNUSED(p); FM_UNUSED(len); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bs_getpos(IMFByteStream *p, QWORD *pos) {
  SDL_LockMutex(BS(p)->mx);
  *pos = BS(p)->cursor;
  SDL_UnlockMutex(BS(p)->mx);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_setpos(IMFByteStream *p, QWORD pos) {
  i64 size = ns_size(BS(p)->ns);
  if (size >= 0 && (i64)pos > size) return E_INVALIDARG;
  if (BS(p)->hist && pos < BS(p)->hist_start) return E_FAIL;   /* fell out of the window */
  SDL_LockMutex(BS(p)->mx);
  if (BS(p)->probing && pos + (1u << 20) < BS(p)->cursor) BS(p)->probing = false;   /* the probe's rewind */
  BS(p)->cursor = pos;                        /* reads seek there when they run */
  BS(p)->eof = false;
  SDL_UnlockMutex(BS(p)->mx);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_iseos(IMFByteStream *p, BOOL *eos) { *eos = BS(p)->eof; return S_OK; }
/* MPEG-TS null packets (PID 0x1FFF, ignored by demuxers) for the stream
** positions at..at+n, on the 188-byte grid of the stream. */
static void ts_null_fill(QWORD at, BYTE *buf, ULONG n) {
  static const u8 kHead[4] = { 0x47, 0x1F, 0xFF, 0x10 };
  for (ULONG i = 0; i < n; i++) {
    unsigned o = (unsigned)((at + i) % 188);
    buf[i] = o < 4 ? kHead[o] : 0xFF;
  }
}

/* Live: reads cb bytes at `at` through the rewind window. */
static ULONG read_live(NsBS *b, QWORD at, BYTE *buf, ULONG cb) {
  if (at < b->hist_start) return 0;
  ULONG done = 0;
  while (done < cb) {
    QWORD p = at + done;
    if (p < b->hist_start + b->hist_len) {            /* already read: from memory */
      size_t off = (size_t)(p - b->hist_start);
      size_t take = FM_MIN((size_t)(cb - done), b->hist_len - off);
      memcpy(buf + done, b->hist + off, take);
      done += (ULONG)take;
      continue;
    }
    /* the window never grows past synth_from while probing: once the probe
    ** is ahead of the broadcast it stays on null packets until its rewind */
    if (b->probing && (b->synth_from || (b->hist_len >= PROBE_REAL && !ns_buffered(b->ns)))) {
      if (!b->synth_from) b->synth_from = p;
      ts_null_fill(p, buf + done, cb - done);
      done = cb;
      break;
    }
    /* read on from the live position into the window, then copy */
    u8 tmp[16384];
    size_t got = ns_read(b->ns, tmp, sizeof tmp);
    if (!got) break;
    if (b->hist_len + got > b->hist_cap) {            /* slide: drop the oldest */
      size_t drop = b->hist_len + got - b->hist_cap;
      memmove(b->hist, b->hist + drop, b->hist_len - drop);
      b->hist_len -= drop;
      b->hist_start += drop;
    }
    memcpy(b->hist + b->hist_len, tmp, got);
    b->hist_len += got;
    b->live_pos += got;
  }
  return done;
}

/* Reads cb bytes at `at` (only the reader thread or Read calls this). */
static ULONG read_at(NsBS *b, QWORD at, BYTE *buf, ULONG cb) {
  if (b->hist) return read_live(b, at, buf, cb);
  if (ns_tell(b->ns) != (i64)at && !ns_seek(b->ns, (i64)at)) return 0;
  size_t n = 0, r;
  while (n < cb && (r = ns_read(b->ns, buf + n, cb - n)) > 0) n += r;
  return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE bs_read(IMFByteStream *p, BYTE *buf, ULONG cb, ULONG *got) {
  NsBS *b = BS(p);
  SDL_LockMutex(b->mx);
  QWORD at = b->cursor;
  b->cursor += cb;
  SDL_UnlockMutex(b->mx);
  *got = read_at(b, at, buf, cb);
  SDL_LockMutex(b->mx);
  if (*got < cb) { b->eof = true; b->cursor = at + *got; }
  SDL_UnlockMutex(b->mx);
  return S_OK;
}
/* The byte stream's reader thread: one queued read at a time. */
static int bs_reader(void *u) {
  NsBS *b = (NsBS *)u;
  SDL_LockMutex(b->mx);
  while (!b->quit) {
    if (!b->qcount) { SDL_CondWait(b->cv, b->mx); continue; }
    IMFAsyncResult *res = b->q[b->qhead].res;
    BYTE *buf = b->q[b->qhead].buf;
    ULONG cb = b->q[b->qhead].cb;
    QWORD at = b->q[b->qhead].pos;
    b->qhead = (b->qhead + 1) % 8;
    b->qcount--;
    b->busy = true;
    SDL_UnlockMutex(b->mx);
    IUnknown *u2 = NULL;
    ULONG got = read_at(b, at, buf, cb);
    if (got < cb) {
      SDL_LockMutex(b->mx);
      b->eof = true;
      if (b->cursor == at + cb) b->cursor = at + got;   /* nothing was issued after it */
      SDL_UnlockMutex(b->mx);
    }
    if (SUCCEEDED(IMFAsyncResult_GetObject(res, &u2)) && u2) {
      ((ReadRes *)u2)->got = got;
      u2->lpVtbl->Release(u2);
    }
    IMFAsyncResult_SetStatus(res, S_OK);
    mf.invoke(res);
    IMFAsyncResult_Release(res);
    SDL_LockMutex(b->mx);
    b->busy = false;
    SDL_CondBroadcast(b->cv);                 /* bs_quiesce may be waiting */
  }
  SDL_UnlockMutex(b->mx);
  return 0;
}

static HRESULT STDMETHODCALLTYPE bs_beginread(IMFByteStream *p, BYTE *buf, ULONG cb, IMFAsyncCallback *cbk,
                                              IUnknown *state) {
  NsBS *b = BS(p);
  ReadRes *rr = (ReadRes *)fm_calloc(1, sizeof *rr);
  rr->lpVtbl = &g_rr_vtbl;
  rr->ref = 1;
  IMFAsyncResult *res = NULL;
  HRESULT hr = mf.create_async((IUnknown *)rr, cbk, state, &res);
  rr_release((IUnknown *)rr);                 /* the result holds it now */
  if (FAILED(hr)) return hr;
  SDL_LockMutex(b->mx);
  if (b->qcount == 8) {                       /* far more than Media Foundation uses */
    SDL_UnlockMutex(b->mx);
    IMFAsyncResult_Release(res);
    return E_FAIL;
  }
  int slot = (b->qhead + b->qcount) % 8;
  b->q[slot].buf = buf;
  b->q[slot].cb = cb;
  b->q[slot].res = res;                       /* the reader thread releases it */
  b->q[slot].pos = b->cursor;                 /* where this read starts */
  b->cursor += cb;
  b->qcount++;
  SDL_CondSignal(b->cv);
  SDL_UnlockMutex(b->mx);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bs_endread(IMFByteStream *p, IMFAsyncResult *res, ULONG *got) {
  FM_UNUSED(p);
  IUnknown *u = NULL;
  *got = 0;
  if (SUCCEEDED(IMFAsyncResult_GetObject(res, &u)) && u) {
    *got = ((ReadRes *)u)->got;
    u->lpVtbl->Release(u);
  }
  return IMFAsyncResult_GetStatus(res);
}
static HRESULT STDMETHODCALLTYPE bs_write(IMFByteStream *p, const BYTE *b, ULONG n, ULONG *w) {
  FM_UNUSED(p); FM_UNUSED(b); FM_UNUSED(n); *w = 0; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE bs_beginwrite(IMFByteStream *p, const BYTE *b, ULONG n, IMFAsyncCallback *c,
                                               IUnknown *st) {
  FM_UNUSED(p); FM_UNUSED(b); FM_UNUSED(n); FM_UNUSED(c); FM_UNUSED(st); return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE bs_endwrite(IMFByteStream *p, IMFAsyncResult *r, ULONG *w) {
  FM_UNUSED(p); FM_UNUSED(r); *w = 0; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE bs_seek(IMFByteStream *p, MFBYTESTREAM_SEEK_ORIGIN o, LONGLONG off, DWORD flags,
                                         QWORD *cur) {
  FM_UNUSED(flags);
  QWORD now;
  bs_getpos(p, &now);
  i64 to = o == msoBegin ? off : (i64)now + off;
  HRESULT hr = to < 0 ? E_INVALIDARG : bs_setpos(p, (QWORD)to);
  if (cur) bs_getpos(p, cur);
  return hr;
}
static HRESULT STDMETHODCALLTYPE bs_flush(IMFByteStream *p) { FM_UNUSED(p); return S_OK; }
static HRESULT STDMETHODCALLTYPE bs_close(IMFByteStream *p) { FM_UNUSED(p); return S_OK; }

static IMFByteStreamVtbl g_bs_vtbl = {
  bs_qi, bs_addref, bs_release, bs_caps, bs_getlen, bs_setlen, bs_getpos, bs_setpos, bs_iseos, bs_read,
  bs_beginread, bs_endread, bs_write, bs_beginwrite, bs_endwrite, bs_seek, bs_flush, bs_close
};

/* Before MFShutdown: a read still queued or running would invoke its
** callback into a shut-down Media Foundation (crash in RtwqInvokeCallback;
** likely with HLS, whose reads wait between segments). Abort the network
** (pending reads return 0 at once) and wait until the reader is idle. */
static void bs_quiesce(NsBS *b) {
  ns_abort(b->ns);
  SDL_LockMutex(b->mx);
  for (int i = 0; i < 200 && (b->qcount || b->busy); i++) SDL_CondWaitTimeout(b->cv, b->mx, 25);
  SDL_UnlockMutex(b->mx);
}

/* Live HLS packed audio (raw AAC/MP3 segments) starts each segment with an
** ID3 tag whose PRIV frame "com.apple.streaming.transportStreamTimestamp"
** holds the 90 kHz MPEG-TS time of its first sample. Media Foundation counts
** such audio from 0, while the picture beside it (TS) keeps the broadcast's
** clock (YouTube live: hours); this time puts the sound on the same clock.
** Seconds, 0 = none. */
static double id3_ts_time(FmNetStream *ns) {
  static const char kOwner[] = "com.apple.streaming.transportStreamTimestamp";
  u8 h[2048];
  size_t n = ns_peek(ns, h, 10);
  if (n < 10 || memcmp(h, "ID3", 3) != 0) return 0;
  size_t len = 10 + ((size_t)(h[6] & 0x7f) << 21 | (size_t)(h[7] & 0x7f) << 14 | (size_t)(h[8] & 0x7f) << 7 |
                     (size_t)(h[9] & 0x7f));
  n = ns_peek(ns, h, FM_MIN(len, sizeof h));
  for (size_t i = 10; i + sizeof kOwner + 8 <= n; i++) {
    if (memcmp(h + i, kOwner, sizeof kOwner) != 0) continue;  /* the owner and its 0 */
    const u8 *p = h + i + sizeof kOwner;
    u64 pts = 0;
    for (int k = 0; k < 8; k++) pts = pts << 8 | p[k];
    return (double)(pts & 0x1FFFFFFFFull) / 90000.0;
  }
  return 0;
}

/* A byte stream for an http(s) URL, or NULL (err says why). */
static IMFByteStream *net_bytestream(const char *url, char *err, size_t errcap) {
  if (!mf.create_reader_bs || !mf.create_async || !mf.invoke) return NULL;
  FmNetStream *ns = ns_open(url, NULL, err, errcap);
  if (!ns) return NULL;
  NsBS *b = (NsBS *)fm_calloc(1, sizeof *b);
  b->lpVtbl = &g_bs_vtbl;
  b->ref = 1;
  b->ns = ns;
  if (!ns_seekable(ns)) {                                  /* live: rewind window */
    b->probing = strstr(ns_content_type(ns), "mp2t") != NULL;
    b->hist_cap = b->probing ? HIST_TS : HIST;
    b->hist = (u8 *)fm_alloc(b->hist_cap);
    if (ns_is_hls(ns) && strstr(ns_content_type(ns), "audio/")) b->id3_t = id3_ts_time(ns);
  }
  b->mx = SDL_CreateMutex();
  b->cv = SDL_CreateCond();
  b->thr = b->mx && b->cv ? fm_thread_create(bs_reader, "mf-bytestream", b) : NULL;
  if (!b->thr) {
    bs_release((IMFByteStream *)b);
    return NULL;
  }
  if (SUCCEEDED(mf.create_attributes(&b->attrs, 2))) {
    /* the source resolver picks the container from these */
    const char *ct = ns_content_type(ns);
    char type[96];
    fm_strlcpy(type, ct && *ct ? ct : "video/mp4", sizeof type);
    type[strcspn(type, ";")] = 0;
    /* raw AAC radio (ADTS) is ".aac" to Windows; MP4/M4A boxes are not */
    const wchar_t *origin = strstr(type, "webm") ? L"stream.webm"
                            : strstr(type, "mpeg") && strstr(type, "audio") ? L"stream.mp3"
                            : strstr(type, "aac") ? L"stream.aac"
                            : strstr(type, "mp2t") ? L"stream.ts"
                            : strstr(type, "audio") ? L"stream.m4a" : L"stream.mp4";
    wchar_t wtype[96];
    MultiByteToWideChar(CP_UTF8, 0, type, -1, wtype, 96);
    IMFAttributes_SetString(b->attrs, &MF_BYTESTREAM_CONTENT_TYPE, wtype);
    IMFAttributes_SetString(b->attrs, &MF_BYTESTREAM_ORIGIN_NAME, origin);
  }
  return (IMFByteStream *)b;
}

/* ---- state ---------------------------------------------------------------- */

enum { PIX_NONE, PIX_NV12, PIX_I420, PIX_YV12, PIX_RGB32 };

typedef struct MfVid {
  IMFSourceReader *rd;
  bool com_init, started;
  DWORD com_thread;          /* COM is per thread: undo it only where it was done */
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
  /* URLs: our byte stream (a reference), and HLS playlists (fnetstream joins
  ** the segments): Media Foundation sees no duration and cannot seek them,
  ** so seeking reopens at "<url>#t=<s>" (fnetstream starts at that segment) */
  NsBS *bs;
  bool hls;
  double hls_start;          /* the time byte 0 of this open plays at */
  LONGLONG t_off;            /* added to timestamps: older TS restarts at 0 */
  bool t_checked;
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
  if (m->bs) ns_abort(m->bs->ns);             /* the reader stops waiting on the network */
  sr(m->rd);
  if (m->bs) {
    bs_quiesce(m->bs);                        /* no callback may run after MFShutdown */
    bs_release((IMFByteStream *)m->bs);
  }
  if (m->started) mf.shutdown();
  if (m->com_init && m->com_thread == GetCurrentThreadId()) CoUninitialize();
  for (int i = 0; i < 3; i++) fm_free(m->plane[i]);
  fm_free(m->pcm);
  fm_free(m->path);
  fm_free(m);
}

static IMFSourceReader *make_reader(const wchar_t *url, IMFByteStream *bs, bool advanced) {
  IMFAttributes *a = NULL;
  IMFSourceReader *rd = NULL;
  if (FAILED(mf.create_attributes(&a, 2))) return NULL;
  IMFAttributes_SetUINT32(a, advanced ? &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
                                      : &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  HRESULT hr = bs ? mf.create_reader_bs(bs, a, &rd) : mf.create_reader(url, a, &rd);
  if (FAILED(hr)) {
    if (bs) fm_log("video: Media Foundation refused the stream (0x%08lx)", (unsigned long)hr);
    rd = NULL;
  }
  sr(a);
  return rd;
}

static bool is_url(const char *p) { return !fm_strnicmp(p, "http://", 7) || !fm_strnicmp(p, "https://", 8); }

static void *mf_open(const char *path, int flags, FmVidInfo *info) {
  if (!mf_load()) return NULL;
  MfVid *m = (MfVid *)fm_calloc(1, sizeof *m);
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  m->com_init = SUCCEEDED(hr);           /* RPC_E_CHANGED_MODE: COM is already up */
  m->com_thread = GetCurrentThreadId();
  if (FAILED(mf.startup(MF_VERSION_WIN7, 0))) { mf_close(m); return NULL; }
  m->started = true;

  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (n <= 0) { mf_close(m); return NULL; }
  wchar_t *url = (wchar_t *)fm_alloc((size_t)n * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, path, -1, url, n);
  /* Windows 8+: the advanced processor converts to NV12/I420; Windows 7 only
  ** knows the basic one (RGB32 output) */
  IMFByteStream *bs = NULL;
  if (is_url(path)) {
    char e[160];
    bs = net_bytestream(path, e, sizeof e);
    if (!bs) { fm_log("video: %s: %s", path, e); vid_note_net_error(e); }
  }
  m->rd = make_reader(url, bs, true);
  if (!m->rd) m->rd = make_reader(url, bs, false);
  if (bs && m->rd) m->bs = BS(bs);            /* our reference, for mf_close */
  else if (bs) IMFByteStream_Release(bs);     /* else the reader keeps its own */
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
  if (m->bs && ns_is_hls(m->bs->ns)) {
    m->hls = true;
    m->hls_start = ns_hls_start(m->bs->ns);
    if (info->duration <= 0) info->duration = ns_hls_duration(m->bs->ns);
    if (m->bs->id3_t > 0) {                  /* live packed audio: the broadcast's clock */
      m->t_off = (LONGLONG)(m->bs->id3_t * 1e7);
      m->t_checked = true;
    }
  }

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
  if (m->bs) {                               /* open: playback reads only real bytes */
    SDL_LockMutex(m->bs->mx);
    m->bs->probing = false;
    SDL_UnlockMutex(m->bs->mx);
  }
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
    if (FAILED(hr)) { fm_log("video: Media Foundation read failed (0x%08lx)", (unsigned long)hr); return VID_EV_ERROR; }
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
    if (m->hls && !m->t_checked) {       /* opened at #t=: older TS counts from 0 again */
      m->t_checked = true;
      if (m->hls_start > 5 && (double)t / 1e7 < m->hls_start - 5) m->t_off = (LONGLONG)(m->hls_start * 1e7);
    }
    t += m->t_off;
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
  /* HLS never seeks in place: past the segments reached so far that reads
  ** every one up to t (measured 16 s for 90 s of fMP4), a reopen ~1.5 s */
  HRESULT hr = m->can_seek && !m->hls ? IMFSourceReader_SetCurrentPosition(m->rd, &kTimeFormat100ns, &pv) : E_FAIL;
  PropVariantClear(&pv);
  m->skip_until = 0;
  if (FAILED(hr)) {
    /* not seekable (MPEG-TS and some streams): start over, then drop
    ** everything before t; decoding is fast, drawing is skipped */
    FmVidInfo tmp;
    memset(&tmp, 0, sizeof tmp);
    char *at = NULL;
    if (m->hls && m->path) {                  /* HLS: start at t's segment, not at 0 */
      size_t n0 = strcspn(m->path, "#");
      at = (char *)fm_alloc(n0 + 32);
      memcpy(at, m->path, n0);
      fm_snprintf(at + n0, 32, "#t=%.3f", t > 0 ? t : 0);
    }
    MfVid *n = m->path ? (MfVid *)mf_open(at ? at : m->path, m->flags, &tmp) : NULL;
    fm_free(at);
    if (!n) return false;
    IMFSourceReader *old = m->rd;
    m->rd = n->rd;
    n->rd = old;
    NsBS *ob = m->bs;                         /* the old stream closes with the old reader */
    m->bs = n->bs;
    n->bs = ob;
    m->vi = n->vi;
    m->ai = n->ai;
    m->pix = n->pix;
    m->hls_start = n->hls_start;
    m->t_off = n->t_off;                      /* 0 / unchecked, or live packed audio's clock */
    m->t_checked = n->t_checked;
    mf_close(n);
    m->skip_until = t;
  }
  m->vdone = m->vs == NO_STREAM;
  m->adone = m->as == NO_STREAM;
  return true;
}

const FmVidBackend g_vid_mf = { "Media Foundation", mf_open, mf_decode, mf_seek, mf_close };

#endif
