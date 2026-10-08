/* ftest_psrc.c -- online photo sources: parsers, helpers, file names, cache,
** fetch, and (opt-in) real searches and image downloads.
**
** Offline by default: every adapter parses canned replies (shaped like the
** real ones probed on 2026-10-08) and truncated or garbage ones.
** MMCFM_NET_TEST=1 adds a real search on every keyless source, fetches a
** thumbnail and a full image from each and decodes them, saves a download
** with its attribution file, and checks the keyed sources' error paths with
** a bogus key. MMCFM_PEXELS_KEY / MMCFM_UNSPLASH_KEY / MMCFM_PIXABAY_KEY run
** those sources for real.
*/
#include "ftest.h"
#include "fpsrc.h"
#include "fpsrc_int.h"
#include "fplat.h"
#include "fdec_img.h"

/* ---- canned replies ---------------------------------------------------------------- */

static const char *kOvSearch =
  "{\"result_count\":240,\"page_count\":12,\"page_size\":20,\"page\":1,\"results\":["
  "{\"id\":\"1c5442f6-6bb6-4ab7-b603-f598e7579dd2\",\"title\":\"Cat Fish 2\","
  "\"foreign_landing_url\":\"https://www.flickr.com/photos/32426194@N00/3481540500\","
  "\"url\":\"https://live.staticflickr.com/3313/3481540500_c846c62863_b.jpg\",\"creator\":\"admiller\","
  "\"license\":\"by\",\"license_version\":\"2.0\",\"provider\":\"flickr\",\"height\":1024,\"width\":716,"
  "\"thumbnail\":\"https://api.openverse.org/v1/images/1c5442f6-6bb6-4ab7-b603-f598e7579dd2/thumb/\"},"
  "{\"id\":\"b9bbd2be-a68e-4d2c-ad85-f43000cb8d55\",\"title\":\"Le chat \\u00e9t\\u00e9\",\"creator\":\"Zo\\u00eb\","
  "\"url\":\"https://example.org/img/cat.webp\",\"license\":\"by-nc-sa\",\"license_version\":\"4.0\","
  "\"height\":768,\"width\":1024,"
  "\"thumbnail\":\"https://api.openverse.org/v1/images/b9bbd2be-a68e-4d2c-ad85-f43000cb8d55/thumb/\"},"
  "{\"id\":\"0000-pd\",\"title\":null,\"url\":\"https://example.org/x.jpg\",\"license\":\"pdm\","
  "\"thumbnail\":\"javascript:alert(1)\"},"
  "{\"id\":\"bad id\",\"url\":\"https://example.org/y.jpg\"},"
  "{\"id\":\"no-urls\",\"url\":\"ftp://x/y\"},"
  "{\"id\":\"cc0-item\",\"url\":\"https://e.org/z.png\",\"license\":\"cc0\",\"license_version\":\"\"}"
  "]}";

static const char *kWmSearch =
  "{\"batchcomplete\":\"\",\"continue\":{\"gsroffset\":30,\"continue\":\"gsroffset||\"},\"query\":{\"pages\":{"
  "\"12127693\":{\"pageid\":12127693,\"ns\":6,\"title\":\"File:Cat November 2010-1a.jpg\",\"index\":3,"
  "\"imageinfo\":[{\"size\":2833605,\"width\":1795,\"height\":2397,"
  "\"thumburl\":\"https://upload.wikimedia.org/wikipedia/commons/thumb/4/4d/Cat_November_2010-1a.jpg/"
  "500px-Cat_November_2010-1a.jpg?utm_source=x\",\"thumbwidth\":480,\"thumbheight\":641,"
  "\"url\":\"https://upload.wikimedia.org/wikipedia/commons/4/4d/Cat_November_2010-1a.jpg\","
  "\"descriptionurl\":\"https://commons.wikimedia.org/wiki/File:Cat_November_2010-1a.jpg\","
  "\"extmetadata\":{\"ObjectName\":{\"value\":\"Cat November 2010-1a\"},"
  "\"Artist\":{\"value\":\"<a href=\\\"//commons.wikimedia.org/wiki/User:Alvesgaspar\\\" "
  "title=\\\"User:Alvesgaspar\\\">Alvesgaspar</a>\"},"
  "\"LicenseShortName\":{\"value\":\"CC BY-SA 3.0\"}},\"mime\":\"image/jpeg\"}]},"
  "\"68960758\":{\"pageid\":68960758,\"ns\":6,\"title\":\"File:Cat playing with a lizard.jpg\",\"index\":1,"
  "\"imageinfo\":[{\"width\":5557,\"height\":3125,"
  "\"thumburl\":\"https://upload.wikimedia.org/wikipedia/commons/thumb/7/72/Cat_playing_with_a_lizard.jpg/"
  "500px-Cat_playing_with_a_lizard.jpg\",\"thumbwidth\":480,\"thumbheight\":270,"
  "\"url\":\"https://upload.wikimedia.org/wikipedia/commons/7/72/Cat_playing_with_a_lizard.jpg\","
  "\"descriptionurl\":\"https://commons.wikimedia.org/wiki/File:Cat_playing_with_a_lizard.jpg\","
  "\"extmetadata\":{\"Artist\":{\"value\":\"<b>J&amp;J</b>  <i>Smith</i>&nbsp;Co\"},"
  "\"LicenseShortName\":{\"value\":\"Public domain\"}},\"mime\":\"image/jpeg\"}]},"
  "\"5\":{\"pageid\":5,\"ns\":6,\"title\":\"File:Old map.tif\",\"index\":2,"
  "\"imageinfo\":[{\"width\":1500,\"height\":1000,"
  "\"thumburl\":\"https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/Old_map.tif/"
  "lossy-page1-500px-Old_map.tif.jpg\",\"thumbwidth\":480,\"thumbheight\":320,"
  "\"url\":\"https://upload.wikimedia.org/wikipedia/commons/a/ab/Old_map.tif\",\"mime\":\"image/tiff\"}]},"
  "\"6\":{\"pageid\":6,\"ns\":6,\"title\":\"File:Cat.pdf\",\"index\":4,\"imageinfo\":[{\"mime\":\"application/pdf\","
  "\"thumburl\":\"https://upload.wikimedia.org/x/page1-500px-Cat.pdf.jpg\"}]},"
  "\"7\":{\"pageid\":7,\"ns\":6,\"title\":\"File:Cat.webp\",\"index\":5,\"imageinfo\":[{\"mime\":\"image/webp\","
  "\"width\":800,\"height\":600,\"thumburl\":\"https://upload.wikimedia.org/x/500px-Cat.webp\"}]}"
  "}}}";

static const char *kNasaSearch =
  "{\"collection\":{\"version\":\"1.1\",\"items\":["
  "{\"href\":\"https://images-assets.nasa.gov/image/PIA12235/collection.json\",\"data\":[{\"center\":\"JPL\","
  "\"media_type\":\"image\",\"nasa_id\":\"PIA12235\",\"secondary_creator\":\"ISRO/NASA/JPL-Caltech\","
  "\"title\":\"Nearside of the Moon\"}],\"links\":["
  "{\"href\":\"https://images-assets.nasa.gov/image/PIA12235/PIA12235~medium.jpg\",\"rel\":\"alternate\","
  "\"width\":1280,\"height\":896},"
  "{\"href\":\"https://images-assets.nasa.gov/image/PIA12235/PIA12235~thumb.jpg\",\"rel\":\"preview\","
  "\"width\":640,\"height\":448},"
  "{\"href\":\"https://images-assets.nasa.gov/image/PIA12235/PIA12235~large.jpg\",\"rel\":\"alternate\","
  "\"width\":1920,\"height\":1344},"
  "{\"href\":\"https://images-assets.nasa.gov/image/PIA12235/PIA12235~orig.tif\",\"rel\":\"canonical\","
  "\"width\":5000,\"height\":3500}]},"
  "{\"data\":[{\"center\":\"HQ\",\"media_type\":\"image\",\"nasa_id\":\"Moon to Mars Science\","
  "\"title\":\"Moon to Mars Science\"}],\"links\":["
  "{\"href\":\"https://images-assets.nasa.gov/image/Moon to Mars Science/Moon to Mars Science~thumb.jpg\","
  "\"rel\":\"preview\"}]},"
  "{\"data\":[{\"media_type\":\"video\",\"nasa_id\":\"vid1\",\"title\":\"v\"}],\"links\":[]},"
  "{\"data\":[{\"media_type\":\"image\",\"nasa_id\":\"../evil\",\"title\":\"x\"}],\"links\":[]}"
  "],\"metadata\":{\"total_hits\":1522},\"links\":[{\"rel\":\"next\",\"prompt\":\"Next\","
  "\"href\":\"http://images-api.nasa.gov/search?q=moon&page=2\"}]}}";

