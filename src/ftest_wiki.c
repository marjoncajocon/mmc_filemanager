/* ftest_wiki.c -- Wikipedia (fwiki_api.c): the parsers on captured replies,
** offline; MMCFM_WIKI_TEST=<article> runs every call against Wikipedia and
** opens one picture, one video and one sound of the article in the app's
** own decoders (fetched at the size the viewers use).
*/
#include "ftest.h"
#include "flyrics.h"
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

static const char kDevArt[] =
  "{\"id\":4731022,\"title\":\"Pointers Aren't Arrows\",\"url\":\"https://dev.to/u/pointers\",\"readable_publish_date\":\"Sep 25\","
  "\"reading_time_minutes\":8,\"public_reactions_count\":50,\"tags\":\"c, cpp\",\"cover_image\":"
  "\"https://media2.dev.to/dynamic/image/width=1000,height=420,fit=cover,gravity=auto,format=auto/https%3A%2F%2Fx.png\","
  "\"user\":{\"name\":\"S <M>\"},\"body_html\":\"<p>Hi <code>int</code></p><div class=\\\"highlight js-code-highlight\\\">"
  "<pre class=\\\"highlight c\\\"><code>int x;\\n</code></pre><div class=\\\"highlight__panel js-actions-panel\\\">"
  "<p>Enter fullscreen mode</p></div></div><img src=\\\"https://media2.dev.to/dynamic/image/width=800%2Cformat=auto/https%3A%2F%2Fy.png\\\">\"}";
static const char kSoQ[] =
  "{\"items\":[{\"question_id\":1642028,\"title\":\"What is the &#39;--&gt;&#39; operator?\",\"score\":10267,\"view_count\":5,"
  "\"tags\":[\"c\",\"c++\"],\"accepted_answer_id\":2,\"answer_count\":2,\"link\":\"https://stackoverflow.com/questions/1642028/x\","
  "\"owner\":{\"display_name\":\"GMan&#39;s\"},\"body\":\"<p>Why?</p><pre><code>x --&gt; 0\\n</code></pre>\","
  "\"answers\":[{\"is_accepted\":false,\"score\":99,\"owner\":{\"display_name\":\"B\"},\"body\":\"<p>second</p>\"},"
  "{\"is_accepted\":true,\"score\":3,\"owner\":{\"display_name\":\"A\"},\"body\":\"<p>first</p>\"}]}],\"has_more\":false}";

/* dev.to and Stack Overflow: pages from their JSON (no network). */
static void web_offline(void) {
  WkArticle a;
  memset(&a, 0, sizeof a);
  TEST_CHECK(rd_devto_parse(kDevArt, sizeof kDevArt - 1, &a) == FM_OK && a.doc);
  TEST_CHECK(!strcmp(a.id, "4731022") && !strcmp(a.title, "Pointers Aren't Arrows") && strstr(a.desc, "8 min read"));
  TEST_CHECK(strstr(a.lead, "format=jpeg") && !strstr(a.lead, "format=auto"));          /* no WebP */
  if (a.doc) {
    int pre = -1;
    for (int i = 0; i < a.doc->nb; i++)
      if (a.doc->b[i].kind == DB_PRE) pre = i;
    char t[200];
    if (pre >= 0) doc_block_text(a.doc, pre, t, sizeof t);
    TEST_CHECK(pre >= 0 && !strcmp(t, "int x;"));                                          /* no trailing empty line */
    TEST_CHECK(!strstr(a.doc->text, "fullscreen") && strstr(a.doc->text, "S <M>"));      /* panel out; the name escaped and back */
    TEST_CHECK(a.doc->nimgs == 1 && strstr(a.doc->imgs[0].src, "format=jpeg"));
  }
  wk_article_free(&a);
  memset(&a, 0, sizeof a);
  TEST_CHECK(rd_so_parse(kSoQ, sizeof kSoQ - 1, &a) == FM_OK && a.doc);
  TEST_CHECK(!strcmp(a.title, "What is the '-->' operator?") && !strcmp(a.id, "1642028") && strstr(a.desc, "Asked by GMan's") &&
             strstr(a.url, "/questions/1642028"));
  if (a.doc) {
    const char *first = strstr(a.doc->text, "first"), *second = strstr(a.doc->text, "second");
    TEST_CHECK(first && second && first < second);                                        /* the accepted answer first */
    TEST_CHECK(strstr(a.doc->text, "x --> 0") && strstr(a.doc->text, "2 answers") && strstr(a.doc->text, "CC BY-SA"));
  }
  wk_article_free(&a);
  char tg[32];
  rd_devto_tag("C++", tg, sizeof tg);
  TEST_CHECK(!strcmp(tg, "cpp"));
  rd_devto_tag("C#", tg, sizeof tg);
  TEST_CHECK(!strcmp(tg, "csharp"));
  rd_devto_tag("Data Structures!", tg, sizeof tg);
  TEST_CHECK(!strcmp(tg, "datastructures"));
}

