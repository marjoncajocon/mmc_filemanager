/* fdec_img.c -- still images (stb_image), SVG (nanosvg), streaming GIF, EXIF.
**
** Design decisions:
**   - stb_image reads through callbacks over an fm_fopen FILE, so the file
**     is streamed into the decoder rather than slurped first.
**   - The size is checked with stbi_info before any pixel is allocated;
**     stb's own buffers use malloc so a failure is a NULL, never an abort.
**   - Downscaling is an alpha-weighted box filter in one pass over the
**     source rows with 64-bit accumulators: exact enough for photos and it
**     never needs a second full-size buffer.
**   - The GIF decoder keeps one RGBA canvas plus, only for "restore to
**     previous" frames, a copy of the frame rectangle. LZW tables live in
**     the decoder struct; input goes through a 16 KB buffer.
*/
#include "fdec_img.h"
#include "fplat.h"

#include "stb_image.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

void img_free(FmImage *im) {
  if (!im) return;
  fm_free(im->px);
  im->px = NULL;
  im->w = im->h = 0;
}

/* ---- sniffing ------------------------------------------------------------ */

static FmImgKind sniff_bytes(const u8 *b, size_t n, const char *path) {
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return IMGK_JPEG;
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return IMGK_PNG;
  if (n >= 4 && memcmp(b, "GIF8", 4) == 0) return IMGK_GIF;
  if (n >= 2 && b[0] == 'B' && b[1] == 'M') return IMGK_BMP;
  if (n >= 4 && memcmp(b, "8BPS", 4) == 0) return IMGK_PSD;
  if (n >= 6 && (memcmp(b, "#?RADI", 6) == 0 || memcmp(b, "#?RGBE", 6) == 0)) return IMGK_HDR;
  if (n >= 4 && b[0] == 0x53 && b[1] == 0x80 && b[2] == 0xF6 && b[3] == 0x34) return IMGK_PIC;
  if (n >= 2 && b[0] == 'P' && (b[1] == '5' || b[1] == '6')) return IMGK_PNM;
  /* SVG: text that mentions <svg early (after an XML prolog / comments). */
  for (size_t i = 0; i + 4 <= n; i++)
    if (b[i] == '<' && (b[i + 1] == 's' || b[i + 1] == 'S') && (b[i + 2] == 'v' || b[i + 2] == 'V') &&
        (b[i + 3] == 'g' || b[i + 3] == 'G'))
      return IMGK_SVG;
  if (path) {
    if (fm_ends_with_i(path, ".svg")) return IMGK_SVG;
    if (fm_ends_with_i(path, ".tga")) return IMGK_TGA;
  }
  return IMGK_UNKNOWN;
}

FmImgKind img_sniff(const char *path) {
  u8 b[1024];
  size_t n = 0;
  FILE *f = fm_fopen(path, "rb");
  if (f) {
    n = fread(b, 1, sizeof b, f);
    fclose(f);
  }
  return sniff_bytes(b, n, path);
}

const char *img_kind_name(FmImgKind k) {
  static const char *const s[] = { "Image", "JPEG", "PNG", "GIF", "SVG", "BMP", "PSD",
                                   "HDR", "PNM", "TGA", "PIC" };
  return (unsigned)k < (unsigned)FM_COUNT(s) ? s[k] : "Image";
}

/* ---- stb callbacks -------------------------------------------------------- */

static int cb_read(void *u, char *data, int size) {
  return (int)fread(data, 1, (size_t)size, (FILE *)u);
}
static void cb_skip(void *u, int n) { fm_fseek64((FILE *)u, n, SEEK_CUR); }
static int cb_eof(void *u) { return feof((FILE *)u); }
static const stbi_io_callbacks kStbIo = { cb_read, cb_skip, cb_eof };

/* ---- EXIF ----------------------------------------------------------------- */

