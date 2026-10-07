// heff's runtime for the C backend (src/heff/emitc.hf), bundled whole
// into each program it compiles, before the program's own code and its
// worlds' providers (`M.provider.c` beside `M.hf`).
//
// A value is a word.  Its low bit set, it is an integer (`n << 1 | 1`,
// 62 bits and a sign); its low bits `10`, a tag with no fields (`id << 2
// | 2`); otherwise it points to an object, whose header says what it
// is.  Tags are numbered (`TAG_x`), the ones the runtime knows first
// (below), the program's after them; record labels too (`LBL_x`), a
// record's fields in the order it was written with them.  Strings are
// UTF-16 code units, as JavaScript's are, so that what a program
// computes with them is what the other backends compute.
//
// An object is counted (Perceus): each owner of it holds a reference,
// `heff_dup` takes one more and `heff_drop` gives one back, the last
// freeing it.  What a function is given it owns: it consumes it (gives
// it to what it calls, keeps it in what it builds) or drops it -- the
// emitter says where (src/heff/emitc.hf, "Reference counts"); every
// primitive and the runtime's calls consume their arguments too.  What
// is static (a string written in the program, a definition's closure) is
// immortal, its count never changed.  A program runs in a thread of its
// own with a large stack, as the Node.js backend's does.  A call last of an unknown
// function counts itself and every so often gives back a bounce (`K_BNC`),
// let go of where the value is asked for (`heff_tr`), as the JavaScript
// backend does (src/heff/emit.hf, "Tail calls").

#include <alloca.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t V;

// the tags the runtime knows; a program's own are numbered after them
enum {
  TAG_True, TAG_False, TAG_Nil, TAG_Cons, TAG_MkUnit, TAG_Just, TAG_Nothing,
  TAG_MkPair, TAG_InL, TAG_InR, TAG_KNOWN
};

enum { K_TAG, K_REC, K_CLO, K_PAP, K_STR, K_FLT, K_ARR, K_BNC, K_CELL };

typedef struct Obj {
  uint32_t rc;
  uint16_t kind;
  uint16_t tag;
} Obj;

typedef V (*Fn)(V self, V *args);

typedef struct { Obj h; int64_t n; V f[]; } Tag;
typedef struct { Obj h; int64_t n; uint32_t *lbl; V f[]; } Rec;
typedef struct { Obj h; Fn fn; int32_t arity; int32_t nenv; V env[]; } Clo;
typedef struct { Obj h; V fun; int32_t n; V a[]; } Pap;
typedef struct { Obj h; int64_t len; uint16_t s[]; } Str;
typedef struct { Obj h; double d; } Flt;
typedef struct { Obj h; int64_t n; V a[]; } Arr;
typedef struct { Obj h; V fun; int32_t n; V a[]; } Bnc;

#define IS_INT(v) (((v) & 1) == 1)
#define INT_OF(v) ((int64_t)(v) >> 1)
#define MK_INT(n) ((V)((((uint64_t)(int64_t)(n)) << 1) | 1))
#define IS_NTAG(v) (((v) & 3) == 2)
#define NTAG(id) ((V)(((uintptr_t)(id) << 2) | 2))
#define OBJ(v) ((Obj *)(v))
#define INT_MAX62 ((int64_t)1 << 61)
#define IS_OBJ(v) (((v) & 3) == 0)
#define IMMORTAL 0x80000000u

static void heff_free(V v);

static inline V heff_dup(V v) {
  if (IS_OBJ(v) && !(OBJ(v)->rc & IMMORTAL)) OBJ(v)->rc++;
  return v;
}

static inline void heff_drop(V v) {
  if (IS_OBJ(v) && !(OBJ(v)->rc & IMMORTAL) && --OBJ(v)->rc == 0) heff_free(v);
}

static inline int heff_unique(V v) { return OBJ(v)->rc == 1; }

// (where a value is asked for that may fail -- `heff compile` without
// `--name`, each definition printed -- a failure is caught there, and
// said; otherwise it ends the program)
static jmp_buf *heff_catch = NULL;
static const char *heff_caught = NULL;

