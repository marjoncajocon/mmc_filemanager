/* ftest_hls.c -- HLS playlists (fhls.c), the HLS stream reader and the native
** Dailymotion resolve.
**
** Offline by default: URL resolution (RFC 3986 examples plus Dailymotion's
** "../../../frag(1)/" segment names), master and media playlists (TS, fMP4
** with #EXT-X-MAP, byte ranges, keys, live), AES-128 segment decryption,
** the Dailymotion metadata/master parsers and the quality flags, and garbage.
** Opt-in, with the network:
**   MMCFM_HLS_TEST=<m3u8 url>[|MB]  reads MB (default 4) through ns_open and
**       checks the container (TS sync byte 0x47 every 188 bytes, or fMP4 box
**       chain), seeks back and forward exactly, and opens again at #t=
**   MMCFM_DM_TEST=<id>[|<id>...]    native resolve (no yt-dlp), the quality
**       list, then vid_open on the stream: time to the first frame, a seek
*/
#include "ftest.h"
#include "fhls.h"
#include "fnetstream.h"
#include "fvsrc_int.h"
#include "fdec_vid.h"
#include "fplat.h"

#define TXT(s) s, strlen(s)               /* a literal and its length */

/* ---- URLs -------------------------------------------------------------------------- */

static void check_res(const char *base, const char *ref, const char *want) {
  char out[512];
  hls_resolve(base, ref, out, sizeof out);
  if (strcmp(out, want)) printf("  resolve \"%s\" + \"%s\" = \"%s\" (want \"%s\")\n", base, ref, out, want);
  TEST_CHECK(!strcmp(out, want));
}

static void th_urls(void) {
  /* RFC 3986 5.4.1 */
  const char *b = "http://a/b/c/d;p?q";
  check_res(b, "g", "http://a/b/c/g");
  check_res(b, "./g", "http://a/b/c/g");
  check_res(b, "g/", "http://a/b/c/g/");
  check_res(b, "/g", "http://a/g");
  check_res(b, "//g", "http://g");
  check_res(b, "?y", "http://a/b/c/d;p?y");
  check_res(b, "g?y", "http://a/b/c/g?y");
  check_res(b, "g#s", "http://a/b/c/g");                 /* fragments are dropped */
  check_res(b, ";x", "http://a/b/c/;x");
  check_res(b, "", "http://a/b/c/d;p?q");
  check_res(b, ".", "http://a/b/c/");
  check_res(b, "./", "http://a/b/c/");
  check_res(b, "..", "http://a/b/");
  check_res(b, "../", "http://a/b/");
  check_res(b, "../g", "http://a/b/g");
  check_res(b, "../..", "http://a/");
  check_res(b, "../../g", "http://a/g");
  check_res(b, "../../../g", "http://a/g");               /* 5.4.2: no climbing above the root */
  check_res(b, "/./g", "http://a/g");
  check_res(b, "/../g", "http://a/g");
  check_res(b, "g.", "http://a/b/c/g.");
  check_res(b, "..g", "http://a/b/c/..g");
  check_res(b, "./../g", "http://a/b/g");
  check_res(b, "g/./h", "http://a/b/c/g/h");
  check_res(b, "g/../h", "http://a/b/c/h");
  check_res(b, "g?y/./x", "http://a/b/c/g?y/./x");
  /* the shapes HLS servers use */
  check_res("https://vod3.cf.dmcdn.net/sec2(Ab-c_)/video/176/884/8488671_mp4_h264_aac.m3u8#cell=cf3",
            "../../../frag(1)/video/176/884/8488671_mp4_h264_aac.ts",
            "https://vod3.cf.dmcdn.net/sec2(Ab-c_)/frag(1)/video/176/884/8488671_mp4_h264_aac.ts");
  check_res("https://h.example/v/fmp4/1/manifest.m3u8?sig=x", "0.m4s", "https://h.example/v/fmp4/1/0.m4s");
  check_res("https://h.example/v/a.m3u8", "https://cdn.example/s/1.ts?t=9#frag", "https://cdn.example/s/1.ts?t=9");
  check_res("https://h.example", "a.ts", "https://h.example/a.ts");
  check_res("https://h.example/x/a.m3u8", "//c.example/b.ts", "https://c.example/b.ts");
  char tiny[8];
  hls_resolve("https://h.example/", "long-name.ts", tiny, sizeof tiny);
  TEST_CHECK(tiny[0] == 0);                              /* does not fit: empty, not cut */

  TEST_CHECK(hls_url_like("https://x/a/manifest.m3u8"));
  TEST_CHECK(hls_url_like("https://x/a/MASTER.M3U8?sec=1#cell=cf3"));
  TEST_CHECK(hls_url_like("https://x/list.m3u"));
  TEST_CHECK(!hls_url_like("https://x/a.mp4?u=b.m3u8"));
  TEST_CHECK(!hls_url_like("https://x/a.m3u8x"));
  TEST_CHECK(hls_type_like("application/vnd.apple.mpegurl"));
  TEST_CHECK(hls_type_like("audio/x-mpegURL; charset=utf-8"));
  TEST_CHECK(!hls_type_like("video/mp4") && !hls_type_like(NULL));
  TEST_CHECK(hls_sniff("\xEF\xBB\xBF  #EXTM3U\n", 12));
  TEST_CHECK(!hls_sniff("#EXTM3", 6) && !hls_sniff("<html>", 6));
}

