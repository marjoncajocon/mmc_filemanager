/* fpsrc_int.h -- shared pieces of the online photo adapters (fpsrc*.c only).
**
** The adapters, the helpers they share, and the small URL builders and
** parsers the self test feeds with canned replies.
**
** Design decisions:
**   - Parsers take the raw reply text, not a connection, so ftest_psrc.c can
**     check every adapter offline with sample JSON (and with garbage).
**   - Page tokens are plain numbers: the page for most sites, the result
**     offset for Wikimedia Commons ("" = first page).
**   - Limits live here so every adapter bounds the same things the same way.
*/
#ifndef FPSRC_INT_H
#define FPSRC_INT_H

#include "fpsrc.h"
#include "fjson.h"
#include "fnet.h"

#define PSRC_PAGE_SIZE   30             /* items asked for per page */
#define PSRC_MAX_ITEMS   30             /* hard cap per page = the one allocation */
#define PSRC_MAX_REPLY   (4u << 20)     /* API reply size cap */
#define PSRC_CACHE_MAX   ((u64)500 << 20) /* photo cache kept after a fetch */
#define PSRC_OV_PAGE     20             /* Openverse: anonymous page_size limit */
#define PSRC_OV_MAX_PAGE 12             /* Openverse: anonymous depth limit (240 results) */

extern const FmPsrc g_psrc_openverse;
extern const FmPsrc g_psrc_wikimedia;
extern const FmPsrc g_psrc_nasa;
extern const FmPsrc g_psrc_artic;
extern const FmPsrc g_psrc_pexels;
extern const FmPsrc g_psrc_unsplash;
extern const FmPsrc g_psrc_pixabay;

/* ---- helpers (fpsrc.c) ------------------------------------------------------ */

/* false (with a message in err) when this system has no HTTPS client */
bool  psrc_need_net(char *err, size_t cap);
/* GET into r (capped at PSRC_MAX_REPLY). FM_OK means a reply arrived with
** any HTTP status; transport failures fill err. */
FmErr psrc_http_get(const char *url, const char *headers, FmNetResp *r, char *err, size_t errcap,
                    volatile int *cancel);
/* User text for a failed reply: 401/403 (keyed: "check the key"), 429,
** 5xx, anything else. Returns the FmErr the search should return. */
FmErr psrc_http_error(const char *site, int status, bool keyed, char *err, size_t cap);
/* "Add a free <site> key in Settings" when key is empty; true when set. */
bool  psrc_have_key(const char *site, const char *key, char *err, size_t cap);
/* "" / NULL -> first; else the number in tok clamped to lo..hi. */
int   psrc_page_num(const char *tok, int first, int lo, int hi);
/* Copies a URL, percent-encoding spaces; false (out = "") when it is not
** a plain http(s) URL (controls, quotes, angle brackets, too long). */
bool  psrc_url_copy(const char *url, char *out, size_t cap);
/* HTML fragment -> plain text: tags dropped, entities decoded, blank runs
** collapsed, trimmed ("<a href=..>Bob</a> &amp; co" -> "Bob & co"). */
void  psrc_html_text(const char *html, char *out, size_t cap);
/* true when s is 1..max chars from [A-Za-z0-9] plus `extra` */
bool  psrc_id_ok(const char *s, const char *extra, size_t max);
/* Copies at most cap-1 bytes without splitting a UTF-8 character. */
void  psrc_copy(char *out, const char *s, size_t cap);
/* "#7B6B5B" -> 0x7B6B5B; 0 when malformed. */
u32   psrc_hex_color(const char *s);
/* Scales w x h down to fit max_w wide (keeps the aspect); 0 stays 0. */
void  psrc_fit_width(int *w, int *h, int max_w);
/* ".jpg" style extension of a URL's path, lower case, "" when none. */
void  psrc_url_ext(const char *url, char *out, size_t cap);
/* true when the URL's extension is one the image viewer decodes (or the
** URL has none, as API image URLs often do). */
bool  psrc_url_viewable(const char *url);
/* "<title> - <author>" made safe as a file name (UTF-8 kept, at most max
** bytes, no reserved characters or names); "photo" when both are empty. */
void  psrc_file_base(const FmPsrcItem *item, char *out, size_t max);
/* ".jpg" / ".png" / ".gif" / ".webp" / ".tif" / ".svg" / ".bmp" from the
** first bytes of a file; "" when it is not an image. */
const char *psrc_sniff_ext(const u8 *head, size_t n);
/* The attribution text saved next to a downloaded photo. */
void  psrc_attribution(const FmPsrcItem *item, const char *site, char *out, size_t cap);
/* "<source>-<16 hex>" cache key for a URL. */
void  psrc_cache_key(const FmPsrcItem *item, const char *url, char *out, size_t cap);
/* Deletes the oldest cache files (only names an adapter wrote) until the
** folder holds at most max_bytes; files starting with keep are spared. */
void  psrc_cache_trim(const char *cache_dir, u64 max_bytes, const char *keep);

/* ---- URL builders and parsers (exposed for the self test) ------------------- */

void  psrc_openverse_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap);
FmErr psrc_openverse_parse(const char *json, size_t len, int page, FmPsrcPage *out);
/* a Flickr static photo URL resized to 400 px ("_w"); false for others */
bool  psrc_flickr_thumb(const char *url, char *out, size_t cap);

void  psrc_wikimedia_url(const char *q, int offset, char *out, size_t cap);
FmErr psrc_wikimedia_parse(const char *json, size_t len, FmPsrcPage *out);
/* A thumbnail URL ("/500px-Name.jpg") resized to w px wide; false when the
** URL has no size part. */
bool  psrc_wikimedia_resize(const char *thumb, int w, char *out, size_t cap);

void  psrc_nasa_url(const char *q, int page, char *out, size_t cap);
FmErr psrc_nasa_parse(const char *json, size_t len, int page, FmPsrcPage *out);
/* collection.json (a list of asset URLs) -> item full/original */
FmErr psrc_nasa_parse_assets(const char *json, size_t len, FmPsrcItem *item);

void  psrc_artic_url(const char *q, int page, char *out, size_t cap);
FmErr psrc_artic_parse(const char *json, size_t len, int page, FmPsrcPage *out);

void  psrc_pexels_url(const char *q, int page, char *out, size_t cap);
FmErr psrc_pexels_parse(const char *json, size_t len, int page, FmPsrcPage *out);

void  psrc_unsplash_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap);
FmErr psrc_unsplash_parse(const char *json, size_t len, int page, FmPsrcPage *out);
/* the download-tracking endpoint Unsplash's API terms ask apps to call */
bool  psrc_unsplash_track_url(const FmPsrcItem *item, char *out, size_t cap);

void  psrc_pixabay_url(const FmPsrcConf *c, const char *q, int page, char *out, size_t cap);
FmErr psrc_pixabay_parse(const char *json, size_t len, int page, FmPsrcPage *out);

#endif
