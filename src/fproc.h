/* fproc.h -- run a helper program and read its output line by line.
**
** Used for yt-dlp. No shell is involved: arguments are passed as a list and
** quoted correctly for CreateProcessW on Windows (no console window) and
** execvp on POSIX. Unsupported on Android and the web.
*/
#ifndef FPROC_H
#define FPROC_H

#include "fcore.h"

/* line(user, text, is_stderr): return false to stop (the process is killed) */
typedef bool (*FmProcLine)(void *user, const char *line, bool is_stderr);

bool  proc_available(void);

/* Runs argv[0] with argv[1..] (NULL-terminated) and waits. Every output line
** goes to `cb` (without the newline). *exit_code gets the status (-1 when it
** did not start or was killed). */
FmErr proc_run(const char *const *argv, FmProcLine cb, void *user, int *exit_code, volatile int *cancel);

/* Convenience: stdout into one NUL-terminated buffer (fm_free), capped. */
FmErr proc_capture(const char *const *argv, size_t max_bytes, char **out, size_t *len, int *exit_code,
                   volatile int *cancel);

#endif
