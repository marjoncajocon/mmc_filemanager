/* ftest_media.c -- self test for the media decoders (no window, no audio device).
**
** Design decisions:
**   - Every input is generated here: a WAV and a GIF written byte by byte
**     (the GIF's LZW stream is encoded with periodic clear codes so the code
**     width never grows), a tiny PNG embedded as bytes, an SVG string and
**     crafted EXIF / ID3 headers. Nothing depends on files outside tmp.
**   - The checks are on decoded values (pixels, delays, PCM samples, line
**     offsets), not just on "it opened".
*/
#include "ftest.h"
#include "fplat.h"
#include "fdec_img.h"
#include "fdec_aud.h"
#include "fdec_vid.h"
#include "fview_int.h"

/* ---- helpers ----------------------------------------------------------------- */

static bool write_file(const char *path, const void *data, size_t n) {
  FILE *f = fm_fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  fclose(f);
  return ok;
}

typedef struct Buf { u8 *p; size_t n, cap; } Buf;

static void put(Buf *b, const void *d, size_t n) {
  if (b->n + n > b->cap) {
    b->cap = FM_MAX(b->cap * 2, b->n + n + 256);
    b->p = (u8 *)fm_realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void put8(Buf *b, int v) { u8 c = (u8)v; put(b, &c, 1); }
static void put16le(Buf *b, int v) { put8(b, v & 255); put8(b, (v >> 8) & 255); }
static void put32le(Buf *b, u32 v) { put16le(b, (int)(v & 0xFFFF)); put16le(b, (int)(v >> 16)); }

static bool px_is(const u8 *p, int r, int g, int b, int a) {
  return abs(p[0] - r) <= 2 && abs(p[1] - g) <= 2 && abs(p[2] - b) <= 2 && abs(p[3] - a) <= 2;
}

/* ---- WAV ------------------------------------------------------------------------- */

static void test_wav(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "tone.wav");
  Buf b = { 0 };
  int frames = 1000;
  put(&b, "RIFF", 4); put32le(&b, (u32)(36 + frames * 4)); put(&b, "WAVE", 4);
  put(&b, "fmt ", 4); put32le(&b, 16); put16le(&b, 1); put16le(&b, 2); put32le(&b, 8000);
  put32le(&b, 8000 * 4); put16le(&b, 4); put16le(&b, 16);
  put(&b, "data", 4); put32le(&b, (u32)(frames * 4));
  for (int i = 0; i < frames; i++) { put16le(&b, i * 30); put16le(&b, -i * 30); }
  TEST_CHECK(write_file(path, b.p, b.n));
  fm_free(b.p);

  FmErr err;
  FmAudio *a = aud_open(path, &err);
  TEST_CHECK(a != NULL);
  if (!a) return;
  TEST_CHECK(aud_channels(a) == 2);
  TEST_CHECK(aud_rate(a) == 8000);
  TEST_CHECK(aud_length(a) == (u64)frames);
  TEST_CHECK(strcmp(aud_codec(a), "WAV") == 0);
  float *pcm = (float *)fm_alloc(sizeof(float) * 2 * 1200);
  int got = 0, n;
  while ((n = aud_read(a, pcm + got * 2, 128)) > 0) got += n;
  TEST_CHECK(got == frames);
  TEST_CHECK(fabs(pcm[2 * 10] - 300 / 32768.0) < 1e-4);
  TEST_CHECK(fabs(pcm[2 * 10 + 1] + 300 / 32768.0) < 1e-4);
  TEST_CHECK(fabs(pcm[2 * 999] - 29970 / 32768.0) < 1e-4);
  TEST_CHECK(aud_seek(a, 500));
  TEST_CHECK(aud_read(a, pcm, 1) == 1 && fabs(pcm[0] - 15000 / 32768.0) < 1e-4);
  TEST_CHECK(aud_tell(a) == 501);
  fm_free(pcm);
  aud_close(a);

  /* a 4-channel file is delivered as stereo */
  fm_path_join(path, sizeof path, tmp, "quad.wav");
  Buf q = { 0 };
  put(&q, "RIFF", 4); put32le(&q, 36 + 10 * 8); put(&q, "WAVE", 4);
  put(&q, "fmt ", 4); put32le(&q, 16); put16le(&q, 1); put16le(&q, 4); put32le(&q, 8000);
  put32le(&q, 8000 * 8); put16le(&q, 8); put16le(&q, 16);
  put(&q, "data", 4); put32le(&q, 10 * 8);
  for (int i = 0; i < 10; i++) { put16le(&q, 1000); put16le(&q, 2000); put16le(&q, 3000); put16le(&q, 4000); }
  TEST_CHECK(write_file(path, q.p, q.n));
  fm_free(q.p);
  a = aud_open(path, &err);
  TEST_CHECK(a && aud_channels(a) == 2);
  if (a) {
    float s[4];
    TEST_CHECK(aud_read(a, s, 2) == 2);
    /* L = (FL + 0.707 C) / 1, R = (FR + 0.707 C + BL?) ... checked loosely: both positive, R > L */
    TEST_CHECK(s[0] > 0 && s[1] > s[0]);
    aud_close(a);
  }
}

/* ---- GIF ---------------------------------------------------------------------- */

/* LZW with min code size 2: a clear code every two pixels keeps codes 3 bits wide. */
static void gif_lzw(Buf *b, const u8 *idx, int n) {
  u8 data[512];
  int dn = 0, nbits = 0;
  u32 acc = 0;
#define EMIT(c) do { acc |= (u32)(c) << nbits; nbits += 3; \
    while (nbits >= 8) { data[dn++] = (u8)acc; acc >>= 8; nbits -= 8; } } while (0)
  for (int i = 0; i < n; i++) {
    if (i % 2 == 0) EMIT(4);
    EMIT(idx[i]);
  }
  EMIT(5);
  if (nbits > 0) data[dn++] = (u8)acc;
#undef EMIT
  put8(b, 2);
  for (int p = 0; p < dn; p += 255) {
    int len = FM_MIN(255, dn - p);
    put8(b, len);
    put(b, data + p, (size_t)len);
  }
  put8(b, 0);
}

