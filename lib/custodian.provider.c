// Custodians, for the C backend: each a foreign value, the runtime's,
// let go of where the program drops its last reference to it -- what it
// was given dropped then, not run -- and its struct by the runtime's
// part for custodians, which keeps them all and carries their failures.
// A custodian keeps what it is given in the order given, those made
// under it, which it holds, and the things in its custody, which hold
// it.  What it was given runs with no lock held.

// (the runtime's part told what this provider does with a thing let go
// of, as the first custodian is made)
static void heff_custody_ready(void) {
  heff_foreign_disowned = heff_custody_disowned;
  heff_foreign_failed = heff_custody_thing_failed;
}

static void heff_custody_let_go(void *p) {
  HeffCustody *c = p;
  HEFF_CUSTODY_LOCK();
  // (those under it go under the one it was under, and stop where it
  // stopped)
  for (int64_t i = 0; i < c->nk; i++) {
    HeffCustody *k = heff_foreign_ptr(c->kids[i]);
    k->parent = c->parent;
    k->contains |= c->contains;
  }
  V *stops = c->stops, *kids = c->kids;
  int64_t n = c->n, nk = c->nk;
  free(c->things);
  // (a failure that pinned it meanwhile finds nothing left in it, and
  // goes on as from the root: it is under none now)
  c->stops = c->kids = NULL;
  c->things = NULL;
  c->n = c->nk = c->nt = 0;
  c->parent = NULL;
  c->gone = 1;
  if (c->pins == 0) free(c);
  HEFF_CUSTODY_UNLOCK();
  for (int64_t i = 0; i < n; i++) heff_drop(stops[i]);
  for (int64_t i = 0; i < nk; i++) heff_drop(kids[i]);
  free(stops);
  free(kids);
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

// (what an operation's `Some` holds, where `heff_custody_open` puts it)
static HEFF_TLS V heff_custody_held;

static V heff_custody_open_fn(V self, V *a) {
  heff_custody_held = a[0];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what `s` holds; `s` consumed)
static V heff_custody_open(V s) {
  heff_drop(heff_answer(s, heff_clo(heff_custody_open_fn, 1, 0, NULL)));
  return heff_custody_held;
}

// (the custodian a `Maybe Custodian` names: 0 for the root's)
static HeffCustody *heff_custody_of(V at) {
  return at && heff_tag_of(at) == TAG_Just ? heff_foreign_ptr(heff_field(at, 0)) : NULL;
}

// (the custodian current where the operation was asked: `here`, where
// it says one -- std.domain's `rooted` says it -- else as the runtime
// was told; its foreign value, or 0 for the root's)
static V heff_custody_current(V here) {
  V at = here && heff_tag_of(here) == TAG_Just ? here : heff_custody_env;
  return at && heff_tag_of(at) == TAG_Just ? heff_field(at, 0) : 0;
}

// (what follows a computation run with a custodian current: its answer
// given what comes after it, and that given the operation's
// continuation)
static V heff_custody_after(V self, V *a) {
  V *e = heff_env(self);
  V g = heff_dup(e[0]), k = heff_dup(e[1]);
  heff_drop(self);
  V x = heff_answer(g, a[0]);
  return heff_tail(k, 1, &x);
}

#define HEFF_CUSTODY 1

// Releases kept for `with`s (resource's `held`).  A ticket holds one,
// and the custodian whose custody the `with` is in; dropped by its
// `with`, the release goes with it.  A ticket the program lets go of
// otherwise -- its `with` left by a throw, or by a handler dropping the
// continuation -- puts its release on the queue of the domain that kept
// it (the runtime's part for custodians), never running it there; a
// failure of it there is its custodian's.

typedef struct HeffTicket {
  V release;
  V owner;
  HeffDueQ *q;
} HeffTicket;

static void heff_ticket_let_go(void *p) {
  HeffTicket *t = p;
  if (t->release) {
    HeffCustody *o = NULL;
    if (t->owner) {
      o = heff_foreign_ptr(t->owner);
      HEFF_CUSTODY_LOCK();
      o->pins++;
      HEFF_CUSTODY_UNLOCK();
    }
    heff_due_put(t->q, t->release, o);
  }
  if (t->owner) heff_drop(t->owner);
  free(t);
}

// (an operation, `here` the custodian current where it was asked, if
// its asker says so -- std.domain's `rooted`, in a world without the
// custodian's effect -- else 0, the one the runtime was told)
static V heff_custodian_at(V p, V k, V here) {
  V x;
  // (an operation, taken out of its `Asked` or `Given`)
  if (heff_tag_of(p) == TAG_Asked || heff_tag_of(p) == TAG_Given) {
    V q = heff_dup(heff_field(p, 0));
    heff_drop(p);
    p = q;
  }
  switch (heff_tag_of(p)) {
  case TAG_Made: {
    V at = heff_field(p, 0);
    int contains = heff_tag_of(heff_field(p, 1)) == TAG_True;
    V g = heff_dup(heff_field(p, 2));
    heff_custody_ready();
    HeffCustody *c = heff_alloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->contains = contains;
    c->q = heff_due_queue();
    V cv = heff_share(heff_foreign(c, heff_custody_let_go));
    V pv = heff_tag_of(at) == TAG_Just ? heff_field(at, 0) : heff_custody_current(here);
    HeffCustody *parent = pv ? heff_foreign_ptr(pv) : NULL;
    if (parent) {
      HEFF_CUSTODY_LOCK();
      c->shut = parent->shut;
      c->parent = parent;
      heff_custody_put(&parent->kids, &parent->nk, &parent->capk, sizeof(V), &(V){ heff_dup(cv) });
      HEFF_CUSTODY_UNLOCK();
    }
    heff_drop(p);
    x = heff_answer(g, cv);
    break;
  }
  case TAG_Managed: {
    HeffCustody *c = heff_foreign_ptr(heff_field(p, 0));
    V stop = heff_share(heff_carried_fn(heff_dup(heff_field(p, 1))));
    V g = heff_dup(heff_field(p, 2));
    int kept = 0;
    HEFF_CUSTODY_LOCK();
    if (!c->shut) {
      heff_custody_put(&c->stops, &c->n, &c->cap, sizeof(V), &stop);
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
    HeffStop *stops = NULL;
    int64_t n = 0, cap = 0;
    char *ended = NULL;
    HEFF_CUSTODY_LOCK();
    heff_custody_taken(c, &stops, &n, &cap, &ended);
    for (int64_t i = 0; i < n; i++) heff_custody_unpinned(stops[i].of);
    HEFF_CUSTODY_UNLOCK();
    // (the custodian kept alive, by `p`, until what it was given has run;
    // a thing of it that did not end, its failure)
    if (ended) {
      heff_custody_failed(c, ended);
      free(ended);
    }
    V *whys = heff_alloc((size_t)(n ? n : 1) * sizeof(V));
    int64_t failed = 0;
    for (int64_t i = 0; i < n; i++) {
      const char *why = heff_custody_run(stops[i].stop);
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
  case TAG_Own: {
    V cv = heff_custody_current(here);
    // (a thing of the root's: as it is)
    if (!cv) {
      x = heff_dup(heff_field(p, 1));
      heff_drop(p);
      break;
    }
    HeffCustody *c = heff_foreign_ptr(cv);
    V o = heff_custody_open(heff_dup(heff_field(p, 0)));
    V thing = heff_field(o, 0);
    V was = 0;
    char *ended = NULL;
    // (no thing the runtime can end: as it is)
    if (IS_OBJ(thing) && OBJ(thing)->kind == K_FGN) {
      HEFF_CUSTODY_LOCK();
      was = heff_custody_owned(c, cv, (Fgn *)thing, &ended);
      HEFF_CUSTODY_UNLOCK();
    }
    if (ended) {
      heff_custody_failed(c, ended);
      free(ended);
    }
    x = heff_dup(heff_field(p, 1));
    heff_drop(o);
    heff_drop(p);
    if (was) heff_drop(was);
    break;
  }
  case TAG_Failed: {
    V cv = heff_custody_current(here);
    size_t len;
    char *why = heff_bytes(heff_field(p, 0), &len);
    if (cv) heff_custody_failed(heff_foreign_ptr(cv), why);
    else heff_root_failed(why);
    free(why);
    x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    break;
  }
  case TAG_Current: {
    V g = heff_dup(heff_field(p, 0));
    V at = here && heff_tag_of(here) == TAG_Just ? heff_dup(here)
         : heff_custody_env ? heff_dup(heff_custody_env) : NTAG(TAG_Nothing);
    heff_drop(p);
    x = heff_answer(g, at);
    break;
  }
  // (a computation run with a custodian current: given `Just` it, and
  // what follows it after)
  case TAG_Under: {
    V kv = heff_dup(heff_field(p, 0));
    V o = heff_custody_open(heff_dup(heff_field(p, 1)));
    heff_drop(p);
    V body = heff_dup(heff_field(o, 0));
    V e[2] = { heff_dup(heff_field(o, 1)), k };
    heff_drop(o);
    V run = heff_custody_env ? heff_answer(body, heff_tag_n(TAG_Just, 1, &kv)) : (heff_drop(kv), body);
    V after = heff_clo(heff_custody_after, 1, 2, e);
    return heff_tail(run, 1, &after);
  }
  case TAG_Kept: {
    HeffTicket *t = heff_alloc(sizeof *t);
    V cv = heff_custody_current(here);
    t->owner = cv ? heff_dup(cv) : 0;
    t->release = heff_share(heff_carried_fn(heff_dup(heff_field(p, 0))));
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

static V heff_provider_custodian(V p, V k) { return heff_custodian_at(p, k, 0); }
