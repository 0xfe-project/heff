// The process's side of std.process, for the C backend: an effect
// provider.
// `Exit` ends the program there; `Exec` runs the command in its place.
// A command `Start`ed runs beside it, the host's own thing -- a foreign
// value, a `Started`, held by a `with` -- until it is waited for or
// its `with` ends (`Release`: ended, if it has not, and waited for);
// those not ended at the program's end are terminated then (atexit),
// and asked to end where a signal ends it.  A wait on one started in a
// group of std.reactor's is pending while the other threads go on: this
// provider is a source of the reactor's.

#ifdef _WIN32
#include <direct.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#define HEFF_KQUEUE 1
#elif defined(__linux__)
#include <sys/syscall.h>
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
// `Exit` clears them as the program ends)
static void (*heff_lines)(int);

#ifndef _WIN32
extern char **environ;
#endif

// A command run to its end (`Run`, an `Invocation`): its standard input
// the input given, written through a pipe and then closed, or none, the
// null device (not this program's, nor a closed descriptor, which the
// command's next open would take); its output and error stream read
// together, each kept up to `keep` bytes; killed (SIGKILL) at its time
// limit.  How it ended is an `Ended`.

// (what a command's input is: none, the null device; a string given,
// written to it; a pipe the program writes to; the program's)
enum { HEFF_IN_NONE, HEFF_IN_GIVEN, HEFF_IN_PIPED, HEFF_IN_INHERITED };

// (what its output or its error stream is: collected, read and kept; a
// pipe the program reads; the program's)
enum { HEFF_OUT_COLLECTED, HEFF_OUT_PIPED, HEFF_OUT_INHERITED };

