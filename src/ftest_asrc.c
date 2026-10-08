/* ftest_asrc.c -- online audio sources: the XML reader, parsers, codec and
** file-name helpers, streams, and (opt-in) real searches, streams and a
** download.
**
** Offline by default: every adapter parses canned replies (shaped like the
** real ones probed on 2026-10-08) and garbage; the XML reader gets an odd
** but valid feed, every truncation of it, random bytes and hostile input.
** MMCFM_NET_TEST=1 adds a real search and browse on every keyless source,
** podcast episodes, archive.org album tracks, the first 64 KB of a radio
** station and an Audius track checked to be MP3, 2 s decoded through
** aud_open_ex, a download with its credit file, and the keyed sources'
** answers to a bogus key. MMCFM_JAMENDO_KEY / MMCFM_FREESOUND_KEY run those
** sources for real.
*/
#include "ftest.h"
#include "fasrc.h"
#include "fasrc_int.h"
#include "fxml.h"
#include "fplat.h"
#include "fdec_aud.h"

/* ---- canned replies ---------------------------------------------------------------- */

static const char *kRadio =
  "[{\"changeuuid\":\"c7162475\",\"stationuuid\":\"d28420a4-eccf-47a2-ace1-088c7e7cb7e0\",\"name\":\"  101 SMOOTH  JAZZ \","
  "\"url\":\"http://www.101smoothjazz.com/101-smoothjazz.m3u\",\"url_resolved\":\"http://jking.cdnstream1.com/b22139_128mp3\","
  "\"homepage\":\"http://101smoothjazz.com/\",\"favicon\":\"http://101smoothjazz.com/favicon.ico\","
  "\"tags\":\"easy listening,jazz,smooth jazz\",\"country\":\"The United States Of America\",\"countrycode\":\"US\","
  "\"votes\":90243,\"codec\":\"MP3\",\"bitrate\":192,\"hls\":0,\"lastcheckok\":1,\"clickcount\":473,\"geo_lat\":null},"
  "{\"stationuuid\":\"604bb4af-ccf6-47bd-819e-7d1accd15e05\",\"name\":\"Barangay LS 97.1 Manila\","
  "\"url_resolved\":\"http://28093.live.streamtheworld.com:3690/MORFM_S01AAC_SC\",\"codec\":\"AAC+\",\"bitrate\":64,"
  "\"hls\":0,\"country\":\"The Philippines\",\"tags\":\"\",\"favicon\":\"\",\"clickcount\":10},"
  "{\"stationuuid\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"name\":\"HLS one\",\"url_resolved\":\"https://x/live.m3u8\","
  "\"codec\":\"MP3\",\"hls\":1},"
  "{\"stationuuid\":\"bad uuid\",\"name\":\"x\",\"url_resolved\":\"http://x/y\",\"codec\":\"MP3\"},"
  "{\"stationuuid\":\"aaaaaaaa-0000-0000-0000-000000000002\",\"name\":\"Bad url\",\"url_resolved\":\"javascript:alert(1)\","
  "\"codec\":\"MP3\"},"
  "{\"stationuuid\":\"aaaaaaaa-0000-0000-0000-000000000003\",\"name\":\"Caf\\u00e9 &amp; Bar\",\"url\":\"https://s/stream\","
  "\"url_resolved\":\"\",\"codec\":\"UNKNOWN\",\"bitrate\":0,\"favicon\":\"ftp://x\"}]";

static const char *kRadioClick =
  "{\"ok\":true,\"message\":\"retrieved station url\",\"stationuuid\":\"2940057c-ccb6-4d81-a95b-06a74eccd5d4\","
  "\"name\":\"Sports Radio Brila FM\",\"url\":\"https://atunwadigital.streamguys1.com/brilafm\"}";

static const char *kAudius =
  "{\"data\":["
  "{\"id\":\"95wro\",\"title\":\"Stars In The Sky\",\"is_streamable\":false,\"user\":{\"name\":\"Lofi Beats\"},\"duration\":71},"
  "{\"track_id\":14815,\"id\":\"ng9rl\",\"title\":\"lofi type beat\",\"genre\":\"Lo-Fi\",\"duration\":334,"
  "\"is_streamable\":true,\"is_stream_gated\":false,\"play_count\":68873,\"license\":\"Attribution CC BY\","
  "\"permalink\":\"/bsdu/lofi-type-beat-14815\",\"release_date\":\"2019-07-01T00:00:00Z\","
  "\"artwork\":{\"150x150\":\"https://n/150x150.jpg\",\"480x480\":\"https://n/480x480.jpg\",\"mirrors\":[\"https://m\"]},"
  "\"stream\":{\"url\":\"https://node/tracks/cidstream/Qm?signature=x\"},\"user\":{\"name\":\"bsdu\",\"handle\":\"bsdu\"}},"
  "{\"id\":\"gat3d\",\"title\":\"Gated\",\"is_stream_gated\":true,\"user\":{\"name\":\"x\"}},"
  "{\"id\":\"bad/id\",\"title\":\"x\"},"
  "{\"id\":\"RKxOQ\",\"playlist_name\":\"Lofi Space inspired\",\"is_album\":false,\"total_play_count\":22046,"
  "\"artwork\":{\"480x480\":\"https://n/p480.jpg\"},\"user\":{\"name\":\"Lofi Army\"},"
  "\"permalink\":\"/LofiArmy/playlist/lofi-space-inspired-97517\"}"
  "]}";

static const char *kArchiveSearch =
  "{\"responseHeader\":{\"status\":0},\"response\":{\"numFound\":142829,\"start\":0,\"docs\":["
  "{\"creator\":\"Old Time Radio Researchers Group\",\"downloads\":2855786,\"identifier\":\"OTRR_Dragnet_Singles\","
  "\"publicdate\":\"2007-10-12T21:00:51Z\",\"title\":\"Dragnet - Single Episodes\"},"
  "{\"downloads\":1267804,\"identifier\":\"mainseapro_gmail_Jazz\",\"date\":\"1959-01-01T00:00:00Z\",\"title\":[\"jazz\"]},"
  "{\"identifier\":\"../x\",\"title\":\"bad\"}]}}";

static const char *kArchiveMeta =
  "{\"metadata\":{\"identifier\":\"gd77\",\"title\":\"Grateful Dead Live 1977\",\"creator\":\"Grateful Dead\","
  "\"date\":\"1977-05-08\",\"licenseurl\":\"http://creativecommons.org/licenses/by-nc-sa/3.0/\"},"
  "\"files\":["
  "{\"name\":\"gd77d01t02.flac\",\"format\":\"Flac\",\"title\":\"Minglewood Blues\",\"track\":\"02\",\"length\":\"323.81\","
  "\"source\":\"original\",\"creator\":\"Grateful Dead\",\"album\":\"1977-05-08 - Barton Hall\"},"
  "{\"name\":\"gd77d01t02.mp3\",\"format\":\"VBR MP3\",\"title\":\"Minglewood Blues\",\"track\":\"02\",\"length\":\"05:23\","
  "\"source\":\"derivative\",\"original\":\"gd77d01t02.flac\"},"
  "{\"name\":\"gd77d01t02.ogg\",\"format\":\"Ogg Vorbis\",\"length\":\"323.81\",\"source\":\"derivative\","
  "\"original\":\"gd77d01t02.flac\"},"
  "{\"name\":\"gd77d01t01.flac\",\"format\":\"Flac\",\"title\":\"Turning\",\"track\":\"1/12\",\"length\":\"39.8\","
  "\"source\":\"original\"},"
  "{\"name\":\"gd77d01t01_64kb.mp3\",\"format\":\"64Kbps MP3\",\"length\":\"00:40\",\"source\":\"derivative\","
  "\"original\":\"gd77d01t01.flac\"},"
  "{\"name\":\"gd77d01t01.mp3\",\"format\":\"VBR MP3\",\"length\":\"00:39\",\"source\":\"derivative\","
  "\"original\":\"gd77d01t01.flac\"},"
  "{\"name\":\"gd77d01t10.flac\",\"format\":\"Flac\",\"title\":\"Ten\",\"length\":\"60\",\"source\":\"original\"},"
  "{\"name\":\"gd77d01t3 extra.flac\",\"format\":\"Flac\",\"length\":\"61\",\"source\":\"original\"},"
  "{\"name\":\"secret.mp3\",\"format\":\"VBR MP3\",\"private\":\"true\",\"source\":\"original\"},"
  "{\"name\":\"../evil.mp3\",\"format\":\"VBR MP3\",\"source\":\"original\"},"
  "{\"name\":\"cover.jpg\",\"format\":\"JPEG\",\"source\":\"original\"},"
  "{\"name\":\"only.ogg\",\"format\":\"Ogg Vorbis\",\"source\":\"original\",\"length\":\"5\"}"
  "]}";

static const char *kItunes =
  "\n\n\n{\n \"resultCount\":3,\n \"results\": [\n"
  "{\"wrapperType\":\"track\", \"kind\":\"podcast\", \"collectionId\":1291579828, \"artistName\":\"Prof. Greg Jackson\","
  " \"collectionName\":\"History That Doesn't Suck\","
  " \"collectionViewUrl\":\"https://podcasts.apple.com/us/podcast/history-that-doesnt-suck/id1291579828?uo=4\","
  " \"feedUrl\":\"https://rss.amperwave.net/v2/feed/audacynetwork/e69da14a26973f1dc761432876205254\","
  " \"artworkUrl100\":\"https://is1-ssl.mzstatic.com/a/100x100bb.jpg\","
  " \"artworkUrl600\":\"https://is1-ssl.mzstatic.com/a/600x600bb.jpg\", \"releaseDate\":\"2026-10-05T03:00:00Z\","
  " \"trackCount\":235, \"primaryGenreName\":\"History\"},\n"
  "{\"kind\":\"podcast\", \"collectionId\":1537788786, \"artistName\":\"Goalhanger\", \"collectionName\":\"The Rest Is History\","
  " \"feedUrl\":\"https://feeds.megaphone.fm/GLT4787413333\", \"artworkUrl100\":\"https://x/100x100bb.jpg\"},\n"
  "{\"kind\":\"podcast\", \"collectionId\":5, \"collectionName\":\"No feed\"}]\n}\n\n\n";

