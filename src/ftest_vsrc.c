/* ftest_vsrc.c -- online video sources: parsers, helpers, cache, and (opt-in)
** real searches, resolves and playback.
**
** Offline by default: every adapter parses canned replies (and truncated or
** garbage ones). MMCFM_NET_TEST=1 adds a real search on every keyless source
** and plays an Internet Archive and a PeerTube stream. MMCFM_YTDLP=<path to
** yt-dlp> adds a keyless YouTube search, a resolve into a temp cache, pair
** playback, a Dailymotion (HLS) resolve and downloads with a non-ASCII title
** (MMCFM_JS=node:<path> picks the JS runtime, MMCFM_FFMPEG=<dir> enables the
** merged download).
*/
#include "ftest.h"
#include "fvsrc.h"
#include "fvsrc_int.h"
#include "fcrypt.h"
#include "fplat.h"
#include "fdec_vid.h"
#include "fproc.h"

/* ---- canned replies ---------------------------------------------------------------- */

static const char *kYtSearch =
    "{\"kind\":\"youtube#searchListResponse\",\"nextPageToken\":\"CBgQAA\",\"regionCode\":\"US\","
    "\"items\":["
    "{\"kind\":\"youtube#searchResult\",\"id\":{\"kind\":\"youtube#video\",\"videoId\":\"jNQXAC9IVRw\"},"
    "\"snippet\":{\"publishedAt\":\"2005-04-24T03:31:52Z\",\"title\":\"Me at the zoo &amp; Tom&#39;s "
    "&quot;cat&quot;\","
    "\"channelTitle\":\"jawed\",\"liveBroadcastContent\":\"none\"}},"
    "{\"id\":{\"kind\":\"youtube#video\",\"videoId\":\"upcoming123\"},\"snippet\":{\"title\":\"soon\","
    "\"liveBroadcastContent\":\"upcoming\"}},"
    "{\"id\":{\"kind\":\"youtube#video\",\"videoId\":\"../etc/x\"},\"snippet\":{\"title\":\"bad id\"}},"
    "{\"id\":{\"kind\":\"youtube#video\",\"videoId\":\"rFZHOHl-L8A\"},\"snippet\":{\"title\":\"lofi "
    "\\ud83d\\udcda radio\","
    "\"channelTitle\":\"Lofi "
    "Girl\",\"publishedAt\":\"2022-07-12T00:00:00Z\",\"liveBroadcastContent\":\"live\"}}"
    "]}";

static const char *kYtVideos =
    "{\"items\":[{\"id\":\"jNQXAC9IVRw\",\"contentDetails\":{\"duration\":\"PT19S\"},"
    "\"statistics\":{\"viewCount\":\"380123456\"}},"
    "{\"id\":\"rFZHOHl-L8A\",\"contentDetails\":{\"duration\":\"P0D\"},\"statistics\":{\"viewCount\":"
    "\"9247\"}}]}";

static const char *kYtQuota =
  "{\"error\":{\"code\":403,\"message\":\"The request cannot be completed because you have exceeded your "
  "<a href=\\\"/youtube/v3/getting-started#quota\\\">quota</a>.\",\"errors\":[{\"message\":\"x\","
  "\"domain\":\"youtube.quota\",\"reason\":\"quotaExceeded\"}]}}";

static const char *kYtBadKey =
    "{\"error\":{\"code\":400,\"message\":\"API key not valid. Please pass a valid API key.\",\"errors\":"
    "[{\"message\":\"API key not valid. Please pass a valid API "
    "key.\",\"domain\":\"global\",\"reason\":\"badRequest\"}],"
    "\"status\":\"INVALID_ARGUMENT\",\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.ErrorInfo\","
    "\"reason\":\"API_KEY_INVALID\",\"domain\":\"googleapis.com\"}]}}";

static const char *kYtDisabled =
  "{\"error\":{\"code\":403,\"message\":\"YouTube Data API v3 has not been used in project 1 before or it is "
  "disabled.\",\"errors\":[{\"reason\":\"accessNotConfigured\"}]}}";

static const char *kIaSearch =
    "{\"responseHeader\":{\"status\":0},\"response\":{\"numFound\":46688,\"start\":0,\"docs\":["
    "{\"creator\":\"rtil\",\"downloads\":18,\"identifier\":\"move_your_feet_by_rtil\",\"publicdate\":"
    "\"2022-07-30T05:15:35Z\",\"title\":\"Move Your Feet\"},"
    "{\"creator\":[\"Fleischer "
    "Studios\",\"Paramount\"],\"downloads\":12345,\"identifier\":\"Popeye_forPresident\","
    "\"publicdate\":\"2006-01-01T00:00:00Z\",\"title\":\"Popeye for President\"},"
    "{\"identifier\":\"bad id with spaces\",\"title\":\"x\"},"
    "{\"identifier\":\"no-title-item\"}]}}";

static const char *kIaMeta =
  "{\"created\":1,\"dir\":\"/0/items/night\",\"files\":["
  "{\"name\":\"Night.mp3\",\"format\":\"MP3\",\"length\":\"95:17\"},"
  "{\"name\":\"Night.mp4\",\"format\":\"h.264\",\"length\":\"5731.83\",\"height\":\"480\",\"width\":\"640\"},"
  "{\"name\":\"Night.ogv\",\"format\":\"Ogg Video\",\"length\":\"5731.78\",\"height\":\"300\"},"
  "{\"name\":\"Night_512kb.mp4\",\"format\":\"512Kb MPEG4\",\"length\":\"5731.8\",\"height\":\"240\"},"
  "{\"name\":\"VIDEO_TS.mp4\",\"format\":\"h.264\",\"length\":\"10.01\",\"height\":\"480\"},"
  "{\"name\":\"extras/My Trailer (1).mp4\",\"format\":\"h.264\",\"length\":\"60\",\"height\":\"720\"}],"
  "\"metadata\":{\"identifier\":\"night\"}}";

static const char *kIaOgg =
  "{\"files\":[{\"name\":\"a.ogv\",\"format\":\"Ogg Video\",\"length\":\"20\",\"height\":\"300\"},"
  "{\"name\":\"a.swf\",\"format\":\"Shockwave Flash\"}]}";

static const char *kIaSub =
  "{\"files\":[{\"name\":\"disc 1/My Film (1).mp4\",\"format\":\"h.264\",\"length\":\"1:02:03\"}]}";

static const char *kPtSearch =
  "{\"total\":15282,\"data\":["
  "{\"uuid\":\"da5463ca-fbc7-4311-948a-356594d5c70a\",\"name\":\"Installing Linux\",\"duration\":639,"
  "\"views\":1016,\"url\":\"https://spectra.video/videos/watch/da5463ca-fbc7-4311-948a-356594d5c70a\","
  "\"thumbnailUrl\":\"https://spectra.video/lazy-static/thumbnails/e6.jpg\",\"publishedAt\":"
  "\"2025-05-11T20:43:40.247Z\",\"isLive\":false,\"account\":{\"displayName\":\"Trafotin\"}},"
  "{\"uuid\":\"11111111-2222-3333-4444-555555555555\",\"name\":\"Live now\",\"duration\":0,\"views\":5,"
  "\"url\":\"https://tube.example/w/abc\",\"previewUrl\":\"https://tube.example/p.jpg\",\"isLive\":true,"
  "\"channel\":{\"displayName\":\"Chan\"}},"
  "{\"uuid\":\"bad uuid\",\"url\":\"https://x/y\"},"
  "{\"uuid\":\"aaaaaaaa-0000-0000-0000-000000000000\",\"url\":\"javascript:alert(1)\"}]}";

static const char *kPtWeb =
  "{\"isLive\":false,\"duration\":639,\"files\":["
  "{\"resolution\":{\"id\":1080},\"fileUrl\":\"https://s.example/web/v-1080.mp4\"},"
  "{\"resolution\":{\"id\":720},\"fileUrl\":\"https://s.example/web/v-720.mp4\"},"
  "{\"resolution\":{\"id\":360},\"fileUrl\":\"https://s.example/web/v-360.mp4\"}],"
  "\"streamingPlaylists\":[{\"type\":1,\"files\":[{\"resolution\":{\"id\":480},"
  "\"fileUrl\":\"https://s.example/hls/v-480-fragmented.mp4\"}]}]}";

static const char *kPtHls =
  "{\"isLive\":false,\"duration\":100,\"files\":[],\"streamingPlaylists\":[{\"type\":1,\"files\":["
  "{\"resolution\":{\"id\":1080},\"fileUrl\":\"https://s.example/hls/a-1080-fragmented.mp4\"},"
  "{\"resolution\":{\"id\":360},\"fileUrl\":\"https://s.example/hls/a-360-fragmented.mp4\"}]}]}";

static const char *kPtSplit = "{\"isLive\":false,\"files\":["
                              "{\"resolution\":{\"id\":720},\"hasAudio\":false,\"hasVideo\":true,\"fileUrl\":"
                              "\"https://s.example/v-720.mp4\"},"
                              "{\"resolution\":{\"id\":0},\"hasAudio\":true,\"hasVideo\":false,\"fileUrl\":"
                              "\"https://s.example/a-0.m4a\"}]}";

static const char *kDmSearch =
    "{\"page\":1,\"limit\":3,\"explicit\":false,\"total\":1000,\"has_more\":true,\"list\":["
    "{\"id\":\"x2zv4fm\",\"title\":\"Cats Cats Cats\",\"duration\":100,\"views_total\":183,"
    "\"thumbnail_360_url\":\"https:\\/\\/s2.dmcdn.net\\/v\\/Ap7Co\\/"
    "x360\",\"owner.screenname\":\"Vaultaspiring\","
    "\"url\":\"https:\\/\\/www.dailymotion.com\\/video\\/"
    "x2zv4fm\",\"created_time\":1438344370,\"onair\":false},"
    "{\"id\":\"x9live\",\"title\":\"Live TV\",\"duration\":0,\"views_total\":7,\"onair\":true},"
    "{\"id\":\"bad/id\",\"title\":\"x\"}]}";

static const char *kDmError =
  "{\"error\":{\"more_info\":\"https:\\/\\/developer.dailymotion.com\\/api#error-codes\",\"code\":400,"
  "\"message\":\"Unrecognized value (bogus)\",\"type\":\"invalid_parameter\"}}";

static const char *kYtdlpFlat =
    "{\"id\":\"lofi\",\"title\":\"lofi\",\"_type\":\"playlist\",\"entries\":["
    "{\"_type\":\"url\",\"ie_key\":\"Youtube\",\"id\":\"rFZHOHl-L8A\",\"url\":\"https://www.youtube.com/"
    "watch?v=rFZHOHl-L8A\","
    "\"title\":\"lofi hip hop radio \\ud83d\\udcda\",\"duration\":null,\"channel\":\"Lofi "
    "Girl\",\"view_count\":null,"
    "\"concurrent_view_count\":9247,\"live_status\":\"is_live\"},"
    "{\"_type\":\"url\",\"ie_key\":\"YoutubeTab\",\"id\":\"UCSJ4gkVC6NrvII8umztf0Ow\",\"title\":\"a "
    "channel\"},"
    "{\"_type\":\"url\",\"ie_key\":\"Youtube\",\"id\":\"OO2kPK5-qno\",\"title\":\"Lofi "
    "Coffee\",\"duration\":88718,"
    "\"view_count\":13358338,\"uploader\":\"Someone\"},"
    "null]}";

static const char *kYtdlpSingle =
    "{\"id\":\"12345\",\"title\":\"A clip\",\"uploader\":\"Up\",\"duration\":61.5,\"view_count\":42,"
    "\"webpage_url\":\"https://vimeo.com/12345\",\"upload_date\":\"20240131\",\"extractor_key\":\"Vimeo\","
    "\"thumbnails\":[{\"url\":\"https://i.example/t-100.jpg\",\"width\":100},"
    "{\"url\":\"https://i.example/t-340.jpg\",\"width\":340},{\"url\":\"https://i.example/"
    "t-1280.jpg\",\"width\":1280},"
    "{\"url\":\"not a url\",\"width\":320}]}";

