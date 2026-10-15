// Time's side of std.time, for the C backend: an effect provider.  A
// sleep is a wait of the runtime's reactor with a deadline and no
// descriptor: in a group of std.reactor's (green threads scheduled by
// `Reactor.scheduled`) it is pending, the other threads going on until
// its time has passed; elsewhere the program waits where it is.

#include <time.h>

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
  default:
    heff_fail("not an operation of time");
    return 0;
  }
}
