/* ftest_net.c -- JSON parser, helper processes and (optionally) HTTPS.
**
** The network part runs only with MMCFM_NET_TEST=1: the self test must pass
** offline. It asks the YouTube Data API with an invalid key, which proves
** TLS, headers, status codes and error parsing without needing a real key.
*/
#include "ftest.h"
#include "fjson.h"
#include "fnet.h"
#include "fproc.h"
#include "fnetstream.h"
#include "fsdl.h"
#include "fplat.h"
#include "fcrypt.h"

static void test_json(void) {
  /* the shape of a YouTube search response */
  const char *yt =
      "{\"kind\":\"youtube#searchListResponse\",\"nextPageToken\":\"CBkQAA\",\"pageInfo\":{\"totalResults\":1000000,"
      "\"resultsPerPage\":2},\"items\":[{\"id\":{\"kind\":\"youtube#video\",\"videoId\":\"dQw4w9WgXcQ\"},"
      "\"snippet\":{\"title\":\"Caf\\u00e9 \\\"live\\\" \\ud83c\\udfb5\",\"channelTitle\":\"Ch\",\"publishedAt\":"
      "\"2009-10-25T06:57:33Z\",\"thumbnails\":{\"medium\":{\"url\":\"https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg\","
      "\"width\":320,\"height\":180}},\"liveBroadcastContent\":\"none\"}},{\"id\":{\"videoId\":\"x2\"},\"snippet\":{}}]}";
  FmJson j;
  TEST_CHECK(json_parse(&j, yt, strlen(yt)) == FM_OK);
  const FmJsonNode *r = json_root(&j);
  TEST_CHECK(r && r->type == JSON_OBJ);
  TEST_CHECK(!strcmp(json_str(json_get(r, "nextPageToken"), ""), "CBkQAA"));
  TEST_CHECK(json_num(json_path(r, "pageInfo.totalResults"), 0) == 1000000);
  const FmJsonNode *items = json_get(r, "items");
  TEST_CHECK(items && items->type == JSON_ARR && items->count == 2);
  const FmJsonNode *it = json_first(items);
  TEST_CHECK(!strcmp(json_str(json_path(it, "id.videoId"), ""), "dQw4w9WgXcQ"));
  /* escapes: e-acute, quotes, a surrogate pair (U+1F3B5) */
  TEST_CHECK(!strcmp(json_str(json_path(it, "snippet.title"), ""), "Caf\xC3\xA9 \"live\" \xF0\x9F\x8E\xB5"));
  TEST_CHECK(json_num(json_path(it, "snippet.thumbnails.medium.width"), 0) == 320);
  TEST_CHECK(!strcmp(json_str(json_path(r, "items.1.id.videoId"), ""), "x2"));
  TEST_CHECK(json_path(r, "items.2") == NULL);
  TEST_CHECK(json_get(json_path(it, "snippet"), "missing") == NULL);
  TEST_CHECK(json_next(json_next(it)) == NULL);
  json_free(&j);

  /* scalars and odd but valid input */
  const char *ok[] = { "1", " -2.5e3 ", "\"x\"", "true", "null", "[]", "{}", "[[],[{}]]", "\xEF\xBB\xBF{\"a\":1}" };
  for (int i = 0; i < FM_COUNT(ok); i++) {
    TEST_CHECK(json_parse(&j, ok[i], strlen(ok[i])) == FM_OK);
    json_free(&j);
  }
  TEST_CHECK(json_parse(&j, " -2.5e3 ", 8) == FM_OK && json_num(json_root(&j), 0) == -2500);
  json_free(&j);
  TEST_CHECK(json_parse(&j, "\"42\"", 4) == FM_OK && json_num(json_root(&j), 0) == 42);
  json_free(&j);

  /* rejected */
  const char *bad[] = { "", "{", "[1,]", "{\"a\"}", "{\"a\":}", "tru", "\"abc", "\"\\x\"", "[1] 2", "{'a':1}",
                        "\"a\x01b\"", "[\"\\u12\"]" };
  for (int i = 0; i < FM_COUNT(bad); i++) {
    TEST_CHECK(json_parse(&j, bad[i], strlen(bad[i])) == FM_ERR_FORMAT);
    TEST_CHECK(j.nodes == NULL);
  }
  /* depth limit: 200 nested arrays fail cleanly instead of overflowing */
  char deep[401];
  for (int i = 0; i < 200; i++) { deep[i] = '['; deep[200 + i] = ']'; }
  deep[400] = 0;
  TEST_CHECK(json_parse(&j, deep, 400) == FM_ERR_FORMAT);
}

