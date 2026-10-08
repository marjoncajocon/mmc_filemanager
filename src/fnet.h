/* fnet.h -- HTTPS requests without linking a network library.
**
** Windows: WinHTTP (system DLL, loaded at run time, Windows 7+).
** Linux / BSD / macOS: libcurl loaded with dlopen when installed.
** Android / web: not available yet (FM_ERR_UNSUPPORTED).
**
** Blocking calls meant for worker threads; `cancel` is polled between reads.
** Responses are capped (max_bytes) so a hostile server cannot exhaust memory.
*/
#ifndef FNET_H
#define FNET_H

#include "fcore.h"

typedef struct FmNetResp {
  int status;          /* HTTP status, 0 when no response */
  u8 *data;            /* body, NUL-terminated (fm_free); NULL for downloads to a file */
  size_t len;
  char type[96];       /* Content-Type */
  char error[160];     /* transport error text */
  /* filled before the body arrives (net_get_stream's head callback sees them) */
  i64 length;          /* Content-Length, -1 when unknown (live streams) */
  i64 total;           /* whole resource from Content-Range "bytes a-b/TOTAL", 0 = not given */
  bool ranges;         /* byte ranges accepted (Accept-Ranges: bytes, or a 206 reply) */
  int icy_metaint;     /* SHOUTcast/Icecast metadata interval, 0 = none */
  char icy_name[96];   /* station name from icy-name */
} FmNetResp;

/* progress(user, done, total(0 = unknown)); return false to cancel */
typedef bool (*FmNetProgress)(void *user, u64 done, u64 total);

bool  net_available(void);
const char *net_backend(void);        /* "WinHTTP", "libcurl 8.5.0", or why not */

/* GET into memory. headers: extra "Key: value\r\n" lines or NULL. */
FmErr net_get(const char *url, const char *headers, size_t max_bytes, FmNetResp *out, volatile int *cancel);
/* GET into a file (written as path.part, renamed when complete). */
FmErr net_download(const char *url, const char *headers, const char *path, FmNetProgress cb, void *user,
                   FmNetResp *out, volatile int *cancel);
void  net_resp_free(FmNetResp *r);
/* POST with a body (headers should give its Content-Type); reply like net_get. */
FmErr net_post(const char *url, const char *headers, const void *body, size_t len, size_t max_bytes, FmNetResp *out,
               volatile int *cancel);

/* Streaming GET: `head` runs once the response headers are in (status,
** length, ranges, icy fields), then `data` for every chunk as it arrives;
** data returning false stops the transfer (FM_ERR_CANCEL). For readers that
** play while downloading (fnetstream.c). */
typedef void (*FmNetHead)(void *user, const FmNetResp *r);
typedef bool (*FmNetData)(void *user, const u8 *p, size_t n);
FmErr net_get_stream(const char *url, const char *headers, FmNetHead head, FmNetData data, void *user,
                     FmNetResp *out, volatile int *cancel);

/* Percent-encodes s for a query string (spaces become %20). */
void  net_urlencode(const char *s, char *out, size_t cap);

#endif
