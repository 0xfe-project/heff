// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place.
// A command `Launch`ed runs beside it, held by the world (a `Child`)
// until it is terminated; those not ended at the program's end are
// terminated then (atexit), and asked to end where a signal ends it.

#ifdef _WIN32
#include <direct.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// (where the program is: its executable, as the system says)
static V heff_program_path(void) {
  char buf[4096];
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
  if (n == 0 || n >= sizeof buf) return heff_str_text(heff_argv[0]);
  return heff_str_text(buf);
#elif defined(__APPLE__)
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return heff_str_text(heff_argv[0]);
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return heff_str_text(heff_argv[0]);
  buf[n] = 0;
#endif
#ifndef _WIN32
  char real[4096];
  return heff_str_text(realpath(buf, real) ? real : buf);
#endif
}

// (a list of strings as a NULL-ended array of C strings, `first` first:
// NULL if one of them cannot be a C string)
static char **heff_argv_of(const char *first, V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  char **as = heff_alloc((n + 2) * sizeof(char *));
  as[0] = (char *)first;
  int64_t i = 1;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    as[i] = heff_cstr_text(heff_field(c, 0));
    if (!as[i]) {
      for (int64_t j = 1; j < i; j++) free(as[j]);
      free(as);
      return NULL;
    }
    i++;
  }
  as[i] = NULL;
  return as;
}


// (the lines std.progress draws on the terminal, once there are any,
// cleared for good where a command runs in the program's place: an
// `Exit` clears them as the program ends -- lib/std/progress.provider.c)
static void (*heff_lines)(int);

// (a command started beside the program: while `on`, not yet waited
// for; its status once it has ended; among the others started, for the
// program's end)
typedef struct HeffChild HeffChild;
struct HeffChild {
#ifdef _WIN32
  HANDLE h;
#else
  pid_t pid;
#endif
  int on;
  int status;
  HeffChild *next;
};

static HeffChild *heff_children = NULL;

// (whether it has ended, not waiting: its status set, `on` no more)
static int heff_child_ended(HeffChild *c) {
  if (!c->on) return 1;
#ifdef _WIN32
  if (WaitForSingleObject(c->h, 0) != WAIT_OBJECT_0) return 0;
  DWORD code = (DWORD)-1;
  GetExitCodeProcess(c->h, &code);
  CloseHandle(c->h);
  c->status = (int)code;
#else
  int st;
  pid_t r = waitpid(c->pid, &st, WNOHANG);
  if (r == 0) return 0;
  c->status = r < 0 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
#endif
  c->on = 0;
  return 1;
}

// (it ended, if it has not: asked, then made to, two seconds after)
static void heff_child_end(HeffChild *c) {
  if (heff_child_ended(c)) return;
#ifdef _WIN32
  TerminateProcess(c->h, 1);
  WaitForSingleObject(c->h, INFINITE);
#else
  kill(c->pid, SIGTERM);
  for (int i = 0; i < 100 && !heff_child_ended(c); i++) {
    struct timespec t = { 0, 20000000L };
    nanosleep(&t, NULL);
  }
  if (c->on) kill(c->pid, SIGKILL);
#endif
  if (c->on) {
#ifdef _WIN32
    heff_child_ended(c);
#else
    int st;
    waitpid(c->pid, &st, 0);
    c->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    c->on = 0;
#endif
  }
}

static void heff_children_atexit(void) {
  for (HeffChild *c = heff_children; c; c = c->next) heff_child_end(c);
}

