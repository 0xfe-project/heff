// boc's side, for the C backend: behaviour-oriented concurrency as the
// runtime's own effect.  It runs heff on threads of the system truly at
// once, so it says so:
#define HEFF_THREADS 1
// A cown is a foreign value: a turnstile (the last request queued at
// it), the value it holds, the behaviours waiting for it to be written
// (`once`), and what the last behaviour on it knew of the others (its
// causal context, below), each read and written by the one behaviour
// holding it, once it is first at the turnstile.
//
// Which behaviour comes before which is the paper's causality
// (Cheeseman et al., "When Behaviours Have to Happen: An Axiomatic
// Model of Causality in BoC", CONCUR 2026, Definition 7): a behaviour
// i is done before a behaviour j runs iff the two share a cown and i's
// spawn comes before j's in the causal order -- the order within one
// body or one `main` (program order), from a spawn to the body it
// spawned (the body runs after its spawn), and from a behaviour done
// on a cown to the next to hold that cown (the hand-over), these
// three chained.  Behaviours spawned by bodies with no such path
// between them are in no order, whichever was spawned first in time:
// the order of the clock is not causality (the paper's Definition 9,
// the operational semantics' `hb_os`, which Verona and this runtime
// before it kept by queueing each behaviour at its cowns as it was
// spawned, is more than the model asks for, and which of two unrelated
// behaviours it put first turned on where in the source each `when`
// stood).
//
// So a behaviour is not queued at its cowns as it is spawned, but once
// every behaviour it is to come after is done.  Each context that
// spawns -- a body running, `main`, the rest of an act awaited --
// carries what it knows: for each cown, the behaviours spawned on it
// before this point in the causal order and perhaps not done (`BcCtx`:
// a base, laid flat, and a list of its own spawns since).  A spawn
// looks up the latest the context knows of on each of the new
// behaviour's cowns and waits for those not done (`pending`, the
// behaviour on each one's list of dependents), captures what the
// context knows, for its body, and puts itself in the context.  The
// context passes along program order within the body, is captured by
// the spawn for the body it spawns, and is left at each cown by the
// behaviour done on it for the next holder, which merges it with its
// own as it starts: the paper's three edges.  Only the latest on a
// cown need be waited for: it waits for the earlier ones itself.  A
// context is laid flat again, those done dropped, once its list has
// grown past twice its base, so that `main`'s, which is never done,
// stays as large as what is live.  Each behaviour is counted
// (`refs`) by the contexts that know it, so one done is not reused
// while one still asks whether it is.
//
// Once none is left to wait for, the behaviour is queued and run as
// the OOPSLA 2023 paper has it (as Verona keeps it): a request at each
// of its cowns, in the order of their addresses, queued by swapping
// each in as the last, the one before it -- once that one's behaviour
// is done queueing at all of its own -- told it is next.  A behaviour
// on one cown is done queueing as it is swapped in, so its request
// says so from the start, and the one behind it never waits.  It runs
// once it is first at every one: a count of the cowns it still waits
// at, and one more until its queueing is done.  Done, it lets the next
// through at each, or, where none is behind it, takes its request out
// as the last by a compare-and-swap -- or, one coming meanwhile, waits
// for it to say so.  The turnstile takes the cowns all at once, so
// behaviours do not deadlock on them, and is first come first served
// at each: a behaviour queued is passed by none that comes to the cown
// after it, so none waits for ever.  No mutex but the pool's, around
// the behaviours ready to run that no thread of the pool readied --
// main's -- and around sleeping on its condition.  A behaviour readied
// by one a thread of the pool runs is kept in that thread's slot, the
// next it runs, where the slot is empty: none woken, the mutex not
// taken -- a chain of behaviours each readying the next runs on one
// thread -- and beyond the slot, in a queue of the thread's own, which
// the others take from (see "A worker's own queue").  One that has run
// 64 (`BC_BATCH`) from its slot in a row puts the next behind the others
// ready, where any are.  A body is never stopped partway, so the one
// kept would wait for the end of the body running: the thread counts the
// body's operations of this effect, and at the 256th (`BC_FUEL`) puts
// the one kept with the others ready, waking a thread to take it: not in
// its own queue, which the thread, held by the body, would not run, and
// a wake asked for it might be one already on its way to another.
//
// The body is a computation of the program's, `Turn -> m Unit`, `m` the
// monad of the handler in force at the `when`, which wrapped it there
// (`hmap`): given its turn, it takes what the cowns hold (`Taken`), runs
// what the program gave `when` on it, and gives what that comes to
// (`Given`) -- each an operation the program does at once where its
// world is known (heff.compile.direct: `heff_bocTaken`,
// `heff_bocGiven`), a slot of this thread's (`bc_taken`, `bc_given`)
// read or written; the turn, nothing here.  The cowns, as the program
// gave them -- a cown, or a tuple (`MkPair`) of cowns and tuples -- say
// the tuple: the runner reads each cown, builds the tuple of what they
// hold as theirs is built, runs the body, and, the body done, takes
// what it gave apart the same way, writing back each cown its bit of
// the behaviour's `ws` says.  No operation of the program's for any of
// it, and nothing built at run time between the body and the runtime.
// A condition (`once`) is checked of the tuple first: where it does
// not hold, the behaviour lets go of its cowns, untouched, and is done
// as far as those after it are concerned; it waits, parked on those
// its `rs` says, until a body writes one.  That body, at its end,
// spawns it again, as its own last spawn would be: after the writer,
// and after all the writer knew of on the waiter's cowns, and in no
// order with what others spawn after the writer not knowing of it.
// One still waiting once every other behaviour of its pool is done is
// dropped, its body never run.
//
// What a cown holds is the cown's: read by the behaviour holding it as
// a thread's own, not shared, marked `OWNED` (`heff_own_at`) once a body
// has written it, where the body's log (`HeffOwnLog`, the behaviour's
// while it runs) keeps what the body counted up of it.  The write back
// is kept by the log too, and done once the body is: a body that
// abandons, or does not write a cown, leaves it as it was.  An array a cown holds,
// held by the log and by the body alone, the body writes in place
// (`heff_own_set`), the log keeping what was there, put back where the
// cown is to hold what it held.  A value of a word read and written
// keeps nothing in the log.
//
// A body that abandons leaves its cowns as they were, and that is said
// on stderr, on the thread, before the cowns are let go; the program
// ends by 1 (`bc_failures`).  A body that fails -- panics, or fails in
// the runtime -- ends the program there and then, from its thread, as a
// failure anywhere does: nothing it held is let go of but by the system,
// and no other behaviour runs on.
//
// The program has one pool (`bc_program`), opened by its first operation
// of boc's outside every worker, and drained once the program's
// computation has returned (`heff_returned`).  Which pool a `when`
// spawns in is this thread's (`bc_current`): the program's, or the pool
// whose worker this is.  So no operation carries a pool.
// Each body starts its chain of calls last from naught (`heff_dep`), as
// a bounce's step does, not one past the last body's.

#include <stdatomic.h>
#include <sched.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static char *bc_strdup(const char *s) {
  char *c = heff_alloc(strlen(s) + 1);
  strcpy(c, s);
  return c;
}

