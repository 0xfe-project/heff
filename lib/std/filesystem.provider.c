// The disk's side of std.filesystem, for the C backend: an effect
// provider.
//
// Whole files, directories and paths are done there and then.  An open
// file is a `FsFile`, the host's own, which the program holds as a
// foreign value (held by a `with`): closed by `CloseFile` when the
// `with` ends, and let go of -- closed, unless the `with` closed it --
// where the program drops its last reference to it.  It reads ahead
// into a buffer of its own, from which lines and chunks are taken.
//
// A read or a write of an open file is a `FsOp`, done in steps as
// std.net's are: where a scheduler of
// std.reactor's runs, a pipe, a terminal or a socket -- what poll can
// say is ready -- is polled first, and the op waits in the runtime's
// reactor until it
// is, the other green threads going on.  The descriptor is left as it
// is, never made non-blocking (a standard stream's flags are its
// parent's too): a read is done once poll says there is something to
// read, and a write of no more than PIPE_BUF bytes once it says there
// is room.  A regular file is always ready, as poll sees it, and is read
// and written there and then, the whole program waiting for the disk.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
// (no links on Windows as stat sees them, and mkdir takes no mode)
#define lstat stat
#define S_ISLNK(m) 0
#define heff_mkdir(p) mkdir(p)
#define heff_is_sep(c) ((c) == '/' || (c) == '\\')
#else
#include <limits.h>
#define heff_mkdir(p) mkdir(p, 0777)
#define heff_is_sep(c) ((c) == '/')
#endif

// Why an operation failed
// ---------------------------------------------------------------------
//
// A `FileError` names the path the failing call was given -- in a tree,
// the one inside it -- as the program wrote it: the system is given a
// path from this thread's directory (`heff_path_in_dir`), and a path it
// was given under one the program wrote is said from that one again.

static uint32_t fs_error_shape[5] = { LBL_code, LBL_name, LBL_message, LBL_path, LBL_other };

// (a `FileError` of errno `err`, of the path `path` and the other,
// `other` -- 0, none -- both taken)
static V fs_error(int err, V path, V other) {
  const char *name = heff_errno_name(err);
  char number[24];
  if (name[1] == '?') {
    snprintf(number, sizeof number, "errno %d", err);
    name = number;
  }
  V f[5] = { heff_int(err), heff_str_text(name), heff_str_text(strerror(err)), path,
             other ? heff_just(other) : heff_nothing };
  return heff_rec(5, fs_error_shape, f);
}

static V fs_right(V x) { return heff_tag_n(TAG_Right, 1, &x); }

// (`Right MkUnit`)
static V fs_done(void) { return fs_right(NTAG(TAG_MkUnit)); }

// (`Left` a `FileError` of errno `err`, of the path the C string `path`)
static V fs_left_at(int err, const char *path) {
  V x = fs_error(err, heff_str_bytes(path, (int64_t)strlen(path)), 0);
  return heff_tag_n(TAG_Left, 1, &x);
}

// (`Left` EINVAL, of a path that is none, a string with a NUL in it:
// `s`, not taken)
static V fs_invalid(V s) {
  V x = fs_error(EINVAL, heff_dup(s), 0);
  return heff_tag_n(TAG_Left, 1, &x);
}

// (a path as the program wrote it, `given`, and as the system is given
// it, `full`: both malloc'd, NULL where it has a NUL in it)
typedef struct {
  char *given;
  char *full;
} FsPath;

// (the path `s` as the system is to be given it: whether it is one)
static int fs_path(FsPath *p, V s) {
  p->given = heff_cstr(s);
  p->full = p->given ? heff_path_in_dir(heff_strdup(p->given)) : NULL;
  return p->full != NULL;
}

static void fs_path_free(FsPath *p) {
  free(p->given);
  free(p->full);
}

// (`n` bytes at `a` and the C string `b`, as a string)
static V fs_joined(const char *a, size_t n, const char *b) {
  size_t m = strlen(b);
  char *s = heff_alloc(n + m + 1);
  memcpy(s, a, n);
  memcpy(s + n, b, m + 1);
  V v = heff_str_bytes(s, (int64_t)(n + m));
  free(s);
  return v;
}

// (the path `at`, one the system was given, as the program wrote it:
// one of the paths `ps` or under it, from that one as written; one above
// it, where what is between is written the same; else as it is)
static V fs_named(const char *at, const FsPath *ps, int n) {
  size_t k = strlen(at);
  for (int i = 0; i < n; i++) {
    const char *full = ps[i].full, *given = ps[i].given;
    if (!full) continue;
    size_t f = strlen(full), g = strlen(given);
    if (k >= f && strncmp(at, full, f) == 0 && (at[f] == 0 || heff_is_sep(at[f]))) {
      // (under it: `given`, then what follows)
      const char *rest = at + f;
      if (*rest && g > 0 && heff_is_sep(given[g - 1])) rest++;
      return fs_joined(given, g, rest);
    }
    if (k < f && strncmp(at, full, k) == 0 && heff_is_sep(full[k])) {
      // (above it: `given`, less what follows `at` in `full`)
      size_t tail = f - k;
      if (g > tail && strcmp(given + g - tail, full + k) == 0) return fs_joined(given, g - tail, "");
    }
  }
  return heff_str_bytes(at, (int64_t)k);
}

// (why an operation failed, as it is found: the errno, the path the
// failing call was given and, where it took two, the other -- the
// system's, malloc'd)
typedef struct {
  int err;
  char *at, *other;
} FsWhy;

// (a failure of the call given `at`, and `other` (NULL, none), with the
// errno `err` -- 0: errno's, or EIO where it left none -- kept, unless
// one was before, the innermost of a tree's: 0)
static int fs_fail2(FsWhy *w, const char *at, const char *other, int err) {
  if (w->at) return 0;
  w->err = err ? err : errno ? errno : EIO;
  w->at = heff_strdup(at);
  w->other = other ? heff_strdup(other) : NULL;
  return 0;
}

