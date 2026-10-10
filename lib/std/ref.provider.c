// The world's cells, lib/std/ref.hf's, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  A cell
// is `MkRef i`, its value the provider's `i`th, as in ref.provider.js
// (each cell holding a reference to it).  Threads at once
// (`HEFF_THREADS`, docs/threads.md): the cells theirs, each holding them
// while it looks at them, and a value going in or out a copy
// (`heff_copy_for_thread`) -- the cells' own, or the reader's -- so that
// no object in a cell is counted by two threads.

static V *heff_cells = NULL;
static int64_t heff_ncells = 0, heff_capcells = 0;

#ifdef HEFF_THREADS
static pthread_mutex_t heff_cells_mu = PTHREAD_MUTEX_INITIALIZER;
#define HEFF_CELLS_LOCK() pthread_mutex_lock(&heff_cells_mu)
#define HEFF_CELLS_UNLOCK() pthread_mutex_unlock(&heff_cells_mu)
// (a value given to the cells, or taken from them: a copy)
static V heff_cell_in(V v) {
  V c = heff_copy_for_thread(v);
  heff_drop(v);
  return c;
}
#define heff_cell_out(v) heff_copy_for_thread(v)
#else
#define HEFF_CELLS_LOCK() ((void)0)
#define HEFF_CELLS_UNLOCK() ((void)0)
#define heff_cell_in(v) (v)
#define heff_cell_out(v) heff_dup(v)
#endif

static V heff_provider_ref(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_NewRef: {
    V v = heff_cell_in(heff_dup(heff_field(p, 0)));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HEFF_CELLS_LOCK();
    if (heff_ncells == heff_capcells) {
      heff_capcells = heff_capcells ? heff_capcells * 2 : 16;
      V *c = heff_alloc(heff_capcells * sizeof(V));
      if (heff_ncells) memcpy(c, heff_cells, heff_ncells * sizeof(V));
      free(heff_cells);
      heff_cells = c;
    }
    heff_cells[heff_ncells] = v;
    V i = MK_INT(heff_ncells++);
    HEFF_CELLS_UNLOCK();
    V x = heff_tr(heff_app1(f, heff_tag(TAG_MkRef, 1, &i)));
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadRef: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HEFF_CELLS_LOCK();
    V v = heff_cell_out(heff_cells[i]);
    HEFF_CELLS_UNLOCK();
    V x = heff_tr(heff_app1(f, v));
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteRef: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V v = heff_cell_in(heff_dup(heff_field(p, 1)));
    V x = heff_dup(heff_field(p, 2));
    heff_drop(p);
    HEFF_CELLS_LOCK();
    V old = heff_cells[i];
    heff_cells[i] = v;
    HEFF_CELLS_UNLOCK();
    heff_drop(old);
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of ref");
    return 0;
  }
}