static void rt_offline(void) {
  web_offline();
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
  /* the HTML edition (a.doc) when there is one, else the text */
  int ch = 0, nb = a.doc ? a.doc->nb : a.nb;
  for (int i = 0; i < nb; i++) ch += a.doc ? a.doc->b[i].kind == DB_H : a.b[i].kind != WB_PARA;
  printf("  book 20228: %s in %d ms, \"%s\" by %s, %d blocks%s, %d headings %s\n", fm_err_str(e), (int)(plat_now_ms() - t0), a.title,
         a.desc, nb, a.doc ? " (HTML)" : "", ch, a.error);
  TEST_CHECK(e == FM_OK && nb > 500 && ch > 20);
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
  /* DEV: a tag's list and its first article */
  e = rd_devto_list("c", 1, &l, NULL);
  printf("  dev.to tag c: %s, %d articles, next %d %s\n", fm_err_str(e), l.n, l.next, l.error);
  TEST_CHECK(e == FM_OK && l.n >= 10);
  if (l.n) {
    e = rd_devto_article(l.it[0].id, &a, NULL);
    printf("  dev.to \"%s\": %s, %d blocks %s\n", a.title, fm_err_str(e), a.doc ? a.doc->nb : 0, a.error);
    TEST_CHECK(e == FM_OK && a.doc && a.doc->nb > 3);
    wk_article_free(&a);
  }
  wk_list_free(&l);
  /* Stack Overflow (2 of the 300 requests a day): a tag's list, a question with answers */
  e = rd_so_list("c", 1, &l, NULL);
  printf("  stackoverflow tag c: %s, %d questions, next %d %s\n", fm_err_str(e), l.n, l.next, l.error);
  TEST_CHECK(e == FM_OK && l.n >= 10 && l.next == 2);
  wk_list_free(&l);
  e = rd_so_question("1642028", &a, NULL);
  printf("  stackoverflow 1642028 \"%s\": %s, %d blocks %s\n", a.title, fm_err_str(e), a.doc ? a.doc->nb : 0, a.error);
  TEST_CHECK(e == FM_OK && a.doc && a.doc->nb > 20);
  wk_article_free(&a);
}

static void dt_offline(void);

/* ---- lyrics (flyrics.c): LRCLIB replies and titles ------------------------------------------ */

static const char kLrc[] =
  "{\"id\":1,\"trackName\":\"Hello\",\"artistName\":\"Adele\",\"instrumental\":false,"
  "\"plainLyrics\":\"Hello, it's me\\nI was wondering\","
  "\"syncedLyrics\":\"[ar:Adele]\\n[00:13.50] Hello, it's me\\n[00:19.10] I was wondering\\n[00:05.00]\\n"
  "[01:00.00][02:00.25] Chorus line\"}";