#ifndef _WIN32
// (the program ended by a signal -- SIGINT, SIGTERM, SIGHUP, where
// nothing else was to be done at it -- what it started and has not
// ended asked to end too, then the signal as it was: ended as it would
// have been)
static void heff_children_signal(int sig) {
  for (HeffChild *c = heff_children; c; c = c->next)
    if (c->on) kill(c->pid, SIGTERM);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void heff_children_watch(void) {
  static const int sigs[3] = { SIGINT, SIGTERM, SIGHUP };
  for (int i = 0; i < 3; i++) {
    struct sigaction before;
    if (sigaction(sigs[i], NULL, &before) != 0 || before.sa_handler != SIG_DFL) continue;
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = heff_children_signal;
    sigemptyset(&a.sa_mask);
    a.sa_flags = SA_RESTART;
    sigaction(sigs[i], &a, NULL);
  }
}
#endif

// (let go of: ended first, taken out of those started)
static void heff_child_let_go(void *x) {
  HeffChild *c = x;
  heff_child_end(c);
  for (HeffChild **at = &heff_children; *at; at = &(*at)->next)
    if (*at == c) { *at = c->next; break; }
  free(c);
}

static HeffChild *heff_child_of(V h) {
  return heff_held(INT_OF(heff_field(h, 0)));
}

#ifndef _WIN32
extern char **environ;
#endif

// (the command started beside the program: Just its handle, or Nothing)
static V heff_launch(V cmd, V args) {
  char *c = heff_cstr_text(cmd);
  char **as = c ? heff_argv_of(c, args) : NULL;
  if (!as) {
    free(c);
    return heff_nothing;
  }
  fflush(stdout);
  fflush(stderr);
  HeffChild *ch = heff_alloc(sizeof *ch);
  memset(ch, 0, sizeof *ch);
#ifdef _WIN32
  ch->h = heff_spawn(as, GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
  int ok = ch->h != 0;
#else
  int ok = posix_spawnp(&ch->pid, c, NULL, NULL, as, environ) == 0;
#endif
  for (int64_t j = 0; as[j]; j++) free(as[j]);
  free(as);
  if (!ok) {
    free(ch);
    return heff_nothing;
  }
  static int registered = 0;
  if (!registered) {
    registered = 1;
    atexit(heff_children_atexit);
#ifndef _WIN32
    heff_children_watch();
#endif
  }
  ch->on = 1;
  ch->next = heff_children;
  heff_children = ch;
  V i = MK_INT(heff_hold(ch, heff_child_let_go));
  return heff_just(heff_tag_n(TAG_MkHandle, 1, &i));
}

// A command run to its end (`Run`, an `Invocation`): its standard input
// the input given, written through a pipe and then closed, or none, the
// null device (not this program's, nor a closed descriptor, which the
// command's next open would take); its output and error stream read
// together, each kept up to `keep` bytes; killed (SIGKILL) at its time
// limit.  How it ended is an `Ended`.

// (an invocation, as the system is to be given it)
typedef struct {
  char **as;     // its name, then its arguments: NULL if one has a NUL
  char *dir;     // where it runs: NULL, the program's
  char **env;    // its environment: NULL, the program's
  char *in;      // its input: NULL, none
  size_t nin;
  int64_t timeout, keep;  // milliseconds, bytes: 0, no limit
} HeffInvocation;

// (the environment, `NAME=VALUE`s: the program's but those `sets` names,
// then those; malloc'd, NULL-ended -- NULL where one has a NUL)
static char **heff_env_with(char **sets, int64_t n) {
#ifdef _WIN32
  char *block = GetEnvironmentStringsA();
  int64_t ne = 0;
  for (char *e = block; *e; e += strlen(e) + 1) ne++;
#else
  int64_t ne = 0;
  while (environ[ne]) ne++;
#endif
  char **env = heff_alloc((size_t)(ne + n + 1) * sizeof(char *));
  int64_t k = 0;
#ifdef _WIN32
  for (char *e = block; *e; e += strlen(e) + 1) {
#else
  for (char **at = environ; *at; at++) {
    char *e = *at;
#endif
    const char *eq = strchr(e + (*e == '=' ? 1 : 0), '=');
    size_t len = eq ? (size_t)(eq - e) : strlen(e);
    int set = 0;
    for (int64_t i = 0; i < n && !set; i++)
#ifdef _WIN32
      set = _strnicmp(sets[i], e, len) == 0 && sets[i][len] == '=';
#else
      set = strncmp(sets[i], e, len) == 0 && sets[i][len] == '=';
#endif
    if (!set) {
      size_t l = strlen(e) + 1;
      env[k] = heff_alloc(l);
      memcpy(env[k++], e, l);
    }
  }
#ifdef _WIN32
  FreeEnvironmentStringsA(block);
#endif
  for (int64_t i = 0; i < n; i++) env[k++] = sets[i];
  env[k] = NULL;
  return env;
}

// (the invocation `v`, an `Invocation` (its fields in its type's order:
// command, args, dir, env, input, timeout, keep), as the system is to be
// given it: 0 where a string of it has a NUL in it)
static int heff_invocation(V v, HeffInvocation *r) {
  memset(r, 0, sizeof *r);
  V cmd = heff_rget_v(v, 0), args = heff_rget_v(v, 1), dir = heff_rget_v(v, 2);
  V env = heff_rget_v(v, 3), in = heff_rget_v(v, 4);
  V timeout = heff_rget_v(v, 5), keep = heff_rget_v(v, 6);
  int ok = 1;
  char *c = heff_cstr_text(cmd);
  r->as = c ? heff_argv_of(c, args) : NULL;
  if (!r->as) { free(c); ok = 0; }
  if (((Str *)dir)->len > 0) {
    r->dir = heff_cstr(dir);
    if (!r->dir) ok = 0;
  }
  int64_t n = 0;
  for (V l = env; heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1)) n++;
  if (n > 0) {
    char **sets = heff_alloc((size_t)n * sizeof(char *));
    int64_t i = 0;
    for (V l = env; heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1)) {
      V pair = heff_field(l, 0);
      char *name = heff_cstr_text(heff_field(pair, 0)), *value = heff_cstr_text(heff_field(pair, 1));
      if (name && value && *name && !strchr(name, '=')) {
        size_t a = strlen(name), b = strlen(value);
        sets[i] = heff_alloc(a + b + 2);
        memcpy(sets[i], name, a);
        sets[i][a] = '=';
        memcpy(sets[i] + a + 1, value, b + 1);
        i++;
      } else ok = 0;
      free(name);
      free(value);
    }
    r->env = heff_env_with(sets, i);
    free(sets);
  }
  if (heff_tag_of(in) == TAG_Just) r->in = heff_bytes(heff_field(in, 0), &r->nin);
  r->timeout = IS_INT(timeout) && INT_OF(timeout) > 0 ? INT_OF(timeout) : 0;
  r->keep = IS_INT(keep) && INT_OF(keep) > 0 ? INT_OF(keep) : 0;
  heff_drop(cmd);
  heff_drop(args);
  heff_drop(dir);
  heff_drop(env);
  heff_drop(in);
  heff_drop(timeout);
  heff_drop(keep);
  return ok;
}

static void heff_invocation_free(HeffInvocation *r) {
  if (r->as) {
    free(r->as[0]);
    for (int64_t j = 1; r->as[j]; j++) free(r->as[j]);
    free(r->as);
  }
  if (r->env) {
    for (int64_t j = 0; r->env[j]; j++) free(r->env[j]);
    free(r->env);
  }
  free(r->dir);
  free(r->in);
}

// (`n` bytes of what a command wrote, kept while fewer than `keep` are)
static void heff_keep_put(Buf *b, const char *c, size_t n, int64_t keep) {
  if (keep > 0) {
    if (b->n >= (size_t)keep) return;
    if (n > (size_t)keep - b->n) n = (size_t)keep - b->n;
  }
  buf_put(b, c, n);
}

// (how a command ended, as an `Ended`: `how` 0 by itself, its status
// `code`; 1 by the signal `code`; 2 at its time limit; 3 not run)
static V heff_ended(int how, int64_t code) {
  V x = MK_INT(code);
  switch (how) {
  case 0: return heff_tag_n(TAG_Exited, 1, &x);
  case 1: return heff_tag_n(TAG_Signalled, 1, &x);
  case 2: return NTAG(TAG_TimedOut);
  default: return NTAG(TAG_NotRun);
  }
}

#ifdef _WIN32
typedef struct { HANDLE h; Buf *b; int64_t keep; } HeffKeepDrain;

// (all a pipe gives, until it is closed, or the read cancelled)
static DWORD WINAPI heff_keep_drain(LPVOID u) {
  HeffKeepDrain *d = u;
  char chunk[65536];
  DWORD got;
  while (ReadFile(d->h, chunk, sizeof chunk, &got, NULL) && got > 0) heff_keep_put(d->b, chunk, got, d->keep);
  return 0;
}

typedef struct { HANDLE h; const char *s; size_t n; } HeffFeed;

// (the input written to the pipe, then the pipe closed)
static DWORD WINAPI heff_feed(LPVOID u) {
  HeffFeed *f = u;
  size_t at = 0;
  DWORD put;
  while (at < f->n) {
    DWORD want = f->n - at > 65536 ? 65536 : (DWORD)(f->n - at);
    if (!WriteFile(f->h, f->s + at, want, &put, NULL)) break;
    at += put;
  }
  CloseHandle(f->h);
  return 0;
}

// (a thread running `fn`, or, where none can be made, `fn` run here)
static HANDLE heff_thread_or_here(LPTHREAD_START_ROUTINE fn, void *u) {
  HANDLE t = CreateThread(NULL, 0, fn, u, 0, NULL);
  if (!t) fn(u);
  return t;
}

static void heff_thread_join(HANDLE t, int cancel) {
  if (!t) return;
  if (cancel) CancelSynchronousIo(t);
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}

// (the environment as CreateProcess takes it: each `NAME=VALUE` ended by
// a NUL, and a NUL after them; malloc'd)
static char *heff_env_block(char **env) {
  Buf b = {0};
  for (char **e = env; *e; e++) buf_put(&b, *e, strlen(*e) + 1);
  buf_put(&b, "", 1);
  return b.s;
}

// (the command run, as heff_spawn starts one, but with what `v` says)
static int heff_run_with(HeffInvocation *v, Buf *bo, Buf *be, int64_t *code) {
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE or_ = 0, ow = 0, er = 0, ew = 0, ir = 0, iw = 0;
  int how = 3;
  if (!CreatePipe(&or_, &ow, &sa, 0)) return 3;
  if (!CreatePipe(&er, &ew, &sa, 0)) { CloseHandle(or_); CloseHandle(ow); return 3; }
  int piped = v->in != NULL;
  if (piped ? !CreatePipe(&ir, &iw, &sa, 0)
            : (ir = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE) {
    CloseHandle(or_); CloseHandle(ow); CloseHandle(er); CloseHandle(ew);
    return 3;
  }
  SetHandleInformation(or_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(er, HANDLE_FLAG_INHERIT, 0);
  if (piped) SetHandleInformation(iw, HANDLE_FLAG_INHERIT, 0);
  char *line = heff_command_line(v->as);
  char *block = v->env ? heff_env_block(v->env) : NULL;
  STARTUPINFOA si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = ir;
  si.hStdOutput = ow;
  si.hStdError = ew;
  PROCESS_INFORMATION pi;
  int ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, block, v->dir, &si, &pi);
  free(line);
  free(block);
  CloseHandle(ow);
  CloseHandle(ew);
  CloseHandle(ir);
  HeffFeed feed = { iw, v->in, v->nin };
  HANDLE tf = 0;
  if (piped) {
    if (ok) tf = heff_thread_or_here(heff_feed, &feed);
    else CloseHandle(iw);
  }
  HeffKeepDrain dout = { or_, bo, v->keep }, derr = { er, be, v->keep };
  HANDLE to = heff_thread_or_here(heff_keep_drain, &dout);
  HANDLE te = heff_thread_or_here(heff_keep_drain, &derr);
  int timed = 0;
  if (ok) {
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, v->timeout > 0 ? (DWORD)v->timeout : INFINITE) == WAIT_TIMEOUT) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, INFINITE);
      timed = 1;
    }
    DWORD c = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hProcess);
    how = timed ? 2 : 0;
    *code = (int64_t)c;
  }
  // (what still holds the pipes, the command's own children, not waited
  // for past its time limit)
  heff_thread_join(tf, timed);
  heff_thread_join(to, timed);
  heff_thread_join(te, timed);
  CloseHandle(or_);
  CloseHandle(er);
  return how;
}
#else
// (a descriptor not given to the commands run: those of another
// command's pipes, run at once by another thread, would keep them open)
static int heff_cloexec(int fd) {
  if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

static int heff_pipe(int p[2]) {
  if (pipe(p) != 0) return 0;
  heff_cloexec(p[0]);
  heff_cloexec(p[1]);
  return 1;
}

static int64_t heff_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// (how a command ended, by waitpid's status)
static int heff_ended_how(int st, int64_t *code) {
  if (WIFEXITED(st)) { *code = WEXITSTATUS(st); return 0; }
  if (WIFSIGNALED(st)) { *code = WTERMSIG(st); return 1; }
  return 3;
}

// (the command run, forked and exec'd -- in its directory, with its
// environment, its input `in` -- its streams read: how it ended)
static int heff_run_with(HeffInvocation *v, Buf *bo, Buf *be, int64_t *code) {
  int po[2], pe[2], pi[2] = { -1, -1 }, px[2];
  if (!heff_pipe(po)) return 3;
  if (!heff_pipe(pe)) { close(po[0]); close(po[1]); return 3; }
  if (!heff_pipe(px)) { close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); return 3; }
  int piped = v->in != NULL;
  if (piped ? !heff_pipe(pi) : (pi[0] = heff_cloexec(open("/dev/null", O_RDONLY))) < 0) {
    close(po[0]); close(po[1]); close(pe[0]); close(pe[1]); close(px[0]); close(px[1]);
    return 3;
  }
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    // (the child: nothing but what is safe after a fork)
    int e;
    if (v->dir && chdir(v->dir) != 0) goto failed;
    if (dup2(pi[0], 0) < 0 || dup2(po[1], 1) < 0 || dup2(pe[1], 2) < 0) goto failed;
    if (v->env) environ = v->env;
    execvp(v->as[0], v->as);
  failed:
    e = errno;
    (void)!write(px[1], &e, sizeof e);
    _exit(127);
  }
  close(po[1]);
  close(pe[1]);
  close(px[1]);
  close(pi[0]);
  int how = 3;
  if (pid < 0) {
    close(po[0]); close(pe[0]); close(px[0]);
    if (piped) close(pi[1]);
    return 3;
  }
  // (the command's name found and run, or why not, said by the child)
  int e;
  ssize_t said;
  do said = read(px[0], &e, sizeof e); while (said < 0 && errno == EINTR);
  close(px[0]);
  if (said == (ssize_t)sizeof e) {
    close(po[0]); close(pe[0]);
    if (piped) close(pi[1]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return 3;
  }
  // (a write to an input it does not read fails, not the program by
  // SIGPIPE: held off in this thread, and taken back if it came)
  sigset_t sp, before;
  sigemptyset(&sp);
  sigaddset(&sp, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &sp, &before);
  if (piped) fcntl(pi[1], F_SETFL, fcntl(pi[1], F_GETFL) | O_NONBLOCK);
  size_t at = 0;
  if (piped && v->nin == 0) { close(pi[1]); piped = 0; }
  int64_t deadline = v->timeout > 0 ? heff_now_ms() + v->timeout : 0;
  int timed = 0, streams = 2;
  struct pollfd fds[3] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 }, { piped ? pi[1] : -1, POLLOUT, 0 } };
  char chunk[65536];
  while (streams > 0 || fds[2].fd >= 0) {
    int wait = -1;
    if (deadline) {
      int64_t left = deadline - heff_now_ms();
      if (left <= 0) { timed = 1; break; }
      wait = left > 1000000 ? 1000000 : (int)left;
    }
    int r = poll(fds, 3, wait);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t got = read(fds[i].fd, chunk, sizeof chunk);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) { close(fds[i].fd); fds[i].fd = -1; streams--; }
      else heff_keep_put(i == 0 ? bo : be, chunk, (size_t)got, v->keep);
    }
    if (fds[2].fd >= 0 && fds[2].revents) {
      ssize_t put = (fds[2].revents & POLLOUT) ? write(fds[2].fd, v->in + at, v->nin - at) : -1;
      if (put > 0) at += (size_t)put;
      if ((put < 0 && errno != EAGAIN && errno != EINTR) || at >= v->nin) {
        close(fds[2].fd);
        fds[2].fd = -1;
      }
    }
  }
  for (int i = 0; i < 3; i++) if (fds[i].fd >= 0) close(fds[i].fd);
  sigset_t pending;
  sigpending(&pending);
  if (sigismember(&pending, SIGPIPE) && !sigismember(&before, SIGPIPE)) {
    int s;
    sigwait(&sp, &s);
  }
  pthread_sigmask(SIG_SETMASK, &before, NULL);
  // (its streams closed, it ended -- waited for up to its time limit)
  int st = 0, waited = 0;
  while (!timed && deadline) {
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid || (w < 0 && errno != EINTR)) { waited = w == pid; break; }
    if (heff_now_ms() >= deadline) timed = 1;
    else {
      struct timespec t = { 0, 5000000L };
      nanosleep(&t, NULL);
    }
  }
  if (timed) kill(pid, SIGKILL);
  if (!waited) {
    pid_t w;
    while ((w = waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
    if (w != pid) return 3;
  }
  how = timed ? 2 : heff_ended_how(st, code);
  return how;
}
#endif

static uint32_t heff_run_shape[3] = { LBL_ended, LBL_out, LBL_err };

// (a command run, as an `Invocation` says: a `Ran`)
static V heff_run(V inv) {
  HeffInvocation v;
  Buf bo = {0}, be = {0};
  int64_t code = 0;
  int how = heff_invocation(inv, &v) ? heff_run_with(&v, &bo, &be, &code) : 3;
  heff_invocation_free(&v);
  V f[3] = {
    heff_ended(how, code),
    heff_str_bytes(bo.s ? bo.s : "", (int64_t)bo.n),
    heff_str_bytes(be.s ? be.s : "", (int64_t)be.n),
  };
  free(bo.s);
  free(be.s);
  return heff_rec(3, heff_run_shape, f);
}

static V heff_provider_process(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Args: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_strs_text(heff_argv + 1, heff_argc - 1));
    return heff_tail(k, 1, &x);
  }
  case TAG_GetEnv: {
    char *name = heff_cstr_text(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    const char *v = name ? getenv(name) : NULL;
    free(name);
    V x = heff_answer(f, v ? heff_just(heff_str_text(v)) : heff_nothing);
    return heff_tail(k, 1, &x);
  }
  case TAG_ProgramPath: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_program_path());
    return heff_tail(k, 1, &x);
  }
  case TAG_WorkingDir: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    char buf[4096];
