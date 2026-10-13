// The terminal's side of lib/std/progress.hf, for the C backend: an
// effect provider (the shape of one is said in lib/std/io.provider.c).
//
// The lines are drawn on the standard error by a thread of their own,
// as zig draws its progress (std.Progress): every 80 ms, the newest
// lines given, if they are not those drawn -- none in the first 200 ms
// after lines are first given, so that what is quickly done shows
// nothing -- each cut to the terminal's width, as many as leave two of
// its rows.  The cursor is left where the first line starts: what the
// program writes there clears them first (`heff_lines`, which
// lib/std/io.provider.c calls around each write), and they come back
// below it.  At the program's end, or where it runs a command in its
// place, they are cleared for good.
//
// A command run under a task (`ShowRun`) is given a pipe for its own
// progress, its number in `ZIG_PROGRESS`, as zig gives its children
// one: zig writes the tree of its tasks there every 80 ms, a packet
// each -- `n`, a byte; `n` tasks of 128 bytes (how many of its items
// are done, of how many: 32 bits each, little-endian; its name, 120
// bytes, ended by a 0 if shorter); and the index of each one's parent,
// a byte each (255: none) -- and the newest tree is given to the
// program's function, whose lines are drawn.  (On Windows, where zig's
// is a handle, the command is run without it, by runtime.c's
// heff_capture, its input the program's.)  Elsewhere it has no input,
// as std.process's `run` gives none.

#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <time.h>

// (`heff_lines(1)`: what is drawn cleared, and nothing drawn, until
// `heff_lines(0)`; `heff_lines(2)`: cleared, and nothing drawn again.
// Set once lines are first given; the providers of the streams declare
// it too -- one variable, however many of them a program has)
static void (*heff_lines)(int);

#ifdef _WIN32
static SRWLOCK heff_st_mu = SRWLOCK_INIT;
static CONDITION_VARIABLE heff_st_cv = CONDITION_VARIABLE_INIT;
static HANDLE heff_st_thread;
static void heff_st_lock(void) { AcquireSRWLockExclusive(&heff_st_mu); }
static void heff_st_unlock(void) { ReleaseSRWLockExclusive(&heff_st_mu); }
static void heff_st_wait(int ms) { SleepConditionVariableSRW(&heff_st_cv, &heff_st_mu, (DWORD)ms, 0); }
static void heff_st_wake(void) { WakeAllConditionVariable(&heff_st_cv); }
static int64_t heff_st_now(void) { return (int64_t)GetTickCount64(); }
#else
static pthread_mutex_t heff_st_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t heff_st_cv = PTHREAD_COND_INITIALIZER;
static pthread_t heff_st_thread;
static void heff_st_lock(void) { pthread_mutex_lock(&heff_st_mu); }
static void heff_st_unlock(void) { pthread_mutex_unlock(&heff_st_mu); }
static void heff_st_wait(int ms) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  t.tv_nsec += (long)ms * 1000000L;
  t.tv_sec += t.tv_nsec / 1000000000L;
  t.tv_nsec %= 1000000000L;
  pthread_cond_timedwait(&heff_st_cv, &heff_st_mu, &t);
}
static void heff_st_wake(void) { pthread_cond_broadcast(&heff_st_cv); }
static int64_t heff_st_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif

// (the newest lines given, `heff_st_n` of them; how many are on the
// terminal now; whether they are not those; since when lines have been
// given, nothing drawn before; whether the thread draws; whether the
// program is ending)
static char **heff_st_lines = NULL;
static int heff_st_n = 0, heff_st_drawn = 0, heff_st_dirty = 0;
static int64_t heff_st_since = 0;
static int heff_st_going = 0, heff_st_over = 0;

// (lines are drawn where the standard error is a terminal and `TERM` is
// not `dumb`; on Windows, a console that takes escape sequences: 1, 0,
// or -1 not asked yet)
static int heff_st_tty = -1;

static int heff_st_terminal(void) {
  if (heff_st_tty >= 0) return heff_st_tty;
  const char *term = getenv("TERM");
  int ok = !(term && strcmp(term, "dumb") == 0);
#ifdef _WIN32
  DWORD mode;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  ok = ok && _isatty(2) && GetConsoleMode(h, &mode)
    && ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) || SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING));
#else
  ok = ok && isatty(2);
