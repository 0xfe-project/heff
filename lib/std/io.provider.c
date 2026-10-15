// The standard streams' side of std.io, for the C backend: an
// effect provider.  A provider of the C backend defines, for each
// effect definition X of its module, `static V heff_provider_X(V p, V
// k)`: the operation `p` (a tag, `heff_tag_of`, its fields
// `heff_field`), done, and its answer given to the continuation `k`,
// last (`heff_tail`), as the JavaScript providers do (`k(a[1])`).  A
// function of the operation's (`ReadStdin`'s) is asked for its value
// through `heff_tr`.  It owns `p` and `k` (reference counts, as
// the runtime says): it takes what it keeps of `p` (`heff_dup`),
// drops `p`, and gives `k` away.  A number is as the runtime keeps it: an
// Int a word (`INT_OF`, `MK_INT`) or big, an Int64 and a UInt64 as an Int
// is, an integer of a fixed-width type of up to 32 bits a word of its
// value, a Float32 a word of its bits (`heff_f32_of`, `heff_f32`), a Float64
// boxed (`FLT_OF`, `heff_flt`); a Rune is a word of its code point.  It
// is bundled after the runtime, whose functions it uses -- among them
// those of foreign values, the host's things a provider hands out and
// another takes (`heff_foreign`, `heff_foreign_ptr`).
// A string it is given, or gives, crosses to the system and back only
// through the runtime's helpers, by what it is there -- content, a path,
// a name or text: `heff_str_bytes`, `heff_put`, `heff_cstr`,
// `heff_str_text`, `heff_cstr_text` and the like ("What providers share"
// in the runtime says which is which) -- never by its units, so that
// how the runtime keeps a string is the runtime's alone.

// (the lines std.progress draws on the terminal, once there are any:
// what is written clears them first, and they come back below it)
static void (*heff_lines)(int);

static V heff_provider_stdio(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Write: {
    if (heff_lines) heff_lines(1);
    heff_put(stdout, heff_field(p, 0));
    if (heff_lines) heff_lines(0);
    V x = heff_dup(heff_field(p, 1));
    heff_drop(p);
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteErr: {
    if (heff_lines) heff_lines(1);
    fflush(stdout);
    heff_put(stderr, heff_field(p, 0));
    if (heff_lines) heff_lines(0);
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
    V s = heff_str_bytes(buf, (int64_t)n);
    free(buf);
    V x = heff_tr(heff_app1(f, s));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of stdio");
    return 0;
  }
}
