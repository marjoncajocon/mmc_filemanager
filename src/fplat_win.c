/* fplat_win.c -- fplat.h for Windows (UTF-8 in, UTF-16 to the OS).
**
** Design decisions:
**   - Long paths: every path handed to the OS gets the \\?\ prefix when it is
**     absolute and longer than MAX_PATH, so deep trees copy fine.
**   - shell32 / advapi32 functions are looked up with GetProcAddress, so the
**     tcc build links with kernel32/user32 only (tcc ships no shell32.def).
*/
#include "fcore.h"
#ifdef FM_WIN
#include "fplat.h"

#include "fwin.h"
#include <io.h>
#include <wchar.h>

/* ---- utf-8 <-> utf-16 --------------------------------------------------- */

#define WPATH 4096

static int to_w(const char *s, wchar_t *out, int cap) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
  if (n <= 0) out[0] = 0;
  return n;
}

static int to_u8(const wchar_t *s, char *out, int cap) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
  if (n <= 0) out[0] = 0;
  return n;
}

/* Native path: '/' -> '\', and the \\?\ prefix for long absolute paths. */
static wchar_t *wpath(const char *path, wchar_t *buf) {
  wchar_t tmp[WPATH];
  to_w(path, tmp, WPATH);
  for (wchar_t *p = tmp; *p; p++) if (*p == L'/') *p = L'\\';
  size_t n = wcslen(tmp);
  if (n >= MAX_PATH - 12 && tmp[1] == L':' && tmp[2] == L'\\') {
    wcscpy(buf, L"\\\\?\\");
    wcsncpy(buf + 4, tmp, WPATH - 5);
    buf[WPATH - 1] = 0;
  } else if (n >= MAX_PATH - 12 && tmp[0] == L'\\' && tmp[1] == L'\\' && tmp[2] != L'?') {
    wcscpy(buf, L"\\\\?\\UNC\\");
    wcsncpy(buf + 8, tmp + 2, WPATH - 9);
    buf[WPATH - 1] = 0;
  } else {
    wcscpy(buf, tmp);
  }
  return buf;
}

static FmErr win_err(void) {
  switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
      return FM_ERR_NOT_FOUND;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return FM_ERR_EXISTS;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return FM_ERR_ACCESS;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return FM_ERR_FULL;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return FM_ERR_NOMEM;
    default: return FM_ERR_IO;
  }
}

/* FILETIME (100 ns since 1601) <-> unix seconds. */
static i64 ft_to_unix(FILETIME ft) {
  u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (i64)(t / 10000000ULL) - 11644473600LL;
}

static FILETIME unix_to_ft(i64 s) {
  u64 t = (u64)(s + 11644473600LL) * 10000000ULL;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)t;
  ft.dwHighDateTime = (DWORD)(t >> 32);
  return ft;
}

static u32 attr_flags(DWORD a, const wchar_t *name) {
  u32 f = 0;
  if (a & FILE_ATTRIBUTE_DIRECTORY) f |= FM_ST_DIR;
  if (a & FILE_ATTRIBUTE_HIDDEN) f |= FM_ST_HIDDEN;
  if (a & FILE_ATTRIBUTE_READONLY) f |= FM_ST_READONLY;
  if (a & FILE_ATTRIBUTE_SYSTEM) f |= FM_ST_SYSTEM;
  if (a & FILE_ATTRIBUTE_REPARSE_POINT) f |= FM_ST_LINK;
  if (name && name[0] == L'.') f |= FM_ST_HIDDEN;
  if (name && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    const wchar_t *dot = wcsrchr(name, L'.');
    if (dot && (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd") ||
                !_wcsicmp(dot, L".com")))
      f |= FM_ST_EXEC;
  }
  return f;
}

/* ---- stat --------------------------------------------------------------- */

bool plat_stat(const char *path, FmStat *st) {
  wchar_t w[WPATH];
  WIN32_FILE_ATTRIBUTE_DATA d;
  memset(st, 0, sizeof *st);
  if (!GetFileAttributesExW(wpath(path, w), GetFileExInfoStandard, &d)) {
    /* "C:" or "C:\" style roots answer through GetFileAttributesW. */
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    st->flags = attr_flags(a, NULL);
    return true;
  }
  st->size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  st->mtime = ft_to_unix(d.ftLastWriteTime);
  st->flags = attr_flags(d.dwFileAttributes, fm_path_base(path)[0] == '.' ? L"." : NULL);
  return true;
}

bool plat_exists(const char *path) {
  wchar_t w[WPATH];
  return GetFileAttributesW(wpath(path, w)) != INVALID_FILE_ATTRIBUTES;
}