static u32 rd16(const u8 *p, bool le) { return le ? (u32)(p[0] | p[1] << 8) : (u32)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p, bool le) {
  return le ? (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24
            : (u32)p[3] | (u32)p[2] << 8 | (u32)p[1] << 16 | (u32)p[0] << 24;
}

int exif_orientation(const u8 *b, size_t n) {
  if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return 1;
  size_t p = 2;
  while (p + 4 <= n) {
    if (b[p] != 0xFF) return 1;
    u8 m = b[p + 1];
    if (m == 0xFF) { p++; continue; }               /* fill byte */
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { p += 2; continue; }
    if (m == 0xDA || m == 0xD9) return 1;            /* image data: no EXIF before it */
    size_t len = rd16(b + p + 2, false);
    if (len < 2) return 1;
    if (m == 0xE1 && len >= 16 && p + 4 + 6 <= n && memcmp(b + p + 4, "Exif\0\0", 6) == 0) {
      const u8 *t = b + p + 10;                      /* TIFF header */
      size_t tn = FM_MIN(len - 8, n - (p + 10));
      if (tn < 8) return 1;
      bool le;
      if (t[0] == 'I' && t[1] == 'I') le = true;
      else if (t[0] == 'M' && t[1] == 'M') le = false;
      else return 1;
      if (rd16(t + 2, le) != 42) return 1;
      u32 ifd = rd32(t + 4, le);
      if (ifd < 8 || (size_t)ifd + 2 > tn) return 1;
      u32 cnt = rd16(t + ifd, le);
      for (u32 i = 0; i < cnt; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * 12;
        if (e + 12 > tn) return 1;
        if (rd16(t + e, le) == 0x0112) {
          u32 type = rd16(t + e + 2, le);
          u32 v = type == 3 ? rd16(t + e + 8, le) : type == 4 ? rd32(t + e + 8, le) : 1;
          return (v >= 1 && v <= 8) ? (int)v : 1;
        }
      }
      return 1;
    }
    p += 2 + len;
  }
  return 1;
}

/* ---- pixel helpers --------------------------------------------------------- */

void img_fit(int w, int h, int max_w, int max_h, int *ow, int *oh) {
  if (w <= 0 || h <= 0) { *ow = *oh = 0; return; }
  double s = 1.0;
  if (max_w > 0 && w > max_w) s = (double)max_w / w;
  if (max_h > 0 && h * s > max_h) s = (double)max_h / h;
  int rw = (int)(w * s + 0.5), rh = (int)(h * s + 0.5);
  *ow = FM_MAX(1, FM_MIN(rw, w));
  *oh = FM_MAX(1, FM_MIN(rh, h));
}

void img_box_downscale(const u8 *src, int sw, int sh, int sstride, u8 *dst, int dw, int dh) {
  if (dw == sw && dh == sh) {
    for (int y = 0; y < sh; y++) memcpy(dst + (size_t)y * dw * 4, src + (size_t)y * sstride, (size_t)sw * 4);
    return;
  }
  /* acc per destination column: r*a, g*a, b*a, a, count */
  u64 *acc = (u64 *)fm_calloc((size_t)dw * 5, sizeof(u64));
  int *xmap = (int *)fm_alloc((size_t)sw * sizeof(int));
  for (int x = 0; x < sw; x++) xmap[x] = (int)((i64)x * dw / sw);
  int dy = 0;
  for (int y = 0; y < sh; y++) {
    const u8 *s = src + (size_t)y * sstride;
    for (int x = 0; x < sw; x++, s += 4) {
      u64 *a = acc + (size_t)xmap[x] * 5;
      u32 al = s[3];
      a[0] += (u64)s[0] * al;
      a[1] += (u64)s[1] * al;
      a[2] += (u64)s[2] * al;
      a[3] += al;
      a[4] += 1;
    }
    int next = (int)((i64)(y + 1) * dh / sh);
    if (next != dy || y == sh - 1) {
      u8 *d = dst + (size_t)dy * dw * 4;
      for (int x = 0; x < dw; x++, d += 4) {
        u64 *a = acc + (size_t)x * 5;
        if (a[3]) {
          d[0] = (u8)(a[0] / a[3]);
          d[1] = (u8)(a[1] / a[3]);
          d[2] = (u8)(a[2] / a[3]);
          d[3] = (u8)(a[3] / (a[4] ? a[4] : 1));
        } else {
          d[0] = d[1] = d[2] = d[3] = 0;
        }
      }
      memset(acc, 0, (size_t)dw * 5 * sizeof(u64));
      /* a source row maps to exactly one destination row when sh >= dh */
      dy = next;
      if (dy >= dh) break;
    }
  }
  fm_free(xmap);
  fm_free(acc);
}

FmImage img_orient(FmImage im, int o) {
  if (o < 2 || o > 8 || !im.px) return im;
  int W = im.w, H = im.h;
  bool swap = o >= 5;
  FmImage r = { swap ? H : W, swap ? W : H, NULL };
  r.px = (u8 *)fm_alloc((size_t)r.w * r.h * 4);
  const u32 *s = (const u32 *)im.px;
  u32 *d = (u32 *)r.px;
  for (int dy = 0; dy < r.h; dy++)
    for (int dx = 0; dx < r.w; dx++) {
      int sx, sy;
      switch (o) {
        case 2: sx = W - 1 - dx; sy = dy; break;
        case 3: sx = W - 1 - dx; sy = H - 1 - dy; break;
        case 4: sx = dx; sy = H - 1 - dy; break;
        case 5: sx = dy; sy = dx; break;
        case 6: sx = dy; sy = H - 1 - dx; break;
        case 7: sx = W - 1 - dy; sy = H - 1 - dx; break;
        default: sx = W - 1 - dy; sy = dx; break;   /* 8 */
      }
      d[(size_t)dy * r.w + dx] = s[(size_t)sy * W + sx];
    }
  img_free(&im);
  return r;
}

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

void img_yuv420_to_rgba(const u8 *Y, int ys, const u8 *U, int us, const u8 *V, int vs, int w, int h,
                        u8 *dst, int dw, int dh) {
  bool direct = dw == w && dh == h;
  u8 *full = direct ? dst : (u8 *)fm_alloc((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    const u8 *yr = Y + (size_t)y * ys, *ur = U + (size_t)(y >> 1) * us, *vr = V + (size_t)(y >> 1) * vs;
    u8 *d = full + (size_t)y * w * 4;
    for (int x = 0; x < w; x++, d += 4) {
      int c = (yr[x] - 16) * 298, du = ur[x >> 1] - 128, dv = vr[x >> 1] - 128;
      d[0] = clamp8((c + 409 * dv + 128) >> 8);
      d[1] = clamp8((c - 100 * du - 208 * dv + 128) >> 8);
      d[2] = clamp8((c + 516 * du + 128) >> 8);
      d[3] = 255;
    }
  }
  if (!direct) {
    img_box_downscale(full, w, h, w * 4, dst, dw, dh);
    fm_free(full);
  }
}

/* Takes stb's buffer (w*h*4), fits it and returns an fm_alloc image. */
static FmImage fit_take(u8 *stb_px, int w, int h, int max_w, int max_h) {
  FmImage r;
  img_fit(w, h, max_w, max_h, &r.w, &r.h);
  r.px = (u8 *)fm_alloc((size_t)r.w * r.h * 4);
  img_box_downscale(stb_px, w, h, w * 4, r.px, r.w, r.h);
  stbi_image_free(stb_px);
  return r;
}

FmErr img_load_mem(const u8 *data, size_t n, int max_px, FmImage *out) {
  memset(out, 0, sizeof *out);
  if (!data || n < 8 || n > 0x7FFFFFFF) return FM_ERR_FORMAT;
  int w, h, c;
  if (!stbi_info_from_memory(data, (int)n, &w, &h, &c)) return FM_ERR_FORMAT;
  if ((u64)w * h > IMG_MAX_PIXELS) return FM_ERR_UNSUPPORTED;
  u8 *px = stbi_load_from_memory(data, (int)n, &w, &h, &c, 4);
  if (!px) return FM_ERR_FORMAT;
  *out = fit_take(px, w, h, max_px, max_px);
  return FM_OK;
}

/* ---- still images ----------------------------------------------------------- */

bool img_info(const char *path, FmImgInfo *info) {
  memset(info, 0, sizeof *info);
  info->orient = 1;
  info->frames = 1;
  info->kind = img_sniff(path);
  if (info->kind == IMGK_GIF) {
    FmErr e;
    FmGif *g = gif_open(path, true, &e);
    if (!g) return false;
    info->w = gif_width(g);
    info->h = gif_height(g);
    info->frames = gif_frames(g);
    gif_close(g);
    return true;
  }
  if (info->kind == IMGK_SVG) {
    FmErr e;
    FmSvg *s = svg_open(path, &e);
    if (!s) return false;
    float w, h;
    svg_size(s, &w, &h);
    info->w = (int)(w + 0.5f);
    info->h = (int)(h + 0.5f);
    svg_close(s);
    return true;
  }
  FILE *f = fm_fopen(path, "rb");
  if (!f) return false;
  int c;
  bool ok = stbi_info_from_callbacks(&kStbIo, f, &info->w, &info->h, &c) != 0;
  if (ok && info->kind == IMGK_JPEG) {
    u8 *b = (u8 *)fm_alloc(65536);
    fm_fseek64(f, 0, SEEK_SET);
    size_t n = fread(b, 1, 65536, f);
    info->orient = exif_orientation(b, n);
    fm_free(b);
  }
  fclose(f);
  return ok;
}

static FmErr load_svg_fit(const char *path, int max_w, int max_h, FmImage *out, FmImgInfo *info) {
  FmErr err;
  FmSvg *s = svg_open(path, &err);
  if (!s) return err;
  float w, h;
  svg_size(s, &w, &h);
  if (info) { info->w = (int)(w + 0.5f); info->h = (int)(h + 0.5f); }
  /* vectors scale up for free: fill the box when one is given */
  double sc = 1.0;
  if (max_w > 0 && max_h > 0) sc = FM_MIN(max_w / w, max_h / h);
  else if (max_w > 0) sc = max_w / w;
  else if (max_h > 0) sc = max_h / h;
  int rw = FM_MAX(1, (int)(w * sc + 0.5)), rh = FM_MAX(1, (int)(h * sc + 0.5));
  if ((u64)rw * rh > IMG_MAX_PIXELS / 4) { svg_close(s); return FM_ERR_UNSUPPORTED; }
  err = svg_render(s, rw, rh, out);
  svg_close(s);
  return err;
}

static FmErr load_gif_first(const char *path, int max_w, int max_h, FmImage *out, FmImgInfo *info) {
  FmErr err;
  FmGif *g = gif_open(path, false, &err);
  if (!g) return err;
  int delay;
  if (gif_next(g, &delay) != 1) { gif_close(g); return FM_ERR_FORMAT; }
  int w = gif_width(g), h = gif_height(g);
  if (info) { info->w = w; info->h = h; }
  img_fit(w, h, max_w, max_h, &out->w, &out->h);
  out->px = (u8 *)fm_alloc((size_t)out->w * out->h * 4);
  img_box_downscale(gif_pixels(g), w, h, w * 4, out->px, out->w, out->h);
  gif_close(g);
  return FM_OK;
}

FmErr img_load(const char *path, int max_w, int max_h, FmImage *out, FmImgInfo *info,
               const volatile int *cancel) {
  memset(out, 0, sizeof *out);
  FmImgInfo tmp;
  if (!info) info = &tmp;
  memset(info, 0, sizeof *info);
  info->orient = 1;
  info->frames = 1;
  FILE *f = fm_fopen(path, "rb");
  if (!f) return FM_ERR_NOT_FOUND;
  u8 head[1024];
  size_t hn = fread(head, 1, sizeof head, f);
  info->kind = sniff_bytes(head, hn, path);
  if (info->kind == IMGK_SVG) { fclose(f); return load_svg_fit(path, max_w, max_h, out, info); }
  if (info->kind == IMGK_GIF) { fclose(f); return load_gif_first(path, max_w, max_h, out, info); }
  if (info->kind == IMGK_UNKNOWN) { fclose(f); return FM_ERR_FORMAT; }

  if (info->kind == IMGK_JPEG) {
    u8 *b = (u8 *)fm_alloc(65536);
    fm_fseek64(f, 0, SEEK_SET);
    size_t n = fread(b, 1, 65536, f);
    info->orient = exif_orientation(b, n);
    fm_free(b);
  }
  fm_fseek64(f, 0, SEEK_SET);
  int w, h, c;
  if (!stbi_info_from_callbacks(&kStbIo, f, &w, &h, &c)) { fclose(f); return FM_ERR_FORMAT; }
  info->w = w;
  info->h = h;
  if ((u64)w * h > IMG_MAX_PIXELS) { fclose(f); return FM_ERR_UNSUPPORTED; }
  if (cancel && *cancel) { fclose(f); return FM_ERR_CANCEL; }
  fm_fseek64(f, 0, SEEK_SET);
  clearerr(f);
  u8 *px = stbi_load_from_callbacks(&kStbIo, f, &w, &h, &c, 4);
  fclose(f);
  if (!px) return FM_ERR_FORMAT;
  if (cancel && *cancel) { stbi_image_free(px); return FM_ERR_CANCEL; }
  bool swap = info->orient >= 5;
  *out = fit_take(px, w, h, swap ? max_h : max_w, swap ? max_w : max_h);
  *out = img_orient(*out, info->orient);
  return FM_OK;
}

/* ---- SVG -------------------------------------------------------------------- */

struct FmSvg {
  NSVGimage *img;
  NSVGrasterizer *rast;
};

FmSvg *svg_open_mem(const char *text, size_t n, FmErr *err) {
  char *buf = (char *)fm_alloc(n + 1);
  memcpy(buf, text, n);
  buf[n] = 0;
  NSVGimage *img = nsvgParse(buf, "px", 96.0f);   /* modifies buf */
  fm_free(buf);
  if (!img) { *err = FM_ERR_FORMAT; return NULL; }
  if (!(img->width > 0) || !(img->height > 0) || img->width > 1e6f || img->height > 1e6f) {
    img->width = 512;
    img->height = 512;
  }
  FmSvg *s = (FmSvg *)fm_calloc(1, sizeof *s);
  s->img = img;
  *err = FM_OK;
  return s;
}

FmSvg *svg_open(const char *path, FmErr *err) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) { *err = FM_ERR_NOT_FOUND; return NULL; }
  i64 n = fm_fsize(f);
  if (n <= 0 || n > (i64)IMG_SVG_MAX_FILE) { fclose(f); *err = n <= 0 ? FM_ERR_FORMAT : FM_ERR_UNSUPPORTED; return NULL; }
  char *buf = (char *)fm_alloc((size_t)n);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  FmSvg *s = svg_open_mem(buf, got, err);
  fm_free(buf);
  return s;
}

