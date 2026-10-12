// heff's runtime for the C backend (src/heff/compile/emitc.hf), bundled whole
// into each program it compiles, before the program's own code and its
// worlds' providers (`M.provider.c` beside `M.hf`).
//
// A value is a word.  Its low bit set, it is an integer (`n << 1 | 1`,
// 62 bits and a sign); its low bits `10`, a tag with no fields (`id << 2
// | 2`); otherwise it points to an object, whose header says what it
// is.  Tags are numbered (`TAG_x`), the ones the runtime knows first
// (below), the program's after them, each by a hash of its name; record
// labels too (`LBL_x`), a record's fields in the order it was written
// with them.  A string is bytes, `len` of them (no NUL after them, and
// any byte may be in them), UTF-8 as the program wrote it -- its runes
// read as `heff_u8_decode` reads them, what is not UTF-8 U+FFFD -- as
// the other backends have it; a provider gives one to the system, and
// makes one of what the system gives, only through the runtime's
// helpers ("What providers share" says which).  `Bytes` (lib/bytes.hf)
// are kept as a string is, a `Str` of another kind (`K_BYT`), and cross
// by helpers of their own.
//
// An object is counted (Perceus): each owner of it holds a reference,
// `heff_dup` takes one more and `heff_drop` gives one back, the last
// freeing it.  What a function is given it owns: it consumes it (gives
// it to what it calls, keeps it in what it builds) or drops it -- the
// compiler says where (src/heff/compile/rc.hf, "Reference counts"); every
// primitive and the runtime's calls consume their arguments too.  What
// is static (a string written in the program, a definition's closure) is
// immortal, its count never changed.  A program runs in a thread of its
// own with a large stack, as the Node.js backend's does.  A call last of an unknown
// function counts itself and every so often gives back a bounce (`K_BNC`),
// let go of where the value is asked for (`heff_tr`), as the JavaScript
// backend does (src/heff/compile/emit.hf, "Tail calls") -- unless it is a jump
// (`HEFF_LAST`), from a lambda to a closure of as many parameters, its
// arguments put where the lambda's were or, more than those, in
// `heff_room`.  A call not last of a closure of as many parameters is
// made where it is (`heff_call`), not through `heff_apply`.

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <alloca.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// (a float's operations each rounded, as JavaScript's are: none of them
// fused into another, `a * b + c` into an FMA -- the program's own,
// written as C's operators where its numbers are unboxed (#39))
#pragma STDC FP_CONTRACT OFF

// (the runtime's state: defined once, in the program's first file; a
// program split into parts (`HEFF_PART`, `heff build`) declares it in the
// others, each of which has its own copy of the runtime's functions)
#ifdef HEFF_PART
#define HEFF_VAR(decl) extern decl
#else
#define HEFF_VAR(decl) decl
#endif

// Threads (#36).  A program runs in one thread of its own, unless a
// provider runs it in more, truly at once: such a provider says so by
// `#define HEFF_THREADS 1`, a line of its own, which the emitter puts
// before the runtime too (`-DHEFF_THREADS` does the same).  Then each
// thread has the runtime's per-run state for itself (`HEFF_TLS`: its
// free lists, its stack of what is given back, its count of calls and
// its room, where a failure of it is caught), and a mortal object is
// only ever one thread's, its count changed by that thread alone --
// counts are not atomic.  What is immortal, static or made so
// (`heff_freeze`), is shared: a definition's value is computed once,
// under a lock, and made immortal (`heff_global`), as is a static
// thunk's value (`heff_force_shared`).  What a provider gives another
// thread it copies for it (`heff_copy_for_thread`), and runs there
// under `heff_try`, ending the thread by `heff_thread_end`.  Without
// `HEFF_THREADS`, all of this is as one thread has it: `HEFF_TLS` is
// nothing, and nothing is locked.  (docs/threads.md)
#ifdef HEFF_THREADS
#define HEFF_TLS _Thread_local
#else
#define HEFF_TLS
#endif

typedef uintptr_t V;

// (a call last whose callee takes what its caller takes -- the same
// parameters, the same result -- made a jump, the caller's frame let go
// of: clang's `musttail`, on the processors it always does it for
// (`HEFF_JUMPS`).  Elsewhere, or with `-DHEFF_NO_JUMPS`, a call, which
// a C compiler may still make a jump: a call of what is not known then
// counted, as any other (`heff_exact`))
#if defined(__clang__) && (defined(__x86_64__) || defined(__aarch64__)) && !defined(HEFF_NO_JUMPS)
#define HEFF_LAST __attribute__((musttail))
#define HEFF_JUMPS 1
#else
#define HEFF_LAST
#define HEFF_JUMPS 0
#endif

// (what is done often and is short, an integer's operation on words,
// inlined where it is used, at any optimisation; what is done seldom
// and is long, its operation on big ones, a call, out of the way, so
// that what is inlined stays short)
#define HEFF_FAST static inline __attribute__((always_inline))
#define HEFF_SLOW static __attribute__((noinline, cold))
#define HEFF_LIKELY(c) __builtin_expect(!!(c), 1)

// the tags the runtime knows; a program's own are numbered after them
enum {
  TAG_True, TAG_False, TAG_Nil, TAG_Cons, TAG_MkUnit, TAG_Just, TAG_Nothing,
  TAG_MkPair, TAG_InL, TAG_InR, TAG_KNOWN
};

enum { K_TAG, K_REC, K_CLO, K_PAP, K_STR, K_FLT, K_ARR, K_BNC, K_CELL, K_BIG, K_BYT };

typedef struct Obj {
  uint32_t rc;
  uint16_t kind;
  uint16_t tag;
} Obj;

typedef V (*Fn)(V self, V *args);

typedef struct { Obj h; int64_t n; V f[]; } Tag;
// (a record: its fields, `n` of them, their labels `lbl`; `flat`, which
// of its first 16 fields are flat (#39) -- not a `V` but a number's 64
// bits, a `Float64`'s, an `Int64`'s or a `UInt64`'s: 2 bits each, as
// `HEFF_FLAT_F64` and the others say, 0 a `V`)
typedef struct { Obj h; int32_t n; uint32_t flat; uint32_t *lbl; V f[]; } Rec;
// (a closure: its code, how many arguments it takes, and what it holds;
// `memo`, once a thunk -- a function of one argument -- is forced, its
// value, or 0)
typedef struct { Obj h; Fn fn; int32_t arity; int32_t nenv; V memo; V env[]; } Clo;
typedef struct { Obj h; V fun; int32_t n; V a[]; } Pap;
// (a string, or `Bytes` (`K_BYT`): its `len` bytes, room for `cap`)
typedef struct { Obj h; int64_t len; int64_t cap; uint8_t s[]; } Str;
// (a string written in the program, static: its `n` bytes, as a C string
// literal has them -- the NUL C ends it with, after them, no byte of it)
#define HEFF_STATIC_STR(name, kind, n, bytes) \
  static struct { Obj h; int64_t len; int64_t cap; uint8_t s[(n) + 1]; } name = {{0x80000000u, kind, 0}, n, n, bytes}
#define HEFF_LIT(name, n, bytes) HEFF_STATIC_STR(name, K_STR, n, bytes)
// (`Bytes` written in the program, `b"..."`, static as a string is)
#define HEFF_BYTES(name, n, bytes) HEFF_STATIC_STR(name, K_BYT, n, bytes)
typedef struct { Obj h; double d; } Flt;
typedef struct { Obj h; int64_t n; V a[]; } Arr;
typedef struct { Obj h; V fun; int32_t n; V a[]; } Bnc;
typedef struct { Obj h; int32_t neg; int32_t n; uint32_t d[]; } Big;

#define IS_INT(v) (((v) & 1) == 1)
#define INT_OF(v) ((int64_t)(v) >> 1)
#define MK_INT(n) ((V)((((uint64_t)(int64_t)(n)) << 1) | 1))
#define IS_NTAG(v) (((v) & 3) == 2)
#define NTAG(id) ((V)(((uintptr_t)(id) << 2) | 2))
#define OBJ(v) ((Obj *)(v))
// (a word's integers: from -INT_LIM up to, not with, INT_LIM)
#define INT_LIM ((int64_t)1 << 62)
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
HEFF_VAR(HEFF_TLS jmp_buf *heff_catch);
HEFF_VAR(HEFF_TLS const char *heff_caught);

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
// computation's continuations, its operations, a list's cells): those of
// up to `SMALL_MAX` bytes are kept, for the next of their size, each
// multiple of 8 bytes apart.  (`-DSMALL_MAX=0`: none kept, every object
// given back to the system, so that AddressSanitizer sees a use after
// its last reference)
#ifndef SMALL_MAX
#define SMALL_MAX 128
#endif

#if defined(HEFF_THREADS) || SMALL_MAX == 0
// (with threads, an object one thread made may be given back by another
// (`heff_copy_for_thread`): each its own, from the system, kept on a list
// of its size of the thread that gives it back)
HEFF_VAR(HEFF_TLS void *heff_free_list[SMALL_MAX / 8 + 1]);

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
#else
// (in pages, each of one size, its own list of what is given back: an
// object is made in the page made in last, what it was given back first,
// else in what of it was never used yet, else in another page that has
// room -- so what is made one after another is near one another, however
// long the program has run, and a list made a cell at a time from the
// front is read from the front by addresses that go up, a page at a
// time.  One list of what is given back for each size, not each page,
// would hand cells out in the order they came back -- after a list is
// filtered, every third far from the one before it -- and a long list
// read through the cache misses at each cell)
#define HEFF_PAGE ((size_t)1 << 16)
typedef struct HeffPage {
  void *free;               // what was given back here
  char *bump, *end;         // what was never used yet
  struct HeffPage *next;    // the next of its size with room
  uint32_t c, listed;       // its size, in words; on that list
} HeffPage;
HEFF_VAR(HeffPage *heff_page_at[SMALL_MAX / 8 + 1]);
HEFF_VAR(HeffPage *heff_page_room[SMALL_MAX / 8 + 1]);

static void *heff_page_alloc(size_t c);

static inline void *heff_obj_alloc(size_t n) {
  size_t c = (n + 7) / 8;
  if (c > SMALL_MAX / 8) return heff_alloc(c * 8);
  HeffPage *pg = heff_page_at[c];
  if (pg) {
    void *p = pg->free;
    if (p) { pg->free = *(void **)p; return p; }
    if (pg->bump + c * 8 <= pg->end) { p = pg->bump; pg->bump += c * 8; return p; }
  }
  return heff_page_alloc(c);
}

static inline void heff_obj_free(void *p, size_t n) {
  size_t c = (n + 7) / 8;
  if (c > SMALL_MAX / 8) { free(p); return; }
  HeffPage *pg = (HeffPage *)((uintptr_t)p & ~(uintptr_t)(HEFF_PAGE - 1));
  *(void **)p = pg->free;
  pg->free = p;
  if (!pg->listed && pg != heff_page_at[pg->c]) {
    pg->listed = 1;
    pg->next = heff_page_room[pg->c];
    heff_page_room[pg->c] = pg;
  }
}
#endif

#if !(defined(HEFF_THREADS) || SMALL_MAX == 0)
// (the page made in last full: another of the size with room, else a
// new one)
static __attribute__((noinline)) void *heff_page_alloc(size_t c) {
  HeffPage *pg = heff_page_room[c];
  if (pg) {
    heff_page_room[c] = pg->next;
    pg->listed = 0;
  } else {
    void *m = NULL;
#ifdef _WIN32
    m = _aligned_malloc(HEFF_PAGE, HEFF_PAGE);
#else
    if (posix_memalign(&m, HEFF_PAGE, HEFF_PAGE) != 0) m = NULL;
#endif
    if (!m) heff_fail("out of memory");
    pg = m;
    pg->free = NULL;
    pg->bump = (char *)m + (sizeof(HeffPage) + 15) / 16 * 16;
    pg->end = (char *)m + HEFF_PAGE;
    pg->next = NULL;
    pg->c = (uint32_t)c;
    pg->listed = 0;
  }
  heff_page_at[c] = pg;
  void *p = pg->free;
  if (p) { pg->free = *(void **)p; return p; }
  p = pg->bump;
  pg->bump += c * 8;
  return p;
}
#endif

static inline int heff_tag_of(V v) {
  return IS_NTAG(v) ? (int)(v >> 2) : OBJ(v)->tag;
}

// (C's condition as a Bool)
#define heff_bool(b) ((b) ? NTAG(TAG_True) : NTAG(TAG_False))

static V heff_tag(int id, int64_t n, V *f) {
  if (n == 0) return NTAG(id);
  Tag *t = heff_obj_alloc(sizeof(Tag) + n * sizeof(V));
  t->h.rc = 1; t->h.kind = K_TAG; t->h.tag = (uint16_t)id; t->n = n;
  memcpy(t->f, f, n * sizeof(V));
  return (V)t;
}

static inline V heff_field(V v, int i) { return ((Tag *)v)->f[i]; }

// Flat fields (#39): a record's field of a type of 64 bits a `V` would
// allocate for -- `Float64`, `Int64`, `UInt64` -- is kept as its bits
// where the code building the record knows the type
// (src/heff/compile/repr.hf, `flatOf`), and the record says so, 2 bits a
// field (`Rec`'s `flat`), so that what walks it -- a drop, a copy, a
// field looked for by its label, a value shown -- does not take the bits
// for a `V`, and a read where the type is not known boxes them.  Only the
// first `HEFF_FLAT_FIELDS` fields may be flat.
enum { HEFF_FLAT_V, HEFF_FLAT_F64, HEFF_FLAT_I64, HEFF_FLAT_U64 };
#define HEFF_FLAT_FIELDS 16
// (what field `i` is, of the bits `flat`)
#define heff_flat_kind(flat, i) \
  ((i) < HEFF_FLAT_FIELDS ? (unsigned)(((flat) >> (2 * (i))) & 3) : 0u)

// (of the first `n` fields, the low bit of each's 2: `n` a constant where
// it is asked, so is this)
static inline uint32_t heff_flat_lows(int64_t n) {
  return n >= HEFF_FLAT_FIELDS ? 0x55555555u
                               : (uint32_t)(((uint64_t)1 << (2 * n)) - 1) & 0x55555555u;
}
// (whether each of the fields `lows` says is flat)
#define heff_flat_all(flat, lows) ((((flat) | ((flat) >> 1)) & (lows)) == (lows))

// (a flat field's bits, of the kind `k`, as a `V`: (below))
static V heff_flat_box(V w, unsigned k);

// (the labels, in the order the record was written, are the program's:
// one array for each)
static V heff_rec(int64_t n, uint32_t *lbl, V *f) {
  Rec *r = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  r->h.rc = 1; r->h.kind = K_REC; r->h.tag = 0; r->n = (int32_t)n; r->flat = 0; r->lbl = lbl;
  memcpy(r->f, f, n * sizeof(V));
  return (V)r;
}

// (field `i` as a `V` counted, a flat one boxed)
static inline V heff_rec_dup_field(Rec *r, int64_t i) {
  unsigned k = heff_flat_kind(r->flat, i);
  return k ? heff_flat_box(r->f[i], k) : heff_dup(r->f[i]);
}

// (a field, the record consumed)
static V heff_get(V v, uint32_t l) {
  Rec *r = (Rec *)v;
  for (int64_t i = 0; i < r->n; i++)
    if (r->lbl[i] == l) {
      V x = heff_rec_dup_field(r, i);
      heff_drop(v);
      return x;
    }
  heff_fail("no such field");
  return 0;
}

// (a match's scrutinee given up on an arm, which takes the fields in
// `keep` (a bit each): its last reference, its shell freed and the
// other fields dropped; otherwise each kept field counted)
static inline void heff_take(V v, uint64_t keep) {
  if (!IS_OBJ(v)) return;
  Tag *t = (Tag *)v;
  if (t->h.rc == 1) {
    for (int64_t i = 0; i < t->n; i++)
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + t->n * sizeof(V));
  } else {
    for (int64_t i = 0; i < t->n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    if (!(t->h.rc & IMMORTAL)) t->h.rc--;
  }
}

