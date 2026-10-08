/* xminiz.c -- compiles vendor/miniz (raw deflate/inflate for fstream.c).
**
** Design decisions:
**   - Only tdefl/tinfl are used: the zip, stdio and time parts are left
**     out, and the zlib-compatible names too, so nothing clashes with a
**     system zlib that SDL or a platform library might pull in.
**   - Warnings in vendored code are not ours to fix; they are silenced
**     here rather than by editing miniz.
*/
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#if defined(__clang__)
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#include "miniz.c"
