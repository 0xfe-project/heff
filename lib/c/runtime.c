// heff's runtime for the C backend, bundled whole
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
// helpers ("What providers share" says which).  `Bytes`
// are kept as a string is, a `Str` of another kind (`K_BYT`), and cross
// by helpers of their own.
//
// An object is counted (Perceus): each owner of it holds a reference,
// `heff_dup` takes one more and `heff_drop` gives one back, the last
// freeing it.  What a function is given it owns: it consumes it (gives
// it to what it calls, keeps it in what it builds) or drops it -- the
// compiler says where; every
// primitive and the runtime's calls consume their arguments too.  What
// is static (a string written in the program, a definition's closure) is
// immortal, its count never changed.  A program runs in a thread of its
// own with a large stack.  A call last of an unknown
// function counts itself and every so often gives back a bounce (`K_BNC`),
// let go of where the value is asked for (`heff_tr`), as the JavaScript
// backend does -- unless it is a jump
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
// others)
#ifdef HEFF_PART
#define HEFF_VAR(decl) extern decl
#else
#define HEFF_VAR(decl) decl
#endif

// (the runtime's functions, but the `static inline` ones: in a program
// split into parts by an emitter that says so (`HEFF_SPLIT`, which it
// puts first), defined once too, in the first part, and in the others
// C's `inline` alone, which makes no function of a definition, and
// `noinline`, which keeps it from being copied into its callers: each a
// call to the first part's -- not a copy of each in each part, 2.6 MB
// of heff's own 15 MB of code, which ran no slower for it (#69); in a
// program of one file, or of an emitter from before, each the file's
// own)
#if defined(HEFF_SPLIT) && defined(HEFF_PART)
#define HEFF_FN inline __attribute__((noinline))
#elif defined(HEFF_SPLIT)
#define HEFF_FN
#else
#define HEFF_FN static
#endif

// Threads (#36).  A program runs in one thread of its own, unless a
// provider runs it in more, truly at once: such a provider says so by
// `#define HEFF_THREADS 1`, a line of its own, which the emitter puts
// before the runtime too (`-DHEFF_THREADS` does the same).  Then each
// thread has the runtime's per-run state for itself (`HEFF_TLS`: its
// pages, its stack of what is given back, its count of calls and
// its room, where a failure of it is caught).  An object is one thread's,
// counted by it alone, not atomically; or shared (`SHARED`, #66),
// counted atomically by every thread that holds it; or immortal,
// counted by none.  What a provider gives another thread it shares
// (`heff_share`), and runs there under `heff_try`, ending the thread by
// `heff_thread_end`.  A definition's value is computed once, under a
// lock, and made immortal (`heff_global`, `heff_freeze`), as is a static
// thunk's (`heff_force_shared`).  Without `HEFF_THREADS`, all of this is
// as one thread has it: `HEFF_TLS` is nothing, no object is shared
// (`SHARED` is 0), and nothing is locked.
#ifdef HEFF_THREADS
#define HEFF_TLS _Thread_local
#else
#define HEFF_TLS
#endif

typedef uintptr_t V;

// (a tuple (#80): what a worker gives back of a
// tag or a record it built, not built -- each value a `V`, or a flat
// number's bits as a flat field's; given back in registers where the C
// ABI lets it, else in its caller's frame, never allocated)
typedef struct { V f[1]; } HeffT1;
typedef struct { V f[2]; } HeffT2;
typedef struct { V f[3]; } HeffT3;
typedef struct { V f[4]; } HeffT4;

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
#define HEFF_SLOW HEFF_FN __attribute__((noinline, cold))
#define HEFF_LIKELY(c) __builtin_expect(!!(c), 1)

// the tags the runtime knows; a program's own are numbered after them
enum {
  TAG_True, TAG_False, TAG_Nil, TAG_Cons, TAG_MkUnit, TAG_Just, TAG_Nothing,
  TAG_MkPair, TAG_InL, TAG_InR, TAG_KNOWN
};

// (`K_TAGF`, a tag with flat fields (#64): one without is given back as
// ever, nothing looked for in it)
enum { K_TAG, K_REC, K_CLO, K_PAP, K_STR, K_FLT, K_ARR, K_BNC, K_CELL, K_BIG, K_BYT, K_TAGF, K_BIT };

typedef struct Obj {
  uint32_t rc;
  uint16_t kind;
  uint16_t tag;
} Obj;

typedef V (*Fn)(V self, V *args);

// (a tag: its fields, `n` of them; `flat`, which of its first 16 are
// flat (#64), as a record's are, below)
typedef struct { Obj h; int32_t n; uint32_t flat; V f[]; } Tag;

// (a tag's size and its flat fields, said at once: one word)
static inline void heff_tag_head(Tag *t, int64_t n, uint32_t flat) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  uint64_t w = (uint64_t)(uint32_t)n | ((uint64_t)flat << 32);
  memcpy(&t->n, &w, sizeof w);
#else
  t->n = (int32_t)n; t->flat = flat;
#endif
}
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
// (`Bytes` written in the program, a binary written whole, `b["hi", 255]`,
// static as a string is)
#define HEFF_BYTES(name, n, bytes) HEFF_STATIC_STR(name, K_BYT, n, bytes)
typedef struct { Obj h; double d; } Flt;
// (an array: its elements, `n` of them, `V`s or flat -- a number's C
// type, as its header's `tag` says, `HEFF_ARR_V` or another of these
// ("Arrays", below))
typedef struct { Obj h; int64_t n; V a[]; } Arr;
enum {
  HEFF_ARR_V, HEFF_ARR_F64, HEFF_ARR_I64, HEFF_ARR_U64, HEFF_ARR_I32, HEFF_ARR_U32,
  HEFF_ARR_I16, HEFF_ARR_U16, HEFF_ARR_I8, HEFF_ARR_U8, HEFF_ARR_F32
};
// (the bytes an element of each takes)
static const uint8_t heff_arr_width[] = { sizeof(V), 8, 8, 8, 4, 4, 2, 2, 1, 1, 4 };
#define heff_arr_bytes(r) (sizeof(Arr) + (size_t)(r)->n * heff_arr_width[(r)->h.tag])
// (the elements an object's walk sees: a flat array's none)
#define heff_arr_slots(r) ((r)->h.tag == HEFF_ARR_V ? (r)->n : 0)
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

// (an object shared between threads (#66, `heff_share`): counted by
// them all, atomically, and freed by the one that lets it go last.
// What a shared object holds is shared or immortal; a thread's own
// object may hold a shared one, not the other way.  Without
// `HEFF_THREADS`, no object is: `SHARED` is 0, and a count is read and
// changed as it always was.  Threads: a count is read atomically too,
// relaxed -- a plain load, to the processor -- since another thread may
// change a shared one meanwhile)
#ifdef HEFF_THREADS
#define SHARED 0x40000000u
#define heff_rc(o) __atomic_load_n(&(o)->rc, __ATOMIC_RELAXED)
#else
#define SHARED 0u
#define heff_rc(o) ((o)->rc)
#endif
// (an object's count but its marks)
#define HEFF_COUNT(rc) ((rc) & ~(IMMORTAL | SHARED))

HEFF_FN void heff_free(V v);

#ifdef HEFF_THREADS
// (a shared object counted, out of line: the fast paths below test both
// marks at once, where one thread tests `IMMORTAL` alone)
HEFF_FN __attribute__((noinline)) void heff_dup_shared(Obj *o) {
  if (!(heff_rc(o) & IMMORTAL)) __atomic_fetch_add(&o->rc, 1, __ATOMIC_RELAXED);
}

// (... counted down: whether that was its last reference, which this
// thread then frees -- acquiring what every other thread did with it
// before it let it go.  One made immortal meanwhile (`heff_freeze`, of
// a static thunk's value) is never its last: what made it so holds it)
HEFF_FN __attribute__((noinline)) int heff_down_shared(Obj *o) {
  if (heff_rc(o) & IMMORTAL) return 0;
  uint32_t was = __atomic_fetch_sub(&o->rc, 1, __ATOMIC_ACQ_REL);
  return !(was & IMMORTAL) && HEFF_COUNT(was) == 1;
}

static inline V heff_dup(V v) {
  if (IS_OBJ(v)) {
    uint32_t rc = heff_rc(OBJ(v));
    if (HEFF_LIKELY(!(rc & (IMMORTAL | SHARED)))) OBJ(v)->rc = rc + 1;
    else heff_dup_shared(OBJ(v));
  }
  return v;
}

// (a count taken down: whether that was its last reference -- never of
// an immortal object)
static inline int heff_down(Obj *o) {
  uint32_t rc = heff_rc(o);
  if (HEFF_LIKELY(!(rc & (IMMORTAL | SHARED)))) return (o->rc = rc - 1) == 0;
  return heff_down_shared(o);
}

// (a count taken down where it is not the last unless the object is
// shared -- the caller knows it held more than once, or immortal: the
// last of a shared one frees it)
static inline void heff_unref(V v) {
  if (heff_down(OBJ(v))) heff_free(v);
}
#else
static inline V heff_dup(V v) {
  if (IS_OBJ(v) && !(OBJ(v)->rc & IMMORTAL)) OBJ(v)->rc++;
  return v;
}

static inline int heff_down(Obj *o) {
  return !(o->rc & IMMORTAL) && --o->rc == 0;
}

static inline void heff_unref(V v) {
  if (!(OBJ(v)->rc & IMMORTAL)) OBJ(v)->rc--;
}
#endif

static inline void heff_drop(V v) {
  if (IS_OBJ(v) && heff_down(OBJ(v))) heff_free(v);
}

static inline int heff_unique(V v) { return heff_rc(OBJ(v)) == 1; }

// (where a value is asked for that may fail -- `heff compile` without
// `--name`, each definition printed -- a failure is caught there, and
// said; otherwise it ends the program)
HEFF_VAR(HEFF_TLS jmp_buf *heff_catch);
HEFF_VAR(HEFF_TLS const char *heff_caught);

HEFF_FN void heff_fail(const char *why) {
  if (heff_catch) { heff_caught = why; longjmp(*heff_catch, 1); }
  fflush(stdout);
  fprintf(stderr, "heff: %s\n", why);
  exit(1);
}

HEFF_FN V heff_abandon(void) {
  if (heff_catch) { heff_caught = "heff:abandon"; longjmp(*heff_catch, 1); }
  fflush(stdout);
  fprintf(stderr, "heff:abandon\n");
  exit(1);
}

HEFF_FN V heff_nomatch(V s);