static const char *kNasaAssets =
  "[\"http://images-assets.nasa.gov/image/Moon to Mars Science/Moon to Mars Science~orig.jpg\","
  "\"http://images-assets.nasa.gov/image/Moon to Mars Science/Moon to Mars Science~medium.jpg\","
  "\"http://images-assets.nasa.gov/image/Moon to Mars Science/Moon to Mars Science~thumb.jpg\","
  "\"http://images-assets.nasa.gov/image/Moon to Mars Science/metadata.json\"]";

static const char *kArticSearch =
  "{\"preference\":null,\"pagination\":{\"total\":133120,\"limit\":30,\"offset\":0,\"total_pages\":4438,"
  "\"current_page\":1},\"data\":["
  "{\"_score\":91.8,\"id\":656,\"title\":\"Lion (One of a Pair, South Pedestal)\",\"thumbnail\":{\"lqip\":\"x\","
  "\"width\":8430,\"height\":5620,\"alt_text\":\"A bronze lion\"},\"artist_title\":\"Edward Kemeys\","
  "\"image_id\":\"6b1edb9c-0f3f-0ee3-47c7-ca25c39ee360\",\"is_public_domain\":false},"
  "{\"id\":119335,\"title\":\"Baroque Pearl\",\"thumbnail\":{\"width\":600,\"height\":400},"
  "\"is_public_domain\":true,\"image_id\":\"fe394433-14ae-89e0-136f-31cbdb390771\",\"artist_title\":null},"
  "{\"id\":5,\"title\":\"No image\",\"image_id\":null},"
  "{\"id\":6,\"title\":\"Bad image id\",\"image_id\":\"../../x\"}],"
  "\"config\":{\"iiif_url\":\"https:\\/\\/www.artic.edu\\/iiif\\/2\",\"website_url\":\"http:\\/\\/www.artic.edu\"}}";

static const char *kPexelsSearch =
  "{\"page\":1,\"per_page\":30,\"photos\":[{\"id\":2014422,\"width\":3024,\"height\":3024,"
  "\"url\":\"https://www.pexels.com/photo/brown-rocks-2014422/\",\"photographer\":\"Joey Farina\","
  "\"avg_color\":\"#978E82\",\"src\":{\"original\":\"https://images.pexels.com/photos/2014422/"
  "pexels-photo-2014422.jpeg\",\"large2x\":\"https://images.pexels.com/photos/2014422/"
  "pexels-photo-2014422.jpeg?auto=compress&cs=tinysrgb&dpr=2&h=650&w=940\","
  "\"medium\":\"https://images.pexels.com/photos/2014422/pexels-photo-2014422.jpeg?auto=compress&cs=tinysrgb&h=350\"},"
  "\"alt\":\"Brown Rocks During Golden Hour\"},"
  "{\"id\":-5,\"src\":{\"medium\":\"https://x/y.jpg\"}},{\"id\":7,\"src\":{}}],"
  "\"total_results\":8000,\"next_page\":\"https://api.pexels.com/v1/search/?page=2&per_page=30&query=nature\"}";

static const char *kPexelsError = "{\"status\":401,\"code\":\"Unauthorized\",\"message\":\"Missing API key\"}";

static const char *kUnsplashSearch =
  "{\"total\":10000,\"total_pages\":334,\"results\":[{\"id\":\"eOLpJytrbsQ\",\"width\":4000,\"height\":3000,"
  "\"color\":\"#A7A2A1\",\"description\":null,\"alt_description\":\"woman in black top\","
  "\"urls\":{\"raw\":\"https://images.unsplash.com/photo-1416339306562?ixid=abc\","
  "\"full\":\"https://images.unsplash.com/photo-1416339306562?ixid=abc&fm=jpg&q=85\","
  "\"regular\":\"https://images.unsplash.com/photo-1416339306562?ixid=abc&w=1080\","
  "\"small\":\"https://images.unsplash.com/photo-1416339306562?ixid=abc&w=400\"},"
  "\"links\":{\"html\":\"https://unsplash.com/photos/eOLpJytrbsQ\","
  "\"download_location\":\"https://api.unsplash.com/photos/eOLpJytrbsQ/download?ixid=abc\"},"
  "\"user\":{\"name\":\"Jeff Sheldon\"}},"
  "{\"id\":\"bad/id\",\"urls\":{\"small\":\"https://x/y\"}}]}";

static const char *kUnsplashList =
  "[{\"id\":\"abc_DEF-1\",\"width\":1000,\"height\":2000,\"color\":\"#0c2626\",\"description\":\"A tall one\","
  "\"urls\":{\"small\":\"https://images.unsplash.com/p2?w=400\",\"regular\":\"https://images.unsplash.com/p2?w=1080\"},"
  "\"links\":{\"html\":\"https://unsplash.com/photos/abc_DEF-1\"},\"user\":{\"name\":\"N\"}}]";

static const char *kUnsplashError = "{\"errors\":[\"OAuth error: The access token is invalid\"]}";

static const char *kPixabaySearch =
  "{\"total\":4692,\"totalHits\":500,\"hits\":[{\"id\":195893,"
  "\"pageURL\":\"https://pixabay.com/en/blossom-bloom-flower-195893/\",\"type\":\"photo\","
  "\"tags\":\"blossom, bloom, flower\",\"previewURL\":\"https://cdn.pixabay.com/photo/2013/10/15/09/12/"
  "flower-195893_150.jpg\",\"webformatURL\":\"https://pixabay.com/get/35bbf209e13e39d2_640.jpg\","
  "\"largeImageURL\":\"https://pixabay.com/get/ed6a99fd0a76647_1280.jpg\",\"imageWidth\":4000,"
  "\"imageHeight\":2250,\"user\":\"Josch13\"},{\"id\":0,\"webformatURL\":\"https://x/y.jpg\"}]}";

static const char *kPixabayKeyError = "[ERROR 400] Invalid or missing API key (https://pixabay.com/api/docs/).";

/* ---- offline ---------------------------------------------------------------------- */

static void pt_registry(void) {
  static const char *const kKeys[] = { "openverse", "wikimedia", "nasa", "artic", "pexels", "unsplash",
                                       "pixabay" };
  TEST_CHECK(psrc_count() == 7);
  for (int i = 0; i < psrc_count() && i < 7; i++) {
    const FmPsrc *s = psrc_at(i);
    TEST_CHECK(s && !strcmp(s->key, kKeys[i]));
    TEST_CHECK(s && s->name && s->about && s->search && (s->flags & PSRC_SEARCH));
    TEST_CHECK(s && psrc_find(s->key) == s);
    TEST_CHECK(s && strlen(s->key) < sizeof ((FmPsrcItem *)0)->source);
    TEST_CHECK(s && ((s->flags & PSRC_NEEDKEY) != 0) == (i >= 4));
    TEST_CHECK(s && !(s->flags & PSRC_SIGNIN));
  }
  TEST_CHECK(psrc_at(-1) == NULL && psrc_at(7) == NULL);
  TEST_CHECK(psrc_find("nope") == NULL && psrc_find(NULL) == NULL);
  TEST_CHECK(psrc_find("artic")->img_headers && strstr(psrc_find("artic")->img_headers, "AIC-User-Agent:"));
  TEST_CHECK(psrc_find("unsplash")->saved != NULL && psrc_find("pexels")->saved == NULL);
  TEST_CHECK(psrc_find("nasa")->details != NULL);
  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  TEST_CHECK(psrc_item_headers(&it) == NULL && psrc_item_headers(NULL) == NULL);
  fm_strlcpy(it.source, "artic", sizeof it.source);
  TEST_CHECK(psrc_item_headers(&it) && strstr(psrc_item_headers(&it), "\r\n"));
  fm_strlcpy(it.source, "pexels", sizeof it.source);
  TEST_CHECK(psrc_item_headers(&it) == NULL);
}

static void pt_page(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  FmPsrcItem *first = psrc_page_add(&p);
  for (int i = 1; i < PSRC_MAX_ITEMS; i++) fm_snprintf(psrc_page_add(&p)->id, 8, "%d", i);
  /* a full page is still the first (and only) allocation */
  TEST_CHECK(p.count == PSRC_MAX_ITEMS && p.cap == PSRC_MAX_ITEMS && first == &p.items[0]);
  TEST_CHECK(!strcmp(p.items[PSRC_MAX_ITEMS - 1].id, "29") || PSRC_MAX_ITEMS != 30);
  FmPsrcItem *x = psrc_page_add(&p);            /* beyond the cap still works (grows) */
  TEST_CHECK(x && x->id[0] == 0 && x->width == 0 && p.count == PSRC_MAX_ITEMS + 1);
  psrc_page_free(&p);
  TEST_CHECK(p.items == NULL && p.count == 0 && p.cap == 0);
  psrc_page_free(&p);
  psrc_page_free(NULL);
}