static const char *kItunesTop =
  "{\"feed\":{\"author\":{\"name\":{\"label\":\"iTunes Store\"}},\"entry\":["
  "{\"im:name\":{\"label\":\"Barangay Love Stories\"},\"im:image\":["
  "{\"label\":\"https://is1-ssl.mzstatic.com/image/thumb/P/v4/27/mza_6.jpg/55x55bb.png\",\"attributes\":{\"height\":\"55\"}},"
  "{\"label\":\"https://is1-ssl.mzstatic.com/image/thumb/P/v4/27/mza_6.jpg/170x170bb.png\",\"attributes\":{\"height\":\"170\"}}],"
  "\"summary\":{\"label\":\"Weekly stories\"},\"title\":{\"label\":\"Barangay Love Stories - Barangay LS 97.1\"},"
  "\"link\":{\"attributes\":{\"rel\":\"alternate\",\"type\":\"text/html\","
  "\"href\":\"https://podcasts.apple.com/ph/podcast/barangay-love-stories/id1101180313?uo=2\"}},"
  "\"id\":{\"label\":\"https://podcasts.apple.com/ph/podcast/id1101180313\",\"attributes\":{\"im:id\":\"1101180313\"}},"
  "\"im:artist\":{\"label\":\"Barangay LS 97.1 Manila | GMA Network Inc.\"},"
  "\"category\":{\"attributes\":{\"im:id\":\"1324\",\"term\":\"Society & Culture\",\"label\":\"Society &amp; Culture\"}},"
  "\"im:releaseDate\":{\"label\":\"2026-10-08T03:35:00-07:00\"}},"
  "{\"im:name\":{\"label\":\"bad id\"},\"id\":{\"attributes\":{\"im:id\":\"x y\"}}}]}}";

static const char *kItunesTopOne =
  "{\"feed\":{\"entry\":{\"im:name\":{\"label\":\"The Daily\"},\"id\":{\"attributes\":{\"im:id\":\"1200361736\"}}}}}";

static const char *kItunesLookup =
  "{\"resultCount\":1,\"results\":[{\"kind\":\"podcast\",\"collectionId\":1537788786,"
  "\"feedUrl\":\"https://feeds.megaphone.fm/GLT4787413333\"}]}";

/* odd but valid: CDATA, entities, a non-"itunes" prefix for the iTunes
** namespace, both duration forms, missing fields, comments, a DOCTYPE */
static const char *kFeed =
  "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<?xml-stylesheet type=\"text/xsl\" href=\"x.xsl\"?>\n"
  "<!DOCTYPE rss [ <!ENTITY boom \"BOOM\"> ]>\n"
  "<!-- a comment with <item> inside -->\n"
  "<rss version=\"2.0\" xmlns:it=\"http://www.itunes.com/dtds/podcast-1.0.dtd\" xmlns:media='http://search.yahoo.com/mrss/'>\n"
  "<channel>\n"
  "  <title>Odd &amp; Valid</title>\n"
  "  <it:author>Ann &lt;A&gt; Author</it:author>\n"
  "  <image><url>https://example.org/show.jpg</url><title>x</title></image>\n"
  "  <it:image href=\"https://example.org/show-it.jpg\"/>\n"
  "  <it:category text=\"History\"><it:category text=\"Nested\"/></it:category>\n"
  "  <item>\n"
  "    <title><![CDATA[Ep 1: <b>Bold</b> & \"quoted\"]]></title>\n"
  "    <enclosure url=\"https://example.org/ep1.mp3?a=1&amp;b=2\" type=\"audio/mpeg\" length=\"123\"/>\n"
  "    <it:duration>1:02:03</it:duration>\n"
  "    <pubDate>Mon, 05 Oct 2026 10:00:00 +0000</pubDate>\n"
  "    <guid isPermaLink=\"false\">guid-1</guid>\n"
  "    <description><![CDATA[<p>Has a ]] inside &boom;</p>]]></description>\n"
  "    <media:group><media:content url=\"https://example.org/wrong.mp3\" type=\"audio/mpeg\"/></media:group>\n"
  "  </item>\n"
  "  <item>\n"
  "    <title>Ep 2 &#8212; dash &#x263A;</title>\n"
  "    <enclosure url='https://example.org/ep2.m4a' type='audio/x-m4a'></enclosure>\n"
  "    <it:duration>3723</it:duration>\n"
  "    <pubDate>Wed, 07 Oct 2026 23:05:00 -0000</pubDate>\n"
  "    <it:image href=\"https://example.org/ep2.jpg\" />\n"
  "    <link>https://example.org/ep2</link>\n"
  "  </item>\n"
  "  <item>\n"
  "    <title>Video episode</title>\n"
  "    <enclosure url=\"https://example.org/v.mp4\" type=\"video/mp4\"/>\n"
  "  </item>\n"
  "  <item><title>No enclosure</title></item>\n"
  "  <item>\n"
  "    <title>Ep 0 oldest</title>\n"
  "    <enclosure url=\"https://example.org/ep0.mp3\" length=\"5\"/>\n"
  "    <it:duration>62:03</it:duration>\n"
  "    <pubDate>Thu, 1 Jan 2026 08:00:00 GMT</pubDate>\n"
  "    <it:author>Guest</it:author>\n"
  "  </item>\n"
  "  <item><title>No date</title><enclosure url=\"https://example.org/nd.mp3\" type=\"audio/mpeg\"/>"
  "<it:duration> 95 </it:duration></item>\n"
  "  <item><title>A PDF</title><enclosure url=\"https://example.org/x.pdf\" type=\"application/pdf\"/></item>\n"
  "</channel>\n"
  "</rss>\n";

static const char *kJamendo =
  "{\"headers\":{\"status\":\"success\",\"code\":0,\"error_message\":\"\",\"warnings\":\"\",\"results_count\":2},"
  "\"results\":[{\"id\":\"1532771\",\"name\":\"Let&#39;s go\",\"duration\":192,\"artist_id\":\"7\",\"artist_name\":\"Artist A\","
  "\"album_name\":\"Album\",\"releasedate\":\"2018-03-02\",\"album_image\":\"https://usercontent.jamendo.com/a.jpg\","
  "\"image\":\"https://usercontent.jamendo.com?type=album&id=1&width=300\","
  "\"audio\":\"https://prod-1.storage.jamendo.com/?trackid=1532771&format=mp32\","
  "\"audiodownload\":\"https://prod-1.storage.jamendo.com/download/track/1532771/mp32/\","
  "\"shareurl\":\"https://www.jamendo.com/track/1532771\","
  "\"license_ccurl\":\"http://creativecommons.org/licenses/by-nc-nd/3.0/\"},"
  "{\"id\":\"2\",\"name\":\"no audio\",\"audio\":\"\"}]}";

static const char *kJamendoBadKey =
  "{\"headers\":{\"status\":\"failed\",\"code\":5,\"error_message\":\"Jamendo Api Invalid Client Id Error: Your "
  "credential is not authorized.\",\"warnings\":\"\",\"results_count\":0},\"results\":[]}";

static const char *kFreesound =
  "{\"count\":3,\"next\":\"https://freesound.org/apiv2/search/text/?&query=rain&page=2\",\"previous\":null,\"results\":["
  "{\"id\":398275,\"name\":\"Rain on window.wav\",\"tags\":[\"rain\",\"window\",\"storm\",\"weather\",\"field-recording\"],"
  "\"license\":\"https://creativecommons.org/publicdomain/zero/1.0/\",\"username\":\"inchadney\",\"duration\":64.5,"
  "\"previews\":{\"preview-hq-mp3\":\"https://cdn.freesound.org/previews/398/398275_123-hq.mp3\","
  "\"preview-lq-mp3\":\"https://cdn.freesound.org/previews/398/398275_123-lq.mp3\"},"
  "\"images\":{\"waveform_m\":\"https://cdn.freesound.org/displays/398/398275_123_wave_M.png\"},"
  "\"url\":\"https://freesound.org/people/inchadney/sounds/398275/\",\"created\":\"2017-06-25T10:00:00\"},"
  "{\"id\":0,\"name\":\"x\"},{\"id\":5,\"name\":\"no previews\",\"previews\":{}}]}";

/* ---- offline: registry and helpers ------------------------------------------------- */

static void at_registry(void) {
  static const char *const kKeys[] = { "radio", "audius", "archive", "podcasts", "jamendo", "freesound" };
  TEST_CHECK(asrc_count() == 6);
  for (int i = 0; i < asrc_count() && i < 6; i++) {
    const FmAsrc *s = asrc_at(i);
    TEST_CHECK(s && !strcmp(s->key, kKeys[i]));
    TEST_CHECK(s && s->name && s->about && s->search && (s->flags & ASRC_SEARCH));
    TEST_CHECK(s && asrc_find(s->key) == s);
    TEST_CHECK(s && strlen(s->key) < sizeof ((FmAsrcItem *)0)->source);
    TEST_CHECK(s && ((s->flags & ASRC_NEEDKEY) != 0) == (i >= 4));
    TEST_CHECK(s && ((s->flags & ASRC_LIVE) != 0) == (i == 0));
    TEST_CHECK(s && ((s->flags & ASRC_BROWSE) != 0) == (s->categories != NULL && s->browse != NULL));
    if (s && s->categories) {
      FmAsrcCat cats[ASRC_MAX_CATS];
      FmAsrcConf c;
      memset(&c, 0, sizeof c);
      int n = s->categories(&c, cats, ASRC_MAX_CATS);
      TEST_CHECK(n > 0 && n <= ASRC_MAX_CATS);
      for (int k = 0; k < n; k++) TEST_CHECK(cats[k].id[0] && cats[k].name[0]);
      TEST_CHECK(s->categories(&c, cats, 1) == 1 && s->categories(&c, cats, 0) == 0);
    }
  }
  TEST_CHECK(asrc_find("archive")->children && asrc_find("podcasts")->children && asrc_find("audius")->children);
  TEST_CHECK(!asrc_find("radio")->children && asrc_find("radio")->resolve);
  TEST_CHECK(asrc_at(-1) == NULL && asrc_at(6) == NULL && asrc_find("nope") == NULL && asrc_find(NULL) == NULL);
  FmAsrcConf c;
  asrc_conf_snapshot(&c);
  TEST_CHECK(strstr(c.cache_dir, "online-audio") != NULL && c.download_dir[0]);
  TEST_CHECK(c.country[0] == 0 || strlen(c.country) == 2);
  printf("  conf: country \"%s\", ffmpeg %s\n", c.country, c.have_ffmpeg ? "yes" : "no");
}

static void at_page(void) {
  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  asrc_page_reserve(&p, ASRC_MAX_CHILDREN);
  FmAsrcItem *first = asrc_page_add(&p);
  for (int i = 1; i < ASRC_MAX_CHILDREN; i++) asrc_item_new(&p, "radio", AITEM_STATION);
  TEST_CHECK(p.count == ASRC_MAX_CHILDREN && p.cap == ASRC_MAX_CHILDREN && first == &p.items[0]);
  TEST_CHECK(p.items[1].plays == -1 && p.items[1].kind == AITEM_STATION && !strcmp(p.items[1].source, "radio"));
  asrc_page_reserve(&p, 5);                        /* a used page keeps its block */
  TEST_CHECK(p.cap == ASRC_MAX_CHILDREN);
  asrc_page_free(&p);
  TEST_CHECK(p.items == NULL && p.count == 0 && p.cap == 0);
  FmAsrcItem *x = asrc_page_add(&p);               /* no reserve: the default block */
  TEST_CHECK(x && p.cap == ASRC_MAX_ITEMS);
  asrc_page_free(&p);
  asrc_page_free(NULL);
}