HEFF_FN void *heff_alloc(size_t n) {
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

// (each object made counted, with its bytes, where a benchmark is built
// (`-DHEFF_COUNT_ALLOCS`, as `heff bench` builds one): what its allocs/op
// and B/op are of.  Elsewhere nothing, not a counter kept)
#ifdef HEFF_COUNT_ALLOCS
HEFF_VAR(HEFF_TLS uint64_t heff_allocs);
HEFF_VAR(HEFF_TLS uint64_t heff_alloc_bytes);
#define HEFF_COUNTED(c) (heff_allocs++, heff_alloc_bytes += (uint64_t)(c) * 8)
#else
#define HEFF_COUNTED(c) ((void)0)
#endif

#if SMALL_MAX == 0
// (none kept: each object the system's, given back to it)
HEFF_VAR(HEFF_TLS void *heff_free_list[SMALL_MAX / 8 + 1]);

static inline void *heff_obj_alloc(size_t n) {
  size_t c = (n + 7) / 8;
  HEFF_COUNTED(c);
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
//
// (with threads, #66: a page is the thread's that made it, its owner --
// its pages are its heap, `heff_heap`, each thread's own.  What the owner
// gives back goes on the page's list, as in one thread, nothing atomic.
// What another thread gives back -- a shared object, its last holder
// another thread -- goes on the page's second list, `remote`, by a
// compare-and-swap, nothing else of the page touched; the owner takes
// that list over whole, by one exchange, where it finds no room (the
// slow path, `heff_page_alloc`).  A thread that ends gives back its pages
// that hold nothing, and leaves the others, abandoned, to whichever
// thread next finds no room in a page of their size: it adopts them)
#define HEFF_PAGE ((size_t)1 << 16)
typedef struct HeffPage {
  void *free;               // what was given back here
  char *bump, *end;         // what was never used yet
  struct HeffPage *next;    // the next of its size with room
  uint32_t c, listed;       // its size, in words; on that list
#ifdef HEFF_THREADS
  struct HeffHeap *owner;   // the thread's whose page it is, else none
  void *remote;             // what other threads gave back here
  struct HeffPage *all;     // the owner's next page of its size
#endif
} HeffPage;
#define HEFF_PAGE_OF(p) ((HeffPage *)((uintptr_t)(p) & ~(uintptr_t)(HEFF_PAGE - 1)))
#define HEFF_PAGE_FIRST(pg) ((char *)(pg) + (sizeof(HeffPage) + 15) / 16 * 16)

#ifdef HEFF_THREADS
typedef struct HeffHeap {
  HeffPage *at[SMALL_MAX / 8 + 1];       // the page made in last, each size
  HeffPage *room[SMALL_MAX / 8 + 1];     // pages with room, each size
  HeffPage *all[SMALL_MAX / 8 + 1];      // all its pages, each size
  unsigned long news[SMALL_MAX / 8 + 1]; // `heff_remote_news`, as last looked
} HeffHeap;
HEFF_VAR(HEFF_TLS HeffHeap heff_heap);
HEFF_FN void heff_page_remote(HeffPage *pg, void *p);
#define HEFF_HEAP HeffHeap *h = &heff_heap
#define HEFF_AT h->at
#define HEFF_WITH_ROOM h->room
#else
HEFF_VAR(HeffPage *heff_page_at[SMALL_MAX / 8 + 1]);
HEFF_VAR(HeffPage *heff_page_room[SMALL_MAX / 8 + 1]);
#define HEFF_HEAP
#define HEFF_AT heff_page_at
#define HEFF_WITH_ROOM heff_page_room
#endif

HEFF_FN void *heff_page_alloc(size_t c);

static inline void *heff_obj_alloc(size_t n) {
  size_t c = (n + 7) / 8;
  HEFF_COUNTED(c);
  if (c > SMALL_MAX / 8) return heff_alloc(c * 8);
  HEFF_HEAP;
  HeffPage *pg = HEFF_AT[c];
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
  HeffPage *pg = HEFF_PAGE_OF(p);
  HEFF_HEAP;
#ifdef HEFF_THREADS
  if (__builtin_expect(__atomic_load_n(&pg->owner, __ATOMIC_RELAXED) != h, 0)) {
    heff_page_remote(pg, p);
    return;
  }
#endif
  *(void **)p = pg->free;
  pg->free = p;
  if (!pg->listed && pg != HEFF_AT[pg->c]) {
    pg->listed = 1;
    pg->next = HEFF_WITH_ROOM[pg->c];
    HEFF_WITH_ROOM[pg->c] = pg;
  }
}

// (a new page, of objects of `c` words)
HEFF_FN HeffPage *heff_page_new(size_t c) {
  void *m = NULL;
#ifdef _WIN32
  m = _aligned_malloc(HEFF_PAGE, HEFF_PAGE);
#else
  if (posix_memalign(&m, HEFF_PAGE, HEFF_PAGE) != 0) m = NULL;
#endif
  if (!m) heff_fail("out of memory");
  HeffPage *pg = m;
  pg->free = NULL;
  pg->bump = HEFF_PAGE_FIRST(pg);
  pg->end = (char *)m + HEFF_PAGE;
  pg->next = NULL;
  pg->c = (uint32_t)c;
  pg->listed = 0;
  return pg;
}

#ifndef HEFF_THREADS
// (the page made in last full: another of the size with room, else a
// new one)
HEFF_FN __attribute__((noinline)) void *heff_page_alloc(size_t c) {
  HeffPage *pg = heff_page_room[c];
  if (pg) {
    heff_page_room[c] = pg->next;
    pg->listed = 0;
  } else {
    pg = heff_page_new(c);
  }
  heff_page_at[c] = pg;
  void *p = pg->free;
  if (p) { pg->free = *(void **)p; return p; }
  p = pg->bump;
  pg->bump += c * 8;
  return p;
}
#else
// (how many times a page's `remote` has had something put on it when it
// had nothing -- an owner that finds no room looks through its pages for
// what other threads gave back only where this has moved since it last
// looked; and the pages each size abandoned by threads that ended)
#ifdef HEFF_PART
extern unsigned long heff_remote_news;
extern HeffPage *heff_abandoned[SMALL_MAX / 8 + 1];
extern pthread_mutex_t heff_abandoned_mu;
#else
unsigned long heff_remote_news;
HeffPage *heff_abandoned[SMALL_MAX / 8 + 1];
pthread_mutex_t heff_abandoned_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

// (`p`, of a page another thread owns or none does, given back: on the
// page's `remote`)
HEFF_FN __attribute__((noinline)) void heff_page_remote(HeffPage *pg, void *p) {
  void *old = __atomic_load_n(&pg->remote, __ATOMIC_RELAXED);
  do *(void **)p = old;
  while (!__atomic_compare_exchange_n(&pg->remote, &old, p, 1, __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED));
  if (!old) __atomic_fetch_add(&heff_remote_news, 1, __ATOMIC_RELEASE);
}

// (what other threads gave back to `pg` taken over by its owner, onto its
// own list: whether there was any)
HEFF_FN int heff_page_take(HeffPage *pg) {
  if (!__atomic_load_n(&pg->remote, __ATOMIC_RELAXED)) return 0;
  void *r = __atomic_exchange_n(&pg->remote, NULL, __ATOMIC_ACQUIRE);
  if (!r) return 0;
  if (pg->free) {
    void *t = r;
    while (*(void **)t) t = *(void **)t;
    *(void **)t = pg->free;
  }
  pg->free = r;
  return 1;
}

// (`pg`, if it has room and is not the page made in last, on the list of
// those with room)
HEFF_FN void heff_page_list(HeffHeap *h, HeffPage *pg) {
  if (pg->listed || pg == h->at[pg->c]) return;
  if (!pg->free && pg->bump + pg->c * 8 > pg->end) return;
  pg->listed = 1;
  pg->next = h->room[pg->c];
  h->room[pg->c] = pg;
}

// (the page made in last full: what other threads gave back to it; else
// another of the size with room; else, if other threads gave back to any
// page since the thread last looked, what they gave to its own; else the
// pages of the size abandoned, adopted; else a new one)
HEFF_FN __attribute__((noinline)) void *heff_page_alloc(size_t c) {
  HeffHeap *h = &heff_heap;
  HeffPage *pg = h->at[c];
  if (!(pg && heff_page_take(pg))) {
    if (!h->room[c]) {
      unsigned long news = __atomic_load_n(&heff_remote_news, __ATOMIC_ACQUIRE);
      if (news != h->news[c]) {
        h->news[c] = news;
        for (HeffPage *q = h->all[c]; q; q = q->all)
          if (heff_page_take(q)) heff_page_list(h, q);
      }
    }
    if (!h->room[c] && __atomic_load_n(&heff_abandoned[c], __ATOMIC_RELAXED)) {
      pthread_mutex_lock(&heff_abandoned_mu);
      HeffPage *q = heff_abandoned[c];
      __atomic_store_n(&heff_abandoned[c], NULL, __ATOMIC_RELAXED);
      pthread_mutex_unlock(&heff_abandoned_mu);
      while (q) {
        HeffPage *next = q->all;
        __atomic_store_n(&q->owner, h, __ATOMIC_RELAXED);
        q->listed = 0;
        q->all = h->all[c];
        h->all[c] = q;
        heff_page_take(q);
        heff_page_list(h, q);
        q = next;
      }
    }
    pg = h->room[c];
    if (pg) {
      h->room[c] = pg->next;
      pg->listed = 0;
    } else {
      pg = heff_page_new(c);
      pg->remote = NULL;
      __atomic_store_n(&pg->owner, h, __ATOMIC_RELAXED);
      pg->all = h->all[c];
      h->all[c] = pg;
    }
    h->at[c] = pg;
  }
  void *p = pg->free;
  if (p) { pg->free = *(void **)p; return p; }
  p = pg->bump;
  pg->bump += c * 8;
  return p;
}

// (as a thread ends: its pages that hold nothing given back to the
// system, the others abandoned, for another thread to adopt)
HEFF_FN void heff_heap_end(void) {
  HeffHeap *h = &heff_heap;
  for (size_t c = 1; c <= SMALL_MAX / 8; c++) {
    HeffPage *left = NULL, *last = NULL;
    for (HeffPage *pg = h->all[c], *next; pg; pg = next) {
      next = pg->all;
      heff_page_take(pg);
      size_t made = (size_t)(pg->bump - HEFF_PAGE_FIRST(pg)) / (c * 8), back = 0;
      for (void *p = pg->free; p; p = *(void **)p) back++;
      if (back == made) {
#ifdef _WIN32
        _aligned_free(pg);
#else
        free(pg);
#endif
        continue;
      }
      __atomic_store_n(&pg->owner, NULL, __ATOMIC_RELAXED);
      pg->listed = 0;
      pg->all = left;
      if (!left) last = pg;
      left = pg;
    }
    if (left) {
      pthread_mutex_lock(&heff_abandoned_mu);
      last->all = heff_abandoned[c];
      __atomic_store_n(&heff_abandoned[c], left, __ATOMIC_RELAXED);
      pthread_mutex_unlock(&heff_abandoned_mu);
    }
  }
  memset(h, 0, sizeof *h);
}
#endif
#endif

static inline int heff_tag_of(V v) {
  return IS_NTAG(v) ? (int)(v >> 2) : OBJ(v)->tag;
}

// (C's condition as a Bool)
#define heff_bool(b) ((b) ? NTAG(TAG_True) : NTAG(TAG_False))

HEFF_FN V heff_tag(int id, int64_t n, V *f) {
  if (n == 0) return NTAG(id);
  Tag *t = heff_obj_alloc(sizeof(Tag) + n * sizeof(V));
  t->h.rc = 1; t->h.kind = K_TAG; t->h.tag = (uint16_t)id; heff_tag_head(t, n, 0);
  memcpy(t->f, f, n * sizeof(V));
  return (V)t;
}

static inline V heff_field(V v, int i) { return ((Tag *)v)->f[i]; }

// Flat fields (#39): a record's field of a type of 64 bits a `V` would
// allocate for -- `Float64`, `Int64`, `UInt64` -- is kept as its bits
// where the code building the record knows the type
// (the compiler's `flatOf`), and the record says so, 2 bits a
// field (`Rec`'s `flat`), so that what walks it -- a drop, a copy, a
// field looked for by its label, a value shown -- does not take the bits
// for a `V`, and a read where the type is not known boxes them.  Only the
// first `HEFF_FLAT_FIELDS` fields may be flat.  A tag's fields are kept
// so too (#64, `Tag`'s `flat`): a match reads one that may be flat as the
// tag holds it (`heff_tget_S`), and a provider given a tag with a field
// of such a type, or of a type not known, reads it by `heff_tget_v`.
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
HEFF_FN V heff_flat_box(V w, unsigned k);

// (the labels, in the order the record was written, are the program's:
// one array for each)
HEFF_FN V heff_rec(int64_t n, uint32_t *lbl, V *f) {
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
HEFF_FN V heff_get(V v, uint32_t l) {
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
  if (heff_rc(&t->h) == 1) {
    for (int64_t i = 0; i < t->n; i++)
      if (!((keep >> i) & 1) && !heff_flat_kind(t->flat, i)) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + t->n * sizeof(V));
  } else {
    for (int64_t i = 0; i < t->n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    heff_unref((V)t);
  }
}

// (as `heff_take`, the cell kept for a tag of its size to be built in
// (`heff_tag_at`): its last reference, the cell itself, its fields
// taken or dropped and blanked; otherwise none, 0)
static inline V heff_take_reuse(V v, uint64_t keep) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) {
    for (int64_t i = 0; i < t->n; i++) {
      if (!((keep >> i) & 1) && !heff_flat_kind(t->flat, i)) heff_drop(t->f[i]);
      t->f[i] = MK_INT(0);
    }
    t->flat = 0; t->h.kind = K_TAG;
    return v;
  }
  for (int64_t i = 0; i < t->n; i++)
    if ((keep >> i) & 1) heff_dup(t->f[i]);
  heff_unref((V)t);
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

// (the fields of a tag not in `keep` dropped, its flat ones (#64) none)
HEFF_FN __attribute__((noinline)) void heff_tag_unset(Tag *t, uint64_t keep, int64_t n) {
  for (int64_t i = 0; i < n; i++)
    if (!((keep >> i) & 1) && !heff_flat_kind(t->flat, i)) heff_drop(t->f[i]);
}

// (`heff_take`, of a tag of `n` fields)
HEFF_KNOWN void heff_take_n(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) {
    for (int64_t i = 0; i < n; i++)
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else {
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    heff_unref((V)t);
  }
}

// (... of a tag that may have flat fields (#64): one of them is never in
// `keep` -- it is read as the tag holds it -- and dropped here only if it
// is a `V`)
HEFF_KNOWN void heff_take_nf(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1 && t->flat) {
    heff_tag_unset(t, keep, n);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else heff_take_n(v, keep, n);
}

// (`heff_take_reuse`, of a tag of `n` fields: its fields not blanked, the
// cell kept being either built in (`heff_tag_at`, each field written) or
// freed as the cell it is (`heff_drop_cell`), never looked into)
HEFF_KNOWN V heff_take_reuse_n(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) {
    for (int64_t i = 0; i < n; i++)
      if (!((keep >> i) & 1)) heff_drop(t->f[i]);
    return v;
  }
  for (int64_t i = 0; i < n; i++)
    if ((keep >> i) & 1) heff_dup(t->f[i]);
  heff_unref((V)t);
  return MK_INT(0);
}

// (... of a tag that may have flat fields (#64), as `heff_take_nf`: the
// cell kept made plain, as `heff_tag_at_n` wants it -- `heff_tag_at_nf`
// says its flat fields anew)
HEFF_KNOWN V heff_take_reuse_nf(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1 && t->flat) {
    heff_tag_unset(t, keep, n);
    t->flat = 0; t->h.kind = K_TAG;
    return v;
  }
  return heff_take_reuse_n(v, keep, n);
}

// (... of a tag whose fields not in `keep` are all flat, in every tag of
// it the program builds (the compiler's `sure`): none
// dropped, nothing looked for)
HEFF_KNOWN void heff_take_na(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  else {
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    heff_unref((V)t);
  }
}

HEFF_KNOWN V heff_take_reuse_na(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) {
    t->flat = 0; t->h.kind = K_TAG;
    return v;
  }
  return heff_take_reuse_n(v, keep, n);
}

// (a tag of `n` fields dropped: its last reference, its fields dropped
// and its cell freed at once; otherwise counted down)
HEFF_KNOWN void heff_drop_tag(V v, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_rc(&t->h) == 1) {
    if (t->flat) heff_tag_unset(t, 0, n);
    else
      for (int64_t i = 0; i < n; i++) heff_drop(t->f[i]);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else heff_unref((V)t);
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
  t->h.kind = K_TAG; t->h.tag = (uint16_t)id; t->flat = 0;
  memcpy(t->f, f, n * sizeof(V));
  return r;
}

HEFF_FN V heff_clo(Fn fn, int arity, int nenv, V *env) {
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
  t->h.rc = 1; t->h.kind = K_TAG; t->h.tag = (uint16_t)id; heff_tag_head(t, n, 0);
  heff_fields_n(t->f, f, n);
  return (V)t;
}

// (... some of its fields flat (#64), as `flat` says: each given as its
// bits, `heff_word_f64` and the others)
HEFF_KNOWN V heff_tag_nf(int id, int64_t n, uint32_t flat, V *f) {
  Tag *t = heff_obj_alloc(sizeof(Tag) + n * sizeof(V));
  t->h.rc = 1; t->h.kind = K_TAGF; t->h.tag = (uint16_t)id; heff_tag_head(t, n, flat);
  heff_fields_n(t->f, f, n);
  return (V)t;
}

// (a tag of `n` fields built in the cell `r` kept for it, or a new one:
// a cell kept by `heff_take_reuse_n`, which says it has no flat fields)
HEFF_KNOWN V heff_tag_at_n(V r, int id, int64_t n, V *f) {
  if (!IS_OBJ(r)) return heff_tag_n(id, n, f);
  Tag *t = (Tag *)r;
  t->h.tag = (uint16_t)id;
  heff_fields_n(t->f, f, n);
  return r;
}

HEFF_KNOWN V heff_tag_at_nf(V r, int id, int64_t n, uint32_t flat, V *f) {
  if (!IS_OBJ(r)) return heff_tag_nf(id, n, flat, f);
  Tag *t = (Tag *)r;
  t->h.kind = K_TAGF; t->h.tag = (uint16_t)id; t->flat = flat;
  heff_fields_n(t->f, f, n);
  return r;
}

// (what a tag's field waits as, a hole: no object, nothing to drop; and
// field `i` of the cell `d`, made with a hole there that nothing has
// seen yet, set to `v`: what a call of itself gives put where a tag waits
// for it -- a destination; `d` consumed)
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
// so where the emitter knows the type -- erasure gives each
// projection its record's shape -- it reads a field at its place, not
// looking for its label (`heff_get`).

// (field `i`, the record not consumed)
#define heff_rfield(v, i) (((Rec *)(v))->f[i])

// (a record's fields but the `i`th dropped and its cell freed: for a
// record of many fields, a call, not as many drops where it is read)
#ifndef HEFF_FEW_FIELDS
#define HEFF_FEW_FIELDS 2
#endif
HEFF_FN __attribute__((noinline)) void heff_rec_give(V v, int64_t i) {
  Rec *r = (Rec *)v;
  for (int64_t j = 0; j < r->n; j++)
    if (j != i) heff_drop(r->f[j]);
  heff_obj_free(r, sizeof(Rec) + r->n * sizeof(V));
}

// (`heff_get_n` of a record with flat fields: the field as a `V`, the
// record dropped)
HEFF_FN __attribute__((noinline)) V heff_get_flat(V v, int64_t i) {
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
  if (heff_rc(&r->h) == 1) {
    if (n > HEFF_FEW_FIELDS) heff_rec_give(v, i);
    else {
      for (int64_t j = 0; j < n; j++)
        if (j != i) heff_drop(r->f[j]);
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    }
  } else {
    heff_dup(x);
    heff_unref((V)r);
  }
  return x;
}