static void pt_text(void) {
  char out[256];
  psrc_html_text("<a href=\"//commons.wikimedia.org/wiki/User:X\" title=\"User:X\">Alves&#233;gaspar</a>", out,
                 sizeof out);
  TEST_CHECK(!strcmp(out, "Alves\xC3\xA9gaspar"));
  psrc_html_text("  <b>J&amp;J</b>\n\t<i>Smith</i>&nbsp;Co <br/>Ltd  ", out, sizeof out);
  TEST_CHECK(!strcmp(out, "J&J Smith Co Ltd"));
  psrc_html_text("<span class=\"x\">Unknown author</span><div>Line2</div>", out, sizeof out);
  TEST_CHECK(!strcmp(out, "Unknown author Line2"));
  psrc_html_text("broken <a href=", out, sizeof out);
  TEST_CHECK(!strcmp(out, "broken"));
  psrc_html_text("\xE7\x8C\xAB\xE7\x8C\xAB\xE7\x8C\xAB", out, 8);          /* cut inside a character */
  TEST_CHECK(!strcmp(out, "\xE7\x8C\xAB\xE7\x8C\xAB"));
  psrc_html_text(NULL, out, sizeof out);
  TEST_CHECK(out[0] == 0);
  psrc_html_text("<p></p>", out, sizeof out);
  TEST_CHECK(out[0] == 0);

  psrc_copy(out, "\xC3\xA9\xC3\xA9", 4);
  TEST_CHECK(!strcmp(out, "\xC3\xA9"));
  TEST_CHECK(psrc_hex_color("#978E82") == 0x978E82 && psrc_hex_color("#0c2626") == 0x0C2626);
  TEST_CHECK(psrc_hex_color("978E82") == 0 && psrc_hex_color("#12345") == 0 && psrc_hex_color("#GG0000") == 0 &&
             psrc_hex_color(NULL) == 0);
  int w = 4000, h = 3000;
  psrc_fit_width(&w, &h, 1920);
  TEST_CHECK(w == 1920 && h == 1440);
  w = 800; h = 600;
  psrc_fit_width(&w, &h, 1920);
  TEST_CHECK(w == 800 && h == 600);
  w = 0; h = 600;
  psrc_fit_width(&w, &h, 1920);
  TEST_CHECK(w == 0 && h == 0);
  TEST_CHECK(psrc_page_num(NULL, 1, 1, 12) == 1 && psrc_page_num("", 0, 0, 9) == 0);
  TEST_CHECK(psrc_page_num("5", 1, 1, 12) == 5 && psrc_page_num("99", 1, 1, 12) == 12);
  TEST_CHECK(psrc_page_num("0", 1, 1, 12) == 1 && psrc_page_num("x1", 1, 1, 12) == 1);
  TEST_CHECK(psrc_page_num("99999999999999999999", 1, 1, 12) == 12);
  TEST_CHECK(psrc_id_ok("a-b", "-", 8) && !psrc_id_ok("a b", "-", 8) && !psrc_id_ok("", NULL, 8) &&
             !psrc_id_ok("abcdefghi", NULL, 8));
}

static void pt_url_helpers(void) {
  char u[2048];
  TEST_CHECK(psrc_url_copy("https://a.b/Moon to Mars/x~thumb.jpg", u, sizeof u) &&
             !strcmp(u, "https://a.b/Moon%20to%20Mars/x~thumb.jpg"));
  TEST_CHECK(psrc_url_copy("https://a.b/caf\xC3\xA9.jpg?x=1&y=%20", u, sizeof u) &&
             !strcmp(u, "https://a.b/caf%C3%A9.jpg?x=1&y=%20"));
  TEST_CHECK(!psrc_url_copy("javascript:alert(1)", u, sizeof u) && u[0] == 0);
  TEST_CHECK(!psrc_url_copy("https://a.b/\"><script>", u, sizeof u) && u[0] == 0);
  TEST_CHECK(!psrc_url_copy("https://a.b/x\r\nHost: evil", u, sizeof u));
  TEST_CHECK(!psrc_url_copy("ftp://a.b/x", u, sizeof u) && !psrc_url_copy(NULL, u, sizeof u));
  TEST_CHECK(!psrc_url_copy("https://a.b/0123456789", u, 12));       /* does not fit: nothing */
  TEST_CHECK(psrc_url_copy("http://a.b/c", u, sizeof u) && !strcmp(u, "http://a.b/c"));

  char e[16];
  psrc_url_ext("https://x/a/B.JPG?w=1#f", e, sizeof e);
  TEST_CHECK(!strcmp(e, ".jpg"));
  psrc_url_ext("https://x/a.b/thumb/", e, sizeof e);
  TEST_CHECK(e[0] == 0);
  psrc_url_ext("https://x/photo.jpeg?auto=compress", e, sizeof e);
  TEST_CHECK(!strcmp(e, ".jpeg"));
  psrc_url_ext("https://x/a.longext", e, sizeof e);
  TEST_CHECK(e[0] == 0);
  TEST_CHECK(psrc_url_viewable("https://x/a.jpg") && psrc_url_viewable("https://x/a.PNG?x") &&
             psrc_url_viewable("https://x/thumb/") && psrc_url_viewable("https://x/p?fm=jpg"));
  TEST_CHECK(!psrc_url_viewable("https://x/a.webp") && !psrc_url_viewable("https://x/a.tif") &&
             !psrc_url_viewable("https://x/a.TIFF?x=1") && !psrc_url_viewable("https://x/a.svg"));

  TEST_CHECK(psrc_wikimedia_resize("https://u.org/thumb/4/4d/C.jpg/500px-C.jpg?utm=1", 1920, u, sizeof u) &&
             !strcmp(u, "https://u.org/thumb/4/4d/C.jpg/1920px-C.jpg?utm=1"));
  TEST_CHECK(psrc_wikimedia_resize("https://u.org/thumb/a/ab/M.tif/lossy-page1-500px-M.tif.jpg", 1280, u,
                                   sizeof u) &&
             !strcmp(u, "https://u.org/thumb/a/ab/M.tif/lossy-page1-1280px-M.tif.jpg"));
  TEST_CHECK(!psrc_wikimedia_resize("https://u.org/a/ab/C.jpg", 1920, u, sizeof u) && u[0] == 0);
  TEST_CHECK(!psrc_wikimedia_resize("https://u.org/a/px-C.jpg", 1920, u, sizeof u));
  TEST_CHECK(!psrc_wikimedia_resize("https://u.org/a/ab500px-C.jpg", 1920, u, sizeof u));
  TEST_CHECK(!psrc_wikimedia_resize("https://u.org/a/C.jpg?q=500px-x", 1920, u, sizeof u));

  TEST_CHECK(psrc_flickr_thumb("https://live.staticflickr.com/3313/3481540500_c846c62863_b.jpg", u, sizeof u) &&
             !strcmp(u, "https://live.staticflickr.com/3313/3481540500_c846c62863_w.jpg"));
  TEST_CHECK(psrc_flickr_thumb("https://farm4.staticflickr.com/1262/1484248550_8cce4fc8a4.jpg", u, sizeof u) &&
             !strcmp(u, "https://farm4.staticflickr.com/1262/1484248550_8cce4fc8a4_w.jpg"));
  TEST_CHECK(!psrc_flickr_thumb("https://live.staticflickr.com/1/2_abc_o.png", u, sizeof u) && u[0] == 0);
  TEST_CHECK(!psrc_flickr_thumb("https://evil.com/x.staticflickr.com/1/2_ab.jpg", u, sizeof u));
  TEST_CHECK(!psrc_flickr_thumb("https://staticflickr.com.evil/1/2_ab.jpg", u, sizeof u));
  TEST_CHECK(!psrc_flickr_thumb("https://live.staticflickr.com/1/2_ab_b.jpg?x=1", u, sizeof u));
  TEST_CHECK(!psrc_flickr_thumb("https://live.staticflickr.com/1/name_b.jpg", u, sizeof u));
  TEST_CHECK(!psrc_flickr_thumb("https://live.staticflickr.com/1/2_ab_big.jpg", u, sizeof u));
}

