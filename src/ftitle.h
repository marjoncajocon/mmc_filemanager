/* ftitle.h -- the custom title bar: themed minimize / maximize / close.
**
** On desktops the window is borderless and the app's top bar becomes the
** title bar. An SDL hit test tells the OS which parts drag the window and
** which edges resize it, so snapping, double-click maximize and resize
** stay native. Setting conf.system_title brings the OS frame back.
*/
#ifndef FTITLE_H
#define FTITLE_H

#include "fcore.h"
#include "fgfx.h"

/* Applies conf.system_title to the window; call after conf changes. */
void title_apply(void);
/* True while the app draws its own title bar. */
bool title_custom(void);
/* Each frame: the bar's rect, then every widget on it that must not drag. */
void title_bar(FmRect r);
void title_nodrag(FmRect r);
/* Draws the window buttons, cutting their space from `in` (right on
** Windows/Linux, left on macOS). Does nothing with the system frame. */
void title_buttons(FmRect *in);
/* A 1px outline around the window when it is not maximized. */
void title_outline(void);

#endif