// (as `heff_take`, the cell kept for a tag of its size to be built in
// (`heff_tag_at`): its last reference, the cell itself, its fields
// taken or dropped and blanked; otherwise none, 0)
static inline V heff_take_reuse(V v, uint64_t keep) {
  Tag *t = (Tag *)v;
  if (t->h.rc == 1) {
    for (int64_t i = 0; i < t->n; i++) {
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
      t->f[i] = MK_INT(0);
    }
    return v;
  }
  for (int64_t i = 0; i < t->n; i++)
    if ((keep >> i) & 1) heff_dup(t->f[i]);
  if (!(t->h.rc & IMMORTAL)) t->h.rc--;
  return MK_INT(0);
}

// Drops specialised (#26): where the emitter knows what a value is -- a
// tag of `n` fields, the arm it is in matching it -- it gives it back
// as that, not through `heff_drop`, nor `heff_take`'s loops over what
// the cell says.  `n` and `keep` are constants where these are called,
// so each is compiled to the few steps that tag needs: its loops
// unrolled, its size class known.  (`heff_take` and `heff_take_reuse`
// stay for a compiler from before, building this runtime's compiler.)
// (Not optimised, `-O0`, each is a call: inlined there, its every step a
// slot of its caller's frame, a recursion a million deep -- a list built
// by a call that is not last -- would overflow the stack.)
#ifdef __OPTIMIZE__
#define HEFF_KNOWN static inline __attribute__((always_inline))
#else
#define HEFF_KNOWN static inline
#endif

// (`heff_take`, of a tag of `n` fields)
HEFF_KNOWN void heff_take_n(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (t->h.rc == 1) {
    for (int64_t i = 0; i < n; i++)
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else {
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    if (!(t->h.rc & IMMORTAL)) t->h.rc--;
  }
}

// (`heff_take_reuse`, of a tag of `n` fields: its fields not blanked, the
// cell kept being either built in (`heff_tag_at`, each field written) or
// freed as the cell it is (`heff_drop_cell`), never looked into)
HEFF_KNOWN V heff_take_reuse_n(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (t->h.rc == 1) {
    for (int64_t i = 0; i < n; i++)
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
    return v;
  }
  for (int64_t i = 0; i < n; i++)
    if ((keep >> i) & 1) heff_dup(t->f[i]);
  if (!(t->h.rc & IMMORTAL)) t->h.rc--;
  return MK_INT(0);
}

// (a tag of `n` fields dropped: its last reference, its fields dropped
// and its cell freed at once; otherwise counted down)
HEFF_KNOWN void heff_drop_tag(V v, int64_t n) {
  Tag *t = (Tag *)v;
  if (t->h.rc == 1) {
    for (int64_t i = 0; i < n; i++) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else if (!(t->h.rc & IMMORTAL)) t->h.rc--;
}

// (a cell kept for a tag of `n` fields (`heff_take_reuse_n`) and not
// built in: freed, what was in it given back already; none kept,
// nothing)
HEFF_KNOWN void heff_drop_cell(V r, int64_t n) {
  if (IS_OBJ(r)) heff_obj_free((void *)r, sizeof(Tag) + n * sizeof(V));
}

// (a tag built in the cell `r` kept for it, or a new one)
static inline V heff_tag_at(V r, int id, int64_t n, V *f) {
  if (!IS_OBJ(r)) return heff_tag(id, n, f);
  Tag *t = (Tag *)r;
  t->h.tag = (uint16_t)id;
  memcpy(t->f, f, n * sizeof(V));
  return r;
}

static V heff_clo(Fn fn, int arity, int nenv, V *env) {
  Clo *c = heff_obj_alloc(sizeof(Clo) + nenv * sizeof(V));
  c->h.rc = 1; c->h.kind = K_CLO; c->h.tag = 0;
  c->fn = fn; c->arity = arity; c->nenv = nenv; c->memo = 0;
  if (nenv) memcpy(c->env, env, nenv * sizeof(V));
  return (V)c;
}

static inline V *heff_env(V self) { return ((Clo *)self)->env; }

// Constructions at their known size (#34): where the emitter builds a
// tag, a record or a closure, it knows how many fields it has, a
// constant where these are called -- so each is compiled to its size
// class's free list taken from and its fields stored, not given to a
// function that copies them with `memcpy`.  (`heff_tag`, `heff_rec`,
// `heff_clo` stay for what does not know, and for a compiler from
// before.)

// (the fields at `f`, `n` of them, stored at `to`: `n` a constant, the
// loop unrolled, `f` a compound literal's, each stored where it is
// computed)
HEFF_KNOWN void heff_fields_n(V *to, V *f, int64_t n) {
  for (int64_t i = 0; i < n; i++) to[i] = f[i];
}

// (a tag of `n` fields, `n` more than none)
HEFF_KNOWN V heff_tag_n(int id, int64_t n, V *f) {
  Tag *t = heff_obj_alloc(sizeof(Tag) + n * sizeof(V));
  t->h.rc = 1; t->h.kind = K_TAG; t->h.tag = (uint16_t)id; t->n = n;
  heff_fields_n(t->f, f, n);
  return (V)t;
}

// (a tag of `n` fields built in the cell `r` kept for it, or a new one)
HEFF_KNOWN V heff_tag_at_n(V r, int id, int64_t n, V *f) {
  if (!IS_OBJ(r)) return heff_tag_n(id, n, f);
  Tag *t = (Tag *)r;
  t->h.tag = (uint16_t)id;
  heff_fields_n(t->f, f, n);
  return r;
}

// (what a tag's field waits as, a hole: no object, nothing to drop; and
// field `i` of the cell `d`, made with a hole there that nothing has
// seen yet, set to `v`: what a call of itself gives put where a tag waits
// for it -- src/heff/compile/trmc.hf's destination; `d` consumed)
static inline V heff_hole(void) { return MK_INT(0); }

static inline V heff_setField(V d, V i, V v) {
  ((Tag *)d)->f[INT_OF(i)] = v;
  heff_drop(d);
  return NTAG(TAG_MkUnit);
}

// (a record of `n` fields, its labels `lbl`)
HEFF_KNOWN V heff_rec_n(int64_t n, uint32_t *lbl, V *f) {
  Rec *r = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  r->h.rc = 1; r->h.kind = K_REC; r->h.tag = 0; r->n = (int32_t)n; r->flat = 0; r->lbl = lbl;
  heff_fields_n(r->f, f, n);
  return (V)r;
}

// (... some of its fields flat, as `flat` says: each given as its bits,
// `heff_word_f64` and the others)
HEFF_KNOWN V heff_rec_nf(int64_t n, uint32_t *lbl, uint32_t flat, V *f) {
  Rec *r = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  r->h.rc = 1; r->h.kind = K_REC; r->h.tag = 0; r->n = (int32_t)n; r->flat = flat; r->lbl = lbl;
  heff_fields_n(r->f, f, n);
  return (V)r;
}

// (a closure holding `n`, more than none)
HEFF_KNOWN V heff_clo_n(Fn fn, int arity, int64_t n, V *env) {
  Clo *c = heff_obj_alloc(sizeof(Clo) + n * sizeof(V));
  c->h.rc = 1; c->h.kind = K_CLO; c->h.tag = 0;
  c->fn = fn; c->arity = arity; c->nenv = (int32_t)n; c->memo = 0;
  heff_fields_n(c->env, env, n);
  return (V)c;
}

// Fields at their known place (#34): a record's fields are those of its
// type that are not types, in the order the type has them (its shape),
// so where the emitter knows the type -- src/heff/compile/erase.hf gives each
// projection its record's shape -- it reads a field at its place, not
// looking for its label (`heff_get`).

// (field `i`, the record not consumed)
#define heff_rfield(v, i) (((Rec *)(v))->f[i])

// (a record's fields but the `i`th dropped and its cell freed: for a
// record of many fields, a call, not as many drops where it is read)
#ifndef HEFF_FEW_FIELDS
#define HEFF_FEW_FIELDS 2
#endif
static __attribute__((noinline)) void heff_rec_give(V v, int64_t i) {
  Rec *r = (Rec *)v;
  for (int64_t j = 0; j < r->n; j++)
    if (j != i) heff_drop(r->f[j]);
  heff_obj_free(r, sizeof(Rec) + r->n * sizeof(V));
}

// (`heff_get_n` of a record with flat fields: the field as a `V`, the
// record dropped)
static __attribute__((noinline)) V heff_get_flat(V v, int64_t i) {
  V x = heff_rec_dup_field((Rec *)v, i);
  heff_drop(v);
  return x;
}

// (field `i` of a record of `n` fields, the record consumed: its last
// reference, the field taken over, the others dropped and its cell freed;
// otherwise the field counted)
HEFF_KNOWN V heff_get_n(V v, int64_t i, int64_t n) {
  Rec *r = (Rec *)v;
  if (r->flat) return heff_get_flat(v, i);
  V x = r->f[i];
  if (r->h.rc == 1) {
    if (n > HEFF_FEW_FIELDS) heff_rec_give(v, i);
    else {
      for (int64_t j = 0; j < n; j++)
        if (j != i) heff_drop(r->f[j]);
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    }
  } else {
    heff_dup(x);
    if (!(r->h.rc & IMMORTAL)) r->h.rc--;
  }
  return x;
}

// (field `i` of a record wanted after, which after it is only read other
// fields of, then updated or dropped (src/heff/compile/rc.hf, `takesFor`):
// its last reference, the field taken out of it -- moved, not counted, a
// hole left that drops as nothing; otherwise the field counted.  Never a
// field that may be flat: one that is is read where it is)
HEFF_KNOWN V heff_take_field(V v, int64_t i) {
  Rec *r = (Rec *)v;
  V x = r->f[i];
  if (r->h.rc == 1) r->f[i] = MK_INT(0);
  else heff_dup(x);
  return x;
}

// A record updated (#34): one built of the same shape as the record `v`
// it is built from, some of its fields read from `v`'s at their own
// places (`keep`, a bit each) and `v` given up there, is built in `v`'s
// cell where that was `v`'s last reference -- the fields kept left where
// they are, counted neither up nor down, the others dropped -- as a tag
// is in a cell kept for it (Perceus's reuse).  Shared, `v` is copied:
// the fields kept counted, `v` counted down.  Either way the fields not
// kept are stored by the caller, at once.
//
// Its flat fields (#39) are as `v`'s are where they are kept -- `kept`,
// the bits of those of them that may be flat, 3 each -- and as `flat`
// says where they are stored (`heff_rec_reuse_nf`).
static __attribute__((noinline)) V heff_rec_copy(V v, uint64_t keep, uint32_t *lbl, uint32_t kept, uint32_t flat) {
  Rec *r = (Rec *)v;
  int64_t n = r->n;
  uint32_t was = r->flat;
  Rec *c = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  c->h.rc = 1; c->h.kind = K_REC; c->h.tag = 0; c->n = (int32_t)n; c->lbl = lbl;
  c->flat = (was & kept) | flat;
  for (int64_t i = 0; i < n; i++)
    c->f[i] = !((keep >> i) & 1) ? MK_INT(0)
            : heff_flat_kind(was, i) ? r->f[i] : heff_dup(r->f[i]);
  if (!(r->h.rc & IMMORTAL)) r->h.rc--;
  return (V)c;
}

// (a record with flat fields, built in its cell: the fields not kept
// dropped, as they were)
static __attribute__((noinline)) void heff_rec_unset(Rec *r, uint64_t keep) {
  for (int64_t i = 0; i < r->n; i++)
    if (!((keep >> i) & 1) && !heff_flat_kind(r->flat, i)) heff_drop(r->f[i]);
}

HEFF_KNOWN V heff_rec_reuse_nf(V v, uint64_t keep, int64_t n, uint32_t *lbl, uint32_t kept, uint32_t flat) {
  Rec *r = (Rec *)v;
  if (r->h.rc != 1) {
    if (n > HEFF_FEW_FIELDS || r->flat) return heff_rec_copy(v, keep, lbl, kept, flat);
    Rec *c = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
    c->h.rc = 1; c->h.kind = K_REC; c->h.tag = 0; c->n = (int32_t)n; c->flat = flat; c->lbl = lbl;
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) c->f[i] = heff_dup(r->f[i]);
    if (!(r->h.rc & IMMORTAL)) r->h.rc--;
    return (V)c;
  }
  uint32_t was = r->flat;
  if (was) {
    // (none to drop where each field stored anew was flat: a number's
    // record updated with numbers)
    uint32_t anew = heff_flat_lows(n) & ~kept;
    if (n > HEFF_FLAT_FIELDS || !heff_flat_all(was, anew)) heff_rec_unset(r, keep);
    r->flat = (was & kept) | flat;
  } else {
    for (int64_t i = 0; i < n; i++)
      if (!((keep >> i) & 1)) heff_drop(r->f[i]);
    if (flat) r->flat = flat;
  }
  r->lbl = lbl;
  return v;
}

// (... of a compiler from before #39, which keeps no field flat)
static inline uint32_t heff_kept_bits(uint64_t keep) {
  uint32_t kept = 0;
  for (int i = 0; i < HEFF_FLAT_FIELDS; i++)
    if ((keep >> i) & 1) kept |= 3u << (2 * i);
  return kept;
}

HEFF_KNOWN V heff_rec_reuse_n(V v, uint64_t keep, int64_t n, uint32_t *lbl) {
  return heff_rec_reuse_nf(v, keep, n, lbl, heff_kept_bits(keep), 0);
}

// (a lambda's closure, holding `n`, given up where its body starts,
// which takes what it holds (#26): its last reference, its shell freed;
// otherwise each counted.  `n` is a constant where it is called)
static inline __attribute__((always_inline)) void heff_clo_take(V self, int64_t n) {
  Clo *c = (Clo *)self;
  if (c->h.rc == 1) {
    if (c->memo) heff_drop(c->memo);
    heff_obj_free(c, sizeof(Clo) + n * sizeof(V));
  } else {
    for (int64_t i = 0; i < n; i++) heff_dup(c->env[i]);
    if (!(c->h.rc & IMMORTAL)) c->h.rc--;
  }
}

// (an object's size, from what it is)
static size_t heff_size(V v) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: return sizeof(Tag) + ((Tag *)v)->n * sizeof(V);
  case K_REC: return sizeof(Rec) + ((Rec *)v)->n * sizeof(V);
  case K_CLO: return sizeof(Clo) + ((Clo *)v)->nenv * sizeof(V);
  case K_PAP: return sizeof(Pap) + ((Pap *)v)->n * sizeof(V);
  case K_STR: case K_BYT: return sizeof(Str) + ((Str *)v)->cap;
  case K_FLT: return sizeof(Flt);
  case K_ARR: return sizeof(Arr) + ((Arr *)v)->n * sizeof(V);
  case K_BNC: return sizeof(Bnc) + ((Bnc *)v)->n * sizeof(V);
  case K_BIG: return sizeof(Big) + ((Big *)v)->n * sizeof(uint32_t);
  default: return SMALL_MAX + 1;
  }
}

// (a record whose last reference is gone, its flat fields (#39) made
// words that drop as nothing, as its other fields are given back)
static __attribute__((noinline)) void heff_flat_clear(Rec *r) {
  for (int64_t i = 0; i < r->n && i < HEFF_FLAT_FIELDS; i++)
    if (heff_flat_kind(r->flat, i)) r->f[i] = MK_INT(0);
}

// (what is given back, a stack of it, not a recursion: a long list's
// cells given back one after another, the stack as deep as the list
// never)
HEFF_VAR(HEFF_TLS V *heff_dead);
HEFF_VAR(HEFF_TLS size_t heff_ndead);
HEFF_VAR(HEFF_TLS size_t heff_cdead);

// (a value whose last reference this was, put on the stack)
static inline void heff_dead_put(V v) {
  if (heff_ndead == heff_cdead) {
    heff_cdead = heff_cdead ? heff_cdead * 2 : 1024;
    heff_dead = realloc(heff_dead, heff_cdead * sizeof(V));
    if (!heff_dead) heff_fail("out of memory");
  }
  heff_dead[heff_ndead++] = v;
}

