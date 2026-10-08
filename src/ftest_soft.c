/* ftest_soft.c -- self tests for the built-in video backend (fdec_vid_soft.c):
** VP9 + Opus in WebM through nestegg, libvpx and libopus.
**
** Design decisions:
**   - Encoding is not possible in the test, so a 47 KB clip is kept in
**     tests/fixtures (vp9_opus.webm: ffmpeg's testsrc2 at 320x180, 25 fps,
**     2 s, with a 440 Hz sine in mono Opus). It is found like the rar
**     fixtures ($MMCFM_FIXTURES or tests/fixtures); without it the clip
**     tests are skipped, not failed.
**   - The backend is forced (vid_force_backend), so the test checks this
**     decoder even where FFmpeg or Media Foundation would take the file.
**   - Pixels are checked on two colour bars of the first frame: that proves
**     the plane pointers, strides and chroma order are right.
**   - MMCFM_SOFT_BENCH=<file or url>[|...] reports decode speed (picture
**     only, 600 frames at most); off by default.
*/
#include "ftest.h"
#include "fplat.h"
#include "fdec_img.h"
#include "fdec_vid.h"
#include "fdec_vid_int.h"

#include <math.h>

static bool soft_fixture(char *out, size_t cap) {
  const char *dir = getenv("MMCFM_FIXTURES");
  fm_path_join(out, cap, dir && *dir ? dir : "tests/fixtures", "vp9_opus.webm");
  FILE *f = fm_fopen(out, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

/* RGB of the first frame at (x, y), averaged over 5x5 pixels. */
static void frame_rgb(const FmVidFrame *vf, int x, int y, int rgb[3]) {
  u8 *px = (u8 *)fm_alloc((size_t)vf->w * vf->h * 4);
  img_yuv420_to_rgba(vf->plane[0], vf->stride[0], vf->plane[1], vf->stride[1], vf->plane[2], vf->stride[2], vf->w,
                     vf->h, px, vf->w, vf->h);
  int s[3] = { 0, 0, 0 };
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      const u8 *p = px + ((size_t)(y + dy) * vf->w + (x + dx)) * 4;
      for (int c = 0; c < 3; c++) s[c] += p[c];
    }
  for (int c = 0; c < 3; c++) rgb[c] = s[c] / 25;
  fm_free(px);
}

static void test_soft_clip(const char *path) {
  FmErr err;
  FmVid *v = vid_open(path, 0, &err);
  TEST_CHECK(v != NULL);
  if (!v) return;
  FmVidInfo in = *vid_info(v);
  TEST_CHECK(in.has_video && in.has_audio);
  TEST_CHECK(in.w == 320 && in.h == 180);
  TEST_CHECK(fabs(in.fps - 25) < 0.01);
  TEST_CHECK(fabs(in.duration - 2.0) < 0.05);
  TEST_CHECK(in.rate == 48000 && in.channels == 1);
  TEST_CHECK(!strcmp(in.vcodec, "vp9") && !strcmp(in.acodec, "opus") && !strcmp(in.backend, "built-in"));
  FmVidFrame vf;
  FmVidPcm pc;
  int ev, guard = 0, nv = 0, bad_planes = 0, bad_time = 0;
  double last_v = -1, last_a = -1, first_a = -1, samples = 0, peak = 0;
  int red[3] = { 0 }, blue[3] = { 0 };
  while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 2000) {
    if (ev == VID_EV_VIDEO) {
      if (vf.w != 320 || vf.h != 180 || !vf.plane[0] || !vf.plane[1] || !vf.plane[2] || vf.stride[0] < 320 ||
          vf.stride[1] < 160 || vf.stride[2] < 160)
        bad_planes++;
      if (vf.t <= last_v) bad_time++;
      if (!nv) {
        TEST_CHECK(fabs(vf.t) < 0.001);
        if (!bad_planes) {
          frame_rgb(&vf, 25, 120, red);
          frame_rgb(&vf, 185, 140, blue);
        }
      }
      last_v = vf.t;
      nv++;
    } else {
      if (pc.rate != 48000 || pc.channels != 1 || pc.frames <= 0) bad_planes++;
      if (first_a < 0) first_a = pc.t;
      if (pc.t + 0.0001 < last_a) bad_time++;
      last_a = pc.t + (double)pc.frames / 48000;
      samples += pc.frames;
      for (int i = 0; i < pc.frames; i++) peak = FM_MAX(peak, fabs(pc.pcm[i]));
    }
  }
  TEST_CHECK(ev == VID_EV_END);
  TEST_CHECK(nv == 50 && bad_planes == 0 && bad_time == 0);
  TEST_CHECK(fabs(last_v - 1.96) < 0.002);
  /* 2 s of sound: pre-skip and the end padding are gone */
  TEST_CHECK(fabs(first_a) < 0.001);
  TEST_CHECK(fabs(samples - 96000) <= 48);
  TEST_CHECK(peak > 0.1 && peak <= 1.0);
  TEST_CHECK(red[0] > 200 && red[1] < 50 && red[2] < 50);
  TEST_CHECK(blue[0] < 50 && blue[1] < 50 && blue[2] > 200);
  printf("  soft: %dx%d %.2f fps %.2fs, v%d a%.0f samples (peak %.2f), red %d,%d,%d blue %d,%d,%d\n", in.w, in.h,
         in.fps, in.duration, nv, samples, peak, red[0], red[1], red[2], blue[0], blue[1], blue[2]);

  /* seek into the middle: the first picture and sound are at the target */
  TEST_CHECK(vid_seek(v, 1.0));
  double sv = -1, sa = -1;
  guard = 0;
  while ((sv < 0 || sa < 0) && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 500) {
    if (ev == VID_EV_VIDEO && sv < 0) sv = vf.t;
    if (ev == VID_EV_AUDIO && sa < 0) sa = pc.t;
  }
  TEST_CHECK(sv >= 0.999 && sv < 1.05);
  TEST_CHECK(sa >= 0.999 && sa < 1.03);
  double sv1 = sv, sa1 = sa;
  /* and back to the start */
  TEST_CHECK(vid_seek(v, 0));
  sv = sa = -1;
  guard = 0;
  while ((sv < 0 || sa < 0) && (ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 500) {
    if (ev == VID_EV_VIDEO && sv < 0) sv = vf.t;
    if (ev == VID_EV_AUDIO && sa < 0) sa = pc.t;
  }
  TEST_CHECK(fabs(sv) < 0.001 && fabs(sa) < 0.001);
  printf("  soft: seek 1.0 -> v %.3f a %.3f, seek 0 -> v %.3f a %.3f\n", sv1, sa1, sv, sa);
  vid_close(v);

  /* one stream only */
  v = vid_open(path, VID_OPEN_AUDIO_ONLY, &err);
  TEST_CHECK(v && !vid_info(v)->has_video && vid_info(v)->has_audio);
  if (v) {
    int nvid = 0, naud = 0;
    guard = 0;
    while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 2000) ev == VID_EV_VIDEO ? nvid++ : naud++;
    TEST_CHECK(nvid == 0 && naud > 50);
    TEST_CHECK(vid_seek(v, 1.5));
    ev = vid_decode(v, &vf, &pc);
    TEST_CHECK(ev == VID_EV_AUDIO && pc.t >= 1.499 && pc.t < 1.53);
    vid_close(v);
  }
  v = vid_open(path, VID_OPEN_NO_AUDIO, &err);
  TEST_CHECK(v && vid_info(v)->has_video && !vid_info(v)->has_audio);
  if (v) {
    int nvid = 0, naud = 0;
    guard = 0;
    while ((ev = vid_decode(v, &vf, &pc)) > 0 && guard++ < 2000) ev == VID_EV_VIDEO ? nvid++ : naud++;
    TEST_CHECK(nvid == 50 && naud == 0);
    vid_close(v);
  }
  FmImage th;
  TEST_CHECK(vid_thumb(path, 64, &th) == FM_OK);
  if (th.px) {
    TEST_CHECK(th.w == 64 && th.h == 36);
    img_free(&th);
  }
}

