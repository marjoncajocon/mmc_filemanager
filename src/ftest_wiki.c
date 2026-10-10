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
  FmErr e = wk_feed("en", &fd, NULL);
  printf("  wiki feed: %s in %d ms, featured \"%s\", %d most read, picture %d, %d on this day\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), fd.tfa.title, fd.most.n, (int)fd.has_potd, fd.notd);
  TEST_CHECK(e == FM_OK && fd.has_tfa && fd.most.n > 5);
  wk_feed_free(&fd);
  WkList l;
  t0 = plat_now_ms();
  e = wk_search("en", title, 0, &l, NULL);
  printf("  wiki search: %s, %d results in %d ms, next %d, first \"%s\" (%s)\n", fm_err_str(e), l.n,
         (int)(plat_now_ms() - t0), l.next, l.n ? l.it[0].title : "", l.n ? l.it[0].desc : "");
  TEST_CHECK(e == FM_OK && l.n >= 10 && l.next > 0);
  wk_list_free(&l);
  WkArticle a;
  t0 = plat_now_ms();
  e = wk_article("en", title, &a, NULL);
  int heads = 0;
  for (int i = 0; i < a.nb; i++) heads += a.b[i].kind != WB_PARA;
  printf("  wiki article: %s in %d ms, \"%s\", %d blocks (%d headings), lead %s\n", fm_err_str(e),
         (int)(plat_now_ms() - t0), a.title, a.nb, heads, a.lead[0] ? "yes" : "no");
  TEST_CHECK(e == FM_OK && a.nb > 10 && heads > 3);
  WkMediaList m;
  e = wk_media("en", a.title, &m, NULL);
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
    e = wk_file("en", pick->file, type, &f, NULL);
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
  e = wk_langs("en", a.title, &lg, NULL);
  printf("  wiki languages: %s, %d (first %s: \"%s\")\n", fm_err_str(e), lg.n, lg.n ? lg.it[0].lang : "",
         lg.n ? lg.it[0].title : "");
  TEST_CHECK(e == FM_OK && lg.n > 5);
  wk_langs_free(&lg);
  e = wk_related("en", a.title, &l, NULL);
  printf("  wiki related: %s, %d\n", fm_err_str(e), l.n);
  TEST_CHECK(e == FM_OK && l.n > 3);
  wk_list_free(&l);
  e = wk_links("en", a.title, &l, NULL);
  printf("  wiki links: %s, %d\n", fm_err_str(e), l.n);
  TEST_CHECK(e == FM_OK && l.n > 20);
  wk_list_free(&l);
  char rt[256];
  e = wk_random("en", rt, sizeof rt, NULL);
  printf("  wiki random: %s \"%s\"\n", fm_err_str(e), rt);
  TEST_CHECK(e == FM_OK && rt[0]);
  e = wk_article("tl", "Cebu", &a, NULL);                /* another language */
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