static void lt_offline(void) {
  TEST_CHECK(lyrics_parse(kLrc, sizeof kLrc - 1) == LY_SYNCED);
  double t0 = 0, t1 = 0, t4 = 0;
  const char *l0 = lyrics_line(0, &t0), *l1 = lyrics_line(1, &t1), *l4 = lyrics_line(4, &t4);
  /* sorted by time: the empty 0:05 line first, a line with two stamps twice, the [ar:] tag skipped */
  TEST_CHECK(lyrics_count() == 5 && l0 && !l0[0] && t0 == 5.0 && l1 && !strcmp(l1, "Hello, it's me") && t1 == 13.5);
  TEST_CHECK(l4 && !strcmp(l4, "Chorus line") && t4 > 120.2 && t4 < 120.3);
  TEST_CHECK(!strcmp(lyrics_song(), "Adele - Hello"));
  /* no synced lyrics: the plain ones, without times */
  static const char kPlain[] = "{\"trackName\":\"X\",\"artistName\":\"Y\",\"plainLyrics\":\"a\\nb\",\"syncedLyrics\":null}";
  double tp = 0;
  TEST_CHECK(lyrics_parse(kPlain, sizeof kPlain - 1) == LY_PLAIN && lyrics_count() == 2 && lyrics_line(1, &tp) && tp < 0);
  static const char kInstr[] = "{\"trackName\":\"X\",\"artistName\":\"Y\",\"instrumental\":true}";
  TEST_CHECK(lyrics_parse(kInstr, sizeof kInstr - 1) == LY_MISSING);
  /* titles as videos and files name them */
  static const char *const kCase[][4] = {
    { "", "Adele - Hello (Official Music Video)", "Adele", "Hello" },
    { "AdeleVEVO", "Hello [HD]", "Adele", "Hello" },
    { "Bruno Mars - Topic", "Uptown Funk (feat. Bruno Mars)", "Bruno Mars", "Uptown Funk" },
    { "", "Ed Sheeran - Perfect.mp3", "Ed Sheeran", "Perfect" },
    { "x", "Coldplay - Yellow | Lyrics", "Coldplay", "Yellow" },
    { "", "Mark Ronson - Uptown Funk ft. Bruno Mars", "Mark Ronson", "Uptown Funk" },
  };
  for (int i = 0; i < FM_COUNT(kCase); i++) {
    char a[200], t[300];
    lyrics_clean(kCase[i][0], kCase[i][1], a, sizeof a, t, sizeof t);
    bool ok = !strcmp(a, kCase[i][2]) && !strcmp(t, kCase[i][3]);
    if (!ok) printf("    lyrics_clean(\"%s\", \"%s\") = \"%s\" / \"%s\"\n", kCase[i][0], kCase[i][1], a, t);
    TEST_CHECK(ok);
  }
}

/* ---- the Bible and Merriam-Webster (fwiki_more.c) ------------------------------------------ */

static const char kChapter[] =
  "{\"translation\":{\"id\":\"BSB\",\"name\":\"Berean Standard Bible\",\"website\":\"https://berean.bible/\"},"
  "\"book\":{\"id\":\"JHN\",\"name\":\"John\",\"numberOfChapters\":21},"
  "\"chapter\":{\"number\":3,\"content\":[{\"type\":\"heading\",\"content\":[\"Jesus and Nicodemus\"]},"
  "{\"type\":\"verse\",\"number\":1,\"content\":[\"Now there was a man.\"]},{\"type\":\"line_break\"},"
  "{\"type\":\"verse\",\"number\":16,\"content\":[\"He gave His one and only\",{\"noteId\":17},\"Son.\",{\"noteId\":18},\"\u201d\"]}]},"
  "\"previousChapterApiLink\":\"/api/BSB/JHN/2.json\",\"nextChapterApiLink\":\"/api/BSB/JHN/4.json\","
  "\"thisChapterAudioLinks\":{\"david\":\"https://audio.example/david.mp3\",\"bad\":\"http://plain/x.mp3\"}}";

static const char kMw[] =
  "[{\"meta\":{\"id\":\"love:1\"},\"hwi\":{\"hw\":\"love\",\"prs\":[{\"mw\":\"\u02c8l\u0259v\",\"sound\":{\"audio\":\"love0001\"}}]},"
  "\"fl\":\"noun\",\"def\":[{\"sseq\":[[[\"sense\",{\"dt\":[[\"text\",\"{bc}strong affection\"],"
  "[\"vis\",[{\"t\":\"maternal {it}love{/it} for a child\"}]]]}]]]}],"
  "\"et\":[[\"text\",\"Middle English, from Old English {it}lufu{/it}\"]],\"date\":\"before the 12th century{ds||1||}\","
  "\"shortdef\":[\"strong affection for another\",\"warm attachment\"]},"
  "{\"meta\":{\"id\":\"love:2\"},\"hwi\":{\"hw\":\"love\"},\"fl\":\"verb\",\"shortdef\":[\"to hold dear\"]}]";

