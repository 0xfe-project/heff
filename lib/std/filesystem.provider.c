// The disk's side of std.filesystem, for the C backend: an effect
// provider.
//
// Whole files, directories and paths are done there and then.  An open
// file is a `FsFile` the world holds, let go of --
// closed -- by `CloseFile`; the program has its number.  It reads ahead
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

// (a file's text, if it can be read)
static V heff_read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return heff_nothing;
  Buf b = {0};
  char chunk[65536];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&b, chunk, got);
  int bad = ferror(f);
  fclose(f);
  V s = bad ? 0 : heff_str_bytes(b.s ? b.s : "", (int64_t)b.n);
  free(b.s);
  return bad ? heff_nothing : heff_just(s);
}

static int heff_write_file(const char *path, V s) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  heff_put(f, s);
  return fclose(f) == 0;
}

// (a directory is at `path`, links followed)
static int heff_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// (a directory and those it is in: whether a directory is there -- a
// file there is none)
static int heff_mkdir_p(char *path) {
  for (char *c = path + 1; *c; c++)
    if (heff_is_sep(*c)) {
      char sep = *c;
      *c = 0;
      // (a drive, `C:`, is there already)
      int drive = c[-1] == ':';
      int ok = drive || heff_mkdir(path) == 0 || errno == EEXIST;
      *c = sep;
      if (!ok) return 0;
    }
  return heff_mkdir(path) == 0 || (errno == EEXIST && heff_is_dir(path));
}

// (the names in a directory, sorted)
static V heff_read_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d) return heff_nothing;
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
  V l = heff_strs_bytes(ns, n);
  for (int64_t i = 0; i < n; i++) free(ns[i]);
  free(ns);
  return heff_just(l);
}

// (every file and directory under `p`, its mode's write bits off, or on:
// a directory is made writable before what it holds, read-only after)
static int heff_chmod_all(const char *p, int writable) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT ? -1 : 0;
  if (S_ISLNK(st.st_mode)) return 1;
  mode_t mode = writable ? (st.st_mode | 0200) : (st.st_mode & ~0222);
  mode &= 07777;
  if (S_ISDIR(st.st_mode)) {
    if (writable && chmod(p, mode) != 0) return 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    int ok = 1;
    while (ok && (e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char *q = heff_join(p, e->d_name);
      ok = heff_chmod_all(q, writable) != 0;
      free(q);
    }
    closedir(d);
    if (!ok) return 0;
    if (!writable && chmod(p, mode) != 0) return 0;
    return 1;
  }
  return chmod(p, mode) == 0;
}

// (a file, or a directory and all it holds; nothing there is no failure)
static int heff_remove_all(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return unlink(p) == 0;
  DIR *d = opendir(p);
  if (!d) return 0;
  struct dirent *e;
  int ok = 1;
  while (ok && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *q = heff_join(p, e->d_name);
    ok = heff_remove_all(q);
    free(q);
  }
  closedir(d);
  return ok && rmdir(p) == 0;
}

// (a file's bytes copied to `q`, which is given `mode`)
static int heff_copy_file(const char *p, const char *q, mode_t mode) {
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  FILE *g = fopen(q, "wb");
  if (!g) {
    fclose(f);
    return 0;
  }
  char *chunk = heff_alloc(65536);
  size_t got;
  int ok = 1;
  while (ok && (got = fread(chunk, 1, 65536, f)) > 0) ok = fwrite(chunk, 1, got, g) == got;
  ok = ok && !ferror(f);
  free(chunk);
  fclose(f);
  ok = fclose(g) == 0 && ok;
  return ok && chmod(q, mode) == 0;
}

