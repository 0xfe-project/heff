// Time's side of std.time, for the C backend: an effect provider.  A
// sleep is a wait of the runtime's reactor with a deadline and no
// descriptor: in a group of std.reactor's (cml's threads scheduled by
// `scheduledWithReactor`) it is pending, the other threads going on until
// its time has passed; elsewhere the program waits where it is.  A
// clock is read there and then: the time of day (CLOCK_REALTIME), and
// one that only goes forward (CLOCK_MONOTONIC), each in nanoseconds.

#include <time.h>

// (the time of day, in nanoseconds since the epoch)
static int64_t tm_now_ns(void) {
  struct timespec t;
#ifdef _WIN32
  timespec_get(&t, TIME_UTC);
#else
  clock_gettime(CLOCK_REALTIME, &t);
#endif
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

// (nanoseconds of a clock that only goes forward)
static int64_t tm_monotonic_ns(void) {
#ifdef _WIN32
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return (int64_t)(c.QuadPart / f.QuadPart) * 1000000000
         + (int64_t)(c.QuadPart % f.QuadPart) * 1000000000 / f.QuadPart;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
#endif
}

// (a clock's reading given to the operation's function, and what it
// gives to the continuation)
static V tm_read(V p, int64_t ns, V k) {
  V f = heff_dup(heff_field(p, 0));
  heff_drop(p);
  V x = heff_tr(heff_app1(f, heff_int(ns)));
  return heff_tail(k, 1, &x);
}

// (a sleep pending: its wait, and what comes after it)
typedef struct { HeffWait w; V x; } TmSleep;

static int tm_step(HeffWait *w) {
  (void)w;
  return HEFF_STEP_WAIT;
}

static V tm_answer(HeffWait *w) {
  TmSleep *s = (TmSleep *)w;
  V x = s->x;
  free(s);
  return x;
}

static void tm_cancel(HeffWait *w) {
  TmSleep *s = (TmSleep *)w;
  heff_drop(s->x);
  free(s);
}

// (what comes after the sleep, given by the reactor as the wait's
// answer: given back as it is)
static V tm_then(V self, V *a) {
  heff_drop(self);
  return a[0];
}

static V heff_provider_time(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Sleep: {
    int64_t ms = INT_OF(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    if (ms > 0 && heff_group) {
      TmSleep *s = heff_alloc(sizeof *s);
      memset(s, 0, sizeof *s);
      s->w.fd = -1;
      s->w.until = heff_now_ms() + ms;
      s->w.step = tm_step; s->w.answer = tm_answer; s->w.cancel = tm_cancel;
      s->x = x;
      return heff_outside(&s->w, heff_clo(tm_then, 1, 0, NULL), k);
    }
    if (ms > 0) {
#ifdef _WIN32
      Sleep((DWORD)ms);
#else
      struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
      while (nanosleep(&t, &t) != 0) {}
#endif
    }
    return heff_tail(k, 1, &x);
  }
  case TAG_Now:
    return tm_read(p, tm_now_ns(), k);
  case TAG_Monotonic:
    return tm_read(p, tm_monotonic_ns(), k);
  default:
    heff_fail("not an operation of time");
    return 0;
  }
}