static bool valid_utf8(const char *s) {
  for (size_t i = 0; s[i];) {
    u32 cp;
    int n = utf8_decode(s + i, &cp);
    if (n <= 0 || cp == 0xFFFD) return false;
    i += (size_t)n;
  }
  return true;
}

static void at_helpers(void) {
  char b[256];
  asrc_codec_from_mime("audio/mpeg", b, sizeof b);              TEST_CHECK(!strcmp(b, "MP3"));
  asrc_codec_from_mime("audio/x-m4a; charset=x", b, sizeof b);  TEST_CHECK(!strcmp(b, "AAC"));
  asrc_codec_from_mime("audio/mpegurl", b, sizeof b);           TEST_CHECK(!strcmp(b, ""));
  asrc_codec_from_mime(NULL, b, sizeof b);                      TEST_CHECK(!strcmp(b, ""));
  asrc_codec_from_ext("https://x/a/song.FLAC?x=1", b, sizeof b); TEST_CHECK(!strcmp(b, "FLAC"));
  asrc_codec_from_ext("ep.m4a", b, sizeof b);                   TEST_CHECK(!strcmp(b, "AAC"));
  asrc_codec_from_ext("https://x/stream", b, sizeof b);         TEST_CHECK(!strcmp(b, ""));
  asrc_codec_norm("AAC+,H.264", b, sizeof b);                   TEST_CHECK(!strcmp(b, "AAC"));
  asrc_codec_norm("UNKNOWN", b, sizeof b);                      TEST_CHECK(!strcmp(b, ""));
  asrc_codec_norm("ogg", b, sizeof b);                          TEST_CHECK(!strcmp(b, "OGG"));
  asrc_codec_norm("weird codec!", b, sizeof b);                 TEST_CHECK(!strcmp(b, "WEIRDCOD"));
  TEST_CHECK(asrc_codec_builtin("MP3") && asrc_codec_builtin("FLAC") && asrc_codec_builtin(""));
  TEST_CHECK(!asrc_codec_builtin("AAC") && !asrc_codec_builtin("OGG") && !asrc_codec_builtin("OPUS"));
  TEST_CHECK(!strcmp(asrc_mime_ext("audio/mpeg"), ".mp3") && !strcmp(asrc_mime_ext("audio/flac"), ".flac") &&
             !strcmp(asrc_mime_ext("text/html; charset=utf-8"), "") && !strcmp(asrc_mime_ext(NULL), ""));

  FmAsrcConf c;
  FmAsrcItem it;
  memset(&c, 0, sizeof c);
  memset(&it, 0, sizeof it);
  it.kind = AITEM_STATION;
  TEST_CHECK(asrc_codec_blocked(&c, &it, "AAC", b, sizeof b) &&
             !strcmp(b, "This station uses AAC \xE2\x80\x94 install FFmpeg to play it"));
  it.kind = AITEM_TRACK;
  fm_strlcpy(it.source, "podcasts", sizeof it.source);
  TEST_CHECK(asrc_codec_blocked(&c, &it, "AAC", b, sizeof b) && strstr(b, "This episode uses AAC"));
  TEST_CHECK(!asrc_codec_blocked(&c, &it, "MP3", b, sizeof b));
  c.have_ffmpeg = true;
  TEST_CHECK(!asrc_codec_blocked(&c, &it, "AAC", b, sizeof b));

  asrc_rfc822_date("Wed, 07 Oct 2026 23:05:00 -0000", b, sizeof b); TEST_CHECK(!strcmp(b, "2026-10-07"));
  asrc_rfc822_date("Thu, 1 Jan 2026 08:00:00 GMT", b, sizeof b);   TEST_CHECK(!strcmp(b, "2026-01-01"));
  asrc_rfc822_date("  5 Sept 2019 1:00 PST", b, sizeof b);         TEST_CHECK(!strcmp(b, "2019-09-05"));
  asrc_rfc822_date("Tuesday, 03 Mar 99 10:00", b, sizeof b);       TEST_CHECK(!strcmp(b, "1999-03-03"));
  asrc_rfc822_date("2026-10-05T03:00:00Z", b, sizeof b);           TEST_CHECK(!strcmp(b, "2026-10-05"));
  asrc_rfc822_date("yesterday", b, sizeof b);                      TEST_CHECK(!strcmp(b, ""));
  asrc_rfc822_date("Mon, 45 Foo 2026", b, sizeof b);               TEST_CHECK(!strcmp(b, ""));
  asrc_rfc822_date(NULL, b, sizeof b);                             TEST_CHECK(!strcmp(b, ""));
  TEST_CHECK(asrc_duration("1:02:03") == 3723 && asrc_duration("3723") == 3723 && asrc_duration("62:03") == 3723);
  TEST_CHECK(asrc_duration(" 95 \n") == 95 && asrc_duration("12.5") == 12.5);
  TEST_CHECK(asrc_duration("") == 0 && asrc_duration("abc") == 0 && asrc_duration(NULL) == 0 &&
             asrc_duration("1:2:3:4") == 0);

  asrc_cc_license("http://creativecommons.org/licenses/by-nc-sa/3.0/", b, sizeof b); TEST_CHECK(!strcmp(b, "CC BY-NC-SA 3.0"));
  asrc_cc_license("https://creativecommons.org/publicdomain/zero/1.0/", b, sizeof b); TEST_CHECK(!strcmp(b, "CC0 1.0"));
  asrc_cc_license("http://creativecommons.org/publicdomain/mark/1.0/", b, sizeof b); TEST_CHECK(!strcmp(b, "Public domain mark"));
  asrc_cc_license("https://creativecommons.org/licenses/by/4.0", b, sizeof b);      TEST_CHECK(!strcmp(b, "CC BY 4.0"));
  asrc_cc_license("Attribution", b, sizeof b);                                      TEST_CHECK(!strcmp(b, "Attribution"));
  asrc_cc_license("https://example.org/terms", b, sizeof b);                        TEST_CHECK(!strcmp(b, ""));

  asrc_text("  Jazz &amp;\n\t Blues &#233;  ", b, sizeof b);
  TEST_CHECK(!strcmp(b, "Jazz & Blues \xC3\xA9"));
  char small[6];
  asrc_text("ab\xC3\xA9\xC3\xA9", small, sizeof small);          /* 5 bytes would split the second e-acute */
  TEST_CHECK(!strcmp(small, "ab\xC3\xA9"));

  /* file names: "<artist> - <title>", UTF-8 safe, no reserved names */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.artist, "AC/DC", sizeof it.artist);
  fm_strlcpy(it.title, "Back: \"In\" Black?", sizeof it.title);
  asrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "AC DC - Back In Black"));
  memset(&it, 0, sizeof it);
  asrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "audio"));
  fm_strlcpy(it.title, "nul", sizeof it.title);
  asrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "_nul"));
  fm_strlcpy(it.title, ".hidden.", sizeof it.title);
  asrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "hidden"));
  char longt[256];
  longt[0] = 0;
  for (int i = 0; i < 80; i++) fm_strlcat(longt, "\xE6\x97\xA5", sizeof longt);  /* 240 bytes of CJK */
  fm_strlcpy(it.title, longt, sizeof it.title);
  fm_strlcpy(it.artist, "\xF0\x9F\x8E\xB5 Band With A Rather Long Name That Goes On And On Forever", sizeof it.artist);
  asrc_file_base(&it, b, 150);
  TEST_CHECK(strlen(b) < 150 && valid_utf8(b) && !strncmp(b, "\xF0\x9F\x8E\xB5 Band", 8) && strstr(b, " - \xE6\x97\xA5"));

  /* sniffing: first bytes win over names */
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"ID3\x04\0\0\0\0\0b", 10), ".mp3"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"\xFF\xFB\x90\x64", 4), ".mp3"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"\xFF\xF1\x50\x80", 4), ".aac"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"fLaC\0\0\0\x22", 8), ".flac"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"OggS\0\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x13OpusHead", 36),
                     ".opus"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"OggS\0\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x1E\x01vorbis", 35),
                     ".ogg"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"RIFF\0\0\0\0WAVEfmt ", 16), ".wav"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"\0\0\0\x20" "ftypM4A ", 12), ".m4a"));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"<!DOCTYPE html><html>", 21), ""));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"{\"error\":1}", 11), ""));
  TEST_CHECK(!strcmp(asrc_sniff_ext((const u8 *)"\xFF\xFF\xFF\xFF", 4), ""));
  TEST_CHECK(!strcmp(asrc_sniff_ext(NULL, 0), "") && !strcmp(asrc_sniff_ext((const u8 *)"ID", 2), ""));

  char text[2048];
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "jamendo", sizeof it.source);
  fm_strlcpy(it.title, "Song", sizeof it.title);
  fm_strlcpy(it.license, "CC BY 3.0", sizeof it.license);
  fm_strlcpy(it.page, "https://p/1", sizeof it.page);
  asrc_credit(&it, "Jamendo", "https://f/1.mp3", text, sizeof text);
  TEST_CHECK(strstr(text, "Title: Song\n") && strstr(text, "Artist: (unknown)\n") && strstr(text, "Licence: CC BY 3.0\n") &&
             strstr(text, "Source: Jamendo\n") && strstr(text, "Page: https://p/1\n") && strstr(text, "File: https://f/1.mp3\n"));
  TEST_CHECK(asrc_wants_credit(&it));
  fm_strlcpy(it.source, "audius", sizeof it.source);
  fm_strlcpy(it.license, "All rights reserved", sizeof it.license);
  TEST_CHECK(!asrc_wants_credit(&it));
  fm_strlcpy(it.license, "Attribution CC BY", sizeof it.license);
  TEST_CHECK(asrc_wants_credit(&it) && !asrc_wants_credit(NULL));
}

/* ---- offline: XML reader ---------------------------------------------------------- */

