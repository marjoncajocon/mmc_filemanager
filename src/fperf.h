/* fperf.h -- the rendering and resource smoke test: `mmcfm --perf`.
**
**   mmcfm --perf [--size WxH] [--frames N] [--out report.txt]
**               [--perf-shots DIR] [--perf-only a,b] [--theme N] [--light]
**   mmcfm --perf --perf-probe     driver cost of textures and geometry
**
** Opens the real window and walks a scripted tour of the main screens
** (panels with 2,000 generated files in list and grid view, with and
** without thumbnails, scrolling, settings with the theme picker, a context
** menu, the music player with its visualizer, the mini player, the video
** player, the media library and the online videos view before a search).
** Each screen is drawn N times (default 90) with forced redraws and
** measured: frame CPU time, draw calls, vertices, textures, the glyph
** atlas, our heap and the process memory. Then it stops forcing redraws
** and counts what an idle window costs over 5 s (frames, wakeups, CPU and
** the events that woke it). --perf-shots saves every screen as a BMP plus
** a contact sheet. SDL_RENDER_DRIVER picks the renderer to compare.
**
** Design decisions:
**   - The tour drives the app from outside: public calls (app_open,
**     lib_demo ...) and synthesized SDL input (Ctrl+, for settings, a right
**     click for the context menu, wheel events for scrolling), so the
**     screens run exactly the code a user runs and fapp.c needs no hooks.
**   - The test folder and media (BMP photos, a WAV tone, an intra-only
**     MPEG-1 clip written here) are generated once under the cache folder
**     and reused, so the numbers do not depend on the user's files.
**   - The app starts with the --shot defaults: nothing is saved.
*/
#ifndef FPERF_H
#define FPERF_H

#include "fcore.h"

/* True when the command line asks for the tour. */
bool perf_wanted(int argc, char **argv);
/* Records a startup milestone (only while the tour runs). */
void perf_mark(const char *what);
/* Before app_init: makes the test folder and swaps *argc / *argv for the
** app's arguments (screenshot defaults, both panels on that folder). */
bool perf_prepare(int *argc, char ***argv);
/* Runs the tour; frame_fn is one turn of the main loop. Returns the exit code. */
int perf_run(void (*frame_fn)(void));

#endif
