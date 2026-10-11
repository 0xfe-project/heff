# Commands

`heff help` lists them, and `heff help COMMAND` says a command's flags.

```sh
heff check FILE                # the file's interface, and its errors
heff run FILE [ARGS...]        # run the file's `main` (on Node.js)
heff test [--seed N] [FILE...] # check the doc tests' and watches' expectations
                               # (`-- | >>> e`, `-- >>> e`), and run the tests
                               # (lib/check.hf); no FILE: the project's test/
heff promote FILE              # write each doc test's and watch's value as
                               # its expectation
heff fmt [--check] FILE...     # format in place
heff compile FILE OUT --backend=MODE [--name NAME]
                               # esm: an ES module
                               # page: a script running the app NAME in a web page
                               # node: a Node.js program running NAME
                               # c: a C program running NAME, for `zig cc`
heff explain FILE              # the elaboration's trace
heff type-at FILE OFFSET...    # what the name at each offset resolved to, and its type
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

What `heff test` and `heff promote` check is in [tests.md](tests.md);
how a project and its programs are laid out for `heff build`, `heff
install` and `heff get`, in [projects.md](projects.md).

## Formatting

`heff fmt` lays a file out anew, keeping its comments, and at most one
blank line between two declarations; a doc test or a watch is kept as
written, after its `>>>` and a space.  A line `-- fmt: skip` among the
comments right above a top-level declaration (no blank line between),
or above the doc tests of its doc comment, keeps the definition as it
is written, byte for byte, to the end of its last line: a signature
with the equations of its name after it, or any other declaration.
What is around it is laid out.  Anywhere else, the line is a comment
like any other.

```haskell
-- | the corners: x, y, then r, g, b
-- fmt: skip
corners : List Float64
corners =
  [  0.0,  0.8,   1.0, 0.0, 0.0
  , -0.8, -0.8,   0.0, 1.0, 0.0
  ,  0.8, -0.8,   0.0, 0.0, 1.0
  ]
```

## Progress

On a terminal, `heff build`, `install`, `test` and `get` draw what
they are doing below what they say, as zig draws its own -- the
modules checked, each program compiled, zig's own tasks as `zig cc`
builds it, the packages fetched -- and clear it at the end; written to
a file or a pipe (or with `TERM=dumb`), they say what they say and
nothing more (`std.progress`).

## How deep a program goes

A call that is no tail call holds its place on the stack until it
returns -- but for a call of itself put in a constructor's last field
(`map f (x :: xs) = f x :: map f xs`) or right of a string append
(`showInt x ^ "," ^ showAll xs`): such a function is made a loop, on
either backend, however long its list.  A C program (`c`) runs in a
thread of its own, with a stack of 1 GiB (`-DHEFF_MAIN_STACK=BYTES` when
its C is built, for another size; where it runs out, the program says
`heff: the stack overflowed` and ends with 1), and a Node.js program (`node`, what `heff run` runs) in a worker
thread, with one of 512 MB: recursion millions deep -- three million of
`sumTo n = if n == 0 then 0 else n + sumTo (n - 1)`.  An ES module
(`esm`) and a page's script (`page`) run on the stack of what loads
them -- Node.js's own, a browser's -- which has room for some ten
thousand such calls (on Node.js 26, 9765 of `sumTo`, fewer of one that
calls a closure); a call deeper throws a `RangeError`.

heff does not take those calls off the stack: a trampoline, or
continuations, would slow every call that is no tail call (twice as
slow, `sumTo` measured), for a depth few modules need.  What must go
deeper runs as a Node.js program, recurs by a tail call (`List.foldl`,
not `List.foldr`), or is loaded on a larger stack: a `Worker`'s
`resourceLimits.stackSizeMb`, as a Node.js program is, or
`node --stack-size=N`, kept within what the system gives the thread.
