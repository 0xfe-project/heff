---
title: Effects of your own
---

In this chapter a program walks the todo list with a cursor, an effect
of your own, and a handler keeps where the cursor is.

## A zipper

Create `src/hello/zipper.hf`:

```haskell
module hello.zipper

-- | a list, one of its elements in focus: those before it (nearest
-- | first), and those after it
type Zipper a = { before : List a, focus : a, after : List a }

nextZ : {a : Type} -> Zipper a -> Maybe (Zipper a)
nextZ z = case z.after of
  [] -> Nothing
  n :: rest -> Just { before = z.focus :: z.before, focus = n, after = rest }

previousZ : {a : Type} -> Zipper a -> Maybe (Zipper a)
previousZ z = case z.before of
  [] -> Nothing
  p :: rest -> Just { before = rest, focus = p, after = z.focus :: z.after }

-- | the list, the focus put back
toList : {a : Type} -> Zipper a -> List a
toList z = List.reverse z.before ++ (z.focus :: z.after)
```

A zipper keeps the list split where the cursor is: the element under the
cursor is `focus`, the ones before it are in `before`, nearest first, and
the ones after it are in `after`. For the list `a b c d e` with the
cursor on `c`:

```
before = [b, a]    focus = c    after = [d, e]
```

`nextZ` takes the first of `after` as the new focus and puts the old one
on `before`; `previousZ` does the opposite. When there is nothing left
to move to, the move gives `Nothing`. `toList` joins the three parts
back into one list.

## The effect

Create `src/hello/cursor.hf`:

```haskell
module hello.cursor

import co
import hello.todo
import hello.zipper

data CursorF (m : Type -> Type) x
  = Current (Todo -> x)
  | Change (Todo -> Todo) x
  | Next (Bool -> x)
  | Previous (Bool -> x)

cursor : HFunctor
cursor =
  { f = CursorF
  , hfmap = \m g p -> case p of
      Current k -> Current (\t -> g (k t))
      Change f x -> Change f (g x)
      Next k -> Next (\b -> g (k b))
      Previous k -> Previous (\b -> g (k b))
  , hmap = \m n nt p -> case p of
      Current k -> Current k
      Change f x -> Change f x
      Next k -> Next k
      Previous k -> Previous k
  }

current : {e : HFunctor} -> {{ev : cursor <: e}} -> Co e Todo
current = op (ev (Current (\t -> t))) val

change : {e : HFunctor} -> {{ev : cursor <: e}} -> (Todo -> Todo) -> Co e Unit
change f = op (ev (Change f MkUnit)) val

next : {e : HFunctor} -> {{ev : cursor <: e}} -> Co e Bool
next = op (ev (Next (\b -> b))) val

previous : {e : HFunctor} -> {{ev : cursor <: e}} -> Co e Bool
previous = op (ev (Previous (\b -> b))) val
```

`CursorF` lists the operations: what the todo at the cursor is, a change
to it, and the two moves, each saying whether it moved. Write `hfmap`
and `hmap` as they are here; every effect like this one has them in this
shape.

## A handler

Add to `src/hello/cursor.hf`:

```haskell
-- (a move: whether it was made, and where the cursor is after it)
moved : {x : Type}
      -> (Bool -> x)
      -> Maybe (Zipper Todo)
      -> Zipper Todo
      -> Pair x (Zipper Todo)
moved k (Just z2) z = MkPair (k True) z2
moved k Nothing z = MkPair (k False) z

-- | the cursor, on a zipper of todos
onZipper : Handler cursor
onZipper =
  { M = \x -> Zipper Todo -> Pair x (Zipper Todo)
  , ret = \x z -> MkPair x z
  , bind = \m k z -> case m z of
      MkPair x z2 -> k x z2
  , malg = \p z -> case p of
      Current k -> MkPair (k z.focus) z
      Change f x -> MkPair x { z | focus = f z.focus }
      Next k -> moved k (nextZ z) z
      Previous k -> moved k (previousZ z) z
  }

-- | `c` run on the zipper: the zipper after it
runCursor : {a : Type} -> Co cursor a -> Zipper Todo -> Zipper Todo
runCursor c z = snd (handle onZipper c z)
```

`onZipper` keeps the cursor on a zipper: a computation becomes a
function from the zipper to its result and the zipper after it.
`Current` reads the focus, `Change` changes it, and the moves are the
zipper's.

## Running it

In `app/hello/main.hf`, keep `printTodos` from chapter 3, and add
`import hello.cursor` and `import hello.zipper` to the imports:

```haskell
program : {e : HFunctor} -> {{cur : cursor <: e}} -> Co e Unit
program = do
  _ <- next
  change finish

start : Zipper Todo
start =
  { before = []
  , focus = todo "write the tutorial"
  , after = [todo "read the reference", todo "test the examples"]
  }

main : Co stdio Unit
main = printTodos (toList (runCursor program start))
```

`program` moves to the second todo and marks it done. It says nothing of
lists: only the handler knows the zipper.

```sh
$ heff run app/hello/main.hf
[ ] write the tutorial
[x] read the reference
[ ] test the examples
```