/* Cut and corrupted copies must fail or end, never crash or hang. */
static void test_soft_damaged(const char *path, const char *tmp) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) return;
  u8 *buf = (u8 *)fm_alloc(64 * 1024);
  size_t n = fread(buf, 1, 64 * 1024, f);
  fclose(f);
  char out[FM_PATH_MAX];
  fm_path_join(out, sizeof out, tmp, "damaged.webm");
  static const size_t kCuts[] = { 4, 40, 300, 2000, 9000, 30000 };
  u32 seed = 12345;
  int opened = 0;
  for (int k = 0; k < FM_COUNT(kCuts) * 2; k++) {
    size_t len = FM_MIN(n, kCuts[k / 2]);
    if (k & 1) {                         /* the whole clip with flipped bytes */
      len = n;
      for (int i = 0; i < 40; i++) {
        seed = seed * 1103515245u + 12345u;
        size_t at = 200 + (seed >> 8) % (n - 200);
        buf[at] ^= (u8)(seed >> 3 | 1);
      }
    }
    FILE *w = fm_fopen(out, "wb");
    if (!w) break;
    fwrite(buf, 1, len, w);
    fclose(w);
    FmErr err;
    FmVid *v = vid_open(out, 0, &err);
    if (!v) continue;
    opened++;
    FmVidFrame vf;
    FmVidPcm pc;
    int guard = 0;
    while (vid_decode(v, &vf, &pc) > 0 && guard < 5000) guard++;
    TEST_CHECK(guard < 5000);
    vid_seek(v, 1.0);
    while (vid_decode(v, &vf, &pc) > 0 && guard < 10000) guard++;
    vid_close(v);
  }
  fm_free(buf);
  printf("  soft: %d damaged copies opened, none crashed\n", opened);
}

