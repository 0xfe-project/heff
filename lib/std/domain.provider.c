// Domains' side of std.domain, for the C backend: an effect provider
// that runs heff on threads of the system truly at once -- so it says
// so, and the runtime is built for threads:
#define HEFF_THREADS 1
// A domain is a thread, started with the computation it runs (the
// world's: given what to do with its answer), moved to it where this
// thread held it alone, else shared (`heff_move`, #125; `heff_share`,
// #66); it runs it by `heff_try`, keeps its answer, shared, or why it
// failed, and ends by `heff_thread_end`.  A join waits for it, on the
// domain's own mutex and condition, and is given the answer, one more
// count of it (`heff_dup`) each join, or fails as it did
// (`heff_fail_again`).
//
// A domain, a cell, a mutex, a condition and a queue are each a foreign
// value (`heff_foreign`), let go of when the program drops its last
// reference to it: a cell drops the value it holds, a queue what is
// left in it, a mutex and a condition are destroyed, a domain drops its
// answer -- or, its thread still running,
// is marked dropped, and the thread frees it at its end: the later of
// the two to let go frees it, which is settled under the domain's own
// mutex.  An operation keeps the value holding its handle until it has
// acted on it, since dropping the value may drop the handle's last
// reference.
//
// A cell holds a value shared, read by one more count, written by a
// swap, the old value let go of after: a read and its count, and a
// swap, each under the cell's own lock, held a moment, so that no
// value is let go of while another domain counts it.  A number of a
// word (or a constructor without fields) is no object: read without
// the lock, and `fetchAdd` adds to one by a compare-and-swap alone.
// `cas` compares the words, so by identity.
//
// A queue is a list of what was put, under its own mutex, with a
// condition the takers wait on while it is empty.  What is put is moved
// (`heff_move`): the putter's reference becomes the queue's, and the
// taker's, with no count touched between, so what the putter held alone
// the taker holds alone, counted by it without atomics.
//
// An operation of a type of its own holds it as a function given what
// to do with it (`Some`, its types erased): `dm_open` takes it.  A
// computation a domain is spawned with, or one tried, is taken given
// the custodian current where it was asked (`heff_carried`): the
// domain's custodian is its spawner's.

#include <stdatomic.h>
#include <sched.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// (what an operation's `Some` holds, where `dm_open` puts it)
static HEFF_TLS V dm_held;

static V dm_open_fn(V self, V *a) {
  dm_held = a[0];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what `s` holds; `s` consumed)
static V dm_open(V s) {
  heff_drop(heff_answer(s, heff_clo(dm_open_fn, 1, 0, NULL)));
  return dm_held;
}

static char *dm_dup(const char *s) {
  char *c = heff_alloc(strlen(s) + 1);
  strcpy(c, s);
  return c;
}

// Domains ---------------------------------------------------------------------------

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t ended;
  int done;     // its thread ended
  int dropped;  // the program's last reference to it gone
  V answer;     // shared, once it is done
  char *why;    // or why it failed
} DmDomain;

// (the domain freed, its thread ended and the program's last reference
// gone: its answer dropped)
static void dm_domain_free(DmDomain *d) {
  pthread_mutex_destroy(&d->mu);
  pthread_cond_destroy(&d->ended);
  if (d->answer) heff_drop(d->answer);
  free(d->why);
  free(d);
}

// (the program's last reference to the domain gone: freed, unless its
// thread still runs -- then the thread frees it at its end)
static void dm_domain_let_go(void *x) {
  DmDomain *d = x;
  pthread_mutex_lock(&d->mu);
  int done = d->done;
  d->dropped = 1;
  pthread_mutex_unlock(&d->mu);
  if (done) dm_domain_free(d);
}

typedef struct { DmDomain *d; V c; char *dir; } DmStart;

