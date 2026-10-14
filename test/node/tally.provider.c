// tally.hf's world, for the C backend: an effect provider.  It acts on the boxes
// boxes.hf's provider hands out, by the world's numbers for them:
// a box is that provider's int64_t.

static V heff_provider_tally(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Bump: {
    int64_t *box = heff_held(INT_OF(heff_field(heff_field(p, 0), 0)));
    (*box)++;
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_Again: {
    V i = MK_INT(heff_hold(heff_held(INT_OF(heff_field(heff_field(p, 0), 0))), NULL));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_tag(TAG_MkHandle, 1, &i));
    return heff_tail(k, 1, &x);
  }
  case TAG_LetGo: {
    heff_release(INT_OF(heff_field(heff_field(p, 0), 0)));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of tally");
    return 0;
  }
}
