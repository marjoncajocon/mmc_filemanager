/* fvsrc.h -- online video sources: one adapter struct per website.
**
** An adapter is a table of functions (like FmVidBackend for decoders). The
** gallery, the player hookup and downloads are shared; a new website is one
** new FmVsrc and one line in the registry (fvsrc.c).
**
**   const FmVsrc g_vsrc_mysite = {
**     "mysite", "My Site", IC_VIDEO, VSRC_SEARCH | VSRC_DIRECT,
**     mysite_search, mysite_resolve, NULL, NULL, "Free, no key needed"
**   };
**
** Threading: every function may block (network, processes) and is called on
** a worker thread, never on the UI thread. They must be reentrant: no
** globals except read-only data and the settings snapshot they are given.
** `cancel` is polled; when it becomes nonzero return FM_ERR_CANCEL soon.
**
** Design decisions:
**   - Results are plain structs with fixed-size strings: no ownership rules
**     across threads, one free for a whole page (FmVsrcPage).
**   - Sites that hand out plain media URLs (Internet Archive, PeerTube)
**     resolve to a network URL the player streams at once (Media Foundation
**     and FFmpeg read http(s); measured: archive.org opens in ~5 s, seeks).
**   - Sites that do not (YouTube, Dailymotion, "any site") resolve through
**     yt-dlp. Since the streaming work (2026-10-08 night) one `yt-dlp -J`
**     lists the formats and resolve returns their URLs (local = false), so
**     playback starts once the first frames decode; the same reply fills
**     FmVsrcStream.q[] for the player's quality menu. Only when no format
**     streams (HLS-only sites such as Dailymotion, formats needing request
**     headers the decoders cannot send) or with FmVsrcConf.force_cache (the
**     player's fallback after a stream failed) does yt-dlp download into
**     the cache first and return local files, as it always did before.
**     Older findings that shaped this (2026-10-08, this PC):
**       * YouTube no longer serves one file with picture and sound for most
**         videos; streams come as separate video-only and audio-only files,
**         so FmVsrcStream has `audio` and the player opens them as a pair
**         (vid_open_pair).
**       * Streaming YouTube URLs through Media Foundation directly is
**         throttled to uselessness; yt-dlp downloads 10 min of 360p in ~8 s.
**         (Fixed since: the decoders now read URLs through FmNetStream.)
**       * Windows Media Foundation does not decode YouTube's fragmented
**         H.264 MP4 (DASH); VP9 WebM video + Opus WebM audio work. Without
**         FFmpeg prefer webm; with FFmpeg anything goes.
**       * Current yt-dlp needs a JavaScript runtime (deno, or node via
**         --js-runtimes node:PATH) for YouTube; without one only blocked
**         formats remain (HTTP 403 without a PO token).
**     When yt-dlp (or the runtime) is missing, the UI offers "Open in
**     browser" and explains what to install.
*/
#ifndef FVSRC_H
#define FVSRC_H

#include "fcore.h"
#include "ficon.h"

enum {
  VSRC_SEARCH  = 1 << 0,  /* has search() */
  VSRC_DIRECT  = 1 << 1,  /* resolve() needs no helper program */
  VSRC_YTDLP   = 1 << 2,  /* resolve()/download need yt-dlp */
  VSRC_NEEDKEY = 1 << 3,  /* search needs an API key in the settings */
  VSRC_URL     = 1 << 4,  /* accepts a pasted page URL instead of a query */
};

typedef struct FmVsrcItem {
  char id[64];            /* site-specific id */
  char title[256];
  char channel[128];      /* uploader / creator */
  char thumb[512];        /* thumbnail URL (jpeg/png/webp) */
  char page[512];         /* the item's web page, for "Open in browser" */
  char published[32];     /* ISO 8601 date or "" */
  double duration;        /* seconds, 0 = unknown / live */
  i64 views;              /* -1 = unknown */
  bool live;
} FmVsrcItem;

typedef struct FmVsrcPage {
  FmVsrcItem *items;
  int count, cap;
  char next[256];         /* continuation token for the next page, "" = end */
  char error[256];        /* human-readable reason when the call failed */
} FmVsrcPage;

/* One quality the item comes in (the player's quality menu). Built from the
** same reply that resolved the stream, so listing costs nothing extra. */
#define VSRC_QMAX 12            /* at most 11 picture sizes plus "Audio only" */
#define VSRC_QURL 2048          /* longer stream URLs are left out (re-resolved on demand) */
typedef struct FmVsrcQuality {
  char label[16];         /* "1080p60", "720p", "Audio only" */
  int height;             /* the smaller side of the picture ("res"); 0 = audio only */
  int fps;                /* rounded; 0 = unknown or audio */
  char codec[24];         /* "VP9", "H.264", "AV1", "Opus" ... (the picture's when both) */
  int kbps;               /* approximate total bitrate (picture + sound), 0 = unknown */
  i64 bytes;              /* approximate total size, 0 = unknown */
  bool playable;          /* streams with the current decoders (url is set) */
  bool cache_only;        /* plays, but only after a download into the cache (HLS, headers) */
  bool needs_ffmpeg;      /* exists, but only the FFmpeg libraries decode it */
  bool muxed;             /* url has picture and sound (no separate audio) */
  bool audio_only;
  char url[VSRC_QURL];    /* the stream; with !muxed the sound is FmVsrcStream.audio */
} FmVsrcQuality;