// (field `i` of a record wanted after, which after it is only read other
// fields of, then updated or dropped (the compiler's `takesFor`):
// its last reference, the field taken out of it -- moved, not counted, a
// hole left that drops as nothing; otherwise the field counted.  Never a
// field that may be flat: one that is is read where it is)
HEFF_KNOWN V heff_take_field(V v, int64_t i) {
  Rec *r = (Rec *)v;
  V x = r->f[i];
  if (heff_rc(&r->h) == 1) r->f[i] = MK_INT(0);
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
HEFF_FN __attribute__((noinline)) V heff_rec_copy(V v, uint64_t keep, uint32_t *lbl, uint32_t kept, uint32_t flat) {
  Rec *r = (Rec *)v;
  int64_t n = r->n;
  uint32_t was = r->flat;
  Rec *c = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
  c->h.rc = 1; c->h.kind = K_REC; c->h.tag = 0; c->n = (int32_t)n; c->lbl = lbl;
  c->flat = (was & kept) | flat;
  for (int64_t i = 0; i < n; i++)
    c->f[i] = !((keep >> i) & 1) ? MK_INT(0)
            : heff_flat_kind(was, i) ? r->f[i] : heff_dup(r->f[i]);
  heff_unref((V)r);
  return (V)c;
}

// (a record with flat fields, built in its cell: the fields not kept
// dropped, as they were)
HEFF_FN __attribute__((noinline)) void heff_rec_unset(Rec *r, uint64_t keep) {
  for (int64_t i = 0; i < r->n; i++)
    if (!((keep >> i) & 1) && !heff_flat_kind(r->flat, i)) heff_drop(r->f[i]);
}

HEFF_KNOWN V heff_rec_reuse_nf(V v, uint64_t keep, int64_t n, uint32_t *lbl, uint32_t kept, uint32_t flat) {
  Rec *r = (Rec *)v;
  if (heff_rc(&r->h) != 1) {
    if (n > HEFF_FEW_FIELDS || r->flat) return heff_rec_copy(v, keep, lbl, kept, flat);
    Rec *c = heff_obj_alloc(sizeof(Rec) + n * sizeof(V));
    c->h.rc = 1; c->h.kind = K_REC; c->h.tag = 0; c->n = (int32_t)n; c->flat = flat; c->lbl = lbl;
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) c->f[i] = heff_dup(r->f[i]);
    heff_unref((V)r);
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

// (... where the fields stored anew are flat in every record of its
// shape the program builds (the compiler's `sureRecs`): the
// record's last reference, none dropped, nothing looked for)
HEFF_KNOWN V heff_rec_reuse_na(V v, uint64_t keep, int64_t n, uint32_t *lbl, uint32_t kept, uint32_t flat) {
  Rec *r = (Rec *)v;
  if (heff_rc(&r->h) == 1) {
    r->flat = (r->flat & kept) | flat;
    r->lbl = lbl;
    return v;
  }
  return heff_rec_reuse_nf(v, keep, n, lbl, kept, flat);
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
  if (heff_rc(&c->h) == 1) {
    if (c->memo) heff_drop(c->memo);
    heff_obj_free(c, sizeof(Clo) + n * sizeof(V));
  } else {
    for (int64_t i = 0; i < n; i++) heff_dup(c->env[i]);
    heff_unref((V)c);
  }
}

// (an object's size, from what it is)
HEFF_FN size_t heff_size(V v) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: case K_TAGF: return sizeof(Tag) + ((Tag *)v)->n * sizeof(V);
  case K_REC: return sizeof(Rec) + ((Rec *)v)->n * sizeof(V);
  case K_CLO: return sizeof(Clo) + ((Clo *)v)->nenv * sizeof(V);
  case K_PAP: return sizeof(Pap) + ((Pap *)v)->n * sizeof(V);
  case K_STR: case K_BYT: case K_BIT: return sizeof(Str) + ((Str *)v)->cap;
  case K_FLT: return sizeof(Flt);
  case K_ARR: return heff_arr_bytes((Arr *)v);
  case K_BNC: return sizeof(Bnc) + ((Bnc *)v)->n * sizeof(V);
  case K_BIG: return sizeof(Big) + ((Big *)v)->n * sizeof(uint32_t);
  default: return SMALL_MAX + 1;
  }
}

// (a record or a tag whose last reference is gone, its flat fields (#39,
// #64) made words that drop as nothing, as its other fields are given
// back)
HEFF_FN __attribute__((noinline)) void heff_flat_clear(V *f, int64_t n, uint32_t flat) {
  for (int64_t i = 0; i < n && i < HEFF_FLAT_FIELDS; i++)
    if (heff_flat_kind(flat, i)) f[i] = MK_INT(0);
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
  if (!IS_OBJ(v) || !heff_down(OBJ(v))) return;
  heff_dead_put(v);
}

// (an object whose last reference is gone: what it holds counted down,
// its size reckoned as it is looked at, and it freed; of what it holds,
// each whose last reference that was is given back after it -- the last
// of them next, at once, the others from the stack: a list's cells one
// after another, its spine never on the stack)
__attribute__((noinline)) HEFF_FN void heff_free(V v) {
  size_t base = heff_ndead;
  for (;;) {
    Obj *o = OBJ(v);
    V *f = NULL, next = 0;
    int64_t n = 0;
    size_t size;
    switch (o->kind) {
    case K_TAG: { Tag *t = (Tag *)v; n = t->n; f = t->f; size = sizeof(Tag) + n * sizeof(V); break; }
    case K_TAGF: {
      Tag *t = (Tag *)v; n = t->n; f = t->f; size = sizeof(Tag) + n * sizeof(V);
      heff_flat_clear(t->f, t->n, t->flat);
      break;
    }
    case K_REC: {
      Rec *r = (Rec *)v; n = r->n; f = r->f; size = sizeof(Rec) + n * sizeof(V);
      if (r->flat) heff_flat_clear(r->f, r->n, r->flat);
      break;
    }
    case K_CLO: {
      Clo *c = (Clo *)v; n = c->nenv; f = c->env; size = sizeof(Clo) + n * sizeof(V);
      if (c->memo) heff_dead_push(c->memo);
      break;
    }
    case K_PAP: { Pap *p = (Pap *)v; heff_dead_push(p->fun); n = p->n; f = p->a; size = sizeof(Pap) + n * sizeof(V); break; }
    case K_ARR: { Arr *r = (Arr *)v; n = heff_arr_slots(r); f = r->a; size = heff_arr_bytes(r); break; }
    case K_BNC: { Bnc *b = (Bnc *)v; heff_dead_push(b->fun); n = b->n; f = b->a; size = sizeof(Bnc) + n * sizeof(V); break; }
    default: size = heff_size(v); break;
    }
    for (int64_t i = 0; i < n; i++) {
      V x = f[i];
      if (!IS_OBJ(x) || !heff_down(OBJ(x))) continue;
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
  if (heff_rc(&r->h) == 1) {
    // (a record of numbers, all flat: its cell, nothing in it)
    if (r->flat && n <= HEFF_FLAT_FIELDS && heff_flat_all(r->flat, heff_flat_lows(n)))
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    else if (n > HEFF_FEW_FIELDS || r->flat) heff_free(v);
    else {
      for (int64_t i = 0; i < n; i++) heff_drop(r->f[i]);
      heff_obj_free(r, sizeof(Rec) + n * sizeof(V));
    }
  } else heff_unref((V)r);
}

// (a closure holding `n` dropped, as `heff_drop_rec` a record)
HEFF_KNOWN void heff_drop_clo(V v, int64_t n) {
  Clo *c = (Clo *)v;
  if (heff_rc(&c->h) == 1) {
    if (n > HEFF_FEW_FIELDS) heff_free(v);
    else {
      if (c->memo) heff_drop(c->memo);
      for (int64_t i = 0; i < n; i++) heff_drop(c->env[i]);
      heff_obj_free(c, sizeof(Clo) + n * sizeof(V));
    }
  } else heff_unref((V)c);
}

// Values between threads (#36)
// ---------------------------------------------------------------------

// (a tag's or a record's fields, an array's elements: `*n` of them --
// a closure's, a partial application's and a bounce's are said by
// `HEFF_EACH_SLOT`; a string, a float and a big integer hold none)
HEFF_FN V *heff_slots(V v, int64_t *n) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: case K_TAGF: *n = ((Tag *)v)->n; return ((Tag *)v)->f;
  case K_REC: *n = ((Rec *)v)->n; return ((Rec *)v)->f;
  case K_ARR: *n = heff_arr_slots((Arr *)v); return ((Arr *)v)->a;
  default: *n = 0; return NULL;
  }
}

// (a stack of objects, for a walk that is not a recursion: a long
// list's spine never on the C stack)
typedef struct { V *v; size_t n, cap; } HeffStack;

HEFF_FN void heff_stack_push(HeffStack *s, V v) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->v = realloc(s->v, s->cap * sizeof(V));
    if (!s->v) heff_fail("out of memory");
  }
  s->v[s->n++] = v;
}

// (each place of the object `v` holds a value in, given to `f`)
// (a record's flat fields (#39) none, nor a tag's (#64))
#define HEFF_EACH_SLOT(v, f, u)                                         \
  do {                                                                  \
    Obj *o_ = OBJ(v);                                                   \
    int64_t n_;                                                         \
    V *s_ = heff_slots(v, &n_);                                         \
    uint32_t fl_ = o_->kind == K_REC ? ((Rec *)(v))->flat               \
                 : o_->kind == K_TAGF ? ((Tag *)(v))->flat : 0;         \
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

HEFF_FN void heff_freeze_one(HeffStack *s, V *at) {
#ifdef HEFF_THREADS
  // (a shared thunk's `memo` another thread may put a value in meanwhile)
  V v = __atomic_load_n(at, __ATOMIC_ACQUIRE);
#else
  V v = *at;
#endif
  if (!IS_OBJ(v) || !v) return;
  uint32_t rc = heff_rc(OBJ(v));
  if (rc & IMMORTAL) return;
#ifdef HEFF_THREADS
  // (a shared object, other threads counting it meanwhile: the mark set
  // over its count, which they leave as it is from then on)
  if (rc & SHARED) __atomic_fetch_or(&OBJ(v)->rc, IMMORTAL, __ATOMIC_RELAXED);
  else
#endif
    OBJ(v)->rc = IMMORTAL;
  heff_stack_push(s, v);
}

// (`v` made immortal, and all it holds: shared between threads from now
// on, each counting it never, and never freed -- what holds it now
// leaks it.  What of `v` is mortal is this thread's, no other's yet;
// what is shared another thread may hold too, and counts no more)
HEFF_FN void heff_freeze(V v) {
  HeffStack s = {0};
  heff_freeze_one(&s, &v);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_freeze_one, &s);
  }
  free(s.v);
}

#ifdef HEFF_THREADS
HEFF_FN void heff_share_one(HeffStack *s, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v) return;
  uint32_t rc = heff_rc(OBJ(v));
  if (rc & (IMMORTAL | SHARED)) return;
  OBJ(v)->rc = rc | SHARED;
  heff_stack_push(s, v);
}

// (`v` shared (#66), to hand to other threads: each mortal object of it
// marked `SHARED`, counted atomically from then on, by every thread that
// holds it, and freed by the one that lets it go last.  The walk stops
// at what is shared already or immortal -- what a shared object holds
// is shared or immortal -- so a value shared again is walked only where
// it is new.  `v` not consumed, nothing copied: another thread holds it
// by one more count (`heff_dup`).  What of `v` is mortal is this
// thread's: no other changes it meanwhile)
HEFF_FN V heff_share(V v) {
  if (!IS_OBJ(v) || !v || (heff_rc(OBJ(v)) & (IMMORTAL | SHARED))) return v;
  HeffStack s = {0};
  heff_share_one(&s, &v);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_share_one, &s);
  }
  free(s.v);
  return v;
}
#else
// (one thread: nothing is shared, nothing to do)
static inline V heff_share(V v) { return v; }
#endif

// Calls of what is not known
// ---------------------------------------------------------------------

HEFF_VAR(HEFF_TLS long heff_dep);

// (where a lambda's call last of more arguments than it took is put, up
// to `HEFF_ROOM` of them, to be made a jump: its own are where its
// caller put them, room for only as many as it takes -- and what is
// called reads them at once, before it calls anything -- see the C
// emitter's `roomed`)
#define HEFF_ROOM 4
HEFF_VAR(HEFF_TLS V heff_room[HEFF_ROOM]);