void svg_size(const FmSvg *s, float *w, float *h) {
  *w = s->img->width;
  *h = s->img->height;
}

FmErr svg_render(FmSvg *s, int w, int h, FmImage *out) {
  memset(out, 0, sizeof *out);
  if (w <= 0 || h <= 0) return FM_ERR_FORMAT;
  if (!s->rast) s->rast = nsvgCreateRasterizer();
  if (!s->rast) return FM_ERR_NOMEM;
  out->w = w;
  out->h = h;
  out->px = (u8 *)fm_calloc((size_t)w * h, 4);
  float sc = FM_MIN(w / s->img->width, h / s->img->height);
  float tx = (w - s->img->width * sc) * 0.5f, ty = (h - s->img->height * sc) * 0.5f;
  nsvgRasterize(s->rast, s->img, tx, ty, sc, out->px, w, h, w * 4);
  return FM_OK;
}

void svg_close(FmSvg *s) {
  if (!s) return;
  if (s->rast) nsvgDeleteRasterizer(s->rast);
  nsvgDelete(s->img);
  fm_free(s);
}

/* ---- GIF: buffered reader ------------------------------------------------------ */

typedef struct GifRd {
  FILE *f;
  const u8 *mem;
  size_t mem_n;
  i64 base;           /* stream offset of buf[0] */
  int pos, len;
  u8 buf[16384];
} GifRd;

