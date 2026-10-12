// Threads' side of lib/std/thread.hf, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c) that runs
// heff in threads truly at once -- so it says so, and the runtime is
// built for threads (docs/threads.md):
#define HEFF_THREADS 1
// What a thread is given is a copy (`heff_copy_for_thread`), which it
// runs by `heff_try`, ending by `heff_thread_end`; what a thread gives
// back is its own, no other's, and after the join its parent's.  A
// failure in a thread is handed back, and is its parent's when it joins
// it (`heff_fail_again`).
//
// `Pool n work`: the work's `f` of each of its `xs`, by n threads, each
// taking the next as it ends one, thread 0 the caller; `f` copied for
// each thread, each value for the thread that takes it.  `Spread n`
// starts n-1 threads, each with a copy of the rest of the program, its
// index its answer; `Gather`, in one of them, leaves its value and ends
// it, and in thread 0 joins them all and goes on with every one's
// value.  The spreads open are a stack, each thread's own (`th_top`), so
// that a spread inside a spread joins its own first.
//
// A `Work` and a `Joined` are each a function given what to do with
// what they hold, their types erased: `th_open` takes it -- `f`, `xs`
// and `k` of a work, `v` and `k` of a joined -- so that the provider has
// each apart.

#include <stdatomic.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// (what a work or a joined holds, where `th_open` puts it)
static HEFF_TLS V th_held[3];

