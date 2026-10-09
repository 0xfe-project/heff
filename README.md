# heff

heff is a functional language with a Haskell-like syntax, based on
Hippogriff (Lynch & Staton, [arXiv:2608.19728](https://arxiv.org/abs/2608.19728)),
a typed language with a phase distinction.

- **Types at compile time.** Types, functions of types and records of
  them (theories) are values the type checker computes with -- a
  function takes its types as arguments, `{a : Type} -> ...` -- while
  run-time data (numbers, strings, data types) never reaches a type.
- **Algebraic effects**, as a library (`lib/co.hf`, after Yang and Wu,
  POPL 2026): a computation names the effects it needs as instance
  arguments, and is given them where it runs.
- **Self-hosting**: the compiler is written in heff, and builds from a
  seed of itself compiled to C.
- **Two backends**: C (native programs, by `zig cc`) and JavaScript
  (Node.js programs, web pages, ES modules).
- **Tools** in the one executable: a formatter, a language server, doc
  tests and property tests, and packages fetched by git.

```haskell
import co
import std.io

-- | `f` applied to each element
-- | >>> map not [True, False]
-- | [False, True]
map : {a b : Type} -> (a -> b) -> List a -> List b
map f Nil = Nil
map f (y :: ys) = f y :: map f ys

-- | each line printed: a computation that needs the standard streams,
-- | in any world `e` that has them
printAll : {e : HFunctor} -> {{io : stdio <: e}} -> List String -> Co e Unit
printAll Nil = pure MkUnit
printAll (l :: ls) = do
  print l
  printAll ls

main : Co stdio Unit
main =
  printAll
    (io := here stdio)
    (map (\n -> "Hello, " ^ n ^ "!") ["heff", "world"])
```

`heff run hello.hf` prints the two greetings; `heff test hello.hf`
checks the doc test of `map`.

## Installing

A [release](https://github.com/dannypsnl/heff/releases) has heff for
Linux, macOS and Windows, a program needing nothing installed:

```sh
tar xzf heff-0.3.0-linux-x64.tar.gz
heff-0.3.0-linux-x64/bin/heff help
```

From the repository, with Node.js and [zig](https://ziglang.org/download/):

```sh
node scripts/build.mjs     # .heff/out/heff
.heff/out/heff install     # ~/.heff/bin/heff (HEFF_HOME/bin/heff)
```

Put `~/.heff/bin` on your `PATH`.

## Using it

```sh
heff init hello            # a new project: a module, a program, a test
cd hello
heff build                 # check src/, build each program to .heff/bin/
heff test                  # the doc tests, watches and tests of test/
heff run app/hello/main.hf # run a program on Node.js, without a build
heff install               # build to run fastest, into ~/.heff/bin/
heff fmt src/hello/*.hf    # format in place
```

`heff help` lists every command, and `heff help COMMAND` its flags.

## Editors

- **VS Code**: [`editors/vscode`](editors/vscode/README.md), a client of
  `heff lsp` -- diagnostics, hover, go to definition, completion,
  formatting, renaming.  A release has it built (`heff-X.Y.Z.vsix`).
- **bat** (and Sublime Text): [`editors/bat`](editors/bat/README.md), a
  syntax for `.hf` files.

## Documentation

- [The language](docs/language.md): modules, records, implicit
  arguments, operators, numbers, runes and strings, effects
- [The library](docs/library.md): `lib/`, the batteries of `lib/std/`,
  the examples
- [Projects](docs/projects.md): `package.hf`, packages, programs
- [Commands](docs/commands.md): every command, the formatter
- [Tests](docs/tests.md): doc tests, watches, unit tests and properties
- [Providers](docs/providers.md): an effect's JavaScript and C sides,
  strings and numbers at the host, `JSString`
- [Developing heff](docs/developing.md): the source, the seed, the
  tests

## Developing

```sh
node scripts/build.mjs           # build .heff/out/heff (from the seed, the first time)
node test/run.mjs compiler lsp   # the tests, by group
```

How the source is laid out, the seed and the test groups are in
[docs/developing.md](docs/developing.md).