/* ---- offline ---------------------------------------------------------------------- */

static void vt_registry(void) {
  static const char *const kKeys[] = { "youtube", "archive", "peertube", "dailymotion", "bilibili", "web" };
  TEST_CHECK(vsrc_count() == FM_COUNT(kKeys));
  for (int i = 0; i < vsrc_count() && i < FM_COUNT(kKeys); i++) {
    const FmVsrc *s = vsrc_at(i);
    TEST_CHECK(s && !strcmp(s->key, kKeys[i]));
    TEST_CHECK(s && s->name && s->about && s->search && s->resolve);
    TEST_CHECK(s && vsrc_find(s->key) == s);
    TEST_CHECK(s && ((s->flags & VSRC_DIRECT) != 0) != ((s->flags & VSRC_YTDLP) != 0));
  }
  TEST_CHECK(vsrc_at(-1) == NULL && vsrc_at(FM_COUNT(kKeys)) == NULL);
  TEST_CHECK(vsrc_find("nope") == NULL && vsrc_find(NULL) == NULL);
  TEST_CHECK(vsrc_find("web")->flags & VSRC_URL);
}

static void vt_page(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  for (int i = 0; i < 100; i++) {
    FmVsrcItem *it = vsrc_page_add(&p);
    TEST_CHECK(it->views == -1 && it->title[0] == 0 && it->duration == 0);
    fm_snprintf(it->id, sizeof it->id, "%d", i);
  }
  TEST_CHECK(p.count == 100 && p.cap >= 100 && !strcmp(p.items[99].id, "99") && !strcmp(p.items[0].id, "0"));
  vsrc_page_free(&p);
  TEST_CHECK(p.items == NULL && p.count == 0 && p.cap == 0);
  vsrc_page_free(&p);                     /* twice is harmless */
  /* one page of results is one allocation */
  vsrc_page_add(&p);
  TEST_CHECK(p.cap == VSRC_PAGE_SIZE);
  vsrc_page_free(&p);
}

static void vt_text(const char *tmp) {
  TEST_CHECK(vsrc_iso_duration("PT1H2M3S") == 3723);
  TEST_CHECK(vsrc_iso_duration("PT45S") == 45);
  TEST_CHECK(vsrc_iso_duration("PT4M") == 240);
  TEST_CHECK(vsrc_iso_duration("P1DT1S") == 86401);
  TEST_CHECK(vsrc_iso_duration("P1W") == 7 * 86400);
  TEST_CHECK(vsrc_iso_duration("PT1.5S") == 1.5);
  TEST_CHECK(vsrc_iso_duration("P0D") == 0);
  const char *bad[] = { "", "P", "PT", "1H", "PTXS", "PT-5S", "PT5", "PT5Q", "P5H", "PT1H2", "xyz" };
  for (int i = 0; i < FM_COUNT(bad); i++) TEST_CHECK(vsrc_iso_duration(bad[i]) == 0);
  TEST_CHECK(vsrc_iso_duration(NULL) == 0);
  TEST_CHECK(vsrc_clock_seconds("95:17") == 5717);
  TEST_CHECK(vsrc_clock_seconds("1:02:03") == 3723);
  TEST_CHECK(fabs(vsrc_clock_seconds("370.2") - 370.2) < 1e-9);
  TEST_CHECK(vsrc_clock_seconds("abc") == 0 && vsrc_clock_seconds("1:2:3:4") == 0 &&
             vsrc_clock_seconds("") == 0);

  char out[64];
  vsrc_html_unescape("Tom &amp; Jerry&#39;s &quot;x&quot; &lt;3 &#x1F600;&#233; &bogus; &#;", out,
                     sizeof out);
  TEST_CHECK(!strcmp(out, "Tom & Jerry's \"x\" <3 \xF0\x9F\x98\x80\xC3\xA9 &bogus; &#;"));
  vsrc_html_unescape("\xC3\xA9\xC3\xA9\xC3\xA9", out, 4);       /* cut between two bytes of one char */
  TEST_CHECK(!strcmp(out, "\xC3\xA9"));
  vsrc_html_unescape("&#55357;&#1114112;", out, sizeof out);      /* surrogate and out of range stay text */
  TEST_CHECK(!strcmp(out, "&#55357;&#1114112;"));

  vsrc_unix_date(1438344370, out, sizeof out);
  TEST_CHECK(!strcmp(out, "2015-07-31"));
  vsrc_unix_date(951782400, out, sizeof out);
  TEST_CHECK(!strcmp(out, "2000-02-29"));
  vsrc_unix_date(0, out, sizeof out);
  TEST_CHECK(out[0] == 0);
  vsrc_iso_date("2005-04-24T03:31:52Z", out, sizeof out);
  TEST_CHECK(!strcmp(out, "2005-04-24"));
  vsrc_iso_date("yesterday", out, sizeof out);
  TEST_CHECK(out[0] == 0);

  vsrc_safe_name("a/b:c*?\"<>|d  e.", out, sizeof out);
  TEST_CHECK(!strcmp(out, "a_b_c______d e"));
  vsrc_safe_name("\xE7\x8C\xAB\xE7\x8C\xAB\xE7\x8C\xAB", out, 8);   /* 3-byte chars cut at 7 bytes */
  TEST_CHECK(!strcmp(out, "\xE7\x8C\xAB\xE7\x8C\xAB"));
  TEST_CHECK(vsrc_id_ok("jNQXAC9IVRw", "-_", 32) && !vsrc_id_ok("a/b", "-_", 32) && !vsrc_id_ok("", NULL, 8));
  TEST_CHECK(!vsrc_id_ok("abcdefghi", NULL, 8));
  TEST_CHECK(vsrc_url_ok("https://a.b/c?d=1") && !vsrc_url_ok("javascript:x") && !vsrc_url_ok("https://a b"));
  FM_UNUSED(tmp);
}