// (a failure of the call given `at`, errno's: 0)
static int fs_fail(FsWhy *w, const char *at) { return fs_fail2(w, at, NULL, 0); }

// (`Left` why, its paths as the program wrote them, of the paths `ps`:
// `w` let go of)
static V fs_failed(FsWhy *w, const FsPath *ps, int n) {
  V at = fs_named(w->at ? w->at : "", ps, n);
  V other = w->other ? fs_named(w->other, ps, n) : 0;
  V x = fs_error(w->err ? w->err : EIO, at, other);
  free(w->at);
  free(w->other);
  w->at = w->other = NULL;
  return heff_tag_n(TAG_Left, 1, &x);
}

// Whole files, directories and trees
// ---------------------------------------------------------------------
//
// Each answers whether it was done; where it was not, why is in its
// `FsWhy`.

// (a file's text, at `*out`)
static int heff_read_file(const char *path, V *out, FsWhy *w) {
  errno = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return fs_fail(w, path);
  // (a regular file read straight into the string, as long as it says)
  struct stat st;
  int64_t size =
    fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) ? (int64_t)st.st_size : 0;
  *out = heff_str_read(f, size);
  if (!*out) fs_fail(w, path);
  fclose(f);
  return *out != 0;
}

static int heff_write_file(const char *path, V s, FsWhy *w) {
  errno = 0;
  FILE *f = fopen(path, "wb");
  if (!f) return fs_fail(w, path);
  heff_put(f, s);
  return fclose(f) == 0 || fs_fail(w, path);
}

// (a directory is at `path`, links followed)
static int heff_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// (a directory and those it is in: whether a directory is there -- a
// file there is none, EEXIST)
static int heff_mkdir_p(char *path, FsWhy *w) {
  for (char *c = path + 1; *c; c++)
    if (heff_is_sep(*c)) {
      char sep = *c;
      *c = 0;
      // (a drive, `C:`, is there already)
      int drive = c[-1] == ':';
      int ok = drive || heff_mkdir(path) == 0 || errno == EEXIST;
      if (!ok) fs_fail(w, path);
      *c = sep;
      if (!ok) return 0;
    }
  errno = 0;
  if (heff_mkdir(path) == 0) return 1;
  int e = errno;
  if (e == EEXIST && heff_is_dir(path)) return 1;
  return fs_fail2(w, path, NULL, e);
}

// (the names in a directory, sorted, at `*out`)
static int heff_read_dir(const char *path, V *out, FsWhy *w) {
  errno = 0;
  DIR *d = opendir(path);
  if (!d) return fs_fail(w, path);
  int64_t n = 0, cap = 64;
  char **ns = heff_alloc(cap * sizeof(char *));
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (n == cap) {
      char **n2 = heff_alloc(cap * 2 * sizeof(char *));
      memcpy(n2, ns, n * sizeof(char *));
      free(ns);
      ns = n2;
      cap *= 2;
    }
    ns[n++] = strdup(e->d_name);
  }
  closedir(d);
  qsort(ns, n, sizeof(char *), heff_cmp_cstr);
  *out = heff_strs_bytes(ns, n);
  for (int64_t i = 0; i < n; i++) free(ns[i]);
  free(ns);
  return 1;
}

// (every file and directory under `p`, its mode's write bits off, or on:
// a directory is made writable before what it holds, read-only after.
// -1 where nothing is at `p`)
static int heff_chmod_all(const char *p, int writable, FsWhy *w) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT ? -1 : fs_fail(w, p);
  if (S_ISLNK(st.st_mode)) return 1;
  mode_t mode = writable ? (st.st_mode | 0200) : (st.st_mode & ~0222);
  mode &= 07777;
  if (S_ISDIR(st.st_mode)) {
    if (writable && chmod(p, mode) != 0) return fs_fail(w, p);
    DIR *d = opendir(p);
    if (!d) return fs_fail(w, p);
    struct dirent *e;
    int ok = 1;
    while (ok && (e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char *q = heff_join(p, e->d_name);
      ok = heff_chmod_all(q, writable, w) != 0;
      free(q);
    }
    closedir(d);
    if (!ok) return 0;
    if (!writable && chmod(p, mode) != 0) return fs_fail(w, p);
    return 1;
  }
  return chmod(p, mode) == 0 || fs_fail(w, p);
}

// (a file, or a directory and all it holds; nothing there is no failure)
static int heff_remove_all(const char *p, FsWhy *w) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT || fs_fail(w, p);
  if (!S_ISDIR(st.st_mode)) return unlink(p) == 0 || fs_fail(w, p);
  DIR *d = opendir(p);
  if (!d) return fs_fail(w, p);
  struct dirent *e;
  int ok = 1;
  while (ok && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *q = heff_join(p, e->d_name);
    ok = heff_remove_all(q, w);
    free(q);
  }
  closedir(d);
  return ok && (rmdir(p) == 0 || fs_fail(w, p));
}

// (a file's bytes copied to `q`, which is given `mode`)
static int heff_copy_file(const char *p, const char *q, mode_t mode, FsWhy *w) {
  FILE *f = fopen(p, "rb");
  if (!f) return fs_fail(w, p);
  FILE *g = fopen(q, "wb");
  if (!g) {
    int e = errno;
    fclose(f);
    return fs_fail2(w, q, NULL, e);
  }
  char *chunk = heff_alloc(65536);
  size_t got;
  const char *bad = NULL;
  int e = 0;
  while (!bad && (got = fread(chunk, 1, 65536, f)) > 0)
    if (fwrite(chunk, 1, got, g) != got) {
      e = errno;
      bad = q;
    }
  if (!bad && ferror(f)) {
    e = errno;
    bad = p;
  }
  free(chunk);
  fclose(f);
  if (fclose(g) != 0 && !bad) {
    e = errno;
    bad = q;
  }
  if (!bad && chmod(q, mode) != 0) {
    e = errno;
    bad = q;
  }
  return bad ? fs_fail2(w, bad, NULL, e) : 1;
}