// (a value counted down: put on the stack if that was its last reference)
static inline void heff_dead_push(V v) {
  if (!IS_OBJ(v) || (OBJ(v)->rc & IMMORTAL) || --OBJ(v)->rc != 0) return;
  heff_dead_put(v);
}

// (an object whose last reference is gone: what it holds counted down,
// its size reckoned as it is looked at, and it freed; of what it holds,
// each whose last reference that was is given back after it -- the last
// of them next, at once, the others from the stack: a list's cells one
// after another, its spine never on the stack)
__attribute__((noinline)) static void heff_free(V v) {
  size_t base = heff_ndead;
  for (;;) {
    Obj *o = OBJ(v);
    V *f = NULL, next = 0;
    int64_t n = 0;
    size_t size;
    switch (o->kind) {
    case K_TAG: { Tag *t = (Tag *)v; n = t->n; f = t->f; size = sizeof(Tag) + n * sizeof(V); break; }
    case K_REC: {
      Rec *r = (Rec *)v; n = r->n; f = r->f; size = sizeof(Rec) + n * sizeof(V);
      if (r->flat) heff_flat_clear(r);
      break;
    }
    case K_CLO: {
      Clo *c = (Clo *)v; n = c->nenv; f = c->env; size = sizeof(Clo) + n * sizeof(V);
      if (c->memo) heff_dead_push(c->memo);
      break;
    }
    case K_PAP: { Pap *p = (Pap *)v; heff_dead_push(p->fun); n = p->n; f = p->a; size = sizeof(Pap) + n * sizeof(V); break; }
    case K_ARR: { Arr *r = (Arr *)v; n = r->n; f = r->a; size = sizeof(Arr) + n * sizeof(V); break; }
    case K_BNC: { Bnc *b = (Bnc *)v; heff_dead_push(b->fun); n = b->n; f = b->a; size = sizeof(Bnc) + n * sizeof(V); break; }
    default: size = heff_size(v); break;
    }
    for (int64_t i = 0; i < n; i++) {
      V x = f[i];
      if (!IS_OBJ(x) || (OBJ(x)->rc & IMMORTAL) || --OBJ(x)->rc != 0) continue;
      if (next) heff_dead_put(next);
      next = x;
    }
    heff_obj_free(o, size);
    if (next) v = next;
    else if (heff_ndead > base) v = heff_dead[--heff_ndead];
    else return;
  }
}

// (a record of `n` fields dropped, its shape known (#34): its last
// reference, its fields dropped and its cell freed at once -- of many
// fields, by the walk; otherwise counted down)
HEFF_KNOWN void heff_drop_rec(V v, int64_t n) {
  Rec *r = (Rec *)v;
  if (r->h.rc == 1) {
    // (a record of numbers, all flat: its cell, nothing in it)
    if (r->flat && n <= HEFF_FLAT_FIELDS && heff_flat_all(r->flat, heff_flat_lows(n)))
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    else if (n > HEFF_FEW_FIELDS || r->flat) heff_free(v);
    else {
      for (int64_t i = 0; i < n; i++) heff_drop(r->f[i]);
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    }
  } else if (!(r->h.rc & IMMORTAL)) r->h.rc--;
}

// (a closure holding `n` dropped, as `heff_drop_rec` a record)
HEFF_KNOWN void heff_drop_clo(V v, int64_t n) {
  Clo *c = (Clo *)v;
  if (c->h.rc == 1) {
    if (n > HEFF_FEW_FIELDS) heff_free(v);
    else {
      if (c->memo) heff_drop(c->memo);
      for (int64_t i = 0; i < n; i++) heff_drop(c->env[i]);
      heff_obj_free(c, sizeof(Clo) + n * sizeof(V));
    }
  } else if (!(c->h.rc & IMMORTAL)) c->h.rc--;
}

// Values between threads (#36)
// ---------------------------------------------------------------------

// (a tag's or a record's fields, an array's elements: `*n` of them --
// a closure's, a partial application's and a bounce's are said by
// `HEFF_EACH_SLOT`; a string, a float and a big integer hold none)
static V *heff_slots(V v, int64_t *n) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: *n = ((Tag *)v)->n; return ((Tag *)v)->f;
  case K_REC: *n = ((Rec *)v)->n; return ((Rec *)v)->f;
  case K_ARR: *n = ((Arr *)v)->n; return ((Arr *)v)->a;
  default: *n = 0; return NULL;
  }
}

// (a stack of objects, for a walk that is not a recursion: a long
// list's spine never on the C stack)
typedef struct { V *v; size_t n, cap; } HeffStack;

static void heff_stack_push(HeffStack *s, V v) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->v = realloc(s->v, s->cap * sizeof(V));
    if (!s->v) heff_fail("out of memory");
  }
  s->v[s->n++] = v;
}

// (each place of the object `v` holds a value in, given to `f`)
// (a record's flat fields (#39) none)
#define HEFF_EACH_SLOT(v, f, u)                                         \
  do {                                                                  \
    Obj *o_ = OBJ(v);                                                   \
    int64_t n_;                                                         \
    V *s_ = heff_slots(v, &n_);                                         \
    uint32_t fl_ = o_->kind == K_REC ? ((Rec *)(v))->flat : 0;          \
    for (int64_t i_ = 0; i_ < n_; i_++)                                 \
      if (!heff_flat_kind(fl_, i_)) f(u, &s_[i_]);                      \
    if (o_->kind == K_CLO) {                                            \
      Clo *c_ = (Clo *)(v);                                             \
      for (int32_t i_ = 0; i_ < c_->nenv; i_++) f(u, &c_->env[i_]);     \
      if (c_->memo) f(u, &c_->memo);                                    \
    } else if (o_->kind == K_PAP) {                                     \
      Pap *p_ = (Pap *)(v);                                             \
      for (int32_t i_ = 0; i_ < p_->n; i_++) f(u, &p_->a[i_]);          \
      f(u, &p_->fun);                                                   \
    } else if (o_->kind == K_BNC) {                                     \
      Bnc *b_ = (Bnc *)(v);                                             \
      for (int32_t i_ = 0; i_ < b_->n; i_++) f(u, &b_->a[i_]);          \
      f(u, &b_->fun);                                                   \
    }                                                                   \
  } while (0)

static void heff_freeze_one(HeffStack *s, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v || (OBJ(v)->rc & IMMORTAL)) return;
  OBJ(v)->rc = IMMORTAL;
  heff_stack_push(s, v);
}

// (`v` made immortal, and all it holds: shared between threads from now
// on, each counting it never, and never freed -- what holds it now
// leaks it.  `v` is this thread's, and no other's yet)
static void heff_freeze(V v) {
  HeffStack s = {0};
  heff_freeze_one(&s, &v);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_freeze_one, &s);
  }
  free(s.v);
}

// (what a copy has made: each object copied, by the object it is a copy
// of -- open addressing, by address)
typedef struct { V *from, *to; size_t cap, n; HeffStack todo; } HeffCopies;

static size_t heff_addr_hash(V v) {
  uint64_t x = (uint64_t)v;
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
  return (size_t)x;
}

static void heff_copies_put(HeffCopies *m, V from, V to);

static void heff_copies_grow(HeffCopies *m) {
  V *from = m->from, *to = m->to;
  size_t cap = m->cap;
  m->cap = cap ? cap * 2 : 64;
  m->n = 0;
  m->from = calloc(m->cap, sizeof(V));
  m->to = calloc(m->cap, sizeof(V));
  if (!m->from || !m->to) heff_fail("out of memory");
  for (size_t i = 0; i < cap; i++) if (from[i]) heff_copies_put(m, from[i], to[i]);
  free(from); free(to);
}

static void heff_copies_put(HeffCopies *m, V from, V to) {
  if (2 * (m->n + 1) > m->cap) heff_copies_grow(m);
  size_t i = heff_addr_hash(from) & (m->cap - 1);
  while (m->from[i]) i = (i + 1) & (m->cap - 1);
  m->from[i] = from; m->to[i] = to; m->n++;
}

static V heff_copies_get(HeffCopies *m, V from) {
  if (!m->cap) return 0;
  size_t i = heff_addr_hash(from) & (m->cap - 1);
  for (; m->from[i]; i = (i + 1) & (m->cap - 1))
    if (m->from[i] == from) return m->to[i];
  return 0;
}

// (the value at `at` made its copy's: what is immortal as it is, an
// object copied before that copy counted once more, any other copied --
// its shell, what it holds copied after (`todo`))
static void heff_copy_one(HeffCopies *m, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v || (OBJ(v)->rc & IMMORTAL)) return;
  V done = heff_copies_get(m, v);
  if (done) { OBJ(done)->rc++; *at = done; return; }
  size_t sz = heff_size(v);
  if (sz == SMALL_MAX + 1) heff_fail("an object of a kind not known, copied for a thread");
  Obj *o = heff_obj_alloc(sz);
  memcpy(o, OBJ(v), sz);
  o->rc = 1;
  heff_copies_put(m, v, (V)o);
  heff_stack_push(&m->todo, (V)o);
  *at = (V)o;
}

// (a copy of `v` for another thread, `v` not consumed: what is immortal
// shared, every other object made again, what is shared in `v` shared in
// the copy -- so that no object is counted by two threads.  Made of this
// thread's memory, it is the other's from then on, and given back to
// its free lists.  `v` is this thread's: no other changes it meanwhile)
static V heff_copy_for_thread(V v) {
  HeffCopies m = {0};
  heff_copy_one(&m, &v);
  while (m.todo.n) {
    V o = m.todo.v[--m.todo.n];
    HEFF_EACH_SLOT(o, heff_copy_one, &m);
  }
  free(m.from); free(m.to); free(m.todo.v);
  return v;
}

// Calls of what is not known
// ---------------------------------------------------------------------

HEFF_VAR(HEFF_TLS long heff_dep);

// (where a lambda's call last of more arguments than it took is put, up
// to `HEFF_ROOM` of them, to be made a jump: its own are where its
// caller put them, room for only as many as it takes -- and what is
// called reads them at once, before it calls anything -- see `roomed`
// in src/heff/compile/emitc.hf)
#define HEFF_ROOM 4
HEFF_VAR(HEFF_TLS V heff_room[HEFF_ROOM]);

static V heff_apply(V f, int n, V *a);

// (a call's `n` arguments copied, `n` known only where it is run and
// most often a few: stored one by one, not given to `memcpy`, a call that
// costs more than such a copy)
static inline void heff_copy(V *to, const V *from, int64_t n) {
  switch (n) {
  case 4: to[3] = from[3]; /* fall through */
  case 3: to[2] = from[2]; /* fall through */
  case 2: to[1] = from[1]; /* fall through */
  case 1: to[0] = from[0]; /* fall through */
  case 0: return;
  default: memcpy(to, from, n * sizeof(V));
  }
}

// (the bounces run until a value comes: each a chain of calls last
// counted from naught -- and, done, the count of the chain it was called
// in put back, so that a chain calling one in each of its steps still
// comes to its bounce, not ever deeper on the C stack.  A bounce's
// arguments are in an array of the loop's step, not `alloca`'s, which
// would keep each step's until the end.)
static V heff_trs(V r) {
  long outer = heff_dep;
  do {
    Bnc *b = (Bnc *)r;
    heff_dep = 0;
    V fun = b->fun;
    int n = b->n;
    V a[n > 0 ? n : 1];
    heff_copy(a, b->a, n);
    heff_obj_free(b, sizeof(Bnc) + n * sizeof(V));
    r = heff_apply(fun, n, a);
  } while (!IS_INT(r) && !IS_NTAG(r) && OBJ(r)->kind == K_BNC);
  heff_dep = outer;
  return r;
}

static inline V heff_tr(V r) {
  return (!IS_INT(r) && !IS_NTAG(r) && OBJ(r)->kind == K_BNC) ? heff_trs(r) : r;
}

static V heff_bounce(V f, int n, V *a) {
  Bnc *b = heff_obj_alloc(sizeof(Bnc) + n * sizeof(V));
  b->h.rc = 1; b->h.kind = K_BNC; b->h.tag = 0; b->fun = f; b->n = n;
  heff_copy(b->a, a, n);
  return (V)b;
}

// (a call last: counted, and every so often a bounce)
static inline V heff_tail(V f, int n, V *a) {
  if (++heff_dep > 500) return heff_bounce(f, n, a);
  return heff_apply(f, n, a);
}

#ifdef HEFF_THREADS
// (an immortal thunk forced, threads at once: its memo shared by them
// all, so its value made immortal too, and kept by the first to have it
// -- another computing it meanwhile, the language pure, has the same
// value, and lets its own go, leaking it)
static __attribute__((noinline)) V heff_force_shared(V f, V *a) {
  Clo *c = (Clo *)f;
  V m = __atomic_load_n(&c->memo, __ATOMIC_ACQUIRE);
  if (m) return m;
  V r = heff_tr(c->fn(f, a));
  heff_freeze(r);
  V none = 0;
  if (__atomic_compare_exchange_n(&c->memo, &none, r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return r;
  return none;
}
#endif

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
        heff_copy(all, p->a, pn);
        fun = p->fun;
        heff_obj_free(p, sizeof(Pap) + pn * sizeof(V));
      } else {
        for (int i = 0; i < pn; i++) all[i] = heff_dup(p->a[i]);
        fun = heff_dup(p->fun);
        heff_drop(f);
      }
      heff_copy(all + pn, a, n);
      f = fun; a = all; n = m;
      continue;
    }
    if (o->kind != K_CLO) heff_fail("applying what is not a function");
    Clo *c = (Clo *)f;
    // (a thunk forced: its value kept on it, so that forcing it again does
    // not compute it again -- the language is pure, `f MkUnit` is the same
    // value every time -- as the JavaScript backend's `$force` keeps it)
    if (n == 1 && c->arity == 1 && a[0] == NTAG(TAG_MkUnit)) {
#ifdef HEFF_THREADS
      if (c->h.rc & IMMORTAL) return heff_force_shared(f, a);
#endif
      if (c->memo) {
        V r = heff_dup(c->memo);
        heff_drop(f);
        return r;
      }
      if (heff_unique(f)) return c->fn(f, a);
      heff_dup(f);
      V r = heff_tr(c->fn(f, a));
      c->memo = heff_dup(r);
      heff_drop(f);
      return r;
    }
    if (n == c->arity) return c->fn(f, a);
    if (n < c->arity) {
      Pap *p = heff_obj_alloc(sizeof(Pap) + n * sizeof(V));
      p->h.rc = 1; p->h.kind = K_PAP; p->h.tag = 0; p->fun = f; p->n = n;
      heff_copy(p->a, a, n);
      return (V)p;
    }
    // (more arguments than it takes: the rest given to what it gives --
    // its arity read first, the closure consumed by the call; the rest
    // kept apart first, as they may be in `heff_room`, which the call
    // may put others in)
    int ar = c->arity, rest = n - ar;
    V *more = alloca(rest * sizeof(V));
    heff_copy(more, a + ar, rest);
    V r = heff_tr(c->fn(f, a));
    f = r; a = more; n = rest;
  }
}

static inline V heff_app1(V f, V x) { return heff_apply(f, 1, &x); }

// (a call not last of an unknown function: a closure taking exactly the
// arguments given, and not a thunk forced, called at once, where the
// call is -- what `heff_apply` would call, without the call of it;
// anything else through it)
HEFF_FAST V heff_call(V f, int n, V *a) {
  Clo *c = (Clo *)f;
  if (c->h.kind == K_CLO && c->arity == n && !(n == 1 && a[0] == NTAG(TAG_MkUnit)))
    return c->fn(f, a);
  return heff_apply(f, n, a);
}

// (whether `f`, given the `n` arguments at `a`, is a closure taking
// exactly them and not a thunk forced -- what `heff_apply` would call at
// once, as it is -- and a call last of it a jump: a lambda's call last
// of it is then not counted (src/heff/compile/emitc.hf, `inPlace`))
static inline int heff_exact(V f, int n, V *a) {
  return HEFF_JUMPS && IS_OBJ(f) && OBJ(f)->kind == K_CLO
      && ((Clo *)f)->arity == n && !(n == 1 && a[0] == NTAG(TAG_MkUnit));
}

