/* ftray.h -- the notification area ("system tray") icon, Windows.
**
** The icon shows while conf.tray is on. Clicking it shows the window again
** (or hides it); its menu offers show/hide, play/pause and next for the
** music player, and quit. With conf.tray_min the minimize button hides the
** window to the tray (no taskbar button); with conf.tray_close the close
** button does too. Music, a background video and downloads keep going.
**
** Elsewhere every call is a no-op and tray_available() is false (Linux and
** BSD would need a D-Bus StatusNotifier, macOS a menu bar item).
*/
#ifndef FTRAY_H
#define FTRAY_H

#include "fcore.h"

bool tray_available(void);
/* After the window exists, and again whenever conf.tray changes. */
void tray_apply(void);
void tray_shutdown(void);
/* The app's logo as 32-bit pixels (also the window's own icon). */
void tray_set_window_icon(void);

bool tray_window_hidden(void);
void tray_hide_window(void);           /* to the tray: no taskbar button */
void tray_show_window(void);

/* Every frame: carries out what was picked on the icon (main thread). */
void tray_pump(void);
/* The window's close / minimize buttons: true = it went to the tray instead
** (conf.tray_close / conf.tray_min), the caller does nothing else. */
bool tray_take_close(void);
bool tray_take_minimize(void);

#endif