static bool rd_fill(GifRd *r) {
  r->base += r->len;
  r->pos = 0;
  if (r->mem) {
    size_t off = (size_t)r->base;
    size_t n = off < r->mem_n ? FM_MIN(r->mem_n - off, sizeof r->buf) : 0;
    memcpy(r->buf, r->mem + off, n);
    r->len = (int)n;
  } else {
    r->len = (int)fread(r->buf, 1, sizeof r->buf, r->f);
  }
  return r->len > 0;
}

static inline int rd_byte(GifRd *r) {
  if (r->pos >= r->len && !rd_fill(r)) return -1;
  return r->buf[r->pos++];
}

static bool rd_bytes(GifRd *r, u8 *d, int n) {
  for (int i = 0; i < n; i++) {
    int c = rd_byte(r);
    if (c < 0) return false;
    d[i] = (u8)c;
  }
  return true;
}

static i64 rd_tell(const GifRd *r) { return r->base + r->pos; }

static bool rd_seek(GifRd *r, i64 off) {
  if (off >= r->base && off <= r->base + r->len) { r->pos = (int)(off - r->base); return true; }
  r->base = off;
  r->pos = r->len = 0;
  if (!r->mem && fm_fseek64(r->f, off, SEEK_SET) != 0) return false;
  return true;
}

