---
title: Effects
---

So far the todos only exist in the code. In this chapter the program
prints them, then reads them from a file. Write it in
`app/hello/main.hf`, in place of the greeting.

## Printing the todos

```haskell
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

As in chapter 1, `{{io : stdio <: e}}` says: to run this, you need
stdio. `do` runs its lines one after another. `main` gives `printTodos`
the stdio it asks for, `(io := here stdio)`.

## Reading a file

Add `import std.filesystem` to the imports, then:

```haskell
loadTodos : {e : HFunctor}
          -> {{disk : filesystem <: e}}
          -> String
          -> Co e (List Todo)
loadTodos path = do
  text <- readFile path
  case text of
    Just s -> pure (List.map todo (lines s))
    Nothing -> pure []
```

`loadTodos` reads the file at `path` and makes each of its lines an open
todo. `text <- readFile path` runs `readFile` and names its result
`text`: `Just` the file's text, or `Nothing` if it cannot be read, and
then there are no todos.

Where `printTodos` needs stdio, `loadTodos` needs `filesystem`.

## Both

```haskell
program : {e : HFunctor}
        -> {{io : stdio <: e}}
        -> {{disk : filesystem <: e}}
        -> Co e Unit
program = do
  ts <- loadTodos "todos.txt"
  printTodos ts

world : HFunctor
world = stdio <+> filesystem

main : Co world Unit
main = program (io := inl stdio filesystem) (disk := inr stdio filesystem)
```

`program` needs both. Its world, `stdio <+> filesystem`, has both, and
`main` says where each is: stdio on the left, the files on the right.

```sh
$ printf 'write the tutorial\nread the reference' > todos.txt
$ heff run app/hello/main.hf
[ ] write the tutorial
[ ] read the reference
```
