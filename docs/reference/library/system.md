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

-- | the words of the standard input, read a line at a time
wordsIn : {e : HFunctor} -> {{disk : filesystem <: e}} -> Co e (Either FileError Int)
wordsIn = withStdin (\i -> foldLines i 0 (\n line -> pure (n + List.length (words line))))

-- | a file's text, with a line added
appended : {e : HFunctor} -> {{disk : filesystem <: e}} -> String -> String -> Co e Bool
appended path line = do
  text <- readFile path
  writeFile path (fromMaybe "" text ^ line ^ "\n")
```

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

`std.path` puts paths together and takes them apart, and is pure.

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
import std.process

-- | the current git commit of a repository
commitOf : {e : HFunctor} -> {{sys : process <: e}} -> String -> Co e (Maybe String)
commitOf repo = do
  r <- run { command = "git", args = ["rev-parse", "HEAD"], dir = repo }
  pure (if succeeded r then Just (trim r.out) else Nothing)
```

The program's own process: `args`, `getEnv`, `programPath`, `platform`,
`processors`, `exit`, and `workingDir` and `setWorkingDir`. The working
directory is each [place](concurrency.md#stdplace)'s own.

### Running commands

`run` runs a command to its end. It takes an `Invocation`, whose fields
besides `command` have defaults:

| Field | Default | Meaning |
|---|---|---|
| `args` | `[]` | its arguments |
| `dir` | `""` | the directory it runs in |
| `env` | `[]` | variables set over the program's own |
| `input` | `NoInput` | `Given` a string, `Piped`, `Inherited`, or `NoInput` (the null device) |
| `output`, `errors` | `Collected` | `Collected`, `Piped` or `Inherited` |
| `timeout` | `0` | milliseconds before it is killed; 0 for none |
| `keep` | `0` | bytes kept of each stream; 0 for all |
| `group` | `False` | a process group of its own, killed whole |

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
