# Providers

An effect of the world is carried out by its provider: an effect's
JavaScript side is in `M.provider.js` beside `M.hf`, its C side in
`M.provider.c`; the things they hand out the world holds for them all.
This is where a heff program meets its host, and so where how each
backend keeps a value matters.  A C provider may run the program in
threads truly at once: see [threads.md](threads.md).

## Where each runs

A JavaScript provider that lives in one host only says so, `export
const $host = "page"` (a page's script, `--backend=page`) or `"node"`
(Node.js, `--backend=node`); without it, both
(`lib/browser/dom.provider.js` says the shape of a provider).  The
library is split by it ([library.md](library.md#the-batteries)):

- `lib/std/`'s effects are the system's, with a provider in C:
  `std.io`, `std.filesystem`, `std.process`, `std.progress`, `std.http`,
  `std.net`;
- `std.ref` and `std.time` are needed on both sides: one module each,
  with a provider in C and one in JavaScript of no `$host`, which a
  page's script runs as Node.js does;
- `lib/browser/`'s are the browser's, in JavaScript alone: `browser.dom`
  (`$host = "page"`) and `browser.js`, which has no effect, only its
  type.

`std.io`, `std.filesystem`, `std.process` and `std.progress` keep a
provider for Node.js (`$host = "node"`), for heff itself and its tests
only, as each says at its top; no other module of `lib/std/` is given
one.  `--backend=node` is there for them, not for programs.

What a backend has no side of is refused where it is imported, before
the program's effect is looked at: by `heff compile --backend=c` (and
`heff build`'s C programs), an import of a module of `lib/browser/`
(E0351); by `--backend=page` or `node`, an import of an effect of
`lib/std/` with a provider in C and none in JavaScript for that host,
or of a module of `lib/std/` that imports one, itself or by the modules
it imports (E0352; one that imports only pure modules, as `std.path`
and `std.handle` do, is not refused).  The library's own imports are
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
as one -- to a provider, into a list, a tag or a closure, to a function
or back from it -- where an integer of up to 32 bits and a `Float32`
cost none.  Where a program computes with a fixed-width number or a
`Float64`, it keeps it unboxed, as its C type (`int64_t`, `double`,
...), and boxes it only where it is given so (src/heff/compile/repr.hf);
a function that takes such a number as a number, or gives one back, is
called, where the call is known, by a C function of its own taking and
giving it unboxed (its worker, `g_K_w`, src/heff/compile/worker.hf),
and from a closure or a provider as any other, through a function of
`V`s that unboxes and boxes (its wrapper, `g_K`).
A record's `Float64`, `Int64` and `UInt64` fields (of its first 16),
built where their type is known, are kept flat, their bits in the
record: a provider given a record reads a field by `heff_rget_v` (the
field as a `V`, counted; boxed if it is flat), not out of the record
itself.

A provider is given a rune as a number in JavaScript, and in C as a
word of its code point (`INT_OF`, `MK_INT`).
