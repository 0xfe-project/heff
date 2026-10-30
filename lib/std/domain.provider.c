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
  (void)self;
  dm_held = a[0];
  return NTAG(TAG_MkUnit);
}

// (`dm_open_fn` as a function of the program's: one for good, never
// counted, as a lambda capturing nothing is -- not one made for each
// operation)
static Clo dm_open_clo = {{0x80000000u, K_CLO, 0}, dm_open_fn, 1, 0};

// (what `s` holds; `s` consumed)
static V dm_open(V s) {
  heff_drop(heff_answer(s, (V)&dm_open_clo));
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

// A cown's cell (`getOwned`, `setOwned`) is read and written by the one
// behaviour holding the cown.  What it holds is the cown's: not shared,
// but marked `OWNED` (`heff_own_at`), counted as a thread's own, out of
// line, where the body's log (`HeffOwnLog`, the behaviour's while it
// runs) keeps what the body counted up of it.  A write is kept by the
// log too, and done once the body is: a body that fails, or does not
// write a cell, leaves it as it was.  An array of what the cell holds
// -- the value itself, or held in it by a way nothing but the log holds
// any of, so that the body sees no other way to it -- held there, by the
// log and by the body alone, the body writes in place (`heff_own_set`),
// the log keeping what was there, put back where the cell is to hold
// what it held.  Once the body is done, what more than one holds of
// what it counted up is shared.

// (the log's entry for the cell, where it has one)
static HeffOwnRoot *dm_own_entry(HeffOwnLog *g, DmAtomic *a) {
  for (int64_t j = 0; j < g->nr; j++)
    if (g->roots[j].cell == a) return &g->roots[j];
  return NULL;
}

// (the log's entry for the cell `held` has, made where there is none:
// `held` held by it, so that the cell is not let go of before the log
// is done with it)
static HeffOwnRoot *dm_own_root(HeffOwnLog *g, V held) {
  DmAtomic *a = heff_foreign_ptr(held);
  HeffOwnRoot *had = dm_own_entry(g, a);
  if (had) return had;
  if (!g->roots) {
    g->roots = g->first;
    g->cr = sizeof g->first / sizeof g->first[0];
  } else if (g->roots == g->first && g->nr == g->cr) {
    g->roots = malloc(sizeof g->first);
    if (!g->roots) heff_fail("out of memory");
    memcpy(g->roots, g->first, sizeof g->first);
  }
  g->roots = heff_own_room(g->roots, g->nr, &g->cr, sizeof(HeffOwnRoot));
  HeffOwnRoot *e = &g->roots[g->nr++];
  e->cell = a;
  e->held = heff_dup(held);
  e->v = atomic_load_explicit(&a->v, memory_order_acquire);
  e->put = 0;
  e->written = 0;
  return e;
}

// (what a cown's cell holds, one more count of it: held by the log
// once too, where it is the cown's.  A number of a word, or a
// constructor without fields, is no object: nothing of it to keep, the
// log left as it is)
static V dm_get_owned(V held) {
  HeffOwnLog *g = heff_own_log;
  DmAtomic *a = heff_foreign_ptr(held);
  if (!g) return dm_get(a);
  V now = atomic_load_explicit(&a->v, memory_order_acquire);
  if ((!IS_OBJ(now) || !now) && !dm_own_entry(g, a)) return now;
  V v = dm_own_root(g, held)->v;
  if (!IS_OBJ(v) || !v) return v;
  uint32_t rc = heff_rc(OBJ(v));
  if (heff_owned(rc)) OBJ(v)->rc = heff_own_touch(OBJ(v), rc) + 1;
  else heff_dup(v);
  return v;
}

// (... it is to hold `v` once the body is done; `v` consumed)
static void dm_set_owned(V held, V v) {
  HeffOwnLog *g = heff_own_log;
  DmAtomic *a = heff_foreign_ptr(held);
  if (!g) {
    dm_set(a, v);
    return;
  }
  // (no object written over another, the cell not read as one: written
  // now, as the body's last steps write it, nothing kept)
  if ((!IS_OBJ(v) || !v) && !dm_own_entry(g, a)) {
    V now = atomic_load_explicit(&a->v, memory_order_acquire);
    if (!IS_OBJ(now) || !now) {
      atomic_store_explicit(&a->v, v, memory_order_release);
      return;
    }
  }
  HeffOwnRoot *e = dm_own_root(g, held);
  if (e->written) heff_drop(e->put);
  e->put = v;
  e->written = 1;
}

// (what the body wrote of `r` put back as it was in `s` -- `r`, or a
// copy of it -- the last first, each entry done with)
static void dm_own_unwrite(HeffOwnLog *g, Arr *r, Arr *s) {
  unsigned w = heff_arr_width[HEFF_ARR_KIND(r)];
  for (int64_t j = g->ns; j-- > 0;) {
    HeffOwnSet *e = &g->sets[j];
    if (e->r != r) continue;
    if (HEFF_ARR_KIND(r) == HEFF_ARR_V) heff_drop(s->a[e->i]);
    memcpy((char *)s->a + e->i * w, &e->old, w);
    e->r = NULL;
  }
}

// (what the cell holds holding the array as it was before the body
// wrote it: in place, where only what holds it there and the log do;
// else a copy, shared, put in the slot -- or the cell -- that held it,
// what else holds it keeping it as the body made it)
static void dm_own_restore(HeffOwnLog *g, HeffOwnArr *e) {
  Arr *r = e->r;
  if (HEFF_COUNT(heff_rc(&r->h)) == 2) {
    dm_own_unwrite(g, r, r);
    return;
  }
  Arr *s = heff_arr_alloc(r->n, HEFF_ARR_KIND(r));
  heff_arr_move(s, 0, r, 0, r->n);
  dm_own_unwrite(g, r, s);
  V c = heff_share((V)s);
  if (e->at) *e->at = c;
  else atomic_store_explicit(&((DmAtomic *)g->roots[e->root].cell)->v, c, memory_order_release);
  heff_drop((V)r);
}

// (a behaviour's body done, or failed, and its log finished, with no
// log of its own: what it was to write let go of where it failed; each
// array it wrote in place, of what a cell not written holds, put back;
// each cell
// written; and what it counted up shared where more than one holds it)
static void dm_own_done(HeffOwnLog *g, int failed) {
  if (!g->nr && !g->nt) return;
  if (failed)
    for (int64_t j = 0; j < g->nr; j++)
      if (g->roots[j].written) {
        heff_drop(g->roots[j].put);
        g->roots[j].written = 0;
      }
  for (int64_t j = 0; j < g->na; j++)
    if (!g->roots[g->arrs[j].root].written) dm_own_restore(g, &g->arrs[j]);
  for (int64_t j = 0; j < g->ns; j++) {
    HeffOwnSet *e = &g->sets[j];
    if (e->r && HEFF_ARR_KIND(e->r) == HEFF_ARR_V) heff_own_at(&e->r->a[e->i]);
  }
  for (int64_t j = 0; j < g->nr; j++) {
    HeffOwnRoot *e = &g->roots[j];
    if (!e->written) continue;
    DmAtomic *a = e->cell;
    dm_hold(a);
    V old = atomic_exchange_explicit(&a->v, e->put, memory_order_acq_rel);
    dm_release(a);
    heff_drop(old);
    heff_own_at(&e->put);
  }
  for (int64_t j = 0; j < g->ns; j++) {
    HeffOwnSet *e = &g->sets[j];
    if (e->r && HEFF_ARR_KIND(e->r) == HEFF_ARR_V) heff_drop((V)e->old);
  }
  for (int64_t j = 0; j < g->nt; j++) {
    V o = g->touched[j];
    uint32_t rc = heff_rc(OBJ(o));
    if (heff_owned(rc)) {
      OBJ(o)->rc = rc &= ~HEFF_TOUCHED_BIT;
      if (HEFF_COUNT(rc) > 2) heff_share(o);
    }
    heff_drop(o);
  }
  for (int64_t j = 0; j < g->nr; j++) heff_drop(g->roots[j].held);
  free(g->touched);
  if (g->roots != g->first) free(g->roots);
  free(g->arrs);
  free(g->sets);
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

// (a mutex held briefly by another domain is waited for spinning, a
// little, before the system is asked to put this one to sleep: on a
// system whose mutexes never spin, every lock contended would be a
// sleep and a wake, costing more than most holds)
#define DM_LOCK_SPINS 100

static void dm_relax(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause");
#endif
}

static void dm_lock(pthread_mutex_t *m) {
  for (int i = 0; i < DM_LOCK_SPINS; i++) {
    if (pthread_mutex_trylock(m) == 0) return;
    dm_relax();
  }
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

// Behaviours ------------------------------------------------------------------------

// Behaviour-oriented concurrency's order (Cheeseman et al., OOPSLA 2023,
// as Verona keeps it): a turnstile holds the last request queued at it;
// a behaviour has a request at each of its turnstiles, in the order of
// their addresses, and is queued by swapping each in as the last, the
// one before it -- once that one's behaviour is done queueing at all of
// its own -- told it is next.  It runs once it is first at every one: a
// count of the turnstiles it still waits at, and one more until its
// queueing is done.  Done, it lets the next through at each, or, where
// none is behind it, takes its request out as the last by a
// compare-and-swap -- or, one coming meanwhile, waits for it to say so.
// No mutex but the pool's, around the behaviours ready to run, which its
// domains take in turn, sleeping on its condition while there are none.
// A behaviour readied by one a domain of the pool runs is kept in that
// domain's slot, the next it runs, where the slot is empty: none woken,
// the mutex not taken -- a chain of behaviours each readying the next
// runs on one domain.  One that has run 64 (`DM_BATCH`) from its slot in
// a row puts the next behind the others ready, where any are, so that
// they are not passed over.  A body is never stopped partway, so the
// one kept would wait for the end of the body running, however long:
// the domain counts the body's operations of this effect, and at the
// 256th (`DM_FUEL`) puts the one kept with the others ready, waking a
// domain to take it.
//
// A behaviour holds its turnstiles' handles (the list it was given)
// until it is done, so none is freed while a request of it is queued.
// What it runs, and what it holds, stays the spawning domain's -- counted
// by it alone, not marked shared -- for as long as no other domain can
// reach it: it is moved (`heff_move`, `heff_share` for the handles) by
// that domain only where another may take it from then on -- before it
// is linked behind another behaviour, whose end, on whichever domain,
// readies it, and before it is put with those ready.  One readied into
// this domain's slot, run here next, is never moved: a chain of
// behaviours on one domain marks nothing shared.  It is run by `heff_try`: where it fails, what it was given to run then
// is given why and run, before the next is let through; what that fails
// at is the pool's, the first kept, for `drain`.  The pool counts the behaviours queued and running, and the
// holds; `drain` waits for that to come to none, then stops its domains.

typedef struct DmBehaviour DmBehaviour;
typedef struct DmTurnstile DmTurnstile;

typedef struct {
  _Atomic(DmBehaviour *) next;
  atomic_bool queued;
  DmTurnstile *t;
} DmRequest;

struct DmTurnstile {
  _Atomic(DmRequest *) last;
};

typedef struct DmPool DmPool;

struct DmBehaviour {
  atomic_int waits;
  DmPool *pool;
  V c;
  V failed;
  V held;
  DmBehaviour *after;
  int n;
  int moved;
  DmRequest req[];
};

#define DM_POLLERS 1024

struct DmPool {
  pthread_mutex_t mu;
  pthread_cond_t work, idle;
  DmBehaviour *front, *back;
  atomic_llong live;
  int sleeping, stopping, n;
  pthread_t *threads;
  char *why;
  // (the reactor groups of the domains working it that wait in their
  // reactors, to be woken there)
  int npolling;
  int64_t polling[DM_POLLERS];
};

static DmTurnstile *dm_turnstile(void) {
  DmTurnstile *t = heff_alloc(sizeof(DmTurnstile));
  atomic_init(&t->last, NULL);
  return t;
}

static void dm_turnstile_let_go(void *x) { free(x); }

// (one fewer behaviour or hold: at none, `drain` woken)
static void dm_pool_less(DmPool *pl) {
  if (atomic_fetch_sub_explicit(&pl->live, 1, memory_order_acq_rel) == 1) {
    pthread_mutex_lock(&pl->mu);
    pthread_cond_broadcast(&pl->idle);
    pthread_mutex_unlock(&pl->mu);
  }
}

#define DM_BATCH 64
#define DM_FUEL 256
#define DM_SPINS 4000

// (the pool whose behaviours this domain runs, if one, and its number
// among those working it; its slot; the fuel of the body it runs,
// where it runs one; and the group it waits on in its reactor, where it
// said so)
static HEFF_TLS DmPool *dm_running;
static HEFF_TLS int64_t dm_index = -1;
static HEFF_TLS DmBehaviour *dm_slot;
static HEFF_TLS int dm_fuel, dm_in_body;
static HEFF_TLS int64_t dm_polled;

// (what the behaviour runs and holds made another domain's to take: by
// the domain whose it is, once)
static void dm_movable(DmBehaviour *b) {
  if (b->moved) return;
  b->c = heff_move(b->c);
  b->failed = heff_move(b->failed);
  b->held = heff_share(b->held);
  b->moved = 1;
}

// (the behaviour put behind those ready, a sleeping domain woken)
// (or, none sleeping, one waiting in its reactor woken there)
static void dm_queued(DmBehaviour *b) {
  DmPool *pl = b->pool;
  dm_movable(b);
  b->after = NULL;
  int64_t wake = 0;
  pthread_mutex_lock(&pl->mu);
  if (pl->back) pl->back->after = b;
  else __atomic_store_n(&pl->front, b, __ATOMIC_RELAXED);
  pl->back = b;
  if (pl->sleeping > 0) pthread_cond_signal(&pl->work);
  else if (pl->npolling > 0) wake = pl->polling[--pl->npolling];
  pthread_mutex_unlock(&pl->mu);
  if (wake) heff_reactor_wake_group(wake);
}

// (the behaviour ready: in this domain's slot, where it runs the pool's
// behaviours and the slot is empty, else queued)
static void dm_ready(DmBehaviour *b) {
  if (dm_running == b->pool && !dm_slot) dm_slot = b;
  else dm_queued(b);
}

// (one wait of the behaviour's over: at none, it is ready)
static void dm_through(DmBehaviour *b) {
  if (atomic_fetch_sub_explicit(&b->waits, 1, memory_order_acq_rel) == 1) dm_ready(b);
}

static int dm_by_address(const void *x, const void *y) {
  uintptr_t a = (uintptr_t)((const DmRequest *)x)->t, b = (uintptr_t)((const DmRequest *)y)->t;
  return a < b ? -1 : a > b;
}

#define DM_TURNSTILE(v) ((DmTurnstile *)heff_foreign_ptr(v))
#define DM_POOL(v) ((DmPool *)heff_foreign_ptr(v))

// (`c` queued at the turnstiles of the list `ts`, `f` what runs where it
// fails; all three consumed)
static void dm_behave(DmPool *pl, V ts, V c, V f) {
  int n = 0;
  for (V l = ts; heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1)) n++;
  DmBehaviour *b = heff_alloc(sizeof(DmBehaviour) + (size_t)n * sizeof(DmRequest));
  int i = 0;
  for (V l = ts; heff_tag_of(l) == TAG_Cons; l = heff_field(l, 1), i++) {
    b->req[i].t = DM_TURNSTILE(heff_field(l, 0));
    atomic_init(&b->req[i].next, NULL);
    atomic_init(&b->req[i].queued, 0);
  }
  qsort(b->req, (size_t)n, sizeof(DmRequest), dm_by_address);
  for (i = 1; i < n; i++)
    if (b->req[i].t == b->req[i - 1].t) {
      free(b);
      heff_drop(ts);
      heff_drop(c);
      heff_drop(f);
      heff_abandon();
    }
  b->n = n;
  b->pool = pl;
  b->c = c;
  b->failed = f;
  b->held = ts;
  b->moved = 0;
  atomic_init(&b->waits, n + 1);
  atomic_fetch_add_explicit(&pl->live, 1, memory_order_acq_rel);
  for (i = 0; i < n; i++) {
    DmRequest *r = &b->req[i];
    DmRequest *before = atomic_exchange_explicit(&r->t->last, r, memory_order_acq_rel);
    if (!before) {
      atomic_fetch_sub_explicit(&b->waits, 1, memory_order_acq_rel);
      continue;
    }
    dm_movable(b);
    while (!atomic_load_explicit(&before->queued, memory_order_acquire)) dm_relax();
    atomic_store_explicit(&before->next, b, memory_order_release);
  }
  for (i = 0; i < n; i++) atomic_store_explicit(&b->req[i].queued, 1, memory_order_release);
  dm_through(b);
}

// (the behaviour done: the next let through at each turnstile)
static void dm_behaved_all(DmBehaviour *b) {
  for (int i = 0; i < b->n; i++) {
    DmRequest *r = &b->req[i];
    DmBehaviour *next = atomic_load_explicit(&r->next, memory_order_acquire);
    if (!next) {
      DmRequest *mine = r;
      if (atomic_compare_exchange_strong_explicit(&r->t->last, &mine, NULL, memory_order_acq_rel, memory_order_acquire))
        continue;
      while (!(next = atomic_load_explicit(&r->next, memory_order_acquire))) dm_relax();
    }
    dm_through(next);
  }
}

static V dm_behaved(V self, V *a) {
  (void)self;
  heff_drop(a[0]);
  return NTAG(TAG_MkUnit);
}

static Clo dm_behaved_clo = {{0x80000000u, K_CLO, 0}, dm_behaved, 1, 0};

// (the behaviour run, here, then let go of)
static void dm_run_behaviour(DmBehaviour *b) {
  DmPool *pl = b->pool;
  V out;
  dm_fuel = 0;
  dm_in_body = 1;
  // (the log's lists empty, its room for the first cells' entries not
  // cleared: none is read before it is written)
  HeffOwnLog log, *outer = heff_own_log;
  memset(&log, 0, __builtin_offsetof(HeffOwnLog, first));
  heff_own_log = &log;
  // (the body's chain of calls last counted from naught: each body starts
  // a chain of its own, as a bounce's step does, not one past the last
  // body's on this thread, every call of which would be a bounce)
  long dep = heff_dep;
  heff_dep = 0;
  const char *why = heff_try(b->c, (V)&dm_behaved_clo, &out);
  heff_dep = dep;
  heff_own_log = NULL;
  dm_own_done(&log, why != NULL);
  if (why) {
    V fc = heff_answer(b->failed, heff_str_text(why));
    why = heff_try(fc, (V)&dm_behaved_clo, &out);
    if (why) {
      pthread_mutex_lock(&pl->mu);
      if (!pl->why) pl->why = dm_dup(why);
      pthread_mutex_unlock(&pl->mu);
    } else heff_drop(out);
  } else {
    heff_drop(out);
    heff_drop(b->failed);
  }
  heff_own_log = outer;
  dm_in_body = 0;
  dm_behaved_all(b);
  heff_drop(b->held);
  free(b);
  dm_pool_less(pl);
}

typedef struct { DmPool *pl; char *dir; } DmWorkerStart;

static void *dm_worker(void *u) {
  DmWorkerStart *s = u;
  DmPool *pl = s->pl;
  heff_dir_take(s->dir);
  free(s);
  dm_running = pl;
  int row = 0;
  for (;;) {
    DmBehaviour *kept = dm_slot;
    if (kept) {
      dm_slot = NULL;
      if (row >= DM_BATCH && __atomic_load_n(&pl->front, __ATOMIC_RELAXED))
        dm_queued(kept);
      else {
        row = row >= DM_BATCH ? 1 : row + 1;
        dm_run_behaviour(kept);
        continue;
      }
    }
    row = 0;
    // (a while looked at before sleeping: a behaviour readied soon after,
    // by an act come back or another domain, taken without a wake)
    for (int i = 0; i < DM_SPINS && !__atomic_load_n(&pl->front, __ATOMIC_RELAXED)
                    && !__atomic_load_n(&pl->stopping, __ATOMIC_RELAXED); i++)
      dm_relax();
    pthread_mutex_lock(&pl->mu);
    while (!pl->front && !pl->stopping) {
      pl->sleeping++;
      pthread_cond_wait(&pl->work, &pl->mu);
      pl->sleeping--;
    }
    DmBehaviour *b = pl->front;
    if (!b) {
      pthread_mutex_unlock(&pl->mu);
      break;
    }
    __atomic_store_n(&pl->front, b->after, __ATOMIC_RELAXED);
    if (!pl->front) pl->back = NULL;
    pthread_mutex_unlock(&pl->mu);
    dm_run_behaviour(b);
  }
  heff_thread_end();
  return NULL;
}

// (a pool of `n` domains, the machine's cores where `n` is none, and
// none of its own where it is less: its behaviours run by `dm_work`)
static DmPool *dm_pool(int64_t n) {
  if (n == 0) n = dm_cores();
  if (n < 0) n = 0;
  DmPool *pl = heff_alloc(sizeof(DmPool));
  pthread_mutex_init(&pl->mu, NULL);
  pthread_cond_init(&pl->work, NULL);
  pthread_cond_init(&pl->idle, NULL);
  pl->front = pl->back = NULL;
  atomic_init(&pl->live, 0);
  pl->sleeping = 0;
  pl->stopping = 0;
  pl->n = (int)n;
  pl->why = NULL;
  pl->npolling = 0;
  pl->threads = heff_alloc((size_t)(n ? n : 1) * sizeof(pthread_t));
  for (int i = 0; i < pl->n; i++) {
    DmWorkerStart *s = heff_alloc(sizeof(DmWorkerStart));
    s->pl = pl;
    s->dir = heff_dir_copy();
    if (heff_thread_start(&pl->threads[i], dm_worker, s) != 0) heff_fail("domain: cannot start a thread");
  }
  return pl;
}

// (this domain no longer said to wait in its reactor)
static void dm_unpolled(DmPool *pl) {
  pthread_mutex_lock(&pl->mu);
  for (int i = 0; i < pl->npolling; i++)
    if (pl->polling[i] == dm_polled) {
      pl->polling[i] = pl->polling[--pl->npolling];
      break;
    }
  pthread_mutex_unlock(&pl->mu);
  dm_polled = 0;
}

// (up to `budget` of the pool's behaviours run here, this domain the
// pool's `i`th: the one in its slot first, then those ready; how many)
static int64_t dm_work(DmPool *pl, int64_t i, int64_t budget) {
  dm_running = pl;
  dm_index = i;
  if (dm_polled) dm_unpolled(pl);
  int64_t ran = 0;
  while (ran < budget) {
    DmBehaviour *b = dm_slot;
    if (b) dm_slot = NULL;
    else {
      if (!__atomic_load_n(&pl->front, __ATOMIC_RELAXED)) break;
      pthread_mutex_lock(&pl->mu);
      b = pl->front;
      if (b) {
        __atomic_store_n(&pl->front, b->after, __ATOMIC_RELAXED);
        if (!pl->front) pl->back = NULL;
      }
      pthread_mutex_unlock(&pl->mu);
      if (!b) break;
    }
    dm_run_behaviour(b);
    ran++;
  }
  return ran;
}

// (this domain, working the pool, with nothing to run: 0 where there is
// something after all; 1 where acts are pending in its reactor group
// `g`, said to wait there, to be woken where a behaviour is ready; else
// sleeping until one is, 0 then, or 2 once the pool is stopped)
static int64_t dm_idle(DmPool *pl, int64_t g, int pending) {
  if (dm_polled) dm_unpolled(pl);
  if (dm_slot) return 0;
  pthread_mutex_lock(&pl->mu);
  int64_t r = 0;
  if (pl->front) r = 0;
  else if (pl->stopping) r = 2;
  else if (pending && pl->npolling < DM_POLLERS) {
    pl->polling[pl->npolling++] = g;
    dm_polled = g;
    r = 1;
  } else {
    pl->sleeping++;
    pthread_cond_wait(&pl->work, &pl->mu);
    pl->sleeping--;
    r = pl->stopping && !pl->front ? 2 : 0;
  }
  pthread_mutex_unlock(&pl->mu);
  return r;
}

// (no behaviour left and no hold: the pool's domains stopped, those
// waiting in their reactors woken; `Just` why the first `failed` failed,
// where one did)
static V dm_drain(DmPool *pl) {
  pthread_mutex_lock(&pl->mu);
  while (atomic_load_explicit(&pl->live, memory_order_acquire) > 0) pthread_cond_wait(&pl->idle, &pl->mu);
  int was = pl->stopping;
  __atomic_store_n(&pl->stopping, 1, __ATOMIC_RELAXED);
  pthread_cond_broadcast(&pl->work);
  int np = pl->npolling;
  int64_t woken[DM_POLLERS];
  memcpy(woken, pl->polling, (size_t)np * sizeof(int64_t));
  pl->npolling = 0;
  pthread_mutex_unlock(&pl->mu);
  for (int i = 0; i < np; i++) heff_reactor_wake_group(woken[i]);
  if (!was)
    for (int i = 0; i < pl->n; i++) pthread_join(pl->threads[i], NULL);
  if (!pl->why) return NTAG(TAG_Nothing);
  V why = heff_str_text(pl->why);
  free(pl->why);
  pl->why = NULL;
  return heff_tag_n(TAG_Just, 1, &why);
}

// (the program's last reference gone: drained, or never, its domains
// stopped first)
static void dm_pool_let_go(void *x) {
  DmPool *pl = x;
  pthread_mutex_lock(&pl->mu);
  int was = pl->stopping;
  __atomic_store_n(&pl->stopping, 1, __ATOMIC_RELAXED);
  pthread_cond_broadcast(&pl->work);
  pthread_mutex_unlock(&pl->mu);
  if (!was)
    for (int i = 0; i < pl->n; i++) pthread_join(pl->threads[i], NULL);
  pthread_mutex_destroy(&pl->mu);
  pthread_cond_destroy(&pl->work);
  pthread_cond_destroy(&pl->idle);
  free(pl->threads);
  free(pl->why);
  free(pl);
}

// The operations --------------------------------------------------------------------

// (the custodian's provider, bundled wherever this one is: `rooted`'s
// operations are answered by it, the custodian current as `rooted` says)
static V heff_custodian_at(V p, V k, V here);

#define DM_ATOMIC(v) ((DmAtomic *)heff_foreign_ptr(v))

// (a cell read, and written, where the program does it at once: its
// operation not built, nor what comes after it, heff.compile.direct's --
// the same as `GetAtomic`'s and `SetAtomic`'s, `at` let go of after)
V heff_domainGet(V at) {
  V v = dm_get(DM_ATOMIC(at));
  heff_drop(at);
  return v;
}

V heff_domainSet(V at, V v) {
  dm_set(DM_ATOMIC(at), v);
  heff_drop(at);
  return NTAG(TAG_MkUnit);
}
#define DM_DOMAIN(v) ((DmDomain *)heff_foreign_ptr(v))
#define DM_QUEUE(v) ((DmQueue *)heff_foreign_ptr(v))

static V heff_provider_domain(V p, V k) {
  V x;
  // (a body's operation counted: the behaviour kept let out at its fuel's end)
  if (dm_slot && dm_in_body && ++dm_fuel >= DM_FUEL) {
    DmBehaviour *kept = dm_slot;
    dm_slot = NULL;
    dm_queued(kept);
  }
  // (an operation holding computations or none, as the effect sorts
  // them: the operation itself, inside)
  { V q = heff_dup(heff_field(p, 0)); heff_drop(p); p = q; }
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
  case TAG_GetOwned: {
    V rd = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V g = heff_dup(heff_field(rd, 1));
    V v = dm_get_owned(heff_field(rd, 0));
    heff_drop(rd);
    x = heff_answer(g, v);
    break;
  }
  case TAG_SetOwned: {
    V wr = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V v = heff_tget_v(wr, 1);
    x = heff_dup(heff_field(wr, 2));
    dm_set_owned(heff_field(wr, 0), v);
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
  // (a value kept for as long as the program runs: immortal, with all
  // it holds, counted by no domain from now on)
  case TAG_Forever: {
    V kp = dm_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V v = heff_tget_v(kp, 0), g = heff_dup(heff_field(kp, 1));
    heff_drop(kp);
    heff_freeze(v);
    x = heff_answer(g, v);
    break;
  }
  case TAG_NewPool: {
    int64_t n = INT_OF(heff_field(p, 0));
    V g = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(g, heff_foreign(dm_pool(n), dm_pool_let_go));
    break;
  }
  case TAG_NewTurnstile: {
    V g = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(g, heff_foreign(dm_turnstile(), dm_turnstile_let_go));
    break;
  }
  case TAG_Behave: {
    V bh = heff_dup(heff_field(p, 0));
    heff_drop(p);
    DmPool *pl = DM_POOL(heff_field(bh, 0));
    V ts = heff_dup(heff_field(bh, 1)), c = heff_carried(heff_dup(heff_field(bh, 2)));
    V f = heff_carried_fn(heff_dup(heff_field(bh, 3)));
    x = heff_dup(heff_field(bh, 4));
    dm_behave(pl, ts, c, f);
    heff_drop(bh);
    break;
  }
  case TAG_Work: {
    DmPool *pl = DM_POOL(heff_field(p, 0));
    int64_t i = INT_OF(heff_field(p, 1)), budget = INT_OF(heff_field(p, 2));
    V g = heff_dup(heff_field(p, 3));
    V pv = heff_dup(heff_field(p, 0));
    heff_drop(p);
    int64_t ran = dm_work(pl, i, budget);
    heff_drop(pv);
    x = heff_answer(g, heff_int(ran));
    break;
  }
  case TAG_Idle: {
    DmPool *pl = DM_POOL(heff_field(p, 0));
    int64_t grp = INT_OF(heff_field(p, 1));
    int pending = heff_field(p, 2) == NTAG(TAG_True);
    V g = heff_dup(heff_field(p, 3));
    V pv = heff_dup(heff_field(p, 0));
    heff_drop(p);
    int64_t r = dm_idle(pl, grp, pending);
    heff_drop(pv);
    x = heff_answer(g, heff_int(r));
    break;
  }
  case TAG_Here: {
    DmPool *pl = DM_POOL(heff_field(p, 0));
    V g = heff_dup(heff_field(p, 1));
    int64_t i = dm_running == pl ? dm_index : -1;
    heff_drop(p);
    x = heff_answer(g, heff_int(i));
    break;
  }
  case TAG_Hold: {
    atomic_fetch_add_explicit(&DM_POOL(heff_field(p, 0))->live, 1, memory_order_acq_rel);
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    break;
  }
  case TAG_Unhold: {
    dm_pool_less(DM_POOL(heff_field(p, 0)));
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    break;
  }
  case TAG_Drain: {
    V g = heff_dup(heff_field(p, 1));
    V pv = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V r = dm_drain(DM_POOL(pv));
    heff_drop(pv);
    x = heff_answer(g, r);
    break;
  }
  // (an operation of the custodian's, from `rooted`: the custodian's
  // provider's, the custodian current the one `rooted` says)
  case TAG_CustodyOp: {
    V here = heff_dup(heff_field(p, 0)), q = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V r = heff_custodian_at(q, k, here);
    heff_drop(here);
    return r;
  }
  default:
    heff_fail("not an operation of domain");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
