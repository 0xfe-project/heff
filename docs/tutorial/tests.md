---
title: Tests
---

Chapter 1's doc test checks one value. In this chapter you test the
cursor, the zipper and the screen, in `test/todos.hf`:

```haskell
module test.todos

import app.hello.main as Main
import check
import co
import generator as Generator
import hello.cursor
import hello.todo
import std.terminal.widget as Widget
import zipper.list

-- (three todos, the cursor on the first)
three : Zipper Todo
three =
  { before = []
  , focus = todo "write"
  , after = [todo "read", todo "test"]
  }
```

## Unit tests

```haskell
-- (to the second todo, and marked done)
markSecond : {e : HFunctor} -> {{cur : cursor <: e}} -> Co e Unit
markSecond = do
  _ <- next
  change finish

-- (the todos, as they are shown)
shown : Zipper Todo -> List String
shown z = List.map show (toList z)

cursorTests : Test
cursorTests =
  group
    "cursor"
    [ unit
      "the second marked done"
      (assertEqual
         (shown (runCursor (markSecond (cur := _)) three))
         ["[ ] write", "[x] read", "[ ] test"])
    , unit
      "no previous at the start"
      (assertEqual
         (shown (runCursor (previous (ev := _)) three))
         ["[ ] write", "[ ] read", "[ ] test"])
    ]
```

A test is a definition of type `Test`. `unit` names one check, and
`group` puts tests under one name. The cursor's programs run on a zipper
by `runCursor`, with no terminal: the handler is the same one the app
uses.

## Properties

```haskell
moves : Test
moves =
  property "a move keeps the list" (do
  x <- forAll (Generator.int 0 99)
  xs <- forAll (Generator.list 0 10 (Generator.int 0 99))
  z <- pure { before = [], focus = x, after = xs }
  case nextZ z of
    Just z2 -> assertEqual (toList z2) (toList z)
    Nothing -> label "at the end")
```

A property checks something of many values. `forAll` draws one from a
generator, here a number and a list of up to ten, and `heff test` runs
the property a hundred times. `label` counts the runs it is in, so you
can see how many ended at the end of the list.

## The screen

```haskell
screen : Test
screen =
  unit
    "a todo marked done, on the screen"
    (assertEqual
       (Widget.shownAfter
          Main.app
          { columns = 30, rows = 5 }
          [ArrowDown, Char ' '])
       [ "[ ] write the tutorial"
       , "[x] read the reference"
       , "[ ] test the examples"
       , ""
       , "↑↓ move  space done  q quit   "
       ])
```

`Widget.shownAfter` runs the app on the keys given, and gives the rows
it shows after them.

```sh
$ heff test
...
  ok    cursor / the second marked done
  ok    cursor / no previous at the start
  ok    a move keeps the list  (100 runs, 33% at the end)
  ok    a todo marked done, on the screen
ok    test/todos.hf  0/0, tests 4/4
```
