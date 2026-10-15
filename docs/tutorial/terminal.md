---
title: A terminal app
---

Chapter 4's program moves the cursor by itself. In this chapter you move
it, with the keys of a terminal. Write it in `app/hello/main.hf`, in place
of chapter 4's program:

```haskell
module app.hello.main

import co
import std.io
import std.terminal as Terminal
import std.terminal.widget as Widget
import hello.cursor
import hello.todo
import hello.zipper
```

## Keys

```haskell
toggle : Todo -> Todo
toggle t = if pending t.status then finish t else { t | status = Open }

-- | what a key does at the cursor
act : {e : HFunctor} -> {{cur : cursor <: e}} -> Widget.Key -> Co e Unit
act ArrowDown = do
  _ <- next
  pure MkUnit
act ArrowUp = do
  _ <- previous
  pure MkUnit
act (Char ' ') = change toggle
act _ = pure MkUnit

step : Widget.Key -> Zipper Todo -> Widget.Step (Zipper Todo) (List Todo)
step (Char 'q') z = Done (toList z)
step k z = Continue (runCursor (act (cur := here cursor) k) z)
```

`act` says what each key does with the cursor: the arrows move it, and
space marks the todo under it done, or open again. A key is written by
its name alone, `ArrowDown` or `Char ' '`, without `Widget.`.

`step` runs a key's action on the zipper by `runCursor`. `q` is `Done`:
the app ends, with the list as it is then.

## The screen

```haskell
-- (a todo's row)
row : (String -> Widget.Span) -> Todo -> Widget.Row
row shown t = [shown (toString t)]

view : Widget.Size -> Zipper Todo -> Widget.Frame
view size z =
  { rows =
      Widget.fill
        (size.rows - 1)
        (List.map (row Widget.plain) (List.reverse z.before)
         ++ (row Widget.inverse z.focus :: List.map (row Widget.plain) z.after))
      ++ [Widget.status size.columns "↑↓ move  space done  q quit"]
  , cursor = Nothing
  }

app : Widget.App (Zipper Todo) (List Todo)
app =
  { start =
      { before = []
      , focus = todo "write the tutorial"
      , after = [todo "read the reference", todo "test the examples"]
      }
  , step = step
  , view = view
  }
```

`view` draws a zipper: one row for each todo, the one under the cursor
in inverse, and a line of help at the bottom. `app` puts the start, the
step and the view together.

## Running it

```haskell
program : {e : HFunctor}
        -> {{con : Terminal.console <: e}}
        -> {{io : stdio <: e}}
        -> Co e Unit
program = do
  r <- Terminal.interact app
  case r of
    Just ts -> print (joinWith "\n" (List.map toString ts))
    Nothing -> pure MkUnit

main : Co (Terminal.console <+> stdio) Unit
main = program (con := \p -> InL p) (io := \p -> InR p)
```

`Terminal.interact` runs the app on the terminal: it draws the view,
reads a key, steps, and draws again, until the app is `Done`. The
program then prints the todos.

```sh
$ heff run app/hello/main.hf
```
