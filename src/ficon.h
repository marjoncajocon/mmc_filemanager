/* ficon.h -- vector icons drawn with fgfx (no icon font, no bitmaps).
**
** Icons are designed on a 24x24 grid with 2-unit rounded strokes, in the
** spirit of Material Symbols "rounded". They scale to any size crisply.
*/
#ifndef FICON_H
#define FICON_H

#include "fgfx.h"

typedef enum FmIcon {
  IC_NONE = 0,
  /* file types (outline glyphs; see icon_file for the coloured tiles) */
  IC_FOLDER, IC_FOLDER_OPEN, IC_FILE, IC_IMAGE, IC_AUDIO, IC_VIDEO, IC_ARCHIVE, IC_TEXT,
  IC_CODE, IC_PDF, IC_DOC, IC_SHEET, IC_SLIDE, IC_APK, IC_EXE, IC_FONT, IC_DISK,
  /* navigation */
  IC_BACK, IC_FORWARD, IC_UP, IC_HOME, IC_MENU, IC_MORE, IC_MORE_H, IC_CLOSE, IC_CHECK,
  IC_PLUS, IC_MINUS, IC_CHEVRON_RIGHT, IC_CHEVRON_LEFT, IC_CHEVRON_DOWN, IC_CHEVRON_UP,
  IC_ARROW_RIGHT, IC_ARROW_LEFT, IC_ARROW_DOWN, IC_ARROW_UP, IC_SWAP,
  /* actions */
  IC_COPY, IC_MOVE, IC_CUT, IC_PASTE, IC_DELETE, IC_RENAME, IC_NEW_FOLDER, IC_NEW_FILE,
  IC_SEARCH, IC_SETTINGS, IC_STAR, IC_STAR_FILL, IC_REFRESH, IC_SORT, IC_GRID, IC_LIST,
  IC_EYE, IC_EYE_OFF, IC_LOCK, IC_UNLOCK, IC_EXTRACT, IC_COMPRESS, IC_SHARE, IC_INFO,
  IC_WARN, IC_SELECT_ALL, IC_OPEN_WITH, IC_HISTORY, IC_FILTER, IC_PANELS, IC_HEX,
  /* media */
  IC_PLAY, IC_PAUSE, IC_STOP, IC_NEXT, IC_PREV, IC_VOLUME, IC_MUTE, IC_REPEAT, IC_REPEAT_ONE,
  IC_SHUFFLE, IC_ZOOM_IN, IC_ZOOM_OUT, IC_ROTATE, IC_FULLSCREEN, IC_FIT, IC_MUSIC,
  /* places */
  IC_DRIVE, IC_SDCARD, IC_USB, IC_PHONE, IC_NETWORK, IC_DESKTOP, IC_DOWNLOAD, IC_PICTURES,
  IC_DOCUMENTS, IC_DISC,
  /* misc */
  IC_MOON, IC_SUN, IC_KEY, IC_EQUALIZER,
  /* media library (flib) */
  IC_HEART, IC_HEART_FILL, IC_LIBRARY, IC_PERSON,
  IC_COUNT
} FmIcon;

/* Stroked icon in r (square looks best), colour c. */
void icon_draw(FmIcon ic, FmRect r, FmColor c);

/* File-type tile: a coloured folder or a document with folded corner and a
** glyph, like modern file managers. type is an FmType. */
void icon_file(int type, FmRect r, bool dark);

/* The accent colour used for a file type (labels, thumbnails borders). */
FmColor icon_type_color(int type);

/* Icon glyph that represents a file type (for menus). */
FmIcon icon_for_type(int type);

#endif
