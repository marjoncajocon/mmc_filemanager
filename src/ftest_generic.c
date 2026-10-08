/* ftest_generic.c -- the native "Any site" resolver (fvsrc_generic.c):
** every discovery path on inline HTML fixtures, URL joining, entity and JS
** escape decoding, the playable/needs-FFmpeg marks and garbage pages.
**
** Offline by default. MMCFM_GENERIC_TEST=<page url> also resolves a real
** page and prints what was found (title, thumb, duration, every quality).
*/
#include "ftest.h"
#include "fvsrc.h"
#include "fvsrc_generic.h"
#include "fplat.h"

static FmVsrcConf conf(bool os_mp4, bool ffmpeg, int max_h) {
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.os_mp4 = os_mp4;
  c.have_ffmpeg_libs = ffmpeg;
  c.max_height = max_h;
  return c;
}

/* parses html as page url; returns the result code, fills *st and *it */
static FmErr parse(const char *html, const char *url, const FmVsrcConf *c, FmVsrcStream *st, FmVsrcItem *it,
                   char *embed, size_t ecap) {
  return vsrc_generic_parse(html, strlen(html), url, c, it, st, embed, ecap);
}

static void gt_urls(void) {
  const char *b = "https://ex.com/a/b/c.html?q=1#frag";
  char o[VSRC_QURL];
  TEST_CHECK(vsrc_generic_join(b, "d.mp4", o, sizeof o) && !strcmp(o, "https://ex.com/a/b/d.mp4"));
  TEST_CHECK(vsrc_generic_join(b, "../d.mp4", o, sizeof o) && !strcmp(o, "https://ex.com/a/d.mp4"));
  TEST_CHECK(vsrc_generic_join(b, "/x/./y/../z.mp4?a=1#t", o, sizeof o) && !strcmp(o, "https://ex.com/x/z.mp4?a=1"));
  TEST_CHECK(vsrc_generic_join(b, "//cdn.ex.com/v.mp4", o, sizeof o) && !strcmp(o, "https://cdn.ex.com/v.mp4"));
  TEST_CHECK(vsrc_generic_join(b, "?p=2", o, sizeof o) && !strcmp(o, "https://ex.com/a/b/c.html?p=2"));
  TEST_CHECK(vsrc_generic_join(b, "#x", o, sizeof o) && !strcmp(o, "https://ex.com/a/b/c.html?q=1"));
  TEST_CHECK(vsrc_generic_join(b, "  my clip.mp4 ", o, sizeof o) && !strcmp(o, "https://ex.com/a/b/my%20clip.mp4"));
  TEST_CHECK(vsrc_generic_join(b, "../../../../v.mp4", o, sizeof o) && !strcmp(o, "https://ex.com/v.mp4"));
  TEST_CHECK(vsrc_generic_join(b, "HTTP://Other.com/x", o, sizeof o) && !strcmp(o, "http://Other.com/x"));
  TEST_CHECK(vsrc_generic_join("https://ex.com", "v.mp4", o, sizeof o) && !strcmp(o, "https://ex.com/v.mp4"));
  TEST_CHECK(vsrc_generic_join("https://ex.com/a/", "./", o, sizeof o) && !strcmp(o, "https://ex.com/a/"));
  TEST_CHECK(!vsrc_generic_join(b, "javascript:void(0)", o, sizeof o));
  TEST_CHECK(!vsrc_generic_join(b, "data:video/mp4;base64,AAAA", o, sizeof o));
  TEST_CHECK(!vsrc_generic_join(b, "blob:https://ex.com/1", o, sizeof o));
  TEST_CHECK(!vsrc_generic_join(b, "", o, sizeof o));
  TEST_CHECK(!vsrc_generic_join("ftp://x/", "v.mp4", o, sizeof o));

  TEST_CHECK(vsrc_generic_kind("https://x/a.MP4?x=1.webm", NULL) == GEN_MP4);
  TEST_CHECK(vsrc_generic_kind("https://x/hls/master.m3u8", NULL) == GEN_HLS);
  TEST_CHECK(vsrc_generic_kind("https://x/watch?v=1", NULL) == GEN_NONE);
  TEST_CHECK(vsrc_generic_kind("https://x.mp4/", NULL) == GEN_NONE);
  TEST_CHECK(vsrc_generic_kind("clip.webm", NULL) == GEN_WEBM);
  TEST_CHECK(vsrc_generic_kind("https://x/s", "application/x-mpegURL") == GEN_HLS);
  TEST_CHECK(vsrc_generic_kind("https://x/s", "video/x-flv") == GEN_VIDEO);
  TEST_CHECK(vsrc_generic_kind("https://x/s", "audio/mpeg") == GEN_MP3);
  TEST_CHECK(vsrc_generic_kind("https://x/s", "text/html") == GEN_NONE);

  TEST_CHECK(vsrc_generic_height_hint("File.webm.720p.vp9.webm") == 720);
  TEST_CHECK(vsrc_generic_height_hint("clip-1280x720.mp4") == 720);
  TEST_CHECK(vsrc_generic_height_hint("video_480.mp4") == 480);
  TEST_CHECK(vsrc_generic_height_hint("1080p60") == 1080);
  TEST_CHECK(vsrc_generic_height_hint("trailer 4K") == 2160);
  TEST_CHECK(vsrc_generic_height_hint("id=12345&w_640") == 0);
  TEST_CHECK(vsrc_generic_height_hint("Night_512kb.mp4") == 0);
  TEST_CHECK(vsrc_generic_height_hint(NULL) == 0);

  char u[64];
  const char *js = "https:\\/\\/a.b\\/c?x=1\\u0026y=2\\x26z \\ud83d\\ude00\\\"";
  vsrc_generic_js_unescape(js, strlen(js), u, sizeof u);
  TEST_CHECK(!strcmp(u, "https://a.b/c?x=1&y=2&z \xF0\x9F\x98\x80\""));
  vsrc_generic_js_unescape("abcdef", 6, u, 4);
  TEST_CHECK(!strcmp(u, "abc"));
}

