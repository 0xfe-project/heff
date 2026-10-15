// Places' side of std.place, for the C backend: an effect provider that
// runs heff in places -- threads with heaps of their own, sharing
// nothing (#97) -- so it says so, and the runtime is built for threads:
#define HEFF_THREADS 1
// A place runs the computation it is given, crossed to it as a message
// (`heff_place_start`, the place a foreign value), and is joined for
// its value, crossed back (`heff_place_join`): copied, or moved where
// the sender held it alone and it is the system's; a failure in it is
// the joiner's at the join.  A channel (`heff_chan_new`) holds the
// messages sent on it, and is a foreign value the places that are given
// it share, each holding it once (`heff_chan_hold`) and letting it go
// (`heff_chan_drop`) as it drops its last reference; a receive takes
// the first message, waiting for one, the whole place with it -- or, in
// a group of std.reactor's, pending: a wait of the reactor's on the
// descriptor the channel keeps readable while it holds something, the
// other green threads of the place going on.
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

// (a channel of its own, as the program holds it: a foreign value that
// crosses to other places, each holding the channel once more and
// letting it go as it drops its last reference)
static V pl_channel(void) {
  return heff_foreign_shared(heff_chan_new(), heff_chan_drop, heff_chan_hold);
}

// (a receive pending in a group of the reactor's: its channel, and the
// message once it has one)
typedef struct { HeffWait w; HeffChan *ch; HeffMsg *got; V held; } PlRecv;

static int pl_recv_step(HeffWait *w) {
  PlRecv *r = (PlRecv *)w;
  r->got = heff_chan_take(r->ch, 0);
  return r->got ? HEFF_STEP_DONE : HEFF_STEP_WAIT;
}

static V pl_recv_answer(HeffWait *w) {
  PlRecv *r = (PlRecv *)w;
  HeffMsg *got = r->got;
  heff_drop(r->held);
  free(r);
  return heff_msg_value(got);
}

static void pl_recv_cancel(HeffWait *w) {
  PlRecv *r = (PlRecv *)w;
  heff_drop(r->held);
  free(r);
}

// (the next value on channel `ch` given to `f`, then `k`: waited for
// here, or pending in the group being started in; `p`, the operation,
// held meanwhile and dropped after)
static V pl_recv(HeffChan *ch, V p, V f, V k) {
  if (heff_group) {
    int fd = heff_chan_fd(ch);
    if (fd >= 0) {
      PlRecv *r = heff_alloc(sizeof *r);
      r->w.fd = fd; r->w.events = POLLIN; r->w.err = 0;
      r->w.step = pl_recv_step; r->w.answer = pl_recv_answer; r->w.cancel = pl_recv_cancel;
      r->ch = ch; r->got = NULL; r->held = p;
      if (pl_recv_step(&r->w) != HEFF_STEP_DONE) return heff_outside(&r->w, f, k);
      V x = heff_answer(f, pl_recv_answer(&r->w));
      return heff_tail(k, 1, &x);
    }
  }
  V got = heff_chan_recv(ch);
  heff_drop(p);
  V x = heff_answer(f, got);
  return heff_tail(k, 1, &x);
}

static V heff_provider_places(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Spawn: {
    V s = heff_dup(heff_field(p, 0));
    heff_drop(p);
    pl_open(s, 2);
    V c = pl_held[0], f = pl_held[1];
    V spot = heff_place_start(c);
    V x = heff_answer(f, heff_tag_n(TAG_MkPlace, 1, &spot));
    return heff_tail(k, 1, &x);
  }
  case TAG_Join: {
    V spot = heff_dup(heff_field(p, 0));
    V j = heff_dup(heff_field(p, 1));
    heff_drop(p);
    pl_open(j, 1);
    V f = pl_held[0];
    V r = heff_place_join(spot);
    heff_drop(spot);
    V x = heff_answer(f, r);
    return heff_tail(k, 1, &x);
  }
  case TAG_NewChannel: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, pl_channel());
    return heff_tail(k, 1, &x);
  }
  // (a send or a receive keeps `p`, and so the channel, while it is
  // under way)
  case TAG_Send: {
    HeffChan *ch = heff_foreign_ptr(heff_field(p, 0));
    V s = heff_dup(heff_field(p, 1));
    pl_open(s, 2);
    V v = pl_held[0], x = pl_held[1];
    heff_chan_send(ch, v);
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_Recv: {
    HeffChan *ch = heff_foreign_ptr(heff_field(p, 0));
    V j = heff_dup(heff_field(p, 1));
    pl_open(j, 1);
    V f = pl_held[0];
    return pl_recv(ch, p, f, k);
  }
  default:
    heff_fail("not an operation of places");
    return 0;
  }
}
