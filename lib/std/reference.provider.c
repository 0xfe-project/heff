// The world's cells, lib/std/reference.hf's, for the C backend: an effect
// provider (the shape of one is said in lib/std/io.provider.c).  A cell
// is `MkReference i`, its value the provider's `i`th, as in
// reference.provider.js (each cell holding a reference to it).  Threads
// at once (`HEFF_THREADS`, docs/threads.md): the cells theirs, each
// holding them while it looks at them; a value going in is shared
// (`heff_share`, #66), and a cell holds it so, read by one more count
// (`heff_dup`) and written by a swap, the old value let go of after the
// lock -- nothing copied, any thread counting it atomically.

static V *heff_cells = NULL;
static int64_t heff_ncells = 0, heff_capcells = 0;

#ifdef HEFF_THREADS
static pthread_mutex_t heff_cells_mu = PTHREAD_MUTEX_INITIALIZER;
#define HEFF_CELLS_LOCK() pthread_mutex_lock(&heff_cells_mu)
#define HEFF_CELLS_UNLOCK() pthread_mutex_unlock(&heff_cells_mu)
#else
#define HEFF_CELLS_LOCK() ((void)0)
#define HEFF_CELLS_UNLOCK() ((void)0)
#endif

static V heff_provider_reference(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_NewReference: {
    V v = heff_share(heff_dup(heff_field(p, 0)));
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
    V x = heff_tr(heff_app1(f, heff_tag(TAG_MkReference, 1, &i)));
    return heff_tail(k, 1, &x);
  }
  case TAG_ReadReference: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    HEFF_CELLS_LOCK();
    V v = heff_dup(heff_cells[i]);
    HEFF_CELLS_UNLOCK();
    V x = heff_tr(heff_app1(f, v));
    return heff_tail(k, 1, &x);
  }
  case TAG_WriteReference: {
    int64_t i = INT_OF(heff_field(heff_field(p, 0), 0));
    V v = heff_share(heff_dup(heff_field(p, 1)));
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
    heff_fail("not an operation of reference");
    return 0;
  }
}
