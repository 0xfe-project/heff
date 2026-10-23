---
title: Interacting with the outside world
---

So far the todos only exist in the code. In this chapter the program
prints them, then reads them from a file. Write it in
`app/hello/main.hf`, in place of the greeting.

## Printing the todos

```haskell
module app.hello.main

import co
import hello.todo
import std.io

printTodos : {e : HFunctor} -> {{io : stdio <: e}} -> List Todo -> Co e Unit
printTodos [] = pure MkUnit
printTodos (t :: ts) = do
  print (toString t)
  printTodos ts

main : Co stdio Unit
main =
  printTodos
    (io := here stdio)
    [ todo "write"
    , finish (todo "read")
    ]
```

```sh
$ heff run app/hello/main.hf
[ ] write
[x] read
```

Read `printTodos`'s type from the left:

- `{e : HFunctor}`: an implicit argument, the world the computation runs
  in; it can be any world.
- `{{io : stdio <: e}}`: an instance argument, `io`, showing that stdio
  is part of the world `e`.
- `Co e Unit`: a computation in the world `e`, which gives back `Unit`
  when it is done.

`do` runs its lines one after another. `main` runs in the world `stdio`
alone, and gives `printTodos` what it asks for: `here stdio` shows that
stdio is part of `stdio`.

## Reading a file

Add `import exception`, `import std.filesystem` and `import std.path` to
the imports, then:

```haskell
loadTodos : {e : HFunctor}
          -> {{disk : filesystem <: e}}
          -> {{ex : exception FileError <: e}}
          -> Path
          -> Co e (List Todo)
loadTodos path = do
  text <- readFile path
  pure (List.map todo (lines text))
```

`loadTodos` reads the file at `path`, a `Path`, and makes each of its
lines an open todo. `text <- readFile path` runs `readFile` and names its
result `text`, the file's text. Where the file cannot be read, `readFile`
throws why, a `FileError` -- the system's errno, what it says of it, and
the path it says it of: so `loadTodos` needs `exception FileError` as
well as `filesystem`.

As you can see, `printTodos` needs stdio and `loadTodos` needs
`filesystem`: a computation must claim the parts of the world it
interacts with.

## Put it all together

```haskell
program : {e : HFunctor}
        -> {{io : stdio <: e}}
        -> {{disk : filesystem <: e}}
        -> {{ex : exception FileError <: e}}
        -> Co e Unit
program = do
  ts <- loadTodos "todos.txt"
  printTodos ts

main : Co (stdio <+> filesystem) Unit
main = do
  r <- attempt FileError (program (io := _) (disk := _) (ex := _))
  case r of
    Left err -> print (ev := _) (showFileError err)
    Right _ -> pure MkUnit
```

`loadTodos "todos.txt"` gives it a string where a `Path` is expected:
`std.path` takes a string as a path, read as the system the program runs
on writes one.

`program` needs all three. `main`'s world, `stdio <+> filesystem`, has
stdio and the files; `attempt FileError` runs `program` in that world,
as `main`'s type says it, with `exception FileError` added on its left,
and gives back `Left` what was thrown, or `Right` what `program` gave
(see [handlers](../reference/language/effects.md#handlers) and
[`attempt`](../api/exception.md#attempt)). Where nothing heard a throw,
`main` prints it.

`(io := _)` asks heff for the evidence: it finds where stdio is in the
world, here in `attempt`'s, and shows it so (see
[giving the evidence](../reference/language/effects.md#giving-the-evidence)).

```sh
$ printf 'write the tutorial\nread the reference' > todos.txt
$ heff run app/hello/main.hf
[ ] write the tutorial
[ ] read the reference
$ rm todos.txt
$ heff run app/hello/main.hf
todos.txt: ENOENT: No such file or directory
```

## Output, bytes and time

What `print` and `write` send to a file or a pipe is kept and written a
buffer at a time, and all of it by the time the program ends; to a
terminal, a line at a time. A program that runs for long, such as a
server writing a log, calls [`flush`](../api/std/io.md#flush) to have
what it printed written now. The standard error keeps nothing.

`readFile` gives a file's text; [`readBytes`](../api/std/filesystem.md#readBytes)
gives its `Bytes`, as they are. A string is written as its bytes are, so
`write (Bytes.asString b)` writes the bytes `b` to the output unchanged,
and so does std.filesystem's `withStdout` with `writeBytes`.
`Bytes.toString` is for text: it reads the bytes as UTF-8, and what is
not UTF-8 becomes U+FFFD, which is not what a PNG wants.

std.time has [`sleep`](../api/std/time.md#sleep) and two clocks, both in
nanoseconds: `now`, the time of day since the epoch, and `monotonic`,
which only goes forward and measures how long something took.
