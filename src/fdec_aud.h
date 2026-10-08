/* fdec_aud.h -- streaming audio decoding and tag reading.
**
** MP3/MP2 (dr_mp3), FLAC (dr_flac), WAV/AIFF (dr_wav), Ogg Vorbis
** (stb_vorbis) built in; anything else (m4a, aac, opus, wma ...) through
** FFmpeg when it is installed. Tags: ID3v2/v1, Vorbis comments, FLAC
** pictures, MP4 ilst atoms and RIFF INFO.
**
** Design decisions:
**   - Pull decoding in small blocks into the caller's buffer: memory is the
**     decoder state only, whatever the file length.
**   - Output is float, at most two channels (wider layouts are downmixed
**     here), so the player only has to resample.
**   - Tag reading is separate from decoding and shared with the thumbnail
**     service, which only wants the cover.
*/
#ifndef FDEC_AUD_H
#define FDEC_AUD_H

#include "fcore.h"

typedef struct FmAudio FmAudio;

/* path may be an http(s) URL: an HTTP stream reader (fnet) feeds the
** decoder; seeking re-requests at a byte offset when the server allows it,
** live streams (radio) cannot seek. MP3 streams decode built in; others need
** FFmpeg. */
FmAudio *aud_open(const char *path, FmErr *err);
/* Same, with extra request header lines ("Key: value" + CRLF each). */
FmAudio *aud_open_ex(const char *path, const char *headers, FmErr *err);
/* Same again; on failure msg gets the reason in words when there is one
** ("The server answered 404", "Not an audio stream ..."), else "". */
FmAudio *aud_open_msg(const char *path, const char *headers, FmErr *err, char *msg, size_t cap);
/* Live radio: the station's current "Artist - Title" (ICY metadata), "" when
** unknown. Changes while playing; copy it out. Thread-safe. */
void aud_now_playing(const FmAudio *a, char *out, size_t cap);
/* An endless stream (no length, no seeking). */
bool aud_is_live(const FmAudio *a);
int  aud_channels(const FmAudio *a);       /* 1 or 2 */
int  aud_rate(const FmAudio *a);
u64  aud_length(const FmAudio *a);         /* frames, 0 when unknown */
u64  aud_tell(const FmAudio *a);           /* frames decoded so far */
const char *aud_codec(const FmAudio *a);   /* "MP3", "FLAC" ... */
/* Decodes up to `frames` frames into out (interleaved); 0 at the end. */
int  aud_read(FmAudio *a, float *out, int frames);
bool aud_seek(FmAudio *a, u64 frame);
void aud_close(FmAudio *a);

#define AUD_META_TEXT 256
#define AUD_COVER_MAX (16u * 1024u * 1024u)

typedef struct FmAudMeta {
  char title[AUD_META_TEXT];
  char artist[AUD_META_TEXT];
  char album[AUD_META_TEXT];
  u8 *cover;          /* encoded JPEG/PNG, fm_alloc; NULL when none */
  size_t cover_len;
} FmAudMeta;

/* Reads tags (and the embedded cover when want_cover). True if anything was found. */
bool aud_meta(const char *path, FmAudMeta *m, bool want_cover);
void aud_meta_free(FmAudMeta *m);

/* Parsers exposed for the self test. */
bool aud_meta_id3v2(const u8 *tag, size_t n, FmAudMeta *m, bool want_cover);

#endif
