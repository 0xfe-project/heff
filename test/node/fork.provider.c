// fork.hf's world, for the C backend (heff_parallel's
// fork.provider.c): an effect provider.
// `Spread n` forks n-1 children and gives the
// continuation to each, its index its answer; `Gather s`, in a child,
// writes `s` down its pipe and ends the child, and in the parent reads
// every child's string and goes on with them all.  The spreads open are
// a stack, copied into each child by fork, so that a spread inside a
// spread joins its own children first.  What is written is flushed
// before a fork, and a child ends by `_exit`: nothing written before is
// written twice.

#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  int n;       // processes in this spread
  int self;    // which of them this one is
  int wfd;     // a child's pipe to its parent
  int *rfd;    // the parent's pipes from its children (1 .. n-1)
  pid_t *pid;
} heff_fork_spread;

#define HEFF_FORK_DEPTH 64
static heff_fork_spread heff_fork_spreads[HEFF_FORK_DEPTH];
static int heff_fork_depth;

static void heff_fork_write(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w < 0) heff_fail("fork: cannot write to the parent");
    s += w;
    n -= (size_t)w;
  }
}

static V heff_fork_read(int fd) {
  size_t cap = 256, n = 0;
  unsigned char *buf = heff_alloc(cap);
  ssize_t got;
  while ((got = read(fd, buf + n, cap - n)) > 0) {
    n += (size_t)got;
    if (n == cap) {
      unsigned char *b2 = heff_alloc(cap * 2);
      memcpy(b2, buf, n);
      free(buf);
      buf = b2;
      cap *= 2;
    }
  }
  V s = heff_str_bytes(buf, (int64_t)n);
  free(buf);
  return s;
}

static int heff_fork_spread_into(int n) {
  if (heff_fork_depth == HEFF_FORK_DEPTH) heff_fail("fork: spreads too deep");
  heff_fork_spread *sp = &heff_fork_spreads[heff_fork_depth++];
  if (n < 1) n = 1;
  sp->n = n;
  sp->self = 0;
  sp->wfd = -1;
  sp->rfd = heff_alloc(sizeof(int) * (size_t)n);
  sp->pid = heff_alloc(sizeof(pid_t) * (size_t)n);
  fflush(stdout);
  fflush(stderr);
  for (int i = 1; i < n; i++) {
    int fd[2];
    if (pipe(fd) != 0) heff_fail("fork: no pipe");
    pid_t c = fork();
    if (c < 0) heff_fail("fork: cannot fork");
    if (c == 0) {
      close(fd[0]);
      for (int j = 1; j < i; j++) close(sp->rfd[j]);
      sp->self = i;
      sp->wfd = fd[1];
      return i;
    }
    close(fd[1]);
    sp->rfd[i] = fd[0];
    sp->pid[i] = c;
  }
  return 0;
}

static V heff_fork_gather(V s) {
  if (heff_fork_depth == 0) return heff_cons(s, NTAG(TAG_Nil));
  heff_fork_spread *sp = &heff_fork_spreads[heff_fork_depth - 1];
  if (sp->self != 0) {
    size_t n;
    char *c = heff_bytes(s, &n);
    heff_drop(s);
    heff_fork_write(sp->wfd, c, n);
    close(sp->wfd);
    _exit(0);
  }
  V *got = heff_alloc(sizeof(V) * (size_t)sp->n);
  got[0] = s;
  for (int i = 1; i < sp->n; i++) {
    got[i] = heff_fork_read(sp->rfd[i]);
    close(sp->rfd[i]);
    int st;
    waitpid(sp->pid[i], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) heff_fail("fork: a child failed");
  }
  V l = NTAG(TAG_Nil);
  for (int i = sp->n - 1; i >= 0; i--) l = heff_cons(got[i], l);
  free(got);
  free(sp->rfd);
  free(sp->pid);
  heff_fork_depth--;
  return l;
}

static V heff_provider_fork(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Spread: {
    int64_t n = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(heff_fork_spread_into((int)n)));
    return heff_tail(k, 1, &x);
  }
  case TAG_Gather: {
    V s = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_fork_gather(s));
    return heff_tail(k, 1, &x);
  }
  case TAG_Suspend: {
    V x = heff_dup(heff_field(p, 0));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of fork");
    return 0;
  }
}