/* ---- playlists --------------------------------------------------------------------- */

/* the shape of Dailymotion's master (2026-10): no FRAME-RATE, a subtitle group */
static const char *kMaster =
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=836280,CODECS=\"mp4a.40.2,avc1.64001f\",RESOLUTION=848x480,NAME=\"480\","
    "SUBTITLES=\"subtitles\"\n"
    "https://vod3.cf.dmcdn.net/sec2(AAA)/video/fmp4/698651490/h264_aac_hq/3/manifest.m3u8#cell=cf3\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=460560,CODECS=\"mp4a.40.2,avc1.42001e\",RESOLUTION=512x288,NAME=\"380\","
    "SUBTITLES=\"subtitles\"\n"
    "https://vod3.cf.dmcdn.net/sec2(BBB)/video/fmp4/698651490/h264_aac/3/manifest.m3u8#cell=cf3\n"
    "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subtitles\",NAME=\"Fran\xC3\xA7" "ais, auto\",URI=\"https://x/s.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=6000000,CODECS=\"mp4a.40.2,avc1.640028\",RESOLUTION=1920x1080,FRAME-RATE=59.940,"
    "NAME=\"1080\"\r\n"
    "hd/1080.m3u8\r\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS=\"mp4a.40.2,hvc1.1.6.L120.90\",RESOLUTION=1280x720,NAME=\"720h\"\n"
    "hd/720-hevc.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"mp4a.40.2,avc1.64001f\",RESOLUTION=1280x720,NAME=\"720\"\n"
    "hd/720.m3u8\n"
    "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=90000,URI=\"iframes.m3u8\"\n";

static const char *kMasterAudio =
    "#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=NO,URI=\"a/en2.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en main\",DEFAULT=YES,URI=\"a/en.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360,AUDIO=\"aud\"\n"
    "v/360.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=300000\n"
    "v/low.m3u8\n";

static void th_master(void) {
  FmHlsMaster *m = (FmHlsMaster *)fm_alloc(sizeof *m);
  const char *base = "https://cdndirector.dailymotion.com/cdn/manifest/video/x9.m3u8?sec=abc";
  char err[128];
  TEST_CHECK(hls_kind(kMaster, strlen(kMaster)) == HLS_MASTER);
  TEST_CHECK(hls_parse_master(kMaster, strlen(kMaster), base, m, err, sizeof err) == FM_OK);
  TEST_CHECK(m->n == 5);
  if (m->n == 5) {
    TEST_CHECK(!strcmp(m->v[0].uri, "https://vod3.cf.dmcdn.net/sec2(AAA)/video/fmp4/698651490/h264_aac_hq/3/"
                                     "manifest.m3u8"));
    TEST_CHECK(m->v[0].width == 848 && m->v[0].height == 480 && m->v[0].bandwidth == 836280);
    TEST_CHECK(!strcmp(m->v[0].codecs, "mp4a.40.2,avc1.64001f") && !strcmp(m->v[0].name, "480"));
    TEST_CHECK(!strcmp(m->v[2].uri, "https://cdndirector.dailymotion.com/cdn/manifest/video/hd/1080.m3u8"));
    TEST_CHECK(m->v[2].fps > 59.9 && m->v[2].fps < 60 && m->v[2].height == 1080);
    TEST_CHECK(!m->v[0].audio_uri);
    TEST_CHECK(m->v[hls_pick_variant(m, 1080)].height == 1080);
    int i720 = hls_pick_variant(m, 720);                  /* H.264 wins the tie with HEVC */
    TEST_CHECK(i720 == 4);
    TEST_CHECK(hls_pick_variant(m, 480) == 0);
    TEST_CHECK(hls_pick_variant(m, 144) == 1);            /* nothing fits: the smallest */
  }
  hls_master_free(m);

  TEST_CHECK(hls_parse_master(kMasterAudio, strlen(kMasterAudio), "http://h/m/master.m3u8", m, err, sizeof err) ==
             FM_OK);
  TEST_CHECK(m->n == 2);
  if (m->n == 2) {
    TEST_CHECK(m->v[0].audio_uri && !strcmp(m->v[0].audio_uri, "http://h/m/a/en.m3u8"));   /* DEFAULT=YES */
    TEST_CHECK(!m->v[1].audio_uri && m->v[1].height == 0);
    TEST_CHECK(hls_pick_variant(m, 720) == 0);
  }
  hls_master_free(m);

  /* a media playlist is not a master, and the other way round */
  TEST_CHECK(hls_parse_master(TXT("#EXTM3U\n#EXTINF:3,\na.ts\n"), "http://h/", m, err, sizeof err) == FM_ERR_FORMAT);
  TEST_CHECK(hls_parse_master(TXT("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n"), "http://h/", m, err, sizeof err) ==
             FM_ERR_FORMAT && err[0]);
  fm_free(m);
}

