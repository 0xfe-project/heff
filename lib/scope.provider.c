// The world's scopes, the scope module's, for the C backend: an effect
// provider.  A token is `Held a`, `a` a foreign value holding the scope's
// release (a computation of the world) and its number, the scopes
// entered counted in each thread; `Leave` disarms it, the release let go
// of.  Where its last reference goes armed, its release is put by for the
// runtime to run, the newest scope's first, once what was dropped is all
// given back ("Scopes left by a dropped continuation").

typedef struct { V release; uint64_t seq; } HeffArmed;

static HEFF_TLS uint64_t heff_scopes_entered = 0;

static void heff_armed_let_go(void *p) {
  HeffArmed *a = p;
  if (a->release) heff_unwound_put(a->seq, a->release);
  free(a);
}

static V heff_provider_scope(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Enter: {
    HeffArmed *a = heff_alloc(sizeof *a);
    a->release = heff_dup(heff_field(p, 0));
    a->seq = ++heff_scopes_entered;
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V g = heff_foreign(a, heff_armed_let_go);
    V x = heff_answer(f, heff_tag(TAG_Held, 1, &g));
    return heff_tail(k, 1, &x);
  }
  case TAG_Leave: {
    V t = heff_field(p, 0);
    if (heff_tag_of(t) == TAG_Held) {
      HeffArmed *a = heff_foreign_ptr(heff_field(t, 0));
      V r = a->release;
      a->release = 0;
      if (r) heff_drop(r);
    }
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of scope");
    return 0;
  }
}