// Definitions' values
// ---------------------------------------------------------------------
// A definition that is no function is computed when it is first asked
// for, and kept (src/heff/compile/emitc.hf, `valDef`): its value
// `g_X_val`, its state `g_X_st` (`HEFF_ONCE`), asked for by `g_X_get`,
// which has it at once where it is computed (`HEFF_DONE`), and otherwise
// asks `heff_global` to compute it by `g_X_init`.  One thread: the state
// 0, then 1 while it is computed -- asked for again then, it is defined
// by itself -- then 2.  Threads (`HEFF_THREADS`): computed once, by the
// first thread to ask, the others asking meanwhile waiting for it, and
// made immortal (`heff_freeze`), so that every thread has the same value
// and none counts it; while it is computed, its state is the thread
// computing it (`heff_me`'s address), so that a thread asking for what
// it is computing itself is told it is defined by itself, not left to
// wait.  (Definitions are in order, none using one after it: a thread
// computing one waits only for one before it, never for one waiting for
// it.)  A failure caught (`heff_try`) where values were being computed
// leaves them to be computed again.
#ifdef HEFF_THREADS
#define HEFF_ONCE uintptr_t
#define HEFF_DONE(st) HEFF_LIKELY(__atomic_load_n(&(st), __ATOMIC_ACQUIRE) == 2)
#ifdef HEFF_PART
extern pthread_mutex_t heff_once_mu;
extern pthread_cond_t heff_once_cv;
#else
pthread_mutex_t heff_once_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t heff_once_cv = PTHREAD_COND_INITIALIZER;
#endif
HEFF_VAR(HEFF_TLS char heff_me);
// (the states of what this thread is computing, the last innermost)
HEFF_VAR(HEFF_TLS HEFF_ONCE **heff_open);
HEFF_VAR(HEFF_TLS size_t heff_nopen);
HEFF_VAR(HEFF_TLS size_t heff_capopen);

static __attribute__((noinline)) V heff_global(V *val, HEFF_ONCE *st, V (*init)(void)) {
  uintptr_t me = (uintptr_t)&heff_me;
  pthread_mutex_lock(&heff_once_mu);
  for (;;) {
    uintptr_t s = __atomic_load_n(st, __ATOMIC_ACQUIRE);
    if (s == 2) { pthread_mutex_unlock(&heff_once_mu); return *val; }
    if (s == me) { pthread_mutex_unlock(&heff_once_mu); heff_fail("defined by itself"); }
    if (s == 0) break;
    pthread_cond_wait(&heff_once_cv, &heff_once_mu);
  }
  __atomic_store_n(st, me, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&heff_once_mu);
  if (heff_nopen == heff_capopen) {
    heff_capopen = heff_capopen ? heff_capopen * 2 : 16;
    heff_open = realloc(heff_open, heff_capopen * sizeof(HEFF_ONCE *));
    if (!heff_open) heff_fail("out of memory");
  }
  heff_open[heff_nopen++] = st;
  V v = heff_tr(init());
  heff_nopen--;
  heff_freeze(v);
  pthread_mutex_lock(&heff_once_mu);
  *val = v;
  __atomic_store_n(st, 2, __ATOMIC_RELEASE);
  pthread_cond_broadcast(&heff_once_cv);
  pthread_mutex_unlock(&heff_once_mu);
  return v;
}

// (what this thread was computing when a failure was caught, from the
// `base`th on, to be computed again: the threads waiting for it woken)
static void heff_reopen(size_t base) {
  if (heff_nopen <= base) return;
  pthread_mutex_lock(&heff_once_mu);
  while (heff_nopen > base) __atomic_store_n(heff_open[--heff_nopen], 0, __ATOMIC_RELAXED);
  pthread_cond_broadcast(&heff_once_cv);
  pthread_mutex_unlock(&heff_once_mu);
}
#else
#define HEFF_ONCE int
#define HEFF_DONE(st) HEFF_LIKELY((st) == 2)

static __attribute__((noinline)) V heff_global(V *val, HEFF_ONCE *st, V (*init)(void)) {
  if (*st == 1) heff_fail("defined by itself");
  *st = 1;
  *val = heff_tr(init());
  *st = 2;
  return *val;
}
#endif

// Integers
// ---------------------------------------------------------------------
// An integer is a word while it fits in 62 bits and a sign (`MK_INT`,
// from -2^62 up to 2^62); beyond, a big one (`K_BIG`): its sign and its
// magnitude's 32-bit digits, the least first.  Each result fitting in a
// word is one, so that two equal integers are both words or both big.

// (an integer's sign and digits, to compute with: a word's in `buf`)
typedef struct { int neg; int n; const uint32_t *d; uint32_t buf[2]; } Num;

static void num_of(V v, Num *x) {
  if (IS_INT(v)) {
    int64_t i = INT_OF(v);
    uint64_t m = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
    x->neg = i < 0;
    x->buf[0] = (uint32_t)m;
    x->buf[1] = (uint32_t)(m >> 32);
    x->n = x->buf[1] ? 2 : x->buf[0] ? 1 : 0;
    x->d = x->buf;
  } else {
    Big *b = (Big *)v;
    x->neg = b->neg; x->n = b->n; x->d = b->d;
  }
}

// (digits as an integer: a word if they fit)
static V big_make(int neg, const uint32_t *d, int n) {
  while (n > 0 && d[n - 1] == 0) n--;
  if (n <= 2) {
    uint64_t m = n == 0 ? 0 : n == 1 ? d[0] : ((uint64_t)d[1] << 32) | d[0];
    if (m < (uint64_t)INT_LIM || (neg && m == (uint64_t)INT_LIM))
      return MK_INT(neg ? (int64_t)((uint64_t)0 - m) : (int64_t)m);
  }
  Big *b = heff_obj_alloc(sizeof(Big) + n * sizeof(uint32_t));
  b->h.rc = 1; b->h.kind = K_BIG; b->h.tag = 0; b->neg = neg; b->n = n;
  memcpy(b->d, d, n * sizeof(uint32_t));
  return (V)b;
}

HEFF_SLOW V heff_int_big(int64_t i) {
  uint64_t m = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
  uint32_t d[2] = { (uint32_t)m, (uint32_t)(m >> 32) };
  return big_make(i < 0, d, 2);
}

HEFF_FAST V heff_int(int64_t i) {
  if (HEFF_LIKELY(i >= -INT_LIM && i < INT_LIM)) return MK_INT(i);
  return heff_int_big(i);
}

