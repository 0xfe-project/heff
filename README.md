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

A [release](https://github.com/dannypsnl/heff/releases) has the
compiler ready to run, needing nothing but Node.js:

```sh
tar xzf heff-0.1.0.tar.gz
node heff-0.1.0/output/heff.mjs help
```

From the repository:

```sh
npm install       # also builds heff
npm link          # puts `heff` on the PATH
```

heff is written in heff (`src/heff/`).  `boot/heff.mjs` is a compiled
copy of the compiler; the build (`npm run build`) uses it to compile the
compiler into `output/heff.mjs`, which is `heff` (minified; the
compiler's own output is `output/heff.raw.mjs`).

`editors/vscode` is a VS Code extension for the language server; a
release has it built (`heff-X.Y.Z.vsix`).

## Commands

```sh
heff check FILE                # the file's interface, and its errors
heff run FILE [ARGS...]        # run the file's `main` (on Node.js)
heff test FILE...              # check the watches' expectations (`-- >>> e`)
heff promote FILE              # write each watch's value as its expectation
heff fmt [--check] FILE...     # format in place
heff compile FILE OUT --backend=MODE [--name NAME]
                               # esm: an ES module
                               # page: a script running the app NAME in a web page
                               # node: a Node.js program running NAME
heff init [DIR]                # a new project
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
Compiled modules are cached in `.heff/`.  A package required is fetched
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
`M.provider.js` beside `M.hf`.

`examples/` has apps for the browser (`npm run examples`).

## Developing

`npm test` builds the compiler and runs the tests.  Among them,
`test/golden/` holds the output of `check`, `explain`, `eval` and
`type-at` on the library and test files; after an intended change,
update it with `node test/golden.mjs --update`.

`npm run boot` replaces `boot/heff.mjs` with the current compiler, once it
compiles itself to an identical file.  This is needed only when a
language change means the old copy can no longer compile the source.
