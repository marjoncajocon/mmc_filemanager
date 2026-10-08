/* ficon.c -- the icon set, as code on a 24-unit grid.
**
** Design decisions:
**   - Each icon is a few strokes and fills through tiny helpers (L, P, RR,
**     RING, ARC, DOT, FILL, TRI) that map grid units to the target rect.
**   - Stroke width follows the size (2 units, at least 1.2 px), so icons
**     stay legible from 14 px menu icons to 64 px empty-state art.
*/
#include "ficon.h"
#include "fui.h"
#include <math.h>

#define PI_F 3.14159265f

static FmRect R;     /* target */
static FmColor C;
static float S;      /* px per unit */
static float SW;     /* stroke width px */

#define X(u) (R.x + (u) * S)
#define Y(u) (R.y + (u) * S)

static void L(float x0, float y0, float x1, float y1) {
  gfx_line(X(x0), Y(y0), X(x1), Y(y1), SW, C);
  gfx_circle(X(x0), Y(y0), SW * 0.5f, C);
  gfx_circle(X(x1), Y(y1), SW * 0.5f, C);
}

static void P(int n, const float *uv, bool closed) {
  float xy[64];
  if (n > 32) n = 32;
  for (int i = 0; i < n; i++) { xy[2 * i] = X(uv[2 * i]); xy[2 * i + 1] = Y(uv[2 * i + 1]); }
  gfx_polyline(xy, n, SW, C, closed);
  if (SW < 2.0f)   /* polyline adds round joins only for thick strokes */
    for (int i = 0; i < n; i++) gfx_circle(xy[2 * i], xy[2 * i + 1], SW * 0.5f, C);
}
#define POLY(closed, ...) do { static const float p_[] = { __VA_ARGS__ }; \
    P(FM_COUNT(p_) / 2, p_, closed); } while (0)

static void RR(float x, float y, float w, float h, float r) {
  gfx_rrect_line(FM_RECT(X(x), Y(y), w * S, h * S), r * S, SW, C);
}

static void FILL(float x, float y, float w, float h, float r) {
  gfx_rrect(FM_RECT(X(x), Y(y), w * S, h * S), r * S, C);
}

static void RING(float cx, float cy, float r) { gfx_ring(X(cx), Y(cy), r * S, SW, C); }
static void DOT(float cx, float cy, float r) { gfx_circle(X(cx), Y(cy), r * S, C); }

static void ARC(float cx, float cy, float r, float a0deg, float a1deg) {
  gfx_arc(X(cx), Y(cy), r * S, SW, a0deg * PI_F / 180.0f, a1deg * PI_F / 180.0f, C);
}

static void TRI(float x0, float y0, float x1, float y1, float x2, float y2) {
  gfx_tri(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), C);
}

static void CONVEX(int n, const float *uv) {
  float xy[64];
  for (int i = 0; i < n && i < 32; i++) { xy[2 * i] = X(uv[2 * i]); xy[2 * i + 1] = Y(uv[2 * i + 1]); }
  gfx_convex(xy, n, C);
}
#define SHAPE(...) do { static const float p_[] = { __VA_ARGS__ }; CONVEX(FM_COUNT(p_) / 2, p_); } while (0)

/* ---- shared parts ------------------------------------------------------- */

static void doc_outline(void) {
  POLY(true, 6, 3, 14, 3, 19, 8, 19, 21, 6, 21);
  POLY(false, 14, 3, 14, 8, 19, 8);
}

static void folder_outline(void) {
  POLY(true, 3, 6, 9, 6, 11, 8, 21, 8, 21, 19, 3, 19);
}

static void arrow_head(float x, float y, float dx, float dy) {
  /* chevron pointing (dx,dy) with tip at x,y */
  float px = -dy, py = dx;
  L(x, y, x - dx * 4 + px * 4, y - dy * 4 + py * 4);
  L(x, y, x - dx * 4 - px * 4, y - dy * 4 - py * 4);
}