static int64_t bc_cores(void) {
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

static void bc_relax(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause");
#endif
}

// (`f a0 a1`, asked for here, an abandon caught: as `heff_try`, two
// arguments given at once, so a body of two parameters -- its turn and
// what comes after it -- is called, not made a partial application)
static const char *bc_try2(V f, V a0, V a1, V *out) {
  jmp_buf here;
  jmp_buf *volatile up = heff_catch;
  volatile long dep = heff_dep;
  volatile size_t open = heff_nopen;
  heff_catch = &here;
  if (HEFF_SETJMP(here) == 0) {
    V a[2] = { a0, a1 };
    *out = heff_tr(heff_apply(f, 2, a));
    heff_catch = up;
    return NULL;
  }
  heff_catch = up;
  heff_dep = dep;
  heff_reopen(open);
  return heff_caught;
}

// (what an operation's `Some` holds, where `bc_open` puts it)
static HEFF_TLS V bc_held;

static V bc_open_fn(V self, V *a) {
  (void)self;
  bc_held = a[0];
  return NTAG(TAG_MkUnit);
}

static Clo bc_open_clo = {{0x80000000u, K_CLO, 0}, bc_open_fn, 1, 0};

// (what `s` holds, its types erased; `s` consumed)
static V bc_open(V s) {
  heff_drop(heff_answer(s, (V)&bc_open_clo));
  return bc_held;
}

// Cowns and behaviours ------------------------------------------------------

#define BC_POLLERS 1024

typedef struct BcBehaviour BcBehaviour;
typedef struct BcCown BcCown;
typedef struct BcPool BcPool;
typedef struct BcWait BcWait;
typedef struct BcPark BcPark;
typedef struct BcAct BcAct;
typedef struct BcOwn BcOwn;
typedef struct BcDep BcDep;

// (a behaviour waiting for another to be done: its place in that one's
// list of them -- one of its own two, or the system's where it waits
// for more; `BC_DONE` the list of one done)
struct BcDep {
  BcBehaviour *b;
  BcDep *next;
};

#define BC_DONE ((BcDep *)1)

// (a behaviour known on a cown: which, and the number it was spawned
// with, so that the room reused for a later one is known not to be it;
// and the context that spawned it and its number there, copied here so
// that merging two contexts' knowledge reads no behaviour, only the
// entries -- a behaviour another thread wrote is a cache line's trip.
// No count is kept: a behaviour's room is never given back to the
// system, so what is known may always be looked at, and one is spawned
// again in a room only once every look that might have found its
// former behaviour live is over -- see "Reclaiming")
typedef struct {
  BcCown *c;
  BcBehaviour *b;
  uint64_t serial, ctxid, seq;
} BcKnown;

#define BC_KNOWN 8
#define BC_PEND 16

// (a causal context: what it knows, a few entries of its own, more on
// the heap; how many were left after it was last pruned; its identity,
// and how many it has spawned -- each behaviour knows the context that
// spawned it and its number there, so that of two a context spawned on
// a cown, the later, which waited for the earlier, stands for it
// wherever both are known)
typedef struct {
  BcKnown *e;
  int n, cap, pruned;
  uint64_t id, seq;
  BcKnown few[BC_KNOWN];
} BcCtx;

typedef struct {
  _Atomic(BcBehaviour *) next;
  atomic_bool queued;
  BcCown *t;
} BcRequest;

// (a behaviour parked on cowns, to be queued again by the first body to
// write one of them -- 1 while it waits, 2 while one queues it, 0 once
// it is queued, or dropped -- held by each cown's list it is in, and by
// its pool's list while it waits)
struct BcWait {
  atomic_int state;
  atomic_int refs;
  BcBehaviour *b;
  BcWait *prev, *next;
};

// (a cown's list of those waiting on it, the last parked first)
struct BcPark {
  BcWait *w;
  BcPark *next;
};

#define BC_LINE 128

struct BcCown {
  _Atomic(BcRequest *) last;
  // (what the holder reads and writes on a line apart from `last`: each
  // behaviour queued on the cown swaps `last`, and each body run on it
  // writes `v`)
  _Alignas(BC_LINE) V v;  // the holder's
  BcPark *waiting;        // the holder's
  // (what the last behaviour done on it knew, for the next to hold it:
  // the hand-over's edge -- laid flat, `co`, and what the last few
  // holders that spawned nothing were spawned knowing, as it came,
  // `pend`, laid into `co` once there is no room for more)
  BcCtx co;
  int npend;
  BcKnown pend[BC_PEND];
};

struct BcBehaviour {
  // (what is written as it is spawned and as it waits, on a line of its
  // own: those it waits for to be done and one more until its spawn is
  // done (queued at 0), those waiting for it to be done (`BC_DONE` once
  // it is), and its places on the lists of those it waits for.  The
  // spawner of the next behaviour on a cown writes to this line while
  // the thread running this one reads the next)
  atomic_int pending;
  _Atomic(BcDep *) deps;
  BcDep dep0[2];
  // (what the thread running it reads and writes, on a line: the cowns
  // it still waits at, what it runs and holds, and its place in a
  // queue)
  _Alignas(BC_LINE) atomic_int waits;
  int n;
  int moved;
  BcBehaviour *after;
  BcPool *pool;
  V cs;      // the cowns, a cown or a tuple of them
  V body;    // Turn -> m Unit
  V cond;    // Nothing, or Just (t -> Bool)
  int64_t ws, rs;
  BcRequest *req;  // `few`, where the cowns are as few
  BcRequest few[2];
  // (the rest, read as it is spawned and as its body ends: its number,
  // no two behaviours' the same, by which what knows it tells it from
  // a later one in its room; the context that spawned it, and its
  // number of that context's spawns; what that context knew, captured
  // for its body, a few entries of its own; an act awaited outside
  // every worker, run by one; and the worker's that made it, to be
  // given back to, or none)
  _Alignas(BC_LINE) uint64_t serial, ctxid, seq;
  BcKnown *known;
  int nknown;
  BcAct *act;
  BcOwn *home;
  BcKnown kfew[4];
};

struct BcPool {
  pthread_mutex_t mu;
  pthread_cond_t work, idle;
  BcBehaviour *front, *back;
  atomic_llong live;
  int sleeping, stopping, n;
  // (the threads started, at most `n`, and those that look for a
  // behaviour not sleeping -- spinning before they sleep, or just
  // started -- which take one put with those ready without a wake)
  int started, ready;  // `ready`: how many are put with those ready, under `mu`
  atomic_int looking;
  pthread_t *threads;
  BcWait *parked;  // those waiting, under `mu`
  // (the reactor groups of the workers waiting in their reactors, acts
  // pending there, to be woken there where a behaviour is ready)
  int npolling;
  int64_t polling[BC_POLLERS];
  // (each worker's own queue, by its place among those started; a wake
  // asked of one sleeping, to take from them, not yet taken, under `mu`;
  // and whether one is being woken to, which keeps another from being)
  BcOwn *own;
  int wanted;
  atomic_int waking;
};

static void bc_wait_less(BcWait *w) {
  if (atomic_fetch_sub_explicit(&w->refs, 1, memory_order_acq_rel) == 1) free(w);
}

static void bc_ctx_free(BcCtx *h);

// (a cown holding `v`, shared; `v` consumed)
// (a cown on cache lines of its own: each spawn on it swaps `last`, and
// a line two cowns shared would be written by the spawns on both)
static BcCown *bc_cown(V v) {
  BcCown *c = aligned_alloc(BC_LINE, sizeof(BcCown));
  if (!c) heff_fail("out of memory");
  atomic_init(&c->last, NULL);
  c->v = heff_share(v);
  c->waiting = NULL;
  memset(&c->co, 0, sizeof c->co);
  c->npend = 0;
  return c;
}

// (the program's last reference to the cown gone: no behaviour holds it,
// since each holds a reference)
static void bc_cown_let_go(void *x) {
  BcCown *c = x;
  for (BcPark *p = c->waiting, *next; p; p = next) {
    next = p->next;
    bc_wait_less(p->w);
    free(p);
  }
  bc_ctx_free(&c->co);
  heff_drop(c->v);
  free(c);
}

static int bc_is_cown(V v) {
  return IS_OBJ(v) && v && OBJ(v)->kind == K_FGN && ((Fgn *)v)->let_go == bc_cown_let_go;
}

#define BC_COWN(v) ((BcCown *)((Fgn *)(v))->p)

// (none: a behaviour on no cown, `()`, boc's own -- what an act's
// `with` of no cown spawns)
#define BC_NONE(cs) ((cs) == NTAG(TAG_MkUnit))

// (how many cowns `cs` is, a cown or a tuple of them)
static int bc_count(V cs) {
  if (bc_is_cown(cs)) return 1;
  if (BC_NONE(cs)) return 0;
  if (heff_tag_of(cs) != TAG_MkPair) heff_fail("boc: a behaviour on what is not a cown, nor a tuple of them");
  return bc_count(heff_field(cs, 0)) + bc_count(heff_field(cs, 1));
}

// (... each, in the order of the tuple, from `*i`)
static void bc_each(V cs, BcRequest *req, int *i) {
  if (bc_is_cown(cs)) {
    req[(*i)++].t = BC_COWN(cs);
    return;
  }
  if (BC_NONE(cs)) return;
  bc_each(heff_field(cs, 0), req, i);
  bc_each(heff_field(cs, 1), req, i);
}

// (whether the `i`th cown's bit is set: all of them where the bits are
// -1)
static inline int bc_bit(int64_t bits, int i) {
  return bits < 0 ? 1 : i < 62 && ((bits >> i) & 1);
}

// The pool counts what is queued, running or pending (`live`) a batch
// at a time, not one at a time, each a write all threads make to one
// line: a thread takes `BC_CREDIT` at once and spends them as it
// spawns, and what is done on it -- a behaviour, an act -- is its
// credit again, given back a batch at a time where it has too many.
// So `live` is what is queued, running or pending and the credit the
// threads hold; a thread gives back all it holds before it waits --
// to sleep, in its reactor -- and the program's before the end, so
// that `live` comes to 0 once nothing is left.
#define BC_CREDIT 64
static HEFF_TLS int64_t bc_credit;

// (one more queued, running or pending)
static inline void bc_pool_more(BcPool *pl) {
  if (bc_credit > 0) {
    bc_credit--;
    return;
  }
  atomic_fetch_add_explicit(&pl->live, BC_CREDIT, memory_order_acq_rel);
  bc_credit = BC_CREDIT - 1;
}

// (what this thread holds given back, before it waits: where nothing is
// left then, the end told)
static void bc_runs_back(BcPool *pl);
static void bc_settle(BcPool *pl) {
  bc_runs_back(pl);
  int64_t c = bc_credit;
  bc_credit = 0;
  if (c && atomic_fetch_sub_explicit(&pl->live, c, memory_order_acq_rel) == c) {
    pthread_mutex_lock(&pl->mu);
    pthread_cond_broadcast(&pl->idle);
    pthread_mutex_unlock(&pl->mu);
  }
}

// (one done: this thread's credit; a batch given back where it holds
// two, never all it holds, so `live` stays above 0 here)
static void bc_pool_less(BcPool *pl) {
  if (++bc_credit > 2 * BC_CREDIT) {
    atomic_fetch_sub_explicit(&pl->live, BC_CREDIT, memory_order_acq_rel);
    bc_credit -= BC_CREDIT;
  }
}

#define BC_BATCH 64
#define BC_FUEL 256
#define BC_SPINS 4000

// (the pool whose worker this thread is, if one; its slot; the fuel of
// the body it runs; and the pool a `when` here spawns in)
static HEFF_TLS BcPool *bc_running;
static HEFF_TLS BcBehaviour *bc_slot;
static HEFF_TLS int bc_fuel, bc_in_body;
static HEFF_TLS BcPool *bc_current;

// (what the behaviour runs and holds made another thread's to take: by
// the thread whose it is, once)
static void bc_movable(BcBehaviour *b) {
  if (b->moved) return;
  b->body = heff_move(b->body);
  b->cond = heff_move(b->cond);
  b->cs = heff_share(b->cs);
  b->moved = 1;
}

static void bc_thread_more(BcPool *pl);

// Causality -------------------------------------------------------------------
//
// What a context knows (`BcCtx`) is a few entries, a behaviour at a
// cown each (`BcKnown`): for each cown, the latest it knows of on it
// -- as it spawns one on a cown, that one stands for every other it
// knew of on the cown, having waited for them, so a context that
// spawns keeps one entry a cown.  A spawn copies what the context
// knows into the behaviour, for its body (a copy, not a share: the
// entries are few, and nothing is counted).  A body begins by taking
// what its spawn knew and what each of its cowns' last holder knew,
// together, those done dropped, and of those one context spawned on a
// cown only the latest; and leaves what it knows at each of its cowns
// as it ends.  Entries point at behaviours without counting them, so
// a behaviour's room is never given back to the system, and one is
// spawned again in a room only once no thread may still take its
// former behaviour for live: see "Reclaiming".  Entries that are dead
// -- the behaviour done, or its room another's by now (its `serial`
// changed) -- are dropped as a context is pruned, when it has grown to
// twice what the last pruning left.

static void bc_enqueue(BcBehaviour *b);
static void bc_body_fill(void);

// (whether the entry's behaviour is not yet done: its list of
// dependents first -- `BC_DONE` once done, and empty again only once
// the room is another's, whose number is then seen too, the number
// written before the list is)
static inline int bc_live(const BcKnown *k) {
  if (atomic_load_explicit(&k->b->deps, memory_order_acquire) == BC_DONE) return 0;
  return k->b->serial == k->serial;
}

static inline void bc_ctx_ready(BcCtx *h) {
  if (!h->e) {
    h->e = h->few;
    h->cap = BC_KNOWN;
  }
}

// (the context's heap given back; the context empty)
static void bc_ctx_free(BcCtx *h) {
  if (h->e && h->e != h->few) free(h->e);
  h->e = NULL;
  h->n = h->cap = h->pruned = 0;
}

static inline void bc_ctx_clear(BcCtx *h) {
  h->n = 0;
  h->pruned = 0;
}

// (by the cown, then by the context that spawned them, the later
// spawn first, then by address)
static int bc_by_known(const void *x, const void *y) {
  const BcKnown *a = x, *b = y;
  if (a->c != b->c) return (uintptr_t)a->c < (uintptr_t)b->c ? -1 : 1;
  if (a->ctxid != b->ctxid) return a->ctxid < b->ctxid ? -1 : 1;
  if (a->seq != b->seq) return a->seq > b->seq ? -1 : 1;
  return a->b == b->b ? 0 : (uintptr_t)a->b < (uintptr_t)b->b ? -1 : 1;
}

// (what the context knows pruned: those dead dropped, and of those one
// context spawned on a cown only the latest -- the entries sorted so,
// and then each once)
static void bc_ctx_prune(BcCtx *h) {
  if (h->n > 1) qsort(h->e, (size_t)h->n, sizeof(BcKnown), bc_by_known);
  int n = 0;
  for (int i = 0; i < h->n; i++) {
    if (!bc_live(&h->e[i])) continue;
    if (n && h->e[n - 1].c == h->e[i].c && (h->e[n - 1].ctxid == h->e[i].ctxid || h->e[n - 1].b == h->e[i].b)) continue;
    h->e[n++] = h->e[i];
  }
  h->n = n;
  h->pruned = n > BC_KNOWN ? n : BC_KNOWN;
}

// (room for one more entry: pruned first where it has grown to twice
// what the last pruning left, else the heap)
static void bc_ctx_room(BcCtx *h) {
  bc_ctx_ready(h);
  if (h->n >= 2 * h->pruned && h->n >= BC_KNOWN) bc_ctx_prune(h);
  if (h->n < h->cap) return;
  BcKnown *e = heff_alloc(2 * (size_t)h->cap * sizeof *e);
  memcpy(e, h->e, (size_t)h->n * sizeof *e);
  if (h->e != h->few) free(h->e);
  h->e = e;
  h->cap *= 2;
}

// (a context's identity, no two the same: this thread's number, given
// once, and a count of its own; and a behaviour's number the same way)
static atomic_uint_fast32_t bc_threads;
static HEFF_TLS uint64_t bc_thread_tag, bc_serials;

static inline uint64_t bc_numbered(void) {
  if (!bc_thread_tag) bc_thread_tag = (uint64_t)(atomic_fetch_add_explicit(&bc_threads, 1, memory_order_relaxed) + 1) << 40;
  return bc_thread_tag | ++bc_serials;
}

static inline void bc_ctx_open(BcCtx *h) {
  h->id = bc_numbered();
  h->seq = 0;
}

// (`b`, spawned by the context on its cowns, known to it: the one
// entry at each of them)
static void bc_ctx_add(BcCtx *h, BcBehaviour *b) {
  for (int i = 0; i < b->n; i++) {
    BcCown *c = b->req[i].t;
    int n = 0;
    for (int j = 0; j < h->n; j++)
      if (h->e[j].c != c) h->e[n++] = h->e[j];
    h->n = n;
    bc_ctx_room(h);
    h->e[h->n].c = c;
    h->e[h->n].b = b;
    h->e[h->n].serial = b->serial;
    h->e[h->n].ctxid = b->ctxid;
    h->e[h->n].seq = b->seq;
    h->n++;
  }
}

// (the entries `e`, `n` of them, put in `h`: each once, and of those
// one context spawned on a cown only the latest.  Nothing but the
// entries is read; those dead are dropped as the context is pruned)
// (room for `want` entries in all)
static void bc_ctx_reserve(BcCtx *h, int want) {
  bc_ctx_ready(h);
  if (want <= h->cap) return;
  int cap = h->cap;
  while (cap < want) cap *= 2;
  BcKnown *e = heff_alloc((size_t)cap * sizeof *e);
  memcpy(e, h->e, (size_t)h->n * sizeof *e);
  if (h->e != h->few) free(h->e);
  h->e = e;
  h->cap = cap;
}

#define BC_MANY 64

static void bc_ctx_merge(BcCtx *h, const BcKnown *e, int n) {
  if (!h->n) {
    // (one context's knowledge, an entry a cown: copied)
    bc_ctx_reserve(h, n);
    if (n) memcpy(h->e, e, (size_t)n * sizeof *e);
    h->n = n;
    return;
  }
  if (h->n > BC_MANY) {
    // (one that knows many: the entries put in as they are, each once
    // and the latest only once it is pruned, which it is once it has
    // doubled -- not looked for among the many each)
    for (int i = 0; i < n; i++) {
      bc_ctx_room(h);
      h->e[h->n++] = e[i];
    }
    return;
  }
  for (int i = 0; i < n; i++) {
    int keep = 1;
    for (int j = 0; j < h->n; j++) {
      BcKnown *k = &h->e[j];
      if (k->c != e[i].c) continue;
      if (k->b == e[i].b) {
        keep = 0;
        break;
      }
      if (k->ctxid == e[i].ctxid) {
        if (k->seq < e[i].seq) *k = e[i];
        keep = 0;
        break;
      }
    }
    if (!keep) continue;
    bc_ctx_room(h);
    h->e[h->n++] = e[i];
  }
}

// (what `h` knows copied into `b`, for its body)
static void bc_captured(BcBehaviour *b, BcCtx *h) {
  int n = h->n;
  b->nknown = n;
  if (n <= (int)(sizeof b->kfew / sizeof b->kfew[0])) b->known = b->kfew;
  else b->known = heff_alloc((size_t)n * sizeof(BcKnown));
  if (n) memcpy(b->known, h->e, (size_t)n * sizeof(BcKnown));
}

// (what `src` knows copied into `dst`, which knew nothing)
static void bc_ctx_copy(BcCtx *dst, const BcCtx *src) {
  bc_ctx_ready(dst);
  if (src->n > dst->cap) {
    if (dst->e != dst->few) free(dst->e);
    dst->e = heff_alloc((size_t)src->n * sizeof(BcKnown));
    dst->cap = src->n;
  }
  if (src->n) memcpy(dst->e, src->e, (size_t)src->n * sizeof(BcKnown));
  dst->n = src->n;
  dst->pruned = src->pruned;
}

static inline void bc_capture_free(BcBehaviour *b) {
  if (b->known && b->known != b->kfew) free(b->known);
  b->known = NULL;
  b->nknown = 0;
}

// (the context of what runs on this thread: `bc_outside`, main's, or a
// worker's outside every body; `bc_body`, the body a worker runs; or an
// act's turn's, while its rest runs -- `bc_now` which, NULL for
// `bc_outside`)
static HEFF_TLS BcCtx bc_outside, bc_body;
static HEFF_TLS BcCtx *bc_now;

// Reclaiming.  A behaviour's room is reused, never given back to the
// system (but at the program's end): the entries of what contexts know
// point at it without counting it, and look at it whenever they are
// pruned or merged, to see whether its behaviour is done -- a dead
// entry may be looked at long after.  Reusing a room is the one danger:
// a thread that has just found a behaviour live, by its number, and is
// about to wait for it must not find the room spawned again under it
// (it would wait for the wrong one, perhaps for ever).  So a thread
// looks at what is known only within a section, during which it says
// which epoch it is in (`BcEpoch`, one a thread, `local`), and a room
// let go of (`bc_retire`) is kept until every thread that was in a
// section when it was let go of has left it: the epoch is advanced
// once every thread in a section is in the current one, and the rooms
// let go of two epochs ago are reused then -- by a look at every
// thread's epoch every `BC_RETIRES`.  A thread that leaves a section
// says it is in none, so one asleep holds nothing up.
#define BC_RETIRES 128

typedef struct BcEpoch BcEpoch;
struct BcEpoch {
  _Alignas(BC_LINE) atomic_uint_fast64_t local;  // 0: in no section; else the epoch, shifted, and 1
  BcEpoch *next;
  int depth;
  BcBehaviour *retired[3];  // by the epoch they were let go of in, mod 3
  uint64_t retired_in[3];   // which epoch each holds
  int since;
};

static atomic_uint_fast64_t bc_epoch = 1;
static pthread_mutex_t bc_epochs_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(BcEpoch *) bc_epochs;
static HEFF_TLS BcEpoch *bc_mine;

static BcEpoch *bc_epoch_mine(void) {
  if (bc_mine) return bc_mine;
  BcEpoch *m = aligned_alloc(BC_LINE, sizeof(BcEpoch));
  if (!m) heff_fail("out of memory");
  memset(m, 0, sizeof *m);
  atomic_init(&m->local, 0);
  pthread_mutex_lock(&bc_epochs_mu);
  m->next = atomic_load_explicit(&bc_epochs, memory_order_relaxed);
  atomic_store_explicit(&bc_epochs, m, memory_order_release);
  pthread_mutex_unlock(&bc_epochs_mu);
  return bc_mine = m;
}

// (a section begun: in the epoch of now, said before anything is
// looked at)
static void bc_enter(void) {
  BcEpoch *m = bc_epoch_mine();
  if (m->depth++) return;
  uint64_t e = atomic_load_explicit(&bc_epoch, memory_order_relaxed);
  atomic_store_explicit(&m->local, (e << 1) | 1, memory_order_seq_cst);
  atomic_thread_fence(memory_order_seq_cst);
}

static void bc_exit(void) {
  BcEpoch *m = bc_mine;
  if (--m->depth) return;
  atomic_store_explicit(&m->local, 0, memory_order_release);
}

static void bc_behaviour_free(BcBehaviour *b);

// (the rooms of bucket `k` reused)
static void bc_reused(BcEpoch *m, int k) {
  for (BcBehaviour *b = m->retired[k], *next; b; b = next) {
    next = b->after;
    bc_behaviour_free(b);
  }
  m->retired[k] = NULL;
}

// (the rooms this thread let go of two epochs or more before `now`
// reused: safe, every thread then in a section having left it since)
static void bc_reclaimed(BcEpoch *m, uint64_t now) {
  for (int k = 0; k < 3; k++)
    if (m->retired[k] && m->retired_in[k] + 2 <= now) bc_reused(m, k);
}

// (the epoch advanced where every thread in a section is in it -- by
// whichever thread looks first -- and what this thread may reuse by
// now reused)
static void bc_reclaim(BcEpoch *m) {
  m->since = 0;
  uint64_t e = atomic_load_explicit(&bc_epoch, memory_order_acquire);
  int all = 1;
  for (BcEpoch *r = atomic_load_explicit(&bc_epochs, memory_order_acquire); r && all; r = r->next) {
    uint64_t l = atomic_load_explicit(&r->local, memory_order_acquire);
    if ((l & 1) && (l >> 1) != e) all = 0;
  }
  if (all) atomic_compare_exchange_strong_explicit(&bc_epoch, &e, e + 1, memory_order_acq_rel, memory_order_acquire);
  bc_reclaimed(m, atomic_load_explicit(&bc_epoch, memory_order_acquire));
}

// (`b`'s room let go of: kept until it is safe to reuse.  The bucket
// of this epoch holds, if anything, rooms of three epochs back, safe
// by now: reused first)
static void bc_retire(BcBehaviour *b) {
  BcEpoch *m = bc_epoch_mine();
  uint64_t e = atomic_load_explicit(&bc_epoch, memory_order_acquire);
  int k = (int)(e % 3);
  if (m->retired[k] && m->retired_in[k] != e) bc_reused(m, k);
  m->retired_in[k] = e;
  b->after = m->retired[k];
  m->retired[k] = b;
  if (++m->since >= BC_RETIRES) bc_reclaim(m);
}

// (the rooms a thread ending has let go of, and those it kept, handed
// to the program's end to free: under the epochs' mutex)
static BcBehaviour *bc_orphans;

// (`j` to wait for `i`, where `i` is not done and `j` does not already:
// `j`'s place on `i`'s list of dependents, one of its own two where it
// waits for two at most, and `j`'s pending one more.  `j` may then be
// queued by another thread, so what it holds is made another's to
// take.  Within a section: `i` found live is not spawned again under
// it)
static void bc_need(BcBehaviour *j, const BcKnown *k, BcBehaviour **seen, int *nseen, int *ndeps) {
  BcBehaviour *i = k->b;
  if (i == j) return;
  for (int s = 0; s < *nseen; s++)
    if (seen[s] == i) return;
  if (*nseen < 16) seen[(*nseen)++] = i;
  if (!bc_live(k)) return;
  BcDep *old = atomic_load_explicit(&i->deps, memory_order_acquire);
  if (old == BC_DONE) return;
  if (!*ndeps) bc_movable(j);
  BcDep *d = *ndeps < 2 ? &j->dep0[*ndeps] : heff_alloc(sizeof(BcDep));
  d->b = j;
  // (counted before it is on the list: `i`, done as soon as it is,
  // takes one off, and `j` is not queued before its spawn is done, the
  // spawn's own count still held)
  atomic_fetch_add_explicit(&j->pending, 1, memory_order_relaxed);
  for (;;) {
    d->next = old;
    if (atomic_compare_exchange_weak_explicit(&i->deps, &old, d, memory_order_release, memory_order_acquire)) break;
    if (old == BC_DONE) {
      if (d != &j->dep0[0] && d != &j->dep0[1]) free(d);
      atomic_fetch_sub_explicit(&j->pending, 1, memory_order_relaxed);
      return;
    }
  }
  (*ndeps)++;
}

// (`j` to wait for what the context knows of on each of its cowns, and
// for `also`, where one is given: those not done; whether it waits for
// any)
static int bc_needs(BcBehaviour *j, BcCtx *h, BcBehaviour *also) {
  BcBehaviour *seen[16];
  int nseen = 0, ndeps = 0;
  if (also) {
    BcKnown k = { NULL, also, also->serial, also->ctxid, also->seq };
    bc_need(j, &k, seen, &nseen, &ndeps);
  }
  for (int i = 0; i < j->n; i++) {
    BcCown *c = j->req[i].t;
    for (int at = 0; at < h->n; at++)
      if (h->e[at].c == c) bc_need(j, &h->e[at], seen, &nseen, &ndeps);
  }
  return ndeps > 0;
}

// (one it waited for done, or its spawn: queued at its cowns where none
// is left)
static inline void bc_resolve(BcBehaviour *j) {
  if (atomic_fetch_sub_explicit(&j->pending, 1, memory_order_acq_rel) == 1) bc_enqueue(j);
}

// (a number of this thread's, the next: xorshift, seeded by the clock
// and the thread)
static HEFF_TLS uint64_t bc_seed;

static inline uint64_t bc_random(void) {
  uint64_t x = bc_seed;
  if (!x) x = (uint64_t)heff_now_ms() * 0x9e3779b97f4a7c15ULL ^ (uintptr_t)&bc_seed ^ 0x2545f4914f6cdd1dULL;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return bc_seed = x;
}

// (`b` done: those waiting for it told, each perhaps queued here.
// Several told at once are in no order with one another -- that is
// why they waited for `b` and not for each other -- and they are
// queued in an order drawn at random, not the order they came to wait
// or its reverse: either would put the ones a body spawned first
// first, or last, at every cown, every time, an order of the clock's
// the model does not have.  A place of the system's freed)
static void bc_done(BcBehaviour *b) {
  BcDep *d = atomic_exchange_explicit(&b->deps, BC_DONE, memory_order_acq_rel);
  if (!d) return;
  if (!d->next) {
    BcBehaviour *j = d->b;
    if (d < j->dep0 || d >= j->dep0 + 2) free(d);
    bc_resolve(j);
    return;
  }
  BcBehaviour *few[32], **js = few;
  int n = 0, cap = 32;
  for (BcDep *next; d; d = next) {
    next = d->next;
    BcBehaviour *j = d->b;
    if (d < j->dep0 || d >= j->dep0 + 2) free(d);
    if (n == cap) {
      BcBehaviour **more = heff_alloc(2 * (size_t)cap * sizeof *more);
      memcpy(more, js, (size_t)n * sizeof *more);
      if (js != few) free(js);
      js = more;
      cap *= 2;
    }
    js[n++] = j;
  }
  for (int i = n - 1; i > 0; i--) {
    int k = (int)(bc_random() % (uint64_t)(i + 1));
    BcBehaviour *t = js[i];
    js[i] = js[k];
    js[k] = t;
  }
  for (int i = 0; i < n; i++) bc_resolve(js[i]);
  if (js != few) free(js);
}

// (`b`, spawned by the context `h`: those it is to come after waited
// for, what `h` knows copied for its body, `h` told of it; and, where
// none is to be waited for, queued now.  Within a section)
#define BC_AHEAD 8192
#define BC_AHEAD_SPAWNS 4096
#define BC_AHEAD_SPINS 1024
static void bc_held_back(BcPool *pl);

static void bc_spawned(BcBehaviour *b, BcCtx *h, BcBehaviour *also) {
  bc_ctx_ready(h);
  if (!h->id) bc_ctx_open(h);
  b->ctxid = h->id;
  b->seq = ++h->seq;
  bc_enter();
  int waiting = bc_needs(b, h, also);
  bc_captured(b, h);
  bc_ctx_add(h, b);
  bc_exit();
  // (its spawn done first -- queued now, or by the last it waits for --
  // and only then the spawner held back: a behaviour whose spawn is not
  // done cannot be queued, and would hold up the runner it is waiting
  // on, which the spawner is waiting for)
  BcPool *pl = b->pool;
  bc_resolve(b);
  if (waiting && h->seq > BC_AHEAD_SPAWNS && pl && atomic_load_explicit(&pl->live, memory_order_relaxed) > BC_AHEAD)
    bc_held_back(pl);
}

// (the behaviour put behind those ready, a sleeping thread woken; where
// none sleeps or waits in its reactor, more are ready than threads look
// for one, and fewer than the machine's cores are started, one more
// started for it)
static void bc_queued(BcBehaviour *b) {
  BcPool *pl = b->pool;
  bc_movable(b);
  b->after = NULL;
  pthread_mutex_lock(&pl->mu);
  if (pl->back) pl->back->after = b;
  else __atomic_store_n(&pl->front, b, __ATOMIC_RELAXED);
  pl->back = b;
  pl->ready++;
  int64_t wake = 0;
  if (pl->sleeping > 0) pthread_cond_signal(&pl->work);
  else if (pl->npolling > 0) wake = pl->polling[--pl->npolling];
  else if (pl->started < pl->n && pl->ready > atomic_load_explicit(&pl->looking, memory_order_acquire))
    bc_thread_more(pl);
  pthread_mutex_unlock(&pl->mu);
  if (wake) heff_reactor_wake_group(wake);
}

// A worker's own queue.  A behaviour a worker readies beyond its slot
// is put at the back of a queue of the worker's own, not with those
// ready: a ring of `BC_RING`, the worker alone putting at its back, the
// worker and the others taking from its front by a compare-and-swap, no
// lock.  The worker runs them after its slot, in their order; one with
// nothing to run takes half of another's into its own, and runs them.
// The order behaviours run in is the cowns' queues' still: one comes to
// a queue only once its turn has come.  A ring full, the behaviour goes
// with those ready.
//
// What is in a worker's queue its worker is there to run -- it waits,
// to sleep or in its reactor, only once its queue is empty -- so none is
// lost for want of a wake: a wake is for running them at once, not for
// running them at all.  A burst of small behaviours run where they were
// readied is run on one core, what they share in its cache; spread over
// every core, each of them is slower than the burst is long: 2000
// streams sent an event at once take a third of the time on one core
// that they take spread over fourteen.  So another worker is woken to
// take from a queue only where its front has waited `BC_AGE` ms
// (`bc_aged`) -- a thief's queue ages as any does, so that a long burst
// is spread a worker at a time -- or where a body readies the first of
// it, the body perhaps running on for long, the queue behind it; and
// only one at a time: none where a worker looks for one already, or is
// being woken.  (Waking at a millisecond spreads a burst of 15 ms over
// every core again.)  The ring is as large as a burst of a few thousand,
// so that one does not spill to those ready, which any thread takes.
#define BC_RING 4096
#define BC_AGE 5

struct BcOwn {
  _Alignas(BC_LINE) atomic_uint head;  // taken from: by any
  atomic_uint tail;                    // put at: by its worker
  int64_t since;                       // its worker's: when it last held none, ms
  BcPool *pool;                        // whose worker's it is
  struct BcRun *runs;                  // its worker's: one for each of the pool's
  _Atomic(BcBehaviour *) at[BC_RING];
  // (the behaviours it made let go of by others, given back to it, and
  // about how many since it last took them)
  _Alignas(BC_LINE) _Atomic(BcBehaviour *) back;
  atomic_int nback;
};

// (this worker's queue, and its place among the pool's)
static HEFF_TLS BcOwn *bc_own;
static HEFF_TLS int bc_me;

static inline int bc_own_empty(BcOwn *q) {
  return atomic_load_explicit(&q->head, memory_order_acquire) == atomic_load_explicit(&q->tail, memory_order_acquire);
}

// (`b` at the back of this worker's queue: 0 where it is full)
static int bc_own_put(BcBehaviour *b) {
  BcOwn *q = bc_own;
  unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
  unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
  if (t - h >= BC_RING) return 0;
  if (t == h) q->since = heff_now_ms();
  atomic_store_explicit(&q->at[t % BC_RING], b, memory_order_relaxed);
  atomic_store_explicit(&q->tail, t + 1, memory_order_release);
  return 1;
}

// (the front of this worker's queue taken, where it holds one)
static BcBehaviour *bc_own_take(void) {
  BcOwn *q = bc_own;
  for (;;) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (t == h) return NULL;
    BcBehaviour *b = atomic_load_explicit(&q->at[h % BC_RING], memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(&q->head, &h, h + 1, memory_order_release, memory_order_relaxed))
      return b;
  }
}

