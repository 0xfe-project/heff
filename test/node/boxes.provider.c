// boxes.hf's world, for the C backend: an effect provider.  A box is an
// int64_t of its own, handed out as a foreign value, freed and counted
// when the program drops its last reference to it -- whichever
// provider's operations it was given to meanwhile.

static int64_t heff_boxes_freed = 0;

static void heff_box_free(void *box) {
  heff_boxes_freed++;
  free(box);
}

static V heff_provider_boxes(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_NewBox: {
    int64_t *box = heff_alloc(sizeof(int64_t));
    *box = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, heff_foreign(box, heff_box_free));
    return heff_tail(k, 1, &x);
  }
  case TAG_Peek: {
    int64_t *box = heff_foreign_ptr(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(*box));
    return heff_tail(k, 1, &x);
  }
  case TAG_Clear: {
    int64_t *box = heff_foreign_ptr(heff_field(p, 0));
    *box = 0;
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_Freed: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(heff_boxes_freed));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of boxes");
    return 0;
  }
}
