// Places' side of std.place, for the C backend: an effect provider that
// runs heff in places -- threads with heaps of their own, sharing
// nothing (#97) -- so it says so, and the runtime is built for threads:
#define HEFF_THREADS 1
// A place runs the computation it is given, crossed to it as a message
// (`heff_place_start`), and is joined for its value, crossed back
// (`heff_place_join`): copied, or moved where the sender held it alone
// and it is the system's; a failure in it is the joiner's at the join.
// A channel (`heff_chan_new`) holds the messages sent on it; a receive
// takes the first, waiting for one, the whole place with it -- or, in a
// group of std.reactor's, pending: a wait of the reactor's on the
// descriptor the channel keeps readable while it holds something, the
// other green threads of the place going on.  A join in a group is
// pending the same way, on the descriptor the place writes as it ends.
//
// A `Started`, a `Got` and a `Sent` are each a function given what to do
// with what they hold, their types erased: `pl_open` takes it, as
// std.thread's provider takes a work.

// (what a started, a got or a sent holds, where `pl_open` puts it)
static HEFF_TLS V pl_held[2];

static V pl_open_fn(V self, V *a) {
  int n = ((Clo *)self)->arity;
  for (int i = 0; i < n; i++) pl_held[i] = a[i];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (`w` given a function of `n` arguments that keeps them: what it holds
// at `pl_held`; `w` consumed)
static void pl_open(V w, int n) {
  heff_drop(heff_answer(w, heff_clo(pl_open_fn, n, 0, NULL)));
}

// (a channel of its own, held by the world: its number)
static int64_t pl_channel(void) {
  return heff_hold(heff_chan_new(), NULL);
}

// (a join pending in a group of the reactor's: the place's number)
typedef struct { HeffWait w; int64_t i; } PlJoin;

static int pl_join_step(HeffWait *w) {
  (void)w;
  return HEFF_STEP_DONE;
}

static V pl_join_answer(HeffWait *w) {
  PlJoin *j = (PlJoin *)w;
  int64_t i = j->i;
  free(j);
  return heff_place_join(i);
}

static void pl_join_cancel(HeffWait *w) { free(w); }

// (place `i`'s value given to `f`, then `k`: waited for here, or
// pending in the group being started in, until the place has ended)
static V pl_join(int64_t i, V f, V k) {
  int fd = heff_group ? heff_place_fd(i) : -1;
  if (fd >= 0) {
    PlJoin *j = heff_alloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->w.fd = fd; j->w.events = POLLIN;
    j->w.step = pl_join_step; j->w.answer = pl_join_answer; j->w.cancel = pl_join_cancel;
    j->i = i;
    return heff_outside(&j->w, f, k);
  }
  V x = heff_answer(f, heff_place_join(i));
  return heff_tail(k, 1, &x);
}

// (a receive pending in a group of the reactor's: its channel, and the
// message once it has one)
typedef struct { HeffWait w; HeffChan *ch; HeffMsg *got; } PlRecv;

static int pl_recv_step(HeffWait *w) {
  PlRecv *r = (PlRecv *)w;
  r->got = heff_chan_take(r->ch, 0);
  return r->got ? HEFF_STEP_DONE : HEFF_STEP_WAIT;
}

static V pl_recv_answer(HeffWait *w) {
  PlRecv *r = (PlRecv *)w;
  HeffMsg *got = r->got;
  free(r);
  return heff_msg_value(got);
}

static void pl_recv_cancel(HeffWait *w) { free(w); }

// (the next value on channel `ch` given to `f`, then `k`: waited for
// here, or pending in the group being started in)
static V pl_recv(HeffChan *ch, V f, V k) {
  if (heff_group) {
    int fd = heff_chan_fd(ch);
    if (fd >= 0) {
      PlRecv *r = heff_alloc(sizeof *r);
      memset(r, 0, sizeof *r);
      r->w.fd = fd; r->w.events = POLLIN;
      r->w.step = pl_recv_step; r->w.answer = pl_recv_answer; r->w.cancel = pl_recv_cancel;
      r->ch = ch; r->got = NULL;
      if (pl_recv_step(&r->w) != HEFF_STEP_DONE) return heff_outside(&r->w, f, k);
      V x = heff_answer(f, pl_recv_answer(&r->w));
      return heff_tail(k, 1, &x);
    }
  }
  V x = heff_answer(f, heff_chan_recv(ch));
  return heff_tail(k, 1, &x);
}

static V heff_described_places(V p, V k);

static V heff_provider_places(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Spawn: {
    V s = heff_dup(heff_field(p, 0));
    heff_drop(p);
    pl_open(s, 2);
    V c = pl_held[0], f = pl_held[1];
    V i = heff_int(heff_place_start(c));
    V x = heff_answer(f, heff_tag_n(TAG_MkPlace, 1, &i));
    return heff_tail(k, 1, &x);
  }
  case TAG_Join: {
    int64_t i = INT_OF(heff_field(p, 0));
    V j = heff_dup(heff_field(p, 1));
    heff_drop(p);
    pl_open(j, 1);
    return pl_join(i, pl_held[0], k);
  }
  case TAG_Send: {
    HeffChan *ch = heff_held(INT_OF(heff_field(p, 0)));
    V s = heff_dup(heff_field(p, 1));
    heff_drop(p);
    pl_open(s, 2);
    V v = pl_held[0], x = pl_held[1];
    heff_chan_send(ch, v);
    return heff_tail(k, 1, &x);
  }
  case TAG_Recv: {
    HeffChan *ch = heff_held(INT_OF(heff_field(p, 0)));
    V j = heff_dup(heff_field(p, 1));
    heff_drop(p);
    pl_open(j, 1);
    return pl_recv(ch, pl_held[0], k);
  }
  // (`NewChannel`, and whatever else std.place describes in C: the glue
  // the compiler made of the description)
  default:
    return heff_described_places(p, k);
  }
}