// (an invocation, as the system is to be given it)
typedef struct {
  char **as;     // its name, then its arguments: NULL if one has a NUL
  char *dir;     // where it runs: the thread's, unless it says
  char **env;    // its environment: NULL, the program's
  int in_mode;   // HEFF_IN_*: its input
  char *in;      // the input `Given`: NULL, none
  size_t nin;
  int out_mode, err_mode;  // HEFF_OUT_*: its output, its error stream
  int64_t timeout, keep;  // milliseconds, bytes: 0, no limit
  int group;     // started in a process group of its own, ended with it
  V command;     // its name, and its directory, as the program gave them
  V given_dir;   //  (a failure's: 0, none)
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

// (an `Output` as a mode)
static int heff_output_mode(V o) {
  switch (heff_tag_of(o)) {
  case TAG_Piped: return HEFF_OUT_PIPED;
  case TAG_Inherited: return HEFF_OUT_INHERITED;
  default: return HEFF_OUT_COLLECTED;
  }
}

// (the invocation `v`, an `Invocation` (its fields in its type's order:
// command, args, dir, env, input, output, errors, timeout, keep, group), as the
// system is to be given it: 0 where a string of it has a NUL in it)
static int heff_invocation(V v, HeffInvocation *r) {
  memset(r, 0, sizeof *r);
  V cmd = heff_rget_v(v, 0), args = heff_rget_v(v, 1), dir = heff_rget_v(v, 2);
  V env = heff_rget_v(v, 3), in = heff_rget_v(v, 4);
  V out = heff_rget_v(v, 5), err = heff_rget_v(v, 6);
  V timeout = heff_rget_v(v, 7), keep = heff_rget_v(v, 8);
  V group = heff_rget_v(v, 9);
  int ok = 1;
  char *c = heff_cstr_text(cmd);
  r->as = c ? heff_argv_of(c, args) : NULL;
  if (!r->as) { free(c); ok = 0; }
  // (where it runs: from this thread's directory, or that itself)
  if (((Str *)dir)->len > 0) {
    r->dir = heff_path_in_dir(heff_cstr(dir));
    if (!r->dir) ok = 0;
  } else r->dir = heff_dir_copy();
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
  switch (heff_tag_of(in)) {
  case TAG_Given:
    r->in_mode = HEFF_IN_GIVEN;
    r->in = heff_bytes(heff_field(in, 0), &r->nin);
    break;
  case TAG_Piped: r->in_mode = HEFF_IN_PIPED; break;
  case TAG_Inherited: r->in_mode = HEFF_IN_INHERITED; break;
  default: r->in_mode = HEFF_IN_NONE;
  }
  r->out_mode = heff_output_mode(out);
  r->err_mode = heff_output_mode(err);
  r->timeout = IS_INT(timeout) && INT_OF(timeout) > 0 ? INT_OF(timeout) : 0;
  r->keep = IS_INT(keep) && INT_OF(keep) > 0 ? INT_OF(keep) : 0;
  r->group = group == NTAG(TAG_True);
  r->command = cmd;
  r->given_dir = dir;
  heff_drop(args);
  heff_drop(env);
  heff_drop(in);
  heff_drop(out);
  heff_drop(err);
  heff_drop(timeout);
  heff_drop(keep);
  heff_drop(group);
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
  if (r->command) heff_drop(r->command);
  if (r->given_dir) heff_drop(r->given_dir);
}

// (`n` bytes of what a command wrote, kept while fewer than `keep` are)
static void heff_keep_put(Buf *b, const char *c, size_t n, int64_t keep) {
  if (keep > 0) {
    if (b->n >= (size_t)keep) return;
    if (n > (size_t)keep - b->n) n = (size_t)keep - b->n;
  }
  buf_put(b, c, n);
}

static uint32_t heff_error_shape[4] = { LBL_code, LBL_name, LBL_message, LBL_path };

// (a `ProcessError` of errno `err`, of the program or the directory
// `path`, taken)
static V heff_process_error(int err, V path) {
  const char *name = heff_errno_name(err);
  char number[24];
  if (name[1] == '?') {
    snprintf(number, sizeof number, "errno %d", err);
    name = number;
  }
  V f[4] = { heff_int(err), heff_str_text(name), heff_str_text(strerror(err)), path };
  return heff_rec(4, heff_error_shape, f);
}

// (why the invocation `v` could not be run: errno `err`, of its
// directory, `at_dir`, or its command -- as the program gave them)
static V heff_why_not(HeffInvocation *v, int err, int at_dir) {
  V path;
  if (at_dir && v->given_dir && ((Str *)v->given_dir)->len > 0) path = heff_dup(v->given_dir);
  else if (at_dir && v->dir) path = heff_str_bytes(v->dir, (int64_t)strlen(v->dir));
  else path = v->command ? heff_dup(v->command) : heff_str_bytes("", 0);
  return heff_process_error(err, path);
}

// (how a command ended, as an `Ended`: `how` 0 by itself, its status
// `code`; 1 by the signal `code`; 2 at its time limit; 3 started, its
// end lost -- the command named `name`'s, ECHILD)
static V heff_ended(int how, int64_t code, const char *name) {
  V x = MK_INT(code);
  switch (how) {
  case 0: return heff_tag_n(TAG_Exited, 1, &x);
  case 1: return heff_tag_n(TAG_Signalled, 1, &x);
  case 2: return NTAG(TAG_TimedOut);
  default:
    x = heff_process_error(ECHILD, heff_str_bytes(name, (int64_t)strlen(name)));
    return heff_tag_n(TAG_NotRun, 1, &x);
  }
}

// Commands started (`Start`; `Run` is one started and waited for at
// once): each a `HeffSub`, the pointer of a foreign value (a `Started`)
// until the program lets go of it.  What it writes to a pipe is read
// into its buffers, what it is given written to its input, and its time
// limit kept, whenever the program waits on it -- `heff_round`, over
// those waited on.  (On Windows, a thread reads each of its pipes, and
// one writes its input.)  It has ended once it has exited and its pipes
// are closed, read to their end -- or once its time limit has passed:
// then killed, and its pipes closed, not even what its own children
// still write waited for.

// (a `HeffSub`'s mark: what a foreign value given an operation must be)
#define HEFF_SUB_MAGIC 0x53554250u

typedef struct HeffSub HeffSub;

// (an output stream of a command's: what it is, its pipe while open,
// what has come of it)
typedef struct {
  int mode;  // HEFF_OUT_*
#ifdef _WIN32
  HANDLE h, thread;  // its pipe, and the thread reading it: 0, none
  volatile LONG closed;
  HeffSub *owner;
#else
  int fd;  // -1: closed, or none
#endif
  Buf b;
  size_t at;  // what is read of `b` (a `Piped` stream's): taken before it
} HeffStream;

struct HeffSub {
  uint32_t magic;  // HEFF_SUB_MAGIC
#ifdef _WIN32
  HANDLE h;        // the process
  HANDLE feedh;    // the pipe its `Given` input is written to, by `feeder`
  HANDLE feeder;
  HANDLE fin;      // its input's pipe, `Piped`, while open: 0, none
  HANDLE came;     // set as something comes on a stream, or it ends
  CRITICAL_SECTION lock;  // its streams' buffers, the threads writing them
  int exited;
  DWORD code;
#else
  pid_t pid;
  int group;       // the leader of a process group of its own, `pid`
  int reaped;      // 0: not yet; 1: reaped, `st` its status; 2: lost
  int st;
  int fin;         // its input's pipe, while open: -1, none
  size_t at;       // how much of `given` is written
  int exitfd;      // what says it exited, readable then: -1, nothing does
  int exit_tried;  // (whether `exitfd` was asked for)
#endif
  int in_mode;     // HEFF_IN_*
  char *given;     // its input, `Given`: NULL, none
  size_t ngiven;
  HeffStream out, err;
  int64_t keep, deadline;  // bytes, the time it is killed at: 0, none
  int timed;
  char *name;      // its command's name, as it was given
  HeffSub *next;   // among those started, to be ended at the program's end
};

// (`n` bytes come on the stream: kept, as its mode says -- those
// `Collected` up to `keep`, those `Piped` until they are read)
static void heff_stream_put(HeffStream *st, const char *c, size_t n, int64_t keep) {
  if (st->mode == HEFF_OUT_PIPED) buf_put(&st->b, c, n);
  else heff_keep_put(&st->b, c, n, keep);
}

#ifdef _WIN32
static SRWLOCK heff_subs_lock = SRWLOCK_INIT;
#define HEFF_SUBS_LOCK() AcquireSRWLockExclusive(&heff_subs_lock)
#define HEFF_SUBS_UNLOCK() ReleaseSRWLockExclusive(&heff_subs_lock)

// (all a pipe gives, until it is closed, or the read cancelled)
static DWORD WINAPI heff_stream_drain(LPVOID u) {
  HeffStream *st = u;
  HeffSub *s = st->owner;
  char chunk[65536];
  DWORD got;
  while (ReadFile(st->h, chunk, sizeof chunk, &got, NULL) && got > 0) {
    EnterCriticalSection(&s->lock);
    heff_stream_put(st, chunk, got, s->keep);
    LeaveCriticalSection(&s->lock);
    SetEvent(s->came);
  }
  InterlockedExchange(&st->closed, 1);
  SetEvent(s->came);
  return 0;
}

// (its input written to the pipe, then the pipe closed)
static DWORD WINAPI heff_sub_feeder(LPVOID u) {
  HeffSub *s = u;
  size_t at = 0;
  DWORD put;
  while (at < s->ngiven) {
    DWORD want = s->ngiven - at > 65536 ? 65536 : (DWORD)(s->ngiven - at);
    if (!WriteFile(s->feedh, s->given + at, want, &put, NULL)) break;
    at += put;
  }
  CloseHandle(s->feedh);
  return 0;
}

// (a thread running `fn`, or, where none can be made, `fn` run here)
static HANDLE heff_thread_or_here(LPTHREAD_START_ROUTINE fn, void *u) {
  HANDLE t = CreateThread(NULL, 0, fn, u, 0, NULL);
  if (!t) fn(u);
  return t;
}

// (the thread ended: waited for, its read or write cancelled first
// where `cancel` says, again until it has ended)
static void heff_thread_join(HANDLE *t, int cancel) {
  if (!*t) return;
  if (cancel)
    do CancelSynchronousIo(*t);
    while (WaitForSingleObject(*t, 50) == WAIT_TIMEOUT);
  WaitForSingleObject(*t, INFINITE);
  CloseHandle(*t);
  *t = 0;
}

static int heff_stream_closed(HeffStream *st) {
  return !st->thread || st->closed;
}

// (whether it has ended: exited, its pipes read to their end)
static int heff_sub_done(HeffSub *s) {
  if (!s->exited) {
    if (WaitForSingleObject(s->h, 0) != WAIT_OBJECT_0) return 0;
    GetExitCodeProcess(s->h, &s->code);
    s->exited = 1;
  }
  return heff_stream_closed(&s->out) && heff_stream_closed(&s->err);
}

// (ended now: killed, if it has not exited, and its pipes no more read)
static void heff_sub_cut(HeffSub *s) {
  if (!s->exited) {
    TerminateProcess(s->h, 1);
    WaitForSingleObject(s->h, INFINITE);
    GetExitCodeProcess(s->h, &s->code);
    s->exited = 1;
  }
  heff_thread_join(&s->out.thread, 1);
  heff_thread_join(&s->err.thread, 1);
  heff_thread_join(&s->feeder, 1);
}

// (past its time limit, and not ended: cut short)
static void heff_sub_expire(HeffSub *s, int64_t now) {
  if (!s->deadline || now < s->deadline || heff_sub_done(s)) return;
  heff_sub_cut(s);
  s->timed = 1;
}

// (one round over the commands `ss`: waited for up to `ms` (-1: as long
// as it takes) for one to exit, those past their time limit cut short)
static void heff_round(HeffSub **ss, int n, int64_t ms) {
  HANDLE hs[MAXIMUM_WAIT_OBJECTS];
  DWORD k = 0;
  int64_t now = heff_now_ms();
  for (int i = 0; i < n; i++) {
    HeffSub *s = ss[i];
    if (heff_sub_done(s)) { ms = 0; continue; }
    if (!s->exited && k < MAXIMUM_WAIT_OBJECTS) hs[k++] = s->h;
    else if (ms < 0 || ms > 5) ms = 5;
    if (s->deadline) {
      int64_t left = s->deadline > now ? s->deadline - now : 0;
      if (ms < 0 || left < ms) ms = left;
    }
  }
  if (k) WaitForMultipleObjects(k, hs, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
  else if (ms != 0) Sleep(ms < 0 ? 5 : (DWORD)ms);
  now = heff_now_ms();
  for (int i = 0; i < n; i++) heff_sub_expire(ss[i], now);
}

// (the environment as CreateProcess takes it: each `NAME=VALUE` ended by
// a NUL, and a NUL after them; malloc'd)
static char *heff_env_block(char **env) {
  Buf b = {0};
  for (char **e = env; *e; e++) buf_put(&b, *e, strlen(*e) + 1);
  buf_put(&b, "", 1);
  return b.s;
}

// (an output stream's pipe made, where it has one: its read end the
// stream's, its write end the command's; else the program's own `std`)
static int heff_stream_pipe(HeffStream *st, SECURITY_ATTRIBUTES *sa, HANDLE *w, DWORD std) {
  if (st->mode == HEFF_OUT_INHERITED) {
    *w = GetStdHandle(std);
    return 1;
  }
  if (!CreatePipe(&st->h, w, sa, 0)) return 0;
  SetHandleInformation(st->h, HANDLE_FLAG_INHERIT, 0);
  return 1;
}

static void heff_sub_free(HeffSub *s) {
  heff_thread_join(&s->out.thread, 0);
  heff_thread_join(&s->err.thread, 0);
  heff_thread_join(&s->feeder, 0);
  if (s->out.h) CloseHandle(s->out.h);
  if (s->err.h) CloseHandle(s->err.h);
  if (s->h) CloseHandle(s->h);
  if (s->fin) CloseHandle(s->fin);
  if (s->came) CloseHandle(s->came);
  DeleteCriticalSection(&s->lock);
  free(s->out.b.s);
  free(s->err.b.s);
  free(s->given);
  free(s->name);
  free(s);
}

// (the command started, as `v` says: NULL where it could not be, why in
// `*err` -- its directory's, `*at_dir`)
static HeffSub *heff_sub_spawn(HeffInvocation *v, int *err, int *at_dir) {
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HeffSub *s = heff_alloc(sizeof *s);
  memset(s, 0, sizeof *s);
  s->magic = HEFF_SUB_MAGIC;
  s->name = heff_strdup(v->as[0]);
  InitializeCriticalSection(&s->lock);
  s->came = CreateEventA(NULL, FALSE, FALSE, NULL);
  s->in_mode = v->in_mode;
  s->out.owner = s->err.owner = s;
  s->out.mode = v->out_mode;
  s->err.mode = v->err_mode;
  HANDLE ow = 0, ew = 0, ir = 0, iw = 0;
  int ok = heff_stream_pipe(&s->out, &sa, &ow, STD_OUTPUT_HANDLE)
           && heff_stream_pipe(&s->err, &sa, &ew, STD_ERROR_HANDLE);
  if (ok && (v->in_mode == HEFF_IN_GIVEN || v->in_mode == HEFF_IN_PIPED)) {
    ok = CreatePipe(&ir, &iw, &sa, 0);
    if (ok) SetHandleInformation(iw, HANDLE_FLAG_INHERIT, 0);
  } else if (ok && v->in_mode == HEFF_IN_NONE) {
    ir = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    ok = ir != INVALID_HANDLE_VALUE;
    if (!ok) ir = 0;
  }
  PROCESS_INFORMATION pi;
  if (ok) {
    char *line = heff_command_line(v->as);
    char *block = v->env ? heff_env_block(v->env) : NULL;
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = v->in_mode == HEFF_IN_INHERITED ? GetStdHandle(STD_INPUT_HANDLE) : ir;
    si.hStdOutput = ow;
    si.hStdError = ew;
    fflush(stdout);
    fflush(stderr);
    ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, block, v->dir, &si, &pi);
    free(line);
    free(block);
  }
  // (the command's ends of its pipes, its now)
  if (s->out.h && ow) CloseHandle(ow);
  if (s->err.h && ew) CloseHandle(ew);
  if (ir) CloseHandle(ir);
  if (!ok) {
    DWORD last = GetLastError();
    *at_dir = last == ERROR_DIRECTORY;
    *err = last == ERROR_FILE_NOT_FOUND || last == ERROR_DIRECTORY ? ENOENT : EACCES;
    if (iw) CloseHandle(iw);
    heff_sub_free(s);
    return NULL;
  }
  CloseHandle(pi.hThread);
  s->h = pi.hProcess;
  s->keep = v->keep;
  s->deadline = v->timeout > 0 ? heff_now_ms() + v->timeout : 0;
  if (s->out.h) s->out.thread = heff_thread_or_here(heff_stream_drain, &s->out);
  if (s->err.h) s->err.thread = heff_thread_or_here(heff_stream_drain, &s->err);
  if (iw && v->in_mode == HEFF_IN_PIPED) s->fin = iw;
  else if (iw) {
    s->given = v->in;
    s->ngiven = v->nin;
    v->in = NULL;
    s->feedh = iw;
    s->feeder = heff_thread_or_here(heff_sub_feeder, s);
  }
  return s;
}

// (how it ended: `how` as heff_ended takes it, its status `code`)
static int heff_sub_how(HeffSub *s, int64_t *code) {
  *code = (int64_t)s->code;
  return s->timed ? 2 : 0;
}

static void heff_sub_kill(HeffSub *s) {
  if (!heff_sub_done(s) && !s->exited) TerminateProcess(s->h, 1);
}

// (it ended, if it has not: made to at once, as Windows has no asking)
static void heff_sub_end(HeffSub *s) {
  if (!heff_sub_done(s)) heff_sub_cut(s);
}
#else
static pthread_mutex_t heff_subs_mu = PTHREAD_MUTEX_INITIALIZER;
#define HEFF_SUBS_LOCK() pthread_mutex_lock(&heff_subs_mu)
#define HEFF_SUBS_UNLOCK() pthread_mutex_unlock(&heff_subs_mu)

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

static void heff_close(int *fd) {
  if (*fd >= 0) close(*fd);
  *fd = -1;
}

static int heff_nonblock(int fd) {
  if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

// (how a command ended, by waitpid's status)
static int heff_ended_how(int st, int64_t *code) {
  if (WIFEXITED(st)) { *code = WEXITSTATUS(st); return 0; }
  if (WIFSIGNALED(st)) { *code = WTERMSIG(st); return 1; }
  return 3;
}

// (a write to an input not read fails, not the program by SIGPIPE: held
// off in this thread while it is written to, and taken back if it came)
static void heff_nopipe(sigset_t *before) {
  sigset_t sp;
  sigemptyset(&sp);
  sigaddset(&sp, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &sp, before);
}

static void heff_nopipe_end(const sigset_t *before) {
  sigset_t pending, sp;
  sigemptyset(&sp);
  sigaddset(&sp, SIGPIPE);
  sigpending(&pending);
  if (sigismember(&pending, SIGPIPE) && !sigismember(before, SIGPIPE)) {
    int s;
    sigwait(&sp, &s);
  }
  pthread_sigmask(SIG_SETMASK, before, NULL);
}

// (as much of what is to be written to its input as its pipe takes now,
// written: the pipe closed where it is read no more, or, `Given`, once
// all of it is)
static void heff_sub_feed(HeffSub *s) {
  while (s->fin >= 0 && s->at < s->ngiven) {
    ssize_t put = write(s->fin, s->given + s->at, s->ngiven - s->at);
    if (put > 0) s->at += (size_t)put;
    else if (put < 0 && errno == EINTR) continue;
    else if (put < 0 && errno == EAGAIN) return;
    else {
      heff_close(&s->fin);
      return;
    }
  }
  if (s->in_mode == HEFF_IN_GIVEN) heff_close(&s->fin);
}

// (what has come on the stream, read: closed at its end)
static void heff_stream_read(HeffSub *s, HeffStream *st) {
  char chunk[65536];
  ssize_t got = read(st->fd, chunk, sizeof chunk);
  if (got < 0 && (errno == EINTR || errno == EAGAIN)) return;
  if (got <= 0) heff_close(&st->fd);
  else heff_stream_put(st, chunk, (size_t)got, s->keep);
}

// (whether it has exited, reaped where it has: waiting, where `block`)
static void heff_sub_reap(HeffSub *s, int block) {
  if (s->reaped) return;
  int st;
  pid_t w;
  while ((w = waitpid(s->pid, &st, block ? 0 : WNOHANG)) < 0 && errno == EINTR) {}
  if (w == s->pid) {
    s->reaped = 1;
    s->st = st;
  } else if (w < 0) s->reaped = 2;
}

// (whether it has ended: exited, its pipes read to their end)
static int heff_sub_done(HeffSub *s) {
  return s->reaped && s->out.fd < 0 && s->err.fd < 0;
}

// (the signal sent to it -- to its process group, the group's own --
// if it has not been reaped)
static void heff_sub_signal(HeffSub *s, int sig) {
  if (!s->reaped) kill(s->group ? -s->pid : s->pid, sig);
}

// (ended now: killed, if it has not exited, and its pipes closed; of a
// group's own, what is left of the group killed too)
static void heff_sub_cut(HeffSub *s) {
  if (s->group) kill(-s->pid, SIGKILL);
  if (!s->reaped) {
    kill(s->pid, SIGKILL);
    heff_sub_reap(s, 1);
  }
  heff_close(&s->out.fd);
  heff_close(&s->err.fd);
  heff_close(&s->fin);
}

// (past its time limit, and not ended: cut short)
static void heff_sub_expire(HeffSub *s, int64_t now) {
  if (!s->deadline || now < s->deadline || heff_sub_done(s)) return;
  heff_sub_cut(s);
  s->timed = 1;
}

// (a descriptor that says when the command exits, readable then: a
// kqueue's, its NOTE_EXIT (macOS, the BSDs), or a pidfd (Linux); -1
// where the system has neither, or it has exited already -- then it is
// looked at every 5 ms until it is reaped.  Asked for once.)
static int heff_sub_exit_fd(HeffSub *s) {
  if (s->exit_tried) return s->exitfd;
  s->exit_tried = 1;
#if defined(HEFF_KQUEUE)
  int kq = kqueue();
  if (kq >= 0) {
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)s->pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) == 0) s->exitfd = heff_cloexec(kq);
    else close(kq);
  }