/* Skips data sub-blocks up to and including the terminator. */
static bool rd_skip_blocks(GifRd *r) {
  for (;;) {
    int n = rd_byte(r);
    if (n < 0) return false;
    if (n == 0) return true;
    i64 to = rd_tell(r) + n;
    if (to <= r->base + r->len) r->pos += n;
    else if (!rd_seek(r, to)) return false;
  }
}

/* ---- GIF: decoder ------------------------------------------------------------- */

struct FmGif {
  GifRd rd;
  int w, h;
  u8 gct[256 * 3];
  int gct_n;
  int frames, loops;
  i64 data_start;
  u8 *canvas;
  u8 *saved;                    /* rect copy for disposal 3 */
  size_t saved_cap;
  int prev_disp, px, py, pw, ph;
  int decoded;                  /* frames decoded since the last rewind */
  int gce_disp, gce_delay, gce_trans;
  /* LZW */
  u16 prefix[4096];
  u8 suffix[4096];
  u8 stack[4097];
};

#define GIF_MAX_SIDE 16384
#define GIF_MAX_PIXELS (64u * 1024u * 1024u)

static void gif_reset_state(FmGif *g) {
  g->prev_disp = 0;
  g->decoded = 0;
  g->gce_disp = 0;
  g->gce_delay = 0;
  g->gce_trans = -1;
  memset(g->canvas, 0, (size_t)g->w * g->h * 4);
}

