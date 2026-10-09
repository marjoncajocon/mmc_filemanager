/* fmedia.h -- the system's media controls for what the app plays.
**
** Android: a notification with the title, cover art and Previous /
** Play-Pause / Next / Stop, the lock screen and headset buttons (a
** MediaSession), and a foreground service so music and a background video
** keep playing with the screen off (FmMedia.java). Elsewhere media_pump only
** keeps the background work going.
*/
#ifndef FMEDIA_H
#define FMEDIA_H

#include "fcore.h"

/* Every turn of the main loop, also while the app is hidden: carries out
** what was pressed in the notification and tells it what plays now. */
void media_pump(void);
/* While nothing is drawn (Android in the background, desktop hidden to the
** tray): the work app_frame would do for music and a background video. */
void media_bg_pump(void);

#endif