static void gt_jsonld(void) {
  static const char *kPage =
    "<!DOCTYPE html><html><head><title>Fallback</title>"
    "<script type=\"application/ld+json\">{\"@context\":\"https://schema.org\",\"@graph\":["
    "{\"@type\":\"WebPage\",\"name\":\"not this\"},"
    "{\"@type\":[\"VideoObject\"],\"name\":\"Clip &amp; Co\",\"duration\":\"PT1M30S\","
    "\"contentUrl\":\"/media/clip-720p.mp4\",\"thumbnailUrl\":[\"https://img.ex.com/t.jpg\"],"
    "\"embedUrl\":\"https://ex.com/embed/1\",\"width\":1280,\"height\":\"720\"}]}</script>"
    "</head><body></body></html>";
  FmVsrcConf c = conf(true, false, 1080);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmVsrcItem it;
  char emb[512];
  TEST_CHECK(parse(kPage, "https://ex.com/videos/1", &c, st, &it, emb, sizeof emb) == FM_OK);
  TEST_CHECK(st->nq == 1 && st->cur == 0 && !strcmp(st->video, "https://ex.com/media/clip-720p.mp4"));
  TEST_CHECK(st->q[0].height == 720 && !strcmp(st->q[0].label, "720p") && st->q[0].playable && st->q[0].muxed);
  TEST_CHECK(st->duration == 90 && it.duration == 90);
  TEST_CHECK(!strcmp(it.title, "Clip & Co") && !strcmp(it.thumb, "https://img.ex.com/t.jpg"));
  TEST_CHECK(!strcmp(it.page, "https://ex.com/videos/1") && it.id[0] == 'g' && it.views == -1);
  TEST_CHECK(!strcmp(emb, "https://ex.com/embed/1"));
  /* no OS decoders and no FFmpeg: listed, but marked */
  c = conf(false, false, 1080);
  TEST_CHECK(vsrc_generic_parse(kPage, strlen(kPage), "https://ex.com/videos/1", &c, NULL, st, NULL, 0) ==
             FM_ERR_UNSUPPORTED);
  TEST_CHECK(st->nq == 1 && st->cur == -1 && !st->q[0].playable && st->q[0].needs_ffmpeg && !st->video[0]);
  fm_free(st);
}

