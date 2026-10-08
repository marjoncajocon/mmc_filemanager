/* fvfs.c -- listing a local folder or a folder inside an archive.
**
** Design decisions:
**   - Names live in the listing's arena and the entry array keeps its
**     capacity between folders, so browsing allocates almost nothing after
**     the first few folders.
**   - An archive is opened once (FmListing.arc) and kept while the panel
**     stays inside it; every folder inside is synthesized from the flat
**     entry list. Folders that only exist implicitly ("a/b/c.txt" implies
**     "a" and "a/b") get arc_index -1; a hash set keeps names unique.
**   - The archive password comes from the shared password cache (fops), so
**     the panel asks once and listing, viewing and extracting all reuse it.
**   - vfs_materialize writes to PLACE_CACHE/view/<hash>/<name>, where the
**     hash covers archive path, entry path, size and mtime: a cached copy is
**     reused only while it is still the same entry.
*/
#include "fvfs.h"
#include "fops.h"

/* ---- locations ---------------------------------------------------------- */

static bool same_path(const char *a, const char *b) {
#ifdef FM_WIN
  return fm_stricmp(a, b) == 0;
#else
  return strcmp(a, b) == 0;
#endif
}

void loc_local(FmLoc *loc, const char *path) {
  char p[FM_PATH_MAX];
  fm_strlcpy(p, path, sizeof p);
  memset(loc, 0, sizeof *loc);
  fm_path_normalize(p);
  fm_strlcpy(loc->path, p, sizeof loc->path);
}

bool loc_up(FmLoc *loc) {
  if (loc->in_arc) {
    size_t n = strlen(loc->inner);
    if (n == 0) {
      /* leave the archive: land in the folder that holds it */
      char p[FM_PATH_MAX];
      fm_strlcpy(p, loc->path, sizeof p);
      if (!fm_path_parent(p)) return false;
      loc_local(loc, p);
      return true;
    }
    n--;                                   /* drop the trailing '/' */
    while (n > 0 && loc->inner[n - 1] != '/') n--;
    loc->inner[n] = 0;
    return true;
  }
  return fm_path_parent(loc->path);
}

void loc_title(const FmLoc *loc, char *out, size_t cap) {
  if (loc->in_arc && loc->inner[0]) {
    char tmp[FM_PATH_MAX];
    fm_strlcpy(tmp, loc->inner, sizeof tmp);
    size_t n = strlen(tmp);
    if (n && tmp[n - 1] == '/') tmp[--n] = 0;
    const char *s = strrchr(tmp, '/');
    fm_strlcpy(out, s ? s + 1 : tmp, cap);
    return;
  }
  if (fm_path_is_root(loc->path)) {
    fm_strlcpy(out, loc->path, cap);
    return;
  }
  fm_strlcpy(out, fm_path_base(loc->path), cap);
}

void loc_display(const FmLoc *loc, char *out, size_t cap) {
  fm_strlcpy(out, loc->path, cap);
  if (!loc->in_arc || !loc->inner[0]) return;
  size_t n = strlen(out);
  if (n && !fm_is_sep(out[n - 1])) fm_strlcat(out, FM_SEP_STR, cap);
  size_t start = strlen(out);
  fm_strlcat(out, loc->inner, cap);
  for (char *s = out + start; *s; s++) if (*s == '/') *s = FM_SEP;
  n = strlen(out);
  if (n > start && out[n - 1] == FM_SEP) out[n - 1] = 0;
}

bool loc_equal(const FmLoc *a, const FmLoc *b) {
  return a->in_arc == b->in_arc && same_path(a->path, b->path) && strcmp(a->inner, b->inner) == 0;
}

/* ---- listing ------------------------------------------------------------ */

static FmEntry *push(FmListing *l) {
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->items = (FmEntry *)fm_realloc(l->items, (size_t)l->cap * sizeof *l->items);
  }
  FmEntry *e = &l->items[l->count++];
  memset(e, 0, sizeof *e);
  e->arc_index = -1;
  return e;
}