static void heff_fail(const char *why) {
  if (heff_catch) { heff_caught = why; longjmp(*heff_catch, 1); }
  fflush(stdout);
  fprintf(stderr, "heff: %s\n", why);
  exit(1);
}

static V heff_abandon(void) {
  if (heff_catch) { heff_caught = "heff:abandon"; longjmp(*heff_catch, 1); }
  fflush(stdout);
  fprintf(stderr, "heff:abandon\n");
  exit(1);
}

static V heff_nomatch(V s);

static void *heff_alloc(size_t n) {
  void *p = malloc(n);
  if (!p) heff_fail("out of memory");
  return p;
}

// Objects are small and made and given back all the time (a
// computation's continuations, its operations): those of up to
// `SMALL_MAX` bytes come from free lists, one for each multiple of 8
// bytes, an object given back kept for the next of its size.
#define SMALL_MAX 128
static void *heff_free_list[SMALL_MAX / 8 + 1];

static inline void *heff_obj_alloc(size_t n) {
  size_t c = (n + 7) / 8;
  if (c <= SMALL_MAX / 8 && heff_free_list[c]) {
    void *p = heff_free_list[c];
    heff_free_list[c] = *(void **)p;
    return p;
  }
  return heff_alloc(c * 8);
}

static inline void heff_obj_free(void *p, size_t n) {
  size_t c = (n + 7) / 8;
  if (c <= SMALL_MAX / 8) {
    *(void **)p = heff_free_list[c];
    heff_free_list[c] = p;
  } else free(p);
}

static inline int heff_tag_of(V v) {
  return IS_NTAG(v) ? (int)(v >> 2) : OBJ(v)->tag;
}

static V heff_tag(int id, int64_t n, V *f) {
  if (n == 0) return NTAG(id);
  Tag *t = heff_obj_alloc(sizeof(Tag) + n * sizeof(V));
  t->h.rc = 1; t->h.kind = K_TAG; t->h.tag = (uint16_t)id; t->n = n;
  memcpy(t->f, f, n * sizeof(V));
  return (V)t;
}

static inline V heff_field(V v, int i) { return ((Tag *)v)->f[i]; }

// (the labels, in the order the record was written, are the program's:
// one array for each)
static V heff_rec(int64_t n, uint32_t *lbl, V *f) {
  Rec *r = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  r->h.rc = 1; r->h.kind = K_REC; r->h.tag = 0; r->n = n; r->lbl = lbl;
  memcpy(r->f, f, n * sizeof(V));
  return (V)r;
}

// (a field, the record consumed)
static V heff_get(V v, uint32_t l) {
  Rec *r = (Rec *)v;
  for (int64_t i = 0; i < r->n; i++)
    if (r->lbl[i] == l) {
      V x = heff_dup(r->f[i]);
      heff_drop(v);
      return x;
    }
  heff_fail("no such field");
  return 0;
}

static V heff_clo(Fn fn, int arity, int nenv, V *env) {
  Clo *c = heff_obj_alloc(sizeof(Clo) + nenv * sizeof(V));
  c->h.rc = 1; c->h.kind = K_CLO; c->h.tag = 0;
  c->fn = fn; c->arity = arity; c->nenv = nenv;
  if (nenv) memcpy(c->env, env, nenv * sizeof(V));
  return (V)c;
}

static inline V *heff_env(V self) { return ((Clo *)self)->env; }

// (the last reference gone: what it holds given back, then itself)
// (an object's size, from what it is)
static size_t heff_size(V v) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: return sizeof(Tag) + ((Tag *)v)->n * sizeof(V);
  case K_REC: return sizeof(Rec) + ((Rec *)v)->n * sizeof(V);
  case K_CLO: return sizeof(Clo) + ((Clo *)v)->nenv * sizeof(V);
  case K_PAP: return sizeof(Pap) + ((Pap *)v)->n * sizeof(V);
  case K_STR: return sizeof(Str) + ((Str *)v)->len * sizeof(uint16_t);
  case K_FLT: return sizeof(Flt);
  case K_ARR: return sizeof(Arr) + ((Arr *)v)->n * sizeof(V);
  case K_BNC: return sizeof(Bnc) + ((Bnc *)v)->n * sizeof(V);
  default: return SMALL_MAX + 1;
  }
}