static void at_xml(void) {
  FmXml x;
  char b[512];
  int starts = 0, ends = 0, texts = 0;
  const char *doc = "<a x='1' y=\"two &amp; &#65;\" z=bare empty=''><b/>t&lt;1<![CDATA[<raw>&amp;]]><c>in</c></a>";
  xml_init(&x, doc, strlen(doc));
  while (xml_next(&x) != XML_EOF) {
    if (x.type == XML_START) {
      starts++;
      if (xml_is(&x, "a")) {
        TEST_CHECK(xml_attr(&x, "x", b, sizeof b) && !strcmp(b, "1"));
        TEST_CHECK(xml_attr(&x, "y", b, sizeof b) && !strcmp(b, "two & A"));
        TEST_CHECK(xml_attr(&x, "z", b, sizeof b) && !strcmp(b, "bare"));
        TEST_CHECK(xml_attr(&x, "empty", b, sizeof b) && !strcmp(b, ""));
        TEST_CHECK(!xml_attr(&x, "q", b, sizeof b) && !strcmp(b, ""));
        TEST_CHECK(x.depth == 1 && !x.empty);
      }
      if (xml_is(&x, "b")) TEST_CHECK(x.empty && x.depth == 1);
    } else if (x.type == XML_END) ends++;
    else texts++;
  }
  TEST_CHECK(starts == 3 && ends == 2 && texts == 3 && x.depth == 0);

  xml_init(&x, doc, strlen(doc));
  xml_next(&x);
  xml_inner_text(&x, b, sizeof b);
  TEST_CHECK(!strcmp(b, "t<1<raw>&amp;in") && x.type == XML_END && x.depth == 0);
  TEST_CHECK(xml_next(&x) == XML_EOF);

  /* no entity expansion: a DOCTYPE "billion laughs" stays inert */
  const char *lol = "<!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;\"> ]>"
                    "<r>&lol2;&unknown;&#0;&#xD800;&#x110000;&#9999999999;&amp</r>";
  xml_init(&x, lol, strlen(lol));
  TEST_CHECK(xml_next(&x) == XML_START && xml_is(&x, "r"));
  xml_inner_text(&x, b, sizeof b);
  TEST_CHECK(!strcmp(b, "&lol2;&unknown;&#0;&#xD800;&#x110000;&#9999999999;&amp"));

  /* ISO-8859-1 feeds become UTF-8 */
  const char *lat = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><t a=\"\xE9\">caf\xE9 <![CDATA[\xFC]]></t>";
  xml_init(&x, lat, strlen(lat));
  TEST_CHECK(xml_next(&x) == XML_START && x.latin1);
  TEST_CHECK(xml_attr(&x, "a", b, sizeof b) && !strcmp(b, "\xC3\xA9"));
  xml_inner_text(&x, b, sizeof b);
  TEST_CHECK(!strcmp(b, "caf\xC3\xA9 \xC3\xBC"));

  /* namespaces: whatever prefix the feed binds */
  const char *ns = "<rss xmlns:pod=\"http://www.itunes.com/dtds/podcast-1.0.dtd\"><pod:duration>5</pod:duration></rss>";
  xml_init(&x, ns, strlen(ns));
  xml_next(&x);
  TEST_CHECK(xml_ns_prefix(&x, "http://www.itunes.com/dtds/podcast-1.0.dtd", b, sizeof b) && !strcmp(b, "pod"));
  TEST_CHECK(!xml_ns_prefix(&x, "urn:none", b, sizeof b) && !b[0]);
  xml_next(&x);
  TEST_CHECK(xml_is_ns(&x, "pod", "duration") && !xml_is_ns(&x, "itunes", "duration") && !xml_is(&x, "duration"));

  /* small output buffers: cut on a character boundary, NUL-terminated */
  const char *u = "<t>\xE6\x97\xA5\xE6\x97\xA5\xE6\x97\xA5</t>";
  char tiny[5];
  xml_init(&x, u, strlen(u));
  xml_next(&x);
  xml_inner_text(&x, tiny, sizeof tiny);
  TEST_CHECK(!strcmp(tiny, "\xE6\x97\xA5"));

  /* stray '<', unterminated things, mismatched ends */
  const char *odd = "<a>1 < 2 </b></a></a></a><x";
  xml_init(&x, odd, strlen(odd));
  int n = 0;
  while (xml_next(&x) != XML_EOF && n < 100) { n++; TEST_CHECK(x.depth >= 0); }
  TEST_CHECK(n < 100 && x.depth == 0);
  const char *opens[] = { "<!-- never closed", "<![CDATA[never", "<!DOCTYPE x [ <!ENTITY", "<?xml ", "<a b=\"x>",
                          "<", "&", "", "</" };
  for (int i = 0; i < FM_COUNT(opens); i++) {
    xml_init(&x, opens[i], strlen(opens[i]));
    n = 0;
    while (xml_next(&x) != XML_EOF && n < 10) n++;
    TEST_CHECK(n < 10);
  }
  xml_init(&x, NULL, 5);
  TEST_CHECK(xml_next(&x) == XML_EOF);

  /* deep nesting: the depth counter is clamped, skipping still ends */
  size_t dn = 20000 * 3 + 16;
  char *deep = (char *)fm_alloc(dn);
  size_t o = 0;
  for (int i = 0; i < 20000; i++) { memcpy(deep + o, "<a>", 3); o += 3; }
  memcpy(deep + o, "x", 1);
  o++;
  xml_init(&x, deep, o);
  xml_next(&x);
  xml_skip(&x);
  TEST_CHECK(x.type == XML_EOF && x.depth <= XML_MAX_DEPTH);
  fm_free(deep);

  /* random bytes, rich in markup characters: bounded, no crash */
  size_t gn = 64 * 1024;
  char *g = (char *)fm_alloc(gn);
  u32 seed = 12345;
  static const char kAlpha[] = "<<>>/&;#x!-[]CDATA?=\"' abc\n";
  for (int round = 0; round < 4; round++) {
    for (size_t i = 0; i < gn; i++) {
      seed = seed * 1103515245u + 12345u;
      g[i] = round & 1 ? (char)(seed >> 16) : kAlpha[(seed >> 16) % (sizeof kAlpha - 1)];
    }
    xml_init(&x, g, gn);
    size_t tokens = 0;
    while (xml_next(&x) != XML_EOF && tokens <= gn) {
      tokens++;
      if (x.type == XML_START) { xml_attr(&x, "a", b, sizeof b); xml_ns_prefix(&x, "u", b, sizeof b); }
      if (x.type == XML_TEXT) { b[0] = 0; xml_text_append(&x, b, sizeof b); }
    }
    TEST_CHECK(tokens <= gn);
    FmAsrcPage p;
    memset(&p, 0, sizeof p);
    asrc_podcasts_parse_feed(g, gn, NULL, &p);
    TEST_CHECK(p.count == 0);
    asrc_page_free(&p);
  }
  fm_free(g);
}

/* ---- offline: adapters ------------------------------------------------------------ */

static void at_radio(void) {
  FmAsrcConf c;
  FmAsrcPage p;
  char b[1400];
  memset(&c, 0, sizeof c);
  TEST_CHECK(asrc_radio_path(&c, "smooth jazz", NULL, 40, b, sizeof b) &&
             !strcmp(b, "stations/search?name=smooth%20jazz&order=clickcount&reverse=true&limit=40&offset=40"
                        "&hidebroken=true&codec=MP3"));
  c.have_ffmpeg = true;
  TEST_CHECK(asrc_radio_path(&c, "tag:jazz", NULL, 0, b, sizeof b) && strstr(b, "tag=jazz&tagExact=true") &&
             !strstr(b, "codec="));
  TEST_CHECK(asrc_radio_path(&c, NULL, "votes", 0, b, sizeof b) && strstr(b, "order=votes"));
  TEST_CHECK(asrc_radio_path(&c, NULL, "trending", 0, b, sizeof b) && strstr(b, "order=clicktrend"));
  TEST_CHECK(asrc_radio_path(&c, NULL, "country:PH", 0, b, sizeof b) && strstr(b, "countrycode=PH"));
  TEST_CHECK(asrc_radio_path(&c, NULL, "tag:hip hop", 0, b, sizeof b) && strstr(b, "tag=hip%20hop"));
  TEST_CHECK(!asrc_radio_path(&c, NULL, "country:p&", 0, b, sizeof b) && !asrc_radio_path(&c, NULL, "bogus", 0, b, sizeof b));
  TEST_CHECK(!asrc_radio_path(&c, "  ", NULL, 0, b, sizeof b) && !asrc_radio_path(&c, NULL, NULL, 0, b, sizeof b));
  TEST_CHECK(asrc_radio_mirror_count() == 4 && asrc_radio_mirror(0) && strlen(asrc_radio_mirror(3)) == 3);

  FmAsrcCat cats[ASRC_MAX_CATS];
  fm_strlcpy(c.country, "PH", sizeof c.country);
  int n = asrc_find("radio")->categories(&c, cats, ASRC_MAX_CATS);
  TEST_CHECK(n > 10 && !strcmp(cats[0].id, "top") && !strcmp(cats[3].id, "country:PH"));
  for (int i = 0; i < n; i++) TEST_CHECK(asrc_radio_path(&c, NULL, cats[i].id, 0, b, sizeof b));

  /* without FFmpeg only stations that play built in */
  c.have_ffmpeg = false;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_radio_parse(&c, kRadio, strlen(kRadio), 0, &p) == FM_OK && p.count == 2 && p.cap == ASRC_MAX_ITEMS);
  if (p.count == 2) {
    FmAsrcItem *it = &p.items[0];
    TEST_CHECK(!strcmp(it->id, "d28420a4-eccf-47a2-ace1-088c7e7cb7e0") && !strcmp(it->title, "101 SMOOTH JAZZ"));
    TEST_CHECK(it->kind == AITEM_STATION && !strcmp(it->source, "radio") && !strcmp(it->codec, "MP3") &&
               it->bitrate == 192 && it->plays == 473 && it->duration == 0);
    TEST_CHECK(!strcmp(it->url, "http://jking.cdnstream1.com/b22139_128mp3") &&
               !strcmp(it->art, "http://101smoothjazz.com/favicon.ico") && !strcmp(it->page, "http://101smoothjazz.com/"));
    TEST_CHECK(!strcmp(it->artist, "The United States Of America") &&
               !strcmp(it->album, "easy listening, jazz, smooth jazz"));
    it = &p.items[1];
    TEST_CHECK(!strcmp(it->title, "Caf\xC3\xA9 & Bar") && !strcmp(it->url, "https://s/stream") && !it->codec[0] &&
               !it->art[0]);
  }
  TEST_CHECK(!p.next[0]);
  asrc_page_free(&p);
  c.have_ffmpeg = true;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_radio_parse(&c, kRadio, strlen(kRadio), 0, &p) == FM_OK && p.count == 3);
  if (p.count == 3) TEST_CHECK(!strcmp(p.items[1].codec, "AAC") && p.items[1].bitrate == 64);
  asrc_page_free(&p);

  /* a full page has a next token */
  char *big = (char *)fm_alloc(64 * 1024);
  size_t o = (size_t)fm_snprintf(big, 64 * 1024, "[");
  for (int i = 0; i < 60; i++)
    o += (size_t)fm_snprintf(big + o, 64 * 1024 - o, "%s{\"stationuuid\":\"00000000-0000-0000-0000-%012d\","
                             "\"name\":\"S%d\",\"url_resolved\":\"http://s/%d\",\"codec\":\"MP3\"}", i ? "," : "", i, i, i);
  fm_snprintf(big + o, 64 * 1024 - o, "]");
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_radio_parse(&c, big, strlen(big), 80, &p) == FM_OK && p.count == ASRC_MAX_ITEMS &&
             p.cap == ASRC_MAX_ITEMS && !strcmp(p.next, "120"));
  asrc_page_free(&p);
  fm_free(big);

  TEST_CHECK(asrc_radio_parse_click(kRadioClick, strlen(kRadioClick), b, sizeof b) &&
             !strcmp(b, "https://atunwadigital.streamguys1.com/brilafm"));
  TEST_CHECK(!asrc_radio_parse_click("{\"ok\":false,\"url\":\"https://x\"}", 31, b, sizeof b) && !b[0]);
  TEST_CHECK(!asrc_radio_parse_click("nope", 4, b, sizeof b));
}

