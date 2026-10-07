// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place
// (Node.js's first thread does that once the program has ended).

#ifndef _WIN32
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

// (where the program is: its executable, as the system says)
static V heff_program_path(void) {
  char buf[4096];
#if defined(_WIN32)
  DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
  if (n == 0 || n >= sizeof buf) return heff_str_c(heff_argv[0]);
  return heff_str_c(buf);
#elif defined(__APPLE__)
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return heff_str_c(heff_argv[0]);
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return heff_str_c(heff_argv[0]);
  buf[n] = 0;
#endif
#ifndef _WIN32
  char real[4096];
  return heff_str_c(realpath(buf, real) ? real : buf);
#endif
}

// (a list of strings as a NULL-ended array of C strings, `first` first)
static char **heff_argv_of(const char *first, V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  char **as = heff_alloc((n + 2) * sizeof(char *));
  as[0] = (char *)first;
  int64_t i = 1;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) as[i++] = heff_cstr(heff_field(c, 0));
  as[i] = NULL;
  return as;
}

#ifdef _WIN32
// (a command line of `as`, each quoted as the C runtime's parser reads it
// back: malloc'd)
static char *heff_command_line(char **as) {
  Buf b = {0};
  for (char **a = as; *a; a++) {
    if (a != as) buf_put(&b, " ", 1);
    const char *s = *a;
    if (*s && !strpbrk(s, " \t\n\v\"")) { buf_str(&b, s); continue; }
    buf_put(&b, "\"", 1);
    for (;;) {
      size_t slashes = 0;
      while (*s == '\\') { s++; slashes++; }
      if (!*s) {
        for (size_t i = 0; i < slashes * 2; i++) buf_put(&b, "\\", 1);
        break;
      }
      size_t k = *s == '"' ? slashes * 2 + 1 : slashes;
      for (size_t i = 0; i < k; i++) buf_put(&b, "\\", 1);
      buf_put(&b, s++, 1);
    }
    buf_put(&b, "\"", 1);
  }
  buf_put(&b, "", 1);
  return b.s;
}

typedef struct { HANDLE h; Buf *b; } HeffDrain;

// (all a pipe gives, until it is closed)
static DWORD WINAPI heff_drain(LPVOID u) {
  HeffDrain *d = u;
  char chunk[65536];
  DWORD got;
  while (ReadFile(d->h, chunk, sizeof chunk, &got, NULL) && got > 0) buf_put(d->b, chunk, got);
  return 0;
}

// (a command started, its stdin this program's, its stdout and stderr
// `out` and `err`, or 0 if it cannot be)
static HANDLE heff_spawn(char **as, HANDLE out, HANDLE err) {
  char *line = heff_command_line(as);
  STARTUPINFOA si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi;
  int ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  free(line);
  if (!ok) return 0;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

// (a command's exit code once it has ended)
static int64_t heff_wait(HANDLE h) {
  DWORD code = (DWORD)-1;
  WaitForSingleObject(h, INFINITE);
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return (int64_t)code;
}
#else
// (what a command writes, both streams read together, so that neither
// fills its pipe while the other is waited for)
static void heff_read_both(int out, int err, Buf *bo, Buf *be) {
  struct pollfd fds[2] = { { out, POLLIN, 0 }, { err, POLLIN, 0 } };
  int open = 2;
  char chunk[65536];
  while (open > 0) {
    if (poll(fds, 2, -1) < 0) break;
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP))) continue;
      ssize_t got = read(fds[i].fd, chunk, sizeof chunk);
      if (got <= 0) { close(fds[i].fd); fds[i].fd = -1; open--; }
      else buf_put(i == 0 ? bo : be, chunk, (size_t)got);
    }
  }
}

#endif

static uint32_t heff_run_shape[3] = { LBL_status, LBL_out, LBL_err };

static V heff_run(V cmd, V args) {
  char *c = heff_cstr(cmd);
  char **as = heff_argv_of(c, args);
  int64_t status = -1;
  Buf bo = {0}, be = {0};
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE or_, ow, er, ew;
  if (CreatePipe(&or_, &ow, &sa, 0) && CreatePipe(&er, &ew, &sa, 0)) {
    SetHandleInformation(or_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(er, HANDLE_FLAG_INHERIT, 0);
    HANDLE h = heff_spawn(as, ow, ew);
    CloseHandle(ow);
    CloseHandle(ew);
    HeffDrain de = { er, &be }, dout = { or_, &bo };
    HANDLE t = CreateThread(NULL, 0, heff_drain, &de, 0, NULL);
    heff_drain(&dout);
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    else heff_drain(&de);
    CloseHandle(or_);
    CloseHandle(er);
    if (h) status = heff_wait(h);
  }
#else
  int po[2], pe[2];
  if (pipe(po) == 0 && pipe(pe) == 0) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    posix_spawn_file_actions_addclose(&fa, po[0]);
    posix_spawn_file_actions_addclose(&fa, pe[0]);
    pid_t pid;
    int ok = posix_spawnp(&pid, c, &fa, NULL, as, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(po[1]);
    close(pe[1]);
    heff_read_both(po[0], pe[0], &bo, &be);
    if (ok) {
      int st;
      if (waitpid(pid, &st, 0) >= 0 && WIFEXITED(st)) status = WEXITSTATUS(st);
    }
  }
#endif
  V f[3] = {
    MK_INT(status),
    heff_str_utf8((const unsigned char *)(bo.s ? bo.s : ""), (int64_t)bo.n),
    heff_str_utf8((const unsigned char *)(be.s ? be.s : ""), (int64_t)be.n),
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
    V x = heff_answer(f, heff_strs(heff_argv + 1, heff_argc - 1));
    return heff_tail(k, 1, &x);
  }
  case TAG_GetEnv: {
    char *name = heff_cstr(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    const char *v = getenv(name);
    free(name);
    V x = heff_answer(f, v ? heff_just(heff_str_c(v)) : heff_nothing);
    return heff_tail(k, 1, &x);
  }
  case TAG_ProgramPath: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_program_path());
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
    V x = heff_answer(f, heff_str_c(pl));
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
  case TAG_Exec: {
    char *c = heff_cstr(heff_field(p, 0));
    char **as = heff_argv_of(c, heff_field(p, 1));
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    // (no exec on Windows: the command is run, and its exit code this
    // program's)
    HANDLE h = heff_spawn(as, GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE));
    if (h) exit((int)heff_wait(h));
#else
    execvp(c, as);
#endif
    fprintf(stderr, "%s: cannot run it\n", c);
    exit(1);
  }
  default:
    heff_fail("not an operation of process");
    return 0;
  }
}
