// The world of test.node.first, for the C backend: jobs lent to the first thread
// (`heff_first_thread`), each saying what it found there on the
// standard output, by C's own stdio.  `Looping` says it began, as
// `Returns` does, and waits until its `stop` says to end; `RunLoop` (macOS) runs the main run loop, stopped from
// within it by what its `stop` sends to the main queue.

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#endif

static pthread_mutex_t first_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t first_cv = PTHREAD_COND_INITIALIZER;
static int first_ran, first_ran_here, first_stopped;

static const char *first_said(int b) { return b ? "True" : "False"; }

static void first_returns(void *u) {
  (void)u;
  pthread_mutex_lock(&first_mu);
  first_ran = 1;
  first_ran_here = heff_is_first_thread();
  pthread_cond_broadcast(&first_cv);
  pthread_mutex_unlock(&first_mu);
}

static void first_forever(void *u) {
  (void)u;
  pthread_mutex_lock(&first_mu);
  for (;;) pthread_cond_wait(&first_cv, &first_mu);
}

static void first_looping(void *u) {
  first_returns(u);
  pthread_mutex_lock(&first_mu);
  while (!first_stopped) pthread_cond_wait(&first_cv, &first_mu);
  pthread_mutex_unlock(&first_mu);
  printf("the loop returned, on the first thread: %s\n", first_said(heff_is_first_thread()));
}

static void first_stop(void *u) {
  (void)u;
  printf("stopped, on the first thread: %s\n", first_said(heff_is_first_thread()));
  pthread_mutex_lock(&first_mu);
  first_stopped = 1;
  pthread_cond_broadcast(&first_cv);
  pthread_mutex_unlock(&first_mu);
}

#ifdef __APPLE__
static void first_run_loop(void *u) {
  (void)u;
  CFRunLoopRun();
  printf("the run loop returned, on the first thread: %s\n", first_said(heff_is_first_thread()));
}

static void first_halt(void *u) {
  (void)u;
  CFRunLoopStop(CFRunLoopGetCurrent());
}

static void first_run_loop_stop(void *u) {
  (void)u;
  dispatch_async_f(dispatch_get_main_queue(), NULL, first_halt);
}

static void first_asked(void *u) { *(int *)u = heff_is_first_thread(); }
#endif

static V heff_provider_firstThread(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_Lend: {
    uint32_t j = heff_tag_of(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    int c;
    if (j == TAG_Returns) c = heff_first_thread(first_returns, NULL, NULL);
    else if (j == TAG_Forever) c = heff_first_thread(first_forever, NULL, NULL);
    else if (j == TAG_Looping) c = heff_first_thread(first_looping, first_stop, NULL);
#ifdef __APPLE__
    else c = heff_first_thread(first_run_loop, first_run_loop_stop, NULL);
#else
    else c = -1;
#endif
    x = heff_answer(f, MK_INT(c));
    break;
  }
  case TAG_HasRun: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    pthread_mutex_lock(&first_mu);
    while (!first_ran) pthread_cond_wait(&first_cv, &first_mu);
    int here = first_ran_here;
    pthread_mutex_unlock(&first_mu);
    x = heff_answer(f, heff_bool(here));
    break;
  }
  case TAG_Here: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(f, heff_bool(heff_is_first_thread()));
    break;
  }
  case TAG_Dispatched: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    int here = 0;
#ifdef __APPLE__
    dispatch_sync_f(dispatch_get_main_queue(), &here, first_asked);
#endif
    x = heff_answer(f, heff_bool(here));
    break;
  }
  case TAG_Failed:
    heff_fail("first: failed");
    return 0;
  default:
    heff_fail("not an operation of firstThread");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