static void heff_free(V v) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: { Tag *t = (Tag *)v; for (int64_t i = 0; i < t->n; i++) heff_drop(t->f[i]); break; }
  case K_REC: { Rec *r = (Rec *)v; for (int64_t i = 0; i < r->n; i++) heff_drop(r->f[i]); break; }
  case K_CLO: { Clo *c = (Clo *)v; for (int i = 0; i < c->nenv; i++) heff_drop(c->env[i]); break; }
  case K_PAP: { Pap *p = (Pap *)v; heff_drop(p->fun); for (int i = 0; i < p->n; i++) heff_drop(p->a[i]); break; }
  case K_ARR: { Arr *r = (Arr *)v; for (int64_t i = 0; i < r->n; i++) heff_drop(r->a[i]); break; }
  case K_BNC: { Bnc *b = (Bnc *)v; heff_drop(b->fun); for (int i = 0; i < b->n; i++) heff_drop(b->a[i]); break; }
  default: break;
  }
  heff_obj_free(o, heff_size(v));
}

// Calls of what is not known
// ---------------------------------------------------------------------

static long heff_dep = 0;

static V heff_apply(V f, int n, V *a);

static V heff_trs(V r) {
  do {
    Bnc *b = (Bnc *)r;
    heff_dep = 0;
    V fun = b->fun;
    int n = b->n;
    V *a = alloca(n * sizeof(V));
    memcpy(a, b->a, n * sizeof(V));
    heff_obj_free(b, sizeof(Bnc) + n * sizeof(V));
    r = heff_apply(fun, n, a);
  } while (!IS_INT(r) && !IS_NTAG(r) && OBJ(r)->kind == K_BNC);
  return r;
}

static inline V heff_tr(V r) {
  return (!IS_INT(r) && !IS_NTAG(r) && OBJ(r)->kind == K_BNC) ? heff_trs(r) : r;
}

static V heff_bounce(V f, int n, V *a) {
  Bnc *b = heff_obj_alloc(sizeof(Bnc) + n * sizeof(V));
  b->h.rc = 1; b->h.kind = K_BNC; b->h.tag = 0; b->fun = f; b->n = n;
  memcpy(b->a, a, n * sizeof(V));
  return (V)b;
}

// (a call last: counted, and every so often a bounce)
static inline V heff_tail(V f, int n, V *a) {
  if (++heff_dep > 500) return heff_bounce(f, n, a);
  return heff_apply(f, n, a);
}

static V heff_apply(V f, int n, V *a) {
  for (;;) {
    Obj *o = OBJ(f);
    if (o->kind == K_PAP) {
      // (its arguments before the ones given: taken over where it is
      // held only here, else shared)
      Pap *p = (Pap *)f;
      int pn = p->n, m = pn + n;
      V *all = alloca(m * sizeof(V));
      V fun;
      if (heff_unique(f)) {
        memcpy(all, p->a, pn * sizeof(V));
        fun = p->fun;
        heff_obj_free(p, sizeof(Pap) + pn * sizeof(V));
      } else {
        for (int i = 0; i < pn; i++) all[i] = heff_dup(p->a[i]);
        fun = heff_dup(p->fun);
        heff_drop(f);
      }
      memcpy(all + pn, a, n * sizeof(V));
      f = fun; a = all; n = m;
      continue;
    }
    if (o->kind != K_CLO) heff_fail("applying what is not a function");
    Clo *c = (Clo *)f;
    if (n == c->arity) return c->fn(f, a);
    if (n < c->arity) {
      Pap *p = heff_obj_alloc(sizeof(Pap) + n * sizeof(V));
      p->h.rc = 1; p->h.kind = K_PAP; p->h.tag = 0; p->fun = f; p->n = n;
      memcpy(p->a, a, n * sizeof(V));
      return (V)p;
    }
    // (more arguments than it takes: the rest given to what it gives --
    // its arity read first, the closure consumed by the call)
    int ar = c->arity;
    V r = heff_tr(c->fn(f, a));
    f = r; a = a + ar; n = n - ar;
  }
}

