// Time's side of lib/std/time.hf, for the C backend: an effect provider
// (the shape of one is said in lib/std/io.provider.c).  A program runs
// one thing at a time, so a sleep is the whole program's.

#include <time.h>

static V heff_provider_time(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Sleep: {
    int64_t ms = INT_OF(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
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