#endif
  heff_st_tty = ok;
  return ok;
}

// (the terminal's columns and rows: 80 by 25 if it does not say)
static void heff_st_size(int *cols, int *rows) {
  *cols = 80;
  *rows = 25;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) {
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
#else
  struct winsize w;
  if (ioctl(2, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
    *cols = w.ws_col;
    *rows = w.ws_row;
  }
#endif
}

// (how many columns a character takes: two, East Asian wide ones, else
// one)
static int heff_st_width(uint32_t u) {
  return (u >= 0x1100 && u <= 0x115F) || (u >= 0x2E80 && u <= 0xA4CF) || (u >= 0xAC00 && u <= 0xD7A3)
    || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFE30 && u <= 0xFE4F) || (u >= 0xFF00 && u <= 0xFF60)
    || (u >= 0xFFE0 && u <= 0xFFE6) || (u >= 0x1F300 && u <= 0x1F64F) || (u >= 0x1F900 && u <= 0x1F9FF)
    || (u >= 0x20000 && u <= 0x3FFFD) ? 2 : 1;
}

// (a line, UTF-8, as much of it as fits in `width` columns)
static void heff_st_cut(Buf *b, const char *s, int width) {
  int w = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
    uint32_t u = len == 1 ? *p : len == 2 ? *p & 0x1F : len == 3 ? *p & 0x0F : *p & 0x07;
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) { len = i; break; }
      u = (u << 6) | (p[i] & 0x3F);
    }
    int cw = heff_st_width(u);
    if (w + cw > width) break;
    buf_put(b, (const char *)p, (size_t)len);
    w += cw;
    p += len;
  }
}

// (what is drawn cleared: the cursor is where it starts)
static void heff_st_clear(void) {
  if (heff_st_drawn == 0) return;
  fputs("\x1b[J", stderr);
  fflush(stderr);
  heff_st_drawn = 0;
  heff_st_dirty = heff_st_n > 0;
}

// (the newest lines drawn in place of what was, at once -- the cursor
// back where they start -- as one write, the terminal told to show it
// whole: the lock held)
static void heff_st_draw(void) {
  int cols, rows;
  heff_st_size(&cols, &rows);
  Buf b = {0};
  buf_str(&b, "\x1b[?2026h\x1b[J");
  int n = 0;
  for (; n < heff_st_n && n + 2 < rows; n++) {
    // (a column short of the width: a line that fills it wraps on some)
    heff_st_cut(&b, heff_st_lines[n], cols - 1);
#ifdef _WIN32
    buf_str(&b, "\r\n");
#else
    buf_str(&b, "\n");
#endif
  }
  if (n > 0) {
    buf_str(&b, "\r");
    for (int i = 0; i < n; i++) buf_str(&b, "\x1bM");
  }
  buf_str(&b, "\x1b[?2026l");
  fwrite(b.s, 1, b.n, stderr);
  fflush(stderr);
  free(b.s);
  heff_st_drawn = n;
  heff_st_dirty = 0;
}