// (half of what `v` holds, the greater half, taken into this worker's
// queue, empty: the last of them handed back to run, or none.  A head
// and a tail read between another's take and put may say more than the
// ring holds: read again)
static BcBehaviour *bc_steal_from(BcOwn *v) {
  BcOwn *q = bc_own;
  unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
  for (;;) {
    unsigned h = atomic_load_explicit(&v->head, memory_order_acquire);
    unsigned vt = atomic_load_explicit(&v->tail, memory_order_acquire);
    unsigned n = vt - h;
    n -= n / 2;
    if (n == 0) return NULL;
    if (n > BC_RING / 2) continue;
    for (unsigned i = 0; i < n; i++)
      atomic_store_explicit(&q->at[(t + i) % BC_RING],
                            atomic_load_explicit(&v->at[(h + i) % BC_RING], memory_order_relaxed),
                            memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(&v->head, &h, h + n, memory_order_acq_rel, memory_order_relaxed)) {
      BcBehaviour *b = atomic_load_explicit(&q->at[(t + n - 1) % BC_RING], memory_order_relaxed);
      if (n > 1) {
        q->since = heff_now_ms();
        atomic_store_explicit(&q->tail, t + n - 1, memory_order_release);
      }
      return b;
    }
  }
}

// (one taken from the first of the others' queues that holds one, from
// the one after this worker's on)
static BcBehaviour *bc_steal(BcPool *pl) {
  for (int k = 1; k < pl->n; k++) {
    BcOwn *v = &pl->own[(bc_me + k) % pl->n];
    if (bc_own_empty(v)) continue;
    BcBehaviour *b = bc_steal_from(v);
    if (b) return b;
  }
  return NULL;
}

