// flaky.hf's world, for the C backend: an effect provider.  A thing is
// a struct of its own, handed out as a foreign value that can be ended
// before it is let go of; one made to fail says so as it is ended,
// once, whoever ends it.

typedef struct FlakyThing {
  int fails, ended;
  int64_t id;
} FlakyThing;

static int64_t flaky_made = 0;
static HEFF_TLS char flaky_why[64];

static const char *flaky_end(void *p) {
  FlakyThing *t = p;
  if (t->ended) return NULL;
  t->ended = 1;
  if (!t->fails) return NULL;
  snprintf(flaky_why, sizeof flaky_why, "flaky: thing %lld not ended", (long long)t->id);
  return flaky_why;
}

static void flaky_free(void *p) { free(p); }

static V heff_provider_flaky(V p, V k) {
  V x;
  switch (heff_tag_of(p)) {
  case TAG_NewThing: {
    FlakyThing *t = heff_alloc(sizeof *t);
    t->fails = heff_tag_of(heff_field(p, 0)) == TAG_True;
    t->ended = 0;
    t->id = __atomic_add_fetch(&flaky_made, 1, __ATOMIC_SEQ_CST);
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V v = heff_foreign_ending(t, flaky_free, flaky_end);
    x = heff_answer(f, heff_tag_n(TAG_Right, 1, &v));
    break;
  }
  case TAG_LetGoThing: {
    FlakyThing *t = heff_foreign_ptr(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    // (where this is its one holder, ended here; else by the last)
    const char *why = heff_unique(p) && heff_unique(heff_field(p, 0)) ? flaky_end(t) : NULL;
    V r;
    if (why) {
      V s = heff_str_text(why);
      r = heff_tag_n(TAG_Left, 1, &s);
    } else {
      V u = NTAG(TAG_MkUnit);
      r = heff_tag_n(TAG_Right, 1, &u);
    }
    heff_drop(p);
    x = heff_answer(f, r);
    break;
  }
  default:
    heff_fail("not an operation of flaky");
    return 0;
  }
  return heff_tail(k, 1, &x);
}
