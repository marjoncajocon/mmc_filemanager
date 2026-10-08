/* fplat_posix.c -- fplat.h for Linux, macOS, BSD, Android and the web.
**
** Design decisions:
**   - Directory listing uses fstatat on the open directory fd, so a 10 000
**     file folder costs one path lookup per entry instead of a full path
**     walk; symlinks are stat'ed twice (lstat for the link flag, stat for
**     the target) and a failing second stat marks the entry FM_ST_BROKEN.
**   - plat_rename never replaces: renameat2(RENAME_NOREPLACE) on Linux,
**     renamex_np(RENAME_EXCL) on macOS, an lstat check elsewhere. A rename
**     across file systems returns FM_ERR_UNSUPPORTED so the job engine
**     knows to fall back to copy + delete.
**   - External programs are started with fork + execvp and a double fork,
**     never system(): file names are passed as one argv entry each, so no
**     quoting can go wrong, and no zombie is left behind. A close-on-exec
**     pipe tells the parent whether exec succeeded.
**   - The trash follows the freedesktop.org spec: the .trashinfo file is
**     created first with O_EXCL, which reserves the name atomically, then
**     the file is renamed into Trash/files.
**   - Android: storage permission, opening, sharing and the volume list come
**     from fplat_android.c (JNI); everything else is plain POSIX here.
*/
/* POSIX and BSD calls (fstatat, clock_gettime, localtime_r ...) under strict -std=c11;
** must come before the first system header. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif
#include "fcore.h"
#ifdef FM_POSIX
#include "fplat.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>

#if defined(FM_LINUX) || defined(FM_ANDROID)
#  include <sys/syscall.h>
#endif
#if defined(FM_MACOS) || defined(FM_IOS) || defined(FM_BSD)
#  include <sys/param.h>
#  include <sys/mount.h>
#endif
#ifdef FM_ANDROID
#  include "fsdl.h"
#endif

/* ---- errors ------------------------------------------------------------- */

static FmErr errno_err(int e) {
  switch (e) {
    case 0: return FM_OK;
    case ENOENT: case ENOTDIR: return FM_ERR_NOT_FOUND;
    case EEXIST: case ENOTEMPTY: return FM_ERR_EXISTS;
    case EACCES: case EPERM: case EROFS: return FM_ERR_ACCESS;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
      return FM_ERR_FULL;
    case ENOMEM: return FM_ERR_NOMEM;
    case EXDEV: return FM_ERR_UNSUPPORTED;
    default: return FM_ERR_IO;
  }
}

static FmErr last_err(void) { return errno_err(errno); }

/* ---- stat --------------------------------------------------------------- */

static i64 st_mtime_of(const struct stat *s) { return (i64)s->st_mtime; }

static u32 mode_flags(const struct stat *s) {
  u32 f = 0;
  if (S_ISDIR(s->st_mode)) f |= FM_ST_DIR;
  else if (s->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) f |= FM_ST_EXEC;
  if (!(s->st_mode & S_IWUSR)) f |= FM_ST_READONLY;
  if (!S_ISDIR(s->st_mode) && !S_ISREG(s->st_mode) && !S_ISLNK(s->st_mode)) f |= FM_ST_SYSTEM;
  return f;
}

static void fill_stat(FmStat *st, const struct stat *s) {
  st->size = S_ISREG(s->st_mode) ? (u64)s->st_size : 0;
  st->mtime = st_mtime_of(s);
  st->flags = mode_flags(s);
  st->mode = (u32)(s->st_mode & 07777);
}

/* lst = lstat result; follows the link with the given stat function. */
static void stat_entry(FmStat *st, const struct stat *lst, const char *name, bool link_ok,
                       const struct stat *target) {
  if (S_ISLNK(lst->st_mode)) {
    if (link_ok) {
      fill_stat(st, target);
    } else {
      fill_stat(st, lst);
      st->flags |= FM_ST_BROKEN;
      st->flags &= ~(u32)FM_ST_SYSTEM;
    }
    st->flags |= FM_ST_LINK;
  } else {
    fill_stat(st, lst);
  }
  if (name[0] == '.') st->flags |= FM_ST_HIDDEN;
}