#elif defined(__linux__) && defined(SYS_pidfd_open)
  int fd = (int)syscall(SYS_pidfd_open, s->pid, 0);
  if (fd >= 0) s->exitfd = heff_cloexec(fd);
#endif
  return s->exitfd;
}

// (a descriptor to poll, put at `fds[*k]` where there is room, counted
// either way)
static void heff_polled_put(struct pollfd *fds, int room, int *k, int fd, short events) {
  if (fd < 0) return;
  if (*k < room) fds[*k] = (struct pollfd){ fd, events, 0 };
  (*k)++;
}

// (what is to be polled for the command -- its pipes, to read or to
// write; what says it exited -- put at `fds`, `*k` on, where there is
// room; and how long at most to wait for them, `*ms` (-1: as long as it
// takes): its time limit, 5 ms where nothing says when it exits, none
// where it has ended)
static void heff_sub_polled(HeffSub *s, struct pollfd *fds, int room, int *k, int64_t now, int64_t *ms) {
  if (heff_sub_done(s)) { *ms = 0; return; }
  heff_polled_put(fds, room, k, s->out.fd, POLLIN);
  heff_polled_put(fds, room, k, s->err.fd, POLLIN);
  if (s->fin >= 0 && s->at < s->ngiven) heff_polled_put(fds, room, k, s->fin, POLLOUT);
  if (!s->reaped) {
    int x = heff_sub_exit_fd(s);
    if (x >= 0) heff_polled_put(fds, room, k, x, POLLIN);
    else if (*ms < 0 || *ms > 5) *ms = 5;
  }
  if (s->deadline) {
    int64_t left = s->deadline > now ? s->deadline - now : 0;
    if (*ms < 0 || left < *ms) *ms = left;
  }
}