static void vt_urls(void) {
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  fm_strlcpy(c.api_key_youtube, "K&EY", sizeof c.api_key_youtube);
  fm_strlcpy(c.region, "US", sizeof c.region);
  c.safe_search = true;
  char u[2048];
  vsrc_youtube_search_url(&c, "lo-fi beats & chill", "CBgQAA", u, sizeof u);
  const char *pre = "https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=24&";
  TEST_CHECK(!strncmp(u, pre, strlen(pre)));
  TEST_CHECK(strstr(u, "&q=lo-fi%20beats%20%26%20chill&") && strstr(u, "&key=K%26EY&"));
  TEST_CHECK(strstr(u, "&safeSearch=moderate") && strstr(u, "&regionCode=US") &&
             strstr(u, "&pageToken=CBgQAA"));
  c.safe_search = false;
  c.region[0] = 0;
  vsrc_youtube_search_url(&c, "x", "bad token!", u, sizeof u);
  TEST_CHECK(strstr(u, "safeSearch=none") && !strstr(u, "pageToken") && !strstr(u, "regionCode"));
  vsrc_archive_search_url("night of the living dead", 2, u, sizeof u);
  TEST_CHECK(
      strstr(u, "q=%28night%20of%20the%20living%20dead%29%20AND%20mediatype%3Amovies%20AND%20-access"));
  TEST_CHECK(strstr(u, "fl%5B%5D=identifier") && strstr(u, "&rows=24&page=2&output=json"));
  c.safe_search = true;
  vsrc_peertube_search_url(&c, "linux", 24, u, sizeof u);
  TEST_CHECK(
      !strcmp(u, "https://sepiasearch.org/api/v1/search/videos?search=linux&count=24&start=24&nsfw=false"));
  vsrc_dailymotion_search_url(&c, "cats", 3, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.dailymotion.com/videos?search=cats&fields=id,title,") == u);
  TEST_CHECK(strstr(u, "owner.screenname") && strstr(u, "&limit=24&page=3&") &&
             strstr(u, "family_filter=true"));
}

static void vt_youtube(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_youtube_parse_search(kYtSearch, strlen(kYtSearch), &p) == FM_OK);
  TEST_CHECK(p.count == 2 && !strcmp(p.next, "CBgQAA"));
  if (p.count == 2) {
    FmVsrcItem *a = &p.items[0], *b = &p.items[1];
    TEST_CHECK(!strcmp(a->id, "jNQXAC9IVRw") && !strcmp(a->title, "Me at the zoo & Tom's \"cat\""));
    TEST_CHECK(!strcmp(a->channel, "jawed") && !strcmp(a->published, "2005-04-24") && !a->live);
    TEST_CHECK(!strcmp(a->thumb, "https://i.ytimg.com/vi/jNQXAC9IVRw/mqdefault.jpg"));
    TEST_CHECK(!strcmp(a->page, "https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    TEST_CHECK(b->live && !strcmp(b->title, "lofi \xF0\x9F\x93\x9A radio") && a->views == -1);
    TEST_CHECK(vsrc_youtube_parse_videos(kYtVideos, strlen(kYtVideos), &p) == FM_OK);
    TEST_CHECK(a->duration == 19 && a->views == 380123456 && b->duration == 0 && b->views == 9247);
  }
  vsrc_page_free(&p);
  char e[256];
  TEST_CHECK(!strcmp(vsrc_youtube_error(kYtQuota, strlen(kYtQuota), 403, e, sizeof e), "quota"));
  TEST_CHECK(strstr(e, "Daily YouTube quota used up") == e && strstr(e, "another source"));
  TEST_CHECK(!strcmp(vsrc_youtube_error(kYtBadKey, strlen(kYtBadKey), 400, e, sizeof e), "key"));
  TEST_CHECK(strstr(e, "not valid") && strstr(e, "Settings"));
  TEST_CHECK(!strcmp(vsrc_youtube_error(kYtDisabled, strlen(kYtDisabled), 403, e, sizeof e), "disabled"));
  TEST_CHECK(!strcmp(vsrc_youtube_error("<html>oops", 10, 502, e, sizeof e), "other") && strstr(e, "502"));
  TEST_CHECK(!strcmp(vsrc_youtube_error(NULL, 0, 429, e, sizeof e), "rate"));
}

static void vt_archive(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_archive_parse_search(kIaSearch, strlen(kIaSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 3 && p.next[0] == 0);          /* 4 docs < a full page: no next */
  if (p.count == 3) {
    TEST_CHECK(!strcmp(p.items[1].channel, "Fleischer Studios") && p.items[1].views == 12345);
    TEST_CHECK(!strcmp(p.items[1].thumb, "https://archive.org/services/img/Popeye_forPresident"));
    TEST_CHECK(!strcmp(p.items[1].page, "https://archive.org/details/Popeye_forPresident"));
    TEST_CHECK(!strcmp(p.items[0].published, "2022-07-30"));
    TEST_CHECK(!strcmp(p.items[2].title, "no-title-item") && p.items[2].views == -1);
  }
  vsrc_page_free(&p);

  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  char err[256];
  c.max_height = 720;
  TEST_CHECK(vsrc_archive_pick(kIaMeta, strlen(kIaMeta), "night", &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://archive.org/download/night/Night.mp4") && !st->local &&
             !st->audio[0]);
  TEST_CHECK(fabs(st->duration - 5731.83) < 0.01 && st->height == 480);
  c.max_height = 360;
  TEST_CHECK(vsrc_archive_pick(kIaMeta, strlen(kIaMeta), "night", &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://archive.org/download/night/Night_512kb.mp4"));
  TEST_CHECK(vsrc_archive_pick(kIaOgg, strlen(kIaOgg), "a", &c, st, err, sizeof err) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(strstr(err, "FFmpeg") != NULL);
  c.have_ffmpeg_libs = true;
  TEST_CHECK(vsrc_archive_pick(kIaOgg, strlen(kIaOgg), "a", &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://archive.org/download/a/a.ogv"));
  TEST_CHECK(vsrc_archive_pick(kIaSub, strlen(kIaSub), "sub", &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://archive.org/download/sub/disc%201/My%20Film%20%281%29.mp4"));
  TEST_CHECK(st->duration == 3723);
  TEST_CHECK(vsrc_archive_pick("{}", 2, "gone", &c, st, err, sizeof err) == FM_ERR_NOT_FOUND);
  const char *restricted =
      "{\"metadata\":{\"access-restricted-item\":\"true\"},\"files\":[{\"name\":\"a.mp4\"}]}";
  TEST_CHECK(vsrc_archive_pick(restricted, strlen(restricted), "tv", &c, st, err, sizeof err) ==
             FM_ERR_ACCESS);
  const char *priv = "{\"files\":[{\"name\":\"a.mp4\",\"private\":\"true\"},{\"name\":\"b.webm\"}]}";
  TEST_CHECK(vsrc_archive_pick(priv, strlen(priv), "p", &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://archive.org/download/p/b.webm"));
  fm_free(st);
}

static void vt_peertube(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_peertube_parse_search(kPtSearch, strlen(kPtSearch), 0, &p) == FM_OK);
  TEST_CHECK(p.count == 2 && p.next[0] == 0);
  if (p.count == 2) {
    TEST_CHECK(!strcmp(p.items[0].channel, "Trafotin") && p.items[0].duration == 639 &&
               p.items[0].views == 1016);
    TEST_CHECK(!strcmp(p.items[0].published, "2025-05-11") && !p.items[0].live);
    TEST_CHECK(p.items[1].live && !strcmp(p.items[1].channel, "Chan") &&
               !strcmp(p.items[1].thumb, "https://tube.example/p.jpg"));
  }
  vsrc_page_free(&p);
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.max_height = 720;
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  char err[256];
  TEST_CHECK(vsrc_peertube_pick(kPtWeb, strlen(kPtWeb), &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://s.example/web/v-720.mp4") && st->height == 720 &&
             st->duration == 639);
  c.max_height = 480;     /* a web file at 360 beats an HLS file at exactly 480 */
  TEST_CHECK(vsrc_peertube_pick(kPtWeb, strlen(kPtWeb), &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://s.example/web/v-360.mp4"));
  TEST_CHECK(vsrc_peertube_pick(kPtHls, strlen(kPtHls), &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://s.example/hls/a-360-fragmented.mp4"));
  c.max_height = 1080;
  TEST_CHECK(vsrc_peertube_pick(kPtSplit, strlen(kPtSplit), &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://s.example/v-720.mp4") &&
             !strcmp(st->audio, "https://s.example/a-0.m4a"));
  TEST_CHECK(vsrc_peertube_pick("{\"isLive\":true}", 15, &c, st, err, sizeof err) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(vsrc_peertube_pick("{\"files\":[]}", 12, &c, st, err, sizeof err) == FM_ERR_UNSUPPORTED);
  fm_free(st);
}

static void vt_dailymotion(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_dailymotion_parse_search(kDmSearch, strlen(kDmSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 2 && !strcmp(p.next, "2"));
  if (p.count == 2) {
    TEST_CHECK(!strcmp(p.items[0].channel, "Vaultaspiring") && !strcmp(p.items[0].published, "2015-07-31"));
    TEST_CHECK(!strcmp(p.items[0].thumb, "https://s2.dmcdn.net/v/Ap7Co/x360") && p.items[0].views == 183);
    TEST_CHECK(!strcmp(p.items[1].page, "https://www.dailymotion.com/video/x9live") && p.items[1].live);
  }
  vsrc_page_free(&p);
  TEST_CHECK(vsrc_dailymotion_parse_search(kDmError, strlen(kDmError), 1, &p) == FM_ERR_FORMAT);
  TEST_CHECK(strstr(p.error, "Unrecognized value") != NULL && p.count == 0);
  vsrc_page_free(&p);
}

/* trimmed from a real `yt-dlp -J` of aqz-KE-bpKQ (2026-10-08): 24 of its 47 formats,
** URLs shortened; plus the progressive itag 18 most videos also have */
static const char *kYtdlpFormats =
    "{\"id\":\"aqz-KE-bpKQ\",\"title\":\"Big Buck Bunny 60fps 4K - Official Blender Foundation Short Film\",\"dura"
    "tion\":635,\"live_status\":\"not_live\",\"format_id\":\"302+251\",\"width\":1280,\"height\":720,\"formats\":[{\"form"
    "at_id\":\"sb0\",\"format_note\":\"storyboard\",\"ext\":\"mhtml\",\"protocol\":\"mhtml\",\"vcodec\":\"none\",\"acodec\":\"n"
    "one\",\"width\":320,\"height\":180,\"fps\":0.2015748031496063,\"url\":\"https://rr1---sn-x.googlevideo.com/vid"
    "eoplayback?itag=sb0&expire=1\"},{\"format_id\":\"233\",\"format_note\":\"Default, low\",\"ext\":\"mp4\",\"protocol"
    "\":\"m3u8_native\",\"vcodec\":\"none\",\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=233&exp"
    "ire=1\"},{\"format_id\":\"139-drc\",\"format_note\":\"low, DRC\",\"ext\":\"m4a\",\"protocol\":\"https\",\"vcodec\":\"non"
    "e\",\"acodec\":\"mp4a.40.5\",\"tbr\":48.8,\"filesize\":3871021,\"filesize_approx\":3870998,\"container\":\"m4a_das"
    "h\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=139-drc&exp"
    "ire=1\"},{\"format_id\":\"249\",\"format_note\":\"low\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"none\",\"acod"
    "ec\":\"opus\",\"tbr\":49.6,\"filesize\":3931453,\"filesize_approx\":3931432,\"container\":\"webm_dash\",\"language"
    "_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=249&expire=1\"},{\"format"
    "_id\":\"251-drc\",\"format_note\":\"medium, DRC\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"none\",\"acodec\":"
    "\"opus\",\"tbr\":129.3,\"filesize\":10258925,\"filesize_approx\":10258880,\"container\":\"webm_dash\",\"language_"
    "preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=251-drc&expire=1\"},{\"for"
    "mat_id\":\"140\",\"format_note\":\"medium\",\"ext\":\"m4a\",\"protocol\":\"https\",\"vcodec\":\"none\",\"acodec\":\"mp4a.4"
    "0.2\",\"tbr\":129.5,\"filesize\":10271496,\"filesize_approx\":10271468,\"container\":\"m4a_dash\",\"language_pre"
    "ference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=140&expire=1\"},{\"format_id\""
    ":\"251\",\"format_note\":\"medium\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"none\",\"acodec\":\"opus\",\"tbr\":"
    "128.6,\"filesize\":10202210,\"filesize_approx\":10202162,\"container\":\"webm_dash\",\"language_preference\":-"
    "1,\"http_headers\":{\"User-Agent\":\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML,"
    " like Gecko) Chrome/148.0.0.0 Safari/537.36\",\"Accept\":\"text/html,application/xhtml+xml,application/x"
    "ml;q=0.9,*/*;q=0.8\",\"Accept-Language\":\"en-us,en;q=0.5\",\"Sec-Fetch-Mode\":\"navigate\"},\"url\":\"https://r"
    "r1---sn-x.googlevideo.com/videoplayback?itag=251&expire=1\"},{\"format_id\":\"628\",\"ext\":\"mp4\",\"protocol"
    "\":\"m3u8_native\",\"vcodec\":\"vp09.00.51.08\",\"acodec\":\"none\",\"width\":3840,\"height\":2160,\"fps\":60.0,\"tbr\""
    ":27987.1,\"dynamic_range\":\"SDR\",\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=628&expi"
    "re=1\"},{\"format_id\":\"315\",\"format_note\":\"2160p60\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"vp9\",\"ac"
    "odec\":\"none\",\"width\":3840,\"height\":2160,\"fps\":60,\"tbr\":17174.2,\"filesize\":1362269481,\"filesize_appro"
    "x\":1362269472,\"container\":\"webm_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://"
    "rr1---sn-x.googlevideo.com/videoplayback?itag=315&expire=1\"},{\"format_id\":\"401\",\"format_note\":\"2160p"
    "60\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"av01.0.13M.08\",\"acodec\":\"none\",\"width\":3840,\"height\":21"
    "60,\"fps\":60,\"tbr\":8981.8,\"filesize\":712445280,\"filesize_approx\":712445254,\"container\":\"mp4_dash\",\"dy"
    "namic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?"
    "itag=401&expire=1\"},{\"format_id\":\"299\",\"format_note\":\"1080p60\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcode"
    "c\":\"avc1.64002A\",\"acodec\":\"none\",\"width\":1920,\"height\":1080,\"fps\":60,\"tbr\":3247.8,\"filesize\":2576196"
    "53,\"filesize_approx\":257619597,\"container\":\"mp4_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1"
    ",\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=299&expire=1\"},{\"format_id\":\"303\",\"for"
    "mat_note\":\"1080p60\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"vp9\",\"acodec\":\"none\",\"width\":1920,\"hei"
    "ght\":1080,\"fps\":60,\"tbr\":2127.3,\"filesize\":168736189,\"filesize_approx\":168736175,\"container\":\"webm_d"
    "ash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videop"
    "layback?itag=303&expire=1\"},{\"format_id\":\"399\",\"format_note\":\"1080p60\",\"ext\":\"mp4\",\"protocol\":\"https"
    "\",\"vcodec\":\"av01.0.09M.08\",\"acodec\":\"none\",\"width\":1920,\"height\":1080,\"fps\":60,\"tbr\":1568.2,\"filesiz"
    "e\":124386876,\"filesize_approx\":124386834,\"container\":\"mp4_dash\",\"dynamic_range\":\"SDR\",\"language_pref"
    "erence\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=399&expire=1\"},{\"format_id\":"
    "\"160\",\"format_note\":\"144p\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"avc1.4D400C\",\"acodec\":\"none\",\"wi"
    "dth\":256,\"height\":144,\"fps\":30,\"tbr\":54.5,\"filesize\":4323893,\"filesize_approx\":4323853,\"container\":\""
    "mp4_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/v"
    "ideoplayback?itag=160&expire=1\"},{\"format_id\":\"278\",\"format_note\":\"144p\",\"ext\":\"webm\",\"protocol\":\"ht"
    "tps\",\"vcodec\":\"vp9\",\"acodec\":\"none\",\"width\":256,\"height\":144,\"fps\":30,\"tbr\":66.7,\"filesize\":5289812,"
    "\"filesize_approx\":5289742,\"container\":\"webm_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"ur"
    "l\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=278&expire=1\"},{\"format_id\":\"134\",\"format_"
    "note\":\"360p\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"avc1.4D401E\",\"acodec\":\"none\",\"width\":640,\"heig"
    "ht\":360,\"fps\":30,\"tbr\":230.6,\"filesize\":18294110,\"filesize_approx\":18294061,\"container\":\"mp4_dash\",\""
    "dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplaybac"
    "k?itag=134&expire=1\"},{\"format_id\":\"243\",\"format_note\":\"360p\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcode"
    "c\":\"vp9\",\"acodec\":\"none\",\"width\":640,\"height\":360,\"fps\":30,\"tbr\":303.9,\"filesize\":24109536,\"filesize"
    "_approx\":24109462,\"container\":\"webm_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"http"
    "s://rr1---sn-x.googlevideo.com/videoplayback?itag=243&expire=1\"},{\"format_id\":\"396\",\"format_note\":\"3"
    "60p\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"av01.0.01M.08\",\"acodec\":\"none\",\"width\":640,\"height\":36"
    "0,\"fps\":30,\"tbr\":192.9,\"filesize\":15298808,\"filesize_approx\":15298751,\"container\":\"mp4_dash\",\"dynami"
    "c_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag"
    "=396&expire=1\"},{\"format_id\":\"135\",\"format_note\":\"480p\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"avc"
    "1.4D401F\",\"acodec\":\"none\",\"width\":854,\"height\":480,\"fps\":30,\"tbr\":355.6,\"filesize\":28207144,\"filesiz"
    "e_approx\":28207093,\"container\":\"mp4_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"http"
    "s://rr1---sn-x.googlevideo.com/videoplayback?itag=135&expire=1\"},{\"format_id\":\"244\",\"format_note\":\"4"
    "80p\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"vp9\",\"acodec\":\"none\",\"width\":854,\"height\":480,\"fps\":3"
    "0,\"tbr\":417.8,\"filesize\":33138062,\"filesize_approx\":33137988,\"container\":\"webm_dash\",\"dynamic_range\""
    ":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=244&exp"
    "ire=1\"},{\"format_id\":\"298\",\"format_note\":\"720p60\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"avc1.4D40"
    "20\",\"acodec\":\"none\",\"width\":1280,\"height\":720,\"fps\":60,\"tbr\":1897.7,\"filesize\":150524867,\"filesize_a"
    "pprox\":150524845,\"container\":\"mp4_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https:"
    "//rr1---sn-x.googlevideo.com/videoplayback?itag=298&expire=1\"},{\"format_id\":\"302\",\"format_note\":\"720"
    "p60\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"vp9\",\"acodec\":\"none\",\"width\":1280,\"height\":720,\"fps\":"
    "60,\"tbr\":1420.5,\"filesize\":112676322,\"filesize_approx\":112676315,\"container\":\"webm_dash\",\"dynamic_ra"
    "nge\":\"SDR\",\"language_preference\":-1,\"http_headers\":{\"User-Agent\":\"Mozilla/5.0 (Windows NT 10.0; Win6"
    "4; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36\",\"Accept\":\"text/html,a"
    "pplication/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\",\"Accept-Language\":\"en-us,en;q=0.5\",\"Sec-Fetch-"
    "Mode\":\"navigate\"},\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=302&expire=1\"},{\"form"
    "at_id\":\"398\",\"format_note\":\"720p60\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"av01.0.08M.08\",\"acodec\""
    ":\"none\",\"width\":1280,\"height\":720,\"fps\":60,\"tbr\":898.7,\"filesize\":71283827,\"filesize_approx\":7128381"
    "2,\"container\":\"mp4_dash\",\"dynamic_range\":\"SDR\",\"language_preference\":-1,\"url\":\"https://rr1---sn-x.go"
    "oglevideo.com/videoplayback?itag=398&expire=1\"},{\"format_id\":\"312\",\"ext\":\"mp4\",\"protocol\":\"m3u8_nati"
    "ve\",\"vcodec\":\"avc1.64002A\",\"acodec\":\"none\",\"width\":1920,\"height\":1080,\"fps\":60.0,\"tbr\":7417.7,\"dynam"
    "ic_range\":\"SDR\",\"url\":\"https://rr1---sn-x.googlevideo.com/videoplayback?itag=312&expire=1\"},{\"format"
    "_id\":\"18\",\"format_note\":\"360p\",\"ext\":\"mp4\",\"protocol\":\"https\",\"vcodec\":\"avc1.42001E\",\"acodec\":\"mp4a."
    "40.2\",\"width\":640,\"height\":360,\"fps\":30,\"tbr\":396.3,\"filesize_approx\":31469000,\"url\":\"https://rr1---"
    "sn-x.googlevideo.com/videoplayback?itag=18&expire=1\"}],\"requested_formats\":[{\"format_id\":\"302\",\"ext\""
    ":\"webm\",\"protocol\":\"https\",\"vcodec\":\"vp9\"},{\"format_id\":\"251\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcode"
    "c\":\"none\",\"acodec\":\"opus\"}]}";

/* ---- streaming: format choice and the quality list (vsrc_ytdlp_pick_stream) ---------- */

static const char *kDmHlsOnly =
    "{\"id\":\"xhfpjn\",\"duration\":944,\"formats\":[{\"format_id\":\"hls-380\",\"ext\":\"mp4\","
    "\"protocol\":\"m3u8_native\",\"vcodec\":\"avc1.64000d\",\"acodec\":\"mp4a.40.2\",\"width\":320,"
    "\"height\":240,\"tbr\":460.56,\"url\":\"https://www.dailymotion.com/cdn/manifest/video/xhfpjn.m3u8\"}]}";

static const char *kNeedsReferer =
    "{\"duration\":60,\"formats\":[{\"format_id\":\"hd\",\"ext\":\"mp4\",\"protocol\":\"https\","
    "\"vcodec\":\"avc1.4d401f\",\"acodec\":\"mp4a.40.2\",\"width\":1280,\"height\":720,\"tbr\":2000,"
    "\"url\":\"https://cdn.example.com/v.mp4\",\"http_headers\":{\"User-Agent\":\"Mozilla/5.0\","
    "\"Referer\":\"https://example.com/watch/1\"}}]}";

static int q_find(const FmVsrcStream *st, const char *label) {
  for (int i = 0; i < st->nq; i++)
    if (!strcmp(st->q[i].label, label)) return i;
  return -1;
}

static bool url_itag(const char *url, const char *itag) {
  const char *p = strstr(url, "itag=");
  size_t n = strlen(itag);
  return p && !strncmp(p + 5, itag, n) && p[5 + n] == '&';
}

static void vt_pick_stream(void) {
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  size_t n = strlen(kYtdlpFormats);

  /* no FFmpeg, Windows-like (Media Foundation): VP9 + Opus streams */
  c.os_mp4 = true;
  c.max_height = 720;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK);
  TEST_CHECK(!st->local && st->nq == 7 && st->cur == q_find(st, "720p60"));
  TEST_CHECK(url_itag(st->video, "302") && url_itag(st->audio, "251"));       /* 251: Opus, not the DRC copy */
  TEST_CHECK(st->width == 1280 && st->height == 720 && st->duration == 635);
  TEST_CHECK(strstr(st->headers, "User-Agent: ") && strstr(st->headers, "\r\n"));
  static const char *const kOrder[] = { "2160p60", "1080p60", "720p60", "480p", "360p", "144p", "Audio only" };
  for (int i = 0; i < FM_COUNT(kOrder) && i < st->nq; i++) {
    const FmVsrcQuality *q = &st->q[i];
    TEST_CHECK(!strcmp(q->label, kOrder[i]));
    TEST_CHECK(q->playable && q->url[0] && !q->needs_ffmpeg && !q->cache_only);
  }
  int k = q_find(st, "360p");
  TEST_CHECK(k >= 0 && url_itag(st->q[k].url, "243") && !st->q[k].muxed && !strcmp(st->q[k].codec, "VP9"));
  TEST_CHECK(k >= 0 && st->q[k].height == 360 && st->q[k].fps == 30);
  k = q_find(st, "2160p60");
  TEST_CHECK(k >= 0 && st->q[k].height == 2160 && st->q[k].fps == 60 && st->q[k].kbps > 17000 &&
             st->q[k].bytes > 1362269481LL);
  k = q_find(st, "Audio only");
  TEST_CHECK(k == st->nq - 1 && st->q[k].audio_only && st->q[k].height == 0 && !strcmp(st->q[k].codec, "Opus") &&
             url_itag(st->q[k].url, "251"));

  /* the size setting: the biggest within it, else the smallest */
  c.max_height = 360;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && st->cur == q_find(st, "360p") &&
             url_itag(st->video, "243"));
  c.max_height = 1080;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "303"));
  c.max_height = 2160;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "315"));
  c.max_height = 144;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "278"));

  /* FFmpeg: H.264 + AAC first; the progressive 360p (itag 18) has both */
  c.have_ffmpeg_libs = true;
  c.max_height = 1080;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "299") &&
             url_itag(st->audio, "140"));
  k = q_find(st, "360p");
  TEST_CHECK(k >= 0 && st->q[k].muxed && url_itag(st->q[k].url, "18") && !strcmp(st->q[k].codec, "H.264"));
  c.max_height = 360;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "18") &&
             !st->audio[0]);
  k = q_find(st, "Audio only");
  TEST_CHECK(k >= 0 && !strcmp(st->q[k].codec, "AAC"));

  /* Dailymotion: HLS only, so nothing streams; the cache plays it */
  c.have_ffmpeg_libs = false;
  c.max_height = 720;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kDmHlsOnly, strlen(kDmHlsOnly), &c, st) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(st->nq == 1 && st->cur == -1 && !strcmp(st->q[0].label, "240p") && st->q[0].cache_only &&
             !st->q[0].playable && !st->q[0].url[0] && st->q[0].muxed);
  c.os_mp4 = false;                        /* no system H.264: the built-in decoder is VP9 only */
  TEST_CHECK(vsrc_ytdlp_pick_stream(kDmHlsOnly, strlen(kDmHlsOnly), &c, st) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(st->nq == 1 && st->q[0].needs_ffmpeg && !st->q[0].cache_only);
  /* ... and without MP4 support YouTube still streams VP9 */
  TEST_CHECK(vsrc_ytdlp_pick_stream(kYtdlpFormats, n, &c, st) == FM_OK && url_itag(st->video, "302"));

  /* a site that checks the Referer: through the cache until the player can send headers */
  c.have_ffmpeg_libs = true;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kNeedsReferer, strlen(kNeedsReferer), &c, st) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(st->nq == 1 && st->q[0].cache_only && st->q[0].muxed);
  c.send_headers = true;
  TEST_CHECK(vsrc_ytdlp_pick_stream(kNeedsReferer, strlen(kNeedsReferer), &c, st) == FM_OK);
  TEST_CHECK(!strcmp(st->video, "https://cdn.example.com/v.mp4") && !st->audio[0] &&
             strstr(st->headers, "Referer: https://example.com/watch/1\r\n"));

  /* garbage */
  static const char *const kBad[] = { "", "{", "[]", "null", "{\"formats\":7}", "{\"formats\":[1,\"x\",{}]}" };
  for (int i = 0; i < FM_COUNT(kBad); i++) {
    FmErr e = vsrc_ytdlp_pick_stream(kBad[i], strlen(kBad[i]), &c, st);
    TEST_CHECK(e != FM_OK && st->nq == 0 && st->cur == -1);
  }
  /* every prefix of a real reply: no crash, never a stream from a cut reply */
  for (size_t cut = 0; cut < n; cut += 97) {
    FmErr e = vsrc_ytdlp_pick_stream(kYtdlpFormats, cut, &c, st);
    TEST_CHECK(e != FM_OK);
  }
  fm_free(st);
}

static void vt_ytdlp_parse(void) {
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_ytdlp_parse_search(kYtdlpFlat, strlen(kYtdlpFlat), "youtube", 25, 4, &p) == FM_OK);
  TEST_CHECK(p.count == 2 && !strcmp(p.next, "y:29"));
  if (p.count == 2) {
    TEST_CHECK(p.items[0].live && p.items[0].duration == 0 && !strcmp(p.items[0].channel, "Lofi Girl"));
    TEST_CHECK(!strcmp(p.items[0].thumb, "https://i.ytimg.com/vi/rFZHOHl-L8A/mqdefault.jpg"));
    TEST_CHECK(p.items[1].duration == 88718 && p.items[1].views == 13358338 &&
               !strcmp(p.items[1].channel, "Someone"));
  }
  vsrc_page_free(&p);
  TEST_CHECK(vsrc_ytdlp_parse_search(kYtdlpSingle, strlen(kYtdlpSingle), "web", 1, 24, &p) == FM_OK);
  TEST_CHECK(p.count == 1 && p.next[0] == 0);
  if (p.count == 1) {
    TEST_CHECK(!strcmp(p.items[0].thumb, "https://i.example/t-340.jpg") &&
               !strcmp(p.items[0].page, "https://vimeo.com/12345"));
    TEST_CHECK(!strcmp(p.items[0].published, "2024-01-31") && p.items[0].duration == 61.5 &&
               p.items[0].views == 42);
  }
  vsrc_page_free(&p);

  const char *s0 = vsrc_ytdlp_selector(false, false), *s1 = vsrc_ytdlp_selector(true, false);
  TEST_CHECK(!strncmp(s0, "bv*[ext=webm]+ba[ext=webm]/", 27) && strstr(s1, "avc1") && !strstr(s0, "avc1"));
  TEST_CHECK(!strncmp(vsrc_ytdlp_selector(false, true), "b[ext=mp4][protocol^=http]/", 27));

  FmVsrcItem it;
  memset(&it, 0, sizeof it);
  char k[160], k2[160];
  fm_strlcpy(it.id, "jNQXAC9IVRw", sizeof it.id);
  vsrc_cache_key("youtube", &it, 720, k, sizeof k);
  TEST_CHECK(!strcmp(k, "youtube-jNQXAC9IVRw-720"));
  fm_strlcpy(it.id, "a/b c", sizeof it.id);
  fm_strlcpy(it.page, "https://vimeo.com/1", sizeof it.page);
  vsrc_cache_key("web", &it, 360, k, sizeof k);
  fm_strlcpy(it.page, "https://vimeo.com/2", sizeof it.page);
  vsrc_cache_key("web", &it, 360, k2, sizeof k2);
  TEST_CHECK(!strncmp(k, "web-a_b_c-", 10) && strcmp(k, k2) != 0 && !strchr(k, '/'));
}

/* truncated and garbage replies: every parser fails cleanly */
static void vt_garbage(void) {
  const char *samples[] = {kYtSearch, kYtVideos, kIaSearch,  kIaMeta,      kPtSearch,
                           kPtWeb,    kDmSearch, kYtdlpFlat, kYtdlpSingle, kYtQuota};
  const char *junk[] = {"",
                        "null",
                        "[]",
                        "{}",
                        "42",
                        "\"str\"",
                        "{\"items\":{}}",
                        "{\"items\":[1,null,\"x\",[]]}",
                        "{\"response\":{\"docs\":[1,2]}}",
                        "{\"data\":[{\"uuid\":5}]}",
                        "{\"list\":[[]]}",
                        "{\"files\":[{\"name\":7},{\"name\":\"a.mp4\",\"length\":{}}]}",
                        "{\"entries\":7,\"_type\":\"playlist\"}",
                        "{\"files\":[{\"resolution\":5,\"fileUrl\":[\"https://x/y.mp4\"]}]}",
                        "\xFF\xFE{",
                        "{{{{"};
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.max_height = 720;
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  char err[256];
  int runs = 0;
  for (int pass = 0; pass < 2; pass++) {
    int ns = pass ? FM_COUNT(junk) : FM_COUNT(samples);
    for (int s = 0; s < ns; s++) {
      const char *text = pass ? junk[s] : samples[s];
      size_t full = strlen(text);
      size_t step = pass ? 1 : 5;
      for (size_t len = pass ? full : 0; len <= full; len += step) {
        FmVsrcPage p;
        memset(&p, 0, sizeof p);
        vsrc_youtube_parse_search(text, len, &p);
        vsrc_youtube_parse_videos(text, len, &p);
        vsrc_page_free(&p);
        vsrc_archive_parse_search(text, len, 1, &p);
        vsrc_page_free(&p);
        vsrc_peertube_parse_search(text, len, 0, &p);
        vsrc_page_free(&p);
        vsrc_dailymotion_parse_search(text, len, 1, &p);
        vsrc_page_free(&p);
        vsrc_ytdlp_parse_search(text, len, "youtube", 1, 24, &p);
        vsrc_page_free(&p);
        vsrc_ytdlp_parse_search(text, len, "web", 1, 24, &p);
        vsrc_page_free(&p);
        vsrc_archive_pick(text, len, "x", &c, st, err, sizeof err);
        vsrc_peertube_pick(text, len, &c, st, err, sizeof err);
        vsrc_youtube_error(text, len, 400, err, sizeof err);
        runs++;
      }
    }
  }
  TEST_CHECK(runs > 500);
  /* a truncated reply is an error with text, never a half page */
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(vsrc_youtube_parse_search(kYtSearch, strlen(kYtSearch) / 2, &p) == FM_ERR_FORMAT && p.error[0] &&
             !p.count);
  vsrc_page_free(&p);
  fm_free(st);
}

static void make_file(const char *path, size_t size, i64 mtime) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  for (size_t i = 0; i < size; i++) fputc((int)(i & 0xFF), f);
  fclose(f);
  if (mtime) plat_set_mtime(path, mtime);
}

static void vt_cache(const char *tmp) {
  char dir[FM_PATH_MAX], p[FM_PATH_MAX];
  fm_path_join(dir, sizeof dir, tmp, "cache");
  plat_mkdirs(dir);
  i64 now = plat_time_unix();
  static const char *const names[] = {"youtube-old-360.v.webm", "youtube-mid-360.a.webm",
                                      "archive-x-1.av.mp4", "notours.bin", "youtube-keep-360.v.webm"};
  i64 ages[] = { 5000, 4000, 3000, 9000, 9999 };
  for (int i = 0; i < FM_COUNT(names); i++) {
    fm_path_join(p, sizeof p, dir, names[i]);
    make_file(p, 1000, now - ages[i]);
  }
  /* 5000 bytes, keep at most 3000: the two oldest of ours go, others stay */
  vsrc_cache_trim(dir, 3000, "youtube-keep");
  fm_path_join(p, sizeof p, dir, names[0]); TEST_CHECK(!plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[1]); TEST_CHECK(!plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[2]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[3]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[4]); TEST_CHECK(plat_exists(p));
  vsrc_cache_trim(dir, 0, NULL);            /* only adapter files are ever deleted */
  fm_path_join(p, sizeof p, dir, names[3]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[4]); TEST_CHECK(!plat_exists(p));
  vsrc_cache_trim(NULL, 0, NULL);
  fm_path_join(p, sizeof p, tmp, "no-such-dir");
  vsrc_cache_trim(p, 0, NULL);
}

typedef struct Prog { int calls; float last; char status[160]; } Prog;

static bool on_prog(void *user, float frac, const char *status) {
  Prog *p = (Prog *)user;
  p->calls++;
  p->last = frac;
  fm_strlcpy(p->status, status, sizeof p->status);
  return true;
}

static void vt_save(const char *tmp) {
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "cache2");
  FmVsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "T7ghd5-VfvY", sizeof it.id);
  fm_strlcpy(it.title, "\xE7\x8C\xAB / Caf\xC3\xA9: \"test\"", sizeof it.title);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  fm_path_join(st->video, sizeof st->video, tmp, "v.webm");
  fm_path_join(st->audio, sizeof st->audio, tmp, "a.webm");
  make_file(st->video, 300000, 0);
  make_file(st->audio, 1000, 0);
  st->local = true;
  char dl[FM_PATH_MAX], out[FM_PATH_MAX], err[256], want[FM_PATH_MAX];
  fm_path_join(dl, sizeof dl, tmp, "dl");
  Prog pg;
  memset(&pg, 0, sizeof pg);
  /* no ffmpeg: two files, said in the status */
  TEST_CHECK(vsrc_save_stream(&c, &it, st, dl, out, sizeof out, on_prog, &pg, err, sizeof err, NULL) ==
             FM_OK);
  fm_path_join(want, sizeof want, dl, "\xE7\x8C\xAB _ Caf\xC3\xA9_ _test_ [T7ghd5-VfvY].video.webm");
  TEST_CHECK(!strcmp(out, want) && plat_exists(want));
  fm_path_join(want, sizeof want, dl, "\xE7\x8C\xAB _ Caf\xC3\xA9_ _test_ [T7ghd5-VfvY].audio.webm");
  TEST_CHECK(plat_exists(want));
  TEST_CHECK(strstr(pg.status, "2 files") != NULL && pg.calls >= 2);
  FmStat s;
  TEST_CHECK(plat_stat(out, &s) && s.size == 300000);
  /* single file; a second save does not overwrite the first */
  st->audio[0] = 0;
  TEST_CHECK(vsrc_save_stream(&c, &it, st, dl, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(plat_exists(out) && fm_ends_with_i(out, "[T7ghd5-VfvY].webm"));
  TEST_CHECK(vsrc_save_stream(&c, &it, st, dl, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(plat_exists(out) && fm_ends_with_i(out, "[T7ghd5-VfvY] (2).webm"));
  /* cancelled */
  volatile int cancel = 1;
  TEST_CHECK(vsrc_save_stream(&c, &it, st, dl, out, sizeof out, NULL, NULL, err, sizeof err, &cancel) ==
             FM_ERR_CANCEL);
  fm_free(st);
}

static void vt_no_helpers(void) {
  /* yt-dlp missing: clean errors, no crash */
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.max_height = 360;
  FmVsrcPage p;
  memset(&p, 0, sizeof p);
  FmErr e = g_vsrc_web.search(&c, "https://example.com/v", NULL, &p, NULL);
  TEST_CHECK(e != FM_OK && strstr(p.error, "yt-dlp") != NULL);
  vsrc_page_free(&p);
  /* a playlist link is yt-dlp's (one video, or a search without a key, is built in) */
  e = g_vsrc_youtube.search(&c, "https://www.youtube.com/playlist?list=PL0123456789", NULL, &p, NULL);
  TEST_CHECK(e != FM_OK && strstr(p.error, "yt-dlp") != NULL);
  vsrc_page_free(&p);
  e = g_vsrc_youtube.search(&c, "   ", NULL, &p, NULL);
  TEST_CHECK(e != FM_OK && p.error[0]);
  vsrc_page_free(&p);
  FmVsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "x", sizeof it.id);
  fm_strlcpy(it.page, "https://www.dailymotion.com/video/x", sizeof it.page);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  char err[256];
  TEST_CHECK(g_vsrc_dailymotion.resolve(&c, &it, st, NULL, NULL, err, sizeof err, NULL) != FM_OK && err[0]);
  fm_free(st);
  FmVsrcConf snap;
  vsrc_conf_snapshot(&snap);
  TEST_CHECK(fm_ends_with_i(snap.cache_dir, "online") && snap.download_dir[0] && snap.max_height >= 144);
  printf("  helpers: yt-dlp %s, js %s, ffmpeg %s, ffmpeg libs %s, region \"%s\"\n",
         snap.ytdlp[0] ? snap.ytdlp : "-", snap.js_runtime[0] ? snap.js_runtime : "-",
         snap.ffmpeg_dir[0] ? snap.ffmpeg_dir : "-", snap.have_ffmpeg_libs ? "yes" : "no", snap.region);
}

/* ---- online ---------------------------------------------------------------------- */

static bool env_on(const char *name) {
  const char *v = getenv(name);
  return v && !strcmp(v, "1");
}

/* decodes until enough picture and sound arrived; returns frames seen */
static void play_some(FmVid *v, int want, int *frames, int *blocks) {
  FmVidFrame vf;
  FmVidPcm pcm;
  *frames = *blocks = 0;
  for (int i = 0; i < 4000 && (*frames < want || (*blocks < want && vid_info(v)->has_audio)); i++) {
    int ev = vid_decode(v, &vf, &pcm);
    if (ev == VID_EV_VIDEO) (*frames)++;
    else if (ev == VID_EV_AUDIO) (*blocks)++;
    else break;
  }
}

static bool report_play(const char *what, const char *video, const char *audio) {
  /* Media Foundation is compiled out of tcc builds; elsewhere off Windows
  ** only the FFmpeg libraries decode these streams */
#if defined(FM_WIN) && !defined(__TINYC__)
  bool can = true;
#else
  bool can = ff_available();
#endif
  if (!can) {
    printf("  %s: skipped, this build has no decoder for it (needs the FFmpeg libraries)\n", what);
    return true;
  }
  u64 t0 = plat_now_ms();
  FmErr e = FM_OK;
  FmVid *v = audio && *audio ? vid_open_pair(video, audio, 0, &e) : vid_open(video, 0, &e);
  if (!v) {
    printf("  %s: open FAILED (%s, %d ms)\n", what, fm_err_str(e), (int)(plat_now_ms() - t0));
    return false;
  }
  u64 t1 = plat_now_ms();
  int frames, blocks;
  play_some(v, 5, &frames, &blocks);
  const FmVidInfo *in = vid_info(v);
  printf("  %s: %s %dx%d %s/%s %.0f s, open %d ms, %d frames + %d audio blocks in %d ms\n", what, in->backend,
         in->w, in->h, in->vcodec, in->acodec, in->duration, (int)(t1 - t0), frames, blocks,
         (int)(plat_now_ms() - t1));
  bool ok = frames >= 3 && (!in->has_audio || blocks >= 3);
  vid_close(v);
  return ok;
}

static int search_report(const FmVsrc *s, const FmVsrcConf *c, const char *q, const char *token,
                         FmVsrcPage *p) {
  memset(p, 0, sizeof *p);
  u64 t0 = plat_now_ms();
  FmErr e = s->search(c, q, token, p, NULL);
  int with_dur = 0;
  for (int i = 0; i < p->count; i++) with_dur += p->items[i].duration > 0;
  printf("  %s search \"%s\"%s%s: %s, %d items (%d with duration), next \"%s\" (%d ms)%s%s\n", s->name, q,
         token ? " page " : "", token ? token : "", fm_err_str(e), p->count, with_dur, p->next,
         (int)(plat_now_ms() - t0), p->error[0] ? " -- " : "", p->error);
  if (p->count)
    printf("    first: \"%s\" by %s, %.0f s, %lld views, %s\n", p->items[0].title, p->items[0].channel,
           p->items[0].duration, (long long)p->items[0].views, p->items[0].thumb);
  TEST_CHECK(e == FM_OK && p->count > 0);
  return p->count;
}

static void vt_online(const char *tmp) {
  if (!env_on("MMCFM_NET_TEST")) return;
  if (!net_available()) { printf("  network: %s (skipped)\n", net_backend()); return; }
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.max_height = 480;
  c.safe_search = true;
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "ocache");
  FmVsrcPage p;
  char err[256];
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);

  /* YouTube API with a bad key: the error text the user would see */
  fm_strlcpy(c.api_key_youtube, "INVALID", sizeof c.api_key_youtube);
  memset(&p, 0, sizeof p);
  FmErr e = g_vsrc_youtube.search(&c, "test", NULL, &p, NULL);
  printf("  YouTube API, bad key: %s, \"%s\"\n", fm_err_str(e), p.error);
  TEST_CHECK(e == FM_ERR_ACCESS && strstr(p.error, "not valid"));
  vsrc_page_free(&p);
  c.api_key_youtube[0] = 0;

  if (search_report(&g_vsrc_archive, &c, "popeye", NULL, &p)) {
    FmVsrcPage p2;
    if (p.next[0]) { search_report(&g_vsrc_archive, &c, "popeye", p.next, &p2); vsrc_page_free(&p2); }
    bool played = false;
    for (int i = 0; i < p.count && i < 6 && !played; i++) {
      u64 t0 = plat_now_ms();
      e = g_vsrc_archive.resolve(&c, &p.items[i], st, NULL, NULL, err, sizeof err, NULL);
      printf("  archive resolve %s: %s %s (%d ms)\n", p.items[i].id, fm_err_str(e),
             e == FM_OK ? st->video : err, (int)(plat_now_ms() - t0));
      if (e == FM_OK) played = report_play("archive play", st->video, NULL);
    }
    TEST_CHECK(played);
  }
  vsrc_page_free(&p);

  if (search_report(&g_vsrc_peertube, &c, "blender", NULL, &p)) {
    bool played = false;
    for (int i = 0; i < p.count && i < 6 && !played; i++) {
      if (p.items[i].live) continue;
      u64 t0 = plat_now_ms();
      e = g_vsrc_peertube.resolve(&c, &p.items[i], st, NULL, NULL, err, sizeof err, NULL);
      printf("  peertube resolve %s: %s %s%s%s (%d ms)\n", p.items[i].id, fm_err_str(e),
             e == FM_OK ? st->video : err, st->audio[0] ? " + " : "", st->audio, (int)(plat_now_ms() - t0));
      if (e == FM_OK) played = report_play("peertube play", st->video, st->audio);
    }
    TEST_CHECK(played);
  }
  vsrc_page_free(&p);

  if (search_report(&g_vsrc_dailymotion, &c, "cats", NULL, &p) && p.next[0]) {
    FmVsrcPage p2;
    search_report(&g_vsrc_dailymotion, &c, "cats", p.next, &p2);
    vsrc_page_free(&p2);
  }
  vsrc_page_free(&p);
  fm_free(st);
}

typedef struct CancelAt { float frac; } CancelAt;

static bool cancel_at(void *user, float frac, const char *status) {
  CancelAt *c = (CancelAt *)user;
  FM_UNUSED(status);
  c->frac = frac;
  return frac < 0.05f;              /* stop once the download is under way */
}

static int count_prefix(const char *dir, const char *prefix) {
  FmErr e;
  FmDir *d = plat_dir_open(dir, &e);
  int n = 0;
  const char *name;
  FmStat st;
  while (d && plat_dir_next(d, &name, &st))
    if (!strncmp(name, prefix, strlen(prefix))) { printf("    left: %s\n", name); n++; }
  if (d) plat_dir_close(d);
  return n;
}

static void vt_ytdlp_live(const char *tmp) {
  const char *yt = getenv("MMCFM_YTDLP");
  if (!yt || !*yt || !proc_available()) return;
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  fm_strlcpy(c.ytdlp, yt, sizeof c.ytdlp);
  const char *js = getenv("MMCFM_JS");
  if (js && *js) fm_strlcpy(c.js_runtime, js, sizeof c.js_runtime);
  else vsrc_find_js_runtime(c.js_runtime, sizeof c.js_runtime);
  c.max_height = 360;
  c.have_ffmpeg_libs = ff_available();
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "ycache");
  printf("  yt-dlp %s, js runtime \"%s\", ffmpeg libs %s\n", c.ytdlp, c.js_runtime,
         c.have_ffmpeg_libs ? "yes" : "no");
  FmVsrcPage p;
  char err[256];
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);

  if (search_report(&g_vsrc_youtube, &c, "me at the zoo", NULL, &p) && p.next[0]) {
    FmVsrcPage p2;
    search_report(&g_vsrc_youtube, &c, "me at the zoo", p.next, &p2);
    vsrc_page_free(&p2);
  }
  vsrc_page_free(&p);
  search_report(&g_vsrc_web, &c, "https://www.youtube.com/watch?v=jNQXAC9IVRw", NULL, &p);
  TEST_CHECK(p.count == 1 && strstr(p.items[0].title, "zoo"));
  vsrc_page_free(&p);

  /* resolve a 19 s video: stream URLs and the quality list; play the pair
  ** streaming; then the cache fallback, and again from the cache */
  FmVsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "jNQXAC9IVRw", sizeof it.id);
  fm_strlcpy(it.title, "Me at the zoo", sizeof it.title);
  fm_strlcpy(it.page, "https://www.youtube.com/watch?v=jNQXAC9IVRw", sizeof it.page);
  Prog pg;
  memset(&pg, 0, sizeof pg);
  u64 t0 = plat_now_ms();
  FmErr e = g_vsrc_youtube.resolve(&c, &it, st, on_prog, &pg, err, sizeof err, NULL);
  printf("  youtube resolve (stream): %s (%d ms, %d progress calls, last \"%s\")%s%s\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), pg.calls, pg.status, e ? " -- " : "", e ? err : "");
  TEST_CHECK(e == FM_OK && !st->local && st->nq > 0 && st->cur >= 0 && !strncmp(st->video, "https://", 8));
  if (e == FM_OK) {
    for (int i = 0; i < st->nq; i++)
      printf("    %c %-10s %-6s %5d kb/s %s%s%s\n", i == st->cur ? '*' : ' ', st->q[i].label, st->q[i].codec,
             st->q[i].kbps, st->q[i].playable ? "stream" : st->q[i].cache_only ? "cache" : "-",
             st->q[i].needs_ffmpeg ? " needs FFmpeg" : "", st->q[i].muxed ? " muxed" : "");
    TEST_CHECK(report_play("youtube stream pair", st->video, st->audio[0] ? st->audio : NULL));
  }
  c.force_cache = true;
  t0 = plat_now_ms();
  e = g_vsrc_youtube.resolve(&c, &it, st, on_prog, &pg, err, sizeof err, NULL);
  printf("  youtube resolve (cache): %s (%d ms)%s%s\n", fm_err_str(e), (int)(plat_now_ms() - t0), e ? " -- " : "",
         e ? err : "");
  TEST_CHECK(e == FM_OK && st->local);
  if (e == FM_OK) {
    printf("    video %s\n    audio %s\n", fm_path_base(st->video),
           st->audio[0] ? fm_path_base(st->audio) : "-");
    TEST_CHECK(report_play("youtube pair", st->video, st->audio));
    t0 = plat_now_ms();
    e = g_vsrc_youtube.resolve(&c, &it, st, NULL, NULL, err, sizeof err, NULL);
    int ms = (int)(plat_now_ms() - t0);
    printf("  youtube resolve again (cache): %s, %d ms\n", fm_err_str(e), ms);
    TEST_CHECK(e == FM_OK && ms < 1000);
  }

  /* cancel mid-download: an error, and nothing of it left in the cache */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "z01gL_ahiOQ", sizeof it.id);
  fm_strlcpy(it.page, "https://www.youtube.com/watch?v=z01gL_ahiOQ", sizeof it.page);
  CancelAt ca;
  memset(&ca, 0, sizeof ca);
  t0 = plat_now_ms();
  e = g_vsrc_youtube.resolve(&c, &it, st, cancel_at, &ca, err, sizeof err, NULL);
  printf("  youtube resolve cancelled at %.0f%%: %s, \"%s\" (%d ms)\n", ca.frac * 100, fm_err_str(e), err,
         (int)(plat_now_ms() - t0));
  TEST_CHECK(e == FM_ERR_CANCEL);
  TEST_CHECK(count_prefix(c.cache_dir, "youtube-z01gL_ahiOQ") == 0);
  c.force_cache = false;

  /* Dailymotion: natively it streams (ftest_hls.c, MMCFM_DM_TEST); the
  ** cache request still goes through yt-dlp (the fallback) */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "x9i92l8", sizeof it.id);
  fm_strlcpy(it.title, "Cats", sizeof it.title);
  fm_strlcpy(it.page, "https://www.dailymotion.com/video/x9i92l8", sizeof it.page);
  t0 = plat_now_ms();
  c.force_cache = true;
  e = g_vsrc_dailymotion.resolve(&c, &it, st, NULL, NULL, err, sizeof err, NULL);
  c.force_cache = false;
  printf("  dailymotion resolve (yt-dlp into the cache): %s (%d ms) %s, %d qualities\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), e == FM_OK ? fm_path_base(st->video) : err, st->nq);
  TEST_CHECK(e == FM_OK && st->local);
  if (e == FM_OK) TEST_CHECK(report_play("dailymotion play", st->video, st->audio));

  /* download, non-ASCII title: without ffmpeg (two files), then merged */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "T7ghd5-VfvY", sizeof it.id);
  fm_strlcpy(it.title,
             "10\xE7\xA7\x92\xE7\x8C\xAB\xEF\xBC\x88\xE7\x99\xBD\xE9\xBB\x92\xE3\x81\xB6\xE3\x81\xA1\xEF\xBC"
             "\x89 CAT",
             sizeof it.title);
  fm_strlcpy(it.page, "https://www.youtube.com/watch?v=T7ghd5-VfvY", sizeof it.page);
  char dl[FM_PATH_MAX], out[FM_PATH_MAX];
  fm_path_join(dl, sizeof dl, tmp, "downloads \xC3\xA9");
  memset(&pg, 0, sizeof pg);
  t0 = plat_now_ms();
  e = g_vsrc_youtube.download(&c, &it, dl, out, sizeof out, on_prog, &pg, err, sizeof err, NULL);
  printf("  youtube download, no ffmpeg: %s (%d ms) \"%s\" status \"%s\"%s\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), e == FM_OK ? fm_path_base(out) : err, pg.status,
         plat_exists(out) ? "" : " MISSING");
  TEST_CHECK(e == FM_OK && plat_exists(out));
  const char *ff = getenv("MMCFM_FFMPEG");
  if (ff && *ff) {
    fm_strlcpy(c.ffmpeg_dir, ff, sizeof c.ffmpeg_dir);
    memset(&pg, 0, sizeof pg);
    t0 = plat_now_ms();
    e = g_vsrc_youtube.download(&c, &it, dl, out, sizeof out, on_prog, &pg, err, sizeof err, NULL);
    printf("  youtube download, ffmpeg: %s (%d ms) \"%s\" status \"%s\"\n", fm_err_str(e),
           (int)(plat_now_ms() - t0), e == FM_OK ? fm_path_base(out) : err, pg.status);
    TEST_CHECK(e == FM_OK && plat_exists(out));
    if (e == FM_OK) TEST_CHECK(report_play("merged download", out, NULL));
  }
  fm_free(st);
}

