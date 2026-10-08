# heff

heff is a language based on Hippogriff (Lynch & Staton, [arXiv:2608.19728](https://arxiv.org/abs/2608.19728)), a typed language with a phase distinction, with a Haskell-like syntax, modules, doctests, algebraic effects, a formatter, a language server, and a compiler to JavaScript for web pages and Node.js.

```haskell
import prelude

map : {a b : Type} -> (a -> b) -> List a -> List b
map f Nil = Nil
map f (y :: ys) = f y :: map f ys

-- >>> map not [True, False]
-- Cons False (Cons True Nil)
```

## Installing

A [release](https://github.com/dannypsnl/heff/releases) has heff for
Linux, macOS and Windows, a program needing nothing installed:

```sh
tar xzf heff-0.2.0-linux-x64.tar.gz
heff-0.2.0-linux-x64/bin/heff help
```

From the repository, with Node.js and [zig](https://ziglang.org/download/):

```sh
node scripts/build.mjs     # .heff/out/heff
```

heff is written in heff.  The package's modules (`src/heff/`) are the
compiler as a library, each command's work among them
(`heff.commands`); the program `heff` is `app/heff/main.hf`, which reads
the command line and gives the commands the world they run in.
The build (`node scripts/build.mjs`) compiles the program to C with the
heff built before (`.heff/out/heff`), and `zig cc` builds it into
`.heff/out/heff`.  Where there is no heff yet, it starts from the seed,
`boot/heff.tar.xz`: heff compiled by itself to C, which zig builds for
whatever machine it is on.

`editors/vscode` is a VS Code extension for the language server; a
release has it built (`heff-X.Y.Z.vsix`).

## Commands

```sh
heff check FILE                # the file's interface, and its errors
heff run FILE [ARGS...]        # run the file's `main` (on Node.js)
heff test [--seed N] [FILE...] # check the watches' expectations (`-- >>> e`),
                               # and run the tests (lib/check.hf); no FILE:
                               # the project's test/
heff promote FILE              # write each watch's value as its expectation
heff fmt [--check] FILE...     # format in place
heff compile FILE OUT --backend=MODE [--name NAME]
                               # esm: an ES module
                               # page: a script running the app NAME in a web page
                               # node: a Node.js program running NAME
heff build                     # the whole project: every module of src/
                               # checked, then every program (app/NAME/)
                               # built to .heff/bin/
heff install [DIR]             # the project at DIR built, its programs put in
                               # ~/.heff/bin/ (HEFF_HOME/bin/)
heff init [DIR]                # a new project: a module, a program, a test
heff get [PATH[@VERSION]]      # fetch the packages required, writing heff.sum
heff publish VERSION           # check the project, tag it vVERSION, push the tag
heff lsp                       # the language server, on stdio
heff help [COMMAND]            # the commands, or a command's flags
heff completions SHELL         # fish, bash or zsh completions
```

Flags go anywhere among the words, as getopt_long reads them (`-b node`,
`--back=node`, `--`); `std.cli` reads them, for any heff program.

## Modules

A project is a directory with a `package.hf`: a heff file defining
`package`, of type `Package` (`lib/manifest.hf`), read without being
run.  A module is a file under its `src/`, named by its path there:
`src/parse/surface.hf` is `parse.surface`.  Modules are looked up there
and in `lib/` (`std.io` is `lib/std/io.hf`); a file outside `src/` (a
test) may also import files beside it, by their paths from the root.
Compiled modules are cached in `.heff/`, where all heff makes is kept.  A package required is fetched
by git from its path (`heff get`), the version chosen as Go's minimal
version selection does, into `~/.cache/heff/mod`; `heff.sum` holds the
tree of each version.

```haskell
import manifest

package : Package
package =
  { module = "github.com/someone/hello"
  , heff = "0.1.0"
  , require = []
  , replace = []
  }
```

## Effects

`lib/co.hf` implements effects after Yang and Wu (POPL 2026).  A program
names the effects it needs as instance arguments, in any world that has
them -- it can do nothing else -- and is given them where it runs:

```haskell
import co
import prelude
import std.filesystem
import std.io

world : HFunctor
world = stdio <+> fs

program : {e : HFunctor} -> {{io : stdio <: e}} -> {{disk : fs <: e}} -> Co e Unit
program = do
  ok <- writeFile "hello.txt" "hello"
  print (if ok then "written" else "not written")

main : Co world Unit
main = program (io := \p -> InL p) (disk := \p -> InR p)
```

## The library

`lib/` goes with the compiler, its version the compiler's; `HEFF_LIB`
says where it is, else it is the `lib/` beside the compiler.  Its
modules are imported by their names: `builtin` (implicit in every
module), `prelude`, `co`, `state`, `exn`, `par`, `array`, `map`, `set`,
and `manifest` (the type of `package.hf`).  The batteries ship with it,
under `lib/std/`: `std.io` (the standard streams), `std.filesystem`,
`std.process`, `std.time`, `std.net`, `std.ref` (the world's cells),
`std.dom` (web pages), `std.path`, `std.cli` (command lines) and
`std.doc` (documents laid out).  An effect's JavaScript side is in
`M.provider.js` beside `M.hf`, its C side in `M.provider.c`.

`examples/` has apps for the browser (`node examples/build.mjs`).

## Tests

A watch, `-- >>> e` with its expected value under it, is an example in a
definition's documentation.  A test is a top-level definition of type
`Test` (`lib/check.hf`); `heff test` runs a file's tests after its
watches.  A unit test checks; a property draws values from generators
(`lib/gen.hf`, imported `as G`) and checks them, a hundred runs:

```haskell
import check
import co
import gen as G
import prelude

lists : Test
lists = group "lists"
  [ unit "append" (equal (showList showInt) (concatList [1] [2]) [1, 2])
  , property "reversed twice" (do
      xs <- forAll (showList showInt) (G.list 0 20 (G.int 0 99))
      label (if null xs then "empty" else "not empty")
      equal (showList showInt) (reverse (reverse xs)) xs)
  ]
```

```
  ok    lists / append
  ok    lists / reversed twice  (100 runs, 17% empty, 83% not empty)
ok    lists.hf  0/0, tests 2/2
```

Randomness and checking are effects: a unit test's type grants it only
`check`, a property also `random`, and the runner gives both.  A
generator makes each value with the smaller ones to try instead
(Hedgehog's rose trees), so a property that fails is shrunk, every draw
of it, with nothing written for it; the runner then says the
counterexample and the seed to run it again (`--seed N`; the default
seed is always the same, so a run of the tests is always the same run).
A label counts the runs it is given, so that a property that holds only
of trivial values shows.  There are no type classes, so a value is
shown, and compared by `equal`, by a function given, and a generator is
given by hand.  `agree` runs a program of commands on a system and on a
model of it and compares what they observe; drawn by a property, the
program shrinks to a short one that tells them apart
(`test/check/map.hf` holds `lib/map.hf` to a list of pairs).

## Developing

`node test/run.mjs [GROUP...]` runs the tests on `.heff/out/heff` (or on
`HEFF`), by group: `compiler`, `lsp`, `backends` (each backend held to
the interpreter), `fixpoint` (heff compiles itself to the same C) and
`asan` (heff under AddressSanitizer).  Among them,
`test/golden/` holds the output of `check`, `explain`, `eval` and
`type-at` on the library and test files; after an intended change,
update it with `node test/golden.mjs --update`.

All that is made is under `.heff/`: the compiler built (`out/`), the
cache of compiled modules (`vN/`), the programs `heff build` makes
(`bin/`, their C in `build/`), what `heff run` runs (`run/`), and the
tests' work directories (`ref/`).

`node scripts/boot.mjs` replaces the seed with the current compiler's
C, once it compiles itself to the same C.  This is needed only when a
language change means the old seed can no longer compile the source.