// (a file, or a directory and all it holds, copied to `q`: links
// followed, modes kept -- a directory's once what it holds is in it)
static int heff_copy_all(const char *p, const char *q, FsWhy *w) {
  struct stat st;
  if (stat(p, &st) != 0) return fs_fail(w, p);
  mode_t mode = st.st_mode & 07777;
  if (!S_ISDIR(st.st_mode)) return heff_copy_file(p, q, mode, w);
  if (heff_mkdir(q) != 0) return fs_fail(w, q);
  DIR *d = opendir(p);
  if (!d) return fs_fail(w, p);
  struct dirent *e;
  int ok = 1;
  while (ok && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *a = heff_join(p, e->d_name);
    char *b = heff_join(q, e->d_name);
    ok = heff_copy_all(a, b, w);
    free(a);
    free(b);
  }
  closedir(d);
  return ok && (chmod(q, mode) == 0 || fs_fail(w, q));
}

// (whether something is at `p`, a file, a directory or a link: 1; 0,
// nothing; -1, the system cannot say, why in `w`)
static int heff_something_at(const char *p, FsWhy *w) {
  struct stat st;
  if (lstat(p, &st) == 0) return 1;
  if (errno == ENOENT) return 0;
  fs_fail(w, p);
  return -1;
}

// (a move or a rename's: `to` free, why not in `w`)
static int heff_free_at(const char *to, FsWhy *w) {
  int there = heff_something_at(to, w);
  if (there > 0) fs_fail2(w, to, NULL, EEXIST);
  return there == 0;
}

// (an operation of whole files and paths: answered by an `Either
// FileError a`)
static V heff_fs_whole(V p, V k) {
  int tag = heff_tag_of(p);
  int two = tag == TAG_Copy || tag == TAG_Move || tag == TAG_Replace;
  V f = heff_dup(heff_field(p, tag == TAG_WriteFile || two ? 2 : 1));
  FsPath ps[2] = { { NULL, NULL }, { NULL, NULL } };
  FsWhy why = { 0, NULL, NULL };
  V r = 0;
  if (!fs_path(&ps[0], heff_field(p, 0))) r = fs_invalid(heff_field(p, 0));
  else if (two && !fs_path(&ps[1], heff_field(p, 1))) r = fs_invalid(heff_field(p, 1));
  else {
    char *path = ps[0].full, *to = ps[1].full;
    V got;
    switch (tag) {
    case TAG_ReadFile:
      if (heff_read_file(path, &got, &why)) r = fs_right(got);
      break;
    case TAG_WriteFile:
      if (heff_write_file(path, heff_field(p, 1), &why)) r = fs_done();
      break;
    case TAG_MakeDirectory:
      if (heff_mkdir_p(path, &why)) r = fs_done();
      break;
    case TAG_ReadDirectory:
      if (heff_read_dir(path, &got, &why)) r = fs_right(got);
      break;
    case TAG_Remove:
      errno = 0;
      if (heff_chmod_all(path, 1, &why) != 0 && heff_remove_all(path, &why)) r = fs_done();
      break;
    case TAG_ReadOnly:
    case TAG_Writable: {
      errno = 0;
      int c = heff_chmod_all(path, tag == TAG_Writable, &why);
      if (c == 1) r = fs_done();
      else if (c == -1) fs_fail2(&why, path, NULL, ENOENT);
      break;
    }
    case TAG_Copy:
      errno = 0;
      if (heff_free_at(to, &why) && heff_copy_all(path, to, &why)) r = fs_done();
      break;
    case TAG_Move:
      if (heff_free_at(to, &why)) {
        errno = 0;
        if (rename(path, to) == 0) r = fs_done();
        else fs_fail2(&why, path, to, 0);
      }
      break;
    case TAG_Replace:
      // (rename(2) puts a file in place of another at once; Windows' does
      // not over one there, MoveFileEx does)
      errno = 0;
#ifdef _WIN32
      if (MoveFileExA(path, to, MOVEFILE_REPLACE_EXISTING) != 0) r = fs_done();
#else
      if (rename(path, to) == 0) r = fs_done();
#endif
      else fs_fail2(&why, path, to, 0);
      break;
    default:
      heff_fail("not an operation of filesystem");
      return 0;
    }
    if (!r) r = fs_failed(&why, ps, two ? 2 : 1);
  }
  heff_drop(p);
  fs_path_free(&ps[0]);
  fs_path_free(&ps[1]);
  V x = heff_answer(f, r);
  return heff_tail(k, 1, &x);
}

// What a file is
// ---------------------------------------------------------------------

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifdef _WIN32
// (nothing is polled on Windows: an op is done there and then)
#ifndef POLLIN
#define POLLIN 1
#define POLLOUT 4
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#else
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif

static uint32_t fs_status_shape[4] = { LBL_kind, LBL_size, LBL_modified, LBL_mode };

