/* fdec_vid.h -- video (and FFmpeg audio) decoding: pl_mpeg built in, FFmpeg
** loaded at run time when present.
**
** One pull interface for both backends: vid_decode returns the next video
** frame (YUV 4:2:0 planes) or the next block of audio (interleaved float),
** whichever the demuxer delivers. No SDL rendering here; the viewer copies
** frames into its own queue and uploads them on the main thread.
**
** Design decisions:
**   - FFmpeg is never linked: its libraries are opened with LoadLibrary /
**     dlopen, so builds and machines without it work, and it is only used
**     for library major versions whose struct layouts we declare (see
**     fdec_vid.c). Anything else falls back to "open with the system app".
**   - Frames are always handed out as YUV420P: native for MPEG-1 and most
**     FFmpeg codecs, converted with swscale only when needed.
*/
#ifndef FDEC_VID_H
#define FDEC_VID_H

#include "fcore.h"
#include "fdec_img.h"

enum { VID_OPEN_AUDIO_ONLY = 1, VID_OPEN_NO_AUDIO = 2 };

typedef struct FmVidInfo {
  bool has_video, has_audio;
  int w, h;              /* display size */
  double sar;            /* pixel aspect ratio (1 = square) */
  double fps;            /* 0 when unknown */
  double duration;       /* seconds, 0 when unknown */
  int rate, channels;    /* audio as delivered by vid_decode (channels <= 2) */
  char backend[24];      /* "MPEG-1" or "FFmpeg 7" */
  char vcodec[24], acodec[24];
} FmVidInfo;

typedef struct FmVidFrame {
  double t;              /* presentation time, seconds */
  int w, h;
  const u8 *plane[3];    /* Y, U (Cb), V (Cr) */
  int stride[3];
} FmVidFrame;

typedef struct FmVidPcm {
  double t;              /* time of the first sample, seconds (< 0 unknown) */
  int frames;
  int channels, rate;
  const float *pcm;      /* interleaved, valid until the next vid_decode */
} FmVidPcm;

enum { VID_EV_ERROR = -1, VID_EV_END = 0, VID_EV_VIDEO = 1, VID_EV_AUDIO = 2 };

typedef struct FmVid FmVid;

FmVid *vid_open(const char *path, int flags, FmErr *err);
const FmVidInfo *vid_info(const FmVid *v);
/* Next decoded item; the pointers inside stay valid until the next call. */
int    vid_decode(FmVid *v, FmVidFrame *vf, FmVidPcm *pcm);
/* Seeks to the key frame at or before t (seconds). */
bool   vid_seek(FmVid *v, double t);
void   vid_close(FmVid *v);

/* First meaningful frame (a little into the clip) as RGBA fitting max_px. */
FmErr  vid_thumb(const char *path, int max_px, FmImage *out);

/* True when the file is an MPEG-1 program stream pl_mpeg can play. */
bool   vid_is_mpeg1(const char *path);

/* FFmpeg loader: loads once (thread safe), true when usable. */
bool   ff_available(void);
const char *ff_version_str(void);    /* "FFmpeg 7 (avformat 61.7.100)" or why not */

#endif
