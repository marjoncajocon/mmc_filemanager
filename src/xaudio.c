/* xaudio.c -- compiles dr_mp3, dr_flac and dr_wav (vendor/) for fdec_aud.c.
**
** Design decisions:
**   - *_NO_STDIO: fdec_aud.c feeds every decoder through read/seek/tell
**     callbacks over an fm_fopen FILE, so UTF-8 paths work everywhere.
**   - No SIMD under tcc (it has no intrinsics headers); the scalar paths are
**     plenty fast for playback.
**   - dr_mp3 keeps Layer I/II support, which is how .mp2 files play.
*/
#include "fcore.h"

#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_WAV_NO_STDIO
#if defined(__TINYC__)
#  define DR_MP3_NO_SIMD
#  define DR_FLAC_NO_SIMD
#  define DR_WAV_NO_SIMD
#  define DRFLAC_NO_CPUID
#endif

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