/* "Get yt-dlp" writes into the real config folder: opt-in only (run it with
** APPDATA / XDG_CONFIG_HOME pointed at a scratch folder) */
static void vt_install(void) {
  if (!env_on("MMCFM_INSTALL_TEST") || !net_available() || !proc_available()) return;
  char path[FM_PATH_MAX], found[FM_PATH_MAX];
  Prog pg;
  memset(&pg, 0, sizeof pg);
  u64 t0 = plat_now_ms();
  FmErr e = vsrc_install_ytdlp(path, sizeof path, on_prog, &pg, NULL);
  FmStat st;
  printf("  install yt-dlp: %s, %s, %llu bytes (%d ms, %d progress calls, last \"%s\")\n", fm_err_str(e),
         path, plat_stat(path, &st) ? (unsigned long long)st.size : 0ULL, (int)(plat_now_ms() - t0), pg.calls,
         pg.status);
  TEST_CHECK(e == FM_OK && plat_exists(path));
  TEST_CHECK(vsrc_find_ytdlp(found, sizeof found));
  printf("  found again: %s\n", found);
}

/* MMCFM_STREAM_PROBE=<video url>|<audio url>: how long each half takes to
** open and to give its first frame / sound (MMCFM_VIDEO_BACKEND picks one). */
static void vt_stream_probe(void) {
  const char *spec = getenv("MMCFM_STREAM_PROBE");
  if (!spec || !strchr(spec, '|')) return;
  static char u[2][4096];
  fm_strlcpy(u[0], spec, FM_MIN(sizeof u[0], (size_t)(strchr(spec, '|') - spec) + 1));
  fm_strlcpy(u[1], strchr(spec, '|') + 1, sizeof u[1]);
  for (int i = 0; i < 2; i++) {
    if (!u[i][0]) continue;
    u64 t0 = plat_now_ms();
    FmErr err;
    FmVid *v = vid_open(u[i], i == 0 ? VID_OPEN_NO_AUDIO : VID_OPEN_AUDIO_ONLY, &err);
    u64 t1 = plat_now_ms();
    if (!v) { printf("  probe %s: open failed (%s) after %d ms\n", i ? "audio" : "video", fm_err_str(err), (int)(t1 - t0)); continue; }
    FmVidFrame vf;
    FmVidPcm pc;
    int ev = 0, n = 0;
    while (n++ < 400 && (ev = vid_decode(v, &vf, &pc)) > 0 && ev != (i == 0 ? VID_EV_VIDEO : VID_EV_AUDIO)) {}
    u64 t2 = plat_now_ms();
    bool sk = vid_seek(v, vid_info(v)->duration * 0.5);
    while (sk && n++ < 2000 && (ev = vid_decode(v, &vf, &pc)) > 0 && ev != (i == 0 ? VID_EV_VIDEO : VID_EV_AUDIO)) {}
    u64 t3 = plat_now_ms();
    printf("  probe %s: %s open %d ms, first %s %d ms, seek+first %d ms (ev %d)\n", i ? "audio" : "video",
           vid_info(v)->backend, (int)(t1 - t0), i ? "sound" : "frame", (int)(t2 - t1), (int)(t3 - t2), ev);
    vid_close(v);
  }
}


