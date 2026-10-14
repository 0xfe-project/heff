# Commands

`heff help` lists them, and `heff help COMMAND` says a command's flags.
`heff` with no words, on a terminal, is an app of them to learn them by
([below](#the-commands-app)).

```sh
heff check FILE                # the file's interface, and its errors
heff run FILE [ARGS...]        # run the file's `main` (C, by zig -O1)
heff test [--seed N] [--run REGEXP] [--jobs N] [--update] [--watch] [FILE...]
                               # check the doc tests' and watches' expectations
                               # (`-- | >>> e`, `-- >>> e`), and run the tests
                               # (check's); no FILE: the project's test/;
                               # --run: only the tests whose names it matches;
                               # --jobs: integration tests at once; --update:
                               # write the golden texts; --watch: test again
                               # at each change, until ended
heff bench [--run REGEXP] [--time D] [--count N] [--json] [FILE...]
                               # run the benchmarks (bench's), built through C
                               # (zig -O2), each until a run takes the time (1s),
                               # a line each, as Go's; no FILE: the project's bench/
heff promote FILE              # write each doc test's and watch's value as
                               # its expectation
heff fmt [--check] FILE...     # format in place
heff compile FILE OUT --backend=MODE [--name NAME]
                               # page: a script running the app NAME in a web page
                               # esm: an ES module
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
heff init [DIR]                # a new project: a module, a program, a test,
                               # named as DIR (the working directory)
heff get [PATH[@VERSION]]      # fetch the packages required, writing heff.sum
heff publish VERSION           # check the project, tag it vVERSION, push the tag
heff dev [APP...] [--port N] [--proxy URL]
                               # the project built, its C programs run and its
                               # pages served, again at each change of its files
heff lsp                       # the language server, on stdio
heff version                   # which heff this is
heff help [COMMAND]            # the commands, or a command's flags
heff completions SHELL         # fish, bash or zsh completions
```

Flags go anywhere among the words, as getopt_long reads them (`-b c`,
`--back=c`, `--`); `std.cli` reads them, for any heff program.

The backends are `page` and `esm`, JavaScript for the browser, and `c`,
for everything else; a program imports the library's half that is its
backend's, `lib/browser/` or `lib/std/` ([library.md](library.md#the-batteries)).

`heff run` compiles FILE's `main` to C in `.heff/run/F/` (F the file's
path, its separators `_`), a C file a module, through the backend's
cache, as `heff build` does; builds it there by `zig cc -O1`, which
builds again only the files that changed; and runs it in heff's place,
on its standard streams, with ARGS.  So it needs zig, as `heff build`
does; run again with nothing changed, it costs a check of the cache and
a link.

What `heff test` and `heff promote` check is in [tests.md](tests.md);
what `heff bench` runs, and how, in [benchmarks.md](benchmarks.md);
how a project and its programs are laid out for `heff build`, `heff
install` and `heff get`, in [projects.md](projects.md).

## The commands' app

`heff` with no words, its standard input and output a terminal (and
`TERM` not `dumb`), shows its commands, each with what it does, on a
screen of its own; elsewhere -- a pipe, a file -- it says its usage, as
`heff help` does.  Typing filters the list by the commands' names; up
and down (or `j` and `k`, Ctrl-N and Ctrl-P), Page Up and Down, Home and
End move in it; the status line says how the command picked is called.
Enter (or Tab) opens its form: a field for each of its positionals and
flags, what the one being filled in takes said under them (what it is
for, one word or more, a file of which kind, its values, whether it is
wanted), and the command line the form makes, as it is filled in.  Tab
and down go to the next field, Shift-Tab and up to the one before; a
field of words is typed in as a shell's line is (the arrows, Home, End,
Ctrl-A, -E, -U, -K, -W), a choice -- a switch's off and on, a flag's
values -- turned by left and right or space.  The status line says what
is wrong with the command line yet (`expected FILE`), or that Enter runs
it: the screen put back, the command line written to the standard
error, `$ heff fmt a.hf --check`, and run, as if it had been given.  Esc
goes back to the list (and `q`, on a choice), and on the list clears the
filter, then leaves, as `q` does; to type a `j`, `k` or `q` into the
filter, type `/` first.  Ctrl-C ends heff, the terminal put back.