static int mag_cmp(const uint32_t *a, int an, const uint32_t *b, int bn) {
  if (an != bn) return an < bn ? -1 : 1;
  for (int i = an - 1; i >= 0; i--)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

// (`o` has room for max(an, bn) + 1 digits)
static int mag_add(const uint32_t *a, int an, const uint32_t *b, int bn, uint32_t *o) {
  if (an < bn) { const uint32_t *t = a; a = b; b = t; int tn = an; an = bn; bn = tn; }
  uint64_t c = 0;
  for (int i = 0; i < an; i++) {
    c += (uint64_t)a[i] + (i < bn ? b[i] : 0);
    o[i] = (uint32_t)c;
    c >>= 32;
  }
  o[an] = (uint32_t)c;
  return an + 1;
}

// (a - b, a's magnitude not below b's)
static int mag_sub(const uint32_t *a, int an, const uint32_t *b, int bn, uint32_t *o) {
  int64_t c = 0;
  for (int i = 0; i < an; i++) {
    c += (int64_t)a[i] - (i < bn ? b[i] : 0);
    o[i] = (uint32_t)c;
    c = c < 0 ? -1 : 0;
  }
  return an;
}

// (a + b, signed)
static V num_add(const Num *a, int bneg, const Num *b) {
  int n = (a->n > b->n ? a->n : b->n) + 1;
  uint32_t *o = alloca(n * sizeof(uint32_t));
  if (a->neg == bneg) return big_make(a->neg, o, mag_add(a->d, a->n, b->d, b->n, o));
  if (mag_cmp(a->d, a->n, b->d, b->n) >= 0) return big_make(a->neg, o, mag_sub(a->d, a->n, b->d, b->n, o));
  return big_make(bneg, o, mag_sub(b->d, b->n, a->d, a->n, o));
}

// (u divided by v, v's top digit not 0, as Knuth's algorithm D:
// q gets m - n + 1 digits, r n)
static void mag_divmod(const uint32_t *u, int m, const uint32_t *v, int n, uint32_t *q, uint32_t *r) {
  if (m < n) {
    memcpy(r, u, m * sizeof(uint32_t));
    memset(r + m, 0, (n - m) * sizeof(uint32_t));
    return;
  }
  if (n == 1) {
    uint64_t k = 0;
    for (int j = m - 1; j >= 0; j--) {
      uint64_t t = (k << 32) | u[j];
      q[j] = (uint32_t)(t / v[0]);
      k = t - (uint64_t)q[j] * v[0];
    }
    r[0] = (uint32_t)k;
    return;
  }
  int s = __builtin_clz(v[n - 1]);
  uint32_t *vn = alloca(n * sizeof(uint32_t)), *un = alloca((m + 1) * sizeof(uint32_t));
  for (int i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (s ? v[i - 1] >> (32 - s) : 0);
  vn[0] = v[0] << s;
  un[m] = s ? u[m - 1] >> (32 - s) : 0;
  for (int i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (s ? u[i - 1] >> (32 - s) : 0);
  un[0] = u[0] << s;
  for (int j = m - n; j >= 0; j--) {
    uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
    uint64_t qhat = num / vn[n - 1], rhat = num - qhat * vn[n - 1];
    while (qhat >> 32 || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
      qhat--;
      rhat += vn[n - 1];
      if (rhat >> 32) break;
    }
    int64_t k = 0, t;
    for (int i = 0; i < n; i++) {
      uint64_t p = qhat * vn[i];
      t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
      un[i + j] = (uint32_t)t;
      k = (int64_t)(p >> 32) - (t >> 32);
    }
    t = (int64_t)un[j + n] - k;
    un[j + n] = (uint32_t)t;
    q[j] = (uint32_t)qhat;
    if (t < 0) {
      q[j]--;
      k = 0;
      for (int i = 0; i < n; i++) {
        t = (int64_t)un[i + j] + vn[i] + k;
        un[i + j] = (uint32_t)t;
        k = t >> 32;
      }
      un[j + n] += (uint32_t)k;
    }
  }
  for (int i = 0; i < n; i++) r[i] = (un[i] >> s) | (s ? un[i + 1] << (32 - s) : 0);
}

// (a divided by b, rounded down, as the other backends round: the
// quotient, or the remainder (b's sign))
static V num_divmod(const Num *a, const Num *b, int want_mod) {
  int qn = a->n - b->n + 1 > 0 ? a->n - b->n + 1 : 1;
  uint32_t *q = alloca((qn + 1) * sizeof(uint32_t)), *r = alloca((b->n + 1) * sizeof(uint32_t));
  memset(q, 0, (qn + 1) * sizeof(uint32_t));
  mag_divmod(a->d, a->n, b->d, b->n, q, r);
  int rn = b->n;
  while (rn > 0 && r[rn - 1] == 0) rn--;
  int down = rn > 0 && a->neg != b->neg;
  if (want_mod) {
    if (!down) return big_make(a->neg, r, rn);
    uint32_t *o = alloca((b->n + 1) * sizeof(uint32_t));
    return big_make(b->neg, o, mag_sub(b->d, b->n, r, rn, o));
  }
  if (!down) return big_make(a->neg != b->neg, q, qn);
  uint32_t one = 1, *o = alloca((qn + 2) * sizeof(uint32_t));
  return big_make(1, o, mag_add(q, qn, &one, 1, o));
}

static V num_mul(const Num *a, const Num *b) {
  int n = a->n + b->n;
  uint32_t *o = alloca((n + 1) * sizeof(uint32_t));
  memset(o, 0, (n + 1) * sizeof(uint32_t));
  for (int i = 0; i < a->n; i++) {
    uint64_t c = 0;
    for (int j = 0; j < b->n; j++) {
      c += (uint64_t)a->d[i] * b->d[j] + o[i + j];
      o[i + j] = (uint32_t)c;
      c >>= 32;
    }
    o[i + b->n] = (uint32_t)c;
  }
  return big_make(a->neg != b->neg, o, n);
}

static int num_cmp(const Num *a, const Num *b) {
  if (a->neg != b->neg) return a->neg ? -1 : 1;
  int c = mag_cmp(a->d, a->n, b->d, b->n);
  return a->neg ? -c : c;
}

// An integer's operation (`heff_intAdd` and the others) is inlined
// where it is used, as far as both are words and so is what it makes:
// there it is done on the words as they are, `2x + 1` each -- their
// sum less one is `2(x + y) + 1`, and the machine's overflow of it is
// exactly a sum beyond a word's -- with no call, no frame, nothing to
// drop.  Anything else, a big one or a result beyond a word, is
// `NAME_big`'s, a call: the operation on the integers' digits (`Num`),
// both consumed.
#define INT_BIG(T, name, big)                                           \
  HEFF_SLOW T name##_big(V a, V b) {                                    \
    Num x, y;                                                           \
    num_of(a, &x);                                                      \
    num_of(b, &y);                                                      \
    T r = big;                                                          \
    heff_drop(a);                                                       \
    heff_drop(b);                                                       \
    return r;                                                           \
  }

// (both words: tested at once, their low bits)
#define BOTH_INT(a, b) HEFF_LIKELY(IS_INT((a) & (b)))

INT_BIG(V, heff_intAdd, num_add(&x, y.neg, &y))
HEFF_FAST V heff_intAdd(V a, V b) {
  int64_t r;
  if (BOTH_INT(a, b) && !__builtin_add_overflow((int64_t)a, (int64_t)b - 1, &r)) return (V)r;
  return heff_intAdd_big(a, b);
}

// (`2x + 1 - 2y`)
INT_BIG(V, heff_intSub, num_add(&x, !y.neg && y.n > 0, &y))
HEFF_FAST V heff_intSub(V a, V b) {
  int64_t r;
  if (BOTH_INT(a, b) && !__builtin_sub_overflow((int64_t)a, (int64_t)b - 1, &r)) return (V)r;
  return heff_intSub_big(a, b);
}

// (`2x * y`, then its low bit set)
INT_BIG(V, heff_intMul, num_mul(&x, &y))
HEFF_FAST V heff_intMul(V a, V b) {
  int64_t r;
  if (BOTH_INT(a, b) && !__builtin_mul_overflow((int64_t)a - 1, INT_OF(b), &r)) return (V)r | 1;
  return heff_intMul_big(a, b);
}

// (rounded down, as the other backends round; by 0, 0 -- a division is
// total -- which the call gives.  Of two words only -2^62 / -1 is beyond
// a word)
INT_BIG(V, heff_intDiv, y.n == 0 ? MK_INT(0) : num_divmod(&x, &y, 0))
HEFF_FAST V heff_intDiv(V a, V b) {
  if (BOTH_INT(a, b) && b != MK_INT(0)) {
    int64_t x = INT_OF(a), y = INT_OF(b), q = x / y, r = x % y;
    if (r != 0 && ((r < 0) != (y < 0))) q -= 1;
    if (HEFF_LIKELY(q < INT_LIM)) return MK_INT(q);
  }
  return heff_intDiv_big(a, b);
}

// (of the divisor's sign, and nearer 0 than it: a word; by 0, the
// dividend, which the call gives -- `(a / b) * b + a % b` is `a` still)
INT_BIG(V, heff_intMod, y.n == 0 ? heff_dup(a) : num_divmod(&x, &y, 1))
HEFF_FAST V heff_intMod(V a, V b) {
  if (BOTH_INT(a, b) && b != MK_INT(0)) {
    int64_t y = INT_OF(b), r = INT_OF(a) % y;
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return MK_INT(r);
  }
  return heff_intMod_big(a, b);
}

// A comparison, as C's condition (`heff_intLt_c`, what the emitter
// tests where its Bool is only looked at) and as its Bool.  Words are
// in the order of their integers, and equal as they are; a word and a
// big one are never equal.
#define INT_CMP(name, op, cmp)                                          \
  INT_BIG(int, name##_c, num_cmp(&x, &y) cmp)                           \
  HEFF_FAST int name##_c(V a, V b) {                                    \
    if (BOTH_INT(a, b)) return (int64_t)a op (int64_t)b;                \
    return name##_c_big(a, b);                                          \
  }                                                                     \
  HEFF_FAST V name(V a, V b) { return heff_bool(name##_c(a, b)); }
INT_CMP(heff_intEq, ==, == 0)
INT_CMP(heff_intLt, <, < 0)
INT_CMP(heff_intLe, <=, <= 0)

// An integer's bits are as two's complement has them, its sign extended
// for ever (Python's): two words' operation is the words', a big one's
// done on as many 32-bit words as the longer has, and one more for the
// sign.

// (`x` as `n` words of two's complement, the least first)
static void num_twos(const Num *x, uint32_t *t, int n) {
  uint64_t carry = 1;
  for (int i = 0; i < n; i++) {
    uint32_t w = i < x->n ? x->d[i] : 0;
    if (x->neg) {
      uint64_t s = (uint64_t)(uint32_t)~w + carry;
      t[i] = (uint32_t)s;
      carry = s >> 32;
    } else t[i] = w;
  }
}

// (`n` words of two's complement as an integer; `t` is overwritten)
static V twos_make(uint32_t *t, int n) {
  int neg = (int)(t[n - 1] >> 31);
  if (neg) {
    uint64_t carry = 1;
    for (int i = 0; i < n; i++) {
      uint64_t s = (uint64_t)(uint32_t)~t[i] + carry;
      t[i] = (uint32_t)s;
      carry = s >> 32;
    }
  }
  return big_make(neg, t, n);
}

#define INT_BITS(name, op)                                                  \
  static V name(V a, V b) {                                                 \
    if (IS_INT(a) && IS_INT(b)) return MK_INT(INT_OF(a) op INT_OF(b));      \
    Num x, y;                                                               \
    num_of(a, &x);                                                          \
    num_of(b, &y);                                                          \
    int n = (x.n > y.n ? x.n : y.n) + 1;                                    \
    uint32_t *s = alloca(n * sizeof(uint32_t));                             \
    uint32_t *t = alloca(n * sizeof(uint32_t));                             \
    num_twos(&x, s, n);                                                     \
    num_twos(&y, t, n);                                                     \
    for (int i = 0; i < n; i++) s[i] = s[i] op t[i];                        \
    V r = twos_make(s, n);                                                  \
    heff_drop(a);                                                           \
    heff_drop(b);                                                           \
    return r;                                                               \
  }
INT_BITS(heff_intAnd, &)
INT_BITS(heff_intOr, |)
INT_BITS(heff_intXor, ^)

static int heff_int_neg(V v) { return IS_INT(v) ? INT_OF(v) < 0 : ((Big *)v)->neg; }

static V heff_intShr(V a, V b);

// (`a` times 2^`b`; by a negative count, a shift right by its size; one
// beyond memory fails)
static V heff_intShl(V a, V b) {
  if (heff_int_neg(b)) return heff_intShr(a, heff_intSub(MK_INT(0), b));
  int bbig = !IS_INT(b);
  heff_drop(b);
  if (IS_INT(a) && INT_OF(a) == 0) return a;
  if (bbig) heff_fail("a shift beyond memory");
  int64_t k = INT_OF(b);
  if (IS_INT(a) && k < 62) {
    __int128 r = (__int128)INT_OF(a) * ((__int128)1 << k);
    if (r < INT_LIM && r >= -INT_LIM) return MK_INT((int64_t)r);
  }
  Num x;
  num_of(a, &x);
  int64_t w = k / 32, s = k % 32, n = x.n + w + 1;
  uint32_t *o = heff_alloc(n * sizeof(uint32_t));
  memset(o, 0, n * sizeof(uint32_t));
  for (int i = 0; i < x.n; i++) {
    uint64_t v = (uint64_t)x.d[i] << s;
    o[i + w] |= (uint32_t)v;
    o[i + w + 1] |= (uint32_t)(v >> 32);
  }
  V r = big_make(x.neg, o, (int)n);
  free(o);
  heff_drop(a);
  return r;
}

// (`a` divided by 2^`b`, rounding down: its bits shifted out, the sign
// shifted in; by a negative count, a shift left by its size)
static V heff_intShr(V a, V b) {
  if (heff_int_neg(b)) return heff_intShl(a, heff_intSub(MK_INT(0), b));
  int bbig = !IS_INT(b);
  heff_drop(b);
  int aneg = heff_int_neg(a);
  if (bbig) { heff_drop(a); return MK_INT(aneg ? -1 : 0); }
  int64_t k = INT_OF(b);
  if (IS_INT(a)) return MK_INT(k >= 63 ? (aneg ? -1 : 0) : INT_OF(a) >> k);
  Num x;
  num_of(a, &x);
  int64_t w = k / 32, s = k % 32;
  if (w >= x.n) { heff_drop(a); return MK_INT(aneg ? -1 : 0); }
  int n = x.n - (int)w;
  uint32_t *o = alloca((n + 1) * sizeof(uint32_t));
  for (int i = 0; i < n; i++) {
    uint64_t v = x.d[i + w] | ((uint64_t)(i + w + 1 < x.n ? x.d[i + w + 1] : 0) << 32);
    o[i] = (uint32_t)(v >> s);
  }
  o[n] = 0;
  // (a negative one rounds down: its magnitude up, where a bit set was
  // shifted out)
  int lost = s && (x.d[w] & (((uint32_t)1 << s) - 1));
  for (int64_t i = 0; i < w && !lost; i++) lost = x.d[i] != 0;
  if (x.neg && lost) {
    uint64_t carry = 1;
    for (int i = 0; i <= n && carry; i++) {
      uint64_t v = (uint64_t)o[i] + carry;
      o[i] = (uint32_t)v;
      carry = v >> 32;
    }
  }
  V r = big_make(x.neg, o, n + 1);
  heff_drop(a);
  return r;
}

// (an integer's decimal digits, a '-' before a negative one: malloc'd)
static char *heff_int_cstr(V v) {
  Num x;
  num_of(v, &x);
  uint32_t *m = alloca((x.n + 1) * sizeof(uint32_t));
  memcpy(m, x.d, x.n * sizeof(uint32_t));
  int n = x.n;
  size_t cap = (size_t)n * 10 + 3, at = cap;
  char *c = heff_alloc(cap);
  c[--at] = 0;
  do {
    uint64_t k = 0;
    for (int j = n - 1; j >= 0; j--) {
      uint64_t t = (k << 32) | m[j];
      m[j] = (uint32_t)(t / 1000000000u);
      k = t % 1000000000u;
    }
    while (n > 0 && m[n - 1] == 0) n--;
    for (int i = 0; i < 9 && (n > 0 || k > 0 || i == 0); i++) {
      c[--at] = (char)('0' + k % 10);
      k /= 10;
    }
  } while (n > 0);
  if (x.neg) c[--at] = '-';
  memmove(c, c + at, cap - at);
  return c;
}

// (an integer written in the program, beyond a word: made once, kept)
static V heff_big_lit(const char *s) {
  int neg = *s == '-';
  if (neg) s++;
  size_t len = strlen(s);
  int cap = (int)(len / 9) + 2, n = 0;
  uint32_t *d = alloca(cap * sizeof(uint32_t));
  for (size_t i = 0; i < len;) {
    uint64_t chunk = 0, scale = 1;
    for (int k = 0; k < 9 && i < len; k++, i++) { chunk = chunk * 10 + (uint64_t)(s[i] - '0'); scale *= 10; }
    uint64_t c = chunk;
    for (int j = 0; j < n; j++) {
      c += (uint64_t)d[j] * scale;
      d[j] = (uint32_t)c;
      c >>= 32;
    }
    if (c) d[n++] = (uint32_t)c;
  }
  V v = big_make(neg, d, n);
  if (IS_OBJ(v)) OBJ(v)->rc = IMMORTAL;
  return v;
}
#ifdef HEFF_THREADS
// (threads at once: made by the first to have it, kept; another making
// it meanwhile lets its own go, leaking it)
#define HEFF_BIG(s) ({                                                      \
  static V heff_lit_;                                                       \
  V l_ = __atomic_load_n(&heff_lit_, __ATOMIC_ACQUIRE);                     \
  if (!l_) {                                                                \
    V none_ = 0;                                                            \
    l_ = heff_big_lit(s);                                                   \
    if (!__atomic_compare_exchange_n(&heff_lit_, &none_, l_, 0,             \
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))   \
      l_ = none_;                                                           \
  }                                                                         \
  l_; })
#else
#define HEFF_BIG(s) ({ static V heff_lit_; heff_lit_ ? heff_lit_ : (heff_lit_ = heff_big_lit(s)); })
#endif

// (a float's integer part, beyond a word if it must be)
static V heff_int_of_double(double f) {
  if (f < (double)INT_LIM && f >= -(double)INT_LIM) return MK_INT((int64_t)f);
  int e;
  double fr = frexp(fabs(f), &e);
  uint64_t mant = (uint64_t)ldexp(fr, 53);
  int sh = e - 53, n = sh / 32 + 3;
  uint32_t *d = alloca(n * sizeof(uint32_t));
  memset(d, 0, n * sizeof(uint32_t));
  int w = sh / 32, b = sh % 32;
  unsigned __int128 x = (unsigned __int128)mant << b;
  d[w] = (uint32_t)x;
  d[w + 1] = (uint32_t)(x >> 32);
  d[w + 2] = (uint32_t)(x >> 64);
  return big_make(f < 0, d, n);
}

static double heff_double_of_int(V v) {
  if (IS_INT(v)) return (double)INT_OF(v);
  char *c = heff_int_cstr(v);
  double d = strtod(c, NULL);
  free(c);
  return d;
}

// Strings
// ---------------------------------------------------------------------

static Str *heff_str_new(int64_t len) {
  Str *s = heff_obj_alloc(sizeof(Str) + len);
  s->h.rc = 1; s->h.kind = K_STR; s->h.tag = 0; s->len = len; s->cap = len;
  return s;
}

static V heff_str_ascii(const char *c) {
  int64_t n = (int64_t)strlen(c);
  Str *s = heff_str_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

// (`n` bytes, as they are.  The runtime's own: a provider builds a
// string by what it crosses as, "What providers share")
static V heff_str_of(const void *c, int64_t n) {
  Str *s = heff_str_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

// (the rune whose bytes start at `p`, `n` bytes there: how many bytes
// it takes, the rune `*out`.  What is not UTF-8 is U+FFFD, of as many
// bytes as start a rune's and go on as one's would, at least one -- the
// "maximal subpart" WHATWG's decoder takes, as TextDecoder does: after
// a first byte that starts a rune (C2 to F4), the k-th that does not go
// on as its (a second byte of E0, ED, F0 and F4 in their own ranges,
// Unicode's table 3-7) makes one U+FFFD of the k before it; a byte that
// starts none is one)
static int heff_u8_decode(const uint8_t *p, int64_t n, uint32_t *out) {
  uint8_t b = p[0];
  if (b < 0x80) { *out = b; return 1; }
  int w; uint32_t u, lo = 0x80, hi = 0xBF;
  if (b >= 0xC2 && b <= 0xDF) { w = 2; u = b & 0x1F; }
  else if (b >= 0xE0 && b <= 0xEF) { w = 3; u = b & 0x0F; if (b == 0xE0) lo = 0xA0; else if (b == 0xED) hi = 0x9F; }
  else if (b >= 0xF0 && b <= 0xF4) { w = 4; u = b & 0x07; if (b == 0xF0) lo = 0x90; else if (b == 0xF4) hi = 0x8F; }
  else { *out = 0xFFFD; return 1; }
  for (int k = 1; k < w; k++) {
    if (k >= n) { *out = 0xFFFD; return k; }
    uint8_t c = p[k];
    if (c < (k == 1 ? lo : 0x80) || c > (k == 1 ? hi : 0xBF)) { *out = 0xFFFD; return k; }
    u = (u << 6) | (c & 0x3F);
  }
  *out = u;
  return w;
}

// (a scalar value's UTF-8 bytes, at `o`: how many)
static int heff_u8_encode(uint8_t *o, uint32_t u) {
  if (u < 0x80) { o[0] = (uint8_t)u; return 1; }
  if (u < 0x800) { o[0] = (uint8_t)(0xC0 | (u >> 6)); o[1] = (uint8_t)(0x80 | (u & 0x3F)); return 2; }
  if (u < 0x10000) { o[0] = (uint8_t)(0xE0 | (u >> 12)); o[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3F)); o[2] = (uint8_t)(0x80 | (u & 0x3F)); return 3; }
  o[0] = (uint8_t)(0xF0 | (u >> 18)); o[1] = (uint8_t)(0x80 | ((u >> 12) & 0x3F)); o[2] = (uint8_t)(0x80 | ((u >> 6) & 0x3F)); o[3] = (uint8_t)(0x80 | (u & 0x3F));
  return 4;
}

// (whether the `w` bytes from `p`, read as `u`, are a rune's: not U+FFFD
// read for what is not UTF-8)
static inline int heff_u8_own(const uint8_t *p, uint32_t u, int w) {
  return u != 0xFFFD || (w == 3 && p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD);
}

// (how many bytes `n` bytes are, made text, what is not UTF-8 in them
// U+FFFD; as many, where they are UTF-8)
static int64_t heff_u8_valid_len(const uint8_t *p, int64_t n, int *own) {
  int64_t m = 0;
  *own = 1;
  for (int64_t i = 0; i < n;) {
    uint32_t u;
    int w = heff_u8_decode(p + i, n - i, &u);
    if (heff_u8_own(p + i, u, w)) m += w; else { *own = 0; m += 3; }
    i += w;
  }
  return m;
}

// (`n` bytes, made text: what is not UTF-8 in them U+FFFD, as
// TextDecoder reads them -- as they are, where they are UTF-8)
static V heff_str_valid(const void *c, int64_t n) {
  const uint8_t *p = c;
  int own;
  int64_t m = heff_u8_valid_len(p, n, &own);
  if (own) return heff_str_of(c, n);
  Str *s = heff_str_new(m);
  int64_t k = 0;
  for (int64_t i = 0; i < n;) {
    uint32_t u;
    int w = heff_u8_decode(p + i, n - i, &u);
    if (heff_u8_own(p + i, u, w)) { memcpy(s->s + k, p + i, (size_t)w); k += w; }
    else k += heff_u8_encode(s->s + k, 0xFFFD);
    i += w;
  }
  return (V)s;
}

// (the first string, its last reference, grown in place, room made for
// twice what it needs -- a string built a piece at a time copied a
// logarithmic number of times, not once a piece; `Bytes` too, a new one
// of the kind `kind`)
static V heff_buf_append(V a, V b, uint16_t kind) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t n = x->len + y->len;
  if (x->h.rc == 1) {
    if (n > x->cap) {
      int64_t cap = n > 2 * x->cap ? n : 2 * x->cap;
      size_t was = (sizeof(Str) + x->cap + 7) / 8 * 8;
      // (from the allocator objects are given back to, rounded as it
      // rounds: a block it frees to a free list must be the whole of
      // that list's size)
      Str *g = heff_obj_alloc(sizeof(Str) + cap);
      memcpy(g, x, sizeof(Str) + x->len);
      heff_obj_free(x, was);
      g->cap = cap;
      x = g;
    }
    memcpy(x->s + x->len, y->s, (size_t)y->len);
    x->len = n;
    heff_drop(b);
    return (V)x;
  }
  Str *s = heff_str_new(n);
  s->h.kind = kind;
  memcpy(s->s, x->s, (size_t)x->len);
  memcpy(s->s + x->len, y->s, (size_t)y->len);
  heff_drop(a);
  heff_drop(b);
  return (V)s;
}

static V heff_strAppend(V a, V b) { return heff_buf_append(a, b, K_STR); }

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
  memcpy(s->s, x->s + b, (size_t)(e - b));
  heff_drop(a);
  return (V)s;
}

// (the bytes' order: of UTF-8, its runes')
static int heff_str_cmp(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t n = x->len < y->len ? x->len : y->len;
  int c = n ? memcmp(x->s, y->s, (size_t)n) : 0;
  if (c) return c < 0 ? -1 : 1;
  return x->len == y->len ? 0 : x->len < y->len ? -1 : 1;
}

// (as C's condition, and as a Bool: see `INT_CMP`)
static int heff_strEq_c(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int c = x == y || (x->len == y->len && memcmp(x->s, y->s, (size_t)x->len) == 0);
  heff_drop(a);
  heff_drop(b);
  return c;
}
static inline V heff_strEq(V a, V b) { return heff_bool(heff_strEq_c(a, b)); }

static int heff_strLt_c(V a, V b) {
  int c = heff_str_cmp(a, b);
  heff_drop(a);
  heff_drop(b);
  return c < 0;
}
static inline V heff_strLt(V a, V b) { return heff_bool(heff_strLt_c(a, b)); }

// (the byte at `i`; out of the string, -1)
static V heff_strByteAt(V a, V i) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  V r = k >= 0 && k < x->len ? MK_INT(x->s[k]) : MK_INT(-1);
  heff_drop(a);
  return r;
}