bool plat_stat(const char *path, FmStat *st) {
  struct stat ls, ts;
  memset(st, 0, sizeof *st);
  if (lstat(path, &ls) != 0) return false;
  bool ok = !S_ISLNK(ls.st_mode) || stat(path, &ts) == 0;
  stat_entry(st, &ls, fm_path_base(path), ok, &ts);
  return true;
}

bool plat_exists(const char *path) {
  struct stat s;
  return lstat(path, &s) == 0;
}

bool plat_is_dir(const char *path) {
  struct stat s;
  return stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}

/* ---- directories -------------------------------------------------------- */

struct FmDir {
  DIR *d;
  int fd;
};

FmDir *plat_dir_open(const char *path, FmErr *err) {
  DIR *d = opendir(path);
  if (!d) {
    if (err) *err = last_err();
    return NULL;
  }
  FmDir *r = (FmDir *)fm_calloc(1, sizeof *r);
  r->d = d;
  r->fd = dirfd(d);
  if (err) *err = FM_OK;
  return r;
}

bool plat_dir_next(FmDir *d, const char **name, FmStat *st) {
  for (;;) {
    errno = 0;
    struct dirent *e = readdir(d->d);
    if (!e) return false;
    const char *n = e->d_name;
    if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
    *name = n;
    if (st) {
      struct stat ls, ts;
      memset(st, 0, sizeof *st);
      if (fstatat(d->fd, n, &ls, AT_SYMLINK_NOFOLLOW) != 0) {
        /* vanished between readdir and stat: still show the name */
        if (n[0] == '.') st->flags |= FM_ST_HIDDEN;
        return true;
      }
      bool ok = !S_ISLNK(ls.st_mode) || fstatat(d->fd, n, &ts, 0) == 0;
      stat_entry(st, &ls, n, ok, &ts);
    }
    return true;
  }
}

void plat_dir_close(FmDir *d) {
  if (!d) return;
  if (d->d) closedir(d->d);
  fm_free(d);
}

/* ---- changes ------------------------------------------------------------ */

FmErr plat_mkdir(const char *path) {
  return mkdir(path, 0777) == 0 ? FM_OK : last_err();
}

FmErr plat_mkdirs(const char *path) {
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  size_t n = strlen(p);
  for (size_t i = 1; i <= n; i++) {
    if (i == n || p[i] == '/') {
      char c = p[i];
      p[i] = 0;
      if (!plat_is_dir(p)) {
        FmErr e = plat_mkdir(p);
        if (e != FM_OK && e != FM_ERR_EXISTS) return e;
      }
      p[i] = c;
    }
  }
  return FM_OK;
}

FmErr plat_remove_file(const char *path) {
  return unlink(path) == 0 ? FM_OK : last_err();
}

FmErr plat_remove_dir(const char *path) {
  return rmdir(path) == 0 ? FM_OK : last_err();
}

FmErr plat_rename(const char *from, const char *to) {
#if (defined(FM_LINUX) || defined(FM_ANDROID)) && defined(SYS_renameat2)
  /* RENAME_NOREPLACE = 1; not every libc exposes the constant. */
  if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1) == 0) return FM_OK;
  if (errno != ENOSYS && errno != EINVAL) return last_err();
#elif defined(FM_MACOS) && defined(RENAME_EXCL)
  if (renamex_np(from, to, RENAME_EXCL) == 0) return FM_OK;
  if (errno != ENOTSUP && errno != EINVAL) return last_err();
#endif
  /* Fallback: a small race window, acceptable where no atomic call exists. */
  struct stat s;
  if (lstat(to, &s) == 0) return FM_ERR_EXISTS;
  return rename(from, to) == 0 ? FM_OK : last_err();
}

