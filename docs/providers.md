# Providers

An effect of the world is carried out by its provider: an effect's
JavaScript side is in `M.provider.js` beside `M.hf`, its C side in
`M.provider.c`; the things they hand out the world holds for them all.
This is where a heff program meets its host, and so where how each
backend keeps a value matters.  A C provider may run the program in
threads truly at once: see [threads.md](threads.md).

## Where each runs

A JavaScript provider runs in a page's script (`--backend=page`); one
that runs nowhere else says so, `export const $host = "page"`
(`lib/browser/dom.provider.js` says the shape of a provider).  The
library is split by backend ([library.md](library.md#the-batteries)):

- `lib/std/`'s effects are the system's, with a provider in C:
  `std.io`, `std.filesystem`, `std.process`, `std.progress`, `std.http`,
  `std.net`, `std.thread`, `std.reactor`, `std.terminal`;
- `std.reference` and `std.time` are needed on both sides: one module each,
  with a provider in C and one in JavaScript of no `$host`;
- `lib/browser/`'s are the browser's, in JavaScript alone: `browser.dom`
  and `browser.http` (`$host = "page"`), and `browser.js`, which has no
  effect, only its type.
- `gpu` (lib/gpu.hf), a GPU's effect, has one provider so far,
  WebGPU's, in JavaScript (`$host = "page"`): a kernel is WGSL, its
  buffers bound in order at `@group(0)`; where the browser has no
  `navigator.gpu`, `open` is `Left`, "WebGPU is not here".  Native
  ones (Vulkan, Metal, CUDA's driver API) are to come in C.

What a backend has no side of is refused where it is imported, before
the program's effect is looked at: by `heff compile --backend=c` (and
`heff build`'s C programs), an import of a module of `lib/browser/`
(E0351); by `--backend=page`, an import of an effect of `lib/std/`
with a provider in C and none in JavaScript, or of a module of
`lib/std/` that imports one, itself or by the modules it imports
(E0352; one that imports only pure modules, as `std.path` and
`std.handle` do, is not refused).  The library's own imports are
its own; a program's modules' are each said, at the import, naming the
module and the backend, and in a note, for a module that reaches an
effect by its imports, that effect and the imports by which it does.  An
effect of a program's own, with no provider for the backend, is said
at its computation: it "is not provided here".

## Strings

A provider gives a string to the host, and takes one from it, only
through the runtime's helpers, by what the string is there -- text, or
bytes: `$toJs`, `$fromJs`, `$toBytes` and `$fromBytes` in JavaScript
(`lib/browser/dom.provider.js` says which is for what), `heff_str_text`,
`heff_str_bytes`, `heff_cstr` and the like in C (`lib/c/runtime.c`,
"What providers share").  In JavaScript a string is kept as a
JavaScript string each of whose code units is one of its bytes; in C as
its bytes, no NUL after them.

The same on every backend: content and paths are bytes as they are (a
file's, a stream's, what a command wrote, what was fetched, a
directory's names); names -- the program's arguments, the environment,
the working directory, a command and its arguments -- and the hosts of
text (the DOM, JSON) are text, what is not UTF-8 U+FFFD in and out; a
path with a NUL in it is none, the operation failing.

## Bytes

`Bytes` (lib/bytes.hf) are given and taken as they are kept: in
JavaScript a `Uint8Array`, which a provider makes and reads as it is
(and never changes, nor one it was given); in C a buffer, which a
provider makes by `heff_bytes_in` (`n` bytes copied) and reads by
`heff_bytes_data` (where they are, and how many, for as long as it
holds the value), and neither looks into nor changes otherwise.

## JSString

`browser.js` has JavaScript's text beside them: a `JSString`, which a
string is made by `toJS` (what is not UTF-8 in it U+FFFD, as
`TextDecoder` reads it) and made of by `fromJS` (a surrogate not in a
pair U+FFFD), and nothing else -- a JavaScript string in JavaScript,
and in the interpreter a string of UTF-8, valid, so that `fromJS (toJS
s)` is `R.toValid s` on each (a C program does not import it, E0351).
An operation's field of
type `JSString` is given to a provider, and taken from it, as it is,
through no helper: a JavaScript string in JavaScript (the library's own
effects take `String`s, their providers making them what the host has).

An ES module (`--backend=esm`) gives and takes its values as they are
kept, nothing made anew: a function for JavaScript to call takes and
gives `JSString`s (`greet : JSString -> JSString`), a `String` there in
JavaScript its bytes -- JavaScript calling it makes one
(`Buffer.from(t, "utf8").toString("latin1")`) and reads one back.

## Numbers and runes

A provider is given a number as the backend keeps it: in JavaScript an
`Int`, an `Int64` and a `UInt64` are BigInts, any other number a
number; in C an integer of up to 32 bits is a word of its value
(`INT_OF`), one of 64 bits is kept as an `Int` is, a `Float32` is a
word of its bits (`heff_f32_of`, `heff_f32`), and a `Float64` is boxed
(`FLT_OF`, `heff_flt`).  So in C a `Float64`, and an `Int64` or `UInt64`
beyond 62 bits (as an `Int` is), costs an allocation each value given
as one -- to a provider, into a closure, into a list, a tag or a record
where its type is not known, to a function or back from it -- where an
integer of up to 32 bits and a `Float32` cost none.  Where a program computes with a fixed-width number or a
`Float64`, it keeps it unboxed, as its C type (`int64_t`, `double`,
...), and boxes it only where it is given so (src/heff/compile/repr.hf);
a function that takes such a number as a number, or gives one back, is
called, where the call is known, by a C function of its own taking and
giving it unboxed (its worker, `g_K_w`, src/heff/compile/worker.hf),
and from a closure or a provider as any other, through a function of
`V`s that unboxes and boxes (its wrapper, `g_K`).  So too a function
giving back a tag or a record it builds: a call taking it apart at once
calls a worker giving back the fields unbuilt, a struct of words
(`g_K_24cpr`, src/heff/compile/cpr.hf), and anything else -- a closure,
a provider -- calls `g_K`, which builds it.
A record's `Float64`, `Int64` and `UInt64` fields (of its first 16),
built where their type is known, are kept flat, their bits in the
record: a provider given a record reads a field by `heff_rget_v` (the
field as a `V`, counted; boxed if it is flat), not out of the record
itself.  A tag's are kept so too (`Just 1.5`, a `List Float64`'s cells
built where the type is known): a provider given a tag one of whose
fields is of such a type, or of a type it does not know (a list's
element), reads that field by `heff_tget_v` (as `heff_rget_v`), not by
`heff_field`.

A provider is given a rune as a number in JavaScript, and in C as a
word of its code point (`INT_OF`, `MK_INT`).

## Operations described in C

An operation a C function does -- the fields given to it, its result
the answer -- need not be written in the provider at all: the effect's
module describes it, and the compiler makes that part of the provider,
its dispatch and conversions, where it compiles a program for C.  The
description is a definition of type `Described` (lib/ffi.hf) in the
effect's module, C's types as values, composed as Racket's `ffi` has
them:

```
import ffi as Ffi

-- (std.thread: `Cores` is the provider's `th_cores()`)
threadsInC : Ffi.Described
threadsInC =
  Ffi.describe
    Nil
    [Ffi.operation "Cores" (Ffi.fun "th_cores" Nil (Ffi.gives Ffi.int64))]
```

Each operation is a constructor's name and a function: its name, as
the header the description names declares it (`Ffi.describe
["<stdio.h>"] [...]`), or as the module's `M.provider.c` defines it
(a wrapper of the system's call, `static`, behind an `#ifdef` where
systems differ); its arguments, the operation's fields but the last,
in order, each a `CType` -- `Ffi.int`, `Ffi.int64`, `Ffi.integer
"mode_t"` (an integer type by its C name, of an `Int`), `Ffi.double`
(a `Float64`), `Ffi.bool`, `Ffi.string` (`const char *`, a string's
bytes with a NUL after them; `Ffi.text` the same made valid), and
`Ffi.pointer "FILE" "fclose"` (a `FILE *`, a foreign value); and its
result -- `Ffi.gives t` (the answer; `Ffi.void` gives `MkUnit`, or the
operation's `x` itself where that is its last field),
`Ffi.givesOrErrno t` (an `Either Errno a`: `-1`, or `NULL`, with
`errno` set is `Left` of it, as `Ffi.Errno` has it, the shape of
std.filesystem's `FileError`), `Ffi.doneOrErrno` (an `int`, 0 or `-1`
with `errno`: `Either Errno Unit`); `Ffi.ownedString` is a result
`char *` C malloc'd, copied and freed.  A string argument with a NUL in
it fails the operation `EINVAL`, where it can fail, else the program.

A foreign value is a pointer the program holds (`heff_foreign(p,
let_go)` makes one, `heff_foreign_ptr(v)` reads it back), of whatever
type the operation says it is -- a `data` of no constructors, `data
Stream` -- the provider vouching for it at the boundary; the program
drops its last reference, and the named function lets the thing go
then, at once (Perceus: never while it is referenced).  In JavaScript
a foreign value is to be the object itself, with a
`FinalizationRegistry`; a description means nothing there yet: the
module's `M.provider.js` is the whole of the effect.

The compiler holds the description to the effect: a constructor that
is none of the module's effects' operations, a field too many or too
few, a field of a type that cannot cross as described, each an error
of the compile.  What it cannot see, the C side, the C compiler
checks: the glue is built against the header's own declaration, each
function held to the type described (`_Static_assert` of
`__builtin_types_compatible_p(__typeof__(f), ...)`), so a mismatch --
an `int` for a `size_t`, a `void *` for a `FILE *` -- fails the build,
saying what the module described.  (A function that is a macro cannot
be held so: describe a wrapper.)

The glue is `heff_described_X`, after the provider's own text.  A
provider written by hand hands it the operations it does not do
itself, declaring it first:

```c
static V heff_described_threads(V p, V k);

static V heff_provider_threads(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Pool: ...
  default:
    return heff_described_threads(p, k);
  }
}
```

Operations done in steps, by the reactor (below), stay written by
hand.  A module all of whose operations are described needs no
`M.provider.c`: the glue is the provider (test/node/cstdio.hf, C's
streams as an effect, no C written for it).  Functions of no effect
(libm's, a hash) are not described: they are primitives of the
runtime or the library; C cannot be trusted to say a function is pure.

## Scoped operations

A host's thing that must be let go of -- an open file, a socket, a
canvas -- is handed out by a `with` of the effect's own, which is
`with`'s `scoped` around the effect's operations acquiring and
releasing it (std.filesystem.held's `withFile path options body`): the
body has it as a `With s a`, polymorphic in the region `s` so that what
it comes to cannot hold it, one `With` for every effect ([a thing held
by a `with`](library.md#a-thing-held-by-a-with)).  The effect's
operations on the thing take the `With s a` and give the thing to the
provider by `with`'s `inside`; the module exposes its `with` and those
operations, never the bare acquire.  What uses it needs no effect but
the thing's own.  Its provider makes sure it is let go of, in two
parts:

- the operation that acquires it gives a foreign value whose function
  letting it go is its release (`heff_foreign(p, let_go)`).  Where the
  program drops its last reference -- the body left by a throw heard
  outside it, or by a handler between dropping the continuation that
  holds it -- the thing is let go of then: in C at once, by the count,
  before the handler that dropped it goes on; in JavaScript, by a
  `FinalizationRegistry`, when the collector finds it, if it does.  A
  drop lets go of what it held as it frees it, in no order promised;
- the body ended, the `with` releases it by an operation of the
  effect's own (std.filesystem's `ReleaseHeld`), which marks it
  released: done once, its function letting go of it then frees only
  what holds it, and an operation given it after fails as the host does
  (`EBADF`).

An operation of it that waits keeps the operation, so the thing, until
it is done, so that it is not let go of under it.  The `with` is a
function over these operations, not an operation holding the body: a
computation an operation of the world holds is forwarded to it, by each
handler between, by a natural transformation, which has nothing for a
throw but `abandon` -- a throw out of such a body, heard outside it,
would end the program.

## Outside operations done later

An operation of the system that must wait -- a socket not yet
readable, a name being resolved -- is done in steps, by the runtime's
reactor (lib/c/runtime.c, "Outside operations done later"), so that
green threads ([library.md](library.md#green-threads)) go on while it
waits.  A provider's operation is a `HeffWait` (its first member, of a
struct of the provider's own):

- `fd` and `events` (`POLLIN`, `POLLOUT`): what it waits for; `fd` -1
  where a source says when it is done (below), or where it waits for
  its deadline alone;
- `until`: when it is done regardless, in milliseconds of the
  runtime's clock (`heff_now_ms`), or 0; the reactor polls no longer
  than the nearest deadline, and a wait past its own is done, its
  `answer` asked for -- std.time's sleep is a wait of a deadline and no
  descriptor;
- `step(w)`: what can be done now, without waiting: `HEFF_STEP_DONE`,
  or `HEFF_STEP_WAIT`, its `fd` and `events` set again;
- `answer(w)`: its answer, a value of the program's, made of what it
  did -- or, where `w->err` is set, of why waiting failed (poll's
  errno, or `EBADF`: its descriptor closed meanwhile) -- the wait let
  go of;
- `cancel(w)`: the wait let go of undone (what it made closed).

The provider takes the first step itself: done, it gives `f` of the
answer to `k`, as any operation; not done, it returns `heff_outside(w,
f, k)`, which either waits there, polling `fd` and stepping until it is
done, the whole program waiting (no scheduler of std.reactor's is
running: a program's operations are as they were), or, in a group of
std.reactor's, keeps the wait pending and gives back `HEFF_PENDING`
without calling `k`.  `HEFF_PENDING` comes back, by the calls last
that the world's continuations are, to std.reactor's `Start`, which
tells the scheduler; once the wait is done, the reactor gives `f` of
its answer to `k`, the rest of the computation started run as it
would have been.  Nothing of the heff side knows of it: an operation's
answer is given to its function, now or later.

A provider whose waits are done by something else -- libcurl's
transfers, which have sockets of their own (lib/std/http.provider.c,
its multi interface); std.process's started commands, each with pipes
and a descriptor that says when it exits (a kqueue's `NOTE_EXIT`, a
pidfd), polled with a time limit's -- is a `HeffSource`
(`heff_reactor_source`): `fds` says which descriptors it polls and how
long at most to wait, `ready` is given them as polled, and it says
which of its waits are done by `heff_wait_done(w)`.  A provider that
closes a descriptor others may wait on says so first,
`heff_reactor_closing(fd)`: what waits on it is done, `EBADF`.

What has no descriptor to wait on is given one: a name is resolved by
getaddrinfo in a thread of the system of its own, started for it
(lib/std/net.provider.c), which writes a byte to a socket of a pair
whose other end the wait polls; the thread touches nothing of heff's.

A descriptor that is not the provider's own to make non-blocking -- a
standard stream, whose flags its parent shares -- is stepped as it is
(lib/std/filesystem.provider.c): polled first, not waiting, and read
only once there is something to read, or written no more than PIPE_BUF
bytes at a time once there is room, which never waits.  A regular file
poll says is always ready: it is read and written there and then.