static const char *kFmp4 =
    "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:3\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
    "#EXT-X-MAP:URI=\"init.mp4\"\n"
    "#EXTINF:3.000000,\n0.m4s\n#EXTINF:3.000000,\n1.m4s\n#EXTINF:3.000000,\n2.m4s\n#EXTINF:0.320000,\n3.m4s\n"
    "#EXT-X-ENDLIST\n";

static const char *kTsKeys =
    "#EXTM3U\n#EXT-X-TARGETDURATION:20\n#EXT-X-MEDIA-SEQUENCE:7\n"
    "#EXTINF:10.0,\n../../../frag(1)/v.ts\n"
    "#EXT-X-KEY:METHOD=AES-128,URI=\"https://k.example/key?id=1\",IV=0x000102030405060708090a0b0c0d0e0f\n"
    "#EXTINF:17.16,\n../../../frag(2)/v.ts\n"
    "#EXT-X-KEY:METHOD=AES-128,URI=\"key2\"\n"
    "#EXT-X-DISCONTINUITY\n"
    "#EXTINF:10.0,title\n#EXT-X-BYTERANGE:1000@500\nall.ts\n"
    "#EXT-X-BYTERANGE:200\n#EXTINF:5,\nall.ts\n"
    "#EXT-X-KEY:METHOD=NONE\n"
    "#EXTINF:5,\nlast.ts\n"
    "#EXT-X-ENDLIST\n";

