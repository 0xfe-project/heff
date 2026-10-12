# The library

`lib/` goes with the compiler, its version the compiler's; `HEFF_LIB`
says where it is, else it is the `lib/` the compiler came with: the
repository's, a release's, or the heff home's (`heff install` puts it
there).  Its modules alone declare primitives --
`primitive intAdd : Int -> Int -> Int`, a function the runtime has --
and each primitive once: one a module of a program declares, or one
declared again, is an error.

heff's own repository is the library's source.  Its `package.hf` names
the package `heff`, which the compiler keeps for itself (see
[projects](projects.md#heffs-own-package)): where heff is from, that
project's `lib/` is the library, whatever `HEFF_LIB` or the installed
heff says.  So an installed heff, or one built in another checkout,
checks the repository's `lib/float32.hf` as the library's, its
primitives its own and declared once.  Any other project's `lib/` is no
library, and a project elsewhere that names itself `heff` is an error
(E0347), its `lib/` no library either.

## Modules

Every module has `builtin`, `prelude` and `list` (as `List`) without
writing them ([how names are found](language.md#modules)).  The others
are imported by their names:

- `co` ([effects](language.md#effects)), and effects made with it:
  `state`, `exn` (with `exnDiv` and `exnMod`, a division that throws by
  zero) and `parallel`;
- `array`, `map` and `set`;
- `check` and `gen`, the tests and their generators
  ([tests.md](tests.md#unit-tests-and-properties));
- `manifest`, the types of `package.hf` and `build.hf`
  ([projects.md](projects.md));
- the fixed-width numbers' `int8` to `int64`, `uint8` to `uint64` and
  `float32`, `int` with an `Int`'s bits, and `rune` with a `Rune`'s
  operations ([numbers](language.md#numbers),
  [runes and strings](language.md#runes-and-strings));
- `bytes`, the operations of `Bytes` ([bytes](language.md#bytes)).

## The batteries

The batteries ship with it, under `lib/std/`:

- `std.io` (the standard streams), `std.filesystem`, `std.process`,
  `std.time`, `std.net`;
- `std.ref` (the world's cells), `std.handle` (the world's things, which
  one effect hands out and another takes);
- `std.dom` (web pages), `std.js` (JavaScript's strings);
- `std.path`, `std.cli` (command lines), `std.doc` (documents laid
  out), `std.progress` (what a program is doing, drawn on a terminal);
- `std.math` (a float's functions: `sin`, `atan2`, `exp`, `log`, `pow`,
  `round`, `pi`, `e` and the like).

An effect's JavaScript side is in `M.provider.js` beside `M.hf`, its C
side in `M.provider.c`: see [providers.md](providers.md).

`std.net`'s `fetchText` is Node.js's `fetch` in JavaScript.  In C it is
libcurl, loaded into the process when the program first fetches (no
`-lcurl`), its connections kept for the next fetch; where there is no
libcurl (a static musl program, Windows), a `curl` spawned for each.
Either way a failure is `TypeError: fetch failed: ` and what `curl
-sSL --fail` says.

## Examples

`examples/` has apps for the browser: `node examples/build.mjs` builds
each to `.heff/examples/NAME/`, where its `index.html` opens.
`examples/testbridge/` is a project of another kind, built by `heff
build` there: a Linux program, through C, that makes a bridge and a
veth pair by rtnetlink, its socket's calls a provider of its own.
