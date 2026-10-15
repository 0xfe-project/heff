// tally.hf's world, for the C backend: an effect provider.  It acts on
// the boxes boxes.hf's provider hands out, given to it as they are: a
// box is that provider's int64_t, a foreign value.

static V heff_provider_tally(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Bump: {
    int64_t *box = heff_foreign_ptr(heff_field(p, 0));
    (*box)++;
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of tally");
    return 0;
  }
}