HEFF_FN V heff_apply(V f, int n, V *a);

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
HEFF_FN V heff_trs(V r) {
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

HEFF_FN V heff_bounce(V f, int n, V *a) {
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
// (a thunk forced, threads at once: a shared or immortal one, whose
// memo other threads may read and write meanwhile.  Its value is shared
// (`heff_share`) and then put in the memo, by the first to have it;
// another computing it meanwhile, the language pure, has the same
// value, takes the memo's and lets its own go, freed.  A static thunk is
// immortal, its memo kept for good: the first freezes the value once it
// is there (`heff_freeze`), so that no thread counts it, atomically or
// at all -- a thread that counted it up before then counts it down
// after as nothing, the memo holding it.  A shared thunk's value is
// counted as any shared object is, freed when the thunk and all that
// hold it let it go)
HEFF_FN __attribute__((noinline)) V heff_force_shared(V f, V *a) {
  Clo *c = (Clo *)f;
  V m = __atomic_load_n(&c->memo, __ATOMIC_ACQUIRE);
  if (!m) {
    heff_dup(f);
    V r = heff_share(heff_tr(c->fn(f, a)));
    heff_dup(r);
    if (__atomic_compare_exchange_n(&c->memo, &m, r, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      if (heff_rc(&c->h) & IMMORTAL) heff_freeze(r);
      heff_drop(f);
      return r;
    }
    heff_drop(r);
    heff_drop(r);
  }
  heff_dup(m);
  heff_drop(f);
  return m;
}
#endif

HEFF_FN V heff_apply(V f, int n, V *a) {
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
      if (heff_rc(&c->h) & (IMMORTAL | SHARED)) return heff_force_shared(f, a);
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
// of it is then not counted (the emitter's `inPlace`))
static inline int heff_exact(V f, int n, V *a) {
  return HEFF_JUMPS && IS_OBJ(f) && OBJ(f)->kind == K_CLO
      && ((Clo *)f)->arity == n && !(n == 1 && a[0] == NTAG(TAG_MkUnit));
}

// Definitions' values
// ---------------------------------------------------------------------
// A definition that is no function is computed when it is first asked
// for, and kept (the emitter's `valDef`): its value
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

HEFF_FN __attribute__((noinline)) V heff_global(V *val, HEFF_ONCE *st, V (*init)(void)) {
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
HEFF_FN void heff_reopen(size_t base) {
  if (heff_nopen <= base) return;
  pthread_mutex_lock(&heff_once_mu);
  while (heff_nopen > base) __atomic_store_n(heff_open[--heff_nopen], 0, __ATOMIC_RELAXED);
  pthread_cond_broadcast(&heff_once_cv);
  pthread_mutex_unlock(&heff_once_mu);
}
#else
#define HEFF_ONCE int
#define HEFF_DONE(st) HEFF_LIKELY((st) == 2)

HEFF_FN __attribute__((noinline)) V heff_global(V *val, HEFF_ONCE *st, V (*init)(void)) {
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

HEFF_FN void num_of(V v, Num *x) {
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
HEFF_FN V big_make(int neg, const uint32_t *d, int n) {
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

HEFF_FN int mag_cmp(const uint32_t *a, int an, const uint32_t *b, int bn) {
  if (an != bn) return an < bn ? -1 : 1;
  for (int i = an - 1; i >= 0; i--)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

// (`o` has room for max(an, bn) + 1 digits)
HEFF_FN int mag_add(const uint32_t *a, int an, const uint32_t *b, int bn, uint32_t *o) {
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
HEFF_FN int mag_sub(const uint32_t *a, int an, const uint32_t *b, int bn, uint32_t *o) {
  int64_t c = 0;
  for (int i = 0; i < an; i++) {
    c += (int64_t)a[i] - (i < bn ? b[i] : 0);
    o[i] = (uint32_t)c;
    c = c < 0 ? -1 : 0;
  }
  return an;
}

// (a + b, signed)
HEFF_FN V num_add(const Num *a, int bneg, const Num *b) {
  int n = (a->n > b->n ? a->n : b->n) + 1;
  uint32_t *o = alloca(n * sizeof(uint32_t));
  if (a->neg == bneg) return big_make(a->neg, o, mag_add(a->d, a->n, b->d, b->n, o));
  if (mag_cmp(a->d, a->n, b->d, b->n) >= 0) return big_make(a->neg, o, mag_sub(a->d, a->n, b->d, b->n, o));
  return big_make(bneg, o, mag_sub(b->d, b->n, a->d, a->n, o));
}

// (u divided by v, v's top digit not 0, as Knuth's algorithm D:
// q gets m - n + 1 digits, r n)
HEFF_FN void mag_divmod(const uint32_t *u, int m, const uint32_t *v, int n, uint32_t *q, uint32_t *r) {
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
HEFF_FN V num_divmod(const Num *a, const Num *b, int want_mod) {
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

HEFF_FN V num_mul(const Num *a, const Num *b) {
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

HEFF_FN int num_cmp(const Num *a, const Num *b) {
  if (a->neg != b->neg) return a->neg ? -1 : 1;
  int c = mag_cmp(a->d, a->n, b->d, b->n);
  return a->neg ? -c : c;
}

// (bench's `keep`: the value as it is, but what it is hidden from the C
// compiler -- an empty instruction it must keep, and that it must take
// to read the value and the memory it points to, and may have changed
// the value -- so that neither the work it is the result of is dropped
// nor that made of it computed once, out of a loop: no instruction at
// all, as Rust's `black_box`)
static inline V heff_keep(V x) {
  __asm__ volatile("" : "+r"(x) : : "memory");
  return x;
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
HEFF_FN void num_twos(const Num *x, uint32_t *t, int n) {
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
HEFF_FN V twos_make(uint32_t *t, int n) {
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
  HEFF_FN V name(V a, V b) {                                                 \
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

HEFF_FN int heff_int_neg(V v) { return IS_INT(v) ? INT_OF(v) < 0 : ((Big *)v)->neg; }

HEFF_FN V heff_intShr(V a, V b);

// (`a` times 2^`b`; by a negative count, a shift right by its size; one
// beyond memory fails)
HEFF_FN V heff_intShl(V a, V b) {
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
HEFF_FN V heff_intShr(V a, V b) {
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
HEFF_FN char *heff_int_cstr(V v) {
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
HEFF_FN V heff_big_lit(const char *s) {
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
// it meanwhile lets its own go, freed -- no other has seen it)
#define HEFF_BIG(s) ({                                                      \
  static V heff_lit_;                                                       \
  V l_ = __atomic_load_n(&heff_lit_, __ATOMIC_ACQUIRE);                     \
  if (!l_) {                                                                \
    V none_ = 0;                                                            \
    l_ = heff_big_lit(s);                                                   \
    if (!__atomic_compare_exchange_n(&heff_lit_, &none_, l_, 0,             \
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { \
      if (IS_OBJ(l_)) heff_obj_free((void *)l_, heff_size(l_));             \
      l_ = none_;                                                           \
    }                                                                       \
  }                                                                         \
  l_; })
#else
#define HEFF_BIG(s) ({ static V heff_lit_; heff_lit_ ? heff_lit_ : (heff_lit_ = heff_big_lit(s)); })
#endif

// (a float's integer part, beyond a word if it must be)
HEFF_FN V heff_int_of_double(double f) {
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

HEFF_FN double heff_double_of_int(V v) {
  if (IS_INT(v)) return (double)INT_OF(v);
  char *c = heff_int_cstr(v);
  double d = strtod(c, NULL);
  free(c);
  return d;
}

// Strings
// ---------------------------------------------------------------------

HEFF_FN Str *heff_str_new(int64_t len) {
  Str *s = heff_obj_alloc(sizeof(Str) + len);
  s->h.rc = 1; s->h.kind = K_STR; s->h.tag = 0; s->len = len; s->cap = len;
  return s;
}

HEFF_FN V heff_str_ascii(const char *c) {
  int64_t n = (int64_t)strlen(c);
  Str *s = heff_str_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

// (`n` bytes, as they are.  The runtime's own: a provider builds a
// string by what it crosses as, "What providers share")
HEFF_FN V heff_str_of(const void *c, int64_t n) {
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
HEFF_FN int heff_u8_decode(const uint8_t *p, int64_t n, uint32_t *out) {
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
HEFF_FN int heff_u8_encode(uint8_t *o, uint32_t u) {
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
HEFF_FN int64_t heff_u8_valid_len(const uint8_t *p, int64_t n, int *own) {
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
HEFF_FN V heff_str_valid(const void *c, int64_t n) {
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
HEFF_FN V heff_buf_append(V a, V b, uint16_t kind) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t n = x->len + y->len;
  if (heff_rc(&x->h) == 1) {
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

HEFF_FN V heff_strAppend(V a, V b) { return heff_buf_append(a, b, K_STR); }

// Lent strings: a primitive that only reads its strings has a `_lent`
// form, given besides its operands a mask, `give`, of those given it
// (bit `k` the `k`-th operand's): the others are lent, held by the caller
// through the call, neither counted up for it nor down by it.  The C
// backend calls it where an operand is a variable wanted after the call
// (the emitter's `lendingPrims`), and so saves a count up
// and down of its string each time -- a byte read in a loop, or a key
// compared at each node of a map.  (`give` is a constant there: what is
// not given is not looked at.)
#define HEFF_GIVEN(give, k, v) do { if ((give) >> (k) & 1) heff_drop(v); } while (0)

HEFF_FAST V heff_strLength_lent(V a, int give) {
  V n = MK_INT(((Str *)a)->len);
  HEFF_GIVEN(give, 0, a);
  return n;
}
HEFF_FN V heff_strLength(V a) { return heff_strLength_lent(a, 1); }

// (as JavaScript's `slice`: out of range is clamped, negative from the end)
HEFF_FN V heff_strSlice_lent(V a, V i, V j, int give) {
  Str *x = (Str *)a;
  int64_t n = x->len, b = INT_OF(i), e = INT_OF(j);
  if (b < 0) b = b + n < 0 ? 0 : b + n;
  if (e < 0) e = e + n < 0 ? 0 : e + n;
  if (b > n) b = n;
  if (e > n) e = n;
  if (e < b) e = b;
  Str *s = heff_str_new(e - b);
  memcpy(s->s, x->s + b, (size_t)(e - b));
  HEFF_GIVEN(give, 0, a);
  return (V)s;
}
HEFF_FN V heff_strSlice(V a, V i, V j) { return heff_strSlice_lent(a, i, j, 1); }

// (the 8 bytes at `p` as a number in their order: the first the highest)
static inline uint64_t heff_str_word(const uint8_t *p) {
  uint64_t w;
  memcpy(&w, p, sizeof w);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  w = __builtin_bswap64(w);
#endif
  return w;
}

// (the bytes' order: of UTF-8, its runes'.  Words, mostly short, are
// compared 8 bytes at a time, in place: no call to memcmp)
static inline int heff_str_cmp(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  if (x == y) return 0;
  int64_t n = x->len < y->len ? x->len : y->len, i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t u = heff_str_word(x->s + i), v = heff_str_word(y->s + i);
    if (u != v) return u < v ? -1 : 1;
  }
  for (; i < n; i++)
    if (x->s[i] != y->s[i]) return x->s[i] < y->s[i] ? -1 : 1;
  return x->len == y->len ? 0 : x->len < y->len ? -1 : 1;
}

// (the order of two strings at once: -1, 0 or 1)
HEFF_FAST V heff_strCompare_lent(V a, V b, int give) {
  int c = heff_str_cmp(a, b);
  HEFF_GIVEN(give, 0, a);
  HEFF_GIVEN(give, 1, b);
  return MK_INT(c);
}
HEFF_FN V heff_strCompare(V a, V b) { return heff_strCompare_lent(a, b, 3); }

// (where `needle` is first in `s` from `i` on (from 0, `i` below it), or
// -1: its first byte looked for by memchr, the rest compared there)
HEFF_FN V heff_strFind_lent(V needle, V s, V i, int give) {
  Str *p = (Str *)needle, *x = (Str *)s;
  int64_t k = INT_OF(i), m = p->len, r = -1;
  if (k < 0) k = 0;
  if (k + m <= x->len) {
    if (m == 0) r = k;
    else {
      const uint8_t *at = x->s + k, *end = x->s + x->len - m + 1;
      while (at < end && (at = memchr(at, p->s[0], (size_t)(end - at)))) {
        if (memcmp(at + 1, p->s + 1, (size_t)(m - 1)) == 0) { r = at - x->s; break; }
        at++;
      }
    }
  }
  HEFF_GIVEN(give, 0, needle);
  HEFF_GIVEN(give, 1, s);
  return MK_INT(r);
}
HEFF_FN V heff_strFind(V needle, V s, V i) { return heff_strFind_lent(needle, s, i, 3); }

// Hashes: a string's is FxHash's step (Firefox's and
// rustc's, `rotl(h, 5) ^ w` times its constant) over its bytes 8 at a
// time, little-endian, the last word filled with zeros, from its length;
// then murmur3's finish (`fmix64`), which FxHash lacks, so that the low
// bits a trie looks at first are as mixed as the high.  An integer's is
// that finish of its low 64 bits (two's complement).  Each is shifted
// right by 2, a word's integer, never negative; JavaScript's and the
// interpreter's are the same numbers, so that a map's order is one everywhere.
#define HEFF_FX_K 0x517cc1b727220a95ULL

static inline uint64_t heff_fmix64(uint64_t h) {
  h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
  return h ^ (h >> 33);
}

static inline uint64_t heff_fx_step(uint64_t h, uint64_t w) {
  return (((h << 5) | (h >> 59)) ^ w) * HEFF_FX_K;
}

static inline uint64_t heff_hash_bytes(const uint8_t *p, int64_t n) {
  uint64_t h = (uint64_t)n;
  int64_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, p + i, sizeof w);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    w = __builtin_bswap64(w);
#endif
    h = heff_fx_step(h, w);
  }
  if (i < n) {
    uint64_t w = 0;
    for (int64_t k = n - 1; k >= i; k--) w = (w << 8) | p[k];
    h = heff_fx_step(h, w);
  }
  return heff_fmix64(h);
}

HEFF_FAST V heff_strHash_lent(V a, int give) {
  Str *x = (Str *)a;
  V r = MK_INT((int64_t)(heff_hash_bytes(x->s, x->len) >> 2));
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_strHash(V a) { return heff_strHash_lent(a, 1); }

HEFF_FN V heff_intHash(V a) {
  uint64_t u;
  if (IS_INT(a)) u = (uint64_t)INT_OF(a);
  else {
    Big *b = (Big *)a;
    u = (uint64_t)b->d[0] | (b->n > 1 ? (uint64_t)b->d[1] << 32 : 0);
    if (b->neg) u = ~u + 1;
    heff_drop(a);
  }
  return MK_INT((int64_t)(heff_fmix64(u) >> 2));
}

// (a trie's step: the 5 bits of the hash `h` from bit `s` on choose one of
// 32 children, and the node's bitmap `bm` says which it has -- where the
// one chosen is among them, the bits set below its counted, or, where it
// is not, -1 less that.  The bits are two's complement's, as `shiftR` and
// `and` have them)
HEFF_SLOW int64_t heff_trie_frag_big(V h, V s) {
  return INT_OF(heff_intAnd(heff_intShr(h, s), MK_INT(31)));
}

HEFF_FAST V heff_trieSlot(V bm, V h, V s) {
  int64_t frag;
  if (HEFF_LIKELY(IS_INT(h) && IS_INT(s) && INT_OF(s) >= 0)) {
    int64_t k = INT_OF(s);
    frag = (INT_OF(h) >> (k < 63 ? k : 63)) & 31;
  } else frag = heff_trie_frag_big(h, s);
  uint64_t b = (uint64_t)INT_OF(bm), bit = (uint64_t)1 << frag;
  int64_t i = __builtin_popcountll(b & (bit - 1));
  return MK_INT(b & bit ? i : -1 - i);
}

// (as C's condition, and as a Bool: see `INT_CMP`)
HEFF_FAST int heff_strEq_c_lent(V a, V b, int give) {
  Str *x = (Str *)a, *y = (Str *)b;
  int c = x == y || (x->len == y->len && memcmp(x->s, y->s, (size_t)x->len) == 0);
  HEFF_GIVEN(give, 0, a);
  HEFF_GIVEN(give, 1, b);
  return c;
}
HEFF_FN int heff_strEq_c(V a, V b) { return heff_strEq_c_lent(a, b, 3); }
static inline V heff_strEq(V a, V b) { return heff_bool(heff_strEq_c(a, b)); }
HEFF_FAST V heff_strEq_lent(V a, V b, int give) { return heff_bool(heff_strEq_c_lent(a, b, give)); }

HEFF_FAST int heff_strLt_c_lent(V a, V b, int give) {
  int c = heff_str_cmp(a, b);
  HEFF_GIVEN(give, 0, a);
  HEFF_GIVEN(give, 1, b);
  return c < 0;
}
HEFF_FN int heff_strLt_c(V a, V b) { return heff_strLt_c_lent(a, b, 3); }
static inline V heff_strLt(V a, V b) { return heff_bool(heff_strLt_c(a, b)); }
HEFF_FAST V heff_strLt_lent(V a, V b, int give) { return heff_bool(heff_strLt_c_lent(a, b, give)); }

// (the byte at `i`; out of the string, -1)
HEFF_FAST V heff_strByteAt_lent(V a, V i, int give) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  V r = k >= 0 && k < x->len ? MK_INT(x->s[k]) : MK_INT(-1);
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_strByteAt(V a, V i) { return heff_strByteAt_lent(a, i, 1); }

// (the string of one byte, a `UInt8`: a word of its value, 0 to 255)
HEFF_FN V heff_strFromByte(V c) {
  int64_t k = INT_OF(c);
  Str *s = heff_str_new(1);
  s->s[0] = (uint8_t)k;
  return (V)s;
}

HEFF_FN V heff_intShow(V a) {
  char *c = heff_int_cstr(a);
  heff_drop(a);
  V s = heff_str_ascii(c);
  free(c);
  return s;
}

// Floats
// ---------------------------------------------------------------------

HEFF_FN V heff_flt(double d) {
  Flt *f = heff_obj_alloc(sizeof(Flt));
  f->h.rc = 1; f->h.kind = K_FLT; f->h.tag = 0; f->d = d;
  return (V)f;
}

#define FLT_OF(v) (((Flt *)(v))->d)

// (two floats' operation, both consumed)
#define FLT2(name, expr) \
  HEFF_FN V name(V a, V b) { double x = FLT_OF(a), y = FLT_OF(b); heff_drop(a); heff_drop(b); return expr; }
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
HEFF_FN V heff_floatFromInt(V a) { double d = heff_double_of_int(a); heff_drop(a); return heff_flt(d); }
HEFF_FN V heff_floatSqrt(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(sqrt(x)); }

// (std.math's: libm's, as JavaScript's `Math` has them)
#define FLT1(name, f) \
  HEFF_FN V name(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(f(x)); }
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
HEFF_FN V heff_floatFloor(V a) {
  double d = FLT_OF(a);
  heff_drop(a);
  if (!isfinite(d)) return MK_INT(0);
  double f = floor(d);
  return heff_int_of_double(f);
}

// (as JavaScript writes a number: the fewest digits that read back the
// same, in plain or exponent form by its rules (ECMAScript
// Number::toString); then ".0" where that is a whole number)
HEFF_FN void heff_js_number(double d, char *out) {
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

HEFF_FN V heff_floatShow(V a) {
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
HEFF_FN uint64_t heff_int_low64(V v) {
  if (IS_INT(v)) return (uint64_t)INT_OF(v);
  Big *b = (Big *)v;
  uint64_t m = b->n > 0 ? b->d[0] : 0;
  if (b->n > 1) m |= (uint64_t)b->d[1] << 32;
  return b->neg ? (uint64_t)0 - m : m;
}

HEFF_FN V heff_int_u64(uint64_t m) {
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
HEFF_FN V heff_float32Sqrt(V a) { return heff_f32(sqrtf(heff_f32_of(a))); }

// (the fewest digits that read back as the same `Float32`, read as a float
// reads them (through a double), written as a float is: for each count of
// digits, the number of that many nearest it (of two as near, the one
// whose last digit is even, as printf rounds), if it reads back so; nine
// always do)
HEFF_FN V heff_float32Show(V a) {
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

HEFF_FN V heff_float32FromFloat(V a) { float f = (float)FLT_OF(a); heff_drop(a); return heff_f32(f); }
HEFF_FN V heff_float32ToFloat(V a) { return heff_flt((double)heff_f32_of(a)); }
HEFF_FN V heff_float32FromInt(V a) { double d = heff_double_of_int(a); heff_drop(a); return heff_f32((float)d); }
HEFF_FN V heff_float32Floor(V a) {
  double d = heff_f32_of(a);
  if (!isfinite(d)) return MK_INT(0);
  return heff_int_of_double(floor(d));
}

// Unboxed numbers (#39)
// ---------------------------------------------------------------------
// Where a program computes with a fixed-width number or a `Float64`, it
// keeps it as its C type, and makes it a `V`
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

// An `Int` known to fit a word (#79), kept
// as its `int64_t`: its `V` is a word, nothing to count, so unboxed or
// looked at alike.  A division and a remainder of two, by one not 0,
// rounded down as `heff_intDiv` and `heff_intMod` round.
static inline V heff_box_word(int64_t x) { return MK_INT(x); }
static inline int64_t heff_unbox_word(V v) { return INT_OF(v); }
static inline int64_t heff_peek_word(V v) { return INT_OF(v); }
static inline int64_t heff_word_div(int64_t x, int64_t y) {
  int64_t q = x / y, r = x % y;
  return r != 0 && ((r < 0) != (y < 0)) ? q - 1 : q;
}
static inline int64_t heff_word_mod(int64_t x, int64_t y) {
  int64_t r = x % y;
  return r != 0 && ((r < 0) != (y < 0)) ? r + y : r;
}

// A record's flat fields (#39, "Flat fields" above): a number's bits, the
// field so made (`heff_word_S`), and the field read as a number,
// whichever it is, flat or a `V` (`heff_rget_S`); the record consumed
// after (`heff_get_n_S`), or the field as a `V`, boxed if it is flat
// (`heff_rget_v`).

static inline V heff_word_f64(double d) { V w; memcpy(&w, &d, sizeof w); return w; }
static inline V heff_word_i64(int64_t x) { return (V)(uint64_t)x; }
static inline V heff_word_u64(uint64_t x) { return (V)x; }
static inline double heff_f64_of_word(V w) { double d; memcpy(&d, &w, sizeof d); return d; }

HEFF_FN V heff_flat_box(V w, unsigned k) {
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

// (... a field flat in every record of its shape the program builds
// (the compiler's `sureRecs`), read as it is)
HEFF_FAST double heff_rflat_f64(V v, int64_t i) { return heff_f64_of_word(((Rec *)v)->f[i]); }
HEFF_FAST int64_t heff_rflat_i64(V v, int64_t i) { return (int64_t)((Rec *)v)->f[i]; }
HEFF_FAST uint64_t heff_rflat_u64(V v, int64_t i) { return (uint64_t)((Rec *)v)->f[i]; }
#define HEFF_GET_NFLAT(S, T)                                                \
  HEFF_KNOWN T heff_get_nflat_##S(V v, int64_t i, int64_t n) {              \
    T x = heff_rflat_##S(v, i); heff_drop_rec(v, n); return x; }
HEFF_GET_NFLAT(f64, double)
HEFF_GET_NFLAT(i64, int64_t)
HEFF_GET_NFLAT(u64, uint64_t)

// A tag's fields that may be flat (#64), read where a match binds one:
// as a number, whichever the tag holds (`heff_tget_S`, the tag looked at;
// of a type never kept flat, the `V` looked at), or as a `V` of its own
// (`heff_tget_v`: counted, boxed if it is flat).

HEFF_FAST double heff_tget_f64(V v, int64_t i) {
  Tag *t = (Tag *)v;
  V w = t->f[i];
  return heff_flat_kind(t->flat, i) ? heff_f64_of_word(w) : FLT_OF(w);
}
HEFF_FAST int64_t heff_tget_i64(V v, int64_t i) {
  Tag *t = (Tag *)v;
  V w = t->f[i];
  return heff_flat_kind(t->flat, i) ? (int64_t)w : (int64_t)heff_int_low64(w);
}
HEFF_FAST uint64_t heff_tget_u64(V v, int64_t i) {
  Tag *t = (Tag *)v;
  V w = t->f[i];
  return heff_flat_kind(t->flat, i) ? (uint64_t)w : heff_int_low64(w);
}
// (... one flat in every tag of it the program builds, as it is)
HEFF_FAST double heff_tflat_f64(V v, int64_t i) { return heff_f64_of_word(((Tag *)v)->f[i]); }
HEFF_FAST int64_t heff_tflat_i64(V v, int64_t i) { return (int64_t)((Tag *)v)->f[i]; }
HEFF_FAST uint64_t heff_tflat_u64(V v, int64_t i) { return (uint64_t)((Tag *)v)->f[i]; }
#define HEFF_TGET_PEEK(S, T)                                                \
  HEFF_FAST T heff_tget_##S(V v, int64_t i) { return heff_peek_##S(((Tag *)v)->f[i]); }
HEFF_TGET_PEEK(i8, int8_t)
HEFF_TGET_PEEK(i16, int16_t)
HEFF_TGET_PEEK(i32, int32_t)
HEFF_TGET_PEEK(u8, uint8_t)
HEFF_TGET_PEEK(u16, uint16_t)
HEFF_TGET_PEEK(u32, uint32_t)
HEFF_TGET_PEEK(f32, float)
HEFF_FAST V heff_tget_v(V v, int64_t i) {
  Tag *t = (Tag *)v;
  unsigned k = heff_flat_kind(t->flat, i);
  return k ? heff_flat_box(t->f[i], k) : heff_dup(t->f[i]);
}

// (of what is not finite, 0)
HEFF_FN V heff_floor_f64(double d) {
  if (!isfinite(d)) return MK_INT(0);
  return heff_int_of_double(floor(d));
}
HEFF_FN V heff_floor_f32(float f) { return heff_floor_f64((double)f); }

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
HEFF_FN V heff_runeToInt(V a) { return a; }
HEFF_FN V heff_runeFromInt(V a) { return heff_int32FromInt(a); }

// (a string's runes: read, then put in a list from the last)
HEFF_FN V heff_strRunes(V a) {
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
HEFF_FN V heff_strOfRunes(V l) {
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
HEFF_FN uint32_t heff_str_rune_at(Str *x, int64_t i, int64_t *w) {
  if (i < 0 || i >= x->len) { *w = 0; return 0xFFFD; }
  uint32_t u;
  *w = heff_u8_decode(x->s + i, x->len - i, &u);
  return u;
}

HEFF_FAST V heff_strRuneAt_lent(V a, V i, int give) {
  int64_t w;
  uint32_t r = heff_str_rune_at((Str *)a, INT_OF(i), &w);
  HEFF_GIVEN(give, 0, a);
  return MK_INT((int64_t)r);
}
HEFF_FN V heff_strRuneAt(V a, V i) { return heff_strRuneAt_lent(a, i, 1); }

HEFF_FAST V heff_strRuneWidth_lent(V a, V i, int give) {
  int64_t w;
  heff_str_rune_at((Str *)a, INT_OF(i), &w);
  HEFF_GIVEN(give, 0, a);
  return MK_INT(w);
}
HEFF_FN V heff_strRuneWidth(V a, V i) { return heff_strRuneWidth_lent(a, i, 1); }

HEFF_FN V heff_strRuneCount(V a) {
  Str *x = (Str *)a;
  int64_t n = 0;
  uint32_t u;
  for (int64_t i = 0; i < x->len; n++) i += heff_u8_decode(x->s + i, x->len - i, &u);
  heff_drop(a);
  return MK_INT(n);
}

// Arrays
// ---------------------------------------------------------------------
// An array's elements are `V`s, or flat: a number's C type, an array of
// `double`s, of `int64_t`s, of `uint8_t`s, as the header's `tag` says
// (`HEFF_ARR_F64` and the others), so that nothing is allocated for an
// element, nor counted, and the elements are as near one another as C's
// are.  An array is built flat where the element it is built of is
// known to be a number (the element's repr),
// `heff_arrayReplicate_u8` and the others; built of `V`s, it is flat
// only where they are `Float64`s, each a box of its own (`K_FLT`) -- so
// an array of `Float64`s, if it has an element, is always flat, and read
// where that is known it is read as one, not looked at
// (`heff_arrayIndex_f64`).  An array of another number's type may hold
// `V`s, built where its type is not known (`generate` of a function):
// read where the type is known, it is looked at (`heff_arrayIndex_u8`);
// read where it is not, a flat element is boxed (`heff_arrayIndex`).
//
// `set` writes in place where the array is held only there (its count
// 1), and copies it otherwise: what the program sees is an array not
// changed, as it would be were each `set` a copy.  The path where it is
// held only there, the index in range and the element as the array keeps
// it, is inlined where it is used (`heff_arraySet_u8`); any other is a
// call (`heff_arr_set_slow`).  A read lends the array (`_lent`, as a
// string's), so that a read of an array set after is not a count up and
// down of it.

HEFF_FN Arr *heff_arr_alloc(int64_t n, unsigned kind) {
  Arr *r = heff_obj_alloc(sizeof(Arr) + (size_t)n * heff_arr_width[kind]);
  r->h.rc = 1; r->h.kind = K_ARR; r->h.tag = (uint16_t)kind; r->n = n;
  return r;
}

HEFF_FN Arr *heff_arr_new(int64_t n) { return heff_arr_alloc(n, HEFF_ARR_V); }

// (an array's elements, as C's array of `T`)
#define HEFF_ARR_AT(r, T) ((T *)(void *)(r)->a)

// (an array's length, of how many it is asked for: none for less, and no
// more than memory holds)
HEFF_FN int64_t heff_arr_count(V n) {
  if (IS_INT(n)) return INT_OF(n) > 0 ? INT_OF(n) : 0;
  if (((Big *)n)->neg) return 0;
  heff_fail("out of memory");
  return 0;
}

// (out of range, abandoned)
HEFF_FN __attribute__((noinline, cold)) void heff_arr_out(void) { heff_abandon(); }

// (whether an index is in range)
#define heff_arr_in(r, i) (IS_INT(i) && (uint64_t)INT_OF(i) < (uint64_t)(r)->n)

// (whether the array is held only here and keeps its elements as `kind`
// says: its header, one word, all of it looked at at once -- but in
// threads, where its count is read atomically, `heff_rc`)
static inline int heff_arr_mine(Arr *r, unsigned kind) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(HEFF_THREADS)
  uint64_t h;
  memcpy(&h, &r->h, sizeof h);
  return h == (1u | (uint64_t)K_ARR << 32 | (uint64_t)kind << 48);
#else
  return heff_rc(&r->h) == 1 && r->h.tag == kind;
#endif
}

// (element `k` as a `V` counted: a flat one boxed)
HEFF_FN V heff_arr_get(Arr *r, int64_t k) {
  switch (r->h.tag) {
  case HEFF_ARR_F64: return heff_box_f64(HEFF_ARR_AT(r, double)[k]);
  case HEFF_ARR_I64: return heff_box_i64(HEFF_ARR_AT(r, int64_t)[k]);
  case HEFF_ARR_U64: return heff_box_u64(HEFF_ARR_AT(r, uint64_t)[k]);
  case HEFF_ARR_I32: return heff_box_i32(HEFF_ARR_AT(r, int32_t)[k]);
  case HEFF_ARR_U32: return heff_box_u32(HEFF_ARR_AT(r, uint32_t)[k]);
  case HEFF_ARR_I16: return heff_box_i16(HEFF_ARR_AT(r, int16_t)[k]);
  case HEFF_ARR_U16: return heff_box_u16(HEFF_ARR_AT(r, uint16_t)[k]);
  case HEFF_ARR_I8: return heff_box_i8(HEFF_ARR_AT(r, int8_t)[k]);
  case HEFF_ARR_U8: return heff_box_u8(HEFF_ARR_AT(r, uint8_t)[k]);
  case HEFF_ARR_F32: return heff_box_f32(HEFF_ARR_AT(r, float)[k]);
  default: return heff_dup(r->a[k]);
  }
}

// (`x`, consumed, stored at `k` as the array keeps it: what was there
// not dropped)
HEFF_FN void heff_arr_put(Arr *r, int64_t k, V x) {
  switch (r->h.tag) {
  case HEFF_ARR_F64: HEFF_ARR_AT(r, double)[k] = heff_unbox_f64(x); return;
  case HEFF_ARR_I64: HEFF_ARR_AT(r, int64_t)[k] = heff_unbox_i64(x); return;
  case HEFF_ARR_U64: HEFF_ARR_AT(r, uint64_t)[k] = heff_unbox_u64(x); return;
  case HEFF_ARR_I32: HEFF_ARR_AT(r, int32_t)[k] = heff_unbox_i32(x); return;
  case HEFF_ARR_U32: HEFF_ARR_AT(r, uint32_t)[k] = heff_unbox_u32(x); return;
  case HEFF_ARR_I16: HEFF_ARR_AT(r, int16_t)[k] = heff_unbox_i16(x); return;
  case HEFF_ARR_U16: HEFF_ARR_AT(r, uint16_t)[k] = heff_unbox_u16(x); return;
  case HEFF_ARR_I8: HEFF_ARR_AT(r, int8_t)[k] = heff_unbox_i8(x); return;
  case HEFF_ARR_U8: HEFF_ARR_AT(r, uint8_t)[k] = heff_unbox_u8(x); return;
  case HEFF_ARR_F32: HEFF_ARR_AT(r, float)[k] = heff_unbox_f32(x); return;
  default: r->a[k] = x; return;
  }
}

// (how an array built of `V`s keeps them, by one of them: flat if it is
// a `Float64`'s box)
HEFF_FN unsigned heff_arr_kind_of(V x) {
  return IS_OBJ(x) && OBJ(x)->kind == K_FLT ? HEFF_ARR_F64 : HEFF_ARR_V;
}

// (`r`'s elements from `from`, `n` of them, put at `at` of `s`, each
// counted: as `s` keeps them, if it keeps them otherwise)
HEFF_FN void heff_arr_move(Arr *s, int64_t at, Arr *r, int64_t from, int64_t n) {
  if (s->h.tag == r->h.tag) {
    if (r->h.tag == HEFF_ARR_V)
      for (int64_t j = 0; j < n; j++) s->a[at + j] = heff_dup(r->a[from + j]);
    else memcpy((char *)s->a + at * heff_arr_width[r->h.tag],
                (char *)r->a + from * heff_arr_width[r->h.tag],
                (size_t)n * heff_arr_width[r->h.tag]);
  } else
    for (int64_t j = 0; j < n; j++) heff_arr_put(s, at + j, heff_arr_get(r, from + j));
}

// (a copy of the array `a`, held elsewhere too: `a` counted down)
HEFF_FN __attribute__((noinline)) Arr *heff_arr_copy(V a) {
  Arr *r = (Arr *)a;
  Arr *s = heff_arr_alloc(r->n, r->h.tag);
  heff_arr_move(s, 0, r, 0, r->n);
  heff_drop(a);
  return s;
}

HEFF_FAST V heff_arrayLength_lent(V a, int give) {
  V n = MK_INT(((Arr *)a)->n);
  HEFF_GIVEN(give, 0, a);
  return n;
}
HEFF_FN V heff_arrayLength(V a) { return heff_arrayLength_lent(a, 1); }

// (the element at `i`, a `V`; out of range, abandoned)
HEFF_FAST V heff_arrayIndex_lent(V a, V i, int give) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) heff_arr_out();
  int64_t k = INT_OF(i);
  V x = r->h.tag == HEFF_ARR_V ? heff_dup(r->a[k]) : heff_arr_get(r, k);
  HEFF_GIVEN(give, 0, a);
  return x;
}
HEFF_FN V heff_arrayIndex(V a, V i) { return heff_arrayIndex_lent(a, i, 3); }

// (... read as a number, where the program knows it is one: the
// element as the array keeps it, flat or a `V` -- an array of `Float64`s
// always flat (above))
HEFF_FAST double heff_arrayIndex_f64_lent(V a, V i, int give) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) heff_arr_out();
  double x = HEFF_ARR_AT(r, double)[INT_OF(i)];
  HEFF_GIVEN(give, 0, a);
  return x;
}
HEFF_FAST double heff_arrayIndex_f64(V a, V i) { return heff_arrayIndex_f64_lent(a, i, 3); }

#define HEFF_ARR_INDEX(S, T, K)                                                \
  HEFF_FAST T heff_arrayIndex_##S##_lent(V a, V i, int give) {                 \
    Arr *r = (Arr *)a;                                                         \
    if (!heff_arr_in(r, i)) heff_arr_out();                                    \
    int64_t k = INT_OF(i);                                                     \
    T x = r->h.tag == K ? HEFF_ARR_AT(r, T)[k] : heff_peek_##S(r->a[k]);       \
    HEFF_GIVEN(give, 0, a);                                                    \
    return x;                                                                  \
  }                                                                            \
  HEFF_FAST T heff_arrayIndex_##S(V a, V i) { return heff_arrayIndex_##S##_lent(a, i, 3); }
HEFF_ARR_INDEX(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_INDEX(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_INDEX(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_INDEX(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_INDEX(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_INDEX(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_INDEX(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_INDEX(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_INDEX(f32, float, HEFF_ARR_F32)

// (`set` but where the array is held only here and keeps `x` as it is:
// out of range, the array as it is; held elsewhere too, a copy, `x` at
// `i` in it)
HEFF_FN __attribute__((noinline)) V heff_arr_set_slow(V a, V i, V x) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) {
    heff_drop(i);
    heff_drop(x);
    return a;
  }
  int64_t k = INT_OF(i);
  if (!heff_unique(a)) r = heff_arr_copy(a);
  if (r->h.tag == HEFF_ARR_V) heff_drop(r->a[k]);
  heff_arr_put(r, k, x);
  return (V)r;
}

// (`x` at `i`: in place, the array held only here; out of range, the
// array as it is)
HEFF_FAST V heff_arraySet(V a, V i, V x) {
  Arr *r = (Arr *)a;
  if (HEFF_LIKELY(heff_arr_in(r, i) && heff_arr_mine(r, HEFF_ARR_V))) {
    V old = r->a[INT_OF(i)];
    r->a[INT_OF(i)] = x;
    heff_drop(old);
    return a;
  }
  return heff_arr_set_slow(a, i, x);
}

// (... `x` a number, where the program knows it is one)
#define HEFF_ARR_SET(S, T, K)                                                  \
  HEFF_FAST V heff_arraySet_##S(V a, V i, T x) {                              \
    Arr *r = (Arr *)a;                                                         \
    if (HEFF_LIKELY(heff_arr_in(r, i) && heff_arr_mine(r, K))) {             \
      HEFF_ARR_AT(r, T)[INT_OF(i)] = x;                                        \
      return a;                                                                \
    }                                                                          \
    return heff_arr_set_slow(a, i, heff_box_##S(x));                           \
  }
HEFF_ARR_SET(f64, double, HEFF_ARR_F64)
HEFF_ARR_SET(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_SET(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_SET(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_SET(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_SET(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_SET(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_SET(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_SET(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_SET(f32, float, HEFF_ARR_F32)

// (`n` elements, each `x`: of `V`s, counted once for each -- flat if
// `x` is a `Float64`)
HEFF_FN V heff_arrayReplicate(V n, V x) {
  int64_t m = heff_arr_count(n);
  if (m == 0) {
    heff_drop(x);
    return (V)heff_arr_new(0);
  }
  Arr *r = heff_arr_alloc(m, heff_arr_kind_of(x));
  if (r->h.tag == HEFF_ARR_F64) {
    double d = heff_unbox_f64(x);
    for (int64_t k = 0; k < m; k++) HEFF_ARR_AT(r, double)[k] = d;
    return (V)r;
  }
  for (int64_t k = 0; k < m; k++) r->a[k] = x;
  if (IS_OBJ(x)) {
    uint32_t rc = heff_rc(OBJ(x));
    if (!(rc & (IMMORTAL | SHARED))) OBJ(x)->rc = rc + (uint32_t)(m - 1);
#ifdef HEFF_THREADS
    else if (!(rc & IMMORTAL)) __atomic_fetch_add(&OBJ(x)->rc, (uint32_t)(m - 1), __ATOMIC_RELAXED);
#endif
  }
  return (V)r;
}

// (... of a number, flat)
#define HEFF_ARR_REPLICATE(S, T, K)                                            \
  static V heff_arrayReplicate_##S(V n, T x) {                                 \
    int64_t m = heff_arr_count(n);                                             \
    Arr *r = heff_arr_alloc(m, K);                                             \
    for (int64_t k = 0; k < m; k++) HEFF_ARR_AT(r, T)[k] = x;                  \
    return (V)r;                                                               \
  }
HEFF_ARR_REPLICATE(f64, double, HEFF_ARR_F64)
HEFF_ARR_REPLICATE(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_REPLICATE(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_REPLICATE(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_REPLICATE(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_REPLICATE(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_REPLICATE(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_REPLICATE(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_REPLICATE(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_REPLICATE(f32, float, HEFF_ARR_F32)

HEFF_FN V heff_arrayGenerate(V n, V f) {
  int64_t m = heff_arr_count(n);
  if (m == 0) {
    heff_drop(f);
    return (V)heff_arr_new(0);
  }
  V x = heff_tr(heff_app1(heff_dup(f), MK_INT(0)));
  Arr *r = heff_arr_alloc(m, heff_arr_kind_of(x));
  heff_arr_put(r, 0, x);
  for (int64_t k = 1; k < m; k++) heff_arr_put(r, k, heff_tr(heff_app1(heff_dup(f), MK_INT(k))));
  heff_drop(f);
  return (V)r;
}

// (a list's cell's element, a `V` counted: a flat one (#64) boxed)
HEFF_FN V heff_arrayFromList(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  if (n == 0) {
    heff_drop(l);
    return (V)heff_arr_new(0);
  }
  V x = heff_tget_v(l, 0);
  Arr *r = heff_arr_alloc(n, heff_arr_kind_of(x));
  heff_arr_put(r, 0, x);
  int64_t k = 1;
  for (V c = heff_field(l, 1); heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1))
    heff_arr_put(r, k++, heff_tget_v(c, 0));
  heff_drop(l);
  return (V)r;
}

HEFF_FN V heff_arrayToList(V a) {
  Arr *r = (Arr *)a;
  V l = NTAG(TAG_Nil);
  for (int64_t k = r->n - 1; k >= 0; k--) {
    V f[2] = { heff_arr_get(r, k), l };
    l = heff_tag_n(TAG_Cons, 2, f);
  }
  heff_drop(a);
  return l;
}

// (the elements from `i` up to `j`, as a string's slice (`strSlice`):
// each clamped to them, a negative one counted from the end)
HEFF_FN V heff_arraySlice(V a, V i, V j) {
  Arr *r = (Arr *)a;
  int64_t n = r->n, b = INT_OF(i), e = INT_OF(j);
  if (!IS_INT(i)) b = ((Big *)i)->neg ? 0 : n;
  else if (b < 0) b = b + n < 0 ? 0 : b + n;
  else if (b > n) b = n;
  if (!IS_INT(j)) e = ((Big *)j)->neg ? 0 : n;
  else if (e < 0) e = e + n < 0 ? 0 : e + n;
  else if (e > n) e = n;
  heff_drop(i);
  heff_drop(j);
  if (b == 0 && e == n) return a;
  int64_t m = e > b ? e - b : 0;
  Arr *s = heff_arr_alloc(m, r->h.tag);
  heff_arr_move(s, 0, r, b, m);
  heff_drop(a);
  return (V)s;
}

// (one array's elements, then another's: kept as theirs are, or -- one
// flat and the other of `V`s -- flat, the other's put so)
HEFF_FN V heff_arrayAppend(V a, V b) {
  Arr *r = (Arr *)a, *t = (Arr *)b;
  if (t->n == 0) { heff_drop(b); return a; }
  if (r->n == 0) { heff_drop(a); return b; }
  Arr *s = heff_arr_alloc(r->n + t->n, r->h.tag != HEFF_ARR_V ? r->h.tag : t->h.tag);
  heff_arr_move(s, 0, r, 0, r->n);
  heff_arr_move(s, r->n, t, 0, t->n);
  heff_drop(a);
  heff_drop(b);
  return (V)s;
}

// Bytes
// ---------------------------------------------------------------------

// (`Bytes`, never changed once made: a string's `Str`, of
// the kind `K_BYT` -- dropped, shared with a thread and grown by an append
// as a string is, and shown as a binary, `b[...]`.  Their length, slice and
// equality are a string's; a byte is a `UInt8`, a word of its value)
HEFF_FN Str *heff_bytes_new(int64_t len) {
  Str *s = heff_str_new(len);
  s->h.kind = K_BYT;
  return s;
}

HEFF_FN V heff_bytesLength(V a) { return heff_strLength(a); }

// (the byte at `i`; out of them, abandoned, as `bytes`'s `index` never
// asks)
HEFF_FN V heff_bytesAt(V a, V i) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  if (!IS_INT(i) || k < 0 || k >= x->len) return heff_abandon();
  V r = MK_INT(x->s[k]);
  heff_drop(a);
  return r;
}

HEFF_FN V heff_bytesSlice(V a, V i, V j) {
  V s = heff_strSlice(a, i, j);
  OBJ(s)->kind = K_BYT;
  return s;
}

HEFF_FN V heff_bytesAppend(V a, V b) { return heff_buf_append(a, b, K_BYT); }

// (a list's bytes, one after another: their length added up first, so
// that they are made once, each copied once)
HEFF_FN V heff_bytesConcat(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n += ((Str *)heff_field(c, 0))->len;
  Str *s = heff_bytes_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    Str *x = (Str *)heff_field(c, 0);
    if (x->len) memcpy(s->s + k, x->s, (size_t)x->len);
    k += x->len;
  }
  heff_drop(l);
  return (V)s;
}

HEFF_FN V heff_bytesEq(V a, V b) { return heff_strEq(a, b); }

HEFF_FN V heff_bytesFromList(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n++;
  Str *s = heff_bytes_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) s->s[k++] = (uint8_t)INT_OF(heff_field(c, 0));
  heff_drop(l);
  return (V)s;
}

HEFF_FN V heff_bytesToList(V a) {
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
HEFF_FN V heff_bytesFromString(V a) {
  Str *x = (Str *)a, *s = heff_bytes_new(x->len);
  if (x->len) memcpy(s->s, x->s, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

// (a string of them, as they are: `bytesFromString` undone)
HEFF_FN V heff_bytesAsString(V a) {
  Str *x = (Str *)a;
  V s = heff_str_of(x->s, x->len);
  heff_drop(a);
  return s;
}

// (their text: what is not UTF-8 in them U+FFFD, as TextDecoder reads it)
HEFF_FN V heff_bytesToString(V a) {
  Str *x = (Str *)a;
  V s = heff_str_valid(x->s, x->len);
  heff_drop(a);
  return s;
}

// (what a binary's segment reads and writes, `b[x::16-little]`, as
// Erlang's do: an integer of `n` bits, 0 to 64, its two's complement,
// the most significant bit first -- or, little-endian (where the flags
// `f` have 1), its bytes the least significant first and then the `n %
// 8` bits left, the most significant -- read as signed where `f` has 2.
// `heff_bits_read` and `heff_bits_write` have them in the order they are
// written, `heff_bits_little` turns the one into the other.)
HEFF_FN uint64_t heff_bits_read(const uint8_t *s, int64_t at, int64_t n) {
  uint64_t u = 0;
  if ((at & 7) == 0 && (n & 7) == 0) {
    const uint8_t *p = s + (at >> 3);
    for (int64_t i = 0; i < (n >> 3); i++) u = (u << 8) | p[i];
  } else {
    for (int64_t i = at; i < at + n; i++) u = (u << 1) | ((s[i >> 3] >> (7 - (i & 7))) & 1);
  }
  return u;
}

// (the `n` bits of `w`, the most significant first, put at bit `at` of
// `s`, whose bits there are 0)
HEFF_FN void heff_bits_write(uint8_t *s, int64_t at, uint64_t w, int64_t n) {
  if ((at & 7) == 0 && (n & 7) == 0) {
    uint8_t *p = s + (at >> 3);
    for (int64_t i = 0; i < (n >> 3); i++) p[i] = (uint8_t)(w >> (n - 8 * (i + 1)));
  } else {
    for (int64_t i = 0; i < n; i++)
      if ((w >> (n - 1 - i)) & 1) s[(at + i) >> 3] |= (uint8_t)(0x80 >> ((at + i) & 7));
  }
}

// (an integer of `n` bits, little-endian: its bits as they are written,
// from the integer (`out`), or the integer, from them)
HEFF_FN uint64_t heff_bits_little(uint64_t u, int64_t n, int out) {
  int64_t k = n >> 3, r = n & 7;
  uint64_t w = 0;
  if (out) {
    for (int64_t i = 0; i < k; i++) w = (w << 8) | ((u >> (8 * i)) & 255);
    if (r) w = (w << r) | ((u >> (8 * k)) & ((1u << r) - 1));
  } else {
    if (r) w = (u & ((1u << r) - 1)) << (8 * k);
    for (int64_t i = 0; i < k; i++) w |= ((u >> (n - 8 * (i + 1))) & 255) << (8 * i);
  }
  return w;
}

// (the integer of `n` bits at bit `at` of `s`, as the flags `f` say)
HEFF_FN V heff_bits_int(const uint8_t *s, int64_t at, int64_t n, int64_t flags) {
  uint64_t u = heff_bits_read(s, at, n);
  if (flags & 1) u = heff_bits_little(u, n, 0);
  if ((flags & 2) && n > 0 && n < 64 && (u >> (n - 1)) & 1) return heff_int((int64_t)(u | (~(uint64_t)0 << n)));
  if ((flags & 2) && n == 64) return heff_int((int64_t)u);
  return heff_int_u64(u);
}

// (the integer `v`'s low `n` bits, as they are written)
HEFF_FN uint64_t heff_bits_of_int(V v, int64_t n, int64_t flags) {
  uint64_t u = heff_int_low64(v);
  if (n < 64) u &= ((uint64_t)1 << n) - 1;
  return (flags & 1) ? heff_bits_little(u, n, 1) : u;
}

// (`heff.elab.clauses` checks that it is there before it is read)
HEFF_FN V heff_bytesIntAt(V a, V o, V n, V f) {
  Str *x = (Str *)a;
  int64_t at = INT_OF(o), bits = INT_OF(n);
  if (!IS_INT(o) || !IS_INT(n) || at < 0 || bits < 0 || bits > 64 || at + bits > x->len * 8) return heff_abandon();
  V r = heff_bits_int(x->s, at, bits, INT_OF(f));
  heff_drop(a);
  return r;
}

// (the integer `v`, its low `n` bits (a multiple of 8, up to 64), as
// bytes)
HEFF_FN V heff_bytesOfInt(V n, V f, V v) {
  int64_t bits = INT_OF(n);
  if (!IS_INT(n) || bits < 0 || bits > 64 || (bits & 7)) return heff_abandon();
  uint64_t w = heff_bits_of_int(v, bits, INT_OF(f));
  heff_drop(v);
  Str *s = heff_bytes_new(bits >> 3);
  heff_bits_write(s->s, 0, w, bits);
  return (V)s;
}

// (whether the bytes `b` are those of `a` at `o`)
HEFF_FN V heff_bytesHasAt(V a, V o, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t at = INT_OF(o);
  int r = IS_INT(o) && at >= 0 && at <= x->len - y->len && (y->len == 0 || memcmp(x->s + at, y->s, (size_t)y->len) == 0);
  heff_drop(a);
  heff_drop(b);
  return heff_bool(r);
}

// Bits
// ---------------------------------------------------------------------

// (`Bits`, a bitstring, never changed once made: a
// string's `Str`, of the kind `K_BIT`, its `len` bytes holding its bits
// the first the most significant of the first byte; `h.tag` says how
// many bits of the last byte are none of them, each 0 -- so that two of
// the same bits are the same bytes.  Dropped and shared with a thread as
// a string is, and shown as a binary, `b[...]`)
#define HEFF_BITS_LEN(x) ((x)->len * 8 - (int64_t)(x)->h.tag)

HEFF_FN Str *heff_bits_new(int64_t n) {
  int64_t len = (n + 7) >> 3;
  Str *s = heff_str_new(len);
  s->h.kind = K_BIT;
  s->h.tag = (uint16_t)((8 - (n & 7)) & 7);
  if (len) memset(s->s, 0, (size_t)len);
  return s;
}

// (the `n` bits of `src` from bit `from`, put at bit `at` of `dst`,
// whose bits there are 0)
HEFF_FN void heff_bits_copy(uint8_t *dst, int64_t at, const uint8_t *src, int64_t from, int64_t n) {
  if ((at & 7) == 0 && (from & 7) == 0) {
    int64_t k = n >> 3;
    if (k) memcpy(dst + (at >> 3), src + (from >> 3), (size_t)k);
    if (n & 7) dst[(at >> 3) + k] |= (uint8_t)(src[(from >> 3) + k] & (0xff00 >> (n & 7)));
    return;
  }
  int64_t i = 0;
  for (; i + 8 <= n; i += 8) heff_bits_write(dst, at + i, heff_bits_read(src, from + i, 8), 8);
  if (i < n) heff_bits_write(dst, at + i, heff_bits_read(src, from + i, n - i), n - i);
}

HEFF_FN V heff_bitsLength(V a) {
  int64_t n = HEFF_BITS_LEN((Str *)a);
  heff_drop(a);
  return MK_INT(n);
}

HEFF_FN V heff_bitsIntAt(V a, V o, V n, V f) {
  Str *x = (Str *)a;
  int64_t at = INT_OF(o), bits = INT_OF(n);
  if (!IS_INT(o) || !IS_INT(n) || at < 0 || bits < 0 || bits > 64 || at + bits > HEFF_BITS_LEN(x)) return heff_abandon();
  V r = heff_bits_int(x->s, at, bits, INT_OF(f));
  heff_drop(a);
  return r;
}

// (the integer `v`, its low `n` bits, 0 to 64, as bits)
HEFF_FN V heff_bitsOfInt(V n, V f, V v) {
  int64_t bits = INT_OF(n);
  if (!IS_INT(n) || bits < 0 || bits > 64) return heff_abandon();
  uint64_t w = heff_bits_of_int(v, bits, INT_OF(f));
  heff_drop(v);
  Str *s = heff_bits_new(bits);
  heff_bits_write(s->s, 0, w, bits);
  return (V)s;
}

// (a list's bits, one after another, made at once)
HEFF_FN V heff_bitsConcat(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) n += HEFF_BITS_LEN((Str *)heff_field(c, 0));
  Str *s = heff_bits_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    Str *x = (Str *)heff_field(c, 0);
    heff_bits_copy(s->s, k, x->s, 0, HEFF_BITS_LEN(x));
    k += HEFF_BITS_LEN(x);
  }
  heff_drop(l);
  return (V)s;
}

// (the `n` bits from bit `o`; out of them, abandoned)
HEFF_FN V heff_bitsSlice(V a, V o, V n) {
  Str *x = (Str *)a;
  int64_t at = INT_OF(o), m = INT_OF(n);
  if (!IS_INT(o) || !IS_INT(n) || at < 0 || m < 0 || at + m > HEFF_BITS_LEN(x)) return heff_abandon();
  Str *s = heff_bits_new(m);
  heff_bits_copy(s->s, 0, x->s, at, m);
  heff_drop(a);
  return (V)s;
}

// (bytes as bits, and bits that are whole bytes as bytes (others,
// abandoned): the same buffer where it is the last reference)
HEFF_FN V heff_bitsOfBytes(V a) {
  Str *x = (Str *)a;
  if (heff_rc(&x->h) == 1) { x->h.kind = K_BIT; x->h.tag = 0; return a; }
  Str *s = heff_bits_new(x->len * 8);
  if (x->len) memcpy(s->s, x->s, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

HEFF_FN V heff_bitsBytes(V a) {
  Str *x = (Str *)a;
  if (x->h.tag) return heff_abandon();
  if (heff_rc(&x->h) == 1) { x->h.kind = K_BYT; return a; }
  Str *s = heff_bytes_new(x->len);
  if (x->len) memcpy(s->s, x->s, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

HEFF_FN V heff_bitsEq(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int r = x->len == y->len && x->h.tag == y->h.tag && (x->len == 0 || memcmp(x->s, y->s, (size_t)x->len) == 0);
  heff_drop(a);
  heff_drop(b);
  return heff_bool(r);
}

// (a `Float64`'s bits, IEEE's: of 64 bits, or of 32, rounded to a
// `float` first; a NaN, the quiet one, its sign 0, as every backend has
// it)
HEFF_FN V heff_floatBits(V n, V x) {
  int64_t bits = INT_OF(n);
  double d = FLT_OF(x);
  heff_drop(x);
  if (bits == 64) {
    uint64_t u;
    if (d != d) u = 0x7ff8000000000000ull; else memcpy(&u, &d, 8);
    return heff_int_u64(u);
  }
  if (bits == 32) {
    float g = (float)d;
    uint32_t u;
    if (g != g) u = 0x7fc00000u; else memcpy(&u, &g, 4);
    return heff_int_u64(u);
  }
  return heff_abandon();
}

// (the `Float64` whose bits, of 64 or of 32, are those of `v`)
HEFF_FN V heff_floatOfBits(V n, V v) {
  int64_t bits = INT_OF(n);
  uint64_t u = heff_int_low64(v);
  heff_drop(v);
  if (bits == 64) { double d; memcpy(&d, &u, 8); return heff_flt(d); }
  if (bits == 32) { uint32_t w = (uint32_t)u; float g; memcpy(&g, &w, 4); return heff_flt((double)g); }
  return heff_abandon();
}

// What providers share
// ---------------------------------------------------------------------

HEFF_FN V heff_just(V x) { return heff_tag_n(TAG_Just, 1, &x); }
#define heff_nothing NTAG(TAG_Nothing)

HEFF_FN V heff_cons(V x, V rest) {
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
// text is made valid, what is not UTF-8 in it U+FFFD, both in and out.

// (content in, `n` bytes, as a string)
HEFF_FN V heff_str_bytes(const void *c, int64_t n) { return heff_str_of(c, n); }

// (text in, `n` bytes of it, as a string)
HEFF_FN V heff_str_text_n(const void *c, int64_t n) { return heff_str_valid(c, n); }

// (a C string of text in, as a string: a name)
HEFF_FN V heff_str_text(const char *c) { return heff_str_text_n(c, (int64_t)strlen(c)); }

// (C strings, a directory's names, as a list of strings)
HEFF_FN V heff_strs_bytes(char **xs, int64_t n) {
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(heff_str_bytes(xs[i], (int64_t)strlen(xs[i])), l);
  return l;
}

// (C strings of text, names, as a list of strings)
HEFF_FN V heff_strs_text(char **xs, int64_t n) {
  V l = NTAG(TAG_Nil);
  for (int64_t i = n - 1; i >= 0; i--) l = heff_cons(heff_str_text(xs[i]), l);
  return l;
}

// (a path, as a C string to give the system: malloc'd, or NULL)
HEFF_FN char *heff_cstr(V v) {
  Str *s = (Str *)v;
  if (s->len && memchr(s->s, 0, (size_t)s->len)) return NULL;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->s, (size_t)s->len);
  c[s->len] = 0;
  return c;
}

// (a name or text, as a C string to give the system: malloc'd, or NULL)
HEFF_FN char *heff_cstr_text(V v) {
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
HEFF_FN char *heff_bytes(V v, size_t *n) {
  Str *s = (Str *)v;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->s, (size_t)s->len);
  c[s->len] = 0;
  *n = (size_t)s->len;
  return c;
}

// (content written out to `f`)
HEFF_FN void heff_put(FILE *f, V v) {
  Str *s = (Str *)v;
  if (s->len) fwrite(s->s, 1, (size_t)s->len, f);
}

// `Bytes` cross by these alone, as they are: in, `n` bytes copied, by
// `heff_bytes_in`; out, read where they are kept, `*n` of them, by
// `heff_bytes_data` (the value not consumed, its bytes there as long as
// it is held, and never written to).
HEFF_FN V heff_bytes_in(const void *c, int64_t n) {
  Str *s = heff_bytes_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

HEFF_FN const uint8_t *heff_bytes_data(V v, int64_t *n) {
  Str *s = (Str *)v;
  *n = s->len;
  return s->s;
}

// (C strings' order, for qsort)
HEFF_FN int heff_cmp_cstr(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// (a path joined to a name in it, malloc'd)
HEFF_FN char *heff_join(const char *dir, const char *name) {
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
HEFF_FN V heff_answer(V f, V x) { return heff_tr(heff_app1(f, x)); }

// The world: the things the providers hand out, held in one table for
// them all, so that one effect's provider takes what another's handed
// out (a `Handle` is `MkHandle i`, made and taken
// apart by the provider), as the JavaScript backend's `$world` holds them.
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
HEFF_FN int64_t heff_number_slot(void *x) {
  uint64_t h = (uint64_t)(uintptr_t)x;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  int64_t m = heff_capnumbers - 1, i = (int64_t)(h & (uint64_t)m);
  while (heff_numbers[i] >= 0 && heff_things[heff_numbers[i]].x != x) i = (i + 1) & m;
  return i;
}

// (the things held, numbered again: in four times as many slots, and more)
HEFF_FN void heff_renumber(void) {
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

HEFF_FN int64_t heff_hold(void *x, void (*let_go)(void *)) {
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

HEFF_FN void heff_held_none(int64_t i) {
  static HEFF_TLS char why[64];
  snprintf(why, sizeof why, "the world holds nothing numbered %lld", (long long)i);
  heff_fail(why);
}

HEFF_FN void *heff_held(int64_t i) {
  HEFF_WORLD_LOCK();
  void *x = i < 0 || i >= heff_nthings ? NULL : heff_things[i].x;
  HEFF_WORLD_UNLOCK();
  if (!x) heff_held_none(i);
  return x;
}

HEFF_FN void heff_release(int64_t i) {
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

// Threads a provider starts (#36): each runs what it
// is given -- shared, `heff_share` -- by `heff_try`, so
// that a failure there is the thread's to hand back, not the end of the
// process; and ends by `heff_thread_end`.
// ---------------------------------------------------------------------

// (`f x`, asked for here, a failure caught: 0, its value at `*out`; or
// why it failed -- "heff:abandon" where it abandoned (`heff_abandon`),
// the thread's own string otherwise, to be copied before the thread
// ends.  What was made before the failure is leaked, not given back)
HEFF_FN const char *heff_try(V f, V x, V *out) {
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
HEFF_FN void heff_fail_again(const char *why) {
  if (strcmp(why, "heff:abandon") == 0) heff_abandon();
  heff_fail(why);
}

// (this thread's pages and what else the runtime keeps for it given
// back to the system, as it ends -- its pages that still hold something
// abandoned, for another thread to adopt)
HEFF_FN void heff_thread_end(void) {
#if SMALL_MAX == 0
  for (size_t c = 0; c <= SMALL_MAX / 8; c++) {
    void *p = heff_free_list[c];
    while (p) {
      void *next = *(void **)p;
      free(p);
      p = next;
    }
    heff_free_list[c] = NULL;
  }
#elif defined(HEFF_THREADS)
  heff_heap_end();
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
HEFF_FN int heff_thread_start(pthread_t *t, void *(*run)(void *), void *u) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HEFF_THREAD_STACK);
  int r = pthread_create(t, &attr, run, u);
  pthread_attr_destroy(&attr);
  return r;
}

// Outside operations done later: the reactor (#43)
// ---------------------------------------------------------------------
//
// An outside operation that cannot be done without waiting -- a socket
// not yet readable or writable, a name being resolved, an HTTP transfer
// -- is a wait (`HeffWait`): what it waits for (a descriptor and its
// events; or, its `fd` -1, a source's word, below), its next step, how
// its answer is made of what it did, and how it is let go of undone.
// Its provider takes its first step itself; where that does not finish
// it, it hands the wait, the operation's function `f` and the world's
// continuation `k` to `heff_outside`, which
//
// - where no group is being started in (`heff_group` 0: a program with
//   no scheduler of std.reactor's), waits for it there, polling its
//   descriptor and stepping it until it is done, the whole program
//   waiting with it, and gives `f` of its answer to `k`, as the
//   provider would have;
// - in a group, keeps it among the pending and gives back
//   `HEFF_PENDING`, not calling `k`: what comes back, by the calls last
//   the world's continuations are, to the start (`heff_reactor_start`),
//   which tells its scheduler that the operation is pending.
//
// std.reactor's provider opens a group for
// a scheduler, starts each of its threads' outside operations in it --
// a computation run to its end, its value given back, or to a wait,
// pending -- asks which are finished, and closes it, what is pending in
// it let go of.  `heff_reactor_finished` polls the descriptors of every
// pending wait and of every source, steps each that is ready, and for
// each done, gives `f` of its answer to `k`, the rest of what was
// started run, up to its end -- its value one of its group's finished
// -- or to another wait, pending again.  A source (libcurl's, in
// std.http's provider) has descriptors of its own to poll and how
// long at most to wait for them, and says which of its waits are done
// (`heff_wait_done`).  Each thread of the system has a reactor of its
// own (`HEFF_TLS`): its waits are its own, waited for by it alone.

#include <errno.h>

#define HEFF_STEP_DONE 0
#define HEFF_STEP_WAIT 1

// (what a provider gives back where its operation is pending: no value
// of the program's)
#ifdef HEFF_PART
extern Obj heff_pending_obj;
#else
Obj heff_pending_obj = { IMMORTAL, K_STR, 0 };
#endif
#define HEFF_PENDING ((V)&heff_pending_obj)

typedef struct HeffWait HeffWait;
struct HeffWait {
  int fd;                        // (what it waits for; -1, a source's word)
  short events;                  // (POLLIN, POLLOUT)
  int err;                       // (why waiting failed -- errno, poll's, or
                                 //  EBADF, its socket closed -- its answer's to say)
  int (*step)(HeffWait *);       // (its next step: HEFF_STEP_DONE, or HEFF_STEP_WAIT,
                                 //  its `fd` and `events` again)
  V (*answer)(HeffWait *);       // (its answer, made, the wait let go of)
  void (*cancel)(HeffWait *);    // (the wait let go of, undone)
  // (the reactor's)
  V f, k;
  int64_t group;
  int done;
  HeffWait *prev, *next;
};

#ifndef _WIN32
typedef struct HeffSource HeffSource;
struct HeffSource {
  // (its descriptors to poll, at `p`, room for `room` of them: how many
  // it has, which are written only where there is room; at `*ms`, how
  // long at most to wait for them, -1 for ever)
  int (*fds)(struct pollfd *p, int room, int *ms);
  // (after the poll: its descriptors, `n` of them at `p`, as polled)
  void (*ready)(struct pollfd *p, int n);
  int on;
  size_t at;                     // (the reactor's: where its descriptors are
  int n;                         //  polled, and how many)
  HeffSource *next;
};
#endif

typedef struct { int64_t id, pending; } HeffGroup;
typedef struct { int64_t group; V v; } HeffFinished;

HEFF_VAR(HEFF_TLS int64_t heff_group);
HEFF_VAR(HEFF_TLS int64_t heff_last_group);
HEFF_VAR(HEFF_TLS HeffGroup *heff_groups_open);
HEFF_VAR(HEFF_TLS size_t heff_ngroups);
HEFF_VAR(HEFF_TLS size_t heff_capgroups);
HEFF_VAR(HEFF_TLS HeffWait *heff_waits_first);
HEFF_VAR(HEFF_TLS HeffWait *heff_waits_last);
HEFF_VAR(HEFF_TLS HeffFinished *heff_finished);
HEFF_VAR(HEFF_TLS size_t heff_nfinished);
HEFF_VAR(HEFF_TLS size_t heff_capfinished);
#ifndef _WIN32
HEFF_VAR(HEFF_TLS HeffSource *heff_sources);
HEFF_VAR(HEFF_TLS struct pollfd *heff_polled);
HEFF_VAR(HEFF_TLS HeffWait **heff_polled_waits);
HEFF_VAR(HEFF_TLS size_t heff_cappolled);
#endif

// (the group open of that number, or NULL)
HEFF_FN HeffGroup *heff_group_of(int64_t g) {
  for (size_t i = 0; i < heff_ngroups; i++)
    if (heff_groups_open[i].id == g) return &heff_groups_open[i];
  return NULL;
}

HEFF_FN void heff_wait_link(HeffWait *w) {
  w->prev = heff_waits_last;
  w->next = NULL;
  if (heff_waits_last) heff_waits_last->next = w;
  else heff_waits_first = w;
  heff_waits_last = w;
  HeffGroup *g = heff_group_of(w->group);
  if (g) g->pending++;
}

HEFF_FN void heff_wait_unlink(HeffWait *w) {
  if (w->prev) w->prev->next = w->next;
  else heff_waits_first = w->next;
  if (w->next) w->next->prev = w->prev;
  else heff_waits_last = w->prev;
  w->prev = w->next = NULL;
  HeffGroup *g = heff_group_of(w->group);
  if (g) g->pending--;
}

// (a wait done, as its source says: given its answer at the next turn)
HEFF_FN void heff_wait_done(HeffWait *w) { w->done = 1; }

// (the waits on the descriptor `fd`, which is being closed: done, their
// answers that it was -- EBADF -- not a poll of a descriptor that may be
// another's by then)
HEFF_FN void heff_reactor_closing(int fd) {
  for (HeffWait *w = heff_waits_first; w; w = w->next)
    if (w->fd == fd && !w->done) {
      w->err = EBADF;
      w->done = 1;
    }
}

#ifndef _WIN32
// (a source of waits, polled with the others from now on, by this thread)
HEFF_FN void heff_reactor_source(HeffSource *s) {
  if (s->on) return;
  s->on = 1;
  s->next = heff_sources;
  heff_sources = s;
}
#endif

// (the operation of the wait `w` -- its first step taken, not done --
// waited for here, or, in a group, pending: see above)
HEFF_FN V heff_outside(HeffWait *w, V f, V k) {
  if (heff_group) {
    w->f = f;
    w->k = k;
    w->group = heff_group;
    w->done = 0;
    heff_wait_link(w);
    return HEFF_PENDING;
  }
#ifndef _WIN32
  for (;;) {
    if (w->fd < 0) heff_fail("an outside operation waits for no descriptor, and no scheduler is there to wait for it");
    struct pollfd p = { w->fd, w->events, 0 };
    if (poll(&p, 1, -1) < 0) {
      if (errno == EINTR) continue;
      w->err = errno;
      break;
    }
    if (w->step(w) == HEFF_STEP_DONE) break;
  }
#endif
  V x = heff_answer(f, w->answer(w));
  return heff_tail(k, 1, &x);
}

// (what was started in group `g` come to its value `v`: one of the
// group's finished -- or, the group closed meanwhile, let go of)
HEFF_FN void heff_finished_put(int64_t g, V v) {
  if (!heff_group_of(g)) {
    heff_drop(v);
    return;
  }
  if (heff_nfinished == heff_capfinished) {
    size_t cap = heff_capfinished ? heff_capfinished * 2 : 16;
    HeffFinished *m = heff_alloc(cap * sizeof *m);
    if (heff_nfinished) memcpy(m, heff_finished, heff_nfinished * sizeof *m);
    free(heff_finished);
    heff_finished = m;
    heff_capfinished = cap;
  }
  heff_finished[heff_nfinished].group = g;
  heff_finished[heff_nfinished].v = v;
  heff_nfinished++;
}

// (a function given its argument, and nothing else: where what is
// started comes to its end)
HEFF_FN V heff_kept(V self, V *a) {
  heff_drop(self);
  return a[0];
}

// (what group `g` started, the rest of it run in the group, from `k`
// given `x`: to its end, its value finished, or to a wait, pending)
HEFF_FN void heff_reactor_run(int64_t g, V k, V x) {
  int64_t up = heff_group;
  heff_group = g;
  V r = heff_tr(heff_app1(k, x));
  heff_group = up;
  if (r != HEFF_PENDING) heff_finished_put(g, r);
}

// | A group of its own, for a scheduler: its number.
HEFF_FN int64_t heff_reactor_open(void) {
  if (heff_ngroups == heff_capgroups) {
    size_t cap = heff_capgroups ? heff_capgroups * 2 : 8;
    HeffGroup *m = heff_alloc(cap * sizeof *m);
    if (heff_ngroups) memcpy(m, heff_groups_open, heff_ngroups * sizeof *m);
    free(heff_groups_open);
    heff_groups_open = m;
    heff_capgroups = cap;
  }
  int64_t g = ++heff_last_group;
  heff_groups_open[heff_ngroups].id = g;
  heff_groups_open[heff_ngroups].pending = 0;
  heff_ngroups++;
  return g;
}

// | `c`, a computation of the world (its handler's monad: a function of
// | its continuation), started in group `g`: run to its end, its value;
// | or to a wait, `HEFF_PENDING`, its value to be one of the group's
// | finished.  `c` consumed.
HEFF_FN V heff_reactor_start(int64_t g, V c) {
  int64_t up = heff_group;
  heff_group = g;
  V r = heff_tr(heff_app1(c, heff_clo(heff_kept, 1, 0, NULL)));
  heff_group = up;
  return r;
}

// (the waits done -- stepped done, or said so -- taken out of the
// pending, then each given its answer, the rest of what it was started
// in run, in the order they were pending)
HEFF_FN void heff_reactor_wake(void) {
  HeffWait *first = NULL, *last = NULL;
  for (HeffWait *w = heff_waits_first, *next; w; w = next) {
    next = w->next;
    if (!w->done) continue;
    heff_wait_unlink(w);
    if (last) last->next = w;
    else first = w;
    last = w;
  }
  for (HeffWait *w = first, *next; w; w = next) {
    next = w->next;
    V f = w->f, k = w->k;
    int64_t g = w->group;
    V x = heff_answer(f, w->answer(w));
    heff_reactor_run(g, k, x);
  }
}

#ifndef _WIN32
// (room for `n` descriptors polled)
HEFF_FN void heff_polled_room(size_t n) {
  if (n <= heff_cappolled && heff_polled) return;
  size_t cap = heff_cappolled ? heff_cappolled : 64;
  while (cap < n) cap *= 2;
  free(heff_polled);
  free(heff_polled_waits);
  heff_polled = heff_alloc(cap * sizeof *heff_polled);
  heff_polled_waits = heff_alloc(cap * sizeof *heff_polled_waits);
  heff_cappolled = cap;
}

// (one turn: every pending wait's descriptor and every source's polled
// -- for as long as they say, where `wait`, else not waiting -- each
// ready stepped, and those done woken)
HEFF_FN void heff_reactor_turn(int wait) {
  size_t n = 0;
  int ms = wait ? -1 : 0;
  for (HeffWait *w = heff_waits_first; w; w = w->next) {
    if (w->done) ms = 0;
    else if (w->fd >= 0) n++;
  }
  size_t mine = n;
  for (HeffSource *s = heff_sources; s; s = s->next) {
    int t = -1;
    s->n = s->fds(NULL, 0, &t);
    n += (size_t)s->n;
  }
  heff_polled_room(n);
  n = 0;
  for (HeffWait *w = heff_waits_first; w; w = w->next)
    if (!w->done && w->fd >= 0) {
      heff_polled[n].fd = w->fd;
      heff_polled[n].events = w->events;
      heff_polled[n].revents = 0;
      heff_polled_waits[n] = w;
      n++;
    }
  for (HeffSource *s = heff_sources; s; s = s->next) {
    int t = -1;
    s->at = n;
    s->fds(heff_polled + n, s->n, &t);
    for (int i = 0; i < s->n; i++) heff_polled[n + (size_t)i].revents = 0;
    n += (size_t)s->n;
    if (t >= 0 && (ms < 0 || t < ms)) ms = t;
  }
  if (n == 0 && ms < 0) heff_fail("the reactor waits, with nothing to wait for");
  int r = poll(heff_polled, (nfds_t)n, ms);
  if (r < 0 && errno != EINTR) heff_fail("the reactor's poll failed");
  if (r < 0) for (size_t i = 0; i < n; i++) heff_polled[i].revents = 0;
  for (size_t i = 0; i < mine; i++)
    if (heff_polled[i].revents) {
      HeffWait *w = heff_polled_waits[i];
      if (!w->done && w->step(w) == HEFF_STEP_DONE) w->done = 1;
    }
  for (HeffSource *s = heff_sources; s; s = s->next) s->ready(heff_polled + s->at, s->n);
  heff_reactor_wake();
}
#else
HEFF_FN void heff_reactor_turn(int wait) { (void)wait; heff_reactor_wake(); }
#endif

// | Group `g`'s finished, since it was last asked, in the order they
// | were: where `wait`, waiting for one at least while any of it is
// | pending; none where none is.
HEFF_FN V heff_reactor_finished(int64_t g, int wait) {
  for (;;) {
    int some = 0;
    for (size_t i = 0; i < heff_nfinished && !some; i++) some = heff_finished[i].group == g;
    HeffGroup *gr = heff_group_of(g);
    if (some || !gr || gr->pending == 0) break;
    heff_reactor_turn(wait);
    if (!wait) break;
  }
  // (the group's taken, in order; the others' kept, in theirs)
  V l = NTAG(TAG_Nil);
  for (size_t i = heff_nfinished; i-- > 0;)
    if (heff_finished[i].group == g) l = heff_cons(heff_finished[i].v, l);
  size_t kept = 0;
  for (size_t i = 0; i < heff_nfinished; i++)
    if (heff_finished[i].group != g) heff_finished[kept++] = heff_finished[i];
  heff_nfinished = kept;
  return l;
}

// | Group `g` closed: what is pending in it let go of, undone, and what
// | of it is finished and not asked for.
HEFF_FN void heff_reactor_close(int64_t g) {
  for (HeffWait *w = heff_waits_first, *next; w; w = next) {
    next = w->next;
    if (w->group != g) continue;
    heff_wait_unlink(w);
    V f = w->f, k = w->k;
    w->cancel(w);
    heff_drop(f);
    heff_drop(k);
  }
  size_t kept = 0;
  for (size_t i = 0; i < heff_nfinished; i++) {
    if (heff_finished[i].group == g) heff_drop(heff_finished[i].v);
    else heff_finished[kept++] = heff_finished[i];
  }
  heff_nfinished = kept;
  for (size_t i = 0; i < heff_ngroups; i++)
    if (heff_groups_open[i].id == g) {
      heff_groups_open[i] = heff_groups_open[--heff_ngroups];
      break;
    }
}

// Running
// ---------------------------------------------------------------------

HEFF_FN V heff_nomatch(V s) {
  (void)s;
  heff_fail("no case for a tag");
  return 0;
}

// Values, shown (`heff compile` without `--name`: each definition of
// the file printed as the tests print JavaScript's)
// ---------------------------------------------------------------------

// (the names of the tags and labels, each by its number: a tag's and a
// label's number is a hash of its name)
typedef struct { uint32_t id; const char *name; } HeffName;
HEFF_VAR(const HeffName *heff_tag_names);
HEFF_VAR(int heff_ntag_names);
HEFF_VAR(const HeffName *heff_lbl_names);
HEFF_VAR(int heff_nlbl_names);

HEFF_FN const char *heff_name_of(const HeffName *t, int n, uint32_t id) {
  for (int i = 0; i < n; i++) if (t[i].id == id) return t[i].name;
  return "?";
}

typedef struct { char *s; size_t n, cap; } Buf;

HEFF_FN void buf_put(Buf *b, const char *c, size_t n) {
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

HEFF_FN void buf_str(Buf *b, const char *c) { buf_put(b, c, strlen(c)); }

// Commands
// ---------------------------------------------------------------------

#ifdef _WIN32
// (a command line of `as`, each quoted as the C runtime's parser reads it
// back: malloc'd)
HEFF_FN char *heff_command_line(char **as) {
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
HEFF_FN DWORD WINAPI heff_drain(LPVOID u) {
  HeffDrain *d = u;
  char chunk[65536];
  DWORD got;
  while (ReadFile(d->h, chunk, sizeof chunk, &got, NULL) && got > 0) buf_put(d->b, chunk, got);
  return 0;
}

// (a command started, its stdin this program's, its stdout and stderr
// `out` and `err`, or 0 if it cannot be)
HEFF_FN HANDLE heff_spawn(char **as, HANDLE out, HANDLE err) {
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
HEFF_FN int64_t heff_wait(HANDLE h) {
  DWORD code = (DWORD)-1;
  WaitForSingleObject(h, INFINITE);
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return (int64_t)code;
}
#else
// (what a command writes, both streams read together, so that neither
// fills its pipe while the other is waited for)
HEFF_FN void heff_read_both(int out, int err, Buf *bo, Buf *be) {
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
HEFF_FN int64_t heff_capture(char **as, Buf *bo, Buf *be) {
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
// `\xnn`, as a string literal writes it)
HEFF_FN void buf_json_string_as(Buf *b, V v, int shown) {
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

HEFF_FN void buf_json_string(Buf *b, V v) { buf_json_string_as(b, v, 0); }

// (a tag of that name and that many arguments, its arguments `*fs`)
HEFF_FN int heff_tag_named(V v, const char *name, int n, V **fs) {
  int tag, m = 0;
  V *f = NULL;
  if (IS_NTAG(v)) tag = (int)(v >> 2);
  else if (IS_OBJ(v) && (OBJ(v)->kind == K_TAG || OBJ(v)->kind == K_TAGF)) { tag = OBJ(v)->tag; m = (int)((Tag *)v)->n; f = ((Tag *)v)->f; }
  else return 0;
  if (m != n || strcmp(heff_name_of(heff_tag_names, heff_ntag_names, (uint32_t)tag), name)) return 0;
  *fs = f;
  return 1;
}

// (a list, `Cons a (Cons b Nil)`: whether it is one, a chain that ends
// in `Nil`)
HEFF_FN int heff_is_list(V v) {
  V *f;
  while (heff_tag_named(v, "Cons", 2, &f)) v = f[1];
  return heff_tag_named(v, "Nil", 0, &f);
}

// (the `len` bytes at `p` as the prelude's `showBytes` writes them in a
// binary, `b[...]`: each run of printable ASCII a string, `"` and `\`
// escaped, any other byte its number; whether any is written)
HEFF_FN int heff_show_byte_pieces(Buf *b, const uint8_t *p, int64_t len) {
  int run = 0, any = 0;
  for (int64_t i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c >= 0x20 && c < 0x7f) {
      if (!run) { buf_str(b, any ? ", \"" : "\""); run = any = 1; }
      if (c == '"') buf_str(b, "\\\"");
      else if (c == '\\') buf_str(b, "\\\\");
      else { char o = (char)c; buf_put(b, &o, 1); }
    } else {
      char tmp[8];
      if (run) { buf_str(b, "\""); run = 0; }
      snprintf(tmp, sizeof tmp, "%s%u", any ? ", " : "", (unsigned)c);
      buf_str(b, tmp);
      any = 1;
    }
  }
  if (run) buf_str(b, "\"");
  return any;
}

HEFF_FN void heff_show_bytes(Buf *b, V v) {
  Str *s = (Str *)v;
  buf_str(b, "b[");
  heff_show_byte_pieces(b, s->s, s->len);
  buf_str(b, "]");
}

// (bits as the prelude's `showBits` writes them: their whole bytes as
// bytes are, then the bits left, `v::k`, or, none left, `b[]::bits`)
HEFF_FN void heff_show_bits(Buf *b, V v) {
  Str *s = (Str *)v;
  int64_t k = (8 - (int64_t)s->h.tag) & 7, whole = k ? s->len - 1 : s->len;
  char tmp[32];
  buf_str(b, "b[");
  int any = heff_show_byte_pieces(b, s->s, whole);
  if (k) snprintf(tmp, sizeof tmp, "%s%u::%d", any ? ", " : "", (unsigned)(s->s[whole] >> (8 - k)), (int)k);
  else snprintf(tmp, sizeof tmp, "%sb[]::bits", any ? ", " : "");
  buf_str(b, tmp);
  buf_str(b, "]");
}

HEFF_FN void heff_show(Buf *b, V v, int arg);

// (field `i` of the tag `v` shown, as a `V`: boxed, if it is flat, #64)
HEFF_FN void heff_show_field(Buf *b, V v, int i, int arg) {
  V x = heff_tget_v(v, i);
  heff_show(b, x, arg);
  heff_drop(x);
}

HEFF_FN void heff_show(Buf *b, V v, int arg) {
  char tmp[64];
  if (heff_is_list(v)) {
    V *f;
    buf_str(b, "[");
    for (int i = 0; heff_tag_named(v, "Cons", 2, &f); i++, v = f[1]) {
      if (i) buf_str(b, ", ");
      heff_show_field(b, v, 0, 0);
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
    case K_TAG: case K_TAGF: tag = o->tag; n = (int)((Tag *)v)->n; fs = ((Tag *)v)->f; break;
    case K_FLT: {
      heff_js_number(FLT_OF(v), tmp);
      if (!strpbrk(tmp, ".eNI")) strcat(tmp, ".0");
      if (arg && tmp[0] == '-') { buf_str(b, "("); buf_str(b, tmp); buf_str(b, ")"); }
      else buf_str(b, tmp);
      return;
    }
    case K_STR: buf_json_string_as(b, v, 1); return;
    case K_BYT: heff_show_bytes(b, v); return;
    case K_BIT: heff_show_bits(b, v); return;
    case K_ARR: {
      Arr *r = (Arr *)v;
      buf_str(b, "[");
      for (int64_t i = 0; i < r->n; i++) {
        if (i) buf_str(b, ", ");
        V x = heff_arr_get(r, i);
        heff_show(b, x, 0);
        heff_drop(x);
      }
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
  for (int i = 0; i < n; i++) { buf_str(b, " "); heff_show_field(b, v, i, 1); }
  if (n && arg) buf_str(b, ")");
}

// (a definition by its name, the file's own, `heff_globals`; its value
// asked for, the thing it is shown, or why it failed)
typedef struct { const char *name; V (*get)(void); } Global;

HEFF_FN void heff_check_one(Buf *out, const Global *g) {
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
// and a call of itself put in a tag a loop,
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
