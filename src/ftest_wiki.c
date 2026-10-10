/* ftest_wiki.c -- Wikipedia (fwiki_api.c): the parsers on captured replies,
** offline; MMCFM_WIKI_TEST=<article> runs every call against Wikipedia and
** opens one picture, one video and one sound of the article in the app's
** own decoders (fetched at the size the viewers use).
*/
#include "ftest.h"
#include "fwiki_int.h"
#include "fplat.h"
#include "fnet.h"
#include "fdec_img.h"
#include "fdec_vid.h"

static const char kArt[] =
  "{\"batchcomplete\":true,\"query\":{\"pages\":[{\"pageid\":1,\"ns\":0,\"title\":\"Cebu\","
  "\"extract\":\"Cebu is a province.\\nIt has islands {\\\\displaystyle x^{2}} here.\\n\\n\\n== Etymology ==\\n"
  "The name.\\n\\n=== Old ===\\n\\n\\n== References ==\\n\\n\\n== History ==\\nLong ago.\\n\",\"description\":"
  "\"Province in Central Visayas\",\"fullurl\":\"https://en.wikipedia.org/wiki/Cebu\","
  "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/x/960px-a.jpg\",\"width\":960,\"height\":640}}]}}";
static const char kMissing[] = "{\"query\":{\"pages\":[{\"ns\":0,\"title\":\"Nope\",\"missing\":true}]}}";
static const char kSearch[] =
  "{\"continue\":{\"gsroffset\":20,\"continue\":\"gsroffset||\"},\"query\":{\"pages\":["
  "{\"title\":\"Cebu City\",\"index\":2,\"description\":\"City\"},"
  "{\"title\":\"Cebu\",\"index\":1,\"description\":\"Province\",\"thumbnail\":{\"source\":\"https://u/x.jpg\"}}]}}";
static const char kMedia[] =
  "{\"items\":[{\"title\":\"File:A.jpg\",\"type\":\"image\",\"showInGallery\":true,\"srcset\":[{\"src\":\"//u/a1.jpg\"},"
  "{\"src\":\"//u/a15.jpg\"}],\"caption\":{\"text\":\"An island\"}},"
  "{\"title\":\"File:Flag.svg\",\"type\":\"image\",\"showInGallery\":false},"
  "{\"title\":\"File:B.ogv\",\"type\":\"video\",\"showInGallery\":true},"
  "{\"title\":\"File:C.ogg\",\"type\":\"audio\",\"showInGallery\":true}]}";
static const char kVideo[] =
  "{\"query\":{\"pages\":[{\"title\":\"File:B.ogv\",\"videoinfo\":[{\"url\":\"https://u/B.ogv\",\"mime\":\"application/ogg\","
  "\"width\":1920,\"height\":1080,\"derivatives\":["
  "{\"src\":\"https://u/B.ogv.240p.vp9.webm\",\"type\":\"video/webm; codecs=\\\"vp9, opus\\\"\",\"height\":240},"
  "{\"src\":\"https://u/B.ogv.720p.vp9.webm\",\"type\":\"video/webm; codecs=\\\"vp9, opus\\\"\",\"height\":720},"
  "{\"src\":\"https://u/B.ogv.480p.vp9.webm\",\"type\":\"video/webm; codecs=\\\"vp9, opus\\\"\",\"height\":480},"
  "{\"src\":\"https://u/B.ogv.1080p.vp9.webm\",\"type\":\"video/webm; codecs=\\\"vp9, opus\\\"\",\"height\":1080},"
  "{\"src\":\"https://u/B.ogv.mp3\",\"type\":\"audio/mpeg\"}]}]}]}}";