typedef struct Lines { int n; char first[64], last[64]; size_t longest; } Lines;

static bool on_line(void *user, const char *line, bool is_err) {
  Lines *l = (Lines *)user;
  FM_UNUSED(is_err);
  if (!l->n) fm_strlcpy(l->first, line, sizeof l->first);
  fm_strlcpy(l->last, line, sizeof l->last);
  if (strlen(line) > l->longest) l->longest = strlen(line);
  l->n++;
  return true;
}

static void test_proc(void) {
  if (!proc_available()) return;
#ifdef FM_WIN
  /* quoting: spaces, quotes and backslashes survive CreateProcess */
  const char *argv[] = { "cmd.exe", "/c", "echo", "one two", NULL };
  const char *argv_exit[] = { "cmd.exe", "/c", "exit 3", NULL };
  const char *argv_long[] = { "powershell.exe", "-NoProfile", "-Command", "'x' * 20000", NULL };
#else
  const char *argv[] = { "/bin/sh", "-c", "echo \"$0\"", "one two", NULL };
  const char *argv_exit[] = { "/bin/sh", "-c", "exit 3", NULL };
  const char *argv_long[] = { "/bin/sh", "-c", "head -c 20000 /dev/zero | tr '\\0' x; echo", NULL };
#endif
  Lines l;
  memset(&l, 0, sizeof l);
  int code = -1;
  TEST_CHECK(proc_run(argv, on_line, &l, &code, NULL) == FM_OK);
  TEST_CHECK(code == 0 && l.n >= 1 && strstr(l.first, "one two") != NULL);
  TEST_CHECK(proc_run(argv_exit, NULL, NULL, &code, NULL) == FM_OK && code == 3);
  /* one very long line arrives whole (yt-dlp -J prints one JSON line) */
  char *out = NULL;
  size_t len = 0;
  if (proc_capture(argv_long, 0, &out, &len, &code, NULL) == FM_OK) {
    TEST_CHECK(len >= 20000 && out[0] == 'x');
    fm_free(out);
  }
  const char *missing[] = { "mmcfm-no-such-program-xyz", NULL };
  TEST_CHECK(proc_run(missing, NULL, NULL, &code, NULL) != FM_OK && code == -1);
}

static void test_url(void) {
  char e[128];
  net_urlencode("lo-fi beats & chill/50%", e, sizeof e);
  TEST_CHECK(!strcmp(e, "lo-fi%20beats%20%26%20chill%2F50%25"));
  net_urlencode("caf\xC3\xA9", e, sizeof e);
  TEST_CHECK(!strcmp(e, "caf%C3%A9"));
}

static void test_https(const char *tmp) {
  const char *on = getenv("MMCFM_NET_TEST");
  if (!on || strcmp(on, "1") != 0) return;
  if (!net_available()) { printf("  network: %s (skipped)\n", net_backend()); return; }
  FmNetResp r;
  u64 t0 = plat_now_ms();
  FmErr e = net_get("https://www.googleapis.com/youtube/v3/search?part=snippet&q=test&key=INVALID", NULL, 0, &r,
                    NULL);
  TEST_CHECK(e == FM_OK);
  TEST_CHECK(r.status == 400);
  FmJson j;
  if (e == FM_OK && json_parse(&j, (const char *)r.data, r.len) == FM_OK) {
    const char *msg = json_str(json_path(json_root(&j), "error.message"), "");
    TEST_CHECK(strstr(msg, "API key") != NULL);
    printf("  https: %s, status %d, \"%s\" (%d ms)\n", net_backend(), r.status, msg, (int)(plat_now_ms() - t0));
    json_free(&j);
  } else {
    TEST_CHECK(!"youtube error JSON");
  }
  net_resp_free(&r);
  /* a small binary download to a file, with progress */
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "thumb.jpg");
  e = net_download("https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg", NULL, path, NULL, NULL, &r, NULL);
  TEST_CHECK(e == FM_OK && r.status == 200);
  FmStat st;
  TEST_CHECK(plat_stat(path, &st) && st.size > 1000);
  /* cancelled before it starts */
  volatile int cancel = 1;
  TEST_CHECK(net_get("https://www.googleapis.com/", NULL, 0, &r, &cancel) == FM_ERR_CANCEL);
}