static void bt_offline(void) {
  char book[80];
  int ch, vs;
  rd_bible_ref("1 Corinthians 13:4", book, sizeof book, &ch, &vs);
  TEST_CHECK(!strcmp(book, "1 Corinthians") && ch == 13 && vs == 4);
  rd_bible_ref("Psalm 23", book, sizeof book, &ch, &vs);
  TEST_CHECK(!strcmp(book, "Psalm") && ch == 23 && vs == 0);
  rd_bible_ref("Genesis", book, sizeof book, &ch, &vs);
  TEST_CHECK(!strcmp(book, "Genesis") && ch == 0);
  WkArticle a;
  WkMediaList m;
  memset(&a, 0, sizeof a);
  memset(&m, 0, sizeof m);
  TEST_CHECK(rd_bible_parse_chapter(kChapter, sizeof kChapter - 1, "BSB/JHN/3", &a, &m) == FM_OK && a.doc);
  TEST_CHECK(!strcmp(a.title, "John 3") && !strcmp(a.desc, "Berean Standard Bible") && !strcmp(a.id, "BSB/JHN/3"));
  if (a.doc) {
    TEST_CHECK(strstr(a.doc->text, "one and only Son.\u201d") != NULL);             /* the notes' places: a space between words only */
    TEST_CHECK(doc_anchor(a.doc, "v16") >= 0 && strstr(a.doc->text, "Jesus and Nicodemus"));
    bool prev = false, next = false, ch21 = false;
    for (int i = 0; i < a.doc->nlinks; i++) {
      prev |= !strcmp(a.doc->links[i], "bible:BSB/JHN/2");
      next |= !strcmp(a.doc->links[i], "bible:BSB/JHN/4");
      ch21 |= !strcmp(a.doc->links[i], "bible:BSB/JHN/21");
    }
    TEST_CHECK(prev && next && ch21);
  }
  TEST_CHECK(m.n == 1 && !strcmp(m.it[0].url, "https://audio.example/david.mp3") && strstr(m.it[0].caption, "David"));
  wk_article_free(&a);
  wk_media_free(&m);
  /* Merriam-Webster */
  char mk[300];
  rd_mw_markup("{bc}a {it}feeling{/it} of {sx|affection||} {ldquo}x{rdquo} {a_link|warmth} {dx}see {dxt|like||}{/dx} 1<2", mk, sizeof mk);
  TEST_CHECK(!strcmp(mk, "<b>:</b> a <i>feeling</i> of affection \u201cx\u201d warmth \u2014 see like 1&lt;2"));
  if (strcmp(mk, "<b>:</b> a <i>feeling</i> of affection \u201cx\u201d warmth \u2014 see like 1&lt;2")) printf("    markup: %s\n", mk);
  memset(&a, 0, sizeof a);
  memset(&m, 0, sizeof m);
  TEST_CHECK(rd_mw_parse(kMw, sizeof kMw - 1, "love", &a, &m) == FM_OK && a.doc);
  TEST_CHECK(m.n == 1 && !strcmp(m.it[0].url, "https://media.merriam-webster.com/audio/prons/en/us/mp3/l/love0001.mp3"));
  if (a.doc) {
    TEST_CHECK(strstr(a.doc->text, "strong affection for another") && strstr(a.doc->text, "to hold dear") &&
               strstr(a.doc->text, "maternal love for a child") && strstr(a.doc->text, "Etymology") &&
               strstr(a.doc->text, "before the 12th century") && strstr(a.doc->text, "Merriam-Webster"));
  }
  TEST_CHECK(strstr(a.desc, "noun") != NULL);
  wk_article_free(&a);
  wk_media_free(&m);
  static const char kSugg[] = "[\"lover\",\"glove\"]";
  memset(&a, 0, sizeof a);
  TEST_CHECK(rd_mw_parse(kSugg, sizeof kSugg - 1, "lovr", &a, NULL) == FM_OK && a.doc && a.doc->nlinks == 2 &&
             !strcmp(a.doc->links[0], "mw:lover"));
  wk_article_free(&a);
  memset(&a, 0, sizeof a);
  static const char kBadKey[] = "Invalid API key. Not subscribed for this reference.";
  TEST_CHECK(rd_mw_parse(kBadKey, sizeof kBadKey - 1, "love", &a, NULL) != FM_OK && strstr(a.error, "key"));
  wk_article_free(&a);
}

/* MMCFM_READ_TEST=1: the Bible against its site (MMCFM_MW_KEY: Merriam-Webster too). */
static void bt_live(void) {
  if (!getenv("MMCFM_READ_TEST")) return;
  WkList l;
  FmErr e = rd_bible_find("tgl_ulb", "John 3:16", &l, NULL);
  printf("  bible tgl \"John 3:16\": %s, %d: %s %s\n", fm_err_str(e), l.n, l.n ? l.it[0].id : "", l.error);
  TEST_CHECK(e == FM_OK && l.n >= 1 && !strcmp(l.it[0].id, "tgl_ulb/JHN/3#16"));
  wk_list_free(&l);
  WkArticle a;
  WkMediaList m;
  e = rd_bible_chapter("BSB/JHN/3", &a, &m, NULL);
  printf("  bible BSB/JHN/3: %s, \"%s\", %d blocks, %d narrations %s\n", fm_err_str(e), a.title, a.doc ? a.doc->nb : 0, m.n, a.error);
  TEST_CHECK(e == FM_OK && a.doc && a.doc->nb > 5 && m.n > 0);
  wk_article_free(&a);
  wk_media_free(&m);
  const char *key = getenv("MMCFM_MW_KEY");
  if (key && *key) {
    e = rd_mw(key, "love", &a, &m, NULL);
    printf("  merriam-webster \"love\": %s, %d blocks, %d sounds %s\n", fm_err_str(e), a.doc ? a.doc->nb : 0, m.n, a.error);
    TEST_CHECK(e == FM_OK && a.doc && m.n > 0);
    wk_article_free(&a);
    wk_media_free(&m);
  }
}