// (`Right` what the system says of a file, stat's `st`)
static V fs_status(const struct stat *st) {
  int kind = S_ISREG(st->st_mode) ? TAG_Regular
           : S_ISDIR(st->st_mode) ? TAG_Directory
           : S_ISLNK(st->st_mode) ? TAG_Link
           : TAG_Other;
#if defined(__APPLE__)
  int64_t s = (int64_t)st->st_mtimespec.tv_sec, ns = (int64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
  int64_t s = (int64_t)st->st_mtime, ns = 0;
#else
  int64_t s = (int64_t)st->st_mtim.tv_sec, ns = (int64_t)st->st_mtim.tv_nsec;
#endif
  V f[4] = { NTAG(kind), heff_int((int64_t)st->st_size), heff_int(s * 1000000000 + ns),
             heff_int((int64_t)(st->st_mode & 07777)) };
  return fs_right(heff_rec(4, fs_status_shape, f));
}

// (a number given as an `Int`, at `*n`: whether it is one of a word)
static int fs_int(V v, int64_t *n) {
  if (!IS_INT(v)) return 0;
  *n = INT_OF(v);
  return 1;
}

// Open files
// ---------------------------------------------------------------------

// (this provider's open files, told from what other providers hand out)
#define FS_MAGIC 0x66696c65u
// (how much is read ahead at a time; a chunk's most)
#define FS_BUF ((size_t)65536)
#define FS_READ_MAX ((int64_t)16 << 20)

typedef struct {
  uint32_t magic;
  int fd;            // (-1 once closed)
  int refs;          // (each foreign value's that is it, each op's that
                     //  has it; and a standard stream's own)
  int standard;      // (a standard stream's number, 0 to 2: never closed;
                     //  else -1)
  int pollable;      // (a pipe, a terminal, a socket: polled, where a
                     //  scheduler runs)
  int seekable;      // (a regular file: what was read ahead is given back
                     //  before a write)
  uint8_t *buf;      // (read ahead: what is not yet taken, from `at` to
  size_t cap, at, end;  //  `end`)
  uint64_t taken;    // (how many times something was taken from it)
  int port;          // (a port of its own on standard stream `port - 1`:
                     //  what that one read ahead given to it, and back
                     //  as it is closed; 0 for none)
  char *path;        // (the path it was opened by, as the program wrote
                     //  it, a failure's; a standard stream's, `<stdin>`)
} FsFile;

// (the standard streams' files, once asked for: one each, kept, so
// that what one `with` read ahead is the next one's too)
static FsFile *fs_standard[3];

static void fs_unref(FsFile *h) {
  if (--h->refs > 0) return;
  free(h->buf);
  free(h->path);
  h->magic = 0;
  free(h);
}

// (the program's last reference to it dropped: closed, unless the
// `with` closed it or it is a standard stream, which stays open)
static void fs_let_go(void *x) {
  FsFile *h = x;
  if (h->standard < 0 && h->fd >= 0) {
    close(h->fd);
    h->fd = -1;
  }
  fs_unref(h);
}

// (the file of `fd`, opened by `path`, taken)
static FsFile *fs_new(int fd, int standard, char *path) {
  FsFile *h = heff_alloc(sizeof *h);
  memset(h, 0, sizeof *h);
  h->magic = FS_MAGIC;
  h->fd = fd;
  h->path = path;
  h->refs = standard >= 0;  // (a standard stream's own: kept)
  h->standard = standard;
  struct stat st;
  if (fstat(fd, &st) == 0) {
    h->seekable = S_ISREG(st.st_mode);
#ifndef _WIN32
    h->pollable = S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISSOCK(st.st_mode);
#endif
  }
  return h;
}

// (the file as the program holds it: a foreign value, counted by `h`)
static V fs_value(FsFile *h) {
  h->refs++;
  return heff_foreign(h, fs_let_go);
}

// (the file a foreign value is, or the program failed: it is none)
static FsFile *fs_file(V v) {
  FsFile *h = heff_foreign_ptr(v);
  if (h->magic != FS_MAGIC) heff_fail("std.filesystem: no open file");
  return h;
}

static size_t fs_buffered(FsFile *h) { return h->end - h->at; }

// (`Left` why an operation of the file failed, errno `err`)
static V fs_left_file(FsFile *h, int err) { return fs_left_at(err, h->path); }

// (room for `n` more bytes after what is read ahead: what is taken let
// go of, the buffer grown)
static void fs_room(FsFile *h, size_t n) {
  if (h->at == h->end) h->at = h->end = 0;
  if (h->cap - h->end >= n) return;
  if (h->at > 0) {
    memmove(h->buf, h->buf + h->at, h->end - h->at);
    h->end -= h->at;
    h->at = 0;
  }
  if (h->cap - h->end >= n) return;
  size_t cap = h->cap ? h->cap : FS_BUF;
  while (cap - h->end < n) cap *= 2;
  uint8_t *b = heff_alloc(cap);
  if (h->end) memcpy(b, h->buf, h->end);
  free(h->buf);
  h->buf = b;
  h->cap = cap;
}

// (what was read ahead given back, where the file is a regular one, so
// that a write is where the program has read up to: 0, or -1 with
// errno)
static int fs_unread(FsFile *h) {
  size_t n = fs_buffered(h);
  if (!n || !h->seekable) return 0;
#ifdef _WIN32
  if (_lseeki64(h->fd, -(int64_t)n, SEEK_CUR) < 0) return -1;
#else
  if (lseek(h->fd, -(off_t)n, SEEK_CUR) < 0) return -1;
#endif
  h->at = h->end = 0;
  h->taken++;
  return 0;
}

// (whether `fd` is ready for `events` now, not waiting: 1, 0, or -1
// with errno)
static int fs_ready(int fd, short events) {
#ifdef _WIN32
  (void)fd;
  (void)events;
  return 1;
#else
  struct pollfd p = { fd, events, 0 };
  int r;
  do r = poll(&p, 1, 0); while (r < 0 && errno == EINTR);
  return r < 0 ? -1 : r > 0;
#endif
}

// (`fd` waited for here, the whole program waiting: one another made
// non-blocking, not ready)
static void fs_wait_here(int fd, short events) {
#ifdef _WIN32
  (void)fd;
  (void)events;
#else
  struct pollfd p = { fd, events, 0 };
  while (poll(&p, 1, -1) < 0 && errno == EINTR) {}
#endif
}

// An operation, in steps
// ---------------------------------------------------------------------

enum { FS_CHUNK, FS_LINE, FS_WRITE };

typedef struct {
  HeffWait w;        // (first: an op is its wait; its `fd` the file's)
  int what;
  FsFile *h;         // (counted: the op's while it is)
  int err;           // (errno, where it failed)
  int async;         // (polled before each read and write, waiting in the
                     //  reactor until it is ready: a scheduler runs, and
                     //  the file is pollable)
  int eof;           // (a read came to the end)
  int64_t n;         // (a chunk's most)
  size_t scanned;    // (a line's: how far after `h->at` no newline is,
  uint64_t taken;    //  while `h->taken` is this)
  const uint8_t *data;  // (a write's bytes, `len` of them, `done` written)
  int64_t len, done;
  V held;            // (the operation, which holds the file, and a
                     //  write's bytes)
  char *owned;       // (or a copy of them, a string's)
  V result;          // (its answer, once it is done and did not fail)
} FsOp;

static FsOp *fs_op(int what, FsFile *h) {
  FsOp *o = heff_alloc(sizeof *o);
  memset(o, 0, sizeof *o);
  o->what = what;
  o->h = h;
  h->refs++;
  o->w.fd = h->fd;
  o->w.events = POLLIN;
#ifndef _WIN32
  o->async = heff_group != 0 && h->pollable;
#endif
  o->taken = h->taken;
  return o;
}

static void fs_op_free(FsOp *o) {
  if (o->result) heff_drop(o->result);
  if (o->held) heff_drop(o->held);
  free(o->owned);
  fs_unref(o->h);
  free(o);
}

// (up to `n` bytes read into `dst`: how many, 0 at the end, or -1 with
// errno -- EAGAIN, for an async op, where it is not ready)
static ssize_t fs_read_into(FsOp *o, uint8_t *dst, size_t n) {
  int fd = o->h->fd;
  for (;;) {
    if (o->async) {
      int r = fs_ready(fd, POLLIN);
      if (r < 0) return -1;
      if (r == 0) {
        errno = EAGAIN;
        return -1;
      }
    }
#ifdef _WIN32
    ssize_t r = read(fd, dst, (unsigned)n);
#else
    ssize_t r = read(fd, dst, n);
#endif
    if (r >= 0) return r;
    if (errno == EINTR) continue;
    if ((errno == EAGAIN || errno == EWOULDBLOCK) && !o->async) {
      fs_wait_here(fd, POLLIN);
      continue;
    }
    if (errno == EWOULDBLOCK) errno = EAGAIN;
    return -1;
  }
}

// (more read ahead, as much as there is room for, a buffer's at least:
// as `fs_read_into`)
static ssize_t fs_fill(FsOp *o) {
  FsFile *h = o->h;
  fs_room(h, FS_BUF);
  ssize_t r = fs_read_into(o, h->buf + h->end, h->cap - h->end);
  if (r > 0) h->end += (size_t)r;
  return r;
}

// (`n` bytes taken from what was read ahead)
static V fs_take(FsFile *h, size_t n) {
  V b = heff_bytes_in(h->buf + h->at, (int64_t)n);
  h->at += n;
  h->taken++;
  return b;
}

// (a step's end where a read failed: waiting, where it is not ready)
static int fs_failed_step(FsOp *o) {
  if (o->async && errno == EAGAIN) {
    o->w.events = POLLIN;
    return HEFF_STEP_WAIT;
  }
  o->err = errno;
  return HEFF_STEP_DONE;
}

// A chunk: what was read ahead, or read now -- straight into bytes of
// its own where as many as a buffer's are asked for.
static int fs_chunk_step(HeffWait *w) {
  FsOp *o = (FsOp *)w;
  FsFile *h = o->h;
  if (fs_buffered(h) == 0 && (size_t)o->n >= FS_BUF) {
    uint8_t *mine = heff_alloc((size_t)o->n);
    ssize_t r = fs_read_into(o, mine, (size_t)o->n);
    if (r < 0) {
      int e = errno;
      free(mine);
      errno = e;
      return fs_failed_step(o);
    }
    o->result = fs_right(heff_bytes_in(mine, (int64_t)r));
    free(mine);
    return HEFF_STEP_DONE;
  }
  if (fs_buffered(h) == 0 && fs_fill(o) < 0) return fs_failed_step(o);
  size_t n = fs_buffered(h);
  if (n > (size_t)o->n) n = (size_t)o->n;
  o->result = fs_right(n ? fs_take(h, n) : heff_bytes_in("", 0));
  return HEFF_STEP_DONE;
}

// (`Right (Just` the line of `n` bytes from what was read ahead`)`, its
// newline -- `nl` bytes, and a `\r` before it -- taken, left out)
static V fs_take_line(FsFile *h, size_t n, size_t nl) {
  const uint8_t *s = h->buf + h->at;
  size_t len = n;
  if (nl && len > 0 && s[len - 1] == '\r') len--;
  V line = heff_str_bytes(s, (int64_t)len);
  h->at += n + nl;
  h->taken++;
  return fs_right(heff_just(line));
}

// A line: what was read ahead looked through for a newline, more read
// until there is one or the file has ended.
static int fs_line_step(HeffWait *w) {
  FsOp *o = (FsOp *)w;
  FsFile *h = o->h;
  for (;;) {
    if (o->taken != h->taken) {
      // (another op took from it meanwhile: looked through anew)
      o->scanned = 0;
      o->taken = h->taken;
    }
    size_t have = fs_buffered(h);
    if (have > o->scanned) {
      const uint8_t *s = h->buf + h->at;
      const uint8_t *nl = memchr(s + o->scanned, '\n', have - o->scanned);
      if (nl) {
        o->result = fs_take_line(h, (size_t)(nl - s), 1);
        return HEFF_STEP_DONE;
      }
    }
    o->scanned = have;
    if (o->eof) {
      o->result = have ? fs_take_line(h, have, 0) : fs_right(heff_nothing);
      return HEFF_STEP_DONE;
    }
    ssize_t r = fs_fill(o);
    if (r < 0) return fs_failed_step(o);
    if (r == 0) o->eof = 1;
  }
}

// A write: all of it -- where it is polled, no more than PIPE_BUF bytes
// at a time, each once poll says there is room.
static int fs_write_step(HeffWait *w) {
  FsOp *o = (FsOp *)w;
  int fd = o->h->fd;
  while (o->done < o->len) {
    size_t n = (size_t)(o->len - o->done);
#ifndef _WIN32
    if (o->async) {
      int r = fs_ready(fd, POLLOUT);
      if (r < 0) {
        o->err = errno;
        return HEFF_STEP_DONE;
      }
      if (r == 0) {
        w->events = POLLOUT;
        return HEFF_STEP_WAIT;
      }
      if (n > PIPE_BUF) n = PIPE_BUF;
    }
    ssize_t r = write(fd, o->data + o->done, n);
#else
    ssize_t r = write(fd, o->data + o->done, (unsigned)n);
#endif
    if (r >= 0) {
      o->done += r;
      continue;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (o->async) {
        w->events = POLLOUT;
        return HEFF_STEP_WAIT;
      }
      fs_wait_here(fd, POLLOUT);
      continue;
    }
    o->err = errno;
    return HEFF_STEP_DONE;
  }
  o->result = fs_done();
  return HEFF_STEP_DONE;
}

static V fs_answer(HeffWait *w) {
  FsOp *o = (FsOp *)w;
  // (why waiting failed, where it did -- its file closed meanwhile,
  // EBADF -- or why the op did)
  int err = w->err ? w->err : o->err;
  V r;
  if (err) r = fs_left_file(o->h, err);
  else {
    r = o->result;
    o->result = 0;
  }
  fs_op_free(o);
  return r;
}

static void fs_cancel(HeffWait *w) { fs_op_free((FsOp *)w); }

// (the op, its first step taken: done, its answer given to `k`; or
// waited for by the reactor -- `heff_outside`)
static V fs_go(FsOp *o, int (*step)(HeffWait *), V f, V k) {
  o->w.step = step;
  o->w.answer = fs_answer;
  o->w.cancel = fs_cancel;
  if (step(&o->w) == HEFF_STEP_WAIT) return heff_outside(&o->w, f, k);
  V x = heff_answer(f, fs_answer(&o->w));
  return heff_tail(k, 1, &x);
}

// (`len` bytes at `data` written to `h`: held by `held`, or `owned`)
static V fs_write(FsFile *h, const uint8_t *data, int64_t len, V held, char *owned, V f, V k) {
  // (what std.io wrote before it, first; and what was read ahead given
  // back)
  if (h->standard == 1 || h->standard == 2 || h->port == 2 || h->port == 3) fflush(stdout);
  if (h->standard == 2 || h->port == 3) fflush(stderr);
  if (fs_unread(h) < 0) {
    int e = errno;
    if (held) heff_drop(held);
    free(owned);
    V x = heff_answer(f, fs_left_file(h, e));
    return heff_tail(k, 1, &x);
  }
  FsOp *o = fs_op(FS_WRITE, h);
  o->w.events = POLLOUT;
  o->data = data;
  o->len = len;
  o->held = held;
  o->owned = owned;
  return fs_go(o, fs_write_step, f, k);
}

// Paths
// ---------------------------------------------------------------------

// (where the system keeps temporary files, no separator after it)
static void fs_temp_root(char *buf, size_t n) {
#ifdef _WIN32
  DWORD m = GetTempPathA((DWORD)n, buf);
  if (m == 0 || m >= n) snprintf(buf, n, ".");
#else
  const char *t = getenv("TMPDIR");
  snprintf(buf, n, "%s", t && *t ? t : "/tmp");
#endif
  size_t len = strlen(buf);
  while (len > 1 && heff_is_sep(buf[len - 1])) buf[--len] = 0;
}

static uint64_t fs_seed;

// (six letters, a temporary name's, hardly ever the same twice)
static void fs_letters(char *out) {
  uint64_t x = fs_seed ^ ((uint64_t)time(NULL) * 0x9e3779b97f4a7c15ULL) ^ (uint64_t)clock()
             ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)out;
#ifndef _WIN32
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  x ^= (uint64_t)ts.tv_nsec * 0xbf58476d1ce4e5b9ULL;
#endif
  fs_seed = x * 6364136223846793005ULL + 1442695040888963407ULL;
  uint64_t y = fs_seed;
  y = (y ^ (y >> 30)) * 0xbf58476d1ce4e5b9ULL;
  y = (y ^ (y >> 27)) * 0x94d049bb133111ebULL;
  y ^= y >> 31;
  static const char abc[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < 6; i++) {
    out[i] = abc[y % 36];
    y /= 36;
  }
  out[6] = 0;
}