static void gt_meta(void) {
  static const char *kPage =
    "<html><head><base href=\"https://static.ex.net/assets/\">"
    "<meta property=\"og:title\" content=\"Sunset &#8211; &quot;live&quot;\">"
    "<meta name='twitter:title' content='lower priority'>"
    "<meta property=og:image content=img/t.jpg>"
    "<meta property=\"og:video\" content=\"https://cdn.ex.com/v/abc?sig=1&amp;exp=2\">"
    "<meta property=\"og:video:secure_url\" content=\"https://cdn.ex.com/v/abc?sig=1&amp;exp=2\">"
    "<meta property=\"og:video:type\" content=\"video/mp4\">"
    "<meta property=\"og:video:width\" content=\"854\"><meta property=\"og:video:height\" content=\"480\">"
    "<meta property=\"video:duration\" content=\"75\">"
    "<meta name=\"twitter:player:stream\" content=\"https://cdn.ex.com/tw/stream\">"
    "<meta name=\"twitter:player:stream:content_type\" content=\"video/webm; codecs=&quot;vp9&quot;\">"
    "</head><body><p>text</body></html>";
  FmVsrcConf c = conf(false, false, 720);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmVsrcItem it;
  TEST_CHECK(parse(kPage, "https://www.ex.com/watch/9", &c, st, &it, NULL, 0) == FM_OK);
  TEST_CHECK(!strcmp(it.title, "Sunset \xE2\x80\x93 \"live\""));
  TEST_CHECK(!strcmp(it.thumb, "https://static.ex.net/assets/img/t.jpg"));
  TEST_CHECK(it.duration == 75);
  TEST_CHECK(st->nq == 2);
  /* the og MP4 (480p, needs a decoder) and the twitter VP9 WebM (no size) */
  TEST_CHECK(!strcmp(st->q[0].url, "https://cdn.ex.com/v/abc?sig=1&exp=2") && st->q[0].height == 480 &&
             !st->q[0].playable && st->q[0].needs_ffmpeg);
  TEST_CHECK(!strcmp(st->q[1].url, "https://cdn.ex.com/tw/stream") && st->q[1].playable &&
             !strcmp(st->q[1].codec, "VP9") && !strcmp(st->q[1].label, "Video"));
  TEST_CHECK(st->cur == 1 && !strcmp(st->video, "https://cdn.ex.com/tw/stream"));
  /* with Media Foundation / MediaCodec the bigger MP4 wins */
  c = conf(true, false, 720);
  TEST_CHECK(parse(kPage, "https://www.ex.com/watch/9", &c, st, NULL, NULL, 0) == FM_OK && st->cur == 0 &&
             st->height == 480);
  fm_free(st);
}

