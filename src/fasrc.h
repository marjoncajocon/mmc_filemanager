/* fasrc.h -- online audio sources: one adapter struct per website.
**
** The audio twin of fvsrc.h (video) and fpsrc.h (photos). Each site is a
** table of functions; the "Online audio" view, the music-player hookup and
** downloads are shared.
**
**   const FmAsrc g_asrc_mysite = {
**     "mysite", "My Site", IC_MUSIC, ASRC_SEARCH, mysite_search, NULL, NULL, mysite_resolve,
**     NULL, "Free, no key needed"
**   };
**
** Items come in four kinds: tracks (finite files), stations (live, endless),
** and containers (podcasts, albums/playlists) whose children() are tracks or
** episodes. Playback: resolve() gives a URL the music player opens directly
** (aud_open accepts http(s) URLs: an HTTP stream reader with byte-range
** seeking feeds the decoder; radio "now playing" titles come from ICY
** metadata). Nothing is downloaded before playing.
**
** Threading and memory rules are the same as fvsrc.h: every function blocks,
** runs on a worker thread, is reentrant, polls `cancel`, and fills plain
** structs with fixed-size strings (one free per page).
**
** Probed from this PC (2026-10-08): Radio Browser, Audius, Internet Archive
** (mediatype:audio) and the iTunes podcast search answer without a key;
** Jamendo needs a free client_id, Freesound a free key. Built-in decoders:
** MP3/FLAC/WAV/Vorbis; AAC/Opus/WMA need FFmpeg (FmAsrcConf.have_ffmpeg).
*/
#ifndef FASRC_H
#define FASRC_H

#include "fcore.h"
#include "ficon.h"

enum {
  ASRC_SEARCH  = 1 << 0,   /* has search() */
  ASRC_BROWSE  = 1 << 1,   /* browse() lists categories / top items with an empty query */
  ASRC_NEEDKEY = 1 << 2,   /* needs an API key in the settings */
  ASRC_LIVE    = 1 << 3,   /* items are live stations (radio) */
};

enum { AITEM_TRACK, AITEM_STATION, AITEM_PODCAST, AITEM_ALBUM };

typedef struct FmAsrcItem {
  char source[16];         /* FmAsrc key that made the item */
  int kind;                /* AITEM_* */
  char id[128];
  char title[256];
  char artist[160];        /* artist / uploader / podcast author / station country */
  char album[160];         /* album / podcast name / station tags */
  char art[512];           /* cover / station logo URL, "" = none */
  char url[1024];          /* direct stream or file URL when known at search time, else "" (resolve) */
                           /* (added 2026-10-08) PODCAST containers: the RSS feed address
                           ** ("" for chart entries: children() looks it up). Never played. */
  char page[512];          /* web page for "Open in browser" */
  char codec[16];          /* "MP3", "AAC", "OGG" ..., "" = unknown */
  char license[64];
  int bitrate;             /* kbps, 0 = unknown */
  double duration;         /* seconds, 0 = unknown / live */
  char published[32];      /* ISO date, "" = unknown */
  i64 plays;               /* -1 = unknown */
} FmAsrcItem;

typedef struct FmAsrcPage {
  FmAsrcItem *items;
  int count, cap;
  char next[256];          /* continuation token, "" = end */
  char error[256];
} FmAsrcPage;

/* A browse category ("Top stations", "Jazz", "Trending") */
typedef struct FmAsrcCat { char id[64]; char name[64]; } FmAsrcCat;

typedef struct FmAsrcStream {
  char url[2048];          /* what aud_open gets (http(s) or a local cache file) */
  char headers[256];       /* extra request headers, "" = none */
  char codec[16];          /* best guess, for the "needs FFmpeg" check */
  bool live;               /* endless: no seeking, no duration */
} FmAsrcStream;

typedef struct FmAsrcConf {
  char key_jamendo[64];
  char key_freesound[96];
  char cache_dir[FM_PATH_MAX];
  char download_dir[FM_PATH_MAX];
  bool have_ffmpeg;        /* AAC / Opus / WMA streams can be decoded */
  bool safe_search;
  char country[8];         /* "PH", "" = any (radio defaults) */
} FmAsrcConf;

typedef struct FmAsrc {
  const char *key;          /* "radio", "audius", "archive", "podcasts", "jamendo", "freesound" */
  const char *name;
  FmIcon icon;
  int flags;                /* ASRC_* */
  FmErr (*search)(const FmAsrcConf *c, const char *query, const char *page_token, FmAsrcPage *out,
                  volatile int *cancel);
  /* optional: categories for an empty query, then the items of one */
  int (*categories)(const FmAsrcConf *c, FmAsrcCat *out, int max);
  FmErr (*browse)(const FmAsrcConf *c, const char *cat_id, const char *page_token, FmAsrcPage *out,
                  volatile int *cancel);
  /* a stream for a track or station (podcast episodes usually have url set already) */
  FmErr (*resolve)(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                   volatile int *cancel);
  /* optional: the tracks / episodes of a container item (podcast, album) */
  FmErr (*children)(const FmAsrcConf *c, const FmAsrcItem *container, const char *page_token, FmAsrcPage *out,
                    volatile int *cancel);
  const char *about;        /* "50,000 live radio stations, no key needed" */
} FmAsrc;

/* ---- registry and shared helpers (fasrc.c) ------------------------------------ */

int asrc_count(void);
const FmAsrc *asrc_at(int i);
const FmAsrc *asrc_find(const char *key);

void asrc_page_free(FmAsrcPage *p);
FmAsrcItem *asrc_page_add(FmAsrcPage *p);
void asrc_conf_snapshot(FmAsrcConf *c);

/* The item's stream: item->url when set, else the source's resolve().
** (added 2026-10-08) Stations always go through resolve() (Radio Browser
** counts a click there, as its API asks). Containers are refused ("Open the
** podcast to pick an episode"), and so is a codec that needs FFmpeg when
** have_ffmpeg is false: err = "This station uses AAC -- install FFmpeg to
** play it" (FM_ERR_UNSUPPORTED), before any slow connect. */
FmErr asrc_stream(const FmAsrcConf *c, const FmAsrcItem *item, FmAsrcStream *out, char *err, size_t errcap,
                  volatile int *cancel);
/* Saves a finite item into dir as "<artist> - <title>.<ext>" (tags kept as
** they come); out gets the path. Stations cannot be downloaded.
** (added 2026-10-08) dir NULL/"" = c->download_dir. Never overwrites: a
** taken name becomes "<name> (2).<ext>". The extension comes from the first
** bytes (else the Content-Type); a reply that is not audio is an error.
** Licensed music (Jamendo, Freesound, archive.org, CC licences) also gets
** "<name>.txt" with title, artist, licence and page. Any codec is saved,
** FFmpeg or not. */
FmErr asrc_download(const FmAsrcConf *c, const FmAsrcItem *item, const char *dir, char *out, size_t cap,
                    bool (*progress)(void *user, u64 done, u64 total), void *user, char *err, size_t errcap,
                    volatile int *cancel);

#endif
