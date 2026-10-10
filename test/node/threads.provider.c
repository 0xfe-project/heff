// test/node/threads.hf's world, for the C backend: an effect provider
// (the shape of one is said in lib/std/io.provider.c) that runs heff in
// threads truly at once -- so it says so, and the runtime is built for
// threads (docs/threads.md):
#define HEFF_THREADS 1
// What a thread is given is a copy (`heff_copy_for_thread`), which it
// runs by `heff_try`, ending by `heff_thread_end`; what a thread gives
// back is its own, no other's, and after the join its parent's.  A
// failure in a thread is handed back, and is its parent's when it joins
// it (`heff_fail_again`).
//
// `TSpread n` starts n-1 threads, each with a copy of the rest of the
// program, its index its answer; `TGather x`, in one of them, leaves
// `x` and ends it, and in thread 0 joins them all and goes on with every
// one's `x`.  The spreads open are a stack, each thread's own
// (`th_top`), so that a spread inside a spread joins its own first.
// `TParFor w n f`: f 0 .. f (n-1) by w threads, each taking the next as
// it ends one, thread 0 the caller.  `TForced n f`: `f MkUnit` in n
// threads at once, each given `f` as a thread is (a static closure, as
// it is: its memo shared).

#include <stdatomic.h>

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
// (where a thread of a spread ends, its `x` left: at its start)
static HEFF_TLS jmp_buf *th_end;

typedef struct { V f, k; int i; ThSpread *sp; } ThStart;

static char *th_dup(const char *s) {
  char *c = heff_alloc(strlen(s) + 1);
  strcpy(c, s);
  return c;
}

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

// parFor and forced -----------------------------------------------------------

typedef struct {
  V *fs;        // each thread's f
  V arg;        // what f is given: each task's index, or this
  V *got;
  int64_t n;
  _Atomic int64_t next;
  char **why;   // each thread's failure
} ThJob;

typedef struct { ThJob *j; int w; } ThSlot;

static void th_work(ThJob *j, int w) {
  for (;;) {
    int64_t i = atomic_fetch_add(&j->next, 1);
    if (i >= j->n) return;
    const char *why = heff_try(heff_dup(j->fs[w]), j->arg ? j->arg : MK_INT(i), &j->got[i]);
    if (why) {
      j->why[w] = th_dup(why);
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

// (`f` given to `w` threads -- the caller one of them -- for `n` tasks:
// the answers, in order, as a list; `f` consumed)
static V th_run(int64_t w, int64_t n, V f, V arg) {
  if (w > n) w = n;
  if (w < 1) w = 1;
  ThJob j;
  j.fs = heff_alloc(sizeof(V) * (size_t)w);
  j.fs[0] = f;
  for (int64_t i = 1; i < w; i++) j.fs[i] = heff_copy_for_thread(f);
  j.arg = arg;
  j.got = calloc((size_t)(n > 0 ? n : 1), sizeof(V));
  j.n = n;
  atomic_init(&j.next, 0);
  j.why = calloc((size_t)w, sizeof(char *));
  pthread_t *tid = heff_alloc(sizeof(pthread_t) * (size_t)w);
  ThSlot *slots = heff_alloc(sizeof(ThSlot) * (size_t)w);
  for (int64_t i = 1; i < w; i++) {
    slots[i].j = &j; slots[i].w = (int)i;
    if (heff_thread_start(&tid[i], th_work_thread, &slots[i]) != 0) heff_fail("threads: cannot start a thread");
  }
  th_work(&j, 0);
  for (int64_t i = 1; i < w; i++) pthread_join(tid[i], NULL);
  for (int64_t i = 0; i < w; i++) heff_drop(j.fs[i]);
  free(j.fs); free(tid); free(slots);
  for (int64_t i = 0; i < w; i++)
    if (j.why[i]) heff_fail_again(j.why[i]);
  free(j.why);
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(j.got[i], l);
  free(j.got);
  return l;
}

static V heff_provider_threads(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_TSpread: {
    int64_t n = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    th_spread((int)n, f, k);
    V x = heff_answer(f, MK_INT(0));
    return heff_tail(k, 1, &x);
  }
  case TAG_TGather: {
    V x = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V l = th_gather(x);
    V r = heff_answer(f, l);
    return heff_tail(k, 1, &r);
  }
  case TAG_TLater: {
    V x = heff_dup(heff_field(p, 0));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_TParFor: {
    int64_t w = INT_OF(heff_field(p, 0));
    int64_t n = INT_OF(heff_field(p, 1));
    V f = heff_dup(heff_field(p, 2));
    V g = heff_dup(heff_field(p, 3));
    heff_drop(p);
    V x = heff_answer(g, th_run(w, n, f, 0));
    return heff_tail(k, 1, &x);
  }
  case TAG_TForced: {
    int64_t n = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    V g = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V x = heff_answer(g, th_run(n, n, f, NTAG(TAG_MkUnit)));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of threads");
    return 0;
  }
}
