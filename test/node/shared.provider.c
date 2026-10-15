// shared.hf's world, for the C backend: the functions its description
// names, no provider written by hand.  A token is a count of the
// places holding it, handed out as a foreign value shared between
// places: held once more as it crosses to one, let go of as a place
// drops its last reference, freed by the last to -- the crossings and
// the frees counted, for the program to read back.

typedef struct { int64_t holders; } HeffToken;

static int64_t heff_tokens_held_n = 0;
static int64_t heff_tokens_freed_n = 0;

static HeffToken *heff_token_new(void) {
  HeffToken *t = heff_alloc(sizeof *t);
  t->holders = 1;
  return t;
}

static void heff_token_hold(void *x) {
  HeffToken *t = x;
  __atomic_fetch_add(&t->holders, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&heff_tokens_held_n, 1, __ATOMIC_RELAXED);
}

static void heff_token_drop(void *x) {
  HeffToken *t = x;
  if (__atomic_fetch_sub(&t->holders, 1, __ATOMIC_ACQ_REL) > 1) return;
  free(t);
  __atomic_fetch_add(&heff_tokens_freed_n, 1, __ATOMIC_RELAXED);
}

static int64_t heff_token_holders(HeffToken *t) {
  return __atomic_load_n(&t->holders, __ATOMIC_ACQUIRE);
}

static int64_t heff_tokens_held(void) {
  return __atomic_load_n(&heff_tokens_held_n, __ATOMIC_ACQUIRE);
}

static int64_t heff_tokens_freed(void) {
  return __atomic_load_n(&heff_tokens_freed_n, __ATOMIC_ACQUIRE);
}
