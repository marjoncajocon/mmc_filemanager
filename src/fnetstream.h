/* fnetstream.h -- read an HTTP(S) resource like a file while it downloads.
**
** For playing online audio without downloading it first: a worker thread
** fills a ring buffer; reads block until data arrives; seeking restarts the
** request at a byte offset (when the server accepts ranges). Live radio
** (Icecast / SHOUTcast) works too: its interleaved metadata is removed from
** the audio and the current title is kept for "now playing".
*/
#ifndef FNETSTREAM_H
#define FNETSTREAM_H

#include "fcore.h"

typedef struct FmNetStream FmNetStream;

/* Connects and waits for the response headers (up to ~15 s). headers: extra
** "Key: v\r\n" lines or NULL. On failure err says why (HTTP status ...). */
FmNetStream *ns_open(const char *url, const char *headers, char *err, size_t errcap);
/* Copies up to n of the next bytes without consuming them (waits until n
** are buffered or the stream ends); returns how many. For format sniffing
** on streams that cannot rewind (radio). n <= 64 KB. */
size_t ns_peek(FmNetStream *s, void *out, size_t n);
/* Reads up to n bytes, blocking until some arrive; 0 at the end or on error. */
size_t ns_read(FmNetStream *s, void *out, size_t n);
/* Absolute byte position; false when the server cannot seek there. */
bool   ns_seek(FmNetStream *s, i64 pos);
i64    ns_tell(const FmNetStream *s);
i64    ns_size(const FmNetStream *s);        /* -1 = unknown (live) */
bool   ns_live(const FmNetStream *s);        /* no length and no ranges */
bool   ns_seekable(const FmNetStream *s);
/* Station name (icy-name) and current title (ICY StreamTitle), "" if none. */
void   ns_station(const FmNetStream *s, char *out, size_t cap);
void   ns_now_playing(const FmNetStream *s, char *out, size_t cap);
const char *ns_content_type(const FmNetStream *s);
void   ns_close(FmNetStream *s);
/* stream: for an open stream reading url (the first one found): the next byte
** its reader gets, the end of the data buffered after it, and the size (-1
** unknown). false when none is open. For the video player's buffer bar. */
bool   ns_url_buffered(const char *url, i64 *pos, i64 *end, i64 *size);

#endif