// (a new temporary directory, or file -- made, empty, closed -- its name
// `prefix` and six letters: its path)
static V fs_temp(V prefix, int file) {
  char *pre = heff_cstr(prefix);
  if (!pre) return fs_invalid(prefix);
  char root[4096];
  fs_temp_root(root, sizeof root);
  size_t n = strlen(root) + strlen(pre) + 9;
  char *path = heff_alloc(n);
  V r = 0;
  for (int tries = 0; tries < 100 && !r; tries++) {
    char six[7];
    fs_letters(six);
#ifdef _WIN32
    snprintf(path, n, "%s\\%s%s", root, pre, six);
#else
    snprintf(path, n, "%s/%s%s", root, pre, six);
#endif
    if (!file) {
#ifdef _WIN32
      int made = mkdir(path) == 0;
#else
      int made = mkdir(path, 0700) == 0;
#endif
      if (made) r = fs_right(heff_str_bytes(path, (int64_t)strlen(path)));
      else if (errno != EEXIST) r = fs_left_at(errno, path);
    } else {
      int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_BINARY, 0600);
      if (fd >= 0) {
        close(fd);
        r = fs_right(heff_str_bytes(path, (int64_t)strlen(path)));
      } else if (errno != EEXIST) r = fs_left_at(errno, path);
    }
  }
  if (!r) r = fs_left_at(EEXIST, path);
  free(pre);
  free(path);
  return r;
}

