/* fvsrc_int.h -- shared pieces of the online video adapters (fvsrc*.c only).
**
** The adapters themselves, the yt-dlp runner they share, the cache, and the
** small parsers the self test feeds with canned replies.
**
** Design decisions:
**   - Parsers take the raw reply text, not a connection, so ftest_vsrc.c can
**     check every adapter offline with sample JSON (and with garbage).
**   - Limits live here so every adapter bounds the same things the same way.
*/
#ifndef FVSRC_INT_H
#define FVSRC_INT_H

#include "fvsrc.h"
#include "fjson.h"
#include "fnet.h"

#define VSRC_PAGE_SIZE   24            /* items asked for per page */
#define VSRC_MAX_ITEMS   64            /* hard cap per page, whatever a site returns */
#define VSRC_MAX_REPLY   (4u << 20)    /* API reply size cap */
#define VSRC_MAX_YTJSON  (32u << 20)   /* yt-dlp -J output cap (a playlist can be big) */
#define VSRC_MAX_PAGES   25            /* deepest page a yt-dlp search goes */
#define VSRC_CACHE_MAX   ((u64)1 << 30) /* online cache size kept after a resolve */

extern const FmVsrc g_vsrc_youtube;
extern const FmVsrc g_vsrc_archive;
extern const FmVsrc g_vsrc_peertube;
extern const FmVsrc g_vsrc_dailymotion;
extern const FmVsrc g_vsrc_web;

/* ---- helpers (fvsrc.c) ------------------------------------------------------ */

/* false (with a message in err) when this system has no HTTPS client */
bool   vsrc_need_net(char *err, size_t cap);
/* GET into r (capped at VSRC_MAX_REPLY). FM_OK means a reply arrived with
** any HTTP status; transport failures fill err. */
FmErr  vsrc_http_get(const char *url, struct FmNetResp *r, char *err, size_t errcap, volatile int *cancel);
/* "Server error 503" style text for a non-JSON or unexpected reply. */
void   vsrc_http_error(const char *site, int status, char *err, size_t cap);
/* "PT1H2M3S" -> 3723; 0 when empty or malformed. */
double vsrc_iso_duration(const char *s);
/* "95:17", "1:02:03" or "370.2" -> seconds; 0 when malformed. */
double vsrc_clock_seconds(const char *s);
/* Copies s into out decoding &amp; &lt; &gt; &quot; &#39; &#NNN; &#xHH;. */
void   vsrc_html_unescape(const char *s, char *out, size_t cap);
/* unix seconds -> "YYYY-MM-DD" (UTC). */
void   vsrc_unix_date(i64 t, char *out, size_t cap);
/* Copies the first 10 chars of an ISO 8601 stamp when it starts with a date. */
void   vsrc_iso_date(const char *s, char *out, size_t cap);
/* A JSON string, or the first string of an array (archive.org mixes both). */
const char *vsrc_jstr(const FmJsonNode *n, const char *def);
/* true when s is 1..max chars from [A-Za-z0-9] plus `extra` */
bool   vsrc_id_ok(const char *s, const char *extra, size_t max);
/* true when s starts with http:// or https:// and has no spaces/controls */
bool   vsrc_url_ok(const char *s);
/* A file name from a title: no reserved characters, at most max bytes. */
void   vsrc_safe_name(const char *title, char *out, size_t max);
/* ffmpeg executable from c->ffmpeg_dir; false when there is none. */
bool   vsrc_ffmpeg_exe(const FmVsrcConf *c, char *out, size_t cap);
/* Saves a resolved stream into dir as "<title> [<id>].<ext>": copies local
** files, downloads URLs, merges a video+audio pair with ffmpeg when there is
** one, else saves two files and says so through cb. */
FmErr  vsrc_save_stream(const FmVsrcConf *c, const FmVsrcItem *item, const FmVsrcStream *st, const char *dir,
                        char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                        volatile int *cancel);
/* Deletes the oldest cache files (only names an adapter wrote) until the
** folder holds at most max_bytes; files starting with keep are spared. */
void   vsrc_cache_trim(const char *cache_dir, u64 max_bytes, const char *keep);

/* ---- yt-dlp (fvsrc_ytdlp.c) ------------------------------------------------- */

/* Search through yt-dlp. search_key "ytsearch" (etc.) with a query, or NULL
** with a page URL in target (a playlist URL pages through its entries).
** Page tokens are "y:<first item>". site picks thumbnail/page rules. */
FmErr vsrc_ytdlp_search(const FmVsrcConf *c, const char *site, const char *search_key, const char *target,
                        const char *page_token, FmVsrcPage *out, volatile int *cancel);
/* One yt-dlp -J: stream URLs plus the quality list. When nothing streams
** (or c->force_cache) the chosen formats download into the cache and local
** files come back, the quality list still filled. */
FmErr vsrc_ytdlp_resolve(const char *site, const FmVsrcConf *c, const FmVsrcItem *item, FmVsrcStream *out,
                         FmVsrcProgress cb, void *user, char *err, size_t errcap, volatile int *cancel);
