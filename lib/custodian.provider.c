// Custodians, for the C backend: each a foreign value, the runtime's,
// let go of where the program drops its last reference to it -- what it
// was given dropped then, not run.  A custodian keeps what it is given
// in the order given, and those made under it: a parent holds each of
// them, so they live as long as it does, unless the program holds one
// longer.  The custodians are held under one mutex, which is never held
// while what they were given runs.

typedef struct HeffCustody {
  V *stops, *kids;
  int64_t n, cap, nk, capk;
  int shut;
} HeffCustody;

#ifdef HEFF_THREADS
static pthread_mutex_t heff_custody_mu = PTHREAD_MUTEX_INITIALIZER;
#define HEFF_CUSTODY_LOCK() pthread_mutex_lock(&heff_custody_mu)
#define HEFF_CUSTODY_UNLOCK() pthread_mutex_unlock(&heff_custody_mu)
#else
#define HEFF_CUSTODY_LOCK() ((void)0)
#define HEFF_CUSTODY_UNLOCK() ((void)0)
#endif

// (a value put at the end of an array that grows)
static void heff_custody_put(V **at, int64_t *n, int64_t *cap, V v) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    V *grown = heff_alloc((size_t)*cap * sizeof(V));
    if (*n) memcpy(grown, *at, (size_t)*n * sizeof(V));
    free(*at);
    *at = grown;
  }
  (*at)[(*n)++] = v;
}

static void heff_custody_let_go(void *p) {
  HeffCustody *c = p;
  for (int64_t i = 0; i < c->n; i++) heff_drop(c->stops[i]);
  for (int64_t i = 0; i < c->nk; i++) heff_drop(c->kids[i]);
  free(c->stops);
  free(c->kids);
  free(c);
}

// (what `c` and those under it were given, taken out to run, each shut;
// the lock held)
static void heff_custody_taken(HeffCustody *c, V **out, int64_t *n, int64_t *cap) {
  c->shut = 1;
  for (int64_t i = 0; i < c->n; i++) heff_custody_put(out, n, cap, c->stops[i]);
  c->n = 0;
  for (int64_t i = 0; i < c->nk; i++)
    heff_custody_taken(heff_foreign_ptr(c->kids[i]), out, n, cap);
}