static void gif_count(FmGif *g) {
  GifRd *r = &g->rd;
  int n = 0;
  for (;;) {
    int b = rd_byte(r);
    if (b < 0 || b == 0x3B) break;
    if (b == 0x21) {
      if (rd_byte(r) < 0 || !rd_skip_blocks(r)) break;
    } else if (b == 0x2C) {
      u8 d[9];
      if (!rd_bytes(r, d, 9)) break;
      if (d[8] & 0x80) {
        int lct = 3 * (2 << (d[8] & 7));
        if (!rd_seek(r, rd_tell(r) + lct)) break;
      }
      if (rd_byte(r) < 0 || !rd_skip_blocks(r)) break;
      n++;
    } else {
      break;
    }
  }
  g->frames = n;
}

static FmGif *gif_init(FmGif *g, bool count, FmErr *err) {
  GifRd *r = &g->rd;
  u8 h[13];
  g->gce_trans = -1;
  if (!rd_bytes(r, h, 13) || memcmp(h, "GIF8", 4) != 0) { *err = FM_ERR_FORMAT; goto fail; }
  g->w = h[6] | h[7] << 8;
  g->h = h[8] | h[9] << 8;
  if (h[10] & 0x80) {
    g->gct_n = 2 << (h[10] & 7);
    if (!rd_bytes(r, g->gct, g->gct_n * 3)) { *err = FM_ERR_FORMAT; goto fail; }
  }
  g->data_start = rd_tell(r);
  if (g->w == 0 || g->h == 0) {
    /* No logical screen size: take the first frame's extent. */
    for (;;) {
      int b = rd_byte(r);
      if (b == 0x21) { if (rd_byte(r) < 0 || !rd_skip_blocks(r)) break; continue; }
      if (b == 0x2C) {
        u8 d[9];
        if (rd_bytes(r, d, 9)) {
          g->w = (d[0] | d[1] << 8) + (d[4] | d[5] << 8);
          g->h = (d[2] | d[3] << 8) + (d[6] | d[7] << 8);
        }
      }
      break;
    }
    rd_seek(r, g->data_start);
  }
  if (g->w <= 0 || g->h <= 0) { *err = FM_ERR_FORMAT; goto fail; }
  if (g->w > GIF_MAX_SIDE || g->h > GIF_MAX_SIDE || (u64)g->w * g->h > GIF_MAX_PIXELS) {
    *err = FM_ERR_UNSUPPORTED;
    goto fail;
  }
  /* loop count lives in an application extension before the first frame */
  if (count) {
    gif_count(g);
    rd_seek(r, g->data_start);
  }
  g->canvas = (u8 *)fm_calloc((size_t)g->w * g->h, 4);
  gif_reset_state(g);
  for (int i = 0; i < 256; i++) g->suffix[i] = (u8)i;
  *err = FM_OK;
  return g;
fail:
  if (g->rd.f) fclose(g->rd.f);
  fm_free(g);
  return NULL;
}

FmGif *gif_open(const char *path, bool count, FmErr *err) {
  FILE *f = fm_fopen(path, "rb");
  if (!f) { *err = FM_ERR_NOT_FOUND; return NULL; }
  FmGif *g = (FmGif *)fm_calloc(1, sizeof *g);
  g->rd.f = f;
  return gif_init(g, count, err);
}

FmGif *gif_open_mem(const u8 *data, size_t n, bool count, FmErr *err) {
  FmGif *g = (FmGif *)fm_calloc(1, sizeof *g);
  g->rd.mem = data;
  g->rd.mem_n = n;
  return gif_init(g, count, err);
}

int gif_width(const FmGif *g) { return g->w; }
int gif_height(const FmGif *g) { return g->h; }
int gif_frames(const FmGif *g) { return g->frames; }
int gif_loops(const FmGif *g) { return g->loops; }
const u8 *gif_pixels(const FmGif *g) { return g->canvas; }