// (the computation's answer, kept: its continuation, the domain's
// address its one field)
static V dm_answered(V self, V *a) {
  DmDomain *d = (DmDomain *)(intptr_t)INT_OF(heff_env(self)[0]);
  V v = heff_share(a[0]);
  pthread_mutex_lock(&d->mu);
  V old = d->answer;
  d->answer = v;
  pthread_mutex_unlock(&d->mu);
  if (old) heff_drop(old);
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

static void *dm_thread(void *u) {
  DmStart *s = u;
  DmDomain *d = s->d;
  heff_dir_take(s->dir);
  V env = MK_INT((int64_t)(intptr_t)d);
  V out;
  const char *why = heff_try(s->c, heff_clo(dm_answered, 1, 1, &env), &out);
  if (!why) heff_drop(out);
  pthread_mutex_lock(&d->mu);
  if (why) d->why = dm_dup(why);
  else if (!d->answer) d->why = dm_dup("domain: a domain ended without an answer");
  d->done = 1;
  int dropped = d->dropped;
  pthread_cond_broadcast(&d->ended);
  pthread_mutex_unlock(&d->mu);
  // (the program let go of it first: no joiner can be waiting)
  if (dropped) dm_domain_free(d);
  free(s);
  heff_thread_end();
  return NULL;
}

// (`c` run here, under `heff_try`, its answer kept where the
// continuation's one field says: `Right` it, or `Left` why it failed;
// `c` consumed)
static V dm_tried_answered(V self, V *a) {
  V *at = (V *)(intptr_t)INT_OF(heff_env(self)[0]);
  *at = a[0];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (`c`, a function of `Unit`, computed and run here, its failure caught
// -- running out of fuel, a limit's, handed on: std.fuel's)
static V dm_tried(V c) {
  V answer = 0;
  V env = MK_INT((int64_t)(intptr_t)&answer);
  V out;
  const char *why = heff_try(c, NTAG(TAG_MkUnit), &out);
  if (!why) why = heff_try(out, heff_clo(dm_tried_answered, 1, 1, &env), &out);
  if (why && strcmp(why, "heff:fuel") == 0) heff_fail_again(why);
  if (why) {
    V x = heff_str_text(why);
    return heff_tag_n(TAG_Left, 1, &x);
  }
  heff_drop(out);
  if (!answer) heff_fail("domain: a computation tried ended without an answer");
  return heff_tag_n(TAG_Right, 1, &answer);
}

// (a domain running `c`, moved to it where held alone; `c` consumed)
static DmDomain *dm_spawn(V c) {
  DmDomain *d = heff_alloc(sizeof(DmDomain));
  pthread_mutex_init(&d->mu, NULL);
  pthread_cond_init(&d->ended, NULL);
  d->done = 0;
  d->dropped = 0;
  d->answer = 0;
  d->why = NULL;
  DmStart *s = heff_alloc(sizeof(DmStart));
  s->d = d;
  s->c = heff_move(c);
  s->dir = heff_dir_copy();
  pthread_t t;
  if (heff_thread_start(&t, dm_thread, s) != 0) heff_fail("domain: cannot start a thread");
  pthread_detach(t);
  return d;
}

// (the domain's answer, waited for, one more count of it)
static V dm_join(DmDomain *d) {
  pthread_mutex_lock(&d->mu);
  while (!d->done) pthread_cond_wait(&d->ended, &d->mu);
  pthread_mutex_unlock(&d->mu);
  if (d->why) heff_fail_again(d->why);
  return heff_dup(d->answer);
}

static int64_t dm_cores(void) {
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

// Cells -----------------------------------------------------------------------------

typedef struct {
  _Atomic V v;
  atomic_flag busy;
} DmAtomic;

static void dm_hold(DmAtomic *a) {
  int spins = 0;
  while (atomic_flag_test_and_set_explicit(&a->busy, memory_order_acquire))
    if (++spins > 64) { sched_yield(); spins = 0; }
}

static void dm_release(DmAtomic *a) {
  atomic_flag_clear_explicit(&a->busy, memory_order_release);
}

// (a cell holding `v`; `v` consumed)
static DmAtomic *dm_atomic(V v) {
  DmAtomic *a = heff_alloc(sizeof(DmAtomic));
  atomic_init(&a->v, heff_share(v));
  atomic_flag_clear(&a->busy);
  return a;
}

// (the program's last reference to the cell gone: what it holds dropped)
static void dm_atomic_let_go(void *x) {
  DmAtomic *a = x;
  heff_drop(atomic_load_explicit(&a->v, memory_order_acquire));
  free(a);
}

// (what the cell holds, one more count of it)
static V dm_get(DmAtomic *a) {
  V v = atomic_load_explicit(&a->v, memory_order_acquire);
  if (!IS_OBJ(v)) return v;
  dm_hold(a);
  v = heff_dup(atomic_load_explicit(&a->v, memory_order_acquire));
  dm_release(a);
  return v;
}

// (it holds `v` now; `v` consumed)
static void dm_set(DmAtomic *a, V v) {
  v = heff_share(v);
  dm_hold(a);
  V old = atomic_exchange_explicit(&a->v, v, memory_order_acq_rel);
  dm_release(a);
  heff_drop(old);
}

// (... where it held `seen`, the same word: whether it did; `seen` and
// `v` consumed)
static int dm_cas(DmAtomic *a, V seen, V v) {
  v = heff_share(v);
  dm_hold(a);
  V was = seen;
  int ok = atomic_compare_exchange_strong_explicit(&a->v, &was, v, memory_order_acq_rel, memory_order_acquire);
  dm_release(a);
  heff_drop(seen);
  if (ok) heff_drop(was);
  else heff_drop(v);
  return ok;
}

// (`n` added to the number it holds: what it held; `n` consumed)
static V dm_fetch_add(DmAtomic *a, V n) {
  V old = atomic_load_explicit(&a->v, memory_order_acquire);
  for (;;) {
    int64_t sum;
    if (!IS_INT(old) || !IS_INT(n) || __builtin_add_overflow((int64_t)old, (int64_t)n - 1, &sum)) break;
    if (atomic_compare_exchange_weak_explicit(&a->v, &old, (V)sum, memory_order_acq_rel, memory_order_acquire))
      return old;
  }
  // (a number of more than a word, or the sum one: under the lock, and
  // swapped in still, for an add of words alone may come meanwhile)
  dm_hold(a);
  for (;;) {
    old = atomic_load_explicit(&a->v, memory_order_acquire);
    V sum = heff_share(heff_intAdd(heff_dup(old), heff_dup(n)));
    if (atomic_compare_exchange_strong_explicit(&a->v, &old, sum, memory_order_acq_rel, memory_order_acquire))
      break;
    heff_drop(sum);
  }
  dm_release(a);
  heff_drop(n);
  return old;
}

// Queues ----------------------------------------------------------------------------

typedef struct DmLink { V v; struct DmLink *next; } DmLink;

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t filled;
  DmLink *front, *back;
} DmQueue;

static DmQueue *dm_queue(void) {
  DmQueue *q = heff_alloc(sizeof(DmQueue));
  pthread_mutex_init(&q->mu, NULL);
  pthread_cond_init(&q->filled, NULL);
  q->front = q->back = NULL;
  return q;
}

// (the program's last reference to the queue gone: no domain waits at
// it, since it would hold the reference; what is left in it dropped)
static void dm_queue_let_go(void *x) {
  DmQueue *q = x;
  for (DmLink *l = q->front; l;) {
    DmLink *next = l->next;
    heff_drop(l->v);
    free(l);
    l = next;
  }
  pthread_mutex_destroy(&q->mu);
  pthread_cond_destroy(&q->filled);
  free(q);
}

// (`v` put at the back, moved; `v` consumed, by the queue)
static void dm_put(DmQueue *q, V v) {
  DmLink *l = heff_alloc(sizeof(DmLink));
  l->v = heff_move(v);
  l->next = NULL;
  pthread_mutex_lock(&q->mu);
  if (q->back) q->back->next = l;
  else q->front = l;
  q->back = l;
  pthread_cond_signal(&q->filled);
  pthread_mutex_unlock(&q->mu);
}

// (the front taken out, waited for while there is none: the queue's
// reference to it, now the caller's)
static V dm_take(DmQueue *q) {
  pthread_mutex_lock(&q->mu);
  while (!q->front) pthread_cond_wait(&q->filled, &q->mu);
  DmLink *l = q->front;
  q->front = l->next;
  if (!q->front) q->back = NULL;
  pthread_mutex_unlock(&q->mu);
  V v = l->v;
  free(l);
  return v;
}

// Mutexes and conditions ------------------------------------------------------------

static pthread_mutex_t *dm_mutex(void) {
  pthread_mutex_t *m = heff_alloc(sizeof(pthread_mutex_t));
  pthread_mutexattr_t at;
  pthread_mutexattr_init(&at);
  pthread_mutexattr_settype(&at, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(m, &at);
  pthread_mutexattr_destroy(&at);
  return m;
}

static pthread_cond_t *dm_condition(void) {
  pthread_cond_t *c = heff_alloc(sizeof(pthread_cond_t));
  pthread_cond_init(c, NULL);
  return c;
}

// (the program's last reference gone: no domain holds the mutex, none
// waits on the condition, since it would hold the reference)
static void dm_mutex_let_go(void *x) {
  pthread_mutex_destroy(x);
  free(x);
}

static void dm_condition_let_go(void *x) {
  pthread_cond_destroy(x);
  free(x);
}

#define DM_MUTEX(v) ((pthread_mutex_t *)heff_foreign_ptr(v))
#define DM_CONDITION(v) ((pthread_cond_t *)heff_foreign_ptr(v))

static void dm_lock(pthread_mutex_t *m) {
  int r = pthread_mutex_lock(m);
  if (r == EDEADLK) heff_fail("domain: a deadlock: a mutex locked by the domain holding it");
  if (r != 0) heff_fail("domain: a mutex not locked");
}

static void dm_unlock(pthread_mutex_t *m) {
  if (pthread_mutex_unlock(m) != 0) heff_fail("domain: a mutex unlocked by a domain not holding it");
}

static void dm_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  if (pthread_cond_wait(c, m) != 0) heff_fail("domain: a wait on a mutex the domain does not hold");
}

// The operations --------------------------------------------------------------------

#define DM_ATOMIC(v) ((DmAtomic *)heff_foreign_ptr(v))
#define DM_DOMAIN(v) ((DmDomain *)heff_foreign_ptr(v))
#define DM_QUEUE(v) ((DmQueue *)heff_foreign_ptr(v))

static V heff_provider_domain(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_Spawn: {
    V sp = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V c = heff_carried(heff_dup(heff_field(sp, 0))), g = heff_dup(heff_field(sp, 1));
    heff_drop(sp);
    x = heff_answer(g, heff_foreign(dm_spawn(c), dm_domain_let_go));
    break;
  }
  case TAG_Join: {
    V j = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V d = heff_field(j, 0), g = heff_dup(heff_field(j, 1));
    V v = dm_join(DM_DOMAIN(d));
    heff_drop(j);
    x = heff_answer(g, v);
    break;
  }
  case TAG_Tried: {
    V t = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V c = heff_carried_fn(heff_dup(heff_field(t, 0))), g = heff_dup(heff_field(t, 1));
    heff_drop(t);
    x = heff_answer(g, dm_tried(c));
    break;
  }
  // (this domain failed as the string says: a `tried`'s `Left` handed
  // on, as a join hands on a domain's failure)
  case TAG_Fail: {
    char *why = heff_cstr_text(heff_field(p, 0));
    heff_fail_again(why ? why : "domain: a failure whose words hold a NUL");
    return 0;
  }
  case TAG_Cores: {
    V g = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(g, heff_int(dm_cores()));
    break;
  }
  case TAG_NewAtomic: {
    V mk = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V v = heff_tget_v(mk, 0), g = heff_dup(heff_field(mk, 1));
    heff_drop(mk);
    x = heff_answer(g, heff_foreign(dm_atomic(v), dm_atomic_let_go));
    break;
  }
  case TAG_GetAtomic: {
    V rd = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    DmAtomic *a = DM_ATOMIC(heff_field(rd, 0));
    V g = heff_dup(heff_field(rd, 1));
    V v = dm_get(a);
    heff_drop(rd);
    x = heff_answer(g, v);
    break;
  }
  case TAG_SetAtomic: {
    V wr = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    DmAtomic *a = DM_ATOMIC(heff_field(wr, 0));
    V v = heff_tget_v(wr, 1);
    x = heff_dup(heff_field(wr, 2));
    dm_set(a, v);
    heff_drop(wr);
    break;
  }
  case TAG_CasAtomic: {
    V cp = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    DmAtomic *a = DM_ATOMIC(heff_field(cp, 0));
    V seen = heff_tget_v(cp, 1), v = heff_tget_v(cp, 2), g = heff_dup(heff_field(cp, 3));
    int ok = dm_cas(a, seen, v);
    heff_drop(cp);
    x = heff_answer(g, heff_bool(ok));
    break;
  }
  case TAG_FetchAdd: {
    DmAtomic *a = DM_ATOMIC(heff_field(p, 0));
    V n = heff_dup(heff_field(p, 1)), g = heff_dup(heff_field(p, 2));
    V old = dm_fetch_add(a, n);
    heff_drop(p);
    x = heff_answer(g, old);
    break;
  }
  case TAG_NewMutex: {
    V g = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(g, heff_foreign(dm_mutex(), dm_mutex_let_go));
    break;
  }
  case TAG_Lock: {
    pthread_mutex_t *m = DM_MUTEX(heff_field(p, 0));
    x = heff_dup(heff_field(p, 1));
    dm_lock(m);
    heff_drop(p);
    break;
  }
  case TAG_Unlock: {
    pthread_mutex_t *m = DM_MUTEX(heff_field(p, 0));
    x = heff_dup(heff_field(p, 1));
    dm_unlock(m);
    heff_drop(p);
    break;
  }
  case TAG_NewCondition: {
    V g = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(g, heff_foreign(dm_condition(), dm_condition_let_go));
    break;
  }
  case TAG_Wait: {
    pthread_cond_t *c = DM_CONDITION(heff_field(p, 0));
    pthread_mutex_t *m = DM_MUTEX(heff_field(p, 1));
    x = heff_dup(heff_field(p, 2));
    dm_wait(c, m);
    heff_drop(p);
    break;
  }
  case TAG_Signal: {
    pthread_cond_t *c = DM_CONDITION(heff_field(p, 0));
    x = heff_dup(heff_field(p, 1));
    pthread_cond_signal(c);
    heff_drop(p);
    break;
  }
  case TAG_Broadcast: {
    pthread_cond_t *c = DM_CONDITION(heff_field(p, 0));
    x = heff_dup(heff_field(p, 1));
    pthread_cond_broadcast(c);
    heff_drop(p);
    break;
  }
  case TAG_NewQueue: {
    V mk = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V g = heff_dup(heff_field(mk, 0));
    heff_drop(mk);
    x = heff_answer(g, heff_foreign(dm_queue(), dm_queue_let_go));
    break;
  }
  case TAG_PutQueue: {
    V pt = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V qv = heff_dup(heff_field(pt, 0)), v = heff_tget_v(pt, 1);
    x = heff_dup(heff_field(pt, 2));
    // (the record dropped first: its count of `v` gone, `v` is counted as
    // the program holds it, once where it holds it alone)
    heff_drop(pt);
    dm_put(DM_QUEUE(qv), v);
    heff_drop(qv);
    break;
  }
  case TAG_TakeQueue: {
    V tk = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    DmQueue *q = DM_QUEUE(heff_field(tk, 0));
    V g = heff_dup(heff_field(tk, 1));
    V v = dm_take(q);
    heff_drop(tk);
    x = heff_answer(g, v);
    break;
  }
  default:
    heff_fail("not an operation of domain");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
