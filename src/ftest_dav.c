/* ftest_dav.c -- the WebDAV and S3 cloud adapters (area "dav").
**
** Offline by default: AWS Signature V4 against the published vectors (the
** SigV4 test suite's get/post-vanilla and the S3 developer guide's header
** and presigned-URL examples), ListObjectsV2 and PROPFIND multistatus
** replies shaped like MinIO, AWS, Nextcloud, Apache mod_dav and IIS answer,
** percent-encoding of paths, the LAN rule for plain http.
**
** Live, opt-in:
**   MMCFM_DAV_TEST="url|user|password"   a WebDAV server (a local wsgidav works)
**   MMCFM_S3_TEST="endpoint|region|bucket|key id|secret"   S3 (a local MinIO works)
** Each works in a fresh folder "mmcfm-test-<ms>/" and removes it: list,
** a 20 MB (WebDAV) / 40 MB multipart (S3) upload compared after download,
** mkdir, rename, move, remove, quota, stream_url fetched with net_request.
*/
#include "ftest.h"
#include "fcloud.h"
#include "fcloud_dav.h"
#include "fcrypt.h"
#include "fplat.h"

extern const FmCloud g_cloud_webdav, g_cloud_s3;

/* ---- SigV4 vectors --------------------------------------------------------------------- */

#define EMPTY_SHA "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

