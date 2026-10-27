// Fuel's side of std.fuel, for the C backend: an effect provider, which
// says it needs the runtime's part for fuel:
#define HEFF_FUEL 1
// `limited n c`: `c` run here under `heff_try`, a limit of `n` set on
// this thread's count (`heff_fuel_limit`); `Just` its answer, or
// `Nothing` where it ran out of the limit's own; a failure, or running
// out of the limit around it, handed on as it came.  Its computation is
// a function of `Unit`, computed only here.

// (what an operation's `Some` holds, where `fl_open` puts it)
static HEFF_TLS V fl_held;

static V fl_open_fn(V self, V *a) {
  fl_held = a[0];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (what `s` holds; `s` consumed)
static V fl_open(V s) {
  heff_drop(heff_answer(s, heff_clo(fl_open_fn, 1, 0, NULL)));
  return fl_held;
}

// (the computation's answer, kept where the continuation's one field says)
static V fl_answered(V self, V *a) {
  V *at = (V *)(intptr_t)INT_OF(heff_env(self)[0]);
  *at = a[0];
  heff_drop(self);
  return NTAG(TAG_MkUnit);
}

// (`c` computed, and run, on at most `n`; `c` consumed)
static V fl_limited(int64_t n, V c) {
  V answer = 0;
  V env = MK_INT((int64_t)(intptr_t)&answer);
  V out;
  HeffFuelLimit l = heff_fuel_limit(n);
  const char *why = heff_try(c, NTAG(TAG_MkUnit), &out);
  V comp = why ? 0 : out;
  if (!why) {
    why = heff_try(comp, heff_clo(fl_answered, 1, 1, &env), &out);
    if (!why) heff_drop(out);
  }
  if (heff_fuel_unlimit(l, why)) return NTAG(TAG_Nothing);
  if (why) heff_fail_again(why);
  if (!answer) heff_fail("fuel: a computation limited ended without an answer");
  return heff_tag_n(TAG_Just, 1, &answer);
}

static V heff_provider_fuel(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_Limited: {
    V t = fl_open(heff_dup(heff_field(p, 0)));
    heff_drop(p);
    // (a big number of fuel as good as endless, a negative one none)
    V nv = heff_field(t, 0);
    int64_t n = IS_INT(nv) ? INT_OF(nv) : heff_int_neg(nv) ? 0 : HEFF_FUEL_ENDLESS - 1;
    V c = heff_carried_fn(heff_dup(heff_field(t, 1))), g = heff_dup(heff_field(t, 2));
    heff_drop(t);
    x = heff_answer(g, fl_limited(n, c));
    break;
  }
  default:
    heff_fail("not an operation of fuel");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
