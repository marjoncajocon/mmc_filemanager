/* fproc.c -- run a helper program, read its output (see fproc.h).
**
** Design decisions:
**   - Both pipes are read from one thread: PeekNamedPipe polling on Windows
**     (no overlapped I/O to set up), poll() on POSIX. Output comes in at
**     human speed, so a 15 ms idle sleep costs nothing.
**   - '\r' ends a line too: yt-dlp and similar tools redraw their progress
**     line with carriage returns.
*/
#include "fproc.h"

#if !defined(FM_ANDROID) && !defined(FM_WEB)
#define LINE_MAX_LEN (16u << 20)    /* yt-dlp -J prints one long JSON line */

typedef struct LineBuf { char *buf; size_t n, cap; } LineBuf;

static void lb_free(LineBuf *lb) { fm_free(lb->buf); lb->buf = NULL; lb->n = lb->cap = 0; }

/* Feeds bytes; calls cb per complete line. false when cb asked to stop. */
static bool feed(LineBuf *lb, const char *p, size_t n, bool is_err, FmProcLine cb, void *user) {
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '\n' || c == '\r') {
      if (lb->n && lb->buf) {
        lb->buf[lb->n] = 0;
        lb->n = 0;
        if (cb && !cb(user, lb->buf, is_err)) return false;
      }
    } else if (lb->n < LINE_MAX_LEN - 1) {
      if (lb->n + 1 >= lb->cap) {
        lb->cap = lb->cap ? lb->cap * 2 : 1024;
        lb->buf = (char *)fm_realloc(lb->buf, lb->cap);
      }
      lb->buf[lb->n++] = c;
    }
  }
  return true;
}

static bool flush_line(LineBuf *lb, bool is_err, FmProcLine cb, void *user) {
  if (!lb->n || !lb->buf) return true;
  lb->buf[lb->n] = 0;
  lb->n = 0;
  return !cb || cb(user, lb->buf, is_err);
}
#endif

/* ============================================================================ */
#if defined(FM_WIN)

#include "fwin.h"

bool proc_available(void) { return true; }

/* A job object holds the helper and everything it starts: yt-dlp.exe is a
** launcher that runs a second process, and killing only the launcher left
** that one running. Loaded at run time (tcc headers lack the declarations);
** without it we fall back to killing the main process. */
typedef struct JobBasic {
  LARGE_INTEGER user_time, job_time;
  DWORD flags;
  SIZE_T min_ws, max_ws;
  DWORD procs;
  ULONG_PTR affinity;
  DWORD prio, sched;
} JobBasic;
typedef struct JobExt { JobBasic b; ULONGLONG io[6]; SIZE_T mem[4]; } JobExt;
enum { JOB_EXTENDED_LIMITS = 9, JOB_KILL_ON_CLOSE = 0x2000 };
typedef HANDLE (WINAPI *CreateJobFn)(LPSECURITY_ATTRIBUTES, LPCWSTR);
typedef BOOL (WINAPI *SetJobFn)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI *AssignJobFn)(HANDLE, HANDLE);
typedef BOOL (WINAPI *KillJobFn)(HANDLE, UINT);

static HANDLE job_new(void) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  CreateJobFn create = k ? (CreateJobFn)(void (*)(void))GetProcAddress(k, "CreateJobObjectW") : NULL;
  SetJobFn set = k ? (SetJobFn)(void (*)(void))GetProcAddress(k, "SetInformationJobObject") : NULL;
  if (!create || !set) return NULL;
  HANDLE j = create(NULL, NULL);
  if (!j) return NULL;
  JobExt x;
  memset(&x, 0, sizeof x);
  x.b.flags = JOB_KILL_ON_CLOSE;           /* closing our handle ends the whole tree */
  if (!set(j, JOB_EXTENDED_LIMITS, &x, sizeof x)) { CloseHandle(j); return NULL; }
  return j;
}

static bool job_assign(HANDLE j, HANDLE proc) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  AssignJobFn assign = k ? (AssignJobFn)(void (*)(void))GetProcAddress(k, "AssignProcessToJobObject") : NULL;
  return j && assign && assign(j, proc);
}

static void job_kill(HANDLE j) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  KillJobFn kill = k ? (KillJobFn)(void (*)(void))GetProcAddress(k, "TerminateJobObject") : NULL;
  if (j && kill) kill(j, 1);
}