static void gif_frame(Buf *b, int disp, int delay, int trans, int x, int y, int w, int h, bool inter,
                      const u8 *idx) {
  put8(b, 0x21); put8(b, 0xF9); put8(b, 4);
  put8(b, (disp << 2) | (trans >= 0 ? 1 : 0)); put16le(b, delay); put8(b, trans >= 0 ? trans : 0);
  put8(b, 0);
  put8(b, 0x2C); put16le(b, x); put16le(b, y); put16le(b, w); put16le(b, h);
  put8(b, inter ? 0x40 : 0);
  gif_lzw(b, idx, w * h);
}

static void test_gif(const char *tmp) {
  Buf b = { 0 };
  put(&b, "GIF89a", 6);
  put16le(&b, 4); put16le(&b, 4); put8(&b, 0x81); put8(&b, 0); put8(&b, 0);
  static const u8 pal[12] = { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 };   /* black red green blue */
  put(&b, pal, 12);
  put8(&b, 0x21); put8(&b, 0xFF); put8(&b, 11); put(&b, "NETSCAPE2.0", 11);
  put8(&b, 3); put8(&b, 1); put16le(&b, 3); put8(&b, 0);
  u8 f1[16];
  memset(f1, 1, sizeof f1);
  f1[0] = 3;
  gif_frame(&b, 1, 0, -1, 0, 0, 4, 4, false, f1);
  static const u8 f2[4] = { 0, 2, 2, 0 };
  gif_frame(&b, 2, 5, 0, 1, 1, 2, 2, false, f2);
  static const u8 f3[4] = { 3, 1, 2, 0 };          /* rows 0,2,1,3 in interlace order */
  gif_frame(&b, 0, 1, -1, 0, 0, 1, 4, true, f3);
  put8(&b, 0x3B);

  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "anim.gif");
  TEST_CHECK(write_file(path, b.p, b.n));

  FmErr err;
  FmGif *g = gif_open(path, true, &err);
  TEST_CHECK(g != NULL);
  if (g) {
    TEST_CHECK(gif_width(g) == 4 && gif_height(g) == 4);
    TEST_CHECK(gif_frames(g) == 3);
    int d = 0;
    TEST_CHECK(gif_next(g, &d) == 1 && d == 100);          /* 0 cs -> 100 ms */
    const u8 *c = gif_pixels(g);
    TEST_CHECK(px_is(c, 0, 0, 255, 255));
    TEST_CHECK(px_is(c + (3 * 4 + 3) * 4, 255, 0, 0, 255));
    TEST_CHECK(gif_loops(g) == 3);
    TEST_CHECK(gif_next(g, &d) == 1 && d == 50);
    TEST_CHECK(px_is(c + (1 * 4 + 1) * 4, 255, 0, 0, 255));  /* transparent: red shows */
    TEST_CHECK(px_is(c + (1 * 4 + 2) * 4, 0, 255, 0, 255));
    TEST_CHECK(px_is(c + (2 * 4 + 1) * 4, 0, 255, 0, 255));
    TEST_CHECK(gif_next(g, &d) == 1 && d == 100);          /* 1 cs -> 100 ms */
    TEST_CHECK(px_is(c + (1 * 4 + 1) * 4, 0, 0, 0, 0));      /* disposed to background */
    TEST_CHECK(px_is(c + (1 * 4 + 2) * 4, 0, 0, 0, 0));
    TEST_CHECK(px_is(c + (0 * 4 + 0) * 4, 0, 0, 255, 255));  /* interlaced column */
    TEST_CHECK(px_is(c + (1 * 4 + 0) * 4, 0, 255, 0, 255));
    TEST_CHECK(px_is(c + (2 * 4 + 0) * 4, 255, 0, 0, 255));
    TEST_CHECK(px_is(c + (3 * 4 + 0) * 4, 0, 0, 0, 255));
    TEST_CHECK(px_is(c + (3 * 4 + 3) * 4, 255, 0, 0, 255));
    TEST_CHECK(gif_next(g, &d) == 0);
    TEST_CHECK(gif_rewind(g) && gif_next(g, &d) == 1 && px_is(gif_pixels(g), 0, 0, 255, 255));
    gif_close(g);
  }
  /* same bytes from memory, and a truncated copy must not crash */
  g = gif_open_mem(b.p, b.n, false, &err);
  TEST_CHECK(g != NULL);
  if (g) { int d; TEST_CHECK(gif_next(g, &d) == 1); gif_close(g); }
  for (size_t cut = 13; cut < b.n; cut += 7) {
    g = gif_open_mem(b.p, cut, true, &err);
    if (g) { int d, k = 0; while (gif_next(g, &d) == 1 && k < 10) k++; gif_close(g); }
  }
  /* first frame through the generic loader */
  FmImage im;
  FmImgInfo info;
  TEST_CHECK(img_load(path, 0, 0, &im, &info, NULL) == FM_OK);
  TEST_CHECK(im.w == 4 && im.h == 4 && info.kind == IMGK_GIF && px_is(im.px, 0, 0, 255, 255));
  img_free(&im);
  fm_free(b.p);
}