static inline V heff_app1(V f, V x) { return heff_apply(f, 1, &x); }

// Integers
// ---------------------------------------------------------------------

static V heff_overflow(void) {
  heff_fail("an integer beyond 62 bits (big integers are not in the C backend yet)");
  return 0;
}

static inline V heff_int(int64_t n) {
  if (n >= INT_MAX62 || n < -INT_MAX62) return heff_overflow();
  return MK_INT(n);
}

static inline V heff_intAdd(V a, V b) {
  int64_t r;
  if (__builtin_add_overflow(INT_OF(a), INT_OF(b), &r)) return heff_overflow();
  return heff_int(r);
}

static inline V heff_intSub(V a, V b) {
  int64_t r;
  if (__builtin_sub_overflow(INT_OF(a), INT_OF(b), &r)) return heff_overflow();
  return heff_int(r);
}

static inline V heff_intMul(V a, V b) {
  int64_t r;
  if (__builtin_mul_overflow(INT_OF(a), INT_OF(b), &r)) return heff_overflow();
  return heff_int(r);
}

// (rounded down, as the other backends round)
static V heff_intDiv(V a, V b) {
  int64_t x = INT_OF(a), y = INT_OF(b);
  if (y == 0) return heff_abandon();
  int64_t q = x / y, r = x % y;
  if (r != 0 && ((r < 0) != (y < 0))) q -= 1;
  return heff_int(q);
}

static V heff_intMod(V a, V b) {
  int64_t x = INT_OF(a), y = INT_OF(b);
  if (y == 0) return heff_abandon();
  int64_t r = x % y;
  if (r != 0 && ((r < 0) != (y < 0))) r += y;
  return heff_int(r);
}

#define heff_bool(b) ((b) ? NTAG(TAG_True) : NTAG(TAG_False))

static inline V heff_intEq(V a, V b) { return heff_bool(a == b); }
static inline V heff_intLt(V a, V b) { return heff_bool(INT_OF(a) < INT_OF(b)); }
static inline V heff_intLe(V a, V b) { return heff_bool(INT_OF(a) <= INT_OF(b)); }

// Strings
// ---------------------------------------------------------------------

static Str *heff_str_new(int64_t len) {
  Str *s = heff_obj_alloc(sizeof(Str) + len * sizeof(uint16_t));
  s->h.rc = 1; s->h.kind = K_STR; s->h.tag = 0; s->len = len;
  return s;
}

static V heff_str_ascii(const char *c) {
  int64_t n = (int64_t)strlen(c);
  Str *s = heff_str_new(n);
  for (int64_t i = 0; i < n; i++) s->s[i] = (uint16_t)(unsigned char)c[i];
  return (V)s;
}

// (UTF-8 in, as read; UTF-16 kept)
static V heff_str_utf8(const unsigned char *c, int64_t n) {
  Str *s = heff_str_new(n);
  int64_t k = 0;
  for (int64_t i = 0; i < n;) {
    uint32_t u = c[i];
    int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
    if (extra) u &= (0x3F >> extra);
    i++;
    for (int j = 0; j < extra && i < n; j++, i++) u = (u << 6) | (c[i] & 0x3F);
    if (u >= 0x10000) {
      u -= 0x10000;
      s->s[k++] = (uint16_t)(0xD800 + (u >> 10));
      s->s[k++] = (uint16_t)(0xDC00 + (u & 0x3FF));
    } else s->s[k++] = (uint16_t)u;
  }
  s->len = k;
  return (V)s;
}