/* MMCFM_NS_TEST=<url>|<outfile>|<pause ms>|<seek>: reads a stream through
** FmNetStream (pausing after 1 MB, optionally seeking to the end and back like
** demuxers do) and writes what it got, to compare with a plain download. */
static void test_ns_dump(void) {
  const char *spec = getenv("MMCFM_NS_TEST");
  if (!spec) return;
  char url[4096], out[FM_PATH_MAX];
  const char *b1 = strchr(spec, '|');
  if (!b1) return;
  fm_strlcpy(url, spec, FM_MIN(sizeof url, (size_t)(b1 - spec) + 1));
  const char *b2 = strchr(b1 + 1, '|');
  fm_strlcpy(out, b1 + 1, FM_MIN(sizeof out, (size_t)((b2 ? b2 : b1 + 1 + strlen(b1 + 1)) - (b1 + 1)) + 1));
  int pause = b2 ? atoi(b2 + 1) : 0;
  const char *b3 = b2 ? strchr(b2 + 1, '|') : NULL;
  int seekprobe = b3 ? atoi(b3 + 1) : 0;
  char err[160];
  FmNetStream *ns = ns_open(url, NULL, err, sizeof err);
  if (!ns) { printf("  ns: open failed: %s\n", err); return; }
  FILE *f = fm_fopen(out, "wb");
  u64 t0 = plat_now_ms();
  static u8 buf[65536];
  i64 total = 0;
  size_t n;
  bool paused = false;
  while ((n = ns_read(ns, buf, sizeof buf)) > 0) {
    fwrite(buf, 1, n, f);
    total += (i64)n;
    if (!paused && total >= (1 << 20)) {
      paused = true;
      if (seekprobe) {                  /* like a demuxer: peek at the end, come back */
        i64 here = ns_tell(ns);
        ns_seek(ns, ns_size(ns) - 4096);
        ns_read(ns, buf, 4096);
        ns_seek(ns, here);
      }
      if (pause > 0) SDL_Delay((Uint32)pause);
    }
  }
  fclose(f);
  u64 ms = plat_now_ms() - t0;
  printf("  ns: %lld bytes (size %lld) in %llu ms (%.1f Mbit/s), seekprobe %d, pause %d ms\n", (long long)total,
         (long long)ns_size(ns), (unsigned long long)ms, ms ? (double)total * 8 / 1000.0 / (double)ms : 0, seekprobe,
         pause);
  ns_close(ns);
}


