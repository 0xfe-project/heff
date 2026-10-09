# Providers

An effect of the world is carried out by its provider: an effect's
JavaScript side is in `M.provider.js` beside `M.hf`, its C side in
`M.provider.c`; the things they hand out the world holds for them all.
This is where a heff program meets its host, and so where how each
backend keeps a value matters.

## Strings

A provider gives a string to the host, and takes one from it, only
through the runtime's helpers, by what the string is there -- text, or
bytes: `$toJs`, `$fromJs`, `$toBytes` and `$fromBytes` in JavaScript
(`lib/std/dom.provider.js` says which is for what), `heff_str_text`,
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

## JSString

`std.js` has JavaScript's text beside them: a `JSString`, which a
string is made by `toJS` (what is not UTF-8 in it U+FFFD, as
`TextDecoder` reads it) and made of by `fromJS` (a surrogate not in a
pair U+FFFD), and nothing else -- a JavaScript string in JavaScript,
and in C and the interpreter a string of UTF-8, valid, so that
`fromJS (toJS s)` is `R.toValid s` on each.  An operation's field of
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
word of its bits (`heff_f32_of`, `heff_f32`), and a `Float` is boxed
(`FLT_OF`, `heff_flt`).  So in C a `Float`, and an `Int64` or `UInt64`
beyond 62 bits (as an `Int` is), costs an allocation each value, where
an integer of up to 32 bits and a `Float32` cost none.

A provider is given a rune as a number in JavaScript, and in C as a
word of its code point (`INT_OF`, `MK_INT`).
