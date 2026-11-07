// The system's randomness, std.entropy's side for the C backend: an
// effect provider.  The bytes are the system's own generator's, read
// there and then: `getentropy` (256 bytes at most a call, so asked for
// so many at a time), or on Windows the C library's `rand_s`, 4 bytes a
// call.  Neither fails but where the system cannot give them at all.

#ifdef _WIN32
int __cdecl rand_s(unsigned int *r);
#elif defined(__APPLE__)
#include <sys/random.h>
#endif

// (`n` bytes of the system's randomness at `at`)
static void en_gather(uint8_t *at, int64_t n) {
#ifdef _WIN32
  for (int64_t i = 0; i < n; i += 4) {
    unsigned int r;
    if (rand_s(&r) != 0) heff_fail("entropy: the system gave no randomness");
    for (int64_t j = 0; j < 4 && i + j < n; j++) at[i + j] = (uint8_t)(r >> (8 * j));
  }
#else
  for (int64_t i = 0; i < n; i += 256) {
    size_t m = (size_t)(n - i < 256 ? n - i : 256);
    if (getentropy(at + i, m) != 0) heff_fail("entropy: the system gave no randomness");
  }
#endif
}

static V heff_provider_entropy(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Gather: {
    int64_t n = INT_OF(heff_field(p, 0));
    V f = heff_dup(heff_field(p, 1));
    heff_drop(p);
    uint8_t *at;
    V b = heff_bytes_room(n, &at);
    en_gather(at, n);
    V x = heff_tr(heff_app1(f, b));
    return heff_tail(k, 1, &x);
  }
  default:
    heff_fail("not an operation of entropy");
    return 0;
  }
}