#ifdef _WIN32
static DWORD WINAPI heff_st_loop(LPVOID u) {
#else
static void *heff_st_loop(void *u) {
#endif
  (void)u;
  heff_st_lock();
  while (heff_st_going) {
    heff_st_wait(80);
    if (heff_st_going && heff_st_dirty && heff_st_now() - heff_st_since >= 200) heff_st_draw();
  }
  heff_st_unlock();
  return 0;
}

static void heff_st_free(void) {
  for (int i = 0; i < heff_st_n; i++) free(heff_st_lines[i]);
  free(heff_st_lines);
  heff_st_lines = NULL;
  heff_st_n = 0;
}

// (what the streams' providers call: see `heff_lines`)
static void heff_st_hold(int what) {
  if (what == 0) {
    // (what was written out before the lines come back)
    if (heff_st_n > 0) fflush(stdout);
    heff_st_unlock();
    return;
  }
  heff_st_lock();
  heff_st_clear();
  if (what == 2) {
    heff_st_over = 1;
    heff_st_going = 0;
    heff_st_free();
    heff_st_wake();
    heff_st_unlock();
  }
}

static void heff_st_atexit(void) {
  if (heff_lines) heff_lines(2);
}

// (the lines given are those there are: the lock held)
static int heff_st_same(char **lines, int n) {
  if (n != heff_st_n) return 0;
  for (int i = 0; i < n; i++)
    if (strcmp(lines[i], heff_st_lines[i]) != 0) return 0;
  return 1;
}

// (the lines `l`, a list of strings, drawn from now on, unless they are
// those there are; none: nothing drawn, what was cleared at once.  The
// thread, once started, draws until the program ends: lines that come
// again later are drawn at its next turn, not after 200 ms -- zig waits
// once, too)
static void heff_st_show(V l) {
  if (!heff_st_terminal() || heff_st_over) return;
  int n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  char **lines = heff_alloc((size_t)(n ? n : 1) * sizeof(char *));
  int i = 0;
  // (a line with a NUL in it, which cannot be a C string, drawn empty)
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    char *line = heff_cstr_text(heff_field(c, 0));
    lines[i++] = line ? line : strdup("");
  }
  heff_st_lock();
  if (heff_st_same(lines, n)) {
    heff_st_unlock();
    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
    return;
  }
  heff_st_free();
  heff_st_lines = lines;
  heff_st_n = n;
  heff_st_dirty = n > 0;
  if (n == 0) heff_st_clear();
  else if (!heff_st_going) {
    static int registered = 0;
    if (!registered) { registered = 1; atexit(heff_st_atexit); }
    heff_lines = heff_st_hold;
    heff_st_going = 1;
    heff_st_since = heff_st_now();
#ifdef _WIN32
    heff_st_thread = CreateThread(NULL, 0, heff_st_loop, NULL, 0, NULL);
    if (!heff_st_thread) heff_st_going = 0;
#else
    if (pthread_create(&heff_st_thread, NULL, heff_st_loop, NULL) != 0) heff_st_going = 0;
#endif
  }
  heff_st_unlock();
}

static uint32_t heff_st_ran_shape[3] = { LBL_ended, LBL_out, LBL_err };

// (how a command ended, as std.process's `Ended`: `how` 0 by itself,
// its status `code`; 1 by the signal `code`; 3 not run)
static V heff_st_ended(int how, int64_t code) {
  V x = MK_INT(code);
  if (how == 0) return heff_tag_n(TAG_Exited, 1, &x);
  if (how == 1) return heff_tag_n(TAG_Signalled, 1, &x);
  return NTAG(TAG_NotRun);
}

#ifndef _WIN32
// (a packet of zig's, the tasks of its tree, as a list of `MkKid`s)
static V heff_st_kids(const unsigned char *pk) {
  int n = pk[0];
  V l = NTAG(TAG_Nil);
  for (int i = n - 1; i >= 0; i--) {
    const unsigned char *s = pk + 1 + i * 128;
    uint32_t done = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
    uint32_t total = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    char name[121];
    memcpy(name, s + 8, 120);
    name[120] = 0;
    V f[4] = { heff_str_text(name), MK_INT(done), MK_INT(total == 0xFFFFFFFFu ? 0 : total), MK_INT(pk[1 + n * 128 + i]) };
    l = heff_cons(heff_tag(TAG_MkKid, 4, f), l);
  }
  return l;
}

// (what the command has said of its progress, read: each whole packet
// taken, the newest one's tree given to `f`, whose lines are drawn)
static void heff_st_packets(Buf *pb, V f) {
  size_t at = 0, newest = (size_t)-1;
  while (pb->n - at >= 1) {
    size_t len = 1 + (size_t)(unsigned char)pb->s[at] * 129;
    if (pb->n - at < len) break;
    newest = at;
    at += len;
  }
  if (newest != (size_t)-1) {
    V lines = heff_tr(heff_app1(heff_dup(f), heff_st_kids((const unsigned char *)pb->s + newest)));
    heff_st_show(lines);
    heff_drop(lines);
  }
  memmove(pb->s, pb->s + at, pb->n - at);
  pb->n -= at;
}

