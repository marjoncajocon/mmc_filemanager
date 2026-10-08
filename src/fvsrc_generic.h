/* fvsrc_generic.h -- the "Any site" source without yt-dlp: finds the video
** on a pasted web page natively, like yt-dlp's generic extractor.
**
** Used by the "Any site" adapter (fvsrc_ytdlp.c) before it falls back to
** yt-dlp, and on systems that cannot run yt-dlp at all (Android, web).
**
**   FmVsrcStream st;
**   if (vsrc_generic_resolve(&conf, "https://example.org/clip", &st, err, sizeof err, &cancel) == FM_OK)
**     play(st.video);                          // st.q[] lists the other sizes
**
** Threading: like every adapter function, blocking and reentrant (worker
** threads only); `cancel` is polled.
*/
#ifndef FVSRC_GENERIC_H
#define FVSRC_GENERIC_H

#include "fvsrc.h"

#define GEN_MAX_PAGE (2u << 20)   /* bytes of a page that are read; the rest is ignored */

/* A stream for a page URL (or a link straight to a media file). FM_OK =
** out->video streams with the decoders c describes; FM_ERR_UNSUPPORTED =
** videos were found but none plays here (out->q[] still lists them, err
** says why); FM_ERR_NOT_FOUND = "No video found on this page"; network
** errors and FM_ERR_CANCEL as usual. Never downloads a media body: a media
** link costs only its headers and first bytes. */
FmErr vsrc_generic_resolve(const FmVsrcConf *c, const char *page_url, FmVsrcStream *out, char *err,
                           size_t errcap, volatile int *cancel);

/* The gallery card for a page URL: id, title, thumb, page and duration when
** the page tells them. c may be NULL. FM_OK when the page has a video
** (playable here or not), FM_ERR_NOT_FOUND when it has none. */
FmErr vsrc_generic_probe(const FmVsrcConf *c, const char *page_url, FmVsrcItem *item, char *err, size_t errcap,
                         volatile int *cancel);

/* ---- offline pieces (exposed for the self test) -------------------------------- */

enum {                    /* media kinds, from a URL's extension, a MIME type or the first bytes */
  GEN_NONE, GEN_MP4, GEN_WEBM, GEN_MKV, GEN_HLS, GEN_TS, GEN_OGV, GEN_VIDEO,   /* GEN_VIDEO: other video */
  GEN_MP3, GEN_M4A, GEN_OGG, GEN_OPUS, GEN_AUDIO,                               /* GEN_AUDIO: other audio */
  GEN_UNKNOWN,            /* declared as a video by the page, container not known yet */
};

/* Scans one page (no network): fills item (title/thumb/duration, page =
** page_url) and out (q[], cur, video), like resolve. embed gets a player
** page to read next when the page only names one ("" = none). Candidates
** whose container is unknown count as needing FFmpeg. */
FmErr vsrc_generic_parse(const char *html, size_t len, const char *page_url, const FmVsrcConf *c,
                         FmVsrcItem *item, FmVsrcStream *out, char *embed, size_t embedcap);
/* ref resolved against base (RFC 3986, dot segments removed, fragment and
** spaces dropped/encoded); false for non-http(s) results (javascript:, data:). */
bool  vsrc_generic_join(const char *base, const char *ref, char *out, size_t cap);
/* GEN_* from the URL path's extension, else from mime (either may be NULL). */
int   vsrc_generic_kind(const char *url, const char *mime);
/* A picture height named in s ("720p", "1280x720", "_480.mp4"); 0 = none. */
int   vsrc_generic_height_hint(const char *s);
/* Copies s decoding JavaScript/JSON string escapes (\/ \uXXXX \n ...). */
void  vsrc_generic_js_unescape(const char *s, size_t len, char *out, size_t cap);

#endif
