/* xvorbis.c -- compiles stb_vorbis (vendor/stb_vorbis.c) for fdec_aud.c.
**
** Design decisions:
**   - A file of its own: stb_vorbis defines short macros (L, R, C ...) that
**     would collide with the dr_* libraries in xaudio.c.
**   - The pull API with stdio stays: fdec_aud.c opens the FILE with fm_fopen
**     and hands it over with stb_vorbis_open_file, so it streams.
**   - The push API is not needed and is left out.
*/
#include "fcore.h"

#define STB_VORBIS_NO_PUSHDATA_API

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#include "stb_vorbis.c"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
