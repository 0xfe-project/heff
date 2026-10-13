// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place.
// A command `Launch`ed runs beside it, held by the world (a `Child`)
// until it is terminated; those not ended at the program's end are
// terminated then (atexit), and asked to end where a signal ends it.

#ifdef _WIN32
#include <direct.h>
#else
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

static uint32_t heff_run_shape[3] = { LBL_status, LBL_out, LBL_err };

// (a command run, as runtime.c's heff_capture runs it: -1 its status if
// it cannot be, a NUL in its name or an argument)
static V heff_run(V cmd, V args) {
  char *c = heff_cstr_text(cmd);
  char **as = c ? heff_argv_of(c, args) : NULL;
  Buf bo = {0}, be = {0};
  int64_t status = as ? heff_capture(as, &bo, &be) : -1;
  V f[3] = {
    MK_INT(status),
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
    V cmd = heff_dup(heff_field(p, 0)), args = heff_dup(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V r = heff_run(cmd, args);
    heff_drop(cmd);
    heff_drop(args);
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
  case TAG_Exited: {
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
