---
title: A first program
---

This tutorial teaches heff by writing it. Each chapter builds on the one
before; by the end you will have written a program with effects of its
own and tested it.

## Installing

heff is young and changes quickly, so install a release rather than
building from source. Download the archive for your platform from the
[release](https://github.com/dannypsnl/heff/releases) and unpack it into
`~/.heff/`:

```sh
mkdir -p ~/.heff
tar xzf heff-0.4.0-macos-arm64.tar.gz -C ~/.heff --strip-components=1
```

Then put `~/.heff/bin` on your `PATH`. heff builds native programs with
[zig](https://ziglang.org/download/), so install zig too.

## A new project

```sh
heff init hello
cd hello
```

This makes a project with one module, one program, and one test:

| File | What it is |
|---|---|
| `package.hf` | the package: its name and the heff it is for |
| `src/hello/greeting.hf` | a module, `hello.greeting` |
| `app/hello/main.hf` | a program |
| `app/hello/build.hf` | how that program is built |
| `test/greeting.hf` | a test |

## Running it

```sh
$ heff run app/hello/main.hf
Hello, world!
```

## The module

Open `src/hello/greeting.hf`:

```haskell
module hello.greeting

-- | a greeting, of `name`
-- | >>> greeting "heff"
-- | "Hello, heff!"
greeting : String -> String
greeting name = "Hello, " ^ name ^ "!"
```

The first line names the module. A definition is a type signature,
`greeting : String -> String`, followed by its equations. `^` joins
strings.

The `-- |` lines above it are its documentation, and they are also a
test. The line after `>>>` is an expression, and the line after that is
the value it should have. `heff test` checks it.

## The program

Open `app/hello/main.hf`:

```haskell
module app.hello.main

import co
import hello.greeting
import std.io

program : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
program = print (greeting "world")

main : Co stdio Unit
main = program (io := _)
```

`import hello.greeting` brings in the module above. A module's name is
its path under `src/`.

`program`'s type says what it does: it is a computation (`Co e Unit`)
that needs the standard streams (`stdio`), and nothing else. `main` runs
it and gives it those streams. [Chapter 3](effects.md#printing-the-todos)
explains how this works. For now, read `{{io : stdio <: e}}` as "to run
this, you need stdio".

## Testing it

```sh
$ heff test
ok    src/hello/greeting.hf  1/1
  ok    greeting
ok    test/greeting.hf  0/0, tests 1/1
1/1 expectations hold, 1/1 tests hold, 2/2 files check as they should
```

`heff test` checks the doc tests in `src/`, and runs the tests in
`test/`. Chapter 6 covers tests in full.