// (UTF-16 out, as UTF-8)
static void heff_put(FILE *f, V v) {
  Str *s = (Str *)v;
  for (int64_t i = 0; i < s->len; i++) {
    uint32_t u = s->s[i];
    if (u >= 0xD800 && u < 0xDC00 && i + 1 < s->len) {
      uint32_t w = s->s[i + 1];
      if (w >= 0xDC00 && w < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (w - 0xDC00); i++; }
    }
    if (u < 0x80) fputc((int)u, f);
    else if (u < 0x800) { fputc(0xC0 | (u >> 6), f); fputc(0x80 | (u & 0x3F), f); }
    else if (u < 0x10000) { fputc(0xE0 | (u >> 12), f); fputc(0x80 | ((u >> 6) & 0x3F), f); fputc(0x80 | (u & 0x3F), f); }
    else { fputc(0xF0 | (u >> 18), f); fputc(0x80 | ((u >> 12) & 0x3F), f); fputc(0x80 | ((u >> 6) & 0x3F), f); fputc(0x80 | (u & 0x3F), f); }
  }
}

static V heff_strAppend(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  Str *s = heff_str_new(x->len + y->len);
  memcpy(s->s, x->s, x->len * sizeof(uint16_t));
  memcpy(s->s + x->len, y->s, y->len * sizeof(uint16_t));
  heff_drop(a);
  heff_drop(b);
  return (V)s;
}

static V heff_strLength(V a) {
  V n = MK_INT(((Str *)a)->len);
  heff_drop(a);
  return n;
}

// (as JavaScript's `slice`: out of range is clamped, negative from the end)
static V heff_strSlice(V a, V i, V j) {
  Str *x = (Str *)a;
  int64_t n = x->len, b = INT_OF(i), e = INT_OF(j);
  if (b < 0) b = b + n < 0 ? 0 : b + n;
  if (e < 0) e = e + n < 0 ? 0 : e + n;
  if (b > n) b = n;
  if (e > n) e = n;
  if (e < b) e = b;
  Str *s = heff_str_new(e - b);
  memcpy(s->s, x->s + b, (e - b) * sizeof(uint16_t));
  heff_drop(a);
  return (V)s;
}

static int heff_str_cmp(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t n = x->len < y->len ? x->len : y->len;
  for (int64_t i = 0; i < n; i++)
    if (x->s[i] != y->s[i]) return x->s[i] < y->s[i] ? -1 : 1;
  return x->len == y->len ? 0 : x->len < y->len ? -1 : 1;
}

static V heff_strEq(V a, V b) {
  int c = heff_str_cmp(a, b);
  heff_drop(a);
  heff_drop(b);
  return heff_bool(c == 0);
}

static V heff_strLt(V a, V b) {
  int c = heff_str_cmp(a, b);
  heff_drop(a);
  heff_drop(b);
  return heff_bool(c < 0);
}

static V heff_strCharCode(V a, V i) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  V r = k >= 0 && k < x->len ? MK_INT(x->s[k]) : MK_INT(-1);
  heff_drop(a);
  return r;
}

static V heff_strFromCharCode(V c) {
  int64_t k = INT_OF(c);
  if (k < 0 || k > 65535) return (V)heff_str_new(0);
  Str *s = heff_str_new(1);
  s->s[0] = (uint16_t)k;
  return (V)s;
}

static V heff_intShow(V a) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", (long long)INT_OF(a));
  return heff_str_ascii(buf);
}

// Floats
// ---------------------------------------------------------------------

static V heff_flt(double d) {
  Flt *f = heff_obj_alloc(sizeof(Flt));
  f->h.rc = 1; f->h.kind = K_FLT; f->h.tag = 0; f->d = d;
  return (V)f;
}

#define FLT_OF(v) (((Flt *)(v))->d)

// (two floats' operation, both consumed)
#define FLT2(name, expr) \
  static V name(V a, V b) { double x = FLT_OF(a), y = FLT_OF(b); heff_drop(a); heff_drop(b); return expr; }
FLT2(heff_floatAdd, heff_flt(x + y))
FLT2(heff_floatSub, heff_flt(x - y))
FLT2(heff_floatMul, heff_flt(x * y))
FLT2(heff_floatDiv, heff_flt(x / y))
FLT2(heff_floatEq, heff_bool(x == y))
FLT2(heff_floatLt, heff_bool(x < y))
FLT2(heff_floatLe, heff_bool(x <= y))
static V heff_floatFromInt(V a) { return heff_flt((double)INT_OF(a)); }
static V heff_floatSqrt(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(sqrt(x)); }