static void pt_urls(void) {
  FmPsrcConf c;
  memset(&c, 0, sizeof c);
  char u[2048];
  const char *q = "a&b c/\xC3\xBC";
  const char *qe = "a%26b%20c%2F%C3%BC";

  psrc_openverse_url(&c, q, 3, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.openverse.org/v1/images/?q=") == u && strstr(u, qe) &&
             strstr(u, "page_size=20") && strstr(u, "&page=3") && !strstr(u, "mature"));
  c.safe_search = true;
  psrc_openverse_url(&c, q, 99, u, sizeof u);
  TEST_CHECK(strstr(u, "&page=12") && strstr(u, "&mature=false"));

  psrc_wikimedia_url(q, 60, u, sizeof u);
  TEST_CHECK(strstr(u, "https://commons.wikimedia.org/w/api.php?") == u && strstr(u, "gsrsearch=a%26b%20c%2F%C3%BC"
             "%20filetype%3Abitmap") && strstr(u, "gsroffset=60") && strstr(u, "gsrnamespace=6") &&
             strstr(u, "iiurlwidth=480") && strstr(u, "gsrlimit=30"));

  psrc_nasa_url(q, 2, u, sizeof u);
  TEST_CHECK(strstr(u, "https://images-api.nasa.gov/search?q=") == u && strstr(u, qe) &&
             strstr(u, "media_type=image") && strstr(u, "page=2") && strstr(u, "page_size=30"));

  psrc_artic_url(q, 50, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.artic.edu/api/v1/artworks/search?q=") == u && strstr(u, qe) &&
             strstr(u, "page=33") && strstr(u, "fields=id,title,artist_title,image_id,thumbnail,is_public_domain"));

  psrc_pexels_url(q, 2, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.pexels.com/v1/search?query=") == u && strstr(u, qe) && strstr(u, "page=2"));
  psrc_pexels_url("", 1, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.pexels.com/v1/curated?") == u);

  psrc_unsplash_url(&c, q, 1, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.unsplash.com/search/photos?query=") == u && strstr(u, qe) &&
             strstr(u, "content_filter=high"));
  psrc_unsplash_url(&c, NULL, 4, u, sizeof u);
  TEST_CHECK(strstr(u, "https://api.unsplash.com/photos?") == u && strstr(u, "page=4"));

  fm_strlcpy(c.key_pixabay, "12-ab&cd", sizeof c.key_pixabay);
  psrc_pixabay_url(&c, q, 1, u, sizeof u);
  TEST_CHECK(strstr(u, "https://pixabay.com/api/?key=12-ab%26cd&q=") == u && strstr(u, qe) &&
             strstr(u, "safesearch=true") && strstr(u, "image_type=photo"));
  c.safe_search = false;
  psrc_pixabay_url(&c, "", 99, u, sizeof u);
  TEST_CHECK(strstr(u, "safesearch=false") && strstr(u, "&q=&") && strstr(u, "page=17"));

  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.id, "eOLpJytrbsQ", sizeof it.id);
  TEST_CHECK(psrc_unsplash_track_url(&it, u, sizeof u) &&
             !strcmp(u, "https://api.unsplash.com/photos/eOLpJytrbsQ/download"));
  fm_strlcpy(it.id, "../x", sizeof it.id);
  TEST_CHECK(!psrc_unsplash_track_url(&it, u, sizeof u) && u[0] == 0);
}

static void pt_openverse(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_openverse_parse(kOvSearch, strlen(kOvSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 4 && !strcmp(p.next, "2") && p.cap == PSRC_MAX_ITEMS);
  if (p.count == 4) {
    FmPsrcItem *a = &p.items[0], *b = &p.items[1], *c = &p.items[2], *d = &p.items[3];
    TEST_CHECK(!strcmp(a->source, "openverse") && !strcmp(a->title, "Cat Fish 2") && !strcmp(a->author, "admiller"));
    TEST_CHECK(!strcmp(a->license, "CC BY 2.0") && a->width == 716 && a->height == 1024);
    TEST_CHECK(!strcmp(a->thumb, "https://live.staticflickr.com/3313/3481540500_c846c62863_w.jpg"));
    TEST_CHECK(strstr(a->full, "_b.jpg") && !a->original[0]);
    TEST_CHECK(!strcmp(a->page, "https://www.flickr.com/photos/32426194@N00/3481540500"));
    /* a WebP original: the viewer gets the proxy thumbnail, Download the original */
    TEST_CHECK(!strcmp(b->license, "CC BY-NC-SA 4.0") && !strcmp(b->author, "Zo\xC3\xAB"));
    TEST_CHECK(strstr(b->full, "/thumb/") && !strcmp(b->thumb, b->full) && strstr(b->original, "cat.webp") &&
               b->width == 0);
    TEST_CHECK(!strcmp(b->title, "Le chat \xC3\xA9t\xC3\xA9"));
    TEST_CHECK(!strcmp(c->id, "0000-pd") && !strcmp(c->license, "Public domain mark") && c->title[0] == 0);
    TEST_CHECK(!strcmp(c->thumb, "https://example.org/x.jpg") && strstr(c->page, "openverse.org/image/0000-pd"));
    TEST_CHECK(!strcmp(d->license, "CC0 1.0"));
  }
  psrc_page_free(&p);
  /* the last page has no next token */
  TEST_CHECK(psrc_openverse_parse(kOvSearch, strlen(kOvSearch), 12, &p) == FM_OK && !p.next[0]);
  psrc_page_free(&p);
  const char *deep = "{\"detail\":\"pagination depth may not exceed 240 for anonymous requests\"}";
  TEST_CHECK(psrc_openverse_parse(deep, strlen(deep), 13, &p) == FM_ERR_FORMAT && strstr(p.error, "240"));
  psrc_page_free(&p);
}

static void pt_wikimedia(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_wikimedia_parse(kWmSearch, strlen(kWmSearch), &p) == FM_OK);
  TEST_CHECK(p.count == 3 && !strcmp(p.next, "30"));
  if (p.count == 3) {
    FmPsrcItem *a = &p.items[0], *b = &p.items[1], *c = &p.items[2];
    /* search order (index), not page-id order */
    TEST_CHECK(!strcmp(a->id, "68960758") && !strcmp(b->id, "5") && !strcmp(c->id, "12127693"));
    /* 5557 px wide: full is the 1920 px rendering, Download the original */
    TEST_CHECK(strstr(a->full, "/1920px-Cat_playing") && a->width == 1920 && a->height == 1079);
    TEST_CHECK(strstr(a->original, "/commons/7/72/Cat_playing_with_a_lizard.jpg"));
    TEST_CHECK(!strcmp(a->author, "J&J Smith Co") && !strcmp(a->license, "Public domain"));
    TEST_CHECK(!strcmp(a->title, "Cat playing with a lizard") && !strcmp(a->source, "wikimedia"));
    /* TIFF: a JPEG rendering at the largest standard width below 1500 */
    TEST_CHECK(strstr(b->full, "lossy-page1-1280px-Old_map.tif.jpg") && strstr(b->original, "Old_map.tif") &&
               b->width == 1280 && b->height == 853);
    /* a small JPEG: the original itself */
    TEST_CHECK(!strcmp(c->full, "https://upload.wikimedia.org/wikipedia/commons/4/4d/Cat_November_2010-1a.jpg") &&
               !c->original[0] && c->width == 1795 && c->height == 2397);
    TEST_CHECK(!strcmp(c->author, "Alvesgaspar") && !strcmp(c->license, "CC BY-SA 3.0") &&
               !strcmp(c->title, "Cat November 2010-1a"));
    TEST_CHECK(!strcmp(c->page, "https://commons.wikimedia.org/wiki/File:Cat_November_2010-1a.jpg"));
  }
  psrc_page_free(&p);
  const char *none = "{\"batchcomplete\":\"\"}";
  TEST_CHECK(psrc_wikimedia_parse(none, strlen(none), &p) == FM_OK && p.count == 0 && !p.next[0]);
  psrc_page_free(&p);
  const char *err = "{\"error\":{\"code\":\"badvalue\",\"info\":\"Unrecognized value for parameter\"}}";
  TEST_CHECK(psrc_wikimedia_parse(err, strlen(err), &p) == FM_ERR_FORMAT && strstr(p.error, "Unrecognized"));
  psrc_page_free(&p);
}

static void pt_nasa(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_nasa_parse(kNasaSearch, strlen(kNasaSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 2 && !strcmp(p.next, "2"));
  if (p.count == 2) {
    FmPsrcItem *a = &p.items[0], *b = &p.items[1];
    TEST_CHECK(!strcmp(a->id, "PIA12235") && !strcmp(a->title, "Nearside of the Moon"));
    TEST_CHECK(strstr(a->thumb, "~thumb.jpg") && strstr(a->full, "~large.jpg") && strstr(a->original, "~orig.tif"));
    TEST_CHECK(a->width == 1920 && a->height == 1344 && !strcmp(a->author, "ISRO/NASA/JPL-Caltech"));
    TEST_CHECK(strstr(a->license, "public domain") && !strcmp(a->page, "https://images.nasa.gov/details/PIA12235"));
    /* only a thumbnail, blanks in the name: encoded; details() fills the rest */
    TEST_CHECK(strstr(b->thumb, "Moon%20to%20Mars%20Science~thumb.jpg") && !strcmp(b->full, b->thumb));
    TEST_CHECK(!b->original[0] && b->width == 0 && !strcmp(b->author, "NASA HQ"));
    TEST_CHECK(!strcmp(b->page, "https://images.nasa.gov/details/Moon%20to%20Mars%20Science"));
    TEST_CHECK(psrc_nasa_parse_assets(kNasaAssets, strlen(kNasaAssets), b) == FM_OK);
    TEST_CHECK(!strcmp(b->full, "https://images-assets.nasa.gov/image/Moon%20to%20Mars%20Science/"
                                "Moon%20to%20Mars%20Science~medium.jpg"));
    TEST_CHECK(strstr(b->original, "~orig.jpg") && !strncmp(b->original, "https://", 8));
  }
  psrc_page_free(&p);
  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  TEST_CHECK(psrc_nasa_parse_assets("[\"https://x/a~thumb.jpg\"]", 25, &it) == FM_ERR_NOT_FOUND);
  TEST_CHECK(psrc_nasa_parse_assets("{}", 2, &it) == FM_ERR_FORMAT);
  const char *none = "{\"collection\":{\"items\":[],\"metadata\":{\"total_hits\":0}}}";
  TEST_CHECK(psrc_nasa_parse(none, strlen(none), 1, &p) == FM_OK && p.count == 0 && !p.next[0]);
  psrc_page_free(&p);
  const char *err = "{\"reason\":\"page_size must be less than 100\"}";
  TEST_CHECK(psrc_nasa_parse(err, strlen(err), 1, &p) == FM_ERR_FORMAT && strstr(p.error, "page_size"));
  psrc_page_free(&p);
}

static void pt_artic(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_artic_parse(kArticSearch, strlen(kArticSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 2 && !strcmp(p.next, "2"));
  if (p.count == 2) {
    FmPsrcItem *a = &p.items[0], *b = &p.items[1];
    TEST_CHECK(!strcmp(a->id, "656") && !strcmp(a->author, "Edward Kemeys") && !strcmp(a->source, "artic"));
    TEST_CHECK(!strcmp(a->thumb, "https://www.artic.edu/iiif/2/6b1edb9c-0f3f-0ee3-47c7-ca25c39ee360/full/400,/0/"
                                 "default.jpg"));
    /* in copyright: 843 px; public domain: up to 1686, never above the original */
    TEST_CHECK(strstr(a->full, "/full/843,/0/default.jpg") && a->width == 843 && a->height == 562);
    TEST_CHECK(strstr(a->license, "artic.edu") && !strcmp(a->page, "https://www.artic.edu/artworks/656"));
    TEST_CHECK(strstr(b->full, "/full/600,/0/") && strstr(b->thumb, "/full/400,/") && b->width == 600 &&
               b->height == 400);
    TEST_CHECK(!strcmp(b->license, "Public domain") && b->author[0] == 0);
  }
  psrc_page_free(&p);
  TEST_CHECK(psrc_artic_parse(kArticSearch, strlen(kArticSearch), 33, &p) == FM_OK && !p.next[0]);
  psrc_page_free(&p);
  const char *deep = "{\"status\":403,\"error\":\"Invalid number of results\",\"detail\":\"You have requested too "
                     "many results.\"}";
  TEST_CHECK(psrc_artic_parse(deep, strlen(deep), 34, &p) == FM_ERR_FORMAT && strstr(p.error, "too many"));
  psrc_page_free(&p);
}

static void pt_keyed(void) {
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_pexels_parse(kPexelsSearch, strlen(kPexelsSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 1 && !strcmp(p.next, "2"));
  if (p.count == 1) {
    FmPsrcItem *a = &p.items[0];
    TEST_CHECK(!strcmp(a->id, "2014422") && !strcmp(a->author, "Joey Farina") && a->color == 0x978E82);
    TEST_CHECK(strstr(a->thumb, "h=350") && strstr(a->full, "dpr=2") && strstr(a->original, "2014422.jpeg") &&
               !strchr(a->original, '?'));
    TEST_CHECK(a->width == 1880 && a->height == 1880 && !strcmp(a->license, "Pexels License"));
    TEST_CHECK(!strcmp(a->title, "Brown Rocks During Golden Hour") && strstr(a->page, "pexels.com/photo/"));
  }
  psrc_page_free(&p);
  TEST_CHECK(psrc_pexels_parse(kPexelsError, strlen(kPexelsError), 1, &p) == FM_ERR_FORMAT &&
             strstr(p.error, "Missing API key"));
  psrc_page_free(&p);

  TEST_CHECK(psrc_unsplash_parse(kUnsplashSearch, strlen(kUnsplashSearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 1 && !strcmp(p.next, "2"));
  if (p.count == 1) {
    FmPsrcItem *a = &p.items[0];
    TEST_CHECK(!strcmp(a->id, "eOLpJytrbsQ") && !strcmp(a->author, "Jeff Sheldon") && a->color == 0xA7A2A1);
    TEST_CHECK(!strcmp(a->title, "Woman in black top") && strstr(a->thumb, "w=400"));
    TEST_CHECK(!strcmp(a->full, "https://images.unsplash.com/photo-1416339306562?ixid=abc&w=1920&fit=max&fm=jpg&q=85"));
    TEST_CHECK(strstr(a->original, "fm=jpg&q=85") && a->width == 1920 && a->height == 1440);
    TEST_CHECK(!strcmp(a->page, "https://unsplash.com/photos/eOLpJytrbsQ?utm_source=mmcfm&utm_medium=referral"));
  }
  psrc_page_free(&p);
  TEST_CHECK(psrc_unsplash_parse(kUnsplashList, strlen(kUnsplashList), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 1 && !p.next[0]);       /* a short browse page is the last */
  if (p.count == 1)
    TEST_CHECK(!strcmp(p.items[0].full, "https://images.unsplash.com/p2?w=1080") && p.items[0].width == 1000 &&
               !strcmp(p.items[0].title, "A tall one"));
  psrc_page_free(&p);
  TEST_CHECK(psrc_unsplash_parse(kUnsplashError, strlen(kUnsplashError), 1, &p) == FM_ERR_FORMAT &&
             strstr(p.error, "access token is invalid"));
  psrc_page_free(&p);

  TEST_CHECK(psrc_pixabay_parse(kPixabaySearch, strlen(kPixabaySearch), 1, &p) == FM_OK);
  TEST_CHECK(p.count == 1 && !strcmp(p.next, "2"));
  if (p.count == 1) {
    FmPsrcItem *a = &p.items[0];
    TEST_CHECK(!strcmp(a->title, "Blossom, bloom, flower") && !strcmp(a->author, "Josch13"));
    TEST_CHECK(strstr(a->thumb, "_640.jpg") && strstr(a->full, "_1280.jpg") && a->width == 1280 && a->height == 720);
    TEST_CHECK(strstr(a->page, "195893") && strstr(a->license, "Pixabay"));
  }
  psrc_page_free(&p);
  TEST_CHECK(psrc_pixabay_parse(kPixabaySearch, strlen(kPixabaySearch), 17, &p) == FM_OK && !p.next[0]);
  psrc_page_free(&p);
  TEST_CHECK(psrc_pixabay_parse(kPixabayKeyError, strlen(kPixabayKeyError), 1, &p) == FM_ERR_FORMAT &&
             strstr(p.error, "key"));
  psrc_page_free(&p);

  /* missing and malformed keys never reach the network */
  FmPsrcConf c;
  memset(&c, 0, sizeof c);
  const char *names[] = { "Pexels", "Unsplash", "Pixabay" };
  for (int i = 0; i < 3; i++) {
    const FmPsrc *s = psrc_at(4 + i);
    memset(&p, 0, sizeof p);
    FmErr e = s->search(&c, "cat", NULL, &p, NULL);
    char want[64];
    fm_snprintf(want, sizeof want, "Add a free %s key in Settings", names[i]);
    TEST_CHECK(e == FM_ERR_ACCESS && !strcmp(p.error, want) && p.count == 0);
    psrc_page_free(&p);
  }
  fm_strlcpy(c.key_pexels, "abc\r\nX-Evil: 1", sizeof c.key_pexels);
  fm_strlcpy(c.key_unsplash, "abc def", sizeof c.key_unsplash);
  TEST_CHECK(g_psrc_pexels.search(&c, "cat", NULL, &p, NULL) == FM_ERR_ACCESS && strstr(p.error, "not valid"));
  psrc_page_free(&p);
  TEST_CHECK(g_psrc_unsplash.search(&c, "cat", NULL, &p, NULL) == FM_ERR_ACCESS && strstr(p.error, "not valid"));
  psrc_page_free(&p);
  /* keyless sources want a query */
  for (int i = 0; i < 4; i++) {
    memset(&p, 0, sizeof p);
    TEST_CHECK(psrc_at(i)->search(&c, "", NULL, &p, NULL) == FM_ERR_NOT_FOUND && p.error[0]);
    psrc_page_free(&p);
  }
  char err[256];
  TEST_CHECK(psrc_http_error("Pexels", 401, true, err, sizeof err) == FM_ERR_ACCESS && strstr(err, "check it in Settings"));
  TEST_CHECK(psrc_http_error("Pexels", 429, true, err, sizeof err) == FM_ERR_IO &&
             !strncmp(err, "Too many requests \xE2\x80\x94 wait a minute", 30));
  TEST_CHECK(psrc_http_error("NASA", 503, false, err, sizeof err) == FM_ERR_IO && strstr(err, "503"));
  TEST_CHECK(psrc_http_error("NASA", 401, false, err, sizeof err) == FM_ERR_ACCESS && !strstr(err, "key"));
}

static void pt_garbage(void) {
  const char *samples[] = { kOvSearch, kWmSearch, kNasaSearch, kNasaAssets, kArticSearch, kPexelsSearch,
                            kUnsplashSearch, kUnsplashList, kPixabaySearch };
  const char *junk[] = { "", "null", "[]", "{}", "42", "\"str\"", "{\"results\":{}}", "{\"results\":[1,null,[]]}",
                         "{\"query\":{\"pages\":[1,{\"imageinfo\":7},{\"imageinfo\":[{\"mime\":5}]}]}}",
                         ("{\"query\":{\"pages\":{\"1\":{\"imageinfo\":[{\"mime\":\"image/jpeg\","
                          "\"thumburl\":\"https://x/500px-\",\"width\":1e300}]}}}}"),
                         "{\"collection\":{\"items\":[{\"data\":7,\"links\":[{\"href\":[]}]}]}}",
                         "{\"data\":[{\"image_id\":\"a\",\"id\":1e300,\"thumbnail\":{\"width\":-5}}]}",
                         "{\"photos\":[{\"id\":1,\"src\":{\"medium\":{}}}]}", "[{\"id\":{}}]",
                         "{\"hits\":[{\"id\":\"x\"}],\"totalHits\":\"lots\"}", "\xFF\xFE{", "{{{{", "[\"" };
  int runs = 0;
  FmPsrcItem it;
  for (int pass = 0; pass < 2; pass++) {
    int ns = pass ? FM_COUNT(junk) : FM_COUNT(samples);
    for (int s = 0; s < ns; s++) {
      const char *text = pass ? junk[s] : samples[s];
      size_t full = strlen(text);
      size_t step = pass ? 1 : 7;
      for (size_t len = pass ? full : 0; len <= full; len += step) {
        FmPsrcPage p;
        memset(&p, 0, sizeof p);
        psrc_openverse_parse(text, len, 1, &p);
        psrc_page_free(&p);
        psrc_wikimedia_parse(text, len, &p);
        psrc_page_free(&p);
        psrc_nasa_parse(text, len, 1, &p);
        psrc_page_free(&p);
        psrc_artic_parse(text, len, 1, &p);
        psrc_page_free(&p);
        psrc_pexels_parse(text, len, 1, &p);
        psrc_page_free(&p);
        psrc_unsplash_parse(text, len, 1, &p);
        psrc_page_free(&p);
        psrc_pixabay_parse(text, len, 1, &p);
        TEST_CHECK(p.count <= PSRC_MAX_ITEMS);
        psrc_page_free(&p);
        memset(&it, 0, sizeof it);
        psrc_nasa_parse_assets(text, len, &it);
        runs++;
      }
    }
  }
  TEST_CHECK(runs > 500);
  /* a truncated reply is an error with text, never a half page */
  FmPsrcPage p;
  memset(&p, 0, sizeof p);
  TEST_CHECK(psrc_wikimedia_parse(kWmSearch, strlen(kWmSearch) / 2, &p) == FM_ERR_FORMAT && p.error[0] && !p.count);
  psrc_page_free(&p);
  /* more results than a page holds: capped, one allocation */
  char *big = (char *)fm_alloc(64 * 1024);
  size_t o = (size_t)fm_snprintf(big, 64 * 1024, "{\"hits\":[");
  for (int i = 0; i < 100; i++)
    o += (size_t)fm_snprintf(big + o, 64 * 1024 - o, "%s{\"id\":%d,\"webformatURL\":\"https://x/%d_640.jpg\"}",
                             i ? "," : "", i + 1, i);
  fm_snprintf(big + o, 64 * 1024 - o, "],\"totalHits\":500}");
  TEST_CHECK(psrc_pixabay_parse(big, strlen(big), 1, &p) == FM_OK && p.count == PSRC_MAX_ITEMS &&
             p.cap == PSRC_MAX_ITEMS);
  psrc_page_free(&p);
  fm_free(big);
}

static void make_file(const char *path, const void *head, size_t hn, size_t size, i64 mtime) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return;
  for (size_t i = 0; i < size; i++) fputc(i < hn ? ((const u8 *)head)[i] : (int)(i & 0xFF), f);
  fclose(f);
  if (mtime) plat_set_mtime(path, mtime);
}

static bool read_text(const char *path, char *out, size_t cap) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  size_t n = fread(out, 1, cap - 1, f);
  out[n] = 0;
  fclose(f);
  return true;
}

static void pt_names(void) {
  FmPsrcItem it;
  char b[200];
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.title, "\xE7\x8C\xAB on the roof: \"Tom\" / Jerry?", sizeof it.title);
  fm_strlcpy(it.author, "Zo\xC3\xAB <x>", sizeof it.author);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "\xE7\x8C\xAB on the roof Tom Jerry - Zo\xC3\xAB x"));
  memset(&it, 0, sizeof it);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "photo"));
  fm_strlcpy(it.title, "...", sizeof it.title);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "photo"));
  fm_strlcpy(it.title, "con", sizeof it.title);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "_con"));
  fm_strlcpy(it.title, "COM1.txt", sizeof it.title);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(b[0] == '_');
  fm_strlcpy(it.title, "Console", sizeof it.title);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "Console"));
  fm_strlcpy(it.title, ".hidden", sizeof it.title);
  fm_strlcpy(it.author, "Bob.", sizeof it.author);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(!strcmp(b, "hidden - Bob"));
  /* long titles: cut on a character boundary, short enough for any file system */
  char longt[256];
  longt[0] = 0;
  for (int i = 0; i < 60; i++) fm_strlcat(longt, "\xD0\x96", sizeof longt);    /* 120 bytes of Cyrillic */
  fm_strlcpy(it.title, longt, sizeof it.title);
  fm_strlcpy(it.author, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80 Photographer With A Long Name Indeed", sizeof it.author);
  psrc_file_base(&it, b, 150);
  TEST_CHECK(strlen(b) < 150 && utf8_len(b) > 0 && strstr(b, " - \xF0\x9F\x98\x80"));
  bool valid = true;
  for (size_t i = 0; b[i];) {
    u32 cp;
    int n = utf8_decode(b + i, &cp);
    if (n <= 0 || cp == 0xFFFD) { valid = false; break; }
    i += (size_t)n;
  }
  TEST_CHECK(valid);

  static const u8 jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
  TEST_CHECK(!strcmp(psrc_sniff_ext(jpg, 4), ".jpg"));
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)"\x89PNG\r\n\x1A\n....", 12), ".png"));
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)"GIF89a..", 8), ".gif"));
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)"RIFF\0\0\0\0WEBPVP8 ", 16), ".webp"));
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)"II*\0....", 8), ".tif"));
  const char *svg = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)svg, strlen(svg)), ".svg"));
  const char *html = "<!DOCTYPE html><html><head><title>Just a moment...</title><svg/>";
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)html, strlen(html)), ""));
  TEST_CHECK(!strcmp(psrc_sniff_ext((const u8 *)"{\"error\":1}", 11), ""));
  TEST_CHECK(!strcmp(psrc_sniff_ext(jpg, 2), "") && !strcmp(psrc_sniff_ext(NULL, 0), ""));

  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "openverse", sizeof it.source);
  char k1[64], k2[64], k3[64];
  psrc_cache_key(&it, "https://a/1.jpg", k1, sizeof k1);
  psrc_cache_key(&it, "https://a/2.jpg", k2, sizeof k2);
  psrc_cache_key(&it, "https://a/1.jpg", k3, sizeof k3);
  TEST_CHECK(!strncmp(k1, "openverse-", 10) && strlen(k1) == 26 && strcmp(k1, k2) && !strcmp(k1, k3));
  fm_strlcpy(it.source, "", sizeof it.source);
  psrc_cache_key(&it, "https://a/1.jpg", k3, sizeof k3);
  TEST_CHECK(!strncmp(k3, "photo-", 6) && strlen(k3) == 22);

  char text[2048];
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.title, "Cat", sizeof it.title);
  fm_strlcpy(it.license, "CC BY 2.0", sizeof it.license);
  fm_strlcpy(it.page, "https://p/1", sizeof it.page);
  fm_strlcpy(it.full, "https://f/1.jpg", sizeof it.full);
  psrc_attribution(&it, "Openverse", text, sizeof text);
  TEST_CHECK(strstr(text, "Title: Cat\n") && strstr(text, "Author: (unknown)\n") &&
             strstr(text, "Licence: CC BY 2.0\n") && strstr(text, "Source: Openverse\n") &&
             strstr(text, "Page: https://p/1\n") && strstr(text, "Image: https://f/1.jpg\n"));
}

