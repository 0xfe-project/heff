// The standard streams' side of lib/std/io.hf, for the C backend: an
// effect provider.  A provider of the C backend defines, for each
// effect definition X of its module, `static V heff_provider_X(V p, V
// k)`: the operation `p` (a tag, `heff_tag_of`, its fields
// `heff_field`), done, and its answer given to the continuation `k`,
// last (`heff_tail`), as the JavaScript providers do (`k(a[1])`).  A
// function of the operation's (`ReadStdin`'s) is asked for its value
// through `heff_tr`.  It owns `p` and `k` (reference counts, as
// lib/c/runtime.c says): it takes what it keeps of `p` (`heff_dup`),
// drops `p`, and gives `k` away.  It is bundled after lib/c/runtime.c,
// whose functions it uses -- among them the world's, which hold what
// every provider hands out (`heff_hold`, `heff_held`, `heff_release`:
// lib/std/handle.hf says how).

static V heff_provider_stdio(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Write: {
    heff_put(stdout, heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteErr: {
    fflush(stdout);
    heff_put(stderr, heff_field(p, 0));
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadStdin: {
    V f = heff_dup(heff_field(p, 0));
    heff_drop(p);
    size_t cap = 4096, n = 0;
    unsigned char *buf = heff_alloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n, stdin)) > 0) {
      n += got;
      if (n == cap) {
        unsigned char *b2 = heff_alloc(cap * 2);
        memcpy(b2, buf, n);
        free(buf);
        buf = b2;
        cap *= 2;
      }
    }
    V s = heff_str_utf8(buf, (int64_t)n);
    free(buf);
    V x = heff_tr(heff_app1(f, s));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of stdio");
    return 0;
  }
}