static V heff_floatFloor(V a) {
  double d = FLT_OF(a);
  heff_drop(a);
  if (!isfinite(d)) return heff_abandon();
  double f = floor(d);
  if (f >= 4611686018427387904.0 || f < -4611686018427387904.0) return heff_overflow();
  return MK_INT((int64_t)f);
}

// (as JavaScript writes a number: the fewest digits that read back the
// same, in plain or exponent form by its rules (ECMAScript
// Number::toString); then ".0" where that is a whole number)
static void heff_js_number(double d, char *out) {
  if (isnan(d)) { strcpy(out, "NaN"); return; }
  if (isinf(d)) { strcpy(out, d < 0 ? "-Infinity" : "Infinity"); return; }
  if (d == 0) { strcpy(out, "0"); return; }
  char *o = out;
  if (d < 0) { *o++ = '-'; d = -d; }
  char buf[40];
  int p;
  for (p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, d);
    if (strtod(buf, NULL) == d) break;
  }
  // digits and exponent: d = 0.digits * 10^n, k digits
  char digits[20];
  int k = 0;
  char *c = buf;
  for (; *c && *c != 'e'; c++) if (*c >= '0' && *c <= '9') digits[k++] = *c;
  while (k > 1 && digits[k - 1] == '0') k--;
  int e = atoi(c + 1);
  int n = e + 1;
  if (k <= n && n <= 21) {
    memcpy(o, digits, k); o += k;
    for (int i = k; i < n; i++) *o++ = '0';
  } else if (0 < n && n <= 21) {
    memcpy(o, digits, n); o += n; *o++ = '.';
    memcpy(o, digits + n, k - n); o += k - n;
  } else if (-6 < n && n <= 0) {
    *o++ = '0'; *o++ = '.';
    for (int i = 0; i < -n; i++) *o++ = '0';
    memcpy(o, digits, k); o += k;
  } else {
    *o++ = digits[0];
    if (k > 1) { *o++ = '.'; memcpy(o, digits + 1, k - 1); o += k - 1; }
    o += sprintf(o, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
  }
  *o = 0;
}

static V heff_floatShow(V a) {
  char buf[64];
  heff_js_number(FLT_OF(a), buf);
  heff_drop(a);
  if (!strpbrk(buf, ".eNI")) strcat(buf, ".0");
  return heff_str_ascii(buf);
}

// Arrays
// ---------------------------------------------------------------------

static Arr *heff_arr_new(int64_t n) {
  Arr *r = heff_obj_alloc(sizeof(Arr) + n * sizeof(V));
  r->h.rc = 1; r->h.kind = K_ARR; r->h.tag = 0; r->n = n;
  return r;
}

static V heff_arrayLength(V a) {
  V n = MK_INT(((Arr *)a)->n);
  heff_drop(a);
  return n;
}

static V heff_arrayIndex(V a, V i) {
  Arr *r = (Arr *)a;
  int64_t k = INT_OF(i);
  if (k < 0 || k >= r->n) return heff_abandon();
  V x = heff_dup(r->a[k]);
  heff_drop(a);
  return x;
}

// (an array held only here is set in place: it is not seen again)
static V heff_arraySet(V a, V i, V x) {
  Arr *r = (Arr *)a;
  int64_t k = INT_OF(i);
  if (k < 0 || k >= r->n) return heff_abandon();
  if (heff_unique(a)) {
    heff_drop(r->a[k]);
    r->a[k] = x;
    return a;
  }
  Arr *s = heff_arr_new(r->n);
  for (int64_t j = 0; j < r->n; j++) s->a[j] = j == k ? x : heff_dup(r->a[j]);
  heff_drop(a);
  return (V)s;
}

static V heff_arrayGenerate(V n, V f) {
  int64_t m = INT_OF(n) > 0 ? INT_OF(n) : 0;
  Arr *r = heff_arr_new(m);
  for (int64_t k = 0; k < m; k++) r->a[k] = heff_tr(heff_app1(heff_dup(f), MK_INT(k)));
  heff_drop(f);
  return (V)r;
}

