/* xmpeg.c -- compiles pl_mpeg (vendor/pl_mpeg.h) for fdec_vid.c.
**
** Design decisions:
**   - stdio stays enabled: fdec_vid.c opens the FILE with fm_fopen (UTF-8
**     paths) and passes it to plm_create_with_file, which streams it through
**     pl_mpeg's 128 KB buffer.
*/
#include "fcore.h"

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#endif

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