/* ---- PNG, downscale, orientation ----------------------------------------------------- */

static const u8 kPng2x2[] = {
  0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x02,
  0x00,0x00,0x00,0x02,0x08,0x06,0x00,0x00,0x00,0x72,0xb6,0x0d,0x24,0x00,0x00,0x00,0x13,0x49,0x44,0x41,
  0x54,0x78,0xda,0x63,0xf8,0xcf,0xc0,0xf0,0x1f,0x0c,0x81,0x34,0x08,0x34,0x00,0x00,0x49,0x49,0x09,0x78,
  0x9c,0x51,0x17,0x92,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
};

static void test_png(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "px.png");
  TEST_CHECK(write_file(path, kPng2x2, sizeof kPng2x2));
  TEST_CHECK(img_sniff(path) == IMGK_PNG);
  FmImgInfo info;
  TEST_CHECK(img_info(path, &info) && info.w == 2 && info.h == 2);
  FmImage im;
  TEST_CHECK(img_load(path, 0, 0, &im, &info, NULL) == FM_OK);
  TEST_CHECK(im.w == 2 && im.h == 2);
  if (im.px) {
    TEST_CHECK(px_is(im.px, 255, 0, 0, 255));
    TEST_CHECK(px_is(im.px + 4, 0, 255, 0, 255));
    TEST_CHECK(px_is(im.px + 8, 0, 0, 255, 255));
    TEST_CHECK(px_is(im.px + 12, 255, 255, 255, 128));
  }
  img_free(&im);
  /* fitting into 1x1 averages with alpha weights */
  TEST_CHECK(img_load(path, 1, 1, &im, NULL, NULL) == FM_OK);
  TEST_CHECK(im.w == 1 && im.h == 1);
  if (im.px) {
    /* r = (255*255 + 255*128) / (255*3+128) = 109 */
    TEST_CHECK(abs(im.px[0] - 109) <= 2 && abs(im.px[3] - 223) <= 2);
  }
  img_free(&im);
  TEST_CHECK(img_load_mem(kPng2x2, sizeof kPng2x2, 64, &im) == FM_OK && im.w == 2);
  img_free(&im);

  /* orientation 6 = rotate 90 degrees clockwise */
  FmImage o;
  o.w = 3; o.h = 2;
  o.px = (u8 *)fm_alloc(3 * 2 * 4);
  for (int i = 0; i < 6; i++) { o.px[i * 4] = (u8)i; o.px[i * 4 + 1] = o.px[i * 4 + 2] = 0; o.px[i * 4 + 3] = 255; }
  o = img_orient(o, 6);
  TEST_CHECK(o.w == 2 && o.h == 3);
  /* source rows: 0 1 2 / 3 4 5 -> rotated: 3 0 / 4 1 / 5 2 */
  TEST_CHECK(o.px[0] == 3 && o.px[4] == 0 && o.px[8] == 4 && o.px[20] == 2);
  img_free(&o);

  int w, h;
  img_fit(4000, 3000, 400, 400, &w, &h);
  TEST_CHECK(w == 400 && h == 300);
  img_fit(100, 50, 400, 400, &w, &h);
  TEST_CHECK(w == 100 && h == 50);
}

