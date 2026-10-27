// Custodians, the runtime's part: bundled after the runtime only into a
// program whose world has the custodian's effect, so a program without
// it is as it was.
//
// A custodian is a struct of its own, `HeffCustody`, which its foreign
// value points to.  It keeps what it was given to run at its shutdown,
// the custodians under it -- held, so they live as long as it does --
// and the things in its custody, not held: a thing holds its custodian
// (`Fgn`'s `owner`), so a custodian lives as long as a thing in its
// custody does, and the thing leaves its custody as it is let go of.  A
// custodian under another knows it, not holding it: as that one is let
// go of, those under it go under the one it was under, and stop where
// it stopped.  All of them are under one mutex, never held while a
// computation of the program's runs, nor while a value is let go of; a
// struct a failure goes up through is pinned meanwhile (`pins`), and
// freed by the last of its foreign value and its pins.
//
// A failure of a custodian's -- a thing in its custody that did not
// end, there or later, or a release run for it that failed -- shuts it
// down: the things in its custody, and of those under it, ended at once,
// where the failure is heard; what it was given put on the queue of the
// domain that made it, to run there.  Then it goes to the custodian it
// is under; one made to contain its failures says it, and it goes no
// further; past the root's it is the program's: said, the program ended.
//
// Each domain's queue of releases due: those of `with`s (resource's
// `held`) a throw left, put there by the custodian's provider as the
// program lets go of what kept them, from whichever thread that is, and
// what a failure shut down.  The domain runs them before its next
// operation (`heff_custodianDue`, which the compiler puts before each,
// where the world has the custodian's effect), each once, in the order
// kept, and what is left as the domain ends (its thread's
// `heff_thread_end`) -- not as the program ends, which the system
// cleans up after; one that fails is a failure of its custodian's.

typedef struct HeffDueQ HeffDueQ;

typedef struct HeffCustody {
  V *stops, *kids;
  Fgn **things;
  int64_t n, cap, nk, capk, nt, capt;
  struct HeffCustody *parent;
  HeffDueQ *q;
  int64_t pins;
  int shut, contains, gone;
} HeffCustody;

// (what a shutdown takes out to run: a `stop`, and whose it was)
typedef struct HeffStop {
  V stop;
  HeffCustody *of;
} HeffStop;

#ifdef HEFF_THREADS
static pthread_mutex_t heff_custody_mu = PTHREAD_MUTEX_INITIALIZER;
#define HEFF_CUSTODY_LOCK() pthread_mutex_lock(&heff_custody_mu)
#define HEFF_CUSTODY_UNLOCK() pthread_mutex_unlock(&heff_custody_mu)
#else
#define HEFF_CUSTODY_LOCK() ((void)0)
#define HEFF_CUSTODY_UNLOCK() ((void)0)
#endif

typedef struct HeffDue {
  V release;
  HeffCustody *owner;
  struct HeffDue *next;
} HeffDue;

struct HeffDueQ {
  HeffDue *head;
  int n;
#ifdef HEFF_THREADS
  pthread_mutex_t mu;
#endif
};

// (each domain's queue, made as it first keeps a release; never freed,
// as what a domain kept may outlive it)
HEFF_VAR(HEFF_TLS HeffDueQ *heff_due_here);

static void heff_due_drained(void);

HEFF_FN HeffDueQ *heff_due_queue(void) {
  if (!heff_due_here) {
    heff_thread_ending = heff_due_drained;
    HeffDueQ *q = heff_alloc(sizeof *q);
    memset(q, 0, sizeof *q);
#ifdef HEFF_THREADS
    pthread_mutex_init(&q->mu, NULL);
#endif
    heff_due_here = q;
  }
  return heff_due_here;
}

// (a release put on the queue, from any thread: where it fails, a
// failure of `owner`'s, pinned for it, or of the root's, 0)
HEFF_FN void heff_due_put(HeffDueQ *q, V release, HeffCustody *owner) {
  HeffDue *d = heff_alloc(sizeof *d);
  d->release = release;
  d->owner = owner;
#ifdef HEFF_THREADS
  pthread_mutex_lock(&q->mu);
#endif
  d->next = q->head;
  q->head = d;
  __atomic_add_fetch(&q->n, 1, __ATOMIC_RELEASE);
#ifdef HEFF_THREADS
  pthread_mutex_unlock(&q->mu);
#endif
}

// (an element put at the end of an array that grows, of `size` bytes)
static void heff_custody_put(void *at, int64_t *n, int64_t *cap, size_t size, const void *v) {
  char **a = at;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    char *grown = heff_alloc((size_t)*cap * size);
    if (*n) memcpy(grown, *a, (size_t)*n * size);
    free(*a);
    *a = grown;
  }
  memcpy(*a + (size_t)(*n)++ * size, v, size);
}

// (a pin let go of: the struct freed by the last of its pins and its
// foreign value; the lock held)
static void heff_custody_unpinned(HeffCustody *c) {
  if (--c->pins == 0 && c->gone) free(c);
}

static void heff_custody_unpin(HeffCustody *c) {
  HEFF_CUSTODY_LOCK();
  heff_custody_unpinned(c);
  HEFF_CUSTODY_UNLOCK();
}

// (a thing ended, where it is not already: why it failed, the first of
// them, at `*why`; the lock held)
static void heff_custody_end(Fgn *g, char **why) {
  if (g->ended || !g->end) return;
  g->ended = 1;
  const char *w = g->end(g->p);
  if (w && !*why) *why = heff_strdup(w);
}