static V th_open_fn(V self, V *a) {
  int n = ((Clo *)self)->arity;
  for (int i = 0; i < n; i++) th_held[i] = a[i];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (`w` given a function of `n` arguments that keeps them: what it holds
// at `th_held`; `w` consumed)
static void th_open(V w, int n) {
  heff_drop(heff_answer(w, heff_clo(th_open_fn, n, 0, NULL)));
}

static char *th_dup(const char *s) {
  char *c = heff_alloc(strlen(s) + 1);
  strcpy(c, s);
  return c;
}

// Spreads ---------------------------------------------------------------------------

typedef struct {
  int n;
  V *got;
  char **why;
  pthread_t *tid;
} ThSpread;

typedef struct ThOpen {
  ThSpread *sp;
  int self;
  struct ThOpen *up;
} ThOpen;

static HEFF_TLS ThOpen *th_top;
// (where a thread of a spread ends, its value left: at its start)
static HEFF_TLS jmp_buf *th_end;

typedef struct { V f, k; int i; ThSpread *sp; } ThStart;

static void *th_spread_thread(void *u) {
  ThStart *s = u;
  ThSpread *sp = s->sp;
  int i = s->i;
  ThOpen o = { sp, i, NULL };
  th_top = &o;
  jmp_buf end;
  th_end = &end;
  if (setjmp(end) == 0) {
    V x, r;
    const char *why = heff_try(s->f, MK_INT(i), &x);
    if (!why) why = heff_try(s->k, x, &r);
    if (!why) why = "threads: a thread ended without a gather";
    sp->why[i] = th_dup(why);
  }
  free(s);
  heff_thread_end();
  return NULL;
}

// (n-1 threads, each with a copy of `f` and `k`, the continuation: the
// two copied at once, so that what they share they share in the copy)
static void th_spread(int n, V f, V k) {
  if (n < 1) n = 1;
  ThSpread *sp = heff_alloc(sizeof(ThSpread));
  sp->n = n;
  sp->got = calloc((size_t)n, sizeof(V));
  sp->why = calloc((size_t)n, sizeof(char *));
  sp->tid = heff_alloc(sizeof(pthread_t) * (size_t)n);
  ThOpen *o = heff_alloc(sizeof(ThOpen));
  o->sp = sp; o->self = 0; o->up = th_top;
  th_top = o;
  V both[2] = { heff_dup(f), heff_dup(k) };
  V pair = heff_tag_n(TAG_MkPair, 2, both);
  for (int i = 1; i < n; i++) {
    V c = heff_copy_for_thread(pair);
    ThStart *s = heff_alloc(sizeof(ThStart));
    s->f = heff_dup(heff_field(c, 0)); s->k = heff_dup(heff_field(c, 1)); s->i = i; s->sp = sp;
    heff_drop(c);
    if (heff_thread_start(&sp->tid[i], th_spread_thread, s) != 0) heff_fail("threads: cannot start a thread");
  }
  heff_drop(pair);
}

// (`x` gathered: every thread's, in thread 0; another thread ends here)
static V th_gather(V x) {
  ThOpen *o = th_top;
  if (!o) return heff_cons(x, NTAG(TAG_Nil));
  ThSpread *sp = o->sp;
  if (o->self != 0) {
    sp->got[o->self] = x;
    longjmp(*th_end, 1);
  }
  sp->got[0] = x;
  for (int i = 1; i < sp->n; i++) pthread_join(sp->tid[i], NULL);
  th_top = o->up;
  free(o);
  for (int i = 1; i < sp->n; i++)
    if (sp->why[i]) heff_fail_again(sp->why[i]);
  V l = NTAG(TAG_Nil);
  for (int i = sp->n - 1; i >= 0; i--) l = heff_cons(sp->got[i], l);
  free(sp->got); free(sp->why); free(sp->tid); free(sp);
  return l;
}

// The pool ---------------------------------------------------------------------------

typedef struct {
  V *fs;        // each thread's f
  V *xs;        // each task's value, a copy for whoever takes it
  V *got;       // each task's answer
  int64_t n;
  _Atomic int64_t next;
  int64_t *failed;  // the task each thread failed in, else -1
  char **why;       // ... and why
} ThJob;

typedef struct { ThJob *j; int w; } ThSlot;

static void th_work(ThJob *j, int w) {
  for (;;) {
    int64_t i = atomic_fetch_add(&j->next, 1);
    if (i >= j->n) return;
    V x = j->xs[i];
    j->xs[i] = 0;
    const char *why = heff_try(heff_dup(j->fs[w]), x, &j->got[i]);
    if (why) {
      j->failed[w] = i;
      j->why[w] = th_dup(why);
      // (no one takes another: those before it, in order, were all taken)
      atomic_store(&j->next, j->n);
      return;
    }
  }
}

static void *th_work_thread(void *u) {
  ThSlot *s = u;
  th_work(s->j, s->w);
  heff_thread_end();
  return NULL;
}

// (`f` of each of the list `l`, by `w` threads -- the caller one of them:
// the answers, in order, as a list; `f` and `l` consumed.  One thread,
// or one value, is the caller alone, nothing copied)
static V th_pool(int64_t w, V f, V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  if (w > n) w = n;
  if (w <= 1) {
    V *got = heff_alloc(sizeof(V) * (size_t)(n > 0 ? n : 1));
    int64_t i = 0;
    for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1))
      got[i++] = heff_tr(heff_app1(heff_dup(f), heff_tget_v(c, 0)));
    heff_drop(f); heff_drop(l);
    V r = NTAG(TAG_Nil);
    for (i = n - 1; i >= 0; i--) r = heff_cons(got[i], r);
    free(got);
    return r;
  }
  ThJob j;
  j.n = n;
  j.xs = heff_alloc(sizeof(V) * (size_t)n);
  int64_t i = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    // (an element of a list of anything: one kept flat (#64) boxed)
    V x = heff_tget_v(c, 0);
    j.xs[i++] = heff_copy_for_thread(x);
    heff_drop(x);
  }
  heff_drop(l);
  j.fs = heff_alloc(sizeof(V) * (size_t)w);
  j.fs[0] = f;
  for (i = 1; i < w; i++) j.fs[i] = heff_copy_for_thread(f);
  j.got = calloc((size_t)n, sizeof(V));
  atomic_init(&j.next, 0);
  j.failed = heff_alloc(sizeof(int64_t) * (size_t)w);
  j.why = calloc((size_t)w, sizeof(char *));
  for (i = 0; i < w; i++) j.failed[i] = -1;
  pthread_t *tid = heff_alloc(sizeof(pthread_t) * (size_t)w);
  ThSlot *slots = heff_alloc(sizeof(ThSlot) * (size_t)w);
  for (i = 1; i < w; i++) {
    slots[i].j = &j; slots[i].w = (int)i;
    if (heff_thread_start(&tid[i], th_work_thread, &slots[i]) != 0) heff_fail("threads: cannot start a thread");
  }
  th_work(&j, 0);
  for (i = 1; i < w; i++) pthread_join(tid[i], NULL);
  for (i = 0; i < w; i++) heff_drop(j.fs[i]);
  free(j.fs); free(tid); free(slots);
  // (the first task that failed, in the list's order: the one the
  // program, one thread doing all, would have failed in)
  int64_t first = -1;
  for (i = 0; i < w; i++)
    if (j.failed[i] >= 0 && (first < 0 || j.failed[i] < j.failed[first])) first = i;
  if (first >= 0) {
    char *why = j.why[first];
    for (i = 0; i < w; i++) if (i != first) free(j.why[i]);
    for (i = 0; i < n; i++) {
      if (j.xs[i]) heff_drop(j.xs[i]);
      if (j.got[i]) heff_drop(j.got[i]);
    }
    free(j.xs); free(j.got); free(j.failed); free(j.why);
    heff_fail_again(why);
  }
  free(j.xs); free(j.failed); free(j.why);
  V r = NTAG(TAG_Nil);
  for (i = n - 1; i >= 0; i--) r = heff_cons(j.got[i], r);
  free(j.got);
  return r;
}

static int64_t th_cores(void) {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (int64_t)si.dwNumberOfProcessors : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
  long c = sysconf(_SC_NPROCESSORS_ONLN);
  return c > 0 ? (int64_t)c : 1;
#else
  return 1;
#endif
}

static V heff_provider_threads(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Pool: {
    int64_t w = INT_OF(heff_field(p, 0));
    V work = heff_dup(heff_field(p, 1));
    heff_drop(p);
    th_open(work, 3);
    V f = th_held[0], xs = th_held[1], g = th_held[2];
    V x = heff_answer(g, th_pool(w, f, xs));
    return heff_tail(k, 1, &x);
  }
  case TAG_Spread: {
    int64_t n = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    th_spread((int)n, f, k);
    V x = heff_answer(f, MK_INT(0));
    return heff_tail(k, 1, &x);
  }
  case TAG_Gather: {
    V joined = heff_dup(heff_field(p, 0));
    heff_drop(p);
    th_open(joined, 2);
    V v = th_held[0], g = th_held[1];
    V r = heff_answer(g, th_gather(v));
    return heff_tail(k, 1, &r);
  }
  case TAG_Cores: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(th_cores()));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of threads");
    return 0;
  }
}
