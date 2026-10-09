/* fnetstream.h -- read an HTTP(S) resource like a file while it downloads.
**
** For playing online audio without downloading it first: a worker thread
** fills a ring buffer; reads block until data arrives; seeking restarts the
** request at a byte offset (when the server accepts ranges). Live radio
** (Icecast / SHOUTcast) works too: its interleaved metadata is removed from
** the audio and the current title is kept for "now playing".
**
** HLS (.m3u8) works too: the segments of the playlist are read as one
** continuous MPEG-TS or fragmented MP4 file (content type video/mp2t or
** video/mp4), live playlists included. A master playlist plays its best
** variant up to 1080p (pass a variant's URL for another size), and a
** "#t=SECONDS" fragment on a VOD playlist's URL starts the stream at the
** segment holding that time (see fnetstream.c for seeking and limits).
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
/* HLS: true for a playlist's segments; the time byte 0 plays at (a "#t="
** open starts at a segment boundary at or before it); the playlist's length
** in seconds (0 for live). Fixed once open. */
bool   ns_is_hls(const FmNetStream *s);
/* Live HLS packed audio (raw AAC/MP3 segments, YouTube live's sound): the
** ID3 tags the segments start with are cut out of the stream; this is the
** first one's time, the broadcast clock (s) of the first sample read, or 0.
** Known once the first bytes arrived. */
double ns_hls_audio_time(FmNetStream *s);
/* The read-ahead buffer grows to 4 MB while the network is ahead; a reader
** that only needs a little (a second cursor into the same file) caps it. */
void   ns_limit_ring(FmNetStream *s, size_t max);
/* Bytes at pos when they are already in the read-ahead buffer (nothing is
** consumed, nothing waits); 0 when they are not there yet. */
size_t ns_peek_at(FmNetStream *s, i64 pos, void *out, size_t n);
/* Bytes read ahead and not consumed yet. */
size_t ns_buffered(FmNetStream *s);
double ns_hls_start(const FmNetStream *s);
double ns_hls_duration(const FmNetStream *s);
/* From any thread: a blocked ns_read/ns_peek returns 0 now, later ones at
** once, seeks fail; the network work stops. For owners that must finish
** pending reads before tearing down (Media Foundation's async reads must
** complete before MFShutdown). ns_close is still needed afterwards. */
void   ns_abort(FmNetStream *s);
void   ns_close(FmNetStream *s);
/* stream: for an open stream reading url (the first one found): the next byte
** its reader gets, the end of the data buffered after it, and the size (-1
** unknown). false when none is open. For the video player's buffer bar. */
bool   ns_url_buffered(const char *url, i64 *pos, i64 *end, i64 *size);

#endif