static void star(bool filled) {
  float pts[20];
  for (int i = 0; i < 10; i++) {
    float a = -PI_F / 2 + i * PI_F / 5;
    float r = (i & 1) ? 4.0f : 9.0f;
    pts[2 * i] = 12 + cosf(a) * r;
    pts[2 * i + 1] = 12.5f + sinf(a) * r;
  }
  if (filled) {
    /* center fan of convex triangles */
    for (int i = 0; i < 10; i++) {
      int j = (i + 1) % 10;
      TRI(12, 12.5f, pts[2 * i], pts[2 * i + 1], pts[2 * j], pts[2 * j + 1]);
    }
  } else {
    P(10, pts, true);
  }
}

static void gear(void) {
  RING(12, 12, 3);
  for (int i = 0; i < 8; i++) {
    float a = i * PI_F / 4;
    float c = cosf(a), s = sinf(a);
    L(12 + c * 7.0f, 12 + s * 7.0f, 12 + c * 9.0f, 12 + s * 9.0f);
  }
  RING(12, 12, 7);
}

/* ---- icon table --------------------------------------------------------- */

void icon_draw(FmIcon ic, FmRect r, FmColor c) {
  float s = FM_MIN(r.w, r.h);
  R = rect_center(r, s, s);
  C = c;
  S = s / 24.0f;
  SW = FM_MAX(2.0f * S * 0.9f, 1.2f);
  switch (ic) {
    case IC_NONE: case IC_COUNT: break;

    case IC_FOLDER: folder_outline(); break;
    case IC_FOLDER_OPEN:
      POLY(false, 3, 18, 3, 6, 9, 6, 11, 8, 19, 8, 19, 10);
      POLY(true, 3, 19, 6, 11, 22, 11, 19, 19);
      break;
    case IC_FILE: doc_outline(); break;
    case IC_IMAGE:
      RR(3, 4, 18, 16, 3);
      DOT(8.5f, 9, 1.8f);
      POLY(false, 4, 18, 10, 12, 14, 16, 16, 14, 20, 18);
      break;
    case IC_AUDIO: case IC_MUSIC:
      L(10, 17, 10, 5);
      L(10, 5, 18, 3);
      L(18, 3, 18, 15);
      RING(7.5f, 17, 2.5f);
      RING(15.5f, 15, 2.5f);
      break;
    case IC_VIDEO:
      RR(2, 6, 14, 12, 3);
      POLY(true, 16, 10, 22, 7, 22, 17, 16, 14);
      break;
    case IC_ARCHIVE:
      RR(4, 3, 16, 18, 3);
      L(12, 3, 12, 5); L(12, 7, 12, 9); L(12, 11, 12, 12);
      RR(10, 12, 4, 5, 1);
      break;
    case IC_TEXT:
      doc_outline();
      L(9, 12, 15, 12); L(9, 15, 15, 15); L(9, 18, 13, 18);
      break;
    case IC_CODE:
      POLY(false, 8, 7, 3, 12, 8, 17);
      POLY(false, 16, 7, 21, 12, 16, 17);
      L(13.5f, 5, 10.5f, 19);
      break;
    case IC_PDF:
      doc_outline();
      POLY(false, 9, 18, 9, 12, 11, 12, 12, 13, 11, 15, 9, 15);
      break;
    case IC_DOC:
      doc_outline();
      L(9, 11, 16, 11); L(9, 14, 16, 14); L(9, 17, 16, 17);
      break;
    case IC_SHEET:
      RR(3, 3, 18, 18, 3);
      L(3, 9, 21, 9); L(3, 15, 21, 15); L(9, 3, 9, 21);
      break;
    case IC_SLIDE:
      RR(3, 4, 18, 13, 2);
      L(12, 17, 12, 21); L(8, 21, 16, 21);
      break;
    case IC_APK:
      RR(5, 9, 14, 11, 3);
      ARC(12, 10, 6, 180, 360);
      L(7, 4, 8.5f, 6.5f); L(17, 4, 15.5f, 6.5f);
      DOT(9.5f, 8, 0.9f); DOT(14.5f, 8, 0.9f);
      break;
    case IC_EXE:
      RR(3, 4, 18, 16, 3);
      L(3, 8, 21, 8);
      POLY(false, 7, 12, 10, 14, 7, 16);
      L(12, 16, 16, 16);
      break;
    case IC_FONT:
      POLY(false, 5, 20, 11, 4, 13, 4, 19, 20);
      L(7.5f, 14, 16.5f, 14);
      break;
    case IC_DISK: case IC_DISC:
      RING(12, 12, 9); RING(12, 12, 2.5f);
      break;

    case IC_BACK: case IC_ARROW_LEFT:
      L(20, 12, 4, 12); arrow_head(4, 12, -1, 0) ; break;
    case IC_FORWARD: case IC_ARROW_RIGHT:
      L(4, 12, 20, 12); arrow_head(20, 12, 1, 0); break;
    case IC_UP: case IC_ARROW_UP:
      L(12, 20, 12, 4); arrow_head(12, 4, 0, -1); break;
    case IC_ARROW_DOWN:
      L(12, 4, 12, 20); arrow_head(12, 20, 0, 1); break;
    case IC_HOME:
      POLY(false, 3, 11, 12, 3, 21, 11);
      POLY(false, 5, 9.5f, 5, 20, 19, 20, 19, 9.5f);
      RR(10, 14, 4, 6, 1);
      break;
    case IC_MENU:
      L(4, 6, 20, 6); L(4, 12, 20, 12); L(4, 18, 20, 18);
      break;
    case IC_MORE:
      DOT(12, 5, 2); DOT(12, 12, 2); DOT(12, 19, 2);
      break;
    case IC_MORE_H:
      DOT(5, 12, 2); DOT(12, 12, 2); DOT(19, 12, 2);
      break;
    case IC_CLOSE:
      L(6, 6, 18, 18); L(18, 6, 6, 18);
      break;
    case IC_CHECK:
      POLY(false, 4, 12.5f, 9.5f, 18, 20, 6.5f);
      break;
    case IC_PLUS: L(12, 5, 12, 19); L(5, 12, 19, 12); break;
    case IC_MINUS: L(5, 12, 19, 12); break;
    case IC_CHEVRON_RIGHT: POLY(false, 9, 5, 16, 12, 9, 19); break;
    case IC_CHEVRON_LEFT: POLY(false, 15, 5, 8, 12, 15, 19); break;
    case IC_CHEVRON_DOWN: POLY(false, 5, 9, 12, 16, 19, 9); break;
    case IC_CHEVRON_UP: POLY(false, 5, 15, 12, 8, 19, 15); break;
    case IC_SWAP:
      L(4, 8, 19, 8); arrow_head(20, 8, 1, 0);
      L(20, 16, 5, 16); arrow_head(4, 16, -1, 0);
      break;

    case IC_COPY:
      RR(8, 8, 13, 13, 3);
      POLY(false, 16, 4.5f, 16, 4, 5.5f, 3, 3, 5.5f, 3, 16);
      POLY(false, 3, 13, 3, 6, 6, 3, 13, 3, 16, 3);
      break;
    case IC_MOVE:
      POLY(false, 14, 4, 4, 4, 4, 20, 14, 20);
      L(9, 12, 21, 12); arrow_head(21, 12, 1, 0);
      break;
    case IC_CUT:
      RING(6.5f, 17.5f, 3); RING(17.5f, 17.5f, 3);
      L(8.5f, 15, 18, 3); L(15.5f, 15, 6, 3);
      break;
    case IC_PASTE:
      RR(5, 4, 14, 17, 3);
      FILL(9, 2.5f, 6, 3.5f, 1.2f);
      L(9, 11, 15, 11); L(9, 15, 15, 15);
      break;
    case IC_DELETE:
      L(4, 6, 20, 6);
      POLY(false, 9, 6, 9.5f, 3.5f, 14.5f, 3.5f, 15, 6);
      POLY(false, 6, 6, 7, 20, 17, 20, 18, 6);
      L(10, 10, 10, 16.5f); L(14, 10, 14, 16.5f);
      break;
    case IC_RENAME:
      POLY(true, 4, 20, 4.5f, 16, 15.5f, 5, 19, 8.5f, 8, 19.5f);
      L(13, 7.5f, 16.5f, 11);
      break;
    case IC_NEW_FOLDER:
      folder_outline();
      L(12, 11, 12, 17); L(9, 14, 15, 14);
      break;
    case IC_NEW_FILE:
      doc_outline();
      L(12.5f, 11, 12.5f, 18); L(9, 14.5f, 16, 14.5f);
      break;
    case IC_SEARCH:
      RING(10.5f, 10.5f, 6.5f);
      L(15.5f, 15.5f, 20.5f, 20.5f);
      break;
    case IC_SETTINGS: gear(); break;
    case IC_STAR: star(false); break;
    case IC_STAR_FILL: star(true); break;
    case IC_REFRESH:
      ARC(12, 12, 7.5f, -60, 240);
      POLY(false, 18.5f, 3.5f, 18.5f, 8, 14, 8);
      break;
    case IC_SORT:
      L(7, 5, 7, 19); POLY(false, 3.5f, 15.5f, 7, 19, 10.5f, 15.5f);
      L(17, 19, 17, 5); POLY(false, 13.5f, 8.5f, 17, 5, 20.5f, 8.5f);
      break;
    case IC_GRID:
      RR(4, 4, 7, 7, 2); RR(13, 4, 7, 7, 2); RR(4, 13, 7, 7, 2); RR(13, 13, 7, 7, 2);
      break;
    case IC_LIST:
      DOT(5, 6, 1.4f); DOT(5, 12, 1.4f); DOT(5, 18, 1.4f);
      L(9, 6, 20, 6); L(9, 12, 20, 12); L(9, 18, 20, 18);
      break;
    case IC_EYE:
      ARC(12, 19, 11, 215, 325);
      ARC(12, 5, 11, 35, 145);
      RING(12, 12, 3);
      break;
    case IC_EYE_OFF:
      ARC(12, 19, 11, 215, 325);
      ARC(12, 5, 11, 35, 145);
      RING(12, 12, 3);
      L(4, 4, 20, 20);
      break;
    case IC_LOCK:
      RR(5, 10, 14, 11, 3);
      ARC(12, 9, 4, 180, 360); L(8, 9, 8, 10); L(16, 9, 16, 10);
      DOT(12, 15.5f, 1.5f);
      break;
    case IC_UNLOCK:
      RR(5, 10, 14, 11, 3);
      ARC(12, 7, 4, 180, 360); L(8, 7, 8, 10);
      DOT(12, 15.5f, 1.5f);
      break;
    case IC_EXTRACT:
      RR(4, 10, 16, 11, 3);
      L(12, 3, 12, 14); POLY(false, 8.5f, 10.5f, 12, 14, 15.5f, 10.5f);
      break;
    case IC_COMPRESS:
      RR(4, 10, 16, 11, 3);
      L(12, 14, 12, 3); POLY(false, 8.5f, 6.5f, 12, 3, 15.5f, 6.5f);
      break;
    case IC_SHARE:
      RING(18, 5, 2.5f); RING(6, 12, 2.5f); RING(18, 19, 2.5f);
      L(8.2f, 10.8f, 15.8f, 6.2f); L(8.2f, 13.2f, 15.8f, 17.8f);
      break;
    case IC_INFO:
      RING(12, 12, 9); L(12, 11, 12, 17); DOT(12, 7.5f, 1.3f);
      break;
    case IC_WARN:
      POLY(true, 12, 3.5f, 21, 19.5f, 3, 19.5f);
      L(12, 9.5f, 12, 14); DOT(12, 17, 1.2f);
      break;
    case IC_SELECT_ALL:
      RR(3, 3, 18, 18, 3);
      POLY(false, 7.5f, 12, 10.5f, 15, 16.5f, 9);
      break;
    case IC_OPEN_WITH:
      POLY(false, 13, 4, 20, 4, 20, 11);
      L(20, 4, 11, 13);
      POLY(false, 17, 14, 17, 20, 4, 20, 4, 7, 10, 7);
      break;
    case IC_HISTORY:
      ARC(12, 12, 8.5f, 150, 480);
      POLY(false, 12, 8, 12, 12, 15, 14);
      POLY(false, 2.5f, 10, 4.6f, 16.2f, 9, 12.5f);
      break;
    case IC_FILTER:
      POLY(true, 3, 5, 21, 5, 14, 13, 14, 19, 10, 21, 10, 13);
      break;
    case IC_PANELS:
      RR(3, 4, 18, 16, 3); L(12, 4, 12, 20);
      break;
    case IC_HEX:
      POLY(true, 12, 3, 20, 7.5f, 20, 16.5f, 12, 21, 4, 16.5f, 4, 7.5f);
      L(9.5f, 9, 9.5f, 15); L(14.5f, 9, 14.5f, 15); L(9.5f, 12, 14.5f, 12);
      break;

    case IC_PLAY: SHAPE(7, 4.5f, 19.5f, 12, 7, 19.5f); break;
    case IC_PAUSE: FILL(6, 4.5f, 4, 15, 1.2f); FILL(14, 4.5f, 4, 15, 1.2f); break;
    case IC_STOP: FILL(5.5f, 5.5f, 13, 13, 2.5f); break;
    case IC_NEXT: SHAPE(5, 5, 15, 12, 5, 19); FILL(16, 5, 3, 14, 1); break;
    case IC_PREV: SHAPE(19, 5, 9, 12, 19, 19); FILL(5, 5, 3, 14, 1); break;
    case IC_VOLUME:
      SHAPE(3, 9, 7, 9, 12, 4.5f, 12, 19.5f, 7, 15, 3, 15);
      ARC(13, 12, 4, -50, 50); ARC(13, 12, 8, -50, 50);
      break;
    case IC_MUTE:
      SHAPE(3, 9, 7, 9, 12, 4.5f, 12, 19.5f, 7, 15, 3, 15);
      L(15.5f, 9, 21, 15); L(21, 9, 15.5f, 15);
      break;
    case IC_REPEAT: case IC_REPEAT_ONE:
      POLY(false, 4, 11, 4, 7, 19, 7); POLY(false, 16, 4, 19, 7, 16, 10);
      POLY(false, 20, 13, 20, 17, 5, 17); POLY(false, 8, 14, 5, 17, 8, 20);
      if (ic == IC_REPEAT_ONE) DOT(12, 12, 1.6f);
      break;
    case IC_SHUFFLE:
      POLY(false, 3, 7, 8, 7, 16, 17, 20, 17); POLY(false, 17.5f, 14, 20.5f, 17, 17.5f, 20);
      POLY(false, 3, 17, 8, 17, 10.5f, 14); POLY(false, 13.5f, 10, 16, 7, 20, 7);
      POLY(false, 17.5f, 4, 20.5f, 7, 17.5f, 10);
      break;
    case IC_ZOOM_IN:
      RING(10.5f, 10.5f, 6.5f); L(15.5f, 15.5f, 20.5f, 20.5f);
      L(10.5f, 7.5f, 10.5f, 13.5f); L(7.5f, 10.5f, 13.5f, 10.5f);
      break;
    case IC_ZOOM_OUT:
      RING(10.5f, 10.5f, 6.5f); L(15.5f, 15.5f, 20.5f, 20.5f);
      L(7.5f, 10.5f, 13.5f, 10.5f);
      break;
    case IC_ROTATE:
      ARC(12, 13, 7.5f, -90, 200);
      POLY(false, 9, 2.5f, 12, 5.5f, 9, 8.5f);
      break;
    case IC_FULLSCREEN:
      POLY(false, 4, 9, 4, 4, 9, 4); POLY(false, 15, 4, 20, 4, 20, 9);
      POLY(false, 20, 15, 20, 20, 15, 20); POLY(false, 9, 20, 4, 20, 4, 15);
      break;
    case IC_FIT:
      RR(3, 5, 18, 14, 3); RR(7.5f, 9, 9, 6, 1.5f);
      break;

    case IC_DRIVE:
      RR(3, 13, 18, 7, 2.5f);
      POLY(false, 4, 13.5f, 7, 5, 17, 5, 20, 13.5f);
      DOT(17, 16.5f, 1.2f);
      break;
    case IC_SDCARD:
      POLY(true, 9, 3, 18, 3, 18, 21, 6, 21, 6, 6);
      L(10, 6.5f, 10, 9); L(13, 6.5f, 13, 9); L(16, 6.5f, 16, 9) ;
      break;
    case IC_USB:
      L(12, 3, 12, 19); DOT(12, 19.5f, 2.2f);
      POLY(false, 12, 15, 7, 12, 7, 9); RR(5.5f, 6.5f, 3, 3, 0.5f);
      POLY(false, 12, 13, 17, 10, 17, 8); DOT(17, 7, 1.6f);
      POLY(false, 9.5f, 6, 12, 3, 14.5f, 6);
      break;
    case IC_PHONE:
      RR(6, 2.5f, 12, 19, 3); L(10.5f, 18, 13.5f, 18);
      break;
    case IC_NETWORK:
      RING(12, 12, 9);
      L(3, 12, 21, 12);
      /* meridian as two arcs */
      gfx_arc(X(12) - 4.5f * S, Y(12), 9 * S, SW, -1.05f, 1.05f, C);
      gfx_arc(X(12) + 4.5f * S, Y(12), 9 * S, SW, PI_F - 1.05f, PI_F + 1.05f, C);
      break;
    case IC_DESKTOP:
      RR(3, 4, 18, 12, 2.5f); L(12, 16, 12, 20); L(8, 20, 16, 20);
      break;
    case IC_DOWNLOAD:
      L(12, 3, 12, 15); POLY(false, 7.5f, 10.5f, 12, 15, 16.5f, 10.5f);
      POLY(false, 4, 16, 4, 20, 20, 20, 20, 16);
      break;
    case IC_PICTURES:
      RR(3, 4, 18, 16, 3); DOT(8.5f, 9, 1.8f);
      POLY(false, 4, 18, 10, 12, 14, 16, 16, 14, 20, 18);
      break;
    case IC_DOCUMENTS:
      doc_outline(); L(9, 13, 15, 13); L(9, 17, 15, 17);
      break;

    case IC_MOON:
      ARC(12, 12, 8.5f, 70, 330);
      ARC(16.5f, 8.5f, 6.5f, 95, 225);
      break;
    case IC_SUN:
      RING(12, 12, 4);
      for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4;
        L(12 + cosf(a) * 7, 12 + sinf(a) * 7, 12 + cosf(a) * 9.5f, 12 + sinf(a) * 9.5f);
      }
      break;
    case IC_KEY:
      RING(8, 12, 4); L(12, 12, 21, 12); L(18, 12, 18, 15.5f); L(15, 12, 15, 14.5f);
      break;
  }
}