// (whether any is ready: with those ready, or in a worker's queue)
static int bc_any_ready(BcPool *pl) {
  if (__atomic_load_n(&pl->front, __ATOMIC_RELAXED)) return 1;
  for (int k = 0; k < pl->n; k++)
    if (!bc_own_empty(&pl->own[k])) return 1;
  return 0;
}

// (one more worker to take from the queues: where none looks for one
// already, nor is being woken -- one sleeping woken, else one waiting
// in its reactor, else one more started.  What was put is put before
// either is read, and one that stops looking, or is woken, looks at the
// queues after it says so: so the one looking, or woken, sees what it
// was not woken for)
static void bc_wake_one(BcPool *pl) {
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(&pl->looking, memory_order_acquire) > 0) return;
  int no = 0;
  if (!atomic_compare_exchange_strong_explicit(&pl->waking, &no, 1, memory_order_acq_rel, memory_order_relaxed))
    return;
  int64_t wake = 0;
  int one = 1;
  pthread_mutex_lock(&pl->mu);
  if (pl->sleeping > pl->wanted) {
    pl->wanted++;
    pthread_cond_signal(&pl->work);
  } else if (pl->npolling > 0) wake = pl->polling[--pl->npolling];
  else if (pl->started < pl->n) bc_thread_more(pl);
  else one = 0;
  pthread_mutex_unlock(&pl->mu);
  if (wake) heff_reactor_wake_group(wake);
  if (!one) atomic_store_explicit(&pl->waking, 0, memory_order_seq_cst);
}

// A spawner far ahead.  A body that spawns behaviours faster than
// they are run -- each on a cown, behind the one before -- piles them
// up: every one takes a room, the rooms stop fitting in the caches, and
// the thread running them, which must fetch each from memory, falls
// further behind, so that more pile up.  (The turnstile alone used to
// hold such a spawner back by chance: each of its spawns swapped at a
// line the runner wrote too.)  So a spawner whose behaviour must wait
// for one not done, while the pool has more than `BC_AHEAD` behaviours
// queued, pending or running, waits a little itself, up to
// `BC_AHEAD_SPINS` rounds, until the pool has fewer: the pile stays as
// large as the caches, and the runners stay fast.  Only a spawner that
// has spawned many (`BC_AHEAD_SPAWNS`) is held back: a body that spawns
// a few while many are queued -- a node of a tree of them -- is not the
// cause, and waiting would not help.  A wait of time alone, on no
// condition, so nothing is held up for good.

static void bc_held_back(BcPool *pl) {
  for (int i = 0; i < BC_AHEAD_SPINS; i++) {
    // (what this body readied waits in this worker's queue, which the
    // body, spinning here, will not run: put with those ready, which
    // wakes a thread to take them)
    if (bc_own)
      for (BcBehaviour *x; (x = bc_own_take());) bc_queued(x);
    for (int j = 0; j < 256; j++) bc_relax();
    if (atomic_load_explicit(&pl->live, memory_order_relaxed) <= BC_AHEAD) return;
  }
}

// (this worker's queue's front waited `BC_AGE`: one more woken to take
// from it, and the wait counted from now)
static void bc_aged(BcPool *pl) {
  BcOwn *q = bc_own;
  if (bc_own_empty(q)) return;
  int64_t now = heff_now_ms();
  if (now - q->since < BC_AGE) return;
  q->since = now;
  bc_wake_one(pl);
}

// (the behaviour made another's to take, at the back of this worker's
// queue: where it is full, with those ready.  Readied by a body, the
// queue's first, one more woken -- its pool read before it is put, as
// another may take it, run it and let go of it at once)
static void bc_spill(BcBehaviour *b) {
  BcPool *pl = b->pool;
  bc_movable(b);
  int first = bc_own_empty(bc_own);
  if (!bc_own_put(b)) bc_queued(b);
  // (the first of the queue: one more woken to take it -- the slot is
  // taken, so this worker has two to run, and a body readying it may
  // run on for long.  Before behaviours were queued only once those
  // they come after are done, most were readied by the body that
  // spawned them, and taken from its queue by workers looking for one;
  // now most are readied by a behaviour done on their cown, on the
  // worker running that cown's chain, and a worker that comes to hold
  // two cowns' chains would keep both while the others sleep)
  else if (first) bc_wake_one(pl);
}

// (whether this worker is looking at its reactor: where an act's rest
// readies a behaviour into its slot, the look comes back, not waiting
// on, so that the behaviour runs now)
static HEFF_TLS int bc_polling;

// (the behaviour ready: in this thread's slot, where it runs the pool's
// behaviours and the slot is empty, else in its queue; one readied by
// any other thread queued with those ready)
static void bc_ready(BcBehaviour *b) {
  if (bc_running != b->pool) bc_queued(b);
  else if (!bc_slot && !bc_in_body) {
    bc_slot = b;
#ifndef _WIN32
    if (bc_polling) heff_woken = 1;
#endif
  } else bc_spill(b);
}

static void bc_through(BcBehaviour *b) {
  if (atomic_fetch_sub_explicit(&b->waits, 1, memory_order_acq_rel) == 1) bc_ready(b);
}

// (a behaviour's room: one this thread let go of, kept for the next --
// a chain of behaviours on one thread mallocs none -- else one a worker
// made given back to it, else one made by no worker given back by one
// (`bc_rooms`), else the system's.  A behaviour one worker makes and
// another runs is given back to the worker that made it, its `home`:
// the one letting go of it keeps a run of `BC_RUN` for each worker, and
// puts the run on the maker's `back` at once, by one compare-and-swap;
// the maker takes all its `back` holds where it has none kept.  So a
// spawner and a runner apart do not each go to the system's malloc and
// free, the one never finding one kept, the other keeping too many.  No
// room is the system's again before the program's end: what the
// contexts know points at rooms (see "Causality"), so a room is kept,
// and reused only once it is safe to (see "Reclaiming"))
#define BC_RUN 32
static HEFF_TLS BcBehaviour *bc_spare;
static HEFF_TLS int bc_nspare;
typedef struct BcRun { BcBehaviour *first, *last; int n; } BcRun;

// (the rooms of behaviours made by no worker -- main's -- let go of by
// workers: a pool of the program's, under a mutex, which main takes
// from a run at a time; a worker puts them there a run at a time too,
// `bc_strays`, as it does a worker's)
static pthread_mutex_t bc_rooms_mu = PTHREAD_MUTEX_INITIALIZER;
static BcBehaviour *bc_rooms;
static HEFF_TLS BcRun bc_strays;

static void bc_strays_back(void) {
  if (!bc_strays.n) return;
  pthread_mutex_lock(&bc_rooms_mu);
  bc_strays.last->after = bc_rooms;
  bc_rooms = bc_strays.first;
  pthread_mutex_unlock(&bc_rooms_mu);
  bc_strays.n = 0;
}

static BcBehaviour *bc_behaviour_room(BcPool *pl) {
  BcBehaviour *b = bc_spare;
  BcOwn *home = bc_own;
  if (home && home->pool != pl) home = NULL;
  if (!b && home && atomic_load_explicit(&home->back, memory_order_relaxed)) {
    b = atomic_exchange_explicit(&home->back, NULL, memory_order_acquire);
    atomic_store_explicit(&home->nback, 0, memory_order_relaxed);
    bc_nspare = 0;
  }
  if (!b && !bc_own && bc_rooms) {
    pthread_mutex_lock(&bc_rooms_mu);
    b = bc_rooms;
    BcBehaviour *last = b;
    for (int i = 1; last && last->after && i < BC_RUN; i++) last = last->after;
    if (last) {
      bc_rooms = last->after;
      last->after = NULL;
    }
    pthread_mutex_unlock(&bc_rooms_mu);
    bc_nspare = 0;
  }
  if (b) {
    bc_spare = b->after;
    if (bc_nspare > 0) bc_nspare--;
  } else {
    b = aligned_alloc(BC_LINE, sizeof(BcBehaviour));
    if (!b) heff_fail("out of memory");
  }
  b->home = home;
  return b;
}

// (a run given back to its worker, `home`: or, where it has enough not
// taken, to the system)
static void bc_run_back(BcOwn *home, BcRun *r) {
  if (!r->n) return;
  BcBehaviour *old = atomic_load_explicit(&home->back, memory_order_relaxed);
  do r->last->after = old;
  while (!atomic_compare_exchange_weak_explicit(&home->back, &old, r->first, memory_order_release,
                                                memory_order_relaxed));
  atomic_fetch_add_explicit(&home->nback, r->n, memory_order_relaxed);
  r->n = 0;
}

// (all this worker's runs given back: before it waits, and as it ends)
static void bc_runs_back(BcPool *pl) {
  bc_strays_back();
  BcOwn *me = bc_own;
  if (!me || me->pool != pl) return;
  for (int k = 0; k < pl->n; k++) bc_run_back(&pl->own[k], &me->runs[k]);
}

// (the room of a behaviour, done and safe to reuse (see "Reclaiming"),
// kept for the next: this thread's, or given back to the worker that
// made it)
static void bc_behaviour_free(BcBehaviour *b) {
  if (b->req != b->few) free(b->req);
  b->req = NULL;
  BcOwn *home = b->home, *me = bc_own;
  if (home && home != me) {
    BcPool *pl = b->pool;
    if (me && me->pool == pl) {
      BcRun *r = &me->runs[home - pl->own];
      b->after = r->first;
      r->first = b;
      if (!r->n++) r->last = b;
      if (r->n >= BC_RUN) bc_run_back(home, r);
    } else {
      BcRun one = {b, b, 1};
      bc_run_back(home, &one);
    }
    return;
  }
  if (!home && me) {
    // (made by no worker, let go of by one: the program's pool)
    b->after = bc_strays.first;
    bc_strays.first = b;
    if (!bc_strays.n++) bc_strays.last = b;
    if (bc_strays.n >= BC_RUN) bc_strays_back();
    return;
  }
  b->after = bc_spare;
  bc_spare = b;
  bc_nspare++;
}

// (... and what it holds let go of, where it still holds it: the
// runtime's count of it given back, the room freed by the last to know
// it)
static void bc_behaviour_drop(BcBehaviour *b) {
  if (b->cs) heff_drop(b->cs);
  if (b->body) heff_drop(b->body);
  if (b->cond) heff_drop(b->cond);
  bc_capture_free(b);
  bc_retire(b);
}