#ifdef _WIN32
    const char *d = _getcwd(buf, sizeof buf);
#else
    const char *d = getcwd(buf, sizeof buf);
#endif
    V x = heff_answer(f, heff_str_text(d ? d : "."));
    return heff_tail(k, 1, &x);
  }
  case TAG_Platform: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
#if defined(__APPLE__)
    const char *pl = "darwin";
#elif defined(_WIN32)
    const char *pl = "win32";
#else
    const char *pl = "linux";
#endif
    V x = heff_answer(f, heff_str_text(pl));
    return heff_tail(k, 1, &x);
  }
  case TAG_Exit: {
    int code = (int)INT_OF(heff_field(p, 0));
    fflush(stdout);
    fflush(stderr);
    exit(code);
  }
  case TAG_Run: {
    V inv = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V r = heff_run(inv);
    heff_drop(inv);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Launch: {
    V cmd = heff_dup(heff_field(p, 0)), args = heff_dup(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V r = heff_launch(cmd, args);
    heff_drop(cmd);
    heff_drop(args);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Poll: {
    HeffChild *c = heff_child_of(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_child_ended(c) ? heff_just(MK_INT(c->status)) : heff_nothing);
    return heff_tail(k, 1, &x);
  }
  case TAG_Terminate: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    HeffChild *c = heff_held(i);
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    heff_child_end(c);
    int status = c->status;
    heff_release(i);
    V x = heff_answer(f, MK_INT(status));
    return heff_tail(k, 1, &x);
  }
  case TAG_Exec: {
    char *c = heff_cstr_text(heff_field(p, 0));
    char **as = c ? heff_argv_of(c, heff_field(p, 1)) : NULL;
    // (std.progress's lines, if any are drawn, cleared for good: the
    // command has the terminal now)
    if (heff_lines) heff_lines(2);
    fflush(stdout);
    fflush(stderr);
    if (as) {
#ifdef _WIN32
      // (no exec on Windows: the command is run, and its exit code this
      // program's)
      HANDLE h = heff_spawn(as, GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
      if (h) exit((int)heff_wait(h));
#else
      execvp(c, as);
#endif
    }
    fprintf(stderr, "%s: cannot run it\n", c ? c : "the command");
    exit(1);
  }
  default:
    heff_fail("not an operation of process");
    return 0;
  }
}