static void test_exif(void) {
  static const u8 be[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,           /* APP0 stub */
    0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
    'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xFF, 0xDA,
  };
  static const u8 le[] = {
    0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
    'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
  };
  TEST_CHECK(exif_orientation(be, sizeof be) == 6);
  TEST_CHECK(exif_orientation(le, sizeof le) == 8);
  TEST_CHECK(exif_orientation(le, 20) == 1);                 /* truncated */
  TEST_CHECK(exif_orientation(kPng2x2, sizeof kPng2x2) == 1);
}

/* ---- SVG --------------------------------------------------------------------- */

static void test_svg(const char *tmp) {
  static const char svg[] =
    "<?xml version=\"1.0\"?>\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"10\">"
    "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";
  FmErr err;
  FmSvg *s = svg_open_mem(svg, sizeof svg - 1, &err);
  TEST_CHECK(s != NULL);
  if (s) {
    float w, h;
    svg_size(s, &w, &h);
    TEST_CHECK(fabs(w - 20) < 0.01 && fabs(h - 10) < 0.01);
    FmImage im;
    TEST_CHECK(svg_render(s, 40, 20, &im) == FM_OK);
    TEST_CHECK(im.w == 40 && im.h == 20);
    if (im.px) {
      TEST_CHECK(px_is(im.px + (10 * 40 + 5) * 4, 255, 0, 0, 255));
      TEST_CHECK(im.px[(10 * 40 + 30) * 4 + 3] == 0);
    }
    img_free(&im);
    svg_close(s);
  }
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "shape.svg");
  TEST_CHECK(write_file(path, svg, sizeof svg - 1));
  TEST_CHECK(img_sniff(path) == IMGK_SVG);
  FmImage im;
  TEST_CHECK(img_load(path, 64, 64, &im, NULL, NULL) == FM_OK && im.w == 64 && im.h == 32);
  img_free(&im);
}

/* ---- tags ------------------------------------------------------------------------- */

