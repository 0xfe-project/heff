// test/node/jstext.hf's world, for the C backend: an effect provider
// (the shape of one is said in lib/std/io.provider.c).  A `JSString`
// field is a string here, its text UTF-8 and valid (all a `toJS` makes),
// read as text is (`heff_cstr_text`, which gives it as it is), and
// given back as it is.

static V heff_provider_jstext(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Units: {
    // (the UTF-16 code units of its runes: one each, two beyond U+FFFF)
    char *c = heff_cstr_text(heff_field(p, 0));
    int64_t n = 0;
    for (const unsigned char *b = (const unsigned char *)c; c && *b; b++)
      n += ((*b & 0xC0) != 0x80) + (*b >= 0xF0);
    free(c);
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, MK_INT(n));
    return heff_tail(k, 1, &x);
  }
  case TAG_Echo: {
    V t = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_answer(f, t);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of jstext");
    return 0;
  }
}
