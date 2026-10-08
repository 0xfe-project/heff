# heff

heff is a language based on Hippogriff (Lynch & Staton, [arXiv:2608.19728](https://arxiv.org/abs/2608.19728)), a typed language with a phase distinction, with a Haskell-like syntax, modules, doctests, algebraic effects, a formatter, a language server, and a compiler to JavaScript for web pages and Node.js.

```haskell
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
tar xzf heff-0.3.0-linux-x64.tar.gz
heff-0.3.0-linux-x64/bin/heff help
```

From the repository, with Node.js and [zig](https://ziglang.org/download/):

```sh
node scripts/build.mjs     # .heff/out/heff
.heff/out/heff install     # ~/.heff/bin/heff (HEFF_HOME/bin/heff)
```

`heff install` lays the heff home out as a release is: `bin/heff`, and
beside it what heff finds there, its library and its version, `lib/`
and `package.hf`, which `app/heff/build.hf` ships.

heff is written in heff.  The package's modules (`src/heff/`) are the
compiler as a library, each command's work among them
(`heff.commands`); the program `heff` is `app/heff/main.hf`, which reads
the command line and gives the commands the world they run in.
The build (`node scripts/build.mjs`) compiles the program to C with the
heff built before (`.heff/out/heff`), and `zig cc` builds it into
`.heff/out/heff`.  Where there is no heff yet, or only one of another
seed, it starts from the seed, `boot/heff.tar.xz`: heff compiled by
itself to C, which zig builds for whatever machine it is on.

`editors/vscode` is a VS Code extension for the language server; a
release has it built (`heff-X.Y.Z.vsix`).  `editors/bat` is its grammar
for bat.

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
                               # built to .heff/bin/ (C by zig -O1: quick to
                               # build again)
heff install [DIR]             # the project at DIR built (C by zig -O2: to run
                               # fastest), its programs put in ~/.heff/bin/
                               # (HEFF_HOME/bin/), what they ship beside it
heff init [DIR]                # a new project: a module, a program, a test
heff get [PATH[@VERSION]]      # fetch the packages required, writing heff.sum
heff publish VERSION           # check the project, tag it vVERSION, push the tag
heff lsp                       # the language server, on stdio
heff version                   # which heff this is
heff help [COMMAND]            # the commands, or a command's flags
heff completions SHELL         # fish, bash or zsh completions
```

Flags go anywhere among the words, as getopt_long reads them (`-b node`,
`--back=node`, `--`); `std.cli` reads them, for any heff program.

`heff fmt` lays a file out anew, keeping its comments, and at most one
blank line between two declarations.  A line `-- fmt: skip` among the
comments right above a top-level declaration (no blank line between)
keeps the definition as it is written, byte for byte, to the end of
its last line: a signature with the equations of its name after it, or
any other declaration.  What is around it is laid out.
Anywhere else, the line is a comment like any other.

```haskell
-- | the corners: x, y, then r, g, b
-- fmt: skip
corners : List Float
corners =
  [  0.0,  0.8,   1.0, 0.0, 0.0
  , -0.8, -0.8,   0.0, 1.0, 0.0
  ,  0.8, -0.8,   0.0, 0.0, 1.0
  ]
```

On a terminal, `heff build`, `install`, `test` and `get` draw what
they are doing below what they say, as zig draws its own -- the
modules checked, each program compiled, zig's own tasks as `zig cc`
builds it, the packages fetched -- and clear it at the end; written to
a file or a pipe (or with `TERM=dumb`), they say what they say and
nothing more (`std.progress`).

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
tree of each version.  What a project requires (`require`) and the
packages it takes from directories instead (`replace`) are none where
left out, as their defaults in `lib/manifest.hf` say.

```haskell
import manifest

package : Package
package =
  { module = "github.com/someone/hello"
  , heff = "0.1.0"
  , require = [{ name = "json", path = "github.com/someone/json", version = "1.2.0" }]
  }
```

A program is `app/NAME/main.hf`, with an `app/NAME/build.hf` beside it
defining `build`, of type `Build`: the computation run, what it is
compiled to (`C`, `Node`, `Page`), and the files and directories of the
project's root it ships (`ships`, none where left out).  `heff build`
builds every program to `.heff/bin/`; `heff install` builds them into
the heff home's `bin/`, and puts what they ship beside it, in place of
what was there.

```haskell
import manifest

build : Build
build = { backend = C, run = "main" }
```

## Records

A record type names its fields, and a record gives each, in the type's
order.  A field may have a default, `x : A = d`: a record checked
against the type may leave the field out, and is given `d`.  A default
names definitions and the fields before it -- a record that leaves it
out is given it of its own fields, each computed once (`let`s around the
record) -- and nothing else where the type is, not its parameters, so
that it means the same wherever a record leaves it out; a theory's
fields may have defaults too.

```haskell
type Opts = { name : String, verbose : Bool = False, label : String = name }

quiet : Opts
quiet = { name = "quiet" }   -- { name = "quiet", verbose = False, label = "quiet" }
```

## Implicit arguments

An implicit argument, `{x : A} -> B`, is inferred where the function is
applied, unless it is given, `f (x := e)`; an instance argument,
`{{x : A}} -> B`, is found among the instance binders in scope.  An
implicit argument may have a default, `{x : A := d} -> B`: where it is
not given, it is `d`, as it is -- not inferred, so that its type may be
small.  The default names definitions only, so that it means the same
wherever the function is applied.

```haskell
greet : {loud : Bool := False} -> String -> String
greet name = if loud then name ^ "!" else name

calm : String
calm = greet "hi"                 -- "hi"; greet (loud := True) "hi" is "hi!"
```

## Operators

An operator is a name of symbols, defined as `(op) a b = ...`, and how
it binds is declared relatively: `infixl`, `infixr` or `infix`, the
operators of a group, then `above` the groups it binds tighter than and
`below` those it binds looser than, each named by any of its operators.
Binding tighter goes along a chain of them; two operators no chain
relates are refused side by side, parentheses saying which is first
(the prelude's `^` and `+`, `a + b ^ c`).  `below` alone is below all,
any module's operators too.  The prelude's `$` is: `f $ x` is `f x`, to
the right, so `f $ g $ h x` is `f (g (h x))` and `showInt $ a + b * c`
shows the sum; it is inlined, a call of `f` itself.

```haskell
infixl .+. below +

(.+.) : Int -> Int -> Int
(.+.) a b = a * 10 + b

-- >>> showInt $ 1 .+. 2 + 3
-- "15"
```

## Numbers

`Int` is an integer of any size, `Float` IEEE 754's double (`Float64`
is another name for it).  Built in beside them are the fixed-width
numbers: `Int8`, `Int16`, `Int32` and `Int64` (two's complement),
`UInt8` to `UInt64`, and `Float32` (IEEE 754's single).  An integer of
one wraps around, as the machine's does, divides rounding toward zero,
and shifts by the amount modulo its bits; each operation of a `Float32`
is rounded to one.

A number literal is of the type it is used at: an integer literal of any
number type, a fractional one (`2.5`) of a float type.  An integer
literal may be written in hexadecimal, `0x605`, or in binary,
`0b1010`, and any number's digits may be set apart by `_`
(`1_000_000`); the formatter keeps it as it is written.  A literal may
be negative: `-` right before a digit is its sign, unless it is right
after an operand (a name, a literal, a closing bracket, with no space
between).  So `a - 1` and `a-1` subtract, while `f -1` and `a -1` are
applied to -1; `-128` as an `Int8` is its least, and a pattern may be
negative too.  There is no other unary minus: `0 - x` negates.  Where the type is
not known yet, the literal waits for it, and unification finds it from
how the literal is used (`id 5` wanted as an `Int32` is one).  It must be
one of the type's values, however late the type is found: `300` as a
`UInt8` is an error, as `1.5` as an `Int32` or `5` as a `String` is.
What nothing decides is an `Int`, or a `Float`, when the definition is
settled, before its instance arguments are last looked for (an integer
literal is a `Float` where it shares its type with a fractional one,
`[1, 2.5]`).  A `let` does not generalise: what it binds has one type,
for every use.  A literal pattern is of the type of what it matches, and
compared by that type's equality (an `Int`'s is the prelude's `intEq`,
a fixed-width type's builtin's, `int32Eq` and the others).

```haskell
import uint8 as U8

mask : UInt8
mask = 240

-- >>> U8.show (U8.add mask 255)
-- "239"
```

Each has a module, imported by a name (`import int32 as I32`), whose
operations are named as the prelude's: `add`, `div`, `rem`, `compare`,
`show`, the bits' (`and`, `or`, `xor`, `complement`, `shiftL`,
`shiftR`), and `fromInt` and `toInt`, by way of which one type's value
becomes another's (`I8.fromInt (I32.toInt x)`); `float32`'s has
`fromFloat`, `toFloat` and `sqrt`.  An `Int`'s bits are `int`'s
(`import int as I`: `I.and`, `I.or`, `I.xor`, `I.complement`,
`I.shiftL`, `I.shiftR`), as two's complement has them with the sign
extended for ever; a shift right rounds down, and one by a negative
count abandons.  A fixed-width type's operators, `+` to `>=`, are its
module's too, for a module that imports it as it is: the prelude's,
which it has without writing it, give way to them.  A provider is given them as the backend keeps them: in
JavaScript an `Int`, an `Int64` and a `UInt64` are BigInts, any other
number a number; in C an integer of up to 32 bits is a word of its value
(`INT_OF`), one of 64 bits is kept as an `Int` is, a `Float32` is a word
of its bits (`heff_f32_of`, `heff_f32`), and a `Float` is boxed
(`FLT_OF`, `heff_flt`).  So in C a `Float`, and an `Int64` or `UInt64`
beyond 62 bits (as an `Int` is), costs an allocation each value, where
an integer of up to 32 bits and a `Float32` cost none.

## Effects

`lib/co.hf` implements effects after Yang and Wu (POPL 2026).  A program
names the effects it needs as instance arguments, in any world that has
them -- it can do nothing else -- and is given them where it runs:

```haskell
import co
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
says where it is, else it is the `lib/` the compiler came with: the
repository's, a release's, or the heff home's (`heff install` puts it
there).  Its modules alone declare primitives --
`primitive intAdd : Int -> Int -> Int`, a function the runtime has --
and each primitive once: one a module of a program declares, or one
declared again, is an error.  Every module imports `builtin` and `prelude` without writing
them, and `list` as `List`, the operations of lists (`List.map`,
`List.length`) -- but those they are made of.  A name is the module's
own definition's, else an import's it writes, else one of those it has
without writing them, a primitive (the prelude's `intAdd`) as any other
name; two imports written that have it are an error
only where it is used unqualified.  An import a module has already is
an error -- `import prelude`, or one written twice -- but
`import prelude as P` is another.  Its other modules are imported by
their names: `co`, `state`, `exn`, `parallel`, `array`,
`map`, `set`, and `manifest` (the types of `package.hf` and
`build.hf`); the fixed-width numbers' are `int8` to `int64`, `uint8` to
`uint64` and `float32`, and `int` has an `Int`'s bits.  The batteries
ship with it, under `lib/std/`: `std.io` (the standard streams),
`std.filesystem`, `std.process`, `std.time`, `std.net`, `std.ref` (the world's cells),
`std.dom` (web pages), `std.handle` (the world's things, which one
effect hands out and another takes), `std.path`, `std.cli` (command
lines), `std.doc` (documents laid out), `std.progress` (what a
program is doing, drawn on a terminal) and `std.math` (a float's
functions: `sin`, `atan2`, `exp`, `log`, `pow`, `round`, `pi`, `e` and
the like).  An effect's JavaScript side is in `M.provider.js` beside
`M.hf`, its C side in `M.provider.c`; the things they hand out the
world holds for them all.

`examples/` has apps for the browser: `node examples/build.mjs` builds
each to `.heff/examples/NAME/`, where its `index.html` opens.
`examples/testbridge/` is a project of another kind, built by `heff
build` there: a Linux program, through C, that makes a bridge and a
veth pair by rtnetlink, its socket's calls a provider of its own.

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

lists : Test
lists = group "lists"
  [ unit "append" (equal (showList showInt) ([1] ++ [2]) [1, 2])
  , property "reversed twice" (do
      xs <- forAll (showList showInt) (G.list 0 20 (G.int 0 99))
      label (if List.null xs then "empty" else "not empty")
      equal (showList showInt) (List.reverse (List.reverse xs)) xs)
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
(`bin/`, their C in `build/`), what `heff run` runs (`run/`), the
example apps (`examples/`), and the tests' work directories (`ref/`).

`node scripts/boot.mjs` replaces the seed with the current compiler's
C, once it compiles itself to the same C.  This is needed only when a
language change means the old seed can no longer compile the source.