static void test_id3(void) {
  Buf f = { 0 };
  /* TIT2 UTF-8, TPE1 UTF-16 with BOM, APIC with 16 bytes of "image" */
  put(&f, "TIT2", 4); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, 6); put16le(&f, 0);
  put8(&f, 3); put(&f, "Hello", 5);
  put(&f, "TPE1", 4); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, 7); put16le(&f, 0);
  put8(&f, 1); put8(&f, 0xFF); put8(&f, 0xFE); put16le(&f, 'A'); put16le(&f, 0xE9);
  int apic = 1 + 11 + 1 + 1 + 16;
  put(&f, "APIC", 4); put8(&f, 0); put8(&f, 0); put8(&f, 0); put8(&f, apic); put16le(&f, 0);
  put8(&f, 0); put(&f, "image/jpeg", 11); put8(&f, 3); put8(&f, 0);
  for (int i = 0; i < 16; i++) put8(&f, 0xA0 + i);
  Buf t = { 0 };
  put(&t, "ID3", 3); put8(&t, 3); put8(&t, 0); put8(&t, 0);
  size_t n = f.n;
  put8(&t, (int)(n >> 21) & 127); put8(&t, (int)(n >> 14) & 127); put8(&t, (int)(n >> 7) & 127); put8(&t, (int)n & 127);
  put(&t, f.p, f.n);
  FmAudMeta m;
  memset(&m, 0, sizeof m);
  TEST_CHECK(aud_meta_id3v2(t.p, t.n, &m, true));
  TEST_CHECK(strcmp(m.title, "Hello") == 0);
  TEST_CHECK(strcmp(m.artist, "A\xC3\xA9") == 0);
  TEST_CHECK(m.cover_len == 16 && m.cover && m.cover[0] == 0xA0 && m.cover[15] == 0xAF);
  aud_meta_free(&m);
  /* truncated tags must be rejected or parsed without overruns */
  for (size_t cut = 0; cut < t.n; cut += 5) {
    memset(&m, 0, sizeof m);
    aud_meta_id3v2(t.p, cut, &m, true);
    aud_meta_free(&m);
  }
  fm_free(f.p);
  fm_free(t.p);
}

/* ---- video -------------------------------------------------------------------- */

static void test_video(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "junk.mpg");
  static const u8 junk[] = { 0, 0, 1, 0xBA, 0x44, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  TEST_CHECK(write_file(path, junk, sizeof junk));
  TEST_CHECK(!vid_is_mpeg1(path));          /* MPEG-2 pack header */
  FmErr err;
  FmVid *v = vid_open(path, 0, &err);
  if (v) vid_close(v);                       /* FFmpeg may accept it; must not crash */
  FmImage im;
  if (vid_thumb(path, 64, &im) == FM_OK) img_free(&im);
  printf("  %s\n", ff_version_str());
}

/* ---- text index ------------------------------------------------------------------- */

static void test_text(const char *tmp) {
  char path[FM_PATH_MAX];
  fm_path_join(path, sizeof path, tmp, "mixed.txt");
  static const char mixed[] = "a\nbb\r\nccc\rdd";
  TEST_CHECK(write_file(path, mixed, sizeof mixed - 1));
  FmTxtIndex ix;
  TEST_CHECK(txt_index_open(&ix, path));
  while (txt_index_step(&ix, 3)) {}
  TEST_CHECK(ix.done && ix.lines == 4 && !ix.binary);
  TEST_CHECK(txt_line_offset(&ix, 0) == 0);
  TEST_CHECK(txt_line_offset(&ix, 1) == 2);
  TEST_CHECK(txt_line_offset(&ix, 2) == 6);
  TEST_CHECK(txt_line_offset(&ix, 3) == 10);
  txt_index_close(&ix);

  /* many lines: offsets past the sparse marks */
  fm_path_join(path, sizeof path, tmp, "big.log");
  FILE *f = fm_fopen(path, "wb");
  TEST_CHECK(f != NULL);
  if (!f) return;
  u64 off777 = 0, pos = 0;
  for (int i = 0; i < 5000; i++) {
    char line[64];
    int n = fm_snprintf(line, sizeof line, "line %d%s", i, i % 3 == 0 ? "\r\n" : "\n");
    if (i == 777) off777 = pos;
    fwrite(line, 1, (size_t)n, f);
    pos += (u64)n;
  }
  fclose(f);
  TEST_CHECK(txt_index_open(&ix, path));
  while (txt_index_step(&ix, 1000)) {}
  TEST_CHECK(ix.lines == 5001);              /* trailing newline opens an empty last line */
  TEST_CHECK(txt_line_offset(&ix, 777) == off777);
  TEST_CHECK(txt_line_offset(&ix, 5000) == pos);
  txt_index_close(&ix);

  /* UTF-16LE with BOM */
  fm_path_join(path, sizeof path, tmp, "u16.txt");
  static const u8 u16[] = { 0xFF, 0xFE, 'x', 0, '\n', 0, 'y', 0, '\r', 0, '\n', 0, 'z', 0 };
  TEST_CHECK(write_file(path, u16, sizeof u16));
  TEST_CHECK(txt_index_open(&ix, path));
  while (txt_index_step(&ix, 4)) {}
  TEST_CHECK(ix.unit == 2 && ix.lines == 3);
  TEST_CHECK(txt_line_offset(&ix, 2) == 12);
  txt_index_close(&ix);

  /* binary detection */
  fm_path_join(path, sizeof path, tmp, "bin.dat");
  static const u8 bin[] = { 0x7F, 'E', 'L', 'F', 0, 0, 0, 1, 2, 3 };
  TEST_CHECK(write_file(path, bin, sizeof bin));
  TEST_CHECK(txt_index_open(&ix, path));
  TEST_CHECK(ix.binary);
  txt_index_close(&ix);
}