static void pt_cache(const char *tmp) {
  char dir[FM_PATH_MAX], p[FM_PATH_MAX];
  fm_path_join(dir, sizeof dir, tmp, "pcache");
  plat_mkdirs(dir);
  i64 now = plat_time_unix();
  static const char *const names[] = {
    "openverse-0123456789abcdef.jpg", "nasa-0123456789abcdef.png", "artic-0123456789abcdef.jpg",
    "notours.jpg", "pexels-0123456789abcdef.jpg", "nasa-xyz.jpg", "photo-0123456789abcdef.gif",
    "wikimedia-0123456789abcdef.dl.part", "unsplash-0123456789abcdef.webp" };
  i64 ages[] = { 5000, 4000, 3000, 9000, 9999, 9999, 2000, 30, 100 };
  for (int i = 0; i < FM_COUNT(names); i++) {
    fm_path_join(p, sizeof p, dir, names[i]);
    make_file(p, NULL, 0, 1000, now - ages[i]);
  }
  /* 9000 bytes; keep 5000: the oldest of ours go first, pexels is kept */
  psrc_cache_trim(dir, 5000, "pexels-0123");
  bool gone[FM_COUNT(names)];
  for (int i = 0; i < FM_COUNT(names); i++) {
    fm_path_join(p, sizeof p, dir, names[i]);
    gone[i] = !plat_exists(p);
  }
  TEST_CHECK(gone[0] && gone[1] && gone[2] && gone[6]);         /* 4 oldest of ours */
  TEST_CHECK(!gone[3] && !gone[4] && !gone[5] && !gone[7] && !gone[8]);
  psrc_cache_trim(dir, 0, NULL);                                /* only our names, never a fresh .part */
  fm_path_join(p, sizeof p, dir, names[3]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[5]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[7]); TEST_CHECK(plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[4]); TEST_CHECK(!plat_exists(p));
  fm_path_join(p, sizeof p, dir, names[8]); TEST_CHECK(!plat_exists(p));
  psrc_cache_trim(NULL, 0, NULL);
  fm_path_join(p, sizeof p, tmp, "no-such-dir");
  psrc_cache_trim(p, 0, NULL);
}