static int bc_by_address(const void *x, const void *y) {
  uintptr_t a = (uintptr_t)((const BcRequest *)x)->t, b = (uintptr_t)((const BcRequest *)y)->t;
  return a < b ? -1 : a > b;
}

// (the behaviour queued at its cowns, which it is now first at or
// behind the last at each: counted one more of its pool's beforehand,
// and every behaviour it was to come after done)
static void bc_enqueue(BcBehaviour *b) {
  int n = b->n;
  for (int i = 0; i < n; i++) {
    atomic_init(&b->req[i].next, NULL);
    atomic_init(&b->req[i].queued, n == 1);
  }
  atomic_init(&b->waits, n + 1);
  for (int i = 0; i < n; i++) {
    BcRequest *r = &b->req[i];
    BcRequest *before = atomic_exchange_explicit(&r->t->last, r, memory_order_acq_rel);
    if (!before) {
      atomic_fetch_sub_explicit(&b->waits, 1, memory_order_acq_rel);
      continue;
    }
    bc_movable(b);
    while (!atomic_load_explicit(&before->queued, memory_order_acquire)) bc_relax();
    atomic_store_explicit(&before->next, b, memory_order_release);
  }
  for (int i = 0; i < n; i++) atomic_store_explicit(&b->req[i].queued, 1, memory_order_release);
  bc_through(b);
}

static void bc_failed(BcPool *pl, const char *why);

// (where in `cs` the cown `t` is, from `*i`: the first two times, the
// first cown counted 1)
static void bc_where(V cs, BcCown *t, int *i, int at[2]) {
  if (bc_is_cown(cs)) {
    ++*i;
    if (BC_COWN(cs) == t && !at[1]) at[at[0] != 0] = *i;
    return;
  }
  if (BC_NONE(cs)) return;
  bc_where(heff_field(cs, 0), t, i, at);
  bc_where(heff_field(cs, 1), t, i, at);
}

static const char *bc_th(int n) {
  if (n % 100 / 10 == 1) return "th";
  return n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
}

// (a behaviour asked for on the cowns `cs`, `t` among them twice: a
// failure, said as a body's is, where in `cs` it is)
static void bc_twice(BcPool *pl, V cs, BcCown *t) {
  int i = 0, at[2] = {0, 0};
  bc_where(cs, t, &i, at);
  char why[160];
  snprintf(why, sizeof why,
           "boc: the same cown twice in one `with`, its %d%s and %d%s cowns: the behaviour not spawned",
           at[0], bc_th(at[0]), at[1], bc_th(at[1]));
  bc_failed(pl, why);
}

static BcCtx *bc_ctx_here(void);

// (`body` spawned on the cowns `cs`, in the pool, by the context of
// here, `cond` what it waits for, `ws` the cowns it writes back, `rs`
// those its condition reads; all consumed.  The same cown twice is
// found here, as the cowns are put in order: a failure, the behaviour
// never queued, the spawner going on)
static void bc_behave(BcPool *pl, V cs, V body, V cond, int64_t ws, int64_t rs) {
  int n = bc_count(cs);
  if (n > 62 && (ws >= 0 || rs > 0)) heff_fail("boc: a behaviour on more than 62 cowns, some not written");
  BcBehaviour *b = bc_behaviour_room(pl);
  b->req = n <= 2 ? b->few : heff_alloc((size_t)n * sizeof(BcRequest));
  int i = 0;
  bc_each(cs, b->req, &i);
  if (n == 2) {
    i = 1;
    if (b->req[0].t == b->req[1].t) goto twice;
    if ((uintptr_t)b->req[0].t > (uintptr_t)b->req[1].t) {
      BcCown *t = b->req[0].t;
      b->req[0].t = b->req[1].t;
      b->req[1].t = t;
    }
  } else if (n > 2) {
    qsort(b->req, (size_t)n, sizeof(BcRequest), bc_by_address);
    for (i = 1; i < n; i++)
      if (b->req[i].t == b->req[i - 1].t) goto twice;
  }
  b->n = n;
  b->pool = pl;
  b->cs = cs;
  b->body = body;
  b->cond = cond;
  b->ws = ws;
  b->rs = rs;
  b->act = NULL;
  b->moved = 0;
  b->known = NULL;
  b->nknown = 0;
  // (its number before its list of dependents, which says it is live:
  // a look that sees the list empty sees the number)
  b->serial = bc_numbered();
  atomic_init(&b->pending, 1);
  atomic_store_explicit(&b->deps, NULL, memory_order_release);
  bc_pool_more(pl);
  bc_spawned(b, bc_ctx_here(), NULL);
  return;
twice:
  bc_twice(pl, cs, b->req[i].t);
  bc_behaviour_free(b);
  heff_drop(cs);
  heff_drop(body);
  heff_drop(cond);
}

static void bc_behaved_all(BcBehaviour *b) {
  for (int i = 0; i < b->n; i++) {
    BcRequest *r = &b->req[i];
    BcBehaviour *next = atomic_load_explicit(&r->next, memory_order_acquire);
    if (!next) {
      BcRequest *mine = r;
      if (atomic_compare_exchange_strong_explicit(&r->t->last, &mine, NULL, memory_order_acq_rel, memory_order_acquire))
        continue;
      while (!(next = atomic_load_explicit(&r->next, memory_order_acquire))) bc_relax();
    }
    bc_through(next);
  }
}

// Waiting (`once`) ---------------------------------------------------------

// (the behaviour, its condition not holding, parked on the cowns `rs`
// says, the `i`th of the tuple `cs` from `*i`: it holds them, so their
// lists are its)
static void bc_park_on(V cs, BcWait *w, int64_t rs, int *i) {
  if (bc_is_cown(cs)) {
    if (bc_bit(rs, (*i)++)) {
      BcCown *c = BC_COWN(cs);
      BcPark *p = heff_alloc(sizeof(BcPark));
      p->w = w;
      p->next = c->waiting;
      c->waiting = p;
      atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);
    }
    return;
  }
  if (BC_NONE(cs)) return;
  bc_park_on(heff_field(cs, 0), w, rs, i);
  bc_park_on(heff_field(cs, 1), w, rs, i);
}

// (the behaviour, its condition not holding, as it is to be queued
// again: one of its own, what it runs and holds handed to it, so that
// the one running lets go of its cowns as it is, whenever it is queued)
static BcBehaviour *bc_again(BcBehaviour *b) {
  bc_movable(b);
  BcBehaviour *nb = bc_behaviour_room(b->pool);
  nb->req = b->n <= 2 ? nb->few : heff_alloc((size_t)b->n * sizeof(BcRequest));
  for (int i = 0; i < b->n; i++) nb->req[i].t = b->req[i].t;
  nb->n = b->n;
  nb->pool = b->pool;
  nb->cs = b->cs;
  nb->body = b->body;
  nb->cond = b->cond;
  nb->ws = b->ws;
  nb->rs = b->rs;
  nb->act = NULL;
  nb->moved = 1;
  nb->known = NULL;
  nb->nknown = 0;
  nb->serial = bc_numbered();
  atomic_init(&nb->pending, 1);
  atomic_store_explicit(&nb->deps, NULL, memory_order_release);
  b->cs = b->body = b->cond = 0;
  return nb;
}

static void bc_park(BcBehaviour *b) {
  BcPool *pl = b->pool;
  BcWait *w = heff_alloc(sizeof(BcWait));
  atomic_init(&w->state, 1);
  atomic_init(&w->refs, 1);
  w->b = b;
  int i = 0;
  bc_park_on(b->cs, w, b->rs, &i);
  pthread_mutex_lock(&pl->mu);
  w->prev = NULL;
  w->next = pl->parked;
  if (pl->parked) pl->parked->prev = w;
  pl->parked = w;
  pthread_mutex_unlock(&pl->mu);
}

// (the pool's list without `w`, under its mutex)
static void bc_unparked(BcPool *pl, BcWait *w) {
  if (w->prev) w->prev->next = w->next;
  else pl->parked = w->next;
  if (w->next) w->next->prev = w->prev;
}

// (the behaviour parked spawned again, where it still waits, by the
// body `by`, which wrote a cown it waits on, as the body's last spawn:
// after `by`, and after all `by` knew of on its cowns.  Counted its
// pool's again before it says it no longer waits, so that `drain` does
// not end meanwhile)
static void bc_wake(BcWait *w, BcBehaviour *by) {
  int one = 1;
  if (!atomic_compare_exchange_strong_explicit(&w->state, &one, 2, memory_order_acq_rel, memory_order_acquire))
    return;
  BcBehaviour *b = w->b;
  BcPool *pl = b->pool;
  bc_pool_more(pl);
  pthread_mutex_lock(&pl->mu);
  bc_unparked(pl, w);
  pthread_mutex_unlock(&pl->mu);
  atomic_store_explicit(&w->state, 0, memory_order_release);
  bc_wait_less(w);
  bc_body_fill();
  bc_spawned(b, &bc_body, by);
}

// (those waiting on the cowns `by` wrote woken, the `i`th of the tuple
// from `*i`: in the order they came to wait)
static void bc_woken(V cs, BcBehaviour *by, int64_t ws, int *i) {
  if (bc_is_cown(cs)) {
    BcCown *c = BC_COWN(cs);
    if (bc_bit(ws, (*i)++) && c->waiting) {
      BcPark *p = c->waiting, *order = NULL;
      c->waiting = NULL;
      while (p) {
        BcPark *next = p->next;
        p->next = order;
        order = p;
        p = next;
      }
      while (order) {
        BcPark *next = order->next;
        bc_wake(order->w, by);
        bc_wait_less(order->w);
        free(order);
        order = next;
      }
    }
    return;
  }
  if (BC_NONE(cs)) return;
  bc_woken(heff_field(cs, 0), by, ws, i);
  bc_woken(heff_field(cs, 1), by, ws, i);
}

// What a cown holds, owned ----------------------------------------------------

// (the log's entry for the cown, where it has one)
static HeffOwnRoot *bc_own_entry(HeffOwnLog *g, BcCown *c) {
  for (int64_t j = 0; j < g->nr; j++)
    if (g->roots[j].cell == c) return &g->roots[j];
  return NULL;
}

// (the log's entry for the cown, made where there is none: the
// behaviour holds the cown, so the entry holds nothing of it)
static HeffOwnRoot *bc_own_root(HeffOwnLog *g, BcCown *c) {
  HeffOwnRoot *had = bc_own_entry(g, c);
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
  e->cell = c;
  e->held = 0;
  e->v = c->v;
  e->put = 0;
  e->written = 0;
  return e;
}

// (what the cown holds, one more count of it: held by the log once
// too, where it is the cown's.  A number of a word, or a constructor
// without fields, is no object: nothing of it to keep)
static V bc_get_owned(HeffOwnLog *g, BcCown *c) {
  V now = c->v;
  if ((!IS_OBJ(now) || !now) && !bc_own_entry(g, c)) return now;
  V v = bc_own_root(g, c)->v;
  if (!IS_OBJ(v) || !v) return v;
  uint32_t rc = heff_rc(OBJ(v));
  if (heff_owned(rc)) OBJ(v)->rc = heff_own_touch(OBJ(v), rc) + 1;
  else heff_dup(v);
  return v;
}

// (... it is to hold `v` once the body is done; `v` consumed)
static void bc_set_owned(HeffOwnLog *g, BcCown *c, V v) {
  // (no object written over another: written now, nothing kept)
  if ((!IS_OBJ(v) || !v) && !bc_own_entry(g, c)) {
    V now = c->v;
    if (!IS_OBJ(now) || !now) {
      c->v = v;
      return;
    }
  }
  HeffOwnRoot *e = bc_own_root(g, c);
  if (e->written) heff_drop(e->put);
  e->put = v;
  e->written = 1;
}

// (what the body wrote of `r` put back as it was in `s` -- `r`, or a
// copy of it -- the last first, each entry done with)
static void bc_own_unwrite(HeffOwnLog *g, Arr *r, Arr *s) {
  unsigned w = heff_arr_width[HEFF_ARR_KIND(r)];
  for (int64_t j = g->ns; j-- > 0;) {
    HeffOwnSet *e = &g->sets[j];
    if (e->r != r) continue;
    if (HEFF_ARR_KIND(r) == HEFF_ARR_V) heff_drop(s->a[e->i]);
    memcpy((char *)s->a + e->i * w, &e->old, w);
    e->r = NULL;
  }
}

// (the cown holding the array as it was before the body wrote it: in
// place, where only what holds it there and the log do; else a copy,
// shared, put in the slot -- or the cown -- that held it, what else
// holds it keeping it as the body made it)
static void bc_own_restore(HeffOwnLog *g, HeffOwnArr *e) {
  Arr *r = e->r;
  if (HEFF_COUNT(heff_rc(&r->h)) == 2) {
    bc_own_unwrite(g, r, r);
    return;
  }
  Arr *s = heff_arr_alloc(r->n, HEFF_ARR_KIND(r));
  heff_arr_move(s, 0, r, 0, r->n);
  bc_own_unwrite(g, r, s);
  V c = heff_share((V)s);
  if (e->at) *e->at = c;
  else ((BcCown *)g->roots[e->root].cell)->v = c;
  heff_drop((V)r);
}

