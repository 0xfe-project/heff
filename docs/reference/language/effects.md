---
title: Effects
---

Effects are a library, [`co`](../../api/co.md): `import co` to use
them. The idea is from Yang and Wu's "Handling Higher-Order Effectful
Operations with Judgemental Monadic Laws" (POPL 2026).

## Computations

```haskell
import co
import std.filesystem
import std.io

look : {e : HFunctor}
     -> {{io : stdio <: e}}
     -> {{disk : filesystem <: e}}
     -> Co e Unit
look = do
  there <- exists "hello.txt"
  print (if there then "hello.txt is there" else "no hello.txt")

main : Co (stdio <+> filesystem) Unit
main = look (io := _) (disk := _)
```

- `Co e a` is a computation in the world `e` that gives back an `a`.
- A world is an `HFunctor`: one effect, such as `stdio`, or several
  joined by `<+>`, flat: `stdio <+> filesystem <+> process`.
- `h <: e` says the effect `h` is part of the world `e`. A computation
  asks for each effect it uses as an instance argument, and can use
  nothing else.
- Write computations for any world `e`, asking only for what they use.
  Only `main` names a world, in its type.

### Giving the evidence

`(x := _)` lets heff find the evidence: where the world is a sum by
`<+>`, known at the call (`main`'s, say), the one place of `h` in it,
put there by `InL`s and `InR`s.

A part of the world that is a world of its own, `e`, is reached through
the evidence in scope that says where `h` is in `e`: in `attempt
FileError (c (io := _) (fx := _))`, whose world is `exception FileError
<+> e`, `fx` is `\p -> InL p`, and `io`
is `\p -> InR (io p)`, through the `io` in scope. An effect in no place
of the world is E0355; in more than one, E0356 (give the evidence
there); an argument that is no `h <: e`, or a world not known where the
call is, E0357.

A value of `h <: e` is a function, and shows where `h` is in `e`:

| Evidence | Shows |
|---|---|
| `here h` | `h <: h` |
| `inl h k` | `h <: h <+> k` |
| `inr h k` | `k <: h <+> k` |
| `\p -> InL (InR p)` | the second of three, `k <: h <+> k <+> j` |

`<+>` groups to the left, and `<:` binds looser than it: `h <+> k <+> j`
is `(h <+> k) <+> j`, and `h <: h <+> k` is `h <: (h <+> k)`. Give it
by hand only where `_` cannot choose: an effect in two places of the
world.

## Defining an effect

```haskell
data TickF (m : Type -> Type) x = Tick x

tick : HFunctor
tick =
  { f = TickF
  , hfmap = \m g p -> case p of
      Tick x -> Tick (g x)
  , hmap = \m n nt p -> case p of
      Tick x -> Tick x
  }

tickOnce : {e : HFunctor} -> {{ev : tick <: e}} -> Co e Unit
tickOnce = op (ev (Tick MkUnit)) val
```

1. A data type lists the operations. Its last parameter, `x`, is what
   comes after the operation: an operation that gives back a value takes
   a function to `x`, `Get (s -> x)`; one that gives back nothing takes
   an `x`.
2. An `HFunctor` record gives the type, `f`, and its two maps. For an
   effect whose operations hold no computations, `hfmap` applies `g` to
   each `x`, and `hmap` returns each operation unchanged.
3. Each operation is `op (ev (Tag ...)) val`, with `MkUnit` for an `x`,
   and the identity, `\t -> t`, for a function to `x`.

### Operations that hold computations

An operation may hold computations of the program, not only values:
`recover c h` runs `c`, and `h` of what was thrown if `c` throws. Its
tag holds them at `m`, the data type's first parameter:

```haskell
data ExceptionF err (m : Type -> Type) x
  = Throw err
  | Catch (m x) (err -> m x)
```

`m` is the computation type, and the one who maps the operation chooses
it, so the maps have work to do here:

- `hfmap` applies `g` to what comes after, and that is now inside the
  held computations too:
  `Catch c h -> Catch (m.fmap g c) (\s -> m.fmap g (h s))`.
- `hmap` turns `m` into another computation type, `n`, by `nt`:
  `Catch c h -> Catch (nt c) (\s -> nt (h s))`.

By `hmap`, a handler gets an operation's computations as its own: when
it meets `Catch c h`, `c` and `h` are already of its `M`, handled, and
it says how they run. `attempt` runs `c`, and `h` on a throw
([Handlers](#handlers)).

## Handlers

```haskell
-- | counts the ticks
counted : Handler tick
counted =
  { M = \x -> Int -> Pair x Int
  , ret = \x n -> MkPair x n
  , bind = \m k n -> case m n of
      MkPair x n2 -> k x n2
  , malg = \p n -> case p of
      Tick x -> MkPair x (n + 1)
  }

thrice : {e : HFunctor} -> {{ev : tick <: e}} -> Co e Unit
thrice = do
  tickOnce
  tickOnce
  tickOnce

-- | >>> ticks
-- | 3
ticks : Int
ticks = snd (handle counted (thrice (ev := _)) 0)
```

A `Handler h` is a monad, `M`, `ret` and `bind`, and `malg`, which says
what each operation does in it. No monad laws are asked of it.
`handle hd c` runs `c` with it.

### Handling one effect of many

```haskell
-- | each tick printed
printed : {e : HFunctor}
        -> {a : Type}
        -> {{io : stdio <: e}}
        -> Co (tick <+> e) a
        -> Co e a
printed c = handle (through tick e said) c
  where
  said : {x : Type} -> TickF (Co e) x -> Co e x
  said (Tick x) = do
    print "tick"
    pure x
```

`through h e alg` handles `h` by turning each of its operations into a
computation of the rest of the world, `Co e`, and leaves the rest
untouched. The handler can ask for effects of `e` as instance arguments,
as `printed` asks for stdio.

The library has handlers ready, such as `state`'s
[`runState`](../../api/state.md#runState) and `exception`'s
[`attempt`](../../api/exception.md#attempt).

### Resuming more than once

```haskell
data ChooseF (m : Type -> Type) x = Choose (Bool -> x)

choice : HFunctor
choice =
  { f = ChooseF
  , hfmap = \m g p -> case p of
      Choose k -> Choose (\b -> g (k b))
  , hmap = \m n nt p -> case p of
      Choose k -> Choose k
  }

choose : {e : HFunctor} -> {{ev : choice <: e}} -> Co e Bool
choose = op (ev (Choose (\b -> b))) val

-- | how many of `n` coins come up heads
heads : {e : HFunctor} -> {{ev : choice <: e}} -> Int -> Co e Int
heads n =
  if n == 0 then pure 0
  else do
    b <- choose
    rest <- heads (n - 1)
    pure (if b then rest + 1 else rest)

-- | every way the coins fall: each choice resumed with True, then False
every : Handler choice
every =
  { M = \a -> (a -> List Int) -> List Int
  , ret = \x c -> c x
  , bind = \m k c -> m (\x -> k x c)
  , malg = \p c -> case p of
      Choose k -> c (k True) ++ c (k False)
  }

-- | >>> handle every (heads (ev := _) 2) (\n -> [n])
-- | [2, 1, 1, 0]
```

`malg` is not given the rest of the computation: an operation's `x` is
its answer (`k True` is `True`). The rest belongs to `M`. Make `M` a
continuation monad, `\a -> (a -> R) -> R`, and a value of it is given
the rest, `c`, which `malg` may call as often as it likes: here twice,
the answers appended. `handle every c` is then a function, given what
each answer comes to, `\n -> [n]`.

`R` may be a computation of the rest of the world, run later by a
handler outside. For a handler of `choice <+> state Int`, `M` is `\a ->
(a -> Co (state Int) (List Int)) -> Co (state Int) (List Int)`, and
`malg` chooses by `bind (c (k True)) (\xs -> bind (c (k False)) (\ys ->
val (xs ++ ys)))`, and does each operation of the state again:

```haskell
InR (Get k) -> bind (get (ev := _)) (\s -> c (k s))
InR (Put s x) -> bind (put (ev := _) s) (\u -> c x)
```

The state, handled outside, is not restored between the two ways.

## When work happens

Pure work runs where it is reached: an argument when the computation is
built (`pure (fib n)` computes `fib n` there), a step after `bind` as
the computation is handled. Only what comes after an operation waits for
the handler's monad.

`later (\u -> e)` delays `e` until the handler's monad runs it. This
matters for handlers that run part of a computation elsewhere, such as
`parallel`'s.
