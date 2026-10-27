// Fuel (#133), the runtime's part: bundled after the runtime only into a
// program built with fuel (each loop ticked by the compiler) or one whose
// world has std.fuel's effect, so a program with neither is as it was.
//
// What is left of the fuel, counted down by each tick, and how many
// limits are set on this thread: each thread's own, a thread started with
// none, nothing here knowing what a limit is for.  With none, a tick that
// finds the fuel out (as a thread's first does, its count 0) fills it
// again, all but endless; under one, it stops the computation -- fails as
// `heff_fail` does, `HEFF_FUEL`, caught by the innermost `heff_try`, the
// limit's, which says how much is left of it then.  It does not resume:
// what the frames jumped over were doing is left undone, what they made
// leaked, as a failure leaves it.

#define HEFF_FUEL "heff:fuel"
// (as good as endless, and a word)
#define HEFF_FUEL_ENDLESS (((int64_t)1 << 62) - 1)

HEFF_VAR(HEFF_TLS int64_t heff_fuel);
HEFF_VAR(HEFF_TLS long heff_fuel_on);

// (the fuel found out by a tick: filled again, there being no limit, or
// the computation stopped)
HEFF_SLOW void heff_fuel_out(void) {
  if (heff_fuel_on == 0) { heff_fuel = HEFF_FUEL_ENDLESS; return; }
  heff_fuel = 0;
  heff_fail(HEFF_FUEL);
}

// (a tick: a decrement and a branch, the branch seldom taken)
static inline V heff_fuelTick(void) {
  if (__builtin_expect(--heff_fuel < 0, 0)) heff_fuel_out();
  return NTAG(TAG_MkUnit);
}

// (`n` ticks at once, of a batch of rounds: the same, `n` at a time)
static inline V heff_fuelTicks(V n) {
  heff_fuel -= INT_OF(n);
  if (__builtin_expect(heff_fuel < 0, 0)) heff_fuel_out();
  return NTAG(TAG_MkUnit);
}

// (a limit of `n` set, under what is left of this thread's: where there
// is less left than `n`, the limit is what is left, and running out of
// it is running out of the one around it too)
typedef struct { int64_t outer, given; int bounded; } HeffFuelLimit;

HEFF_FN HeffFuelLimit heff_fuel_limit(int64_t n) {
  HeffFuelLimit l;
  l.outer = heff_fuel_on > 0 ? (heff_fuel > 0 ? heff_fuel : 0) : HEFF_FUEL_ENDLESS;
  l.bounded = n >= l.outer;
  l.given = n < 0 ? 0 : l.bounded ? l.outer : n;
  heff_fuel = l.given;
  heff_fuel_on++;
  return l;
}

// (... the limit ended, as the computation ended or was stopped: what it
// used taken from the fuel around it.  Whether the computation ran out
// of this limit's own (not of the one around it): it is stopped there,
// the one around going on; else, out of the one around too)
HEFF_FN int heff_fuel_unlimit(HeffFuelLimit l, const char *why) {
  int64_t left = heff_fuel > 0 ? heff_fuel : 0;
  heff_fuel_on--;
  int out = why && strcmp(why, HEFF_FUEL) == 0;
  heff_fuel = heff_fuel_on == 0 ? HEFF_FUEL_ENDLESS
    : out && l.bounded ? 0 : l.outer - (l.given - left);
  return out && !l.bounded;
}
