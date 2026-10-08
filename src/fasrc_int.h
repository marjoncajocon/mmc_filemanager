/* fasrc_int.h -- shared pieces of the online audio adapters (fasrc*.c only).
**
** The adapters, the helpers they share, and the URL builders and parsers the
** self test feeds with canned replies.
**
** Design decisions:
**   - Parsers take the raw reply text, not a connection, so ftest_asrc.c can
**     check every adapter offline with sample JSON / RSS (and with garbage).
**   - Page tokens are plain numbers: the result offset for most sites, the
**     page for Freesound and archive.org ("" = first).
**   - Text, URL and date helpers are the photo and video adapters' own
**     (psrc_copy, psrc_url_copy, vsrc_clock_seconds ...): one tested copy.
**   - Limits live here so every adapter bounds the same things the same way.
*/
#ifndef FASRC_INT_H
#define FASRC_INT_H

#include "fasrc.h"
#include "fjson.h"
#include "fnet.h"
#include "fpsrc_int.h"     /* psrc_copy, psrc_url_copy, psrc_page_num, psrc_http_error ... */
#include "fvsrc_int.h"     /* vsrc_clock_seconds, vsrc_iso_date, vsrc_unix_date, vsrc_jstr */

#define ASRC_PAGE_SIZE    40              /* items asked for per search / browse page */
#define ASRC_MAX_ITEMS    40              /* hard cap per search page = the one allocation */
#define ASRC_MAX_CHILDREN 300             /* episodes / album tracks per children() page */
#define ASRC_MAX_REPLY    (4u << 20)      /* API reply size cap */
#define ASRC_MAX_META     (16u << 20)     /* archive.org /metadata of a big item (thousands of files) */
#define ASRC_MAX_FEED     (8u << 20)      /* podcast RSS feed cap */
#define ASRC_MAX_CATS     40              /* categories() rows */
#define ASRC_MAX_OFFSET   2000            /* deepest result offset a page token may ask for */

extern const FmAsrc g_asrc_radio;
extern const FmAsrc g_asrc_audius;
extern const FmAsrc g_asrc_archive;
extern const FmAsrc g_asrc_podcasts;
extern const FmAsrc g_asrc_jamendo;
extern const FmAsrc g_asrc_freesound;

/* ---- helpers (fasrc.c) ------------------------------------------------------ */

/* false (with a message in err) when this system has no HTTPS client */
bool  asrc_need_net(char *err, size_t cap);
/* GET into r (capped at max bytes). FM_OK means a reply arrived with any
** HTTP status; transport failures fill err. */
FmErr asrc_http_get(const char *url, const char *headers, size_t max, FmNetResp *r, char *err, size_t errcap,
                    volatile int *cancel);
/* Plain text from an API string: entities decoded, blank runs collapsed,
** trimmed, cut on a UTF-8 boundary ("  Jazz &amp; Blues " -> "Jazz & Blues"). */
void  asrc_text(const char *s, char *out, size_t cap);
/* Makes room for n items in one allocation (a fresh page only). */
void  asrc_page_reserve(FmAsrcPage *p, int n);
/* A new item of the source and kind (plays = -1). */
FmAsrcItem *asrc_item_new(FmAsrcPage *p, const char *source, int kind);
/* "audio/mpeg" -> "MP3", "audio/x-m4a" -> "AAC" ...; "" when unknown */
void  asrc_codec_from_mime(const char *mime, char *out, size_t cap);
/* ".mp3" style extension or file name -> codec; "" when unknown */
void  asrc_codec_from_ext(const char *name, char *out, size_t cap);
/* Radio Browser's codec field ("MP3", "AAC+", "OGG") made one of ours */
void  asrc_codec_norm(const char *codec, char *out, size_t cap);
/* true when the built-in decoders play this codec over HTTP (MP3, FLAC,
** WAV); "" (unknown) counts as playable: the decoder sniffs the bytes */
bool  asrc_codec_builtin(const char *codec);
/* "This station uses AAC -- install FFmpeg to play it" when the item's
** codec needs FFmpeg and there is none; false (err untouched) when fine */
bool  asrc_codec_blocked(const FmAsrcConf *c, const FmAsrcItem *item, const char *codec, char *err, size_t cap);
/* "http://creativecommons.org/licenses/by-nc-sa/3.0/" -> "CC BY-NC-SA 3.0",
** ".../publicdomain/zero/1.0/" -> "CC0 1.0"; a name that is not a URL is kept */
void  asrc_cc_license(const char *url, char *out, size_t cap);
/* RFC 822 date ("Wed, 07 Oct 2026 23:05:00 -0000") -> "2026-10-07"; "" when malformed */
void  asrc_rfc822_date(const char *s, char *out, size_t cap);
/* itunes:duration ("1:02:03", "62:03", "3723", "3723.5") -> seconds; 0 when malformed */
double asrc_duration(const char *s);
/* "<artist> - <title>" made safe as a file name (UTF-8 kept, at most max
** bytes, no reserved characters or names); "audio" when both are empty. */
void  asrc_file_base(const FmAsrcItem *item, char *out, size_t max);
/* ".mp3" / ".flac" / ".ogg" / ".opus" / ".wav" / ".m4a" / ".aac" / ".wma"
** from the first bytes of a file; "" when it is not audio we know. */
const char *asrc_sniff_ext(const u8 *head, size_t n);
/* the same from a Content-Type; "" for anything not audio */
const char *asrc_mime_ext(const char *type);
/* The credit text saved next to downloaded licensed music. */
void  asrc_credit(const FmAsrcItem *item, const char *site, const char *file_url, char *out, size_t cap);
/* true when a download of the item gets a credit file */
bool  asrc_wants_credit(const FmAsrcItem *item);