// (what has come for the command, taken, nothing waited for: what it
// wrote read, what it is given written, reaped where it has exited, cut
// short where it is past its time limit)
static void heff_sub_came(HeffSub *s) {
  sigset_t before;
  heff_nopipe(&before);
  if (s->out.fd >= 0) heff_stream_read(s, &s->out);
  if (s->err.fd >= 0) heff_stream_read(s, &s->err);
  if (s->fin >= 0 && s->at < s->ngiven) heff_sub_feed(s);
  heff_nopipe_end(&before);
  heff_sub_reap(s, 0);
#if defined(HEFF_KQUEUE)
  // (the kqueue said it exited, and waitpid has not yet: the kqueue let
  // go of, so that it is not said again and again; looked at every 5 ms)
  if (!s->reaped && s->exitfd >= 0) {
    struct kevent ev;
    struct timespec none = { 0, 0 };
    if (kevent(s->exitfd, NULL, 0, &ev, 1, &none) > 0) heff_close(&s->exitfd);
  }
#endif
  heff_sub_expire(s, heff_now_ms());
}

// (one round over the commands `ss`: waited up to `ms` (-1: as long as
// it takes) for anything to come for any of them, then what came taken)
static void heff_round(HeffSub **ss, int n, int64_t ms) {
  int room = 4 * n + 1;
  struct pollfd *fds = heff_alloc((size_t)room * sizeof *fds);
  int k = 0;
  int64_t now = heff_now_ms();
  for (int i = 0; i < n; i++) heff_sub_polled(ss[i], fds, room, &k, now, &ms);
  if (ms > 1000000) ms = 1000000;
  poll(fds, (nfds_t)k, (int)ms);
  free(fds);
  for (int i = 0; i < n; i++) heff_sub_came(ss[i]);
}

static void heff_sub_free(HeffSub *s) {
  heff_close(&s->out.fd);
  heff_close(&s->err.fd);
  heff_close(&s->fin);
  heff_close(&s->exitfd);
  free(s->out.b.s);
  free(s->err.b.s);
  free(s->given);
  free(s->name);
  free(s);
}

// (the signals that end a program, held back by `heff_start` from
// before a command is started until it is among those ended with the
// program -- else one coming between would leave it running -- and the
// signals as they were held before)
static const int heff_ending[3] = { SIGINT, SIGTERM, SIGHUP };
static HEFF_TLS int heff_spawn_held = 0;
static HEFF_TLS sigset_t heff_spawn_mask;