// (the string of one byte, a `UInt8`: a word of its value, 0 to 255)
static V heff_strFromByte(V c) {
  int64_t k = INT_OF(c);
  Str *s = heff_str_new(1);
  s->s[0] = (uint8_t)k;
  return (V)s;
}

static V heff_intShow(V a) {
  char *c = heff_int_cstr(a);
  heff_drop(a);
  V s = heff_str_ascii(c);
  free(c);
  return s;
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
// (as C's condition, and as a Bool: see `INT_CMP`)
#define FLT_CMP(name, op)                                                   \
  static inline int name##_c(V a, V b) {                                    \
    double x = FLT_OF(a), y = FLT_OF(b); heff_drop(a); heff_drop(b); return x op y; } \
  static inline V name(V a, V b) { return heff_bool(name##_c(a, b)); }
FLT_CMP(heff_floatEq, ==)
FLT_CMP(heff_floatLt, <)
FLT_CMP(heff_floatLe, <=)
static V heff_floatFromInt(V a) { double d = heff_double_of_int(a); heff_drop(a); return heff_flt(d); }
static V heff_floatSqrt(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(sqrt(x)); }

// (std.math's: libm's, as JavaScript's `Math` has them)
#define FLT1(name, f) \
  static V name(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(f(x)); }
FLT1(heff_floatSin, sin)
FLT1(heff_floatCos, cos)
FLT1(heff_floatTan, tan)
FLT1(heff_floatAsin, asin)
FLT1(heff_floatAcos, acos)
FLT1(heff_floatAtan, atan)
FLT1(heff_floatExp, exp)
FLT1(heff_floatLog, log)
FLT2(heff_floatAtan2, heff_flt(atan2(x, y)))
FLT2(heff_floatPow, heff_flt(pow(x, y)))

// (of what is not finite, 0)
static V heff_floatFloor(V a) {
  double d = FLT_OF(a);
  heff_drop(a);
  if (!isfinite(d)) return MK_INT(0);
  double f = floor(d);
  return heff_int_of_double(f);
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

// A Float32 is kept in a word, its bits as an integer's: nothing is
// allocated for one, as it is for a float.
static inline V heff_f32(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return MK_INT((int64_t)u);
}

static inline float heff_f32_of(V v) {
  uint32_t u = (uint32_t)INT_OF(v);
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

// Fixed-width numbers
// ---------------------------------------------------------------------
// (heff.elab.fixed's.)  An integer of up to 32 bits is a word of its value,
// in its type's range; one of 64 bits is kept as an `Int` is (a word, big
// beyond 62 bits), so `toInt` is nothing.  An integer's operation is
// done on its 64 bits, unsigned (where no overflow is undefined), and the
// result wrapped around to its type; a division by -1 is a negation
// (that of the least, itself), as no C division by -1 is safe, and one
// by 0 is 0, its remainder the dividend, as an `Int`'s are.  A
// `Float32` is a word of its bits (`heff_f32`): C's `float` is IEEE's,
// each operation rounded, as JavaScript's `Math.fround` rounds.

// (an integer's lowest 64 bits, as two's complement has them; borrowed)
static uint64_t heff_int_low64(V v) {
  if (IS_INT(v)) return (uint64_t)INT_OF(v);
  Big *b = (Big *)v;
  uint64_t m = b->n > 0 ? b->d[0] : 0;
  if (b->n > 1) m |= (uint64_t)b->d[1] << 32;
  return b->neg ? (uint64_t)0 - m : m;
}

static V heff_int_u64(uint64_t m) {
  if (m < (uint64_t)INT_LIM) return MK_INT((int64_t)m);
  uint32_t d[2] = { (uint32_t)m, (uint32_t)(m >> 32) };
  return big_make(0, d, 2);
}

// (a value of the type `T`, its 64 bits taken (and consumed), and one
// made of 64 bits, wrapped around to `T`)
#define FX_WORD(T)                                                          \
  static inline uint64_t fx_get_##T(V v) { return (uint64_t)INT_OF(v); }    \
  static inline V fx_mk_##T(uint64_t u) { return MK_INT((int64_t)(T)u); }
FX_WORD(int8_t)
FX_WORD(int16_t)
FX_WORD(int32_t)
FX_WORD(uint8_t)
FX_WORD(uint16_t)
FX_WORD(uint32_t)

static inline uint64_t fx_get_int64_t(V v) {
  uint64_t u = heff_int_low64(v);
  heff_drop(v);
  return u;
}
static inline V fx_mk_int64_t(uint64_t u) { return heff_int((int64_t)u); }
static inline uint64_t fx_get_uint64_t(V v) { return fx_get_int64_t(v); }
static inline V fx_mk_uint64_t(uint64_t u) { return heff_int_u64(u); }

// (an integer type's operations on its C type `T` (`heff_div_i32` and the
// rest, `S` its suffix, `BITS` its bits), which its primitives are made of
// and a program's unboxed numbers (#39) are computed by: a shift is by the
// count modulo the bits, and by a negative count, of a signed type, the
// other way by its size)
#define FX_OPS(name, S, T, BITS)                                                     \
  static inline T heff_div_##S(T x, T y) {                                           \
    if (y == 0) return 0;                                                            \
    if ((T)-1 < (T)0 && y == (T)-1) return (T)((uint64_t)0 - (uint64_t)x);          \
    return (T)(x / y); }                                                             \
  static inline T heff_rem_##S(T x, T y) {                                           \
    if (y == 0) return x;                                                            \
    if ((T)-1 < (T)0 && y == (T)-1) return 0;                                        \
    return (T)(x % y); }                                                             \
  static inline uint64_t heff_##name##Left(uint64_t x, uint64_t n) {                 \
    return x << (n & (BITS - 1)); }                                                  \
  static inline uint64_t heff_##name##Right(uint64_t x, uint64_t n) {                \
    return (uint64_t)((T)x >> (n & (BITS - 1))); }                                   \
  static inline T heff_shl_##S(T a, T b) {                                           \
    uint64_t x = (uint64_t)a, y = (uint64_t)b;                                       \
    if ((T)-1 < (T)0 && (int64_t)y < 0) return (T)heff_##name##Right(x, 0 - y);      \
    return (T)heff_##name##Left(x, y); }                                             \
  static inline T heff_shr_##S(T a, T b) {                                           \
    uint64_t x = (uint64_t)a, y = (uint64_t)b;                                       \
    if ((T)-1 < (T)0 && (int64_t)y < 0) return (T)heff_##name##Left(x, 0 - y);       \
    return (T)heff_##name##Right(x, y); }

// (an integer type's primitives: `heff_int32Add` and the rest, `T` its C
// type, `S` its suffix, `BITS` its bits)
#define FX_INT(name, S, T, BITS)                                                     \
  FX_OPS(name, S, T, BITS)                                                           \
  static V heff_##name##Add(V a, V b) {                                              \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x + y); }       \
  static V heff_##name##Sub(V a, V b) {                                              \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x - y); }       \
  static V heff_##name##Mul(V a, V b) {                                              \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x * y); }       \
  static V heff_##name##Div(V a, V b) {                                              \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b);                                   \
    return fx_mk_##T((uint64_t)heff_div_##S(x, y)); }                                \
  static V heff_##name##Rem(V a, V b) {                                              \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b);                                   \
    return fx_mk_##T((uint64_t)heff_rem_##S(x, y)); }                                \
  static inline int heff_##name##Eq_c(V a, V b) {                                   \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b); return x == y; }                  \
  static inline int heff_##name##Lt_c(V a, V b) {                                   \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b); return x < y; }                   \
  static inline int heff_##name##Le_c(V a, V b) {                                   \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b); return x <= y; }                  \
  static inline V heff_##name##Eq(V a, V b) { return heff_bool(heff_##name##Eq_c(a, b)); } \
  static inline V heff_##name##Lt(V a, V b) { return heff_bool(heff_##name##Lt_c(a, b)); } \
  static inline V heff_##name##Le(V a, V b) { return heff_bool(heff_##name##Le_c(a, b)); } \
  static V heff_##name##And(V a, V b) {                                              \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x & y); }       \
  static V heff_##name##Or(V a, V b) {                                               \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x | y); }       \
  static V heff_##name##Xor(V a, V b) {                                              \
    uint64_t x = fx_get_##T(a), y = fx_get_##T(b); return fx_mk_##T(x ^ y); }       \
  static V heff_##name##Shl(V a, V b) {                                              \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b);                                   \
    return fx_mk_##T((uint64_t)heff_shl_##S(x, y)); }                                \
  static V heff_##name##Shr(V a, V b) {                                              \
    T x = (T)fx_get_##T(a), y = (T)fx_get_##T(b);                                   \
    return fx_mk_##T((uint64_t)heff_shr_##S(x, y)); }                                \
  static V heff_##name##Show(V a) { return heff_intShow(a); }                        \
  static V heff_##name##FromInt(V a) {                                               \
    uint64_t u = heff_int_low64(a); heff_drop(a); return fx_mk_##T(u); }             \
  static V heff_##name##ToInt(V a) { return a; }
FX_INT(int8, i8, int8_t, 8)
FX_INT(int16, i16, int16_t, 16)
FX_INT(int32, i32, int32_t, 32)
FX_INT(int64, i64, int64_t, 64)
FX_INT(uint8, u8, uint8_t, 8)
FX_INT(uint16, u16, uint16_t, 16)
FX_INT(uint32, u32, uint32_t, 32)
FX_INT(uint64, u64, uint64_t, 64)

#define F32_2(name, expr) \
  static V heff_float32##name(V a, V b) { float x = heff_f32_of(a), y = heff_f32_of(b); return expr; }
F32_2(Add, heff_f32(x + y))
F32_2(Sub, heff_f32(x - y))
F32_2(Mul, heff_f32(x * y))
F32_2(Div, heff_f32(x / y))
#define F32_CMP(name, op)                                                                  \
  static inline int heff_float32##name##_c(V a, V b) { return heff_f32_of(a) op heff_f32_of(b); } \
  static inline V heff_float32##name(V a, V b) { return heff_bool(heff_float32##name##_c(a, b)); }
F32_CMP(Eq, ==)
F32_CMP(Lt, <)
F32_CMP(Le, <=)
static V heff_float32Sqrt(V a) { return heff_f32(sqrtf(heff_f32_of(a))); }

// (the fewest digits that read back as the same `Float32`, read as a float
// reads them (through a double), written as a float is: for each count of
// digits, the number of that many nearest it (of two as near, the one
// whose last digit is even, as printf rounds), if it reads back so; nine
// always do)
static V heff_float32Show(V a) {
  float f = heff_f32_of(a);
  double d = f;
  char buf[64];
  if (isfinite(d) && d != 0)
    for (int p = 1; p <= 9; p++) {
      snprintf(buf, sizeof buf, "%.*e", p - 1, d);
      double t = strtod(buf, NULL);
      if ((float)t == f) { d = t; break; }
    }
  heff_js_number(d, buf);
  if (!strpbrk(buf, ".eNI")) strcat(buf, ".0");
  return heff_str_ascii(buf);
}

static V heff_float32FromFloat(V a) { float f = (float)FLT_OF(a); heff_drop(a); return heff_f32(f); }
static V heff_float32ToFloat(V a) { return heff_flt((double)heff_f32_of(a)); }
static V heff_float32FromInt(V a) { double d = heff_double_of_int(a); heff_drop(a); return heff_f32((float)d); }
static V heff_float32Floor(V a) {
  double d = heff_f32_of(a);
  if (!isfinite(d)) return MK_INT(0);
  return heff_int_of_double(floor(d));
}

// Unboxed numbers (#39)
// ---------------------------------------------------------------------
// Where a program computes with a fixed-width number or a `Float64`, it
// keeps it as its C type (src/heff/compile/repr.hf), and makes it a `V`
// only where one is wanted: `heff_box_S` (`S` the type's suffix, `i64`,
// `f32`, ...), the `V` made of it; `heff_unbox_S`, the number of a `V`,
// consumed; `heff_peek_S`, looked at.  `heff_from_int_S` is the type's
// `fromInt` of an `Int` (consumed), `heff_floor_S` its `floor`.

#define FX_BOXES(S, T)                                                      \
  static inline V heff_box_##S(T x) { return fx_mk_##T((uint64_t)x); }      \
  static inline T heff_unbox_##S(V v) { return (T)fx_get_##T(v); }         \
  static inline T heff_peek_##S(V v) { return (T)heff_int_low64(v); }      \
  static inline T heff_from_int_##S(V v) {                                 \
    uint64_t u = heff_int_low64(v); heff_drop(v); return (T)u; }
FX_BOXES(i8, int8_t)
FX_BOXES(i16, int16_t)
FX_BOXES(i32, int32_t)
FX_BOXES(i64, int64_t)
FX_BOXES(u8, uint8_t)
FX_BOXES(u16, uint16_t)
FX_BOXES(u32, uint32_t)
FX_BOXES(u64, uint64_t)

static inline V heff_box_f32(float x) { return heff_f32(x); }
static inline float heff_unbox_f32(V v) { return heff_f32_of(v); }
static inline float heff_peek_f32(V v) { return heff_f32_of(v); }
static inline float heff_from_int_f32(V v) {
  double d = heff_double_of_int(v); heff_drop(v); return (float)d; }

static inline V heff_box_f64(double x) { return heff_flt(x); }
static inline double heff_unbox_f64(V v) { double d = FLT_OF(v); heff_drop(v); return d; }
static inline double heff_peek_f64(V v) { return FLT_OF(v); }
static inline double heff_from_int_f64(V v) {
  double d = heff_double_of_int(v); heff_drop(v); return d; }

// A record's flat fields (#39, "Flat fields" above): a number's bits, the
// field so made (`heff_word_S`), and the field read as a number,
// whichever it is, flat or a `V` (`heff_rget_S`); the record consumed
// after (`heff_get_n_S`), or the field as a `V`, boxed if it is flat
// (`heff_rget_v`).

static inline V heff_word_f64(double d) { V w; memcpy(&w, &d, sizeof w); return w; }
static inline V heff_word_i64(int64_t x) { return (V)(uint64_t)x; }
static inline V heff_word_u64(uint64_t x) { return (V)x; }
static inline double heff_f64_of_word(V w) { double d; memcpy(&d, &w, sizeof d); return d; }

static V heff_flat_box(V w, unsigned k) {
  if (k == HEFF_FLAT_F64) return heff_flt(heff_f64_of_word(w));
  if (k == HEFF_FLAT_I64) return heff_int((int64_t)w);
  return heff_int_u64((uint64_t)w);
}

HEFF_FAST double heff_rget_f64(V v, int64_t i) {
  Rec *r = (Rec *)v;
  V w = r->f[i];
  return heff_flat_kind(r->flat, i) ? heff_f64_of_word(w) : FLT_OF(w);
}
HEFF_FAST int64_t heff_rget_i64(V v, int64_t i) {
  Rec *r = (Rec *)v;
  V w = r->f[i];
  return heff_flat_kind(r->flat, i) ? (int64_t)w : (int64_t)heff_int_low64(w);
}
HEFF_FAST uint64_t heff_rget_u64(V v, int64_t i) {
  Rec *r = (Rec *)v;
  V w = r->f[i];
  return heff_flat_kind(r->flat, i) ? (uint64_t)w : heff_int_low64(w);
}
HEFF_FAST V heff_rget_v(V v, int64_t i) { return heff_rec_dup_field((Rec *)v, i); }