/* fetch without network: a cached file is reused, and saving copies it with
** a readable name and the attribution file */
static void pt_fetch_offline(const char *tmp) {
  FmPsrcConf c;
  memset(&c, 0, sizeof c);
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "fcache");
  fm_path_join(c.download_dir, sizeof c.download_dir, tmp, "dl");
  plat_mkdirs(c.cache_dir);
  FmPsrcItem it;
  memset(&it, 0, sizeof it);
  fm_strlcpy(it.source, "openverse", sizeof it.source);
  fm_strlcpy(it.title, "Chat \xC3\xA9t\xC3\xA9: \"noir\"", sizeof it.title);
  fm_strlcpy(it.author, "Zo\xC3\xAB", sizeof it.author);
  fm_strlcpy(it.license, "CC BY 2.0", sizeof it.license);
  fm_strlcpy(it.page, "https://example.org/page", sizeof it.page);
  fm_strlcpy(it.full, "https://example.org/a b.jpg", sizeof it.full);
  char key[64], cached[FM_PATH_MAX], name[96], out[FM_PATH_MAX], err[256];
  psrc_cache_key(&it, "https://example.org/a%20b.jpg", key, sizeof key);
  fm_snprintf(name, sizeof name, "%s.png", key);
  fm_path_join(cached, sizeof cached, c.cache_dir, name);
  make_file(cached, "\x89PNG\r\n\x1A\n", 8, 3000, plat_time_unix() - 5000);

  TEST_CHECK(psrc_fetch(&c, &it, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(!strcmp(out, cached));
  FmStat st;
  TEST_CHECK(plat_stat(cached, &st) && st.mtime > plat_time_unix() - 100);    /* touched: used recently */

  TEST_CHECK(psrc_fetch(&c, &it, c.download_dir, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(!strcmp(fm_path_base(out), "Chat \xC3\xA9t\xC3\xA9 noir - Zo\xC3\xAB.png"));
  TEST_CHECK(plat_stat(out, &st) && st.size == 3000);
  char txt[FM_PATH_MAX], text[2048];
  fm_path_join(txt, sizeof txt, c.download_dir, "Chat \xC3\xA9t\xC3\xA9 noir - Zo\xC3\xAB.txt");
  TEST_CHECK(read_text(txt, text, sizeof text) && strstr(text, "Licence: CC BY 2.0") &&
             strstr(text, "Author: Zo\xC3\xAB") && strstr(text, "Page: https://example.org/page") &&
             strstr(text, "Source: Openverse"));
  /* a second save does not overwrite */
  TEST_CHECK(psrc_fetch(&c, &it, c.download_dir, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_OK);
  TEST_CHECK(!strcmp(fm_path_base(out), "Chat \xC3\xA9t\xC3\xA9 noir - Zo\xC3\xAB (2).png"));
  fm_path_join(txt, sizeof txt, c.download_dir, "Chat \xC3\xA9t\xC3\xA9 noir - Zo\xC3\xAB (2).txt");
  TEST_CHECK(plat_exists(txt));

  /* bad input */
  FmPsrcItem bad = it;
  fm_strlcpy(bad.full, "javascript:x", sizeof bad.full);
  TEST_CHECK(psrc_fetch(&c, &bad, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) == FM_ERR_NOT_FOUND &&
             err[0] && out[0] == 0);
  TEST_CHECK(psrc_fetch(&c, NULL, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) != FM_OK);
  if (!net_available()) {
    fm_strlcpy(bad.full, "https://example.org/not-cached.jpg", sizeof bad.full);
    TEST_CHECK(psrc_fetch(&c, &bad, NULL, out, sizeof out, NULL, NULL, err, sizeof err, NULL) ==
               FM_ERR_UNSUPPORTED && err[0]);
  }
}

/* ---- online ---------------------------------------------------------------------- */

static bool env_on(const char *name) {
  const char *v = getenv(name);
  return v && !strcmp(v, "1");
}

static int search_report(const FmPsrc *s, const FmPsrcConf *c, const char *q, const char *token, FmPsrcPage *p) {
  memset(p, 0, sizeof *p);
  u64 t0 = plat_now_ms();
  FmErr e = s->search(c, q, token, p, NULL);
  int sized = 0;
  for (int i = 0; i < p->count; i++) sized += p->items[i].width > 0;
  printf("  %s \"%s\"%s%s: %s, %d items (%d with size), next \"%s\" (%d ms)%s%s\n", s->name, q,
         token ? " page " : "", token ? token : "", fm_err_str(e), p->count, sized, p->next,
         (int)(plat_now_ms() - t0), p->error[0] ? " -- " : "", p->error);
  if (p->count)
    printf("    first: \"%s\" by %s, %s, %dx%d\n      thumb %s\n      full  %s\n", p->items[0].title,
           p->items[0].author, p->items[0].license, p->items[0].width, p->items[0].height, p->items[0].thumb,
           p->items[0].full);
  TEST_CHECK(e == FM_OK && p->count > 0);
  return p->count;
}

/* GETs a thumbnail with the source's headers and decodes it */
static bool thumb_check(const FmPsrcItem *it) {
  FmNetResp r;
  u64 t0 = plat_now_ms();
  FmErr e = net_get(it->thumb, psrc_item_headers(it), 8u << 20, &r, NULL);
  bool ok = false;
  if (e == FM_OK && r.status == 200) {
    FmImage im;
    memset(&im, 0, sizeof im);
    if (img_load_mem(r.data, r.len, 1024, &im) == FM_OK) {
      printf("    thumb: %d bytes -> %dx%d (%d ms)\n", (int)r.len, im.w, im.h, (int)(plat_now_ms() - t0));
      ok = im.w > 0 && im.h > 0;
      img_free(&im);
    } else printf("    thumb: %d bytes, %s, does not decode\n", (int)r.len, r.type);
  } else printf("    thumb: %s, HTTP %d\n", fm_err_str(e), r.status);
  net_resp_free(&r);
  return ok;
}

/* psrc_fetch into the cache, then a full decode like the viewer's */
static bool full_check(const FmPsrcConf *c, FmPsrcItem *it) {
  const FmPsrc *s = psrc_find(it->source);
  if (s && s->details) s->details(c, it, NULL);
  char path[FM_PATH_MAX], err[256];
  u64 t0 = plat_now_ms();
  FmErr e = psrc_fetch(c, it, NULL, path, sizeof path, NULL, NULL, err, sizeof err, NULL);
  if (e != FM_OK) {
    printf("    full: %s -- %s (%s)\n", fm_err_str(e), err, it->full);
    return false;
  }
  u64 t1 = plat_now_ms();
  FmStat st;
  plat_stat(path, &st);
  FmImage im;
  FmImgInfo info;
  memset(&im, 0, sizeof im);
  memset(&info, 0, sizeof info);
  e = img_load(path, 0, 0, &im, &info, NULL);
  printf("    full: %s, %llu bytes in %d ms -> %s %dx%d (listed %dx%d), decode %d ms\n", fm_path_base(path),
         (unsigned long long)st.size, (int)(t1 - t0), fm_err_str(e), im.w, im.h, it->width, it->height,
         (int)(plat_now_ms() - t1));
  bool ok = e == FM_OK && im.w > 0 && im.h > 0;
  img_free(&im);
  /* a second fetch is the cache */
  t0 = plat_now_ms();
  char again[FM_PATH_MAX];
  TEST_CHECK(psrc_fetch(c, it, NULL, again, sizeof again, NULL, NULL, err, sizeof err, NULL) == FM_OK &&
             !strcmp(again, path) && plat_now_ms() - t0 < 1000);
  return ok;
}

static void pt_online(const char *tmp) {
  if (!env_on("MMCFM_NET_TEST")) return;
  if (!net_available()) { printf("  network: %s (skipped)\n", net_backend()); return; }
  FmPsrcConf c;
  memset(&c, 0, sizeof c);
  c.safe_search = true;
  fm_path_join(c.cache_dir, sizeof c.cache_dir, tmp, "online-photos");
  fm_path_join(c.download_dir, sizeof c.download_dir, tmp, "downloads");
  static const char *const kQuery[] = { "cat", "cat", "moon", "cat" };
  FmPsrcPage p;
  for (int i = 0; i < 4; i++) {
    const FmPsrc *s = psrc_at(i);
    if (!search_report(s, &c, kQuery[i], NULL, &p)) { psrc_page_free(&p); continue; }
    if (p.next[0]) {
      FmPsrcPage p2;
      search_report(s, &c, kQuery[i], p.next, &p2);
      psrc_page_free(&p2);
    }
    bool thumb = false, full = false;
    for (int k = 0; k < p.count && k < 4 && !thumb; k++) thumb = thumb_check(&p.items[k]);
    if (i == 0) {
      /* Openverse's own proxy (non-Flickr results). One CDN entry seen today
      ** answered WebP to WinHTTP whatever the Accept header, so try a few */
      bool proxy = false;
      for (int k = p.count - 1; k >= 0 && k >= p.count - 3 && !proxy; k--) {
        FmPsrcItem it = p.items[k];
        fm_snprintf(it.thumb, sizeof it.thumb, "https://api.openverse.org/v1/images/%s/thumb/", it.id);
        printf("    proxy %s\n", it.thumb);
        proxy = thumb_check(&it);
      }
      TEST_CHECK(proxy);
    }
    for (int k = 0; k < p.count && k < 4 && !full; k++) full = full_check(&c, &p.items[k]);
    TEST_CHECK(thumb);
    TEST_CHECK(full);
    if (i == 3) {                                  /* a public-domain work comes at 1686 px */
      int k = 0;
      while (k < p.count && strcmp(p.items[k].license, "Public domain")) k++;
      if (k < p.count) {
        printf("    public domain: \"%s\"\n", p.items[k].title);
        TEST_CHECK(full_check(&c, &p.items[k]));
      }
    }
    if (i == 2 && p.count) {                       /* Download: original + attribution file */
      char out[FM_PATH_MAX], err[256], txt[FM_PATH_MAX], text[2048];
      FmPsrcItem it = p.items[0];
      it.original[0] = 0;                          /* the TIFF original can be 100 MB */
      u64 t0 = plat_now_ms();
      FmErr e = psrc_fetch(&c, &it, c.download_dir, out, sizeof out, NULL, NULL, err, sizeof err, NULL);
      printf("    download: %s %s (%d ms)\n", fm_err_str(e), e == FM_OK ? fm_path_base(out) : err,
             (int)(plat_now_ms() - t0));
      fm_strlcpy(txt, out, sizeof txt);
      char *dot = strrchr(txt, '.');
      if (dot) fm_strlcpy(dot, ".txt", (size_t)(txt + sizeof txt - dot));
      TEST_CHECK(e == FM_OK && read_text(txt, text, sizeof text) && strstr(text, "NASA"));
    }
    psrc_page_free(&p);
  }

  /* the Art Institute's image server turns away requests without its header */
  memset(&p, 0, sizeof p);
  if (g_psrc_artic.search(&c, "monet", NULL, &p, NULL) == FM_OK && p.count) {
    FmNetResp r;
    net_get(p.items[0].thumb, NULL, 1u << 20, &r, NULL);
    printf("  Art Institute thumb without AIC-User-Agent: HTTP %d %s\n", r.status, r.type);
    net_resp_free(&r);
  }
  psrc_page_free(&p);

  /* keyed sources: real keys from the environment, else the error paths live */
  static const char *const kEnv[] = { "MMCFM_PEXELS_KEY", "MMCFM_UNSPLASH_KEY", "MMCFM_PIXABAY_KEY" };
  for (int i = 0; i < 3; i++) {
    const FmPsrc *s = psrc_at(4 + i);
    FmPsrcConf k = c;
    const char *key = getenv(kEnv[i]);
    char *slot = i == 0 ? k.key_pexels : i == 1 ? k.key_unsplash : k.key_pixabay;
    if (key && *key) {
      fm_strlcpy(slot, key, sizeof k.key_pexels);
      search_report(s, &k, "", NULL, &p);
      psrc_page_free(&p);
      if (search_report(s, &k, "mountain lake", NULL, &p)) {
        TEST_CHECK(thumb_check(&p.items[0]));
        TEST_CHECK(full_check(&k, &p.items[0]));
      }
      psrc_page_free(&p);
      continue;
    }
    memset(&p, 0, sizeof p);
    u64 t0 = plat_now_ms();
    FmErr e = s->search(&k, "cat", NULL, &p, NULL);
    printf("  %s, no key: %s \"%s\"\n", s->name, fm_err_str(e), p.error);
    TEST_CHECK(e == FM_ERR_ACCESS && strstr(p.error, "Add a free"));
    psrc_page_free(&p);
    fm_strlcpy(slot, "INVALIDKEY0123456789", sizeof k.key_pexels);
    memset(&p, 0, sizeof p);
    e = s->search(&k, "cat", NULL, &p, NULL);
    printf("  %s, bogus key: %s \"%s\" (%d ms)\n", s->name, fm_err_str(e), p.error, (int)(plat_now_ms() - t0));
    TEST_CHECK(e == FM_ERR_ACCESS && strstr(p.error, "did not accept the API key"));
    psrc_page_free(&p);
  }
}

int test_psrc(const char *tmp) {
  int before = g_test_fail;
  pt_registry();
  pt_page();
  pt_text();
  pt_url_helpers();
  pt_urls();
  pt_openverse();
  pt_wikimedia();
  pt_nasa();
  pt_artic();
  pt_keyed();
  pt_garbage();
  pt_names();
  pt_cache(tmp);
  pt_fetch_offline(tmp);
  pt_online(tmp);
  return g_test_fail - before;
}