// (the command started, forked and exec'd as `v` says -- in its
// directory, with its environment, its input -- or NULL where it could
// not be: not found, not a program, its directory not there, as the
// child says through a pipe of its own; why in `*err`, its directory's
// where `*at_dir`)
static HeffSub *heff_sub_spawn(HeffInvocation *v, int *err, int *at_dir) {
  HeffSub *s = heff_alloc(sizeof *s);
  memset(s, 0, sizeof *s);
  s->magic = HEFF_SUB_MAGIC;
  s->name = heff_strdup(v->as[0]);
  s->out.fd = s->err.fd = s->fin = s->exitfd = -1;
  s->in_mode = v->in_mode;
  s->out.mode = v->out_mode;
  s->err.mode = v->err_mode;
  int po[2] = { -1, -1 }, pe[2] = { -1, -1 }, pi[2] = { -1, -1 }, px[2] = { -1, -1 };
  int ok = heff_pipe(px);
  if (ok && v->out_mode != HEFF_OUT_INHERITED) ok = heff_pipe(po);
  if (ok && v->err_mode != HEFF_OUT_INHERITED) ok = heff_pipe(pe);
  if (ok && (v->in_mode == HEFF_IN_GIVEN || v->in_mode == HEFF_IN_PIPED)) ok = heff_pipe(pi);
  else if (ok && v->in_mode == HEFF_IN_NONE) ok = (pi[0] = heff_cloexec(open("/dev/null", O_RDONLY))) >= 0;
  pid_t pid = -1;
  if (ok) {
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
      // (the child: nothing but what is safe after a fork; why it failed
      // said, its errno and whether its directory's)
      int e[2] = { 0, 0 };
      if (heff_spawn_held) {
        // (the program's handlers of the ending signals not the
        // command's, and the signals held as the program held them)
        for (int i = 0; i < 3; i++) {
          struct sigaction was;
          if (sigaction(heff_ending[i], NULL, &was) == 0 && was.sa_handler != SIG_IGN)
            signal(heff_ending[i], SIG_DFL);
        }
        sigprocmask(SIG_SETMASK, &heff_spawn_mask, NULL);
      }
      if (v->group) setpgid(0, 0);
      if (chdir(v->dir) != 0) {
        e[1] = 1;
        goto failed;
      }
      if ((pi[0] >= 0 && dup2(pi[0], 0) < 0) || (po[1] >= 0 && dup2(po[1], 1) < 0)
          || (pe[1] >= 0 && dup2(pe[1], 2) < 0))
        goto failed;
      if (v->env) environ = v->env;
      execvp(v->as[0], v->as);
    failed:
      e[0] = errno;
      (void)!write(px[1], e, sizeof e);
      _exit(127);
    }
  }
  // (its group made here too, so that it is there before it is killed)
  if (pid > 0 && v->group) setpgid(pid, pid);
  heff_close(&pi[0]);
  heff_close(&po[1]);
  heff_close(&pe[1]);
  heff_close(&px[1]);
  if (pid > 0) {
    // (the command's name found and run, or why not, said by the child)
    int e[2];
    ssize_t said;
    do said = read(px[0], e, sizeof e); while (said < 0 && errno == EINTR);
    if (said == (ssize_t)sizeof e) {
      int st;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
      pid = -1;
      errno = e[0];
      *at_dir = e[1];
    }
  }
  *err = errno ? errno : EINVAL;
  heff_close(&px[0]);
  if (pid <= 0) {
    heff_close(&po[0]);
    heff_close(&pe[0]);
    heff_close(&pi[1]);
    heff_sub_free(s);
    return NULL;
  }
  s->pid = pid;
  s->group = v->group;
  // (its pipes read as what has come, never waiting on one: a read of
  // nothing is nothing)
  s->out.fd = heff_nonblock(po[0]);
  s->err.fd = heff_nonblock(pe[0]);
  s->fin = pi[1];
  s->keep = v->keep;
  s->deadline = v->timeout > 0 ? heff_now_ms() + v->timeout : 0;
  if (s->fin >= 0) {
    fcntl(s->fin, F_SETFL, fcntl(s->fin, F_GETFL) | O_NONBLOCK);
#ifdef F_SETNOSIGPIPE
    // (macOS: SIGPIPE is the process's, not the thread's, there -- not
    // held off by a thread's mask: none sent for this pipe at all)
    fcntl(s->fin, F_SETNOSIGPIPE, 1);
#endif
    if (v->in_mode == HEFF_IN_GIVEN) {
      s->given = v->in;
      s->ngiven = v->nin;
      v->in = NULL;
      // (what its pipe takes now, written at once: a short input is all
      // given before it is waited on)
      sigset_t before;
      heff_nopipe(&before);
      heff_sub_feed(s);
      heff_nopipe_end(&before);
    }
  }
  return s;
}

// (how it ended: `how` as heff_ended takes it, its status or signal
// `code`)
static int heff_sub_how(HeffSub *s, int64_t *code) {
  if (s->timed) return 2;
  return s->reaped == 1 ? heff_ended_how(s->st, code) : 3;
}

static void heff_sub_kill(HeffSub *s) {
  heff_sub_signal(s, SIGKILL);
}
#endif

// (the first of the commands `ss` to end, waited for up to `ms`
// milliseconds (-1: as long as it takes; 0: not at all): its index, or
// -1 where none has ended)
static int heff_await(HeffSub **ss, int n, int64_t ms) {
  int64_t until = ms > 0 ? heff_now_ms() + ms : 0;
  for (int first = 1;; first = 0) {
    for (int i = 0; i < n; i++)
      if (heff_sub_done(ss[i])) return i;
    if (n == 0 || (ms == 0 && !first)) return -1;
    int64_t wait = ms == 0 ? 0 : -1;
    if (until) {
      wait = until - heff_now_ms();
      if (wait <= 0) return -1;
    }
    heff_round(ss, n, wait);
  }
}

#ifndef _WIN32
// (it ended, if it has not: its input closed, asked to end (SIGTERM),
// then made to, two seconds after)
static void heff_sub_end(HeffSub *s) {
  if (heff_sub_done(s)) return;
  heff_close(&s->fin);
  heff_sub_signal(s, SIGTERM);
  if (heff_await(&s, 1, 2000) < 0) heff_sub_cut(s);
}
#endif

static uint32_t heff_run_shape[3] = { LBL_ended, LBL_out, LBL_err };

// (an ended command's `Ran`: how it ended, what it wrote)
static V heff_sub_ran(HeffSub *s) {
  int64_t code = 0;
  int how = heff_sub_how(s, &code);
  V f[3] = {
    heff_ended(how, code, s->name),
    heff_str_bytes(s->out.b.s ? s->out.b.s + s->out.at : "", (int64_t)(s->out.b.n - s->out.at)),
    heff_str_bytes(s->err.b.s ? s->err.b.s + s->err.at : "", (int64_t)(s->err.b.n - s->err.at)),
  };
  return heff_rec(3, heff_run_shape, f);
}

// (a command not run, as `why`, a `ProcessError`, says: its `Ran`)
static V heff_not_ran(V why) {
  V f[3] = { heff_tag_n(TAG_NotRun, 1, &why), heff_str_bytes("", 0), heff_str_bytes("", 0) };
  return heff_rec(3, heff_run_shape, f);
}

// (the command of the invocation `inv` started, as `v`; NULL where it
// could not be, why at `*why`)
static HeffSub *heff_launched(V inv, HeffInvocation *v, V *why) {
  int err = EINVAL, at_dir = 0;
  HeffSub *s = heff_invocation(inv, v) ? heff_sub_spawn(v, &err, &at_dir) : NULL;
  if (!s) *why = heff_why_not(v, err, at_dir);
  heff_invocation_free(v);
  return s;
}

// (a command run, as an `Invocation` says, to its end: a `Ran`)
static V heff_run(V inv) {
  HeffInvocation v;
  V why;
  HeffSub *s = heff_launched(inv, &v, &why);
  if (!s) return heff_not_ran(why);
  heff_await(&s, 1, -1);
  V r = heff_sub_ran(s);
  heff_sub_free(s);
  return r;
}

// Those started and not yet waited for: ended at the program's end, and
// asked to end where a signal ends it.

static HeffSub *heff_subs = NULL;

