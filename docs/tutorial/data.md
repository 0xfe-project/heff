---
title: Values, functions and data
---

Chapter 1's program greets the world. From here on, you build something
more useful: a todo list. Create `src/hello/todo.hf`, and add each
definition in this chapter to it, in order, after its name:

```haskell
module hello.todo
```

## Data types

```haskell
data Status = Open | Done
```

A data type lists its constructors. A `Status` is either `Open` or
`Done`.

## Records

```haskell
type Todo = { title : String, status : Status }

todo : String -> Todo
todo title = { title = title, status = Open }

finish : Todo -> Todo
finish t = { t | status = Done }
```

A record type names its fields. `t.title` reads a field, and
`{ t | status = Done }` is `t` with one field replaced.

## Pattern matching

```haskell
pending : Status -> Bool
pending Open = True
pending Done = False

isOpen : Todo -> Bool
isOpen t = pending t.status
```

A function can have one equation per pattern.

## As text

```haskell
instance toStringStatus : ToString Status
toStringStatus = { toString = mark }
  where
  mark : Status -> String
  mark Open = "[ ]"
  mark Done = "[x]"

instance toStringTodo : ToString Todo
toStringTodo = { toString = \t -> toString t.status ^ " " ^ t.title }
```

`toStringStatus` comes before `toStringTodo` because a definition only
sees the ones above it.

## Lists

```haskell
title : Todo -> String
title t = t.title

openTitles : List Todo -> List String
openTitles ts = List.map title (List.filter isOpen ts)
```

`List.filter` keeps the todos that are open, and `List.map` takes the
title of each.
