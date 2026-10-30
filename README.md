# heff

heff is a functional language with a Haskell-like syntax and a phase
distinction: types are computed at compile time, data at run time.

- **Types at compile time.** Types, functions of types and records of
  them (theories) are values the type checker computes with -- a
  function takes its types as arguments, `{a : Type} -> ...` -- while
  run-time data (numbers, strings, data types) never reaches a type.
- **Algebraic effects** as a library: a computation names the effects it
  needs as instance arguments, and is given them where it runs.
- **Self-hosting**: the compiler is written in heff, and builds from a
  seed of itself compiled to C.
- **Two backends**: C (native programs, built by zig; the runtime is
  part C, part Zig) and JavaScript (web pages, ES modules).
- **Tools** in the one executable: a formatter, a language server, doc
  tests and property tests, and packages fetched by git.

```haskell
module hello

import co
import std.io

-- | `f` applied to each element
-- | >>> map not [True, False]
-- | [False, True]
map : {a b : Type} -> (a -> b) -> List a -> List b
map f [] = []
map f (y :: ys) = f y :: map f ys

main : Co stdio Unit
main = do
  let ls : List String = ["heff", "world"]
  for x in ls do
    print $ "Hello, " ^ x ^ "!"
```

`heff run hello.hf` prints the two greetings; `heff test hello.hf`
checks the doc test of `map`.

## Installing

With [zig](https://ziglang.org/download/) and make (Node.js only for
some of the tests):

```sh
make install               # build .heff/out/heff, then install it into ~/.heff/bin/
```

Put `~/.heff/bin` on your `PATH`.

## Using it

```sh
heff init hello            # a new project: a module, a program, a test
cd hello
heff build                 # check src/, build each program to .heff/bin/
heff test                  # the doc tests, watches and tests of src/ and test/
heff run app/hello/main.hf # run a program (C, by zig), without a build
heff install               # build to run fastest, into ~/.heff/bin/
heff fmt src/hello/*.hf    # format in place
heff dev                   # build and run; again at each change
```

`heff help` lists every command, and `heff help COMMAND` its flags.

## Editors

- **VS Code**: [`editors/vscode`](editors/vscode/README.md), a client of
  `heff lsp` -- diagnostics, hover, go to definition, completion,
  formatting, renaming.  A release has it built (`heff-X.Y.Z.vsix`).
- **bat** (and Sublime Text): [`editors/bat`](editors/bat/README.md), a
  syntax for `.hf` files.

## Documentation

The tutorial and the reference: <https://0xfe-project.github.io/heff/>.

## Developing

```sh
make build   # .heff/out/heff (from the seed, the first time)
make check   # all a change is held to: from the seed, every test, the layout
make         # every target
```

How the source is laid out, the seed and the test groups are in
[DEVELOPING.md](DEVELOPING.md).

## References

- Lynch & Staton, *Hippogriff*, [arXiv:2608.19728](https://arxiv.org/abs/2608.19728):
  the core theory heff uses, and [its implementation](https://tangled.org/owenlynch.tngl.sh/hippogriff/),
  which heff refers to.
- Yang & Wu, POPL 2026, [arXiv:2511.05739](https://arxiv.org/abs/2511.05739):
  the idea behind heff's effects library, `co`.