/* ---- YouTube without yt-dlp (fvsrc_innertube.c) ------------------------------------ */

static const char *kItSearch =
  "{\"contents\":{\"twoColumnSearchResultsRenderer\":{\"primaryContents\":{\"sectionListRenderer\":{\"contents\":["
  "{\"itemSectionRenderer\":{\"contents\":["
  "{\"videoRenderer\":{\"videoId\":\"aqz-KE-bpKQ\",\"title\":{\"runs\":[{\"text\":\"Big Buck Bunny\"}]},"
  "\"ownerText\":{\"runs\":[{\"text\":\"Blender\"}]},\"lengthText\":{\"simpleText\":\"10:35\"},"
  "\"viewCountText\":{\"simpleText\":\"1,234,567 views\"},"
  "\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/vi/aqz-KE-bpKQ/default.jpg\"},"
  "{\"url\":\"https://i.ytimg.com/vi/aqz-KE-bpKQ/hq720.jpg\"}]}}},"
  "{\"shelfRenderer\":{\"content\":{\"verticalListRenderer\":{\"items\":["
  "{\"videoRenderer\":{\"videoId\":\"rFZHOHl-L8A\",\"title\":{\"runs\":[{\"text\":\"radio\"}]},"
  "\"viewCountText\":{\"runs\":[{\"text\":\"9,247\"},{\"text\":\" watching\"}]},"
  "\"badges\":[{\"metadataBadgeRenderer\":{\"style\":\"BADGE_STYLE_TYPE_LIVE_NOW\",\"label\":\"LIVE\"}}]}},"
  "{\"videoRenderer\":{\"videoId\":\"aqz-KE-bpKQ\",\"title\":{\"runs\":[{\"text\":\"again\"}]}}},"
  "{\"videoRenderer\":{\"videoId\":\"bad id\",\"title\":{\"simpleText\":\"x\"}}}"
  "]}}}}]}},"
  "{\"continuationItemRenderer\":{\"continuationEndpoint\":{\"continuationCommand\":{\"token\":\"NEXT-PAGE-TOKEN\"}}}}"
  "]}}}}}";