void gif_close(FmGif *g) {
  if (!g) return;
  if (g->rd.f) fclose(g->rd.f);
  fm_free(g->canvas);
  fm_free(g->saved);
  fm_free(g);
}

bool gif_rewind(FmGif *g) {
  if (!rd_seek(&g->rd, g->data_start)) return false;
  gif_reset_state(g);
  return true;
}

/* Bit reader over the image's data sub-blocks. */
typedef struct LzwIn {
  GifRd *r;
  int left;           /* bytes left in the current sub-block */
  bool done;          /* terminator seen */
  u32 bits;
  int nbits;
} LzwIn;

static int lzw_code(LzwIn *in, int size) {
  while (in->nbits < size) {
    if (in->done) return -1;
    if (in->left == 0) {
      int n = rd_byte(in->r);
      if (n <= 0) { in->done = true; return -1; }
      in->left = n;
    }
    int c = rd_byte(in->r);
    if (c < 0) { in->done = true; return -1; }
    in->left--;
    in->bits |= (u32)c << in->nbits;
    in->nbits += 8;
  }
  int code = (int)(in->bits & ((1u << size) - 1));
  in->bits >>= size;
  in->nbits -= size;
  return code;
}

static void lzw_finish(LzwIn *in) {
  if (in->done) return;
  while (in->left > 0) { if (rd_byte(in->r) < 0) return; in->left--; }
  rd_skip_blocks(in->r);
}

typedef struct GifOut {
  u8 *canvas;
  int cw, ch;
  int fx, fy, fw, fh;
  int x, y, pass;
  bool interlace;
  int trans;
  const u8 *pal;
  int pal_n;
  u32 left;           /* pixels still expected */
} GifOut;

static const int kPassStart[4] = { 0, 4, 2, 1 };
static const int kPassStep[4] = { 8, 8, 4, 2 };

static inline void gif_put(GifOut *o, int idx) {
  if (o->left == 0) return;
  o->left--;
  int cx = o->fx + o->x, cy = o->fy + o->y;
  if (idx != o->trans && cx < o->cw && cy < o->ch) {
    u8 *d = o->canvas + ((size_t)cy * o->cw + cx) * 4;
    if (idx < o->pal_n) {
      d[0] = o->pal[idx * 3];
      d[1] = o->pal[idx * 3 + 1];
      d[2] = o->pal[idx * 3 + 2];
    } else {
      d[0] = d[1] = d[2] = 0;
    }
    d[3] = 255;
  }
  if (++o->x >= o->fw) {
    o->x = 0;
    if (o->interlace) {
      o->y += kPassStep[o->pass];
      while (o->y >= o->fh && o->pass < 3) {
        o->pass++;
        o->y = kPassStart[o->pass];
      }
    } else {
      o->y++;
    }
  }
}

static void gif_lzw(FmGif *g, GifOut *o) {
  int min = rd_byte(&g->rd);
  LzwIn in = { &g->rd, 0, false, 0, 0 };
  if (min < 1 || min > 11) { lzw_finish(&in); return; }
  int clear = 1 << min, eoi = clear + 1;
  int size = min + 1, avail = clear + 2, old = -1, first = 0;
  for (;;) {
    int code = lzw_code(&in, size);
    if (code < 0 || code == eoi) break;
    if (code == clear) {
      size = min + 1;
      avail = clear + 2;
      old = -1;
      continue;
    }
    if (old < 0) {
      if (code >= clear) break;
      gif_put(o, code);
      old = first = code;
      continue;
    }
    int in_code = code, sp = 0;
    if (code > avail) break;               /* corrupt */
    if (code == avail) {
      g->stack[sp++] = (u8)first;
      code = old;
    }
    while (code >= clear) {
      if (sp >= 4096 || code >= 4096) { sp = -1; break; }
      g->stack[sp++] = g->suffix[code];
      code = g->prefix[code];
    }
    if (sp < 0) break;
    first = code;
    g->stack[sp++] = (u8)first;
    if (avail < 4096) {
      g->prefix[avail] = (u16)old;
      g->suffix[avail] = (u8)first;
      avail++;
      if (avail == (1 << size) && size < 12) size++;
    }
    old = in_code;
    while (sp > 0) gif_put(o, g->stack[--sp]);
    if (o->left == 0) break;
  }
  lzw_finish(&in);
}

