// std.reactor's side, for the C backend: an effect provider (the shape
// of one is said in lib/std/io.provider.c) over the runtime's reactor
// (lib/c/runtime.c, "Outside operations done later"), whose groups it
// opens, starts computations in, asks for what is finished and closes.
//
// A `Begun` and a `Finished` are each a function given what to do with
// what they hold, their types erased: `rx_open` takes it -- a begun's
// computation `c` and its function `k`, a finished's function `k` -- so
// that the provider has each apart, as lib/std/thread.provider.c takes
// a work.  A computation `c` is the world's: a function of its
// continuation, which the reactor runs.

// (what a begun or a finished holds, where `rx_open` puts it)
static HEFF_TLS V rx_held[2];

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
  case TAG_Open: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, heff_int(heff_reactor_open()));
    return heff_tail(k, 1, &x);
  }
  case TAG_Start: {
    int64_t g = INT_OF(heff_field(p, 0));
    V b = heff_dup(heff_field(p, 1));
    heff_drop(p);
    rx_open(b, 2);
    V c = rx_held[0], f = rx_held[1];
    V r = heff_reactor_start(g, c);
    V x = heff_answer(f, r == HEFF_PENDING ? heff_nothing : heff_just(r));
    return heff_tail(k, 1, &x);
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