static void gt_video_tags(void) {
  /* Wikimedia Commons style: VP8 original plus VP9 transcodes, sizes in data- attributes */
  static const char *kPage =
    "<div><VIDEO poster=\"//upload.ex.org/thumb/File.jpg\" controls preload=none>"
    "<source src=\"https://upload.ex.org/File.webm\" type=\"video/webm; codecs=&quot;vp8, vorbis&quot;\" "
    "data-width=\"1920\" data-height=\"1080\">"
    "<source src=\"/t/File.webm/File.webm.720p.vp9.webm\" type='video/webm; codecs=\"vp9, opus\"' "
    "data-transcodekey=\"720p.vp9.webm\" data-width=\"1280\" data-height=\"720\">"
    "<source src=/t/File.webm/File.webm.480p.vp9.webm type=video/webm data-transcodekey=480p.vp9.webm>"
    "<source src=\"/t/File.webm/File.webm.360p.mp4\" type=\"video/mp4\" label=\"360p\">"
    "<track src=\"subs.vtt\" kind=subtitles></VIDEO></div>"
    "<picture><source srcset=\"a.webp\" type=\"image/webp\"><img src=\"a.jpg\"></picture>"
    "<audio src=\"/a/song.mp3\"></audio>"
    "<link rel=\"preload\" as=\"video\" href=\"../media/pre.webm\">";
  FmVsrcConf c = conf(false, false, 720);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmVsrcItem it;
  TEST_CHECK(parse(kPage, "https://commons.ex.org/wiki/File:File.webm", &c, st, &it, NULL, 0) == FM_OK);
  TEST_CHECK(st->nq == 6);
  TEST_CHECK(!strcmp(st->q[0].label, "1080p") && !strcmp(st->q[0].codec, "VP8") && !st->q[0].playable &&
             st->q[0].needs_ffmpeg);
  TEST_CHECK(!strcmp(st->q[1].label, "720p") && st->q[1].playable &&
             !strcmp(st->q[1].url, "https://commons.ex.org/t/File.webm/File.webm.720p.vp9.webm"));
  TEST_CHECK(!strcmp(st->q[2].label, "480p") && st->q[2].playable);
  TEST_CHECK(!strcmp(st->q[3].label, "360p") && !st->q[3].playable);
  TEST_CHECK(!strcmp(st->q[4].label, "Video") && st->q[4].playable &&
             !strcmp(st->q[4].url, "https://commons.ex.org/media/pre.webm"));
  TEST_CHECK(st->q[5].audio_only && !st->q[5].muxed && !strcmp(st->q[5].label, "Audio only"));
  TEST_CHECK(st->cur == 1 && st->height == 720 && st->width == 1280);
  TEST_CHECK(!strcmp(it.thumb, "https://upload.ex.org/thumb/File.jpg"));
  TEST_CHECK(!strcmp(it.title, "File:File.webm"));
  /* a smaller limit; then nothing within it but the one of unknown size */
  c = conf(false, false, 480);
  TEST_CHECK(parse(kPage, "https://commons.ex.org/wiki/x", &c, st, NULL, NULL, 0) == FM_OK && st->cur == 2);
  c = conf(false, false, 144);
  TEST_CHECK(parse(kPage, "https://commons.ex.org/wiki/x", &c, st, NULL, NULL, 0) == FM_OK && st->cur == 4);
  /* FFmpeg plays everything: the biggest within the limit */
  c = conf(false, true, 1080);
  TEST_CHECK(parse(kPage, "https://commons.ex.org/wiki/x", &c, st, NULL, NULL, 0) == FM_OK && st->cur == 0 &&
             !st->q[0].needs_ffmpeg);
  fm_free(st);
}

static void gt_raw(void) {
  /* a JS player config with escaped slashes, and an HTML-escaped JSON attribute */
  static const char *kPage =
    "<html><body><div id=player data-config=\"{&quot;src&quot;:&quot;https://x.ex.com/a/clip_360.mp4"
    "?a=1&amp;b=2&quot;}\"></div><script>var cfg = {\"file\":\"https:\\/\\/cdn.ex.com\\/hls\\/master.m3u8"
    "?token=a\\u0026b=c\",\"poster\":\"https:\\/\\/cdn.ex.com\\/p.jpg\"};"
    "var other = 'https://cdn.ex.com/js/app.ts'; // not a video</script></body></html>";
  FmVsrcConf c = conf(true, false, 720);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  TEST_CHECK(parse(kPage, "https://ex.com/p", &c, st, NULL, NULL, 0) == FM_OK);
  TEST_CHECK(st->nq == 2);
  TEST_CHECK(!strcmp(st->q[0].url, "https://x.ex.com/a/clip_360.mp4?a=1&b=2") && st->q[0].height == 360);
  TEST_CHECK(!strcmp(st->q[1].url, "https://cdn.ex.com/hls/master.m3u8?token=a&b=c") &&
             !strcmp(st->q[1].label, "Auto") && !strcmp(st->q[1].codec, "HLS") && st->q[1].playable);
  TEST_CHECK(st->cur == 0);
  /* declared videos hide the URLs that only appear in the text (ads, previews) */
  static const char *kMixed =
    "<video src=\"main.webm\"></video><script>ad = \"https://ads.ex.com/promo.mp4\";</script>";
  TEST_CHECK(parse(kMixed, "https://ex.com/p", &c, st, NULL, NULL, 0) == FM_OK && st->nq == 1 &&
             !strcmp(st->video, "https://ex.com/main.webm"));
  fm_free(st);
}