/* HMAC-SHA256: RFC 4231 test cases 1 and 2 */
static void test_hmac256(void) {
  u8 key[20], mac[32];
  memset(key, 0x0b, sizeof key);
  hmac_sha256(key, 20, "Hi There", 8, mac);
  static const u8 k1[32] = { 0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
                             0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7 };
  TEST_CHECK(memcmp(mac, k1, 32) == 0);
  hmac_sha256((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, mac);
  static const u8 k2[32] = { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                             0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 };
  TEST_CHECK(memcmp(mac, k2, 32) == 0);
}

/* net_resp_header on a canned block */
static void test_resp_header(void) {
  FmNetResp r;
  memset(&r, 0, sizeof r);
  r.headers = fm_strdup("Content-Type: text/plain\r\nETag:  \"abc\"\r\nX-Empty:\r\nlocation: /next\r\n");
  char v[64];
  TEST_CHECK(net_resp_header(&r, "etag", v, sizeof v) && !strcmp(v, "\"abc\""));
  TEST_CHECK(net_resp_header(&r, "Location", v, sizeof v) && !strcmp(v, "/next"));
  TEST_CHECK(net_resp_header(&r, "X-Empty", v, sizeof v) && !v[0]);
  TEST_CHECK(!net_resp_header(&r, "Content", v, sizeof v));
  net_resp_free(&r);
  TEST_CHECK(!r.headers);
}

/* MMCFM_NETREQ_TEST=1: net_request against httpbin.org (PUT with a file
** body, DELETE, HEAD, a redirect reported instead of followed, a reply to a file) */
static void test_netreq_live(const char *tmp) {
  if (!getenv("MMCFM_NETREQ_TEST") || !net_available()) return;
  char src[FM_PATH_MAX], dst[FM_PATH_MAX];
  fm_path_join(src, sizeof src, tmp, "up.bin");
  fm_path_join(dst, sizeof dst, tmp, "reply.json");
  FILE *f = fm_fopen(src, "wb");
  for (int i = 0; i < 300000; i++) fputc('a' + i % 26, f);
  fclose(f);
  FmNetReq rq;
  memset(&rq, 0, sizeof rq);
  rq.method = "PUT";
  rq.headers = "Content-Type: application/octet-stream\r\nX-Test: mmcfm\r\n";
  rq.body_file = src;
  rq.body_off = 1000;
  rq.body_file_len = 200000;
  rq.out_file = dst;
  FmNetResp r;
  u64 t0 = plat_now_ms();
  FmErr e = net_request("https://httpbin.org/put", &rq, &r, NULL);
  char ct[96];
  bool hct = net_resp_header(&r, "Content-Type", ct, sizeof ct);
  printf("  netreq PUT 200000 B: %s status %d in %d ms, content-type %s\n", fm_err_str(e), r.status,
         (int)(plat_now_ms() - t0), hct ? ct : "-");
  TEST_CHECK(e == FM_OK && r.status == 200 && hct && strstr(ct, "json"));
  net_resp_free(&r);
  /* httpbin echoes the body back as "data" */
  u8 *echo = (u8 *)fm_alloc(4u << 20);
  FILE *rf = fm_fopen(dst, "rb");
  size_t n = rf ? fread(echo, 1, (4u << 20) - 1, rf) : 0;
  if (rf) fclose(rf);
  echo[n] = 0;
  TEST_CHECK(n > 200000 && strstr((const char *)echo, "\"X-Test\": \"mmcfm\"") && strstr((const char *)echo, "lmnopq"));
  fm_free(echo);
  memset(&rq, 0, sizeof rq);
  rq.method = "DELETE";
  e = net_request("https://httpbin.org/delete", &rq, &r, NULL);
  TEST_CHECK(e == FM_OK && r.status == 200 && r.data && strstr((const char *)r.data, "httpbin.org/delete"));
  net_resp_free(&r);
  memset(&rq, 0, sizeof rq);
  rq.no_redirect = true;
  e = net_request("https://httpbin.org/redirect-to?url=%2Fget&status_code=302", &rq, &r, NULL);
  char loc[256];
  TEST_CHECK(e == FM_OK && r.status == 302 && net_resp_header(&r, "Location", loc, sizeof loc) && !strcmp(loc, "/get"));
  printf("  netreq no-redirect: status %d location %s\n", r.status, loc);
  net_resp_free(&r);
  memset(&rq, 0, sizeof rq);
  rq.method = "HEAD";
  e = net_request("https://httpbin.org/bytes/1000", &rq, &r, NULL);
  TEST_CHECK(e == FM_OK && r.status == 200 && r.len == 0);
  net_resp_free(&r);
}

int test_net(const char *tmp) {
  int before = g_test_fail;
  test_ns_dump();
  test_json();
  test_url();
  test_proc();
  test_https(tmp);
  test_hmac256();
  test_resp_header();
  test_netreq_live(tmp);
  return g_test_fail - before;
}