// (`c` and those under it shut down: their things ended, why the first
// that failed did at `*why`, and what they were given taken out to run,
// each pinned to its custodian; the lock held)
static void heff_custody_taken(HeffCustody *c, HeffStop **out, int64_t *n, int64_t *cap, char **why) {
  c->shut = 1;
  for (int64_t i = 0; i < c->nt; i++) heff_custody_end(c->things[i], why);
  for (int64_t i = 0; i < c->n; i++) {
    HeffStop s = { c->stops[i], c };
    c->pins++;
    heff_custody_put(out, n, cap, sizeof s, &s);
  }
  c->n = 0;
  for (int64_t i = 0; i < c->nk; i++) heff_custody_taken(heff_foreign_ptr(c->kids[i]), out, n, cap, why);
}

// (a failure said where it stops: at a custodian that contains it, the
// program going on, or past the root, the program ended)
static void heff_custody_said(int contained, const char *why) {
  if (!contained) heff_root_failed(why);
  fflush(stdout);
  fprintf(stderr, "heff: %s (its custodian shut down)\n", why);
}

// (a failure of `c`'s, why said: `c` shut down, with those under it --
// what they were given run by the domains that made them, a failure of
// theirs there their custodian's -- and the failure passed up, to where
// it stops; a thing that fails to end meanwhile is of this failure.  `c`
// is held, or pinned, by the caller)
static void heff_custody_failed(HeffCustody *c, const char *why) {
  char *said = heff_strdup(why);
  HEFF_CUSTODY_LOCK();
  c->pins++;
  HEFF_CUSTODY_UNLOCK();
  for (;;) {
    HeffStop *stops = NULL;
    int64_t n = 0, cap = 0;
    char *more = NULL;
    HEFF_CUSTODY_LOCK();
    heff_custody_taken(c, &stops, &n, &cap, &more);
    int contains = c->contains;
    HeffCustody *up = c->parent;
    if (up && !contains) up->pins++;
    heff_custody_unpinned(c);
    HEFF_CUSTODY_UNLOCK();
    for (int64_t i = 0; i < n; i++) heff_due_put(stops[i].of->q, stops[i].stop, stops[i].of);
    free(stops);
    free(more);
    if (contains || !up) {
      heff_custody_said(contains, said);
      free(said);
      return;
    }
    c = up;
  }
}

static V heff_due_finished(V self, V *a) {
  heff_drop(a[0]);
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what is on this domain's queue run, each once, in the order kept; one
// that fails, its custodian's failure)
HEFF_SLOW void heff_due_run(void) {
  HeffDueQ *q = heff_due_here;
#ifdef HEFF_THREADS
  pthread_mutex_lock(&q->mu);
#endif
  HeffDue *d = q->head;
  q->head = NULL;
  __atomic_store_n(&q->n, 0, __ATOMIC_RELEASE);
#ifdef HEFF_THREADS
  pthread_mutex_unlock(&q->mu);
#endif
  HeffDue *in = NULL;
  while (d) { HeffDue *n = d->next; d->next = in; in = d; d = n; }
  while (in) {
    HeffDue *n = in->next;
    V out;
    const char *why = heff_try(in->release, NTAG(TAG_MkUnit), &out);
    if (!why) why = heff_try(out, heff_clo(heff_due_finished, 1, 0, NULL), &out);
    if (!why) heff_drop(out);
    HeffCustody *owner = in->owner;
    free(in);
    if (why) {
      char said[512];
      snprintf(said, sizeof said, "a release, run after its `with` was left: %s", why);
      if (owner) heff_custody_failed(owner, said);
      else heff_root_failed(said);
    }
    if (owner) heff_custody_unpin(owner);
    in = n;
  }
}

// (what is due on this domain run, before an operation)
static void heff_due_drained(void) {
  if (heff_due_here && __atomic_load_n(&heff_due_here->n, __ATOMIC_ACQUIRE)) heff_due_run();
}

static inline V heff_custodianDue(void) {
  if (__builtin_expect(heff_due_here && __atomic_load_n(&heff_due_here->n, __ATOMIC_ACQUIRE), 0))
    heff_due_run();
  return NTAG(TAG_MkUnit);
}

// (a thing out of the custody of the custodian `o`, the last of its
// things put where it was; the lock held)
static void heff_custody_out(HeffCustody *o, Fgn *g) {
  Fgn *last = o->things[--o->nt];
  o->things[g->at] = last;
  last->at = g->at;
}

// (a thing let go of, out of its custodian's custody)
static void heff_custody_disowned(Fgn *g) {
  HEFF_CUSTODY_LOCK();
  heff_custody_out(heff_foreign_ptr(g->owner), g);
  HEFF_CUSTODY_UNLOCK();
}

// (a thing that failed to end as it was let go of: its custodian's
// failure, the custodian held by the thing until it is done)
static void heff_custody_thing_failed(V owner, const char *why) {
  heff_custody_failed(heff_foreign_ptr(owner), why);
}

// (a thing taken into `c`'s custody, out of another's where it was in
// one: that one, to let go of, or 0; ended at once where `c` is shut
// down; the lock held)
static V heff_custody_owned(HeffCustody *c, V cv, Fgn *g, char **why) {
  V was = g->owner;
  if (was) heff_custody_out(heff_foreign_ptr(was), g);
  g->owner = heff_dup(cv);
  g->at = (int32_t)c->nt;
  heff_custody_put(&c->things, &c->nt, &c->capt, sizeof g, &g);
  if (c->shut) heff_custody_end(g, why);
  return was;
}