static void at_audius(void) {
  char b[1200];
  asrc_audius_search_url("lo fi", 40, b, sizeof b);
  TEST_CHECK(!strcmp(b, "https://api.audius.co/v1/tracks/search?query=lo%20fi&limit=40&offset=40&app_name=mmcfm"));
  TEST_CHECK(asrc_audius_browse_url("genre:Hip-Hop/Rap", 0, b, sizeof b) && strstr(b, "genre=Hip-Hop%2FRap") &&
             strstr(b, "app_name=mmcfm"));
  TEST_CHECK(asrc_audius_browse_url("trending:month", 0, b, sizeof b) && strstr(b, "time=month"));
  TEST_CHECK(asrc_audius_browse_url("playlists", 20, b, sizeof b) && strstr(b, "/playlists/trending?") &&
             strstr(b, "offset=20"));
  TEST_CHECK(asrc_audius_browse_url(NULL, 0, b, sizeof b) && strstr(b, "/tracks/trending?"));
  TEST_CHECK(!asrc_audius_browse_url("genre:Nope", 0, b, sizeof b) && !asrc_audius_browse_url("x", 0, b, sizeof b));
  asrc_audius_stream_url("ng9rl", b, sizeof b);
  TEST_CHECK(!strcmp(b, "https://api.audius.co/v1/tracks/ng9rl/stream?app_name=mmcfm"));

  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_audius_parse(kAudius, strlen(kAudius), 0, ASRC_PAGE_SIZE, &p) == FM_OK && p.count == 2);
  if (p.count == 2) {
    FmAsrcItem *t = &p.items[0], *a = &p.items[1];
    TEST_CHECK(t->kind == AITEM_TRACK && !strcmp(t->id, "ng9rl") && !strcmp(t->title, "lofi type beat") &&
               !strcmp(t->artist, "bsdu") && !strcmp(t->album, "Lo-Fi") && t->duration == 334 && t->plays == 68873);
    TEST_CHECK(!strcmp(t->url, "https://api.audius.co/v1/tracks/ng9rl/stream?app_name=mmcfm") &&
               !strcmp(t->art, "https://n/480x480.jpg") && !strcmp(t->page, "https://audius.co/bsdu/lofi-type-beat-14815") &&
               !strcmp(t->license, "Attribution CC BY") && !strcmp(t->published, "2019-07-01") && !strcmp(t->codec, "MP3"));
    TEST_CHECK(a->kind == AITEM_ALBUM && !strcmp(a->id, "RKxOQ") && !strcmp(a->title, "Lofi Space inspired") &&
               !strcmp(a->artist, "Lofi Army") && a->plays == 22046 && !a->url[0] && !strcmp(a->album, "Playlist"));
  }
  TEST_CHECK(!p.next[0]);
  asrc_page_free(&p);
  /* want caps the page and gives a next token */
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_audius_parse(kAudius, strlen(kAudius), 20, 2, &p) == FM_OK && p.count == 1 && p.cap == 2 &&
             !strcmp(p.next, "22"));
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_audius_parse("{\"error\":\"bad app\"}", 19, 0, 40, &p) == FM_ERR_FORMAT && strstr(p.error, "bad app"));
  asrc_page_free(&p);
}

static void at_archive(void) {
  char b[2400];
  asrc_archive_search_url("grateful dead", NULL, 2, b, sizeof b);
  TEST_CHECK(strstr(b, "mediatype%3A%28audio%20OR%20etree%29") && strstr(b, "-access-restricted-item") &&
             strstr(b, "rows=40&page=2") && strstr(b, "downloads%20desc"));
  asrc_archive_search_url(NULL, "etree", 1, b, sizeof b);
  TEST_CHECK(strstr(b, "collection%3Aetree"));

  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_search(kArchiveSearch, strlen(kArchiveSearch), 1, &p) == FM_OK && p.count == 2);
  if (p.count == 2) {
    TEST_CHECK(p.items[0].kind == AITEM_ALBUM && !strcmp(p.items[0].id, "OTRR_Dragnet_Singles") &&
               !strcmp(p.items[0].artist, "Old Time Radio Researchers Group") && p.items[0].plays == 2855786 &&
               !strcmp(p.items[0].published, "2007-10-12") &&
               !strcmp(p.items[0].art, "https://archive.org/services/img/OTRR_Dragnet_Singles"));
    TEST_CHECK(!strcmp(p.items[1].title, "jazz") && !strcmp(p.items[1].published, "1959-01-01"));
  }
  asrc_page_free(&p);

  FmAsrcConf c;
  FmAsrcItem album;
  memset(&c, 0, sizeof c);
  memset(&album, 0, sizeof album);
  fm_strlcpy(album.id, "gd77", sizeof album.id);
  fm_strlcpy(album.title, "Item title", sizeof album.title);
  fm_strlcpy(album.art, "https://archive.org/services/img/gd77", sizeof album.art);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, kArchiveMeta, strlen(kArchiveMeta), &album, 0, &p) == FM_OK && p.count == 4 &&
             p.cap == 4);
  if (p.count == 4) {
    FmAsrcItem *t = p.items;
    TEST_CHECK(!strcmp(t[0].title, "Turning") && !strcmp(t[0].url, "https://archive.org/download/gd77/gd77d01t01.mp3") &&
               t[0].duration == 39 && !strcmp(t[0].codec, "MP3"));
    TEST_CHECK(!strcmp(t[1].title, "Minglewood Blues") && !strcmp(t[1].artist, "Grateful Dead") &&
               !strcmp(t[1].album, "1977-05-08 - Barton Hall") && t[1].duration == 323);
    TEST_CHECK(!strcmp(t[2].title, "gd77d01t3 extra") && !strcmp(t[2].codec, "FLAC") &&
               !strcmp(t[2].url, "https://archive.org/download/gd77/gd77d01t3%20extra.flac"));
    TEST_CHECK(!strcmp(t[3].title, "Ten"));
    TEST_CHECK(!strcmp(t[0].license, "CC BY-NC-SA 3.0") && !strcmp(t[0].published, "1977-05-08") &&
               !strcmp(t[0].album, "Grateful Dead Live 1977") && !strcmp(t[0].art, album.art) &&
               !strcmp(t[0].page, "https://archive.org/details/gd77") && t[0].kind == AITEM_TRACK &&
               !strcmp(t[0].id, "gd77/gd77d01t01.mp3"));
  }
  asrc_page_free(&p);
  c.have_ffmpeg = true;                            /* Ogg joins with FFmpeg */
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, kArchiveMeta, strlen(kArchiveMeta), &album, 0, &p) == FM_OK && p.count == 5);
  asrc_page_free(&p);
  c.have_ffmpeg = false;

  const char *restricted = "{\"metadata\":{\"access-restricted-item\":\"true\"},\"files\":[]}";
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, restricted, strlen(restricted), &album, 0, &p) == FM_ERR_ACCESS && p.error[0]);
  asrc_page_free(&p);
  const char *ogg = "{\"metadata\":{},\"files\":[{\"name\":\"a.ogg\",\"format\":\"Ogg Vorbis\"}]}";
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, ogg, strlen(ogg), &album, 0, &p) == FM_ERR_UNSUPPORTED &&
             strstr(p.error, "FFmpeg"));
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, "{}", 2, &album, 0, &p) == FM_ERR_NOT_FOUND);
  asrc_page_free(&p);

  /* 350 tracks: pages of 300 */
  size_t cap = 64 * 1024;
  char *big = (char *)fm_alloc(cap);
  size_t o = (size_t)fm_snprintf(big, cap, "{\"metadata\":{},\"files\":[");
  for (int i = 0; i < 350; i++)
    o += (size_t)fm_snprintf(big + o, cap - o, "%s{\"name\":\"ep%d.mp3\",\"format\":\"VBR MP3\"}", i ? "," : "", i);
  fm_snprintf(big + o, cap - o, "]}");
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, big, strlen(big), &album, 0, &p) == FM_OK && p.count == ASRC_MAX_CHILDREN &&
             !strcmp(p.next, "300") && !strcmp(p.items[2].title, "ep2") && !strcmp(p.items[10].title, "ep10"));
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_archive_parse_files(&c, big, strlen(big), &album, 300, &p) == FM_OK && p.count == 50 && !p.next[0] &&
             !strcmp(p.items[0].title, "ep300"));
  asrc_page_free(&p);
  fm_free(big);
}