static V heff_arrayFromList(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  Arr *r = heff_arr_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) r->a[k++] = heff_dup(heff_field(c, 0));
  heff_drop(l);
  return (V)r;
}

static V heff_arrayToList(V a) {
  Arr *r = (Arr *)a;
  V l = NTAG(TAG_Nil);
  for (int64_t k = r->n - 1; k >= 0; k--) {
    V f[2] = { heff_dup(r->a[k]), l };
    l = heff_tag(TAG_Cons, 2, f);
  }
  heff_drop(a);
  return l;
}

// Running
// ---------------------------------------------------------------------

static V heff_nomatch(V s) {
  (void)s;
  heff_fail("no case for a tag");
  return 0;
}

// Values, shown (`heff compile` without `--name`: each definition of
// the file printed as the tests print JavaScript's, test/load-js.mjs)
// ---------------------------------------------------------------------

static const char **heff_tag_names = NULL;
static const char **heff_lbl_names = NULL;

typedef struct { char *s; size_t n, cap; } Buf;

static void buf_put(Buf *b, const char *c, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t cap = (b->cap ? b->cap * 2 : 256);
    while (cap < b->n + n + 1) cap *= 2;
    char *s = heff_alloc(cap);
    if (b->n) memcpy(s, b->s, b->n);
    free(b->s);
    b->s = s; b->cap = cap;
  }
  memcpy(b->s + b->n, c, n);
  b->n += n;
  b->s[b->n] = 0;
}

static void buf_str(Buf *b, const char *c) { buf_put(b, c, strlen(c)); }