// (`p` with its links resolved: `Right` it, or `Left` why not)
static V fs_real_path(const FsPath *p) {
  const char *path = p->full;
#ifdef _WIN32
  struct stat st;
  if (stat(path, &st) != 0) return fs_left_at(errno, p->given);
  char *r = _fullpath(NULL, path, 0);
#else
  char *r = realpath(path, NULL);
#endif
  if (!r) return fs_left_at(errno, p->given);
  V s = heff_str_bytes(r, (int64_t)strlen(r));
  free(r);
  return fs_right(s);
}

// (what the link at `path` says, as it was written: `Right` it, or
// `Left` why not -- not a link, EINVAL; on Windows, where links are not
// seen, EINVAL)
static V fs_read_link(const FsPath *p) {
#ifdef _WIN32
  return fs_left_at(EINVAL, p->given);
#else
  size_t cap = 256;
  for (;;) {
    char *b = heff_alloc(cap);
    ssize_t n = readlink(p->full, b, cap);
    if (n < 0) {
      int err = errno;
      free(b);
      return fs_left_at(err, p->given);
    }
    if ((size_t)n < cap) {
      V s = heff_str_bytes(b, (int64_t)n);
      free(b);
      return fs_right(s);
    }
    free(b);
    cap *= 2;
  }
#endif
}