static void th_media(void) {
  FmHlsMedia p;
  char err[128];
  const char *base = "https://vod3.cf.dmcdn.net/sec2(x)/video/fmp4/1/h264_aac/3/manifest.m3u8";
  TEST_CHECK(hls_parse_media(kFmp4, strlen(kFmp4), base, &p, err, sizeof err) == FM_OK);
  TEST_CHECK(p.n == 4 && p.endlist && p.vod && p.target == 3 && p.first_seq == 0);
  TEST_CHECK(p.map_uri && !strcmp(p.map_uri, "https://vod3.cf.dmcdn.net/sec2(x)/video/fmp4/1/h264_aac/3/init.mp4"));
  TEST_CHECK(!p.map_changes && p.map_len < 0 && p.nkey == 0);
  TEST_CHECK(p.total > 9.31 && p.total < 9.33);
  TEST_CHECK(p.n == 4 && !strcmp(p.seg[3].uri, "3.m4s") && p.seg[3].start == 9 && p.seg[2].seq == 2);
  TEST_CHECK(hls_seg_at(&p, 0) == 0 && hls_seg_at(&p, 2.99) == 0 && hls_seg_at(&p, 3) == 1);
  TEST_CHECK(hls_seg_at(&p, 8.5) == 2 && hls_seg_at(&p, 100) == 3 && hls_seg_at(&p, -4) == 0);
  hls_media_free(&p);

  base = "https://vod3.cf.dmcdn.net/sec2(x)/video/176/884/8488671_mp4_h264_aac.m3u8";
  TEST_CHECK(hls_parse_media(kTsKeys, strlen(kTsKeys), base, &p, err, sizeof err) == FM_OK);
  TEST_CHECK(p.n == 5 && p.endlist && !p.vod && !p.map_uri && p.first_seq == 7 && p.nkey == 2);
  if (p.n == 5 && p.nkey == 2) {
    char u[256];
    hls_resolve(p.base, p.seg[0].uri, u, sizeof u);
    TEST_CHECK(!strcmp(u, "https://vod3.cf.dmcdn.net/sec2(x)/frag(1)/v.ts"));
    TEST_CHECK(p.seg[0].key == -1 && p.seg[1].key == 0 && p.seg[2].key == 1 && p.seg[3].key == 1 && p.seg[4].key == -1);
    TEST_CHECK(p.key[0].method == HLS_KEY_AES128 && p.key[0].has_iv && p.key[0].iv[0] == 0 && p.key[0].iv[15] == 15);
    TEST_CHECK(!strcmp(p.key[0].uri, "https://k.example/key?id=1"));
    TEST_CHECK(!p.key[1].has_iv && !strcmp(p.key[1].uri, "https://vod3.cf.dmcdn.net/sec2(x)/video/176/884/key2"));
    TEST_CHECK(p.seg[1].dur > 17.15 && p.seg[1].dur < 17.17 && p.seg[1].seq == 8);
    TEST_CHECK(p.seg[2].disc && !p.seg[1].disc && !p.seg[3].disc);
    TEST_CHECK(p.seg[2].br_off == 500 && p.seg[2].br_len == 1000);
    TEST_CHECK(p.seg[3].br_off == 1500 && p.seg[3].br_len == 200);      /* continues the previous range */
    TEST_CHECK(p.seg[4].br_len < 0 && p.seg[3].uri == p.seg[2].uri);   /* repeated names stored once */
  }
  hls_media_free(&p);

  /* live: no ENDLIST; an empty live list is fine (segments come later) */
  const char *live = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1234\n"
                     "#EXTINF:6,\ns1234.ts\n#EXTINF:6,\ns1235.ts\n";
  TEST_CHECK(hls_parse_media(live, strlen(live), "http://h/l/index.m3u8", &p, err, sizeof err) == FM_OK);
  TEST_CHECK(!p.endlist && p.n == 2 && p.first_seq == 1234 && p.seg[1].seq == 1235);
  hls_media_free(&p);
  TEST_CHECK(hls_parse_media(TXT("#EXTM3U\n#EXT-X-TARGETDURATION:6\n"), "http://h/", &p, err, sizeof err) == FM_OK &&
             p.n == 0 && !p.endlist);
  hls_media_free(&p);

  /* SAMPLE-AES is listed (the reader refuses it); a second MAP is flagged */
  const char *drm = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
                    "#EXT-X-MAP:URI=\"i1.mp4\"\n#EXTINF:4,\na.m4s\n#EXT-X-MAP:URI=\"i2.mp4\"\n#EXTINF:4,\nb.m4s\n"
                    "#EXT-X-ENDLIST\n";
  TEST_CHECK(hls_parse_media(drm, strlen(drm), "http://h/", &p, err, sizeof err) == FM_OK);
  TEST_CHECK(p.nkey == 1 && p.key[0].method == HLS_KEY_OTHER && p.seg[0].key == 0 && p.map_changes);
  hls_media_free(&p);

  /* wrong kinds and garbage */
  TEST_CHECK(hls_parse_media(kMaster, strlen(kMaster), "http://h/", &p, err, sizeof err) == FM_ERR_FORMAT && err[0]);
  TEST_CHECK(hls_parse_media(TXT("<html>#EXTINF"), "http://h/", &p, err, sizeof err) == FM_ERR_FORMAT);
  TEST_CHECK(hls_parse_media(TXT("#EXTM3U\n#EXT-X-ENDLIST\n"), "http://h/", &p, err, sizeof err) == FM_ERR_FORMAT);
  TEST_CHECK(hls_kind(NULL, 0) == HLS_NONE && hls_kind("", 0) == HLS_NONE);
  u32 seed = 12345;
  char junk[600];
  for (int round = 0; round < 300; round++) {                   /* no crash, no leak, bounded */
    size_t n = 0;
    n += (size_t)fm_snprintf(junk, sizeof junk, "#EXTM3U\n");
    while (n < sizeof junk - 1) {
      seed = seed * 1103515245u + 12345u;
      static const char *const bits[] = { "#EXTINF:", "#EXT-X-BYTERANGE:", "@", "\n", "a.ts", "#EXT-X-KEY:METHOD=",
                                          "AES-128,URI=\"", "\"", ",", "#EXT-X-MAP:URI=", "../", "-1", "99999999999999",
                                          "#EXT-X-STREAM-INF:", "RESOLUTION=", "x", "0x", "#EXT-X-MEDIA-SEQUENCE:", "\r" };
      const char *b = bits[(seed >> 16) % FM_COUNT(bits)];
      size_t k = strlen(b);
      if (n + k >= sizeof junk) break;
      memcpy(junk + n, b, k);
      n += k;
    }
    junk[n] = 0;
    if (hls_parse_media(junk, n, "http://h/a/b.m3u8", &p, NULL, 0) == FM_OK) {
      for (int i = 0; i < p.n; i++) TEST_CHECK(p.seg[i].dur >= 0 && p.seg[i].uri);
      hls_media_free(&p);
    }
    FmHlsMaster *m = (FmHlsMaster *)fm_alloc(sizeof *m);
    if (hls_parse_master(junk, n, "http://h/a/b.m3u8", m, NULL, 0) == FM_OK) {
      TEST_CHECK(m->n > 0 && hls_pick_variant(m, 720) >= 0);
      hls_master_free(m);
    }
    fm_free(m);
  }
}

/* ---- AES-128 ---------------------------------------------------------------------- */

typedef struct Collect { u8 buf[512]; size_t n; } Collect;
static bool collect(void *u, const u8 *p, size_t n) {
  Collect *c = (Collect *)u;
  if (c->n + n > sizeof c->buf) return false;
  memcpy(c->buf + c->n, p, n);
  c->n += n;
  return true;
}