FmErr plat_set_mtime(const char *path, i64 mtime) {
  struct timespec ts[2];
  ts[0].tv_sec = (time_t)mtime;
  ts[0].tv_nsec = 0;
  ts[1] = ts[0];
  return utimensat(AT_FDCWD, path, ts, 0) == 0 ? FM_OK : last_err();
}

FmErr plat_set_mode(const char *path, u32 mode) {
  return chmod(path, (mode_t)(mode & 07777)) == 0 ? FM_OK : last_err();
}

/* Device of path, or of its nearest existing parent (targets may not exist yet). */
static bool path_dev(const char *path, dev_t *dev) {
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  for (;;) {
    struct stat s;
    if (stat(p, &s) == 0) { *dev = s.st_dev; return true; }
    if (!fm_path_parent(p)) return false;
  }
}

bool plat_same_volume(const char *a, const char *b) {
  dev_t da, db;
  return path_dev(a, &da) && path_dev(b, &db) && da == db;
}

/* ---- files -------------------------------------------------------------- */

FILE *fm_fopen(const char *path, const char *mode) { return fopen(path, mode); }

int fm_fseek64(FILE *f, i64 off, int whence) { return fseeko(f, (off_t)off, whence); }

i64 fm_ftell64(FILE *f) { return (i64)ftello(f); }

i64 fm_fsize(FILE *f) {
  struct stat s;
  if (fstat(fileno(f), &s) != 0) return -1;
  return (i64)s.st_size;
}

void *plat_mmap(const char *path, size_t *size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat s;
  if (fstat(fd, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size <= 0 ||
      (u64)s.st_size > (u64)(SIZE_MAX >> 1)) {
    close(fd);
    return NULL;
  }
  void *p = mmap(NULL, (size_t)s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return NULL;
  *size = (size_t)s.st_size;
  return p;
}

void plat_munmap(void *p, size_t size) {
  if (p) munmap(p, size);
}

/* ---- places ------------------------------------------------------------- */

static bool home_dir(char *out, size_t cap) {
#ifdef FM_ANDROID
  fm_strlcpy(out, "/storage/emulated/0", cap);
  return true;
#else
  const char *h = getenv("HOME");
  if (h && h[0]) { fm_strlcpy(out, h, cap); return true; }
  struct passwd *pw = getpwuid(getuid());
  if (pw && pw->pw_dir && pw->pw_dir[0]) { fm_strlcpy(out, pw->pw_dir, cap); return true; }
  fm_strlcpy(out, "/", cap);
  return true;
#endif
}

static bool home_sub(const char *sub, char *out, size_t cap) {
  char h[FM_PATH_MAX];
  home_dir(h, sizeof h);
  return fm_path_join(out, cap, h, sub);
}

#if !defined(FM_ANDROID) && !defined(FM_MACOS) && !defined(FM_IOS)
/* $VAR/sub, or ~/fallback/sub when the variable is unset or relative. */
static bool xdg_base(const char *var, const char *fallback, const char *sub, char *out,
                     size_t cap) {
  const char *v = getenv(var);
  char base[FM_PATH_MAX];
  if (v && v[0] == '/') fm_strlcpy(base, v, sizeof base);
  else if (!home_sub(fallback, base, sizeof base)) return false;
  return fm_path_join(out, cap, base, sub);
}

/* Reads XDG_<key>_DIR from ~/.config/user-dirs.dirs ("$HOME/..." or absolute). */
static bool xdg_user_dir(const char *key, char *out, size_t cap) {
  char cfg[FM_PATH_MAX];
  if (!xdg_base("XDG_CONFIG_HOME", ".config", "user-dirs.dirs", cfg, sizeof cfg)) return false;
  FILE *f = fopen(cfg, "r");
  if (!f) return false;
  char want[64], line[FM_PATH_MAX + 64];
  fm_snprintf(want, sizeof want, "XDG_%s_DIR=", key);
  size_t wl = strlen(want);
  bool found = false;
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, want, wl) != 0) continue;
    s += wl;
    if (*s != '"') continue;
    s++;
    char *e = strchr(s, '"');
    if (!e) continue;
    *e = 0;
    if (strncmp(s, "$HOME", 5) == 0 && (s[5] == '/' || !s[5])) {
      char h[FM_PATH_MAX];
      home_dir(h, sizeof h);
      found = s[5] ? fm_path_join(out, cap, h, s + 6) : (fm_strlcpy(out, h, cap), true);
    } else if (s[0] == '/') {
      fm_strlcpy(out, s, cap);
      found = true;
    }
    break;
  }
  fclose(f);
  return found;
}
#endif