static V heff_custody_finished(V self, V *a) {
  heff_drop(a[0]);
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what was given run here, a computation of the program's: why it
// failed, or 0)
static const char *heff_custody_run(V stop) {
  V out;
  const char *why = heff_try(stop, NTAG(TAG_MkUnit), &out);
  if (!why) why = heff_try(out, heff_clo(heff_custody_finished, 1, 0, NULL), &out);
  if (!why) heff_drop(out);
  return why;
}

// Releases kept for `with`s (resource's `held`).  A ticket holds one;
// dropped by its `with`, the release goes with it.  A ticket the program
// lets go of otherwise -- its `with` left by a throw, or by a handler
// dropping the continuation -- puts its release on the queue of the
// domain that kept it, never running it there: the domain runs what is
// on its queue at its next operation (`heff_due_run`, which the
// program's `heff_perform` calls where its world has custodians).

typedef struct HeffDue {
  V release;
  struct HeffDue *next;
} HeffDue;

typedef struct HeffDueQ {
  HeffDue *head;
  int n;
#ifdef HEFF_THREADS
  pthread_mutex_t mu;
#endif
} HeffDueQ;

// (each domain's queue, made as it first keeps one; never freed, as a
// ticket of the domain's may outlive it)
static HEFF_TLS HeffDueQ *heff_due_here = NULL;

static HeffDueQ *heff_due_queue(void) {
  if (!heff_due_here) {
    HeffDueQ *q = heff_alloc(sizeof *q);
    memset(q, 0, sizeof *q);
#ifdef HEFF_THREADS
    pthread_mutex_init(&q->mu, NULL);
#endif
    heff_due_here = q;
  }
  return heff_due_here;
}

typedef struct HeffTicket {
  V release;
  HeffDueQ *q;
} HeffTicket;

static void heff_ticket_let_go(void *p) {
  HeffTicket *t = p;
  if (t->release) {
    HeffDue *d = heff_alloc(sizeof *d);
    d->release = t->release;
#ifdef HEFF_THREADS
    pthread_mutex_lock(&t->q->mu);
#endif
    d->next = t->q->head;
    t->q->head = d;
    __atomic_add_fetch(&t->q->n, 1, __ATOMIC_RELEASE);
#ifdef HEFF_THREADS
    pthread_mutex_unlock(&t->q->mu);
#endif
  }
  free(t);
}

// (what is on this domain's queue run, each once: one that fails a
// failure of the runtime's, said, the program ended)
static void heff_due_run(void) {
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
  // (in the order kept: the list is pushed at its front)
  HeffDue *in = NULL;
  while (d) { HeffDue *n = d->next; d->next = in; in = d; d = n; }
  while (in) {
    HeffDue *n = in->next;
    const char *why = heff_custody_run(in->release);
    free(in);
    if (why) {
      fflush(stdout);
      fprintf(stderr, "heff: a release, run after its `with` was left: %s\n", why);
      exit(1);
    }
    in = n;
  }
}

static V heff_provider_custodian(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_Made: {
    V at = heff_field(p, 0);
    V g = heff_dup(heff_field(p, 1));
    HeffCustody *c = heff_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    V cv = heff_foreign(c, heff_custody_let_go);
    if (heff_tag_of(at) == TAG_Just) {
      HeffCustody *parent = heff_foreign_ptr(heff_field(at, 0));
      cv = heff_share(cv);
      HEFF_CUSTODY_LOCK();
      c->shut = parent->shut;
      heff_custody_put(&parent->kids, &parent->nk, &parent->capk, heff_dup(cv));
      HEFF_CUSTODY_UNLOCK();
    }
    heff_drop(p);
    x = heff_answer(g, cv);
    break;
  }
  case TAG_Managed: {
    HeffCustody *c = heff_foreign_ptr(heff_field(p, 0));
    V stop = heff_share(heff_dup(heff_field(p, 1)));
    V g = heff_dup(heff_field(p, 2));
    int kept = 0;
    HEFF_CUSTODY_LOCK();
    if (!c->shut) {
      heff_custody_put(&c->stops, &c->n, &c->cap, stop);
      kept = 1;
    }
    HEFF_CUSTODY_UNLOCK();
    heff_drop(p);
    if (!kept) heff_drop(stop);
    x = heff_answer(g, NTAG(kept ? TAG_True : TAG_False));
    break;
  }
  case TAG_ShutDown: {
    HeffCustody *c = heff_foreign_ptr(heff_field(p, 0));
    V g = heff_dup(heff_field(p, 1));
    V *stops = NULL;
    int64_t n = 0, cap = 0;
    HEFF_CUSTODY_LOCK();
    heff_custody_taken(c, &stops, &n, &cap);
    HEFF_CUSTODY_UNLOCK();
    // (the custodian kept alive, by `p`, until what it was given has run)
    V *whys = heff_alloc((size_t)(n ? n : 1) * sizeof(V));
    int64_t failed = 0;
    for (int64_t i = 0; i < n; i++) {
      const char *why = heff_custody_run(stops[i]);
      if (why) whys[failed++] = heff_str_text(why);
    }
    free(stops);
    heff_drop(p);
    V l = NTAG(TAG_Nil);
    for (int64_t i = failed - 1; i >= 0; i--) {
      V f[2] = { whys[i], l };
      l = heff_tag(TAG_Cons, 2, f);
    }
    free(whys);
    x = heff_answer(g, l);
    break;
  }
  case TAG_Kept: {
    HeffTicket *t = heff_alloc(sizeof *t);
    t->release = heff_share(heff_dup(heff_field(p, 0)));
    t->q = heff_due_queue();
    V g = heff_dup(heff_field(p, 1));
    heff_drop(p);
    x = heff_answer(g, heff_foreign(t, heff_ticket_let_go));
    break;
  }
  case TAG_Dropped: {
    HeffTicket *t = heff_foreign_ptr(heff_field(p, 0));
    V r = t->release;
    t->release = 0;
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    if (r) heff_drop(r);
    break;
  }
  default:
    heff_fail("not an operation of custodian");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