static void heff_subs_atexit(void) {
  for (HeffSub *s = heff_subs; s; s = s->next) heff_sub_end(s);
}

#ifndef _WIN32
// (commands being started now, by any thread, and whether a signal is
// ending the program: it waits for those being started to be among
// those it ends -- two seconds at most, should a thread starting one
// wait on the one it came to -- and none is started after it)
static int heff_spawning = 0;
static int heff_ending_now = 0;

// (the program ended by a signal -- SIGINT, SIGTERM, SIGHUP, where
// nothing else was to be done at it -- what it started and has not
// ended asked to end too, then the signal as it was: ended as it would
// have been)
static void heff_subs_signal(int sig) {
  __atomic_store_n(&heff_ending_now, 1, __ATOMIC_SEQ_CST);
  for (int i = 0; i < 2000 && __atomic_load_n(&heff_spawning, __ATOMIC_SEQ_CST) > 0; i++) {
    struct timespec a = { 0, 1000000 };
    nanosleep(&a, NULL);
  }
  for (HeffSub *s = heff_subs; s; s = s->next)
    heff_sub_signal(s, SIGTERM);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void heff_subs_watch(void) {
  for (int i = 0; i < 3; i++) {
    struct sigaction before;
    if (sigaction(heff_ending[i], NULL, &before) != 0 || before.sa_handler != SIG_DFL) continue;
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = heff_subs_signal;
    sigemptyset(&a.sa_mask);
    a.sa_flags = SA_RESTART;
    sigaction(heff_ending[i], &a, NULL);
  }
}

// (a command about to be started: the ending signals held back from
// this thread -- heard by another, the handler waits for it -- until
// it is among those started; where the program is being ended,
// nothing more is started: this thread waits for its end)
static void heff_spawn_hold(void) {
  sigset_t ending;
  sigemptyset(&ending);
  for (int i = 0; i < 3; i++) sigaddset(&ending, heff_ending[i]);
  pthread_sigmask(SIG_BLOCK, &ending, &heff_spawn_mask);
  heff_spawn_held = 1;
  __atomic_fetch_add(&heff_spawning, 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&heff_ending_now, __ATOMIC_SEQ_CST)) {
    __atomic_fetch_sub(&heff_spawning, 1, __ATOMIC_SEQ_CST);
    for (;;) pause();
  }
}

// (it is among those started, or was not started: the signals heard
// again, one that came meanwhile now)
static void heff_spawn_release(void) {
  __atomic_fetch_sub(&heff_spawning, 1, __ATOMIC_SEQ_CST);
  heff_spawn_held = 0;
  pthread_sigmask(SIG_SETMASK, &heff_spawn_mask, NULL);
}
#endif

// (let go of -- the program dropped its last reference to its value:
// ended first, taken out of those started)
static void heff_sub_let_go(void *x) {
  HeffSub *s = x;
  heff_sub_end(s);
  HEFF_SUBS_LOCK();
  for (HeffSub **at = &heff_subs; *at; at = &(*at)->next)
    if (*at == s) { *at = s->next; break; }
  HEFF_SUBS_UNLOCK();
  heff_sub_free(s);
}

// (the command started, as an `Invocation` says: `Right` it, the host's
// own -- a foreign value, ended and let go of where the program drops
// its last reference to it -- or `Left` why it was not)
static V heff_start(V inv) {
  HeffInvocation v;
  V why = 0;
  // (those not ended at the end ended then, from before the first is
  // started)
  HEFF_SUBS_LOCK();
  static int registered = 0;
  if (!registered) {
    registered = 1;
    atexit(heff_subs_atexit);
#ifndef _WIN32
    heff_subs_watch();
#endif
  }
  HEFF_SUBS_UNLOCK();
#ifndef _WIN32
  heff_spawn_hold();
#endif
  HeffSub *s = heff_launched(inv, &v, &why);
  if (s) {
    HEFF_SUBS_LOCK();
    s->next = heff_subs;
    heff_subs = s;
    HEFF_SUBS_UNLOCK();
  }
#ifndef _WIN32
  heff_spawn_release();
#endif
  if (!s) return heff_tag_n(TAG_Left, 1, &why);
  V x = heff_foreign(s, heff_sub_let_go);
  return heff_tag_n(TAG_Right, 1, &x);
}

// (the command a `Started` is, or the program failed: it is none)
static HeffSub *heff_sub_of(V v) {
  HeffSub *s = heff_foreign_ptr(v);
  if (s->magic != HEFF_SUB_MAGIC) heff_fail("std.process: no command started");
  return s;
}

// A command spoken to: its input `Piped`, written to and closed; its
// output or its error stream `Piped`, read as it goes.  What it writes
// is read into the stream's buffer whenever the program waits on it --
// a write as much as a read, so that it never waits on a program that
// waits on it -- and a read takes from there.

// (what a read asks for, a `Portion`: `kind` 0, whatever there is; 1,
// `n` bytes; 2, up to and with the `nd` bytes `d`)
typedef struct {
  int kind;
  int64_t n;
  const uint8_t *d;
  int64_t nd;
} HeffPortion;

static void heff_portion(V v, HeffPortion *w) {
  memset(w, 0, sizeof *w);
  switch (heff_tag_of(v)) {
  case TAG_Exactly:
    w->kind = 1;
    w->n = INT_OF(heff_field(v, 0));
    if (w->n < 0) w->n = 0;
    break;
  case TAG_Through:
    w->kind = 2;
    w->d = heff_bytes_data(heff_field(v, 0), &w->nd);
    break;
  default: w->kind = 0;
  }
}

// (what is taken of what has come on the stream and is not yet read, as
// `w` asks -- all that is left, at its end (`closed`): 1, the bytes in
// `*got`; 0, not yet; -1, none, at its end)
static int heff_stream_take(HeffStream *st, HeffPortion *w, int closed, V *got) {
  size_t avail = st->b.n - st->at, k = 0;
  const char *base = st->b.s ? st->b.s + st->at : "";
  int ok = 0;
  if (w->kind == 0) {
    ok = avail > 0;
    k = avail;
  } else if (w->kind == 1) {
    ok = avail >= (size_t)w->n;
    k = (size_t)w->n;
  } else {
    size_t nd = (size_t)w->nd;
    for (size_t i = 0; !ok && i + nd <= avail; i++)
      if (memcmp(base + i, w->d, nd) == 0) {
        ok = 1;
        k = i + nd;
      }
  }
  if (!ok && closed) {
    if (!avail) return -1;
    ok = 1;
    k = avail;
  }
  if (!ok) return 0;
  *got = heff_bytes_in(base, (int64_t)k);
  st->at += k;
  if (st->at == st->b.n) st->at = st->b.n = 0;
  else if (st->at > 65536 && st->at * 2 > st->b.n) {
    memmove(st->b.s, st->b.s + st->at, st->b.n - st->at);
    st->b.n -= st->at;
    st->at = 0;
  }
  return 1;
}

static V heff_got(V b) { return heff_tag_n(TAG_Got, 1, &b); }

