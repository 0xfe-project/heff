// Custodians, the runtime's part: bundled after the runtime only into a
// program whose world has the custodian's effect, so a program without
// it is as it was.
//
// Each domain's queue of releases due: those of `with`s (resource's
// `held`) a throw left, put there by the custodian's provider as the
// program lets go of what kept them, from whichever thread that is.
// The domain runs them before its next operation (`heff_custodianDue`,
// which the compiler puts before each, where the world has the
// custodian's effect), each once, in the order kept; one that fails is
// a failure of the runtime's, said, the program ended.

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

// (each domain's queue, made as it first keeps a release; never freed,
// as what a domain kept may outlive it)
HEFF_VAR(HEFF_TLS HeffDueQ *heff_due_here);

HEFF_FN HeffDueQ *heff_due_queue(void) {
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

// (a release put on the queue, from any thread)
HEFF_FN void heff_due_put(HeffDueQ *q, V release) {
  HeffDue *d = heff_alloc(sizeof *d);
  d->release = release;
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

static V heff_due_finished(V self, V *a) {
  heff_drop(a[0]);
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what is on this domain's queue run, each once, in the order kept)
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
    free(in);
    if (why) {
      fflush(stdout);
      fprintf(stderr, "heff: a release, run after its `with` was left: %s\n", why);
      exit(1);
    }
    in = n;
  }
}

// (what is due on this domain run, before an operation)
static inline V heff_custodianDue(void) {
  if (__builtin_expect(heff_due_here && __atomic_load_n(&heff_due_here->n, __ATOMIC_ACQUIRE), 0))
    heff_due_run();
  return NTAG(TAG_MkUnit);
}