bool plat_is_dir(const char *path) {
  wchar_t w[WPATH];
  DWORD a = GetFileAttributesW(wpath(path, w));
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- directories -------------------------------------------------------- */

struct FmDir {
  HANDLE h;
  WIN32_FIND_DATAW fd;
  int first;
  char name[MAX_PATH * 3 + 4];
};

FmDir *plat_dir_open(const char *path, FmErr *err) {
  char pat[FM_PATH_MAX + 4];
  wchar_t w[WPATH];
  fm_strlcpy(pat, path, sizeof pat);
  size_t n = strlen(pat);
  if (n && !fm_is_sep(pat[n - 1])) fm_strlcat(pat, "\\", sizeof pat);
  fm_strlcat(pat, "*", sizeof pat);
  FmDir *d = (FmDir *)fm_calloc(1, sizeof *d);
  d->h = FindFirstFileExW(wpath(pat, w), FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL,
                          FIND_FIRST_EX_LARGE_FETCH);
  if (d->h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (err) *err = (e == ERROR_FILE_NOT_FOUND) ? FM_OK : win_err();
    if (e == ERROR_FILE_NOT_FOUND) { d->first = -1; return d; }   /* empty volume */
    fm_free(d);
    return NULL;
  }
  d->first = 1;
  if (err) *err = FM_OK;
  return d;
}

bool plat_dir_next(FmDir *d, const char **name, FmStat *st) {
  for (;;) {
    if (d->first < 0) return false;
    if (d->first) d->first = 0;
    else if (!FindNextFileW(d->h, &d->fd)) return false;
    const wchar_t *fn = d->fd.cFileName;
    if (fn[0] == L'.' && (!fn[1] || (fn[1] == L'.' && !fn[2]))) continue;
    to_u8(fn, d->name, sizeof d->name);
    *name = d->name;
    if (st) {
      st->size = ((u64)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
      st->mtime = ft_to_unix(d->fd.ftLastWriteTime);
      st->flags = attr_flags(d->fd.dwFileAttributes, fn);
      st->mode = 0;
    }
    return true;
  }
}

void plat_dir_close(FmDir *d) {
  if (!d) return;
  if (d->h && d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  fm_free(d);
}

/* ---- changes ------------------------------------------------------------ */

FmErr plat_mkdir(const char *path) {
  wchar_t w[WPATH];
  return CreateDirectoryW(wpath(path, w), NULL) ? FM_OK : win_err();
}

FmErr plat_mkdirs(const char *path) {
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  size_t n = strlen(p);
  for (size_t i = 0; i <= n; i++) {
    if (i == n || fm_is_sep(p[i])) {
      char c = p[i];
      p[i] = 0;
      if (i > 0 && !(i == 2 && p[1] == ':') && p[0] && !plat_is_dir(p)) {
        FmErr e = plat_mkdir(p);
        if (e != FM_OK && e != FM_ERR_EXISTS) return e;
      }
      p[i] = c;
    }
  }
  return FM_OK;
}

FmErr plat_remove_file(const char *path) {
  wchar_t w[WPATH];
  wpath(path, w);
  if (DeleteFileW(w)) return FM_OK;
  if (GetLastError() == ERROR_ACCESS_DENIED) {   /* read-only file */
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
      SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
      if (DeleteFileW(w)) return FM_OK;
    }
  }
  return win_err();
}

FmErr plat_remove_dir(const char *path) {
  wchar_t w[WPATH];
  return RemoveDirectoryW(wpath(path, w)) ? FM_OK : win_err();
}

FmErr plat_rename(const char *from, const char *to) {
  wchar_t a[WPATH], b[WPATH];
  return MoveFileExW(wpath(from, a), wpath(to, b), 0) ? FM_OK : win_err();
}

typedef struct {
  HWND hwnd; UINT wFunc; const wchar_t *pFrom; const wchar_t *pTo; WORD fFlags;
  BOOL fAnyOperationsAborted; void *hNameMappings; const wchar_t *lpszProgressTitle;
} FmShFileOp;   /* SHFILEOPSTRUCTW, declared here for tcc */

FmErr plat_trash(const char *path) {
  typedef int (WINAPI *ShFileOpFn)(FmShFileOp *);
  static ShFileOpFn fn;
  if (!fn) {
    HMODULE m = LoadLibraryW(L"shell32.dll");
    if (m) fn = (ShFileOpFn)(void *)GetProcAddress(m, "SHFileOperationW");
  }
  if (!fn) return FM_ERR_UNSUPPORTED;
  wchar_t w[WPATH + 2];
  to_w(path, w, WPATH);                      /* the shell API wants no \\?\ prefix */
  for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
  w[wcslen(w) + 1] = 0;                      /* double-NUL terminated list */
  FmShFileOp op;
  memset(&op, 0, sizeof op);
  op.wFunc = 3;                              /* FO_DELETE */
  op.pFrom = w;
  op.fFlags = 0x0040 | 0x0010 | 0x0400 | 0x0004;   /* ALLOWUNDO|NOCONFIRMATION|NOERRORUI|SILENT */
  int r = fn(&op);
  if (r != 0) return FM_ERR_IO;
  return op.fAnyOperationsAborted ? FM_ERR_CANCEL : FM_OK;
}

FmErr plat_set_mtime(const char *path, i64 mtime) {
  wchar_t w[WPATH];
  HANDLE h = CreateFileW(wpath(path, w), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) return win_err();
  FILETIME ft = unix_to_ft(mtime);
  BOOL ok = SetFileTime(h, NULL, &ft, &ft);
  CloseHandle(h);
  return ok ? FM_OK : FM_ERR_IO;
}

FmErr plat_set_mode(const char *path, u32 mode) {
  FM_UNUSED(path); FM_UNUSED(mode);
  return FM_OK;
}

static void volume_root(const char *path, wchar_t *out) {
  wchar_t w[WPATH];
  to_w(path, w, WPATH);
  for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
  if (w[0] && w[1] == L':') { out[0] = (wchar_t)towupper(w[0]); out[1] = L':'; out[2] = L'\\'; out[3] = 0; return; }
  if (w[0] == L'\\' && w[1] == L'\\') {      /* \\server\share\ */
    int seps = 0, i = 2;
    for (; w[i]; i++) if (w[i] == L'\\' && ++seps == 2) break;
    wcsncpy(out, w, (size_t)i);
    out[i] = L'\\'; out[i + 1] = 0;
    return;
  }
  out[0] = 0;
}

bool plat_same_volume(const char *a, const char *b) {
  wchar_t ra[WPATH], rb[WPATH];
  volume_root(a, ra);
  volume_root(b, rb);
  return ra[0] && _wcsicmp(ra, rb) == 0;
}

/* ---- files -------------------------------------------------------------- */

FILE *fm_fopen(const char *path, const char *mode) {
  wchar_t w[WPATH], m[16];
  to_w(mode, m, 16);
  return _wfopen(wpath(path, w), m);
}

#ifdef __TINYC__
/* tcc links the system msvcrt.dll, which has _fseeki64 but no _ftelli64;
** its fpos_t is the 64-bit offset. */
int __cdecl _fseeki64(FILE *, __int64, int);
int fm_fseek64(FILE *f, i64 off, int whence) { return _fseeki64(f, off, whence); }
i64 fm_ftell64(FILE *f) {
  fpos_t p;
  if (fgetpos(f, &p) != 0) return -1;
  return (i64)p;
}
#else
int fm_fseek64(FILE *f, i64 off, int whence) { return _fseeki64(f, off, whence); }
i64 fm_ftell64(FILE *f) { return _ftelli64(f); }
#endif

i64 fm_fsize(FILE *f) {
  i64 cur = fm_ftell64(f);
  if (cur < 0 || fm_fseek64(f, 0, SEEK_END) != 0) return -1;
  i64 end = fm_ftell64(f);
  fm_fseek64(f, cur, SEEK_SET);
  return end;
}

void *plat_mmap(const char *path, size_t *size) {
  wchar_t w[WPATH];
  HANDLE f = CreateFileW(wpath(path, w), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0 || (u64)sz.QuadPart > (SIZE_MAX >> 1)) {
    CloseHandle(f);
    return NULL;
  }
  HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
  CloseHandle(f);
  if (!m) return NULL;
  void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(m);
  if (p) *size = (size_t)sz.QuadPart;
  return p;
}

void plat_munmap(void *p, size_t size) {
  FM_UNUSED(size);
  if (p) UnmapViewOfFile(p);
}

/* ---- places ------------------------------------------------------------- */

static bool env_path(const wchar_t *var, const char *sub, char *out, size_t cap) {
  wchar_t w[WPATH];
  DWORD n = GetEnvironmentVariableW(var, w, WPATH);
  if (n == 0 || n >= WPATH) return false;
  char u[FM_PATH_MAX];
  to_u8(w, u, sizeof u);
  if (sub) return fm_path_join(out, cap, u, sub);
  fm_strlcpy(out, u, cap);
  return true;
}

bool plat_place(FmPlace p, char *out, size_t cap) {
  switch (p) {
    case PLACE_HOME: return env_path(L"USERPROFILE", NULL, out, cap);
    case PLACE_DESKTOP: return env_path(L"USERPROFILE", "Desktop", out, cap);
    case PLACE_DOCUMENTS: return env_path(L"USERPROFILE", "Documents", out, cap);
    case PLACE_DOWNLOADS: return env_path(L"USERPROFILE", "Downloads", out, cap);
    case PLACE_PICTURES: return env_path(L"USERPROFILE", "Pictures", out, cap);
    case PLACE_MUSIC: return env_path(L"USERPROFILE", "Music", out, cap);
    case PLACE_VIDEOS: return env_path(L"USERPROFILE", "Videos", out, cap);
    case PLACE_CONFIG:
      if (!env_path(L"APPDATA", "mmcfm", out, cap)) return false;
      plat_mkdirs(out);
      return true;
    case PLACE_CACHE:
      if (!env_path(L"LOCALAPPDATA", "mmcfm\\cache", out, cap)) return false;
      plat_mkdirs(out);
      return true;
    case PLACE_TEMP: {
      wchar_t w[MAX_PATH + 1];
      DWORD n = GetTempPathW(MAX_PATH, w);
      if (n == 0) return false;
      if (n > 0 && w[n - 1] == L'\\') w[n - 1] = 0;
      to_u8(w, out, (int)cap);
      return true;
    }
  }
  return false;
}

int plat_volumes(FmVolume *out, int max) {
  int n = 0;
  if (n < max && plat_place(PLACE_HOME, out[n].path, sizeof out[n].path)) {
    fm_strlcpy(out[n].name, "Home", sizeof out[n].name);
    out[n].kind = VOL_HOME;
    plat_disk_space(out[n].path, &out[n].total, &out[n].free);
    n++;
  }
  DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26 && n < max; i++) {
    if (!(mask & (1u << i))) continue;
    wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
    UINT t = GetDriveTypeW(root);
    if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) continue;
    FmVolume *v = &out[n];
    memset(v, 0, sizeof *v);
    fm_snprintf(v->path, sizeof v->path, "%c:\\", 'A' + i);
    v->kind = t == DRIVE_REMOVABLE ? VOL_REMOVABLE : t == DRIVE_REMOTE ? VOL_NETWORK :
              t == DRIVE_CDROM ? VOL_OPTICAL : VOL_DRIVE;
    wchar_t label[MAX_PATH + 1] = { 0 };
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);   /* no "insert disk" box */
    BOOL have = (t == DRIVE_FIXED || t == DRIVE_REMOTE)
                ? GetVolumeInformationW(root, label, MAX_PATH, NULL, NULL, NULL, NULL, 0) : FALSE;
    if (t == DRIVE_FIXED || t == DRIVE_REMOTE) plat_disk_space(v->path, &v->total, &v->free);
    SetErrorMode(old);
    char lab[160] = { 0 };
    if (have && label[0]) to_u8(label, lab, sizeof lab);
    if (lab[0]) fm_snprintf(v->name, sizeof v->name, "%s (%c:)", lab, 'A' + i);
    else fm_snprintf(v->name, sizeof v->name, "%s (%c:)",
                     v->kind == VOL_REMOVABLE ? "Removable" : v->kind == VOL_NETWORK ? "Network" :
                     v->kind == VOL_OPTICAL ? "Disc" : "Local Disk", 'A' + i);
    n++;
  }
  return n;
}