static void gt_misc(void) {
  FmVsrcConf c = conf(true, true, 720);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  FmVsrcItem it;
  char emb[512];
  /* og:video naming a player page: nothing playable, the player page to read next */
  static const char *kEmbed =
    "<meta property=\"og:video\" content=\"/player/77\"><meta property=\"og:video:type\" content=\"text/html\">"
    "<meta property=\"og:title\" content=\"Embedded\">";
  TEST_CHECK(parse(kEmbed, "https://ex.com/w/77", &c, st, &it, emb, sizeof emb) == FM_ERR_NOT_FOUND);
  TEST_CHECK(st->nq == 0 && st->cur == -1 && !strcmp(emb, "https://ex.com/player/77") && !strcmp(it.title, "Embedded"));
  /* an iframe player */
  TEST_CHECK(parse("<iframe src=\"https://player.ex.org/embed/5?x=1\"></iframe>", "https://ex.com/", &c, st, NULL,
                   emb, sizeof emb) == FM_ERR_NOT_FOUND && !strcmp(emb, "https://player.ex.org/embed/5?x=1"));
  /* declared without a container: listed as unknown, never "playable" offline */
  TEST_CHECK(parse("<video src=\"/stream/42\"></video>", "https://ex.com/", &c, st, NULL, NULL, 0) ==
             FM_ERR_UNSUPPORTED && st->nq == 1 && !st->q[0].playable && !st->q[0].needs_ffmpeg);
  /* hidden in comments, scripts with markup, javascript: links */
  TEST_CHECK(parse("<!-- <video src=\"a.mp4\"> --><script>x='<video src=b.mp4>'</script>"
                   "<a href=\"javascript:play('c.mp4')\">x</a><video src=\"javascript:void(0)\">",
                   "https://ex.com/", &c, st, &it, NULL, 0) == FM_ERR_NOT_FOUND);
  TEST_CHECK(!strcmp(it.title, "ex.com"));
  /* garbage and cut pages: no crash, no candidates */
  static const char *kBad[] = {
    "", "<", "<<<>>>", "<video src=\"abc", "<meta property=\"og:video\" content=", "<source src='x.mp4'",
    "<script type=application/ld+json>{\"@type\":\"VideoObject\",\"contentUrl\":", "<!--", "<title>t",
    "<video src=x.mp4><source src=\"y.webm\" type=\"video/webm",
    "\xFF\xFE\x00<html>\x01\x02", "https:\\/\\/", "http", "<a b=c d='e' f=\"g\" ==== / >",
  };
  for (int i = 0; i < FM_COUNT(kBad); i++) {
    FmErr e = parse(kBad[i], "https://ex.com/a", &c, st, &it, emb, sizeof emb);
    TEST_CHECK(e == FM_OK || e == FM_ERR_NOT_FOUND || e == FM_ERR_UNSUPPORTED);
    TEST_CHECK(st->nq <= VSRC_QMAX && strlen(it.title) < sizeof it.title);
  }
  /* .ogg inside <video> is Ogg video; an HLS playlist typed audio/mpegurl is not "Audio only" */
  TEST_CHECK(parse("<video><source src=\"m.ogg\" type=\"video/ogg\"></video><video src=\"n.ogg\"></video>"
                   "<meta property=\"og:video\" content=\"https://ex.com/live\">"
                   "<meta property=\"og:video:type\" content=\"audio/mpegurl\">", "https://ex.com/", &c, st, NULL,
                   NULL, 0) == FM_OK && st->nq == 3);
  for (int i = 0; i < st->nq; i++)
    TEST_CHECK(!st->q[i].audio_only && (!strcmp(st->q[i].codec, "Ogg") || !strcmp(st->q[i].codec, "HLS")));
  /* a page URL that is not one: absolute links still work, relative ones are dropped */
  TEST_CHECK(parse("<iframe src=\"https://p.ex.org/embed/1\"></iframe><video src=\"rel.mp4\">", "not a url", &c, st,
                   &it, emb, sizeof emb) == FM_ERR_NOT_FOUND && !strcmp(emb, "https://p.ex.org/embed/1"));
  TEST_CHECK(parse("<video src=\"https://ex.com/a.webm\">", "", &c, st, &it, NULL, 0) == FM_OK);
  /* "<video src=x.mp4>" (cut after the tag) still counts */
  TEST_CHECK(parse("<video src=x.mp4>", "https://ex.com/a/", &c, st, NULL, NULL, 0) == FM_OK &&
             !strcmp(st->video, "https://ex.com/a/x.mp4"));
  /* a big page of noise: bounded, and the late video is still found */
  size_t n = 3u << 20;
  char *big = (char *)fm_alloc(n + 64);
  for (size_t i = 0; i < n; i++) big[i] = "<a href=\"http://x\"> \"'<!-- &amp;\\"[i % 32];
  strcpy(big + n, "--><video src=\"late.webm\"></video>");
  TEST_CHECK(vsrc_generic_parse(big, strlen(big), "https://ex.com/", &c, NULL, st, NULL, 0) == FM_OK &&
             !strcmp(st->video, "https://ex.com/late.webm"));
  fm_free(big);
  /* many candidates: the list stays within VSRC_QMAX */
  char *many = (char *)fm_alloc(64 * 80);
  many[0] = 0;
  for (int i = 0; i < 60; i++) {
    char t[80];
    fm_snprintf(t, sizeof t, "<source src=\"v%d.webm\" type=video/webm>", i);
    fm_strlcat(many, t, 64 * 80);
  }
  TEST_CHECK(parse(many, "https://ex.com/", &c, st, NULL, NULL, 0) == FM_OK && st->nq == VSRC_QMAX);
  fm_free(many);
  fm_free(st);
}