static void gif_dispose_prev(FmGif *g) {
  if (g->prev_disp == 2) {
    for (int y = 0; y < g->ph; y++)
      memset(g->canvas + ((size_t)(g->py + y) * g->w + g->px) * 4, 0, (size_t)g->pw * 4);
  } else if (g->prev_disp == 3 && g->saved) {
    for (int y = 0; y < g->ph; y++)
      memcpy(g->canvas + ((size_t)(g->py + y) * g->w + g->px) * 4, g->saved + (size_t)y * g->pw * 4,
             (size_t)g->pw * 4);
  }
  g->prev_disp = 0;
}

int gif_next(FmGif *g, int *delay_ms) {
  GifRd *r = &g->rd;
  gif_dispose_prev(g);
  for (;;) {
    int b = rd_byte(r);
    if (b < 0 || b == 0x3B) return 0;
    if (b == 0x21) {
      int label = rd_byte(r);
      if (label < 0) return 0;
      int n = rd_byte(r);
      if (n < 0) return 0;
      if (n == 0) continue;
      u8 blk[255];
      if (!rd_bytes(r, blk, n)) return 0;
      if (label == 0xF9 && n >= 4) {
        g->gce_disp = (blk[0] >> 2) & 7;
        g->gce_delay = blk[1] | blk[2] << 8;
        g->gce_trans = (blk[0] & 1) ? blk[3] : -1;
      } else if (label == 0xFF && n == 11 && memcmp(blk, "NETSCAPE2.0", 11) == 0) {
        for (;;) {
          int m = rd_byte(r);
          if (m < 0) return 0;
          if (m == 0) break;
          if (!rd_bytes(r, blk, m)) return 0;
          if (m >= 3 && blk[0] == 1) g->loops = blk[1] | blk[2] << 8;
        }
        continue;
      }
      if (!rd_skip_blocks(r)) return 0;
      continue;
    }
    if (b != 0x2C) return g->decoded ? 0 : -1;
    u8 d[9];
    if (!rd_bytes(r, d, 9)) return 0;
    int fx = d[0] | d[1] << 8, fy = d[2] | d[3] << 8, fw = d[4] | d[5] << 8, fh = d[6] | d[7] << 8;
    u8 lct[256 * 3];
    const u8 *pal = g->gct;
    int pal_n = g->gct_n;
    if (d[8] & 0x80) {
      pal_n = 2 << (d[8] & 7);
      if (!rd_bytes(r, lct, pal_n * 3)) return 0;
      pal = lct;
    }
    if (pal_n == 0) {
      for (int i = 0; i < 256; i++) lct[i * 3] = lct[i * 3 + 1] = lct[i * 3 + 2] = (u8)i;
      pal = lct;
      pal_n = 256;
    }
    /* visible part of the frame, for disposal */
    int cx0 = FM_MIN(fx, g->w), cy0 = FM_MIN(fy, g->h);
    int cx1 = FM_MIN(fx + fw, g->w), cy1 = FM_MIN(fy + fh, g->h);
    int vw = cx1 - cx0, vh = cy1 - cy0;
    if (g->gce_disp == 3 && vw > 0 && vh > 0) {
      size_t need = (size_t)vw * vh * 4;
      if (need > g->saved_cap) {
        fm_free(g->saved);
        g->saved = (u8 *)fm_alloc(need);
        g->saved_cap = need;
      }
      for (int y = 0; y < vh; y++)
        memcpy(g->saved + (size_t)y * vw * 4, g->canvas + ((size_t)(cy0 + y) * g->w + cx0) * 4,
               (size_t)vw * 4);
    }
    GifOut o;
    memset(&o, 0, sizeof o);
    o.canvas = g->canvas;
    o.cw = g->w;
    o.ch = g->h;
    o.fx = fx; o.fy = fy; o.fw = fw; o.fh = fh;
    o.interlace = (d[8] & 0x40) != 0;
    o.trans = g->gce_trans;
    o.pal = pal;
    o.pal_n = pal_n;
    o.left = (u32)fw * (u32)fh;
    gif_lzw(g, &o);
    g->prev_disp = (vw > 0 && vh > 0) ? g->gce_disp : 0;
    g->px = cx0; g->py = cy0; g->pw = FM_MAX(vw, 0); g->ph = FM_MAX(vh, 0);
    int ms = g->gce_delay * 10;
    if (ms <= 10) ms = 100;
    else if (ms < 20) ms = 20;
    *delay_ms = ms;
    g->decoded++;
    g->gce_disp = 0;
    g->gce_delay = 0;
    g->gce_trans = -1;
    return 1;
  }
}
