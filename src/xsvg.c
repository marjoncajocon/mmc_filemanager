/* xsvg.c -- compiles nanosvg + nanosvgrast (vendor/) for fdec_img.c.
**
** Design decisions:
**   - fdec_img.c reads the file itself (fm_fopen, size cap) and calls
**     nsvgParse on the buffer, so nanosvg's own fopen path is never used.
*/
#include "fcore.h"

#if defined(__TINYC__)
/* 32-bit msvcrt.dll has neither strtoll nor acosf. */
__int64 __cdecl _strtoi64(const char *s, char **end, int base);
#  define strtoll _strtoi64
#  define acosf(x) ((float)acos((double)(x)))
#endif

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
