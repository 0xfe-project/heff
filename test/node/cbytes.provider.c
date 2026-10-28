// cbytes.hf's functions, for the C backend: no provider, the compiler
// makes it from the module's description.  Bytes come in as a pointer
// and a count, NULs among them; bytes go out as a pointer, their count
// put where the last argument points.

// (how many of the bytes are NUL)
static int64_t cb_zeros(const unsigned char *p, size_t n) {
  int64_t z = 0;
  for (size_t i = 0; i < n; i++)
    if (!p[i]) z++;
  return z;
}

// (the bytes backwards, malloc'd)
static unsigned char *cb_reverse(const unsigned char *p, size_t n, size_t *out) {
  unsigned char *r = malloc(n ? n : 1);
  for (size_t i = 0; i < n; i++) r[i] = p[n - 1 - i];
  *out = n;
  return r;
}

// (the bytes `times` times, malloc'd; none of a negative count, EINVAL)
static unsigned char *cb_repeat(const unsigned char *p, size_t n, int64_t times, size_t *out) {
  if (times < 0) {
    errno = EINVAL;
    return NULL;
  }
  unsigned char *r = malloc(n * (size_t)times + 1);
  for (int64_t t = 0; t < times; t++) memcpy(r + (size_t)t * n, p, n);
  *out = n * (size_t)times;
  return r;
}

// (a PNG file's first bytes, kept by C: not freed)
static const unsigned char cb_png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0};

static const unsigned char *cb_magic(size_t *out) {
  *out = sizeof cb_png;
  return cb_png;
}
