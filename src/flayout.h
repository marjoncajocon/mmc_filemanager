/* flayout.h -- where everything goes for the current window shape.
**
** Pure geometry: given the window, the settings and what is visible (jobs,
** audio bar, sidebar), it returns the rectangles of every region. The app
** recomputes it every frame, so resizing and rotation need no special code.
**
** Design decisions:
**   - LAYOUT_AUTO picks side-by-side panels in landscape, stacked panels in
**     portrait (X-plore style, each at least 40% of the height), and a
**     single panel with a Left | Right switcher below 360 dp width.
**   - The middle action bar doubles as the splitter: dragging its empty
**     space moves conf.split.
*/
#ifndef FLAYOUT_H
#define FLAYOUT_H

#include "fui.h"

typedef struct FmLayout {
  int mode;                /* LAYOUT_SIDE, LAYOUT_STACK or LAYOUT_SINGLE (resolved) */
  bool narrow;             /* the top bar shows the path instead of the title */
  bool wide;               /* room for the places sidebar */
  bool sidebar;            /* the sidebar is shown */
  bool action_vertical;    /* thin column between side-by-side panels */
  bool show[2];            /* panel visible (single mode shows one) */
  FmRect top, tabs, side, panel[2], action, jobs, audio, status;
  FmRect content;          /* panels + action bar, for the splitter */
} FmLayout;

int  layout_resolve(void);
void layout_compute(FmLayout *L, int single_tab, bool sidebar_open, float jobs_h, bool audio);
/* Splitter drag: new conf.split for pointer position (side or stack). */
float layout_split_at(const FmLayout *L, float mx, float my);

#endif
