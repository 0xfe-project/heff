// The process's side of lib/std/process.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).
// `Exit` ends the program there; `Exec` runs the command in its place
// (Node.js's first thread does that once the program has ended).

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

// (where the program is: its executable, as the system says)
static V heff_program_path(void) {
  char buf[4096];
#ifdef __APPLE__
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return heff_str_c(heff_argv[0]);
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return heff_str_c(heff_argv[0]);
  buf[n] = 0;
#endif
  char real[4096];
  return heff_str_c(realpath(buf, real) ? real : buf);
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

static uint32_t heff_run_shape[3] = { LBL_status, LBL_out, LBL_err };

static V heff_run(V cmd, V args) {
  char *c = heff_cstr(cmd);
  char **as = heff_argv_of(c, args);
  int po[2], pe[2];
  int64_t status = -1;
  Buf bo = {0}, be = {0};
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
    execvp(c, as);
    fprintf(stderr, "%s: cannot run it\n", c);
    exit(1);
  }
  default:
    heff_fail("not an operation of process");
    return 0;
  }
}
