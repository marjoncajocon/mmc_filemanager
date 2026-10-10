/* fdoc.h -- a small HTML document for the readers: parsed into blocks
** (headings, paragraphs, lists, code, quotes, pictures, captions, tables)
** whose text carries inline styles (bold, italic, code, links), laid out at
** a width and drawn with the app's fonts.
**
** Not a browser: no CSS, no scripts, no floats. What a page is for a reader
** survives (structure, emphasis, code, pictures, tables, links); site chrome
** (navigation boxes, edit links, reference markers, hidden elements) goes.
*/
#ifndef FDOC_H
#define FDOC_H

#include "fcore.h"
#include "fui.h"

enum { DB_P, DB_H, DB_LI, DB_DT, DB_DD, DB_PRE, DB_QUOTE, DB_IMG, DB_CAPTION, DB_HR, DB_TABLE };
enum { DS_BOLD = 1, DS_ITALIC = 2, DS_CODE = 4, DS_LINK = 8, DS_SMALL = 16 };

typedef struct DocRun { int off, len; u8 st; int link; } DocRun;   /* link: index in links, -1 = none */
typedef struct DocFrag { float x, y, w; int run, off, len; } DocFrag;

typedef struct DocBlock {
  u8 kind, level;              /* heading 1..6; list / quote depth */
  char marker[12];             /* list item: "\xE2\x80\xA2" or "3." */
  int run0, nrun;
  int img;                     /* DB_IMG: index in imgs */
  int tab;                     /* DB_TABLE: index in tabs */
  /* layout (doc_layout) */
  float y, h;                  /* top inside the document, height */
  int f0, nf;                  /* fragments */
} DocBlock;

typedef struct DocCell {
  int row, col, span;
  bool head;
  int run0, nrun;
  int img;                     /* a picture in the cell, -1 = none */
  float x, y, w, h;            /* layout, inside the table */
  int f0, nf;
} DocCell;

typedef struct DocTable { int rows, cols, cell0, ncell; bool box; } DocTable;

typedef struct DocImage {
  char *src;                   /* absolute URL of what the page shows */
  char *href;                  /* where it links (a file page, a bigger picture), may be NULL */
  char *alt;
  int w, h;                    /* as the page asks, 0 = unknown */
} DocImage;

typedef struct DocAnchor { char *id; int block; } DocAnchor;

typedef struct FmDoc {
  char *text;
  size_t tn, tcap;
  DocRun *runs;
  int nruns, cruns;
  DocBlock *b;
  int nb, cb;
  DocCell *cells;
  int ncells, ccells;
  DocTable *tabs;
  int ntabs, ctabs;
  DocImage *imgs;
  int nimgs, cimgs;
  char **links;
  int nlinks, clinks;
  DocAnchor *anch;
  int nanch, canch;
  /* layout */
  DocFrag *frag;
  int nfrag, cfrag;
  float lay_w, lay_fs, total;
  /* find in page: matches (text offsets, sorted) drawn highlighted, cur strongest */
  int *hl;
  int nhl, hl_len, hl_cur;
} FmDoc;

enum { DOC_WIKI, DOC_BOOK };

/* base: the page's own URL (relative links and pictures resolve against it).
** flavor: what to drop (DOC_WIKI: navboxes, references markers...; DOC_BOOK:
** Project Gutenberg's licence sections). Never fails on bad HTML. */
void  doc_parse(const char *html, size_t len, const char *base, int flavor, FmDoc *d);
void  doc_free(FmDoc *d);
/* The visible text of a block or a cell (for contents lists). */
void  doc_block_text(const FmDoc *d, int block, char *out, size_t cap);
int   doc_anchor(const FmDoc *d, const char *id);       /* block index, -1 */
void  doc_url_decode(const char *s, char *out, size_t cap);

/* Layout at width w with body text size fs (px); returns the height. Cheap
** when w and fs did not change. */
float doc_layout(FmDoc *d, float w, float fs);

/* What a click hit (doc_draw's result). */
typedef struct DocHit { int link; int img; } DocHit;
/* Draws the document with its top at (x, y), only what is inside clip;
** id seeds the clickable parts. thumb draws a picture (the caller's
** thumbnail cache); returns what was clicked ({-1,-1} = nothing). */
typedef void (*DocThumbFn)(FmDoc *d, DocImage *im, FmRect r);
DocHit doc_draw(FmDoc *d, float x, float y, FmRect clip, u32 id, DocThumbFn thumb);

/* Find: every case-insensitive match of q (ASCII letters fold) into d->hl;
** returns the count. doc_offset_y: where a text offset sits (laid out). */
int   doc_find(FmDoc *d, const char *q);
float doc_offset_y(const FmDoc *d, int off);
/* A picture whose size the page did not say learnt it: lay out again. */
void  doc_image_shape(FmDoc *d, DocImage *im, int w, int h);

#endif