#define HEFF_GET_N_FLAT(S, T)                                               \
  HEFF_KNOWN T heff_get_n_##S(V v, int64_t i, int64_t n) {                  \
    T x = heff_rget_##S(v, i); heff_drop_rec(v, n); return x; }
HEFF_GET_N_FLAT(f64, double)
HEFF_GET_N_FLAT(i64, int64_t)
HEFF_GET_N_FLAT(u64, uint64_t)

// (of what is not finite, 0)
static V heff_floor_f64(double d) {
  if (!isfinite(d)) return MK_INT(0);
  return heff_int_of_double(floor(d));
}
static V heff_floor_f32(float f) { return heff_floor_f64((double)f); }

// Runes
// ---------------------------------------------------------------------
// A `Rune` is a word of its code point, a 32-bit value, as Go's `rune` is:
// `fromInt` wraps an integer around to 32 bits, as `int32FromInt` does,
// so `toInt` is nothing.  One that is no Unicode scalar value (a
// surrogate, below 0, beyond U+10FFFF) is U+FFFD in a string made of it;
// what is not UTF-8 in a string is read as U+FFFD (`heff_u8_decode`).

static inline int heff_runeEq_c(V a, V b) { return a == b; }
static inline int heff_runeLt_c(V a, V b) { return INT_OF(a) < INT_OF(b); }
static inline int heff_runeLe_c(V a, V b) { return INT_OF(a) <= INT_OF(b); }
static inline V heff_runeEq(V a, V b) { return heff_bool(heff_runeEq_c(a, b)); }
static inline V heff_runeLt(V a, V b) { return heff_bool(heff_runeLt_c(a, b)); }
static inline V heff_runeLe(V a, V b) { return heff_bool(heff_runeLe_c(a, b)); }
static V heff_runeToInt(V a) { return a; }
static V heff_runeFromInt(V a) { return heff_int32FromInt(a); }

// (a string's runes: read, then put in a list from the last)
static V heff_strRunes(V a) {
  Str *x = (Str *)a;
  uint32_t *rs = heff_alloc((size_t)(x->len + 1) * sizeof(uint32_t));
  int64_t m = 0;
  for (int64_t i = 0; i < x->len; m++) i += heff_u8_decode(x->s + i, x->len - i, &rs[m]);
  V l = NTAG(TAG_Nil);
  for (int64_t k = m - 1; k >= 0; k--) {
    V f[2] = { MK_INT((int64_t)rs[k]), l };
    l = heff_tag(TAG_Cons, 2, f);
  }
  free(rs);
  heff_drop(a);
  return l;
}

static inline int heff_rune_scalar(int64_t r) {
  return r >= 0 && r <= 0x10FFFF && !(r >= 0xD800 && r < 0xE000);
}

// (the runes' string: each its UTF-8 bytes)
static V heff_strOfRunes(V l) {
  int64_t n = 0;
  uint8_t tmp[4];
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    int64_t r = INT_OF(heff_field(c, 0));
    n += heff_u8_encode(tmp, heff_rune_scalar(r) ? (uint32_t)r : 0xFFFD);
  }
  Str *s = heff_str_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    int64_t r = INT_OF(heff_field(c, 0));
    k += heff_u8_encode(s->s + k, heff_rune_scalar(r) ? (uint32_t)r : 0xFFFD);
  }
  heff_drop(l);
  return (V)s;
}

// (the rune at `i`, and how many bytes it takes, `*w`; out of the
// string, U+FFFD of none)
static uint32_t heff_str_rune_at(Str *x, int64_t i, int64_t *w) {
  if (i < 0 || i >= x->len) { *w = 0; return 0xFFFD; }
  uint32_t u;
  *w = heff_u8_decode(x->s + i, x->len - i, &u);
  return u;
}

static V heff_strRuneAt(V a, V i) {
  int64_t w;
  uint32_t r = heff_str_rune_at((Str *)a, INT_OF(i), &w);
  heff_drop(a);
  return MK_INT((int64_t)r);
}

static V heff_strRuneWidth(V a, V i) {
  int64_t w;
  heff_str_rune_at((Str *)a, INT_OF(i), &w);
  heff_drop(a);
  return MK_INT(w);
}

static V heff_strRuneCount(V a) {
  Str *x = (Str *)a;
  int64_t n = 0;
  uint32_t u;
  for (int64_t i = 0; i < x->len; n++) i += heff_u8_decode(x->s + i, x->len - i, &u);
  heff_drop(a);
  return MK_INT(n);
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

// (an array held only here is set in place: it is not seen again; out of
// range, the array as it is)
static V heff_arraySet(V a, V i, V x) {
  Arr *r = (Arr *)a;
  int64_t k = INT_OF(i);
  if (!IS_INT(i) || k < 0 || k >= r->n) {
    heff_drop(i);
    heff_drop(x);
    return a;
  }
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
    l = heff_tag_n(TAG_Cons, 2, f);
  }
  heff_drop(a);
  return l;
}

// Bytes
// ---------------------------------------------------------------------

// (`Bytes` (lib/bytes.hf), never changed once made: a string's `Str`, of
// the kind `K_BYT` -- dropped, copied for a thread and grown by an append
// as a string is, and shown as `b"..."`.  Their length, slice and
// equality are a string's; a byte is a `UInt8`, a word of its value)
static Str *heff_bytes_new(int64_t len) {
  Str *s = heff_str_new(len);
  s->h.kind = K_BYT;
  return s;
}

static V heff_bytesLength(V a) { return heff_strLength(a); }

// (the byte at `i`; out of them, abandoned, as `bytes`'s `index` never
// asks)
static V heff_bytesAt(V a, V i) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  if (!IS_INT(i) || k < 0 || k >= x->len) return heff_abandon();
  V r = MK_INT(x->s[k]);
  heff_drop(a);
  return r;
}

static V heff_bytesSlice(V a, V i, V j) {
  V s = heff_strSlice(a, i, j);
  OBJ(s)->kind = K_BYT;
  return s;
}

static V heff_bytesAppend(V a, V b) { return heff_buf_append(a, b, K_BYT); }

static V heff_bytesEq(V a, V b) { return heff_strEq(a, b); }

static V heff_bytesFromList(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  Str *s = heff_bytes_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) s->s[k++] = (uint8_t)INT_OF(heff_field(c, 0));
  heff_drop(l);
  return (V)s;
}

static V heff_bytesToList(V a) {
  Str *x = (Str *)a;
  V l = NTAG(TAG_Nil);
  for (int64_t k = x->len - 1; k >= 0; k--) {
    V f[2] = { MK_INT(x->s[k]), l };
    l = heff_tag_n(TAG_Cons, 2, f);
  }
  heff_drop(a);
  return l;
}

// (a string's bytes, as they are)
static V heff_bytesFromString(V a) {
  Str *x = (Str *)a, *s = heff_bytes_new(x->len);
  if (x->len) memcpy(s->s, x->s, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

// (their text: what is not UTF-8 in them U+FFFD, as TextDecoder reads it)
static V heff_bytesToString(V a) {
  Str *x = (Str *)a;
  V s = heff_str_valid(x->s, x->len);
  heff_drop(a);
  return s;
}

// What providers share
// ---------------------------------------------------------------------

static V heff_just(V x) { return heff_tag_n(TAG_Just, 1, &x); }
#define heff_nothing NTAG(TAG_Nothing)

static V heff_cons(V x, V rest) {
  V f[2] = { x, rest };
  return heff_tag_n(TAG_Cons, 2, f);
}

// A string crosses between the program and the system through these
// alone: a provider neither reads a `Str`'s bytes nor makes one of its
// own, and the runtime alone knows how a string is kept.  Which of them
// it crosses by is what it is to the system:
//
// - content -- a file's, a stream's, a pipe's, what a command wrote,
//   what was fetched -- is bytes, as they are: in by `heff_str_bytes`,
//   out by `heff_put` or `heff_bytes`;
// - a path is bytes too: out as a C string (`heff_cstr`), and a
//   directory's names in (`heff_strs_bytes`);
// - a name -- an argument of the program, the environment's, the
//   working directory, the program's path, a command and its
//   arguments -- and what a host of text has (JSON, a terminal's
//   lines) are text: in by `heff_str_text` (`heff_str_text_n`,
//   `heff_strs_text`), out by `heff_cstr_text`.
//
// A C string out is malloc'd, or NULL where the string cannot be one --
// it has a NUL in it -- and the operation then fails (its `Nothing`, its
// `False`).  Content and paths are the string's bytes, as they are;
// text is made valid, what is not UTF-8 in it U+FFFD, both in and out,
// as the JavaScript backend's names are (Node.js has an argument, the
// environment, as text alone).

// (content in, `n` bytes, as a string)
static V heff_str_bytes(const void *c, int64_t n) { return heff_str_of(c, n); }

// (text in, `n` bytes of it, as a string)
static V heff_str_text_n(const void *c, int64_t n) { return heff_str_valid(c, n); }

// (a C string of text in, as a string: a name)
static V heff_str_text(const char *c) { return heff_str_text_n(c, (int64_t)strlen(c)); }

// (C strings, a directory's names, as a list of strings)
static V heff_strs_bytes(char **xs, int64_t n) {
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(heff_str_bytes(xs[i], (int64_t)strlen(xs[i])), l);
  return l;
}

// (C strings of text, names, as a list of strings)
static V heff_strs_text(char **xs, int64_t n) {
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(heff_str_text(xs[i]), l);
  return l;
}

// (a path, as a C string to give the system: malloc'd, or NULL)
static char *heff_cstr(V v) {
  Str *s = (Str *)v;
  if (s->len && memchr(s->s, 0, (size_t)s->len)) return NULL;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->s, (size_t)s->len);
  c[s->len] = 0;
  return c;
}

// (a name or text, as a C string to give the system: malloc'd, or NULL)
static char *heff_cstr_text(V v) {
  Str *s = (Str *)v;
  if (s->len && memchr(s->s, 0, (size_t)s->len)) return NULL;
  int own;
  heff_u8_valid_len(s->s, s->len, &own);
  if (own) return heff_cstr(v);
  Str *t = (Str *)heff_str_valid(s->s, s->len);
  char *c = heff_cstr((V)t);
  heff_drop((V)t);
  return c;
}

// (content out, malloc'd, `*n` bytes, a NUL after them)
static char *heff_bytes(V v, size_t *n) {
  Str *s = (Str *)v;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->s, (size_t)s->len);
  c[s->len] = 0;
  *n = (size_t)s->len;
  return c;
}

// (content written out to `f`)
static void heff_put(FILE *f, V v) {
  Str *s = (Str *)v;
  if (s->len) fwrite(s->s, 1, (size_t)s->len, f);
}

