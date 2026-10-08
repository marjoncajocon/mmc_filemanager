/* fviz.h -- music visualizer: spectrum analysis, styles, presets and the
** settings panel used by the music player (fview_aud.c).
**
** The player hands over the last VIZ_FFT samples it played each frame
** (only while it is visible); viz_update turns them into smoothed band
** levels, peaks, a waveform and a bass "beat", and viz_draw paints one of
** the styles into any rectangle. The knobs live in conf.viz (fconf.h).
*/
#ifndef FVIZ_H
#define FVIZ_H

#include "fui.h"
#include "fconf.h"

#define VIZ_FFT 1024          /* samples per analysis (21 ms at 48 kHz) */
#define VIZ_MIN_BANDS 16
#define VIZ_MAX_BANDS 96
#define VIZ_WAVE 256          /* points of the oscilloscope line */

enum {
  VIZ_OFF = 0, VIZ_BARS, VIZ_MIRROR, VIZ_PILLS, VIZ_WAVEFILL, VIZ_SCOPE, VIZ_RADIAL, VIZ_PULSE,
  VIZ_DOTS, VIZ_NEON, VIZ_STYLES
};
enum { VIZ_COL_ACCENT = 0, VIZ_COL_GRADIENT, VIZ_COL_RAINBOW, VIZ_COL_HEAT, VIZ_COLS };

const char *viz_style_name(int style);
/* The preset: sensible knob values for a style. */
void viz_preset(FmVizConf *v, int style);
/* Clamps every knob into range (after loading settings). */
void viz_sanitize(FmVizConf *v);
/* Styles drawn around the cover art instead of in a strip. */
bool viz_around_cover(int style);
/* Next style for "tap the visualizer": presets, then the saved custom setup. */
void viz_cycle(FmVizConf *v, const FmVizConf *saved);

/* samples: the last VIZ_FFT mono samples, oldest first, or NULL when nothing
** plays (everything then falls to rest). Returns true while anything still
** moves, so the caller keeps animating only as long as needed. */
bool viz_update(const FmVizConf *v, const float *samples, int rate, float dt);
/* Forgets levels, peaks and rings (new playlist). */
void viz_reset(void);
/* Bass pulse 0..1, for the cover bounce of the Pulse style. */
float viz_beat(void);

/* Draws v->style in r. hole > 0: radius of the cover the ring styles wrap
** around (centre of r); 0 draws a self-contained version with a centre disc. */
void viz_draw(const FmVizConf *v, FmRect r, float hole);
/* The same over video or on the black viewer: neutral colours come from a
** dark palette whatever the theme, everything is faded to alpha, and
** hole < 0 leaves the centre of the ring styles empty. */
void viz_draw_media(const FmVizConf *v, FmRect r, float hole, float alpha);
/* Smooth filled curve (n points, fill down or up to y = base) with a 1 px
** feathered line; the equalizer draws its response with it. */
void viz_curve(const float *x, const float *y, int n, float base, FmColor line, FmColor fill, float thick);

/* Settings panel inside r: a Visualizer tab (presets with live previews,
** knobs) and an Equalizer tab. Edits conf.viz, conf.viz_saved and conf.eq;
** returns false when the user closed it. */
bool viz_panel(FmRect r);
/* Which tab the panel shows next: 0 visualizer, 1 equalizer (feq.h). */
void viz_panel_tab(int tab);

/* ---- internals shared with the self test ------------------------------------ */

/* Band edges in FFT bins (bands + 1 values, fractional) for a sample rate. */
void viz_band_edges(int bands, bool log_scale, int rate, float *edges);
/* One smoothing step of a level towards target over dt seconds. */
float viz_smooth(float level, float target, float attack, float decay, float dt);
/* Spectrum level 0..1 of band b after the last viz_update (b < bands). */
float viz_level(int b);

#endif