/* Appends one argument quoted for CommandLineToArgvW / the MS C runtime. */
static void quote_arg(wchar_t *cmd, size_t cap, size_t *pos, const wchar_t *a) {
  size_t o = *pos;
#define PUT(ch) do { if (o + 1 < cap) cmd[o++] = (ch); } while (0)
  if (o) PUT(L' ');
  bool need = !*a || wcspbrk(a, L" \t\"") != NULL;
  if (!need) {
    for (; *a; a++) PUT(*a);
  } else {
    PUT(L'"');
    for (const wchar_t *p = a;; p++) {
      size_t bs = 0;
      while (*p == L'\\') { p++; bs++; }
      if (!*p) { for (size_t i = 0; i < bs * 2; i++) PUT(L'\\'); break; }
      if (*p == L'"') { for (size_t i = 0; i < bs * 2 + 1; i++) PUT(L'\\'); PUT(L'"'); }
      else { for (size_t i = 0; i < bs; i++) PUT(L'\\'); PUT(*p); }
    }
    PUT(L'"');
  }
#undef PUT
  cmd[o] = 0;
  *pos = o;
}

static wchar_t *wdup(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)fm_alloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  else w[0] = 0;
  return w;
}

FmErr proc_run(const char *const *argv, FmProcLine cb, void *user, int *exit_code, volatile int *cancel) {
  if (exit_code) *exit_code = -1;
  if (!argv || !argv[0]) return FM_ERR_UNSUPPORTED;
  size_t cap = 32768, pos = 0;
  wchar_t *cmd = (wchar_t *)fm_alloc(cap * sizeof(wchar_t));
  cmd[0] = 0;
  for (int i = 0; argv[i]; i++) {
    wchar_t *w = wdup(argv[i]);
    quote_arg(cmd, cap, &pos, w);
    fm_free(w);
  }
  wchar_t *exe = wdup(argv[0]);

  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
  FmErr err = FM_ERR_IO;
  HANDLE job = NULL;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof pi);
  if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) goto done;
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out_w;
  si.hStdError = err_w;
  /* a path with a separator runs as given; a bare name goes through PATH */
  bool has_dir = wcschr(exe, L'\\') || wcschr(exe, L'/');
  job = job_new();
  /* suspended until it is in the job, so nothing it starts escapes */
  if (!CreateProcessW(has_dir ? exe : NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL,
                      &si, &pi)) {
    err = GetLastError() == ERROR_FILE_NOT_FOUND ? FM_ERR_NOT_FOUND : FM_ERR_IO;
    goto done;
  }
  if (!job_assign(job, pi.hProcess) && job) { CloseHandle(job); job = NULL; }
  ResumeThread(pi.hThread);
  CloseHandle(out_w); out_w = NULL;
  CloseHandle(err_w); err_w = NULL;

  LineBuf lo, le;
  memset(&lo, 0, sizeof lo);
  memset(&le, 0, sizeof le);
  char buf[4096];
  bool stop = false, out_open = true, err_open = true;
  while (!stop && (out_open || err_open)) {
    if (cancel && *cancel) { stop = true; err = FM_ERR_CANCEL; break; }
    bool any = false;
    HANDLE hs[2] = { out_r, err_r };
    bool *open[2] = { &out_open, &err_open };
    for (int k = 0; k < 2 && !stop; k++) {
      if (!*open[k]) continue;
      DWORD avail = 0, got = 0;
      if (!PeekNamedPipe(hs[k], NULL, 0, NULL, &avail, NULL)) { *open[k] = false; continue; }
      if (!avail) continue;
      if (!ReadFile(hs[k], buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, NULL) || !got) {
        *open[k] = false;
        continue;
      }
      any = true;
      if (!feed(k ? &le : &lo, buf, got, k == 1, cb, user)) stop = true;
    }
    if (!any && !stop) {
      if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
        /* exited: drain what is left, then end */
        for (int k = 0; k < 2; k++) {
          DWORD avail = 0, got = 0;
          while (*open[k] && PeekNamedPipe(hs[k], NULL, 0, NULL, &avail, NULL) && avail &&
                 ReadFile(hs[k], buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, NULL) && got)
            if (!feed(k ? &le : &lo, buf, got, k == 1, cb, user)) { stop = true; break; }
          *open[k] = false;
        }
      }
    }
  }
  if (stop) {
    if (job) job_kill(job);
    TerminateProcess(pi.hProcess, 1);
    if (err != FM_ERR_CANCEL) err = FM_ERR_CANCEL;
  } else {
    flush_line(&lo, false, cb, user);
    flush_line(&le, true, cb, user);
    err = FM_OK;
  }
  lb_free(&lo);
  lb_free(&le);
  WaitForSingleObject(pi.hProcess, 5000);
  DWORD code = (DWORD)-1;
  if (GetExitCodeProcess(pi.hProcess, &code) && exit_code && !stop) *exit_code = (int)code;
done:
  if (job) CloseHandle(job);                 /* kill-on-close ends any leftovers */
  if (pi.hProcess) CloseHandle(pi.hProcess);
  if (pi.hThread) CloseHandle(pi.hThread);
  if (out_r) CloseHandle(out_r);
  if (out_w) CloseHandle(out_w);
  if (err_r) CloseHandle(err_r);
  if (err_w) CloseHandle(err_w);
  fm_free(cmd);
  fm_free(exe);
  return err;
}

