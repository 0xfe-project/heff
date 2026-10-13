// The terminal's side of lib/std/terminal.hf, for the C backend: an
// effect provider (the shape of one is said in lib/std/io.provider.c).
//
// `Begin` makes the terminal raw -- the standard input read a byte at a
// time as it is typed, not echoed, not a line at a time; its signals
// kept, so that Ctrl-C is SIGINT still -- and shows the alternate
// screen.  What was is kept, and put back by `Finish`, at the program's
// end (atexit: `exit`, a failure), and by a handler of the signals that
// end a program (SIGINT, SIGTERM, SIGHUP, SIGQUIT), which puts it back
// and raises the signal again, as it was, so that the program ends as it
// would have.  Ctrl-Z (SIGTSTP) puts it back while the program is
// stopped; SIGCONT, and a change of the terminal's size (SIGWINCH), make
// it raw again and have `ReadInput` answer "", so that the app is drawn
// anew.  A read that finds ESC alone waits 25 ms for what follows, so
// that the key Escape and the start of a sequence are told apart.
//
// On Windows there is no terminal of this kind yet: `Interactive` and
// `Begin` say no.

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif

static uint32_t heff_cn_size_shape[2] = { LBL_columns, LBL_rows };

#ifndef _WIN32
// (whether the terminal is raw now, and as it was before)
static volatile sig_atomic_t heff_cn_raw = 0;
static struct termios heff_cn_saved;
// (a change of size, or a program gone on: the app to be drawn anew)
static volatile sig_atomic_t heff_cn_anew = 0;
static int heff_cn_handled = 0;
static struct sigaction heff_cn_before[7];
static const int heff_cn_signals[7] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT, SIGWINCH };

// (all of `n` bytes written to the terminal: what a handler of a
// signal may do)
static void heff_cn_write(const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(1, s, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return;
    s += w;
    n -= (size_t)w;
  }
}

static const char heff_cn_enter[] = "\x1b[?1049h\x1b[H\x1b[2J";
static const char heff_cn_leave[] = "\x1b[0m\x1b[?25h\x1b[?1049l";

// (raw, from the terminal as it was)
static void heff_cn_make_raw(void) {
  struct termios t = heff_cn_saved;
  t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN);
  t.c_cflag |= CS8;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(0, TCSANOW, &t);
  heff_cn_write(heff_cn_enter, sizeof heff_cn_enter - 1);
}

// (the terminal as it was, where it is raw; `over`: for good)
static void heff_cn_put_back(int over) {
  if (!heff_cn_raw) return;
  heff_cn_write(heff_cn_leave, sizeof heff_cn_leave - 1);
  tcsetattr(0, TCSANOW, &heff_cn_saved);
  if (over) heff_cn_raw = 0;
}

static void heff_cn_atexit(void) { heff_cn_put_back(1); }

static void heff_cn_signal(int sig) {
  int saved = errno;
  if (sig == SIGWINCH) heff_cn_anew = 1;
  else if (sig == SIGCONT) {
    if (heff_cn_raw) heff_cn_make_raw();
    heff_cn_anew = 1;
  } else if (sig == SIGTSTP) {
    // (put back while stopped: stopped as it would have been, by the
    // default; raw again at SIGCONT)
    heff_cn_put_back(0);
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_handler = heff_cn_signal;
    sigemptyset(&a.sa_mask);
    sigaction(SIGTSTP, &a, NULL);
  } else {
    // (an end: put back, then the signal as it was before)
    heff_cn_put_back(1);
    for (int i = 0; i < 7; i++)
      if (heff_cn_signals[i] == sig) sigaction(sig, &heff_cn_before[i], NULL);
    raise(sig);
  }
  errno = saved;
}

// (the handlers set, or those before put back)
static void heff_cn_handle(int on) {
  if (on == heff_cn_handled) return;
  heff_cn_handled = on;
  for (int i = 0; i < 7; i++) {
    if (on) {
      struct sigaction a;
      memset(&a, 0, sizeof a);
      a.sa_handler = heff_cn_signal;
      sigemptyset(&a.sa_mask);
      // (no SA_RESTART: a read waiting is interrupted, to draw anew)
      sigaction(heff_cn_signals[i], &a, &heff_cn_before[i]);
    } else sigaction(heff_cn_signals[i], &heff_cn_before[i], NULL);
  }
}

static int heff_cn_terminal(void) {
  const char *term = getenv("TERM");
  return isatty(0) && isatty(1) && !(term && strcmp(term, "dumb") == 0);
}

static int heff_cn_begin(void) {
  if (heff_cn_raw) return 1;
  if (!heff_cn_terminal() || tcgetattr(0, &heff_cn_saved) != 0) return 0;
  static int registered = 0;
  if (!registered) { registered = 1; atexit(heff_cn_atexit); }
  fflush(stdout);
  fflush(stderr);
  heff_cn_raw = 1;
  heff_cn_handle(1);
  heff_cn_make_raw();
  return 1;
}

static void heff_cn_finish(void) {
  fflush(stdout);
  heff_cn_put_back(1);
  heff_cn_handle(0);
}

// (what is typed next: Just it, Just "" where the app is to be drawn
// anew, Nothing at the end of the input)
static V heff_cn_read(void) {
  char buf[512];
  for (;;) {
    if (heff_cn_anew) {
      heff_cn_anew = 0;
      return heff_just(heff_str_bytes("", 0));
    }
    ssize_t n = read(0, buf, sizeof buf - 64);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return heff_nothing;
    // (ESC last: what follows it, if it comes at once)
    if (buf[n - 1] == 0x1b) {
      struct pollfd p = { 0, POLLIN, 0 };
      if (poll(&p, 1, 25) > 0) {
        ssize_t m = read(0, buf + n, 64);
        if (m > 0) n += m;
      }
    }
    return heff_just(heff_str_bytes(buf, (int64_t)n));
  }
}
#endif

// (the terminal's columns and rows: 80 by 24 where it does not say)
static V heff_cn_measure(void) {
  int cols = 80, rows = 24;
#ifndef _WIN32
  struct winsize w;
  if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
    cols = w.ws_col;
    rows = w.ws_row;
  }
#endif
  V f[2] = { MK_INT(cols), MK_INT(rows) };
  return heff_rec(2, heff_cn_size_shape, f);
}

static V heff_provider_console(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Interactive: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
#ifdef _WIN32
    V x = heff_answer(f, heff_bool(0));
#else
    V x = heff_answer(f, heff_bool(heff_cn_terminal()));
#endif
    return heff_tail(k, 1, &x);
  }
  case TAG_Begin: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
#ifdef _WIN32
    V x = heff_answer(f, heff_bool(0));
#else
    V x = heff_answer(f, heff_bool(heff_cn_begin()));
#endif
    return heff_tail(k, 1, &x);
  }
  case TAG_Finish: {
    V x = heff_dup(heff_field(p, 0));
    heff_drop(p);
#ifndef _WIN32
    heff_cn_finish();
#endif
    return heff_tail(k, 1, &x);
  }
  case TAG_Measure: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_cn_measure());
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadInput: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
#ifdef _WIN32
    V x = heff_answer(f, heff_nothing);
#else
    V x = heff_answer(f, heff_cn_read());
#endif
    return heff_tail(k, 1, &x);
  }
  case TAG_Draw: {
    fflush(stdout);
    size_t n = 0;
    char *s = heff_bytes(heff_field(p, 0), &n);
#ifdef _WIN32
    fwrite(s, 1, n, stdout);
    fflush(stdout);
#else
    heff_cn_write(s, n);
#endif
    free(s);
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of console");
    return 0;
  }
}