The app is std.cli.interactive's, of the commands std.cli reads -- the
same definitions the usage, the help and the completions are written
from -- run by std.terminal ([library.md](library.md#the-terminal)).

## heff dev

`heff dev`, at a project's root, builds it as `heff build` does (heff
run as `heff build`, so that a build that fails says why and is waited
past), runs its C programs from `.heff/bin/` -- the APPs named, else
all -- and serves its pages, then looks at the project's files
(`package.hf`, `src/`, `app/`) every quarter of a second: at a change,
it builds again and, where that went well, ends what runs and runs it
anew.  What it says is a line for each build (`heff dev: built (2);
running server`), what a build that failed said, and a line where a
program ends by itself; the programs' own output is theirs, on the same
terminal; a build is drawn as it goes, as `heff build` draws one
([progress](#progress)).  A program writes its standard output a line
at a time on a terminal, but a buffer at a time into a pipe: what it
says reaches a pipe late, where it does not write to its standard
error.

A page (a program of `backend = Page`) is served at
`http://127.0.0.1:PORT/` (`--port`, 8000): `/NAME/` is the program
NAME's page -- its `app/NAME/index.html`, else one made for it, an
element `#NAME` and its script -- `/NAME/NAME.js` its script as built,
and `/NAME/...` the rest of `app/NAME/` as it is.  A page so served asks
every half second whether the project has been built again
(`/.heff/built`), and loads itself again where it has.  With `--proxy
URL` (`http://127.0.0.1:8080`, a C program's server), a request that is
none of a page's -- the API a page calls, `POST /api` -- is sent there,
and its response given back, so that a page and its server
([remote.md](remote.md)) are developed together:

```sh
cd examples/notes
heff dev --proxy http://127.0.0.1:8080   # the server run, the page at
                                         # http://127.0.0.1:8000/client/
```

Ctrl-C ends heff dev and what it runs; so does a signal that ends it.

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

Where a line breaks is the writer's to choose, as with ormolu and
elm-format: a record, a record type or update, a list, a tuple or a
`data { .. }` written over several lines stays one item a line, commas
first, even if it would fit on one; an application, a chain of
operators (a signature's arrows among them), `if`'s `then` and `else`,
and `let`'s `in` written broken stay broken.  Written on one line,
each is put on one if it fits, and broken if it does not.

```haskell
type ElabCtx =
  { type_context : List VType
  , env : List Value
  }
```

An equation's `where` goes on a line of its own, under the equation,
and its local definitions at the column of the `where`, not past it; a
theory's fields, one to a line, past the `theory`.

```haskell
nested n = outer n
  where
  outer : Int -> Int
  outer m = inner m
    where
    inner : Int -> Int
    inner k = k * n
```

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

## The language server

`heff lsp` speaks the Language Server Protocol on its standard streams:
diagnostics as the text is typed, hover, go to definition, completion,
formatting, an outline, references, renaming and symbols across the
project, and one code action, splitting a variable into cases.  On a
variable of an equation's patterns, of a `data` type or a record type,
it writes the equation again for each of the type's tags (once, a
record's fields named), the variable replaced by the tag applied to
names the equation has none of; the other patterns and the body are
kept as they are.

```haskell
len : List Int -> Int
len xs = 0
-- split `xs`:
len Nil = 0
len (x :: xs1) = 0
```

A variable of another type, a lambda's, or one a `case` matches is not
split: what a split makes is equations.

## Progress

On a terminal, `heff build`, `install`, `test` and `get` draw what
they are doing below what they say, as zig draws its own -- the
modules checked, each program compiled, zig's own tasks as `zig cc`
builds it, the packages fetched -- and clear it at the end; written to
a file or a pipe (or with `TERM=dumb`), they say what they say and
nothing more (`std.progress`).

## How deep a program goes

A tail call holds no place on the stack: of a function of itself, or of
functions calling one another last round a cycle -- of whatever
arities, made one loop, on either backend and whatever C compiler
builds it -- a loop; of a closure, the stack let go of every so often.
A call that is no tail call holds its place on the stack until it
returns -- but for a call of itself put in a constructor's last field
(`map f (x :: xs) = f x :: map f xs`) or right of a string append
(`show x ^ "," ^ showAll xs`): such a function is made a loop, on
either backend, however long its list.  A C program (`c`) runs in a
thread of its own, with a stack of 1 GiB (`-DHEFF_MAIN_STACK=BYTES` when
its C is built, for another size; where it runs out, the program says
`heff: the stack overflowed` and ends with 1): recursion millions deep
-- twenty million of `sumTo n = if n == 0 then 0 else n + sumTo (n -
1)`.  An ES module (`esm`) and a page's script (`page`) run on the
stack of what loads them -- Node.js's own, a browser's -- which has
room for some ten thousand such calls (on Node.js 26, 9765 of `sumTo`,
fewer of one that calls a closure); a call deeper throws a
`RangeError`.

heff does not take those calls off the stack: a trampoline, or
continuations, would slow every call that is no tail call (twice as
slow, `sumTo` measured), for a depth few modules need.  What must go
deeper runs as a C program, recurs by a tail call (`List.foldl`, not
`List.foldr`), or is loaded on a larger stack: a `Worker`'s
`resourceLimits.stackSizeMb`, or `node --stack-size=N`, kept within
what the system gives the thread.
