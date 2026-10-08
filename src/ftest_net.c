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
#include "fplat.h"

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

int test_net(const char *tmp) {
  int before = g_test_fail;
  test_json();
  test_url();
  test_proc();
  test_https(tmp);
  return g_test_fail - before;
}