static void at_podcasts(void) {
  FmAsrcConf c;
  char b[1200];
  memset(&c, 0, sizeof c);
  asrc_podcasts_search_url(&c, "true crime", b, sizeof b);
  TEST_CHECK(!strcmp(b, "https://itunes.apple.com/search?term=true%20crime&media=podcast&entity=podcast&limit=40&country=us"));
  fm_strlcpy(c.country, "PH", sizeof c.country);
  c.safe_search = true;
  asrc_podcasts_search_url(&c, "x", b, sizeof b);
  TEST_CHECK(strstr(b, "country=ph&explicit=No"));

  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_search(kItunes, strlen(kItunes), &p) == FM_OK && p.count == 2);
  if (p.count == 2) {
    FmAsrcItem *it = &p.items[0];
    TEST_CHECK(it->kind == AITEM_PODCAST && !strcmp(it->id, "1291579828") && !strcmp(it->title, "History That Doesn't Suck") &&
               !strcmp(it->artist, "Prof. Greg Jackson") && !strcmp(it->album, "History") &&
               !strcmp(it->art, "https://is1-ssl.mzstatic.com/a/600x600bb.jpg") &&
               !strcmp(it->url, "https://rss.amperwave.net/v2/feed/audacynetwork/e69da14a26973f1dc761432876205254") &&
               !strcmp(it->published, "2026-10-05"));
    TEST_CHECK(!strcmp(p.items[1].art, "https://x/100x100bb.jpg"));
  }
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_top(kItunesTop, strlen(kItunesTop), &p) == FM_OK && p.count == 1);
  if (p.count == 1)
    TEST_CHECK(!strcmp(p.items[0].id, "1101180313") && !strcmp(p.items[0].title, "Barangay Love Stories") &&
               !strcmp(p.items[0].artist, "Barangay LS 97.1 Manila | GMA Network Inc.") &&
               !strcmp(p.items[0].album, "Society & Culture") && !p.items[0].url[0] &&
               !strcmp(p.items[0].art, "https://is1-ssl.mzstatic.com/image/thumb/P/v4/27/mza_6.jpg/600x600bb.png") &&
               !strcmp(p.items[0].page, "https://podcasts.apple.com/ph/podcast/barangay-love-stories/id1101180313?uo=2") &&
               !strcmp(p.items[0].published, "2026-10-08") && p.items[0].kind == AITEM_PODCAST);
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_top(kItunesTopOne, strlen(kItunesTopOne), &p) == FM_OK && p.count == 1 &&
             !strcmp(p.items[0].title, "The Daily") && !p.items[0].art[0]);
  asrc_page_free(&p);
  memset(&c, 0, sizeof c);
  TEST_CHECK(asrc_podcasts_top_url(&c, "top", b, sizeof b) &&
             !strcmp(b, "https://itunes.apple.com/us/rss/toppodcasts/limit=40/json"));
  fm_strlcpy(c.country, "PH", sizeof c.country);
  c.safe_search = true;
  TEST_CHECK(asrc_podcasts_top_url(&c, "genre:1488", b, sizeof b) &&
             !strcmp(b, "https://itunes.apple.com/ph/rss/toppodcasts/limit=40/genre=1488/explicit=false/json"));
  TEST_CHECK(!asrc_podcasts_top_url(&c, "genre:1", b, sizeof b) && !asrc_podcasts_top_url(&c, "nope", b, sizeof b));
  TEST_CHECK(asrc_podcasts_parse_lookup(kItunesLookup, strlen(kItunesLookup), b, sizeof b) &&
             !strcmp(b, "https://feeds.megaphone.fm/GLT4787413333"));
  TEST_CHECK(!asrc_podcasts_parse_lookup("{\"results\":[]}", 14, b, sizeof b));

  /* the odd feed: newest first, all fields */
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_feed(kFeed, strlen(kFeed), NULL, &p) == FM_OK && p.count == 4);
  if (p.count == 4) {
    FmAsrcItem *e = p.items;
    TEST_CHECK(!strcmp(e[0].title, "Ep 2 \xE2\x80\x94 dash \xE2\x98\xBA") && !strcmp(e[0].codec, "AAC") &&
               e[0].duration == 3723 && !strcmp(e[0].published, "2026-10-07") &&
               !strcmp(e[0].art, "https://example.org/ep2.jpg") && !strcmp(e[0].page, "https://example.org/ep2") &&
               !strcmp(e[0].url, "https://example.org/ep2.m4a"));
    TEST_CHECK(!strcmp(e[1].title, "Ep 1: <b>Bold</b> & \"quoted\"") && !strcmp(e[1].id, "guid-1") &&
               !strcmp(e[1].url, "https://example.org/ep1.mp3?a=1&b=2") && e[1].duration == 3723 &&
               !strcmp(e[1].codec, "MP3") && !strcmp(e[1].artist, "Ann <A> Author") &&
               !strcmp(e[1].album, "Odd & Valid") && !strcmp(e[1].art, "https://example.org/show-it.jpg"));
    TEST_CHECK(!strcmp(e[2].title, "Ep 0 oldest") && !strcmp(e[2].artist, "Guest") && e[2].duration == 3723 &&
               !strcmp(e[2].codec, "MP3") && !strcmp(e[2].published, "2026-01-01"));
    TEST_CHECK(!strcmp(e[3].title, "No date") && e[3].duration == 95 && !e[3].published[0] &&
               !strncmp(e[3].id, "ep-", 3));
    for (int i = 0; i < 4; i++) TEST_CHECK(e[i].kind == AITEM_TRACK && !strcmp(e[i].source, "podcasts") && e[i].plays == -1);
  }
  asrc_page_free(&p);

  /* the show's own fields fill the gaps */
  FmAsrcItem show;
  memset(&show, 0, sizeof show);
  fm_strlcpy(show.title, "Show From Directory", sizeof show.title);
  fm_strlcpy(show.page, "https://podcasts.apple.com/x", sizeof show.page);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_feed(kFeed, strlen(kFeed), &show, &p) == FM_OK && p.count == 4);
  if (p.count == 4) TEST_CHECK(!strcmp(p.items[1].album, "Show From Directory") && !strcmp(p.items[1].page, show.page));
  asrc_page_free(&p);

  /* every truncation of the feed: no crash, never more episodes */
  size_t fl = strlen(kFeed);
  bool ok = true;
  for (size_t n = 0; n <= fl; n++) {
    memset(&p, 0, sizeof p);
    asrc_podcasts_parse_feed(kFeed, n, NULL, &p);
    if (p.count > 4 || p.count > p.cap) ok = false;
    asrc_page_free(&p);
  }
  TEST_CHECK(ok);

  /* not a feed */
  const char *html = "<!DOCTYPE html><html><body><item>x</item></body></html>";
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_feed(html, strlen(html), NULL, &p) == FM_ERR_FORMAT && p.count == 0 && p.error[0]);
  asrc_page_free(&p);

  /* 400 episodes, oldest first: the newest 300 are kept, newest first */
  size_t cap = 200 * 1024;
  char *big = (char *)fm_alloc(cap);
  static const char *const kMon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  size_t o = (size_t)fm_snprintf(big, cap, "<rss><channel><title>Serial</title>");
  for (int i = 0; i < 400; i++)
    o += (size_t)fm_snprintf(big + o, cap - o, "<item><title>E%d</title><enclosure url=\"https://s/%d.mp3\" type=\"audio/mpeg\"/>"
                             "<pubDate>Mon, %02d %s %d 10:00:00 GMT</pubDate></item>\n", i, i, i % 28 + 1,
                             kMon[(i / 28) % 12], 2020 + i / 336);
  fm_snprintf(big + o, cap - o, "</channel></rss>");
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_podcasts_parse_feed(big, strlen(big), NULL, &p) == FM_OK && p.count == ASRC_MAX_CHILDREN &&
             p.cap == ASRC_MAX_CHILDREN);
  if (p.count == ASRC_MAX_CHILDREN)
    TEST_CHECK(!strcmp(p.items[0].title, "E399") && !strcmp(p.items[1].title, "E398") &&
               !strcmp(p.items[299].title, "E100"));
  asrc_page_free(&p);
  fm_free(big);
}

static void at_keyed(void) {
  FmAsrcConf c;
  char b[1600];
  memset(&c, 0, sizeof c);
  fm_strlcpy(c.key_jamendo, "ab&cd", sizeof c.key_jamendo);
  asrc_jamendo_url(&c, "chill out", NULL, 40, b, sizeof b);
  TEST_CHECK(strstr(b, "client_id=ab%26cd&") && strstr(b, "search=chill%20out") && strstr(b, "offset=40") &&
             strstr(b, "audioformat=mp32"));
  asrc_jamendo_url(&c, NULL, "tag:rock", 0, b, sizeof b);
  TEST_CHECK(strstr(b, "fuzzytags=rock"));
  asrc_jamendo_url(&c, NULL, "new", 0, b, sizeof b);
  TEST_CHECK(strstr(b, "order=releasedate_desc"));
  asrc_jamendo_url(&c, NULL, NULL, 0, b, sizeof b);
  TEST_CHECK(strstr(b, "order=popularity_week"));

  FmAsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_jamendo_parse(kJamendo, strlen(kJamendo), 0, &p) == FM_OK && p.count == 1);
  if (p.count == 1) {
    FmAsrcItem *t = p.items;
    TEST_CHECK(!strcmp(t->title, "Let's go") && !strcmp(t->artist, "Artist A") && !strcmp(t->album, "Album") &&
               !strcmp(t->license, "CC BY-NC-ND 3.0") && t->duration == 192 && !strcmp(t->published, "2018-03-02") &&
               !strcmp(t->url, "https://prod-1.storage.jamendo.com/?trackid=1532771&format=mp32") &&
               !strcmp(t->page, "https://www.jamendo.com/track/1532771") && !strcmp(t->codec, "MP3"));
  }
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_jamendo_parse(kJamendoBadKey, strlen(kJamendoBadKey), 0, &p) == FM_ERR_ACCESS &&
             strstr(p.error, "did not accept the API key"));
  asrc_page_free(&p);

  fm_strlcpy(c.key_freesound, "k3y", sizeof c.key_freesound);
  asrc_freesound_url(&c, "rain on roof", 3, b, sizeof b);
  TEST_CHECK(strstr(b, "query=rain%20on%20roof&token=k3y&") && strstr(b, "page=3") && strstr(b, "previews"));
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_freesound_parse(kFreesound, strlen(kFreesound), 1, &p) == FM_OK && p.count == 1 && !strcmp(p.next, "2"));
  if (p.count == 1) {
    FmAsrcItem *t = p.items;
    TEST_CHECK(!strcmp(t->id, "398275") && !strcmp(t->title, "Rain on window.wav") && !strcmp(t->artist, "inchadney") &&
               !strcmp(t->album, "rain, window, storm, weather") && !strcmp(t->license, "CC0 1.0") &&
               t->duration == 64.5 && !strcmp(t->url, "https://cdn.freesound.org/previews/398/398275_123-hq.mp3") &&
               !strcmp(t->art, "https://cdn.freesound.org/displays/398/398275_123_wave_M.png") &&
               !strcmp(t->page, "https://freesound.org/people/inchadney/sounds/398275/"));
  }
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(asrc_freesound_parse("{\"detail\":\"Invalid token.\"}", 27, 1, &p) == FM_ERR_FORMAT &&
             strstr(p.error, "Invalid token"));
  asrc_page_free(&p);

  /* missing keys: no network needed to say so */
  memset(&c, 0, sizeof c);
  memset(&p, 0, sizeof p);
  TEST_CHECK(g_asrc_jamendo.search(&c, "x", NULL, &p, NULL) == FM_ERR_ACCESS && strstr(p.error, "Add a free Jamendo key"));
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(g_asrc_jamendo.browse(&c, "week", NULL, &p, NULL) == FM_ERR_ACCESS);
  asrc_page_free(&p);
  memset(&p, 0, sizeof p);
  TEST_CHECK(g_asrc_freesound.search(&c, "x", NULL, &p, NULL) == FM_ERR_ACCESS && strstr(p.error, "Add a free Freesound key"));
  asrc_page_free(&p);
}