/* ============================================================================ */
#elif !defined(FM_ANDROID) && !defined(FM_WEB)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

bool proc_available(void) { return true; }

FmErr proc_run(const char *const *argv, FmProcLine cb, void *user, int *exit_code, volatile int *cancel) {
  if (exit_code) *exit_code = -1;
  if (!argv || !argv[0]) return FM_ERR_UNSUPPORTED;
  int po[2], pe[2], px[2];
  if (pipe(po)) return FM_ERR_IO;
  if (pipe(pe)) { close(po[0]); close(po[1]); return FM_ERR_IO; }
  if (pipe(px)) { close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); return FM_ERR_IO; }
  fcntl(px[1], F_SETFD, FD_CLOEXEC);       /* closes on a successful exec */
  pid_t pid = fork();
  if (pid < 0) {
    close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); close(px[0]); close(px[1]);
    return FM_ERR_IO;
  }
  if (pid == 0) {
    dup2(po[1], 1);
    dup2(pe[1], 2);
    close(po[0]); close(pe[0]); close(px[0]);
    execvp(argv[0], (char *const *)argv);
    int e = errno;
    if (write(px[1], &e, sizeof e) < 0) {}
    _exit(127);
  }
  close(po[1]); close(pe[1]); close(px[1]);
  int child_err = 0;
  bool exec_failed = read(px[0], &child_err, sizeof child_err) == (ssize_t)sizeof child_err;
  close(px[0]);
  FmErr err = FM_OK;
  if (exec_failed) {
    err = child_err == ENOENT ? FM_ERR_NOT_FOUND : FM_ERR_IO;
  } else {
    LineBuf lo, le;
    memset(&lo, 0, sizeof lo);
    memset(&le, 0, sizeof le);
    struct pollfd fds[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
    int open_n = 2;
    char buf[4096];
    bool stop = false;
    while (open_n > 0 && !stop) {
      if (cancel && *cancel) { stop = true; break; }
      if (poll(fds, 2, 100) <= 0) continue;
      for (int k = 0; k < 2 && !stop; k++) {
        if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t n = read(fds[k].fd, buf, sizeof buf);
        if (n <= 0) { fds[k].fd = -1; open_n--; continue; }
        if (!feed(k ? &le : &lo, buf, (size_t)n, k == 1, cb, user)) stop = true;
      }
    }
    if (stop) {
      kill(pid, SIGTERM);
      err = FM_ERR_CANCEL;
    } else {
      flush_line(&lo, false, cb, user);
      flush_line(&le, true, cb, user);
    }
    lb_free(&lo);
    lb_free(&le);
  }
  close(po[0]);
  close(pe[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (exit_code && err == FM_OK) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return err;
}

/* ============================================================================ */
#else

bool proc_available(void) { return false; }

FmErr proc_run(const char *const *argv, FmProcLine cb, void *user, int *exit_code, volatile int *cancel) {
  FM_UNUSED(argv); FM_UNUSED(cb); FM_UNUSED(user); FM_UNUSED(cancel);
  if (exit_code) *exit_code = -1;
  return FM_ERR_UNSUPPORTED;
}

#endif

/* ---- capture ------------------------------------------------------------------ */

typedef struct Cap { char *buf; size_t len, cap, max; bool over; } Cap;

static bool cap_line(void *user, const char *line, bool is_err) {
  Cap *c = (Cap *)user;
  if (is_err) return true;
  size_t n = strlen(line);
  if (c->len + n + 2 > c->max) { c->over = true; return false; }
  if (c->len + n + 2 > c->cap) {
    size_t nc = c->cap ? c->cap * 2 : 8192;
    while (nc < c->len + n + 2) nc *= 2;
    c->buf = (char *)fm_realloc(c->buf, nc);
    c->cap = nc;
  }
  memcpy(c->buf + c->len, line, n);
  c->len += n;
  c->buf[c->len++] = '\n';
  c->buf[c->len] = 0;
  return true;
}

FmErr proc_capture(const char *const *argv, size_t max_bytes, char **out, size_t *len, int *exit_code,
                   volatile int *cancel) {
  Cap c;
  memset(&c, 0, sizeof c);
  c.max = max_bytes ? max_bytes : (8u << 20);
  FmErr err = proc_run(argv, cap_line, &c, exit_code, cancel);
  if (c.over) err = FM_ERR_FULL;
  if (err != FM_OK) { fm_free(c.buf); c.buf = NULL; c.len = 0; }
  else if (!c.buf) c.buf = (char *)fm_calloc(1, 1);
  *out = c.buf;
  if (len) *len = c.len;
  return err;
}
