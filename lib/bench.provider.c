// bench's timer, for the C backend: an effect provider.  Started, it
// notes the time, by the monotonic clock, and how many objects the
// program has made and their bytes (the runtime's counts, kept where it
// is built to count them, as `heff bench` builds it: else 0), the last
// thing before the run goes on; stopped, it reads them again, the first
// thing, and gives what the run took.

#include <time.h>

static int64_t heff_bench_start;
static uint64_t heff_bench_allocs, heff_bench_bytes;

// (the monotonic clock, in nanoseconds: on macOS the one that counts
// them, as Go's, its CLOCK_MONOTONIC counting microseconds)
static int64_t heff_bench_now(void) {
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#elif defined(__APPLE__)
  return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
#endif
}

static uint64_t heff_bench_allocs_now(void) {
#ifdef HEFF_COUNT_ALLOCS
  return heff_allocs;
#else
  return 0;
#endif
}

static uint64_t heff_bench_bytes_now(void) {
#ifdef HEFF_COUNT_ALLOCS
  return heff_alloc_bytes;
#else
  return 0;
#endif
}

static V heff_provider_timer(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_StartTimer: {
    V n = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_tr(heff_app1(f, n));
    heff_bench_allocs = heff_bench_allocs_now();
    heff_bench_bytes = heff_bench_bytes_now();
    heff_bench_start = heff_bench_now();
    return heff_tail(k, 1, &x);
  }
  case TAG_StopTimer: {
    int64_t ns = heff_bench_now() - heff_bench_start;
    uint64_t allocs = heff_bench_allocs_now() - heff_bench_allocs;
    uint64_t bytes = heff_bench_bytes_now() - heff_bench_bytes;
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V fs[3] = { MK_INT(ns), MK_INT((int64_t)allocs), MK_INT((int64_t)bytes) };
    V x = heff_tr(heff_app1(f, heff_tag(TAG_MkSample, 3, fs)));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of the timer");
    return 0;
  }
}