/* ---- coloured file tiles ------------------------------------------------ */

FmColor icon_type_color(int t) {
  switch (t) {
    case FT_DIR: case FT_UP: return T.types[TC_FOLDER];
    case FT_IMAGE: case FT_GIF: case FT_SVG: return T.types[TC_IMAGE];
    case FT_AUDIO: return T.types[TC_AUDIO];
    case FT_VIDEO: return T.types[TC_VIDEO];
    case FT_ARCHIVE: return T.types[TC_ARCHIVE];
    case FT_TEXT: return T.types[TC_TEXT];
    case FT_CODE: return T.types[TC_CODE];
    case FT_PDF: return T.types[TC_PDF];
    case FT_DOC: return T.types[TC_DOC];
    case FT_SHEET: return FM_HEX(0x1E9E57);
    case FT_SLIDE: return FM_HEX(0xF07B26);
    case FT_APK: return FM_HEX(0x7CB342);
    case FT_EXE: return FM_HEX(0x5C6BC0);
    case FT_FONT: return FM_HEX(0x9C6ADE);
    case FT_DISK: return FM_HEX(0x78909C);
    default: return FM_HEX(0x90A0B5);
  }
}

FmIcon icon_for_type(int t) {
  switch (t) {
    case FT_DIR: return IC_FOLDER;
    case FT_UP: return IC_UP;
    case FT_IMAGE: case FT_GIF: case FT_SVG: return IC_IMAGE;
    case FT_AUDIO: return IC_AUDIO;
    case FT_VIDEO: return IC_VIDEO;
    case FT_ARCHIVE: return IC_ARCHIVE;
    case FT_TEXT: return IC_TEXT;
    case FT_CODE: return IC_CODE;
    case FT_PDF: return IC_PDF;
    case FT_DOC: return IC_DOC;
    case FT_SHEET: return IC_SHEET;
    case FT_SLIDE: return IC_SLIDE;
    case FT_APK: return IC_APK;
    case FT_EXE: return IC_EXE;
    case FT_FONT: return IC_FONT;
    case FT_DISK: return IC_DISK;
    default: return IC_FILE;
  }
}