/* MMCFM_GENERIC_TEST=<page url>: resolves a real page and prints the result */
static void gt_live(void) {
  const char *url = getenv("MMCFM_GENERIC_TEST");
  if (!url || !*url) return;
  FmVsrcConf c = conf(true, false, 720);
  const char *ff = getenv("MMCFM_GENERIC_FFMPEG");
  if (ff && *ff == '1') c.have_ffmpeg_libs = true;
  volatile int cancel = 0;
  char err[256];
  FmVsrcItem it;
  u64 t0 = plat_now_ms();
  FmErr e = vsrc_generic_probe(&c, url, &it, err, sizeof err, &cancel);
  printf("  probe %s: %s (%d ms) %s\n", url, fm_err_str(e), (int)(plat_now_ms() - t0), err);
  printf("    title \"%s\"\n    thumb %s\n    duration %.1f s, id %s\n", it.title, it.thumb, it.duration, it.id);
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  t0 = plat_now_ms();
  e = vsrc_generic_resolve(&c, url, st, err, sizeof err, &cancel);
  printf("  resolve: %s (%d ms) %s\n", fm_err_str(e), (int)(plat_now_ms() - t0), err);
  for (int i = 0; i < st->nq; i++) {
    const FmVsrcQuality *q = &st->q[i];
    printf("   %c %-12s %-6s %s%s%s %s\n", i == st->cur ? '*' : ' ', q->label, q->codec,
           q->playable ? "plays" : "-", q->needs_ffmpeg ? " needs-ffmpeg" : "", q->audio_only ? " audio" : "", q->url);
  }
  if (st->cur >= 0) printf("    video %s (%dx%d, %.1f s)\n", st->video, st->width, st->height, st->duration);
  fm_free(st);
}

int test_generic(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  gt_urls();
  gt_jsonld();
  gt_meta();
  gt_video_tags();
  gt_raw();
  gt_misc();
  gt_live();
  return g_test_fail - before;
}
