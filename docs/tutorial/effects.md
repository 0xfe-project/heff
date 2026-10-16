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

Add `import std.filesystem` and `import std.path` to the imports, then:

```haskell
loadTodos : {e : HFunctor}
          -> {{disk : filesystem <: e}}
          -> Path
          -> Co e (List Todo)
loadTodos path = do
  text <- readFile path
  case text of
    Just s -> pure (List.map todo (lines s))
    Nothing -> pure []
```

`loadTodos` reads the file at `path`, a `Path`, and makes each of its
lines an open todo. `text <- readFile path` runs `readFile` and names its result
`text`: `Just` the file's text, or `Nothing` if it cannot be read, and
then there are no todos.

As you can see, `printTodos` needs stdio and `loadTodos` needs
`filesystem`: a computation must claim the parts of the world it
interacts with.

## Put it all together

```haskell
program : {e : HFunctor}
        -> {{io : stdio <: e}}
        -> {{disk : filesystem <: e}}
        -> Co e Unit
program = do
  ts <- loadTodos "todos.txt"
  printTodos ts

main : Co (stdio <+> filesystem) Unit
main =
  program
    (io := inl stdio filesystem)
    (disk := inr stdio filesystem)
```

`loadTodos "todos.txt"` gives it a string where a `Path` is expected:
`std.path` takes a string as a path, read as the system the program runs
on writes one.

`program` needs both. `main`'s world, `stdio <+> filesystem`, has both,
and `main` shows where each is: `inl stdio filesystem` shows stdio is on
the left of it, and `inr stdio filesystem` that the files are on the
right.

```sh
$ printf 'write the tutorial\nread the reference' > todos.txt
$ heff run app/hello/main.hf
[ ] write the tutorial
[ ] read the reference
```
