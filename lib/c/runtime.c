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
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <alloca.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define HEFF_KQUEUE 1
#include <sys/event.h>
#endif
#if defined(__linux__)
#define HEFF_EPOLL 1
#include <malloc.h>
#include <sys/epoll.h>
#if defined(__has_include)
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#if defined(IORING_FEAT_EXT_ARG) && defined(__NR_io_uring_setup)
#define HEFF_URING 1
#endif
#endif
#endif
#endif
#if defined(HEFF_KQUEUE) || defined(HEFF_EPOLL)
#define HEFF_EVQ 1
#endif
#endif
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
// its room, where an abandon of it is caught).  An object is one thread's,
// counted by it alone, not atomically; or shared (`SHARED`, #66),
// counted atomically by every thread that holds it; or immortal,
// counted by none.  What a provider gives another thread it shares
// (`heff_share`), and runs there under `heff_try`, ending the thread by
// `heff_thread_end`; a failure there ends the program, as on its own.  A definition's value is computed once, under a
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
typedef struct { V f[5]; } HeffT5;
typedef struct { V f[6]; } HeffT6;
typedef struct { V f[7]; } HeffT7;
typedef struct { V f[8]; } HeffT8;
typedef struct { V f[9]; } HeffT9;
typedef struct { V f[10]; } HeffT10;
typedef struct { V f[11]; } HeffT11;
typedef struct { V f[12]; } HeffT12;

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

static inline uint64_t heff_fmix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdu;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53u;
  return h ^ (h >> 33);
}

static inline uint64_t heff_fx_step(uint64_t h, uint64_t w) {
  return (((h << 5) | (h >> 59)) ^ w) * 0x517cc1b727220a95u;
}

static inline uint64_t heff_le64(const uint8_t *p) {
  uint64_t w;
  memcpy(&w, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  w = __builtin_bswap64(w);
#endif
  return w;
}

static inline uint64_t heff_hash_bytes(const uint8_t *p, int64_t n) {
  uint64_t h = (uint64_t)n;
  int64_t i = 0;
  for (; i + 8 <= n; i += 8) h = heff_fx_step(h, heff_le64(p + i));
  if (i < n) {
    uint8_t last[8] = {0};
    memcpy(last, p + i, (size_t)(n - i));
    h = heff_fx_step(h, heff_le64(last));
  }
  return heff_fmix64(h);
}

static inline int heff_u8_decode(const uint8_t *p, int64_t n, uint32_t *out) {
  uint8_t first = p[0];
  if (HEFF_LIKELY(first < 0x80)) { *out = first; return 1; }
  int width;
  uint32_t rune;
  if (first >= 0xC2 && first <= 0xDF) { width = 2; rune = first & 0x1F; }
  else if (first >= 0xE0 && first <= 0xEF) { width = 3; rune = first & 0x0F; }
  else if (first >= 0xF0 && first <= 0xF4) { width = 4; rune = first & 0x07; }
  else { *out = 0xFFFD; return 1; }
  uint8_t lo = first == 0xE0 ? 0xA0 : first == 0xF0 ? 0x90 : 0x80;
  uint8_t hi = first == 0xED ? 0x9F : first == 0xF4 ? 0x8F : 0xBF;
  int k = 1;
  for (; k < width && k < n; k++) {
    uint8_t next = p[k];
    if (next < (k == 1 ? lo : 0x80) || next > (k == 1 ? hi : 0xBF)) break;
    rune = (rune << 6) | (next & 0x3F);
  }
  if (k == width) { *out = rune; return width; }
  *out = 0xFFFD;
  return k;
}

static inline int heff_u8_encode(uint8_t *o, uint32_t u) {
  if (u < 0x80) { o[0] = (uint8_t)u; return 1; }
  if (u < 0x800) {
    o[0] = (uint8_t)(0xC0 | (u >> 6));
    o[1] = (uint8_t)(0x80 | (u & 0x3F));
    return 2;
  }
  if (u < 0x10000) {
    o[0] = (uint8_t)(0xE0 | (u >> 12));
    o[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
    o[2] = (uint8_t)(0x80 | (u & 0x3F));
    return 3;
  }
  o[0] = (uint8_t)(0xF0 | (u >> 18));
  o[1] = (uint8_t)(0x80 | ((u >> 12) & 0x3F));
  o[2] = (uint8_t)(0x80 | ((u >> 6) & 0x3F));
  o[3] = (uint8_t)(0x80 | (u & 0x3F));
  return 4;
}

static inline int heff_u8_own(const uint8_t *p, uint32_t u, int w) {
  return u != 0xFFFD || (w == 3 && p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD);
}

static inline int64_t heff_u8_valid_len(const uint8_t *p, int64_t n, int *own) {
  int64_t made = 0;
  *own = 1;
  for (int64_t i = 0; i < n;) {
    uint32_t u;
    int w = heff_u8_decode(p + i, n - i, &u);
    if (heff_u8_own(p + i, u, w)) made += w;
    else { *own = 0; made += 3; }
    i += w;
  }
  return made;
}

static inline uint64_t heff_bits_read(const uint8_t *s, int64_t at, int64_t n) {
  uint64_t u = 0;
  if ((at & 7) == 0 && (n & 7) == 0)
    for (int64_t i = at >> 3, e = (at + n) >> 3; i < e; i++) u = (u << 8) | s[i];
  else
    for (int64_t i = at; i < at + n; i++) u = (u << 1) | ((s[i >> 3] >> (7 - (i & 7))) & 1);
  return u;
}

static inline void heff_bits_write(uint8_t *s, int64_t at, uint64_t w, int64_t n) {
  if ((at & 7) == 0 && (n & 7) == 0) {
    for (int64_t i = 0; i < n >> 3; i++) s[(at >> 3) + i] = (uint8_t)(w >> (n - 8 * (i + 1)));
    return;
  }
  for (int64_t i = 0; i < n; i++)
    if ((w >> (n - 1 - i)) & 1) s[(at + i) >> 3] |= (uint8_t)(0x80 >> ((at + i) & 7));
}

static inline uint64_t heff_bits_little(uint64_t u, int64_t n, int out) {
  int64_t k = n >> 3, r = n & 7;
  uint64_t w = 0;
  if (out) {
    for (int64_t i = 0; i < k; i++) w = (w << 8) | ((u >> (8 * i)) & 255);
    if (r) w = (w << r) | ((u >> (8 * k)) & ((UINT64_C(1) << r) - 1));
    return w;
  }
  if (r) w = (u & ((UINT64_C(1) << r) - 1)) << (8 * k);
  for (int64_t i = 0; i < k; i++) w |= ((u >> (n - 8 * (i + 1))) & 255) << (8 * i);
  return w;
}

// the tags the runtime knows; a program's own are numbered after them
enum {
  TAG_True, TAG_False, TAG_Nil, TAG_Cons, TAG_MkUnit, TAG_Just, TAG_Nothing,
  TAG_MkPair, TAG_InL, TAG_InR, TAG_KNOWN
};

// (`K_TAGF`, a tag with flat fields (#64): one without is given back as
// ever, nothing looked for in it)
// (`K_FGN`, a foreign value: the host's thing, a pointer, and what lets
// it go ("Foreign values", below))
enum { K_TAG, K_REC, K_CLO, K_PAP, K_STR, K_FLT, K_ARR, K_BNC, K_CELL, K_BIG, K_BYT, K_TAGF, K_BIT, K_FGN };

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
// (a string, `Bytes` (`K_BYT`) or `Bits` (`K_BIT`): its `len` bytes, at
// `p`, which every read of them goes by.  Its own bytes are after it,
// `s`, room for `cap` of them, and `p` is `s`; unless it is a view, `p`
// in another's bytes -- its root's, never a view itself -- and that root
// held where its own bytes would be, `cap` the room it takes ("Slices").
// A view's bytes are its root's: never written, nor grown in place)
typedef struct { Obj h; int64_t len; int64_t cap; uint8_t *p; uint8_t s[]; } Str;
#define HEFF_STR_VIEW(x) ((x)->p != (x)->s)
#define HEFF_STR_ROOT(x) ((V *)(x)->s)
// (a string written in the program, static: its `n` bytes, as a C string
// literal has them -- the NUL C ends it with, after them, no byte of it)
#define HEFF_STATIC_STR(name, kind, n, bytes) \
  static struct { Obj h; int64_t len; int64_t cap; uint8_t *p; uint8_t s[(n) + 1]; } name = {{0x80000000u, kind, 0}, n, n, name.s, bytes}
#define HEFF_LIT(name, n, bytes) HEFF_STATIC_STR(name, K_STR, n, bytes)
// (`Bytes` written in the program, a binary written whole, `b["hi", 255]`,
// static as a string is)
#define HEFF_BYTES(name, n, bytes) HEFF_STATIC_STR(name, K_BYT, n, bytes)
// (a tag, a record of a definition's value of constants, static as a
// string written in the program is, its `size` fields -- more than none,
// none flat -- after it (the compiler's heff.compile.statics: one with
// flat fields it writes as a struct of its own))
#define HEFF_TAG_LIT(name, id, size, ...) \
  static struct { Obj h; int32_t n; uint32_t flat; V f[size]; } name = {{0x80000000u, K_TAG, id}, size, 0, {__VA_ARGS__}}
#define HEFF_REC_LIT(name, size, shape, ...) \
  static struct { Obj h; int32_t n; uint32_t flat; uint32_t *lbl; V f[size]; } name = {{0x80000000u, K_REC, 0}, size, 0, shape, {__VA_ARGS__}}
typedef struct { Obj h; double d; } Flt;
// (a foreign value: a pointer of the host's, the function that lets it
// go, called when the program drops the last reference; and, of a thing
// that can be ended before it is let go of, the function that ends it
// -- 0, or why it failed -- whether it is ended already, and the
// custodian it is in the custody of, 0 for the root's, and where it is
// among that one's things ("Foreign values", below))
typedef struct Fgn {
  Obj h;
  void *p;
  void (*let_go)(void *);
  const char *(*end)(void *);
  V owner;
  int32_t at;
  int32_t ended;
} Fgn;
// (an array: its elements, `n` of them, `V`s or flat -- a number's C
// type, as its header's `tag` says, `HEFF_ARR_V` or another of these
// ("Arrays", below), in the low byte of the tag.  Its room is told by
// the tag too, not by a field, so that the header stays 16 bytes and the
// elements aligned to 16 as the block is (at 24, a vectorised loop over
// `double`s splits its loads and stores across cache lines; at 32, a
// hash map's nodes take a tenth more): bits 8 to 13, `e`, 0 where the
// room is `n`, as an array is made, else the room is 2 to the `e`, `n`
// or more -- an array grown in place, `push`ed or `append`ed to, its
// room a power of 2, at least 4 (`heff_arr_cap`))
typedef struct { Obj h; int64_t n; V a[]; } Arr;
enum {
  HEFF_ARR_V, HEFF_ARR_F64, HEFF_ARR_I64, HEFF_ARR_U64, HEFF_ARR_I32, HEFF_ARR_U32,
  HEFF_ARR_I16, HEFF_ARR_U16, HEFF_ARR_I8, HEFF_ARR_U8, HEFF_ARR_F32
};
// (the bytes an element of each takes)
static const uint8_t heff_arr_width[] = { sizeof(V), 8, 8, 8, 4, 4, 2, 2, 1, 1, 4 };
// (how it keeps its elements, `HEFF_ARR_V` or another; its room's bits)
#define HEFF_ARR_KIND(r) ((unsigned)(r)->h.tag & 0xffu)
#define HEFF_ARR_ROOM 8
// (how many elements it has room for)
#define heff_arr_cap(r) \
  ((r)->h.tag >> HEFF_ARR_ROOM ? (int64_t)1 << ((r)->h.tag >> HEFF_ARR_ROOM) : (r)->n)
#define heff_arr_bytes(r) (sizeof(Arr) + (size_t)heff_arr_cap(r) * heff_arr_width[HEFF_ARR_KIND(r)])
// (the elements an object's walk sees: a flat array's none)
#define heff_arr_slots(r) (HEFF_ARR_KIND(r) == HEFF_ARR_V ? (r)->n : 0)
typedef struct { Obj h; V fun; int32_t n; V a[]; } Bnc;
typedef struct { Obj h; int32_t neg; int32_t n; uint64_t d[]; } Big;

#define IS_INT(v) (((v) & 1) == 1)
#define INT_OF(v) ((int64_t)(v) >> 1)
#define MK_INT(n) ((V)((((uint64_t)(int64_t)(n)) << 1) | 1))
#define IS_NTAG(v) (((v) & 3) == 2)
#define NTAG(id) ((V)(((uintptr_t)(id) << 2) | 2))
#define OBJ(v) ((Obj *)(v))
// (a word's integers: from -INT_LIM up to, not with, INT_LIM)
#define INT_LIM ((int64_t)1 << 62)
#define IS_OBJ(v) (((v) & 3) == 0)
// (an object's marks, the high bits of its count: the highest,
// `IMMORTAL`, set where the object is not this thread's alone -- alone,
// it is immortal, counted by none -- so that a count's fast path tests
// one bit, `heff_mine`)
#define IMMORTAL 0x80000000u
#define heff_mine(rc) (!((rc) & IMMORTAL))

// (an object shared between threads (#66, `heff_share`): counted by
// them all, atomically, and freed by the one that lets it go last --
// marked `SHARED`, both high bits, the one below the highest telling it
// from an immortal one.  What a shared object holds is shared or
// immortal; a thread's own object may hold a shared one, not the other
// way.  Without `HEFF_THREADS`, no object is: `SHARED` is 0, and a
// count is read and changed as it always was.  Threads: a count is read
// atomically too, relaxed -- a plain load, to the processor -- since
// another thread may change a shared one meanwhile)
#ifdef HEFF_THREADS
#define HEFF_SHARED_BIT 0x40000000u
#define SHARED (IMMORTAL | HEFF_SHARED_BIT)
#define heff_shared(rc) ((rc) & HEFF_SHARED_BIT)
// (an object of a cown's (boc's, `heff_own_at`): one
// behaviour's at a time, the one holding the cown, so counted as a
// thread's own, but out of line, where what a body does to it is kept
// (`HeffOwnLog`) -- marked `OWNED`, the highest bit and the one two
// below it.  `TOUCHED`, one below that, where the body's log holds it
// once, counted up)
#define HEFF_OWNED_BIT 0x20000000u
#define OWNED (IMMORTAL | HEFF_OWNED_BIT)
#define heff_owned(rc) ((rc) & HEFF_OWNED_BIT)
#define HEFF_TOUCHED_BIT 0x10000000u
#define heff_immortal(rc) (((rc) & (SHARED | OWNED)) == IMMORTAL)
#define heff_rc(o) __atomic_load_n(&(o)->rc, __ATOMIC_RELAXED)
#else
#define SHARED 0u
#define OWNED 0u
#define HEFF_TOUCHED_BIT 0u
#define heff_shared(rc) 0
#define heff_owned(rc) 0
#define heff_immortal(rc) ((rc) & IMMORTAL)
#define heff_rc(o) ((o)->rc)
#endif
// (an object's count but its marks)
#define HEFF_COUNT(rc) ((rc) & ~(IMMORTAL | SHARED | OWNED | HEFF_TOUCHED_BIT))

HEFF_FN void heff_free(V v);
HEFF_FN void heff_fail(const char *why);

#ifdef HEFF_THREADS
// (the fast paths below test the highest mark, as one thread does, and
// leave an immortal object as it is where they are; a shared one is
// counted where it is tested, up -- an instruction, where the processor
// has one, as arm64's and x86's have, where a call, even one never
// made, costs the function it is in a frame and a way out through it --
// and down, out of line, the test of its last reference a constant the
// compiler would keep in a register of its own all through the
// function.  As those of one thread, they are copied into each place
// they are used)
//
// (a shared object counted down: whether that was its last reference,
// which this thread then frees -- acquiring what every other thread did
// with it before it let it go.  One made immortal meanwhile
// (`heff_freeze`, of a static thunk's value) is never its last: what
// made it so holds it)
HEFF_FN __attribute__((noinline)) int heff_down_shared(Obj *o) {
  uint32_t was = __atomic_fetch_sub(&o->rc, 1, __ATOMIC_ACQ_REL);
  return heff_shared(was) && HEFF_COUNT(was) == 1;
}

// (what a behaviour's body did to its cowns' objects, kept while it
// runs (boc's provider), to be finished once it is done or has failed:
// the owned objects it counted up, each held here once (`TOUCHED`);
// the cowns it read, each held here, by the value that has it, with
// what it held, held here once too, and what it is to hold after; the
// arrays of what a cown holds that it writes in place, each with the
// cown's and the slot that holds it there (none where the cown does);
// and the elements it wrote of them, with what was there -- held here,
// where an array keeps `V`s, else its bits)
typedef struct { void *cell; V held, v, put; int written; } HeffOwnRoot;
typedef struct { Arr *r; V *at; int64_t root; } HeffOwnArr;
typedef struct { Arr *r; int64_t i; uint64_t old; } HeffOwnSet;
typedef struct {
  V *touched;
  int64_t nt, ct;
  HeffOwnRoot *roots;
  int64_t nr, cr;
  HeffOwnArr *arrs;
  int64_t na, ca;
  HeffOwnSet *sets;
  int64_t ns, cs;
  // (the first cells' entries, most bodies' all: none allocated)
  HeffOwnRoot first[4];
} HeffOwnLog;
HEFF_VAR(HEFF_TLS HeffOwnLog *heff_own_log);

// (room in one of a log's lists for one more)
HEFF_FN void *heff_own_room(void *p, int64_t n, int64_t *cap, size_t size) {
  if (n < *cap) return p;
  *cap = *cap ? *cap * 2 : 8;
  p = realloc(p, (size_t)*cap * size);
  if (!p) heff_fail("out of memory");
  return p;
}

// (the owned object held by the log, where it is not yet: one more
// count, the log's)
static inline uint32_t heff_own_touch(Obj *o, uint32_t rc) {
  HeffOwnLog *g = heff_own_log;
  if (!g || (rc & HEFF_TOUCHED_BIT)) return rc;
  g->touched = heff_own_room(g->touched, g->nt, &g->ct, sizeof(V));
  g->touched[g->nt++] = (V)o;
  return (rc | HEFF_TOUCHED_BIT) + 1;
}

// (an owned object counted up `m` more: as a thread's own -- only the
// behaviour holding its cown has it -- and kept by the body's log)
HEFF_FN __attribute__((noinline)) void heff_own_dup(Obj *o, uint32_t m) {
  o->rc = heff_own_touch(o, o->rc) + m;
}

// (... down: whether that was its last reference)
HEFF_FN __attribute__((noinline)) int heff_own_down(Obj *o) {
  uint32_t rc = o->rc - 1;
  o->rc = rc;
  return HEFF_COUNT(rc) == 0;
}

HEFF_FAST V heff_dup(V v) {
  if (IS_OBJ(v)) {
    uint32_t rc = heff_rc(OBJ(v));
    if (HEFF_LIKELY(heff_mine(rc))) OBJ(v)->rc = rc + 1;
    else if (heff_shared(rc)) __atomic_fetch_add(&OBJ(v)->rc, 1, __ATOMIC_RELAXED);
    else if (heff_owned(rc)) heff_own_dup(OBJ(v), 1);
  }
  return v;
}

// (a count taken down: whether that was its last reference -- never of
// an immortal object)
static inline int heff_down(Obj *o) {
  uint32_t rc = heff_rc(o);
  if (HEFF_LIKELY(heff_mine(rc))) return (o->rc = rc - 1) == 0;
  if (heff_shared(rc)) return heff_down_shared(o);
  return heff_owned(rc) && heff_own_down(o);
}

// (a count taken down where it is not the last unless the object is
// shared -- the caller knows it held more than once, or immortal: the
// last of a shared one frees it)
HEFF_FAST void heff_unref(V v) {
  uint32_t rc = heff_rc(OBJ(v));
  if (HEFF_LIKELY(heff_mine(rc))) OBJ(v)->rc = rc - 1;
  else if (heff_shared(rc)) { if (heff_down_shared(OBJ(v))) heff_free(v); }
  else if (heff_owned(rc) && heff_own_down(OBJ(v))) heff_free(v);
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

// (an object held by the caller alone, its count 1: its last reference,
// to take apart or build in.  Threads: a shared one too, its count back
// to 1 -- the other threads let it go, and none can take it again but
// through a reference the caller holds.  It is read again, acquiring
// what they did with it before they let it go, and unshared: counted by
// this thread alone from then on, so that what is built in it may be
// the thread's own, as a shared object's may not.  What it holds stays
// as it is, shared or not.  Never an immortal one)
#ifdef HEFF_THREADS
HEFF_FN __attribute__((noinline)) int heff_unshare(Obj *o) {
  if (__atomic_load_n(&o->rc, __ATOMIC_ACQUIRE) != (SHARED | 1)) return 0;
  o->rc = 1;
  return 1;
}

// (an owned one counted once: unmarked, a thread's own from then on.
// No object of a cell a body has read is held by the body alone: the
// cell holds it too, or what holds it there, so the body counted it up)
HEFF_FN __attribute__((noinline)) int heff_own_alone(Obj *o) {
  if (HEFF_COUNT(o->rc) != 1) return 0;
  o->rc = 1;
  return 1;
}

// (a shared one's count compared out of line, as `heff_down_shared`
// compares it)
static inline int heff_alone(Obj *o) {
  uint32_t rc = heff_rc(o);
  if (HEFF_LIKELY(rc == 1)) return 1;
  if (heff_shared(rc)) return heff_unshare(o);
  return heff_owned(rc) && heff_own_alone(o);
}
#else
static inline int heff_alone(Obj *o) { return o->rc == 1; }
#endif

static inline int heff_unique(V v) { return heff_alone(OBJ(v)); }

// The first thread, lent (#142).  The program runs on a thread of its
// own, its stack large and watched; the process's first thread only
// waits for its end.  A toolkit that must have that thread for its own
// (AppKit's run loop, UIKit's) is given it by a provider:
// `heff_first_thread(run, stop, u)` asks for `run(u)` there -- 0 where
// it is queued, 1 where a job is lent already (queued or running: one
// at a time), 2 where the program is ending.  The first thread runs it
// as soon as it is free; where `run` returns, the thread waits again,
// for the program's end or another job; where it never does, the
// thread is the toolkit's from then on.  No heff code runs there: what
// the program asks of the toolkit goes by the toolkit's own way to its
// thread (macOS's main queue), what the toolkit tells the program by
// the provider's (a queue and a condition, or a byte on a pipe the
// reactor waits on).  `heff_is_first_thread()`, from `main` on, says
// whether the caller is on it.
//
// The program's end while a job runs there (`heff_end`: its thread
// returning, or `exit n` on any thread -- not a failure, which ends it
// at once): with no `stop`, the ending thread
// ends the process itself, its output flushed -- the first thread need
// not come back; with `stop`, it calls `stop(u)`, which is to make
// `run` return, and waits, and the first thread, `run` returned, ends
// the process by the program's status, as it does where nothing is
// lent.  Nothing times a `stop` out: winding the toolkit up is the
// provider's, and a toolkit cut short mid-run is what `stop` is there
// to spare.  (`run` may return on its own as `stop` is called: `stop`
// only asks, never waits.)  A job queued, not begun, when the program
// ends is not run.
// The program ends once: a thread ending it after another waits, and
// the thread ending it, ending it again (an `atexit` handler's `exit`),
// ends it at once, as `exit` does.
//
// Its end runs the C libraries' own cleanup (`exit`: their `atexit`
// handlers, their destructors).  So a provider whose threads of its own
// call into a library has them done with it by then: its release waits
// for its thread to be through, as the library's own close would.  A
// thread still in the library as it is torn down may crash the program
// at its very end.
typedef struct {
  void (*run)(void *);
  void (*stop)(void *);
  void *u;
  int lent;     // a job queued or running
  int running;  // begun on the first thread
  int ended;    // 0; 1, the first thread ends the process; 2, another
  int waits;    // whether the ending thread waits for the first
  int status;
  pthread_t first, ender;
  int known;    // whether `first` is set
} HeffFirst;
#ifdef HEFF_PART
extern HeffFirst heff_first;
extern pthread_mutex_t heff_first_mu;
extern pthread_cond_t heff_first_cv;
#else
HeffFirst heff_first;
pthread_mutex_t heff_first_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t heff_first_cv = PTHREAD_COND_INITIALIZER;
#endif

HEFF_FN int heff_is_first_thread(void) {
  return heff_first.known && pthread_equal(pthread_self(), heff_first.first);
}

HEFF_FN int heff_first_thread(void (*run)(void *), void (*stop)(void *), void *u) {
  pthread_mutex_lock(&heff_first_mu);
  int r = heff_first.ended ? 2 : heff_first.lent ? 1 : 0;
  if (r == 0) {
    heff_first.run = run;
    heff_first.stop = stop;
    heff_first.u = u;
    heff_first.lent = 1;
    pthread_cond_broadcast(&heff_first_cv);
  }
  pthread_mutex_unlock(&heff_first_mu);
  return r;
}

// (the program's end, by `status`, begun on this thread: `returned`,
// the program's thread returning, which leaves the end to the first
// thread where nothing runs there, and returns; every other end this
// thread's, as before anything was lent)
HEFF_FN void heff_end(int status, int returned) {
  pthread_t self = pthread_self();
  pthread_mutex_lock(&heff_first_mu);
  if (heff_first.ended) {
    if (pthread_equal(heff_first.ender, self)) {
      pthread_mutex_unlock(&heff_first_mu);
      exit(status);
    }
    for (;;) pthread_cond_wait(&heff_first_cv, &heff_first_mu);
  }
  heff_first.ender = self;
  heff_first.status = status;
  if (heff_first.running && heff_first.stop && !heff_is_first_thread()) {
    void (*stop)(void *) = heff_first.stop;
    void *u = heff_first.u;
    heff_first.ended = 1;
    heff_first.waits = 1;
    pthread_mutex_unlock(&heff_first_mu);
    stop(u);
    pthread_mutex_lock(&heff_first_mu);
    for (;;) pthread_cond_wait(&heff_first_cv, &heff_first_mu);
  }
  if (returned && !heff_first.running) {
    heff_first.ended = 1;
    pthread_cond_broadcast(&heff_first_cv);
    pthread_mutex_unlock(&heff_first_mu);
    return;
  }
  heff_first.ended = 2;
  pthread_mutex_unlock(&heff_first_mu);
  fflush(stdout);
  exit(status);
}

// (what a provider runs once the program's computation has returned, on
// the program's thread, before its end: boc's, its behaviours waited
// for.  Nonzero, the program ends by 1 where it would by 0)
HEFF_VAR(int (*heff_returned)(void));

// (the program ended by `status`, from any thread of it)
HEFF_FN __attribute__((noreturn)) void heff_exit(int status) {
  heff_end(status, 0);
  exit(status);
}

// (the program ended by a failure, by `status`, from any thread of it,
// at once: its output flushed, what was put by `at_quick_exit` run)
HEFF_FN __attribute__((noreturn)) void heff_fail_exit(int status) {
  fflush(stdout);
  quick_exit(status);
}

// Failures.  A failure -- a panic, or the runtime's own (out of memory,
// applying what is not a function, a definition defined by itself, a
// stack that overflowed) -- ends the program at once, from whichever
// thread it is on: said on stderr (`heff: why`), and the process exited
// by 1 (an overflow, 2), nothing unwound, nothing shut down first.  So
// what the program held -- a connection, a transaction, a file -- the
// system closes, and whoever was at its other end sees it closed, as
// it would not if the program went on without the part that failed.
// It is ended by `quick_exit` (`heff_fail_exit`): what a provider must
// put back even then -- the terminal's mode, the commands it started --
// it puts back by `at_quick_exit`; the C libraries' own cleanup is not
// run, since other threads may be in them still.
//
// What may be caught is an abandon (`heff_abandon`) and fuel run out
// (std.fuel's limit): each jumps to the innermost catch (`heff_catch`,
// set by `heff_try`), where there is one, else is said and the program
// ended by 1.  Where a value is asked for that may abandon -- `heff
// compile` without `--name`, each definition printed -- the abandon is
// caught there, and said.
//
// Jumped to by `_longjmp`, which neither saves nor restores the signal
// mask: macOS's `setjmp` does, a call of the system each time, a third
// of a fork's cost.  A stack that overflowed comes back by a way of its
// own, which restores the mask.
HEFF_VAR(HEFF_TLS jmp_buf *heff_catch);
HEFF_VAR(HEFF_TLS const char *heff_caught);
#ifdef _WIN32
#define HEFF_SETJMP(b) setjmp(b)
#define HEFF_LONGJMP(b) longjmp(b, 1)
#else
#define HEFF_SETJMP(b) _setjmp(b)
#define HEFF_LONGJMP(b) _longjmp(b, 1)
#endif

HEFF_FN void heff_fail(const char *why) {
  fflush(stdout);
  fprintf(stderr, "heff: %s\n", why);
  heff_fail_exit(1);
}

HEFF_FN V heff_abandon(void) {
  if (heff_catch) { heff_caught = "heff:abandon"; HEFF_LONGJMP(*heff_catch); }
  fflush(stdout);
  fprintf(stderr, "heff:abandon\n");
  heff_fail_exit(1);
}

// (a computation stopped, `why` saying so -- fuel run out: caught as an
// abandon is, else a failure)
HEFF_FN void heff_stopped(const char *why) {
  if (heff_catch) { heff_caught = why; HEFF_LONGJMP(*heff_catch); }
  heff_fail(why);
}

HEFF_FN V heff_nomatch(V s);

// (`panic why`: a failure, `why` its words -- the program ended, said)
HEFF_FN char *heff_cstr_text(V v);
HEFF_FN V heff_panic(V s) {
  char *why = heff_cstr_text(s);
  heff_drop(s);
  heff_fail(why ? why : "panic");
  return NTAG(TAG_MkUnit);
}

HEFF_FN void *heff_alloc(size_t n) {
  void *p = malloc(n);
  if (!p) heff_fail("out of memory");
  return p;
}

// Large blocks, kept (Linux).  A block of 64 KiB to 4 MiB -- a read's
// buffer, a file's chunk, a long string or array -- given back is kept,
// a few of each size, for the next of that size, not given back to the
// system: a static build's malloc (musl's, zig's) maps and unmaps each
// such block afresh, a system call each way and every page of it faulted
// in again, where glibc's keeps them -- a server's every read of a
// socket was a map and an unmap.  Sizes are rounded up, eight to each
// power of two, so that what is kept fits what is asked; a block is kept
// only where the system says it is as large as its size rounded
// (`malloc_usable_size`), so that one grown in place (`realloc`) is kept
// by what it is.  Each thread keeps its own, `HEFF_BIG_KEEP` of a size
// and `HEFF_BIG_ROOM` bytes in all, given back as it ends.  Not where
// objects are given back to the system at once (`SMALL_MAX` 0, for
// AddressSanitizer); elsewhere, the system's malloc keeps them itself.
// `heff_big_alloc` and `heff_big_free` are a provider's too, for a
// buffer of its own, its size given back with it.
#if defined(__linux__) && (!defined(SMALL_MAX) || SMALL_MAX != 0)
#define HEFF_BLOCKS 1
#define HEFF_BIG_SIZES 49
#define HEFF_BIG_KEEP 4
#define HEFF_BIG_ROOM ((size_t)32 << 20)
HEFF_VAR(HEFF_TLS void *heff_big[HEFF_BIG_SIZES][HEFF_BIG_KEEP]);
HEFF_VAR(HEFF_TLS uint8_t heff_nbig[HEFF_BIG_SIZES]);
HEFF_VAR(HEFF_TLS size_t heff_big_kept);

// (`n`'s size, rounded up -- `*r` -- as a kept block's: or -1, `n` kept
// by none)
static inline int heff_big_size(size_t n, size_t *r) {
  if (n < ((size_t)1 << 16) || n > ((size_t)1 << 22)) return -1;
  int k = 63 - __builtin_clzll((unsigned long long)n);
  size_t step = (size_t)1 << (k - 3);
  size_t q = (n + step - 1) / step;
  if (q == 16) {
    k++;
    q = 8;
    step <<= 1;
  }
  *r = q * step;
  return (k - 16) * 8 + (int)(q - 8);
}

HEFF_FN void *heff_big_alloc(size_t n) {
  size_t r;
  int i = heff_big_size(n, &r);
  if (i < 0) return heff_alloc(n);
  if (heff_nbig[i]) {
    heff_big_kept -= r;
    return heff_big[i][--heff_nbig[i]];
  }
  return heff_alloc(r);
}

HEFF_FN void heff_big_free(void *p, size_t n) {
  if (!p) return;
  size_t r;
  int i = heff_big_size(n, &r);
  if (i >= 0 && heff_nbig[i] < HEFF_BIG_KEEP && heff_big_kept + r <= HEFF_BIG_ROOM && malloc_usable_size(p) >= r) {
    heff_big[i][heff_nbig[i]++] = p;
    heff_big_kept += r;
    return;
  }
  free(p);
}

// (what this thread kept given back: as it ends)
HEFF_FN void heff_big_end(void) {
  for (int i = 0; i < HEFF_BIG_SIZES; i++) {
    while (heff_nbig[i]) free(heff_big[i][--heff_nbig[i]]);
  }
  heff_big_kept = 0;
}
#else
HEFF_FN void *heff_big_alloc(size_t n) { return heff_alloc(n); }
HEFF_FN void heff_big_free(void *p, size_t n) {
  (void)n;
  free(p);
}
#endif

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
#ifndef HEFF_THREADS
HEFF_VAR(uint64_t heff_allocs);
HEFF_VAR(uint64_t heff_alloc_bytes);
#endif
#define HEFF_COUNTED(c) (heff_allocs++, heff_alloc_bytes += (uint64_t)(c) * 8)
#else
#define HEFF_COUNTED(c) ((void)0)
#endif

// (a page of objects of one size, and a thread's pages, its heap -- below)
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
#endif

// (where a lambda's call last of more arguments than it took is put, up
// to `HEFF_ROOM` of them -- "Calls of what is not known", below)
#define HEFF_ROOM 4

// (with threads: what each thread keeps of the runtime and uses all the
// time -- its heap, its count of calls last, its room, its stack of what
// is given back, its count of what it made -- in one block of its own,
// `heff_tls()`, found once in a function for all of them.  Where a
// thread's own variable costs a call, macOS on arm64 (`_tlv_get_addr`,
// one for each variable a function uses, and a frame for the call even
// in a leaf), the block is found as the system's own thread-specific
// data is, by the register that points to it (`tpidrro_el0`) and a key
// of heff's (`heff_tls_key`), the block put in its slot on each thread
// heff starts before it runs anything of the program's
// (`heff_tls_enter`) -- the slot read with nothing checked.  Elsewhere
// C's `_Thread_local`, an offset from the thread's register already.
// Without threads, each a variable of its own, as ever)
#ifdef HEFF_THREADS
typedef struct {
#if SMALL_MAX != 0
  HeffHeap heap;
#endif
  long dep;
  V room[HEFF_ROOM];
  V *dead;
  size_t ndead, cdead;
#ifdef HEFF_COUNT_ALLOCS
  uint64_t allocs, alloc_bytes;
#endif
} HeffTls;
HEFF_VAR(_Thread_local HeffTls heff_tls_own);
#if defined(__APPLE__) && defined(__aarch64__)
HEFF_VAR(pthread_key_t heff_tls_key);
// (the block: the key read in the same instructions, out of sight of
// the compiler, which may then find the block once for a whole
// function -- the thread, its key and its slot the same all through
// it.  `tpidrro_el0`'s low bits are not the address's)
static inline HeffTls *heff_tls(void) {
  HeffTls *t;
  __asm__("adrp %0, _heff_tls_key@PAGE\n\t"
          "ldr %0, [%0, _heff_tls_key@PAGEOFF]\n\t"
          "mrs x16, tpidrro_el0\n\t"
          "and x16, x16, #~7\n\t"
          "ldr %0, [x16, %0, lsl #3]"
          : "=r"(t) : : "x16");
  return t;
}
#define HEFF_TLS_SLOT 1
#else
#define heff_tls() (&heff_tls_own)
#endif
#if SMALL_MAX != 0
#define heff_heap (heff_tls()->heap)
#endif
#define heff_dep (heff_tls()->dep)
#define heff_room (heff_tls()->room)
#define heff_dead (heff_tls()->dead)
#define heff_ndead (heff_tls()->ndead)
#define heff_cdead (heff_tls()->cdead)
#define heff_allocs (heff_tls()->allocs)
#define heff_alloc_bytes (heff_tls()->alloc_bytes)

// (the block set as this thread's, before it runs anything of the
// program's: in a function of its own, none that uses the block, so
// that no reading of it is moved before it)
HEFF_FN __attribute__((noinline)) void heff_tls_enter(void) {
#ifdef HEFF_TLS_SLOT
  if (pthread_setspecific(heff_tls_key, &heff_tls_own) != 0) {
    fputs("heff: no room for a thread's own\n", stderr);
    exit(1);
  }
#endif
}

// (the key made, as the program starts, before any thread of it: and
// the block of the thread that made it found by it, as `heff_tls`
// finds it -- else the system keeps its own otherwise than heff knows)
HEFF_FN __attribute__((noinline)) void heff_tls_found(void) {
#ifdef HEFF_TLS_SLOT
  if (heff_tls() != &heff_tls_own) {
    fputs("heff: a thread's own not found where it was put\n", stderr);
    exit(1);
  }
#endif
}

HEFF_FN void heff_tls_begin(void) {
#ifdef HEFF_TLS_SLOT
  if (pthread_key_create(&heff_tls_key, NULL) != 0) {
    fputs("heff: no room for a thread's own\n", stderr);
    exit(1);
  }
#endif
  heff_tls_enter();
  heff_tls_found();
}
#else
HEFF_FN void heff_tls_enter(void) {}
HEFF_FN void heff_tls_begin(void) {}
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
#ifdef HEFF_THREADS
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
  if (c > SMALL_MAX / 8) return heff_big_alloc(c * 8);
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
  if (c > SMALL_MAX / 8) { heff_big_free(p, c * 8); return; }
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
  if (heff_alone(&t->h)) {
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
  if (heff_alone(&t->h)) {
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
  if (heff_alone(&t->h)) {
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
  if (heff_alone(&t->h) && t->flat) {
    heff_tag_unset(t, keep, n);
    heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  } else heff_take_n(v, keep, n);
}

// (`heff_take_reuse`, of a tag of `n` fields: its fields not blanked, the
// cell kept being either built in (`heff_tag_at`, each field written) or
// freed as the cell it is (`heff_drop_cell`), never looked into)
HEFF_KNOWN V heff_take_reuse_n(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_alone(&t->h)) {
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
  if (heff_alone(&t->h) && t->flat) {
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
  if (heff_alone(&t->h)) heff_obj_free(t, sizeof(Tag) + n * sizeof(V));
  else {
    for (int64_t i = 0; i < n; i++)
      if ((keep >> i) & 1) heff_dup(t->f[i]);
    heff_unref((V)t);
  }
}

HEFF_KNOWN V heff_take_reuse_na(V v, uint64_t keep, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_alone(&t->h)) {
    t->flat = 0; t->h.kind = K_TAG;
    return v;
  }
  return heff_take_reuse_n(v, keep, n);
}

// (a tag of `n` fields dropped: its last reference, its fields dropped
// and its cell freed at once; otherwise counted down)
HEFF_KNOWN void heff_drop_tag(V v, int64_t n) {
  Tag *t = (Tag *)v;
  if (heff_alone(&t->h)) {
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

// (... the fields of `same` there already, each read from the cell at
// its place and given back there: stored only in a new one)
HEFF_KNOWN V heff_tag_re_n(V r, int id, int64_t n, uint64_t same, V *f) {
  if (!IS_OBJ(r)) return heff_tag_n(id, n, f);
  Tag *t = (Tag *)r;
  t->h.tag = (uint16_t)id;
  for (int64_t i = 0; i < n; i++)
    if (!((same >> i) & 1)) t->f[i] = f[i];
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
// for it -- a destination; `d` consumed, `v` given back, counted)
static inline V heff_hole(void) { return MK_INT(0); }

static inline V heff_setField(V d, V i, V v) {
  ((Tag *)d)->f[INT_OF(i)] = v;
  heff_drop(d);
  return heff_dup(v);
}

// (... of a cell lent, a destination's: not given up, and the value set
// given back lent, held by the cell -- no reference taken nor dropped)
static inline V heff_setFieldPeek(V d, V i, V v) {
  ((Tag *)d)->f[INT_OF(i)] = v;
  return v;
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
  if (heff_alone(&r->h)) {
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
  if (heff_alone(&r->h)) r->f[i] = MK_INT(0);
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
  if (!heff_alone(&r->h)) {
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
  if (heff_alone(&r->h)) {
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
  if (heff_alone(&c->h)) {
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
  case K_BIG: return sizeof(Big) + ((Big *)v)->n * sizeof(uint64_t);
  case K_FGN: return sizeof(Fgn);
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
// never -- with threads, each thread's, in its block)
#ifndef HEFF_THREADS
HEFF_VAR(V *heff_dead);
HEFF_VAR(size_t heff_ndead);
HEFF_VAR(size_t heff_cdead);
#endif

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

static void heff_foreign_gone(struct Fgn *g);

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
    // (a foreign value's last reference gone: the host's thing let go of)
    // (and its custodian counted down after, where it has one)
    case K_FGN: {
      Fgn *g = (Fgn *)v;
      heff_foreign_gone(g);
      if (g->owner) { n = 1; f = &g->owner; }
      size = sizeof(Fgn);
      break;
    }
    // (a view: its root counted down)
    case K_STR: case K_BYT: case K_BIT: {
      Str *x = (Str *)v; size = sizeof(Str) + x->cap;
      if (HEFF_STR_VIEW(x)) { n = 1; f = HEFF_STR_ROOT(x); }
      break;
    }
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
  if (heff_alone(&r->h)) {
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
  if (heff_alone(&c->h)) {
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

// (a tag's or a record's fields, an array's elements, a string view's
// root: `*n` of them -- a closure's, a partial application's and a
// bounce's are said by `HEFF_EACH_SLOT`; another string, a float and a
// big integer hold none)
HEFF_FN V *heff_slots(V v, int64_t *n) {
  Obj *o = OBJ(v);
  switch (o->kind) {
  case K_TAG: case K_TAGF: *n = ((Tag *)v)->n; return ((Tag *)v)->f;
  case K_REC: *n = ((Rec *)v)->n; return ((Rec *)v)->f;
  case K_ARR: *n = heff_arr_slots((Arr *)v); return ((Arr *)v)->a;
  case K_STR: case K_BYT: case K_BIT:
    *n = HEFF_STR_VIEW((Str *)v); return HEFF_STR_ROOT((Str *)v);
  default: *n = 0; return NULL;
  }
}

// (a stack of objects, for a walk that is not a recursion: a long
// list's spine never on the C stack.  The first few are held in the
// stack itself, so the walk of a small value -- a behaviour's body,
// handed to another thread -- allocates nothing)
typedef struct { V *v; size_t n, cap; V few[16]; } HeffStack;

HEFF_FN void heff_stack_push(HeffStack *s, V v) {
  if (s->n == s->cap) {
    if (!s->cap) {
      s->v = s->few;
      s->cap = sizeof s->few / sizeof(V);
    } else if (s->v == s->few) {
      s->v = malloc(s->cap * 2 * sizeof(V));
      if (!s->v) heff_fail("out of memory");
      memcpy(s->v, s->few, sizeof s->few);
      s->cap *= 2;
    } else {
      s->cap *= 2;
      s->v = realloc(s->v, s->cap * sizeof(V));
      if (!s->v) heff_fail("out of memory");
    }
  }
  s->v[s->n++] = v;
}

HEFF_FN void heff_stack_free(HeffStack *s) {
  if (s->v != s->few) free(s->v);
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
  if (heff_immortal(rc)) return;
#ifdef HEFF_THREADS
  // (a shared object, other threads counting it meanwhile: its mark
  // that tells it from an immortal one taken off, over its count, which
  // they leave as it is from then on)
  if (heff_shared(rc)) __atomic_fetch_and(&OBJ(v)->rc, ~HEFF_SHARED_BIT, __ATOMIC_RELAXED);
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
  heff_stack_free(&s);
}

#ifdef HEFF_THREADS
HEFF_FN void heff_share_one(HeffStack *s, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v) return;
  uint32_t rc = heff_rc(OBJ(v));
  if (!heff_mine(rc) && !heff_owned(rc)) return;
  OBJ(v)->rc = HEFF_COUNT(rc) | SHARED;
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
  if (!IS_OBJ(v) || !v) return v;
  uint32_t rc = heff_rc(OBJ(v));
  if (!heff_mine(rc) && !heff_owned(rc)) return v;
  HeffStack s = {0};
  heff_share_one(&s, &v);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_share_one, &s);
  }
  heff_stack_free(&s);
  return v;
}

// (one slot of what is moved: an object counted once is held through
// this slot alone, and goes on being walked, as it is; one counted more
// is held by something else too, and is shared, with all it holds)
HEFF_FN void heff_move_one(HeffStack *s, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v) return;
  uint32_t rc = heff_rc(OBJ(v));
  if (heff_owned(rc)) {
    if (HEFF_COUNT(rc) != 1) { heff_share(v); return; }
    OBJ(v)->rc = rc = 1;
  }
  if (!heff_mine(rc)) return;
  if (rc == 1) heff_stack_push(s, v);
  else heff_share(v);
}

// (`v` handed to one other thread, which holds it instead of this one
// (#125): where this thread holds `v` alone -- counted once, and not
// shared -- it is moved, its count as it is, no mark: the taker counts
// it alone from then on, as its own, and may take it apart or build in
// it in place.  Below it, each object counted once under one so held is
// the same, moved; one counted more than once something else holds too,
// so it is shared (`heff_share`), with all it holds, and the walk goes
// no further there.  A thread's own object may hold shared ones, so
// what is moved keeps the invariant.  Where `v` is not held alone, it
// is shared, as `heff_share` does.  `v` not consumed: the reference
// handed over is the caller's, which must not count it after.  The walk
// is the same as `heff_share`'s, and finds the objects held more than
// once; what it saves is every mark, and the atomic counting after)
HEFF_FN V heff_move(V v) {
  if (!IS_OBJ(v) || !v) return v;
  uint32_t rc = heff_rc(OBJ(v));
  if (heff_owned(rc) && HEFF_COUNT(rc) == 1) OBJ(v)->rc = rc = 1;
  if (heff_owned(rc)) return heff_share(v);
  if (!heff_mine(rc)) return v;
  if (rc != 1) return heff_share(v);
  HeffStack s = {0};
  heff_stack_push(&s, v);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_move_one, &s);
  }
  heff_stack_free(&s);
  return v;
}

// (one slot of what a cown is given: an object of this thread's counted
// once is held through this slot alone, and is the cown's, marked
// `OWNED`, the walk going on below it; one counted more is held by
// something else too, and is shared, with all it holds.  The walk stops
// at what is owned already: what the body counted up of that its log
// holds (`TOUCHED`), and the provider shares it after where more than
// the cown holds it; what it did not, nothing else holds)
HEFF_FN void heff_own_one(HeffStack *s, V *at) {
  V v = *at;
  if (!IS_OBJ(v) || !v) return;
  uint32_t rc = heff_rc(OBJ(v));
  if (!heff_mine(rc)) return;
  if (rc == 1) {
    OBJ(v)->rc = OWNED | 1;
    heff_stack_push(s, v);
  } else heff_share(v);
}

// (what the slot holds made the cown's, as `heff_own_one` says: a
// cown's objects are held by the one behaviour that holds the cown,
// each counted as a thread's own, and handed on with the cown, by its
// turnstile, never walked again where nothing changed them)
HEFF_FN void heff_own_at(V *at) {
  HeffStack s = {0};
  heff_own_one(&s, at);
  while (s.n) {
    V o = s.v[--s.n];
    HEFF_EACH_SLOT(o, heff_own_one, &s);
  }
  heff_stack_free(&s);
}
#else
// (one thread: nothing is shared, nothing to do)
static inline V heff_share(V v) { return v; }
static inline V heff_move(V v) { return v; }
static inline void heff_own_at(V *at) { (void)at; }
#endif

// Calls of what is not known
// ---------------------------------------------------------------------

// (the calls last counted, `heff_dep`; and where a lambda's call last of
// more arguments than it took is put, up to `HEFF_ROOM` of them, to be
// made a jump, `heff_room`: its own are where its caller put them, room
// for only as many as it takes -- and what is called reads them at
// once, before it calls anything -- see the C emitter's `roomed`.  With
// threads, each thread's, in its block)
#ifndef HEFF_THREADS
HEFF_VAR(long heff_dep);
HEFF_VAR(V heff_room[HEFF_ROOM]);
#endif

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

// (a closure taking exactly the `n` arguments at `a`, not a thunk forced:
// what `heff_apply` calls at once, as it is)
static inline int heff_called_as(V f, int n, V *a) {
  Clo *c = (Clo *)f;
  return c->h.kind == K_CLO && c->arity == n && !(n == 1 && a[0] == NTAG(TAG_MkUnit));
}

// (a call last: counted, and every so often a bounce; a closure taking
// exactly its arguments called where the call is, as `heff_apply` would
// call it, without the call of it)
static inline V heff_tail(V f, int n, V *a) {
  if (++heff_dep > 500) return heff_bounce(f, n, a);
  if (HEFF_LIKELY(heff_called_as(f, n, a))) return ((Clo *)f)->fn(f, a);
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
      if (heff_immortal(heff_rc(&c->h))) heff_freeze(r);
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
      if (!heff_mine(heff_rc(&c->h))) return heff_force_shared(f, a);
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

static inline V heff_app1(V f, V x) {
  if (HEFF_LIKELY(heff_called_as(f, 1, &x))) return ((Clo *)f)->fn(f, &x);
  return heff_apply(f, 1, &x);
}

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
// it.)  An abandon caught (`heff_try`) where values were being computed
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
  // (room for its state among the open first: from its being marked
  // until it is done, it is among them, a failure anywhere between --
  // even a stack's overflow -- leaving it to be computed again)
  if (heff_nopen == heff_capopen) {
    heff_capopen = heff_capopen ? heff_capopen * 2 : 16;
    heff_open = realloc(heff_open, heff_capopen * sizeof(HEFF_ONCE *));
    if (!heff_open) heff_fail("out of memory");
  }
  pthread_mutex_lock(&heff_once_mu);
  for (;;) {
    uintptr_t s = __atomic_load_n(st, __ATOMIC_ACQUIRE);
    if (s == 2) { pthread_mutex_unlock(&heff_once_mu); return *val; }
    if (s == me) { pthread_mutex_unlock(&heff_once_mu); heff_fail("defined by itself"); }
    if (s == 0) break;
    pthread_cond_wait(&heff_once_cv, &heff_once_mu);
  }
  __atomic_store_n(st, me, __ATOMIC_RELAXED);
  heff_open[heff_nopen++] = st;
  pthread_mutex_unlock(&heff_once_mu);
  V v = heff_tr(init());
  heff_freeze(v);
  pthread_mutex_lock(&heff_once_mu);
  *val = v;
  __atomic_store_n(st, 2, __ATOMIC_RELEASE);
  heff_nopen--;
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

// (the states of what is being computed, the last innermost)
HEFF_VAR(HEFF_ONCE **heff_open);
HEFF_VAR(size_t heff_nopen);
HEFF_VAR(size_t heff_capopen);

HEFF_FN __attribute__((noinline)) V heff_global(V *val, HEFF_ONCE *st, V (*init)(void)) {
  if (*st == 1) heff_fail("defined by itself");
  if (heff_nopen == heff_capopen) {
    heff_capopen = heff_capopen ? heff_capopen * 2 : 16;
    heff_open = realloc(heff_open, heff_capopen * sizeof(HEFF_ONCE *));
    if (!heff_open) heff_fail("out of memory");
  }
  *st = 1;
  heff_open[heff_nopen++] = st;
  *val = heff_tr(init());
  heff_nopen--;
  *st = 2;
  return *val;
}

// (what was being computed when a failure was caught, from the `base`th
// on, to be computed again)
HEFF_FN void heff_reopen(size_t base) {
  while (heff_nopen > base) *heff_open[--heff_nopen] = 0;
}
#endif

// Integers
// ---------------------------------------------------------------------
// An integer is a word while it fits in 62 bits and a sign (`MK_INT`,
// from -2^62 up to 2^62); beyond, a big one (`K_BIG`): its sign and its
// magnitude's 64-bit digits, the least first.  Each result fitting in a
// word is one, so that two equal integers are both words or both big.

// (an integer's sign and digits, to compute with: a word's in `buf`)
typedef struct { int neg; int n; const uint64_t *d; uint64_t buf[1]; } Num;

HEFF_FN void num_of(V v, Num *x) {
  if (IS_INT(v)) {
    int64_t i = INT_OF(v);
    x->neg = i < 0;
    x->buf[0] = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
    x->n = x->buf[0] != 0;
    x->d = x->buf;
  } else {
    Big *b = (Big *)v;
    x->neg = b->neg; x->n = b->n; x->d = b->d;
  }
}

// (a big one's bytes, of `n` digits)
#define BIG_BYTES(n) (sizeof(Big) + (size_t)(n) * sizeof(uint64_t))

// (room for `n` digits, a result written into it where it is computed,
// then made an integer by `big_fin`: nothing copied)
static inline Big *big_new(int64_t n) {
  if (n > INT32_MAX) heff_fail("out of memory");
  Big *b = heff_obj_alloc(BIG_BYTES(n));
  b->h.rc = 1; b->h.kind = K_BIG; b->h.tag = 0; b->neg = 0; b->n = (int32_t)n;
  return b;
}

// (the first `n` digits of `b`'s room as an integer: a word if they
// fit, `b` given back; else `b`, its top zeros dropped -- moved to an
// object of its size where that is another of the sizes kept apart, so
// that it is given back as the size it was made)
HEFF_FN V big_fin(Big *b, int neg, int n) {
  int cap = b->n;
  while (n > 0 && b->d[n - 1] == 0) n--;
  if (n <= 1) {
    uint64_t m = n == 0 ? 0 : b->d[0];
    if (m < (uint64_t)INT_LIM || (neg && m == (uint64_t)INT_LIM)) {
      heff_obj_free(b, BIG_BYTES(cap));
      return MK_INT(neg ? (int64_t)((uint64_t)0 - m) : (int64_t)m);
    }
  }
  size_t was = (BIG_BYTES(cap) + 7) / 8, now = (BIG_BYTES(n) + 7) / 8;
  if (now != was && (now <= SMALL_MAX / 8 || was <= SMALL_MAX / 8)) {
    Big *c = big_new(n);
    memcpy(c->d, b->d, (size_t)n * sizeof(uint64_t));
    heff_obj_free(b, BIG_BYTES(cap));
    b = c;
  }
  b->neg = neg; b->n = n;
  return (V)b;
}

HEFF_SLOW V heff_int_big(int64_t i) {
  Big *b = big_new(1);
  b->d[0] = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
  return big_fin(b, i < 0, 1);
}

HEFF_FAST V heff_int(int64_t i) {
  if (HEFF_LIKELY(i >= -INT_LIM && i < INT_LIM)) return MK_INT(i);
  return heff_int_big(i);
}

static inline uint64_t heff_int_hash(V a) {
  if (IS_INT(a)) return heff_fmix64((uint64_t)INT_OF(a));
  const Big *b = (const Big *)a;
  return heff_fmix64(b->neg ? (uint64_t)0 - b->d[0] : b->d[0]);
}

HEFF_FN int heff_mag_cmp(const uint64_t *a, int an, const uint64_t *b, int bn) {
  if (an != bn) return an < bn ? -1 : 1;
  for (int i = an; i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

HEFF_FN int heff_mag_add(const uint64_t *a, int an, const uint64_t *b, int bn, uint64_t *o) {
  if (an < bn) return heff_mag_add(b, bn, a, an, o);
  uint64_t carry = 0;
  int i = 0;
  for (; i < bn; i++) {
    uint64_t s, t;
    uint64_t c1 = __builtin_add_overflow(a[i], b[i], &s);
    uint64_t c2 = __builtin_add_overflow(s, carry, &t);
    o[i] = t;
    carry = c1 | c2;
  }
  for (; i < an; i++) {
    uint64_t s;
    carry = __builtin_add_overflow(a[i], carry, &s);
    o[i] = s;
  }
  o[an] = carry;
  return an + 1;
}

HEFF_FN int heff_mag_sub(const uint64_t *a, int an, const uint64_t *b, int bn, uint64_t *o) {
  uint64_t borrow = 0;
  int i = 0;
  for (; i < bn; i++) {
    uint64_t s, t;
    uint64_t c1 = __builtin_sub_overflow(a[i], b[i], &s);
    uint64_t c2 = __builtin_sub_overflow(s, borrow, &t);
    o[i] = t;
    borrow = c1 | c2;
  }
  for (; i < an; i++) {
    uint64_t s;
    borrow = __builtin_sub_overflow(a[i], borrow, &s);
    o[i] = s;
  }
  return an;
}

HEFF_FN void heff_mag_mul(const uint64_t *a, int an, const uint64_t *b, int bn, uint64_t *o) {
  if (an < bn) { heff_mag_mul(b, bn, a, an, o); return; }
  if (bn == 0) { memset(o, 0, (size_t)an * sizeof(uint64_t)); return; }
  uint64_t carry = 0;
  for (int i = 0; i < an; i++) {
    unsigned __int128 p = (unsigned __int128)a[i] * b[0] + carry;
    o[i] = (uint64_t)p;
    carry = (uint64_t)(p >> 64);
  }
  o[an] = carry;
  for (int j = 1; j < bn; j++) {
    carry = 0;
    for (int i = 0; i < an; i++) {
      unsigned __int128 p = (unsigned __int128)a[i] * b[j] + o[i + j] + carry;
      o[i + j] = (uint64_t)p;
      carry = (uint64_t)(p >> 64);
    }
    o[j + an] = carry;
  }
}

static inline uint64_t heff_reciprocal(uint64_t v) {
  return (uint64_t)(~(unsigned __int128)0 / v);
}

static inline uint64_t heff_div21(uint64_t hi, uint64_t lo, uint64_t v, uint64_t inv, uint64_t *r) {
  unsigned __int128 p = (unsigned __int128)inv * hi + ((((unsigned __int128)(hi + 1)) << 64) | lo);
  uint64_t q = (uint64_t)(p >> 64);
  uint64_t m = lo - q * v;
  if (m > (uint64_t)p) { q -= 1; m += v; }
  if (m >= v) { q += 1; m -= v; }
  *r = m;
  return q;
}

HEFF_FN uint64_t heff_mag_div1(const uint64_t *u, int m, uint64_t d, uint64_t *q) {
  if (m == 0) return 0;
  int s = __builtin_clzll(d);
  uint64_t v = d << s, inv = heff_reciprocal(v), r = 0;
  int j = m;
  if (s == 0) {
    while (j-- > 0) q[j] = heff_div21(r, u[j], v, inv, &r);
    return r;
  }
  int back = 64 - s;
  r = u[j - 1] >> back;
  while (j-- > 0) {
    uint64_t low = (u[j] << s) | (j > 0 ? u[j - 1] >> back : 0);
    q[j] = heff_div21(r, low, v, inv, &r);
  }
  return r >> s;
}

HEFF_FN void heff_mag_divmod(const uint64_t *u, int m, const uint64_t *v, int n, uint64_t *q, uint64_t *r, uint64_t *vn, uint64_t *un) {
  if (m < n) {
    q[0] = 0;
    memcpy(r, u, (size_t)m * sizeof(uint64_t));
    memset(r + m, 0, (size_t)(n - m) * sizeof(uint64_t));
    return;
  }
  if (n == 1) { r[0] = heff_mag_div1(u, m, v[0], q); return; }
  int s = __builtin_clzll(v[n - 1]);
  if (s == 0) {
    memcpy(vn, v, (size_t)n * sizeof(uint64_t));
    memcpy(un, u, (size_t)m * sizeof(uint64_t));
    un[m] = 0;
  } else {
    int back = 64 - s;
    for (int i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (v[i - 1] >> back);
    vn[0] = v[0] << s;
    un[m] = u[m - 1] >> back;
    for (int i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (u[i - 1] >> back);
    un[0] = u[0] << s;
  }
  uint64_t top = vn[n - 1], next = vn[n - 2], inv = heff_reciprocal(top);
  for (int j = m - n + 1; j-- > 0;) {
    uint64_t top2 = un[j + n], top1 = un[j + n - 1], qhat, rhat;
    int over = 0;
    if (top2 >= top) {
      qhat = UINT64_MAX;
      over = __builtin_add_overflow(top1, top, &rhat);
    } else qhat = heff_div21(top2, top1, top, inv, &rhat);
    while (!over && (unsigned __int128)qhat * next > (((unsigned __int128)rhat << 64) | un[j + n - 2])) {
      qhat -= 1;
      over = __builtin_add_overflow(rhat, top, &rhat);
    }
    uint64_t carry = 0, borrow = 0;
    for (int l = 0; l < n; l++) {
      unsigned __int128 p = (unsigned __int128)qhat * vn[l] + carry;
      carry = (uint64_t)(p >> 64);
      uint64_t a, b;
      uint64_t c1 = __builtin_sub_overflow(un[l + j], (uint64_t)p, &a);
      uint64_t c2 = __builtin_sub_overflow(a, borrow, &b);
      un[l + j] = b;
      borrow = c1 | c2;
    }
    uint64_t a, b;
    uint64_t c1 = __builtin_sub_overflow(un[j + n], carry, &a);
    uint64_t c2 = __builtin_sub_overflow(a, borrow, &b);
    un[j + n] = b;
    q[j] = qhat;
    if (c1 | c2) {
      q[j] -= 1;
      uint64_t c = 0;
      for (int l = 0; l < n; l++) {
        uint64_t x, y;
        uint64_t d1 = __builtin_add_overflow(un[l + j], vn[l], &x);
        uint64_t d2 = __builtin_add_overflow(x, c, &y);
        un[l + j] = y;
        c = d1 | d2;
      }
      un[j + n] += c;
    }
  }
  if (s == 0) memcpy(r, un, (size_t)n * sizeof(uint64_t));
  else {
    int back = 64 - s;
    for (int l = 0; l < n; l++) r[l] = (un[l] >> s) | (un[l + 1] << back);
  }
}

// (a + b, signed)
HEFF_FN V num_add(const Num *a, int bneg, const Num *b) {
  if (a->neg == bneg) {
    Big *o = big_new((a->n > b->n ? a->n : b->n) + 1);
    return big_fin(o, a->neg, heff_mag_add(a->d, a->n, b->d, b->n, o->d));
  }
  if (heff_mag_cmp(a->d, a->n, b->d, b->n) >= 0) {
    Big *o = big_new(a->n);
    return big_fin(o, a->neg, heff_mag_sub(a->d, a->n, b->d, b->n, o->d));
  }
  Big *o = big_new(b->n);
  return big_fin(o, bneg, heff_mag_sub(b->d, b->n, a->d, a->n, o->d));
}

// (a divided by b, rounded down, as the other backends round: the
// quotient, or the remainder (b's sign) -- the one wanted written into
// the integer made, the other on the stack)
HEFF_FN V num_divmod(const Num *a, const Num *b, int want_mod) {
  int qn = a->n - b->n + 1 > 0 ? a->n - b->n + 1 : 1;
  uint64_t *vn = alloca(b->n * sizeof(uint64_t)), *un = alloca((a->n + 1) * sizeof(uint64_t));
  if (want_mod) {
    uint64_t *q = alloca(qn * sizeof(uint64_t));
    Big *r = big_new(b->n);
    heff_mag_divmod(a->d, a->n, b->d, b->n, q, r->d, vn, un);
    int rn = b->n;
    while (rn > 0 && r->d[rn - 1] == 0) rn--;
    if (!(rn > 0 && a->neg != b->neg)) return big_fin(r, a->neg, rn);
    return big_fin(r, b->neg, heff_mag_sub(b->d, b->n, r->d, rn, r->d));
  }
  uint64_t *r = alloca(b->n * sizeof(uint64_t));
  Big *q = big_new(qn + 1);
  heff_mag_divmod(a->d, a->n, b->d, b->n, q->d, r, vn, un);
  int rn = b->n;
  while (rn > 0 && r[rn - 1] == 0) rn--;
  if (!(rn > 0 && a->neg != b->neg)) return big_fin(q, a->neg != b->neg, qn);
  uint64_t one = 1;
  return big_fin(q, 1, heff_mag_add(q->d, qn, &one, 1, q->d));
}

HEFF_FN V num_mul(const Num *a, const Num *b) {
  int n = a->n + b->n;
  Big *o = big_new(n);
  heff_mag_mul(a->d, a->n, b->d, b->n, o->d);
  return big_fin(o, a->neg != b->neg, n);
}

HEFF_FN int num_cmp(const Num *a, const Num *b) {
  if (a->neg != b->neg) return a->neg ? -1 : 1;
  int c = heff_mag_cmp(a->d, a->n, b->d, b->n);
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

// (a word made of two: its low bit set, as the C compiler is told, so
// that an operation on it next tests it no more -- `(x + y) % m` tests
// `x` and `y` once)
static inline __attribute__((always_inline)) V heff_word_made(int64_t r) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_assume)
  __builtin_assume((r & 1) == 1);
#endif
#endif
  return (V)r;
}

INT_BIG(V, heff_intAdd, num_add(&x, y.neg, &y))
HEFF_FAST V heff_intAdd(V a, V b) {
  int64_t r;
  if (BOTH_INT(a, b) && !__builtin_add_overflow((int64_t)a, (int64_t)b - 1, &r)) return heff_word_made(r);
  return heff_intAdd_big(a, b);
}

// (`2x + 1 - 2y`)
INT_BIG(V, heff_intSub, num_add(&x, !y.neg && y.n > 0, &y))
HEFF_FAST V heff_intSub(V a, V b) {
  int64_t r;
  if (BOTH_INT(a, b) && !__builtin_sub_overflow((int64_t)a, (int64_t)b - 1, &r)) return heff_word_made(r);
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

// (each given also a mask of the operands given it, the others lent, as
// a comparison is: two words' operation takes nothing, and only where
// one is big is what is lent counted, for `NAME_big` to consume)
#define INT_LENT(name)                                                  \
  HEFF_FAST V name##_lent(V a, V b, int give) {                         \
    if (BOTH_INT(a, b)) return name(a, b);                              \
    return name##_big(give & 1 ? a : heff_dup(a),                       \
                      give & 2 ? b : heff_dup(b));                      \
  }
INT_LENT(heff_intAdd)
INT_LENT(heff_intSub)
INT_LENT(heff_intMul)
INT_LENT(heff_intDiv)
INT_LENT(heff_intMod)

// A comparison, as C's condition (`heff_intLt_c`, what the emitter
// tests where its Bool is only looked at) and as its Bool, each also
// given a mask of the operands given it, the others lent (`_lent`, as a
// string's comparison is).  Words are
// in the order of their integers, and equal as they are; a word and a
// big one are never equal.
#define INT_CMP(name, op, cmp)                                          \
  INT_BIG(int, name##_c, num_cmp(&x, &y) cmp)                           \
  HEFF_FAST int name##_c(V a, V b) {                                    \
    if (BOTH_INT(a, b)) return (int64_t)a op (int64_t)b;                \
    return name##_c_big(a, b);                                          \
  }                                                                     \
  HEFF_FAST V name(V a, V b) { return heff_bool(name##_c(a, b)); }     \
  HEFF_FAST int name##_c_lent(V a, V b, int give) {                     \
    if (BOTH_INT(a, b)) return (int64_t)a op (int64_t)b;                \
    return name##_c_big(give & 1 ? a : heff_dup(a),                     \
                        give & 2 ? b : heff_dup(b));                    \
  }                                                                     \
  HEFF_FAST V name##_lent(V a, V b, int give) {                         \
    return heff_bool(name##_c_lent(a, b, give));                        \
  }
INT_CMP(heff_intEq, ==, == 0)
INT_CMP(heff_intLt, <, < 0)
INT_CMP(heff_intLe, <=, <= 0)

// An integer's bits are as two's complement has them, its sign extended
// for ever (Python's): two words' operation is the words', a big one's
// done on as many 64-bit words as the longer has, and one more for the
// sign.

// (`x` as `n` words of two's complement, the least first)
HEFF_FN void num_twos(const Num *x, uint64_t *t, int n) {
  int carry = 1;
  for (int i = 0; i < n; i++) {
    uint64_t w = i < x->n ? x->d[i] : 0;
    if (x->neg) {
      t[i] = ~w + (uint64_t)carry;
      carry = carry && t[i] == 0;
    } else t[i] = w;
  }
}

// (`n` words of two's complement, written in `b`'s room, as an integer)
HEFF_FN V twos_fin(Big *b, int n) {
  uint64_t *t = b->d;
  int neg = (int)(t[n - 1] >> 63);
  if (neg) {
    int carry = 1;
    for (int i = 0; i < n; i++) {
      t[i] = ~t[i] + (uint64_t)carry;
      carry = carry && t[i] == 0;
    }
  }
  return big_fin(b, neg, n);
}

// (as the sum is: both words, inlined -- the words' operation on `2x + 1`
// and `2y + 1` is `2(x op y) + 1`'s, the low bit set again after `^` --
// else a call, on as many 64-bit words as the longer has, and one more
// for the sign)
#define INT_BITS(name, op)                                                  \
  HEFF_SLOW V name##_big(V a, V b) {                                        \
    Num x, y;                                                               \
    num_of(a, &x);                                                          \
    num_of(b, &y);                                                          \
    int n = (x.n > y.n ? x.n : y.n) + 1;                                    \
    Big *s = big_new(n);                                                    \
    uint64_t *t = alloca(n * sizeof(uint64_t));                             \
    num_twos(&x, s->d, n);                                                  \
    num_twos(&y, t, n);                                                     \
    for (int i = 0; i < n; i++) s->d[i] = s->d[i] op t[i];                  \
    V r = twos_fin(s, n);                                                   \
    heff_drop(a);                                                           \
    heff_drop(b);                                                           \
    return r;                                                               \
  }                                                                         \
  HEFF_FAST V name(V a, V b) {                                              \
    if (BOTH_INT(a, b)) return (a op b) | 1;                                \
    return name##_big(a, b);                                                \
  }
INT_BITS(heff_intAnd, &)
INT_BITS(heff_intOr, |)
INT_BITS(heff_intXor, ^)

HEFF_FN int heff_int_neg(V v) { return IS_INT(v) ? INT_OF(v) < 0 : ((Big *)v)->neg; }

HEFF_SLOW V heff_intShr_big(V a, V b);

// (`a` times 2^`b`; by a negative count, a shift right by its size; one
// beyond memory fails)
HEFF_SLOW V heff_intShl_big(V a, V b) {
  if (heff_int_neg(b)) return heff_intShr_big(a, heff_intSub(MK_INT(0), b));
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
  int64_t w = k / 64, s = k % 64, n = x.n + w + 1;
  Big *ob = big_new(n);
  uint64_t *o = ob->d;
  memset(o, 0, w * sizeof(uint64_t));
  o[x.n + w] = 0;
  for (int i = 0; i < x.n; i++) o[i + w] = s ? x.d[i] << s | (i ? x.d[i - 1] >> (64 - s) : 0) : x.d[i];
  if (s) o[x.n + w] = x.d[x.n - 1] >> (64 - s);
  V r = big_fin(ob, x.neg, (int)n);
  heff_drop(a);
  return r;
}

// (a word by a count from 0 below 62 to a word, inlined: `x << k` is one
// where shifting it back gives `x`, and `2x + 1`'s bits shifted so, the
// low bit set again, are its; else the call)
HEFF_FAST V heff_intShl(V a, V b) {
  if (BOTH_INT(a, b) && b < MK_INT(62)) {
    int64_t k = INT_OF(b), x = (int64_t)a - 1, r = (int64_t)((uint64_t)x << k);
    if ((r >> k) == x) return (V)r | 1;
  }
  return heff_intShl_big(a, b);
}

// (`a` divided by 2^`b`, rounding down: its bits shifted out, the sign
// shifted in; by a negative count, a shift left by its size)
HEFF_SLOW V heff_intShr_big(V a, V b) {
  if (heff_int_neg(b)) return heff_intShl_big(a, heff_intSub(MK_INT(0), b));
  int bbig = !IS_INT(b);
  heff_drop(b);
  int aneg = heff_int_neg(a);
  if (bbig) { heff_drop(a); return MK_INT(aneg ? -1 : 0); }
  int64_t k = INT_OF(b);
  if (IS_INT(a)) return MK_INT(k >= 63 ? (aneg ? -1 : 0) : INT_OF(a) >> k);
  Num x;
  num_of(a, &x);
  int64_t w = k / 64, s = k % 64;
  if (w >= x.n) { heff_drop(a); return MK_INT(aneg ? -1 : 0); }
  int n = x.n - (int)w;
  Big *ob = big_new(n + 1);
  uint64_t *o = ob->d;
  for (int i = 0; i < n; i++) {
    uint64_t hi = i + w + 1 < x.n ? x.d[i + w + 1] : 0;
    o[i] = s ? x.d[i + w] >> s | hi << (64 - s) : x.d[i + w];
  }
  o[n] = 0;
  // (a negative one rounds down: its magnitude up, where a bit set was
  // shifted out)
  int lost = s && (x.d[w] & (((uint64_t)1 << s) - 1));
  for (int64_t i = 0; i < w && !lost; i++) lost = x.d[i] != 0;
  if (x.neg && lost)
    for (int i = 0; i <= n && ++o[i] == 0; i++) {}
  V r = big_fin(ob, x.neg, n + 1);
  heff_drop(a);
  return r;
}

// (a word by a count from 0 to a word, inlined: its bits shifted out, the
// sign shifted in -- by 63 or more, all of them; `2x + 1`'s, the low bit
// set again; else the call)
HEFF_FAST V heff_intShr(V a, V b) {
  if (BOTH_INT(a, b) && (int64_t)b >= 0) {
    int64_t k = INT_OF(b);
    return (V)((int64_t)a >> (k < 63 ? k : 63)) | 1;
  }
  return heff_intShr_big(a, b);
}

// (a word's decimal digits, a '-' before a negative one, written
// backwards to `end`: where they start, 20 bytes at most before it)
static inline char *heff_word_digits(int64_t k, char *end) {
  uint64_t u = k < 0 ? (uint64_t)0 - (uint64_t)k : (uint64_t)k;
  do { *--end = (char)('0' + u % 10); u /= 10; } while (u);
  if (k < 0) *--end = '-';
  return end;
}

// (an integer's decimal digits, a '-' before a negative one: malloc'd.
// Its magnitude divided by 10^19 again and again, in place, each
// remainder 19 of them -- a 64-bit digit has 20 at most)
HEFF_FN char *heff_int_cstr(V v) {
  Num x;
  num_of(v, &x);
  uint64_t *m = alloca((x.n + 1) * sizeof(uint64_t));
  memcpy(m, x.d, x.n * sizeof(uint64_t));
  int n = x.n;
  size_t cap = (size_t)n * 20 + 3, at = cap;
  char *c = heff_alloc(cap);
  c[--at] = 0;
  do {
    uint64_t k = heff_mag_div1(m, n, 10000000000000000000u, m);
    while (n > 0 && m[n - 1] == 0) n--;
    for (int i = 0; i < 19 && (n > 0 || k > 0 || i == 0); i++) {
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
  int n = 0;
  Big *b = big_new((int64_t)(len / 19) + 2);
  uint64_t *d = b->d;
  for (size_t i = 0; i < len;) {
    uint64_t chunk = 0, scale = 1;
    for (int k = 0; k < 19 && i < len; k++, i++) { chunk = chunk * 10 + (uint64_t)(s[i] - '0'); scale *= 10; }
    uint64_t c = chunk;
    for (int j = 0; j < n; j++) {
      unsigned __int128 t = (unsigned __int128)d[j] * scale + c;
      d[j] = (uint64_t)t;
      c = (uint64_t)(t >> 64);
    }
    if (c) d[n++] = c;
  }
  V v = big_fin(b, neg, n);
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

// (a float's integer part, beyond a word if it must be: a word's
// inlined, a big one's the call)
HEFF_SLOW V heff_int_of_double_big(double f) {
  int e;
  double fr = frexp(fabs(f), &e);
  uint64_t mant = (uint64_t)ldexp(fr, 53);
  int sh = e - 53, w = sh / 64, b = sh % 64;
  Big *o = big_new(w + 2);
  memset(o->d, 0, w * sizeof(uint64_t));
  unsigned __int128 x = (unsigned __int128)mant << b;
  o->d[w] = (uint64_t)x;
  o->d[w + 1] = (uint64_t)(x >> 64);
  return big_fin(o, f < 0, w + 2);
}

HEFF_FAST V heff_int_of_double(double f) {
  if (HEFF_LIKELY(f < (double)INT_LIM && f >= -(double)INT_LIM)) return MK_INT((int64_t)f);
  return heff_int_of_double_big(f);
}

// (an integer as the nearest float: a word's inlined, a big one's by its
// digits, the call)
HEFF_SLOW double heff_double_of_big(V v) {
  char *c = heff_int_cstr(v);
  double d = strtod(c, NULL);
  free(c);
  return d;
}

HEFF_FAST double heff_double_of_int(V v) {
  if (HEFF_LIKELY(IS_INT(v))) return (double)INT_OF(v);
  return heff_double_of_big(v);
}

// Strings
// ---------------------------------------------------------------------

HEFF_FN Str *heff_str_new(int64_t len) {
  Str *s = heff_obj_alloc(sizeof(Str) + len);
  s->h.rc = 1; s->h.kind = K_STR; s->h.tag = 0; s->len = len; s->cap = (len + 7) & ~(int64_t)7;
  s->p = s->s;
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

// A string's room.  One made has room for its bytes, and no more than
// the allocator's rounding to 8 gives it: what is made once and kept --
// a map's key, a field -- takes what it needs.  Only a string grown in
// place, its last reference appended to, is given more, and only where
// that is cheap: while it is a small object (`SMALL_MAX`), it grows to
// what it needs, rounded so, each time a copy of at most so many bytes;
// beyond, to twice its room (or what it needs, if more), by `realloc`,
// which extends it where it is where it can -- so a string built a piece
// at a time is copied a logarithmic number of times, not once a piece,
// and the room not yet written in a large block is the system's pages
// not yet touched, not memory.

// (the string `x`, its last reference and not a view, room for `n` bytes
// made in it: the string, moved where it must be)
HEFF_FN Str *heff_str_grow(Str *x, int64_t n) {
  size_t was = (sizeof(Str) + x->cap + 7) / 8 * 8;
  size_t room = (sizeof(Str) + n + 7) / 8 * 8;
  Str *g;
  if (room > SMALL_MAX) {
    size_t twice = (sizeof(Str) + 2 * x->cap + 7) / 8 * 8;
    if (twice > room) room = twice;
  }
  if (was > SMALL_MAX) {
    // (from `malloc`, the allocator's own beyond `SMALL_MAX`)
    HEFF_COUNTED(room / 8);
    g = realloc(x, room);
    if (!g) heff_fail("out of memory");
  } else {
    // (from the allocator objects are given back to, rounded as it
    // rounds: a block it frees to a free list must be the whole of
    // that list's size)
    g = heff_obj_alloc(room);
    memcpy(g, x, sizeof(Str) + x->len);
    heff_obj_free(x, was);
  }
  g->cap = (int64_t)(room - sizeof(Str));
  g->p = g->s;
  return g;
}

// (`n` bytes, at most 64, from `q` to `p`, apart, by no call: a string
// grown in place grows by a byte or a few, most often -- a byte of
// `fromByte`, a digit, a rune, a copy back in an inflated stream -- and
// a call to `memcpy` for each was much of what that cost.  Below 8, a
// byte at a time, as cases, which a compiler does not turn back into the
// call as it would a loop; from 8, two copies of a fixed size, the second
// ending where the bytes end, overlapping the first: a fixed size is
// copied by loads and stores where it is)
HEFF_FAST void heff_copy_short(uint8_t *p, const uint8_t *q, int64_t n) {
  if (n < 8) {
    switch (n) {
    case 7: p[6] = q[6]; __attribute__((fallthrough));
    case 6: p[5] = q[5]; __attribute__((fallthrough));
    case 5: p[4] = q[4]; __attribute__((fallthrough));
    case 4: p[3] = q[3]; __attribute__((fallthrough));
    case 3: p[2] = q[2]; __attribute__((fallthrough));
    case 2: p[1] = q[1]; __attribute__((fallthrough));
    case 1: p[0] = q[0]; __attribute__((fallthrough));
    default: break;
    }
  } else if (n < 16) {
    memcpy(p, q, 8);
    memcpy(p + n - 8, q + n - 8, 8);
  } else if (n < 32) {
    memcpy(p, q, 16);
    memcpy(p + n - 16, q + n - 16, 16);
  } else {
    memcpy(p, q, 32);
    memcpy(p + n - 32, q + n - 32, 32);
  }
}

// (the first string, its last reference and not a view, grown in place,
// as above; `Bytes` too, a new one of the kind `kind`)
HEFF_FN V heff_buf_append(V a, V b, uint16_t kind) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t n = x->len + y->len;
  // (a view has no room: its `len` is more than its `cap`, so that where
  // there is room it is no view, and that is not looked at)
  if (heff_alone(&x->h) && (n <= x->cap || !HEFF_STR_VIEW(x))) {
    if (n > x->cap) {
      if (x->len == 0 && y->h.kind == kind) { heff_drop(a); return b; }
      x = heff_str_grow(x, n);
    }
    if (y->len <= 64) heff_copy_short(x->s + x->len, y->p, y->len);
    else memcpy(x->s + x->len, y->p, (size_t)y->len);
    x->len = n;
    heff_drop(b);
    return (V)x;
  }
  // (to nothing, or nothing to it, where a string would be made: the one
  // that is something, as it is, where it is of the kind wanted -- a
  // view is a string's)
  if (y->len == 0 && x->h.kind == kind) { heff_drop(b); return a; }
  if (x->len == 0 && y->h.kind == kind) { heff_drop(a); return b; }
  Str *s = heff_str_new(n);
  s->h.kind = kind;
  memcpy(s->s, x->p, (size_t)x->len);
  memcpy(s->s + x->len, y->p, (size_t)y->len);
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

// Slices.  A slice of a string is a view of it, taking no copy, where it
// is long -- `HEFF_VIEW_MIN` bytes or more -- and at least a
// `HEFF_VIEW_SHARE`-th of its root: a view of a view is one of the root,
// so that a string whose tail is taken again and again is a view each
// time, until it is a short part of its root, then a copy, a root of its
// own.  A shorter slice is copied: it costs no more than a view, and
// holds no parent; and a view holds its root, so the bytes kept for it
// are at most `HEFF_VIEW_SHARE` times its own.  `Bytes` are sliced so
// too: a view is of any kind
#ifndef HEFF_VIEW_MIN
#define HEFF_VIEW_MIN 64
#endif
#ifndef HEFF_VIEW_SHARE
#define HEFF_VIEW_SHARE 4
#endif
_Static_assert(HEFF_VIEW_MIN > sizeof(V), "a view's length is more than its room");

// (`m` bytes of `x` from `b`, a view if they are enough of its root)
HEFF_FAST Str *heff_str_slice_long(Str *x, int64_t b, int64_t m) {
  Str *r = HEFF_STR_VIEW(x) ? (Str *)*HEFF_STR_ROOT(x) : x;
  if (m * HEFF_VIEW_SHARE >= r->len) {
    Str *w = heff_obj_alloc(sizeof(Str) + sizeof(V));
    w->h.rc = 1; w->h.kind = K_STR; w->h.tag = 0; w->len = m; w->cap = sizeof(V);
    w->p = x->p + b;
    heff_dup((V)r);
    *HEFF_STR_ROOT(w) = (V)r;
    return w;
  }
  Str *s = heff_str_new(m);
  memcpy(s->s, x->p + b, (size_t)m);
  return s;
}

// (as JavaScript's `slice`: out of range is clamped, negative from the
// end; a short one copied here, a long one by `heff_str_slice_long`)
HEFF_FN V heff_strSlice_lent(V a, V i, V j, int give) {
  Str *x = (Str *)a;
  int64_t n = x->len, b = INT_OF(i), e = INT_OF(j);
  if (b < 0) b = b + n < 0 ? 0 : b + n;
  if (e < 0) e = e + n < 0 ? 0 : e + n;
  if (b > n) b = n;
  if (e > n) e = n;
  if (e < b) e = b;
  if (!HEFF_LIKELY(e - b < HEFF_VIEW_MIN)) {
    Str *s = heff_str_slice_long(x, b, e - b);
    HEFF_GIVEN(give, 0, a);
    return (V)s;
  }
  Str *s = heff_str_new(e - b);
  memcpy(s->s, x->p + b, (size_t)(e - b));
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
    uint64_t u = heff_str_word(x->p + i), v = heff_str_word(y->p + i);
    if (u != v) return u < v ? -1 : 1;
  }
  for (; i < n; i++)
    if (x->p[i] != y->p[i]) return x->p[i] < y->p[i] ? -1 : 1;
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
      const uint8_t *at = x->p + k, *end = x->p + x->len - m + 1;
      while (at < end && (at = memchr(at, p->p[0], (size_t)(end - at)))) {
        if (memcmp(at + 1, p->p + 1, (size_t)(m - 1)) == 0) { r = at - x->p; break; }
        at++;
      }
    }
  }
  HEFF_GIVEN(give, 0, needle);
  HEFF_GIVEN(give, 1, s);
  return MK_INT(r);
}
HEFF_FN V heff_strFind(V needle, V s, V i) { return heff_strFind_lent(needle, s, i, 3); }

// Hashes: a string's is FxHash's step over its bytes 8 at a time,
// little-endian, the last word filled with zeros, from its length, then
// murmur3's finish; an integer's that finish of its low 64 bits -- the
// same numbers as JavaScript's and the interpreter's, so that a map's
// order is one everywhere -- each shifted right by 2, a word's integer,
// never negative.
HEFF_FAST V heff_strHash_lent(V a, int give) {
  Str *x = (Str *)a;
  V r = MK_INT((int64_t)(heff_hash_bytes(x->p, x->len) >> 2));
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_strHash(V a) { return heff_strHash_lent(a, 1); }

// (the bytes of `x` from `i` up to `j`, clamped as `strSlice` clamps
// them: where they start, and how many, read in place, not copied)
static inline int64_t heff_str_range(Str *x, V i, V j, int64_t *len) {
  int64_t n = x->len, b = INT_OF(i), e = INT_OF(j);
  if (b < 0) b = b + n < 0 ? 0 : b + n;
  if (e < 0) e = e + n < 0 ? 0 : e + n;
  if (b > n) b = n;
  if (e > n) e = n;
  if (e < b) e = b;
  *len = e - b;
  return b;
}

// (the hash of `strSlice a i j`, no slice made)
HEFF_FAST V heff_strHashRange_lent(V a, V i, V j, int give) {
  Str *x = (Str *)a;
  int64_t m, b = heff_str_range(x, i, j, &m);
  V r = MK_INT((int64_t)(heff_hash_bytes(x->p + b, m) >> 2));
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_strHashRange(V a, V i, V j) { return heff_strHashRange_lent(a, i, j, 1); }

// (whether `t` is `strSlice s i j`, no slice made)
HEFF_FAST V heff_strEqRange_lent(V t, V s, V i, V j, int give) {
  Str *x = (Str *)t, *y = (Str *)s;
  int64_t m, b = heff_str_range(y, i, j, &m);
  int c = x->len == m && memcmp(x->p, y->p + b, (size_t)m) == 0;
  HEFF_GIVEN(give, 0, t);
  HEFF_GIVEN(give, 1, s);
  return heff_bool(c);
}
HEFF_FN V heff_strEqRange(V t, V s, V i, V j) { return heff_strEqRange_lent(t, s, i, j, 3); }

HEFF_SLOW V heff_intHash_big(V a) {
  uint64_t h = heff_int_hash(a);
  heff_drop(a);
  return MK_INT((int64_t)(h >> 2));
}

HEFF_FAST V heff_intHash(V a) {
  if (HEFF_LIKELY(IS_INT(a))) return MK_INT((int64_t)(heff_fmix64((uint64_t)INT_OF(a)) >> 2));
  return heff_intHash_big(a);
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
  int c = x == y || (x->len == y->len && memcmp(x->p, y->p, (size_t)x->len) == 0);
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
  V r = k >= 0 && k < x->len ? MK_INT(x->p[k]) : MK_INT(-1);
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_strByteAt(V a, V i) { return heff_strByteAt_lent(a, i, 1); }

// (the 256 strings of one byte, made by the C compiler, immortal: a
// string's room, as a static one's is, in the program's first file)
typedef struct { Obj h; int64_t len; int64_t cap; uint8_t *p; uint8_t s[8]; } HeffByteStr;
#ifdef HEFF_PART
extern HeffByteStr heff_byte_strs[256];
#else
#define HEFF_BYTE_STR(b) {{IMMORTAL, K_STR, 0}, 1, 1, heff_byte_strs[b].s, {b}}
#define HEFF_BYTE_STR4(b) HEFF_BYTE_STR(b), HEFF_BYTE_STR(b + 1), HEFF_BYTE_STR(b + 2), HEFF_BYTE_STR(b + 3)
#define HEFF_BYTE_STR16(b) HEFF_BYTE_STR4(b), HEFF_BYTE_STR4(b + 4), HEFF_BYTE_STR4(b + 8), HEFF_BYTE_STR4(b + 12)
#define HEFF_BYTE_STR64(b) HEFF_BYTE_STR16(b), HEFF_BYTE_STR16(b + 16), HEFF_BYTE_STR16(b + 32), HEFF_BYTE_STR16(b + 48)
HeffByteStr heff_byte_strs[256] = {
  HEFF_BYTE_STR64(0), HEFF_BYTE_STR64(64), HEFF_BYTE_STR64(128), HEFF_BYTE_STR64(192)
};
#endif

// (the string of one byte, a `UInt8`: a word of its value, 0 to 255 --
// one of the 256, nothing made)
HEFF_FN V heff_strFromByte(V c) { return (V)&heff_byte_strs[(uint8_t)INT_OF(c)]; }

// (a word's digits on the stack, nothing allocated but the string; a
// big one's by its digits')
HEFF_FN V heff_intShow(V a) {
  if (HEFF_LIKELY(IS_INT(a))) {
    char b[24], *p = heff_word_digits(INT_OF(a), b + sizeof b);
    return heff_str_of(p, b + sizeof b - p);
  }
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
// (the target the program is built for)
HEFF_FN V heff_onWindows(V u) {
  heff_drop(u);
#ifdef _WIN32
  return heff_bool(1);
#else
  return heff_bool(0);
#endif
}

// (std.math's: libm's, as JavaScript's `Math` has them -- but sin, cos,
// tan, exp and log, the runtime's own, bundled after it)
#define FLT1(name, f) \
  HEFF_FN V name(V a) { double x = FLT_OF(a); heff_drop(a); return heff_flt(f(x)); }
FLT1(heff_floatAsin, asin)
FLT1(heff_floatAcos, acos)
FLT1(heff_floatAtan, atan)
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

static const uint64_t heff_pow5[326][2] = {
  {0x0000000000000000u, 0x1000000000000000u}, {0x0000000000000000u, 0x1400000000000000u},
  {0x0000000000000000u, 0x1900000000000000u}, {0x0000000000000000u, 0x1f40000000000000u},
  {0x0000000000000000u, 0x1388000000000000u}, {0x0000000000000000u, 0x186a000000000000u},
  {0x0000000000000000u, 0x1e84800000000000u}, {0x0000000000000000u, 0x1312d00000000000u},
  {0x0000000000000000u, 0x17d7840000000000u}, {0x0000000000000000u, 0x1dcd650000000000u},
  {0x0000000000000000u, 0x12a05f2000000000u}, {0x0000000000000000u, 0x174876e800000000u},
  {0x0000000000000000u, 0x1d1a94a200000000u}, {0x0000000000000000u, 0x12309ce540000000u},
  {0x0000000000000000u, 0x16bcc41e90000000u}, {0x0000000000000000u, 0x1c6bf52634000000u},
  {0x0000000000000000u, 0x11c37937e0800000u}, {0x0000000000000000u, 0x16345785d8a00000u},
  {0x0000000000000000u, 0x1bc16d674ec80000u}, {0x0000000000000000u, 0x1158e460913d0000u},
  {0x0000000000000000u, 0x15af1d78b58c4000u}, {0x0000000000000000u, 0x1b1ae4d6e2ef5000u},
  {0x0000000000000000u, 0x10f0cf064dd59200u}, {0x0000000000000000u, 0x152d02c7e14af680u},
  {0x0000000000000000u, 0x1a784379d99db420u}, {0x0000000000000000u, 0x108b2a2c28029094u},
  {0x0000000000000000u, 0x14adf4b7320334b9u}, {0x4000000000000000u, 0x19d971e4fe8401e7u},
  {0x8800000000000000u, 0x1027e72f1f128130u}, {0xaa00000000000000u, 0x1431e0fae6d7217cu},
  {0xd480000000000000u, 0x193e5939a08ce9dbu}, {0xc9a0000000000000u, 0x1f8def8808b02452u},
  {0xbe04000000000000u, 0x13b8b5b5056e16b3u}, {0xad85000000000000u, 0x18a6e32246c99c60u},
  {0xd8e6400000000000u, 0x1ed09bead87c0378u}, {0x878fe80000000000u, 0x13426172c74d822bu},
  {0x6973e20000000000u, 0x1812f9cf7920e2b6u}, {0x03d0da8000000000u, 0x1e17b84357691b64u},
  {0x8262889000000000u, 0x12ced32a16a1b11eu}, {0x22fb2ab400000000u, 0x178287f49c4a1d66u},
  {0xabb9f56100000000u, 0x1d6329f1c35ca4bfu}, {0xcb54395ca0000000u, 0x125dfa371a19e6f7u},
  {0xbe2947b3c8000000u, 0x16f578c4e0a060b5u}, {0x2db399a0ba000000u, 0x1cb2d6f618c878e3u},
  {0xfc90400474400000u, 0x11efc659cf7d4b8du}, {0x7bb4500591500000u, 0x166bb7f0435c9e71u},
  {0xdaa16406f5a40000u, 0x1c06a5ec5433c60du}, {0xa8a4de8459868000u, 0x118427b3b4a05bc8u},
  {0xd2ce16256fe82000u, 0x15e531a0a1c872bau}, {0x87819baecbe22800u, 0x1b5e7e08ca3a8f69u},
  {0xf4b1014d3f6d5900u, 0x111b0ec57e6499a1u}, {0x71dd41a08f48af40u, 0x1561d276ddfdc00au},
  {0x0e549208b31adb10u, 0x1aba4714957d300du}, {0x28f4db456ff0c8eau, 0x10b46c6cdd6e3e08u},
  {0x33321216cbecfb24u, 0x14e1878814c9cd8au}, {0xbffe969c7ee839edu, 0x1a19e96a19fc40ecu},
  {0xf7ff1e21cf512434u, 0x105031e2503da893u}, {0xf5fee5aa43256d41u, 0x14643e5ae44d12b8u},
  {0x337e9f14d3eec892u, 0x197d4df19d605767u}, {0x005e46da08ea7ab6u, 0x1fdca16e04b86d41u},
  {0xa03aec4845928cb2u, 0x13e9e4e4c2f34448u}, {0xc849a75a56f72fdeu, 0x18e45e1df3b0155au},
  {0x7a5c1130ecb4fbd6u, 0x1f1d75a5709c1ab1u}, {0xec798abe93f11d65u, 0x13726987666190aeu},
  {0xa797ed6e38ed64bfu, 0x184f03e93ff9f4dau}, {0x517de8c9c728bdefu, 0x1e62c4e38ff87211u},
  {0xd2eeb17e1c7976b5u, 0x12fdbb0e39fb474au}, {0x87aa5ddda397d462u, 0x17bd29d1c87a191du},
  {0xe994f5550c7dc97bu, 0x1dac74463a989f64u}, {0x11fd195527ce9dedu, 0x128bc8abe49f639fu},
  {0xd67c5faa71c24568u, 0x172ebad6ddc73c86u}, {0x8c1b77950e32d6c2u, 0x1cfa698c95390ba8u},
  {0x57912abd28dfc639u, 0x121c81f7dd43a749u}, {0xad75756c7317b7c8u, 0x16a3a275d494911bu},
  {0x98d2d2c78fdda5bau, 0x1c4c8b1349b9b562u}, {0x9f83c3bcb9ea8794u, 0x11afd6ec0e14115du},
  {0x0764b4abe8652979u, 0x161bcca7119915b5u}, {0x493de1d6e27e73d7u, 0x1ba2bfd0d5ff5b22u},
  {0x6dc6ad264d8f0866u, 0x1145b7e285bf98f5u}, {0xc938586fe0f2ca80u, 0x159725db272f7f32u},
  {0x7b866e8bd92f7d20u, 0x1afcef51f0fb5effu}, {0xad34051767bdae34u, 0x10de1593369d1b5fu},
  {0x9881065d41ad19c1u, 0x15159af804446237u}, {0x7ea147f492186032u, 0x1a5b01b605557ac5u},
  {0x6f24ccf8db4f3c1fu, 0x1078e111c3556cbbu}, {0x4aee003712230b27u, 0x14971956342ac7eau},
  {0xdda98044d6abcdf0u, 0x19bcdfabc13579e4u}, {0x0a89f02b062b60b6u, 0x10160bcb58c16c2fu},
  {0xcd2c6c35c7b638e4u, 0x141b8ebe2ef1c73au}, {0x8077874339a3c71du, 0x1922726dbaae3909u},
  {0xe0956914080cb8e4u, 0x1f6b0f092959c74bu}, {0x6c5d61ac8507f38eu, 0x13a2e965b9d81c8fu},
  {0x4774ba17a649f072u, 0x188ba3bf284e23b3u}, {0x1951e89d8fdc6c8fu, 0x1eae8caef261aca0u},
  {0x0fd3316279e9c3d9u, 0x132d17ed577d0be4u}, {0x13c7fdbb186434cfu, 0x17f85de8ad5c4eddu},
  {0x58b9fd29de7d4203u, 0x1df67562d8b36294u}, {0xb7743e3a2b0e4942u, 0x12ba095dc7701d9cu},
  {0xe5514dc8b5d1db92u, 0x17688bb5394c2503u}, {0xdea5a13ae3465277u, 0x1d42aea2879f2e44u},
  {0x0b2784c4ce0bf38au, 0x1249ad2594c37cebu}, {0xcdf165f6018ef06du, 0x16dc186ef9f45c25u},
  {0x416dbf7381f2ac88u, 0x1c931e8ab871732fu}, {0x88e497a83137abd5u, 0x11dbf316b346e7fdu},
  {0xeb1dbd923d8596cau, 0x1652efdc6018a1fcu}, {0x25e52cf6cce6fc7du, 0x1be7abd3781eca7cu},
  {0x97af3c1a40105dceu, 0x1170cb642b133e8du}, {0xfd9b0b20d0147542u, 0x15ccfe3d35d80e30u},
  {0x3d01cde904199292u, 0x1b403dcc834e11bdu}, {0x462120b1a28ffb9bu, 0x1108269fd210cb16u},
  {0xd7a968de0b33fa82u, 0x154a3047c694fddbu}, {0xcd93c3158e00f923u, 0x1a9cbc59b83a3d52u},
  {0xc07c59ed78c09bb6u, 0x10a1f5b813246653u}, {0xb09b7068d6f0c2a3u, 0x14ca732617ed7fe8u},
  {0xdcc24c830cacf34cu, 0x19fd0fef9de8dfe2u}, {0xc9f96fd1e7ec180fu, 0x103e29f5c2b18bedu},
  {0x3c77cbc661e71e13u, 0x144db473335deee9u}, {0x8b95beb7fa60e598u, 0x1961219000356aa3u},
  {0x6e7b2e65f8f91efeu, 0x1fb969f40042c54cu}, {0xc50cfcffbb9bb35fu, 0x13d3e2388029bb4fu},
  {0xb6503c3faa82a037u, 0x18c8dac6a0342a23u}, {0xa3e44b4f95234844u, 0x1efb1178484134acu},
  {0xe66eaf11bd360d2bu, 0x135ceaeb2d28c0ebu}, {0xe00a5ad62c839075u, 0x183425a5f872f126u},
  {0x980cf18bb7a47493u, 0x1e412f0f768fad70u}, {0x5f0816f752c6c8dcu, 0x12e8bd69aa19cc66u},
  {0xf6ca1cb527787b13u, 0x17a2ecc414a03f7fu}, {0xf47ca3e2715699d7u, 0x1d8ba7f519c84f5fu},
  {0xf8cde66d86d62026u, 0x127748f9301d319bu}, {0xf7016008e88ba830u, 0x17151b377c247e02u},
  {0xb4c1b80b22ae923cu, 0x1cda62055b2d9d83u}, {0x50f91306f5ad1b65u, 0x12087d4358fc8272u},
  {0xe53757c8b318623fu, 0x168a9c942f3ba30eu}, {0x9e852dbadfde7acfu, 0x1c2d43b93b0a8bd2u},
  {0xa3133c94cbeb0cc1u, 0x119c4a53c4e69763u}, {0x8bd80bb9fee5cff1u, 0x16035ce8b6203d3cu},
  {0xaece0ea87e9f43eeu, 0x1b843422e3a84c8bu}, {0x4d40c9294f238a75u, 0x1132a095ce492fd7u},
  {0x2090fb73a2ec6d12u, 0x157f48bb41db7bcdu}, {0x68b53a508ba78856u, 0x1adf1aea12525ac0u},
  {0x417144725748b536u, 0x10cb70d24b7378b8u}, {0x51cd958eed1ae283u, 0x14fe4d06de5056e6u},
  {0xe640faf2a8619b24u, 0x1a3de04895e46c9fu}, {0xefe89cd7a93d00f7u, 0x1066ac2d5daec3e3u},
  {0xebe2c40d938c4134u, 0x14805738b51a74dcu}, {0x26db7510f86f5181u, 0x19a06d06e2611214u},
  {0x9849292a9b4592f1u, 0x100444244d7cab4cu}, {0xbe5b73754216f7adu, 0x1405552d60dbd61fu},
  {0xadf25052929cb598u, 0x1906aa78b912cba7u}, {0x996ee4673743e2ffu, 0x1f485516e7577e91u},
  {0xffe54ec0828a6ddfu, 0x138d352e5096af1au}, {0xbfdea270a32d0957u, 0x18708279e4bc5ae1u},
  {0x2fd64b0ccbf84badu, 0x1e8ca3185deb719au}, {0x5de5eee7ff7b2f4cu, 0x1317e5ef3ab32700u},
  {0x755f6aa1ff59fb1fu, 0x17dddf6b095ff0c0u}, {0x92b7454a7f3079e7u, 0x1dd55745cbb7ecf0u},
  {0x5bb28b4e8f7e4c30u, 0x12a5568b9f52f416u}, {0xf29f2e22335ddf3cu, 0x174eac2e8727b11bu},
  {0xef46f9aac035570bu, 0x1d22573a28f19d62u}, {0xd58c5c0ab8215667u, 0x123576845997025du},
  {0x4aef730d6629ac01u, 0x16c2d4256ffcc2f5u}, {0x9dab4fd0bfb41701u, 0x1c73892ecbfbf3b2u},
  {0xa28b11e277d08e60u, 0x11c835bd3f7d784fu}, {0x8b2dd65b15c4b1f9u, 0x163a432c8f5cd663u},
  {0x6df94bf1db35de77u, 0x1bc8d3f7b3340bfcu}, {0xc4bbcf772901ab0au, 0x115d847ad000877du},
  {0x35eac354f34215cdu, 0x15b4e5998400a95du}, {0x8365742a30129b40u, 0x1b221effe500d3b4u},
  {0xd21f689a5e0ba108u, 0x10f5535fef208450u}, {0x06a742c0f58e894au, 0x1532a837eae8a565u},
  {0x4851137132f22b9du, 0x1a7f5245e5a2cebeu}, {0xed32ac26bfd75b42u, 0x108f936baf85c136u},
  {0xa87f57306fcd3212u, 0x14b378469b673184u}, {0xd29f2cfc8bc07e97u, 0x19e056584240fde5u},
  {0xa3a37c1dd7584f1eu, 0x102c35f729689eafu}, {0x8c8c5b254d2e62e6u, 0x14374374f3c2c65bu},
  {0x6faf71eea079fb9fu, 0x1945145230b377f2u}, {0x0b9b4e6a48987a87u, 0x1f965966bce055efu},
  {0x674111026d5f4c94u, 0x13bdf7e0360c35b5u}, {0xc111554308b71fbau, 0x18ad75d8438f4322u},
  {0x7155aa93cae4e7a8u, 0x1ed8d34e547313ebu}, {0x26d58a9c5ecf10c9u, 0x13478410f4c7ec73u},
  {0xf08aed437682d4fbu, 0x1819651531f9e78fu}, {0xecada89454238a3au, 0x1e1fbe5a7e786173u},
  {0x73ec895cb4963664u, 0x12d3d6f88f0b3ce8u}, {0x90e7abb3e1bbc3fdu, 0x1788ccb6b2ce0c22u},
  {0x352196a0da2ab4fdu, 0x1d6affe45f818f2bu}, {0x0134fe24885ab11eu, 0x1262dfeebbb0f97bu},
  {0xc1823dadaa715d65u, 0x16fb97ea6a9d37d9u}, {0x31e2cd19150db4bfu, 0x1cba7de5054485d0u},
  {0x1f2dc02fad2890f7u, 0x11f48eaf234ad3a2u}, {0xa6f9303b9872b535u, 0x1671b25aec1d888au},
  {0x50b77c4a7e8f6282u, 0x1c0e1ef1a724eaadu}, {0x5272adae8f199d91u, 0x1188d357087712acu},
  {0x670f591a32e004f6u, 0x15eb082cca94d757u}, {0x40d32f60bf980633u, 0x1b65ca37fd3a0d2du},
  {0x4883fd9c77bf03e0u, 0x111f9e62fe44483cu}, {0x5aa4fd0395aec4d8u, 0x156785fbbdd55a4bu},
  {0x314e3c447b1a760eu, 0x1ac1677aad4ab0deu}, {0xded0e5aaccf089c9u, 0x10b8e0acac4eae8au},
  {0x96851f15802cac3bu, 0x14e718d7d7625a2du}, {0xfc2666dae037d74au, 0x1a20df0dcd3af0b8u},
  {0x9d980048cc22e68eu, 0x10548b68a044d673u}, {0x84fe005aff2ba032u, 0x1469ae42c8560c10u},
  {0xa63d8071bef6883eu, 0x198419d37a6b8f14u}, {0xcfcce08e2eb42a4eu, 0x1fe52048590672d9u},
  {0x21e00c58dd309a70u, 0x13ef342d37a407c8u}, {0x2a580f6f147cc10du, 0x18eb0138858d09bau},
  {0xb4ee134ad99bf150u, 0x1f25c186a6f04c28u}, {0x7114cc0ec80176d2u, 0x137798f428562f99u},
  {0xcd59ff127a01d486u, 0x18557f31326bbb7fu}, {0xc0b07ed7188249a8u, 0x1e6adefd7f06aa5fu},
  {0xd86e4f466f516e09u, 0x1302cb5e6f642a7bu}, {0xce89e3180b25c98bu, 0x17c37e360b3d351au},
  {0x822c5bde0def3beeu, 0x1db45dc38e0c8261u}, {0xf15bb96ac8b58575u, 0x1290ba9a38c7d17cu},
  {0x2db2a7c57ae2e6d2u, 0x1734e940c6f9c5dcu}, {0x391f51b6d99ba086u, 0x1d022390f8b83753u},
  {0x03b3931248014454u, 0x1221563a9b732294u}, {0x04a077d6da019569u, 0x16a9abc9424feb39u},
  {0x45c895cc9081fac3u, 0x1c5416bb92e3e607u}, {0x8b9d5d9fda513cbau, 0x11b48e353bce6fc4u},
  {0xae84b507d0e58be8u, 0x1621b1c28ac20bb5u}, {0x1a25e249c51eeee3u, 0x1baa1e332d728ea3u},
  {0xf057ad6e1b33554du, 0x114a52dffc679925u}, {0x6c6d98c9a2002aa1u, 0x159ce797fb817f6fu},
  {0x4788fefc0a803549u, 0x1b04217dfa61df4bu}, {0x0cb59f5d8690214eu, 0x10e294eebc7d2b8fu},
  {0xcfe30734e83429a1u, 0x151b3a2a6b9c7672u}, {0x83dbc9022241340au, 0x1a6208b50683940fu},
  {0xb2695da15568c086u, 0x107d457124123c89u}, {0x1f03b509aac2f0a7u, 0x149c96cd6d16cbacu},
  {0x26c4a24c1573acd1u, 0x19c3bc80c85c7e97u}, {0x783ae56f8d684c03u, 0x101a55d07d39cf1eu},
  {0x16499ecb70c25f03u, 0x1420eb449c8842e6u}, {0x9bdc067e4cf2f6c4u, 0x19292615c3aa539fu},
  {0x82d3081de02fb476u, 0x1f736f9b3494e887u}, {0xb1c3e512ac1dd0c9u, 0x13a825c100dd1154u},
  {0xde34de57572544fcu, 0x18922f31411455a9u}, {0x55c215ed2cee963bu, 0x1eb6bafd91596b14u},
  {0xb5994db43c151de5u, 0x133234de7ad7e2ecu}, {0xe2ffa1214b1a655eu, 0x17fec216198ddba7u},
  {0xdbbf89699de0feb6u, 0x1dfe729b9ff15291u}, {0x2957b5e202ac9f31u, 0x12bf07a143f6d39bu},
  {0xf3ada35a8357c6feu, 0x176ec98994f48881u}, {0x70990c31242db8bdu, 0x1d4a7bebfa31aaa2u},
  {0x865fa79eb69c9376u, 0x124e8d737c5f0aa5u}, {0xe7f791866443b854u, 0x16e230d05b76cd4eu},
  {0xa1f575e7fd54a669u, 0x1c9abd04725480a2u}, {0xa53969b0fe54e801u, 0x11e0b622c774d065u},
  {0x0e87c41d3dea2202u, 0x1658e3ab7952047fu}, {0xd229b5248d64aa82u, 0x1bef1c9657a6859eu},
  {0x435a1136d85eea91u, 0x117571ddf6c81383u}, {0x143095848e76a536u, 0x15d2ce55747a1864u},
  {0x193cbae5b2144e83u, 0x1b4781ead1989e7du}, {0x2fc5f4cf8f4cb112u, 0x110cb132c2ff630eu},
  {0xbbb77203731fdd56u, 0x154fdd7f73bf3bd1u}, {0x2aa54e844fe7d4acu, 0x1aa3d4df50af0ac6u},
  {0xdaa75112b1f0e4ebu, 0x10a6650b926d66bbu}, {0xd15125575e6d1e26u, 0x14cffe4e7708c06au},
  {0x85a56ead360865b0u, 0x1a03fde214caf085u}, {0x7387652c41c53f8eu, 0x10427ead4cfed653u},
  {0x50693e7752368f71u, 0x14531e58a03e8be8u}, {0x64838e1526c4334eu, 0x1967e5eec84e2ee2u},
  {0xfda4719a70754022u, 0x1fc1df6a7a61ba9au}, {0xde86c70086494815u, 0x13d92ba28c7d14a0u},
  {0x162878c0a7db9a1au, 0x18cf768b2f9c59c9u}, {0x5bb296f0d1d280a1u, 0x1f03542dfb83703bu},
  {0x194f9e5683239064u, 0x1362149cbd322625u}, {0x5fa385ec23ec747eu, 0x183a99c3ec7eafaeu},
  {0xf78c67672ce7919du, 0x1e494034e79e5b99u}, {0x3ab7c0a07c10bb02u, 0x12edc82110c2f940u},
  {0x4965b0c89b14e9c3u, 0x17a93a2954f3b790u}, {0x5bbf1cfac1da2433u, 0x1d9388b3aa30a574u},
  {0xb957721cb92856a0u, 0x127c35704a5e6768u}, {0xe7ad4ea3e7726c48u, 0x171b42cc5cf60142u},
  {0xa198a24ce14f075au, 0x1ce2137f74338193u}, {0x44ff65700cd16498u, 0x120d4c2fa8a030fcu},
  {0x563f3ecc1005bdbeu, 0x16909f3b92c83d3bu}, {0x2bcf0e7f14072d2eu, 0x1c34c70a777a4c8au},
  {0x5b61690f6c847c3du, 0x11a0fc668aac6fd6u}, {0xf239c35347a59b4cu, 0x16093b802d578bcbu},
  {0xeec83428198f021fu, 0x1b8b8a6038ad6ebeu}, {0x553d20990ff96153u, 0x1137367c236c6537u},
  {0x2a8c68bf53f7b9a8u, 0x1585041b2c477e85u}, {0x752f82ef28f5a812u, 0x1ae64521f7595e26u},
  {0x093db1d57999890bu, 0x10cfeb353a97dad8u}, {0x0b8d1e4ad7ffeb4eu, 0x1503e602893dd18eu},
  {0x8e7065dd8dffe622u, 0x1a44df832b8d45f1u}, {0xf9063faa78bfefd5u, 0x106b0bb1fb384bb6u},
  {0xb747cf9516efebcau, 0x1485ce9e7a065ea4u}, {0xe519c37a5cabe6bdu, 0x19a742461887f64du},
  {0xaf301a2c79eb7036u, 0x1008896bcf54f9f0u}, {0xdafc20b798664c43u, 0x140aabc6c32a386cu},
  {0x11bb28e57e7fdf54u, 0x190d56b873f4c688u}, {0x1629f31ede1fd72au, 0x1f50ac6690f1f82au},
  {0x4dda37f34ad3e67au, 0x13926bc01a973b1au}, {0xe150c5f01d88e019u, 0x187706b0213d09e0u},
  {0x19a4f76c24eb181fu, 0x1e94c85c298c4c59u}, {0xb0071aa39712ef13u, 0x131cfd3999f7afb7u},
  {0x9c08e14c7cd7aad8u, 0x17e43c8800759ba5u}, {0x030b199f9c0d958eu, 0x1ddd4baa0093028fu},
  {0x61e6f003c1887d79u, 0x12aa4f4a405be199u}, {0xba60ac04b1ea9cd7u, 0x1754e31cd072d9ffu},
  {0xa8f8d705de65440du, 0x1d2a1be4048f907fu}, {0xc99b8663aaff4a88u, 0x123a516e82d9ba4fu},
  {0xbc0267fc95bf1d2au, 0x16c8e5ca239028e3u}, {0xab0301fbbb2ee474u, 0x1c7b1f3cac74331cu},
  {0xeae1e13d54fd4ec9u, 0x11ccf385ebc89ff1u}, {0x659a598caa3ca27bu, 0x1640306766bac7eeu},
  {0xff00efefd4cbcb1au, 0x1bd03c81406979e9u}, {0x3f6095f5e4ff5ef0u, 0x116225d0c841ec32u},
  {0xcf38bb735e3f36acu, 0x15baaf44fa52673eu}, {0x8306ea5035cf0457u, 0x1b295b1638e7010eu},
  {0x11e4527221a162b6u, 0x10f9d8ede39060a9u}, {0x565d670eaa09bb64u, 0x15384f295c7478d3u},
  {0x2bf4c0d2548c2a3du, 0x1a8662f3b3919708u}, {0x1b78f88374d79a66u, 0x1093fdd8503afe65u},
  {0x625736a4520d8100u, 0x14b8fd4e6449bdfeu}, {0xfaed044d6690e140u, 0x19e73ca1fd5c2d7du},
  {0xbcd422b0601a8cc8u, 0x103085e53e599c6eu}, {0x6c092b5c78212ffau, 0x143ca75e8df0038au},
  {0x070b763396297bf8u, 0x194bd136316c046du}, {0x48ce53c07bb3daf6u, 0x1f9ec583bdc70588u},
  {0x2d80f4584d5068dau, 0x13c33b72569c6375u}, {0x78e1316e60a48310u, 0x18b40a4eec437c52u},
};
static const uint64_t heff_pow5_inv[342][2] = {
  {0x0000000000000001u, 0x2000000000000000u}, {0x999999999999999au, 0x1999999999999999u},
  {0x47ae147ae147ae15u, 0x147ae147ae147ae1u}, {0x6c8b4395810624deu, 0x10624dd2f1a9fbe7u},
  {0x7a786c226809d496u, 0x1a36e2eb1c432ca5u}, {0x61f9f01b866e43abu, 0x14f8b588e368f084u},
  {0xb4c7f34938583622u, 0x10c6f7a0b5ed8d36u}, {0x87a6520ec08d236au, 0x1ad7f29abcaf4857u},
  {0x9fb841a566d74f88u, 0x15798ee2308c39dfu}, {0xe62d01511f12a607u, 0x112e0be826d694b2u},
  {0xd6ae6881cb5109a4u, 0x1b7cdfd9d7bdbab7u}, {0xdef1ed34a2a73aeau, 0x15fd7fe17964955fu},
  {0x7f27f0f6e885c8bbu, 0x119799812dea1119u}, {0x650cb4be40d60df8u, 0x1c25c268497681c2u},
  {0xea70909833de7193u, 0x16849b86a12b9b01u}, {0x21f3a6e0297ec143u, 0x1203af9ee756159bu},
  {0x6985d7cd0f313537u, 0x1cd2b297d889bc2bu}, {0x2137dfd73f5a90f9u, 0x170ef54646d49689u},
  {0xe75fe645cc4873fau, 0x12725dd1d243aba0u}, {0xa5663d3c7a0d865du, 0x1d83c94fb6d2ac34u},
  {0x511e976394d79eb1u, 0x179ca10c9242235du}, {0xda7edf82dd794bc1u, 0x12e3b40a0e9b4f7du},
  {0x2a6498d1625bac68u, 0x1e392010175ee596u}, {0xeeb6e0a781e2f053u, 0x182db34012b25144u},
  {0x58924d52ce4f26a9u, 0x1357c299a88ea76au}, {0x27507bb7b07ea441u, 0x1ef2d0f5da7dd8aau},
  {0x52a6c95fc0655034u, 0x18c240c4aecb13bbu}, {0x0eebd44c99eaa690u, 0x13ce9a36f23c0fc9u},
  {0xb17953adc3110a80u, 0x1fb0f6be50601941u}, {0xc12ddc8b02740867u, 0x195a5efea6b34767u},
  {0x3424b06f3529a052u, 0x14484bfeebc29f86u}, {0x901d59f290ee19dbu, 0x1039d66589687f9eu},
  {0x4cfbc31db4b0295fu, 0x19f623d5a8a73297u}, {0x3d9635b15d59bab2u, 0x14c4e977ba1f5bacu},
  {0x97ab5e277de16228u, 0x109d8792fb4c4956u}, {0xf2abc9d8c9689d0du, 0x1a95a5b7f87a0ef0u},
  {0x5bbca17a3aba173eu, 0x154484932d2e725au}, {0xafca1ac82efb45cbu, 0x11039d428a8b8eaeu},
  {0xb2dcf7a6b1920945u, 0x1b38fb9daa78e44au}, {0xf57d92ebc141a104u, 0x15c72fb1552d836eu},
  {0xc46475896767b403u, 0x116c262777579c58u}, {0x6d6d88dbd8a5ecd2u, 0x1be03d0bf225c6f4u},
  {0x8abe071646eb23dbu, 0x164cfda3281e38c3u}, {0x6efe6c11d255b649u, 0x11d7314f534b609cu},
  {0xb197134fb6ef8a0eu, 0x1c8b821885456760u}, {0x27ac0f72f8bfa1a5u, 0x16d601ad376ab91au},
  {0xb95672c260994e1eu, 0x1244ce242c5560e1u}, {0xf5571e03cdc21695u, 0x1d3ae36d13bbce35u},
  {0x2aac18030b01ababu, 0x17624f8a762fd82bu}, {0xbbbce0026f348956u, 0x12b50c6ec4f31355u},
  {0x92c7ccd0b1eda889u, 0x1dee7a4ad4b81eefu}, {0xdbd30a408e57ba07u, 0x17f1fb6f10934bf2u},
  {0x7ca8d50071dfc806u, 0x1327fc58da0f6ff5u}, {0xfaa7bb33e9660cd6u, 0x1ea6608e29b24cbbu},
  {0x9552fc298784d711u, 0x18851a0b548ea3c9u}, {0xaaa8c9bad2d0ac0eu, 0x139dae6f76d88307u},
  {0xdddadc5e1e1aace3u, 0x1f62b0b257c0d1a5u}, {0x7e48b04b4b488a4fu, 0x191bc08eac9a4151u},
  {0xcb6d59d5d5d3a1d9u, 0x141633a556e1cddau}, {0x3c577b1177dc817bu, 0x1011c2eaabe7d7e2u},
  {0xc6f25e825960cf2au, 0x19b604aaaca62636u}, {0x6bf518684780a5bbu, 0x14919d5556eb51c5u},
  {0x232a79ed06008496u, 0x10747ddddf22a7d1u}, {0xd1dd8fe1a3340756u, 0x1a53fc9631d10c81u},
  {0xa7e4731ae8f66c45u, 0x150ffd44f4a73d34u}, {0x531d28e253f8569eu, 0x10d9976a5d52975du},
  {0xeb61db03b98d5762u, 0x1af5bf109550f22eu}, {0xbc4e48cfc7a445e8u, 0x159165a6ddda5b58u},
  {0x6371d3d96c836b20u, 0x11411e1f17e1e2adu}, {0x9f1c8628ad9f11cdu, 0x1b9b6364f3030448u},
  {0xe5b06b53be18db0bu, 0x1615e91d8f359d06u}, {0xeaf3890fcb4715a2u, 0x11ab20e472914a6bu},
  {0x44b8db4c7871bc37u, 0x1c45016d841baa46u}, {0x03c715d6c6c1635fu, 0x169d9abe03495505u},
  {0x3638de456bcde919u, 0x1217aefe69077737u}, {0x56c163a2461641c1u, 0x1cf2b1970e725858u},
  {0xdf011c81d1ab67ceu, 0x17288e1271f51379u}, {0x7f3416ce4155eca5u, 0x1286d80ec190dc61u},
  {0x6520247d3556476eu, 0x1da48ce468e7c702u}, {0xea801d30f7783925u, 0x17b6d71d20b96c01u},
  {0xbb99b0f3f92cfa84u, 0x12f8ac174d612334u}, {0x5f5c4e532847f739u, 0x1e5aacf215683854u},
  {0x7f7d0b75b9d32c2eu, 0x18488a5b44536043u}, {0x9930d5f7c7dc2358u, 0x136d3b7c36a919cfu},
  {0x8eb4898c72f9d226u, 0x1f152bf9f10e8fb2u}, {0x722a07a38f2e41b8u, 0x18ddbcc7f40ba628u},
  {0xc1bb394fa5be9afau, 0x13e497065cd61e86u}, {0x9c5ec2190930f7f6u, 0x1fd424d6faf030d7u},
  {0x49e56814075a5ff8u, 0x197683df2f268d79u}, {0x6e51201005e1e660u, 0x145ecfe5bf520ac7u},
  {0xf1da800cd181851au, 0x104bd984990e6f05u}, {0x4fc400148268d4f5u, 0x1a12f5a0f4e3e4d6u},
  {0xd96999aa01ed772bu, 0x14dbf7b3f71cb711u}, {0xadee1488018ac5bcu, 0x10aff95cc5b09274u},
  {0x497ceda668de092cu, 0x1ab328946f80ea54u}, {0x3aca57b853e4d424u, 0x155c2076bf9a5510u},
  {0x623b7960431d7683u, 0x1116805effaeaa73u}, {0x9d2bf566d1c8bd9eu, 0x1b5733cb32b110b8u},
  {0x7dbcc452416d647fu, 0x15df5ca28ef40d60u}, {0xcafd69db678ab6ccu, 0x117f7d4ed8c33de6u},
  {0xab2f0fc572778adfu, 0x1bff2ee48e052fd7u}, {0x88f273045b92d580u, 0x1665bf1d3e6a8cacu},
  {0xd3f528d049424466u, 0x11eaff4a98553d56u}, {0xb988414d4203a0a3u, 0x1cab3210f3bb9557u},
  {0x6139cdd76802e6e9u, 0x16ef5b40c2fc7779u}, {0xe761717920025254u, 0x125915cd68c9f92du},
  {0xa568b58e999d5086u, 0x1d5b561574765b7cu}, {0x5120913ee14aa6d2u, 0x177c44ddf6c515fdu},
  {0xa74d40ff1aa21f0eu, 0x12c9d0b1923744cau}, {0x0baece64f769cb4au, 0x1e0fb44f50586e11u},
  {0x3c8bd850c5ee3c3bu, 0x180c903f7379f1a7u}, {0xca0979da37f1c9c9u, 0x133d4032c2c7f485u},
  {0xa9a8c2f6bfe942dbu, 0x1ec866b79e0cba6fu}, {0x2153cf2bccba9be3u, 0x18a0522c7e709526u},
  {0x1aa9728970954982u, 0x13b374f06526ddb8u}, {0xf775840f1a88759du, 0x1f8587e7083e2f8cu},
  {0x5f9136727ba05e17u, 0x19379fec0698260au}, {0x1940f85b9619e4dfu, 0x142c7ff0054684d5u},
  {0xe100c6afab47ea4cu, 0x1023998cd1053710u}, {0xce67a44c453fdd47u, 0x19d28f47b4d524e7u},
  {0xd852e9d69dccb106u, 0x14a8729fc3ddb71fu}, {0x79dbee454b0a2738u, 0x1086c219697e2c19u},
  {0x295fe3a211a9d859u, 0x1a71368f0f30468fu}, {0xbab31c81a7bb137au, 0x15275ed8d8f36ba5u},
  {0x6228e39aec95a92fu, 0x10ec4be0ad8f8951u}, {0x9d0e38f7e0ef7517u, 0x1b13ac9aaf4c0ee8u},
  {0xb0d82d931a592a79u, 0x15a956e225d67253u}, {0x8d79be0f4847552eu, 0x11544581b7dec1dcu},
  {0x158f967eda0bbb7cu, 0x1bba08cf8c979c94u}, {0x77a611ff14d62f97u, 0x162e6d72d6dfb076u},
  {0xf951a7ff43de8c79u, 0x11bebdf578b2f391u}, {0xc21c3ffed2fdad8eu, 0x1c6463225ab7ec1cu},
  {0x01b0333242648ad8u, 0x16b6b5b5155ff017u}, {0x0159c28e9b83a246u, 0x122bc490dde659acu},
  {0xcef604175f3903a3u, 0x1d12d41afca3c2acu}, {0x725e69ac4c2d9c83u, 0x17424348ca1c9bbdu},
  {0xf5185489d68ae39cu, 0x129b69070816e2fdu}, {0xee8d540fbdab05c6u, 0x1dc574d80cf16b2fu},
  {0xbed77672fe226b05u, 0x17d12a4670c1228cu}, {0xff12c528cb4ebc04u, 0x130dbb6b8d674ed6u},
  {0xcb513b74787df9a0u, 0x1e7c5f127bd87e24u}, {0x090dc929f9fe614du, 0x18637f41fcad31b7u},
  {0xa0d7d42194cb810au, 0x1382cc34ca2427c5u}, {0x67bfb9cf5478ce77u, 0x1f37ad21436d0c6fu},
  {0x1fcc94a5dd2d71f9u, 0x18f9574dcf8a7059u}, {0x7fd6dd517dbdf4c7u, 0x13faac3e3fa1f37au},
  {0xffbe2ee8c92fee0bu, 0x1ff779fd329cb8c3u}, {0x6631bf20a0f324d6u, 0x1992c7fdc216fa36u},
  {0xb827cc1a1a5c1d78u, 0x14756ccb01abfb5eu}, {0x935309ae7b7ce460u, 0x105df0a267bcc918u},
  {0x1eeb42b0c594a099u, 0x1a2fe76a3f9474f4u}, {0xe58902270476e6e1u, 0x14f31f8832dd2a5cu},
  {0xb7a0ce859d2bebe7u, 0x10c27fa028b0eeb0u}, {0x59014a6f61dfdfd8u, 0x1ad0cc33744e4ab4u},
  {0xe0cdd525e7e64cadu, 0x1573d68f903ea229u}, {0x4d7177518651d6f1u, 0x11297872d9cbb4eeu},
  {0x7be8bee8d6e957e8u, 0x1b758d848fac54b0u}, {0xfcba3253df211320u, 0x15f7a46a0c89dd59u},
  {0x63c8284318e74280u, 0x1192e9ee706e4aaeu}, {0x060d0d3827d86a66u, 0x1c1e43171a4a1117u},
  {0x6b3da42cecad21ebu, 0x167e9c127b6e7412u}, {0x88fe1cf0bd574e56u, 0x11fee341fc585cdbu},
  {0x419694b462254a23u, 0x1ccb0536608d615fu}, {0x67abaa29e81dd4e9u, 0x1708d0f84d3de77fu},
  {0xb95621bb2017dd87u, 0x126d73f9d764b932u}, {0xc223692b668c95a5u, 0x1d7becc2f23ac1eau},
  {0xce82ba891ed6de1du, 0x179657025b6234bbu}, {0xa53562074bdf1818u, 0x12deac01e2b4f6fcu},
  {0x3b889cd87964f359u, 0x1e3113363787f194u}, {0xfc6d4a46c783f5e1u, 0x18274291c6065adcu},
  {0x30576e9f06032b1au, 0x13529ba7d19eaf17u}, {0x1a257dcb3cd1de90u, 0x1eea92a61c311825u},
  {0x481dfe3c30a7e540u, 0x18bba884e35a79b7u}, {0xd34b31c9c0865100u, 0x13c9539d82aec7c5u},
  {0x5211e942cda3b4cdu, 0x1fa885c8d117a609u}, {0x74db21023e1c90a4u, 0x19539e3a40dfb807u},
  {0xf715b401cb4a0d50u, 0x1442e4fb67196005u}, {0xf8de299b09080aa7u, 0x103583fc527ab337u},
  {0x8e304291a80cddd7u, 0x19ef3993b72ab859u}, {0x3e8d020e200a4b13u, 0x14bf6142f8eef9e1u},
  {0x653d9b3e80083c0fu, 0x10991a9bfa58c7e7u}, {0x6ec8f864000d2ce4u, 0x1a8e90f9908e0ca5u},
  {0x8bd3f9e999a423eau, 0x153eda614071a3b7u}, {0x3ca994bae1501cbbu, 0x10ff151a99f482f9u},
  {0xc775bac49bb3612bu, 0x1b31bb5dc320d18eu}, {0xd2c4956a16291a89u, 0x15c162b168e70e0bu},
  {0xdbd0778811ba7ba1u, 0x11678227871f3e6fu}, {0x2c80bf401c5d929bu, 0x1bd8d03f3e9863e6u},
  {0xbd33cc3349e47549u, 0x16470cff6546b651u}, {0xca8fd68f6e505dd4u, 0x11d270cc51055ea7u},
  {0x4419574be3b3c953u, 0x1c83e7ad4e6efdd9u}, {0x0347790982f63aa9u, 0x16cfec8aa52597e1u},
  {0xcf6c60d468c4fbbau, 0x123ff06eea847980u}, {0xe57a34870e07f92au, 0x1d331a4b10d3f59au},
  {0x512e906c0b399422u, 0x175c1508da432ae2u}, {0xda8ba6bcd5c7a9b5u, 0x12b010d3e1cf5581u},
  {0x90df712e22d90f87u, 0x1de6815302e5559cu}, {0xda4c5a8b4f140c6cu, 0x17eb9aa8cf1dde16u},
  {0xaea37ba2a5a9a38au, 0x1322e220a5b17e78u}, {0x7dd25f6aa2a905a9u, 0x1e9e369aa2b59727u},
  {0x97db7f888220d154u, 0x187e92154ef7ac1fu}, {0x797c6606ce80a777u, 0x139874ddd8c6234cu},
  {0x8f2d700ae4010bf1u, 0x1f5a549627a36badu}, {0x0c2459a25000d65au, 0x191510781fb5efbeu},
  {0x701d1481d99a4515u, 0x1410d9f9b2f7f2feu}, {0xc017439b147b6a77u, 0x100d7b2e28c65bfeu},
  {0xccf205c4ed9243f2u, 0x19af2b7d0e0a2ccau}, {0x0a5b37d0be0e9cc2u, 0x148c22ca71a1bd6fu},
  {0x0848f973cb3ee3ceu, 0x10701bd527b4978cu}, {0xda0e5bec78649fb0u, 0x1a4cf9550c5425acu},
  {0x7b3eaff060507fc0u, 0x150a6110d6a9b7bdu}, {0x95cbbff380406633u, 0x10d51a73deee2c97u},
  {0xefac665266cd7052u, 0x1aee90b964b04758u}, {0x2623850eb8a459dbu, 0x158ba6fab6f36c47u},
  {0x1e82d0d893b6ae49u, 0x113c85955f29236cu}, {0xfd9e1af41f8ab075u, 0x1b9408eefea838acu},
  {0x97b1af29b2d559f7u, 0x16100725988693bdu}, {0xac8e25baf5777b2cu, 0x11a66c1e139edc97u},
  {0x7a7d092b2258c513u, 0x1c3d79c9b8fe2dbfu}, {0x61fda0ef4ead6a76u, 0x169794a160cb57ccu},
  {0xe7fe1a590bbdeec5u, 0x1212dd4de7091309u}, {0xa6635d5b45fcb13au, 0x1ceafbafd80e84dcu},
  {0x851c4aaf6b308dc8u, 0x172262f3133ed0b0u}, {0xd0e36ef2bc26d7d4u, 0x1281e8c275cbda26u},
  {0xb49f17eac6a48c86u, 0x1d9ca79d894629d7u}, {0x2a18dfef0550706bu, 0x17b08617a104ee46u},
  {0x54e0b3259dd9f389u, 0x12f39e794d9d8b6bu}, {0x87cdeb6f62f65274u, 0x1e5297287c2f4578u},
  {0xd30b22bf825ea85du, 0x18421286c9bf6ac6u}, {0x0f3c1bcc684bb9e4u, 0x13680ed23aff889fu},
  {0x18602c7a4079296du, 0x1f0ce4839198da98u}, {0x46b356c833942124u, 0x18d71d360e13e213u},
  {0x388f78a029434db6u, 0x13df4a91a4dcb4dcu}, {0x5a7f2766a86baf8au, 0x1fcbaa82a1612160u},
  {0x153285ebb9efbfa2u, 0x196fbb9bb44db44du}, {0xaa8ed189618c994eu, 0x145962e2f6a4903du},
  {0xeed8a7a11ad6e10cu, 0x1047824f2bb6d9cau}, {0x7e27729b5e249b45u, 0x1a0c03b1df8af611u},
  {0xfe85f549181d4904u, 0x14d6695b193bf80du}, {0xcb9e5dd4134aa0d0u, 0x10ab877c142ff9a4u},
  {0xdf63c9535211014du, 0x1aac0bf9b9e65c3au}, {0x191ca10f74da6771u, 0x15566ffafb1eb02fu},
  {0xadb080d92a4852c1u, 0x1111f32f2f4bc025u}, {0x15e7348eaa0d5134u, 0x1b4feb7eb212cd09u},
  {0xab1f5d3eee710dc4u, 0x15d98932280f0a6du}, {0xbc1917658b8da49du, 0x117ad428200c0857u},
  {0x2cf4f23c127c3a94u, 0x1bf7b9d9cce00d59u}, {0xf0c3f4fcdb969543u, 0x165fc7e170b33de0u},
  {0x5a365d9716121103u, 0x11e6398126f5cb1au}, {0x9056fc24f01ce804u, 0x1ca38f350b22de90u},
  {0xd9df301d8ce3ecd0u, 0x16e93f5da2824ba6u}, {0xe17f59b13d8323dau, 0x125432b14ecea2ebu},
  {0x68cbc2b52f38395cu, 0x1d53844ee47dd179u}, {0x53d6355dbf602de3u, 0x177603725064a794u},
  {0xa9782ab165e68b1cu, 0x12c4cf8ea6b6ec76u}, {0x0f26aab56fd744fau, 0x1e07b27dd78b13f1u},
  {0x3f52222abfdf6a62u, 0x18062864ac6f4327u}, {0x65db4e88997f884eu, 0x1338205089f29c1fu},
  {0x6fc54a7428cc0d4au, 0x1ec033b40fea9365u}, {0x596aa1f68709a43bu, 0x1899c2f673220f84u},
  {0xadeee7f86c07b696u, 0x13ae3591f5b4d936u}, {0x497e3ff3e00c5756u, 0x1f7d228322baf524u},
  {0xd464fff64cd6ac45u, 0x1930e868e89590e9u}, {0x4383fff83d7889d1u, 0x14272053ed4473eeu},
  {0xcf9cccc69793a174u, 0x101f4d0ff1038ff1u}, {0x7f6147a425b90252u, 0x19cbae7fe805b31cu},
  {0xcc4dd2e9b7c7350fu, 0x14a2f1ffecd15c16u}, {0x3d0b0f215fd290d9u, 0x10825b3323dab012u},
  {0x61ab4b689950e7c1u, 0x1a6a2b85062ab350u}, {0x4e22a2ba1440b967u, 0x1521bc6a6b555c40u},
  {0x0b4ee894dd009453u, 0x10e7c9eebc4449cdu}, {0x1217da87c800ed51u, 0x1b0c764ac6d3a948u},
  {0xdb46486ca000bddau, 0x15a391d56bdc876cu}, {0x490506bd4ccd64afu, 0x114fa7ddefe39f8au},
  {0xa8080ac87ae23ab1u, 0x1bb2a62fe638ff43u}, {0x5339a239fbe82ef4u, 0x162884f31e93ff69u},
  {0x75c7b4fb2fecf25du, 0x11ba03f5b20fff87u}, {0x22d92191e647ea2eu, 0x1c5cd322b67fff3fu},
  {0xb57a8141850654f2u, 0x16b0a8e891ffff65u}, {0xc4620101373843f5u, 0x1226ed86db3332b7u},
  {0x3a366801f1f39feeu, 0x1d0b15a491eb8459u}, {0xfb5eb99b27f6198bu, 0x173c115074bc69e0u},
  {0x2f7efae2865e7ad6u, 0x129674405d6387e7u}, {0xe597f7d0d6fd9156u, 0x1dbd86cd6238d971u},
  {0x8479930d78cadaabu, 0x17cad23de82d7ac1u}, {0xd06142712d6f1556u, 0x1308a831868ac89au},
  {0x4d686a4eaf182222u, 0x1e74404f3daada91u}, {0xa453883ef279b4e8u, 0x185d003f6488aedau},
  {0xe9dc6cff28615d87u, 0x137d99cc506d58aeu}, {0xa960ae650d6895a4u, 0x1f2f5c7a1a488de4u},
  {0xbab3beb73ded4483u, 0x18f2b061aea07183u}, {0x2ef6322c318a9d36u, 0x13f559e7bee6c136u},
  {0xe4bd1d13827761f0u, 0x1feef63f97d79b89u}, {0x83ca7da9352c4e5au, 0x198bf832dfdfafa1u},
  {0x9ca1fe20f756a515u, 0x146ff9c24cb2f2e7u}, {0x4a1b31b3f9121daau, 0x1059949b708f28b9u},
  {0x435eb5ecc1b695ddu, 0x1a28edc580e50df5u}, {0x35e55e57015ede4au, 0x14ed8b04671da4c4u},
  {0xc4b77eac0118b1d5u, 0x10be08d0527e1d69u}, {0xa12597799b5ab622u, 0x1ac9a7b3b7302f0fu},
  {0x4db7ac6149155e81u, 0x156e1fc2f8f358d9u}, {0xd7c6238107444b9bu, 0x1124e63593f5e0adu},
  {0x593d059b3ed3ac2bu, 0x1b6e3d2286563449u}, {0xe0fd9e15cbdc89bcu, 0x15f1ca820511c36du},
  {0xb3fe18116fe3a163u, 0x118e3b9b37416924u}, {0x866359b57fd29bd1u, 0x1c16c5c525357507u},
  {0xd1e91491330ee30eu, 0x16789e3750f790d2u}, {0x74ba76da8f3f1c0bu, 0x11fa182c40c60d75u},
  {0xedf72490e531c678u, 0x1cc359e067a348bbu}, {0x8b2c1d40b75b052du, 0x1702ae4d1fb5d3c9u},
  {0x6f567dcd5f7c0424u, 0x12688b70e62b0fd4u}, {0x7ef0c94898c66d06u, 0x1d74124e3d11b2edu},
  {0x98c0a106e09ebd9fu, 0x17900ea4fda7c257u}, {0x470080d24d4bcae6u, 0x12d9a550caec9b79u},
  {0xd800ce1d487944a2u, 0x1e29088144adc58eu}, {0x1333d8176d2dd082u, 0x1820d39a9d57d13fu},
  {0xa8f646792424a6ceu, 0x134d76154aaca765u}, {0x74bd3d8ea03aa47du, 0x1ee25688777aa56fu},
  {0x5d64313ee6955064u, 0x18b51206c5fbb78cu}, {0x4ab68dcbebaaa6b7u, 0x13c40e6bd1962c70u},
  {0x1124161312aaa457u, 0x1fa01712e8f0471au}, {0xda8344dc0eeee9dfu, 0x194cdf4253f36c14u},
  {0xe2029d7cd8bf2180u, 0x143d7f6843292343u}, {0x4e687dfd7a328133u, 0x103132b9cf541c36u},
  {0x4a40c9959050ceb8u, 0x19e851294bb9c6bdu}, {0x0833d477a6a70bc6u, 0x14b9da876fc7d231u},
  {0xa02976c61eec096bu, 0x1094aed2bfd30e8du}, {0x004257a364acdbdfu, 0x1a877e1dffb81749u},
  {0xcd01dfb5ea23e319u, 0x153931b1996012a0u}, {0x70ce4c91881cb5aeu, 0x10fa8e27ade6754du},
  {0x1ae3adb5a69455e2u, 0x1b2a7d0c4970bbafu}, {0x7be957c4854377e8u, 0x15bb973d078d62f2u},
  {0xc987796a0435f987u, 0x1162df64060ab58eu}, {0x75a58f1006bcc271u, 0x1bd1656cd67788e4u},
  {0xf7b7a5a66bca3527u, 0x16411df0ab92d3e9u}, {0x5fc61e1ebca1c41fu, 0x11cdb18d560f0feeu},
  {0xffa363646102d365u, 0x1c7c4f4889b1b316u}, {0x32e91c504d9bdc51u, 0x16c9d906d48e28dfu},
  {0x8f20e37371497d0eu, 0x123b140576d820b2u}, {0x7e9b0585820f2e7cu, 0x1d2b533bf159cdeau},
  {0xcbaf379e01a5becau, 0x1755dc2ff447d7eeu}, {0x0958f94b348498a1u, 0x12ab168cc36cacbfu},
};

static inline uint32_t heff_log10_pow2(uint32_t e) { return (uint32_t)(((uint64_t)e * UINT64_C(169464822037455)) >> 49); }
static inline uint32_t heff_log10_pow5(uint32_t e) { return (uint32_t)(((uint64_t)e * UINT64_C(196742565691928)) >> 48); }
static inline uint32_t heff_pow5_bits(uint32_t e) { return (uint32_t)((((uint64_t)e * UINT64_C(163391164108059)) >> 46) + 1); }

static inline int heff_pow5_divides(uint64_t v, uint32_t p) {
  uint32_t count = 0;
  for (; v > 0; v /= 5, count++)
    if (v % 5 != 0) return count >= p;
  return 0 >= p;
}

static inline uint64_t heff_mul_shift(uint64_t m, const uint64_t *mul, uint32_t j) {
  if (j >= 128) return 0;
  unsigned __int128 b0 = (unsigned __int128)m * mul[0], b2 = (unsigned __int128)m * mul[1];
  return (uint64_t)(((b0 >> 64) + b2) >> (j - 64));
}

HEFF_FN uint64_t heff_shortest(uint64_t bits, int mbits, int ebits, int32_t *exponent) {
  int32_t bias = (1 << (ebits - 1)) - 1;
  uint64_t ieee_m = bits & ((UINT64_C(1) << mbits) - 1);
  int32_t ieee_e = (int32_t)((bits >> mbits) & ((UINT64_C(1) << ebits) - 1));
  int32_t e2;
  uint64_t m2;
  if (ieee_e == 0) { e2 = 1 - bias - mbits - 2; m2 = ieee_m; }
  else { e2 = ieee_e - bias - mbits - 2; m2 = (UINT64_C(1) << mbits) | ieee_m; }
  int accept = (m2 & 1) == 0;
  uint64_t mv = 4 * m2;
  uint64_t mm_shift = ieee_m != 0 || ieee_e == 0;
  uint64_t vr, vp, vm;
  int32_t e10;
  int vm_zeros = 0, vr_zeros = 0;
  if (e2 >= 0) {
    uint32_t q = heff_log10_pow2((uint32_t)e2) - (e2 > 3);
    e10 = (int32_t)q;
    int32_t k = 125 + (int32_t)heff_pow5_bits(q) - 1;
    uint32_t i = (uint32_t)(-e2 + (int32_t)q + k);
    vr = heff_mul_shift(4 * m2, heff_pow5_inv[q], i);
    vp = heff_mul_shift(4 * m2 + 2, heff_pow5_inv[q], i);
    vm = heff_mul_shift(4 * m2 - 1 - mm_shift, heff_pow5_inv[q], i);
    if (q <= 21) {
      if (mv % 5 == 0) vr_zeros = heff_pow5_divides(mv, q);
      else if (accept) vm_zeros = heff_pow5_divides(mv - 1 - mm_shift, q);
      else vp -= heff_pow5_divides(mv + 2, q);
    }
  } else {
    uint32_t q = heff_log10_pow5((uint32_t)-e2) - (-e2 > 1);
    e10 = (int32_t)q + e2;
    int32_t i = -e2 - (int32_t)q;
    int32_t k = (int32_t)heff_pow5_bits((uint32_t)i) - 125;
    uint32_t j = (uint32_t)((int32_t)q - k);
    vr = heff_mul_shift(4 * m2, heff_pow5[i], j);
    vp = heff_mul_shift(4 * m2 + 2, heff_pow5[i], j);
    vm = heff_mul_shift(4 * m2 - 1 - mm_shift, heff_pow5[i], j);
    if (q <= 1) {
      vr_zeros = 1;
      if (accept) vm_zeros = mm_shift == 1;
      else vp -= 1;
    } else if (q < 63) vr_zeros = (mv & ((UINT64_C(1) << q) - 1)) == 0;
  }
  int32_t removed = 0;
  uint64_t last = 0;
  while (vp / 10 > vm / 10) {
    vm_zeros = vm_zeros && vm % 10 == 0;
    vr_zeros = vr_zeros && last == 0;
    last = vr % 10;
    vr /= 10; vp /= 10; vm /= 10;
    removed++;
  }
  if (vm_zeros)
    while (vm % 10 == 0) {
      vr_zeros = vr_zeros && last == 0;
      last = vr % 10;
      vr /= 10; vp /= 10; vm /= 10;
      removed++;
    }
  if (vr_zeros && last == 5 && vr % 2 == 0) last = 4;
  *exponent = e10 + removed;
  return vr + ((vr == vm && (!accept || !vm_zeros)) || last >= 5);
}

HEFF_FN void heff_float_literal(uint64_t bits, int mbits, int ebits, char *out) {
  int neg = (int)((bits >> (mbits + ebits)) & 1);
  uint64_t man = bits & ((UINT64_C(1) << mbits) - 1);
  uint64_t ex = (bits >> mbits) & ((UINT64_C(1) << ebits) - 1);
  if (ex == (UINT64_C(1) << ebits) - 1) {
    strcpy(out, man ? "NaN" : neg ? "-Infinity" : "Infinity");
    return;
  }
  if (ex == 0 && man == 0) {
    strcpy(out, neg ? "-0.0" : "0.0");
    return;
  }
  int32_t e;
  uint64_t m = heff_shortest(bits, mbits, ebits, &e);
  while (m % 10 == 0) { m /= 10; e++; }
  char room[20];
  int k = 0;
  do room[19 - k++] = (char)('0' + m % 10); while (m /= 10);
  const char *digits = room + 20 - k;
  int32_t n = e + k;
  char *o = out;
  if (neg) *o++ = '-';
  if (k <= n && n <= 21) {
    memcpy(o, digits, (size_t)k); o += k;
    memset(o, '0', (size_t)(n - k)); o += n - k;
    *o++ = '.'; *o++ = '0';
  } else if (0 < n && n <= 21) {
    memcpy(o, digits, (size_t)n); o += n;
    *o++ = '.';
    memcpy(o, digits + n, (size_t)(k - n)); o += k - n;
  } else if (-6 < n && n <= 0) {
    *o++ = '0'; *o++ = '.';
    memset(o, '0', (size_t)-n); o += -n;
    memcpy(o, digits, (size_t)k); o += k;
  } else {
    *o++ = digits[0]; *o++ = '.';
    if (k > 1) { memcpy(o, digits + 1, (size_t)(k - 1)); o += k - 1; }
    else *o++ = '0';
    *o++ = 'e';
    int32_t x = n - 1;
    if (x < 0) { *o++ = '-'; x = -x; }
    char rev[4];
    int r = 0;
    do rev[r++] = (char)('0' + x % 10); while (x /= 10);
    while (r) *o++ = rev[--r];
  }
  *o = 0;
}

HEFF_FN void heff_show_float(double d, char *out) {
  uint64_t bits;
  memcpy(&bits, &d, 8);
  heff_float_literal(bits, 52, 11, out);
}

HEFF_FN void heff_show_float32(float f, char *out) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  heff_float_literal(bits, 23, 8, out);
}

static inline int64_t heff_digits_end(const char *p, int64_t n, int64_t i) {
  while (i < n && p[i] >= '0' && p[i] <= '9') i++;
  return i;
}

static inline int heff_is_decimal(const char *p, int64_t n) {
  int64_t i = n > 0 && p[0] == '-', j = heff_digits_end(p, n, i);
  if (j == i) return 0;
  if (j < n && p[j] == '.') {
    i = j + 1;
    j = heff_digits_end(p, n, i);
    if (j == i) return 0;
  }
  if (j < n && (p[j] == 'e' || p[j] == 'E')) {
    i = j + 1;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    j = heff_digits_end(p, n, i);
    if (j == i) return 0;
  }
  return j == n;
}

static const uint64_t heff_pow5_128[651][2] = {
  {0xeef453d6923bd65au, 0x113faa2906a13b3fu}, {0x9558b4661b6565f8u, 0x4ac7ca59a424c507u},
  {0xbaaee17fa23ebf76u, 0x5d79bcf00d2df649u}, {0xe95a99df8ace6f53u, 0xf4d82c2c107973dcu},
  {0x91d8a02bb6c10594u, 0x79071b9b8a4be869u}, {0xb64ec836a47146f9u, 0x9748e2826cdee284u},
  {0xe3e27a444d8d98b7u, 0xfd1b1b2308169b25u}, {0x8e6d8c6ab0787f72u, 0xfe30f0f5e50e20f7u},
  {0xb208ef855c969f4fu, 0xbdbd2d335e51a935u}, {0xde8b2b66b3bc4723u, 0xad2c788035e61382u},
  {0x8b16fb203055ac76u, 0x4c3bcb5021afcc31u}, {0xaddcb9e83c6b1793u, 0xdf4abe242a1bbf3du},
  {0xd953e8624b85dd78u, 0xd71d6dad34a2af0du}, {0x87d4713d6f33aa6bu, 0x8672648c40e5ad68u},
  {0xa9c98d8ccb009506u, 0x680efdaf511f18c2u}, {0xd43bf0effdc0ba48u, 0x0212bd1b2566def2u},
  {0x84a57695fe98746du, 0x014bb630f7604b57u}, {0xa5ced43b7e3e9188u, 0x419ea3bd35385e2du},
  {0xcf42894a5dce35eau, 0x52064cac828675b9u}, {0x818995ce7aa0e1b2u, 0x7343efebd1940993u},
  {0xa1ebfb4219491a1fu, 0x1014ebe6c5f90bf8u}, {0xca66fa129f9b60a6u, 0xd41a26e077774ef6u},
  {0xfd00b897478238d0u, 0x8920b098955522b4u}, {0x9e20735e8cb16382u, 0x55b46e5f5d5535b0u},
  {0xc5a890362fddbc62u, 0xeb2189f734aa831du}, {0xf712b443bbd52b7bu, 0xa5e9ec7501d523e4u},
  {0x9a6bb0aa55653b2du, 0x47b233c92125366eu}, {0xc1069cd4eabe89f8u, 0x999ec0bb696e840au},
  {0xf148440a256e2c76u, 0xc00670ea43ca250du}, {0x96cd2a865764dbcau, 0x380406926a5e5728u},
  {0xbc807527ed3e12bcu, 0xc605083704f5ecf2u}, {0xeba09271e88d976bu, 0xf7864a44c633682eu},
  {0x93445b8731587ea3u, 0x7ab3ee6afbe0211du}, {0xb8157268fdae9e4cu, 0x5960ea05bad82964u},
  {0xe61acf033d1a45dfu, 0x6fb92487298e33bdu}, {0x8fd0c16206306babu, 0xa5d3b6d479f8e056u},
  {0xb3c4f1ba87bc8696u, 0x8f48a4899877186cu}, {0xe0b62e2929aba83cu, 0x331acdabfe94de87u},
  {0x8c71dcd9ba0b4925u, 0x9ff0c08b7f1d0b14u}, {0xaf8e5410288e1b6fu, 0x07ecf0ae5ee44dd9u},
  {0xdb71e91432b1a24au, 0xc9e82cd9f69d6150u}, {0x892731ac9faf056eu, 0xbe311c083a225cd2u},
  {0xab70fe17c79ac6cau, 0x6dbd630a48aaf406u}, {0xd64d3d9db981787du, 0x092cbbccdad5b108u},
  {0x85f0468293f0eb4eu, 0x25bbf56008c58ea5u}, {0xa76c582338ed2621u, 0xaf2af2b80af6f24eu},
  {0xd1476e2c07286faau, 0x1af5af660db4aee1u}, {0x82cca4db847945cau, 0x50d98d9fc890ed4du},
  {0xa37fce126597973cu, 0xe50ff107bab528a0u}, {0xcc5fc196fefd7d0cu, 0x1e53ed49a96272c8u},
  {0xff77b1fcbebcdc4fu, 0x25e8e89c13bb0f7au}, {0x9faacf3df73609b1u, 0x77b191618c54e9acu},
  {0xc795830d75038c1du, 0xd59df5b9ef6a2417u}, {0xf97ae3d0d2446f25u, 0x4b0573286b44ad1du},
  {0x9becce62836ac577u, 0x4ee367f9430aec32u}, {0xc2e801fb244576d5u, 0x229c41f793cda73fu},
  {0xf3a20279ed56d48au, 0x6b43527578c1110fu}, {0x9845418c345644d6u, 0x830a13896b78aaa9u},
  {0xbe5691ef416bd60cu, 0x23cc986bc656d553u}, {0xedec366b11c6cb8fu, 0x2cbfbe86b7ec8aa8u},
  {0x94b3a202eb1c3f39u, 0x7bf7d71432f3d6a9u}, {0xb9e08a83a5e34f07u, 0xdaf5ccd93fb0cc53u},
  {0xe858ad248f5c22c9u, 0xd1b3400f8f9cff68u}, {0x91376c36d99995beu, 0x23100809b9c21fa1u},
  {0xb58547448ffffb2du, 0xabd40a0c2832a78au}, {0xe2e69915b3fff9f9u, 0x16c90c8f323f516cu},
  {0x8dd01fad907ffc3bu, 0xae3da7d97f6792e3u}, {0xb1442798f49ffb4au, 0x99cd11cfdf41779cu},
  {0xdd95317f31c7fa1du, 0x40405643d711d583u}, {0x8a7d3eef7f1cfc52u, 0x482835ea666b2572u},
  {0xad1c8eab5ee43b66u, 0xda3243650005eecfu}, {0xd863b256369d4a40u, 0x90bed43e40076a82u},
  {0x873e4f75e2224e68u, 0x5a7744a6e804a291u}, {0xa90de3535aaae202u, 0x711515d0a205cb36u},
  {0xd3515c2831559a83u, 0x0d5a5b44ca873e03u}, {0x8412d9991ed58091u, 0xe858790afe9486c2u},
  {0xa5178fff668ae0b6u, 0x626e974dbe39a872u}, {0xce5d73ff402d98e3u, 0xfb0a3d212dc8128fu},
  {0x80fa687f881c7f8eu, 0x7ce66634bc9d0b99u}, {0xa139029f6a239f72u, 0x1c1fffc1ebc44e80u},
  {0xc987434744ac874eu, 0xa327ffb266b56220u}, {0xfbe9141915d7a922u, 0x4bf1ff9f0062baa8u},
  {0x9d71ac8fada6c9b5u, 0x6f773fc3603db4a9u}, {0xc4ce17b399107c22u, 0xcb550fb4384d21d3u},
  {0xf6019da07f549b2bu, 0x7e2a53a146606a48u}, {0x99c102844f94e0fbu, 0x2eda7444cbfc426du},
  {0xc0314325637a1939u, 0xfa911155fefb5308u}, {0xf03d93eebc589f88u, 0x793555ab7eba27cau},
  {0x96267c7535b763b5u, 0x4bc1558b2f3458deu}, {0xbbb01b9283253ca2u, 0x9eb1aaedfb016f16u},
  {0xea9c227723ee8bcbu, 0x465e15a979c1cadcu}, {0x92a1958a7675175fu, 0x0bfacd89ec191ec9u},
  {0xb749faed14125d36u, 0xcef980ec671f667bu}, {0xe51c79a85916f484u, 0x82b7e12780e7401au},
  {0x8f31cc0937ae58d2u, 0xd1b2ecb8b0908810u}, {0xb2fe3f0b8599ef07u, 0x861fa7e6dcb4aa15u},
  {0xdfbdcece67006ac9u, 0x67a791e093e1d49au}, {0x8bd6a141006042bdu, 0xe0c8bb2c5c6d24e0u},
  {0xaecc49914078536du, 0x58fae9f773886e18u}, {0xda7f5bf590966848u, 0xaf39a475506a899eu},
  {0x888f99797a5e012du, 0x6d8406c952429603u}, {0xaab37fd7d8f58178u, 0xc8e5087ba6d33b83u},
  {0xd5605fcdcf32e1d6u, 0xfb1e4a9a90880a64u}, {0x855c3be0a17fcd26u, 0x5cf2eea09a55067fu},
  {0xa6b34ad8c9dfc06fu, 0xf42faa48c0ea481eu}, {0xd0601d8efc57b08bu, 0xf13b94daf124da26u},
  {0x823c12795db6ce57u, 0x76c53d08d6b70858u}, {0xa2cb1717b52481edu, 0x54768c4b0c64ca6eu},
  {0xcb7ddcdda26da268u, 0xa9942f5dcf7dfd09u}, {0xfe5d54150b090b02u, 0xd3f93b35435d7c4cu},
  {0x9efa548d26e5a6e1u, 0xc47bc5014a1a6dafu}, {0xc6b8e9b0709f109au, 0x359ab6419ca1091bu},
  {0xf867241c8cc6d4c0u, 0xc30163d203c94b62u}, {0x9b407691d7fc44f8u, 0x79e0de63425dcf1du},
  {0xc21094364dfb5636u, 0x985915fc12f542e4u}, {0xf294b943e17a2bc4u, 0x3e6f5b7b17b2939du},
  {0x979cf3ca6cec5b5au, 0xa705992ceecf9c42u}, {0xbd8430bd08277231u, 0x50c6ff782a838353u},
  {0xece53cec4a314ebdu, 0xa4f8bf5635246428u}, {0x940f4613ae5ed136u, 0x871b7795e136be99u},
  {0xb913179899f68584u, 0x28e2557b59846e3fu}, {0xe757dd7ec07426e5u, 0x331aeada2fe589cfu},
  {0x9096ea6f3848984fu, 0x3ff0d2c85def7621u}, {0xb4bca50b065abe63u, 0x0fed077a756b53a9u},
  {0xe1ebce4dc7f16dfbu, 0xd3e8495912c62894u}, {0x8d3360f09cf6e4bdu, 0x64712dd7abbbd95cu},
  {0xb080392cc4349decu, 0xbd8d794d96aacfb3u}, {0xdca04777f541c567u, 0xecf0d7a0fc5583a0u},
  {0x89e42caaf9491b60u, 0xf41686c49db57244u}, {0xac5d37d5b79b6239u, 0x311c2875c522ced5u},
  {0xd77485cb25823ac7u, 0x7d633293366b828bu}, {0x86a8d39ef77164bcu, 0xae5dff9c02033197u},
  {0xa8530886b54dbdebu, 0xd9f57f830283fdfcu}, {0xd267caa862a12d66u, 0xd072df63c324fd7bu},
  {0x8380dea93da4bc60u, 0x4247cb9e59f71e6du}, {0xa46116538d0deb78u, 0x52d9be85f074e608u},
  {0xcd795be870516656u, 0x67902e276c921f8bu}, {0x806bd9714632dff6u, 0x00ba1cd8a3db53b6u},
  {0xa086cfcd97bf97f3u, 0x80e8a40eccd228a4u}, {0xc8a883c0fdaf7df0u, 0x6122cd128006b2cdu},
  {0xfad2a4b13d1b5d6cu, 0x796b805720085f81u}, {0x9cc3a6eec6311a63u, 0xcbe3303674053bb0u},
  {0xc3f490aa77bd60fcu, 0xbedbfc4411068a9cu}, {0xf4f1b4d515acb93bu, 0xee92fb5515482d44u},
  {0x991711052d8bf3c5u, 0x751bdd152d4d1c4au}, {0xbf5cd54678eef0b6u, 0xd262d45a78a0635du},
  {0xef340a98172aace4u, 0x86fb897116c87c34u}, {0x9580869f0e7aac0eu, 0xd45d35e6ae3d4da0u},
  {0xbae0a846d2195712u, 0x8974836059cca109u}, {0xe998d258869facd7u, 0x2bd1a438703fc94bu},
  {0x91ff83775423cc06u, 0x7b6306a34627ddcfu}, {0xb67f6455292cbf08u, 0x1a3bc84c17b1d542u},
  {0xe41f3d6a7377eecau, 0x20caba5f1d9e4a93u}, {0x8e938662882af53eu, 0x547eb47b7282ee9cu},
  {0xb23867fb2a35b28du, 0xe99e619a4f23aa43u}, {0xdec681f9f4c31f31u, 0x6405fa00e2ec94d4u},
  {0x8b3c113c38f9f37eu, 0xde83bc408dd3dd04u}, {0xae0b158b4738705eu, 0x9624ab50b148d445u},
  {0xd98ddaee19068c76u, 0x3badd624dd9b0957u}, {0x87f8a8d4cfa417c9u, 0xe54ca5d70a80e5d6u},
  {0xa9f6d30a038d1dbcu, 0x5e9fcf4ccd211f4cu}, {0xd47487cc8470652bu, 0x7647c3200069671fu},
  {0x84c8d4dfd2c63f3bu, 0x29ecd9f40041e073u}, {0xa5fb0a17c777cf09u, 0xf468107100525890u},
  {0xcf79cc9db955c2ccu, 0x7182148d4066eeb4u}, {0x81ac1fe293d599bfu, 0xc6f14cd848405530u},
  {0xa21727db38cb002fu, 0xb8ada00e5a506a7cu}, {0xca9cf1d206fdc03bu, 0xa6d90811f0e4851cu},
  {0xfd442e4688bd304au, 0x908f4a166d1da663u}, {0x9e4a9cec15763e2eu, 0x9a598e4e043287feu},
  {0xc5dd44271ad3cdbau, 0x40eff1e1853f29fdu}, {0xf7549530e188c128u, 0xd12bee59e68ef47cu},
  {0x9a94dd3e8cf578b9u, 0x82bb74f8301958ceu}, {0xc13a148e3032d6e7u, 0xe36a52363c1faf01u},
  {0xf18899b1bc3f8ca1u, 0xdc44e6c3cb279ac1u}, {0x96f5600f15a7b7e5u, 0x29ab103a5ef8c0b9u},
  {0xbcb2b812db11a5deu, 0x7415d448f6b6f0e7u}, {0xebdf661791d60f56u, 0x111b495b3464ad21u},
  {0x936b9fcebb25c995u, 0xcab10dd900beec34u}, {0xb84687c269ef3bfbu, 0x3d5d514f40eea742u},
  {0xe65829b3046b0afau, 0x0cb4a5a3112a5112u}, {0x8ff71a0fe2c2e6dcu, 0x47f0e785eaba72abu},
  {0xb3f4e093db73a093u, 0x59ed216765690f56u}, {0xe0f218b8d25088b8u, 0x306869c13ec3532cu},
  {0x8c974f7383725573u, 0x1e414218c73a13fbu}, {0xafbd2350644eeacfu, 0xe5d1929ef90898fau},
  {0xdbac6c247d62a583u, 0xdf45f746b74abf39u}, {0x894bc396ce5da772u, 0x6b8bba8c328eb783u},
  {0xab9eb47c81f5114fu, 0x066ea92f3f326564u}, {0xd686619ba27255a2u, 0xc80a537b0efefebdu},
  {0x8613fd0145877585u, 0xbd06742ce95f5f36u}, {0xa798fc4196e952e7u, 0x2c48113823b73704u},
  {0xd17f3b51fca3a7a0u, 0xf75a15862ca504c5u}, {0x82ef85133de648c4u, 0x9a984d73dbe722fbu},
  {0xa3ab66580d5fdaf5u, 0xc13e60d0d2e0ebbau}, {0xcc963fee10b7d1b3u, 0x318df905079926a8u},
  {0xffbbcfe994e5c61fu, 0xfdf17746497f7052u}, {0x9fd561f1fd0f9bd3u, 0xfeb6ea8bedefa633u},
  {0xc7caba6e7c5382c8u, 0xfe64a52ee96b8fc0u}, {0xf9bd690a1b68637bu, 0x3dfdce7aa3c673b0u},
  {0x9c1661a651213e2du, 0x06bea10ca65c084eu}, {0xc31bfa0fe5698db8u, 0x486e494fcff30a62u},
  {0xf3e2f893dec3f126u, 0x5a89dba3c3efccfau}, {0x986ddb5c6b3a76b7u, 0xf89629465a75e01cu},
  {0xbe89523386091465u, 0xf6bbb397f1135823u}, {0xee2ba6c0678b597fu, 0x746aa07ded582e2cu},
  {0x94db483840b717efu, 0xa8c2a44eb4571cdcu}, {0xba121a4650e4ddebu, 0x92f34d62616ce413u},
  {0xe896a0d7e51e1566u, 0x77b020baf9c81d17u}, {0x915e2486ef32cd60u, 0x0ace1474dc1d122eu},
  {0xb5b5ada8aaff80b8u, 0x0d819992132456bau}, {0xe3231912d5bf60e6u, 0x10e1fff697ed6c69u},
  {0x8df5efabc5979c8fu, 0xca8d3ffa1ef463c1u}, {0xb1736b96b6fd83b3u, 0xbd308ff8a6b17cb2u},
  {0xddd0467c64bce4a0u, 0xac7cb3f6d05ddbdeu}, {0x8aa22c0dbef60ee4u, 0x6bcdf07a423aa96bu},
  {0xad4ab7112eb3929du, 0x86c16c98d2c953c6u}, {0xd89d64d57a607744u, 0xe871c7bf077ba8b7u},
  {0x87625f056c7c4a8bu, 0x11471cd764ad4972u}, {0xa93af6c6c79b5d2du, 0xd598e40d3dd89bcfu},
  {0xd389b47879823479u, 0x4aff1d108d4ec2c3u}, {0x843610cb4bf160cbu, 0xcedf722a585139bau},
  {0xa54394fe1eedb8feu, 0xc2974eb4ee658828u}, {0xce947a3da6a9273eu, 0x733d226229feea32u},
  {0x811ccc668829b887u, 0x0806357d5a3f525fu}, {0xa163ff802a3426a8u, 0xca07c2dcb0cf26f7u},
  {0xc9bcff6034c13052u, 0xfc89b393dd02f0b5u}, {0xfc2c3f3841f17c67u, 0xbbac2078d443ace2u},
  {0x9d9ba7832936edc0u, 0xd54b944b84aa4c0du}, {0xc5029163f384a931u, 0x0a9e795e65d4df11u},
  {0xf64335bcf065d37du, 0x4d4617b5ff4a16d5u}, {0x99ea0196163fa42eu, 0x504bced1bf8e4e45u},
  {0xc06481fb9bcf8d39u, 0xe45ec2862f71e1d6u}, {0xf07da27a82c37088u, 0x5d767327bb4e5a4cu},
  {0x964e858c91ba2655u, 0x3a6a07f8d510f86fu}, {0xbbe226efb628afeau, 0x890489f70a55368bu},
  {0xeadab0aba3b2dbe5u, 0x2b45ac74ccea842eu}, {0x92c8ae6b464fc96fu, 0x3b0b8bc90012929du},
  {0xb77ada0617e3bbcbu, 0x09ce6ebb40173744u}, {0xe55990879ddcaabdu, 0xcc420a6a101d0515u},
  {0x8f57fa54c2a9eab6u, 0x9fa946824a12232du}, {0xb32df8e9f3546564u, 0x47939822dc96abf9u},
  {0xdff9772470297ebdu, 0x59787e2b93bc56f7u}, {0x8bfbea76c619ef36u, 0x57eb4edb3c55b65au},
  {0xaefae51477a06b03u, 0xede622920b6b23f1u}, {0xdab99e59958885c4u, 0xe95fab368e45ecedu},
  {0x88b402f7fd75539bu, 0x11dbcb0218ebb414u}, {0xaae103b5fcd2a881u, 0xd652bdc29f26a119u},
  {0xd59944a37c0752a2u, 0x4be76d3346f0495fu}, {0x857fcae62d8493a5u, 0x6f70a4400c562ddbu},
  {0xa6dfbd9fb8e5b88eu, 0xcb4ccd500f6bb952u}, {0xd097ad07a71f26b2u, 0x7e2000a41346a7a7u},
  {0x825ecc24c873782fu, 0x8ed400668c0c28c8u}, {0xa2f67f2dfa90563bu, 0x728900802f0f32fau},
  {0xcbb41ef979346bcau, 0x4f2b40a03ad2ffb9u}, {0xfea126b7d78186bcu, 0xe2f610c84987bfa8u},
  {0x9f24b832e6b0f436u, 0x0dd9ca7d2df4d7c9u}, {0xc6ede63fa05d3143u, 0x91503d1c79720dbbu},
  {0xf8a95fcf88747d94u, 0x75a44c6397ce912au}, {0x9b69dbe1b548ce7cu, 0xc986afbe3ee11abau},
  {0xc24452da229b021bu, 0xfbe85badce996168u}, {0xf2d56790ab41c2a2u, 0xfae27299423fb9c3u},
  {0x97c560ba6b0919a5u, 0xdccd879fc967d41au}, {0xbdb6b8e905cb600fu, 0x5400e987bbc1c920u},
  {0xed246723473e3813u, 0x290123e9aab23b68u}, {0x9436c0760c86e30bu, 0xf9a0b6720aaf6521u},
  {0xb94470938fa89bceu, 0xf808e40e8d5b3e69u}, {0xe7958cb87392c2c2u, 0xb60b1d1230b20e04u},
  {0x90bd77f3483bb9b9u, 0xb1c6f22b5e6f48c2u}, {0xb4ecd5f01a4aa828u, 0x1e38aeb6360b1af3u},
  {0xe2280b6c20dd5232u, 0x25c6da63c38de1b0u}, {0x8d590723948a535fu, 0x579c487e5a38ad0eu},
  {0xb0af48ec79ace837u, 0x2d835a9df0c6d851u}, {0xdcdb1b2798182244u, 0xf8e431456cf88e65u},
  {0x8a08f0f8bf0f156bu, 0x1b8e9ecb641b58ffu}, {0xac8b2d36eed2dac5u, 0xe272467e3d222f3fu},
  {0xd7adf884aa879177u, 0x5b0ed81dcc6abb0fu}, {0x86ccbb52ea94baeau, 0x98e947129fc2b4e9u},
  {0xa87fea27a539e9a5u, 0x3f2398d747b36224u}, {0xd29fe4b18e88640eu, 0x8eec7f0d19a03aadu},
  {0x83a3eeeef9153e89u, 0x1953cf68300424acu}, {0xa48ceaaab75a8e2bu, 0x5fa8c3423c052dd7u},
  {0xcdb02555653131b6u, 0x3792f412cb06794du}, {0x808e17555f3ebf11u, 0xe2bbd88bbee40bd0u},
  {0xa0b19d2ab70e6ed6u, 0x5b6aceaeae9d0ec4u}, {0xc8de047564d20a8bu, 0xf245825a5a445275u},
  {0xfb158592be068d2eu, 0xeed6e2f0f0d56712u}, {0x9ced737bb6c4183du, 0x55464dd69685606bu},
  {0xc428d05aa4751e4cu, 0xaa97e14c3c26b886u}, {0xf53304714d9265dfu, 0xd53dd99f4b3066a8u},
  {0x993fe2c6d07b7fabu, 0xe546a8038efe4029u}, {0xbf8fdb78849a5f96u, 0xde98520472bdd033u},
  {0xef73d256a5c0f77cu, 0x963e66858f6d4440u}, {0x95a8637627989aadu, 0xdde7001379a44aa8u},
  {0xbb127c53b17ec159u, 0x5560c018580d5d52u}, {0xe9d71b689dde71afu, 0xaab8f01e6e10b4a6u},
  {0x9226712162ab070du, 0xcab3961304ca70e8u}, {0xb6b00d69bb55c8d1u, 0x3d607b97c5fd0d22u},
  {0xe45c10c42a2b3b05u, 0x8cb89a7db77c506au}, {0x8eb98a7a9a5b04e3u, 0x77f3608e92adb242u},
  {0xb267ed1940f1c61cu, 0x55f038b237591ed3u}, {0xdf01e85f912e37a3u, 0x6b6c46dec52f6688u},
  {0x8b61313bbabce2c6u, 0x2323ac4b3b3da015u}, {0xae397d8aa96c1b77u, 0xabec975e0a0d081au},
  {0xd9c7dced53c72255u, 0x96e7bd358c904a21u}, {0x881cea14545c7575u, 0x7e50d64177da2e54u},
  {0xaa242499697392d2u, 0xdde50bd1d5d0b9e9u}, {0xd4ad2dbfc3d07787u, 0x955e4ec64b44e864u},
  {0x84ec3c97da624ab4u, 0xbd5af13bef0b113eu}, {0xa6274bbdd0fadd61u, 0xecb1ad8aeacdd58eu},
  {0xcfb11ead453994bau, 0x67de18eda5814af2u}, {0x81ceb32c4b43fcf4u, 0x80eacf948770ced7u},
  {0xa2425ff75e14fc31u, 0xa1258379a94d028du}, {0xcad2f7f5359a3b3eu, 0x096ee45813a04330u},
  {0xfd87b5f28300ca0du, 0x8bca9d6e188853fcu}, {0x9e74d1b791e07e48u, 0x775ea264cf55347eu},
  {0xc612062576589ddau, 0x95364afe032a819eu}, {0xf79687aed3eec551u, 0x3a83ddbd83f52205u},
  {0x9abe14cd44753b52u, 0xc4926a9672793543u}, {0xc16d9a0095928a27u, 0x75b7053c0f178294u},
  {0xf1c90080baf72cb1u, 0x5324c68b12dd6339u}, {0x971da05074da7beeu, 0xd3f6fc16ebca5e04u},
  {0xbce5086492111aeau, 0x88f4bb1ca6bcf585u}, {0xec1e4a7db69561a5u, 0x2b31e9e3d06c32e6u},
  {0x9392ee8e921d5d07u, 0x3aff322e62439fd0u}, {0xb877aa3236a4b449u, 0x09befeb9fad487c3u},
  {0xe69594bec44de15bu, 0x4c2ebe687989a9b4u}, {0x901d7cf73ab0acd9u, 0x0f9d37014bf60a11u},
  {0xb424dc35095cd80fu, 0x538484c19ef38c95u}, {0xe12e13424bb40e13u, 0x2865a5f206b06fbau},
  {0x8cbccc096f5088cbu, 0xf93f87b7442e45d4u}, {0xafebff0bcb24aafeu, 0xf78f69a51539d749u},
  {0xdbe6fecebdedd5beu, 0xb573440e5a884d1cu}, {0x89705f4136b4a597u, 0x31680a88f8953031u},
  {0xabcc77118461cefcu, 0xfdc20d2b36ba7c3eu}, {0xd6bf94d5e57a42bcu, 0x3d32907604691b4du},
  {0x8637bd05af6c69b5u, 0xa63f9a49c2c1b110u}, {0xa7c5ac471b478423u, 0x0fcf80dc33721d54u},
  {0xd1b71758e219652bu, 0xd3c36113404ea4a9u}, {0x83126e978d4fdf3bu, 0x645a1cac083126eau},
  {0xa3d70a3d70a3d70au, 0x3d70a3d70a3d70a4u}, {0xccccccccccccccccu, 0xcccccccccccccccdu},
  {0x8000000000000000u, 0x0000000000000000u}, {0xa000000000000000u, 0x0000000000000000u},
  {0xc800000000000000u, 0x0000000000000000u}, {0xfa00000000000000u, 0x0000000000000000u},
  {0x9c40000000000000u, 0x0000000000000000u}, {0xc350000000000000u, 0x0000000000000000u},
  {0xf424000000000000u, 0x0000000000000000u}, {0x9896800000000000u, 0x0000000000000000u},
  {0xbebc200000000000u, 0x0000000000000000u}, {0xee6b280000000000u, 0x0000000000000000u},
  {0x9502f90000000000u, 0x0000000000000000u}, {0xba43b74000000000u, 0x0000000000000000u},
  {0xe8d4a51000000000u, 0x0000000000000000u}, {0x9184e72a00000000u, 0x0000000000000000u},
  {0xb5e620f480000000u, 0x0000000000000000u}, {0xe35fa931a0000000u, 0x0000000000000000u},
  {0x8e1bc9bf04000000u, 0x0000000000000000u}, {0xb1a2bc2ec5000000u, 0x0000000000000000u},
  {0xde0b6b3a76400000u, 0x0000000000000000u}, {0x8ac7230489e80000u, 0x0000000000000000u},
  {0xad78ebc5ac620000u, 0x0000000000000000u}, {0xd8d726b7177a8000u, 0x0000000000000000u},
  {0x878678326eac9000u, 0x0000000000000000u}, {0xa968163f0a57b400u, 0x0000000000000000u},
  {0xd3c21bcecceda100u, 0x0000000000000000u}, {0x84595161401484a0u, 0x0000000000000000u},
  {0xa56fa5b99019a5c8u, 0x0000000000000000u}, {0xcecb8f27f4200f3au, 0x0000000000000000u},
  {0x813f3978f8940984u, 0x4000000000000000u}, {0xa18f07d736b90be5u, 0x5000000000000000u},
  {0xc9f2c9cd04674edeu, 0xa400000000000000u}, {0xfc6f7c4045812296u, 0x4d00000000000000u},
  {0x9dc5ada82b70b59du, 0xf020000000000000u}, {0xc5371912364ce305u, 0x6c28000000000000u},
  {0xf684df56c3e01bc6u, 0xc732000000000000u}, {0x9a130b963a6c115cu, 0x3c7f400000000000u},
  {0xc097ce7bc90715b3u, 0x4b9f100000000000u}, {0xf0bdc21abb48db20u, 0x1e86d40000000000u},
  {0x96769950b50d88f4u, 0x1314448000000000u}, {0xbc143fa4e250eb31u, 0x17d955a000000000u},
  {0xeb194f8e1ae525fdu, 0x5dcfab0800000000u}, {0x92efd1b8d0cf37beu, 0x5aa1cae500000000u},
  {0xb7abc627050305adu, 0xf14a3d9e40000000u}, {0xe596b7b0c643c719u, 0x6d9ccd05d0000000u},
  {0x8f7e32ce7bea5c6fu, 0xe4820023a2000000u}, {0xb35dbf821ae4f38bu, 0xdda2802c8a800000u},
  {0xe0352f62a19e306eu, 0xd50b2037ad200000u}, {0x8c213d9da502de45u, 0x4526f422cc340000u},
  {0xaf298d050e4395d6u, 0x9670b12b7f410000u}, {0xdaf3f04651d47b4cu, 0x3c0cdd765f114000u},
  {0x88d8762bf324cd0fu, 0xa5880a69fb6ac800u}, {0xab0e93b6efee0053u, 0x8eea0d047a457a00u},
  {0xd5d238a4abe98068u, 0x72a4904598d6d880u}, {0x85a36366eb71f041u, 0x47a6da2b7f864750u},
  {0xa70c3c40a64e6c51u, 0x999090b65f67d924u}, {0xd0cf4b50cfe20765u, 0xfff4b4e3f741cf6du},
  {0x82818f1281ed449fu, 0xbff8f10e7a8921a4u}, {0xa321f2d7226895c7u, 0xaff72d52192b6a0du},
  {0xcbea6f8ceb02bb39u, 0x9bf4f8a69f764490u}, {0xfee50b7025c36a08u, 0x02f236d04753d5b4u},
  {0x9f4f2726179a2245u, 0x01d762422c946590u}, {0xc722f0ef9d80aad6u, 0x424d3ad2b7b97ef5u},
  {0xf8ebad2b84e0d58bu, 0xd2e0898765a7deb2u}, {0x9b934c3b330c8577u, 0x63cc55f49f88eb2fu},
  {0xc2781f49ffcfa6d5u, 0x3cbf6b71c76b25fbu}, {0xf316271c7fc3908au, 0x8bef464e3945ef7au},
  {0x97edd871cfda3a56u, 0x97758bf0e3cbb5acu}, {0xbde94e8e43d0c8ecu, 0x3d52eeed1cbea317u},
  {0xed63a231d4c4fb27u, 0x4ca7aaa863ee4bddu}, {0x945e455f24fb1cf8u, 0x8fe8caa93e74ef6au},
  {0xb975d6b6ee39e436u, 0xb3e2fd538e122b44u}, {0xe7d34c64a9c85d44u, 0x60dbbca87196b616u},
  {0x90e40fbeea1d3a4au, 0xbc8955e946fe31cdu}, {0xb51d13aea4a488ddu, 0x6babab6398bdbe41u},
  {0xe264589a4dcdab14u, 0xc696963c7eed2dd1u}, {0x8d7eb76070a08aecu, 0xfc1e1de5cf543ca2u},
  {0xb0de65388cc8ada8u, 0x3b25a55f43294bcbu}, {0xdd15fe86affad912u, 0x49ef0eb713f39ebeu},
  {0x8a2dbf142dfcc7abu, 0x6e3569326c784337u}, {0xacb92ed9397bf996u, 0x49c2c37f07965404u},
  {0xd7e77a8f87daf7fbu, 0xdc33745ec97be906u}, {0x86f0ac99b4e8dafdu, 0x69a028bb3ded71a3u},
  {0xa8acd7c0222311bcu, 0xc40832ea0d68ce0cu}, {0xd2d80db02aabd62bu, 0xf50a3fa490c30190u},
  {0x83c7088e1aab65dbu, 0x792667c6da79e0fau}, {0xa4b8cab1a1563f52u, 0x577001b891185938u},
  {0xcde6fd5e09abcf26u, 0xed4c0226b55e6f86u}, {0x80b05e5ac60b6178u, 0x544f8158315b05b4u},
  {0xa0dc75f1778e39d6u, 0x696361ae3db1c721u}, {0xc913936dd571c84cu, 0x03bc3a19cd1e38e9u},
  {0xfb5878494ace3a5fu, 0x04ab48a04065c723u}, {0x9d174b2dcec0e47bu, 0x62eb0d64283f9c76u},
  {0xc45d1df942711d9au, 0x3ba5d0bd324f8394u}, {0xf5746577930d6500u, 0xca8f44ec7ee36479u},
  {0x9968bf6abbe85f20u, 0x7e998b13cf4e1ecbu}, {0xbfc2ef456ae276e8u, 0x9e3fedd8c321a67eu},
  {0xefb3ab16c59b14a2u, 0xc5cfe94ef3ea101eu}, {0x95d04aee3b80ece5u, 0xbba1f1d158724a12u},
  {0xbb445da9ca61281fu, 0x2a8a6e45ae8edc97u}, {0xea1575143cf97226u, 0xf52d09d71a3293bdu},
  {0x924d692ca61be758u, 0x593c2626705f9c56u}, {0xb6e0c377cfa2e12eu, 0x6f8b2fb00c77836cu},
  {0xe498f455c38b997au, 0x0b6dfb9c0f956447u}, {0x8edf98b59a373fecu, 0x4724bd4189bd5eacu},
  {0xb2977ee300c50fe7u, 0x58edec91ec2cb657u}, {0xdf3d5e9bc0f653e1u, 0x2f2967b66737e3edu},
  {0x8b865b215899f46cu, 0xbd79e0d20082ee74u}, {0xae67f1e9aec07187u, 0xecd8590680a3aa11u},
  {0xda01ee641a708de9u, 0xe80e6f4820cc9495u}, {0x884134fe908658b2u, 0x3109058d147fdcddu},
  {0xaa51823e34a7eedeu, 0xbd4b46f0599fd415u}, {0xd4e5e2cdc1d1ea96u, 0x6c9e18ac7007c91au},
  {0x850fadc09923329eu, 0x03e2cf6bc604ddb0u}, {0xa6539930bf6bff45u, 0x84db8346b786151cu},
  {0xcfe87f7cef46ff16u, 0xe612641865679a63u}, {0x81f14fae158c5f6eu, 0x4fcb7e8f3f60c07eu},
  {0xa26da3999aef7749u, 0xe3be5e330f38f09du}, {0xcb090c8001ab551cu, 0x5cadf5bfd3072cc5u},
  {0xfdcb4fa002162a63u, 0x73d9732fc7c8f7f6u}, {0x9e9f11c4014dda7eu, 0x2867e7fddcdd9afau},
  {0xc646d63501a1511du, 0xb281e1fd541501b8u}, {0xf7d88bc24209a565u, 0x1f225a7ca91a4226u},
  {0x9ae757596946075fu, 0x3375788de9b06958u}, {0xc1a12d2fc3978937u, 0x0052d6b1641c83aeu},
  {0xf209787bb47d6b84u, 0xc0678c5dbd23a49au}, {0x9745eb4d50ce6332u, 0xf840b7ba963646e0u},
  {0xbd176620a501fbffu, 0xb650e5a93bc3d898u}, {0xec5d3fa8ce427affu, 0xa3e51f138ab4cebeu},
  {0x93ba47c980e98cdfu, 0xc66f336c36b10137u}, {0xb8a8d9bbe123f017u, 0xb80b0047445d4184u},
  {0xe6d3102ad96cec1du, 0xa60dc059157491e5u}, {0x9043ea1ac7e41392u, 0x87c89837ad68db2fu},
  {0xb454e4a179dd1877u, 0x29babe4598c311fbu}, {0xe16a1dc9d8545e94u, 0xf4296dd6fef3d67au},
  {0x8ce2529e2734bb1du, 0x1899e4a65f58660cu}, {0xb01ae745b101e9e4u, 0x5ec05dcff72e7f8fu},
  {0xdc21a1171d42645du, 0x76707543f4fa1f73u}, {0x899504ae72497ebau, 0x6a06494a791c53a8u},
  {0xabfa45da0edbde69u, 0x0487db9d17636892u}, {0xd6f8d7509292d603u, 0x45a9d2845d3c42b6u},
  {0x865b86925b9bc5c2u, 0x0b8a2392ba45a9b2u}, {0xa7f26836f282b732u, 0x8e6cac7768d7141eu},
  {0xd1ef0244af2364ffu, 0x3207d795430cd926u}, {0x8335616aed761f1fu, 0x7f44e6bd49e807b8u},
  {0xa402b9c5a8d3a6e7u, 0x5f16206c9c6209a6u}, {0xcd036837130890a1u, 0x36dba887c37a8c0fu},
  {0x802221226be55a64u, 0xc2494954da2c9789u}, {0xa02aa96b06deb0fdu, 0xf2db9baa10b7bd6cu},
  {0xc83553c5c8965d3du, 0x6f92829494e5acc7u}, {0xfa42a8b73abbf48cu, 0xcb772339ba1f17f9u},
  {0x9c69a97284b578d7u, 0xff2a760414536efbu}, {0xc38413cf25e2d70du, 0xfef5138519684abau},
  {0xf46518c2ef5b8cd1u, 0x7eb258665fc25d69u}, {0x98bf2f79d5993802u, 0xef2f773ffbd97a61u},
  {0xbeeefb584aff8603u, 0xaafb550ffacfd8fau}, {0xeeaaba2e5dbf6784u, 0x95ba2a53f983cf38u},
  {0x952ab45cfa97a0b2u, 0xdd945a747bf26183u}, {0xba756174393d88dfu, 0x94f971119aeef9e4u},
  {0xe912b9d1478ceb17u, 0x7a37cd5601aab85du}, {0x91abb422ccb812eeu, 0xac62e055c10ab33au},
  {0xb616a12b7fe617aau, 0x577b986b314d6009u}, {0xe39c49765fdf9d94u, 0xed5a7e85fda0b80bu},
  {0x8e41ade9fbebc27du, 0x14588f13be847307u}, {0xb1d219647ae6b31cu, 0x596eb2d8ae258fc8u},
  {0xde469fbd99a05fe3u, 0x6fca5f8ed9aef3bbu}, {0x8aec23d680043beeu, 0x25de7bb9480d5854u},
  {0xada72ccc20054ae9u, 0xaf561aa79a10ae6au}, {0xd910f7ff28069da4u, 0x1b2ba1518094da04u},
  {0x87aa9aff79042286u, 0x90fb44d2f05d0842u}, {0xa99541bf57452b28u, 0x353a1607ac744a53u},
  {0xd3fa922f2d1675f2u, 0x42889b8997915ce8u}, {0x847c9b5d7c2e09b7u, 0x69956135febada11u},
  {0xa59bc234db398c25u, 0x43fab9837e699095u}, {0xcf02b2c21207ef2eu, 0x94f967e45e03f4bbu},
  {0x8161afb94b44f57du, 0x1d1be0eebac278f5u}, {0xa1ba1ba79e1632dcu, 0x6462d92a69731732u},
  {0xca28a291859bbf93u, 0x7d7b8f7503cfdcfeu}, {0xfcb2cb35e702af78u, 0x5cda735244c3d43eu},
  {0x9defbf01b061adabu, 0x3a0888136afa64a7u}, {0xc56baec21c7a1916u, 0x088aaa1845b8fdd0u},
  {0xf6c69a72a3989f5bu, 0x8aad549e57273d45u}, {0x9a3c2087a63f6399u, 0x36ac54e2f678864bu},
  {0xc0cb28a98fcf3c7fu, 0x84576a1bb416a7ddu}, {0xf0fdf2d3f3c30b9fu, 0x656d44a2a11c51d5u},
  {0x969eb7c47859e743u, 0x9f644ae5a4b1b325u}, {0xbc4665b596706114u, 0x873d5d9f0dde1feeu},
  {0xeb57ff22fc0c7959u, 0xa90cb506d155a7eau}, {0x9316ff75dd87cbd8u, 0x09a7f12442d588f2u},
  {0xb7dcbf5354e9beceu, 0x0c11ed6d538aeb2fu}, {0xe5d3ef282a242e81u, 0x8f1668c8a86da5fau},
  {0x8fa475791a569d10u, 0xf96e017d694487bcu}, {0xb38d92d760ec4455u, 0x37c981dcc395a9acu},
  {0xe070f78d3927556au, 0x85bbe253f47b1417u}, {0x8c469ab843b89562u, 0x93956d7478ccec8eu},
  {0xaf58416654a6babbu, 0x387ac8d1970027b2u}, {0xdb2e51bfe9d0696au, 0x06997b05fcc0319eu},
  {0x88fcf317f22241e2u, 0x441fece3bdf81f03u}, {0xab3c2fddeeaad25au, 0xd527e81cad7626c3u},
  {0xd60b3bd56a5586f1u, 0x8a71e223d8d3b074u}, {0x85c7056562757456u, 0xf6872d5667844e49u},
  {0xa738c6bebb12d16cu, 0xb428f8ac016561dbu}, {0xd106f86e69d785c7u, 0xe13336d701beba52u},
  {0x82a45b450226b39cu, 0xecc0024661173473u}, {0xa34d721642b06084u, 0x27f002d7f95d0190u},
  {0xcc20ce9bd35c78a5u, 0x31ec038df7b441f4u}, {0xff290242c83396ceu, 0x7e67047175a15271u},
  {0x9f79a169bd203e41u, 0x0f0062c6e984d386u}, {0xc75809c42c684dd1u, 0x52c07b78a3e60868u},
  {0xf92e0c3537826145u, 0xa7709a56ccdf8a82u}, {0x9bbcc7a142b17ccbu, 0x88a66076400bb691u},
  {0xc2abf989935ddbfeu, 0x6acff893d00ea435u}, {0xf356f7ebf83552feu, 0x0583f6b8c4124d43u},
  {0x98165af37b2153deu, 0xc3727a337a8b704au}, {0xbe1bf1b059e9a8d6u, 0x744f18c0592e4c5cu},
  {0xeda2ee1c7064130cu, 0x1162def06f79df73u}, {0x9485d4d1c63e8be7u, 0x8addcb5645ac2ba8u},
  {0xb9a74a0637ce2ee1u, 0x6d953e2bd7173692u}, {0xe8111c87c5c1ba99u, 0xc8fa8db6ccdd0437u},
  {0x910ab1d4db9914a0u, 0x1d9c9892400a22a2u}, {0xb54d5e4a127f59c8u, 0x2503beb6d00cab4bu},
  {0xe2a0b5dc971f303au, 0x2e44ae64840fd61du}, {0x8da471a9de737e24u, 0x5ceaecfed289e5d2u},
  {0xb10d8e1456105dadu, 0x7425a83e872c5f47u}, {0xdd50f1996b947518u, 0xd12f124e28f77719u},
  {0x8a5296ffe33cc92fu, 0x82bd6b70d99aaa6fu}, {0xace73cbfdc0bfb7bu, 0x636cc64d1001550bu},
  {0xd8210befd30efa5au, 0x3c47f7e05401aa4eu}, {0x8714a775e3e95c78u, 0x65acfaec34810a71u},
  {0xa8d9d1535ce3b396u, 0x7f1839a741a14d0du}, {0xd31045a8341ca07cu, 0x1ede48111209a050u},
  {0x83ea2b892091e44du, 0x934aed0aab460432u}, {0xa4e4b66b68b65d60u, 0xf81da84d5617853fu},
  {0xce1de40642e3f4b9u, 0x36251260ab9d668eu}, {0x80d2ae83e9ce78f3u, 0xc1d72b7c6b426019u},
  {0xa1075a24e4421730u, 0xb24cf65b8612f81fu}, {0xc94930ae1d529cfcu, 0xdee033f26797b627u},
  {0xfb9b7cd9a4a7443cu, 0x169840ef017da3b1u}, {0x9d412e0806e88aa5u, 0x8e1f289560ee864eu},
  {0xc491798a08a2ad4eu, 0xf1a6f2bab92a27e2u}, {0xf5b5d7ec8acb58a2u, 0xae10af696774b1dbu},
  {0x9991a6f3d6bf1765u, 0xacca6da1e0a8ef29u}, {0xbff610b0cc6edd3fu, 0x17fd090a58d32af3u},
  {0xeff394dcff8a948eu, 0xddfc4b4cef07f5b0u}, {0x95f83d0a1fb69cd9u, 0x4abdaf101564f98eu},
  {0xbb764c4ca7a4440fu, 0x9d6d1ad41abe37f1u}, {0xea53df5fd18d5513u, 0x84c86189216dc5edu},
  {0x92746b9be2f8552cu, 0x32fd3cf5b4e49bb4u}, {0xb7118682dbb66a77u, 0x3fbc8c33221dc2a1u},
  {0xe4d5e82392a40515u, 0x0fabaf3feaa5334au}, {0x8f05b1163ba6832du, 0x29cb4d87f2a7400eu},
  {0xb2c71d5bca9023f8u, 0x743e20e9ef511012u}, {0xdf78e4b2bd342cf6u, 0x914da9246b255416u},
  {0x8bab8eefb6409c1au, 0x1ad089b6c2f7548eu}, {0xae9672aba3d0c320u, 0xa184ac2473b529b1u},
  {0xda3c0f568cc4f3e8u, 0xc9e5d72d90a2741eu}, {0x8865899617fb1871u, 0x7e2fa67c7a658892u},
  {0xaa7eebfb9df9de8du, 0xddbb901b98feeab7u}, {0xd51ea6fa85785631u, 0x552a74227f3ea565u},
  {0x8533285c936b35deu, 0xd53a88958f87275fu}, {0xa67ff273b8460356u, 0x8a892abaf368f137u},
  {0xd01fef10a657842cu, 0x2d2b7569b0432d85u}, {0x8213f56a67f6b29bu, 0x9c3b29620e29fc73u},
  {0xa298f2c501f45f42u, 0x8349f3ba91b47b8fu}, {0xcb3f2f7642717713u, 0x241c70a936219a73u},
  {0xfe0efb53d30dd4d7u, 0xed238cd383aa0110u}, {0x9ec95d1463e8a506u, 0xf4363804324a40aau},
  {0xc67bb4597ce2ce48u, 0xb143c6053edcd0d5u}, {0xf81aa16fdc1b81dau, 0xdd94b7868e94050au},
  {0x9b10a4e5e9913128u, 0xca7cf2b4191c8326u}, {0xc1d4ce1f63f57d72u, 0xfd1c2f611f63a3f0u},
  {0xf24a01a73cf2dccfu, 0xbc633b39673c8cecu}, {0x976e41088617ca01u, 0xd5be0503e085d813u},
  {0xbd49d14aa79dbc82u, 0x4b2d8644d8a74e18u}, {0xec9c459d51852ba2u, 0xddf8e7d60ed1219eu},
  {0x93e1ab8252f33b45u, 0xcabb90e5c942b503u}, {0xb8da1662e7b00a17u, 0x3d6a751f3b936243u},
  {0xe7109bfba19c0c9du, 0x0cc512670a783ad4u}, {0x906a617d450187e2u, 0x27fb2b80668b24c5u},
  {0xb484f9dc9641e9dau, 0xb1f9f660802dedf6u}, {0xe1a63853bbd26451u, 0x5e7873f8a0396973u},
  {0x8d07e33455637eb2u, 0xdb0b487b6423e1e8u}, {0xb049dc016abc5e5fu, 0x91ce1a9a3d2cda62u},
  {0xdc5c5301c56b75f7u, 0x7641a140cc7810fbu}, {0x89b9b3e11b6329bau, 0xa9e904c87fcb0a9du},
  {0xac2820d9623bf429u, 0x546345fa9fbdcd44u}, {0xd732290fbacaf133u, 0xa97c177947ad4095u},
  {0x867f59a9d4bed6c0u, 0x49ed8eabcccc485du}, {0xa81f301449ee8c70u, 0x5c68f256bfff5a74u},
  {0xd226fc195c6a2f8cu, 0x73832eec6fff3111u}, {0x83585d8fd9c25db7u, 0xc831fd53c5ff7eabu},
  {0xa42e74f3d032f525u, 0xba3e7ca8b77f5e55u}, {0xcd3a1230c43fb26fu, 0x28ce1bd2e55f35ebu},
  {0x80444b5e7aa7cf85u, 0x7980d163cf5b81b3u}, {0xa0555e361951c366u, 0xd7e105bcc332621fu},
  {0xc86ab5c39fa63440u, 0x8dd9472bf3fefaa7u}, {0xfa856334878fc150u, 0xb14f98f6f0feb951u},
  {0x9c935e00d4b9d8d2u, 0x6ed1bf9a569f33d3u}, {0xc3b8358109e84f07u, 0x0a862f80ec4700c8u},
  {0xf4a642e14c6262c8u, 0xcd27bb612758c0fau}, {0x98e7e9cccfbd7dbdu, 0x8038d51cb897789cu},
  {0xbf21e44003acdd2cu, 0xe0470a63e6bd56c3u}, {0xeeea5d5004981478u, 0x1858ccfce06cac74u},
  {0x95527a5202df0ccbu, 0x0f37801e0c43ebc8u}, {0xbaa718e68396cffdu, 0xd30560258f54e6bau},
  {0xe950df20247c83fdu, 0x47c6b82ef32a2069u}, {0x91d28b7416cdd27eu, 0x4cdc331d57fa5441u},
  {0xb6472e511c81471du, 0xe0133fe4adf8e952u}, {0xe3d8f9e563a198e5u, 0x58180fddd97723a6u},
  {0x8e679c2f5e44ff8fu, 0x570f09eaa7ea7648u},
};

typedef struct {
  int mbits, min_fast, max_fast, max_disguised, smallest, largest, min_exp, min_even, max_even, inf_power;
} HeffFloatForm;

static const HeffFloatForm heff_f64_form = {52, -22, 22, 37, -342, 308, -1023, -4, 23, 0x7ff};
static const HeffFloatForm heff_f32_form = {23, -10, 10, 17, -65, 38, -127, -17, 10, 0xff};

static inline __attribute__((always_inline)) int64_t heff_lemire(const HeffFloatForm *f, int64_t q, uint64_t w) {
  if (w == 0 || q < f->smallest) return 0;
  if (q > f->largest) return (int64_t)f->inf_power << f->mbits;
  int lz = __builtin_clzll(w);
  w <<= lz;
  const uint64_t *p = heff_pow5_128[q + 342];
  uint64_t mask = UINT64_MAX >> (f->mbits + 3);
  unsigned __int128 first = (unsigned __int128)w * p[0];
  uint64_t hi = (uint64_t)(first >> 64), lo = (uint64_t)first;
  if ((hi & mask) == mask) {
    uint64_t second = (uint64_t)(((unsigned __int128)w * p[1]) >> 64);
    lo += second;
    if (second > lo) hi += 1;
  }
  if (lo == UINT64_MAX && !(q >= -27 && q <= 55)) return -1;
  int upper = (int)(hi >> 63), shift = upper + 64 - f->mbits - 3;
  uint64_t mantissa = hi >> shift;
  int32_t power2 = ((((int32_t)q * (152170 + 65536)) >> 16) + 63) + upper - lz - f->min_exp;
  if (power2 <= 0) {
    if (-power2 + 1 >= 64) return 0;
    mantissa >>= -power2 + 1;
    mantissa += mantissa & 1;
    mantissa >>= 1;
    return (int64_t)(mantissa | ((uint64_t)(mantissa >= (UINT64_C(1) << f->mbits)) << f->mbits));
  }
  if (lo <= 1 && q >= f->min_even && q <= f->max_even && (mantissa & 3) == 1 && (mantissa << shift) == hi)
    mantissa &= ~UINT64_C(1);
  mantissa += mantissa & 1;
  mantissa >>= 1;
  if (mantissa >= (UINT64_C(2) << f->mbits)) {
    mantissa = UINT64_C(1) << f->mbits;
    power2 += 1;
  }
  mantissa &= ~(UINT64_C(1) << f->mbits);
  if (power2 >= f->inf_power) return (int64_t)f->inf_power << f->mbits;
  return (int64_t)(mantissa | ((uint64_t)power2 << f->mbits));
}

static const double heff_pow10[23] = {
  1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
static const float heff_pow10f[11] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

static inline __attribute__((always_inline)) int heff_fast_float(const HeffFloatForm *f, int64_t q, uint64_t w, double *out) {
  if (q < f->min_fast || q > f->max_disguised || w > (UINT64_C(2) << f->mbits)) return 0;
  if (q > f->max_fast) {
    for (int64_t k = q - f->max_fast; k > 0; k--)
      if (__builtin_mul_overflow(w, (uint64_t)10, &w)) return 0;
    if (w > (UINT64_C(2) << f->mbits)) return 0;
    q = f->max_fast;
  }
  if (f->mbits == 23) {
    float v = (float)w;
    *out = q < 0 ? v / heff_pow10f[-q] : v * heff_pow10f[q];
  } else {
    double v = (double)w;
    *out = q < 0 ? v / heff_pow10[-q] : v * heff_pow10[q];
  }
  return 1;
}

static inline int heff_eight_digits(const char *p, uint64_t *out) {
  uint64_t v = heff_le64((const uint8_t *)p);
  if (((v + UINT64_C(0x4646464646464646)) | (v - UINT64_C(0x3030303030303030))) & UINT64_C(0x8080808080808080)) return 0;
  v -= UINT64_C(0x3030303030303030);
  v = v * 10 + (v >> 8);
  uint64_t v1 = (v & UINT64_C(0x000000ff000000ff)) * UINT64_C(0x000f424000000064);
  uint64_t v2 = ((v >> 16) & UINT64_C(0x000000ff000000ff)) * UINT64_C(0x0000271000000001);
  *out = (uint32_t)((v1 + v2) >> 32);
  return 1;
}

static double heff_float_of_text(const char *p, int64_t n, int bits) {
  char room[128];
  char *t = n < (int64_t)sizeof room ? room : malloc((size_t)n + 1);
  memcpy(t, p, (size_t)n);
  t[n] = 0;
  int was = errno;
  double d = bits == 32 ? (double)strtof(t, NULL) : strtod(t, NULL);
  errno = was;
  if (t != room) free(t);
  return d;
}

static inline __attribute__((always_inline)) double heff_decimal_as(const char *p, int64_t n, const HeffFloatForm *f) {
  int bits = f->mbits == 23 ? 32 : 64;
  int neg = p[0] == '-', digits = 0, many = 0;
  int64_t i = neg, q = 0;
  uint64_t w = 0;
  for (int frac = 0; frac < 2; frac++) {
    int wide = 1;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
      uint64_t eight;
      if (wide && digits > 0 && digits <= 11 && i + 8 <= n) {
        if (heff_eight_digits(p + i, &eight)) {
          w = w * 100000000 + eight;
          digits += 8;
          q -= 8 * frac;
          i += 8;
          continue;
        }
        wide = 0;
      }
      int d = p[i++] - '0';
      if (digits == 0 && d == 0) q -= frac;
      else if (digits < 19) { w = w * 10 + (uint64_t)d; digits++; q -= frac; }
      else { q += !frac; many |= d != 0; }
    }
    if (frac == 0 && i < n && p[i] == '.') i++;
    else break;
  }
  if (i < n) {
    int eneg = p[++i] == '-';
    if (p[i] == '-' || p[i] == '+') i++;
    int64_t e = 0;
    for (; i < n; i++)
      if (e < 0x10000000) e = e * 10 + (p[i] - '0');
    q += eneg ? -e : e;
  }
  double v;
  if (!many && heff_fast_float(f, q, w, &v)) return neg ? -v : v;
  int64_t r = heff_lemire(f, q, w);
  if (r >= 0 && many && heff_lemire(f, q, w + 1) != r) r = -1;
  if (r < 0) return heff_float_of_text(p, n, bits);
  if (bits == 32) {
    uint32_t u = (uint32_t)r | (uint32_t)neg << 31;
    float g;
    memcpy(&g, &u, 4);
    return (double)g;
  }
  uint64_t u = (uint64_t)r | (uint64_t)neg << 63;
  memcpy(&v, &u, 8);
  return v;
}

HEFF_FN double heff_float_of_decimal(const char *p, int64_t n, int bits) {
  if (!heff_is_decimal(p, n)) return NAN;
  return bits == 32 ? heff_decimal_as(p, n, &heff_f32_form) : heff_decimal_as(p, n, &heff_f64_form);
}

HEFF_FN V heff_floatShow(V a) {
  char buf[64];
  heff_show_float(FLT_OF(a), buf);
  heff_drop(a);
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

// (an integer's lowest 64 bits, as two's complement has them; borrowed:
// a word's inlined, a big one's the call)
HEFF_SLOW uint64_t heff_big_low64(V v) {
  Big *b = (Big *)v;
  uint64_t m = b->d[0];
  return b->neg ? (uint64_t)0 - m : m;
}

HEFF_FAST uint64_t heff_int_low64(V v) {
  if (HEFF_LIKELY(IS_INT(v))) return (uint64_t)INT_OF(v);
  return heff_big_low64(v);
}

// (an integer of 64 bits, unsigned: a word inlined, a big one the call)
HEFF_SLOW V heff_u64_big(uint64_t m) {
  Big *b = big_new(1);
  b->d[0] = m;
  return big_fin(b, 0, 1);
}

HEFF_FAST V heff_int_u64(uint64_t m) {
  if (HEFF_LIKELY(m < (uint64_t)INT_LIM)) return MK_INT((int64_t)m);
  return heff_u64_big(m);
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

// (the fewest digits that read back as the same `Float32`, as a literal
// at `Float32` reads them, rounded once, written as a float is: of those,
// the nearest it (of two as near, the one whose last digit is even); nine
// always do)
HEFF_FN V heff_float32Show(V a) {
  char buf[64];
  heff_show_float32(heff_f32_of(a), buf);
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
  for (int64_t i = 0; i < x->len; m++) i += heff_u8_decode(x->p + i, x->len - i, &rs[m]);
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

// (the runes' string: each its UTF-8 bytes, counted first by its value
// alone, then written; an ASCII rune written where it is)
HEFF_FN V heff_strOfRunes(V l) {
  int64_t n = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    int64_t r = INT_OF(heff_field(c, 0));
    n += !heff_rune_scalar(r) ? 3 : r < 0x80 ? 1 : r < 0x800 ? 2 : r < 0x10000 ? 3 : 4;
  }
  Str *s = heff_str_new(n);
  int64_t k = 0;
  for (V c = l; heff_tag_of(c) == TAG_Cons; c = heff_field(c, 1)) {
    int64_t r = INT_OF(heff_field(c, 0));
    if (r >= 0 && r < 0x80) s->s[k++] = (uint8_t)r;
    else k += heff_u8_encode(s->s + k, heff_rune_scalar(r) ? (uint32_t)r : 0xFFFD);
  }
  heff_drop(l);
  return (V)s;
}

// (the rune at `i`, and how many bytes it takes, `*w`; out of the
// string, U+FFFD of none: an ASCII byte read where it is called, any
// other by the decoder)
HEFF_FAST uint32_t heff_str_rune_at(Str *x, int64_t i, int64_t *w) {
  if (i < 0 || i >= x->len) { *w = 0; return 0xFFFD; }
  if (x->p[i] < 0x80) { *w = 1; return x->p[i]; }
  uint32_t u;
  *w = heff_u8_decode(x->p + i, x->len - i, &u);
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

// (the rune at `i` and its width, read once: the two of `strRuneAt` and
// `strRuneWidth`, a tuple)
HEFF_FAST HeffT2 heff_strRuneDecode_lent(V a, V i, int give) {
  int64_t w;
  uint32_t r = heff_str_rune_at((Str *)a, INT_OF(i), &w);
  HEFF_GIVEN(give, 0, a);
  return (HeffT2){{MK_INT((int64_t)r), MK_INT(w)}};
}
HEFF_FN HeffT2 heff_strRuneDecode(V a, V i) { return heff_strRuneDecode_lent(a, i, 1); }

HEFF_FN V heff_strRuneCount(V a) {
  Str *x = (Str *)a;
  int64_t n = 0;
  uint32_t u;
  for (int64_t i = 0; i < x->len; n++) i += heff_u8_decode(x->p + i, x->len - i, &u);
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
//
// An array grows the same way: `push`, `append`, `insertAt`, `reserve`
// write into the room after its elements where it is held only there,
// and where there is none it is grown, its room the next power of 2 (a
// block the system gave, by `realloc`) -- so an array built a push at a
// time is copied a logarithmic number of times, not once a push.  `pop`
// lowers the length, the room kept.  Held elsewhere too, it is copied first,
// with room to grow.

// (the most elements an array has room for: more than memory holds, and
// few enough that their bytes are counted without overflowing -- an
// array asked to be longer fails, as memory running out does, not a
// block too small for it written past)
#define HEFF_ARR_MAX ((int64_t)1 << 56)

// (an array of `n` elements, its room `n`)
HEFF_FN Arr *heff_arr_alloc(int64_t n, unsigned kind) {
  if (n > HEFF_ARR_MAX) heff_fail("out of memory");
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
// says: its header, one word, all of it looked at at once -- in
// threads read atomically, relaxed, as `heff_rc` reads a count, a plain
// load still; one shared, held only here, as `heff_alone` has it)
static inline int heff_arr_mine(Arr *r, unsigned kind) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#ifdef HEFF_THREADS
  uint64_t h = __atomic_load_n((uint64_t *)&r->h, __ATOMIC_RELAXED);
#else
  uint64_t h;
  memcpy(&h, &r->h, sizeof h);
#endif
  if (HEFF_LIKELY((h & ~((uint64_t)0xff00 << 48)) == (1u | (uint64_t)K_ARR << 32 | (uint64_t)kind << 48)))
    return 1;
#ifdef HEFF_THREADS
  if (heff_shared((uint32_t)h)) return HEFF_ARR_KIND(r) == kind && heff_unshare(&r->h);
  return heff_owned((uint32_t)h) && HEFF_ARR_KIND(r) == kind && heff_own_alone(&r->h);
#else
  return 0;
#endif
#else
  return HEFF_ARR_KIND(r) == kind && heff_alone(&r->h);
#endif
}

// (element `k` as a `V` counted: a flat one boxed)
HEFF_FN V heff_arr_get(Arr *r, int64_t k) {
  switch (HEFF_ARR_KIND(r)) {
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
  switch (HEFF_ARR_KIND(r)) {
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

// (whether the array keeps its elements as one built of `x` would: a
// `map` of it may write each element where it read it, of the same
// kind (heff.compile.arrayloops))
HEFF_FAST V heff_arrayKeepsAs_lent(V a, V x, int give) {
  V b = heff_bool(HEFF_ARR_KIND((Arr *)a) == heff_arr_kind_of(x));
  HEFF_GIVEN(give, 0, a);
  HEFF_GIVEN(give, 1, x);
  return b;
}
HEFF_FN V heff_arrayKeepsAs(V a, V x) { return heff_arrayKeepsAs_lent(a, x, 3); }

// (`r`'s elements from `from`, `n` of them, put at `at` of `s`, each
// counted: as `s` keeps them, if it keeps them otherwise)
HEFF_FN void heff_arr_move(Arr *s, int64_t at, Arr *r, int64_t from, int64_t n) {
  if (HEFF_ARR_KIND(s) == HEFF_ARR_KIND(r)) {
    if (HEFF_ARR_KIND(r) == HEFF_ARR_V)
      for (int64_t j = 0; j < n; j++) s->a[at + j] = heff_dup(r->a[from + j]);
    else memcpy((char *)s->a + at * heff_arr_width[HEFF_ARR_KIND(r)],
                (char *)r->a + from * heff_arr_width[HEFF_ARR_KIND(r)],
                (size_t)n * heff_arr_width[HEFF_ARR_KIND(r)]);
  } else
    for (int64_t j = 0; j < n; j++) heff_arr_put(s, at + j, heff_arr_get(r, from + j));
}

// (a copy of the array `a`, held elsewhere too: `a` counted down)
HEFF_FN __attribute__((noinline)) Arr *heff_arr_copy(V a) {
  Arr *r = (Arr *)a;
  Arr *s = heff_arr_alloc(r->n, HEFF_ARR_KIND(r));
  heff_arr_move(s, 0, r, 0, r->n);
  heff_drop(a);
  return s;
}

// (the bits of the room grown to, to hold `need` elements: the power of
// 2 at least `need`, at least 4)
HEFF_FN unsigned heff_arr_room(int64_t need) {
  if (need > HEFF_ARR_MAX) heff_fail("out of memory");
  unsigned e = 2;
  while (((int64_t)1 << e) < need) e++;
  return e;
}

// (an array held only here, its room made 2 to the `e`, at least its
// length: in place where the system gave both blocks (`realloc`), else
// moved to a block of the allocator's, its own given back)
HEFF_FN __attribute__((noinline)) Arr *heff_arr_regrow(Arr *r, unsigned e) {
  size_t w = heff_arr_width[HEFF_ARR_KIND(r)];
  size_t was = heff_arr_bytes(r), now = sizeof(Arr) + ((size_t)1 << e) * w;
  Arr *g;
  if ((was + 7) / 8 > SMALL_MAX / 8 && (now + 7) / 8 > SMALL_MAX / 8) {
    HEFF_COUNTED((now + 7) / 8);
    g = realloc(r, now);
    if (!g) heff_fail("out of memory");
  } else {
    g = heff_obj_alloc(now);
    memcpy(g, r, sizeof(Arr) + (size_t)r->n * w);
    heff_obj_free(r, was);
  }
  g->h.tag = (uint16_t)(HEFF_ARR_KIND(g) | e << HEFF_ARR_ROOM);
  return g;
}

// (a copy of the array `a`, held elsewhere too or kept otherwise than
// `kind` says, kept so, its room 2 to the `e`: `a` counted down)
HEFF_FN __attribute__((noinline)) Arr *heff_arr_fresh(V a, unsigned e, unsigned kind) {
  Arr *r = (Arr *)a;
  Arr *s = heff_arr_alloc((int64_t)1 << e, kind);
  s->n = r->n;
  s->h.tag = (uint16_t)(kind | e << HEFF_ARR_ROOM);
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
  V x = HEFF_ARR_KIND(r) == HEFF_ARR_V ? heff_dup(r->a[k]) : heff_arr_get(r, k);
  HEFF_GIVEN(give, 0, a);
  return x;
}
HEFF_FN V heff_arrayIndex(V a, V i) { return heff_arrayIndex_lent(a, i, 3); }

// (... not counted, of an array of `V`s -- an `Int`'s never flat: the
// element lent where the array is, which holds it)
HEFF_FAST V heff_arrayPeek(V a, V i) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) heff_arr_out();
  return r->a[INT_OF(i)];
}

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
    T x = HEFF_ARR_KIND(r) == K ? HEFF_ARR_AT(r, T)[k] : heff_peek_##S(r->a[k]);       \
    HEFF_GIVEN(give, 0, a);                                                    \
    return x;                                                                  \
  }                                                                            \
  HEFF_FAST T heff_arrayIndex_##S(V a, V i) { return heff_arrayIndex_##S##_lent(a, i, 3); } \
  HEFF_FAST T heff_arrayIndexInAny_##S##_lent(V a, V i, int give) {           \
    Arr *r = (Arr *)a;                                                         \
    int64_t k = INT_OF(i);                                                     \
    T x = HEFF_ARR_KIND(r) == K ? HEFF_ARR_AT(r, T)[k] : heff_peek_##S(r->a[k]);       \
    HEFF_GIVEN(give, 0, a);                                                    \
    return x;                                                                  \
  }                                                                            \
  HEFF_FAST T heff_arrayIndexInAny_##S(V a, V i) { return heff_arrayIndexInAny_##S##_lent(a, i, 3); }
// (... known in range, where a loop checked its index once
// (heff.compile.bounds), or a test found it so (heff.compile.tested): the
// index a word, at least 0, below the length -- a fixed-width number's,
// `arrayIndexInAny_i64` above, how the array keeps it still looked at)
HEFF_FAST V heff_arrayIndexIn_lent(V a, V i, int give) {
  Arr *r = (Arr *)a;
  int64_t k = INT_OF(i);
  V x = HEFF_ARR_KIND(r) == HEFF_ARR_V ? heff_dup(r->a[k]) : heff_arr_get(r, k);
  HEFF_GIVEN(give, 0, a);
  return x;
}
HEFF_FAST V heff_arrayIndexIn(V a, V i) { return heff_arrayIndexIn_lent(a, i, 3); }
HEFF_FAST V heff_arrayPeekIn(V a, V i) { return ((Arr *)a)->a[INT_OF(i)]; }
HEFF_FAST double heff_arrayIndexIn_f64_lent(V a, V i, int give) {
  double x = HEFF_ARR_AT((Arr *)a, double)[INT_OF(i)];
  HEFF_GIVEN(give, 0, a);
  return x;
}
HEFF_FAST double heff_arrayIndexIn_f64(V a, V i) { return heff_arrayIndexIn_f64_lent(a, i, 3); }
// (... of a fixed-width number, the array checked once to keep them
// flat (`heff_arrayKeeps_i64`): read as it keeps them, not looked at)
#define HEFF_ARR_INDEX_IN(S, T, K)                                             \
  HEFF_FAST T heff_arrayIndexIn_##S##_lent(V a, V i, int give) {               \
    T x = HEFF_ARR_AT((Arr *)a, T)[INT_OF(i)];                                 \
    HEFF_GIVEN(give, 0, a);                                                    \
    return x;                                                                  \
  }                                                                            \
  HEFF_FAST T heff_arrayIndexIn_##S(V a, V i) { return heff_arrayIndexIn_##S##_lent(a, i, 3); } \
  HEFF_FAST V heff_arrayKeeps_##S##_lent(V a, int give) {                      \
    V b = heff_bool(HEFF_ARR_KIND(((Arr *)a)) == K);                                   \
    HEFF_GIVEN(give, 0, a);                                                    \
    return b;                                                                  \
  }                                                                            \
  HEFF_FAST V heff_arrayKeeps_##S(V a) { return heff_arrayKeeps_##S##_lent(a, 1); }
#define HEFF_ARR_INDEXES(S, T, K) HEFF_ARR_INDEX(S, T, K) HEFF_ARR_INDEX_IN(S, T, K)
HEFF_ARR_INDEXES(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_INDEXES(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_INDEXES(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_INDEXES(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_INDEXES(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_INDEXES(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_INDEXES(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_INDEXES(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_INDEXES(f32, float, HEFF_ARR_F32)
// (... of `Float64`s: whether it keeps them flat, as one built of them
// does, if it has an element (heff.compile.arrayloops))
HEFF_FAST V heff_arrayKeeps_f64_lent(V a, int give) {
  V b = heff_bool(HEFF_ARR_KIND((Arr *)a) == HEFF_ARR_F64);
  HEFF_GIVEN(give, 0, a);
  return b;
}
HEFF_FAST V heff_arrayKeeps_f64(V a) { return heff_arrayKeeps_f64_lent(a, 1); }

// (`set` but where the array is held only here and keeps `x` as it is:
// out of range, the array as it is; held elsewhere too, a copy, `x` at
// `i` in it)
#ifdef HEFF_THREADS
// (a search, from what a cell holds, for the slot holding `want`: down
// through owned objects held by what holds them there alone, and by the
// log -- those the body counted up (`TOUCHED`), and, where `any` is,
// those it did not, `left` of them at most)
typedef struct {
  HeffStack s;
  V want, from;
  V *at;
  V parent;
  int64_t left;
  int any;
} HeffOwnSeek;

HEFF_FN void heff_own_seek(HeffOwnSeek *k, V *at) {
  V v = *at;
  if (k->at || !IS_OBJ(v) || !v) return;
  if (v == k->want) {
    k->at = at;
    k->parent = k->from;
    return;
  }
  uint32_t rc = heff_rc(OBJ(v));
  if (!heff_owned(rc)) return;
  if (rc & HEFF_TOUCHED_BIT) {
    if (HEFF_COUNT(rc) != 2) return;
  } else if (!k->any || HEFF_COUNT(rc) != 1 || k->left-- <= 0) return;
  heff_stack_push(&k->s, v);
}

// (whether the slot `at` is one of the array's elements)
static inline int heff_own_within(V *at, Arr *r) {
  return HEFF_ARR_KIND(r) == HEFF_ARR_V && at >= r->a && at < r->a + r->n;
}

// (whether `r` is what a cell the body read holds, or is held there by a
// way from it nothing else holds -- but the log, and the cell -- so that
// nothing the body or another has sees it written in place: the way
// counted, each object on it held once there, or twice, once by the log,
// the body holding none; what holds the array then itself none the body
// writes in place, nor the array what holds one: kept by the log, to
// put back)
HEFF_FN int heff_own_found(HeffOwnLog *g, Arr *r) {
  for (int any = 0; any < 2; any++)
    for (int64_t j = 0; j < g->nr; j++) {
      V v = g->roots[j].v;
      if (!IS_OBJ(v) || !v || !heff_owned(heff_rc(OBJ(v)))) continue;
      V *at = NULL;
      V parent = 0;
      if (v == (V)r) {
        if (any) continue;
      } else {
        if (HEFF_COUNT(heff_rc(OBJ(v))) != 2) continue;
        HeffOwnSeek k = {{0}, (V)r, 0, NULL, 0, 256, any};
        heff_stack_push(&k.s, v);
        while (k.s.n && !k.at) {
          V o = k.s.v[--k.s.n];
          k.from = o;
          HEFF_EACH_SLOT(o, heff_own_seek, &k);
        }
        heff_stack_free(&k.s);
        if (!k.at) continue;
        at = k.at;
        parent = k.parent;
      }
      for (int64_t i = 0; i < g->na; i++)
        if ((V)g->arrs[i].r == parent || (g->arrs[i].at && heff_own_within(g->arrs[i].at, r)))
          return 0;
      g->arrs = heff_own_room(g->arrs, g->na, &g->ca, sizeof(HeffOwnArr));
      g->arrs[g->na++] = (HeffOwnArr){r, at, j};
      return 1;
    }
  return 0;
}

// (whether the array, counted up by the body and held by it once, and
// once by what holds it in what a cell the body read holds, may be
// written in place at `k` (`heff_own_found`): what is there then kept
// by the log -- the reference, where the array keeps `V`s, else its
// bits -- to be put back where the cell is to hold what it held (the
// body failed, or did not write the cell))
HEFF_FN __attribute__((noinline)) int heff_own_set(Arr *r, int64_t k) {
  HeffOwnLog *g = heff_own_log;
  uint32_t rc = r->h.rc;
  if (!g || !(rc & HEFF_TOUCHED_BIT) || HEFF_COUNT(rc) != 3) return 0;
  int64_t j = 0;
  while (j < g->na && g->arrs[j].r != r) j++;
  if (j == g->na && !heff_own_found(g, r)) return 0;
  g->sets = heff_own_room(g->sets, g->ns, &g->cs, sizeof(HeffOwnSet));
  HeffOwnSet *e = &g->sets[g->ns++];
  e->r = r;
  e->i = k;
  e->old = 0;
  unsigned w = heff_arr_width[HEFF_ARR_KIND(r)];
  memcpy(&e->old, (char *)r->a + k * w, w);
  return 1;
}
#endif

HEFF_FN __attribute__((noinline)) V heff_arr_set_slow(V a, V i, V x) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) {
    heff_drop(i);
    heff_drop(x);
    return a;
  }
  int64_t k = INT_OF(i);
#ifdef HEFF_THREADS
  if (heff_owned(heff_rc(&r->h)) && heff_own_set(r, k)) {
    heff_arr_put(r, k, x);
    return a;
  }
#endif
  if (!heff_unique(a)) r = heff_arr_copy(a);
  if (HEFF_ARR_KIND(r) == HEFF_ARR_V) heff_drop(r->a[k]);
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

// A loop that threads an array through `set`, using it only so, checks
// it held only there once, where it is entered (`heff_arrayOwn_f64`):
// a copy where it is not, its elements kept as the loop's `set`s give
// them; held only there, it stays so through the loop, and each `set`
// writes in place, unchecked (`heff_arraySetOwned_f64`) -- the array the
// same each round, which C can then see.

// (a copy of the array, its elements kept as `kind` says: `a` counted
// down)
HEFF_FN __attribute__((noinline)) V heff_arr_own_slow(V a, unsigned kind) {
  Arr *r = (Arr *)a;
  Arr *s = heff_arr_alloc(r->n, kind);
  heff_arr_move(s, 0, r, 0, r->n);
  heff_drop(a);
  return (V)s;
}

// (the array held only here: it, or a copy of it)
HEFF_FAST V heff_arrayOwn(V a) {
  if (HEFF_LIKELY(heff_unique(a))) return a;
  return (V)heff_arr_copy(a);
}

// (`x` at `i` of an array held only here, known in range
// (heff.compile.bounds): written here where the array keeps `V`s, the
// way of each other kind only where it is another)
HEFF_FAST V heff_arraySetOwnedIn(V a, V i, V x) {
  Arr *r = (Arr *)a;
  int64_t k = INT_OF(i);
  if (HEFF_LIKELY(HEFF_ARR_KIND(r) == HEFF_ARR_V)) {
    V old = r->a[k];
    r->a[k] = x;
    heff_drop(old);
  } else heff_arr_put(r, k, x);
  return a;
}

// (... out of range, the array as it is)
HEFF_FAST V heff_arraySetOwned(V a, V i, V x) {
  Arr *r = (Arr *)a;
  if (!HEFF_LIKELY(heff_arr_in(r, i))) {
    heff_drop(i);
    heff_drop(x);
    return a;
  }
  return heff_arraySetOwnedIn(a, i, x);
}

// (... keeping its elements as `T`s, where the program knows them so)
#define HEFF_ARR_OWN(S, T, K)                                                  \
  HEFF_FAST V heff_arrayOwn_##S(V a) {                                         \
    if (HEFF_LIKELY(heff_arr_mine((Arr *)a, K))) return a;                     \
    return heff_arr_own_slow(a, K);                                            \
  }                                                                            \
  HEFF_FAST V heff_arraySetOwned_##S(V a, V i, T x) {                          \
    Arr *r = (Arr *)a;                                                         \
    if (HEFF_LIKELY(heff_arr_in(r, i))) HEFF_ARR_AT(r, T)[INT_OF(i)] = x;      \
    else heff_drop(i);                                                         \
    return a;                                                                  \
  }                                                                            \
  HEFF_FAST V heff_arraySetOwnedIn_##S(V a, V i, T x) {                        \
    HEFF_ARR_AT((Arr *)a, T)[INT_OF(i)] = x;                                   \
    return a;                                                                  \
  }
HEFF_ARR_OWN(f64, double, HEFF_ARR_F64)
HEFF_ARR_OWN(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_OWN(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_OWN(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_OWN(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_OWN(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_OWN(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_OWN(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_OWN(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_OWN(f32, float, HEFF_ARR_F32)

// (`n` elements, each `x`: of `V`s, counted once for each -- flat if
// `x` is a `Float64`)
HEFF_FN V heff_arrayReplicate(V n, V x) {
  int64_t m = heff_arr_count(n);
  if (m == 0) {
    heff_drop(x);
    return (V)heff_arr_new(0);
  }
  Arr *r = heff_arr_alloc(m, heff_arr_kind_of(x));
  if (HEFF_ARR_KIND(r) == HEFF_ARR_F64) {
    double d = heff_unbox_f64(x);
    for (int64_t k = 0; k < m; k++) HEFF_ARR_AT(r, double)[k] = d;
    return (V)r;
  }
  for (int64_t k = 0; k < m; k++) r->a[k] = x;
  if (IS_OBJ(x)) {
    uint32_t rc = heff_rc(OBJ(x));
    if (heff_mine(rc)) OBJ(x)->rc = rc + (uint32_t)(m - 1);
#ifdef HEFF_THREADS
    else if (heff_shared(rc)) __atomic_fetch_add(&OBJ(x)->rc, (uint32_t)(m - 1), __ATOMIC_RELAXED);
    else if (heff_owned(rc)) heff_own_dup(OBJ(x), (uint32_t)(m - 1));
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

// (... of a function known to give numbers of one type, `T`s
// (heff.compile.generate): flat, each unboxed as it comes)
#define HEFF_ARR_GENERATE(S, T, K)                                             \
  HEFF_FN V heff_arrayGenerate_##S(V n, V f) {                                 \
    int64_t m = heff_arr_count(n);                                             \
    Arr *r = heff_arr_alloc(m, K);                                             \
    for (int64_t k = 0; k < m; k++) {                                          \
      V x = heff_tr(heff_app1(heff_dup(f), MK_INT(k)));                        \
      HEFF_ARR_AT(r, T)[k] = heff_unbox_##S(x);                                \
    }                                                                          \
    heff_drop(f);                                                              \
    return (V)r;                                                               \
  }
HEFF_ARR_GENERATE(f64, double, HEFF_ARR_F64)
HEFF_ARR_GENERATE(i64, int64_t, HEFF_ARR_I64)
HEFF_ARR_GENERATE(u64, uint64_t, HEFF_ARR_U64)
HEFF_ARR_GENERATE(i32, int32_t, HEFF_ARR_I32)
HEFF_ARR_GENERATE(u32, uint32_t, HEFF_ARR_U32)
HEFF_ARR_GENERATE(i16, int16_t, HEFF_ARR_I16)
HEFF_ARR_GENERATE(u16, uint16_t, HEFF_ARR_U16)
HEFF_ARR_GENERATE(i8, int8_t, HEFF_ARR_I8)
HEFF_ARR_GENERATE(u8, uint8_t, HEFF_ARR_U8)
HEFF_ARR_GENERATE(f32, float, HEFF_ARR_F32)

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
  Arr *s = heff_arr_alloc(m, HEFF_ARR_KIND(r));
  heff_arr_move(s, 0, r, b, m);
  heff_drop(a);
  return (V)s;
}

// (one array's elements, then another's: kept as theirs are, or -- one
// flat and the other of `V`s -- flat, the other's put so.  The first held
// only here, the second's are put after its own, in its room, grown if
// there is too little; the second held only here too, its elements are
// moved over, not counted)
HEFF_FN V heff_arrayAppend(V a, V b) {
  Arr *r = (Arr *)a, *t = (Arr *)b;
  if (t->n == 0) { heff_drop(b); return a; }
  if (r->n == 0 && !(heff_alone(&r->h) && heff_arr_cap(r) >= t->n)) { heff_drop(a); return b; }
  unsigned kind = HEFF_ARR_KIND(r) != HEFF_ARR_V ? HEFF_ARR_KIND(r) : HEFF_ARR_KIND(t);
  int64_t k = r->n, n = r->n + t->n;
  Arr *s;
  if (heff_arr_mine(r, kind)) s = n > heff_arr_cap(r) ? heff_arr_regrow(r, heff_arr_room(n)) : r;
  else {
    s = heff_arr_alloc(n, kind);
    heff_arr_move(s, 0, r, 0, k);
    heff_drop(a);
  }
  if (HEFF_ARR_KIND(t) == kind && heff_alone(&t->h)) {
    size_t w = heff_arr_width[kind];
    memcpy((char *)s->a + (size_t)k * w, t->a, (size_t)t->n * w);
    heff_obj_free(t, heff_arr_bytes(t));
  } else {
    heff_arr_move(s, k, t, 0, t->n);
    heff_drop(b);
  }
  s->n = n;
  return (V)s;
}

// (a new array of `m`, kept as `kind` says: `a`'s elements before `k`,
// then, `put` places on, those from `k` and `skip` past it -- moved over
// by a copy of their bytes where nothing else holds `a`, its cell given
// back, and counted otherwise, `a` counted down)
HEFF_FN Arr *heff_arr_spread(V a, int64_t k, int64_t skip, int64_t put, int64_t m, unsigned kind) {
  Arr *r = (Arr *)a;
  int64_t rest = r->n - k - skip;
  Arr *s = heff_arr_alloc(m, kind);
  if (HEFF_ARR_KIND(r) == kind && heff_alone(&r->h)) {
    size_t w = heff_arr_width[kind];
    memcpy(s->a, r->a, (size_t)k * w);
    memcpy((char *)s->a + (size_t)(k + put) * w, (char *)r->a + (size_t)(k + skip) * w,
           (size_t)rest * w);
    heff_obj_free(r, heff_arr_bytes(r));
    return s;
  }
  heff_arr_move(s, 0, r, 0, k);
  heff_arr_move(s, k + put, r, k + skip, rest);
  heff_drop(a);
  return s;
}

// (`x` put in at `i`, clamped to 0 up to the length, the elements from
// there on one further on)
HEFF_FN V heff_arrayInsertAt(V a, V i, V x) {
  Arr *r = (Arr *)a;
  int64_t n = r->n, k = INT_OF(i);
  if (!IS_INT(i)) k = ((Big *)i)->neg ? 0 : n;
  else if (k < 0) k = 0;
  else if (k > n) k = n;
  heff_drop(i);
  unsigned kind = n == 0 ? heff_arr_kind_of(x) : HEFF_ARR_KIND(r);
  Arr *s;
  if (n < heff_arr_cap(r) && heff_arr_mine(r, kind)) {
    size_t w = heff_arr_width[kind];
    memmove((char *)r->a + (size_t)(k + 1) * w, (char *)r->a + (size_t)k * w, (size_t)(n - k) * w);
    r->n = n + 1;
    s = r;
  } else s = heff_arr_spread(a, k, 0, 1, n + 1, kind);
  heff_arr_put(s, k, x);
  return (V)s;
}

// (the element at `i` taken out, those after it one nearer: moved down
// in place where the array is held only here, its room kept, else a
// copy; out of range, the array as it is.  Room is kept as a power of 2,
// not as the length, which this lowers: so an array whose room is its
// length is given a power of 2 first, the one at least its length -- its
// own, if its length is one, else by `heff_arr_regrow`.  It never
// shrinks the room)
HEFF_FN __attribute__((noinline)) V heff_arr_remove_slow(V a, V i) {
  Arr *r = (Arr *)a;
  if (!heff_arr_in(r, i)) {
    heff_drop(i);
    return a;
  }
  int64_t n = r->n, k = INT_OF(i);
  if (!heff_alone(&r->h)) return (V)heff_arr_spread(a, k, 1, 0, n - 1, HEFF_ARR_KIND(r));
  if (HEFF_ARR_KIND(r) == HEFF_ARR_V) heff_drop(r->a[k]);
  if (!(r->h.tag >> HEFF_ARR_ROOM)) {
    unsigned e = heff_arr_room(n);
    if ((int64_t)1 << e == n) r->h.tag = (uint16_t)(HEFF_ARR_KIND(r) | e << HEFF_ARR_ROOM);
    else r = heff_arr_regrow(r, e);
  }
  size_t w = heff_arr_width[HEFF_ARR_KIND(r)];
  memmove((char *)r->a + (size_t)k * w, (char *)r->a + (size_t)(k + 1) * w, (size_t)(n - k - 1) * w);
  r->n = n - 1;
  return (V)r;
}

// (... the path where it is the last element of an array held only here
// whose room is a power of 2 already: the length lowered)
HEFF_FAST V heff_arrayRemoveAt(V a, V i) {
  Arr *r = (Arr *)a;
  if (r->n != 0 && i == MK_INT(r->n - 1) && (r->h.tag >> HEFF_ARR_ROOM) && heff_alone(&r->h)) {
    r->n--;
    if (HEFF_ARR_KIND(r) == HEFF_ARR_V) heff_drop(r->a[r->n]);
    return a;
  }
  return heff_arr_remove_slow(a, i);
}

// (`x` after the last element: in the room after them where the array
// is held only here, grown where there is none; held elsewhere too, a
// copy with room.  An array of no elements, of `V`s, is kept as `x`
// says, flat if it is a `Float64`)
HEFF_FN __attribute__((noinline)) V heff_arr_push_slow(V a, V x) {
  Arr *r = (Arr *)a;
  int64_t n = r->n;
  unsigned kind = n == 0 && HEFF_ARR_KIND(r) == HEFF_ARR_V ? heff_arr_kind_of(x) : HEFF_ARR_KIND(r);
  if (heff_arr_mine(r, kind)) {
    if (n == heff_arr_cap(r)) r = heff_arr_regrow(r, heff_arr_room(n + 1));
  } else r = heff_arr_fresh(a, heff_arr_room(n + 1), kind);
  heff_arr_put(r, n, x);
  r->n = n + 1;
  return (V)r;
}

// (... the path where there is room and the array, of `V`s, has an
// element -- so `x` is no `Float64`, which would make it flat)
HEFF_FAST V heff_arrayPush(V a, V x) {
  Arr *r = (Arr *)a;
  if (HEFF_LIKELY(r->n != 0 && r->n < heff_arr_cap(r) && heff_arr_mine(r, HEFF_ARR_V))) {
    r->a[r->n++] = x;
    return a;
  }
  return heff_arr_push_slow(a, x);
}

// (room for `k` more elements: grown in place where the array is held
// only here, else a copy with that room; fewer than none, none)
HEFF_FN V heff_arrayReserve(V a, V k) {
  Arr *r = (Arr *)a;
  int64_t more = heff_arr_count(k);
  heff_drop(k);
  int alone = heff_alone(&r->h);
  if (more == 0 || (alone && more <= heff_arr_cap(r) - r->n)) return a;
  int64_t need = r->n + more;
  if (alone) return (V)heff_arr_regrow(r, heff_arr_room(need));
  return (V)heff_arr_fresh(a, heff_arr_room(need), HEFF_ARR_KIND(r));
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

// (each that only reads its bytes with a `_lent` form, as a string's
// has: "Lent strings", above)
HEFF_FAST V heff_bytesLength_lent(V a, int give) { return heff_strLength_lent(a, give); }
HEFF_FN V heff_bytesLength(V a) { return heff_strLength(a); }

// (the byte at `i`; out of them, abandoned, as `bytes`'s `index` never
// asks)
HEFF_FAST V heff_bytesAt_lent(V a, V i, int give) {
  Str *x = (Str *)a;
  int64_t k = INT_OF(i);
  if (!IS_INT(i) || k < 0 || k >= x->len) return heff_abandon();
  V r = MK_INT(x->p[k]);
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_bytesAt(V a, V i) { return heff_bytesAt_lent(a, i, 1); }

HEFF_FN V heff_bytesSlice_lent(V a, V i, V j, int give) {
  V s = heff_strSlice_lent(a, i, j, give);
  OBJ(s)->kind = K_BYT;
  return s;
}
HEFF_FN V heff_bytesSlice(V a, V i, V j) { return heff_bytesSlice_lent(a, i, j, 1); }

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
    if (x->len) memcpy(s->s + k, x->p, (size_t)x->len);
    k += x->len;
  }
  heff_drop(l);
  return (V)s;
}

HEFF_FAST V heff_bytesEq_lent(V a, V b, int give) { return heff_strEq_lent(a, b, give); }
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
    V f[2] = { MK_INT(x->p[k]), l };
    l = heff_tag_n(TAG_Cons, 2, f);
  }
  heff_drop(a);
  return l;
}

// (a string's bytes, as they are)
HEFF_FN V heff_bytesFromString(V a) {
  Str *x = (Str *)a, *s = heff_bytes_new(x->len);
  if (x->len) memcpy(s->s, x->p, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

// (a string of them, as they are: `bytesFromString` undone)
HEFF_FN V heff_bytesAsString(V a) {
  Str *x = (Str *)a;
  V s = heff_str_of(x->p, x->len);
  heff_drop(a);
  return s;
}

// (their text: what is not UTF-8 in them U+FFFD, as TextDecoder reads it)
HEFF_FN V heff_bytesToString(V a) {
  Str *x = (Str *)a;
  V s = heff_str_valid(x->p, x->len);
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
HEFF_FAST V heff_bytesIntAt_lent(V a, V o, V n, V f, int give) {
  Str *x = (Str *)a;
  int64_t at = INT_OF(o), bits = INT_OF(n);
  if (!IS_INT(o) || !IS_INT(n) || at < 0 || bits < 0 || bits > 64 || at + bits > x->len * 8) return heff_abandon();
  V r = heff_bits_int(x->p, at, bits, INT_OF(f));
  HEFF_GIVEN(give, 0, a);
  return r;
}
HEFF_FN V heff_bytesIntAt(V a, V o, V n, V f) { return heff_bytesIntAt_lent(a, o, n, f, 1); }

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
HEFF_FAST V heff_bytesHasAt_lent(V a, V o, V b, int give) {
  Str *x = (Str *)a, *y = (Str *)b;
  int64_t at = INT_OF(o);
  int r = IS_INT(o) && at >= 0 && at <= x->len - y->len && (y->len == 0 || memcmp(x->p + at, y->p, (size_t)y->len) == 0);
  HEFF_GIVEN(give, 0, a);
  HEFF_GIVEN(give, 2, b);
  return heff_bool(r);
}
HEFF_FN V heff_bytesHasAt(V a, V o, V b) { return heff_bytesHasAt_lent(a, o, b, 5); }

// (where the bytes `n` are first in `a` from `i` on (from 0, `i` below
// it), or -1: a string's search, `heff_strFind`)
HEFF_FN V heff_bytesFind_lent(V n, V a, V i, int give) { return heff_strFind_lent(n, a, i, give); }
HEFF_FN V heff_bytesFind(V n, V a, V i) { return heff_strFind_lent(n, a, i, 3); }

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
  V r = heff_bits_int(x->p, at, bits, INT_OF(f));
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
    heff_bits_copy(s->s, k, x->p, 0, HEFF_BITS_LEN(x));
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
  heff_bits_copy(s->s, 0, x->p, at, m);
  heff_drop(a);
  return (V)s;
}

// (bytes as bits, and bits that are whole bytes as bytes (others,
// abandoned): the same buffer where it is the last reference)
HEFF_FN V heff_bitsOfBytes(V a) {
  Str *x = (Str *)a;
  if (heff_alone(&x->h)) { x->h.kind = K_BIT; x->h.tag = 0; return a; }
  Str *s = heff_bits_new(x->len * 8);
  if (x->len) memcpy(s->s, x->p, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

HEFF_FN V heff_bitsBytes(V a) {
  Str *x = (Str *)a;
  if (x->h.tag) return heff_abandon();
  if (heff_alone(&x->h)) { x->h.kind = K_BYT; return a; }
  Str *s = heff_bytes_new(x->len);
  if (x->len) memcpy(s->s, x->p, (size_t)x->len);
  heff_drop(a);
  return (V)s;
}

HEFF_FN V heff_bitsEq(V a, V b) {
  Str *x = (Str *)a, *y = (Str *)b;
  int r = x->len == y->len && x->h.tag == y->h.tag && (x->len == 0 || memcmp(x->p, y->p, (size_t)x->len) == 0);
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

// (the `Float64` nearest the decimal `strSlice s i j`, or the `Float32`
// nearest it, of `w` 64 or 32; of text that is no decimal, NaN)
HEFF_FAST V heff_floatOfDecimal_lent(V w, V s, V i, V j, int give) {
  Str *x = (Str *)s;
  int64_t m, b = heff_str_range(x, i, j, &m), bits = INT_OF(w);
  double d = bits == 64 || bits == 32 ? heff_float_of_decimal((const char *)x->p + b, m, (int)bits) : 0;
  HEFF_GIVEN(give, 1, s);
  if (bits != 64 && bits != 32) return heff_abandon();
  return heff_flt(d);
}
HEFF_FN V heff_floatOfDecimal(V w, V s, V i, V j) { return heff_floatOfDecimal_lent(w, s, i, j, 2); }

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
//   what was fetched -- is bytes, as they are: in by `heff_str_bytes`
//   (a file read to its end, `heff_str_read`), out by `heff_put` or
//   `heff_bytes`;
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
  if (s->len && memchr(s->p, 0, (size_t)s->len)) return NULL;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->p, (size_t)s->len);
  c[s->len] = 0;
  return c;
}

// (a name or text, as a C string to give the system: malloc'd, or NULL)
HEFF_FN char *heff_cstr_text(V v) {
  Str *s = (Str *)v;
  if (s->len && memchr(s->p, 0, (size_t)s->len)) return NULL;
  int own;
  heff_u8_valid_len(s->p, s->len, &own);
  if (own) return heff_cstr(v);
  Str *t = (Str *)heff_str_valid(s->p, s->len);
  char *c = heff_cstr((V)t);
  heff_drop((V)t);
  return c;
}

// (content out, malloc'd, `*n` bytes, a NUL after them)
HEFF_FN char *heff_bytes(V v, size_t *n) {
  Str *s = (Str *)v;
  char *c = heff_alloc((size_t)s->len + 1);
  if (s->len) memcpy(c, s->p, (size_t)s->len);
  c[s->len] = 0;
  *n = (size_t)s->len;
  return c;
}

// (content written out to `f`)
HEFF_FN void heff_put(FILE *f, V v) {
  Str *s = (Str *)v;
  if (s->len) fwrite(s->p, 1, (size_t)s->len, f);
}

// The working directory: each thread's own (#97)
// ---------------------------------------------------------------------
//
// The system has one working directory a process; the runtime keeps a
// directory for each thread (`heff_dir`): the process's own, asked for
// once, in the program's thread; its starter's, copied, in a thread a
// provider starts (`heff_dir_copy`, given to the started thread by
// `heff_dir_take`); another, by std.process's `setWorkingDir`, for
// that thread alone.  A provider gives the system every relative path
// under it (`heff_path_in_dir`), and never changes the process's --
// but in a command started, between its fork and its exec
// (`heff_exec_in_dir`).
HEFF_VAR(HEFF_TLS char *heff_dir);

#ifdef _WIN32
#define HEFF_WINDOWS 1
#else
#define HEFF_WINDOWS 0
#endif

static inline int heff_is_sep(uint8_t c, int windows) {
  return c == '/' || (windows && c == '\\');
}

static inline size_t heff_drive_len(const uint8_t *p, size_t n) {
  if (n >= 2 && p[1] == ':' && ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z')) return 2;
  if (n >= 3 && heff_is_sep(p[0], 1) && heff_is_sep(p[1], 1) && !heff_is_sep(p[2], 1)) {
    size_t i = 2;
    while (i < n && !heff_is_sep(p[i], 1)) i++;
    if (i == n) return 0;
    i++;
    while (i < n && !heff_is_sep(p[i], 1)) i++;
    return i;
  }
  return 0;
}

static inline size_t heff_path_under(const uint8_t *dir, size_t dn, const uint8_t *path, size_t pn, int windows, uint8_t *out) {
  if (pn == 0 || dn == 0 || (windows && heff_drive_len(path, pn) > 0) || (!windows && path[0] == '/')) {
    memcpy(out, path, pn);
    return pn;
  }
  if (windows && heff_is_sep(path[0], 1)) {
    size_t k = heff_drive_len(dir, dn);
    memcpy(out, dir, k);
    memcpy(out + k, path, pn);
    return k + pn;
  }
  memcpy(out, dir, dn);
  size_t n = dn;
  if (!heff_is_sep(dir[dn - 1], windows)) out[n++] = windows ? '\\' : '/';
  memcpy(out + n, path, pn);
  return n + pn;
}

// (`s` copied, malloc'd)
HEFF_FN char *heff_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *c = heff_alloc(n);
  memcpy(c, s, n);
  return c;
}

// (this thread's working directory)
HEFF_FN const char *heff_working_dir(void) {
  if (!heff_dir) {
    char buf[4096];
#ifdef _WIN32
    const char *d = _getcwd(buf, sizeof buf);
#else
    const char *d = getcwd(buf, sizeof buf);
#endif
    heff_dir = heff_strdup(d ? d : ".");
  }
  return heff_dir;
}

// (this thread's working directory, copied for a thread it starts)
HEFF_FN char *heff_dir_copy(void) {
  return heff_strdup(heff_working_dir());
}

// (`dir`, malloc'd, made this thread's working directory: its now)
HEFF_FN void heff_dir_take(char *dir) {
  free(heff_dir);
  heff_dir = dir;
}

// (`path`, malloc'd, as the system is to be given it from this thread:
// under the thread's directory where it is relative; `path` freed, the
// path given back malloc'd -- NULL stays NULL)
HEFF_FN char *heff_path_in_dir(char *path) {
  if (!path) return NULL;
  const char *dir = heff_working_dir();
  size_t dn = strlen(dir), pn = strlen(path);
  char *out = heff_alloc(dn + pn + 2);
  size_t n = heff_path_under((const uint8_t *)dir, dn, (const uint8_t *)path, pn, HEFF_WINDOWS, (uint8_t *)out);
  out[n] = 0;
  free(path);
  return out;
}

// (`dir`, malloc'd, from this thread, made the thread's working
// directory, as the system names it (its links resolved): 1; or 0
// where it is no directory there, the thread's as it was.  `dir`
// consumed)
HEFF_FN int heff_working_dir_set(char *dir) {
  char *p = heff_path_in_dir(dir);
  struct stat st;
  int ok = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
  if (!ok) { free(p); return 0; }
#ifdef _WIN32
  char *r = _fullpath(NULL, p, 0);
#else
  char *r = realpath(p, NULL);
#endif
  if (r) { free(p); p = r; }
  heff_dir_take(p);
  return 1;
}

#ifndef _WIN32
// (the command `as` -- its name found on the PATH -- forked and exec'd
// in this thread's directory, its standard streams `in`, `out` and
// `err` where each is not -1, the descriptors `closed` closed in it,
// its environment `env` where not NULL: its pid, or -1 where it could
// not be run -- not found, not a program, the directory gone -- `errno`
// why, as the child says through a pipe its exec closes)
HEFF_FN pid_t heff_exec_in_dir(char **as, char **env, int in, int out, int err, const int *closed, int nclosed) {
  int px[2];
  if (pipe(px) != 0) return -1;
  fcntl(px[0], F_SETFD, FD_CLOEXEC);
  fcntl(px[1], F_SETFD, FD_CLOEXEC);
  const char *dir = heff_working_dir();
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    // (the child: nothing but what is safe after a fork)
    int e;
    if ((in >= 0 && dup2(in, 0) < 0) || (out >= 0 && dup2(out, 1) < 0) || (err >= 0 && dup2(err, 2) < 0)) goto failed;
    for (int i = 0; i < nclosed; i++) close(closed[i]);
    if (chdir(dir) != 0) goto failed;
    if (env) environ = env;
    execvp(as[0], as);
  failed:
    e = errno;
    (void)!write(px[1], &e, sizeof e);
    _exit(127);
  }
  close(px[1]);
  int e = 0;
  if (pid > 0) {
    ssize_t got;
    while ((got = read(px[0], &e, sizeof e)) < 0 && errno == EINTR) {}
    if (got > 0) {
      int st;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
      pid = -1;
    }
  } else e = errno;
  close(px[0]);
  if (pid < 0) errno = e;
  return pid;
}
#endif

// `Bytes` cross by these alone, as they are: in, `n` bytes copied, by
// `heff_bytes_in`; out, read where they are kept, `*n` of them, by
// `heff_bytes_data` (the value not consumed, its bytes there as long as
// it is held, and never written to).  Or made where they are to be,
// not copied: `heff_bytes_room` makes `n` of them, `*at` where they go,
// written there -- by a read, the kernel's -- before the value is given
// on, and `heff_bytes_cut` keeps the first `m` alone, `m` at most `n`,
// while the value is still the maker's alone.
HEFF_FN V heff_bytes_in(const void *c, int64_t n) {
  Str *s = heff_bytes_new(n);
  if (n) memcpy(s->s, c, (size_t)n);
  return (V)s;
}

HEFF_FN V heff_bytes_room(int64_t n, uint8_t **at) {
  Str *s = heff_bytes_new(n);
  *at = s->s;
  return (V)s;
}

HEFF_FN V heff_bytes_cut(V v, int64_t m) {
  ((Str *)v)->len = m;
  return v;
}

HEFF_FN const uint8_t *heff_bytes_data(V v, int64_t *n) {
  Str *s = (Str *)v;
  *n = s->len;
  return s->p;
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

// Foreign values: a thing of the host's that the program holds as a
// value -- a `FILE *`, a library's handle -- made by a provider of a
// pointer and what lets it go (`heff_foreign`), the pointer read back
// where an operation is given the value (`heff_foreign_ptr`); the
// program drops the last reference, and the thing is let go of then, at
// once (Perceus: never while it is referenced).  Its type in the program
// is whatever the operation says it is -- a `data` of no constructors --
// and the provider vouches for it at the boundary; the compiler's
// generated glue makes and reads them as an effect's module describes
// its operations in C (the `ffi` module).  One effect's provider takes
// what another's handed out, the value given it by the program, as a
// `with` of the one hands its thing to an operation of the other.
HEFF_FN V heff_foreign(void *p, void (*let_go)(void *)) {
  Fgn *g = heff_obj_alloc(sizeof(Fgn));
  g->h.rc = 1; g->h.kind = K_FGN; g->h.tag = 0; g->p = p; g->let_go = let_go;
  g->end = NULL; g->owner = 0; g->at = 0; g->ended = 0;
  return (V)g;
}

// A thing that can be ended before it is let go of -- a socket closed,
// a command killed, while another thread may hold it still -- is made
// with what ends it, `end`, besides what lets it go: `end` ends it, and
// says why it failed, or 0; `let_go` frees what is left.  A custodian
// ends it at its shutdown (the custodians' part of the runtime), the
// struct kept for those that hold it, each of whose operations finds it
// ended; else it is ended as the program drops its last reference, just
// before it is let go of.  `end` is asked once, at most, by the runtime;
// the provider's own operations may end it before, so `end` finds it
// ended, and says 0.  A thing that fails to end there is a failure of
// its custodian's (`heff_foreign_failed`), or, of the root's, the
// program's: said, and the program ended.
HEFF_FN V heff_foreign_ending(void *p, void (*let_go)(void *), const char *(*end)(void *)) {
  V v = heff_foreign(p, let_go);
  ((Fgn *)v)->end = end;
  return v;
}

// (the custodians' part, where a program has it: a thing taken out of
// its custodian's custody, as it is let go of; and a failure of its
// custodian's, `owner`, why said)
HEFF_VAR(void (*heff_foreign_disowned)(struct Fgn *g));
HEFF_VAR(void (*heff_foreign_failed)(V owner, const char *why));

// (a failure of the root's custodian: the program's, said, and the
// program ended)
HEFF_FN void heff_root_failed(const char *why) {
  fflush(stdout);
  fprintf(stderr, "heff: %s\n", why);
  heff_fail_exit(1);
}

// (a foreign value let go of: taken out of its custodian's custody,
// ended where it is not already, then let go of; where it fails to end,
// its custodian's failure)
static void heff_foreign_gone(Fgn *g) {
  if (g->owner && heff_foreign_disowned) heff_foreign_disowned(g);
  const char *why = g->end && !g->ended ? g->end(g->p) : NULL;
  char *said = why ? heff_strdup(why) : NULL;
  if (g->let_go) g->let_go(g->p);
  if (!said) return;
  if (g->owner && heff_foreign_failed) heff_foreign_failed(g->owner, said);
  else heff_root_failed(said);
  free(said);
}

HEFF_FN void *heff_foreign_ptr(V v) {
  if (!IS_OBJ(v) || OBJ(v)->kind != K_FGN) heff_fail("not a foreign value");
  return ((Fgn *)v)->p;
}

// The custodian current.  Where a program's world has the custodian's
// effect, its handler's monad reads the custodian current (heff's
// entry, as Reader does), and tells it here just before each operation
// (`heff_custodyIn`): a `Maybe Custodian`, held until the next, for the
// provider performing it to read.  A computation an operation carries
// to run later -- a custodian's `stop`, a `with`'s release -- is of
// that monad, and the provider takes it given the custodian current
// where it was asked (`heff_carried`, `heff_carried_fn`).  In a world
// without custodians nothing is told, and a computation is taken as it
// is.
HEFF_VAR(HEFF_TLS V heff_custody_env);

static inline V heff_custodyIn(V c) {
  V old = heff_custody_env;
  heff_custody_env = c;
  if (old) heff_drop(old);
  return NTAG(TAG_MkUnit);
}

// (a computation an operation carries, `m a`, consumed: as the provider
// runs it, given the custodian current where the operation was asked)
static inline V heff_carried(V m) {
  return heff_custody_env ? heff_answer(m, heff_dup(heff_custody_env)) : m;
}

static V heff_carried_then(V self, V *a) {
  V *e = heff_env(self);
  V f = heff_dup(e[0]), c = heff_dup(e[1]);
  heff_drop(self);
  return heff_answer(heff_answer(f, a[0]), c);
}

// (... of a function to one, `a -> m b`, consumed: what it comes to
// given the custodian current where the operation was asked)
static inline V heff_carried_fn(V f) {
  if (!heff_custody_env) return f;
  V e[2] = { f, heff_dup(heff_custody_env) };
  return heff_clo(heff_carried_then, 1, 2, e);
}

// (an errno's name, the same on every system where its number is not)
HEFF_FN const char *heff_errno_name(int e) {
  switch (e) {
#define HEFF_E(x) case x: return #x;
  HEFF_E(EACCES) HEFF_E(EAGAIN) HEFF_E(EBADF) HEFF_E(EBUSY) HEFF_E(EEXIST) HEFF_E(EFBIG)
  HEFF_E(EINTR) HEFF_E(EINVAL) HEFF_E(EIO) HEFF_E(EISDIR) HEFF_E(EMFILE)
  HEFF_E(ENAMETOOLONG) HEFF_E(ENFILE) HEFF_E(ENODEV) HEFF_E(ENOENT) HEFF_E(ENOMEM)
  HEFF_E(ENOSPC) HEFF_E(ENOSYS) HEFF_E(ENOTDIR) HEFF_E(ENOTEMPTY) HEFF_E(ENXIO)
  HEFF_E(EPERM) HEFF_E(EPIPE) HEFF_E(EROFS) HEFF_E(ESPIPE) HEFF_E(EXDEV)
#ifdef ELOOP
  HEFF_E(ELOOP)
#endif
#ifdef ETXTBSY
  HEFF_E(ETXTBSY)
#endif
#ifdef EOVERFLOW
  HEFF_E(EOVERFLOW)
#endif
#ifdef EDQUOT
  HEFF_E(EDQUOT)
#endif
#if EWOULDBLOCK != EAGAIN
  HEFF_E(EWOULDBLOCK)
#endif
#undef HEFF_E
  default: return "E?";
  }
}

// (why a call of the system failed, as the program has it: a record of
// the errno `err`, its name and what the system says of it -- its labels
// `code`, `name`, `message`, in `shape`, the caller's, as the emitter
// numbers them)
HEFF_FN V heff_errno(int err, uint32_t *shape) {
  const char *name = heff_errno_name(err);
  char other[24];
  if (name[1] == '?') {
    snprintf(other, sizeof other, "errno %d", err);
    name = other;
  }
  V f[3] = { heff_int(err), heff_str_text(name), heff_str_text(strerror(err)) };
  return heff_rec(3, shape, f);
}

// A stack that overflows
// ---------------------------------------------------------------------
//
// heff's recursion is the C stack's, so a recursion deep enough runs
// out of it: a fault in the guard below the stack, where the system
// would end the process, saying nothing.  Instead each thread heff runs
// in -- the program's own, and each a provider starts by
// `heff_thread_start` -- is watched (`heff_watch_stack`): the bottom of
// its stack known, and a stack of its own for the handler of faults
// (`sigaltstack`).  A fault near the bottom of the stack is an
// overflow; any other is the system's, as without the handler.
//
// The handler does not jump out itself: it has the thread go on at
// `heff_overflowed`, on a stack of the thread's kept for it, once the
// system has put back the signal's mask and taken the thread off the
// handler's stack (macOS's `_longjmp` out of a handler would leave the
// system thinking it still on it, and the next fault unhandled).  There
// it fails as `heff_fail` does: the program says `heff: stack overflow`
// and exits with 2.  Where the fault was in code not the program's --
// the system's `malloc`, a lock's -- what that code holds may be held
// for good, so it ends by `_exit`: nothing flushed, nothing put back.
//
// Not on Windows, nor where a sanitizer handles faults itself; and said,
// never caught, but on x86-64 and arm64.
#define HEFF_OVERFLOW "stack overflow"
#define HEFF_OVERFLOW_STATUS 2

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define HEFF_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HEFF_SANITIZED 1
#endif
#if !defined(HEFF_SANITIZED) && (defined(__APPLE__) || defined(__linux__))
#define HEFF_WATCHED 1
#endif

#ifdef HEFF_WATCHED
#include <sys/ucontext.h>
#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#else
extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
// (the program's code, from its start to the end of its text, as the
// linker marks them)
extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));
#endif

// (the guard below a stack heff starts, and how far below its bottom a
// fault is taken for an overflow; the handler's stack, and the one the
// thread goes on on)
#define HEFF_GUARD ((size_t)1 << 20)
#define HEFF_SIG_STACK ((size_t)1 << 16)

// (where the fault was, and the thread made to go on at `f`, its stack
// pointer `sp`, as if `f` were called -- by the system's view of the
// thread's registers, `uc`)
#if defined(__APPLE__) && defined(__aarch64__)
#define HEFF_PC(uc) ((char *)__darwin_arm_thread_state64_get_pc((uc)->uc_mcontext->__ss))
#define HEFF_GO_ON(uc, f, sp) do { \
    __darwin_arm_thread_state64_set_pc_fptr((uc)->uc_mcontext->__ss, f); \
    __darwin_arm_thread_state64_set_sp((uc)->uc_mcontext->__ss, sp); } while (0)
#elif defined(__APPLE__) && defined(__x86_64__)
#define HEFF_PC(uc) ((char *)(uc)->uc_mcontext->__ss.__rip)
#define HEFF_GO_ON(uc, f, sp) do { \
    (uc)->uc_mcontext->__ss.__rip = (uintptr_t)(f); \
    (uc)->uc_mcontext->__ss.__rsp = (uintptr_t)(sp) - 8; } while (0)
#elif defined(__linux__) && defined(__aarch64__)
#define HEFF_PC(uc) ((char *)(uc)->uc_mcontext.pc)
#define HEFF_GO_ON(uc, f, sp) do { \
    (uc)->uc_mcontext.pc = (uintptr_t)(f); \
    (uc)->uc_mcontext.sp = (uintptr_t)(sp); } while (0)
#elif defined(__linux__) && defined(__x86_64__)
// (`REG_RIP` and `REG_RSP`, 16 and 15, named only where `_GNU_SOURCE` is)
#define HEFF_PC(uc) ((char *)(uc)->uc_mcontext.gregs[16])
#define HEFF_GO_ON(uc, f, sp) do { \
    (uc)->uc_mcontext.gregs[16] = (greg_t)(uintptr_t)(f); \
    (uc)->uc_mcontext.gregs[15] = (greg_t)((uintptr_t)(sp) - 8); } while (0)
#endif

// (the program's code: where a fault in it may be recovered from)
HEFF_VAR(char *heff_code_lo);
HEFF_VAR(char *heff_code_hi);
// (this thread's: the bottom of its stack, where it may go no lower; the
// handler's stack, and above it the one to go on on; and whether the
// fault was in code not the program's)
HEFF_VAR(HEFF_TLS char *heff_stack_lo);
HEFF_VAR(HEFF_TLS char *heff_sig_stack);
HEFF_VAR(HEFF_TLS int heff_overflow_lost);

// (where the thread goes on after its stack overflowed: failed, as
// `heff_fail` fails, or the program ended)
HEFF_FN __attribute__((noreturn)) void heff_overflowed(void) {
  static const char said[] = "heff: " HEFF_OVERFLOW "\n";
  if (!heff_overflow_lost) fflush(stdout);
  if (write(2, said, sizeof said - 1) < 0 || heff_overflow_lost) _exit(HEFF_OVERFLOW_STATUS);
  quick_exit(HEFF_OVERFLOW_STATUS);
}

HEFF_FN void heff_on_fault(int sig, siginfo_t *si, void *ctx) {
  char *a = (char *)si->si_addr, *lo = heff_stack_lo;
  if (!lo || a < lo - HEFF_GUARD || a >= lo + HEFF_GUARD) {
    signal(sig, SIG_DFL);
    return;
  }
#ifdef HEFF_GO_ON
  ucontext_t *uc = ctx;
  char *pc = HEFF_PC(uc);
  heff_overflow_lost = !(pc >= heff_code_lo && pc < heff_code_hi);
  uintptr_t sp = (uintptr_t)(heff_sig_stack + 2 * HEFF_SIG_STACK) & ~(uintptr_t)15;
  HEFF_GO_ON(uc, heff_overflowed, sp);
#else
  (void)ctx;
  heff_overflow_lost = 1;
  heff_overflowed();
#endif
}

// (the handler installed, for every thread, before any is watched)
HEFF_FN void heff_watch_faults(void) {
#ifdef __APPLE__
  unsigned long n = 0;
  heff_code_lo = (char *)getsegmentdata(&_mh_execute_header, "__TEXT", &n);
  heff_code_hi = heff_code_lo ? heff_code_lo + n : NULL;
#else
  heff_code_lo = __executable_start;
  heff_code_hi = etext;
#endif
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = heff_on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}

// (this thread watched: its stack's bottom, and the handler's stack --
// each of its own that the handler reads touched here, not first in the
// handler, which may not allocate)
HEFF_FN void heff_watch_stack(void) {
  char *m = malloc(2 * HEFF_SIG_STACK);
  if (!m) return;
  stack_t ss;
  ss.ss_sp = m;
  ss.ss_size = HEFF_SIG_STACK;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) != 0) { free(m); return; }
  heff_sig_stack = m;
  heff_overflow_lost = 0;
  pthread_t me = pthread_self();
#ifdef __APPLE__
  heff_stack_lo = (char *)pthread_get_stackaddr_np(me) - pthread_get_stacksize_np(me);
#else
  pthread_attr_t attr;
  void *at;
  size_t n;
  if (pthread_getattr_np(me, &attr) == 0) {
    if (pthread_attr_getstack(&attr, &at, &n) == 0) heff_stack_lo = at;
    pthread_attr_destroy(&attr);
  }
#endif
}

// (as the thread ends: no longer watched)
HEFF_FN void heff_unwatch_stack(void) {
  if (!heff_sig_stack) return;
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);
  heff_stack_lo = NULL;
  free(heff_sig_stack);
  heff_sig_stack = NULL;
}
#else
HEFF_FN void heff_watch_faults(void) {}
HEFF_FN void heff_watch_stack(void) {}
HEFF_FN void heff_unwatch_stack(void) {}
#endif

// Threads a provider starts (#36): each runs what it
// is given -- shared, `heff_share` -- by `heff_try`, so
// that an abandon there is the thread's to hand back, not the end of the
// process; and ends by `heff_thread_end`.  A failure there ends the
// process, as it would on the program's thread.
// ---------------------------------------------------------------------

// (`f x`, asked for here, an abandon or fuel run out caught: 0, its
// value at `*out`; or why it stopped -- "heff:abandon" where it
// abandoned (`heff_abandon`), "heff:fuel" where it ran out of fuel, to
// be copied before the thread ends.  What was made before is leaked,
// not given back.  A failure is not caught: the program ends there)
HEFF_FN const char *heff_try(V f, V x, V *out) {
  jmp_buf here;
  jmp_buf *volatile up = heff_catch;
  volatile long dep = heff_dep;
  volatile size_t open = heff_nopen;
  heff_catch = &here;
  if (HEFF_SETJMP(here) == 0) {
    *out = heff_tr(heff_app1(f, x));
    heff_catch = up;
    return NULL;
  }
  heff_catch = up;
  heff_dep = dep;
  heff_reopen(open);
  return heff_caught;
}

// (an abandon, or fuel run out, handed back, as the thread it was caught
// in would have stopped: `why` from `heff_try`, copied)
HEFF_FN void heff_fail_again(const char *why) {
  if (strcmp(why, "heff:abandon") == 0) heff_abandon();
  heff_stopped(why);
}

// (this thread's pages and what else the runtime keeps for it given
// back to the system, as it ends -- its pages that still hold something
// abandoned, for another thread to adopt)
// (what a thread does as it ends, where a part of the runtime asks for
// it: the custodians' part, the releases due on the thread run)
HEFF_VAR(void (*heff_thread_ending)(void));

HEFF_FN void heff_thread_end(void) {
  void (*ending)(void) = __atomic_load_n(&heff_thread_ending, __ATOMIC_ACQUIRE);
  if (ending) ending();
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
#ifdef HEFF_BLOCKS
  heff_big_end();
#endif
  free(heff_dead);
  heff_dead = NULL;
  heff_ndead = heff_cdead = 0;
  free(heff_dir);
  heff_dir = NULL;
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
typedef struct { void *(*run)(void *); void *u; } HeffStart;

// (a thread heff starts, watched while it runs)
HEFF_FN void *heff_started(void *u) {
  HeffStart s = *(HeffStart *)u;
  free(u);
  heff_tls_enter();
  heff_watch_stack();
  void *r = s.run(s.u);
  heff_unwatch_stack();
  return r;
}

HEFF_FN int heff_thread_start(pthread_t *t, void *(*run)(void *), void *u) {
  HeffStart *s = malloc(sizeof(HeffStart));
  if (!s) return ENOMEM;
  s->run = run;
  s->u = u;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HEFF_THREAD_STACK);
#ifdef HEFF_WATCHED
  pthread_attr_setguardsize(&attr, HEFF_GUARD);
#endif
  int r = pthread_create(t, &attr, heff_started, s);
  pthread_attr_destroy(&attr);
  if (r != 0) free(s);
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
// (`heff_wait_done`).  A wait may have a deadline instead of, or as
// well as, a descriptor (`until`, by `heff_now_ms`): a sleep of
// std.time's is one with no descriptor, done when its time has passed,
// the reactor waiting no longer than the nearest deadline.  A wait whose
// deadline passes before it is done is done so, its `err` ETIMEDOUT:
// the time limit of an operation of std.net's, its answer that it timed
// out, what it began undone by it (a socket being connected closed).
// Where no group is being started in, the same deadline bounds the poll
// that waits for it there.
//
// Each thread of the system has a reactor of its own (`HEFF_TLS`): its
// waits are its own, waited for by it alone, driven by the scheduler
// running in it, and nothing of one thread's reactor is touched by
// another.

#include <errno.h>
#include <time.h>

// (now, in milliseconds of a clock that only goes forward: for
// deadlines, not for the time of day)
#ifdef _WIN32
HEFF_FN int64_t heff_now_ms(void) { return (int64_t)GetTickCount64(); }
#else
HEFF_FN int64_t heff_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif

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
  int fd;                        // (what it waits for; -1, a source's word,
                                 //  or its deadline alone)
  short events;                  // (POLLIN, POLLOUT)
  int64_t until;                 // (when it is done regardless, by
                                 //  `heff_now_ms`; 0, never)
  int err;                       // (why waiting failed -- errno, poll's,
                                 //  EBADF, its socket closed, or ETIMEDOUT,
                                 //  its deadline passed -- its answer's to say)
  int (*step)(HeffWait *);       // (its next step: HEFF_STEP_DONE, or HEFF_STEP_WAIT,
                                 //  its `fd` and `events` again)
  V (*answer)(HeffWait *);       // (its answer, made, the wait let go of)
  void (*cancel)(HeffWait *);    // (the wait let go of, undone)
  int loose;                     // (whether its descriptor is to be the
                                 //  kernel's only while a turn waits:
                                 //  polled by the turn, where the reactor
                                 //  has a ring -- a listener, which a poll
                                 //  of the ring's would keep listening
                                 //  once the process is gone)
  // (the reactor's)
  V f, k;
  int64_t group;
  int done;
  int64_t epoch;                 // (how many descriptors had been closed,
                                 //  by any thread, when it began to wait)
  int marked;                    // (the descriptor its reactor is marked
                                 //  as waiting on, for it, or -1)
  int kfd;                       // (where there is an event queue: the
  short kway;                    //  descriptor it is waiting on there, and
                                 //  which way --
                                 //  1 reading, 2 writing -- or 0, on none;
                                 //  3, polled instead; 4 and 5, an
                                 //  operation of the ring's that way, 6
                                 //  and 7, one being taken back, the
                                 //  wait done meanwhile, 8, one whose wait
                                 //  is let go of)
  int32_t ring_res;              // (that operation's result, where it has
  int ring_got;                  //  come and is not yet looked at)
  HeffWait *prev, *next;
  int linked;                    // (whether it is among the pending)
  int64_t seq;                   // (the order it began to wait in, among
                                 //  this thread's)
  size_t heapat, readyat;        // (where it is in the deadlines' heap,
                                 //  and among the done; 1 on, 0 nowhere)
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

// (a group: its number, unique among every thread's; how many waits are
// pending in it -- in a root, those of the groups within it too -- the
// root it is within, or 0 where it is one; in a root, what a step of a
// computation started in it that fails is finished as, given why, or 0:
// the failure is the program's; and, in a group within one, what gives
// it up from any thread, where something does, or 0)
typedef struct HeffGiving HeffGiving;
typedef struct { int64_t id, pending, root; V failed; HeffGiving *giving; } HeffGroup;
typedef struct { int64_t group; V v; } HeffFinished;

HEFF_VAR(HEFF_TLS int64_t heff_group);
HEFF_VAR(int64_t heff_last_group);
HEFF_VAR(HEFF_TLS HeffGroup *heff_groups_open);
HEFF_VAR(HEFF_TLS size_t heff_ngroups);
HEFF_VAR(HEFF_TLS size_t heff_capgroups);
HEFF_VAR(HEFF_TLS HeffWait *heff_waits_first);
HEFF_VAR(HEFF_TLS HeffWait *heff_waits_last);
// (this thread's pending waits with a deadline, in a heap, the nearest
// first, and its waits done and not yet woken, in no order: so that a
// turn asks the nearest deadline, and the waits done, of these alone,
// and does not look at every pending wait, as many as a server has
// quiet connections)
HEFF_VAR(HEFF_TLS HeffWait **heff_dues);
HEFF_VAR(HEFF_TLS size_t heff_ndues);
HEFF_VAR(HEFF_TLS size_t heff_capdues);
HEFF_VAR(HEFF_TLS HeffWait **heff_ready);
HEFF_VAR(HEFF_TLS size_t heff_nready);
HEFF_VAR(HEFF_TLS size_t heff_capready);
HEFF_VAR(HEFF_TLS int64_t heff_wait_seq);
// (how many of its pending waits the turn polls itself: where there is
// an event queue, those it was not given, none mostly)
HEFF_VAR(HEFF_TLS size_t heff_npolled);
HEFF_VAR(HEFF_TLS HeffFinished *heff_finished);
HEFF_VAR(HEFF_TLS size_t heff_nfinished);
HEFF_VAR(HEFF_TLS size_t heff_capfinished);
#ifndef _WIN32
HEFF_VAR(HEFF_TLS HeffSource *heff_sources);
HEFF_VAR(HEFF_TLS struct pollfd *heff_polled);
HEFF_VAR(HEFF_TLS HeffWait **heff_polled_waits);
HEFF_VAR(HEFF_TLS size_t heff_cappolled);
// (what wakes this thread's reactor from another thread: a pipe, made
// with its first group and closed with its last, written a byte to; and
// whether a turn has read one since `heff_reactor_finished` last came
// back)
HEFF_VAR(HEFF_TLS int heff_wake_made);
HEFF_VAR(HEFF_TLS int heff_wake_fds[2]);
HEFF_VAR(HEFF_TLS int heff_woken);
#endif

// (every thread's open groups, each with the pipe that wakes its
// thread's reactor, under a lock: so that a group is woken from any
// thread, and not once its pipe is closed)
typedef struct { int64_t id; int fd; } HeffWaker;
#ifdef HEFF_PART
extern HeffWaker *heff_wakers;
extern size_t heff_nwakers, heff_capwakers;
extern pthread_mutex_t heff_wakers_mu;
#else
HeffWaker *heff_wakers;
size_t heff_nwakers, heff_capwakers;
pthread_mutex_t heff_wakers_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

// (the descriptors closed last, by any thread, under the wakers' lock:
// each with the count of those closed so far, kept at that count modulo
// how many are kept -- so that a wait pending in another thread's
// reactor on one that is closed is done, EBADF, by that thread, and not
// one begun after on a descriptor of the same number; and how many this
// thread has looked at)
#define HEFF_CLOSED_KEPT 1024
typedef struct { int fd; int64_t epoch; } HeffClosed;
#ifdef HEFF_PART
extern HeffClosed heff_closed[HEFF_CLOSED_KEPT];
extern int64_t heff_closed_epoch;
#else
HeffClosed heff_closed[HEFF_CLOSED_KEPT];
int64_t heff_closed_epoch;
#endif
HEFF_VAR(HEFF_TLS int64_t heff_closed_seen);

#ifndef _WIN32
// (which threads' reactors wait on each descriptor: a word of bits for
// each, a bit for each reactor, set by its thread while one of its waits
// is on it and cleared by it when none is -- so that a descriptor being
// closed wakes those alone, and only where there are any.  The words are
// in chunks made when first needed, never let go of; the reactors' bits
// and their pipes are given out with their pipes, under the wakers'
// lock, the last bit standing for every reactor beyond the others, set
// and never cleared, which wakes every reactor.  A descriptor beyond the
// chunks has no word, and wakes every reactor.  Of a thread's own: its
// bit, 0 where it has no pipe, and how many of its waits are on each
// descriptor)
#define HEFF_MARK_CHUNK 4096
#define HEFF_MARK_CHUNKS 1024
#define HEFF_MARK_ANY ((uint64_t)1 << 63)
HEFF_VAR(uint64_t *heff_marks[HEFF_MARK_CHUNKS]);
HEFF_VAR(uint64_t heff_marks_given);
HEFF_VAR(int heff_marks_pipe[63]);
HEFF_VAR(HEFF_TLS uint64_t heff_mark);
HEFF_VAR(HEFF_TLS int32_t *heff_marks_mine);
HEFF_VAR(HEFF_TLS size_t heff_capmarks_mine);

// (the word of descriptor `fd`, made where `make` and it is not; NULL
// where it is beyond the chunks, or, not `make`, not made)
HEFF_FN uint64_t *heff_marks_of(int fd, int make) {
  size_t c = (size_t)fd / HEFF_MARK_CHUNK;
  if (c >= HEFF_MARK_CHUNKS) return NULL;
  uint64_t *m = __atomic_load_n(&heff_marks[c], __ATOMIC_SEQ_CST);
  if (!m && make) {
    uint64_t *made = calloc(HEFF_MARK_CHUNK, sizeof *made);
    if (!made) heff_fail("out of memory");
    if (__atomic_compare_exchange_n(&heff_marks[c], &m, made, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) m = made;
    else free(made);
  }
  return m ? &m[(size_t)fd % HEFF_MARK_CHUNK] : NULL;
}

// (this thread's reactor marked as waiting on `fd` once more: whether it
// is, or once less)
HEFF_FN int heff_mark_on(int fd) {
  if (fd < 0 || !heff_mark || (size_t)fd >= (size_t)HEFF_MARK_CHUNK * HEFF_MARK_CHUNKS) return 0;
  if ((size_t)fd >= heff_capmarks_mine) {
    size_t cap = heff_capmarks_mine ? heff_capmarks_mine : 256;
    while (cap <= (size_t)fd) cap *= 2;
    int32_t *m = heff_alloc(cap * sizeof *m);
    if (heff_capmarks_mine) memcpy(m, heff_marks_mine, heff_capmarks_mine * sizeof *m);
    memset(m + heff_capmarks_mine, 0, (cap - heff_capmarks_mine) * sizeof *m);
    free(heff_marks_mine);
    heff_marks_mine = m;
    heff_capmarks_mine = cap;
  }
  if (heff_marks_mine[fd]++ == 0) __atomic_fetch_or(heff_marks_of(fd, 1), heff_mark, __ATOMIC_SEQ_CST);
  return 1;
}

HEFF_FN void heff_mark_off(int fd) {
  if (fd < 0 || !heff_mark || (size_t)fd >= heff_capmarks_mine) return;
  if (--heff_marks_mine[fd] == 0 && heff_mark != HEFF_MARK_ANY)
    __atomic_fetch_and(heff_marks_of(fd, 1), ~heff_mark, __ATOMIC_SEQ_CST);
}
#endif

#ifdef HEFF_EVQ
// (where the system has an event queue -- kqueue, or Linux's epoll --
// each thread's reactor keeps its waits' descriptors in one of its own,
// each added as its wait begins and taken out as it ends, so that a
// turn asks the kernel for those ready alone -- a poll of every pending
// wait's descriptor, every turn, costs as many as are pending, and a
// server's many quiet connections made each turn as dear as they are
// many.  A descriptor waited on one way by two waits of the thread at
// once, or one the queue refuses, is polled as before, the later
// wait's.  The queue, made with the thread's first group, as its pipe
// is, is polled with the others, and read, not waiting, where it is
// ready; and each wait in it has its descriptor and its way kept, by
// both, at `heff_evq_owner`, which says whose an event is.  The two
// differ in what the kernel is told alone: kqueue keeps each way of a
// descriptor apart, epoll the two together, as one)
HEFF_VAR(HEFF_TLS int heff_evq);
HEFF_VAR(HEFF_TLS int heff_evq_made);
HEFF_VAR(HEFF_TLS HeffWait **heff_evq_owner);
HEFF_VAR(HEFF_TLS size_t heff_capevq_owner);

// (the wait waiting on `fd` the way `way` -- 1 reading, 2 writing -- in
// the queue, or NULL)
static inline HeffWait *heff_evq_whose(int fd, short way) {
  size_t at = (size_t)fd * 2 + (size_t)(way - 1);
  return at < heff_capevq_owner ? heff_evq_owner[at] : NULL;
}

#ifdef HEFF_KQUEUE
// (the kernel told: `fd` waited on `way`, or, not `on`, no longer --
// whether it took it)
HEFF_FN int heff_evq_tell(int fd, short way, int on) {
  struct kevent ch, got;
  struct timespec zero = { 0, 0 };
  short filter = way == 1 ? EVFILT_READ : EVFILT_WRITE;
  if (!on) {
    EV_SET(&ch, (uintptr_t)fd, filter, EV_DELETE, 0, 0, NULL);
    (void)kevent(heff_evq, &ch, 1, NULL, 0, &zero);
    return 1;
  }
  EV_SET(&ch, (uintptr_t)fd, filter, EV_ADD | EV_RECEIPT, 0, 0, NULL);
  int r = kevent(heff_evq, &ch, 1, &got, 1, &zero);
  return !(r < 0 || (r == 1 && (got.flags & EV_ERROR) && got.data != 0));
}
#else
#ifdef HEFF_URING
// (Linux's io_uring, where the kernel and what the program runs in let
// one be made, in the epoll's stead: a ring of each thread's reactor's
// own, the queue's descriptor its.  A wait in it is a poll of its
// descriptor one way, its token -- its slot at `heff_evq_owner` and a
// count, so that a poll taken out, or spent, is not taken for the next
// of that slot's -- kept at `heff_ring_armed` while the kernel has it.
// A poll is spent as it fires, and told again where the wait, stepped,
// waits on as it was: a step may wait again without having read all
// there was, as poll and epoll let it, and most waits fire once and
// end, where a poll that stayed would have to be taken out after.  What
// the ring is told -- a poll, one taken out -- is written to its
// submission queue alone and given to the kernel with the turn's wait,
// in one call, and what fired comes back in its completion queue, read
// with no call.  The ring is polled with the pipe that wakes the thread,
// what it was told given to the kernel first: the pipe polled by the
// ring instead, each wake a request of the ring's, cost a tenth more of
// the CPU of 1000 streams of events at once, measured beside epoll.  A
// poll holds its descriptor's file, a socket closed not let go of by
// the kernel until its poll is taken out: a wait on one closed has its
// poll taken out at once.
//
// A wait may instead be an operation the ring does (`heff_ring_op`): a
// read or a write, an accept, a file opened or looked at -- its
// provider's step taken again with its result once the kernel has done
// it (`heff_ring_took`), into and out of memory the provider keeps until
// then.  So the kernel has that memory until its completion comes back,
// and a wait that ends before -- its deadline passed, its descriptor
// closed, its group closed -- has its operation taken back (an
// IORING_OP_ASYNC_CANCEL) and is ended, or let go of (its `cancel`),
// only as the completion comes; the ring is let go of only once every
// one has.  Such an operation has its descriptor's slot at
// `heff_evq_owner`, its way's, as a poll would, so that a descriptor
// closed finds it there.
//
// Where no ring is made -- no io_uring, one a sandbox refuses, one
// before Linux 5.11 -- or `HEFF_REACTOR` is `epoll`, the epoll is; and,
// one refused, every thread's is)
typedef struct { uint64_t ud; int32_t res; } HeffRingGot;
typedef struct {
  unsigned *sq_head, *sq_tail, *sq_flags, *cq_head, *cq_tail;
  unsigned sq_mask, cq_mask, sq_entries, tail, features;
  uint64_t ops[4];               // (which operations the kernel has, a bit
                                 //  each, as its probe says)
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  void *rings;
  size_t rings_len, sqes_len;
} HeffRing;
// (a completion's user data: a poll's token, the count below the top
// bit; an operation's, its wait, the top bit set; or none to look at)
#define HEFF_RING_OP ((uint64_t)1 << 63)
#define HEFF_RING_NONE UINT64_MAX
HEFF_VAR(int heff_ring_refused);
HEFF_VAR(HEFF_TLS int heff_ring_on);
HEFF_VAR(HEFF_TLS HeffRing heff_ring);
HEFF_VAR(HEFF_TLS uint64_t *heff_ring_armed);
HEFF_VAR(HEFF_TLS uint32_t heff_ring_count);
// (the completions read, not yet looked at; whether the pipe's poll is
// the kernel's)
HEFF_VAR(HEFF_TLS HeffRingGot *heff_ring_got);
HEFF_VAR(HEFF_TLS size_t heff_ring_ngot);
HEFF_VAR(HEFF_TLS size_t heff_capring_got);
// (how many of the ring's operations the kernel has)
HEFF_VAR(HEFF_TLS size_t heff_ring_inflight);

// (the ring made, its descriptor; or -1, and no ring made again)
HEFF_FN int heff_ring_open(void) {
  if (__atomic_load_n(&heff_ring_refused, __ATOMIC_RELAXED)) return -1;
  const char *how = getenv("HEFF_REACTOR");
  struct io_uring_params p;
  memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_CQSIZE;
  p.cq_entries = 4096;
  int fd = how && strcmp(how, "epoll") == 0 ? -1 : (int)syscall(__NR_io_uring_setup, 512, &p);
  unsigned need = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_EXT_ARG;
  HeffRing *g = &heff_ring;
  if (fd >= 0 && (p.features & need) == need) {
    size_t sq = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    g->rings_len = sq > cq ? sq : cq;
    g->sqes_len = p.sq_entries * sizeof(struct io_uring_sqe);
    g->rings = mmap(NULL, g->rings_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    void *e = g->rings == MAP_FAILED ? MAP_FAILED : mmap(NULL, g->sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (e != MAP_FAILED) {
      char *r = g->rings;
      g->sq_head = (unsigned *)(r + p.sq_off.head);
      g->sq_tail = (unsigned *)(r + p.sq_off.tail);
      g->sq_flags = (unsigned *)(r + p.sq_off.flags);
      g->sq_mask = *(unsigned *)(r + p.sq_off.ring_mask);
      g->sq_entries = p.sq_entries;
      g->cq_head = (unsigned *)(r + p.cq_off.head);
      g->cq_tail = (unsigned *)(r + p.cq_off.tail);
      g->cq_mask = *(unsigned *)(r + p.cq_off.ring_mask);
      g->cqes = (struct io_uring_cqe *)(r + p.cq_off.cqes);
      g->sqes = e;
      g->tail = *g->sq_tail;
      g->features = p.features;
      // (each entry of the queue at its own place: written there alone)
      unsigned *at = (unsigned *)(r + p.sq_off.array);
      for (unsigned i = 0; i < p.sq_entries; i++) at[i] = i;
      // (the operations it has, where it says)
      memset(g->ops, 0, sizeof g->ops);
      size_t plen = sizeof(struct io_uring_probe) + 256 * sizeof(struct io_uring_probe_op);
      struct io_uring_probe *pr = calloc(1, plen);
      if (pr && syscall(__NR_io_uring_register, fd, IORING_REGISTER_PROBE, pr, 256) == 0)
        for (unsigned i = 0; i < pr->ops_len && i < 256; i++)
          if (pr->ops[i].flags & IO_URING_OP_SUPPORTED) g->ops[pr->ops[i].op / 64] |= (uint64_t)1 << (pr->ops[i].op % 64);
      free(pr);
      return fd;
    }
    if (g->rings != MAP_FAILED) munmap(g->rings, g->rings_len);
  }
  if (fd >= 0) close(fd);
  __atomic_store_n(&heff_ring_refused, 1, __ATOMIC_RELAXED);
  return -1;
}

// The program's end and the rings.  What the kernel holds of a ring at
// the end -- a poll, a read -- holds its descriptor's file: a connection
// stays open until the kernel tears the ring down, after the process is
// gone, which Linux 6.12 takes a tenth of a second for.  A listener so
// held would stay listening, and a server started again on its port
// meanwhile -- restarted, killed -- find it taken: so a listener is
// never the ring's (`loose`), but polled by the turn, held by the kernel
// only while the turn waits.  At the end (`exit`, a failure's
// `quick_exit`) what each thread's ring has with the kernel is taken
// back first, at once (IORING_REGISTER_SYNC_CANCEL, Linux 6.0 on;
// before, nothing is), and what it held let go of with the process; one
// killed -- SIGKILL, a signal not handled -- runs nothing, its
// connections closed as the kernel tears its rings down.  The rings are
// kept by their descriptors, plus one (0, none), so many at most; one
// let go of is taken out first, under the same lock, so that its number
// is never another's by then.
#define HEFF_RINGS 1024
HEFF_VAR(int heff_rings[HEFF_RINGS]);
HEFF_VAR(int heff_rings_lock);
HEFF_VAR(int heff_rings_ending);

HEFF_FN void heff_rings_take(void) {
  while (__atomic_exchange_n(&heff_rings_lock, 1, __ATOMIC_ACQUIRE))
    ;
}

HEFF_FN void heff_rings_give(void) { __atomic_store_n(&heff_rings_lock, 0, __ATOMIC_RELEASE); }

// (every ring's operations taken back, at the end)
HEFF_FN void heff_rings_end(void) {
#ifdef IORING_ASYNC_CANCEL_ANY
  heff_rings_take();
  for (int i = 0; i < HEFF_RINGS; i++) {
    if (!heff_rings[i]) continue;
    struct io_uring_sync_cancel_reg r;
    memset(&r, 0, sizeof r);
    r.fd = -1;
    r.flags = IORING_ASYNC_CANCEL_ANY;
    r.timeout.tv_nsec = 100000000;
    (void)syscall(__NR_io_uring_register, heff_rings[i] - 1, IORING_REGISTER_SYNC_CANCEL, &r, 1);
  }
  heff_rings_give();
#endif
}

// (the ring `fd` kept, or, `on` 0, taken out; the end's taking back
// asked for, the first time)
HEFF_FN void heff_rings_keep(int fd, int on) {
  heff_rings_take();
  for (int i = 0; i < HEFF_RINGS; i++)
    if (on ? !heff_rings[i] : heff_rings[i] == fd + 1) {
      heff_rings[i] = on ? fd + 1 : 0;
      break;
    }
  int first = on && !heff_rings_ending;
  if (first) heff_rings_ending = 1;
  heff_rings_give();
  if (first) {
    atexit(heff_rings_end);
    at_quick_exit(heff_rings_end);
  }
}

// (the ring let go of: its descriptor closed after, by the queue's)
HEFF_FN void heff_ring_close(void) {
  munmap(heff_ring.sqes, heff_ring.sqes_len);
  munmap(heff_ring.rings, heff_ring.rings_len);
  free(heff_ring_armed);
  free(heff_ring_got);
  heff_ring_armed = NULL;
  heff_ring_got = NULL;
  heff_ring_ngot = heff_capring_got = 0;
  heff_ring_on = 0;
  heff_ring_inflight = 0;
}

// (the ring's completions moved to `heff_ring_got`, to be looked at
// after; and those the kernel held, the ring full, after them)
HEFF_FN void heff_ring_collect(void) {
  for (;;) {
    unsigned head = *heff_ring.cq_head;
    unsigned tail = __atomic_load_n(heff_ring.cq_tail, __ATOMIC_ACQUIRE);
    size_t n = (size_t)(tail - head);
    if (heff_ring_ngot + n > heff_capring_got) {
      size_t cap = heff_capring_got ? heff_capring_got : 256;
      while (cap < heff_ring_ngot + n) cap *= 2;
      HeffRingGot *m = heff_alloc(cap * sizeof *m);
      if (heff_ring_ngot) memcpy(m, heff_ring_got, heff_ring_ngot * sizeof *m);
      free(heff_ring_got);
      heff_ring_got = m;
      heff_capring_got = cap;
    }
    for (; head != tail; head++) {
      struct io_uring_cqe *c = &heff_ring.cqes[head & heff_ring.cq_mask];
      heff_ring_got[heff_ring_ngot].ud = c->user_data;
      heff_ring_got[heff_ring_ngot].res = c->res;
      heff_ring_ngot++;
    }
    __atomic_store_n(heff_ring.cq_head, head, __ATOMIC_RELEASE);
    if (!(__atomic_load_n(heff_ring.sq_flags, __ATOMIC_ACQUIRE) & IORING_SQ_CQ_OVERFLOW)) return;
    if (syscall(__NR_io_uring_enter, heff_evq, 0, 0, IORING_ENTER_GETEVENTS, NULL, 0) < 0) return;
    if (__atomic_load_n(heff_ring.cq_tail, __ATOMIC_ACQUIRE) == head) return;
  }
}

// (what the ring was told given to the kernel, and, `ms` not 0, an event
// waited for, `ms` milliseconds at most, -1 for ever -- not where one
// is in already)
HEFF_FN void heff_ring_enter(int ms) {
  for (int tries = 0;; tries++) {
    unsigned n = heff_ring.tail - __atomic_load_n(heff_ring.sq_head, __ATOMIC_ACQUIRE);
    unsigned flags = 0, min = 0;
    struct io_uring_getevents_arg a;
    struct __kernel_timespec ts;
    void *arg = NULL;
    size_t argsz = 0;
    if (ms != 0 && !heff_ring_ngot && *heff_ring.cq_head == __atomic_load_n(heff_ring.cq_tail, __ATOMIC_ACQUIRE)) {
      flags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
      min = 1;
      memset(&a, 0, sizeof a);
      if (ms > 0) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long long)(ms % 1000) * 1000000;
        a.ts = (uint64_t)(uintptr_t)&ts;
      }
      arg = &a;
      argsz = sizeof a;
    }
    if (!n && !flags) return;
    if (syscall(__NR_io_uring_enter, heff_evq, n, min, flags, arg, argsz) >= 0) return;
    if (errno == ETIME || errno == EINTR) return;
    // (the kernel holding completions it has no room for: those read,
    // and given again, not waiting)
    if ((errno == EBUSY || errno == EAGAIN) && tries < 8) {
      heff_ring_collect();
      ms = 0;
      continue;
    }
    heff_fail("the reactor's ring failed");
  }
}

// (the next entry of the submission queue, cleared -- given to the
// kernel by `heff_ring_push` -- what is in it submitted first where it is
// full)
HEFF_FN struct io_uring_sqe *heff_ring_sqe(void) {
  for (int tries = 0;; tries++) {
    if (heff_ring.tail - __atomic_load_n(heff_ring.sq_head, __ATOMIC_ACQUIRE) < heff_ring.sq_entries) {
      struct io_uring_sqe *e = &heff_ring.sqes[heff_ring.tail & heff_ring.sq_mask];
      memset(e, 0, sizeof *e);
      return e;
    }
    if (tries == 8) heff_fail("the reactor's ring is full");
    heff_ring_enter(0);
  }
}

static inline void heff_ring_push(void) {
  __atomic_store_n(heff_ring.sq_tail, ++heff_ring.tail, __ATOMIC_RELEASE);
}

// (the ring told: `fd` polled `way`, or, not `on`, its poll taken out,
// where it has one -- taken, always: a poll the kernel refuses, the
// descriptor closed meanwhile, fires, as poll has it)
HEFF_FN int heff_ring_tell(int fd, short way, int on) {
  size_t at = (size_t)fd * 2 + (size_t)(way - 1);
  if (!on) {
    if (at >= heff_capevq_owner || !heff_ring_armed[at]) return 1;
    struct io_uring_sqe *e = heff_ring_sqe();
    e->opcode = IORING_OP_POLL_REMOVE;
    e->fd = -1;
    e->addr = heff_ring_armed[at];
    e->user_data = HEFF_RING_NONE;
    if (heff_ring.features & IORING_FEAT_CQE_SKIP) e->flags = IOSQE_CQE_SKIP_SUCCESS;
    heff_ring_push();
    heff_ring_armed[at] = 0;
    return 1;
  }
  heff_ring_count = (heff_ring_count + 1) & 0x7fffffffu;
  if (heff_ring_count == 0) heff_ring_count = 1;
  uint64_t token = (uint64_t)heff_ring_count << 32 | (uint32_t)at;
  uint32_t mask = way == 1 ? POLLIN : POLLOUT;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  mask = mask << 16 | mask >> 16;
#endif
  struct io_uring_sqe *e = heff_ring_sqe();
  e->opcode = IORING_OP_POLL_ADD;
  e->fd = fd;
  e->poll32_events = mask;
  e->user_data = token;
  heff_ring_push();
  heff_ring_armed[at] = token;
  return 1;
}

// (the wait's operation taken back: its completion comes all the same)
HEFF_FN void heff_ring_op_cancel(HeffWait *w) {
  struct io_uring_sqe *e = heff_ring_sqe();
  e->opcode = IORING_OP_ASYNC_CANCEL;
  e->fd = -1;
  e->addr = HEFF_RING_OP | (uint64_t)(uintptr_t)w;
  e->user_data = HEFF_RING_NONE;
  if (heff_ring.features & IORING_FEAT_CQE_SKIP) e->flags = IOSQE_CQE_SKIP_SUCCESS;
  heff_ring_push();
}

// (the slot of the wait's operation free again, where it is the wait's)
HEFF_FN void heff_ring_op_slot_free(HeffWait *w, short way) {
  size_t at = (size_t)w->kfd * 2 + (size_t)(way - 1);
  if (at < heff_capevq_owner && heff_evq_owner[at] == w) {
    heff_evq_owner[at] = NULL;
    heff_ring_armed[at] = 0;
  }
}
#endif

// (epoll: the descriptor's ways, both, as the waits in it say -- the
// other way's kept as it is.  Its registration may be gone, the
// descriptor closed meanwhile, or there, one of the same number's:
// either told again, where a wait is to be added; one taken out of a
// descriptor that is gone is not added back)
HEFF_FN int heff_evq_tell(int fd, short way, int on) {
#ifdef HEFF_URING
  if (heff_ring_on) return heff_ring_tell(fd, way, on);
#endif
  int other = heff_evq_whose(fd, way == 1 ? 2 : 1) != NULL;
  uint32_t mask = 0;
  if (on) mask |= way == 1 ? EPOLLIN : EPOLLOUT;
  if (other) mask |= way == 1 ? EPOLLOUT : EPOLLIN;
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = mask;
  ev.data.u64 = (uint64_t)fd;
  if (!mask) {
    (void)epoll_ctl(heff_evq, EPOLL_CTL_DEL, fd, &ev);
    return 1;
  }
  int op = on && !other ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(heff_evq, op, fd, &ev) == 0) return 1;
  if (op == EPOLL_CTL_ADD && errno == EEXIST) return epoll_ctl(heff_evq, EPOLL_CTL_MOD, fd, &ev) == 0;
  if (op == EPOLL_CTL_MOD && errno == ENOENT && on) return epoll_ctl(heff_evq, EPOLL_CTL_ADD, fd, &ev) == 0;
  return 0;
}
#endif

// (the wait polled by the turn, not the queue: counted so)
static inline void heff_evq_polled(HeffWait *w) {
  w->kway = 3;
  heff_npolled++;
}

// (the wait's descriptor and way in the queue: where it has one, and
// another wait of the thread has not, and the queue takes it; else
// polled)
// (room at `heff_evq_owner` for the slot `at`)
HEFF_FN void heff_evq_room(size_t at) {
  if (at >= heff_capevq_owner) {
    size_t cap = heff_capevq_owner ? heff_capevq_owner : 512;
    while (cap <= at) cap *= 2;
    HeffWait **m = heff_alloc(cap * sizeof *m);
    if (heff_capevq_owner) memcpy(m, heff_evq_owner, heff_capevq_owner * sizeof *m);
    memset(m + heff_capevq_owner, 0, (cap - heff_capevq_owner) * sizeof *m);
    free(heff_evq_owner);
    heff_evq_owner = m;
#ifdef HEFF_URING
    if (heff_ring_on) {
      uint64_t *a = heff_alloc(cap * sizeof *a);
      if (heff_capevq_owner) memcpy(a, heff_ring_armed, heff_capevq_owner * sizeof *a);
      memset(a + heff_capevq_owner, 0, (cap - heff_capevq_owner) * sizeof *a);
      free(heff_ring_armed);
      heff_ring_armed = a;
    }
#endif
    heff_capevq_owner = cap;
  }
}

// (whether the wait has an operation of the ring's that is the kernel's:
// one its step began, as it waits on its slot)
static inline int heff_evq_op(HeffWait *w) {
#ifdef HEFF_URING
  if (heff_ring_on && w->kway >= 4 && w->kway <= 5) {
    // (one of a path's, in no slot: -2)
    if (w->kfd == -2) return heff_ring_inflight > 0;
    size_t at = (size_t)w->kfd * 2 + (size_t)(w->kway - 4);
    return at < heff_capevq_owner && heff_evq_owner[at] == w && heff_ring_armed[at] == HEFF_RING_OP;
  }
#else
  (void)w;
#endif
  return 0;
}

HEFF_FN void heff_evq_add(HeffWait *w) {
  if (heff_evq_op(w)) return;
  w->kway = 0;
  w->kfd = w->fd;
  if (w->fd < 0 || w->done) return;
  short way = w->events == POLLIN ? 1 : w->events == POLLOUT ? 2 : 0;
  if (!way || !heff_evq_made) {
    heff_evq_polled(w);
    return;
  }
#ifdef HEFF_URING
  if (heff_ring_on && w->loose) {
    heff_evq_polled(w);
    return;
  }
#endif
  size_t at = (size_t)w->fd * 2 + (size_t)(way - 1);
  heff_evq_room(at);
  if (heff_evq_owner[at] || !heff_evq_tell(w->fd, way, 1)) {
    heff_evq_polled(w);
    return;
  }
  heff_evq_owner[at] = w;
  w->kway = way;
}

// (the wait's descriptor taken out of the queue, where it is there --
// closed meanwhile, the kernel took it out then)
HEFF_FN void heff_evq_delete(HeffWait *w) {
#ifdef HEFF_URING
  // (an operation of the ring's: taken back, where it is not being, and
  // its wait let go of only as it comes back)
  if (heff_ring_on && w->kway >= 4 && w->kway <= 7) {
    if (w->kway <= 5) heff_ring_op_cancel(w);
    heff_ring_op_slot_free(w, (short)((w->kway - 4) % 2 + 1));
    w->kway = 8;
    return;
  }
#endif
  short way = w->kway;
  w->kway = 0;
  if (way == 3) heff_npolled--;
  if (way != 1 && way != 2) return;
  size_t at = (size_t)w->kfd * 2 + (size_t)(way - 1);
  if (at < heff_capevq_owner && heff_evq_owner[at] == w) heff_evq_owner[at] = NULL;
  heff_evq_tell(w->kfd, way, 0);
}

// (the wait's way, after a step: where it waits on another descriptor,
// or the other way, or is done, so in the queue)
HEFF_FN void heff_evq_again(HeffWait *w) {
  if (heff_evq_op(w)) return;
  short way = w->events == POLLIN ? 1 : w->events == POLLOUT ? 2 : 0;
  if (w->done || w->fd != w->kfd || (w->kway != 3 && way != w->kway)) {
    heff_evq_delete(w);
    heff_evq_add(w);
  }
#ifdef HEFF_URING
  // (a ring's poll, spent as it fired: told again)
  else if (heff_ring_on && (w->kway == 1 || w->kway == 2)) {
    size_t at = (size_t)w->kfd * 2 + (size_t)(w->kway - 1);
    if (!heff_ring_armed[at]) heff_ring_tell(w->kfd, w->kway, 1);
  }
#endif
}

// (the queue made, with the thread's first group, as its pipe is: where
// it cannot be, its waits polled)
HEFF_FN void heff_evq_open(void) {
  if (heff_evq_made) return;
#ifdef HEFF_KQUEUE
  heff_evq = kqueue();
  if (heff_evq < 0) return;
  fcntl(heff_evq, F_SETFD, FD_CLOEXEC);
#else
#ifdef HEFF_URING
  heff_evq = heff_ring_open();
  if (heff_evq >= 0) {
    heff_ring_on = heff_evq_made = 1;
    heff_rings_keep(heff_evq, 1);
    return;
  }
#endif
  heff_evq = epoll_create1(EPOLL_CLOEXEC);
  if (heff_evq < 0) return;
#endif
  heff_evq_made = 1;
}

// (the queue let go of, with the thread's last group: none of its
// waits pending)
HEFF_FN void heff_evq_close(void) {
  if (!heff_evq_made) return;
#ifdef HEFF_URING
  if (heff_ring_on) {
    heff_rings_keep(heff_evq, 0);
    heff_ring_close();
  }
#endif
  close(heff_evq);
  heff_evq_made = 0;
  free(heff_evq_owner);
  heff_evq_owner = NULL;
  heff_capevq_owner = 0;
}
#endif

// | Operations a provider's wait may have the reactor's ring do, where
// | it has one (Linux's io_uring): a socket's `recv`, `send` and
// | `accept`, a file's `read` and `write` at its position, and a path
// | opened, looked at, renamed, a file closed.
enum { HEFF_OP_RECV, HEFF_OP_SEND, HEFF_OP_ACCEPT, HEFF_OP_READ, HEFF_OP_WRITE,
       HEFF_OP_OPEN, HEFF_OP_CLOSE, HEFF_OP_STATX, HEFF_OP_RENAME };

#ifdef HEFF_URING
static const uint8_t heff_ops_ring[] = { IORING_OP_RECV, IORING_OP_SEND, IORING_OP_ACCEPT, IORING_OP_READ,
                                         IORING_OP_WRITE, IORING_OP_OPENAT, IORING_OP_CLOSE, IORING_OP_STATX,
                                         IORING_OP_RENAMEAT };
#endif

// | Whether the operation `op` is the ring's to do here: in a group, the
// | thread's reactor a ring that has it.
HEFF_FN int heff_ring_can(int op) {
#ifdef HEFF_URING
  if (!heff_group || !heff_ring_on || op < 0 || op > HEFF_OP_RENAME) return 0;
  int o = heff_ops_ring[op];
  return (heff_ring.ops[o / 64] >> (o % 64)) & 1;
#else
  (void)op;
  return 0;
#endif
}

// | The wait `w`'s operation `op` begun by the ring, on its `fd` -- of a
// | descriptor its way's slot, reading or writing, a path's the second
// | of a rename's -- with `addr` and `len` as the operation has them
// | (`recv`'s and `read`'s memory, `send`'s and `write`'s bytes, a path
// | and its flags and mode, statx's path, flags, mask and room), all of
// | it the kernel's until its completion comes back: whether it was
// | begun -- not where the ring cannot (`heff_ring_can`, or a wait pending
// | stepped by the reactor), or the slot is
// | another wait's, which the provider then waits for as before.  The
// | step that began it says HEFF_STEP_WAIT, and is taken again once it is
// | done, its result there (`heff_ring_took`); a wait ended before then
// | -- its deadline, its descriptor closed -- is answered, and one let go
// | of let go of, only after it.
HEFF_FN int heff_ring_op(HeffWait *w, int op, int fd, const void *addr, uint64_t len, uint64_t off, uint32_t flags,
                         const void *addr2) {
#ifdef HEFF_URING
  // (in a group, or a wait pending in one, stepped by the reactor)
  if (!heff_ring_on || op < 0 || op > HEFF_OP_RENAME || !(heff_group || w->linked)) return 0;
  if (!((heff_ring.ops[heff_ops_ring[op] / 64] >> (heff_ops_ring[op] % 64)) & 1)) return 0;
  short way = op == HEFF_OP_SEND || op == HEFF_OP_WRITE ? 2 : 1;
  // (a path's operation: no descriptor's slot, the wait's own)
  int slot = fd >= 0 && op != HEFF_OP_OPEN && op != HEFF_OP_STATX && op != HEFF_OP_RENAME && op != HEFF_OP_CLOSE;
  if (slot) {
    size_t at = (size_t)fd * 2 + (size_t)(way - 1);
    heff_evq_room(at);
    if (heff_evq_owner[at] && heff_evq_owner[at] != w) return 0;
    if (heff_evq_owner[at] == w && heff_ring_armed[at]) heff_evq_tell(fd, way, 0);
    heff_evq_owner[at] = w;
    heff_ring_armed[at] = HEFF_RING_OP;
  }
  if (w->linked && w->kway == 3) heff_npolled--;
  struct io_uring_sqe *e = heff_ring_sqe();
  e->opcode = heff_ops_ring[op];
  e->fd = op == HEFF_OP_OPEN || op == HEFF_OP_STATX || op == HEFF_OP_RENAME ? AT_FDCWD : fd;
  e->addr = (uint64_t)(uintptr_t)addr;
  e->len = (uint32_t)len;
  e->off = off;
  e->rw_flags = (int)flags;
  if (op == HEFF_OP_STATX) e->off = (uint64_t)(uintptr_t)addr2;
  if (op == HEFF_OP_RENAME) {
    e->len = (uint32_t)AT_FDCWD;
    e->addr2 = (uint64_t)(uintptr_t)addr2;
  }
  e->user_data = HEFF_RING_OP | (uint64_t)(uintptr_t)w;
  heff_ring_push();
  heff_ring_inflight++;
  w->kfd = slot ? fd : -2;
  w->kway = slot ? (short)(3 + way) : 4;
  w->ring_got = 0;
  return 1;
#else
  (void)w; (void)op; (void)fd; (void)addr; (void)len; (void)off; (void)flags; (void)addr2;
  return 0;
#endif
}

// | Whether the wait's operation of the ring's has come back, not yet
// | looked at: its result at `*res` (as the system call's, or -errno),
// | once.
HEFF_FN int heff_ring_took(HeffWait *w, int32_t *res) {
  if (!w->ring_got) return 0;
  w->ring_got = 0;
  *res = w->ring_res;
  return 1;
}

// (the group open of that number, or NULL)
HEFF_FN HeffGroup *heff_group_of(int64_t g) {
  for (size_t i = 0; i < heff_ngroups; i++)
    if (heff_groups_open[i].id == g) return &heff_groups_open[i];
  return NULL;
}

// (the group's pending, and its root's, `by` more)
HEFF_FN void heff_group_pending(int64_t id, int64_t by) {
  HeffGroup *g = heff_group_of(id);
  if (!g) return;
  g->pending += by;
  if (g->root) {
    HeffGroup *r = heff_group_of(g->root);
    if (r) r->pending += by;
  }
}

// (whether wait `a` is due before `b`: the nearer deadline, or, of the
// same, the one that began to wait first)
static inline int heff_heap_before(HeffWait *a, HeffWait *b) {
  return a->until < b->until || (a->until == b->until && a->seq < b->seq);
}

static inline void heff_heap_put(size_t i, HeffWait *w) {
  heff_dues[i] = w;
  w->heapat = i + 1;
}

// (the wait at `i` moved up the heap, or down, to where it is due)
HEFF_FN void heff_heap_fix(size_t i) {
  HeffWait *w = heff_dues[i];
  while (i > 0 && heff_heap_before(w, heff_dues[(i - 1) / 2])) {
    heff_heap_put(i, heff_dues[(i - 1) / 2]);
    i = (i - 1) / 2;
  }
  for (;;) {
    size_t c = 2 * i + 1;
    if (c >= heff_ndues) break;
    if (c + 1 < heff_ndues && heff_heap_before(heff_dues[c + 1], heff_dues[c])) c++;
    if (!heff_heap_before(heff_dues[c], w)) break;
    heff_heap_put(i, heff_dues[c]);
    i = c;
  }
  heff_heap_put(i, w);
}

// (the wait among the deadlines)
HEFF_FN void heff_heap_add(HeffWait *w) {
  if (heff_ndues == heff_capdues) {
    size_t cap = heff_capdues ? heff_capdues * 2 : 64;
    HeffWait **m = heff_alloc(cap * sizeof *m);
    if (heff_ndues) memcpy(m, heff_dues, heff_ndues * sizeof *m);
    free(heff_dues);
    heff_dues = m;
    heff_capdues = cap;
  }
  heff_heap_put(heff_ndues++, w);
  heff_heap_fix(heff_ndues - 1);
}

// (the wait no longer among the deadlines, where it is)
HEFF_FN void heff_heap_remove(HeffWait *w) {
  if (!w->heapat) return;
  size_t i = w->heapat - 1;
  w->heapat = 0;
  HeffWait *last = heff_dues[--heff_ndues];
  if (i == heff_ndues) return;
  heff_heap_put(i, last);
  heff_heap_fix(i);
}

// (the wait done: where it is pending, among those the turn wakes, once)
// (the wait, done, among those the turn wakes, where it is pending)
HEFF_FN void heff_wait_ready_put(HeffWait *w) {
  if (!w->linked || w->readyat) return;
  if (heff_nready == heff_capready) {
    size_t cap = heff_capready ? heff_capready * 2 : 64;
    HeffWait **m = heff_alloc(cap * sizeof *m);
    if (heff_nready) memcpy(m, heff_ready, heff_nready * sizeof *m);
    free(heff_ready);
    heff_ready = m;
    heff_capready = cap;
  }
  heff_ready[heff_nready++] = w;
  w->readyat = heff_nready;
}

// (the wait done: where it is pending, among those the turn wakes, once
// -- where an operation of the ring's is its, once that has come back,
// taken back first)
HEFF_FN void heff_wait_finish(HeffWait *w) {
  if (w->done) return;
  w->done = 1;
#ifdef HEFF_URING
  if (w->linked && (w->kway == 4 || w->kway == 5)) {
    heff_ring_op_cancel(w);
    w->kway += 2;
    return;
  }
  if (w->linked && w->kway >= 6 && w->kway <= 8) return;
#endif
  heff_wait_ready_put(w);
}

// (the wait no longer among the done, where it is)
static inline void heff_ready_remove(HeffWait *w) {
  if (!w->readyat) return;
  size_t i = w->readyat - 1;
  w->readyat = 0;
  HeffWait *last = heff_ready[--heff_nready];
  if (i == heff_nready) return;
  heff_ready[i] = last;
  last->readyat = i + 1;
}

HEFF_FN void heff_wait_link(HeffWait *w) {
  w->prev = heff_waits_last;
  w->next = NULL;
  if (heff_waits_last) heff_waits_last->next = w;
  else heff_waits_first = w;
  heff_waits_last = w;
  w->linked = 1;
  w->seq = ++heff_wait_seq;
  w->heapat = w->readyat = 0;
  if (w->until) heff_heap_add(w);
#ifndef _WIN32
  // (marked first, then the count read: a thread closing its descriptor
  // sees the mark, or this the close counted)
  w->marked = heff_mark_on(w->fd) ? w->fd : -1;
#endif
#ifdef HEFF_EVQ
  heff_evq_add(w);
#endif
  w->epoch = __atomic_load_n(&heff_closed_epoch, __ATOMIC_SEQ_CST);
  heff_group_pending(w->group, 1);
}

HEFF_FN void heff_wait_unlink(HeffWait *w) {
  if (w->prev) w->prev->next = w->next;
  else heff_waits_first = w->next;
  if (w->next) w->next->prev = w->prev;
  else heff_waits_last = w->prev;
  w->prev = w->next = NULL;
  w->linked = 0;
  heff_heap_remove(w);
  heff_ready_remove(w);
#ifndef _WIN32
  heff_mark_off(w->marked);
#endif
#ifdef HEFF_EVQ
  heff_evq_delete(w);
#endif
  heff_group_pending(w->group, -1);
}

// (a wait done, as its source says: given its answer at the next turn)
HEFF_FN void heff_wait_done(HeffWait *w) { heff_wait_finish(w); }

// (a wait on a descriptor closed: done, EBADF; and, in a ring, its poll
// taken out now, so that the kernel lets go of the descriptor as it is
// closed, not at the next turn's call)
static inline void heff_wait_closed(HeffWait *w) {
  w->err = EBADF;
  heff_wait_finish(w);
#ifdef HEFF_URING
  if (heff_ring_on && (w->kway == 1 || w->kway == 2)) heff_evq_delete(w);
#endif
}

// (this thread's waits on `fd` begun before the count `before`: done,
// EBADF -- whether any was.  Found by its marks, where every wait on it
// is in the event queue, else by every pending wait looked at; none where
// none is marked)
HEFF_FN int heff_fd_waits_found_closed(int fd, int64_t before) {
  int some = 0;
#ifndef _WIN32
  if (fd >= 0 && heff_mark && (size_t)fd < (size_t)HEFF_MARK_CHUNK * HEFF_MARK_CHUNKS) {
    int32_t on = (size_t)fd < heff_capmarks_mine ? heff_marks_mine[fd] : 0;
    if (on == 0) return 0;
#ifdef HEFF_EVQ
    int32_t found = 0;
    for (size_t way = 0; way < 2; way++) {
      HeffWait *w = heff_evq_whose(fd, (short)(way + 1));
      if (!w || w->marked != fd) continue;
      found++;
      if (!w->done && w->epoch < before) {
        heff_wait_closed(w);
        some = 1;
      }
    }
    if (found == on) return some;
#endif
  }
#endif
  for (HeffWait *w = heff_waits_first; w; w = w->next)
    if (w->fd == fd && !w->done && w->epoch < before) {
      heff_wait_closed(w);
      some = 1;
    }
  return some;
}

// (and, in a ring, the polls taken out given to the kernel at once)
HEFF_FN int heff_fd_waits_closed(int fd, int64_t before) {
  int some = heff_fd_waits_found_closed(fd, before);
#ifdef HEFF_URING
  if (some && heff_ring_on) heff_ring_enter(0);
#endif
  return some;
}

// (the waits on the descriptor `fd`, which is being closed: done, their
// answers that it was -- EBADF -- not a poll of a descriptor that may be
// another's by then)
HEFF_FN void heff_reactor_closing(int fd) {
  heff_fd_waits_closed(fd, INT64_MAX);
#ifndef _WIN32
  // (and every other thread's that waits on it, by that thread: told so,
  // and woken -- where none is marked, none told; a wait begun after the
  // marks are read is as one begun after the count: one on a descriptor
  // closed)
  if (fd < 0) return;
  uint64_t *word = heff_marks_of(fd, 0);
  int beyond = (size_t)fd >= (size_t)HEFF_MARK_CHUNK * HEFF_MARK_CHUNKS;
  uint64_t own = heff_mark == HEFF_MARK_ANY ? 0 : heff_mark;
  uint64_t others = beyond ? HEFF_MARK_ANY : word ? __atomic_load_n(word, __ATOMIC_SEQ_CST) & ~own : 0;
  if (!others) return;
  pthread_mutex_lock(&heff_wakers_mu);
  if (heff_nwakers) {
    int64_t e = __atomic_add_fetch(&heff_closed_epoch, 1, __ATOMIC_SEQ_CST);
    heff_closed[e % HEFF_CLOSED_KEPT].fd = fd;
    heff_closed[e % HEFF_CLOSED_KEPT].epoch = e;
    if (word) others |= __atomic_load_n(word, __ATOMIC_SEQ_CST) & ~own;
    char b = 0;
    if (others & HEFF_MARK_ANY) {
      for (size_t i = 0; i < heff_nwakers; i++)
        if (!heff_wake_made || heff_wakers[i].fd != heff_wake_fds[1]) {
          ssize_t r = write(heff_wakers[i].fd, &b, 1);
          (void)r;
        }
    } else
      for (int i = 0; i < 63; i++)
        if (others & heff_marks_given & ((uint64_t)1 << i)) {
          ssize_t r = write(heff_marks_pipe[i], &b, 1);
          (void)r;
        }
  }
  pthread_mutex_unlock(&heff_wakers_mu);
#endif
}

#ifndef _WIN32
// (the waits of this thread's on descriptors another thread closed since
// it last looked, each begun before it was closed: done, EBADF -- where
// any was, the poll to come not waiting)
HEFF_FN int heff_reactor_closed_elsewhere(void) {
  int64_t e = __atomic_load_n(&heff_closed_epoch, __ATOMIC_ACQUIRE);
  if (e == heff_closed_seen) return 0;
  int some = 0;
  pthread_mutex_lock(&heff_wakers_mu);
  e = heff_closed_epoch;
  int64_t from = heff_closed_seen + 1;
  if (e - from >= HEFF_CLOSED_KEPT) from = e - HEFF_CLOSED_KEPT + 1;
  for (int64_t k = from; k <= e; k++) {
    HeffClosed c = heff_closed[k % HEFF_CLOSED_KEPT];
    if (heff_fd_waits_closed(c.fd, c.epoch)) some = 1;
  }
  pthread_mutex_unlock(&heff_wakers_mu);
  heff_closed_seen = e;
  return some;
}
#endif

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
    if (w->fd < 0 && !w->until) heff_fail("an outside operation waits for no descriptor, and no scheduler is there to wait for it");
    int ms = -1;
    if (w->until) {
      int64_t left = w->until - heff_now_ms();
      ms = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
    }
    struct pollfd p = { w->fd, w->events, 0 };
    int r = poll(&p, w->fd >= 0 ? 1 : 0, ms);
    if (r < 0) {
      if (errno == EINTR) continue;
      w->err = errno;
      break;
    }
    if (r == 0) {
      if (w->until && heff_now_ms() >= w->until) {
        w->err = ETIMEDOUT;
        break;
      }
      continue;
    }
    if (w->step(w) == HEFF_STEP_DONE) break;
  }
#else
  if (w->fd < 0 && w->until) {
    int64_t left = w->until - heff_now_ms();
    if (left > 0) Sleep((DWORD)left);
  }
#endif
  V x = heff_answer(f, w->answer(w));
  return heff_tail(k, 1, &x);
}

// (what was started in group `g` come to its value `v`: one of the
// group's finished -- or, the group closed meanwhile, let go of)
HEFF_FN void heff_finished_put(int64_t g, V v) {
  HeffGroup *gr = heff_group_of(g);
  if (gr && gr->root) {
    g = gr->root;
    gr = heff_group_of(g);
  }
  if (!gr) {
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

// (what a failure of a step of what group `g` started is finished as:
// its root's function, or 0)
HEFF_FN V heff_group_failed(int64_t g) {
  HeffGroup *gr = heff_group_of(g);
  if (gr && gr->root) gr = heff_group_of(gr->root);
  return gr ? gr->failed : 0;
}

// (`k` given `x`, a step of what group `g` started, run with the group
// as `heff_group`: what it comes to, at `*r`.  Where the group has a
// function for failures, under `heff_try`: one that abandons is
// finished as that function's value, given why, and 1 is the answer;
// else 0.  A failure ends the program, whatever the group says
// `k` and `x` consumed)
HEFF_FN int heff_group_step(int64_t g, V k, V x, V *r) {
  int64_t up = heff_group;
  heff_group = g;
  V failed = heff_group_failed(g);
  if (!failed) {
    *r = heff_tr(heff_app1(k, x));
    heff_group = up;
    return 0;
  }
  const char *why = heff_try(k, x, r);
  heff_group = up;
  if (!why) return 0;
  heff_finished_put(g, heff_tr(heff_app1(heff_dup(failed), heff_str_text(why))));
  return 1;
}

// (what group `g` started, the rest of it run in the group, from `k`
// given `x`: to its end, its value finished, or to a wait, pending; or,
// where the group says what a failure is, failed, that finished)
HEFF_FN void heff_reactor_run(int64_t g, V k, V x) {
  V r;
  if (heff_group_step(g, k, x, &r)) return;
  if (r != HEFF_PENDING) heff_finished_put(g, r);
}

// (group `g` among those woken by this thread's pipe)
HEFF_FN void heff_waker_add(int64_t g) {
#ifndef _WIN32
  if (!heff_wake_made) {
    if (pipe(heff_wake_fds) != 0) heff_fail("the reactor's pipe could not be made");
    for (int i = 0; i < 2; i++) {
      fcntl(heff_wake_fds[i], F_SETFL, fcntl(heff_wake_fds[i], F_GETFL) | O_NONBLOCK);
      fcntl(heff_wake_fds[i], F_SETFD, FD_CLOEXEC);
    }
    heff_wake_made = 1;
#ifdef HEFF_EVQ
    heff_evq_open();
#endif
  }
  pthread_mutex_lock(&heff_wakers_mu);
  if (heff_nwakers == heff_capwakers) {
    size_t cap = heff_capwakers ? heff_capwakers * 2 : 16;
    HeffWaker *m = heff_alloc(cap * sizeof *m);
    if (heff_nwakers) memcpy(m, heff_wakers, heff_nwakers * sizeof *m);
    free(heff_wakers);
    heff_wakers = m;
    heff_capwakers = cap;
  }
  heff_wakers[heff_nwakers].id = g;
  heff_wakers[heff_nwakers].fd = heff_wake_fds[1];
  heff_nwakers++;
  if (!heff_mark) {
    // (the reactor's bit, and its pipe by it; or, none left, the last)
    heff_mark = HEFF_MARK_ANY;
    for (int i = 0; i < 63; i++)
      if (!(heff_marks_given & ((uint64_t)1 << i))) {
        heff_marks_given |= (uint64_t)1 << i;
        heff_marks_pipe[i] = heff_wake_fds[1];
        heff_mark = (uint64_t)1 << i;
        break;
      }
  }
  pthread_mutex_unlock(&heff_wakers_mu);
#else
  (void)g;
#endif
}

// (group `g` no longer woken; with the thread's last group, its pipe
// closed)
HEFF_FN void heff_waker_remove(int64_t g) {
#ifndef _WIN32
  pthread_mutex_lock(&heff_wakers_mu);
  for (size_t i = 0; i < heff_nwakers; i++)
    if (heff_wakers[i].id == g) {
      heff_wakers[i] = heff_wakers[--heff_nwakers];
      break;
    }
  if (heff_ngroups == 0 && heff_wake_made) {
    close(heff_wake_fds[0]);
    close(heff_wake_fds[1]);
    heff_wake_made = 0;
    heff_woken = 0;
    // (its bit given back: no wait of its is pending, none marked)
    if (heff_mark != HEFF_MARK_ANY) heff_marks_given &= ~heff_mark;
    heff_mark = 0;
    // (and its deadlines' and its done's room: none is pending)
    free(heff_dues);
    free(heff_ready);
    heff_dues = heff_ready = NULL;
    heff_ndues = heff_capdues = heff_nready = heff_capready = 0;
#ifdef HEFF_EVQ
    heff_evq_close();
#endif
  }
  pthread_mutex_unlock(&heff_wakers_mu);
#else
  (void)g;
#endif
}

// | The thread whose group `g`, a root, is woken, from any thread: its
// | reactor's next turn, or the one it is waiting in, comes back, and so
// | does `heff_reactor_finished`, with none finished where none is.
HEFF_FN void heff_reactor_wake_group(int64_t g) {
#ifndef _WIN32
  pthread_mutex_lock(&heff_wakers_mu);
  for (size_t i = 0; i < heff_nwakers; i++)
    if (heff_wakers[i].id == g) {
      char b = 0;
      ssize_t r = write(heff_wakers[i].fd, &b, 1);
      (void)r;
      break;
    }
  pthread_mutex_unlock(&heff_wakers_mu);
#else
  (void)g;
#endif
}

// (a group of this thread's, of a number no other has, within the root
// `root` or 0)
HEFF_FN int64_t heff_group_made(int64_t root) {
  if (heff_ngroups == heff_capgroups) {
    size_t cap = heff_capgroups ? heff_capgroups * 2 : 8;
    HeffGroup *m = heff_alloc(cap * sizeof *m);
    if (heff_ngroups) memcpy(m, heff_groups_open, heff_ngroups * sizeof *m);
    free(heff_groups_open);
    heff_groups_open = m;
    heff_capgroups = cap;
  }
  int64_t g = __atomic_add_fetch(&heff_last_group, 1, __ATOMIC_RELAXED);
  heff_groups_open[heff_ngroups].id = g;
  heff_groups_open[heff_ngroups].pending = 0;
  heff_groups_open[heff_ngroups].root = root;
  heff_groups_open[heff_ngroups].failed = 0;
  heff_groups_open[heff_ngroups].giving = 0;
  heff_ngroups++;
  // (a root alone is woken: one within it is waited on with it)
  if (!root) heff_waker_add(g);
  return g;
}

// | A group of its own, for a scheduler: its number.  `failed`, where it
// | is not 0, is what a step of a computation started in it that
// | abandons is finished as, given why, the others going on (kept by the
// | group, and let go of when it is closed); where it is 0, the abandon
// | is the program's.  A failure is the program's end either way.
HEFF_FN int64_t heff_reactor_open(V failed) {
  int64_t g = heff_group_made(0);
  heff_group_of(g)->failed = failed;
  return g;
}

// | A group within group `g`: what is started in it finishes among the
// | finished of `g`'s root, and is pending among its pending.
HEFF_FN int64_t heff_reactor_open_within(int64_t g) {
  HeffGroup *gr = heff_group_of(g);
  if (!gr) heff_fail("a group within one that is not open");
  return heff_group_made(gr->root ? gr->root : g);
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
static int heff_by_seq(const void *x, const void *y) {
  int64_t a = (*(HeffWait *const *)x)->seq, b = (*(HeffWait *const *)y)->seq;
  return a < b ? -1 : a > b;
}

HEFF_FN void heff_reactor_wake(void) {
  size_t n = heff_nready;
  if (!n) return;
  HeffWait *few[64];
  HeffWait **done = n <= 64 ? few : heff_alloc(n * sizeof *done);
  memcpy(done, heff_ready, n * sizeof *done);
  heff_nready = 0;
  if (n > 1) qsort(done, n, sizeof *done, heff_by_seq);
  for (size_t i = 0; i < n; i++) {
    done[i]->readyat = 0;
    heff_wait_unlink(done[i]);
  }
  for (size_t i = 0; i < n; i++) {
    HeffWait *w = done[i];
    V f = w->f, k = w->k;
    int64_t g = w->group;
    V x = heff_answer(f, w->answer(w));
    // (its group closed meanwhile, by what one woken before it ran: let
    // go of, as one pending there was, not run on in a group no one
    // closes again -- its waits would be pending in none, a job's or a
    // ring's operation of theirs outliving the thread)
    if (!heff_group_of(g)) {
      heff_drop(x);
      heff_drop(k);
      continue;
    }
    heff_reactor_run(g, k, x);
  }
  if (done != few) free(done);
}

// (the waits whose deadline has passed: done, timed out -- a sleep's
// answer minds it not)
HEFF_FN void heff_reactor_expire(void) {
  if (!heff_ndues) return;
  int64_t now = heff_now_ms();
  while (heff_ndues && now >= heff_dues[0]->until) {
    HeffWait *w = heff_dues[0];
    heff_heap_remove(w);
    if (!w->done) {
      if (!w->err) w->err = ETIMEDOUT;
      heff_wait_finish(w);
    }
  }
}

// (how long the turn may wait, as the waits done and the nearest
// deadline say, `ms` before: -1 for ever)
static inline int heff_turn_ms(int ms) {
  if (heff_nready) return 0;
  if (heff_ndues) {
    int64_t now = heff_now_ms(), until = heff_dues[0]->until;
    int64_t left = until > now ? until - now : 0;
    if (ms < 0 || left < ms) ms = left > INT_MAX ? INT_MAX : (int)left;
  }
  return ms;
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

// (whether the wait's descriptor is polled by the turn: where there is
// no event queue, every pending one's; else those it was not given)
static inline int heff_wait_polled(HeffWait *w) {
#ifdef HEFF_EVQ
  return !w->done && w->fd >= 0 && w->kway == 3;
#else
  return !w->done && w->fd >= 0;
#endif
}

// (the wait stepped, its descriptor ready: done, or waiting again, maybe
// on another)
static inline void heff_wait_ready(HeffWait *w) {
  if (!w->done && w->step(w) == HEFF_STEP_DONE) heff_wait_finish(w);
  if (w->fd != w->marked) {
    // (a step on to another descriptor: its reactor marked so)
    heff_mark_off(w->marked);
    w->marked = heff_mark_on(w->fd) ? w->fd : -1;
  }
#ifdef HEFF_EVQ
  heff_evq_again(w);
#endif
}

#ifdef HEFF_EVQ
// (an event of the queue's, `fd` ready `way`: its wait stepped -- not
// one whose way an earlier step of this read took out)
static inline void heff_evq_fire(int fd, short way) {
  HeffWait *w = heff_evq_whose(fd, way);
  if (w && w->kway == way && w->kfd == fd) heff_wait_ready(w);
}

// (the queue's ready read, not waiting, and each ready wait stepped)
HEFF_FN void heff_evq_ready(void) {
  for (;;) {
#ifdef HEFF_KQUEUE
    struct kevent got[256];
    struct timespec zero = { 0, 0 };
    int r = kevent(heff_evq, NULL, 0, got, 256, &zero);
    if (r <= 0) break;
    for (int i = 0; i < r; i++) heff_evq_fire((int)got[i].ident, got[i].filter == EVFILT_READ ? 1 : 2);
#else
#ifdef HEFF_URING
    if (heff_ring_on) {
      // (each poll that fired, not one taken out or told again since;
      // and each operation done: its wait stepped with its result, or,
      // done meanwhile, ended, or, let go of, let go of now)
      heff_ring_collect();
      for (size_t i = 0; i < heff_ring_ngot; i++) {
        uint64_t ud = heff_ring_got[i].ud;
        if (ud == HEFF_RING_NONE) continue;
        if (ud & HEFF_RING_OP) {
          HeffWait *w = (HeffWait *)(uintptr_t)(ud & ~HEFF_RING_OP);
          short was = w->kway;
          heff_ring_inflight--;
          w->kway = 0;
          w->ring_res = heff_ring_got[i].res;
          w->ring_got = 1;
          if (was == 8) w->cancel(w);
          else {
            heff_ring_op_slot_free(w, (short)((was - 4) % 2 + 1));
            if (was >= 6) heff_wait_ready_put(w);
            else heff_wait_ready(w);
          }
          continue;
        }
        size_t at = (uint32_t)ud;
        if (at >= heff_capevq_owner || heff_ring_armed[at] != ud) continue;
        heff_ring_armed[at] = 0;
        heff_evq_fire((int)(at / 2), (short)(at % 2 + 1));
      }
      heff_ring_ngot = 0;
      break;
    }
#endif
    struct epoll_event got[256];
    int r = epoll_wait(heff_evq, got, 256, 0);
    if (r <= 0) break;
    for (int i = 0; i < r; i++) {
      int fd = (int)got[i].data.u64;
      uint32_t e = got[i].events;
      // (a hang-up or an error is for both ways, as poll has it)
      if (e & (EPOLLIN | EPOLLHUP | EPOLLERR)) heff_evq_fire(fd, 1);
      if (e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) heff_evq_fire(fd, 2);
    }
#endif
    if (r < 256) break;
  }
}
#endif

HEFF_FN void heff_jobs_collect(void);
HEFF_FN void heff_jobs_end(void);

// (one turn: every pending wait's descriptor and every source's polled
// -- for as long as they say, where `wait`, else not waiting -- each
// ready stepped, each whose job has come back too, and those done woken)
HEFF_FN void heff_reactor_turn(int wait) {
  size_t n = 0;
  int ms = wait ? -1 : 0;
  if (heff_reactor_closed_elsewhere()) ms = 0;
  ms = heff_turn_ms(ms);
  // (the waits polled here: where there is an event queue, those counted
  // alone, none mostly, so the pending not looked at)
#ifdef HEFF_EVQ
  if (heff_npolled)
#endif
    for (HeffWait *w = heff_waits_first; w; w = w->next)
      if (heff_wait_polled(w)) n++;
  size_t mine = n;
  int ring = 0;
#ifdef HEFF_URING
  ring = heff_ring_on;
#endif
  int piped = heff_wake_made;
#ifdef HEFF_EVQ
  if (heff_evq_made) n++;
#endif
  if (piped) n++;
  for (HeffSource *s = heff_sources; s; s = s->next) {
    int t = -1;
    s->n = s->fds(NULL, 0, &t);
    n += (size_t)s->n;
  }
  heff_polled_room(n);
  n = 0;
  if (mine)
    for (HeffWait *w = heff_waits_first; w; w = w->next)
      if (heff_wait_polled(w)) {
        heff_polled[n].fd = w->fd;
        heff_polled[n].events = w->events;
        heff_polled[n].revents = 0;
        heff_polled_waits[n] = w;
        n++;
      }
#ifdef HEFF_EVQ
  // (the event queue as it is now: a step below may make it)
  size_t q = n;
  int q_polled = heff_evq_made;
  if (q_polled) {
    heff_polled[n].fd = heff_evq;
    heff_polled[n].events = POLLIN;
    heff_polled[n].revents = 0;
    n++;
  }
#endif
  size_t wake = n;
  if (piped) {
    heff_polled[n].fd = heff_wake_fds[0];
    heff_polled[n].events = POLLIN;
    heff_polled[n].revents = 0;
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
#ifdef HEFF_URING
  // (what the ring was told given to the kernel, then the ring polled
  // with the others, its completions there where it is ready)
  if (ring) heff_ring_enter(0);
#endif
  int r = poll(heff_polled, (nfds_t)n, ms);
  if (r < 0 && errno != EINTR) heff_fail("the reactor's poll failed");
  if (r < 0) for (size_t i = 0; i < n; i++) heff_polled[i].revents = 0;
  for (size_t i = 0; i < mine; i++)
    if (heff_polled[i].revents) heff_wait_ready(heff_polled_waits[i]);
#ifdef HEFF_EVQ
  if (q_polled && (ring || heff_polled[q].revents)) heff_evq_ready();
#endif
  if (piped && heff_polled[wake].revents) {
    // (a group woken, by a 0; a job come back, by a 1, which wakes none)
    char b[64];
    ssize_t got;
    while ((got = read(heff_wake_fds[0], b, sizeof b)) > 0)
      if (memchr(b, 0, (size_t)got)) heff_woken = 1;
  }
  heff_jobs_collect();
  for (HeffSource *s = heff_sources; s; s = s->next) s->ready(heff_polled + s->at, s->n);
  heff_reactor_expire();
  heff_reactor_wake();
}
#else
// (no descriptor is polled: the deadlines alone are waited for)
HEFF_FN void heff_reactor_turn(int wait) {
  int ms = heff_turn_ms(wait ? -1 : 0);
  if (ms < 0) heff_fail("the reactor waits, with nothing to wait for");
  if (ms > 0) Sleep((DWORD)ms);
  heff_reactor_expire();
  heff_reactor_wake();
}
#endif

// | Group `g`'s finished, since it was last asked, in the order they
// | were: where `wait`, waiting for one at least while any of it is
// | pending; none where none is.
HEFF_FN void heff_giving_taken(void);

HEFF_FN V heff_reactor_finished(int64_t g, int wait) {
  for (;;) {
    heff_giving_taken();
    int some = 0;
    for (size_t i = 0; i < heff_nfinished && !some; i++) some = heff_finished[i].group == g;
    HeffGroup *gr = heff_group_of(g);
    if (some || !gr || gr->pending == 0) break;
#ifndef _WIN32
    if (heff_woken) break;
#endif
    heff_reactor_turn(wait);
    if (!wait) break;
  }
#ifndef _WIN32
  heff_woken = 0;
#endif
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

// | How many of group `g`'s computations started are pending: in a
// | root, those of the groups within it too; 0 where it is not open.
HEFF_FN int64_t heff_reactor_pending(int64_t g) {
  HeffGroup *gr = heff_group_of(g);
  return gr ? gr->pending : 0;
}

HEFF_FN void heff_giving_closed(int64_t g);

#ifdef HEFF_URING
// (every operation of the ring's come back -- each wait's let go of, as
// none is pending -- so that the ring is let go of with nothing of the
// program's the kernel's)
HEFF_FN void heff_ring_drain(void) {
  heff_ring_enter(0);
  while (heff_ring_inflight) {
    heff_ring_enter(-1);
    heff_evq_ready();
  }
}
#endif

// | Group `g` closed: what is pending in it let go of, undone, and what
// | of it is finished and not asked for.
HEFF_FN void heff_reactor_close(int64_t g) {
  heff_giving_closed(g);
  for (HeffWait *w = heff_waits_first, *next; w; w = next) {
    next = w->next;
    // (and one of a group closed before, where any is: pending in none)
    HeffGroup *in = w->group == g ? NULL : heff_group_of(w->group);
    if (in && in->root != g) continue;
    heff_wait_unlink(w);
    V f = w->f, k = w->k;
    // (one whose operation the ring has, let go of as it comes back)
    if (w->kway != 8) w->cancel(w);
    heff_drop(f);
    heff_drop(k);
  }
#ifdef HEFF_URING
  // (the polls and operations taken out given to the kernel now: it
  // lets go of the descriptors closed as they are; with the thread's
  // last group, every operation come back first)
  if (heff_ring_on) {
    int rest = 0;
    for (size_t i = 0; i < heff_ngroups; i++)
      if (heff_groups_open[i].id != g && heff_groups_open[i].root != g) rest = 1;
    if (rest) heff_ring_enter(0);
    else heff_ring_drain();
  }
#endif
#ifndef _WIN32
  // (with the thread's last group, every job given up come back first)
  int others = 0;
  for (size_t i = 0; i < heff_ngroups; i++)
    if (heff_groups_open[i].id != g && heff_groups_open[i].root != g) others = 1;
  if (!others) heff_jobs_end();
#endif
  size_t kept = 0;
  for (size_t i = 0; i < heff_nfinished; i++) {
    if (heff_finished[i].group == g) heff_drop(heff_finished[i].v);
    else heff_finished[kept++] = heff_finished[i];
  }
  heff_nfinished = kept;
  for (size_t i = 0; i < heff_ngroups;) {
    int64_t id = heff_groups_open[i].id, root = heff_groups_open[i].root;
    if (id == g || root == g) {
      if (heff_groups_open[i].failed) heff_drop(heff_groups_open[i].failed);
      heff_groups_open[i] = heff_groups_open[--heff_ngroups];
      // (a root's waker taken out; and, with the thread's last group,
      // its pipe closed)
      if (!root || heff_ngroups == 0) heff_waker_remove(id);
    } else i++;
  }
}

// Calls that block, on threads of the runtime's
// ---------------------------------------------------------------------
//
// A call that waits with no descriptor to poll -- a read of a regular
// file the disk has not yet given, where no ring does it; a call of a
// library that has no other way, a database client's -- is a job
// (`HeffJob`), done by a thread of the runtime's while the reactor's
// thread goes on.  Where a group is being started in, a wait's step
// hands its job to `heff_job` in place of a descriptor to wait for: the
// job is called (`run`) on a thread of the runtime's, and once it has
// returned, the wait is stepped again by its own reactor at its next
// turn, as one whose descriptor is ready is -- done, or waiting again,
// on another job maybe.  A job touches nothing of heff's -- no value of
// the program's, no reactor, nothing of a thread's own: it reads and
// writes what its wait holds for it, a buffer and the result it leaves
// there.  Where no group is being started in, or no thread can be made
// for it, `heff_job` says no, and the call is made there and then.
//
// The threads are `HEFF_JOB_THREADS` at most, each made as a job comes
// and none is idle, kept after, waiting for the next, signals not
// theirs; jobs beyond them wait their turn in the queue, the first come
// the first begun.  A job done is put among its reactor's come back,
// under the reactor's lock of jobs -- not the queue's, so that the
// threads of the jobs of several reactors' wait little for each other --
// and the reactor is woken by a byte written to its pipe as the first of
// them comes: a 1, where a group is woken by a 0; the turn takes them
// all.  A wait let go of undone -- its group closed, or it done
// otherwise, its deadline passed -- gives up its job
// (`heff_job_give_up`): one not yet begun is taken out of the queue, and
// one come back out of its reactor's, the provider's to let go of then;
// one running runs to its end, and is let go of (`let_go`) once it has
// come back, by the thread that began it -- so that what it holds of
// that thread's is let go of there.  A thread's last group closed waits
// for those to come back first, as it waits for the ring's operations.
// Nothing of this is made where no job is: no thread, no lock taken.

#ifndef _WIN32
#ifndef HEFF_JOB_THREADS
#define HEFF_JOB_THREADS 64
#endif
#define HEFF_JOB_STACK ((size_t)1 << 20)

typedef struct HeffJob HeffJob;
typedef struct HeffJobs HeffJobs;
struct HeffJob {
  void (*run)(HeffJob *);        // (the call, on a thread of the runtime's)
  void (*let_go)(HeffJob *);     // (it let go of, given up as it ran: by
                                 //  the thread that began it, once it has
                                 //  come back)
  // (the runtime's)
  HeffWait *w;
  HeffJobs *of;                  // (its reactor's)
  int state;                     // (0 none, 1 queued, 2 running, 3 come
                                 //  back, 4 running given up, 5 come back
                                 //  given up: from 1, under the queue's
                                 //  lock, after, under its reactor's)
  HeffJob *prev, *next;          // (in the queue, or among its reactor's)
};

// (a reactor's jobs, under its lock: those come back, not yet taken; its
// pipe's end written to; how many given up are still running; and, read
// without the lock, whether any has come back)
struct HeffJobs {
  pthread_mutex_t mu;
  int made;
  HeffJob *first, *last;
  int fd;
  int64_t gone;
  int some;
};

#ifdef HEFF_PART
extern pthread_once_t heff_jobs_once;
extern pthread_mutex_t heff_jobs_mu;
extern pthread_cond_t heff_jobs_cv;
extern HeffJob *heff_jobs_first, *heff_jobs_last;
extern int64_t heff_jobs_queued;
extern int heff_jobs_threads, heff_jobs_idle;
#else
pthread_once_t heff_jobs_once = PTHREAD_ONCE_INIT;
pthread_mutex_t heff_jobs_mu;
pthread_cond_t heff_jobs_cv = PTHREAD_COND_INITIALIZER;
HeffJob *heff_jobs_first, *heff_jobs_last;
int64_t heff_jobs_queued;
int heff_jobs_threads, heff_jobs_idle;
#endif
HEFF_VAR(HEFF_TLS HeffJobs heff_jobs_mine);

// (a lock of the jobs', made: one taken by whichever comes first, not in
// turn, where the system has such -- several times quicker, as many
// threads take it, than one handed on in turn)
static void heff_jobs_lock_made(pthread_mutex_t *m) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
#ifdef PTHREAD_MUTEX_POLICY_FIRSTFIT_NP
  pthread_mutexattr_setpolicy_np(&a, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP);
#endif
  pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
}

static void heff_jobs_made(void) { heff_jobs_lock_made(&heff_jobs_mu); }

static inline void heff_job_put(HeffJob **first, HeffJob **last, HeffJob *j) {
  j->prev = *last;
  j->next = NULL;
  if (*last) (*last)->next = j;
  else *first = j;
  *last = j;
}

static inline void heff_job_out(HeffJob **first, HeffJob **last, HeffJob *j) {
  if (j->prev) j->prev->next = j->next;
  else *first = j->next;
  if (j->next) j->next->prev = j->prev;
  else *last = j->prev;
  j->prev = j->next = NULL;
}

// (a thread of the jobs': each queued taken and run, come back to its
// reactor, which is woken where it had none come back)
static void *heff_job_thread(void *u) {
  (void)u;
  pthread_mutex_lock(&heff_jobs_mu);
  for (;;) {
    while (!heff_jobs_first) {
      heff_jobs_idle++;
      pthread_cond_wait(&heff_jobs_cv, &heff_jobs_mu);
      heff_jobs_idle--;
    }
    HeffJob *j = heff_jobs_first;
    heff_job_out(&heff_jobs_first, &heff_jobs_last, j);
    heff_jobs_queued--;
    __atomic_store_n(&j->state, 2, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&heff_jobs_mu);
    j->run(j);
    HeffJobs *of = j->of;
    pthread_mutex_lock(&of->mu);
    int none = !of->first;
    __atomic_store_n(&j->state, j->state == 4 ? 5 : 3, __ATOMIC_RELAXED);
    heff_job_put(&of->first, &of->last, j);
    __atomic_store_n(&of->some, 1, __ATOMIC_RELEASE);
    if (none) {
      char b = 1;
      ssize_t r = write(of->fd, &b, 1);
      (void)r;
    }
    pthread_mutex_unlock(&of->mu);
    pthread_mutex_lock(&heff_jobs_mu);
  }
  return NULL;
}

// (another thread of the jobs' made, under the queue's lock: whether it
// was)
static int heff_job_thread_made(void) {
  pthread_attr_t attr;
  pthread_t t;
  sigset_t all, was;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, &was);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, HEFF_JOB_STACK);
  int r = pthread_create(&t, &attr, heff_job_thread, NULL);
  pthread_attr_destroy(&attr);
  pthread_sigmask(SIG_SETMASK, &was, NULL);
  if (r != 0) return 0;
  heff_jobs_threads++;
  return 1;
}

// | The job `j` of the wait `w` -- `run` and `let_go` set -- begun on a
// | thread of the runtime's, the wait waiting for it (its `fd` -1), to
// | be stepped once it has come back: whether it was, in a group alone
// | (its first step's), or by a step of one pending.
HEFF_FN int heff_job(HeffWait *w, HeffJob *j) {
  if ((!heff_group && !w->linked) || !heff_wake_made) return 0;
  HeffJobs *of = &heff_jobs_mine;
  if (!of->made) {
    pthread_once(&heff_jobs_once, heff_jobs_made);
    heff_jobs_lock_made(&of->mu);
    of->made = 1;
  }
  // (the pipe is the thread's while it has a group: its jobs' threads
  // read its end under its lock)
  if (of->fd != heff_wake_fds[1]) {
    pthread_mutex_lock(&of->mu);
    of->fd = heff_wake_fds[1];
    pthread_mutex_unlock(&of->mu);
  }
  pthread_mutex_lock(&heff_jobs_mu);
  if (heff_jobs_queued >= heff_jobs_idle && heff_jobs_threads < HEFF_JOB_THREADS && !heff_job_thread_made() &&
      !heff_jobs_threads) {
    pthread_mutex_unlock(&heff_jobs_mu);
    return 0;
  }
  j->w = w;
  j->of = of;
  __atomic_store_n(&j->state, 1, __ATOMIC_RELAXED);
  heff_job_put(&heff_jobs_first, &heff_jobs_last, j);
  heff_jobs_queued++;
  pthread_cond_signal(&heff_jobs_cv);
  pthread_mutex_unlock(&heff_jobs_mu);
  w->fd = -1;
  return 1;
}

// | The job `j` given up, its wait let go of: whether it is the
// | provider's to let go of now -- none begun, one not yet begun, one
// | come back -- or, running, its own `let_go`'s once it has come back.
HEFF_FN int heff_job_give_up(HeffJob *j) {
  if (!__atomic_load_n(&j->state, __ATOMIC_RELAXED)) return 1;
  pthread_mutex_lock(&heff_jobs_mu);
  if (j->state == 1) {
    heff_job_out(&heff_jobs_first, &heff_jobs_last, j);
    heff_jobs_queued--;
    j->state = 0;
    pthread_mutex_unlock(&heff_jobs_mu);
    return 1;
  }
  pthread_mutex_unlock(&heff_jobs_mu);
  HeffJobs *of = j->of;
  pthread_mutex_lock(&of->mu);
  int mine = j->state == 3;
  if (j->state == 2) {
    j->state = 4;
    of->gone++;
  } else if (mine) {
    heff_job_out(&of->first, &of->last, j);
    j->state = 0;
  }
  pthread_mutex_unlock(&of->mu);
  return mine;
}

// (this thread's jobs come back: each's wait stepped, or, given up, it
// let go of)
HEFF_FN void heff_jobs_collect(void) {
  HeffJobs *of = &heff_jobs_mine;
  if (!__atomic_load_n(&of->some, __ATOMIC_ACQUIRE)) return;
  pthread_mutex_lock(&of->mu);
  HeffJob *j = of->first;
  of->first = of->last = NULL;
  of->some = 0;
  for (HeffJob *i = j; i; i = i->next)
    if (i->state == 5) of->gone--;
    else i->state = 0;
  pthread_mutex_unlock(&of->mu);
  while (j) {
    HeffJob *next = j->next;
    j->prev = j->next = NULL;
    if (j->state == 5) {
      j->state = 0;
      j->let_go(j);
    } else heff_wait_ready(j->w);
    j = next;
  }
}

// (this thread's jobs given up, every one come back and let go of: as
// its last group is closed, its pipe open still)
HEFF_FN void heff_jobs_end(void) {
  HeffJobs *of = &heff_jobs_mine;
  if (!of->made) return;
  for (;;) {
    heff_jobs_collect();
    pthread_mutex_lock(&of->mu);
    int64_t gone = of->gone;
    pthread_mutex_unlock(&of->mu);
    if (!gone || !heff_wake_made) return;
    struct pollfd p = { heff_wake_fds[0], POLLIN, 0 };
    if (poll(&p, 1, -1) > 0) {
      char b[64];
      while (read(heff_wake_fds[0], b, sizeof b) > 0) {}
    }
  }
}
#else
typedef struct HeffJob HeffJob;
struct HeffJob {
  void (*run)(HeffJob *);
  void (*let_go)(HeffJob *);
};
HEFF_FN int heff_job(HeffWait *w, HeffJob *j) {
  (void)w;
  (void)j;
  return 0;
}
HEFF_FN int heff_job_give_up(HeffJob *j) {
  (void)j;
  return 1;
}
#endif

// Giving a group up from any thread
// ---------------------------------------------------------------------
//
// A group within another is given up from any thread by what gives it
// up (`heff_reactor_giving`): a foreign value -- a thing a custodian can
// have in its custody -- made with a value of the group's, `gave`.
// Ended, by a custodian's shutdown or as its last reference is dropped,
// it asks the thread whose group it is to give the group up, and wakes
// that thread.  The thread does at its next look at its finished
// (`heff_giving_taken`, at a point where no wait of its is being
// stepped): where any of the group is pending, the group is closed, what
// is pending in it let go of, undone, and `gave` is among its root's
// finished instead, so that what counts the computations started knows
// that one is done; where none is -- what was started came to its
// value, or failed, already -- the group is closed alone.  A group
// closed, by any means, is given up no more.  The asking is under a lock
// of its own: the group is its thread's, the thing anyone's.

struct HeffGiving {
  int64_t group, root;
  int state;                     // (0 open; 1 asked to be given up, among
                                 //  those asked once the lock is had; 2
                                 //  taken by its thread; 3 closed: from 0,
                                 //  by a `cas`, else under the lock)
  int refs;                      // (the thing's, and the group's while it
                                 //  is open: counted atomically)
  V gave;
  HeffGiving *next;              // (among those asked)
};

#ifdef HEFF_PART
extern HeffGiving *heff_giving_asked;
extern int64_t heff_giving_n;
extern pthread_mutex_t heff_giving_mu;
#else
HeffGiving *heff_giving_asked;
int64_t heff_giving_n;
pthread_mutex_t heff_giving_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

// (one of its holders let go of it: freed by the last)
HEFF_FN void heff_giving_unref(HeffGiving *gv) {
  if (__atomic_sub_fetch(&gv->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
  V gave = gv->gave;
  free(gv);
  heff_drop(gave);
}

// (it taken out of those asked; the lock held)
HEFF_FN void heff_giving_unasked(HeffGiving *gv) {
  for (HeffGiving **at = &heff_giving_asked; *at; at = &(*at)->next)
    if (*at == gv) {
      *at = gv->next;
      __atomic_sub_fetch(&heff_giving_n, 1, __ATOMIC_RELEASE);
      return;
    }
}

// (its end, from any thread: its group asked to be given up, and its
// thread woken, where it is open)
static const char *heff_giving_end(void *p) {
  HeffGiving *gv = p;
  int open = 0, asked = 0;
  if (!__atomic_compare_exchange_n(&gv->state, &open, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NULL;
  pthread_mutex_lock(&heff_giving_mu);
  // (not closed meanwhile, by its thread)
  if (__atomic_load_n(&gv->state, __ATOMIC_ACQUIRE) == 1) {
    gv->next = heff_giving_asked;
    heff_giving_asked = gv;
    __atomic_add_fetch(&heff_giving_n, 1, __ATOMIC_RELEASE);
    asked = 1;
  }
  pthread_mutex_unlock(&heff_giving_mu);
  if (asked) heff_reactor_wake_group(gv->root);
  return NULL;
}

static void heff_giving_let_go(void *p) { heff_giving_unref(p); }

// | What gives up group `g`, a group within another of this thread's,
// | from any thread: `gave`, its value where it is given up while any
// | of it is pending, finished in its root.  `gave` consumed.
HEFF_FN V heff_reactor_giving(int64_t g, V gave) {
  HeffGroup *gr = heff_group_of(g);
  if (!gr || !gr->root) heff_fail("a group given up that is not one within another");
  if (gr->giving) heff_fail("a group given up by two things");
  HeffGiving *gv = heff_alloc(sizeof *gv);
  gv->group = g;
  gv->root = gr->root;
  gv->state = 0;
  gv->refs = 2;
  gv->gave = heff_share(gave);
  gv->next = NULL;
  gr->giving = gv;
  return heff_foreign_ending(gv, heff_giving_let_go, heff_giving_end);
}

// (the groups `g` closes -- it, and those within it -- given up no
// more, let go of by them: before what is pending in them is, as what
// that lets go of may hold what gives one up)
HEFF_FN void heff_giving_closed(int64_t g) {
  for (size_t i = 0; i < heff_ngroups; i++) {
    HeffGroup *gr = &heff_groups_open[i];
    if (!gr->giving || (gr->id != g && gr->root != g)) continue;
    HeffGiving *gv = gr->giving;
    gr->giving = 0;
    int open = 0;
    if (!__atomic_compare_exchange_n(&gv->state, &open, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      pthread_mutex_lock(&heff_giving_mu);
      if (__atomic_load_n(&gv->state, __ATOMIC_ACQUIRE) == 1) heff_giving_unasked(gv);
      __atomic_store_n(&gv->state, 3, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&heff_giving_mu);
    }
    heff_giving_unref(gv);
  }
}

// | The groups of this thread's asked to be given up, given up: each
// | closed, and, where any of it was pending, what it gave finished in
// | its root.
HEFF_FN void heff_giving_taken(void) {
  if (__atomic_load_n(&heff_giving_n, __ATOMIC_ACQUIRE) == 0) return;
  HeffGiving *mine = NULL;
  pthread_mutex_lock(&heff_giving_mu);
  for (HeffGiving **at = &heff_giving_asked; *at;) {
    HeffGiving *gv = *at;
    HeffGroup *gr = heff_group_of(gv->group);
    if (gr && gr->giving == gv) {
      *at = gv->next;
      __atomic_sub_fetch(&heff_giving_n, 1, __ATOMIC_RELEASE);
      __atomic_store_n(&gv->state, 2, __ATOMIC_RELEASE);
      gv->next = mine;
      mine = gv;
    } else at = &gv->next;
  }
  pthread_mutex_unlock(&heff_giving_mu);
  while (mine) {
    HeffGiving *gv = mine;
    mine = gv->next;
    HeffGroup *gr = heff_group_of(gv->group);
    if (!gr) continue;
    int64_t root = gr->root;
    V gave = gr->pending > 0 ? heff_dup(gv->gave) : 0;
    heff_reactor_close(gv->group);
    if (gave) heff_finished_put(root, gave);
  }
}

// Running
// ---------------------------------------------------------------------

HEFF_FN V heff_nomatch(V s) {
  (void)s;
  heff_fail("no case for a tag");
  return 0;
}

// Values, shown (`heff dump-c`: each definition of
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

// (content in, `f` read to its end, as a string: straight into it where
// `size`, what the file says it holds, is all there is, else through a
// buffer; 0, where a read failed)
HEFF_FN V heff_str_read(FILE *f, int64_t size) {
  Buf b = {0};
  if (size > 0) {
    Str *s = heff_str_new(size);
    size_t got = fread(s->s, 1, (size_t)size, f);
    int c = got == (size_t)size ? fgetc(f) : EOF;
    if (c == EOF) {
      if (ferror(f)) { heff_drop((V)s); return 0; }
      s->len = (int64_t)got;
      return (V)s;
    }
    // (more than it said: what was read, and the rest, through the buffer)
    char more = (char)c;
    buf_put(&b, (const char *)s->s, got);
    buf_put(&b, &more, 1);
    heff_drop((V)s);
  }
  char chunk[65536];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(&b, chunk, got);
  V s = ferror(f) ? 0 : heff_str_bytes(b.s ? b.s : "", (int64_t)b.n);
  free(b.s);
  return s;
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
  int ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, heff_working_dir(), &si, &pi);
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
    int closed[2] = { po[0], pe[0] };
    pid_t pid = heff_exec_in_dir(as, NULL, -1, po[1], pe[1], closed, 2);
    int ok = pid > 0;
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
    uint32_t u = s->p[i];
    char tmp[16];
    if (u >= 0x80) {
      int w = heff_u8_decode(s->p + i, s->len - i, &u);
      if (heff_u8_own(s->p + i, u, w)) buf_put(b, (const char *)s->p + i, (size_t)w);
      else if (!shown) buf_str(b, "\xEF\xBF\xBD");
      else for (int k = 0; k < w; k++) { snprintf(tmp, sizeof tmp, "\\x%02x", s->p[i + k]); buf_str(b, tmp); }
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
  heff_show_byte_pieces(b, s->p, s->len);
  buf_str(b, "]");
}

// (bits as the prelude's `showBits` writes them: their whole bytes as
// bytes are, then the bits left, `v::k`, or, none left, `b[]::bits`)
HEFF_FN void heff_show_bits(Buf *b, V v) {
  Str *s = (Str *)v;
  int64_t k = (8 - (int64_t)s->h.tag) & 7, whole = k ? s->len - 1 : s->len;
  char tmp[32];
  buf_str(b, "b[");
  int any = heff_show_byte_pieces(b, s->p, whole);
  if (k) snprintf(tmp, sizeof tmp, "%s%u::%d", any ? ", " : "", (unsigned)(s->p[whole] >> (8 - k)), (int)k);
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
    char w[24], *c = IS_INT(v) ? heff_word_digits(INT_OF(v), w + sizeof w - 1) : heff_int_cstr(v);
    w[sizeof w - 1] = 0;
    if (arg && c[0] == '-') { buf_str(b, "("); buf_str(b, c); buf_str(b, ")"); }
    else buf_str(b, c);
    if (!IS_INT(v)) free(c);
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
      heff_show_float(FLT_OF(v), tmp);
      if (arg && tmp[0] == '-') { buf_str(b, "("); buf_str(b, tmp); buf_str(b, ")"); }
      else buf_str(b, tmp);
      return;
    }
    case K_STR: buf_json_string_as(b, v, 1); return;
    case K_BYT: heff_show_bytes(b, v); return;
    case K_BIT: heff_show_bits(b, v); return;
    case K_FGN: buf_str(b, "#foreign"); return;
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
// asked for, the thing it is shown, or why it stopped -- an abandon, or
// fuel run out; a failure ends the program there)
typedef struct { const char *name; V (*get)(void); } Global;

HEFF_FN void heff_check_one(Buf *out, const Global *g) {
  jmp_buf here;
  heff_catch = &here;
  Buf b = {0};
  buf_str(out, "\"");
  buf_str(out, g->name);
  buf_str(out, "\": ");
  if (HEFF_SETJMP(here) == 0) {
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
  heff_tls_enter();
  heff_watch_stack();
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
  heff_tls_enter();
  heff_watch_stack();
  heff_tr(heff_entry());
  if (heff_returned && heff_returned() && heff_status == 0) heff_status = 1;
  return NULL;
}
#endif

// (the program's thread: the program, then its end -- the first
// thread's to make, where nothing lent runs there)
static void *heff_program(void *u) {
  heff_thread(u);
  heff_end(heff_status, 1);
  return NULL;
}

// (the first thread's part: each job lent to it run, until the
// program's end is its own to make -- then the program's status)
static int heff_first_serve(void) {
  pthread_mutex_lock(&heff_first_mu);
  for (;;) {
    if (heff_first.ended == 1 && !heff_first.running) break;
    if (!heff_first.ended && heff_first.lent && !heff_first.running) {
      void (*run)(void *) = heff_first.run;
      void *u = heff_first.u;
      heff_first.running = 1;
      pthread_mutex_unlock(&heff_first_mu);
      run(u);
      pthread_mutex_lock(&heff_first_mu);
      heff_first.running = 0;
      heff_first.lent = 0;
      continue;
    }
    pthread_cond_wait(&heff_first_cv, &heff_first_mu);
  }
  int status = heff_first.status;
  pthread_mutex_unlock(&heff_first_mu);
  return status;
}

static void heff_first_begin(void) {
  heff_first.first = pthread_self();
  heff_first.known = 1;
}

#ifdef _WIN32
// (the program's thread on Windows: its large stack reserved, not
// committed, and its bytes in and out as they are, not as text)
static DWORD WINAPI heff_win_thread(LPVOID u) {
  heff_program(u);
  return 0;
}

int main(int argc, char **argv) {
  heff_argc = argc; heff_argv = argv;
  heff_tls_begin();
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  SetConsoleOutputCP(CP_UTF8);
  heff_first_begin();
  HANDLE t = CreateThread(NULL, (SIZE_T)HEFF_MAIN_STACK, heff_win_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!t) heff_fail("cannot start");
  int status = heff_first_serve();
  if (!heff_first.waits) WaitForSingleObject(t, INFINITE);
  fflush(stdout);
  return status;
}
#else
int main(int argc, char **argv) {
  heff_argc = argc; heff_argv = argv;
  heff_tls_begin();
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, HEFF_MAIN_STACK);
#ifdef HEFF_WATCHED
  pthread_attr_setguardsize(&attr, HEFF_GUARD);
#endif
  heff_watch_faults();
  heff_first_begin();
  pthread_t t;
  if (pthread_create(&t, &attr, heff_program, NULL) != 0) heff_fail("cannot start");
  int status = heff_first_serve();
  if (!heff_first.waits) pthread_join(t, NULL);
  fflush(stdout);
  return status;
}
#endif
#endif