#ifndef FM_ANDROID
/* A user folder: XDG name on Linux/BSD, plain ~/Name elsewhere; must exist. */
static bool user_dir(const char *xdg_key, const char *name, char *out, size_t cap) {
#if !defined(FM_ANDROID) && !defined(FM_MACOS) && !defined(FM_IOS)
  if (xdg_user_dir(xdg_key, out, cap) && plat_is_dir(out)) {
    char h[FM_PATH_MAX];
    home_dir(h, sizeof h);
    /* user-dirs.dirs points a disabled folder at $HOME itself */
    if (strcmp(out, h) != 0) return true;
    return false;
  }
#else
  FM_UNUSED(xdg_key);
#endif
  return home_sub(name, out, cap) && plat_is_dir(out);
}
#endif

#ifdef FM_ANDROID
static bool android_private(const char *sub, char *out, size_t cap) {
  const char *base = SDL_AndroidGetInternalStoragePath();
  if (!base || !base[0]) return false;
  if (!fm_path_join(out, cap, base, sub)) return false;
  plat_mkdirs(out);
  return true;
}
#endif

bool plat_place(FmPlace p, char *out, size_t cap) {
  switch (p) {
    case PLACE_HOME: return home_dir(out, cap);
#ifdef FM_ANDROID
    case PLACE_DESKTOP: return false;
    case PLACE_DOCUMENTS: return home_sub("Documents", out, cap) && plat_is_dir(out);
    case PLACE_DOWNLOADS: return home_sub("Download", out, cap) && plat_is_dir(out);
    case PLACE_PICTURES:
      if (home_sub("DCIM", out, cap) && plat_is_dir(out)) return true;
      return home_sub("Pictures", out, cap) && plat_is_dir(out);
    case PLACE_MUSIC: return home_sub("Music", out, cap) && plat_is_dir(out);
    case PLACE_VIDEOS: return home_sub("Movies", out, cap) && plat_is_dir(out);
    case PLACE_CONFIG: return android_private("config", out, cap);
    case PLACE_CACHE: return android_private("cache", out, cap);
    case PLACE_TEMP: return android_private("cache/tmp", out, cap);
#else
    case PLACE_DESKTOP: return user_dir("DESKTOP", "Desktop", out, cap);
    case PLACE_DOCUMENTS: return user_dir("DOCUMENTS", "Documents", out, cap);
    case PLACE_DOWNLOADS: return user_dir("DOWNLOAD", "Downloads", out, cap);
    case PLACE_PICTURES: return user_dir("PICTURES", "Pictures", out, cap);
    case PLACE_MUSIC: return user_dir("MUSIC", "Music", out, cap);
#  if defined(FM_MACOS) || defined(FM_IOS)
    case PLACE_VIDEOS: return user_dir("VIDEOS", "Movies", out, cap);
    case PLACE_CONFIG:
      if (!home_sub("Library/Application Support/mmcfm", out, cap)) return false;
      plat_mkdirs(out);
      return true;
    case PLACE_CACHE:
      if (!home_sub("Library/Caches/mmcfm", out, cap)) return false;
      plat_mkdirs(out);
      return true;
#  else
    case PLACE_VIDEOS: return user_dir("VIDEOS", "Videos", out, cap);
    case PLACE_CONFIG:
      if (!xdg_base("XDG_CONFIG_HOME", ".config", "mmcfm", out, cap)) return false;
      plat_mkdirs(out);
      return true;
    case PLACE_CACHE:
      if (!xdg_base("XDG_CACHE_HOME", ".cache", "mmcfm", out, cap)) return false;
      plat_mkdirs(out);
      return true;
#  endif
    case PLACE_TEMP: {
      const char *t = getenv("TMPDIR");
      fm_strlcpy(out, (t && t[0] == '/') ? t : "/tmp", cap);
      size_t n = strlen(out);
      while (n > 1 && out[n - 1] == '/') out[--n] = 0;
      return true;
    }
#endif
  }
  return false;
}