static void test_sigv4(void) {
  char sig[65], auth[400], q[400];
  /* AWS SigV4 test suite: get-vanilla, post-vanilla, get-vanilla-query-order-key-case */
  S3Hdr h1[] = { { "host", "example.amazonaws.com" }, { "x-amz-date", "20150830T123600Z" } };
  S3Sig sg = { "GET", "/", "", h1, 2, EMPTY_SHA, "20150830T123600Z", "us-east-1", "service", "AKIDEXAMPLE",
               "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY" };
  s3_sign(&sg, sig, auth, sizeof auth);
  TEST_CHECK(!strcmp(sig, "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"));
  TEST_CHECK(!strcmp(auth, "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
                           "SignedHeaders=host;x-amz-date, "
                           "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"));
  sg.method = "POST";
  s3_sign(&sg, sig, NULL, 0);
  TEST_CHECK(!strcmp(sig, "5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b"));
  const char *kv1[] = { "Param2=value2", "Param1=value1", NULL };
  s3_canon_query(kv1, q, sizeof q);
  TEST_CHECK(!strcmp(q, "Param1=value1&Param2=value2"));
  sg.method = "GET";
  sg.query = q;
  s3_sign(&sg, sig, NULL, 0);
  TEST_CHECK(!strcmp(sig, "b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500"));

  /* S3 developer guide, "Signature Calculations for the Authorization Header" */
  const char *k = "AKIAIOSFODNN7EXAMPLE", *sk = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
  S3Hdr h2[] = { { "range", "bytes=0-9" }, { "x-amz-date", "20130524T000000Z" }, { "host", "examplebucket.s3.amazonaws.com" },
                 { "x-amz-content-sha256", EMPTY_SHA } };
  S3Sig g2 = { "GET", "/test.txt", "", h2, 4, EMPTY_SHA, "20130524T000000Z", "us-east-1", "s3", k, sk };
  s3_sign(&g2, sig, auth, sizeof auth);
  TEST_CHECK(!strcmp(sig, "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
  TEST_CHECK(strstr(auth, "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,") != NULL);
  /* PUT object: "test$file.text" with a body hash and a storage class */
  char enc[64];
  TEST_CHECK(dav_pct_path("/test$file.text", enc, sizeof enc) && !strcmp(enc, "/test%24file.text"));
  const char *body_sha = "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072";
  S3Hdr h3[] = { { "date", "Fri, 24 May 2013 00:00:00 GMT" }, { "host", "examplebucket.s3.amazonaws.com" },
                 { "x-amz-date", "20130524T000000Z" }, { "x-amz-storage-class", "REDUCED_REDUNDANCY" },
                 { "x-amz-content-sha256", body_sha } };
  S3Sig g3 = { "PUT", enc, "", h3, 5, body_sha, "20130524T000000Z", "us-east-1", "s3", k, sk };
  s3_sign(&g3, sig, NULL, 0);
  TEST_CHECK(!strcmp(sig, "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
  {
    FmSha256 s;
    u8 d[32];
    char hx[65];
    sha256_init(&s);
    sha256_update(&s, "Welcome to Amazon S3.", 21);
    sha256_final(&s, d);
    for (int i = 0; i < 32; i++) fm_snprintf(hx + 2 * i, 3, "%02x", d[i]);
    TEST_CHECK(!strcmp(hx, body_sha));
  }
  /* GET bucket lifecycle (a value-less parameter) and a bucket listing */
  S3Hdr h4[] = { { "host", "examplebucket.s3.amazonaws.com" }, { "x-amz-date", "20130524T000000Z" },
                 { "x-amz-content-sha256", EMPTY_SHA } };
  const char *kv2[] = { "lifecycle", NULL };
  s3_canon_query(kv2, q, sizeof q);
  TEST_CHECK(!strcmp(q, "lifecycle="));
  S3Sig g4 = { "GET", "/", q, h4, 3, EMPTY_SHA, "20130524T000000Z", "us-east-1", "s3", k, sk };
  s3_sign(&g4, sig, NULL, 0);
  TEST_CHECK(!strcmp(sig, "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"));
  const char *kv3[] = { "prefix=J", "max-keys=2", NULL };
  s3_canon_query(kv3, q, sizeof q);
  TEST_CHECK(!strcmp(q, "max-keys=2&prefix=J"));
  s3_sign(&g4, sig, NULL, 0);
  TEST_CHECK(!strcmp(sig, "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"));

  /* S3 developer guide, "Authenticating Requests: Using Query Parameters" */
  char url[1024];
  s3_presign("https://examplebucket.s3.amazonaws.com", "examplebucket.s3.amazonaws.com", "/test.txt", "us-east-1", k,
             sk, "20130524T000000Z", 86400, url, sizeof url);
  TEST_CHECK(!strcmp(url, "https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256"
                          "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
                          "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host"
                          "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404"));

  /* query encoding: '/' and ' ' and UTF-8 in values, sorting by encoded name */
  const char *kv4[] = { "prefix=a b/\xC3\xA9", "delimiter=/", "list-type=2", "continuation-token=1+x=", NULL };
  s3_canon_query(kv4, q, sizeof q);
  TEST_CHECK(!strcmp(q, "continuation-token=1%2Bx%3D&delimiter=%2F&list-type=2&prefix=a%20b%2F%C3%A9"));

  int y, mo, d, hh, mi, ss;
  dav_civil(1369353600, &y, &mo, &d, &hh, &mi, &ss);           /* 2013-05-24 00:00:00 */
  TEST_CHECK(y == 2013 && mo == 5 && d == 24 && hh == 0 && mi == 0 && ss == 0);
  dav_civil(951782400 + 86399, &y, &mo, &d, &hh, &mi, &ss);   /* 2000-02-29 23:59:59 */
  TEST_CHECK(y == 2000 && mo == 2 && d == 29 && hh == 23 && mi == 59 && ss == 59);
  dav_civil(0, &y, &mo, &d, &hh, &mi, &ss);
  TEST_CHECK(y == 1970 && mo == 1 && d == 1);
}

/* ---- ListObjectsV2 ---------------------------------------------------------------------- */

static void test_s3_list(void) {
  /* MinIO-shaped, url-encoded keys, truncated */
  const char *x1 =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Name>bkt</Name><Prefix>photos%2F</Prefix>"
    "<NextContinuationToken>1Xx/+9=</NextContinuationToken><KeyCount>4</KeyCount><MaxKeys>4</MaxKeys>"
    "<Delimiter>%2F</Delimiter><IsTruncated>true</IsTruncated>"
    "<Contents><Key>photos%2F</Key><LastModified>2026-10-01T10:00:00.000Z</LastModified><ETag>&quot;d41d8cd98f00b204e9800998ecf8427e&quot;</ETag>"
    "<Size>0</Size><StorageClass>STANDARD</StorageClass></Contents>"
    "<Contents><Key>photos%2Fa%2Bb+%26%20c%C3%A9.jpg</Key><LastModified>2026-10-02T12:30:45.000Z</LastModified>"
    "<ETag>&quot;0123456789abcdef0123456789abcdef-3&quot;</ETag><Size>5368709120</Size><StorageClass>STANDARD</StorageClass></Contents>"
    "<CommonPrefixes><Prefix>photos%2F2026%2F</Prefix></CommonPrefixes>"
    "<CommonPrefixes><Prefix>photos%2Fsub%20dir%2F</Prefix></CommonPrefixes>"
    "<EncodingType>url</EncodingType></ListBucketResult>";
  FmCloudList l;
  char next[256];
  memset(&l, 0, sizeof l);
  TEST_CHECK(s3_parse_list(x1, strlen(x1), "photos/", &l, next, sizeof next) == 3);
  TEST_CHECK(!strcmp(next, "1Xx/+9="));
  TEST_CHECK(l.count == 3);
  if (l.count == 3) {
    TEST_CHECK(!strcmp(l.items[0].id, "photos/a+b & c\xC3\xA9.jpg") && !strcmp(l.items[0].name, "a+b & c\xC3\xA9.jpg"));
    TEST_CHECK(!l.items[0].dir && l.items[0].size == 5368709120ull);
    TEST_CHECK(l.items[0].mtime == 1790944245);                 /* 2026-10-02T12:30:45Z */
    TEST_CHECK(!strcmp(l.items[0].hash, "etag:0123456789abcdef0123456789abcdef-3"));
    TEST_CHECK(l.items[1].dir && !strcmp(l.items[1].id, "photos/2026/") && !strcmp(l.items[1].name, "2026"));
    TEST_CHECK(l.items[2].dir && !strcmp(l.items[2].id, "photos/sub dir/") && !strcmp(l.items[2].name, "sub dir"));
  }
  cloud_list_free(&l);
  /* AWS-shaped, no encoding: '+' and '%' are literal; last page */
  const char *x2 =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Name>b</Name><Prefix></Prefix><KeyCount>2</KeyCount>"
    "<MaxKeys>1000</MaxKeys><Delimiter>/</Delimiter><IsTruncated>false</IsTruncated>"
    "<Contents><Key>100%+real.txt</Key><LastModified>2009-10-12T17:50:30.000Z</LastModified><ETag>\"fba9dede5f27731c9771645a39863328\"</ETag>"
    "<Size>434234</Size><StorageClass>STANDARD</StorageClass></Contents>"
    "<CommonPrefixes><Prefix>docs/</Prefix></CommonPrefixes></ListBucketResult>";
  memset(&l, 0, sizeof l);
  TEST_CHECK(s3_parse_list(x2, strlen(x2), "", &l, next, sizeof next) == 2 && next[0] == 0);
  TEST_CHECK(l.count == 2 && !strcmp(l.items[0].name, "100%+real.txt") && l.items[0].size == 434234);
  TEST_CHECK(l.count == 2 && !strcmp(l.items[0].hash, "etag:fba9dede5f27731c9771645a39863328"));
  TEST_CHECK(l.count == 2 && l.items[1].dir && !strcmp(l.items[1].id, "docs/"));
  cloud_list_free(&l);
  /* errors and garbage */
  const char *x3 = "<?xml version=\"1.0\"?><Error><Code>NoSuchBucket</Code><Message>The specified bucket does not exist"
                   "</Message><BucketName>nope</BucketName></Error>";
  char code[64], msg[128];
  memset(&l, 0, sizeof l);
  TEST_CHECK(s3_parse_list(x3, strlen(x3), "", &l, next, sizeof next) == -1 && l.count == 0);
  TEST_CHECK(s3_parse_error(x3, strlen(x3), code, sizeof code, msg, sizeof msg));
  TEST_CHECK(!strcmp(code, "NoSuchBucket") && !strcmp(msg, "The specified bucket does not exist"));
  TEST_CHECK(!s3_parse_error(x2, strlen(x2), code, sizeof code, msg, sizeof msg));
  TEST_CHECK(s3_parse_list("garbage<<", 9, "", &l, next, sizeof next) == -1);
  for (size_t cut = 0; cut < strlen(x1); cut += 7) {           /* every truncation stays safe */
    s3_parse_list(x1, cut, "photos/", &l, next, sizeof next);
  }
  cloud_list_free(&l);
}

/* ---- PROPFIND ------------------------------------------------------------------------------ */

static const FmCloudEntry *find(const FmCloudList *l, const char *name) {
  for (int i = 0; i < l->count; i++)
    if (!strcmp(l->items[i].name, name)) return &l->items[i];
  return NULL;
}

static void test_propfind(void) {
  /* Nextcloud: d:/oc:/nc: prefixes, a 404 propstat, encoded hrefs */
  const char *nc =
    "<?xml version=\"1.0\"?>\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\" xmlns:oc=\"http://owncloud.org/ns\" "
    "xmlns:nc=\"http://nextcloud.org/ns\">"
    "<d:response><d:href>/remote.php/dav/files/john%40ex.com/Photos/</d:href><d:propstat><d:prop>"
    "<d:resourcetype><d:collection/></d:resourcetype><d:getlastmodified>Tue, 06 Oct 2026 09:00:00 GMT</d:getlastmodified>"
    "<d:getetag>&quot;6512a&quot;</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
    "<d:propstat><d:prop><d:getcontentlength/><d:getcontenttype/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
    "<d:response><d:href>/remote.php/dav/files/john%40ex.com/Photos/My%20Trip%20%231/</d:href><d:propstat><d:prop>"
    "<d:resourcetype><d:collection/></d:resourcetype><d:getlastmodified>Wed, 07 Oct 2026 10:11:12 GMT</d:getlastmodified>"
    "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
    "<d:response><d:href>/remote.php/dav/files/john%40ex.com/Photos/Caf%C3%A9%20100%25.jpg</d:href><d:propstat><d:prop>"
    "<d:resourcetype/><d:getcontentlength>2349811</d:getcontentlength><d:getcontenttype>image/jpeg</d:getcontenttype>"
    "<d:getlastmodified>Thu, 08 Oct 2026 01:02:03 GMT</d:getlastmodified><d:getetag>&quot;9f1e2c&quot;</d:getetag>"
    "<d:displayname>ignored.jpg</d:displayname>"
    "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
    "</d:multistatus>";
  FmCloudList l;
  memset(&l, 0, sizeof l);
  TEST_CHECK(dav_parse_multistatus(nc, strlen(nc), "/remote.php/dav/files/john@ex.com/", "Photos/", &l) == 2);
  const FmCloudEntry *e = find(&l, "My Trip #1");
  TEST_CHECK(e && e->dir && !strcmp(e->id, "Photos/My Trip #1/") && e->mtime == 1791367872);
  e = find(&l, "Caf\xC3\xA9 100%.jpg");
  TEST_CHECK(e && !e->dir && !strcmp(e->id, "Photos/Caf\xC3\xA9 100%.jpg") && e->size == 2349811);
  TEST_CHECK(e && !strcmp(e->mime, "image/jpeg") && !strcmp(e->hash, "etag:9f1e2c") && e->mtime == 1791421323);
  cloud_list_free(&l);

  /* Apache mod_dav: D: and lp1:/lp2: prefixes, absolute URL hrefs, root listing */
  const char *ap =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<D:multistatus xmlns:D=\"DAV:\" xmlns:ns0=\"DAV:\">\n"
    "<D:response xmlns:lp1=\"DAV:\" xmlns:lp2=\"http://apache.org/dav/props/\">\n"
    "<D:href>http://nas.local/dav/</D:href>\n<D:propstat>\n<D:prop>\n<lp1:resourcetype><D:collection/></lp1:resourcetype>\n"
    "</D:prop>\n<D:status>HTTP/1.1 200 OK</D:status>\n</D:propstat>\n</D:response>\n"
    "<D:response xmlns:lp1=\"DAV:\"><D:href>http://nas.local/dav/notes.txt</D:href><D:propstat><D:prop>"
    "<lp1:resourcetype/><lp1:getcontentlength>12</lp1:getcontentlength>"
    "<lp1:getlastmodified xmlns:b=\"urn:uuid:c2f41010-65b3-11d1-a29f-00aa00c14882/\" b:dt=\"dateTime.rfc1123\">"
    "Fri, 09 Oct 2026 08:00:00 GMT</lp1:getlastmodified><lp1:getetag>W/\"c-5a\"</lp1:getetag></D:prop>"
    "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
    "<D:response><D:href>/dav/Music/</D:href><D:propstat><D:prop><lp1:resourcetype xmlns:lp1=\"DAV:\">"
    "<D:collection/></lp1:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
    "</D:multistatus>";
  memset(&l, 0, sizeof l);
  TEST_CHECK(dav_parse_multistatus(ap, strlen(ap), "/dav/", "", &l) == 2);
  e = find(&l, "notes.txt");
  TEST_CHECK(e && !e->dir && !strcmp(e->id, "notes.txt") && e->size == 12 && !strcmp(e->hash, "etag:c-5a"));
  e = find(&l, "Music");
  TEST_CHECK(e && e->dir && !strcmp(e->id, "Music/"));
  cloud_list_free(&l);

  /* default namespace, base without the trailing slash in the self href, lower-case escapes */
  const char *dn =
    "<multistatus xmlns=\"DAV:\"><response><href>/webdav</href><propstat><prop><resourcetype><collection/>"
    "</resourcetype></prop></propstat></response>"
    "<response><href>/webdav/a%c3%a9b.txt</href><propstat><prop><resourcetype></resourcetype>"
    "<getcontentlength>7</getcontentlength></prop></propstat></response></multistatus>";
  memset(&l, 0, sizeof l);
  TEST_CHECK(dav_parse_multistatus(dn, strlen(dn), "/webdav/", "", &l) == 1);
  TEST_CHECK(l.count == 1 && !strcmp(l.items[0].name, "a\xC3\xA9" "b.txt") && l.items[0].size == 7);
  cloud_list_free(&l);

  /* a proxy rewriting the path: names still come through, the folder itself skipped */
  const char *px =
    "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/other/x/</d:href></d:response>"
    "<d:response><d:href>/other/x/y.bin</d:href><d:propstat><d:prop><d:getcontentlength>3</d:getcontentlength>"
    "</d:prop></d:propstat></d:response></d:multistatus>";
  memset(&l, 0, sizeof l);
  TEST_CHECK(dav_parse_multistatus(px, strlen(px), "/dav/", "x/", &l) == 1);
  TEST_CHECK(l.count == 1 && !strcmp(l.items[0].id, "x/y.bin"));
  cloud_list_free(&l);

  /* quota */
  const char *q =
    "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/dav/</d:href><d:propstat><d:prop>"
    "<d:quota-available-bytes>750</d:quota-available-bytes><d:quota-used-bytes>250</d:quota-used-bytes>"
    "</d:prop></d:propstat></d:response></d:multistatus>";
  u64 used = 0, total = 0;
  TEST_CHECK(dav_parse_quota(q, strlen(q), &used, &total) && used == 250 && total == 1000);
  const char *q2 =
    "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/dav/</d:href><d:propstat><d:prop>"
    "<d:quota-available-bytes>-3</d:quota-available-bytes><d:quota-used-bytes>4096</d:quota-used-bytes>"
    "</d:prop></d:propstat></d:response></d:multistatus>";
  TEST_CHECK(dav_parse_quota(q2, strlen(q2), &used, &total) && used == 4096 && total == 0);
  TEST_CHECK(!dav_parse_quota(ap, strlen(ap), &used, &total));

  /* not a multistatus, and every truncation of a real one */
  memset(&l, 0, sizeof l);
  TEST_CHECK(dav_parse_multistatus("<html><body>Login</body></html>", 31, "/", "", &l) == -1);
  for (size_t cut = 0; cut < strlen(nc); cut += 5) dav_parse_multistatus(nc, cut, "/remote.php/dav/files/john@ex.com/",
                                                                       "Photos/", &l);
  cloud_list_free(&l);
}

/* ---- paths, URLs, the http rule --------------------------------------------------------------- */

static void test_paths(void) {
  char o[256];
  TEST_CHECK(dav_pct_path("a b/#c%d/\xC3\xA9?x&y+z", o, sizeof o) && !strcmp(o, "a%20b/%23c%25d/%C3%A9%3Fx%26y%2Bz"));
  TEST_CHECK(dav_pct_path("safe-_.~/AZaz09", o, sizeof o) && !strcmp(o, "safe-_.~/AZaz09"));
  TEST_CHECK(!dav_pct_path("##########", o, 20));
  TEST_CHECK(dav_pct_decode("a%20b%2", 7, o, sizeof o) == 5 && !strcmp(o, "a b%2"));
  TEST_CHECK(dav_pct_decode("%zz%41+", 7, o, sizeof o) && !strcmp(o, "%zzA+"));
  TEST_CHECK(dav_atou64(" 18446744073709551615") == 18446744073709551615ull && dav_atou64("x") == 0);

  bool https;
  char host[64], port[8];
  const char *path;
  TEST_CHECK(dav_split_url("https://u:p@cloud.example.com:8443/remote.php/dav?x", &https, host, sizeof host, port,
                           sizeof port, &path));
  TEST_CHECK(https && !strcmp(host, "cloud.example.com") && !strcmp(port, "8443") && !strcmp(path, "/remote.php/dav?x"));
  TEST_CHECK(dav_split_url("http://[::1]:9000", &https, host, sizeof host, port, sizeof port, &path));
  TEST_CHECK(!https && !strcmp(host, "::1") && !strcmp(port, "9000") && !strcmp(path, "/"));
  TEST_CHECK(!dav_split_url("ftp://x/", &https, host, sizeof host, port, sizeof port, &path));

  const char *local[] = { "localhost", "127.0.0.1", "10.1.2.3", "192.168.1.20", "172.16.0.1", "172.31.255.1",
                          "169.254.1.1", "100.100.1.1", "::1", "fe80::1", "fd12::3", "nas", "diskstation.local",
                          "pi.lan", "box.home.arpa" };
  const char *pub[] = { "example.com", "8.8.8.8", "172.32.0.1", "192.169.0.1", "100.128.0.1", "2001:db8::1",
                        "1.2.3", "localhost.example.com", "10.0.0.1.example.com" };
  for (int i = 0; i < FM_COUNT(local); i++) {
    if (!dav_host_is_local(local[i])) { printf("  local? %s\n", local[i]); TEST_CHECK(0); }
  }
  for (int i = 0; i < FM_COUNT(pub); i++) {
    if (dav_host_is_local(pub[i])) { printf("  public? %s\n", pub[i]); TEST_CHECK(0); }
  }

  /* the adapters refuse plain http on the internet before any request */
  FmCloudAcct a;
  char err[256];
  volatile int cancel = 0;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.server, "http://dav.example.com/remote.php/webdav/", sizeof a.server);
  fm_strlcpy(a.user, "u", sizeof a.user);
  fm_strlcpy(a.secret, "p", sizeof a.secret);
  TEST_CHECK(g_cloud_webdav.login(&a, err, sizeof err, &cancel) == FM_ERR_ACCESS && strstr(err, "https://"));
  fm_strlcpy(a.server, "http://minio.example.com|us-east-1|b", sizeof a.server);
  TEST_CHECK(g_cloud_s3.login(&a, err, sizeof err, &cancel) == FM_ERR_ACCESS);
  /* stream URLs are built offline */
  FmCloudEntry e;
  char url[1024], hdr[512];
  memset(&e, 0, sizeof e);
  fm_strlcpy(e.id, "Music/a b#1.mp3", sizeof e.id);
  fm_strlcpy(a.server, "https://cloud.example.com/remote.php/dav/files/me", sizeof a.server);
  fm_strlcpy(a.user, "me", sizeof a.user);
  fm_strlcpy(a.secret, "secret", sizeof a.secret);
  TEST_CHECK(g_cloud_webdav.stream_url(&a, &e, url, sizeof url, hdr, sizeof hdr, err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(!strcmp(url, "https://cloud.example.com/remote.php/dav/files/me/Music/a%20b%231.mp3"));
  TEST_CHECK(!strcmp(hdr, "Authorization: Basic bWU6c2VjcmV0\r\n"));
  fm_strlcpy(a.server, " http://192.168.1.5:9000/ | eu-central-1 | my.bucket ", sizeof a.server);
  TEST_CHECK(g_cloud_s3.stream_url(&a, &e, url, sizeof url, hdr, sizeof hdr, err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(!strncmp(url, "http://192.168.1.5:9000/my.bucket/Music/a%20b%231.mp3?X-Amz-Algorithm=AWS4-HMAC-SHA256"
                           "&X-Amz-Credential=me%2F", 107) && strstr(url, "%2Feu-central-1%2Fs3%2Faws4_request") &&
             strstr(url, "&X-Amz-Expires=3600&") && hdr[0] == 0);
}

/* ---- live -------------------------------------------------------------------------------------- */

static bool make_file(const char *path, u64 size, u32 seed) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  u8 *buf = (u8 *)fm_alloc(1 << 20);
  u32 x = seed;
  for (u64 done = 0; done < size;) {
    size_t n = (size_t)FM_MIN((u64)(1 << 20), size - done);
    for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; buf[i] = (u8)(x >> 24); }
    fwrite(buf, 1, n, f);
    done += n;
  }
  fm_free(buf);
  return fclose(f) == 0;
}

static bool same_file(const char *a, const char *b) {
  FILE *fa = fm_fopen(a, "rb"), *fb = fm_fopen(b, "rb");
  bool same = fa && fb;
  u8 *x = (u8 *)fm_alloc(1 << 16), *y = (u8 *)fm_alloc(1 << 16);
  while (same) {
    size_t n = fread(x, 1, 1 << 16, fa), m = fread(y, 1, 1 << 16, fb);
    if (n != m || memcmp(x, y, n)) same = false;
    if (!n) break;
  }
  if (fa) fclose(fa);
  if (fb) fclose(fb);
  fm_free(x);
  fm_free(y);
  return same;
}

typedef struct Prog { u64 last, total; int calls; bool backwards; } Prog;

static bool prog_cb(void *user, u64 done, u64 total) {
  Prog *p = (Prog *)user;
  if (done < p->last) p->backwards = true;
  p->last = done;
  p->total = total;
  p->calls++;
  return true;
}

static bool cancel_cb(void *user, u64 done, u64 total) {
  FM_UNUSED(total);
  return done < *(u64 *)user;
}

static const FmCloudEntry *list_find(const FmCloud *c, FmCloudAcct *a, const char *dir, const char *name,
                                     FmCloudList *l) {
  volatile int cancel = 0;
  cloud_list_free(l);
  memset(l, 0, sizeof *l);
  FmErr e = c->list(a, dir, l, &cancel);
  if (e) printf("  list %s: %s\n", dir, l->error);
  return e ? NULL : find(l, name);
}

/* The same tour on both services. */
static void live_tour(const FmCloud *c, FmCloudAcct *a, const char *tmp, u64 big, const char *label) {
  char err[256], dir[64], local[FM_PATH_MAX], back[FM_PATH_MAX];
  volatile int cancel = 0;
  FmCloudList l;
  FmCloudEntry d, f, sub, ent;
  memset(&l, 0, sizeof l);
  u64 t0 = plat_now_ms();

  FmErr e = c->login(a, err, sizeof err, &cancel);
  if (e) printf("  %s login: %s\n", label, err);
  TEST_CHECK(e == FM_OK);
  if (e) return;
  fm_snprintf(dir, sizeof dir, "mmcfm-test-%llu", (unsigned long long)plat_now_ms());
  TEST_CHECK(c->mkdir(a, "", dir, &d, err, sizeof err, &cancel) == FM_OK && d.dir);
  TEST_CHECK(c->mkdir(a, "", dir, &ent, err, sizeof err, &cancel) != FM_OK || c == &g_cloud_s3);  /* S3 just rewrites */
  const FmCloudEntry *le = list_find(c, a, "", dir, &l);
  TEST_CHECK(le && le->dir && !strcmp(le->id, d.id));

  /* a sub folder with awkward characters */
  const char *subname = "sub dir #1 %25 \xC3\xA9";
  TEST_CHECK(c->mkdir(a, d.id, subname, &sub, err, sizeof err, &cancel) == FM_OK);

  /* the big file, with progress */
  fm_path_join(local, sizeof local, tmp, "big.bin");
  fm_path_join(back, sizeof back, tmp, "big.back");
  TEST_CHECK(make_file(local, big, 7));
  Prog pr = { 0, 0, 0, false };
  u64 t1 = plat_now_ms();
  e = c->upload(a, d.id, local, "big file & co.bin", &f, prog_cb, &pr, err, sizeof err, &cancel);
  if (e) printf("  %s upload: %s\n", label, err);
  TEST_CHECK(e == FM_OK && f.size == big);
  TEST_CHECK(pr.calls > 0 && pr.last == big && pr.total == big && !pr.backwards);
  printf("  %s: uploaded %llu MB in %llu ms (%d progress calls)\n", label, (unsigned long long)(big >> 20),
         (unsigned long long)(plat_now_ms() - t1), pr.calls);
  le = list_find(c, a, d.id, "big file & co.bin", &l);
  TEST_CHECK(le && !le->dir && le->size == big && !strcmp(le->id, f.id));
  if (le) {
    ent = *le;
    memset(&pr, 0, sizeof pr);
    t1 = plat_now_ms();
    e = c->download(a, &ent, back, prog_cb, &pr, err, sizeof err, &cancel);
    if (e) printf("  %s download: %s\n", label, err);
    TEST_CHECK(e == FM_OK && same_file(local, back));
    char part[FM_PATH_MAX];
    fm_snprintf(part, sizeof part, "%s.part", back);
    TEST_CHECK(pr.calls > 0 && !plat_exists(part));
    printf("  %s: downloaded in %llu ms, bytes identical: %s\n", label, (unsigned long long)(plat_now_ms() - t1),
           same_file(local, back) ? "yes" : "NO");
    /* stream_url: the first and some middle bytes through net_request */
    char url[2048], hdr[1200], rh[1300];
    TEST_CHECK(c->stream_url(a, &ent, url, sizeof url, hdr, sizeof hdr, err, sizeof err, &cancel) == FM_OK);
    fm_snprintf(rh, sizeof rh, "%sRange: bytes=1000000-1000099\r\n", hdr);
    FmNetReq rq;
    FmNetResp r;
    memset(&rq, 0, sizeof rq);
    rq.headers = rh;
    TEST_CHECK(net_request(url, &rq, &r, &cancel) == FM_OK && r.status == 206 && r.len == 100);
    if (r.len == 100) {
      FILE *fl = fm_fopen(local, "rb");
      u8 want[100];
      fm_fseek64(fl, 1000000, SEEK_SET);
      TEST_CHECK(fread(want, 1, 100, fl) == 100 && !memcmp(want, r.data, 100));
      fclose(fl);
    } else {
      printf("  %s stream_url: HTTP %d %s\n", label, r.status, r.error);
    }
    net_resp_free(&r);
  }
  /* rename the big file away and back (S3: a multipart server-side copy) */
  le = list_find(c, a, d.id, "big file & co.bin", &l);
  if (le) {
    ent = *le;
    e = c->rename(a, &ent, "big2.bin", err, sizeof err, &cancel);
    if (e) printf("  %s big rename: %s\n", label, err);
    TEST_CHECK(e == FM_OK);
    le = list_find(c, a, d.id, "big2.bin", &l);
    TEST_CHECK(le && le->size == big && list_find(c, a, d.id, "big file & co.bin", &l) == NULL);
    le = list_find(c, a, d.id, "big2.bin", &l);
    if (le) {
      ent = *le;
      TEST_CHECK(c->rename(a, &ent, "big file & co.bin", err, sizeof err, &cancel) == FM_OK);
      le = list_find(c, a, d.id, "big file & co.bin", &l);
      TEST_CHECK(le && le->size == big);
      if (le) {
        ent = *le;
        TEST_CHECK(c->download(a, &ent, back, NULL, NULL, err, sizeof err, &cancel) == FM_OK && same_file(local, back));
      }
    }
  }
  /* a cancelled upload leaves an error, not a file */
  u64 stop = big / 3;
  e = c->upload(a, d.id, local, "cancelled.bin", NULL, cancel_cb, &stop, err, sizeof err, &cancel);
  TEST_CHECK(e == FM_ERR_CANCEL);
  le = list_find(c, a, d.id, "cancelled.bin", &l);
  TEST_CHECK(le == NULL || le->size != big);

  /* small file: replace, rename, move into the sub folder */
  fm_path_join(local, sizeof local, tmp, "small.txt");
  TEST_CHECK(make_file(local, 1234, 1));
  TEST_CHECK(c->upload(a, d.id, local, "a.txt", &f, NULL, NULL, err, sizeof err, &cancel) == FM_OK);
  TEST_CHECK(make_file(local, 99, 2));
  TEST_CHECK(c->upload(a, d.id, local, "a.txt", &f, NULL, NULL, err, sizeof err, &cancel) == FM_OK);
  le = list_find(c, a, d.id, "a.txt", &l);
  TEST_CHECK(le && le->size == 99);
  if (le) {
    ent = *le;
    e = c->rename(a, &ent, "b c+%.txt", err, sizeof err, &cancel);
    if (e) printf("  %s rename: %s\n", label, err);
    TEST_CHECK(e == FM_OK);
    TEST_CHECK(list_find(c, a, d.id, "a.txt", &l) == NULL);
    le = list_find(c, a, d.id, "b c+%.txt", &l);
    TEST_CHECK(le && le->size == 99);
  }
  if (le) {
    ent = *le;
    /* renaming onto an existing name is refused */
    TEST_CHECK(c->rename(a, &ent, "big file & co.bin", err, sizeof err, &cancel) == FM_ERR_EXISTS);
    e = c->move(a, &ent, sub.id, err, sizeof err, &cancel);
    if (e) printf("  %s move: %s\n", label, err);
    TEST_CHECK(e == FM_OK);
    TEST_CHECK(list_find(c, a, d.id, "b c+%.txt", &l) == NULL);
    le = list_find(c, a, sub.id, "b c+%.txt", &l);
    TEST_CHECK(le && le->size == 99);
  }
  /* rename the folder with its content */
  le = list_find(c, a, d.id, subname, &l);
  TEST_CHECK(le && le->dir);
  if (le) {
    ent = *le;
    e = c->rename(a, &ent, "renamed", err, sizeof err, &cancel);
    if (e) printf("  %s folder rename: %s\n", label, err);
    TEST_CHECK(e == FM_OK);
    TEST_CHECK(list_find(c, a, d.id, subname, &l) == NULL);
    char rid[CLOUD_ID_MAX];
    fm_snprintf(rid, sizeof rid, "%srenamed/", d.id);
    le = list_find(c, a, rid, "b c+%.txt", &l);
    TEST_CHECK(le && le->size == 99);
    /* move the folder up to the root, then remove it */
    le = list_find(c, a, d.id, "renamed", &l);
    if (le) {
      ent = *le;
      TEST_CHECK(c->move(a, &ent, d.id, err, sizeof err, &cancel) != FM_OK);   /* onto itself */
      char nm[80];
      fm_snprintf(nm, sizeof nm, "%s-moved", dir);
      TEST_CHECK(c->rename(a, &ent, nm, err, sizeof err, &cancel) == FM_OK);
      le = list_find(c, a, d.id, nm, &l);
      TEST_CHECK(le && le->dir);
      if (le) {
        ent = *le;
        TEST_CHECK(c->move(a, &ent, "", err, sizeof err, &cancel) == FM_OK);
        le = list_find(c, a, "", nm, &l);
        TEST_CHECK(le && le->dir);
        if (le) {
          ent = *le;
          TEST_CHECK(c->remove(a, &ent, err, sizeof err, &cancel) == FM_OK);
          TEST_CHECK(list_find(c, a, "", nm, &l) == NULL);
        }
      }
    }
  }
  if (c->quota) {
    u64 used = 0, total = 0;
    e = c->quota(a, &used, &total, err, sizeof err, &cancel);
    printf("  %s quota: %s used %llu total %llu\n", label, e ? err : "ok", (unsigned long long)used,
           (unsigned long long)total);
    TEST_CHECK(e == FM_OK || e == FM_ERR_UNSUPPORTED);
  }
  /* remove the test folder and everything in it */
  e = c->remove(a, &d, err, sizeof err, &cancel);
  if (e) printf("  %s remove: %s\n", label, err);
  TEST_CHECK(e == FM_OK);
  TEST_CHECK(list_find(c, a, "", dir, &l) == NULL);
  cloud_list_free(&l);

  /* wrong password */
  FmCloudAcct bad = *a;
  fm_strlcpy(bad.secret, "wrong-secret", sizeof bad.secret);
  e = c->login(&bad, err, sizeof err, &cancel);
  printf("  %s wrong password -> %s\n", label, err);
  TEST_CHECK(e == FM_ERR_PASSWORD);
  printf("  %s live tour: %llu ms\n", label, (unsigned long long)(plat_now_ms() - t0));
}

static bool split_spec(const char *spec, char f[][512], int n) {
  for (int i = 0; i < n; i++) {
    const char *b = strchr(spec, '|');
    size_t len = b ? (size_t)(b - spec) : strlen(spec);
    if (len >= 512) return false;
    memcpy(f[i], spec, len);
    f[i][len] = 0;
    if (!b) return i == n - 1;
    spec = b + 1;
  }
  return false;
}

static void live_dav(const char *tmp) {
  const char *spec = getenv("MMCFM_DAV_TEST");
  char f[3][512];
  if (!spec || !*spec) return;
  if (!split_spec(spec, f, 3)) { printf("  MMCFM_DAV_TEST wants url|user|password\n"); TEST_CHECK(0); return; }
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "webdav", sizeof a.provider);
  fm_strlcpy(a.server, f[0], sizeof a.server);
  fm_strlcpy(a.user, f[1], sizeof a.user);
  fm_strlcpy(a.secret, f[2], sizeof a.secret);
  const char *cm = getenv("MMCFM_DAV_CHUNK");          /* force chunked upload above this many bytes */
  if (cm) { g_dav_chunk_min = (i64)dav_atou64(cm); g_dav_chunk_size = 5ll << 20; }
  live_tour(&g_cloud_webdav, &a, tmp, 20u << 20, "webdav");
  g_dav_chunk_min = g_dav_chunk_size = 0;
}

static void live_s3(const char *tmp) {
  const char *spec = getenv("MMCFM_S3_TEST");
  char f[5][512];
  if (!spec || !*spec) return;
  if (!split_spec(spec, f, 5)) { printf("  MMCFM_S3_TEST wants endpoint|region|bucket|key id|secret\n"); TEST_CHECK(0); return; }
  FmCloudAcct a;
  memset(&a, 0, sizeof a);
  fm_strlcpy(a.provider, "s3", sizeof a.provider);
  fm_snprintf(a.server, sizeof a.server, "%s|%s|%s", f[0], f[1], f[2]);
  fm_strlcpy(a.user, f[3], sizeof a.user);
  fm_strlcpy(a.secret, f[4], sizeof a.secret);
  g_s3_single_max = 8ll << 20;                          /* 40 MB -> 8 parts of 5 MB (the S3 minimum) */
  g_s3_part_size = 5ll << 20;
  g_s3_max_keys = 2;                                    /* exercise continuation tokens */
  g_s3_copy_max = 8ll << 20;                            /* rename of 40 MB -> UploadPartCopy */
  live_tour(&g_cloud_s3, &a, tmp, 40u << 20, "s3");
  /* a missing bucket */
  char err[256];
  volatile int cancel = 0;
  fm_snprintf(a.server, sizeof a.server, "%s|%s|no-such-bucket-mmcfm-%llu", f[0], f[1], (unsigned long long)plat_now_ms());
  FmErr e = g_cloud_s3.login(&a, err, sizeof err, &cancel);
  printf("  s3 missing bucket -> %s\n", err);
  TEST_CHECK(e == FM_ERR_NOT_FOUND);
  g_s3_single_max = g_s3_part_size = g_s3_copy_max = 0;
  g_s3_max_keys = 0;
}

int test_dav(const char *tmp) {
  int before = g_test_fail;
  test_sigv4();
  test_s3_list();
  test_propfind();
  test_paths();
  live_dav(tmp);
  live_s3(tmp);
  return g_test_fail - before;
}
