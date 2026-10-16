---
title: Files and processes
---

These are C programs' effects, in `lib/std/`.

## std.io

`stdio` is the standard streams: `write`, `print` (with a newline),
`writeErr`, `printErr`, and `readStdin`, which reads all of the input.

## std.filesystem

```haskell
import co
import exception
import std.filesystem
import std.path

-- | the words of the standard input, read a line at a time
wordsIn : {e : HFunctor} -> {{disk : filesystem <: e}} -> Co e (Either FileError Int)
wordsIn = withStdin (\i -> foldLines i 0 (\n line -> pure (n + List.length (words line))))

-- | a file's text, with a line added
appended : {e : HFunctor} -> {{disk : filesystem <: e}} -> Path -> String -> Co e Bool
appended path line = do
  text <- readFile path
  writeFile path (fromMaybe "" text ^ line ^ "\n")

-- | the log of a day, in a directory
logged : {e : HFunctor} -> {{disk : filesystem <: e}} -> Path -> String -> Co e Bool
logged dir line = appended (dir / "log.txt") line
```

A path is `std.path`'s `Path`. A string is taken as one where one is
expected, `readFile "notes.txt"`, wherever `std.path` is imported (a
[coercion](../language/arguments.md#coercions)): it is read as the
system the program runs on writes a path.

### Whole files

`readFile` (`Maybe` the text), `writeFile`, `readDirectory`,
`makeDirectory` (and the directories it is in), `remove`, `copy`,
`move`, `replace`, `readOnly` and `writable`. Each but `readFile` and
`readDirectory` gives back whether it worked.

### What a path is

- `pathStatus path` follows links; `linkStatus path` does not. Both
  give `Either FileError Status`: its `kind` (`Regular`, `Directory`,
  `Link`, `Other`), `size`, `modified` (nanoseconds since the epoch) and
  `mode`.
- `kindAt path` is its kind, or `Nothing`; `exists`, `isFile` and
  `isDirectory` say so.
- `setMode`, `makeExecutable` (`chmod +x`), and `realPath` (links
  resolved).
- `temporaryDirectory prefix` and `temporaryFile prefix` make a new one
  of the program's own; removing it is the program's job.

## std.path

A `Path` is a drive (on Windows, `C:` or `//server/share`), whether it
starts at the root, and its segments. Only `std.path` makes one, so it
is always in that shape: no empty segment, no `.`.

| Function | What it does |
|---|---|
| `parse s`, `render p`, `toString p` | read and write one |
| `join a b`, `a / b`, `joinAll ps` | `b` from `a`: `b` itself where it is absolute |
| `dirname p`, `basename p`, `extension p`, `stem p` | its parts |
| `normal p`, `components p` | without the `..`s that can go |
| `relative a b`, `isUnder a b` | from one to another |

Reading and writing a path take a style, `os`: `Posix` (`/a/b`) or
`Windows` (`C:\a\b`, `\\server\share\a`, either slash read). Unless
it is given, it is `native`, the system the program runs on, so a string
from the program and one from outside, an argument or a variable, are
read the same way. Give it to read one as another system writes it:

```haskell
import std.path

-- | a Windows path, read and written on any system
-- | >>> fromWindows
-- | "C:\\work\\notes.txt"
fromWindows : String
fromWindows = render (os := Windows) (parse (os := Windows) "C:/work" / "notes.txt")
```

`native` is not asked at run time: a C program is built for one system,
whose style it has; in JavaScript it is Node.js's, and `Posix` in a
page. `relative` and `isUnder` take `os` too, as Windows's names are the
same in any case. Two paths are `==` when their drives, roots and
segments are the same, case and all: `normal` them first to compare
`a/../b` with `b`. `show p` writes it as a program would: `"a/b"`.

### Open files

`withFile path { access = Read } body` opens a file, runs `body` with
it, and closes it. `access` is `Read`, `Write`, `Append` or
`ReadWrite`. The other options default by the access: `create`, `mode`,
`truncate`, and `exclusive`.

In the body:

| Function | What it does |
|---|---|
| `readChunk f n` | up to `n` bytes; none at the end |
| `readLine f` | the next line without its `\n` or `\r\n`, or `Nothing` |
| `writeBytes f b`, `writeString f s` | write them all |
| `seek f from n`, `tell f` | where it is read and written next |
| `fileStatus f` | its `Status` |
| `foldLines f acc step` | fold `step` over the lines left, one at a time |
| `copyOpen from to` | copy what is left of one file to another |

`withStdin`, `withStdout` and `withStderr` give a standard stream as an
open file, left open after. Read the input either this way or by
`readStdin`, not both.

A failure is `Either FileError a`: errno as `code`, its `name` (`ENOENT`,
`EACCES`), and its `message`. A file that cannot be opened is thrown.

## std.process

```haskell
import std.path
import std.process

-- | the current git commit of a repository
commitOf : {e : HFunctor} -> {{sys : process <: e}} -> Path -> Co e (Maybe String)
commitOf repo = do
  r <- run { command = "git", args = ["rev-parse", "HEAD"], dir = repo }
  pure (if succeeded r then Just (trim r.out) else Nothing)
```

The program's own process: `args`, `getEnv`, `programPath`, `platform`,
`processors`, `exit`, and `workingDir` and `setWorkingDir`. The working
directory is each [place](concurrency.md#stdplace)'s own. `programPath`
and `workingDir` give a `Path`, read in the system's style, and
`setWorkingDir` takes one; arguments and variables are strings.

### Running commands

`run` runs a command to its end. It takes an `Invocation`, whose fields
besides `command` have defaults:

| Field | Default | Meaning |
|---|---|---|
| `args` | `[]` | its arguments |
| `dir` | `current` | the directory it runs in, a `Path` |
| `env` | `[]` | variables set over the program's own |
| `input` | `NoInput` | `Given` a string, `Piped`, `Inherited`, or `NoInput` (the null device) |
| `output`, `errors` | `Collected` | `Collected`, `Piped` or `Inherited` |
| `timeout` | `0` | milliseconds before it is killed; 0 for none |
| `keep` | `0` | bytes kept of each stream; 0 for all |
| `group` | `False` | a process group of its own, killed whole |

`command` is a string: a name looked for on the `PATH`, or a path
written as the system writes one (`render p`).

It gives a `Ran`: `out` and `err`, and `ended`, which is `Exited`
(a status), `Signalled`, `TimedOut`, or `NotRun`. `succeeded r` is
whether it exited with 0.

`exec` runs a command in the program's place.

### Commands beside the program

```haskell
-- | what a command wrote, or why it did not run
said : {e : HFunctor}
     -> {{sys : process <: e}}
     -> {{ex : exception ProcessError <: e}}
     -> Invocation
     -> Co e String
said c = do
  try
    r <- withProcess c (\p -> wait p)
    pure r.out
  catch (err : ProcessError) ->
    pure ("not run: " ^ err.name)
```

`withProcess c body` starts a command and runs `body` beside it. When
the body ends, the command is asked to stop (SIGTERM), made to two
seconds later (SIGKILL), and waited for. A command that cannot start is
thrown, a `ProcessError`.

| Function | What it does |
|---|---|
| `wait p` | wait for it to end: its `Ran` |
| `tryWait p` | `Just` its `Ran` if it has ended |
| `kill p` | kill it, not waiting |
| `terminate p` | end it as the body's end would, and wait |
| `writeInput p b` | write bytes to its `Piped` input |
| `closeInput p` | close its input |
| `readOutput p portion ms` | read its `Piped` output |
| `readErrors p portion ms` | read its `Piped` error stream |

A portion is `Available` (whatever there is), `Exactly n`, or
`Through d` (up to and including `d`, such as `"\n"`). A read gives
`Got` the bytes, `Timeout`, or `Closed`.

A command's output is read only while the program waits on it, so one
that writes more than a pipe holds waits until then.

Commands left running are terminated when the program ends.