static FmErr list_local(FmListing *l, bool show_hidden) {
  FmErr err = FM_OK;
  FmDir *d = plat_dir_open(l->loc.path, &err);
  if (!d) return err ? err : FM_ERR_IO;
  const char *name;
  FmStat st;
  while (plat_dir_next(d, &name, &st)) {
    if ((st.flags & FM_ST_HIDDEN) && !show_hidden) continue;
    FmEntry *e = push(l);
    e->name = arena_strdup(&l->arena, name);
    e->size = st.size;
    e->mtime = st.mtime;
    e->flags = st.flags;
    e->type = (u16)((st.flags & FM_ST_DIR) ? FT_DIR : fm_type_from_name(name));
    if (!(st.flags & FM_ST_DIR)) l->total_size += st.size;
  }
  plat_dir_close(d);
  return FM_OK;
}

static bool cb_password(void *ud, char *buf, int cap, bool retry) {
  if (retry) return false;
  return ops_password_lookup((const char *)ud, buf, cap);
}

static FmArcCb arc_cb(const char *path) {
  FmArcCb cb;
  memset(&cb, 0, sizeof cb);
  cb.ud = (void *)path;
  cb.password = cb_password;
  return cb;
}

static u32 name_hash(const char *s, size_t n) {
  u32 h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
  return h;
}

static FmErr list_arc(FmListing *l, bool show_hidden) {
  if (!l->arc) {
    FmArcCb cb = arc_cb(l->loc.path);
    FmArc *a = NULL;
    FmErr e = arc_open(l->loc.path, &cb, &a);
    if (e != FM_OK) {
      if (a) arc_close(a);
      return e;
    }
    if (!a) return FM_ERR_FORMAT;
    l->arc = a;
  }
  const char *inner = l->loc.inner;
  size_t il = strlen(inner);
  int n = arc_count(l->arc);
  /* hash set of entry indices by name, sized for the worst case */
  u32 tcap = 64;
  while ((int)tcap < n * 2 && tcap < (1u << 24)) tcap <<= 1;
  int *table = (int *)fm_alloc(tcap * sizeof(int));
  for (u32 i = 0; i < tcap; i++) table[i] = -1;
  for (int i = 0; i < n; i++) {
    const FmArcEntry *ae = arc_entry(l->arc, i);
    if (!ae || !ae->path) continue;
    const char *p = ae->path;
    if (il && strncmp(p, inner, il) != 0) continue;
    const char *rest = p + il;
    while (*rest == '/') rest++;
    if (!*rest) continue;
    const char *slash = strchr(rest, '/');
    size_t nl = slash ? (size_t)(slash - rest) : strlen(rest);
    bool implied = slash && slash[1] != 0;
    bool is_dir = slash != NULL || ae->is_dir;
    if (rest[0] == '.' && !show_hidden) continue;
    if (nl == 0 || (nl == 1 && rest[0] == '.') || (nl == 2 && rest[0] == '.' && rest[1] == '.'))
      continue;
    u32 h = name_hash(rest, nl) & (tcap - 1);
    int found = -1;
    while (table[h] >= 0) {
      FmEntry *o = &l->items[table[h]];
      if (strlen(o->name) == nl && memcmp(o->name, rest, nl) == 0) { found = table[h]; break; }
      h = (h + 1) & (tcap - 1);
    }
    if (found >= 0) {
      FmEntry *o = &l->items[found];
      if (!implied && o->arc_index < 0) {       /* the explicit entry of an implied folder */
        o->arc_index = i;
        o->mtime = ae->mtime;
      }
      continue;
    }
    if (l->count >= (int)(tcap / 2)) continue;   /* cannot happen: tcap >= 2n */
    table[h] = l->count;
    FmEntry *e = push(l);
    e->name = arena_strndup(&l->arena, rest, nl);
    e->flags = is_dir ? FM_ST_DIR : 0;
    if (rest[0] == '.') e->flags |= FM_ST_HIDDEN;
    if (ae->is_link) e->flags |= FM_ST_LINK;
    e->type = (u16)(is_dir ? FT_DIR : fm_type_from_name(e->name));
    e->arc_index = implied ? -1 : i;
    e->mtime = implied ? 0 : ae->mtime;
    if (!is_dir) {
      e->size = ae->size;
      e->encrypted = ae->encrypted;
      l->total_size += ae->size;
    }
  }
  fm_free(table);
  return FM_OK;
}