/* MMCFM_SOFT_BENCH=<path or url>[|...]: picture decode speed
** (600 frames from a quarter into the clip). */
static void test_soft_bench(void) {
  const char *spec = getenv("MMCFM_SOFT_BENCH");
  if (!spec || !*spec) return;
  char one[4096];
  while (*spec) {
    size_t n = strcspn(spec, "|");
    fm_strlcpy(one, spec, FM_MIN(n + 1, sizeof one));
    spec += n;
    if (*spec == '|') spec++;
    u64 t0 = plat_now_ms();
    FmErr err;
    FmVid *v = vid_open(one, VID_OPEN_NO_AUDIO, &err);
    TEST_CHECK(v != NULL);
    if (!v) { printf("  bench: cannot open %.80s\n", one); continue; }
    FmVidFrame vf;
    FmVidPcm pc;
    int frames = 0, ev = 0, w = 0, h = 0;
    /* from a quarter in (the opening seconds are often a still title); the
    ** clock starts at the first picture there */
    if (vid_info(v)->duration > 20) vid_seek(v, vid_info(v)->duration * 0.25);
    while ((ev = vid_decode(v, &vf, &pc)) == VID_EV_AUDIO) {}
    u64 t1 = plat_now_ms();
    while (ev == VID_EV_VIDEO && frames < 600 && (ev = vid_decode(v, &vf, &pc)) > 0)
      if (ev == VID_EV_VIDEO) { frames++; w = vf.w; h = vf.h; }
    u64 t2 = plat_now_ms();
    double secs = (double)(t2 - t1) / 1000;
    printf("  bench: %dx%d %.2f fps clip: %d frames in %.2f s = %.1f fps decoded (open+seek %d ms)\n", w, h,
           vid_info(v)->fps, frames, secs, secs > 0 ? frames / secs : 0, (int)(t1 - t0));
    TEST_CHECK(frames > 0);
    vid_close(v);
  }
}

int test_soft(const char *tmp) {
  int before = g_test_fail;
  vid_force_backend("soft");
  char path[FM_PATH_MAX];
  if (soft_fixture(path, sizeof path)) {
    test_soft_clip(path);
    test_soft_damaged(path, tmp);
  } else {
    printf("  soft: %s not found, clip tests skipped\n", path);
  }
  /* not WebM: refused at once */
  char junk[FM_PATH_MAX];
  fm_path_join(junk, sizeof junk, tmp, "junk.webm");
  FILE *f = fm_fopen(junk, "wb");
  if (f) {
    fputs("this is not a webm file", f);
    fclose(f);
    FmErr err;
    FmVid *v = vid_open(junk, 0, &err);
    TEST_CHECK(v == NULL);
    if (v) vid_close(v);
  }
  test_soft_bench();
  vid_force_backend(NULL);
  return g_test_fail - before;
}
