// Signals' side of std.signal, for the C backend: an effect provider.
// A signal waited for is caught from then on, by a handler that writes
// its number to a pipe -- all a handler may safely do -- read by a
// thread of the provider's own, which gives it to the first wait for it
// or, where none is pending, keeps it for the next.  Each wait has a
// pipe of its own, written a byte when it is given a signal: what the
// reactor polls, so that a wait in a group of std.reactor's is pending
// while the other threads go on, and elsewhere the program waits where
// it is.  A wait let go of undone (an act given up: a race lost, a
// custody shut down) gives the signal it was given, if any, to the
// next.
//
// (Neither signalfd nor kqueue: signalfd hears a signal only where every
// thread holds it back, which the threads started before cannot be made
// to; one handler, for both systems, is std.process's way too.)
//
// A signal not waited for is as it was: std.process's handler, where it
// has one (its commands ended, then the signal as it was), or the
// system's.  One waited for is the program's, its commands ended with
// it at its end.

#ifndef _WIN32
#include <signal.h>
#include <pthread.h>

// (the signals, as std.signal numbers them)
static const int sg_numbers[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGWINCH };
#define SG_SIGNALS 7

// (a wait: the signals it wants, a bit each, its pipe, and the signal it
// was given, 1 more than its number, 0 none yet; among the waits
// pending while it is not given one)
typedef struct SgWait {
  HeffWait w;
  unsigned want;
  int fds[2];
  int given;
  struct SgWait *next;
} SgWait;

// (under the lock: the signals caught, a bit each; those arrived and
// not yet given, in the order they came; the waits pending, in theirs)
static pthread_mutex_t sg_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned sg_caught;
#define SG_KEPT 64
static int sg_kept[SG_KEPT];
static int sg_nkept;
static SgWait *sg_waits;
// (what the handler writes to, and the thread reads; and the process
// whose handler it is)
static int sg_pipe[2] = { -1, -1 };
static pid_t sg_pid;

static void sg_handler(int sig) {
  int e = errno;
  // (in a command forked and not yet exec'd: the signal as it was)
  if (getpid() != sg_pid) {
    signal(sig, SIG_DFL);
    raise(sig);
    errno = e;
    return;
  }
  unsigned char b = (unsigned char)sig;
  ssize_t r = write(sg_pipe[1], &b, 1);
  (void)r;
  errno = e;
}

// (signal `i` given to the first wait for it, its pipe written; else
// kept: under the lock)
static void sg_give(int i) {
  for (SgWait **at = &sg_waits; *at; at = &(*at)->next)
    if ((*at)->want & (1u << i)) {
      SgWait *w = *at;
      *at = w->next;
      w->given = i + 1;
      char b = 0;
      ssize_t r = write(w->fds[1], &b, 1);
      (void)r;
      return;
    }
  if (sg_nkept < SG_KEPT) sg_kept[sg_nkept++] = i;
}

static void *sg_run(void *a) {
  (void)a;
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, NULL);
  for (;;) {
    unsigned char b;
    ssize_t r = read(sg_pipe[0], &b, 1);
    if (r <= 0) continue;
    for (int i = 0; i < SG_SIGNALS; i++)
      if (sg_numbers[i] == b) {
        pthread_mutex_lock(&sg_mu);
        sg_give(i);
        pthread_mutex_unlock(&sg_mu);
      }
  }
  return NULL;
}

// (the signals of `want` caught, from now on: the first time, the pipe
// made and the thread started; under the lock)
static void sg_catch(unsigned want) {
  if (sg_pipe[0] < 0) {
    if (pipe(sg_pipe) != 0) heff_fail("std.signal: the signals' pipe could not be made");
    sg_pid = getpid();
    for (int i = 0; i < 2; i++) fcntl(sg_pipe[i], F_SETFD, FD_CLOEXEC);
    fcntl(sg_pipe[1], F_SETFL, fcntl(sg_pipe[1], F_GETFL) | O_NONBLOCK);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, sg_run, NULL) != 0) heff_fail("std.signal: its thread could not be started");
    pthread_attr_destroy(&at);
  }
  for (int i = 0; i < SG_SIGNALS; i++)
    if ((want & (1u << i)) && !(sg_caught & (1u << i))) {
      struct sigaction a;
      memset(&a, 0, sizeof a);
      a.sa_handler = sg_handler;
      sigemptyset(&a.sa_mask);
      a.sa_flags = SA_RESTART;
      sigaction(sg_numbers[i], &a, NULL);
      sg_caught |= 1u << i;
    }
}

static int sg_step(HeffWait *w) {
  SgWait *s = (SgWait *)w;
  char b;
  return read(s->fds[0], &b, 1) == 1 ? HEFF_STEP_DONE : HEFF_STEP_WAIT;
}

static void sg_free(SgWait *s) {
  close(s->fds[0]);
  close(s->fds[1]);
  free(s);
}

static V sg_answer(HeffWait *w) {
  SgWait *s = (SgWait *)w;
  int i = s->given - 1;
  sg_free(s);
  return heff_int(i);
}

static void sg_cancel(HeffWait *w) {
  SgWait *s = (SgWait *)w;
  pthread_mutex_lock(&sg_mu);
  if (s->given) sg_give(s->given - 1);
  else
    for (SgWait **at = &sg_waits; *at; at = &(*at)->next)
      if (*at == s) { *at = s->next; break; }
  pthread_mutex_unlock(&sg_mu);
  sg_free(s);
}

static V heff_provider_signals(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_WaitFor: {
    unsigned want = 0;
    for (V c = heff_field(p, 0); heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
      int64_t i = INT_OF(heff_field(c, 0));
      if (i >= 0 && i < SG_SIGNALS) want |= 1u << i;
    }
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    pthread_mutex_lock(&sg_mu);
    sg_catch(want);
    // (one kept, the first of those that came: given at once)
    for (int j = 0; j < sg_nkept; j++)
      if (want & (1u << sg_kept[j])) {
        int i = sg_kept[j];
        memmove(sg_kept + j, sg_kept + j + 1, (size_t)(sg_nkept - j - 1) * sizeof *sg_kept);
        sg_nkept--;
        pthread_mutex_unlock(&sg_mu);
        V x = heff_answer(f, heff_int(i));
        return heff_tail(k, 1, &x);
      }
    SgWait *s = heff_alloc(sizeof *s);
    memset(s, 0, sizeof *s);
    if (pipe(s->fds) != 0) {
      pthread_mutex_unlock(&sg_mu);
      heff_fail("std.signal: a wait's pipe could not be made");
    }
    for (int i = 0; i < 2; i++) {
      fcntl(s->fds[i], F_SETFD, FD_CLOEXEC);
      fcntl(s->fds[i], F_SETFL, fcntl(s->fds[i], F_GETFL) | O_NONBLOCK);
    }
    s->want = want;
    s->w.fd = s->fds[0];
    s->w.events = POLLIN;
    s->w.step = sg_step; s->w.answer = sg_answer; s->w.cancel = sg_cancel;
    SgWait **at = &sg_waits;
    while (*at) at = &(*at)->next;
    *at = s;
    pthread_mutex_unlock(&sg_mu);
    return heff_outside(&s->w, f, k);
  }
  default:
    heff_fail("not an operation of signals");
    return 0;
  }
}
#else
static V heff_provider_signals(V p, V k) {
  (void)p; (void)k;
  heff_fail("std.signal: no signals on Windows");
  return 0;
}
#endif
