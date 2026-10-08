// test/node/boxes.hf's world, for the C backend: an effect provider (the
// shape of one is said in lib/std/io.provider.c).  A box is an int64_t
// of its own, held by the world (lib/std/handle.hf), freed and counted
// when the world lets it go -- whichever provider asked it to.

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
    V i = MK_INT(heff_hold(box, heff_box_free));
    V x = heff_answer(f, heff_tag(TAG_MkHandle, 1, &i));
    return heff_tail(k, 1, &x);
  }
  case TAG_Peek: {
    int64_t *box = heff_held(INT_OF(heff_field(heff_field(p, 0), 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(*box));
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