#ifdef _WIN32
// (of what has come on the stream, what `w` asks, waited for up to `ms`
// (0: as long as it takes): a `Received`)
static V heff_sub_read(HeffSub *s, HeffStream *st, HeffPortion *w, int64_t ms) {
  if (st->mode != HEFF_OUT_PIPED) return NTAG(TAG_Closed);
  int64_t until = ms > 0 ? heff_now_ms() + ms : 0;
  for (;;) {
    V got;
    EnterCriticalSection(&s->lock);
    int closed = heff_stream_closed(st);
    int r = heff_stream_take(st, w, closed, &got);
    LeaveCriticalSection(&s->lock);
    if (r > 0) return heff_got(got);
    if (r < 0) return NTAG(TAG_Closed);
    int64_t wait = 50;
    if (until) {
      wait = until - heff_now_ms();
      if (wait <= 0) return NTAG(TAG_Timeout);
      if (wait > 50) wait = 50;
    }
    WaitForSingleObject(s->came, (DWORD)wait);
    heff_sub_expire(s, heff_now_ms());
  }
}

// (the bytes written to its input, all of them: whether they were)
static int heff_sub_write(HeffSub *s, const uint8_t *d, size_t n) {
  if (s->in_mode != HEFF_IN_PIPED || !s->fin) return 0;
  size_t at = 0;
  DWORD put;
  while (at < n) {
    DWORD want = n - at > 65536 ? 65536 : (DWORD)(n - at);
    if (!WriteFile(s->fin, d + at, want, &put, NULL)) {
      CloseHandle(s->fin);
      s->fin = 0;
      return 0;
    }
    at += put;
  }
  return 1;
}

static void heff_sub_close_input(HeffSub *s) {
  if (s->in_mode == HEFF_IN_PIPED && s->fin) {
    CloseHandle(s->fin);
    s->fin = 0;
  }
}
#else
// (of what has come on the stream, what `w` asks, waited for up to `ms`
// (0: as long as it takes): a `Received`)
static V heff_sub_read(HeffSub *s, HeffStream *st, HeffPortion *w, int64_t ms) {
  if (st->mode != HEFF_OUT_PIPED) return NTAG(TAG_Closed);
  int64_t until = ms > 0 ? heff_now_ms() + ms : 0;
  for (;;) {
    V got;
    int r = heff_stream_take(st, w, st->fd < 0, &got);
    if (r > 0) return heff_got(got);
    if (r < 0) return NTAG(TAG_Closed);
    int64_t wait = -1;
    if (until) {
      wait = until - heff_now_ms();
      if (wait <= 0) return NTAG(TAG_Timeout);
    }
    heff_round(&s, 1, wait);
  }
}

// (the bytes written to its input, all of them, what it writes read
// meanwhile: whether they were)
static int heff_sub_write(HeffSub *s, const uint8_t *d, size_t n) {
  if (s->in_mode != HEFF_IN_PIPED || s->fin < 0) return 0;
  s->given = (char *)d;
  s->ngiven = n;
  s->at = 0;
  sigset_t before;
  heff_nopipe(&before);
  heff_sub_feed(s);
  heff_nopipe_end(&before);
  while (s->fin >= 0 && s->at < s->ngiven) {
    if (heff_sub_done(s)) heff_close(&s->fin);
    else heff_round(&s, 1, -1);
  }
  int ok = s->fin >= 0;
  s->given = NULL;
  s->ngiven = s->at = 0;
  return ok;
}

static void heff_sub_close_input(HeffSub *s) {
  if (s->in_mode == HEFF_IN_PIPED) heff_close(&s->fin);
}
#endif

#ifndef _WIN32
// Waits on a started command, pending in a group of std.reactor's: a
// thread's, which goes on once it is done while the other threads go
// on meanwhile -- for its end (`Wait`, `Terminate`), for a portion of
// what it writes (`ReadOutput`, `ReadErrors`), for its input to take
// what is written (`WriteInput`).  Each is a wait of the reactor's with
// no descriptor of its own: this provider is a source of theirs --
// polled with the rest, the commands' pipes and what says each exited,
// for as long at most as a time limit or a read's leaves -- and says
// which are done.  Outside a group of the reactor's a wait is as it was,
// the program waiting with it.

enum { PW_WAIT, PW_READ, PW_WRITE };

typedef struct ProcWait ProcWait;
struct ProcWait {
  HeffWait w;        // (first: a wait is its `HeffWait`, its `fd` -1)
  HeffSub *s;
  V held, held2;     // (the command's value, and a read's portion or a
                     //  write's bytes: kept while the wait is pending)
  int what;          // PW_*
  HeffStream *st;    // (a read's stream)
  HeffPortion portion;
  int64_t until;     // (a read's time limit, or when a terminate makes
                     //  the command end: 0, none)
  int cut;           // (a terminate: cut short at `until`)
  V result;          // (a read's answer, once there is one)
  ProcWait *prev, *next;
};

static HEFF_TLS ProcWait *pw_first;
static HEFF_TLS HeffSource pw_source;

static int pw_step(HeffWait *w) {
  (void)w;
  return HEFF_STEP_WAIT;
}

// (whether the wait is done now; a read's answer made where it is)
static int pw_ready(ProcWait *p, int64_t now) {
  HeffSub *s = p->s;
  switch (p->what) {
  case PW_WAIT:
    if (heff_sub_done(s)) return 1;
    if (p->cut && now >= p->until) {
      heff_sub_cut(s);
      return 1;
    }
    return 0;
  case PW_READ: {
    V got;
    int r = heff_stream_take(p->st, &p->portion, p->st->fd < 0, &got);
    if (r > 0) p->result = heff_got(got);
    else if (r < 0) p->result = NTAG(TAG_Closed);
    else if (p->until && now >= p->until) p->result = NTAG(TAG_Timeout);
    else return 0;
    return 1;
  }
  default:
    if (heff_sub_done(s)) heff_close(&s->fin);
    return s->fin < 0 || s->at >= s->ngiven;
  }
}

// (the source's descriptors: those of every pending wait's command, and
// how long at most to wait for them)
static int pw_fds(struct pollfd *fds, int room, int *ms) {
  int k = 0;
  int64_t now = heff_now_ms(), m = -1;
  for (ProcWait *p = pw_first; p; p = p->next) {
    if (p->w.done) continue;
    heff_sub_polled(p->s, fds, room, &k, now, &m);
    if (p->until) {
      int64_t left = p->until > now ? p->until - now : 0;
      if (m < 0 || left < m) m = left;
    }
  }
  if (m > INT_MAX) m = INT_MAX;
  *ms = (int)m;
  return k;
}

// (after the poll: what came for each pending wait's command taken, and
// those done said so)
static void pw_came(struct pollfd *fds, int n) {
  (void)fds;
  (void)n;
  int64_t now = heff_now_ms();
  for (ProcWait *p = pw_first; p; p = p->next) {
    if (p->w.done) continue;
    heff_sub_came(p->s);
    if (pw_ready(p, now)) heff_wait_done(&p->w);
  }
}

// (the wait let go of: out of the pending, what it held dropped)
static void pw_free(ProcWait *p) {
  if (p->prev) p->prev->next = p->next;
  else pw_first = p->next;
  if (p->next) p->next->prev = p->prev;
  if (p->what == PW_WRITE) {
    p->s->given = NULL;
    p->s->ngiven = p->s->at = 0;
  }
  heff_drop(p->held);
  if (p->held2) heff_drop(p->held2);
  free(p);
}

static V pw_answer(HeffWait *w) {
  ProcWait *p = (ProcWait *)w;
  V r;
  switch (p->what) {
  case PW_WAIT: r = heff_sub_ran(p->s); break;
  case PW_READ: r = p->result; break;
  default: r = heff_bool(p->s->fin >= 0); break;
  }
  pw_free(p);
  return r;
}