/* ---- volumes ------------------------------------------------------------ */

bool plat_disk_space(const char *path, u64 *total, u64 *free_bytes) {
  struct statvfs s;
  if (statvfs(path, &s) != 0) { *total = *free_bytes = 0; return false; }
  u64 frs = s.f_frsize ? (u64)s.f_frsize : (u64)s.f_bsize;
  *total = (u64)s.f_blocks * frs;
  *free_bytes = (u64)s.f_bavail * frs;
  return true;
}

static int vol_add(FmVolume *out, int n, int max, const char *name, const char *path,
                   FmVolKind kind) {
  if (n >= max) return n;
  for (int i = 0; i < n; i++)
    if (strcmp(out[i].path, path) == 0) return n;
  FmVolume *v = &out[n];
  memset(v, 0, sizeof *v);
  fm_strlcpy(v->name, name, sizeof v->name);
  fm_strlcpy(v->path, path, sizeof v->path);
  v->kind = kind;
  plat_disk_space(path, &v->total, &v->free);
  return n + 1;
}

#if defined(FM_LINUX)
/* /proc/mounts escapes space, tab, newline and backslash as \ooo. */
static void unescape_mount(char *s) {
  char *w = s;
  while (*s) {
    if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
        s[3] >= '0' && s[3] <= '7') {
      *w++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
      s += 4;
    } else {
      *w++ = *s++;
    }
  }
  *w = 0;
}

static bool starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int linux_mounts(FmVolume *out, int n, int max) {
  FILE *f = fopen("/proc/self/mounts", "r");
  if (!f) f = fopen("/proc/mounts", "r");
  if (!f) return n;
  char line[FM_PATH_MAX * 2];
  while (n < max && fgets(line, sizeof line, f)) {
    char dev[512], mnt[FM_PATH_MAX], type[64];
    if (sscanf(line, "%511s %1023s %63s", dev, mnt, type) != 3) continue;
    unescape_mount(mnt);
    if (!starts_with(mnt, "/media/") && !starts_with(mnt, "/run/media/") &&
        !starts_with(mnt, "/mnt/"))
      continue;
    bool net = !strcmp(type, "nfs") || !strcmp(type, "nfs4") || !strcmp(type, "cifs") ||
               !strcmp(type, "smb3") || !strcmp(type, "smbfs") || !strcmp(type, "fuse.sshfs") ||
               !strcmp(type, "9p");
    bool optical = !strcmp(type, "iso9660") || !strcmp(type, "udf");
    /* real file systems sit on a block device; network ones are named above */
    if (!net && !starts_with(dev, "/dev/")) continue;
    if (strstr(dev, "/dev/loop") == dev && !optical) continue;
    FmVolKind k = net ? VOL_NETWORK : optical ? VOL_OPTICAL :
                  starts_with(mnt, "/mnt/") ? VOL_DRIVE : VOL_REMOVABLE;
    n = vol_add(out, n, max, fm_path_base(mnt), mnt, k);
  }
  fclose(f);
  return n;
}
#endif

