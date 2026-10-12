// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place.

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
