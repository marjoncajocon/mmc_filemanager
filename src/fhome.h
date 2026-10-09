/* fhome.h -- the home screen: big tiles for the app's main places.
**
** Shown at start (Settings > "Start on the home screen") and from the Home
** button of the file panels. The online views open over it, so their Back
** button returns here; "File manager" closes it.
*/
#ifndef FHOME_H
#define FHOME_H

#include "fcore.h"
#include "fui.h"

enum { HOME_NONE, HOME_FILES, HOME_VIDEOS, HOME_AUDIO, HOME_PHOTOS, HOME_LIBRARY, HOME_CLOUD, HOME_SETTINGS, HOME_THEME };

bool home_is_open(void);
void home_open(void);
void home_close(void);
/* Draws the screen in `area`; returns what was picked (HOME_*). */
int  home_frame(FmRect area);

#endif