#define IT_FMT(itag, mime, extra) \
  "{\"itag\":" #itag ",\"url\":\"https://rr1.googlevideo.com/videoplayback?itag=" #itag "\",\"mimeType\":\"" mime \
  "\",\"bitrate\":500000,\"contentLength\":\"1000000\"" extra "}"

static const char *kItPlayer =
  "{\"playabilityStatus\":{\"status\":\"OK\"},\"videoDetails\":{\"videoId\":\"aqz-KE-bpKQ\",\"lengthSeconds\":\"635\"},"
  "\"streamingData\":{\"adaptiveFormats\":["
  IT_FMT(243, "video/webm; codecs=\\\"vp9\\\"", ",\"width\":640,\"height\":360,\"fps\":30") ","
  IT_FMT(134, "video/mp4; codecs=\\\"avc1.4D401E\\\"", ",\"width\":640,\"height\":360,\"fps\":30") ","
  IT_FMT(302, "video/webm; codecs=\\\"vp9\\\"", ",\"width\":1280,\"height\":720,\"fps\":60") ","
  "{\"itag\":299,\"signatureCipher\":\"s=xx&url=https%3A%2F%2Fexample\",\"mimeType\":\"video/mp4; codecs=\\\"avc1.64002A\\\"\","
  "\"width\":1920,\"height\":1080}," /* ciphered: skipped */
  IT_FMT(160, "video/mp4; codecs=\\\"avc1.4D400C\\\"", ",\"width\":256,\"height\":144,\"type\":\"FORMAT_STREAM_TYPE_OTF\"") ","
  IT_FMT(140, "audio/mp4; codecs=\\\"mp4a.40.2\\\"", "") ","
  IT_FMT(251, "audio/webm; codecs=\\\"opus\\\"", ",\"isDrc\":true") ","
  IT_FMT(251, "audio/webm; codecs=\\\"opus\\\"", "")
  "]}}";