/* Download button: one merged file with ffmpeg, else one single-file format
** when the site has one, else video + audio as two files. */
FmErr vsrc_ytdlp_download(const char *site, const FmVsrcConf *c, const FmVsrcItem *item, const char *dir,
                          char *out_path, size_t cap, FmVsrcProgress cb, void *user, char *err, size_t errcap,
                          volatile int *cancel);

/* ---- parsers (exposed for the self test) ------------------------------------ */

FmErr vsrc_ytdlp_parse_search(const char *json, size_t len, const char *site, int first, int want,
                              FmVsrcPage *out);
/* "-f" selector for playback; prefer_single puts single-file formats first */
const char *vsrc_ytdlp_selector(bool have_ffmpeg_libs, bool prefer_single);
/* Streaming choice from a yt-dlp -J reply: fills out->q[] (always, when the
** reply parses), and when a quality streams with the decoders c describes
** also out->video/audio/headers/cur, at the largest size <= c->max_height.
** FM_OK = streamable, FM_ERR_UNSUPPORTED = only through the cache,
** FM_ERR_FORMAT = not a reply. */
FmErr vsrc_ytdlp_pick_stream(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out);
/* The "<site>-<id>-<height>" cache key; "web" adds a hash of the page URL. */
void  vsrc_cache_key(const char *site, const FmVsrcItem *item, int height, char *out, size_t cap);

/* ---- YouTube without yt-dlp (fvsrc_innertube.c) ------------------------------- */

/* A video id from an id or a YouTube link (watch, youtu.be, shorts, embed, live). */
bool  vsrc_innertube_id(const char *s, char *out, size_t cap);
/* Stream URLs and the quality list for one video id, like vsrc_ytdlp_resolve
** (never downloads). */
FmErr vsrc_innertube_resolve(const FmVsrcConf *c, const char *id, FmVsrcStream *out, char *err, size_t errcap,
                             volatile int *cancel);
/* Title, channel, length ... of one video (a pasted link). */
FmErr vsrc_innertube_item(const char *id, FmVsrcItem *it, char *err, size_t errcap, volatile int *cancel);
/* Keyless search; page tokens are "i:<n>". */
FmErr vsrc_innertube_search(const FmVsrcConf *c, const char *query, const char *page_token, FmVsrcPage *out,
                            volatile int *cancel);
FmErr vsrc_innertube_parse_search(const char *json, size_t len, FmVsrcPage *out);
/* The quality list and the default pick from a player reply (exposed for the self test). */
FmErr vsrc_innertube_pick(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                          size_t errcap);

void vsrc_youtube_search_url(const FmVsrcConf *c, const char *query, const char *token, char *out,
                             size_t cap);
FmErr vsrc_youtube_parse_search(const char *json, size_t len, FmVsrcPage *out);
/* fills duration/views of page items from a videos?part=contentDetails,statistics reply */
FmErr vsrc_youtube_parse_videos(const char *json, size_t len, FmVsrcPage *p);
/* error JSON -> user text; returns the reason code ("quotaExceeded", ...) */
const char *vsrc_youtube_error(const char *json, size_t len, int status, char *out, size_t cap);

void  vsrc_archive_search_url(const char *query, int page, char *out, size_t cap);
FmErr vsrc_archive_parse_search(const char *json, size_t len, int page, FmVsrcPage *out);
FmErr vsrc_archive_pick(const char *json, size_t len, const char *id, const FmVsrcConf *c, FmVsrcStream *out,
                        char *err, size_t errcap);

void  vsrc_peertube_search_url(const FmVsrcConf *c, const char *query, int start, char *out, size_t cap);
FmErr vsrc_peertube_parse_search(const char *json, size_t len, int start, FmVsrcPage *out);
FmErr vsrc_peertube_pick(const char *json, size_t len, const FmVsrcConf *c, FmVsrcStream *out, char *err,
                         size_t errcap);

void  vsrc_dailymotion_search_url(const FmVsrcConf *c, const char *query, int page, char *out, size_t cap);
FmErr vsrc_dailymotion_parse_search(const char *json, size_t len, int page, FmVsrcPage *out);
/* The player metadata reply (www.dailymotion.com/player/metadata/video/<id>). */
typedef struct FmDmMeta {
  char master[2048];      /* qualities.auto[0].url: a signed HLS master playlist */
  double duration;
  bool live;
} FmDmMeta;
/* FM_OK, FM_ERR_NOT_FOUND (the site's own error message in err), FM_ERR_FORMAT */
FmErr vsrc_dailymotion_parse_meta(const char *json, size_t len, FmDmMeta *m, char *err, size_t errcap);
/* Fills out->q[] from the master playlist (base = its URL), and with FM_OK
** also cur/video/audio/size for the tallest playable <= c->max_height.
** FM_ERR_UNSUPPORTED = nothing decodes here, FM_ERR_FORMAT = not a master. */
FmErr vsrc_dailymotion_pick(const char *m3u8, size_t len, const char *base, const FmDmMeta *m,
                            const FmVsrcConf *c, FmVsrcStream *out);

#endif
