/* ximage.c -- compiles stb_image (vendor/stb_image.h) for fdec_img.c.
**
** Design decisions:
**   - Files are read through stbi_load_from_callbacks over fm_fopen, so
**     STBI_NO_STDIO: UTF-8 paths work on Windows and nothing else opens files.
**   - GIF is left out: fdec_img.c has its own frame-by-frame GIF decoder
**     with bounded memory.
**   - stb keeps plain malloc: pixel buffers depend on the file, and a failed
**     allocation must return NULL (fm_alloc aborts) so a huge image is an
**     error message, not a crash. fdec_img.c checks sizes before decoding.
*/
#include "fcore.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_MAX_DIMENSIONS (1 << 16)
#if defined(__TINYC__) && !defined(STBI_NO_SIMD)
#  define STBI_NO_SIMD
#endif
#if defined(NDEBUG)
#  define STBI_ASSERT(x) ((void)0)
#endif

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#include "stb_image.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