/* MMCFM_VIDEO_SAMPLES=<folder>: decode every file there with whatever
** backend takes it (pl_mpeg, FFmpeg, Media Foundation, MediaCodec) and check
** frames, audio, seeking and thumbnails. Off by default: the samples are
** made with an external encoder, not shipped. */
static void test_video_samples(void) {
  const char *dir = getenv("MMCFM_VIDEO_SAMPLES");
  if (!dir || !*dir) return;
  FmErr err;
  FmDir *d = plat_dir_open(dir, &err);
  TEST_CHECK(d != NULL);
  if (!d) return;
  const char *name;
  FmStat st;
  int files = 0, played = 0;
  while (plat_dir_next(d, &name, &st)) {
    if (st.flags & FM_ST_DIR) continue;
    char path[FM_PATH_MAX];
    fm_path_join(path, sizeof path, dir, name);
    files++;
    u64 t0 = plat_now_ms();
    FmVid *v = vid_open(path, 0, &err);
    if (!v) {
      /* without FFmpeg some containers (FLV, RealMedia ...) have no OS
      ** decoder at all; the viewer then offers the system player */
      bool expected = !ff_available();
      printf("  %-22s %s (err %d)\n", name, expected ? "needs FFmpeg" : "cannot open", (int)err);
      if (!expected) g_test_fail++;
      else files--;
      continue;
    }
    FmVidInfo info = *vid_info(v);       /* copied: vid_close frees it */
    const FmVidInfo *in = &info;
    int vframes = 0, ablocks = 0, ev, guard = 0, w = 0, h = 0;
    double last_t = -1, apcm = 0;
    bool mono_t = true, planes_ok = true;
    FmVidFrame vf;
    FmVidPcm pc;
    while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 5000 && vframes < 60) {
      if (ev == VID_EV_VIDEO) {
        if (!vframes) { w = vf.w; h = vf.h; }
        if (vf.t + 0.001 < last_t) mono_t = false;
        last_t = vf.t;
        if (!vf.plane[0] || !vf.plane[1] || !vf.plane[2] || vf.stride[0] < vf.w || vf.stride[1] < vf.w / 2)
          planes_ok = false;
        vframes++;
      } else {
        ablocks++;
        apcm += pc.frames;
        if (pc.channels < 1 || pc.channels > 2 || pc.rate <= 0) planes_ok = false;
      }
    }
    bool seek_ok = !in->has_video || in->duration < 1 || vid_seek(v, in->duration * 0.5);
    int after = 0;
    if (seek_ok && in->has_video) {
      guard = 0;
      while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 2000)
        if (ev == VID_EV_VIDEO) { after = 1; break; }
    }
    vid_close(v);
    FmImage th;
    bool thumb_ok = !in->has_video || vid_thumb(path, 256, &th) == FM_OK;
    if (in->has_video && thumb_ok) img_free(&th);
    bool ok = (!in->has_video || (vframes > 0 && w > 0 && h > 0)) && (!in->has_audio || ablocks > 0) &&
              planes_ok && mono_t && seek_ok && (!in->has_video || after) && thumb_ok;
    printf("  %-22s %-4s %-17s %-6s/%-6s %4dx%-4d v%-3d a%-4d %5.1fs %s%s%s%s (%d ms)\n", name,
           ok ? "ok" : "FAIL", in->backend, in->vcodec[0] ? in->vcodec : "-",
           in->acodec[0] ? in->acodec : "-", w, h, vframes, ablocks, in->duration, mono_t ? "" : " time!",
           planes_ok ? "" : " planes!", !seek_ok ? " seek-call!" : (!in->has_video || after) ? "" : " no-frame-after-seek!",
           thumb_ok ? "" : " thumb!", (int)(plat_now_ms() - t0));
    if (!ok) g_test_fail++;
    else played++;
    (void)apcm;
  }
  plat_dir_close(d);
  printf("  video samples: %d of %d played\n", played, files);
}