FmErr vfs_list(FmListing *l, const FmLoc *loc, bool show_hidden) {
  FmLoc want = *loc;
  if (l->arc && !(want.in_arc && same_path(l->loc.path, want.path))) {
    arc_close(l->arc);
    l->arc = NULL;
  }
  if (l->arena.block_size == 0) arena_init(&l->arena, 64 * 1024);
  else arena_reset(&l->arena);
  l->loc = want;
  l->count = 0;
  l->total_size = 0;
  l->err = want.in_arc ? list_arc(l, show_hidden) : list_local(l, show_hidden);
  if (l->err != FM_OK) l->count = 0;
  return l->err;
}

void vfs_free(FmListing *l) {
  if (l->arc) arc_close(l->arc);
  arena_free(&l->arena);
  fm_free(l->items);
  memset(l, 0, sizeof *l);
}

bool vfs_entry_path(const FmListing *l, const FmEntry *e, char *out, size_t cap) {
  if (l->loc.in_arc) return false;
  return fm_path_join(out, cap, l->loc.path, e->name);
}

/* ---- materializing archive entries -------------------------------------- */

bool vfs_cache_path(const char *arc, const FmArcEntry *e, char *out, size_t cap) {
  u64 h = 1469598103934665603ull;
  const char *parts[2] = { arc, e->path };
  for (int k = 0; k < 2; k++) {
    for (const char *s = parts[k]; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    h ^= 0xFF;
    h *= 1099511628211ull;
  }
  h ^= e->size;
  h *= 1099511628211ull;
  h ^= (u64)e->mtime;
  h *= 1099511628211ull;
  char base[FM_PATH_MAX], sub[48];
  if (!plat_place(PLACE_CACHE, base, sizeof base)) return false;
  fm_snprintf(sub, sizeof sub, "view%c%08x%08x", FM_SEP, (unsigned)(h >> 32), (unsigned)h);
  if (!fm_path_join(base, sizeof base, base, sub)) return false;
  /* the entry's own name, made safe for every file system */
  char name[256];
  fm_strlcpy(name, fm_path_base(e->path), sizeof name);
  size_t n = strlen(name);
  while (n && name[n - 1] == '/') name[--n] = 0;
  for (char *s = name; *s; s++)
    if ((u8)*s < 0x20 || strchr("/\\:*?\"<>|", *s)) *s = '_';
  if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) fm_strlcpy(name, "file", sizeof name);
  return fm_path_join(out, cap, base, name);
}

FmErr vfs_materialize(FmListing *l, const FmEntry *e, char *out, size_t cap) {
  if (!l->arc || e->arc_index < 0) return FM_ERR_NOT_FOUND;
  const FmArcEntry *ae = arc_entry(l->arc, e->arc_index);
  if (!ae || ae->is_dir) return FM_ERR_NOT_FOUND;
  if (!vfs_cache_path(l->loc.path, ae, out, cap)) return FM_ERR_IO;
  FmStat st;
  if (plat_stat(out, &st) && !(st.flags & FM_ST_DIR) && st.size == ae->size) return FM_OK;
  char dir[FM_PATH_MAX];
  fm_strlcpy(dir, out, sizeof dir);
  fm_path_parent(dir);
  FmErr e2 = plat_mkdirs(dir);
  if (e2 != FM_OK) return e2;
  FmArcCb cb = arc_cb(l->loc.path);
  e2 = arc_extract_one(l->arc, e->arc_index, out, &cb);
  if (e2 != FM_OK) plat_remove_file(out);
  return e2;
}