static void pw_cancel(HeffWait *w) {
  ProcWait *p = (ProcWait *)w;
  if (p->what == PW_READ && p->result) heff_drop(p->result);
  pw_free(p);
}

// (a wait on the command `s`, whose value `held` it keeps)
static ProcWait *pw_new(int what, HeffSub *s, V held) {
  ProcWait *p = heff_alloc(sizeof *p);
  memset(p, 0, sizeof *p);
  p->what = what;
  p->s = s;
  p->held = held;
  return p;
}

// (the wait begun, pending: `f` of its answer given to `k` once it is
// done)
static V pw_begin(ProcWait *p, V f, V k) {
  p->w.fd = -1;
  p->w.step = pw_step;
  p->w.answer = pw_answer;
  p->w.cancel = pw_cancel;
  p->prev = NULL;
  p->next = pw_first;
  if (pw_first) pw_first->prev = p;
  pw_first = p;
  if (!pw_source.on) {
    pw_source.fds = pw_fds;
    pw_source.ready = pw_came;
    heff_reactor_source(&pw_source);
  }
  return heff_outside(&p->w, f, k);
}
#endif

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
  // (the working directory is this thread's own, never the process's)
  case TAG_WorkingDir: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_str_text(heff_working_dir()));
    return heff_tail(k, 1, &x);
  }
  case TAG_SetWorkingDir: {
    char *dir = heff_cstr(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_bool(dir && heff_working_dir_set(dir)));
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
  case TAG_Processors: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int64_t n = si.dwNumberOfProcessors > 0 ? (int64_t)si.dwNumberOfProcessors : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    int64_t n = c > 0 ? (int64_t)c : 1;
#else
    int64_t n = 1;
#endif
    V x = heff_answer(f, MK_INT(n));
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
  case TAG_Start: {
    V inv = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V r = heff_start(inv);
    heff_drop(inv);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  // (a command's value is kept through its operation: the operation may
  // hold the last reference, and dropping it lets the command go)
  case TAG_Release: {
    V v = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    heff_sub_end(heff_sub_of(v));
    heff_drop(v);
    V u = NTAG(TAG_MkUnit);
    V x = heff_answer(f, heff_tag_n(TAG_Right, 1, &u));
    return heff_tail(k, 1, &x);
  }
  case TAG_Wait: {
    V v = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HeffSub *s = heff_sub_of(v);
    heff_sub_close_input(s);
#ifndef _WIN32
    if (heff_group && heff_await(&s, 1, 0) < 0) return pw_begin(pw_new(PW_WAIT, s, v), f, k);
#endif
    heff_await(&s, 1, -1);
    V x = heff_answer(f, heff_sub_ran(s));
    heff_drop(v);
    return heff_tail(k, 1, &x);
  }
  case TAG_TryWait: {
    V v = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HeffSub *s = heff_sub_of(v);
    V r = heff_await(&s, 1, 0) == 0 ? heff_just(heff_sub_ran(s)) : heff_nothing;
    heff_drop(v);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_Kill: {
    heff_sub_kill(heff_sub_of(heff_field(p, 0)));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_Terminate: {
    V v = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HeffSub *s = heff_sub_of(v);
#ifndef _WIN32
    if (heff_group && !heff_sub_done(s)) {
      // (asked to end; pending until it has, made to two seconds after)
      heff_close(&s->fin);
      heff_sub_signal(s, SIGTERM);
      if (heff_await(&s, 1, 0) < 0) {
        ProcWait *w = pw_new(PW_WAIT, s, v);
        w->cut = 1;
        w->until = heff_now_ms() + 2000;
        return pw_begin(w, f, k);
      }
    }
#endif
    heff_sub_end(s);
    V x = heff_answer(f, heff_sub_ran(s));
    heff_drop(v);
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteInput: {
    V v = heff_dup(heff_field(p, 0));
    V b = heff_dup(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    HeffSub *s = heff_sub_of(v);
    int64_t n;
    const uint8_t *d = heff_bytes_data(b, &n);
#ifndef _WIN32
    if (heff_group && s->in_mode == HEFF_IN_PIPED && s->fin >= 0) {
      // (what its pipe takes now written; pending for the rest)
      s->given = (char *)d;
      s->ngiven = (size_t)n;
      s->at = 0;
      sigset_t before;
      heff_nopipe(&before);
      heff_sub_feed(s);
      heff_nopipe_end(&before);
      if (s->fin >= 0 && s->at < s->ngiven) {
        ProcWait *w = pw_new(PW_WRITE, s, v);
        w->held2 = b;
        return pw_begin(w, f, k);
      }
      int ok = s->fin >= 0;
      s->given = NULL;
      s->ngiven = s->at = 0;
      heff_drop(v);
      heff_drop(b);
      V x = heff_answer(f, heff_bool(ok));
      return heff_tail(k, 1, &x);
    }
#endif
    int ok = heff_sub_write(s, d, (size_t)n);
    heff_drop(v);
    heff_drop(b);
    V x = heff_answer(f, heff_bool(ok));
    return heff_tail(k, 1, &x);
  }
  case TAG_CloseInput: {
    heff_sub_close_input(heff_sub_of(heff_field(p, 0)));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadOutput:
  case TAG_ReadErrors: {
    V v = heff_dup(heff_field(p, 0));
    V portion = heff_dup(heff_field(p, 1));
    V ms = heff_field(p, 2);
    int64_t limit = IS_INT(ms) && INT_OF(ms) > 0 ? INT_OF(ms) : 0;
    V f = heff_dup(heff_field(p, 3));
    int errors = heff_tag_of(p) == TAG_ReadErrors;
    heff_drop(p);
    HeffSub *s = heff_sub_of(v);
    HeffPortion w;
    heff_portion(portion, &w);
    HeffStream *st = errors ? &s->err : &s->out;
#ifndef _WIN32
    if (heff_group && st->mode == HEFF_OUT_PIPED) {
      // (what has come taken, where it is what is asked; pending else)
      heff_round(&s, 1, 0);
      V got;
      int r = heff_stream_take(st, &w, st->fd < 0, &got);
      if (r == 0) {
        ProcWait *pw = pw_new(PW_READ, s, v);
        pw->held2 = portion;
        pw->st = st;
        pw->portion = w;
        pw->until = limit ? heff_now_ms() + limit : 0;
        return pw_begin(pw, f, k);
      }
      heff_drop(v);
      heff_drop(portion);
      V x = heff_answer(f, r > 0 ? heff_got(got) : NTAG(TAG_Closed));
      return heff_tail(k, 1, &x);
    }
#endif
    V r = heff_sub_read(s, st, &w, limit);
    heff_drop(v);
    heff_drop(portion);
    V x = heff_answer(f, r);
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
    errno = EINVAL;
    if (as) {
#ifdef _WIN32
      // (no exec on Windows: the command is run, and its exit code this
      // program's)
      HANDLE h = heff_spawn(as, GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
      if (h) exit((int)heff_wait(h));
#else
      // (the program's process becomes the command's: in this
      // thread's directory, as it would be between a fork and an exec)
      if (chdir(heff_working_dir()) == 0) execvp(c, as);
#endif
    }
    // (why, as a `ProcessError` says it)
    int e = errno ? errno : ENOENT;
    fprintf(stderr, "%s: %s: %s\n", c ? c : "the command", heff_errno_name(e), strerror(e));
    exit(1);
  }
  default:
    heff_fail("not an operation of process");
    return 0;
  }
}