// (`p`'s mode set: `Right MkUnit`, or `Left` why not)
static V fs_set_mode(const FsPath *p, int64_t mode) {
  const char *path = p->full;
#ifdef _WIN32
  int r = chmod(path, (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
#else
  int r = chmod(path, (mode_t)(mode & 07777));
#endif
  return r == 0 ? fs_done() : fs_left_at(errno, p->given);
}

// (`p` opened as `OpenPath` says: `Right` the file, the host's own;
// or `Left` why not -- a directory's, EISDIR)
static V fs_open(const FsPath *p, int64_t access, int create, int truncate, int exclusive, int64_t mode) {
  const char *path = p->full;
  // (0 read, 1 write, 2 append, 3 read and write, 4 read and append)
  int flags = access == 0 ? O_RDONLY
            : access == 1 ? O_WRONLY
            : access == 2 ? (O_WRONLY | O_APPEND)
            : access == 3 ? O_RDWR
                          : (O_RDWR | O_APPEND);
  if (create) flags |= O_CREAT;
  if (truncate) flags |= O_TRUNC;
  if (exclusive) flags |= O_CREAT | O_EXCL;
  flags |= O_CLOEXEC | O_BINARY;
  int fd;
  do fd = open(path, flags, (int)(mode & 07777)); while (fd < 0 && errno == EINTR);
  if (fd < 0) return fs_left_at(errno, p->given);
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    close(fd);
    return fs_left_at(EISDIR, p->given);
  }
  return fs_right(fs_value(fs_new(fd, -1, heff_strdup(p->given))));
}

// (a file closed, but a standard stream: what waits on it told so, not
// polled closed; errno, where it failed.  Done once: closed, the file's
// operations are `EBADF`, and letting it go frees only what holds it)
static int fs_close(FsFile *h) {
  int err = 0;
  if (h->port && h->fd >= 0) {
    FsFile *s = fs_standard[h->port - 1];
    if (s && !s->buf) {
      s->buf = h->buf; s->cap = h->cap; s->at = h->at; s->end = h->end;
      h->buf = NULL; h->cap = h->at = h->end = 0;
    }
  }
  if (h->standard < 0 && h->fd >= 0) {
    heff_reactor_closing(h->fd);
    if (close(h->fd) != 0 && errno != EINTR) err = errno;
    h->fd = -1;
  }
  return err;
}

// The operations
// ---------------------------------------------------------------------

// (`x` given to the function at field `at` of `p`, its answer to `k`;
// `p` let go of)
static V fs_answered(V p, int at, V x, V k) {
  V f = heff_dup(heff_field(p, at));
  heff_drop(p);
  V y = heff_answer(f, x);
  return heff_tail(k, 1, &y);
}

// (a path given to the system is from this thread's directory, which
// is the thread's own, not the process's)
static V heff_provider_filesystem(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_PathStatus: {
    FsPath path;
    int follow = heff_field(p, 1) == NTAG(TAG_True);
    V r;
    if (!fs_path(&path, heff_field(p, 0))) r = fs_invalid(heff_field(p, 0));
    else {
      struct stat st;
      int s = follow ? stat(path.full, &st) : lstat(path.full, &st);
      r = s == 0 ? fs_status(&st) : fs_left_at(errno, path.given);
    }
    fs_path_free(&path);
    return fs_answered(p, 2, r, k);
  }
  case TAG_SetMode: {
    FsPath path;
    int64_t mode;
    V r = !fs_path(&path, heff_field(p, 0)) ? fs_invalid(heff_field(p, 0))
        : !fs_int(heff_field(p, 1), &mode) ? fs_left_at(EINVAL, path.given)
        : fs_set_mode(&path, mode);
    fs_path_free(&path);
    return fs_answered(p, 2, r, k);
  }
  case TAG_RealPath: {
    FsPath path;
    V r = fs_path(&path, heff_field(p, 0)) ? fs_real_path(&path) : fs_invalid(heff_field(p, 0));
    fs_path_free(&path);
    return fs_answered(p, 1, r, k);
  }
  case TAG_ReadLink: {
    FsPath path;
    V r = fs_path(&path, heff_field(p, 0)) ? fs_read_link(&path) : fs_invalid(heff_field(p, 0));
    fs_path_free(&path);
    return fs_answered(p, 1, r, k);
  }
  case TAG_TemporaryDirectory:
    return fs_answered(p, 1, fs_temp(heff_field(p, 0), 0), k);
  case TAG_TemporaryFile:
    return fs_answered(p, 1, fs_temp(heff_field(p, 0), 1), k);
  case TAG_OpenPath: {
    FsPath path;
    int64_t access, mode;
    V r;
    if (!fs_path(&path, heff_field(p, 0))) r = fs_invalid(heff_field(p, 0));
    else if (!fs_int(heff_field(p, 1), &access) || !fs_int(heff_field(p, 5), &mode))
      r = fs_left_at(EINVAL, path.given);
    else
      r = fs_open(&path, access, heff_field(p, 2) == NTAG(TAG_True), heff_field(p, 3) == NTAG(TAG_True),
                  heff_field(p, 4) == NTAG(TAG_True), mode);
    fs_path_free(&path);
    return fs_answered(p, 6, r, k);
  }
  case TAG_Standard: {
    static const char *const names[3] = { "<stdin>", "<stdout>", "<stderr>" };
    int64_t i = INT_OF(heff_field(p, 0));
    if (i < 0 || i > 2) heff_fail("std.filesystem: no such standard stream");
    if (!fs_standard[i]) fs_standard[i] = fs_new((int)i, (int)i, heff_strdup(names[i]));
    return fs_answered(p, 1, fs_value(fs_standard[i]), k);
  }
  // (a port of its own on a standard stream: its descriptor a copy of
  // the stream's, closed as the port is, the stream left open; what the
  // stream read ahead its, until then)
  case TAG_StandardPort: {
    static const char *const names[3] = { "<stdin>", "<stdout>", "<stderr>" };
    int64_t i = INT_OF(heff_field(p, 0));
    if (i < 0 || i > 2) heff_fail("std.filesystem: no such standard stream");
    if (!fs_standard[i]) fs_standard[i] = fs_new((int)i, (int)i, heff_strdup(names[i]));
    int fd = dup((int)i);
    if (fd < 0) heff_fail("std.filesystem: a standard stream not duplicated");
    FsFile *h = fs_new(fd, -1, heff_strdup(names[i]));
    FsFile *s = fs_standard[i];
    h->port = (int)i + 1;
    h->buf = s->buf; h->cap = s->cap; h->at = s->at; h->end = s->end;
    s->buf = NULL; s->cap = s->at = s->end = 0;
    return fs_answered(p, 1, fs_value(h), k);
  }
  // (an operation of an open file: one that waits keeps `p`, and so the
  // file, while it does, so that it is not let go of under it; after the
  // `with` closed the file, each is `EBADF`)
  case TAG_ReadChunk: {
    FsFile *h = fs_file(heff_field(p, 0));
    if (h->fd < 0) return fs_answered(p, 2, fs_left_file(h, EBADF), k);
    int64_t n;
    if (!fs_int(heff_field(p, 1), &n)) n = FS_READ_MAX;
    V f = heff_dup(heff_field(p, 2));
    if (n <= 0) {
      heff_drop(p);
      V x = heff_answer(f, fs_right(heff_bytes_in("", 0)));
      return heff_tail(k, 1, &x);
    }
    FsOp *o = fs_op(FS_CHUNK, h);
    o->n = n > FS_READ_MAX ? FS_READ_MAX : n;
    o->held = p;
    return fs_go(o, fs_chunk_step, f, k);
  }
  case TAG_ReadLine: {
    FsFile *h = fs_file(heff_field(p, 0));
    if (h->fd < 0) return fs_answered(p, 1, fs_left_file(h, EBADF), k);
    V f = heff_dup(heff_field(p, 1));
    FsOp *o = fs_op(FS_LINE, h);
    o->held = p;
    return fs_go(o, fs_line_step, f, k);
  }
  case TAG_WriteBytes: {
    FsFile *h = fs_file(heff_field(p, 0));
    if (h->fd < 0) return fs_answered(p, 2, fs_left_file(h, EBADF), k);
    V f = heff_dup(heff_field(p, 2));
    int64_t n;
    // (the bytes are `p`'s, held until they are written)
    const uint8_t *data = heff_bytes_data(heff_field(p, 1), &n);
    return fs_write(h, data, n, p, NULL, f, k);
  }
  case TAG_WriteString: {
    FsFile *h = fs_file(heff_field(p, 0));
    if (h->fd < 0) return fs_answered(p, 2, fs_left_file(h, EBADF), k);
    V f = heff_dup(heff_field(p, 2));
    size_t n;
    char *data = heff_bytes(heff_field(p, 1), &n);
    return fs_write(h, (const uint8_t *)data, (int64_t)n, p, data, f, k);
  }
  case TAG_Seek: {
    FsFile *h = fs_file(heff_field(p, 0));
    int64_t from, n;
    V r;
    if (h->fd < 0) r = fs_left_file(h, EBADF);
    else if (!fs_int(heff_field(p, 1), &from) || !fs_int(heff_field(p, 2), &n)) r = fs_left_file(h, EINVAL);
    else {
      // (from where the program has read up to, not where the file was
      // read ahead to)
      if (from == 1) n -= (int64_t)fs_buffered(h);
      int whence = from == 0 ? SEEK_SET : from == 1 ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
      int64_t at = _lseeki64(h->fd, n, whence);
#else
      int64_t at = (int64_t)lseek(h->fd, (off_t)n, whence);
#endif
      if (at < 0) r = fs_left_file(h, errno);
      else {
        h->at = h->end = 0;
        h->taken++;
        r = fs_right(heff_int(at));
      }
    }
    return fs_answered(p, 3, r, k);
  }
  case TAG_FileStatus: {
    FsFile *h = fs_file(heff_field(p, 0));
    if (h->fd < 0) return fs_answered(p, 1, fs_left_file(h, EBADF), k);
    struct stat st;
    V r = fstat(h->fd, &st) == 0 ? fs_status(&st) : fs_left_file(h, errno);
    return fs_answered(p, 1, r, k);
  }
  case TAG_CloseFile: {
    FsFile *h = fs_file(heff_field(p, 0));
    int err = fs_close(h);
    return fs_answered(p, 1, err ? fs_left_file(h, err) : fs_done(), k);
  }
  default:
    return heff_fs_whole(p, k);
  }
}