// `Bytes` cross by these alone, as they are: in, `n` bytes copied, by
// `heff_bytes_in`; out, read where they are kept, `*n` of them, by
// `heff_bytes_data` (the value not consumed, its bytes there as long as
// it is held, and never written to).
static V heff_bytes_in(const void *c, int64_t n) {
  Str *s = heff_bytes_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

static const uint8_t *heff_bytes_data(V v, int64_t *n) {
  Str *s = (Str *)v;
  *n = s->len;
  return s->s;
}

// (C strings' order, for qsort)
static int heff_cmp_cstr(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// (a path joined to a name in it, malloc'd)
static char *heff_join(const char *dir, const char *name) {
  size_t a = strlen(dir), b = strlen(name);
  char *p = heff_alloc(a + b + 2);
  memcpy(p, dir, a);
#ifdef _WIN32
  p[a] = '\\';
#else
  p[a] = '/';
#endif
  memcpy(p + a + 1, name, b + 1);
  return p;
}

// (an answer, for an operation's function: given it, its value asked for)
static V heff_answer(V f, V x) { return heff_tr(heff_app1(f, x)); }

// The world: the things the providers hand out, held in one table for
// them all, so that one effect's provider takes what another's handed
// out (lib/std/handle.hf: a `Handle` is `MkHandle i`, made and taken
// apart by the provider), as src/heff/compile/emit.hf's `$world` holds them.
// `heff_hold(x, free)` is the number of `x`, the same while it is held;
// `heff_held(i)` the thing numbered `i`; `heff_release(i)` lets it go
// -- `free(x)`, if its holder gave a `free` -- and its number is no
// one's again.  (Numbers by address, `heff_numbers`: open addressing, a
// slot -1 where it is empty, never emptied but where all are numbered
// again, the things released left out.)
typedef struct { void *x; void (*free)(void *); } HeffThing;
HEFF_VAR(HeffThing *heff_things);
HEFF_VAR(int64_t heff_nthings);
HEFF_VAR(int64_t heff_capthings);
HEFF_VAR(int64_t *heff_numbers);
HEFF_VAR(int64_t heff_capnumbers);
HEFF_VAR(int64_t heff_usednumbers);

// (the slot of `x`'s number, or the empty slot it would be in)
static int64_t heff_number_slot(void *x) {
  uint64_t h = (uint64_t)(uintptr_t)x;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  int64_t m = heff_capnumbers - 1, i = (int64_t)(h & (uint64_t)m);
  while (heff_numbers[i] >= 0 && heff_things[heff_numbers[i]].x != x) i = (i + 1) & m;
  return i;
}

// (the things held, numbered again: in four times as many slots, and more)
static void heff_renumber(void) {
  int64_t live = 0;
  for (int64_t n = 0; n < heff_nthings; n++) live += heff_things[n].x != NULL;
  int64_t cap = 16;
  while (cap < live * 4) cap *= 2;
  free(heff_numbers);
  heff_numbers = heff_alloc(cap * sizeof(int64_t));
  for (int64_t i = 0; i < cap; i++) heff_numbers[i] = -1;
  heff_capnumbers = cap;
  heff_usednumbers = live;
  for (int64_t n = 0; n < heff_nthings; n++)
    if (heff_things[n].x) heff_numbers[heff_number_slot(heff_things[n].x)] = n;
}

// (threads at once: the table theirs, each holding it while it looks
// at it -- what a thing is, and when it is let go, still their own
// providers' to say)
#ifdef HEFF_THREADS
#ifdef HEFF_PART
extern pthread_mutex_t heff_world_mu;
#else
pthread_mutex_t heff_world_mu = PTHREAD_MUTEX_INITIALIZER;
#endif
#define HEFF_WORLD_LOCK() pthread_mutex_lock(&heff_world_mu)
#define HEFF_WORLD_UNLOCK() pthread_mutex_unlock(&heff_world_mu)
#else
#define HEFF_WORLD_LOCK() ((void)0)
#define HEFF_WORLD_UNLOCK() ((void)0)
#endif

static int64_t heff_hold(void *x, void (*let_go)(void *)) {
  if (!x) heff_fail("the world holds nothing at NULL");
  HEFF_WORLD_LOCK();
  if (heff_usednumbers * 2 >= heff_capnumbers) heff_renumber();
  int64_t s = heff_number_slot(x), n = heff_numbers[s];
  if (n < 0) {
    if (heff_nthings == heff_capthings) {
      heff_capthings = heff_capthings ? heff_capthings * 2 : 16;
      HeffThing *t = heff_alloc(heff_capthings * sizeof(HeffThing));
      if (heff_nthings) memcpy(t, heff_things, heff_nthings * sizeof(HeffThing));
      free(heff_things);
      heff_things = t;
    }
    heff_things[heff_nthings].x = x;
    heff_things[heff_nthings].free = let_go;
    heff_numbers[s] = heff_nthings;
    heff_usednumbers++;
    n = heff_nthings++;
  }
  HEFF_WORLD_UNLOCK();
  return n;
}

static void heff_held_none(int64_t i) {
  static HEFF_TLS char why[64];
  snprintf(why, sizeof why, "the world holds nothing numbered %lld", (long long)i);
  heff_fail(why);
}

static void *heff_held(int64_t i) {
  HEFF_WORLD_LOCK();
  void *x = i < 0 || i >= heff_nthings ? NULL : heff_things[i].x;
  HEFF_WORLD_UNLOCK();
  if (!x) heff_held_none(i);
  return x;
}

static void heff_release(int64_t i) {
  HEFF_WORLD_LOCK();
  void *x = i < 0 || i >= heff_nthings ? NULL : heff_things[i].x;
  void (*let_go)(void *) = x ? heff_things[i].free : NULL;
  if (x) {
    heff_things[i].x = NULL;
    heff_things[i].free = NULL;
  }
  HEFF_WORLD_UNLOCK();
  if (!x) heff_held_none(i);
  if (let_go) let_go(x);
}

// Threads a provider starts (#36; docs/threads.md): each runs what it
// is given -- a copy of it, `heff_copy_for_thread` -- by `heff_try`, so
// that a failure there is the thread's to hand back, not the end of the
// process; and ends by `heff_thread_end`.
// ---------------------------------------------------------------------

// (`f x`, asked for here, a failure caught: 0, its value at `*out`; or
// why it failed -- "heff:abandon" where it abandoned (`heff_abandon`),
// the thread's own string otherwise, to be copied before the thread
// ends.  What was made before the failure is leaked, not given back)
static const char *heff_try(V f, V x, V *out) {
  jmp_buf here;
  jmp_buf *volatile up = heff_catch;
#ifdef HEFF_THREADS
  volatile size_t open = heff_nopen;
#endif
  heff_catch = &here;
  if (setjmp(here) == 0) {
    *out = heff_tr(heff_app1(f, x));
    heff_catch = up;
    return NULL;
  }
  heff_catch = up;
#ifdef HEFF_THREADS
  heff_reopen(open);
#endif
  return heff_caught;
}

// (a failure handed back, as the thread it was caught in would have
// failed: `why` from `heff_try`, copied)
static void heff_fail_again(const char *why) {
  if (strcmp(why, "heff:abandon") == 0) heff_abandon();
  heff_fail(why);
}

// (this thread's free lists and what else the runtime keeps for it given
// back to the system, as it ends)
static void heff_thread_end(void) {
#if defined(HEFF_THREADS) || SMALL_MAX == 0
  for (size_t c = 0; c <= SMALL_MAX / 8; c++) {
    void *p = heff_free_list[c];
    while (p) {
      void *next = *(void **)p;
      free(p);
      p = next;
    }
    heff_free_list[c] = NULL;
  }
#endif
  free(heff_dead);
  heff_dead = NULL;
  heff_ndead = heff_cdead = 0;
#ifdef HEFF_THREADS
  free(heff_open);
  heff_open = NULL;
  heff_nopen = heff_capopen = 0;
#endif
}

// (a thread started, as the program's own is, with a large stack: heff's
// recursion is the C stack's -- 0, or the system's error)
#ifndef HEFF_THREAD_STACK
#define HEFF_THREAD_STACK ((size_t)1 << 28)
#endif
static int heff_thread_start(pthread_t *t, void *(*run)(void *), void *u) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HEFF_THREAD_STACK);
  int r = pthread_create(t, &attr, run, u);
  pthread_attr_destroy(&attr);
  return r;
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

// (the names of the tags and labels, each by its number: a tag's and a
// label's number is a hash of its name, src/heff/compile/emitc.hf)
typedef struct { uint32_t id; const char *name; } HeffName;
HEFF_VAR(const HeffName *heff_tag_names);
HEFF_VAR(int heff_ntag_names);
HEFF_VAR(const HeffName *heff_lbl_names);
HEFF_VAR(int heff_nlbl_names);

static const char *heff_name_of(const HeffName *t, int n, uint32_t id) {
  for (int i = 0; i < n; i++) if (t[i].id == id) return t[i].name;
  return "?";
}

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

// Commands
// ---------------------------------------------------------------------

#ifdef _WIN32
// (a command line of `as`, each quoted as the C runtime's parser reads it
// back: malloc'd)
static char *heff_command_line(char **as) {
  Buf b = {0};
  for (char **a = as; *a; a++) {
    if (a != as) buf_put(&b, " ", 1);
    const char *s = *a;
    if (*s && !strpbrk(s, " \t\n\v\"")) { buf_str(&b, s); continue; }
    buf_put(&b, "\"", 1);
    for (;;) {
      size_t slashes = 0;
      while (*s == '\\') { s++; slashes++; }
      if (!*s) {
        for (size_t i = 0; i < slashes * 2; i++) buf_put(&b, "\\", 1);
        break;
      }
      size_t k = *s == '"' ? slashes * 2 + 1 : slashes;
      for (size_t i = 0; i < k; i++) buf_put(&b, "\\", 1);
      buf_put(&b, s++, 1);
    }
    buf_put(&b, "\"", 1);
  }
  buf_put(&b, "", 1);
  return b.s;
}

typedef struct { HANDLE h; Buf *b; } HeffDrain;

// (all a pipe gives, until it is closed)
static DWORD WINAPI heff_drain(LPVOID u) {
  HeffDrain *d = u;
  char chunk[65536];
  DWORD got;
  while (ReadFile(d->h, chunk, sizeof chunk, &got, NULL) && got > 0) buf_put(d->b, chunk, got);
  return 0;
}

// (a command started, its stdin this program's, its stdout and stderr
// `out` and `err`, or 0 if it cannot be)
static HANDLE heff_spawn(char **as, HANDLE out, HANDLE err) {
  char *line = heff_command_line(as);
  STARTUPINFOA si;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi;
  int ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  free(line);
  if (!ok) return 0;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

// (a command's exit code once it has ended)
static int64_t heff_wait(HANDLE h) {
  DWORD code = (DWORD)-1;
  WaitForSingleObject(h, INFINITE);
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return (int64_t)code;
}
#else
// (what a command writes, both streams read together, so that neither
// fills its pipe while the other is waited for)
static void heff_read_both(int out, int err, Buf *bo, Buf *be) {
  struct pollfd fds[2] = { { out, POLLIN, 0 }, { err, POLLIN, 0 } };
  int open = 2;
  char chunk[65536];
  while (open > 0) {
    if (poll(fds, 2, -1) < 0) break;
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP))) continue;
      ssize_t got = read(fds[i].fd, chunk, sizeof chunk);
      if (got <= 0) { close(fds[i].fd); fds[i].fd = -1; open--; }
      else buf_put(i == 0 ? bo : be, chunk, (size_t)got);
    }
  }
}

#endif

// (the command `as` (a NULL-ended array, its name first, found on the
// PATH) run, its stdin this program's, what it writes to stdout and
// stderr gathered in `bo` and `be`: its exit status, -1 if it could not
// be run)
static int64_t heff_capture(char **as, Buf *bo, Buf *be) {
  int64_t status = -1;
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
  HANDLE or_, ow, er, ew;
  if (CreatePipe(&or_, &ow, &sa, 0) && CreatePipe(&er, &ew, &sa, 0)) {
    SetHandleInformation(or_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(er, HANDLE_FLAG_INHERIT, 0);
    HANDLE h = heff_spawn(as, ow, ew);
    CloseHandle(ow);
    CloseHandle(ew);
    HeffDrain de = { er, be }, dout = { or_, bo };
    HANDLE t = CreateThread(NULL, 0, heff_drain, &de, 0, NULL);
    heff_drain(&dout);
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    else heff_drain(&de);
    CloseHandle(or_);
    CloseHandle(er);
    if (h) status = heff_wait(h);
  }
#else
  int po[2], pe[2];
  if (pipe(po) == 0 && pipe(pe) == 0) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    posix_spawn_file_actions_addclose(&fa, po[0]);
    posix_spawn_file_actions_addclose(&fa, pe[0]);
    pid_t pid;
    int ok = posix_spawnp(&pid, as[0], &fa, NULL, as, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    close(po[1]);
    close(pe[1]);
    heff_read_both(po[0], pe[0], bo, be);
    if (ok) {
      int st;
      if (waitpid(pid, &st, 0) >= 0 && WIFEXITED(st)) status = WEXITSTATUS(st);
    }
  }
#endif
  return status;
}

// (a string as JSON writes one: in quotes, `"` `\` and controls escaped;
// what is not UTF-8 in it U+FFFD -- or, `shown`, each of its bytes
// `\xnn`, as a string literal writes it, test/load-js.mjs's `show`)
static void buf_json_string_as(Buf *b, V v, int shown) {
  Str *s = (Str *)v;
  buf_str(b, "\"");
  for (int64_t i = 0; i < s->len; i++) {
    uint32_t u = s->s[i];
    char tmp[16];
    if (u >= 0x80) {
      int w = heff_u8_decode(s->s + i, s->len - i, &u);
      if (heff_u8_own(s->s + i, u, w)) buf_put(b, (const char *)s->s + i, (size_t)w);
      else if (!shown) buf_str(b, "\xEF\xBF\xBD");
      else for (int k = 0; k < w; k++) { snprintf(tmp, sizeof tmp, "\\x%02x", s->s[i + k]); buf_str(b, tmp); }
      i += w - 1;
    }
    else if (u == '"') buf_str(b, "\\\"");
    else if (u == '\\') buf_str(b, "\\\\");
    else if (u == '\n') buf_str(b, "\\n");
    else if (u == '\t') buf_str(b, "\\t");
    else if (u == '\r') buf_str(b, "\\r");
    else if (u == '\b') buf_str(b, "\\b");
    else if (u == '\f') buf_str(b, "\\f");
    else if (u < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", u); buf_str(b, tmp); }
    else { char o = (char)u; buf_put(b, &o, 1); }
  }
  buf_str(b, "\"");
}

static void buf_json_string(Buf *b, V v) { buf_json_string_as(b, v, 0); }

// (a tag of that name and that many arguments, its arguments `*fs`)
static int heff_tag_named(V v, const char *name, int n, V **fs) {
  int tag, m = 0;
  V *f = NULL;
  if (IS_NTAG(v)) tag = (int)(v >> 2);
  else if (IS_OBJ(v) && OBJ(v)->kind == K_TAG) { tag = OBJ(v)->tag; m = (int)((Tag *)v)->n; f = ((Tag *)v)->f; }
  else return 0;
  if (m != n || strcmp(heff_name_of(heff_tag_names, heff_ntag_names, (uint32_t)tag), name)) return 0;
  *fs = f;
  return 1;
}

// (a list, `Cons a (Cons b Nil)`: whether it is one, a chain that ends
// in `Nil`)
static int heff_is_list(V v) {
  V *f;
  while (heff_tag_named(v, "Cons", 2, &f)) v = f[1];
  return heff_tag_named(v, "Nil", 0, &f);
}

// (bytes as `bytes`'s `show` writes them, `b"..."`: a byte of ASCII that
// is printed as it is, `"`, `\` and three controls escaped, any other
// `\xff`)
static void heff_show_bytes(Buf *b, V v) {
  Str *s = (Str *)v;
  buf_str(b, "b\"");
  for (int64_t i = 0; i < s->len; i++) {
    uint8_t c = s->s[i];
    char tmp[8];
    if (c == '"') buf_str(b, "\\\"");
    else if (c == '\\') buf_str(b, "\\\\");
    else if (c == '\n') buf_str(b, "\\n");
    else if (c == '\r') buf_str(b, "\\r");
    else if (c == '\t') buf_str(b, "\\t");
    else if (c >= 0x20 && c < 0x7f) { char o = (char)c; buf_put(b, &o, 1); }
    else { snprintf(tmp, sizeof tmp, "\\x%02x", c); buf_str(b, tmp); }
  }
  buf_str(b, "\"");
}

static void heff_show(Buf *b, V v, int arg) {
  char tmp[64];
  if (heff_is_list(v)) {
    V *f;
    buf_str(b, "[");
    for (int i = 0; heff_tag_named(v, "Cons", 2, &f); i++, v = f[1]) {
      if (i) buf_str(b, ", ");
      heff_show(b, f[0], 0);
    }
    buf_str(b, "]");
    return;
  }
  if (IS_INT(v) || (IS_OBJ(v) && OBJ(v)->kind == K_BIG)) {
    char *c = heff_int_cstr(v);
    if (arg && c[0] == '-') { buf_str(b, "("); buf_str(b, c); buf_str(b, ")"); }
    else buf_str(b, c);
    free(c);
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
    case K_STR: buf_json_string_as(b, v, 1); return;
    case K_BYT: heff_show_bytes(b, v); return;
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
        buf_str(b, heff_name_of(heff_lbl_names, heff_nlbl_names, r->lbl[i]));
        buf_str(b, " = ");
        V x = heff_rec_dup_field(r, i);
        heff_show(b, x, 0);
        heff_drop(x);
      }
      buf_str(b, " }");
      return;
    }
    default: buf_str(b, "#fun"); return;
    }
  }
  const char *name = heff_name_of(heff_tag_names, heff_ntag_names, (uint32_t)tag);
  if (n && arg) buf_str(b, "(");
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
    V s = heff_str_bytes(b.s, (int64_t)b.n);
    buf_json_string(out, s);
    heff_drop(s);
    buf_str(out, "}");
  } else {
    buf_str(out, "{\"threw\": ");
    buf_json_string(out, heff_str_ascii(heff_caught));
    buf_str(out, "}");
  }
  heff_catch = NULL;
}

HEFF_VAR(int heff_argc);
HEFF_VAR(char **heff_argv);
HEFF_VAR(int heff_status);

#ifndef HEFF_PART

// (the program's stack, which heff's recursion is: a call last a jump,
// and a call of itself put in a tag a loop (src/heff/compile/trmc.hf),
// but what is left is as deep as it goes -- `-DHEFF_MAIN_STACK=BYTES`
// for another size)
#ifndef HEFF_MAIN_STACK
#define HEFF_MAIN_STACK ((size_t)1 << 30)
#endif

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

// (where the program's stack ran out, said: a fault at the bottom of its
// stack, on a stack of the handler's own, is an overflow -- any other
// fault the system's, as without the handler.  Not where a sanitizer
// handles faults itself)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define HEFF_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HEFF_SANITIZED 1
#endif
#if !defined(_WIN32) && !defined(HEFF_SANITIZED)
static char *heff_stack_lo;

static void heff_on_fault(int sig, siginfo_t *si, void *ctx) {
  (void)ctx;
  char *a = (char *)si->si_addr;
  if (heff_stack_lo && a >= heff_stack_lo - ((size_t)1 << 20) && a < heff_stack_lo + ((size_t)1 << 16)) {
    static const char why[] = "heff: the stack overflowed\n";
    fflush(stdout);
    if (write(2, why, sizeof why - 1) < 0) _exit(1);
    _exit(1);
  }
  signal(sig, SIG_DFL);
}

static void heff_watch_stack(void) {
  char here;
  heff_stack_lo = &here - HEFF_MAIN_STACK;
  stack_t ss;
  ss.ss_size = 1 << 16;
  ss.ss_sp = malloc(ss.ss_size);
  ss.ss_flags = 0;
  if (!ss.ss_sp || sigaltstack(&ss, NULL) != 0) return;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = heff_on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}
#else
static void heff_watch_stack(void) {}
#endif

static void *heff_thread(void *u) {
  (void)u;
  heff_watch_stack();
  heff_tr(heff_entry());
  return NULL;
}
#endif

#ifdef _WIN32
// (the program's thread on Windows: its large stack reserved, not
// committed, and its bytes in and out as they are, not as text)
static DWORD WINAPI heff_win_thread(LPVOID u) {
  heff_thread(u);
  return 0;
}

int main(int argc, char **argv) {
  heff_argc = argc; heff_argv = argv;
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  SetConsoleOutputCP(CP_UTF8);
  HANDLE t = CreateThread(NULL, (SIZE_T)HEFF_MAIN_STACK, heff_win_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!t) heff_fail("cannot start");
  WaitForSingleObject(t, INFINITE);
  fflush(stdout);
  return heff_status;
}
#else
int main(int argc, char **argv) {
  heff_argc = argc; heff_argv = argv;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HEFF_MAIN_STACK);
  pthread_t t;
  if (pthread_create(&t, &attr, heff_thread, NULL) != 0) heff_fail("cannot start");
  pthread_join(t, NULL);
  fflush(stdout);
  return heff_status;
}
#endif
#endif