/* MMCFM_URL_TEST=<url>: opens a network stream (online video sources) and
** decodes a few seconds; off by default (needs the network). */
static void test_video_url(void) {
  const char *url = getenv("MMCFM_URL_TEST");
  if (!url || !*url) return;
  u64 t0 = plat_now_ms();
  FmErr err;
  FmVid *v = vid_open(url, 0, &err);
  TEST_CHECK(v != NULL);
  if (!v) { printf("  url: cannot open (err %d)\n", (int)err); return; }
  FmVidInfo in = *vid_info(v);
  u64 t1 = plat_now_ms();
  int vf_n = 0, a_n = 0, ev, guard = 0;
  FmVidFrame vf;
  FmVidPcm pc;
  double last = 0;
  while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 20000 && vf_n < 90) {
    if (ev == VID_EV_VIDEO) { vf_n++; last = vf.t; }
    else a_n++;
  }
  bool seek = in.duration < 30 || vid_seek(v, in.duration * 0.5);
  int after = 0;
  u64 t2 = plat_now_ms();
  while (seek && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 40000)
    if (ev == VID_EV_VIDEO || (!in.has_video && ev == VID_EV_AUDIO)) { after = 1; break; }
  vid_close(v);
  printf("  url: %s %s/%s %dx%d %.0fs  v%d a%d (to %.1fs)  open %dms, decode %dms, seek %s %dms\n", in.backend,
         in.vcodec, in.acodec, in.w, in.h, in.duration, vf_n, a_n, last, (int)(t1 - t0), (int)(t2 - t1),
         seek && after ? "ok" : "FAIL", (int)(plat_now_ms() - t2));
  TEST_CHECK((in.has_video ? vf_n > 0 : a_n > 0) && seek && after);
}

/* MMCFM_PAIR_TEST=<video>|<audio>: separate picture and sound files play as
** one video with both streams, in time order, and seek together. */
static void test_video_pair(void) {
  const char *spec = getenv("MMCFM_PAIR_TEST");
  if (!spec || !strchr(spec, '|')) return;
  char vpath[FM_PATH_MAX];
  fm_strlcpy(vpath, spec, FM_MIN(sizeof vpath, (size_t)(strchr(spec, '|') - spec) + 1));
  const char *apath = strchr(spec, '|') + 1;
  FmErr err;
  FmVid *v = vid_open_pair(vpath, apath, 0, &err);
  TEST_CHECK(v != NULL);
  if (!v) return;
  FmVidInfo in = *vid_info(v);
  TEST_CHECK(in.has_video && in.has_audio && in.rate > 0);
  int nv = 0, na = 0, ev, guard = 0, order_bad = 0;
  double last_v = -1, last_a = -1, last_any = -1;
  FmVidFrame vf;
  FmVidPcm pc;
  while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 5000 && nv < 120) {
    double t = ev == VID_EV_VIDEO ? vf.t : pc.t;
    if (t + 0.25 < last_any) order_bad++;          /* interleaved by time */
    last_any = FM_MAX(last_any, t);
    if (ev == VID_EV_VIDEO) { nv++; last_v = vf.t; }
    else { na++; last_a = pc.t; }
  }
  TEST_CHECK(nv > 0 && na > 0 && order_bad == 0);
  TEST_CHECK(fabs(last_v - last_a) < 1.0);          /* the two halves stay together */
  bool sk = vid_seek(v, in.duration * 0.5);
  int sv = 0, sa = 0;
  guard = 0;
  while (sk && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 3000 && (!sv || !sa)) {
    if (ev == VID_EV_VIDEO) { sv = 1; TEST_CHECK(vf.t > in.duration * 0.3); }
    else sa = 1;
  }
  TEST_CHECK(sk && sv && sa);
  printf("  pair: %s %s+%s %dx%d %.0fs  v%d a%d to %.1f/%.1fs, seek %s\n", in.backend, in.vcodec, in.acodec, in.w,
         in.h, in.duration, nv, na, last_v, last_a, sk && sv && sa ? "ok" : "FAIL");
  vid_close(v);
}

int test_media(const char *tmp) {
  int before = g_test_fail;
  test_wav(tmp);
  test_gif(tmp);
  test_png(tmp);
  test_exif();
  test_svg(tmp);
  test_id3();
  test_video(tmp);
  test_video_samples();
  test_video_url();
  test_video_pair();
  test_text(tmp);
  return g_test_fail - before;
}