static void vt_innertube(void) {
  char id[16];
  TEST_CHECK(vsrc_innertube_id("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=10", id, sizeof id) &&
             !strcmp(id, "aqz-KE-bpKQ"));
  TEST_CHECK(vsrc_innertube_id("https://youtu.be/aqz-KE-bpKQ?si=x", id, sizeof id) && !strcmp(id, "aqz-KE-bpKQ"));
  TEST_CHECK(vsrc_innertube_id("https://m.youtube.com/shorts/aqz-KE-bpKQ", id, sizeof id));
  TEST_CHECK(vsrc_innertube_id("https://www.youtube.com/embed/aqz-KE-bpKQ", id, sizeof id));
  TEST_CHECK(vsrc_innertube_id("aqz-KE-bpKQ", id, sizeof id));
  TEST_CHECK(!vsrc_innertube_id("https://www.youtube.com/playlist?list=PL123", id, sizeof id));
  TEST_CHECK(!vsrc_innertube_id("https://example.com/watch?v=aqz-KE-bpKQ", id, sizeof id));
  TEST_CHECK(!vsrc_innertube_id("https://youtu.be/short", id, sizeof id));

  FmVsrcPage pg;
  memset(&pg, 0, sizeof pg);
  TEST_CHECK(vsrc_innertube_parse_search(kItSearch, strlen(kItSearch), &pg) == FM_OK);
  TEST_CHECK(pg.count == 2);                                  /* the repeat and the bad id dropped */
  if (pg.count == 2) {
    TEST_CHECK(!strcmp(pg.items[0].title, "Big Buck Bunny") && !strcmp(pg.items[0].channel, "Blender"));
    TEST_CHECK(pg.items[0].duration == 635 && pg.items[0].views == 1234567 && !pg.items[0].live);
    TEST_CHECK(strstr(pg.items[0].thumb, "hq720") && strstr(pg.items[0].page, "watch?v=aqz-KE-bpKQ"));
    TEST_CHECK(pg.items[1].live && pg.items[1].views == 9247);
  }
  TEST_CHECK(!strncmp(pg.next, "i:", 2));
  vsrc_page_free(&pg);

  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.max_height = 720;
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  char err[160];
  size_t n = strlen(kItPlayer);
  /* Windows-like: VP9 + Opus (not the DRC copy); the ciphered and OTF formats are gone */
  c.os_mp4 = true;
  TEST_CHECK(vsrc_innertube_pick(kItPlayer, n, &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(strstr(st->video, "itag=302") && strstr(st->audio, "itag=251") && st->duration == 635);
  TEST_CHECK(q_find(st, "1080p") < 0 && q_find(st, "144p") < 0 && q_find(st, "720p60") >= 0);
  TEST_CHECK(st->q[st->nq - 1].audio_only && !strcmp(st->q[st->nq - 1].codec, "Opus"));
  /* Android with MediaCodec streaming DASH: H.264 + AAC preferred */
  c.os_dash = true;
  c.max_height = 360;
  TEST_CHECK(vsrc_innertube_pick(kItPlayer, n, &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(strstr(st->video, "itag=134") && strstr(st->audio, "itag=140"));
  /* only the built-in decoder: VP9 + Opus */
  c.os_mp4 = c.os_dash = false;
  TEST_CHECK(vsrc_innertube_pick(kItPlayer, n, &c, st, err, sizeof err) == FM_OK);
  TEST_CHECK(strstr(st->video, "itag=243") && strstr(st->audio, "itag=251"));
  /* refused / not a reply */
  const char *live = "{\"playabilityStatus\":{\"status\":\"OK\"},\"videoDetails\":{\"isLive\":true}}";
  TEST_CHECK(vsrc_innertube_pick(live, strlen(live), &c, st, err, sizeof err) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(vsrc_innertube_pick("nope", 4, &c, st, err, sizeof err) == FM_ERR_FORMAT);
  fm_free(st);
}

/* MMCFM_YT_TEST=<video id or link>: the built-in path against real YouTube:
** search (two pages), resolve, then open the chosen pair and decode. */
static void vt_innertube_live(void) {
  const char *spec = getenv("MMCFM_YT_TEST");
  if (!spec || !*spec) return;
  FmVsrcConf c;
  vsrc_conf_snapshot(&c);
  char id[16], err[256];
  TEST_CHECK(vsrc_innertube_id(spec, id, sizeof id));
  u64 t0 = plat_now_ms();
  FmVsrcPage pg;
  memset(&pg, 0, sizeof pg);
  FmErr e = vsrc_innertube_search(&c, "big buck bunny", NULL, &pg, NULL);
  u64 t1 = plat_now_ms();
  printf("  yt search: %s, %d results in %d ms%s%s\n", fm_err_str(e), pg.count, (int)(t1 - t0), pg.error[0] ? ": " : "",
         pg.error);
  TEST_CHECK(e == FM_OK && pg.count > 5 && pg.next[0]);
  if (pg.next[0]) {
    char next[256];
    fm_strlcpy(next, pg.next, sizeof next);
    vsrc_page_free(&pg);
    memset(&pg, 0, sizeof pg);
    e = vsrc_innertube_search(&c, "big buck bunny", next, &pg, NULL);
    printf("  yt search page 2: %s, %d results\n", fm_err_str(e), pg.count);
    TEST_CHECK(e == FM_OK && pg.count > 5);
  }
  vsrc_page_free(&pg);
  FmVsrcItem it;
  e = vsrc_innertube_item(id, &it, err, sizeof err, NULL);
  printf("  yt item: %s \"%s\" by %s, %.0f s\n", fm_err_str(e), it.title, it.channel, it.duration);
  TEST_CHECK(e == FM_OK && it.title[0]);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  t0 = plat_now_ms();
  e = vsrc_innertube_resolve(&c, id, st, err, sizeof err, NULL);
  t1 = plat_now_ms();
  printf("  yt resolve: %s in %d ms%s%s\n", fm_err_str(e), (int)(t1 - t0), err[0] ? ": " : "", err);
  TEST_CHECK(e == FM_OK && st->video[0]);
  for (int i = 0; i < st->nq; i++)
    printf("    %s %-10s %-6s %5d kbps%s%s%s\n", i == st->cur ? ">" : " ", st->q[i].label, st->q[i].codec, st->q[i].kbps,
           st->q[i].playable ? " stream" : "", st->q[i].cache_only ? " cache" : "", st->q[i].needs_ffmpeg ? " ffmpeg" : "");
  if (e == FM_OK) {
    t0 = plat_now_ms();
    FmErr ve;
    FmVid *v = vid_open_pair(st->video, st->audio, 0, &ve);
    t1 = plat_now_ms();
    TEST_CHECK(v != NULL);
    if (v) {
      FmVidFrame vf;
      FmVidPcm pc;
      int ev, nv = 0, na = 0, guard = 0;
      while ((nv < 60 || na < 60) && guard++ < 5000 && (ev = vid_decode(v, &vf, &pc)) > 0) {
        if (ev == VID_EV_VIDEO) nv++;
        else if (ev == VID_EV_AUDIO) na++;
      }
      u64 t2 = plat_now_ms();
      printf("  yt play: %s %dx%d, open %d ms, 60 frames + 60 sound blocks %d ms (v%d a%d)\n", vid_info(v)->backend,
             vid_info(v)->w, vid_info(v)->h, (int)(t1 - t0), (int)(t2 - t1), nv, na);
      TEST_CHECK(nv >= 60 && na >= 60);
      vid_close(v);
    }
  }
  fm_free(st);
}


/* ---- Bilibili ------------------------------------------------------------------------ */

static const char *kBlSearch =
  "{\"code\":0,\"data\":{\"page\":1,\"numPages\":50,\"result\":["
  "{\"bvid\":\"BV1Gse26GEqw\",\"title\":\"<em class=\\\"keyword\\\">Cat</em> &amp; dog\",\"author\":\"Up&#39;s\","
  "\"pic\":\"//i0.hdslb.com/bfs/archive/abc.jpg\",\"duration\":\"14:54\",\"play\":12345,\"pubdate\":1700000000},"
  "{\"bvid\":\"bad\",\"title\":\"x\"},"
  "{\"bvid\":\"BV1xx411c7mD\",\"title\":\"two\",\"duration\":\"1:02:03\",\"play\":0}]}}";

static void vt_bilibili(void) {
  char hex[33];
  md5_hex("", 0, hex);
  TEST_CHECK(!strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e"));
  md5_hex("The quick brown fox jumps over the lazy dog", 43, hex);
  TEST_CHECK(!strcmp(hex, "9e107d9d372bb6826bd81d3542a419d6"));
  static char big[1000];
  memset(big, 'a', sizeof big);
  md5_hex(big, sizeof big, hex);                   /* crosses blocks */
  TEST_CHECK(!strcmp(hex, "cabe45dcc9ae5b66ba86600cca6b8ba8"));

  char key[33];
  vsrc_bilibili_mixin("https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png",
                      "https://i0.hdslb.com/bfs/wbi/4932caff0ff746eab6f01bf08b70ac45.png", key);
  TEST_CHECK(!strcmp(key, "ea1db124af3c7062474693fa704f4ff8"));
  BlParam p[3];
  memset(p, 0, sizeof p);
  p[0].k = "foo"; fm_strlcpy(p[0].v, "114", sizeof p[0].v);
  p[1].k = "bar"; fm_strlcpy(p[1].v, "514", sizeof p[1].v);
  p[2].k = "zab"; fm_strlcpy(p[2].v, "1919810", sizeof p[2].v);
  char q[512];
  vsrc_bilibili_sign(p, 3, 1702204169, key, q, sizeof q);
  TEST_CHECK(!strcmp(q, "bar=514&foo=114&wts=1702204169&zab=1919810&w_rid=8f6f2b5b3d485fe1886cec6a0be8c5d4"));
  memset(p, 0, sizeof p);
  p[0].k = "keyword"; fm_strlcpy(p[0].v, "\xE7\x8C\xAB cat (1)!", sizeof p[0].v);
  p[1].k = "page"; fm_strlcpy(p[1].v, "2", sizeof p[1].v);
  vsrc_bilibili_sign(p, 2, 1702204169, key, q, sizeof q);
  TEST_CHECK(!strcmp(q, "keyword=%E7%8C%AB%20cat%201&page=2&wts=1702204169&w_rid=4d5a93a3320d825ad4d742fc34c57221"));

  char bv[16];
  TEST_CHECK(vsrc_bilibili_id("https://www.bilibili.com/video/BV1Gse26GEqw/?spm_id_from=333", bv, sizeof bv) &&
             !strcmp(bv, "BV1Gse26GEqw"));
  TEST_CHECK(vsrc_bilibili_id("BV1Gse26GEqw", bv, sizeof bv));
  TEST_CHECK(!vsrc_bilibili_id("https://example.com/video/BV1Gse26GEqw", bv, sizeof bv));
  TEST_CHECK(!vsrc_bilibili_id("BV1short", bv, sizeof bv));

  FmVsrcPage pg;
  memset(&pg, 0, sizeof pg);
  TEST_CHECK(vsrc_bilibili_parse_search(kBlSearch, strlen(kBlSearch), 1, &pg) == FM_OK);
  TEST_CHECK(pg.count == 2 && !strcmp(pg.next, "b:2"));
  if (pg.count == 2) {
    TEST_CHECK(!strcmp(pg.items[0].title, "Cat & dog") && !strcmp(pg.items[0].channel, "Up's"));
    TEST_CHECK(!strcmp(pg.items[0].thumb, "https://i0.hdslb.com/bfs/archive/abc.jpg@480w_270h_1c.jpg"));
    TEST_CHECK(pg.items[0].duration == 894 && pg.items[0].views == 12345 && pg.items[0].published[0]);
    TEST_CHECK(pg.items[1].duration == 3723 && strstr(pg.items[1].page, "/video/BV1xx411c7mD"));
  }
  vsrc_page_free(&pg);
}

/* MMCFM_BILI_TEST=<query>: search, resolve the first result, open and decode it. */
static void vt_bilibili_live(void) {
  const char *qs = getenv("MMCFM_BILI_TEST");
  if (!qs || !*qs) return;
  FmVsrcConf c;
  vsrc_conf_snapshot(&c);
  FmVsrcPage pg;
  memset(&pg, 0, sizeof pg);
  u64 t0 = plat_now_ms();
  FmErr e = g_vsrc_bilibili.search(&c, qs, NULL, &pg, NULL);
  printf("  bili search: %s, %d results in %d ms %s\n", fm_err_str(e), pg.count, (int)(plat_now_ms() - t0), pg.error);
  TEST_CHECK(e == FM_OK && pg.count > 5 && pg.next[0]);
  if (pg.next[0]) {
    FmVsrcPage p2;
    memset(&p2, 0, sizeof p2);
    e = g_vsrc_bilibili.search(&c, qs, pg.next, &p2, NULL);
    printf("  bili page 2: %s, %d results\n", fm_err_str(e), p2.count);
    TEST_CHECK(e == FM_OK && p2.count > 5);
    vsrc_page_free(&p2);
  }
  if (pg.count) {
    FmVsrcItem it = pg.items[0];
    printf("  bili item: %s \"%.60s\" by %.40s, %.0f s\n", it.id, it.title, it.channel, it.duration);
    FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
    char err[256];
    t0 = plat_now_ms();
    e = g_vsrc_bilibili.resolve(&c, &it, st, NULL, NULL, err, sizeof err, NULL);
    printf("  bili resolve: %s in %d ms %s\n", fm_err_str(e), (int)(plat_now_ms() - t0), err);
    for (int i = 0; i < st->nq; i++)
      printf("    %s %-8s %-6s %s%s\n", i == st->cur ? ">" : " ", st->q[i].label, st->q[i].codec,
             st->q[i].playable ? "stream" : "", st->q[i].needs_ffmpeg ? "ffmpeg" : "");
    TEST_CHECK(e == FM_OK && st->video[0]);
    if (e == FM_OK) {
      FmErr ve;
      t0 = plat_now_ms();
      FmVid *v = vid_open(st->video, 0, &ve);
      TEST_CHECK(v != NULL);
      if (v) {
        FmVidFrame vf;
        FmVidPcm pc;
        int ev, nv = 0, na = 0, guard = 0;
        while ((nv < 60 || na < 30) && guard++ < 5000 && (ev = vid_decode(v, &vf, &pc)) > 0) {
          if (ev == VID_EV_VIDEO) nv++;
          else if (ev == VID_EV_AUDIO) na++;
        }
        printf("  bili play: %s %dx%d, v%d a%d in %d ms\n", vid_info(v)->backend, vid_info(v)->w, vid_info(v)->h, nv, na,
               (int)(plat_now_ms() - t0));
        TEST_CHECK(nv >= 60 && na >= 30);
        vid_close(v);
      }
    }
    fm_free(st);
    /* MMCFM_BILI_DL=<dir>: the download button's path, at 360p to keep it short */
    const char *dl = getenv("MMCFM_BILI_DL");
    if (dl && *dl) {
      FmVsrcConf c2 = c;
      c2.max_height = 360;
      char out[FM_PATH_MAX], derr[256];
      u64 t1 = plat_now_ms();
      e = g_vsrc_bilibili.download(&c2, &it, dl, out, sizeof out, NULL, NULL, derr, sizeof derr, NULL);
      FmStat fs;
      bool there = e == FM_OK && plat_stat(out, &fs) && fs.size > 100000;
      printf("  bili download: %s in %d ms -> %s (%llu bytes) %s\n", fm_err_str(e), (int)(plat_now_ms() - t1), out,
             there ? (unsigned long long)fs.size : 0ull, derr);
      TEST_CHECK(there);
      if (there) {
        FmErr ve;
        FmVid *v = vid_open(out, 0, &ve);                    /* the saved file plays */
        TEST_CHECK(v && vid_info(v)->has_video && vid_info(v)->has_audio && vid_info(v)->duration > 10);
        if (v) {
          printf("  bili saved file: %s %dx%d %.0f s\n", vid_info(v)->backend, vid_info(v)->w, vid_info(v)->h,
                 vid_info(v)->duration);
          vid_close(v);
        }
      }
    }
  }
  vsrc_page_free(&pg);
}

int test_vsrc(const char *tmp) {
  int before = g_test_fail;
  vt_stream_probe();
  vt_registry();
  vt_page();
  vt_text(tmp);
  vt_urls();
  vt_youtube();
  vt_archive();
  vt_peertube();
  vt_dailymotion();
  vt_ytdlp_parse();
  vt_pick_stream();
  vt_innertube();
  vt_innertube_live();
  vt_bilibili();
  vt_bilibili_live();
  vt_garbage();
  vt_cache(tmp);
  vt_save(tmp);
  vt_no_helpers();
  vt_online(tmp);
  vt_ytdlp_live(tmp);
  vt_install();
  return g_test_fail - before;
}