// (a behaviour's body done, or failed, and its log finished, with no
// log of its own: what it was to write let go of where it failed; each
// array it wrote in place, of what a cown not written holds, put back;
// each cown written; and what it counted up shared where more than one
// holds it)
static void bc_own_done(HeffOwnLog *g, int failed) {
  if (!g->nr && !g->nt) return;
  if (failed)
    for (int64_t j = 0; j < g->nr; j++)
      if (g->roots[j].written) {
        heff_drop(g->roots[j].put);
        g->roots[j].written = 0;
      }
  for (int64_t j = 0; j < g->na; j++)
    if (!g->roots[g->arrs[j].root].written) bc_own_restore(g, &g->arrs[j]);
  for (int64_t j = 0; j < g->ns; j++) {
    HeffOwnSet *e = &g->sets[j];
    if (e->r && HEFF_ARR_KIND(e->r) == HEFF_ARR_V) heff_own_at(&e->r->a[e->i]);
  }
  for (int64_t j = 0; j < g->nr; j++) {
    HeffOwnRoot *e = &g->roots[j];
    if (!e->written) continue;
    BcCown *c = e->cell;
    V old = c->v;
    c->v = e->put;
    heff_drop(old);
    heff_own_at(&c->v);
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
  free(g->touched);
  if (g->roots != g->first) free(g->roots);
  free(g->arrs);
  free(g->sets);
}

// (what the cowns `cs` hold, a tuple as they are: each read by the log)
static V bc_contents(HeffOwnLog *g, V cs) {
  if (bc_is_cown(cs)) return bc_get_owned(g, BC_COWN(cs));
  if (BC_NONE(cs)) return cs;
  V f[2] = { bc_contents(g, heff_field(cs, 0)), bc_contents(g, heff_field(cs, 1)) };
  return heff_tag_n(TAG_MkPair, 2, f);
}

// (what the body came to, `v`, taken apart as the cowns are, the `i`th
// written back where its bit of `ws` says, from `*i`; `v` consumed.  A
// tuple's field may be flat -- an `Int64`'s bits, where the body built
// it knowing the type -- so it is read boxed, `heff_tget_v`)
static void bc_written(HeffOwnLog *g, V cs, V v, int64_t ws, int *i) {
  if (bc_is_cown(cs)) {
    if (bc_bit(ws, (*i)++)) bc_set_owned(g, BC_COWN(cs), v);
    else heff_drop(v);
    return;
  }
  if (BC_NONE(cs)) {
    heff_drop(v);
    return;
  }
  V l = heff_tget_v(v, 0), r = heff_tget_v(v, 1);
  heff_drop(v);
  bc_written(g, heff_field(cs, 0), l, ws, i);
  bc_written(g, heff_field(cs, 1), r, ws, i);
}

// Running ---------------------------------------------------------------------

// (what the cowns of the behaviour this thread runs hold, for its body
// to take (`Taken`), and what they are to hold after it, as its body
// gives it (`Given`))
static HEFF_TLS V bc_taken, bc_given;

static V bc_dropped(V self, V *a) {
  (void)self;
  heff_drop(a[0]);
  return NTAG(TAG_MkUnit);
}

static Clo bc_dropped_clo = {{0x80000000u, K_CLO, 0}, bc_dropped, 1, 0};

// (how many behaviours and acts abandoned, or were not spawned: the
// program ends by 1 where any did)
static atomic_int bc_failures;

// (a behaviour or an act abandoned, or a behaviour not spawned, why:
// said on stderr, as the program's own would be, the others going on)
static void bc_failed(BcPool *pl, const char *why) {
  (void)pl;
  atomic_fetch_add_explicit(&bc_failures, 1, memory_order_relaxed);
  fflush(stdout);
  if (strcmp(why, "heff:abandon") == 0) fputs("heff:abandon\n", stderr);
  else fprintf(stderr, "heff: %s\n", why);
}

// Acts awaited ----------------------------------------------------------------

// An act awaited (`Awaits`) is a computation of the program's, `Turn ->
// m Unit`, run outside every behaviour: the act, then what comes once
// it has its value -- the behaviour after it spawned -- its own rest.
// Each worker of a pool has a group of the runtime's reactor
// (`bc_group`), opened as it starts its first act.  An act awaited by a
// behaviour a worker runs is given that worker (its `bc_inbox`), and
// started there once the behaviour is done, in its group: run until it
// is done, or until an operation of it would wait (a read, a sleep),
// which is then pending in the worker's reactor, the rest of the act
// kept with it.  So the act's rest -- the `when` of the behaviour after
// it -- runs on the worker that started it, and that behaviour is the
// one the worker runs next, in its slot: no other thread between an
// act and its behaviour.  An act awaited outside every worker (by a
// `main`) is a behaviour on no cown, whose worker
// takes the act as its own.
//
// A worker with nothing to run waits in its reactor where acts of its
// are pending, said to be (`polling`) so that a behaviour put with
// those ready wakes it there (`heff_reactor_wake_group`), else on the
// pool's condition; and it looks at its reactor, not waiting, after
// every 64 behaviours (`BC_POLL_EVERY`), so that acts done meanwhile
// are not passed over.  The pool counts an act given a worker and not
// yet started, and a worker with acts pending (`bc_holding`): the
// program ends once no behaviour is queued or runs and no act is
// pending.  An act that abandons is as a body that does: that is said
// (the group's function for failures), and what was to come after it
// does not.  An act that fails -- panics, or fails in the runtime --
// ends the program, as a body that fails does.
//
// An act awaited with a custodian current is in that custodian's
// custody, as std.reactor's `giving` has it: started in a group of its
// own within the worker's, which a thing the custodian has among its
// things gives up as it is shut down -- what of the act is pending let
// go of, undone, and the behaviour after it never spawned.  One that
// has come to its value is not given up.  Two acts raced (`Races`) are
// each in a group of their own, given the race as their turn: the
// first to come to its value asks it (`Won`), and is told so, and the
// other's group is closed, its wait undone; the other, coming to its
// value too, is told it is not the first.  Both are on one worker, so
// the race is that worker's alone.

#define BC_POLL_EVERY 64

// (an act awaited: its computation, of the program's, given the
// custodian current at its await, as a body is; or, `race`, two of
// them; that custodian, its foreign value, or 0; and its turn)
struct BcAct {
  BcAct *next;
  V arm[2];
  int race;
  V custody;
  V turn;
};

// (an act's turn: what the context awaiting it knew of the behaviours
// before it -- the context of its rest, which spawns the behaviour
// after it (`After`), so that one comes after them -- and, of a race,
// whether one of its acts has come to its value first, and their
// groups (0: the worker's).  Each turn is among its worker's acts
// begun and not ended, so that a behaviour an act's own computation
// spawns -- which act is not known then -- comes after what any of
// them knew; whether it is there, and whether the program has let go
// of it: the last of the two frees it)
typedef struct BcTurn BcTurn;
struct BcTurn {
  BcCtx ctx;
  int race, won;
  int64_t g[2];
  BcTurn *prev, *next;
  atomic_int st;
};

#define BC_TURN_LISTED 1
#define BC_TURN_DROPPED 2

static HEFF_TLS BcTurn *bc_turns;
static HEFF_TLS int bc_nturns, bc_turns_swept;

static void bc_turn_free(BcTurn *t) {
  bc_ctx_free(&t->ctx);
  free(t);
}

static void bc_turn_let_go(void *p) {
  BcTurn *t = p;
  int old = atomic_fetch_or_explicit(&t->st, BC_TURN_DROPPED, memory_order_acq_rel);
  if (!(old & BC_TURN_LISTED)) bc_turn_free(t);
}

// (the turn among this worker's acts begun)
static void bc_turn_listed(BcTurn *t) {
  atomic_fetch_or_explicit(&t->st, BC_TURN_LISTED, memory_order_acq_rel);
  t->prev = NULL;
  t->next = bc_turns;
  if (bc_turns) bc_turns->prev = t;
  bc_turns = t;
  bc_nturns++;
}

// (... no longer, where it is: the act ended, or given up)
static void bc_turn_unlisted(BcTurn *t) {
  int old = atomic_fetch_and_explicit(&t->st, ~BC_TURN_LISTED, memory_order_acq_rel);
  if (!(old & BC_TURN_LISTED)) return;
  if (t->prev) t->prev->next = t->next;
  else bc_turns = t->next;
  if (t->next) t->next->prev = t->prev;
  bc_nturns--;
  if (old & BC_TURN_DROPPED) bc_turn_free(t);
}

// (the acts given up, their groups closed, taken out of this worker's
// begun: once there are twice as many as after the last sweep)
static void bc_turns_sweep(void) {
  if (bc_nturns <= BC_KNOWN || bc_nturns <= 2 * bc_turns_swept) return;
  for (BcTurn *t = bc_turns, *next; t; t = next) {
    next = t->next;
    if (t->g[0] && !heff_group_of(t->g[0])) bc_turn_unlisted(t);
  }
  bc_turns_swept = bc_nturns;
}

// (the context of a spawn here: a body's, an act's rest's, main's --
// or, on a worker outside every body and rest, an act's own
// computation's, which act not known: what every act begun on this
// worker knows, and what was spawned so before, laid flat together)
static BcCtx *bc_ctx_here(void) {
  if (bc_now == &bc_body) bc_body_fill();
  if (bc_now) return bc_now;
  if (!bc_running) return &bc_outside;
  bc_turns_sweep();
  bc_ctx_ready(&bc_outside);
  bc_enter();
  for (BcTurn *t = bc_turns; t; t = t->next) bc_ctx_merge(&bc_outside, t->ctx.e, t->ctx.n);
  bc_exit();
  return &bc_outside;
}

// (`c`, the rest of the act whose turn is `tn`, run now as the act's:
// what it spawns comes after what the turn knows.  Run to its end
// here: a rest that waits (none of boc's does) runs on as the act's
// computation would)
static void bc_after(V tn, V c) {
  BcTurn *t = heff_foreign_ptr(tn);
  BcCtx *was = bc_now;
  bc_now = &t->ctx;
  V r = heff_tr(heff_app1(c, heff_clo(heff_kept, 1, 0, NULL)));
  bc_now = was;
  if (r != HEFF_PENDING) heff_drop(r);
  heff_drop(tn);
}

// (this worker's acts awaited, to be started, the first first; its
// group, 0 before its first act; and whether it counts as one of its
// pool's for acts pending in its reactor)
static HEFF_TLS BcAct *bc_inbox, *bc_inbox_last;
static HEFF_TLS int64_t bc_group;
static HEFF_TLS int bc_holding;

// (the act given this worker, counted its pool's until it is started.
// Given it by an act's rest, which runs as the worker looks at its
// reactor, the look comes back, not waiting on, so that the act starts
// now -- not once an act of the worker's is done)
static void bc_inboxed(BcPool *pl, BcAct *a) {
  bc_pool_more(pl);
  a->next = NULL;
  if (bc_inbox_last) bc_inbox_last->next = a;
  else bc_inbox = a;
  bc_inbox_last = a;
#ifndef _WIN32
  if (bc_polling) heff_woken = 1;
#endif
}

// (an act awaited where no worker of its pool is: a behaviour on no
// cown, whose worker takes it)
static void bc_act_behaviour(BcPool *pl, BcAct *a) {
  a->arm[0] = heff_move(a->arm[0]);
  if (a->race) a->arm[1] = heff_move(a->arm[1]);
  if (a->custody) a->custody = heff_move(a->custody);
  a->turn = heff_move(a->turn);
  BcBehaviour *b = bc_behaviour_room(pl);
  b->req = b->few;
  b->n = 0;
  b->pool = pl;
  b->cs = NTAG(TAG_MkUnit);
  b->body = 0;
  b->cond = 0;
  b->ws = b->rs = 0;
  b->act = a;
  b->moved = 1;
  b->known = NULL;
  b->nknown = 0;
  b->serial = bc_numbered();
  atomic_init(&b->pending, 0);
  atomic_store_explicit(&b->deps, NULL, memory_order_release);
  bc_pool_more(pl);
  bc_enqueue(b);
}

// (an act awaited, `arm1` 0, or two raced: in the pool of here, given
// this worker where it is one of that pool's, else a behaviour; its
// turn knowing what the context of here knows; the arms consumed)
static void bc_await(BcPool *pl, V arm0, V arm1) {
  BcAct *a = heff_alloc(sizeof(BcAct));
  a->arm[0] = arm0;
  a->arm[1] = arm1;
  a->race = arm1 != 0;
  V env = heff_custody_env;
  a->custody = env && heff_tag_of(env) == TAG_Just ? heff_dup(heff_field(env, 0)) : 0;
  BcTurn *t = heff_alloc(sizeof(BcTurn));
  BcCtx *h = bc_ctx_here();
  memset(&t->ctx, 0, sizeof t->ctx);
  bc_ctx_copy(&t->ctx, h);
  bc_ctx_open(&t->ctx);
  t->race = a->race;
  t->won = 0;
  t->g[0] = t->g[1] = 0;
  t->prev = t->next = NULL;
  atomic_init(&t->st, 0);
  a->turn = heff_foreign(t, bc_turn_let_go);
  if (bc_running == pl) bc_inboxed(pl, a);
  else bc_act_behaviour(pl, a);
}

// (an act that abandoned: `failed` given why; what the group finishes
// with, nothing)
static V bc_act_failed(V self, V *a) {
  size_t len;
  char *why = heff_bytes(a[0], &len);
  heff_drop(a[0]);
  heff_drop(self);
  bc_failed(bc_running, why);
  free(why);
  return NTAG(TAG_MkUnit);
}

// (an act begun: its computation given its turn, and its end)
static V bc_act_begun(V self, V *a) {
  V *e = heff_env(self);
  V act = heff_dup(e[0]), args[2] = { heff_dup(e[1]), heff_dup(e[2]) };
  heff_drop(self);
  heff_drop(a[0]);
  return heff_tail(act, 2, args);
}

// (the end of an act: its group closed where it has one of its own,
// given up no more, before what gives it up, in the closure's room, is
// let go of; and its turn no longer among those begun)
static V bc_act_ended(V self, V *a) {
  V *e = heff_env(self);
  int64_t g = INT_OF(e[0]);
  if (g) heff_reactor_close(g);
  bc_turn_unlisted(heff_foreign_ptr(e[2]));
  heff_drop(self);
  return a[0];
}

// (the thing `thing` in the custody of the custodian `cv`: ended at once
// where it is shut down.  Where no program's world has custodians,
// there is none)
static void bc_owned_by(V cv, V thing) {
#ifdef HEFF_CUSTODY_LOCK
  HeffCustody *c = heff_foreign_ptr(cv);
  char *ended = NULL;
  HEFF_CUSTODY_LOCK();
  V was = heff_custody_owned(c, cv, (Fgn *)thing, &ended);
  HEFF_CUSTODY_UNLOCK();
  if (ended) {
    heff_custody_failed(c, ended);
    free(ended);
  }
  if (was) heff_drop(was);
#else
  (void)cv;
  (void)thing;
#endif
}

// (whether this worker counts as one of its pool's: so while acts of it
// are pending)
static void bc_hold_update(BcPool *pl) {
  int now = bc_group && heff_reactor_pending(bc_group) > 0;
  if (now == bc_holding) return;
  bc_holding = now;
  if (now) bc_pool_more(pl);
  else bc_pool_less(pl);
}

// (the act started, in this worker's group, or one of its own within
// it: run until it is done or pending.  Of a race, both, each in one of
// its own, the second not begun where the first came to its value at
// once.  `a` let go of)
static void bc_start(BcAct *a) {
  if (!bc_group) bc_group = heff_reactor_open(heff_clo(bc_act_failed, 1, 0, NULL));
  int arms = a->race ? 2 : 1;
  V race = a->turn;
  BcTurn *t = heff_foreign_ptr(race);
  bc_turns_sweep();
  bc_turn_listed(t);
  int64_t gs[2];
  V ends[2];
  for (int i = 0; i < arms; i++) {
    int64_t g = bc_group, own = 0;
    V gv = NTAG(TAG_MkUnit);
    if (a->race || a->custody) {
      g = own = heff_reactor_open_within(bc_group);
      if (a->custody) {
        gv = heff_reactor_giving(g, NTAG(TAG_MkUnit));
        bc_owned_by(a->custody, gv);
      }
    }
    V e[3] = { heff_int(own), gv, heff_dup(race) };
    ends[i] = heff_clo(bc_act_ended, 1, 3, e);
    gs[i] = g;
    t->g[i] = g;
  }
  for (int i = 0; i < arms; i++) {
    if (t->won) {
      heff_drop(a->arm[i]);
      heff_drop(ends[i]);
      continue;
    }
    V e[3] = { a->arm[i], heff_dup(race), ends[i] };
    V begin = heff_clo(bc_act_begun, 1, 3, e);
    V r;
    if (!heff_group_step(gs[i], begin, NTAG(TAG_MkUnit), &r) && r != HEFF_PENDING) heff_drop(r);
  }
  heff_drop(race);
  if (a->custody) heff_drop(a->custody);
  free(a);
}

// (the acts given this worker started, in the order given, each then no
// longer counted as given)
static void bc_start_all(BcPool *pl) {
  while (bc_inbox) {
    BcAct *a = bc_inbox;
    bc_inbox = a->next;
    if (!bc_inbox) bc_inbox_last = NULL;
    bc_start(a);
    bc_hold_update(pl);
    bc_pool_less(pl);
  }
}

// (this worker's reactor looked at, waiting where `wait` until an act of
// it is done or a behaviour is ready: the rest of each act done run)
static void bc_poll(BcPool *pl, int wait) {
  bc_polling = 1;
  heff_drop(heff_reactor_finished(bc_group, wait));
  bc_polling = 0;
  bc_hold_update(pl);
}

// (the race `tn`'s act asking, its value come to, whether it is the
// first: where it is, the other's group closed, what of it is pending
// let go of)
static int bc_won(V tn) {
  BcTurn *t = heff_foreign_ptr(tn);
  if (t->won) return 0;
  t->won = 1;
  heff_reactor_close(t->g[heff_group == t->g[0] ? 1 : 0]);
  return 1;
}

// (the behaviour run, here: its condition checked of what its cowns
// hold, and where it holds, or there is none, its body run, given that,
// and what it gives back written back; then let go of -- or, its
// condition not holding, parked.  One that is an act awaited where no
// worker was: the act given this worker)
// (the body's context begun: what the behaviour's spawn knew and what
// the last on each of its cowns knew, laid flat as its base -- its
// capture let go of)
// (the body running, and whether what it knows has been laid out:
// only once it spawns, or something is spawned as its own -- most
// bodies spawn nothing, and what they knew is handed on as it came)
static HEFF_TLS BcBehaviour *bc_body_of;
static HEFF_TLS int bc_body_full;

// (the body's context begun: nothing laid out yet)
static void bc_body_begin(BcBehaviour *b) {
  bc_ctx_ready(&bc_body);
  bc_ctx_clear(&bc_body);
  bc_body_of = b;
  bc_body_full = 0;
}

// (what the body knows laid out, where not yet: what its spawn knew,
// and what the last holder of each of its cowns knew -- laid flat, and
// as it came)
static void bc_body_fill(void) {
  if (bc_body_full) return;
  bc_body_full = 1;
  BcBehaviour *b = bc_body_of;
  bc_enter();
  bc_ctx_merge(&bc_body, b->known, b->nknown);
  for (int i = 0; i < b->n; i++) {
    BcCown *c = b->req[i].t;
    bc_ctx_merge(&bc_body, c->co.e, c->co.n);
    bc_ctx_merge(&bc_body, c->pend, c->npend);
  }
  bc_exit();
  bc_ctx_open(&bc_body);
}

// (what came to the cown as it was handed over, not yet laid flat,
// laid into what it knows)
static void bc_pend_flat(BcCown *c) {
  if (!c->npend) return;
  bc_enter();
  bc_ctx_merge(&c->co, c->pend, c->npend);
  bc_exit();
  c->npend = 0;
}

// (... and ended: what the body knows left at each of its cowns, for
// the next to hold it: laid out, where it was; else what its spawn
// knew, as it came, laid flat with the rest once there is no room.  A
// body on several cowns is laid out first: what came to it at one of
// them, the next holder of another is to know -- the leap-frog of the
// paper's Figure 7, where a behaviour on two cowns carries what the
// one before it on the first knew to the one after it on the second)
static void bc_body_end(BcBehaviour *b) {
  if (b->n > 1) bc_body_fill();
  for (int i = 0; i < b->n; i++) {
    BcCown *c = b->req[i].t;
    if (bc_body_full) {
      bc_ctx_copy(&c->co, &bc_body);
      c->npend = 0;
      continue;
    }
    int n = b->nknown;
    if (c->npend + n > BC_PEND) bc_pend_flat(c);
    if (n > BC_PEND) {
      bc_enter();
      bc_ctx_merge(&c->co, b->known, n);
      bc_exit();
    } else if (n) {
      memcpy(c->pend + c->npend, b->known, (size_t)n * sizeof(BcKnown));
      c->npend += n;
    }
  }
  bc_capture_free(b);
  bc_ctx_clear(&bc_body);
  bc_body_of = NULL;
}

static void bc_run_behaviour(BcBehaviour *b) {
  BcPool *pl = b->pool;
  if (b->act) {
    bc_inboxed(pl, b->act);
    b->act = NULL;
    bc_behaviour_free(b);
    bc_pool_less(pl);
    return;
  }
  BcCtx *was = bc_now;
  bc_body_begin(b);
  bc_now = &bc_body;
  // (the rooms of those waiting for this one fetched meanwhile: the
  // next on its cowns, which this thread runs after it, were spawned
  // long since where many are queued, and are cold)
  {
    int k = 0;
    for (BcDep *d = atomic_load_explicit(&b->deps, memory_order_acquire); d && d != BC_DONE && k < 4; d = d->next, k++) {
      const char *at = (const char *)d->b;
      __builtin_prefetch(at, 1, 3);
      __builtin_prefetch(at + 64, 1, 3);
      __builtin_prefetch(at + 128, 0, 3);
    }
  }
  bc_fuel = 0;
  bc_in_body = 1;
  // (the log's lists empty, its room for the first cowns' entries not
  // cleared: none is read before it is written)
  HeffOwnLog log, *outer = heff_own_log;
  memset(&log, 0, __builtin_offsetof(HeffOwnLog, first));
  heff_own_log = &log;
  long dep = heff_dep;
  heff_dep = 0;
  V contents = bc_contents(&log, b->cs);
  const char *why = NULL;
  int holds = 1;
  V out;
  if (b->cond != NTAG(TAG_Nothing)) {
    V p = heff_dup(heff_field(b->cond, 0));
    why = heff_try(p, heff_dup(contents), &out);
    if (!why) holds = out == NTAG(TAG_True);
  }
  if (!why && holds) {
    V taken = bc_taken, given = bc_given;
    bc_taken = contents;
    bc_given = 0;
    // (the body consumed by its run, where the behaviour runs it once)
    V body = b->body;
    if (b->cond == NTAG(TAG_Nothing)) b->body = 0;
    else body = heff_dup(body);
    why = bc_try2(body, NTAG(TAG_MkUnit), (V)&bc_dropped_clo, &out);
    V result = bc_given;
    if (bc_taken) heff_drop(bc_taken);
    bc_taken = taken;
    bc_given = given;
    if (!why) {
      heff_drop(out);
      if (result) {
        int i = 0;
        bc_written(&log, b->cs, result, b->ws, &i);
      } else why = "boc: a body ended without a value";
    } else if (result) heff_drop(result);
  } else heff_drop(contents);
  heff_own_log = NULL;
  bc_own_done(&log, why != NULL || !holds);
  heff_own_log = outer;
  heff_dep = dep;
  if (why) bc_failed(pl, why);
  else if (!holds) bc_park(bc_again(b));
  else if (b->ws) {
    int i = 0;
    bc_woken(b->cs, b, b->ws, &i);
  }
  bc_in_body = 0;
  bc_body_end(b);
  bc_now = was;
  bc_behaved_all(b);
  bc_done(b);
  bc_behaviour_drop(b);
  bc_pool_less(pl);
}

typedef struct { BcPool *pl; char *dir; int me; } BcWorkerStart;

// (the first of those ready, the mutex taken only where one may be)
static BcBehaviour *bc_shared_take(BcPool *pl) {
  if (!__atomic_load_n(&pl->front, __ATOMIC_RELAXED)) return NULL;
  pthread_mutex_lock(&pl->mu);
  BcBehaviour *b = pl->front;
  if (b) {
    __atomic_store_n(&pl->front, b->after, __ATOMIC_RELAXED);
    if (!pl->front) pl->back = NULL;
    pl->ready--;
  }
  pthread_mutex_unlock(&pl->mu);
  return b;
}

// (a worker: its slot run, then its own queue -- those ready first
// every `BC_BATCH`, so that main's are not passed over -- then those
// ready, then another's queue; with none, a while looking before it
// waits.  A wake asked of it, or that it was started for, is answered
// once it looks)
static void *bc_worker(void *u) {
  BcWorkerStart *s = u;
  BcPool *pl = s->pl;
  heff_dir_take(s->dir);
  bc_me = s->me;
  free(s);
  bc_own = &pl->own[bc_me];
  bc_own->pool = pl;
  bc_own->runs = calloc((size_t)pl->n, sizeof(BcRun));
  if (!bc_own->runs) heff_fail("out of memory");
  bc_running = pl;
  bc_current = pl;
  atomic_fetch_sub_explicit(&pl->looking, 1, memory_order_acq_rel);
  atomic_store_explicit(&pl->waking, 0, memory_order_seq_cst);
  int row = 0, since = 0;
  unsigned picks = 0;
  for (;;) {
    BcBehaviour *kept = bc_slot;
    if (kept) {
      bc_slot = NULL;
      if (row >= BC_BATCH && (__atomic_load_n(&pl->front, __ATOMIC_RELAXED) || !bc_own_empty(bc_own))) {
        if (!bc_own_put(kept)) bc_queued(kept);
      } else {
        row = row >= BC_BATCH ? 1 : row + 1;
        bc_run_behaviour(kept);
        since++;
        bc_aged(pl);
        continue;
      }
    }
    row = 0;
    // (the acts the behaviours run here awaited started, outside them)
    if (bc_inbox) {
      bc_start_all(pl);
      continue;
    }
    // (acts pending, looked at now and then: their rest run)
    if (bc_holding && since >= BC_POLL_EVERY) {
      since = 0;
      bc_poll(pl, 0);
      continue;
    }
    BcBehaviour *b = NULL;
    int first = (++picks % BC_BATCH) == 0;
    if (first) b = bc_shared_take(pl);
    if (!b) b = bc_own_take();
    if (!b && !first) b = bc_shared_take(pl);
    if (!b) b = bc_steal(pl);
    if (b) {
      bc_run_behaviour(b);
      since++;
      bc_aged(pl);
      continue;
    }
    // (a while looked at before sleeping: a behaviour readied soon after
    // by another thread taken without a wake; not where acts are pending,
    // which are waited for in the reactor instead)
    if (!bc_holding) {
      atomic_fetch_add_explicit(&pl->looking, 1, memory_order_acq_rel);
      atomic_store_explicit(&pl->waking, 0, memory_order_seq_cst);
      int found = 0;
      for (int i = 0; i < BC_SPINS && !__atomic_load_n(&pl->stopping, __ATOMIC_RELAXED); i++) {
        if ((i & 63) == 0 && bc_any_ready(pl)) {
          found = 1;
          break;
        }
        bc_relax();
      }
      atomic_fetch_sub_explicit(&pl->looking, 1, memory_order_seq_cst);
      if (found || bc_any_ready(pl)) continue;
    }
    bc_settle(pl);
    pthread_mutex_lock(&pl->mu);
    if (!pl->front && !pl->stopping && bc_holding) {
      // (waiting in the reactor, said to be, so that a behaviour ready
      // wakes it there; where too many are, not waiting)
      int on = pl->npolling < BC_POLLERS;
      if (on) pl->polling[pl->npolling++] = bc_group;
      pthread_mutex_unlock(&pl->mu);
      since = 0;
      bc_poll(pl, on);
      if (on) {
        pthread_mutex_lock(&pl->mu);
        for (int i = 0; i < pl->npolling; i++)
          if (pl->polling[i] == bc_group) {
            pl->polling[i] = pl->polling[--pl->npolling];
            break;
          }
        pthread_mutex_unlock(&pl->mu);
      } else sched_yield();
      atomic_store_explicit(&pl->waking, 0, memory_order_seq_cst);
      continue;
    }
    while (!pl->front && !pl->stopping && !pl->wanted) {
      pl->sleeping++;
      pthread_cond_wait(&pl->work, &pl->mu);
      pl->sleeping--;
    }
    if (pl->wanted) {
      // (woken to take from the others' queues: looked at again)
      pl->wanted--;
      pthread_mutex_unlock(&pl->mu);
      atomic_store_explicit(&pl->waking, 0, memory_order_seq_cst);
      continue;
    }
    b = pl->front;
    if (!b) {
      pthread_mutex_unlock(&pl->mu);
      break;
    }
    __atomic_store_n(&pl->front, b->after, __ATOMIC_RELAXED);
    if (!pl->front) pl->back = NULL;
    pl->ready--;
    pthread_mutex_unlock(&pl->mu);
    bc_run_behaviour(b);
    since++;
    bc_aged(pl);
  }
  // (its group closed: nothing of it is pending, or the pool would not
  // have stopped)
  if (bc_group) {
    heff_reactor_close(bc_group);
    bc_group = 0;
  }
  // (the behaviours' room this thread kept given back, and those others
  // let go of, to their makers)
  bc_runs_back(pl);
  free(bc_own->runs);
  bc_own->runs = NULL;
  bc_own->pool = NULL;
  // (the rooms it kept, and those let go of not yet safe to reuse,
  // handed to the program's end to free, once every thread is done)
  pthread_mutex_lock(&bc_epochs_mu);
  while (bc_spare) {
    BcBehaviour *next = bc_spare->after;
    bc_spare->after = bc_orphans;
    bc_orphans = bc_spare;
    bc_spare = next;
  }
  if (bc_mine)
    for (int k = 0; k < 3; k++)
      while (bc_mine->retired[k]) {
        BcBehaviour *b = bc_mine->retired[k];
        bc_mine->retired[k] = b->after;
        b->after = bc_orphans;
        bc_orphans = b;
      }
  pthread_mutex_unlock(&bc_epochs_mu);
  bc_nspare = 0;
  heff_thread_end();
  return NULL;
}

// (one more thread of the pool's started, looking until it runs: `mu`
// held, so that the end, under it, joins every one started.  Threads
// are started as behaviours are put with those ready and none is free
// to take them, up to as many as the machine has cores; a thread
// started waits, once it has nothing to run, as long as the pool is)
static void bc_thread_more(BcPool *pl) {
  BcWorkerStart *s = heff_alloc(sizeof(BcWorkerStart));
  s->pl = pl;
  s->dir = heff_dir_copy();
  s->me = pl->started;
  atomic_fetch_add_explicit(&pl->looking, 1, memory_order_acq_rel);
  if (heff_thread_start(&pl->threads[pl->started], bc_worker, s) != 0) heff_fail("boc: cannot start a thread");
  pl->started++;
}

// (a pool of up to as many threads as the machine has cores, one
// started)
static BcPool *bc_pool(void) {
  int64_t n = bc_cores();
  BcPool *pl = heff_alloc(sizeof(BcPool));
  pthread_mutex_init(&pl->mu, NULL);
  pthread_cond_init(&pl->work, NULL);
  pthread_cond_init(&pl->idle, NULL);
  pl->front = pl->back = NULL;
  atomic_init(&pl->live, 0);
  pl->sleeping = 0;
  pl->stopping = 0;
  pl->n = (int)n;
  pl->parked = NULL;
  pl->npolling = 0;
  pl->threads = heff_alloc((size_t)n * sizeof(pthread_t));
  pl->started = 0;
  pl->ready = 0;
  atomic_init(&pl->looking, 0);
  pl->own = aligned_alloc(BC_LINE, (size_t)n * sizeof(BcOwn));
  if (!pl->own) heff_fail("out of memory");
  memset(pl->own, 0, (size_t)n * sizeof(BcOwn));
  pl->wanted = 0;
  atomic_init(&pl->waking, 0);
  pthread_mutex_lock(&pl->mu);
  bc_thread_more(pl);
  pthread_mutex_unlock(&pl->mu);
  return pl;
}

// (no behaviour left but those waiting: those dropped, the threads
// stopped and the pool freed.  One being queued again meanwhile is
// waited for, and then what it runs)
static void bc_drain(BcPool *pl) {
  BcWait *dropped = NULL;
  bc_settle(pl);
  pthread_mutex_lock(&pl->mu);
  for (;;) {
    while (atomic_load_explicit(&pl->live, memory_order_acquire) > 0) pthread_cond_wait(&pl->idle, &pl->mu);
    // (what is due on this thread -- the program's -- run first: a
    // custodian's `stop` given by `main`, the custodian shut down by a
    // failure since; what it spawns, or wakes, waited for in turn)
#ifdef HEFF_CUSTODY_LOCK
    if (heff_due_here && __atomic_load_n(&heff_due_here->n, __ATOMIC_ACQUIRE)) {
      pthread_mutex_unlock(&pl->mu);
      heff_due_run();
      bc_settle(pl);
      pthread_mutex_lock(&pl->mu);
      continue;
    }
#endif
    int waking = 0;
    for (BcWait *w = pl->parked, *next; w; w = next) {
      next = w->next;
      int one = 1;
      if (atomic_compare_exchange_strong_explicit(&w->state, &one, 0, memory_order_acq_rel, memory_order_acquire)) {
        bc_unparked(pl, w);
        w->next = dropped;
        dropped = w;
      } else waking = 1;
    }
    if (!waking && atomic_load_explicit(&pl->live, memory_order_acquire) == 0) break;
    pthread_mutex_unlock(&pl->mu);
    sched_yield();
    pthread_mutex_lock(&pl->mu);
  }
  __atomic_store_n(&pl->stopping, 1, __ATOMIC_RELAXED);
  pthread_cond_broadcast(&pl->work);
  int np = pl->npolling;
  int64_t woken[BC_POLLERS];
  memcpy(woken, pl->polling, (size_t)np * sizeof(int64_t));
  pl->npolling = 0;
  pthread_mutex_unlock(&pl->mu);
  for (int i = 0; i < np; i++) heff_reactor_wake_group(woken[i]);
  for (int i = 0; i < pl->started; i++) pthread_join(pl->threads[i], NULL);
  while (dropped) {
    BcWait *next = dropped->next;
    bc_behaviour_drop(dropped->b);
    bc_wait_less(dropped);
    dropped = next;
  }
  pthread_mutex_destroy(&pl->mu);
  pthread_cond_destroy(&pl->work);
  pthread_cond_destroy(&pl->idle);
  free(pl->threads);
  // (the behaviours given back to workers ended since, those the
  // threads handed over as they ended, and this thread's own: every
  // thread is done, so none looks at them any more)
  for (int k = 0; k < pl->n; k++)
    for (BcBehaviour *b = pl->own[k].back, *next; b; b = next) {
      next = b->after;
      if (b->req && b->req != b->few) free(b->req);
      free(b);
    }
  pthread_mutex_lock(&bc_epochs_mu);
  for (BcBehaviour *b = bc_orphans, *next; b; b = next) {
    next = b->after;
    if (b->req && b->req != b->few) free(b->req);
    free(b);
  }
  bc_orphans = NULL;
  pthread_mutex_unlock(&bc_epochs_mu);
  pthread_mutex_lock(&bc_rooms_mu);
  for (BcBehaviour *b = bc_rooms, *next; b; b = next) {
    next = b->after;
    if (b->req && b->req != b->few) free(b->req);
    free(b);
  }
  bc_rooms = NULL;
  pthread_mutex_unlock(&bc_rooms_mu);
  while (bc_spare) {
    BcBehaviour *next = bc_spare->after;
    free(bc_spare);
    bc_spare = next;
  }
  bc_nspare = 0;
  if (bc_mine)
    for (int k = 0; k < 3; k++)
      while (bc_mine->retired[k]) {
        BcBehaviour *b = bc_mine->retired[k];
        bc_mine->retired[k] = b->after;
        if (b->req && b->req != b->few) free(b->req);
        free(b);
      }
  free(pl->own);
  free(pl);
}

// (the program's pool: opened by its first operation of boc's, outside
// every worker, and its behaviours and acts waited for once the
// program's computation has returned (`heff_returned`), then those
// waiting dropped and the threads stopped)
static BcPool *bc_program;
static pthread_mutex_t bc_program_mu = PTHREAD_MUTEX_INITIALIZER;

static int bc_program_end(void) {
  pthread_mutex_lock(&bc_program_mu);
  BcPool *pl = bc_program;
  bc_program = NULL;
  pthread_mutex_unlock(&bc_program_mu);
  if (pl) bc_drain(pl);
  bc_current = NULL;
  return atomic_load_explicit(&bc_failures, memory_order_relaxed) > 0;
}

static BcPool *bc_program_pool(void) {
  pthread_mutex_lock(&bc_program_mu);
  if (!bc_program) {
    bc_program = bc_pool();
    heff_returned = bc_program_end;
  }
  BcPool *pl = bc_program;
  pthread_mutex_unlock(&bc_program_mu);
  return pl;
}

// (how many threads the pool here has: this thread's, else the
// program's; where none is open yet, how many it will have, none
// opened)
static int64_t bc_workers(void) {
  if (bc_current) return bc_current->n;
  pthread_mutex_lock(&bc_program_mu);
  int64_t n = bc_program ? bc_program->n : bc_cores();
  pthread_mutex_unlock(&bc_program_mu);
  return n;
}

// (the pool a `when` here spawns in; a body's operation counted, and at
// its fuel's end the behaviour kept let out)
static BcPool *bc_here(void) {
  BcPool *pl = bc_current;
  if (!pl) pl = bc_current = bc_program_pool();
  if (bc_slot && bc_in_body && ++bc_fuel >= BC_FUEL) {
    BcBehaviour *kept = bc_slot;
    bc_slot = NULL;
    bc_fuel = 0;
    bc_queued(kept);
  }
  return pl;
}

// (a behaviour spawned, a cown made, and what a body takes and gives,
// where the program does it at once: its operation not built, nor what
// comes after it, heff.compile.direct's -- the same as `When`'s,
// `NewCown`'s, `Taken`'s and `Given`'s)
V heff_bocWhen(V cs, V body, V cond, V ws, V rs) {
  BcPool *pl = bc_here();
  bc_behave(pl, cs, heff_carried_fn(body), cond, INT_OF(ws), INT_OF(rs));
  return NTAG(TAG_MkUnit);
}

V heff_bocNew(V v) { return heff_foreign(bc_cown(v), bc_cown_let_go); }

V heff_bocTaken(V tn) {
  (void)tn;
  V v = bc_taken;
  if (!v) heff_fail("boc: what the cowns hold taken where no body runs, or twice");
  bc_taken = 0;
  return v;
}

V heff_bocGiven(V tn, V v) {
  (void)tn;
  if (bc_given) heff_drop(bc_given);
  bc_given = v;
  return NTAG(TAG_MkUnit);
}

static V heff_provider_boc(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_When: {
    V bh = bc_open(heff_dup(heff_field(p, 0)));
    V body = heff_dup(heff_field(p, 1));
    x = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V cs = heff_dup(heff_field(bh, 0)), cond = heff_dup(heff_field(bh, 1));
    V ws = heff_field(bh, 2), rs = heff_field(bh, 3);
    heff_drop(bh);
    heff_bocWhen(cs, body, cond, ws, rs);
    break;
  }
  case TAG_Taken: {
    V tk = bc_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V g = heff_dup(heff_field(tk, 1));
    heff_drop(tk);
    x = heff_answer(g, heff_bocTaken(NTAG(TAG_MkUnit)));
    break;
  }
  case TAG_Given: {
    V gv = bc_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V v = heff_tget_v(gv, 1);
    x = heff_dup(heff_field(gv, 2));
    heff_drop(gv);
    heff_bocGiven(NTAG(TAG_MkUnit), v);
    break;
  }
  case TAG_NewCown: {
    V mk = bc_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    V v = heff_tget_v(mk, 0), g = heff_dup(heff_field(mk, 1));
    heff_drop(mk);
    x = heff_answer(g, heff_bocNew(v));
    break;
  }
  case TAG_Awaits: {
    V act = heff_carried_fn(heff_dup(heff_field(p, 0)));
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    bc_await(bc_here(), act, 0);
    break;
  }
  case TAG_Races: {
    V l = heff_carried_fn(heff_dup(heff_field(p, 0)));
    V r = heff_carried_fn(heff_dup(heff_field(p, 1)));
    x = heff_dup(heff_field(p, 2));
    heff_drop(p);
    bc_await(bc_here(), l, r);
    break;
  }
  case TAG_Won: {
    int first = bc_won(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(f, NTAG(first ? TAG_True : TAG_False));
    break;
  }
  case TAG_After: {
    V tn = heff_dup(heff_field(p, 0)), c = heff_carried(heff_dup(heff_field(p, 1)));
    x = heff_dup(heff_field(p, 2));
    heff_drop(p);
    bc_after(tn, c);
    break;
  }
  case TAG_Workers: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    x = heff_answer(f, MK_INT(bc_workers()));
    break;
  }
  default:
    heff_fail("not an operation of boc");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
