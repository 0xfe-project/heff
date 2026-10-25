---
title: Commands
---

| Command | What it does |
|---|---|
| `heff init [DIR]` | make a project: a module, a program and a test |
| `heff build` | check every module of `src/`, and build every program into `.heff/bin/` |
| `heff run FILE [ARGS...]` | build and run a file's `main` |
| `heff test [FILE...]` | check doc tests and watches, and run tests; no FILE: `src/` and `test/` |
| `heff bench [FILE...]` | run benchmarks; no FILE: `bench/` |
| `heff install [DIR]` | build the project to run fastest, into `~/.heff/bin/` |
| `heff get [PATH[@VERSION]]` | fetch the packages required; with PATH, require it first: at VERSION, a branch's or a commit's pseudo-version, else its newest |
| `heff publish VERSION` | check the project, tag it `vVERSION`, and push the tag |
| `heff dev [APP...]` | build and run the project, again at each change |
| `heff fmt [--check] FILE...` | format files in place |
| `heff check FILE` | print a file's interface and its errors |
| `heff doc [FILE...] [--out DIR]` | write each module's reference page; no FILE: `src/` |
| `heff promote FILE` | write each doc test's and watch's value as its expectation |
| `heff compile FILE OUT --backend=MODE [--name NAME]` | compile a file to `page`, `esm` or `c`; `page` and `c` run `main`, or NAME |
| `heff explain FILE` | print the type checker's trace |
| `heff type-at FILE OFFSET...` | print what the name at each offset is, and its type |
| `heff lsp` | run the language server on stdin and stdout |
| `heff version` | print heff's version |
| `heff help [COMMAND]` | list the commands, or one command's flags |
| `heff completions SHELL` | print completions for `fish`, `bash` or `zsh`, and the shell function that keeps the command chosen in `heff`'s app in the history |

`heff help COMMAND` lists a command's flags. Flags go anywhere among the
words, as `getopt_long` reads them: `-b c`, `--backend=c`, `--`.

`heff` alone, on a terminal, opens an app of the commands: type to
filter them, Enter to fill in a command's arguments in a form, and
Enter again to run it. A command missing what it needs (`heff run`, no
FILE) opens its form, filled in with what was given; Esc leaves it.

The shell runs what is chosen there, and keeps it in its history, when
it has sourced the completions (`heff completions fish | source`, `eval
"$(heff completions bash)"`). zsh keeps the command chosen in addition
to what was typed, except for `heff` alone.

## Building and running

- `heff build` and `heff run` build C with `zig -O1`, quick to build
  again. `heff install` and `heff bench` use `zig -O2`, quick to run.
  All of them need zig.
- `heff run` builds again only what changed, so a second run costs a
  check and a link.
- The backends are `page` and `esm` (JavaScript, for the browser) and
  `c` (everything else). A program imports the library for its backend,
  `lib/browser/` or `lib/std/`.
- `heff compile --backend=c` and `--backend=page` build a program that
  runs the file's `main`, or the definition `--name` gives; a file
  without it is refused. `--backend=esm` exports every definition and
  runs none, so it takes no `--name`.
- OUT's directory must be there. For `c`, an OUT ending in `/` is a
  directory of C files, a file a module, and is made if it is not there.

## Testing

`heff test` flags:

| Flag | Meaning |
|---|---|
| `--run REGEXP` | only the tests whose names match |
| `--seed N` | the seed for properties |
| `--jobs N` | integration tests run at once |
| `--update` | write golden texts |
| `--watch` | test again at each change |

See [Tests](tests.md) and [Benchmarks](benchmarks.md).

## Reference pages

`heff doc` makes a Markdown page of each module: its title the module's
name, the module's header comment (the comment above its `module`
line), then each definition it exposes, in order, with its signature
as `heff check` prints it and its doc comment, the doc tests shown as
examples. What the module claim does not list has no entry.

- Without `--out`, the pages are printed. With `--out DIR`, a module
  `std.path` is written to `DIR/std/path.md`, and `DIR/index.md`
  lists the modules, each with the first sentence of its header
  comment.
- Each definition's heading ends in `{#NAME}`, its anchor: the name
  itself, `#foldMap`; an operator's is `op-` and a word for each of
  its characters, `<+>` is `#op-lt-plus-gt`.
- A module that does not check has no page; its errors are said, and
  `heff doc` fails.

The site's [API pages](https://dannypsnl.github.io/heff/api/) are
`heff doc lib --out docs/api` (`make api`), made each time the site is
published.

## heff dev

```sh
cd examples/notes
heff dev
```

`heff dev` builds the project and runs its C programs (the APPs named,
or all). When a file of `package.hf`, `src/` or `app/` changes, it
builds again and restarts what runs.

- A page's script is built as `heff build` builds it; serve the page
  with any file server.
- Ctrl-C stops it and what it runs.

## Formatting

`heff fmt` lays a file out again, keeping its comments.

- Where a line breaks is yours to choose. A list, record or application
  written over several lines stays broken, one item a line, commas
  first. Written on one line, it stays on one line if it fits.
- `where` goes on its own line under the equation, and its definitions
  at the same column.
- A `-- fmt: skip` line right above a declaration keeps it exactly as
  written.

```haskell
type Context =
  { types : List String
  , values : List Int
  }

-- fmt: skip
corners : List Float64
corners =
  [  0.0,  0.8,   1.0
  , -0.8, -0.8,   0.0
  ]
```

## The language server

`heff lsp` gives diagnostics as you type, hover, go to definition,
completion, formatting, an outline, the occurrences of a name in the
file (highlighted), references, renaming and symbols across the project.

Go to definition and references work for locals as well as globals: a
local's definition is its binder (a parameter, a `let`, a pattern, a
lambda's, `<-`, `let mut`, `for`, a `where` definition), and its
references are the uses of that binder alone, not of a name it shadows
or one that shadows it.  A global's references are found across the
project: qualified and aliased uses, operators and their fixity
declarations, tags, record and theory fields, the module claim's entry,
and names in doc tests.  Only the files whose text has the name are
elaborated for that, the rest read from the cache, so a project of
hundreds of files answers in about a second.

A code action splits a variable of an equation into cases: the
equation is written again for each tag of the variable's type.

```haskell
len : List Int -> Int
len xs = 0
-- split `xs`:
len [] = 0
len (x :: xs1) = 0
```

A quick fix writes a file's module claim where it is missing (E0156)
or names the module otherwise (E0157): `module` and the name the file's
path gives it.  An editor asks for that name by `heff/moduleName`, whose
`textDocument` is the file, and gets the name, or `null` for a manifest
or a file no module claim can name; the VS Code extension writes the
module claim of an empty `.hf` file this way when it is opened, as one just made is.

## Progress

On a terminal, `heff build`, `install`, `test` and `get` show what they
are doing below their output, and clear it at the end. Into a file or a
pipe, or with `TERM=dumb`, they print only their output.

## Recursion depth

A tail call takes no stack, on every backend. So does a call of the
function itself in a list's tail, `f x :: map f xs`, or after `^`.

Other calls take stack:

- A C program has a 1 GiB stack: recursion millions deep. When it runs
  out, the program says `heff: stack overflow` and exits with 2. A
  thread of std.thread has a 256 MiB stack; when it runs out, the
  thread fails, and the program fails so when the thread is joined.
  `-DHEFF_MAIN_STACK=BYTES` when building its C sets another size.
- A JavaScript program runs on the stack of what loads it, room for
  about ten thousand calls. Deeper, it throws a `RangeError`.

What must go deeper should use a tail call (`List.foldl`, not
`List.foldr`), or run as a C program.
