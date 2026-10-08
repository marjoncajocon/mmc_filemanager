/* fdec_vid_int.h -- the plug-in interface for OS video decoders.
**
** fdec_vid.c tries, in order: pl_mpeg (MPEG-1), FFmpeg (when its libraries
** are found), the OS backend for this platform, then the built-in one:
**   - fdec_vid_mf.c:   Windows Media Foundation (Windows 7+, loaded at run time)
**   - fdec_vid_amc.c:  Android MediaCodec (libmediandk, loaded at run time)
**   - fdec_vid_soft.c: VP9 + Opus in WebM in portable C (every target)
** MMCFM_VIDEO_BACKEND=soft|os|ffmpeg makes vid_open try only that one.
**
** A backend fills FmVidInfo on open and then hands out exactly what the
** public API promises: video as YUV420P planes, audio as interleaved float
** with at most 2 channels. Pointers it returns stay valid until its next
** decode call. Backends must not crash on a file they cannot read; they
** return NULL from open instead.
*/
#ifndef FDEC_VID_INT_H
#define FDEC_VID_INT_H

#include "fdec_vid.h"

typedef struct FmVidBackend {
  const char *name;
  /* NULL when this backend cannot play the file (or the OS lacks it). */
  void *(*open)(const char *path, int flags, FmVidInfo *info);
  int   (*decode)(void *st, FmVidFrame *vf, FmVidPcm *pcm);   /* VID_EV_* */
  bool  (*seek)(void *st, double t);
  void  (*close)(void *st);
} FmVidBackend;

#if defined(FM_WIN) && !defined(__TINYC__)
#  define FM_VID_OS 1
extern const FmVidBackend g_vid_mf;
#  define FM_VID_OS_BACKEND g_vid_mf
#elif defined(FM_ANDROID)
#  define FM_VID_OS 1
extern const FmVidBackend g_vid_amc;
#  define FM_VID_OS_BACKEND g_vid_amc
bool amc_can_stream(void);           /* URLs need Android 9+ */
#endif

/* Built in everywhere: nestegg + libvpx (VP9) + libopus. */
extern const FmVidBackend g_vid_soft;

/* For tests: "soft", "os", "ffmpeg" or NULL (default order, or the
** MMCFM_VIDEO_BACKEND environment variable). */
void vid_force_backend(const char *name);

/* Shared helpers for backends (fdec_vid.c). */
/* NV12 (interleaved UV) -> separate U and V planes. */
void vid_nv12_split(const u8 *uv, int uv_stride, int cw, int ch, u8 *u, int ustride, u8 *v,
                    int vstride);

#endif
