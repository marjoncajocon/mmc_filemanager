/* fcloud_dav.h -- internals shared by the WebDAV and S3 adapters
** (fcloud_webdav.c, fcloud_s3.c) and their self test (ftest_dav.c).
**
** Not for the UI: the panels only see the FmCloud tables (fcloud.h). The
** parsers and the signer are exposed so the tests can feed them canned
** replies and published test vectors without a network.
*/
#ifndef FCLOUD_DAV_H
#define FCLOUD_DAV_H

#include "fcloud.h"

/* ---- shared helpers (fcloud_webdav.c) ------------------------------------------ */

/* Splits "scheme://host[:port]/path" -> host without the port ("[::1]" ->
** "::1"), the port text ("" when none) and the path ("/" when none).
** Returns false when it is not an http(s) URL. */
bool  dav_split_url(const char *url, bool *https, char *host, size_t hcap, char *port, size_t pcap,
                    const char **path);
/* True for addresses on the user's own network: loopback, private and
** link-local IPv4/IPv6, CGNAT (Tailscale), single-label and .local/.lan/
** .home/.internal/.home.arpa names. Plain http is only allowed for these. */
bool  dav_host_is_local(const char *host);
/* Percent-encodes a path: every byte except A-Z a-z 0-9 - _ . ~ and '/'
** becomes %XX (UTF-8 bytes one by one). False when it does not fit. */
bool  dav_pct_path(const char *s, char *out, size_t cap);
/* Decodes %XX in s[0..n) (malformed escapes kept as written, '+' kept). */
size_t dav_pct_decode(const char *s, size_t n, char *out, size_t cap);
/* "unix seconds" <-> civil time in UTC. */
void  dav_civil(i64 t, int *y, int *mo, int *d, int *h, int *mi, int *s);
/* Unsigned decimal at s (stops at the first non-digit); 0 when none. */
u64   dav_atou64(const char *s);

/* ---- WebDAV (fcloud_webdav.c) ------------------------------------------------- */

/* Adds the children of dir_id from a PROPFIND multistatus reply. base_path
** is the decoded path of the account URL ("/remote.php/dav/files/me/").
** The folder itself is skipped. Returns the number of entries added, -1
** when the reply is not a multistatus. */
int   dav_parse_multistatus(const char *xml, size_t len, const char *base_path, const char *dir_id,
                            FmCloudList *out);
/* quota-used-bytes / quota-available-bytes; false when absent. */
bool  dav_parse_quota(const char *xml, size_t len, u64 *used, u64 *total);

/* ---- S3 (fcloud_s3.c) --------------------------------------------------------- */

typedef struct S3Hdr { const char *name, *value; } S3Hdr;   /* name in lower case */

typedef struct S3Sig {
  const char *method;
  const char *uri;          /* canonical URI, already percent-encoded ("/bucket/a%20b.txt") */
  const char *query;        /* canonical query (sorted, encoded), "" for none */
  const S3Hdr *hdr;         /* the signed headers, any order (host included) */
  int nhdr;
  const char *payload;      /* hex SHA-256 of the body or "UNSIGNED-PAYLOAD" */
  const char *amzdate;      /* "20130524T000000Z" */
  const char *region, *service, *key_id, *secret;
} S3Sig;

/* SigV4: the hex signature (65 bytes) and, when auth is not NULL, the
** whole Authorization header value. */
void  s3_sign(const S3Sig *sg, char sig[65], char *auth, size_t acap);
/* A canonical query from "k=v" pairs (unencoded, NULL-terminated array):
** names and values URI-encoded, sorted. */
void  s3_canon_query(const char *const *kv, char *out, size_t cap);
/* A presigned GET URL (query string auth, X-Amz-SignedHeaders=host). */
void  s3_presign(const char *scheme_host, const char *host_hdr, const char *uri, const char *region,
                 const char *key_id, const char *secret, const char *amzdate, int expires, char *out, size_t cap);

/* One page of ListObjectsV2: folders (CommonPrefixes) and files under
** prefix, the folder marker itself skipped; next gets the continuation
** token ("" on the last page). Returns entries added, -1 on an error reply. */
int   s3_parse_list(const char *xml, size_t len, const char *prefix, FmCloudList *out, char *next, size_t ncap);
/* <Error><Code>..</Code><Message>..</Message></Error>; false when not one. */
bool  s3_parse_error(const char *xml, size_t len, char *code, size_t ccap, char *msg, size_t mcap);

/* Test hooks: the single-PUT limit, the multipart part size, the largest
** single CopyObject and the keys per listing page (0 = the defaults: 64 MB,
** 16 MB, 5 GB, 1000). With copy_max set, copies use part_size parts too. */
extern i64 g_s3_single_max, g_s3_part_size, g_s3_copy_max;
extern int g_s3_max_keys;
/* Test hooks: files above chunk_min go through Nextcloud chunked upload v2
** in chunk_size pieces (0 = 100 MB and 32 MB). */
extern i64 g_dav_chunk_min, g_dav_chunk_size;

#endif
