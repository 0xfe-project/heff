// std.reactor's side, for the C backend: an effect provider
// over the runtime's reactor, whose groups it
// opens, starts computations in, asks for what is finished and closes.
//
// A computation is started between a `Begin` and an `End`, in the
// world's own course, not handed to an operation: `Begin` runs what
// comes after it -- the computation, to its `End` -- there and then, in
// its group (`heff_group`), as `heff_reactor_start` runs a computation;
// the `End` of that run gives back its value and what comes after it,
// not running it, and `Begin` goes on from there, with `Just` the
// value.  Where the computation waits instead, `HEFF_PENDING` comes
// back, and `Begin` goes on from itself again, told `False`; once the
// wait is done, the rest is run by the reactor, and its `End`, not of a
// run of a `Begin`'s, gives back the value alone, one of the group's
// finished.  Where the group was opened with what a failure is
// finished as, each of these runs is under `heff_try`, and one that
// fails is finished so, its start told it waits.
//
// An `End` and a `Finished` are each a function given what to do with
// what they hold, their types erased: `rx_open` takes it -- an end's
// value and its function `k`, a finished's function `k` -- so that the
// provider has each apart, as std.domain's provider takes a `Some`.

// (what an end or a finished holds, where `rx_open` puts it)
static HEFF_TLS V rx_held[2];

// (the group whose `Begin` is running what comes after it, 0 none; and
// what its `End` gave back: the value, its function and what comes
// after it)
static HEFF_TLS int64_t rx_trying;
static HEFF_TLS V rx_ended[3];
static Obj rx_ended_obj = { IMMORTAL, K_STR, 0 };
#define RX_ENDED ((V)&rx_ended_obj)

static V rx_open_fn(V self, V *a) {
  int n = ((Clo *)self)->arity;
  for (int i = 0; i < n; i++) rx_held[i] = a[i];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (`b` given a function of `n` arguments that keeps them: what it holds
// at `rx_held`; `b` consumed)
static void rx_open(V b, int n) {
  heff_drop(heff_answer(b, heff_clo(rx_open_fn, n, 0, NULL)));
}

static V heff_provider_reactor(V p, V k) {
  switch (heff_tag_of(p)) {
  // (a group opened, with what a failure of a step in it is finished
  // as, where `Just` that is given)
  case TAG_Open: {
    V failing = heff_field(p, 0);
    V failed = 0;
    if (heff_tag_of(failing) == TAG_Just) {
      rx_open(heff_dup(heff_field(failing, 0)), 1);
      failed = rx_held[0];
    }
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_int(heff_reactor_open(failed)));
    return heff_tail(k, 1, &x);
  }
  case TAG_Begin: {
    int64_t g = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    int64_t up = heff_group, upTrying = rx_trying;
    heff_group = g;
    rx_trying = g;
    V x = heff_answer(heff_dup(f), NTAG(TAG_True));
    V r;
    int failed = heff_group_step(g, heff_dup(k), x, &r);
    heff_group = up;
    rx_trying = upTrying;
    // (failed, where the group says what that is: finished so, and the
    // start told it waits)
    if (failed) {
      V y = heff_answer(f, NTAG(TAG_False));
      return heff_tail(k, 1, &y);
    }
    if (r == RX_ENDED) {
      heff_drop(f);
      heff_drop(k);
      V v = rx_ended[0], ef = rx_ended[1], ek = rx_ended[2];
      V y = heff_answer(ef, heff_just(v));
      return heff_tail(ek, 1, &y);
    }
    if (r == HEFF_PENDING) {
      V y = heff_answer(f, NTAG(TAG_False));
      return heff_tail(k, 1, &y);
    }
    // (the program came to its end within: what started was left
    // otherwise than by its `End`, a throw heard outside the scheduler)
    heff_drop(f);
    heff_drop(k);
    return r;
  }
  case TAG_End: {
    int64_t g = INT_OF(heff_field(p, 0));
    V b = heff_dup(heff_field(p, 1));
    heff_drop(p);
    rx_open(b, 2);
    V v = rx_held[0], f = rx_held[1];
    if (g == rx_trying) {
      rx_ended[0] = v;
      rx_ended[1] = f;
      rx_ended[2] = k;
      return RX_ENDED;
    }
    heff_drop(f);
    heff_drop(k);
    return v;
  }
  case TAG_Finish: {
    int64_t g = INT_OF(heff_field(p, 0));
    int wait = heff_field(p, 1) == NTAG(TAG_True);
    V b = heff_dup(heff_field(p, 2));
    heff_drop(p);
    rx_open(b, 1);
    V f = rx_held[0];
    V x = heff_answer(f, heff_reactor_finished(g, wait));
    return heff_tail(k, 1, &x);
  }
  case TAG_Within: {
    int64_t g = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_int(heff_reactor_open_within(g)));
    return heff_tail(k, 1, &x);
  }
  case TAG_Pending: {
    int64_t g = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_int(heff_reactor_pending(g)));
    return heff_tail(k, 1, &x);
  }
  case TAG_Wake: {
    int64_t g = INT_OF(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    heff_reactor_wake_group(g);
    return heff_tail(k, 1, &x);
  }
  // (what gives up a group within another, from any domain, with what
  // it is then finished as)
  case TAG_MakeGiving: {
    int64_t g = INT_OF(heff_field(p, 0));
    rx_open(heff_dup(heff_field(p, 1)), 1);
    V gave = rx_held[0];
    V f = heff_dup(heff_field(p, 2));
    heff_drop(p);
    V x = heff_answer(f, heff_reactor_giving(g, gave));
    return heff_tail(k, 1, &x);
  }
  // (its group closed, here, where it is this domain's)
  case TAG_CloseGiving: {
    HeffGiving *gv = heff_foreign_ptr(heff_field(p, 0));
    int64_t g = gv->group;
    V x = heff_dup(heff_field(p, 1));
    // (closed before the thing may be let go of here, so that its end,
    // then, finds it closed)
    heff_reactor_close(g);
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_Close: {
    int64_t g = INT_OF(heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    heff_reactor_close(g);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of reactor");
    return 0;
  }
}