static void wt_offline(void) {
  WkArticle a;
  memset(&a, 0, sizeof a);
  TEST_CHECK(wk_parse_article(kArt, sizeof kArt - 1, "en", &a) == FM_OK);
  /* paragraphs, the formula cut, empty headings (Old, References) dropped */
  TEST_CHECK(a.nb == 6 && a.b[0].kind == WB_PARA && !strcmp(a.b[0].s, "Cebu is a province."));
  TEST_CHECK(a.nb == 6 && !strstr(a.b[1].s, "displaystyle") && a.b[2].kind == WB_H2 && !strcmp(a.b[2].s, "Etymology"));
  TEST_CHECK(a.nb == 6 && a.b[3].kind == WB_PARA && a.b[4].kind == WB_H2 && !strcmp(a.b[4].s, "History") &&
             a.b[5].kind == WB_PARA);
  TEST_CHECK(!strcmp(a.desc, "Province in Central Visayas") && strstr(a.lead, "960px") && a.lead_w == 960);
  wk_article_free(&a);
  memset(&a, 0, sizeof a);
  TEST_CHECK(wk_parse_article(kMissing, sizeof kMissing - 1, "en", &a) == FM_ERR_NOT_FOUND && a.error[0]);
  wk_article_free(&a);
  WkList l;
  memset(&l, 0, sizeof l);
  TEST_CHECK(wk_parse_search(kSearch, sizeof kSearch - 1, &l) == FM_OK);
  TEST_CHECK(l.n == 2 && !strcmp(l.it[0].title, "Cebu") && l.it[0].thumb[0] && l.next == 20);   /* by index */
  wk_list_free(&l);
  WkMediaList m;
  memset(&m, 0, sizeof m);
  TEST_CHECK(wk_parse_media(kMedia, sizeof kMedia - 1, &m) == FM_OK);
  TEST_CHECK(m.n == 3 && m.it[0].type == WM_IMAGE && !strcmp(m.it[0].thumb, "https://u/a15.jpg") &&
             !strcmp(m.it[0].caption, "An island") && m.it[1].type == WM_VIDEO && m.it[2].type == WM_AUDIO);
  wk_media_free(&m);
  WkFile f;
  memset(&f, 0, sizeof f);
  TEST_CHECK(wk_parse_file(kVideo, sizeof kVideo - 1, WM_VIDEO, &f) == FM_OK && strstr(f.url, "480p.vp9.webm"));
  memset(&f, 0, sizeof f);
  TEST_CHECK(wk_parse_file(kVideo, sizeof kVideo - 1, WM_AUDIO, &f) == FM_OK && strstr(f.url, ".mp3"));
  TEST_CHECK(wk_lang_ok("en") && wk_lang_ok("simple") && wk_lang_ok("zh-yue") && !wk_lang_ok("e") &&
             !wk_lang_ok("en/../x") && !wk_lang_ok("EN"));
}

