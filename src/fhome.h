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

enum { HOME_NONE, HOME_FILES, HOME_VIDEOS, HOME_AUDIO, HOME_PHOTOS, HOME_LIBRARY, HOME_CLOUD, HOME_SETTINGS, HOME_THEME, HOME_WIKI,
       HOME_FOLDER };                         /* a favourite folder: home_picked_folder() */

bool home_is_open(void);
void home_open(void);
void home_close(void);
/* Draws the screen in `area`; returns what was picked (HOME_*). */
int  home_frame(FmRect area);
/* HOME_FOLDER: the bookmarked folder picked in the Favorites strip. */
const char *home_picked_folder(void);
/* A favourite was opened (kind 'f' file, 'd' folder, 'a' online audio key,
** 'p' "source|id" photo): it moves to the front of the strip. */
void home_fav_touch(char kind, const char *key);

#endif
