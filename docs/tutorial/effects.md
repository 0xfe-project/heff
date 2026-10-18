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
throws why, a `FileError`: so `loadTodos` needs `exception FileError` as
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
  r <-
    attempt
      FileError
      (stdio <+> filesystem)
      (program (io := _) (disk := _) (ex := _))
  case r of
    Left err -> print (ev := _) (showFileError err)
    Right _ -> pure MkUnit
```

`loadTodos "todos.txt"` gives it a string where a `Path` is expected:
`std.path` takes a string as a path, read as the system the program runs
on writes one.

`program` needs all three. `main`'s world, `stdio <+> filesystem`, has
stdio and the files; `attempt FileError` runs `program` in that world
with `exception FileError` added on its left, and gives back `Left` what
was thrown, or `Right` what `program` gave (see
[handlers](../reference/language/effects.md#handlers) and
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
ENOENT: No such file or directory
```