/* ---- URL builders and parsers (exposed for the self test) ------------------- */

/* Radio Browser: mirrors are tried in order from the last one that worked */
int   asrc_radio_mirror_count(void);
const char *asrc_radio_mirror(int i);
/* the path + query under /json/ for a search or a category, offset-paged */
bool  asrc_radio_path(const FmAsrcConf *c, const char *query, const char *cat, int offset, char *out, size_t cap);
FmErr asrc_radio_parse(const FmAsrcConf *c, const char *json, size_t len, int offset, FmAsrcPage *out);
/* json/url/<uuid> reply -> stream URL */
bool  asrc_radio_parse_click(const char *json, size_t len, char *url, size_t cap);

void  asrc_audius_search_url(const char *q, int offset, char *out, size_t cap);
bool  asrc_audius_browse_url(const char *cat, int offset, char *out, size_t cap);
/* tracks (search, trending, playlist tracks) or playlists (kind AITEM_ALBUM) */
FmErr asrc_audius_parse(const char *json, size_t len, int offset, int want, FmAsrcPage *out);
void  asrc_audius_stream_url(const char *id, char *out, size_t cap);

void  asrc_archive_search_url(const char *q, const char *collection, int page, char *out, size_t cap);
FmErr asrc_archive_parse_search(const char *json, size_t len, int page, FmAsrcPage *out);
/* /metadata/<id> -> the item's playable tracks, one per recording, in track order */
FmErr asrc_archive_parse_files(const FmAsrcConf *c, const char *json, size_t len, const FmAsrcItem *album,
                               int offset, FmAsrcPage *out);

void  asrc_podcasts_search_url(const FmAsrcConf *c, const char *q, char *out, size_t cap);
FmErr asrc_podcasts_parse_search(const char *json, size_t len, FmAsrcPage *out);
/* the iTunes top-podcasts chart for "top" / "genre:<id>"; false for others */
bool  asrc_podcasts_top_url(const FmAsrcConf *c, const char *cat, char *out, size_t cap);
FmErr asrc_podcasts_parse_top(const char *json, size_t len, FmAsrcPage *out);
/* iTunes lookup reply -> feedUrl */
bool  asrc_podcasts_parse_lookup(const char *json, size_t len, char *feed, size_t cap);
/* RSS feed -> episodes, newest first, at most ASRC_MAX_CHILDREN */
FmErr asrc_podcasts_parse_feed(const char *xml, size_t len, const FmAsrcItem *show, FmAsrcPage *out);

void  asrc_jamendo_url(const FmAsrcConf *c, const char *q, const char *cat, int offset, char *out, size_t cap);
FmErr asrc_jamendo_parse(const char *json, size_t len, int offset, FmAsrcPage *out);

void  asrc_freesound_url(const FmAsrcConf *c, const char *q, int page, char *out, size_t cap);
FmErr asrc_freesound_parse(const char *json, size_t len, int page, FmAsrcPage *out);

#endif
