/* fdec_img.h -- image decoding shared by the image viewer and thumbnails.
**
** Still images go through stb_image, SVG through nanosvg, GIF through our
** own streaming decoder. Everything returns 8-bit RGBA (r,g,b,a bytes in
** memory order) allocated with fm_alloc. No SDL here: safe on any thread.
**
** Design decisions:
**   - Decoding and fitting are one call (img_load): the caller says how big
**     the result may be (texture limit, thumbnail edge) and gets an image
**     already box-filtered down and EXIF-rotated, so the full-size pixels
**     never leave the worker thread.
**   - GIF is decoded frame by frame into one canvas, never all frames at
**     once: memory is a few canvases whatever the frame count.
**   - Limits are explicit (IMG_MAX_PIXELS, file size caps) so a hostile or
**     absurd file gives an error instead of exhausting memory.
*/
#ifndef FDEC_IMG_H
#define FDEC_IMG_H

#include "fcore.h"

#define IMG_MAX_PIXELS (100u * 1000u * 1000u)   /* decoded size cap (400 MB RGBA) */
#define IMG_SVG_MAX_FILE (16u * 1024u * 1024u)

typedef struct FmImage {
  int w, h;
  u8 *px;          /* w*h*4 RGBA, fm_alloc */
} FmImage;

void img_free(FmImage *im);

typedef enum FmImgKind {
  IMGK_UNKNOWN = 0, IMGK_JPEG, IMGK_PNG, IMGK_GIF, IMGK_SVG, IMGK_BMP, IMGK_PSD,
  IMGK_HDR, IMGK_PNM, IMGK_TGA, IMGK_PIC
} FmImgKind;

/* Kind from the first bytes, falling back to the extension (TGA has no magic). */
FmImgKind img_sniff(const char *path);
const char *img_kind_name(FmImgKind k);

typedef struct FmImgInfo {
  int w, h;            /* original size, before EXIF rotation */
  int frames;          /* GIF frame count (1 for stills, 0 if not counted) */
  int orient;          /* EXIF orientation 1..8 (1 = as stored) */
  FmImgKind kind;
} FmImgInfo;

/* Size and kind without decoding pixels. */
bool img_info(const char *path, FmImgInfo *info);

/* Decodes any supported still (or the first GIF frame, or an SVG at its
** natural size) into RGBA that fits max_w x max_h (0 = no limit), keeping
** the aspect ratio, with EXIF orientation applied. `info` may be NULL.
** `cancel`, when not NULL, is polled between stages. */
FmErr img_load(const char *path, int max_w, int max_h, FmImage *out, FmImgInfo *info,
               const volatile int *cancel);

/* ---- pixel helpers ------------------------------------------------------ */

/* Largest size with the same aspect that fits in max_w x max_h (never upscales). */
void img_fit(int w, int h, int max_w, int max_h, int *ow, int *oh);
/* Area-averaging downscale of RGBA (dw <= sw, dh <= sh). */
void img_box_downscale(const u8 *src, int sw, int sh, int sstride, u8 *dst, int dw, int dh);
/* Applies EXIF orientation 2..8; returns a new image and frees the input. */
FmImage img_orient(FmImage im, int orient);
/* EXIF orientation from the start of a JPEG file (1 when absent). */
int exif_orientation(const u8 *jpeg, size_t n);
/* BT.601 limited-range YUV 4:2:0 to RGBA, downscaled to dw x dh. */
void img_yuv420_to_rgba(const u8 *y, int ys, const u8 *u, int us, const u8 *v, int vs,
                        int w, int h, u8 *dst, int dw, int dh);
/* Decodes an encoded image held in memory (cover art) to fit max px. */
FmErr img_load_mem(const u8 *data, size_t n, int max_px, FmImage *out);

/* ---- SVG ------------------------------------------------------------------ */

typedef struct FmSvg FmSvg;

FmSvg *svg_open(const char *path, FmErr *err);
FmSvg *svg_open_mem(const char *text, size_t n, FmErr *err);
void   svg_size(const FmSvg *s, float *w, float *h);       /* document size in px */
/* Rasterizes the whole document scaled to w x h. */
FmErr  svg_render(FmSvg *s, int w, int h, FmImage *out);
void   svg_close(FmSvg *s);

/* ---- GIF ------------------------------------------------------------------ */

typedef struct FmGif FmGif;

/* Opens and reads the header; count_frames scans the file once (fast, no
** LZW decoding) so gif_frames() is exact. */
FmGif *gif_open(const char *path, bool count_frames, FmErr *err);
FmGif *gif_open_mem(const u8 *data, size_t n, bool count_frames, FmErr *err);
int    gif_width(const FmGif *g);
int    gif_height(const FmGif *g);
int    gif_frames(const FmGif *g);        /* 0 when not counted */
int    gif_loops(const FmGif *g);         /* NETSCAPE loop count, 0 = forever */
/* Decodes the next frame onto the canvas. Returns 1 with the frame's delay
** (browser rules: <= 10 ms becomes 100 ms, minimum 20 ms), 0 at the end of
** the stream, -1 on a hard error. */
int    gif_next(FmGif *g, int *delay_ms);
/* Back to the first frame (clears the canvas). */
bool   gif_rewind(FmGif *g);
const u8 *gif_pixels(const FmGif *g);     /* canvas, w*h*4 RGBA */
void   gif_close(FmGif *g);

#endif