#if defined(FM_MACOS)
static int mac_volumes(FmVolume *out, int n, int max) {
  FmDir *d = plat_dir_open("/Volumes", NULL);
  if (!d) return n;
  const char *name;
  FmStat st;
  struct stat root;
  bool have_root = stat("/", &root) == 0;
  while (n < max && plat_dir_next(d, &name, &st)) {
    if (name[0] == '.' || !(st.flags & FM_ST_DIR)) continue;
    char p[FM_PATH_MAX];
    if (!fm_path_join(p, sizeof p, "/Volumes", name)) continue;
    struct stat s;
    /* the boot volume appears here as a link to "/" */
    if (have_root && stat(p, &s) == 0 && s.st_dev == root.st_dev) continue;
    struct statfs fs;
    FmVolKind k = VOL_REMOVABLE;
    if (statfs(p, &fs) == 0 && !(fs.f_flags & MNT_LOCAL)) k = VOL_NETWORK;
    n = vol_add(out, n, max, name, p, k);
  }
  plat_dir_close(d);
  return n;
}
#endif

#if defined(FM_BSD) && !defined(__NetBSD__)
static int bsd_mounts(FmVolume *out, int n, int max) {
  struct statfs *m = NULL;
  int c = getmntinfo(&m, MNT_NOWAIT);
  for (int i = 0; i < c && n < max; i++) {
    const char *mnt = m[i].f_mntonname;
    if (strncmp(mnt, "/media/", 7) != 0 && strncmp(mnt, "/mnt/", 5) != 0 &&
        strcmp(mnt, "/mnt") != 0)
      continue;
    FmVolKind k = (m[i].f_flags & MNT_LOCAL) ? VOL_REMOVABLE : VOL_NETWORK;
    n = vol_add(out, n, max, fm_path_base(mnt), mnt, k);
  }
  return n;
}
#endif

#ifdef FM_ANDROID
/* fplat_android.c: internal storage first, then SD cards and USB drives. */
extern int plat_android_volumes(FmVolume *out, int max);
#endif

int plat_volumes(FmVolume *out, int max) {
  int n = 0;
#ifdef FM_ANDROID
  n = plat_android_volumes(out, max);
  if (n <= 0) n = vol_add(out, 0, max, "Internal storage", "/storage/emulated/0", VOL_INTERNAL);
#else
  char h[FM_PATH_MAX];
  home_dir(h, sizeof h);
  n = vol_add(out, n, max, "Home", h, VOL_HOME);
  n = vol_add(out, n, max, "File system", "/", VOL_ROOT);
#  if defined(FM_LINUX)
  n = linux_mounts(out, n, max);
#  elif defined(FM_MACOS)
  n = mac_volumes(out, n, max);
#  elif defined(FM_BSD) && !defined(__NetBSD__)
  n = bsd_mounts(out, n, max);
#  endif
#endif
  return n;
}

/* ---- trash -------------------------------------------------------------- */

#if !defined(FM_ANDROID) && !defined(FM_WEB) && !defined(FM_IOS)

#  if !defined(FM_MACOS)
static void pct_encode(const char *s, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s && o + 4 < cap; s++) {
    u8 c = (u8)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = 0;
}