static void th_aes(void) {
  u8 key[16], iv[16], plain[300], ct[320];
  for (int i = 0; i < 16; i++) { key[i] = (u8)(i * 7 + 1); iv[i] = (u8)(0xA0 + i); }
  for (int i = 0; i < 300; i++) plain[i] = (u8)(i * 13);
  for (int len = 0; len <= 300; len += (len < 40 ? 1 : 37)) {
    /* PKCS7: 1..16 bytes of padding */
    size_t padded = ((size_t)len / 16 + 1) * 16;
    memcpy(ct, plain, (size_t)len);
    memset(ct + len, (int)(padded - (size_t)len), padded - (size_t)len);
    FmAes a;
    u8 civ[16];
    memcpy(civ, iv, 16);
    aes_init(&a, key, 16);
    aes_cbc_encrypt(&a, civ, ct, padded);
    FmHlsDec d;
    Collect c;
    c.n = 0;
    hls_dec_init(&d, key, iv);
    size_t off = 0, step = 1 + (size_t)len % 23;                /* odd chunk sizes */
    while (off < padded) {
      size_t k = FM_MIN(step, padded - off);
      TEST_CHECK(hls_dec_feed(&d, ct + off, k, collect, &c));
      off += k;
    }
    TEST_CHECK(hls_dec_end(&d, collect, &c));
    TEST_CHECK(c.n == (size_t)len && !memcmp(c.buf, plain, (size_t)len));
  }
  /* a cut-off segment and a wrong key fail */
  FmHlsDec d;
  Collect c;
  c.n = 0;
  hls_dec_init(&d, key, iv);
  TEST_CHECK(hls_dec_feed(&d, ct, 20, collect, &c) && !hls_dec_end(&d, collect, &c));
  u8 bad[16];
  memset(bad, 0x55, 16);
  int fails = 0;
  for (int t = 0; t < 8; t++) {
    bad[0] = (u8)t;
    c.n = 0;
    hls_dec_init(&d, bad, iv);
    hls_dec_feed(&d, ct, 32, collect, &c);
    fails += !hls_dec_end(&d, collect, &c);
  }
  TEST_CHECK(fails >= 6);                                       /* garbage padding, nearly always */
  u8 sv[16];
  hls_seq_iv(0x0102030405LL, sv);
  TEST_CHECK(sv[0] == 0 && sv[10] == 0 && sv[11] == 1 && sv[15] == 5);
}

/* ---- Dailymotion parsers ---------------------------------------------------------- */