extern char **environ;
// (the command `as` run as std.process's `run` runs it, no input given
// it, with `ZIG_PROGRESS` where `f` is given, its progress drawn by `f`
// as it comes: how it ended, as heff_ended says, its status or signal
// `*code`)
static int heff_st_capture(char **as, Buf *bo, Buf *be, V f, int64_t *code) {
  int po[2], pe[2], pp[2] = { -1, -1 };
  if (pipe(po) != 0) return 3;
  if (pipe(pe) != 0) { close(po[0]); close(po[1]); return 3; }
  if (f && pipe(pp) != 0) { close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); return 3; }
  // (the environment, its ZIG_PROGRESS the pipe's end)
  int ne = 0;
  while (environ[ne]) ne++;
  char **env = heff_alloc((size_t)(ne + 2) * sizeof(char *));
  int k = 0;
  for (int i = 0; i < ne; i++)
    if (!f || strncmp(environ[i], "ZIG_PROGRESS=", 13) != 0) env[k++] = environ[i];
  char zp[32];
  snprintf(zp, sizeof zp, "ZIG_PROGRESS=%d", pp[1]);
  if (f) env[k++] = zp;
  env[k] = NULL;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, po[1], 1);
  posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
  posix_spawn_file_actions_addclose(&fa, po[0]);
  posix_spawn_file_actions_addclose(&fa, pe[0]);
  if (f) posix_spawn_file_actions_addclose(&fa, pp[0]);
  pid_t pid;
  int ok = posix_spawnp(&pid, as[0], &fa, NULL, as, env) == 0;
  posix_spawn_file_actions_destroy(&fa);
  free(env);
  close(po[1]);
  close(pe[1]);
  if (f) close(pp[1]);
  // (its streams and its progress read together, until all are closed)
  struct pollfd fds[3] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 }, { pp[0], POLLIN, 0 } };
  Buf pb = {0};
  int open = f ? 3 : 2;
  char chunk[65536];
  while (open > 0) {
    if (poll(fds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 3; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t got = read(fds[i].fd, chunk, sizeof chunk);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) { close(fds[i].fd); fds[i].fd = -1; open--; }
      else if (i == 2) { buf_put(&pb, chunk, (size_t)got); heff_st_packets(&pb, f); }
      else buf_put(i == 0 ? bo : be, chunk, (size_t)got);
    }
  }
  for (int i = 0; i < 3; i++) if (fds[i].fd >= 0) close(fds[i].fd);
  free(pb.s);
  if (!ok) return 3;
  int st;
  pid_t w;
  while ((w = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
  if (w != pid) return 3;
  if (WIFEXITED(st)) { *code = WEXITSTATUS(st); return 0; }
  if (WIFSIGNALED(st)) { *code = WTERMSIG(st); return 1; }
  return 3;
}
#endif

static V heff_provider_terminal(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_IsTerminal: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_bool(heff_st_terminal()));
    return heff_tail(k, 1, &x);
  }
  case TAG_ShowLines: {
    heff_st_show(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_ShowRun: {
    // (a command with a NUL in its name or an argument is not run, as
    // where it cannot be)
    char *c = heff_cstr_text(heff_field(p, 0));
    int ok = c != NULL;
    int64_t n = 0;
    for (V l = heff_field(p, 1); heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1)) n++;
    char **as = heff_alloc((size_t)(n + 2) * sizeof(char *));
    as[0] = c;
    int64_t i = 1;
    for (V l = heff_field(p, 1); heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1)) {
      as[i] = heff_cstr_text(heff_field(l, 0));
      ok = ok && as[i] != NULL;
      i++;
    }
    as[i] = NULL;
    V f = heff_dup(heff_field(p, 2)), r = heff_dup(heff_field(p, 3));
    heff_drop(p);
    Buf bo = {0}, be = {0};
    int64_t code = 0;
#ifdef _WIN32
    // (no signals there: -1 its exit code where it could not be run)
    code = ok ? heff_capture(as, &bo, &be) : -1;
    int how = code == -1 ? 3 : 0;
#else
    int how = !ok ? 3 : heff_st_capture(as, &bo, &be, heff_st_terminal() && !heff_st_over ? f : 0, &code);
#endif
    heff_drop(f);
    for (int64_t j = 0; j < i; j++) free(as[j]);
    free(as);
    V fs[3] = {
      heff_st_ended(how, code),
      heff_str_bytes(bo.s ? bo.s : "", (int64_t)bo.n),
      heff_str_bytes(be.s ? be.s : "", (int64_t)be.n),
    };
    free(bo.s);
    free(be.s);
    V x = heff_answer(r, heff_rec(3, heff_st_ran_shape, fs));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of terminal");
    return 0;
  }
}