static void wt_live(const char *tmp) {
  const char *title = getenv("MMCFM_WIKI_TEST");
  if (!title || !*title) return;
  u64 t0 = plat_now_ms();
  WkFeed fd;
  FmErr e = wk_feed("en.wikipedia.org", &fd, NULL);
  printf("  wiki feed: %s in %d ms, featured \"%s\", %d most read, picture %d, %d on this day\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), fd.tfa.title, fd.most.n, (int)fd.has_potd, fd.notd);
  TEST_CHECK(e == FM_OK && fd.has_tfa && fd.most.n > 5);
  wk_feed_free(&fd);
  WkList l;
  t0 = plat_now_ms();
  e = wk_search("en.wikipedia.org", title, 0, &l, NULL);
  printf("  wiki search: %s, %d results in %d ms, next %d, first \"%s\" (%s)\n", fm_err_str(e), l.n,
         (int)(plat_now_ms() - t0), l.next, l.n ? l.it[0].title : "", l.n ? l.it[0].desc : "");
  TEST_CHECK(e == FM_OK && l.n >= 10 && l.next > 0);
  wk_list_free(&l);
  WkArticle a;
  t0 = plat_now_ms();
  e = wk_article("en.wikipedia.org", title, &a, NULL);
  int heads = 0;
  for (int i = 0; i < a.nb; i++) heads += a.b[i].kind != WB_PARA;
  printf("  wiki article: %s in %d ms, \"%s\", %d blocks (%d headings), lead %s\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), a.title, a.nb, heads, a.lead[0] ? "yes" : "no");
  TEST_CHECK(e == FM_OK && a.nb > 10 && heads > 3);
  WkMediaList m;
  e = wk_media("en.wikipedia.org", a.title, &m, NULL);
  int ni = 0, nv = 0, na = 0;
  for (int i = 0; i < m.n; i++) ni += m.it[i].type == WM_IMAGE, nv += m.it[i].type == WM_VIDEO, na += m.it[i].type == WM_AUDIO;
  printf("  wiki media: %s, %d pictures, %d videos, %d sounds\n", fm_err_str(e), ni, nv, na);
  TEST_CHECK(e == FM_OK && ni > 0);
  for (int k = 0; k < 3; k++) {
    int type = k == 0 ? WM_IMAGE : k == 1 ? WM_VIDEO : WM_AUDIO;
    const WkMedia *pick = NULL;
    for (int i = 0; i < m.n && !pick; i++)
      if (m.it[i].type == type) pick = &m.it[i];
    if (!pick) continue;
    WkFile f;
    e = wk_file("en.wikipedia.org", pick->file, type, &f, NULL);
    printf("  wiki %s %s: %s %s\n", type == WM_IMAGE ? "picture" : type == WM_VIDEO ? "video" : "sound", pick->file + 5,
           fm_err_str(e), f.url);
    TEST_CHECK(e == FM_OK && f.url[0]);
    if (e != FM_OK) continue;
    if (type == WM_IMAGE) {
      char path[FM_PATH_MAX];
      fm_path_join(path, sizeof path, tmp, "wiki-picture");
      FmNetResp r;
      memset(&r, 0, sizeof r);
      FmErr de = net_download(f.url, NULL, path, NULL, NULL, &r, NULL);
      FmImage im;
      memset(&im, 0, sizeof im);
      FmErr ie = de == FM_OK && r.status == 200 ? img_load(path, 1024, 1024, &im, NULL, NULL) : FM_ERR_IO;
      printf("    download %s (%d), decode %s %dx%d\n", fm_err_str(de), r.status, fm_err_str(ie), im.w, im.h);
      TEST_CHECK(ie == FM_OK && im.w > 0);
      img_free(&im);
      net_resp_free(&r);
    } else {
      /* as the view does: downloaded whole, then played from the file */
      char path[FM_PATH_MAX];
      fm_path_join(path, sizeof path, tmp, type == WM_AUDIO ? "wiki-sound.mp3" : "wiki-video.webm");
      FmNetResp r;
      memset(&r, 0, sizeof r);
      t0 = plat_now_ms();
      FmErr de = net_download(f.url, NULL, path, NULL, NULL, &r, NULL);
      printf("    download %s (%d) in %d ms\n", fm_err_str(de), r.status, (int)(plat_now_ms() - t0));
      net_resp_free(&r);
      FmErr ve;
      t0 = plat_now_ms();
      FmVid *v = de == FM_OK ? vid_open(path, type == WM_AUDIO ? VID_OPEN_AUDIO_ONLY : 0, &ve) : NULL;
      int fv = 0, fa = 0, guard = 0, ev;
      FmVidFrame vf;
      FmVidPcm pc;
      while (v && guard++ < 2000 && (fv < 10 || fa < 10) && (ev = vid_decode(v, &vf, &pc)) > 0) {
        if (ev == VID_EV_VIDEO) fv++;
        else if (ev == VID_EV_AUDIO) fa++;
        if (type == WM_AUDIO && fa >= 10) break;
      }
      printf("    %s: %s, %d frames, %d sound blocks in %d ms\n", v ? vid_info(v)->backend : "open failed", v ? "" : fm_err_str(ve),
             fv, fa, (int)(plat_now_ms() - t0));
      TEST_CHECK(v && fa >= 5 && (type == WM_AUDIO || fv >= 5));
      if (v) vid_close(v);
    }
  }
  wk_media_free(&m);
  WkLangs lg;
  e = wk_langs("en.wikipedia.org", a.title, &lg, NULL);
  printf("  wiki languages: %s, %d (first %s: \"%s\")\n", fm_err_str(e), lg.n, lg.n ? lg.it[0].lang : "",
         lg.n ? lg.it[0].title : "");
  TEST_CHECK(e == FM_OK && lg.n > 5);
  wk_langs_free(&lg);
  e = wk_related("en.wikipedia.org", a.title, &l, NULL);
  printf("  wiki related: %s, %d\n", fm_err_str(e), l.n);
  TEST_CHECK(e == FM_OK && l.n > 3);
  wk_list_free(&l);
  e = wk_links("en.wikipedia.org", a.title, &l, NULL);
  printf("  wiki links: %s, %d\n", fm_err_str(e), l.n);
  TEST_CHECK(e == FM_OK && l.n > 20);
  wk_list_free(&l);
  char rt[256];
  e = wk_random("en.wikipedia.org", rt, sizeof rt, NULL);
  printf("  wiki random: %s \"%s\"\n", fm_err_str(e), rt);
  TEST_CHECK(e == FM_OK && rt[0]);
  e = wk_article("tl.wikipedia.org", "Cebu", &a, NULL);                /* another language */
  printf("  wiki tl article: %s, %d blocks\n", fm_err_str(e), a.nb);
  TEST_CHECK(e == FM_OK && a.nb > 0);
  wk_article_free(&a);
}

int test_wiki(const char *tmp) {
  int before = g_test_fail;
  wt_offline();
  wt_live(tmp);
  return g_test_fail - before;
}

/* ---- the other Online reading sources (fwiki_more.c) --------------------------------------- */

static const char kGuten[] =
  "The Project Gutenberg eBook of Test\r\nTitle: A Test Book\r\nAuthor: Jose Rizal\r\n\r\n"
  "*** START OF THE PROJECT GUTENBERG EBOOK A TEST BOOK ***\r\n\r\nPREFACE\r\n\r\nSome words\r\nwrapped here.\r\n\r\n"
  "CHAPTER I. The Start\r\n\r\nFirst paragraph\r\nof the chapter.\r\n\r\n[Illustration: a tree]\r\n\r\nII.\r\n\r\n"
  "Second.\r\n\r\n*** END OF THE PROJECT GUTENBERG EBOOK ***\r\nThe licence.\r\n";
static const char kTaxon[] =
  "{\"results\":[{\"id\":48872,\"name\":\"Mangifera indica\",\"preferred_common_name\":\"indian mango\",\"rank\":\"species\","
  "\"wikipedia_summary\":\"<b>Mangifera indica</b>, the mango &amp; more.\",\"wikipedia_url\":\"http://en.wikipedia.org/wiki/Mangifera_indica\","
  "\"observations_count\":51234,\"default_photo\":{\"medium_url\":\"https://x/photos/1/medium.jpg\"},"
  "\"ancestors\":[{\"rank\":\"kingdom\",\"name\":\"Plantae\",\"preferred_common_name\":\"Plants\"},{\"rank\":\"subkingdom\",\"name\":\"X\"},"
  "{\"rank\":\"genus\",\"name\":\"Mangifera\"}],"
  "\"taxon_photos\":[{\"photo\":{\"id\":7,\"url\":\"https://x/photos/7/square.jpg\",\"medium_url\":\"https://x/photos/7/medium.jpg\","
  "\"attribution\":\"(c) someone, CC BY\"}}]}]}";
static const char kDict[] =
  "[{\"word\":\"hello\",\"phonetic\":\"/h\u0259\u02c8l\u0259\u028a/\",\"phonetics\":[{\"text\":\"/h\u0259\u02c8l\u0259\u028a/\","
  "\"audio\":\"https://api.dictionaryapi.dev/media/pronunciations/en/hello-uk.mp3\"},{\"text\":\"/h\u025b\u02c8lo\u028a/\",\"audio\":\"\"}],"
  "\"meanings\":[{\"partOfSpeech\":\"noun\",\"definitions\":[{\"definition\":\"\\\"Hello!\\\" or an equivalent greeting.\","
  "\"example\":\"She said hello.\"}],\"synonyms\":[\"greeting\"],\"antonyms\":[]},"
  "{\"partOfSpeech\":\"interjection\",\"definitions\":[{\"definition\":\"A greeting.\"}],\"synonyms\":[],\"antonyms\":[\"bye\"]}]}]";

static void rt_offline(void) {
  WkArticle a;
  memset(&a, 0, sizeof a);
  char *t = fm_strdup(kGuten);
  rd_guten_text(t, &a);
  fm_free(t);
  /* PREFACE, its paragraph (lines joined), CHAPTER I + paragraph, II. + paragraph; no illustration, no licence */
  TEST_CHECK(a.nb == 6 && a.b[0].kind == WB_H2 && !strcmp(a.b[0].s, "PREFACE") && !strcmp(a.b[1].s, "Some words wrapped here."));
  TEST_CHECK(a.nb == 6 && a.b[2].kind == WB_H2 && !strcmp(a.b[2].s, "CHAPTER I. The Start") && a.b[4].kind == WB_H2 &&
             !strcmp(a.b[5].s, "Second."));
  wk_article_free(&a);
  WkMediaList m;
  memset(&a, 0, sizeof a);
  TEST_CHECK(rd_inat_parse_taxon(kTaxon, sizeof kTaxon - 1, &a, &m) == FM_OK);
  TEST_CHECK(!strcmp(a.title, "Indian mango") && !strcmp(a.id, "48872") && !strcmp(a.wiki, "Mangifera indica") &&
             strstr(a.desc, "Mangifera indica"));
  TEST_CHECK(a.nb >= 5 && strstr(a.b[0].s, "the mango & more") && !strstr(a.b[0].s, "<b>"));
  TEST_CHECK(m.n == 1 && strstr(m.it[0].url, "/large.jpg") && strstr(m.it[0].caption, "CC BY"));
  wk_article_free(&a);
  wk_media_free(&m);
  memset(&a, 0, sizeof a);
  TEST_CHECK(rd_dict_parse(kDict, sizeof kDict - 1, &a, &m) == FM_OK);
  TEST_CHECK(!strcmp(a.title, "hello") && strstr(a.desc, "English dictionary") && m.n == 1 && strstr(m.it[0].url, "hello-uk.mp3") &&
             strstr(m.it[0].file, "UK"));
  int heads = 0;
  for (int i = 0; i < a.nb; i++) heads += a.b[i].kind == WB_H2;
  TEST_CHECK(heads == 2);
  wk_article_free(&a);
  wk_media_free(&m);
  TEST_CHECK(wk_host_ok("en.wikipedia.org") && wk_host_ok("tl.wiktionary.org") && wk_host_ok("species.wikimedia.org") &&
             !wk_host_ok("evil.com") && !wk_host_ok("a.b.wikipedia.org") && !wk_host_ok("en.wikipedia.org.evil.com"));
}

/* MMCFM_READ_TEST=1: every other source against its site (timed). */
static void rt_live(void) {
  if (!getenv("MMCFM_READ_TEST")) return;
  WkList l;
  WkArticle a;
  WkMediaList m;
  u64 t0 = plat_now_ms();
  FmErr e = rd_guten_search("en", "", 1, &l, NULL);
  printf("  books popular: %s, %d in %d ms, first \"%s\" (%s)\n", fm_err_str(e), l.n, (int)(plat_now_ms() - t0), l.n ? l.it[0].title : "",
         l.n ? l.it[0].desc : l.error);
  TEST_CHECK(e == FM_OK && l.n > 10);
  wk_list_free(&l);
  t0 = plat_now_ms();
  e = rd_guten_book("20228", &a, NULL);
  int ch = 0;
  for (int i = 0; i < a.nb; i++) ch += a.b[i].kind != WB_PARA;
  printf("  book 20228: %s in %d ms, \"%s\" by %s, %d blocks, %d headings %s\n", fm_err_str(e), (int)(plat_now_ms() - t0), a.title,
         a.desc, a.nb, ch, a.error);
  TEST_CHECK(e == FM_OK && a.nb > 500 && ch > 20);
  wk_article_free(&a);
  t0 = plat_now_ms();
  e = rd_inat_search("tl", "mango", &l, NULL);
  printf("  species \"mango\" (tl): %s, %d in %d ms, first %s \"%s\" (%s)\n", fm_err_str(e), l.n, (int)(plat_now_ms() - t0),
         l.n ? l.it[0].id : "", l.n ? l.it[0].title : "", l.n ? l.it[0].desc : l.error);
  TEST_CHECK(e == FM_OK && l.n > 2);
  wk_list_free(&l);
  e = rd_inat_group("en", "3", 1, &l, NULL);
  printf("  birds most observed: %s, %d, first \"%s\", next %d\n", fm_err_str(e), l.n, l.n ? l.it[0].title : "", l.next);
  TEST_CHECK(e == FM_OK && l.n >= 30 && l.next == 2);
  wk_list_free(&l);
  e = rd_inat_taxon("en", "48872", &a, &m, NULL);
  printf("  species 48872: %s \"%s\" (%s), %d blocks, %d photos, wiki \"%s\"\n", fm_err_str(e), a.title, a.desc, a.nb, m.n, a.wiki);
  TEST_CHECK(e == FM_OK && a.nb > 5 && m.n > 0);
  wk_article_free(&a);
  wk_media_free(&m);
  e = rd_fruit_list("an", &l, NULL);
  printf("  fruits with \"an\": %s, %d (first %s)\n", fm_err_str(e), l.n, l.n ? l.it[0].title : l.error);
  TEST_CHECK(e == FM_OK && l.n > 2);
  wk_list_free(&l);
  e = rd_fruit("Mango", &a, NULL);
  printf("  fruit Mango: %s, %d blocks, \"%s\"\n", fm_err_str(e), a.nb, a.nb > 2 ? a.b[2].s : "");
  TEST_CHECK(e == FM_OK && a.nb >= 8);
  wk_article_free(&a);
  e = rd_dict("hello", &a, &m, NULL);
  printf("  dictionary \"hello\": %s, %d blocks, %d sounds %s\n", fm_err_str(e), a.nb, m.n, a.error);
  if (e != FM_OK) printf("    (the dictionary service may be down: not counted as a failure)\n");
  wk_article_free(&a);
  wk_media_free(&m);
  static const char *const kWikis[][2] = { { "en.wiktionary.org", "apple" }, { "en.wikivoyage.org", "Cebu City" },
                                           { "en.wikibooks.org", "Cookbook:Adobo" }, { "en.wikisource.org", "The Raven (Poe)" },
                                           { "species.wikimedia.org", "Mangifera indica" } };
  for (int i = 0; i < FM_COUNT(kWikis); i++) {
    e = wk_article(kWikis[i][0], kWikis[i][1], &a, NULL);
    printf("  %s \"%s\": %s, %d blocks %s\n", kWikis[i][0], kWikis[i][1], fm_err_str(e), a.nb, a.error);
    TEST_CHECK(e == FM_OK && a.nb > 0);
    wk_article_free(&a);
  }
}

static void dt_offline(void);

int test_reading(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  rt_offline();
  dt_offline();
  rt_live();
  return g_test_fail - before;
}

/* ---- fdoc: HTML into blocks ---------------------------------------------------------------- */

static const char kHtml[] =
  "<html><head><title>x</title><style>p{color:red}</style></head><body>"
  "<div class=\"navbox\"><p>NAV</p></div><h2 id=\"Top\">Intro <span class=\"mw-editsection\">edit</span></h2>"
  "<p>Hello <b>bold</b> and <i>it</i> &amp; <a href=\"/wiki/Mango\">mango</a><sup class=\"reference\">[1]</sup>.</p>"
  "<ul><li>one<p>inner</p></li><li>two</li></ul><ol><li>first</li></ol>"
  "<pre>int  main()\n{\n}</pre><p>x<img src=\"//up/x/16px-flag.png\" width=\"16\" height=\"12\">y</p>"
  "<figure><a href=\"/wiki/File:A.jpg\"><img src=\"//up/a/250px-A.jpg\" width=\"250\" height=\"180\"></a>"
  "<figcaption>A caption</figcaption></figure>"
  "<table class=\"infobox\"><tr><th>Capital</th><td>Cebu City<table><tr><td>a</td><td>b</td></tr><tr><td>c</td></tr></table></td></tr>"
  "<tr><td colspan=\"2\">wide</td></tr></table>"
  "<p style=\"display:none\">HIDDEN</p><p>E = <img class=\"mwe-math-fallback-image-inline\" alt=\"{\\displaystyle mc^{2}}\" src=\"//m.svg\"></p>"
  "</body></html>";

static void dt_offline(void) {
  FmDoc d;
  doc_parse(kHtml, sizeof kHtml - 1, "https://en.wikipedia.org/wiki/", DOC_WIKI, &d);
  char t[300];
  int kinds[16] = { 0 };
  for (int i = 0; i < d.nb; i++) kinds[d.b[i].kind]++;
  TEST_CHECK(kinds[DB_H] == 1 && kinds[DB_LI] == 3 && kinds[DB_PRE] == 1 && kinds[DB_IMG] == 1 && kinds[DB_CAPTION] == 1 &&
             kinds[DB_TABLE] == 1);
  doc_block_text(&d, 0, t, sizeof t);
  TEST_CHECK(!strcmp(t, "Intro"));                                   /* no edit link */
  doc_block_text(&d, 1, t, sizeof t);
  TEST_CHECK(!strcmp(t, "Hello bold and it & mango."));               /* no reference marker */
  bool bold = false, ital = false, link = false;
  for (int r = d.b[1].run0; r < d.b[1].run0 + d.b[1].nrun; r++) {
    bold |= (d.runs[r].st & DS_BOLD) != 0;
    ital |= (d.runs[r].st & DS_ITALIC) != 0;
    if (d.runs[r].st & DS_LINK) link = !strcmp(d.links[d.runs[r].link], "/wiki/Mango");
  }
  TEST_CHECK(bold && ital && link);
  TEST_CHECK(d.nb > 3 && d.b[2].kind == DB_LI && !strcmp(d.b[2].marker, "\xE2\x80\xA2"));
  doc_block_text(&d, 2, t, sizeof t);
  TEST_CHECK(!strcmp(t, "one inner"));                                /* a paragraph inside an item stays in it */
  TEST_CHECK(d.nb > 4 && !strcmp(d.b[4].marker, "1."));
  int pre = -1, img = -1, tab = -1;
  for (int i = 0; i < d.nb; i++) {
    if (d.b[i].kind == DB_PRE) pre = i;
    if (d.b[i].kind == DB_IMG) img = i;
    if (d.b[i].kind == DB_TABLE) tab = i;
  }
  TEST_CHECK(pre >= 0 && d.runs[d.b[pre].run0].len == 15 && !strncmp(d.text + d.runs[d.b[pre].run0].off, "int  main()\n{\n}", 15));
  TEST_CHECK(img >= 0 && d.nimgs == 1 && !strcmp(d.imgs[0].src, "https://up/a/250px-A.jpg") && d.imgs[0].href &&
             !strcmp(d.imgs[0].href, "/wiki/File:A.jpg") && d.imgs[0].w == 250);   /* the flag icon left out */
  if (tab >= 0) {
    DocTable *tb = &d.tabs[d.b[tab].tab];
    TEST_CHECK(tb->box && tb->cols == 2 && tb->rows == 2 && tb->ncell == 3 && d.cells[tb->cell0].head);
    char c2[100] = "";
    size_t o = 0;
    const DocCell *cl = &d.cells[tb->cell0 + 1];
    for (int r = cl->run0; r < cl->run0 + cl->nrun; r++) {
      memcpy(c2 + o, d.text + d.runs[r].off, (size_t)d.runs[r].len);
      o += (size_t)d.runs[r].len;
    }
    c2[o] = 0;
    TEST_CHECK(strstr(c2, "Cebu City") && strstr(c2, "a   b") && strstr(c2, "\nc"));   /* the nested table: rows as lines */
  }
  bool hidden = false, math = false;
  for (int i = 0; i < d.nb; i++) {
    doc_block_text(&d, i, t, sizeof t);
    hidden |= strstr(t, "HIDDEN") || strstr(t, "NAV");
    math |= strstr(t, "E = mc^{2}") != NULL;
  }
  TEST_CHECK(!hidden && math);
  TEST_CHECK(doc_anchor(&d, "Top") == 0);
  TEST_CHECK(doc_find(&d, "MANGO") == 1 && d.hl_len == 5);   /* (layout needs the fonts: checked in the app) */
  doc_free(&d);
  /* garbage never fails */
  static const char kBad[] = "<p><b>unclosed <table><td>x<tr><li>y</ul></table></i>&#xZZ;&bogus; <<<>>";
  doc_parse(kBad, sizeof kBad - 1, "", DOC_WIKI, &d);
  TEST_CHECK(d.nb >= 1);
  doc_free(&d);
}