static void th_dailymotion(void) {
  const char *meta =
      "{\"url\":\"https:\\/\\/www.dailymotion.com\\/video\\/xbjyixu\",\"duration\":180,\"id\":\"xbjyixu\","
      "\"mode\":\"vod\",\"stream_type\":\"recorded\",\"stream_formats\":{\"380\":\"fMP4\",\"480\":\"mpegts\"},"
      "\"qualities\":{\"auto\":[{\"type\":\"application\\/x-mpegURL\",\"url\":\"https:\\/\\/cdndirector.dailymotion"
      ".com\\/cdn\\/manifest\\/video\\/xbjyixu.m3u8?sec=abc&dmTs=1&dmV1st=X\"}]}}";
  FmDmMeta m;
  char err[256];
  TEST_CHECK(vsrc_dailymotion_parse_meta(meta, strlen(meta), &m, err, sizeof err) == FM_OK);
  TEST_CHECK(!strcmp(m.master, "https://cdndirector.dailymotion.com/cdn/manifest/video/xbjyixu.m3u8?sec=abc&dmTs=1"
                               "&dmV1st=X"));
  TEST_CHECK(m.duration == 180 && !m.live);
  const char *gone = "{\"error\":{\"title\":\"Content deleted.\",\"message\":\"This video is no longer available "
                     "because it has been deleted.\",\"code\":\"DM002\",\"status_code\":410},\"duration\":105}";
  TEST_CHECK(vsrc_dailymotion_parse_meta(gone, strlen(gone), &m, err, sizeof err) == FM_ERR_NOT_FOUND);
  TEST_CHECK(strstr(err, "no longer available") != NULL);
  TEST_CHECK(vsrc_dailymotion_parse_meta(TXT("{\"qualities\":{}}"), &m, err, sizeof err) == FM_ERR_FORMAT);
  TEST_CHECK(vsrc_dailymotion_parse_meta(TXT("<html>"), &m, err, sizeof err) == FM_ERR_FORMAT && err[0]);
  const char *live = "{\"mode\":\"live\",\"qualities\":{\"auto\":[{\"url\":\"https://x/l.m3u8\"}]}}";
  TEST_CHECK(vsrc_dailymotion_parse_meta(live, strlen(live), &m, err, sizeof err) == FM_OK && m.live);

  /* qualities from the master: Windows/Android (os_mp4), FFmpeg, neither */
  TEST_CHECK(vsrc_dailymotion_parse_meta(meta, strlen(meta), &m, err, sizeof err) == FM_OK);
  FmVsrcConf c;
  memset(&c, 0, sizeof c);
  c.os_mp4 = true;
  c.max_height = 720;
  FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
  TEST_CHECK(vsrc_dailymotion_pick(kMaster, strlen(kMaster), m.master, &m, &c, st) == FM_OK);
  TEST_CHECK(st->nq == 5);
  if (st->nq == 5) {
    TEST_CHECK(st->q[0].height == 1080 && !strcmp(st->q[0].label, "1080p60") && st->q[0].fps == 60);
    TEST_CHECK(st->q[1].height == 720 && st->q[2].height == 720);
    int hevc = 1;                                            /* same size: kept in playlist order */
    TEST_CHECK(!strcmp(st->q[1].codec, "H.265") && !strcmp(st->q[2].codec, "H.264"));
    TEST_CHECK(!st->q[hevc].playable && st->q[hevc].needs_ffmpeg);
    TEST_CHECK(st->q[3].height == 480 && !strcmp(st->q[3].label, "480p") && !strcmp(st->q[3].codec, "H.264"));
    TEST_CHECK(st->q[3].kbps == 836 && st->q[3].bytes == (i64)(836 * 125.0 * 180) && st->q[3].muxed);
    TEST_CHECK(st->q[4].height == 288 && st->q[4].playable && !st->q[4].cache_only);
    TEST_CHECK(st->cur >= 0 && st->q[st->cur].height == 720 && !strcmp(st->q[st->cur].codec, "H.264"));
    TEST_CHECK(!strcmp(st->video, "https://cdndirector.dailymotion.com/cdn/manifest/video/hd/720.m3u8"));
    TEST_CHECK(!st->local && !st->audio[0] && st->width == 1280 && st->height == 720 && st->duration == 180);
  }
  c.max_height = 360;                                          /* nothing at 360: the 288p */
  TEST_CHECK(vsrc_dailymotion_pick(kMaster, strlen(kMaster), m.master, &m, &c, st) == FM_OK && st->cur >= 0 &&
             st->q[st->cur].height == 288);
  c.max_height = 100;                                          /* below everything: the smallest */
  TEST_CHECK(vsrc_dailymotion_pick(kMaster, strlen(kMaster), m.master, &m, &c, st) == FM_OK && st->cur >= 0 &&
             st->q[st->cur].height == 288);
  c.os_mp4 = false;                                            /* Linux without FFmpeg */
  TEST_CHECK(vsrc_dailymotion_pick(kMaster, strlen(kMaster), m.master, &m, &c, st) == FM_ERR_UNSUPPORTED);
  TEST_CHECK(st->nq == 5 && st->cur < 0 && st->q[0].needs_ffmpeg && !st->q[0].playable);
  c.have_ffmpeg_libs = true;
  c.max_height = 1080;
  TEST_CHECK(vsrc_dailymotion_pick(kMaster, strlen(kMaster), m.master, &m, &c, st) == FM_OK &&
             st->q[st->cur].height == 1080 && st->q[1].playable && st->q[2].playable);
  /* separate sound: the stream gets it as the audio half */
  TEST_CHECK(vsrc_dailymotion_pick(kMasterAudio, strlen(kMasterAudio), "http://h/m/master.m3u8", &m, &c, st) == FM_OK);
  TEST_CHECK(st->cur == 0 && !strcmp(st->audio, "http://h/m/a/en.m3u8") && !st->q[0].muxed);
  TEST_CHECK(vsrc_dailymotion_pick(TXT("#EXTM3U\n#EXTINF:1,\na.ts\n"), "http://h/", &m, &c, st) == FM_ERR_FORMAT);
  fm_free(st);
}

/* ---- online ------------------------------------------------------------------------ */

/* TS: a sync byte every 188 bytes. fMP4: a chain of boxes. */
static bool container_ok(const u8 *b, size_t n, bool mp4, size_t *checked) {
  *checked = 0;
  if (!mp4) {
    for (size_t i = 0; i + 188 <= n; i += 188, (*checked)++)
      if (b[i] != 0x47) { printf("  hls: TS sync lost at %llu\n", (unsigned long long)i); return false; }
    return *checked > 0;
  }
  size_t at = 0;
  while (at + 8 <= n) {
    u32 sz = (u32)b[at] << 24 | (u32)b[at + 1] << 16 | (u32)b[at + 2] << 8 | b[at + 3];
    const u8 *t = b + at + 4;
    bool name = true;
    for (int k = 0; k < 4; k++) name &= (t[k] >= 'a' && t[k] <= 'z') || (t[k] >= '0' && t[k] <= '9');
    if (sz < 8 || !name) { printf("  hls: bad box at %llu\n", (unsigned long long)at); return false; }
    at += sz;
    (*checked)++;
  }
  return *checked > 2;
}