/* Mount point of the file system holding path: walk up while st_dev stays. */
static void mount_top(const char *path, dev_t dev, char *out, size_t cap) {
  char p[FM_PATH_MAX], last[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  fm_strlcpy(last, path, sizeof last);
  while (fm_path_parent(p)) {
    struct stat s;
    if (stat(p, &s) != 0 || s.st_dev != dev) break;
    fm_strlcpy(last, p, sizeof last);
  }
  fm_strlcpy(out, last, cap);
}

/* Picks the trash for `abs` (absolute) on device dev. `top` receives the
** mount point for a $topdir trash (Path= is then stored relative to it),
** or "" for the home trash. */
static bool pick_trash(const char *abs, dev_t dev, char *trash, size_t cap, char *top,
                       size_t topcap) {
  char home_trash[FM_PATH_MAX];
  top[0] = 0;
  if (xdg_base("XDG_DATA_HOME", ".local/share", "Trash", home_trash, sizeof home_trash)) {
    char files[FM_PATH_MAX];
    fm_path_join(files, sizeof files, home_trash, "files");
    plat_mkdirs(files);
    fm_path_join(files, sizeof files, home_trash, "info");
    plat_mkdirs(files);
    dev_t hd;
    if (path_dev(home_trash, &hd) && hd == dev) {
      fm_strlcpy(trash, home_trash, cap);
      return true;
    }
  }
  /* $topdir/.Trash-$uid on other file systems */
  mount_top(abs, dev, top, topcap);
  char name[32];
  fm_snprintf(name, sizeof name, ".Trash-%u", (unsigned)getuid());
  if (!fm_path_join(trash, cap, top, name)) return false;
  mkdir(trash, 0700);
  struct stat s;
  if (lstat(trash, &s) != 0 || !S_ISDIR(s.st_mode) || s.st_uid != getuid()) return false;
  char sub[FM_PATH_MAX];
  fm_path_join(sub, sizeof sub, trash, "files");
  mkdir(sub, 0700);
  fm_path_join(sub, sizeof sub, trash, "info");
  mkdir(sub, 0700);
  return true;
}

FmErr plat_trash(const char *path) {
  char abs[FM_PATH_MAX];
  if (path[0] != '/') {
    char cwd[FM_PATH_MAX];
    if (!getcwd(cwd, sizeof cwd) || !fm_path_join(abs, sizeof abs, cwd, path)) return FM_ERR_IO;
  } else {
    fm_strlcpy(abs, path, sizeof abs);
  }
  fm_path_normalize(abs);
  struct stat ls;
  if (lstat(abs, &ls) != 0) return last_err();
  char trash[FM_PATH_MAX], top[FM_PATH_MAX];
  if (!pick_trash(abs, ls.st_dev, trash, sizeof trash, top, sizeof top)) return FM_ERR_UNSUPPORTED;

  /* Path= is relative to the top dir for $topdir trashes, absolute otherwise */
  const char *stored = abs;
  if (top[0] && strcmp(top, "/") != 0) {
    size_t tl = strlen(top);
    if (strncmp(abs, top, tl) == 0 && abs[tl] == '/') stored = abs + tl + 1;
  }
  char enc[FM_PATH_MAX * 3];
  pct_encode(stored, enc, sizeof enc);
  char date[32];
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tmv);

  const char *base = fm_path_base(abs);
  char name[512], info[FM_PATH_MAX], dst[FM_PATH_MAX];
  for (int i = 1; i < 10000; i++) {
    if (i == 1) fm_strlcpy(name, base, sizeof name);
    else fm_snprintf(name, sizeof name, "%s.%d", base, i);
    char rel[600];
    fm_snprintf(rel, sizeof rel, "info/%s.trashinfo", name);
    if (!fm_path_join(info, sizeof info, trash, rel)) return FM_ERR_IO;
    int fd = open(info, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return last_err();
    }
    char body[FM_PATH_MAX * 3 + 96];
    int len = fm_snprintf(body, sizeof body, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
    bool wrote = len > 0 && write(fd, body, (size_t)len) == (ssize_t)len;
    close(fd);
    fm_snprintf(rel, sizeof rel, "files/%s", name);
    fm_path_join(dst, sizeof dst, trash, rel);
    if (!wrote || lstat(dst, &ls) == 0) {
      unlink(info);
      if (!wrote) return FM_ERR_IO;
      continue;
    }
    if (rename(abs, dst) != 0) {
      FmErr e = last_err();
      unlink(info);
      return e;
    }
    return FM_OK;
  }
  return FM_ERR_EXISTS;
}

#  else  /* macOS: ~/.Trash, Finder-style "name 2.ext" on collisions */

FmErr plat_trash(const char *path) {
  char trash[FM_PATH_MAX];
  if (!home_sub(".Trash", trash, sizeof trash)) return FM_ERR_UNSUPPORTED;
  mkdir(trash, 0700);
  if (!plat_same_volume(path, trash)) return FM_ERR_UNSUPPORTED;
  const char *base = fm_path_base(path);
  const char *ext = fm_path_ext(base);
  char stem[512], name[600], dst[FM_PATH_MAX];
  fm_strlcpy(stem, base, FM_MIN(sizeof stem, (size_t)(ext - base) + 1));
  for (int i = 1; i < 10000; i++) {
    if (i == 1) fm_strlcpy(name, base, sizeof name);
    else fm_snprintf(name, sizeof name, "%s %d%s", stem, i, ext);
    if (!fm_path_join(dst, sizeof dst, trash, name)) return FM_ERR_IO;
    FmErr e = plat_rename(path, dst);
    if (e != FM_ERR_EXISTS) return e;
  }
  return FM_ERR_EXISTS;
}

#  endif
#else

FmErr plat_trash(const char *path) {
  FM_UNUSED(path);
  return FM_ERR_UNSUPPORTED;
}

#endif

/* ---- system ------------------------------------------------------------- */

#if !defined(FM_ANDROID)

#  if !defined(FM_WEB) && !defined(FM_IOS)
/* Runs argv detached (double fork); true when exec succeeded. */
static bool spawn_detached(char *const argv[]) {
  int pfd[2];
  if (pipe(pfd) != 0) return false;
  fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  if (pid < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return false;
  }
  if (pid == 0) {
    close(pfd[0]);
    setsid();
    pid_t g = fork();
    if (g == 0) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
      }
      execvp(argv[0], argv);
      char fail = 1;
      if (write(pfd[1], &fail, 1) < 0) _exit(127);
      _exit(127);
    }
    _exit(g < 0 ? 1 : 0);
  }
  close(pfd[1]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  char c;
  ssize_t r;
  do { r = read(pfd[0], &c, 1); } while (r < 0 && errno == EINTR);
  close(pfd[0]);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 && r == 0;
}
#  endif

