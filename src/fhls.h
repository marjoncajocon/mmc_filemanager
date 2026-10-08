/* fhls.h -- HLS (HTTP Live Streaming) playlists: parse, pick, decrypt.
**
** The text side of HLS, kept apart from the network so the self test can
** check it offline with canned playlists. fnetstream.c uses it to present a
** media playlist's segments as one continuous byte stream (an MPEG-TS or a
** fragmented MP4 the decoders already read); fvsrc_dailymotion.c uses it to
** list the qualities of a master playlist.
**
**   master playlist  #EXT-X-STREAM-INF lines: one variant per picture size,
**                    each pointing at a media playlist
**   media playlist   #EXTINF lines: the segments, in play order, each a few
**                    seconds of TS or fMP4 (#EXT-X-MAP names the fMP4 init
**                    section that goes in front of the first fragment)
**
** Design decisions:
**   - Strings live in one arena per parsed playlist; one free releases it.
**     Segment URIs are kept as written (mostly short relative names) and
**     resolved when fetched, so a 3-hour VOD list stays small.
**   - Everything is bounded: playlist text (HLS_MAX_TEXT), segments
**     (HLS_MAX_SEGS), variants (HLS_MAX_VARIANTS), keys (HLS_MAX_KEYS).
**   - AES-128 (whole-segment CBC with PKCS7 padding) is decrypted here, as
**     the bytes stream in, with fcrypt's AES. SAMPLE-AES and other methods
**     are reported, not played (they need a demuxer-level decryptor).
*/
#ifndef FHLS_H
#define FHLS_H

#include "fcore.h"
#include "fcrypt.h"

#define HLS_MAX_TEXT     (8u << 20)    /* playlist size cap */
#define HLS_MAX_SEGS     50000         /* about 40 hours of 3 s segments */
#define HLS_MAX_VARIANTS 32
#define HLS_MAX_KEYS     256
#define HLS_URL_MAX      4096

enum { HLS_NONE = 0, HLS_MASTER, HLS_MEDIA };              /* hls_kind */
enum { HLS_KEY_NONE = 0, HLS_KEY_AES128, HLS_KEY_OTHER };   /* FmHlsKey.method */

typedef struct FmHlsVariant {
  const char *uri;        /* absolute media playlist URL (no fragment) */
  const char *audio_uri;  /* the AUDIO group's default rendition, or NULL (sound is in uri) */
  int bandwidth;          /* peak bits/s (BANDWIDTH) */
  int avg_bandwidth;      /* AVERAGE-BANDWIDTH, 0 = not given */
  int width, height;      /* RESOLUTION, 0 = not given */
  double fps;             /* FRAME-RATE, 0 = not given */
  char codecs[96];        /* "mp4a.40.2,avc1.64001f" */
  char name[32];          /* NAME (Dailymotion: "480"), "" if none */
} FmHlsVariant;

typedef struct FmHlsMaster {
  FmHlsVariant v[HLS_MAX_VARIANTS];
  int n;
  FmArena a;
} FmHlsMaster;

typedef struct FmHlsKey {
  int method;             /* HLS_KEY_* */
  const char *uri;        /* absolute key URL */
  u8 iv[16];
  bool has_iv;            /* else the IV is the segment's sequence number */
} FmHlsKey;

typedef struct FmHlsSeg {
  const char *uri;        /* as written in the playlist (resolve against FmHlsMedia.base) */
  double dur, start;      /* seconds; start = the sum of the durations before it */
  i64 seq;                /* media sequence number */
  i64 br_off, br_len;     /* EXT-X-BYTERANGE, br_len < 0 = the whole resource */
  int key;                /* index into keys, -1 = clear */
  bool disc;              /* EXT-X-DISCONTINUITY before it */
} FmHlsSeg;

typedef struct FmHlsMedia {
  FmHlsSeg *seg;
  int n;
  FmHlsKey *key;
  int nkey;
  double target;          /* EXT-X-TARGETDURATION */
  double total;           /* sum of the segment durations */
  i64 first_seq;          /* EXT-X-MEDIA-SEQUENCE */
  bool endlist;           /* EXT-X-ENDLIST: no more segments will come (VOD) */
  bool vod;               /* PLAYLIST-TYPE:VOD (implies endlist) */
  const char *map_uri;    /* EXT-X-MAP (fMP4 init section), absolute, NULL = none */
  i64 map_off, map_len;   /* its byte range, map_len < 0 = whole */
  bool map_changes;       /* a second, different EXT-X-MAP: not supported */
  const char *base;       /* the playlist's own URL, for resolving segment URIs */
  int segcap, keycap;
  FmArena a;
} FmHlsMedia;

/* What a playlist text is: HLS_MASTER, HLS_MEDIA or HLS_NONE (not #EXTM3U). */
int   hls_kind(const char *text, size_t len);
/* true when s starts like a playlist ("#EXTM3U", BOM and blanks allowed). */
bool  hls_sniff(const void *s, size_t n);
/* true when the URL's path ends in .m3u8 or .m3u (query and fragment ignored). */
bool  hls_url_like(const char *url);
/* RFC 3986 reference resolution (with ./ and ../ removed); the fragment is
** dropped. out gets "" when the result does not fit. */
void  hls_resolve(const char *base, const char *ref, char *out, size_t cap);

/* Parses a master playlist; base is its URL. FM_ERR_FORMAT when it is not
** one (err says why). */
FmErr hls_parse_master(const char *text, size_t len, const char *base, FmHlsMaster *m, char *err, size_t errcap);
void  hls_master_free(FmHlsMaster *m);
/* The variant to play: the tallest with height <= max_height (more bits on a
** tie, H.264 preferred over other codecs), else the smallest; without
** RESOLUTION the best bandwidth up to 5 Mbit/s. -1 when there are none. */
int   hls_pick_variant(const FmHlsMaster *m, int max_height);

FmErr hls_parse_media(const char *text, size_t len, const char *base, FmHlsMedia *p, char *err, size_t errcap);
void  hls_media_free(FmHlsMedia *p);
/* The segment playing at t seconds (0 when t <= 0, the last past the end). */
int   hls_seg_at(const FmHlsMedia *p, double t);
/* "x-mpegurl" style content types */
bool  hls_type_like(const char *content_type);

/* ---- AES-128 segment decryption (CBC, PKCS7) ---------------------------------- */

typedef struct FmHlsDec {
  FmAes aes;
  u8 iv[16];
  u8 in[16];              /* ciphertext not yet a whole block */
  int nin;
  u8 hold[16];            /* the last plain block: may end in padding */
  bool held;
} FmHlsDec;

typedef bool (*FmHlsSink)(void *user, const u8 *p, size_t n);
void hls_dec_init(FmHlsDec *d, const u8 key[16], const u8 iv[16]);
/* Decrypts n more bytes, handing plain text to sink; false when sink did. */
bool hls_dec_feed(FmHlsDec *d, const u8 *p, size_t n, FmHlsSink sink, void *user);
/* The segment ended: strips the padding from the held block. false on a bad
** padding or a length that is not whole blocks. */
bool hls_dec_end(FmHlsDec *d, FmHlsSink sink, void *user);
/* The IV of a key without one: the sequence number, big-endian, 128 bits. */
void hls_seq_iv(i64 seq, u8 iv[16]);

#endif