/* every parser on empty, truncated and wrong-shaped replies */
static void at_garbage(void) {
  static const char *const kBad[] = { "", "{", "[]", "{}", "null", "<html>Bad gateway</html>", "{\"data\":5}",
                                      "{\"results\":{}}", "[1,2,3]", "{\"response\":{\"docs\":7}}" };
  FmAsrcConf c;
  FmAsrcItem album;
  memset(&c, 0, sizeof c);
  memset(&album, 0, sizeof album);
  fm_strlcpy(album.id, "x", sizeof album.id);
  for (int i = 0; i < FM_COUNT(kBad); i++) {
    const char *s = kBad[i];
    size_t n = strlen(s);
    FmAsrcPage p;
#define ONE(call) do { memset(&p, 0, sizeof p); call; TEST_CHECK(p.count == 0 || i == 2 || i == 8); asrc_page_free(&p); } while (0)
    ONE(asrc_radio_parse(&c, s, n, 0, &p));
    ONE(asrc_audius_parse(s, n, 0, 40, &p));
    ONE(asrc_archive_parse_search(s, n, 1, &p));
    ONE(asrc_archive_parse_files(&c, s, n, &album, 0, &p));
    ONE(asrc_podcasts_parse_search(s, n, &p));
    ONE(asrc_podcasts_parse_top(s, n, &p));
    ONE(asrc_podcasts_parse_feed(s, n, NULL, &p));
    ONE(asrc_jamendo_parse(s, n, 0, &p));
    ONE(asrc_freesound_parse(s, n, 1, &p));
#undef ONE
  }
  /* truncations of every canned reply */
  const char *const kAll[] = { kRadio, kAudius, kArchiveSearch, kArchiveMeta, kItunes, kItunesTop, kJamendo, kFreesound };
  for (int k = 0; k < FM_COUNT(kAll); k++) {
    size_t len = strlen(kAll[k]);
    for (size_t n = 0; n < len; n += 7) {
      FmAsrcPage p;
      memset(&p, 0, sizeof p);
      switch (k) {
        case 0: asrc_radio_parse(&c, kAll[k], n, 0, &p); break;
        case 1: asrc_audius_parse(kAll[k], n, 0, 40, &p); break;
        case 2: asrc_archive_parse_search(kAll[k], n, 1, &p); break;
        case 3: asrc_archive_parse_files(&c, kAll[k], n, &album, 0, &p); break;
        case 4: asrc_podcasts_parse_search(kAll[k], n, &p); break;
        case 5: asrc_podcasts_parse_top(kAll[k], n, &p); break;
        case 6: asrc_jamendo_parse(kAll[k], n, 0, &p); break;
        default: asrc_freesound_parse(kAll[k], n, 1, &p); break;
      }
      TEST_CHECK(p.count <= p.cap && p.count <= ASRC_MAX_ITEMS);
      asrc_page_free(&p);
    }
  }
}

