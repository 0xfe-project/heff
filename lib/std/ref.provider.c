// The world's cells, lib/std/ref.hf's, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  A cell
// is `MkRef i`, its value the provider's `i`th, as in ref.provider.js
// (each cell holding a reference to it).

static V *heff_cells = NULL;
static int64_t heff_ncells = 0, heff_capcells = 0;

static V heff_provider_ref(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_NewRef: {
    if (heff_ncells == heff_capcells) {
      heff_capcells = heff_capcells ? heff_capcells * 2 : 16;
      V *c = heff_alloc(heff_capcells * sizeof(V));
      if (heff_ncells) memcpy(c, heff_cells, heff_ncells * sizeof(V));
      free(heff_cells);
      heff_cells = c;
    }
    heff_cells[heff_ncells] = heff_dup(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V i = MK_INT(heff_ncells++);
    V x = heff_tr(heff_app1(f, heff_tag(TAG_MkRef, 1, &i)));
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadRef: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    V x = heff_tr(heff_app1(f, heff_dup(heff_cells[i])));
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteRef: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V v = heff_dup(heff_field(p, 1));
    V x = heff_dup(heff_field(p, 2));
    heff_drop(p);
    heff_drop(heff_cells[i]);
    heff_cells[i] = v;
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of ref");
    return 0;
  }
}