// (a file, or a directory and all it holds, copied to `q`: links
// followed, modes kept -- a directory's once what it holds is in it)
static int heff_copy_all(const char *p, const char *q) {
  struct stat st;
  if (stat(p, &st) != 0) return 0;
  mode_t mode = st.st_mode & 07777;
  if (!S_ISDIR(st.st_mode)) return heff_copy_file(p, q, mode);
  if (heff_mkdir(q) != 0) return 0;
  DIR *d = opendir(p);
  if (!d) return 0;
  struct dirent *e;
  int ok = 1;
  while (ok && (e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *a = heff_join(p, e->d_name);
    char *b = heff_join(q, e->d_name);
    ok = heff_copy_all(a, b);
    free(a);
    free(b);
  }
  closedir(d);
  return ok && chmod(q, mode) == 0;
}

// (nothing is at `p`: no file, directory or link)
static int heff_nothing_at(const char *p) {
  struct stat st;
  return lstat(p, &st) != 0 && errno == ENOENT;
}

// (what an operation answers where its path is none, a string with a NUL
// in it: it fails -- `Nothing` where it reads, `False` where it does)
static V heff_fs_failed(V p) {
  int tag = heff_tag_of(p);
  int at = tag == TAG_WriteFile || tag == TAG_Copy || tag == TAG_Move || tag == TAG_Replace ? 2 : 1;
  V f = heff_dup(heff_field(p, at));
  heff_drop(p);
  return heff_answer(f, tag == TAG_ReadFile || tag == TAG_ReadDirectory ? heff_nothing : heff_bool(0));
}

// (an operation of whole files and paths: answered by a `Bool` or a
// `Maybe`)
static V heff_fs_whole(V p, V k) {
  char *path = heff_cstr(heff_field(p, 0));
  V x;
  if (!path) {
    x = heff_fs_failed(p);
    return heff_tail(k, 1, &x);
  }
  switch (heff_tag_of(p)) {
  case TAG_ReadFile: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_read_file(path));
    break;
  }
  case TAG_WriteFile: {
    int ok = heff_write_file(path, heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_MakeDirectory: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_mkdir_p(path)));
    break;
  }
  case TAG_ReadDirectory: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_read_dir(path));
    break;
  }
  case TAG_Remove: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    int ok = heff_chmod_all(path, 1) != 0 && heff_remove_all(path);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_ReadOnly: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_chmod_all(path, 0) == 1));
    break;
  }
  case TAG_Writable: {
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_chmod_all(path, 1) == 1));
    break;
  }
  case TAG_Copy: {
    char *to = heff_cstr(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    int ok = to && heff_nothing_at(to) && heff_copy_all(path, to);
    free(to);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_Move: {
    char *to = heff_cstr(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    int ok = to && heff_nothing_at(to) && rename(path, to) == 0;
    free(to);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  case TAG_Replace: {
    char *to = heff_cstr(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    // (rename(2) puts a file in place of another at once; Windows' does
    // not over one there, MoveFileEx does)
#ifdef _WIN32
    int ok = to && MoveFileExA(path, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int ok = to && rename(path, to) == 0;
#endif
    free(to);
    x = heff_answer(f, heff_bool(ok));
    break;
  }
  default:
    heff_fail("not an operation of filesystem");
    return 0;
  }
  free(path);
  return heff_tail(k, 1, &x);
}

// Errors
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

static uint32_t fs_error_shape[3] = { LBL_code, LBL_name, LBL_message };
static uint32_t fs_status_shape[4] = { LBL_kind, LBL_size, LBL_modified, LBL_mode };

// (a `FileError` of errno `err`: the runtime's errno record, whose shape
// a `FileError` is)
static V fs_error(int err) { return heff_errno(err, fs_error_shape); }

static V fs_left(int err) {
  V x = fs_error(err);
  return heff_tag_n(TAG_Left, 1, &x);
}

static V fs_right(V x) { return heff_tag_n(TAG_Right, 1, &x); }

// (`Right MkUnit`)
static V fs_done(void) { return fs_right(NTAG(TAG_MkUnit)); }

// (`Right` what the system says of a file -- `r`, stat's answer, 0 -- or
// `Left` why not)
static V fs_status(int r, const struct stat *st) {
  if (r != 0) return fs_left(errno);
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

// (the world's open files, told from what other providers hand out)
#define FS_MAGIC 0x66696c65u
// (how much is read ahead at a time; a chunk's most)
#define FS_BUF ((size_t)65536)
#define FS_READ_MAX ((int64_t)16 << 20)

typedef struct {
  uint32_t magic;
  int fd;            // (-1 once closed)
  int refs;          // (the world's, and each op's that has it)
  int standard;      // (a standard stream's number, 0 to 2: never closed;
                     //  else -1)
  int pollable;      // (a pipe, a terminal, a socket: polled, where a
                     //  scheduler runs)
  int seekable;      // (a regular file: what was read ahead is given back
                     //  before a write)
  uint8_t *buf;      // (read ahead: what is not yet taken, from `at` to
  size_t cap, at, end;  //  `end`)
  uint64_t taken;    // (how many times something was taken from it)
} FsFile;

// (the standard streams' files, once asked for: one each, so that what
// one has read ahead is the other's too)
static FsFile *fs_standard[3];

static void fs_unref(FsFile *h) {
  if (--h->refs > 0) return;
  free(h->buf);
  h->magic = 0;
  free(h);
}

// (the world letting go of it: closed, unless it is a standard stream)
static void fs_let_go(void *x) {
  FsFile *h = x;
  if (h->standard >= 0) fs_standard[h->standard] = NULL;
  else if (h->fd >= 0) close(h->fd);
  h->fd = -1;
  fs_unref(h);
}

static FsFile *fs_new(int fd, int standard) {
  FsFile *h = heff_alloc(sizeof *h);
  memset(h, 0, sizeof *h);
  h->magic = FS_MAGIC;
  h->fd = fd;
  h->refs = 1;
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

// (the file held by the world: `MkHandle i`)
static V fs_handle(FsFile *h) {
  V i = MK_INT(heff_hold(h, fs_let_go));
  return heff_tag_n(TAG_MkHandle, 1, &i);
}

// (the file a handle is the number of, or the program failed: it is none)
static FsFile *fs_file(V handle) {
  FsFile *h = heff_held(INT_OF(heff_field(handle, 0)));
  if (h->magic != FS_MAGIC) heff_fail("std.filesystem: the handle is no open file");
  return h;
}

static size_t fs_buffered(FsFile *h) { return h->end - h->at; }

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
  V held;            // (what holds them: a write's operation)
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
  if (err) r = fs_left(err);
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
  if (h->standard == 1 || h->standard == 2) fflush(stdout);
  if (h->standard == 2) fflush(stderr);
  if (fs_unread(h) < 0) {
    int e = errno;
    if (held) heff_drop(held);
    free(owned);
    V x = heff_answer(f, fs_left(e));
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

// (a new temporary directory, or file -- open, `Pair` its path and it --
// its name `prefix` and six letters)
static V fs_temp(V prefix, int file) {
  char *pre = heff_cstr(prefix);
  if (!pre) return fs_left(EINVAL);
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
      else if (errno != EEXIST) r = fs_left(errno);
    } else {
      int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_BINARY, 0600);
      if (fd >= 0) {
        V both[2] = { heff_str_bytes(path, (int64_t)strlen(path)), fs_handle(fs_new(fd, -1)) };
        r = fs_right(heff_tag_n(TAG_MkPair, 2, both));
      } else if (errno != EEXIST) r = fs_left(errno);
    }
  }
  free(pre);
  free(path);
  return r ? r : fs_left(EEXIST);
}

// (`path` with its links resolved: `Right` it, or `Left` why not)
static V fs_real_path(const char *path) {
#ifdef _WIN32
  struct stat st;
  if (stat(path, &st) != 0) return fs_left(errno);
  char *r = _fullpath(NULL, path, 0);
#else
  char *r = realpath(path, NULL);
#endif
  if (!r) return fs_left(errno);
  V s = heff_str_bytes(r, (int64_t)strlen(r));
  free(r);
  return fs_right(s);
}

// (`path`'s mode set: `Right MkUnit`, or `Left` why not)
static V fs_set_mode(const char *path, int64_t mode) {
#ifdef _WIN32
  int r = chmod(path, (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
#else
  int r = chmod(path, (mode_t)(mode & 07777));
#endif
  return r == 0 ? fs_done() : fs_left(errno);
}

// (`path` opened as `OpenPath` says: `Right` its handle, or `Left` why
// not -- a directory's, EISDIR)
static V fs_open(const char *path, int64_t access, int create, int truncate, int exclusive, int64_t mode) {
  int flags = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : access == 2 ? (O_WRONLY | O_APPEND) : O_RDWR;
  if (create) flags |= O_CREAT;
  if (truncate) flags |= O_TRUNC;
  if (exclusive) flags |= O_CREAT | O_EXCL;
  flags |= O_CLOEXEC | O_BINARY;
  int fd;
  do fd = open(path, flags, (int)(mode & 07777)); while (fd < 0 && errno == EINTR);
  if (fd < 0) return fs_left(errno);
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    close(fd);
    return fs_left(EISDIR);
  }
  return fs_right(fs_handle(fs_new(fd, -1)));
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

static V heff_provider_filesystem(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_PathStatus: {
    char *path = heff_cstr(heff_field(p, 0));
    int follow = heff_field(p, 1) == NTAG(TAG_True);
    V r;
    if (!path) r = fs_left(EINVAL);
    else {
      struct stat st;
      int s = follow ? stat(path, &st) : lstat(path, &st);
      r = fs_status(s, &st);
      free(path);
    }
    return fs_answered(p, 2, r, k);
  }
  case TAG_SetMode: {
    char *path = heff_cstr(heff_field(p, 0));
    int64_t mode;
    V r = !path || !fs_int(heff_field(p, 1), &mode) ? fs_left(EINVAL) : fs_set_mode(path, mode);
    free(path);
    return fs_answered(p, 2, r, k);
  }
  case TAG_RealPath: {
    char *path = heff_cstr(heff_field(p, 0));
    V r = path ? fs_real_path(path) : fs_left(EINVAL);
    free(path);
    return fs_answered(p, 1, r, k);
  }
  case TAG_TemporaryDirectory:
    return fs_answered(p, 1, fs_temp(heff_field(p, 0), 0), k);
  case TAG_TemporaryFile:
    return fs_answered(p, 1, fs_temp(heff_field(p, 0), 1), k);
  case TAG_OpenPath: {
    char *path = heff_cstr(heff_field(p, 0));
    int64_t access, mode;
    int ok = path && fs_int(heff_field(p, 1), &access) && fs_int(heff_field(p, 5), &mode);
    V r = !ok ? fs_left(EINVAL)
              : fs_open(path, access, heff_field(p, 2) == NTAG(TAG_True), heff_field(p, 3) == NTAG(TAG_True),
                        heff_field(p, 4) == NTAG(TAG_True), mode);
    free(path);
    return fs_answered(p, 6, r, k);
  }
  case TAG_Standard: {
    int64_t i = INT_OF(heff_field(p, 0));
    if (i < 0 || i > 2) heff_fail("std.filesystem: no such standard stream");
    if (!fs_standard[i]) fs_standard[i] = fs_new((int)i, (int)i);
    return fs_answered(p, 1, fs_handle(fs_standard[i]), k);
  }
  case TAG_ReadChunk: {
    FsFile *h = fs_file(heff_field(p, 0));
    int64_t n;
    if (!fs_int(heff_field(p, 1), &n)) n = FS_READ_MAX;
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    if (n <= 0) {
      V x = heff_answer(f, fs_right(heff_bytes_in("", 0)));
      return heff_tail(k, 1, &x);
    }
    FsOp *o = fs_op(FS_CHUNK, h);
    o->n = n > FS_READ_MAX ? FS_READ_MAX : n;
    return fs_go(o, fs_chunk_step, f, k);
  }
  case TAG_ReadLine: {
    FsFile *h = fs_file(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return fs_go(fs_op(FS_LINE, h), fs_line_step, f, k);
  }
  case TAG_WriteBytes: {
    FsFile *h = fs_file(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 2));
    int64_t n;
    // (the bytes are `p`'s, held until they are written)
    const uint8_t *data = heff_bytes_data(heff_field(p, 1), &n);
    return fs_write(h, data, n, p, NULL, f, k);
  }
  case TAG_WriteString: {
    FsFile *h = fs_file(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 2));
    size_t n;
    char *data = heff_bytes(heff_field(p, 1), &n);
    heff_drop(p);
    return fs_write(h, (const uint8_t *)data, (int64_t)n, 0, data, f, k);
  }
  case TAG_Seek: {
    FsFile *h = fs_file(heff_field(p, 0));
    int64_t from, n;
    V r;
    if (!fs_int(heff_field(p, 1), &from) || !fs_int(heff_field(p, 2), &n)) r = fs_left(EINVAL);
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
      if (at < 0) r = fs_left(errno);
      else {
        h->at = h->end = 0;
        h->taken++;
        r = fs_right(heff_int(at));
      }
    }
    return fs_answered(p, 3, r, k);
  }
  case TAG_HandleStatus: {
    FsFile *h = fs_file(heff_field(p, 0));
    struct stat st;
    int s = fstat(h->fd, &st);
    return fs_answered(p, 1, fs_status(s, &st), k);
  }
  case TAG_CloseFile: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    FsFile *h = fs_file(heff_field(p, 0));
    int err = 0;
    if (h->standard < 0 && h->fd >= 0) {
      // (what waits on it told so, not polled closed)
      heff_reactor_closing(h->fd);
      if (close(h->fd) != 0 && errno != EINTR) err = errno;
      h->fd = -1;
    }
    heff_release(i);
    return fs_answered(p, 1, err ? fs_left(err) : fs_done(), k);
  }
  default:
    return heff_fs_whole(p, k);
  }
}