// (a string as JSON writes one: in quotes, `"` `\` and controls escaped)
static void buf_json_string(Buf *b, V v) {
  Str *s = (Str *)v;
  buf_str(b, "\"");
  for (int64_t i = 0; i < s->len; i++) {
    uint32_t u = s->s[i];
    char tmp[16];
    if (u == '"') buf_str(b, "\\\"");
    else if (u == '\\') buf_str(b, "\\\\");
    else if (u == '\n') buf_str(b, "\\n");
    else if (u == '\t') buf_str(b, "\\t");
    else if (u == '\r') buf_str(b, "\\r");
    else if (u == '\b') buf_str(b, "\\b");
    else if (u == '\f') buf_str(b, "\\f");
    else if (u < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", u); buf_str(b, tmp); }
    else {
      if (u >= 0xD800 && u < 0xDC00 && i + 1 < s->len && s->s[i + 1] >= 0xDC00 && s->s[i + 1] < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (s->s[i + 1] - 0xDC00);
        i++;
      }
      char o[4]; int k = 0;
      if (u < 0x80) o[k++] = (char)u;
      else if (u < 0x800) { o[k++] = (char)(0xC0 | (u >> 6)); o[k++] = (char)(0x80 | (u & 0x3F)); }
      else if (u < 0x10000) { o[k++] = (char)(0xE0 | (u >> 12)); o[k++] = (char)(0x80 | ((u >> 6) & 0x3F)); o[k++] = (char)(0x80 | (u & 0x3F)); }
      else { o[k++] = (char)(0xF0 | (u >> 18)); o[k++] = (char)(0x80 | ((u >> 12) & 0x3F)); o[k++] = (char)(0x80 | ((u >> 6) & 0x3F)); o[k++] = (char)(0x80 | (u & 0x3F)); }
      buf_put(b, o, k);
    }
  }
  buf_str(b, "\"");
}

static void heff_show(Buf *b, V v, int arg) {
  char tmp[64];
  if (IS_INT(v)) {
    int64_t n = INT_OF(v);
    snprintf(tmp, sizeof tmp, arg && n < 0 ? "(%lld)" : "%lld", (long long)n);
    buf_str(b, tmp);
    return;
  }
  int tag = -1, n = 0;
  V *fs = NULL;
  if (IS_NTAG(v)) tag = (int)(v >> 2);
  else {
    Obj *o = OBJ(v);
    switch (o->kind) {
    case K_TAG: tag = o->tag; n = (int)((Tag *)v)->n; fs = ((Tag *)v)->f; break;
    case K_FLT: {
      heff_js_number(FLT_OF(v), tmp);
      if (!strpbrk(tmp, ".eNI")) strcat(tmp, ".0");
      if (arg && tmp[0] == '-') { buf_str(b, "("); buf_str(b, tmp); buf_str(b, ")"); }
      else buf_str(b, tmp);
      return;
    }
    case K_STR: buf_json_string(b, v); return;
    case K_ARR: {
      Arr *r = (Arr *)v;
      buf_str(b, "[");
      for (int64_t i = 0; i < r->n; i++) { if (i) buf_str(b, ", "); heff_show(b, r->a[i], 0); }
      buf_str(b, "]");
      return;
    }
    case K_REC: {
      Rec *r = (Rec *)v;
      buf_str(b, "{ ");
      for (int64_t i = 0; i < r->n; i++) {
        if (i) buf_str(b, ", ");
        buf_str(b, heff_lbl_names[r->lbl[i]]);
        buf_str(b, " = ");
        heff_show(b, r->f[i], 0);
      }
      buf_str(b, " }");
      return;
    }
    default: buf_str(b, "#fun"); return;
    }
  }
  const char *name = heff_tag_names[tag];
  int upper = name[0] >= 'A' && name[0] <= 'Z';
  if (n && arg) buf_str(b, "(");
  if (!upper) buf_str(b, "'");
  buf_str(b, name);
  for (int i = 0; i < n; i++) { buf_str(b, " "); heff_show(b, fs[i], 1); }
  if (n && arg) buf_str(b, ")");
}

// (a definition by its name, the file's own, `heff_globals`; its value
// asked for, the thing it is shown, or why it failed)
typedef struct { const char *name; V (*get)(void); } Global;

static void heff_check_one(Buf *out, const Global *g) {
  jmp_buf here;
  heff_catch = &here;
  Buf b = {0};
  buf_str(out, "\"");
  buf_str(out, g->name);
  buf_str(out, "\": ");
  if (setjmp(here) == 0) {
    V v = heff_tr(g->get());
    heff_show(&b, v, 0);
    buf_str(out, "{\"value\": ");
    Str *s = (Str *)heff_str_utf8((const unsigned char *)b.s, (int64_t)b.n);
    buf_json_string(out, (V)s);
    buf_str(out, "}");
  } else {
    buf_str(out, "{\"threw\": ");
    buf_json_string(out, heff_str_ascii(heff_caught));
    buf_str(out, "}");
  }
  heff_catch = NULL;
}

static int heff_argc;
static char **heff_argv;
static int heff_status = 0;

#ifdef HEFF_CHECK
static const Global *heff_globals = NULL;
static int heff_nglobals = 0;

// (the program's: its definitions and the names of its tags and labels)
static void heff_check_init(void);

static void *heff_thread(void *u) {
  (void)u;
  heff_check_init();
  Buf out = {0};
  buf_str(&out, "{");
  int first = 1;
  for (int i = 1; i < heff_argc; i++)
    for (int j = 0; j < heff_nglobals; j++)
      if (strcmp(heff_argv[i], heff_globals[j].name) == 0) {
        if (!first) buf_str(&out, ", ");
        first = 0;
        heff_check_one(&out, &heff_globals[j]);
        break;
      }
  buf_str(&out, "}");
  fputs(out.s, stdout);
  return NULL;
}
#else
static V heff_entry(void);

static void *heff_thread(void *u) {
  (void)u;
  heff_tr(heff_entry());
  return NULL;
}
#endif

int main(int argc, char **argv) {
  heff_argc = argc; heff_argv = argv;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, (size_t)1 << 30);
  pthread_t t;
  if (pthread_create(&t, &attr, heff_thread, NULL) != 0) heff_fail("cannot start");
  pthread_join(t, NULL);
  fflush(stdout);
  return heff_status;
}