void icon_file(int type, FmRect r, bool dark) {
  float s = FM_MIN(r.w, r.h);
  FmRect b = rect_center(r, s, s);
  FmColor col = icon_type_color(type);
  if (type == FT_UP) {
    gfx_rrect(rect_inset(b, s * 0.08f), s * 0.22f, col_alpha(col, dark ? 0.18f : 0.16f));
    icon_draw(IC_ARROW_UP, rect_inset(b, s * 0.26f), col);
    return;
  }
  if (type == FT_DIR) {
    /* back flap with tab, then the front, slightly lighter */
    FmColor back = col_mix(col, FM_HEX(0x000000), 0.18f);
    float x = b.x + s * 0.08f, w = s * 0.84f;
    gfx_rrect(FM_RECT(x, b.y + s * 0.17f, w * 0.42f, s * 0.2f), s * 0.07f, back);
    gfx_rrect(FM_RECT(x, b.y + s * 0.24f, w, s * 0.6f), s * 0.09f, back);
    gfx_rrect_vgrad(FM_RECT(x, b.y + s * 0.33f, w, s * 0.51f), s * 0.09f,
                    col_mix(col, FM_HEX(0xFFFFFF), 0.12f), col);
    return;
  }
  /* document with folded corner */
  float x = b.x + s * 0.16f, y = b.y + s * 0.06f, w = s * 0.68f, h = s * 0.88f;
  float fold = s * 0.22f;
  FmColor paper = dark ? col_mix(col, FM_HEX(0x1A1D24), 0.25f) : col;
  float xy[] = {
    x + s * 0.06f, y, x + w - fold, y, x + w, y + fold, x + w, y + h - s * 0.06f,
    x + w - s * 0.06f, y + h, x + s * 0.06f, y + h, x, y + h - s * 0.06f, x, y + s * 0.06f,
  };
  gfx_convex(xy, 8, paper);
  gfx_tri(x + w - fold, y, x + w - fold, y + fold, x + w, y + fold,
          col_mix(paper, FM_HEX(0xFFFFFF), 0.45f));
  /* glyph */
  FmIcon g = icon_for_type(type);
  if (g == IC_FILE) g = IC_NONE;
  if (g != IC_NONE) {
    float gs = s * 0.36f;
    icon_draw(g, FM_RECT(x + (w - gs) * 0.5f, y + h * 0.42f, gs, gs), FM_HEX(0xFFFFFF));
  }
}