/* What the player opens: one source with picture and sound, or two (video +
** audio) that vid_open_pair() plays together. Network URLs or local files. */
typedef struct FmVsrcStream {
  char video[4096];       /* URL or local path (picture, or everything) */
  char audio[4096];       /* separate sound track, "" = none */
  bool local;             /* both are files (already downloaded to the cache) */
  int width, height;      /* 0 = unknown */
  double duration;
  /* additive (streaming): request lines the site wants with the URLs, as
  ** "Key: value\r\n" (yt-dlp's http_headers; "" = none needed) */
  char headers[1024];
  /* the qualities, best first; cur = the one video/audio play (-1 = none) */
  int nq, cur;
  FmVsrcQuality q[VSRC_QMAX];
  bool live;              /* on air now: no length, no seeking, no saving */
} FmVsrcStream;

/* Progress for long steps (downloads into the cache): frac 0..1, or < 0 when
** unknown; status is a short line ("Downloading video 42%"). false = cancel. */
typedef bool (*FmVsrcProgress)(void *user, float frac, const char *status);

/* Settings an adapter may read (copied for the worker; never the live conf). */
typedef struct FmVsrcConf {
  char api_key_youtube[128];
  char ytdlp[FM_PATH_MAX];      /* resolved yt-dlp executable, "" = none */
  char js_runtime[FM_PATH_MAX]; /* for yt-dlp --js-runtimes: "node:<path>" or "deno:<path>", "" = default */
  char ffmpeg_dir[FM_PATH_MAX]; /* folder with ffmpeg(.exe) for yt-dlp merging, "" = none */
  char cache_dir[FM_PATH_MAX];  /* where yt-dlp sources download before playing */
  char download_dir[FM_PATH_MAX];
  int max_height;               /* preferred stream height: 360, 480, 720, 1080 */
  bool have_ffmpeg_libs;        /* the player has FFmpeg: any container/codec plays */
  char region[8];               /* "US", "" = any */
  bool safe_search;
  /* additive (streaming) */
  bool os_mp4;                  /* the system decoders play progressive MP4 (H.264/AAC) */
  bool send_headers;            /* the player can send FmVsrcStream.headers with video URLs */
  bool force_cache;             /* resolve downloads into the cache (streaming failed) */
  bool fresh;                   /* the last links failed: start a new site session (YouTube:
                                ** a new visitor id; its links can be refused for a while) */
  bool os_dash;                 /* ... and stream fragmented (DASH) MP4 too (MediaCodec); then
                                ** H.264 is preferred: every phone decodes it in hardware */
} FmVsrcConf;

typedef struct FmVsrc {
  const char *key;              /* "youtube", "archive", ... (stable, saved in conf) */
  const char *name;             /* "YouTube" */
  FmIcon icon;
  int flags;                    /* VSRC_* */
  /* query, or a page URL when VSRC_URL; page_token from FmVsrcPage.next */
  FmErr (*search)(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                  volatile int *cancel);
  /* a playable stream for one item (or for a pasted URL in item->page) */
  FmErr (*resolve)(const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out, FmVsrcProgress cb,
                   void *user, char *err, size_t errcap, volatile int *cancel);
  /* saves the item into dir; out_path gets the main file. NULL = the shared
  ** default (resolve, then copy/download the stream files). */
  FmErr (*download)(const FmVsrcConf *c, const FmVsrcItem *item, const char *dir, char *out_path, size_t cap,
                    FmVsrcProgress cb, void *user, char *err, size_t errcap, volatile int *cancel);
  /* optional: details for one item (fills duration/views when search could not) */
  FmErr (*details)(const FmVsrcConf *c, FmVsrcItem *item, volatile int *cancel);
  /* a short line for the settings / empty state ("Free, no key needed") */
  const char *about;
} FmVsrc;

/* ---- registry and shared helpers (fvsrc.c) ------------------------------------ */

int vsrc_count(void);
const FmVsrc *vsrc_at(int i);
const FmVsrc *vsrc_find(const char *key);

void vsrc_page_free(FmVsrcPage *p);
/* Adds one item (grows the array); returns it zeroed with views = -1. */
FmVsrcItem *vsrc_page_add(FmVsrcPage *p);

/* Fills c from the app settings (main thread), locating yt-dlp. */
void vsrc_conf_snapshot(FmVsrcConf *c);
/* yt-dlp executable: the setting, next to the app, the config folder, PATH. */
bool vsrc_find_ytdlp(char *out, size_t cap);
/* A JavaScript runtime yt-dlp can use (deno or node), as "node:<path>". */
bool vsrc_find_js_runtime(char *out, size_t cap);

/* Shared download used when an adapter has no download(): resolve, then
** fetch network streams (net_download) or copy cached files into dir. */
FmErr vsrc_download(const FmVsrc *s, const FmVsrcConf *c, const FmVsrcItem *item, const char *dir,
                    char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                    volatile int *cancel);

/* Fetches the official yt-dlp release into the config folder (the settings
** "Get yt-dlp" button); out gets its path. */
FmErr vsrc_install_ytdlp(char *out, size_t cap, FmVsrcProgress cb, void *user, volatile int *cancel);

#endif