bool plat_disk_space(const char *path, u64 *total, u64 *free_bytes) {
  wchar_t w[WPATH];
  ULARGE_INTEGER avail, tot, fr;
  to_w(path, w, WPATH);
  if (!GetDiskFreeSpaceExW(w, &avail, &tot, &fr)) { *total = *free_bytes = 0; return false; }
  *total = tot.QuadPart;
  *free_bytes = avail.QuadPart;
  return true;
}

/* ---- system ------------------------------------------------------------- */

bool plat_open_external(const char *path) {
  typedef HINSTANCE (WINAPI *ShellExecFn)(HWND, const wchar_t *, const wchar_t *,
                                          const wchar_t *, const wchar_t *, int);
  static ShellExecFn fn;
  if (!fn) {
    HMODULE m = LoadLibraryW(L"shell32.dll");
    if (m) fn = (ShellExecFn)(void *)GetProcAddress(m, "ShellExecuteW");
  }
  if (!fn) return false;
  wchar_t w[WPATH];
  to_w(path, w, WPATH);
  for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
  return (INT_PTR)fn(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
}

bool plat_share(const char *path) {
  /* explorer /select,"path" shows the file highlighted. */
  wchar_t w[WPATH], cmd[WPATH + 32];
  to_w(path, w, WPATH);
  for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
  _snwprintf(cmd, WPATH + 31, L"explorer.exe /select,\"%ls\"", w);
  cmd[WPATH + 31] = 0;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void plat_random(void *buf, size_t n) {
  typedef BOOLEAN (WINAPI *GenRandomFn)(void *, ULONG);
  static GenRandomFn fn;
  if (!fn) {
    HMODULE m = LoadLibraryW(L"advapi32.dll");
    if (m) fn = (GenRandomFn)(void *)GetProcAddress(m, "SystemFunction036");
  }
  if (fn && fn(buf, (ULONG)n)) return;
  /* Last resort; never expected on any supported Windows. */
  u8 *b = (u8 *)buf;
  LARGE_INTEGER c;
  for (size_t i = 0; i < n; i++) {
    QueryPerformanceCounter(&c);
    b[i] = (u8)(c.QuadPart ^ (c.QuadPart >> 8) ^ (i * 131));
  }
}

u64 plat_now_ms(void) { return (u64)GetTickCount64(); }

i64 plat_time_unix(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return ft_to_unix(ft);
}

int plat_cpu_count(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (int)si.dwNumberOfProcessors;
}

int plat_ime_inset(void) { return 0; }

bool plat_storage_granted(void) { return true; }
void plat_storage_request(void) {}

/* ---- rounded window corners ---------------------------------------------- */

/* SDL tags every window it creates with this property; our process has one. */
static BOOL CALLBACK find_sdl_window(HWND h, LPARAM out) {
  if (GetPropW(h, L"SDL_WindowData")) {
    *(HWND *)out = h;
    return FALSE;
  }
  return TRUE;
}

static HWND sdl_hwnd(void) {
  HWND h = NULL;
  EnumThreadWindows(GetCurrentThreadId(), find_sdl_window, (LPARAM)&h);
  return h;
}

/* dwmapi is loaded at run time so the exe still starts where it is missing,
** and every attribute below is simply refused by Windows before 11. */
typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);

static DwmSetAttrFn dwm_set_attr(void) {
  static int tried;
  static DwmSetAttrFn fn;
  if (!tried) {
    tried = 1;
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (m) fn = (DwmSetAttrFn)(void (*)(void))GetProcAddress(m, "DwmSetWindowAttribute");
  }
  return fn;
}

enum { DWMWA_CORNER = 33, DWMWA_BORDER = 34 };          /* Windows 11 (build 22000) */
enum { CORNER_DEFAULT = 0, CORNER_NONE = 1, CORNER_ROUND = 2 };

int plat_window_corners(bool round, int radius_px, u32 border_rgb, bool maximized) {
  HWND h = sdl_hwnd();
  if (!h) return 0;
  DwmSetAttrFn set = dwm_set_attr();
  /* MMCFM_CORNERS=region tests the pre-Windows-11 path on a new system */
  const char *force = getenv("MMCFM_CORNERS");
  if (force && !strcmp(force, "region")) set = NULL;
  if (set) {
    DWORD pref = round ? CORNER_ROUND : CORNER_NONE;
    if (SUCCEEDED(set(h, DWMWA_CORNER, &pref, sizeof pref))) {
      /* the compositor rounds, anti-aliases and shadows; it also draws the
      ** 1px edge, which we colour like the theme (0xFFFFFFFE = no border) */
      COLORREF c = round ? RGB((border_rgb >> 16) & 255, (border_rgb >> 8) & 255, border_rgb & 255)
                         : (COLORREF)0xFFFFFFFF;          /* DWMWA_COLOR_DEFAULT */
      set(h, DWMWA_BORDER, &c, sizeof c);
      SetWindowRgn(h, NULL, TRUE);
      return 1;
    }
  }
  /* Windows 7 / 8 / 10: clip the window to a rounded region (no AA). */
  if (!round || maximized) {
    SetWindowRgn(h, NULL, TRUE);
    return 0;
  }
  RECT r;
  if (!GetWindowRect(h, &r)) return 0;
  int d = radius_px * 2;
  HRGN rgn = CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, d, d);
  if (!rgn) return 0;
  if (!SetWindowRgn(h, rgn, TRUE)) {     /* the window owns rgn on success */
    DeleteObject(rgn);
    return 0;
  }
  return 2;
}

#endif