bool plat_open_external(const char *path) {
#  if defined(FM_WEB) || defined(FM_IOS)
  FM_UNUSED(path);
  return false;
#  else
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
#    if defined(FM_MACOS)
  char *argv[] = { (char *)"open", p, NULL };
#    else
  char *argv[] = { (char *)"xdg-open", p, NULL };
#    endif
  return spawn_detached(argv);
#  endif
}

bool plat_share(const char *path) {
#  if defined(FM_WEB) || defined(FM_IOS)
  FM_UNUSED(path);
  return false;
#  elif defined(FM_MACOS)
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  char *argv[] = { (char *)"open", (char *)"-R", p, NULL };
  return spawn_detached(argv);
#  else
  /* no portable "reveal" on Linux/BSD: open the containing folder */
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  if (!plat_is_dir(p) && !fm_path_parent(p)) return false;
  char *argv[] = { (char *)"xdg-open", p, NULL };
  return spawn_detached(argv);
#  endif
}

bool plat_storage_granted(void) { return true; }
void plat_storage_request(void) {}

#endif

void plat_random(void *buf, size_t n) {
#if defined(FM_MACOS) || defined(FM_IOS) || defined(FM_BSD) || defined(FM_ANDROID)
  arc4random_buf(buf, n);
#else
  u8 *b = (u8 *)buf;
  size_t got = 0;
#  if defined(FM_LINUX) && defined(SYS_getrandom)
  while (got < n) {
    long r = syscall(SYS_getrandom, b + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    got += (size_t)r;
  }
#  endif
  if (got < n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
      }
      close(fd);
    }
  }
  /* last resort, never expected on a working system */
  for (; got < n; got++) b[got] = (u8)(rand() ^ (int)(plat_now_ms() * 131));
#endif
}

u64 plat_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000u + (u64)(ts.tv_nsec / 1000000);
}

i64 plat_time_unix(void) { return (i64)time(NULL); }

int plat_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}

#ifndef FM_ANDROID
int plat_ime_inset(void) { return 0; }     /* fplat_android.c on Android */
#endif

/* Linux/BSD/macOS: square corners for now (needs per-platform window work). */
int plat_window_corners(bool round, int radius_px, u32 border_rgb, bool maximized) {
  FM_UNUSED(round); FM_UNUSED(radius_px); FM_UNUSED(border_rgb); FM_UNUSED(maximized);
  return 0;
}

#endif