/* streams and downloads that need no network */
static void at_streams(const char *tmp) {
  FmAsrcConf c;
  FmAsrcItem it;
  FmAsrcStream st;
  char err[256], out[FM_PATH_MAX];
  memset(&c, 0, sizeof c);
  fm_path_join(c.download_dir, sizeof c.download_dir, tmp, "dl");
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "podcasts", sizeof it.source);
  it.kind = AITEM_TRACK;
  fm_strlcpy(it.url, "https://example.org/ep 1.mp3", sizeof it.url);
  fm_strlcpy(it.codec, "MP3", sizeof it.codec);
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_OK && !strcmp(st.url, "https://example.org/ep%201.mp3") &&
             !st.live && !strcmp(st.codec, "MP3"));
  fm_strlcpy(it.url, "https://example.org/ep.m4a", sizeof it.url);
  it.codec[0] = 0;                                 /* the codec comes from the name */
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED &&
             !strcmp(err, "This episode uses AAC \xE2\x80\x94 install FFmpeg to play it"));
  c.have_ffmpeg = true;
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_OK && !strcmp(st.codec, "AAC"));
  c.have_ffmpeg = false;
  fm_strlcpy(it.url, "javascript:alert(1)", sizeof it.url);
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_ERR_NOT_FOUND && err[0]);
  it.kind = AITEM_PODCAST;
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED && strstr(err, "episode"));
  TEST_CHECK(asrc_download(&c, &it, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED &&
             !out[0]);
  TEST_CHECK(asrc_stream(&c, NULL, &st, err, sizeof err, NULL) == FM_ERR_NOT_FOUND);
  /* Audius resolves without the network */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "audius", sizeof it.source);
  fm_strlcpy(it.id, "ng9rl", sizeof it.id);
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_OK &&
             !strcmp(st.url, "https://api.audius.co/v1/tracks/ng9rl/stream?app_name=mmcfm") && !strcmp(st.codec, "MP3"));
  /* stations: an AAC one says what is missing before any network call */
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "radio", sizeof it.source);
  it.kind = AITEM_STATION;
  fm_strlcpy(it.id, "604bb4af-ccf6-47bd-819e-7d1accd15e05", sizeof it.id);
  fm_strlcpy(it.url, "http://x/aac", sizeof it.url);
  fm_strlcpy(it.codec, "AAC", sizeof it.codec);
  TEST_CHECK(asrc_stream(&c, &it, &st, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED &&
             !strcmp(err, "This station uses AAC \xE2\x80\x94 install FFmpeg to play it"));
  TEST_CHECK(asrc_download(&c, &it, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED &&
             strstr(err, "Live radio"));
  if (!net_available()) {
    memset(&it, 0, sizeof it);
    fm_strlcpy(it.source, "jamendo", sizeof it.source);
    fm_strlcpy(it.url, "https://example.org/a.mp3", sizeof it.url);
    TEST_CHECK(asrc_download(&c, &it, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_ERR_UNSUPPORTED &&
               err[0]);
    FmAsrcPage p;
    memset(&p, 0, sizeof p);
    TEST_CHECK(g_asrc_radio.browse(&c, "top", NULL, &p, NULL) == FM_ERR_UNSUPPORTED && p.error[0]);
    asrc_page_free(&p);
  }
}

/* ---- online ---------------------------------------------------------------------- */

static bool env_on(const char *name) {
  const char *v = getenv(name);
  return v && !strcmp(v, "1");
}

static void show_page(const char *what, FmErr e, const FmAsrcPage *p, u64 t0) {
  printf("  %s: %s, %d items, next \"%s\" (%d ms)%s%s\n", what, fm_err_str(e), p->count, p->next,
         (int)(plat_now_ms() - t0), p->error[0] ? " -- " : "", p->error);
  if (p->count) {
    const FmAsrcItem *it = &p->items[0];
    printf("    first: \"%s\" / %s / %s [%s %d kbps %.0f s] %s\n", it->title, it->artist, it->album, it->codec,
           it->bitrate, it->duration, it->url[0] ? it->url : "(container)");
  }
}

static int run_search(const FmAsrc *s, const FmAsrcConf *c, const char *q, FmAsrcPage *p) {
  memset(p, 0, sizeof *p);
  u64 t0 = plat_now_ms();
  FmErr e = s->search(c, q, NULL, p, NULL);
  char what[96];
  fm_snprintf(what, sizeof what, "%s search \"%s\"", s->name, q);
  show_page(what, e, p, t0);
  TEST_CHECK(e == FM_OK && p->count > 0);
  return p->count;
}

static int run_browse(const FmAsrc *s, const FmAsrcConf *c, const char *cat, FmAsrcPage *p) {
  memset(p, 0, sizeof *p);
  u64 t0 = plat_now_ms();
  FmErr e = s->browse(c, cat, NULL, p, NULL);
  char what[96];
  fm_snprintf(what, sizeof what, "%s browse \"%s\"", s->name, cat);
  show_page(what, e, p, t0);
  if (e != FM_OK && strstr(p->error, "server error 5")) {
    /* the site's own outage (Apple's chart answered 504 once), not ours: the error text is the check */
    printf("    (upstream outage, not counted)\n");
    return 0;
  }
  TEST_CHECK(e == FM_OK && p->count > 0);
  return p->count;
}

/* MPEG audio: an ID3 tag, or two frame headers a frame apart */
static bool looks_mp3(const u8 *b, size_t n) {
  static const int kBr1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
  static const int kBr2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
  static const int kSr[] = { 44100, 48000, 32000, 0 };
  if (n >= 3 && !memcmp(b, "ID3", 3)) return true;
  for (size_t i = 0; i + 4 <= n; i++) {
    if (b[i] != 0xFF || (b[i + 1] & 0xE6) != 0xE2) continue;           /* sync + layer III */
    int ver = (b[i + 1] >> 3) & 3, bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3, pad = (b[i + 2] >> 1) & 1;
    if (ver == 1 || !kBr1[bri] || sri == 3) continue;
    int sr = kSr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    int len = ver == 3 ? 144000 * kBr1[bri] / sr + pad : 72000 * kBr2[bri] / sr + pad;
    if (len < 24 || i + (size_t)len + 2 > n) continue;
    if (b[i + len] == 0xFF && (b[i + len + 1] & 0xE6) == 0xE2) return true;
  }
  return false;
}

typedef struct Head { u8 *buf; size_t n, max; } Head;

static bool head_sink(void *user, const u8 *p, size_t n) {
  Head *h = (Head *)user;
  size_t k = FM_MIN(n, h->max - h->n);
  memcpy(h->buf + h->n, p, k);
  h->n += k;
  return h->n < h->max;
}

/* resolve, then the first 64 KB (an endless station too), then 2 s decoded */
static void check_stream(const FmAsrcConf *c, const FmAsrcItem *it) {
  FmAsrcStream st;
  char err[256];
  u64 t0 = plat_now_ms();
  FmErr e = asrc_stream(c, it, &st, err, sizeof err, NULL);
  printf("  stream \"%s\" (%s): %s %s (%d ms)\n", it->title, it->source, fm_err_str(e), e == FM_OK ? st.url : err,
         (int)(plat_now_ms() - t0));
  TEST_CHECK(e == FM_OK && st.url[0]);
  if (e != FM_OK) return;
  Head h;
  h.max = 64 * 1024;
  h.n = 0;
  h.buf = (u8 *)fm_alloc(h.max);
  FmNetResp r;
  memset(&r, 0, sizeof r);
  t0 = plat_now_ms();
  FmErr ge = net_get_stream(st.url, st.headers[0] ? st.headers : NULL, NULL, head_sink, &h, &r, NULL);
  bool mp3 = looks_mp3(h.buf, h.n);
  printf("    first %d bytes: %s, HTTP %d %s, %s%s%s (%d ms)\n", (int)h.n, fm_err_str(ge), r.status, r.type,
         mp3 ? "MP3" : "NOT MP3", r.error[0] ? " -- " : "", r.error, (int)(plat_now_ms() - t0));
  TEST_CHECK(h.n == h.max && mp3);
  fm_free(h.buf);
  /* the music player's way in */
  t0 = plat_now_ms();
  FmErr ae = FM_OK;
  FmAudio *a = aud_open_ex(st.url, st.headers[0] ? st.headers : NULL, &ae);
  if (!a) {
    printf("    aud_open_ex: %s\n", fm_err_str(ae));
    TEST_CHECK(a != NULL);
    return;
  }
  u64 t1 = plat_now_ms();
  int ch = aud_channels(a), rate = aud_rate(a);
  float *buf = (float *)fm_alloc(4096 * 2 * sizeof(float));
  i64 want = (i64)rate * 2, got = 0;
  while (got < want) {
    int k = aud_read(a, buf, 4096);
    if (k <= 0) break;
    got += k;
  }
  char now[256];
  aud_now_playing(a, now, sizeof now);
  printf("    decode: %s %d Hz x%d, live %d, length %llu, %.2f s in %d ms (open %d ms)%s%s\n", aud_codec(a), rate, ch,
         aud_is_live(a), (unsigned long long)aud_length(a), rate ? (double)got / rate : 0.0, (int)(plat_now_ms() - t1),
         (int)(t1 - t0), now[0] ? ", now playing: " : "", now);
  TEST_CHECK(got >= want && rate > 0);
  TEST_CHECK(aud_is_live(a) == (it->kind == AITEM_STATION));
  fm_free(buf);
  aud_close(a);
}

static bool read_text(const char *path, char *out, size_t cap) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  size_t n = fread(out, 1, cap - 1, f);
  out[n] = 0;
  fclose(f);
  return true;
}

static void at_online(const char *tmp) {
  if (!env_on("MMCFM_NET_TEST")) return;
  if (!net_available()) { printf("  network: %s (skipped)\n", net_backend()); return; }
  FmAsrcConf c;
  asrc_conf_snapshot(&c);
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "online-audio");
  fm_path_join(c.download_dir, sizeof c.download_dir, tmp, "downloads");
  c.key_jamendo[0] = c.key_freesound[0] = 0;
  FmAsrcPage p, q;

  /* Radio Browser */
  const FmAsrc *s = asrc_find("radio");
  if (run_search(s, &c, "jazz", &p)) {
    if (p.next[0]) {
      memset(&q, 0, sizeof q);
      u64 t0 = plat_now_ms();
      FmErr e = s->search(&c, "jazz", p.next, &q, NULL);
      show_page("Radio Browser page 2", e, &q, t0);
      TEST_CHECK(e == FM_OK && q.count > 0);
      asrc_page_free(&q);
    }
    if (!c.have_ffmpeg)
      for (int i = 0; i < p.count; i++) TEST_CHECK(asrc_codec_builtin(p.items[i].codec));
  }
  asrc_page_free(&p);
  if (run_browse(s, &c, "top", &p)) {
    int k = 0;                                     /* an MP3 station (with FFmpeg the top one may be AAC) */
    while (k < p.count - 1 && strcmp(p.items[k].codec, "MP3")) k++;
    check_stream(&c, &p.items[k]);
  }
  asrc_page_free(&p);
  run_browse(s, &c, "tag:jazz", &q);
  asrc_page_free(&q);
  if (env_on("MMCFM_AAC_PROBE")) {
    /* AAC radio: without FFmpeg libraries, can the video decoder fallback
    ** (Media Foundation on Windows) still play it? Measured, not required.
    ** Opt-in: on 2026-10-08 an "audio/aac" station was taken for MP3 (a
    ** false frame sync in the first 16 KB) and aud_open_ex never returned. */
    FmAsrcConf k = c;
    k.have_ffmpeg = true;
    memset(&q, 0, sizeof q);
    if (s->browse(&k, "top", NULL, &q, NULL) == FM_OK) {
      int tried = 0;
      for (int i = 0; i < q.count && tried < 3; i++) {
        if (strcmp(q.items[i].codec, "AAC")) continue;
        tried++;
        FmAsrcStream st;
        char err[256];
        if (asrc_stream(&k, &q.items[i], &st, err, sizeof err, NULL) != FM_OK) continue;
        u64 t0 = plat_now_ms();
        FmErr ae = FM_OK;
        FmAudio *a = aud_open_ex(st.url, NULL, &ae);
        i64 got = 0;
        if (a) {
          float *buf = (float *)fm_alloc(4096 * 2 * sizeof(float));
          while (got < (i64)aud_rate(a) * 2) {
            int n = aud_read(a, buf, 4096);
            if (n <= 0) break;
            got += n;
          }
          fm_free(buf);
        }
        printf("  AAC station \"%s\" without FFmpeg: %s, %s %.2f s decoded (%d ms)\n", q.items[i].title,
               a ? "opened" : fm_err_str(ae), a ? aud_codec(a) : "", a && aud_rate(a) ? (double)got / aud_rate(a) : 0.0,
               (int)(plat_now_ms() - t0));
        if (a) aud_close(a);
      }
    }
    asrc_page_free(&q);
  }
  if (c.country[0]) {
    char cat[32];
    fm_snprintf(cat, sizeof cat, "country:%s", c.country);
    run_browse(s, &c, cat, &q);
    asrc_page_free(&q);
  }

  /* Audius */
  s = asrc_find("audius");
  FmAsrcItem shortest;
  memset(&shortest, 0, sizeof shortest);
  if (run_search(s, &c, "lofi", &p)) {
    check_stream(&c, &p.items[0]);
    for (int i = 0; i < p.count; i++)
      if (p.items[i].duration > 20 && (!shortest.id[0] || p.items[i].duration < shortest.duration)) shortest = p.items[i];
  }
  asrc_page_free(&p);
  run_browse(s, &c, "trending", &p);
  asrc_page_free(&p);
  run_browse(s, &c, "genre:Lo-Fi", &p);
  asrc_page_free(&p);
  if (run_browse(s, &c, "playlists", &p)) {
    memset(&q, 0, sizeof q);
    u64 t0 = plat_now_ms();
    FmErr e = s->children(&c, &p.items[0], NULL, &q, NULL);
    show_page("Audius playlist tracks", e, &q, t0);
    TEST_CHECK(e == FM_OK && q.count > 0);
    asrc_page_free(&q);
  }
  asrc_page_free(&p);

  /* Internet Archive */
  s = asrc_find("archive");
  if (run_search(s, &c, "grateful dead 1977", &p)) {
    memset(&q, 0, sizeof q);
    u64 t0 = plat_now_ms();
    FmErr e = s->children(&c, &p.items[0], NULL, &q, NULL);
    show_page("archive.org album tracks", e, &q, t0);
    TEST_CHECK(e == FM_OK && q.count > 0);
    asrc_page_free(&q);
  }
  asrc_page_free(&p);
  if (run_browse(s, &c, "oldtimeradio", &p)) {
    memset(&q, 0, sizeof q);
    u64 t0 = plat_now_ms();
    FmErr e = s->children(&c, &p.items[0], NULL, &q, NULL);
    show_page("archive.org old-time radio tracks", e, &q, t0);
    TEST_CHECK(e == FM_OK && q.count > 0);
    asrc_page_free(&q);
  }
  asrc_page_free(&p);

  /* Podcasts */
  s = asrc_find("podcasts");
  if (run_search(s, &c, "history", &p)) {
    for (int k = 0; k < 2 && k < p.count; k++) {
      memset(&q, 0, sizeof q);
      u64 t0 = plat_now_ms();
      FmErr e = s->children(&c, &p.items[k], NULL, &q, NULL);
      char what[300];
      fm_snprintf(what, sizeof what, "episodes of \"%s\"", p.items[k].title);
      show_page(what, e, &q, t0);
      TEST_CHECK(e == FM_OK && q.count > 0);
      if (q.count) printf("    newest %s, oldest kept %s\n", q.items[0].published, q.items[q.count - 1].published);
      asrc_page_free(&q);
    }
  }
  asrc_page_free(&p);
  if (run_browse(s, &c, "top", &p)) {                /* chart entries: lookup, then the feed */
    memset(&q, 0, sizeof q);
    u64 t0 = plat_now_ms();
    FmErr e = s->children(&c, &p.items[0], NULL, &q, NULL);
    show_page("episodes of the top show", e, &q, t0);
    TEST_CHECK(e == FM_OK && q.count > 0);
    asrc_page_free(&q);
  }
  asrc_page_free(&p);
  run_browse(s, &c, "genre:1487", &p);
  asrc_page_free(&p);

  /* a download: the shortest Audius track found, twice (the second is "(2)") */
  if (shortest.id[0]) {
    char out[FM_PATH_MAX], out2[FM_PATH_MAX], err[256];
    u64 t0 = plat_now_ms();
    FmErr e = asrc_download(&c, &shortest, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL);
    FmStat st;
    memset(&st, 0, sizeof st);
    if (e == FM_OK) plat_stat(out, &st);
    printf("  download \"%s\" (%.0f s): %s %s, %llu bytes (%d ms)\n", shortest.title, shortest.duration, fm_err_str(e),
           e == FM_OK ? fm_path_base(out) : err, (unsigned long long)st.size, (int)(plat_now_ms() - t0));
    TEST_CHECK(e == FM_OK && st.size > 10000 && fm_ends_with_i(out, ".mp3"));
    e = asrc_download(&c, &shortest, NULL, out2, sizeof out2, NULL, NULL, err, sizeof err, NULL);
    TEST_CHECK(e == FM_OK && strcmp(out, out2) && strstr(out2, " (2).mp3"));
    if (asrc_wants_credit(&shortest)) {
      char txt[FM_PATH_MAX], text[2048];
      fm_strlcpy(txt, out, sizeof txt);
      char *dot = strrchr(txt, '.');
      if (dot) fm_strlcpy(dot, ".txt", (size_t)(txt + sizeof txt - dot));
      TEST_CHECK(read_text(txt, text, sizeof text) && strstr(text, "Source: Audius"));
    }
  }

  /* keyed sources: real keys from the environment, else the error paths live */
  static const char *const kEnv[] = { "MMCFM_JAMENDO_KEY", "MMCFM_FREESOUND_KEY" };
  for (int i = 0; i < 2; i++) {
    s = asrc_at(4 + i);
    FmAsrcConf k = c;
    const char *key = getenv(kEnv[i]);
    char *slot = i == 0 ? k.key_jamendo : k.key_freesound;
    size_t cap = i == 0 ? sizeof k.key_jamendo : sizeof k.key_freesound;
    if (key && *key) {
      fm_strlcpy(slot, key, cap);
      if (run_search(s, &k, "rain", &p)) check_stream(&k, &p.items[0]);
      asrc_page_free(&p);
      if (s->browse) { run_browse(s, &k, "week", &p); asrc_page_free(&p); }
      continue;
    }
    fm_strlcpy(slot, "INVALIDKEY0123456789", cap);
    memset(&p, 0, sizeof p);
    u64 t0 = plat_now_ms();
    FmErr e = s->search(&k, "rain", NULL, &p, NULL);
    printf("  %s, bogus key: %s \"%s\" (%d ms)\n", s->name, fm_err_str(e), p.error, (int)(plat_now_ms() - t0));
    if (e != FM_OK && strstr(p.error, "No secure connection")) {
      /* this PC's network re-signs freesound.org with a firewall CA (Sophos, 2026-10-08) */
      printf("    (skipped: HTTPS to %s is intercepted on this network)\n", s->name);
      asrc_page_free(&p);
      continue;
    }
    TEST_CHECK(e == FM_ERR_ACCESS && strstr(p.error, "did not accept the API key"));
    asrc_page_free(&p);
  }
}

int test_asrc(const char *tmp) {
  int before = g_test_fail;
  at_registry();
  at_page();
  at_helpers();
  at_xml();
  at_radio();
  at_audius();
  at_archive();
  at_podcasts();
  at_keyed();
  at_garbage();
  at_streams(tmp);
  at_online(tmp);
  return g_test_fail - before;
}