int test_reading(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  bt_offline();
  bt_live();
  lt_offline();
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
  /* a paragraph that starts with code keeps the space after it (Stack Overflow) */
  static const char kCodeFirst[] = "<p><small>by A</small></p><p><code>--&gt;</code> is not an operator.</p>";
  doc_parse(kCodeFirst, sizeof kCodeFirst - 1, "", DOC_WEB, &d);
  char t2[120] = "";
  if (d.nb >= 2) doc_block_text(&d, 1, t2, sizeof t2);
  TEST_CHECK(!strcmp(t2, "--> is not an operator."));
  if (strcmp(t2, "--> is not an operator.")) printf("    got \"%s\"\n", t2);
  doc_free(&d);
  /* a chess diagram: the board, its pieces laid over it, no a-h / 1-8 labels, the caption kept */
  static const char kChess[] =
    "<p>Before.</p><div class=\"chessboard thumb\"><div class=\"thumbinner\"><table><tr><td></td><td>a</td><td>b</td></tr>"
    "<tr><td>8</td><td colspan=\"8\" rowspan=\"8\"><div class=\"chess-pieces notheme\" style=\"position:relative\">"
    "<span><img src=\"//up/Chessboard480.svg.png\" width=\"208\" height=\"208\"></span>"
    "<div style=\"top:0px;left:26px\"><span><img alt=\"b8 black knight\" src=\"//up/Chess_ndt45.png\" width=\"26\" height=\"26\"></span></div>"
    "<div style=\"top:182px;left:104px\"><span><img alt=\"e1 white king\" src=\"//up/Chess_klt45.png\" width=\"26\" height=\"26\"></span></div>"
    "</div></td><td>8</td></tr></table><div class=\"thumbcaption\">Position after 2.c4</div></div></div><p>After.</p>"
    /* a map with a dot at 50% / 25%, 4 px back to centre it */
    "<div style=\"position:relative\"><img src=\"//up/map.png\" width=\"200\" height=\"100\">"
    "<div style=\"top:25%;left:50%\"><div style=\"top:-4px;left:-4px\"><img src=\"//up/dot.png\" width=\"8\" height=\"8\"></div></div></div>";
  doc_parse(kChess, sizeof kChess - 1, "https://en.wikipedia.org/wiki/", DOC_WIKI, &d);
  int boards = 0, pieces = 0, caps = 0;
  bool labels = false, at = false, dot = false;
  for (int i = 0; i < d.nb; i++) {
    doc_block_text(&d, i, t2, sizeof t2);
    if (d.b[i].kind == DB_IMG && d.imgs[d.b[i].img].has_layers) boards++;
    if (d.b[i].kind == DB_CAPTION && !strcmp(t2, "Position after 2.c4")) caps++;
    if (strstr(t2, "a") == t2 && strlen(t2) < 4) labels = true;
    if (!strcmp(t2, "8") || !strcmp(t2, "a b")) labels = true;
  }
  for (int i = 0; i < d.nimgs; i++) {
    const DocImage *im = &d.imgs[i];
    if (im->layer_of >= 0 && strstr(im->src, "Chess_")) pieces++;
    if (strstr(im->src, "klt45") && im->lx == 104 && im->ly == 182) at = true;
    if (strstr(im->src, "dot.png") && im->layer_of >= 0 && im->lx == 96 && im->ly == 21) dot = true;
  }
  TEST_CHECK(boards == 2 && pieces == 2 && at && dot && caps == 1 && !labels);
  if (!(boards == 2 && pieces == 2 && at && dot && caps == 1 && !labels))
    printf("    chess: %d boards, %d pieces, at %d, dot %d, %d captions, labels %d\n", boards, pieces, at, dot, caps, labels);
  doc_free(&d);
}
