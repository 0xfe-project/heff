// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place
// (Node.js's first thread does that once the program has ended).

#ifdef _WIN32
#include <direct.h>
#else
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


// (the lines std.progress draws on the terminal, once there are any,
// cleared for good where a command runs in the program's place: an
// `Exit` clears them as the program ends -- lib/std/progress.provider.c)
static void (*heff_lines)(int);

static uint32_t heff_run_shape[3] = { LBL_status, LBL_out, LBL_err };

static V heff_run(V cmd, V args) {
  char *c = heff_cstr(cmd);
  char **as = heff_argv_of(c, args);
  Buf bo = {0}, be = {0};
  int64_t status = heff_capture(as, &bo, &be);
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
  case TAG_WorkingDir: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    char buf[4096];
#ifdef _WIN32
    const char *d = _getcwd(buf, sizeof buf);
#else
    const char *d = getcwd(buf, sizeof buf);
#endif
    V x = heff_answer(f, heff_str_c(d ? d : "."));
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
    // (std.progress's lines, if any are drawn, cleared for good: the
    // command has the terminal now)
    if (heff_lines) heff_lines(2);
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