static void th_stream(void) {
  const char *spec = getenv("MMCFM_HLS_TEST");
  if (!spec || !*spec) return;
  char url[4096];
  fm_strlcpy(url, spec, FM_MIN(sizeof url, strcspn(spec, "|") + 1));
  const char *bar = strchr(spec, '|');
  size_t want = (size_t)(bar ? atoi(bar + 1) : 4) << 20;
  if (want < (1u << 20)) want = 1u << 20;
  char err[160];
  u64 t0 = plat_now_ms();
  FmNetStream *ns = ns_open(url, NULL, err, sizeof err);
  u64 t1 = plat_now_ms();
  TEST_CHECK(ns != NULL);
  if (!ns) { printf("  hls: open failed: %s\n", err); return; }
  bool mp4 = strstr(ns_content_type(ns), "mp4") != NULL;
  printf("  hls: open %d ms, %s, hls %d, %.0f s, live %d\n", (int)(t1 - t0), ns_content_type(ns), ns_is_hls(ns),
         ns_hls_duration(ns), ns_live(ns));
  TEST_CHECK(ns_is_hls(ns));
  u8 *b = (u8 *)fm_alloc(want);
  size_t n = 0, r;
  u64 first = 0;
  while (n < want && (r = ns_read(ns, b + n, FM_MIN(want - n, 65536))) > 0) {
    if (!first) first = plat_now_ms();
    n += r;
  }
  u64 t2 = plat_now_ms();
  size_t units = 0;
  bool ok = container_ok(b, n, mp4, &units);
  printf("  hls: read %.2f MB in %d ms (first bytes after %d ms), %s ok %d (%llu %s), size %lld\n", n / 1048576.0,
         (int)(t2 - t1), first ? (int)(first - t1) : -1, mp4 ? "fMP4" : "TS", ok, (unsigned long long)units,
         mp4 ? "boxes" : "packets", (long long)ns_size(ns));
  TEST_CHECK(n > 0 && ok);
  if (!ns_live(ns) && n >= (1u << 20)) {
    /* exact seeks: back to the start, into the middle, forward past the ring */
    static const double kAt[] = { 0.0, 0.37, 0.05, 0.9 };
    for (int i = 0; i < FM_COUNT(kAt); i++) {
      i64 at = (i64)(n * kAt[i]);
      u8 chk[4096];
      u64 s0 = plat_now_ms();
      bool sk = ns_seek(ns, at);
      size_t got = 0;
      while (sk && got < sizeof chk && (r = ns_read(ns, chk + got, sizeof chk - got)) > 0) got += r;
      size_t cmp = FM_MIN(got, n - (size_t)at);
      bool same = sk && cmp > 0 && !memcmp(chk, b + at, cmp);
      printf("  hls: seek to %lld: %s (%d ms)\n", (long long)at, same ? "same bytes" : "DIFFERENT",
             (int)(plat_now_ms() - s0));
      TEST_CHECK(same);
    }
  }
  fm_free(b);
  ns_close(ns);
  /* reopening at a time starts at that time's segment */
  if (!strchr(url, '#')) {
    char tu[4200];
    fm_snprintf(tu, sizeof tu, "%s#t=60", url);
    t0 = plat_now_ms();
    ns = ns_open(tu, NULL, err, sizeof err);
    TEST_CHECK(ns != NULL);
    if (ns) {
      u8 head[188 * 4];
      size_t got = 0;
      while (got < sizeof head && (r = ns_read(ns, head + got, sizeof head - got)) > 0) got += r;
      size_t units2;
      printf("  hls: #t=60 opens at %.1f s in %d ms, first bytes %s\n", ns_hls_start(ns), (int)(plat_now_ms() - t0),
             container_ok(head, got, mp4, &units2) || (mp4 && got >= 8) ? "ok" : "BAD");
      TEST_CHECK(ns_hls_duration(ns) < 60 || (ns_hls_start(ns) <= 60 && ns_hls_start(ns) > 40));
      ns_close(ns);
    }
  }
}

#if defined(_WIN32)
#  include <windows.h>
/* Keeps Media Foundation started for the whole test. Closing the last MF
** video calls MFShutdown while the byte stream's reader thread may still
** finish an async read (a network read blocked between HLS segments) and
** invoke its callback: a crash inside RtwqInvokeCallback, seen with the
** TS stream. fdec_vid_mf.c must complete pending reads (ns_abort) before
** MFShutdown (bs_quiesce does since 2026-10-09); MMCFM_DM_HOLD_MF=1 holds one
** MFStartup reference to rule that out when chasing a crash. */
static void hold_mf(void) {
  static bool done;
  if (done) return;
  done = true;
  HMODULE m = LoadLibraryA("mfplat.dll");
  typedef HRESULT(WINAPI * StartupFn)(ULONG, DWORD);
  StartupFn f = m ? (StartupFn)(void (*)(void))GetProcAddress(m, "MFStartup") : NULL;
  if (f) f(0x00020070, 0);                                  /* MF_VERSION (Windows 7+) */
}
#else
static void hold_mf(void) {}
#endif

static void th_dm_online(void) {
  const char *spec = getenv("MMCFM_DM_TEST");
  if (!spec || !*spec) return;
  if (getenv("MMCFM_DM_HOLD_MF")) hold_mf();  /* fdec_vid_mf.c quiesces its reads now: off by default */
  FmVsrcConf c;
  vsrc_conf_snapshot(&c);
  c.ytdlp[0] = 0;                                              /* native only */
  if (getenv("MMCFM_DM_HEIGHT")) c.max_height = atoi(getenv("MMCFM_DM_HEIGHT"));
  char ids[512];
  fm_strlcpy(ids, spec, sizeof ids);
  for (char *id = strtok(ids, "|,"); id; id = strtok(NULL, "|,")) {
    FmVsrcItem it;
    memset(&it, 0, sizeof it);
    fm_strlcpy(it.id, id, sizeof it.id);
    fm_snprintf(it.title, sizeof it.title, "dm %s", id);
    fm_snprintf(it.page, sizeof it.page, "https://www.dailymotion.com/video/%s", id);
    FmVsrcStream *st = (FmVsrcStream *)fm_calloc(1, sizeof *st);
    char err[256] = "";
    u64 t0 = plat_now_ms();
    FmErr e = g_vsrc_dailymotion.resolve(&c, &it, st, NULL, NULL, err, sizeof err, NULL);
    u64 t1 = plat_now_ms();
    printf("  dm %s: resolve %s in %d ms %s, %d qualities, %.0f s\n", id, fm_err_str(e), (int)(t1 - t0),
           e == FM_OK ? "" : err, st->nq, st->duration);
    for (int i = 0; i < st->nq; i++)
      printf("    %s%-8s %-6s %5d kbps %s%s\n", i == st->cur ? "*" : " ", st->q[i].label, st->q[i].codec,
             st->q[i].kbps, st->q[i].playable ? "stream" : "", st->q[i].needs_ffmpeg ? "needs-ffmpeg" : "");
    TEST_CHECK(e == FM_OK && st->nq > 0 && st->cur >= 0);
    if (e != FM_OK) { fm_free(st); continue; }
    FmErr ve;
    FmVid *v = vid_open(st->video, 0, &ve);
    u64 t2 = plat_now_ms();
    TEST_CHECK(v != NULL);
    if (!v) {
      printf("  dm %s: vid_open failed (%d)\n", id, (int)ve);
      fm_free(st);
      continue;
    }
    FmVidInfo in = *vid_info(v);
    FmVidFrame vf;
    FmVidPcm pc;
    int ev, nv = 0, na = 0, guard = 0;
    u64 tf = 0;
    double last = 0;
    while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 20000 && nv < 90) {
      if (ev == VID_EV_VIDEO) {
        if (!nv) tf = plat_now_ms();
        nv++;
        last = vf.t;
      } else {
        na++;
      }
    }
    printf("  dm %s: %s %s/%s %dx%d  open %d ms, first frame %d ms after resolve (%d ms from the click), "
           "v%d a%d to %.1fs\n",
           id, in.backend, in.vcodec, in.acodec, in.w, in.h, (int)(t2 - t1), tf ? (int)(tf - t1) : -1,
           tf ? (int)(tf - t0) : -1, nv, na, last);
    TEST_CHECK(nv > 0 && na > 0);
    double target = st->duration > 60 ? st->duration * 0.5 : 0;
    TEST_CHECK(in.duration > 0);                               /* MF: from the playlist (ns_hls_duration) */
    if (target > 0) {                                          /* the decoder's own seek (MF: reopen at #t=) */
      u64 t3 = plat_now_ms();
      bool sk = vid_seek(v, target);
      double at = -1;
      while (sk && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 60000)
        if (ev == VID_EV_VIDEO) { at = vf.t; break; }
      printf("  dm %s: vid_seek %.0f -> %s %.1f (%d ms)\n", id, target, at >= 0 ? "ok" : "FAIL", at,
             (int)(plat_now_ms() - t3));
      TEST_CHECK(at >= target - 1 && at < target + 15);
    }
    vid_close(v);
    if (target > 0) {                                         /* a time seek as a reopen at #t= */
      char tu[4200];
      fm_snprintf(tu, sizeof tu, "%s#t=%.0f", st->video, target);
      u64 t3 = plat_now_ms();
      v = vid_open(tu, 0, &ve);
      double vt = -1, at = -1;
      guard = 0;
      while (v && (vt < 0 || at < 0) && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 5000) {
        if (ev == VID_EV_VIDEO && vt < 0) vt = vf.t;
        if (ev == VID_EV_AUDIO && at < 0) at = pc.t;
      }
      printf("  dm %s: reopen at #t=%.0f: %s, first frame at %.2f s, sound at %.2f s, %d ms\n", id, target,
             v ? "ok" : "FAIL", vt, at, (int)(plat_now_ms() - t3));
      TEST_CHECK(v && vt >= 0);
      if (v) vid_close(v);
    }
    fm_free(st);
  }
}

int test_hls(const char *tmp) {
  FM_UNUSED(tmp);
  int before = g_test_fail;
  th_urls();
  th_master();
  th_media();
  th_aes();
  th_dailymotion();
  th_stream();
  th_dm_online();
  return g_test_fail - before;
}
