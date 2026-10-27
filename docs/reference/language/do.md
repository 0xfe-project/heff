---
title: do
---

```haskell
import co
import std.io

echo : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
echo = do
  input <- readStdin
  let n = strLength input
  print ("read " ^ debug n ^ " bytes")
```

A `do` block is a computation written as statements:

- `x <- e` runs `e` and names its result `x`; `(a, b) <- e` takes
  the result apart;
- a statement `e` runs `e`;
- `let x = e` names `e` in the statements after it; `let (a, b) = e`
  takes it apart;
- the last statement is the block's result.

`x <- e` is `bind e (\x -> ...)`, using whatever `bind` and `pure` are
in scope. No `Monad` instance is needed. A pattern that binds must
match every value ([Patterns that bind](definitions.md#patterns-that-bind)).

## Statements

```haskell
-- | >>> sumEvens [1, 2, 4, 2000000, 6]
-- | 6
sumEvens : List Int -> Int
sumEvens xs = run do
  let mut total = 0
  for x in xs do
    if x % 2 /= 0 then continue
    if x > 1000000 then break
    total := total + x
  return total

-- | >>> firstLong ["a", "abcd", "xyz"]
-- | Just ("abcd")
firstLong : List String -> Maybe String
firstLong ws = run do
  for w in ws do
    if strLength w > 3 then return (Just w)
  Nothing
```

- `let mut x = e` declares a variable, and `x := e` sets it. Its type
  can be given: `let mut x : T = e`.
- `while c do ...` repeats while `c` holds.
- `for p in e do ...` runs once for each element of `e`, matched by `p`.
- `break` leaves the innermost loop; `continue` starts its next turn.
- `return e` ends the whole block with `e`.
- An `if` among the statements may have no `else`. The arms of an `if`
  or `case` among the statements are statements too.
- `let p = e else s` binds `p`'s names for the statements after it when
  `e` matches `p`, and runs `s` otherwise. `s`, a statement or the
  statements under it, must leave the block: it ends with `return`,
  `break`, `continue` or `abandon`, or an `if` or `case` whose every
  arm does. An action does not leave: a throw is
  `return (<- throw e)`, and for a value or to go on, write a `case`.
  Here `p` may fail to match; a `p` that cannot is let be, its `else`
  never run.
  ```haskell
  let Just x = lookup k m else return 0
  let (Just a, b) = pair else
    print "no a"
    continue
  ```
- `(<- c)` runs `c` where it is written, before the statement it is in:
  `print (<- readStdin)`.

A lambda cannot set the block's variables, and cannot use `(<- c)`,
`return`, `break` or `continue`: its body runs later. It sees each
variable's value at the time the lambda is made.

`for` goes through anything with an
[`Iterate`](../../api/prelude.md#Iterate) instance, such as a list, a
string (each `Rune`) or `range a b`.

## Pure code

`run do ...` writes pure code as statements, as the examples above do.
`run : Id a -> a` uses the identity monad.

Loops compile to tail calls, so they do not grow the stack, in pure code
or in `Co`.

## try

```haskell
import exception

parsed : {e : HFunctor} -> {{ex : exception String <: e}} -> String -> Co e Int
parsed s = case parseInt s of
  Just n -> pure n
  Nothing -> throw ("not a number: " ^ s)

total : {e : HFunctor}
      -> {{ex : exception String <: e}}
      -> {{io : stdio <: e}}
      -> List String
      -> Co e Int
total words = do
  let mut sum = 0
  for w in words do
    try
      n <- parsed w
      sum := sum + n
    catch (why : String) ->
      print why
  return sum
```

`try` runs its statements with the `exception` effect, and needs
`import exception`.

- A throw inside is caught by the `catch` arm under it, which runs
  instead.
- The arm's type, `catch (why : String)`, says which throws it catches.
  It can also be given: `try (err := String)`.
- The statements and the `catch` arm belong to the block: they can set
  its variables, `break`, `continue` and `return`.
- A throw abandons what it is thrown in, so the `catch` arm sees the
  variables as they were when the `try` began.

Releasing a resource is not `try`'s job: a resource is held by a `with`
of its own effect, such as `with f := File.open path do` of `std.file`.

## with

```haskell
import exception
import std.process

said : {e : HFunctor}
     -> {{sys : process <: e}}
     -> {{ex : exception ProcessError <: e}}
     -> Invocation
     -> Co e String
said c = do
  let mut out = ""
  with withProcess c as p do
    r <- wait p
    out := r.out
  return out
```

`with e as x do` runs the statements under it as the body of the
[`with`](../../api/resource.md) `e`, such as `withProcess c`, with `x`
what it holds. It is `e (\x -> do ...)`, written as a
statement. Where `e` hands its body a tuple, `with e as (a, b) do`
takes it apart: `e (\p -> case p of (a, b) -> do ...)`.

- `x` is in scope in the statements under it alone; a `With s r`, as a
  `with` made by `scoped` holds, cannot leave them (E0311).
- The statements belong to the block, as a `try`'s do: they can set
  its variables, `break`, `continue` and `return`. The thing is let go
  of on the way out.
- As the block's last statement, its value is the block's; otherwise
  it is dropped.
- A `with` throws what it could not acquire or release: a `try` around
  it catches that.

### with, of bindings

```haskell
import boc as BOC

paid : {e w : HFunctor}
     -> {{bc : BOC.boc w <: e}}
     -> BOC.Cown w Int
     -> BOC.Cown w Int
     -> Int
     -> Co e Unit
paid from to n = do
  with a := from
       b := to do
    a := a - n
    b := b + n
```

`with x := M do` binds names, each to a value: one binding, or more, each
on a line of its own under the first, `do` at the end of the last. The
whole group is handed to the handler that owns what they are bound to,
which decides what the names are in the body, when and where the body
runs, and what happens at its end: here one behaviour on both cowns, `a`
and `b` what they hold, written back when it ends; with
`std.file`'s `open path`, a file opened, the body run with it,
and closed; with boc's `r := BOC.await act` last, `act` run outside
every behaviour, and the body a behaviour once it has come to `r`; with
cml's `_ := CML.threads`, the body cml's main thread, in a world with
cml, it and the threads it spawns scheduled on domains, and the `with`
done once they are, no thread of it left running after it.

- The names are the body's variables: they can be set, `a := a - n`.
  The handler is told which ones the body sets, and is given what they
  are at its end beside the body's value.
- `_ := M` binds no name.
- The body is a block of its own: it sets no variable of the block
  around, nor leaves it by `break`, `continue` or `return`; a `return`
  in it ends the body.
- The world the body runs in is the binder's to say. A file's body is
  in the block's world. A cown's body, or an await's, is a behaviour,
  run later on another domain: it is in the scheduler's world,
  `BOC.boc w <+> w`, and none of the handlers around the `with` (a
  `try`, another `with`'s resource) is around it. What it needs, it
  opens itself or is given as a value. cml's threads are the same: a
  `CML.spawn` takes a computation of `CML.cml w <+> w`.
- Its value is what the handler says: a file's `with` comes to what its
  body came to, a cown's to `()` (the behaviour runs later), cml's
  threads' to `Just` main's answer (`Nothing` where every thread
  waits). `x <- with ... do` binds it. A thread or behaviour that
  failed does not stop the others: cml's `with` fails at its end as
  the first thread that failed did.
- A handler is a pair of instances, co's `Binder` (of what the names are
  bound to: what they are in the body, the world it runs in, and what
  the `with` comes to) and `Holder` (how it runs in the world), and the
  `with` is co's `holding`.

The body runs in the world of the block around, `e`, unless the
`Binder` says another, by its field `World`: a function from `e` to the
body's world. What the names are, its field `Contents`, is given `e`
too, and the body's region: a name can be of a type that says the
world around, as std.domain's scope is. A handler that gives its body
a count of its own:

```haskell
import state

data Counter = MkCounter Int

counter : Int -> Counter
counter k = MkCounter k

instance instanceBinderCounter : Binder Counter
instanceBinderCounter =
  { Contents = \w s -> Int
  , World = \w -> state Int <+> w
  , Result = \r -> (r, Int)
  }

instance instanceHolderCounter : {e : HFunctor}
                               -> Holder
                                 e
                                 Counter
                                 (\s -> Int)
                                 (\w -> state Int <+> w)
                                 (\r -> (r, Int))
instanceHolderCounter =
  { held = \c written body -> case c of
      MkCounter k ->
        bind
          (runState k (body (s := Unit) k))
          (\p -> pure (fst (fst p), snd p))
  }

tens : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
tens = do
  r <- with n := counter 5 do
    put (n + 1)
    m <- get
    print ("counted " ^ debug m)
    put (m * 10)
    pure "done"
  print (fst r ^ " at " ^ debug (snd r))
```

The body of `with n := counter 5 do` runs in `state Int <+> e`: `n` is
the count it starts from, and `get` and `put` are its own count's. The
holder's `held` is given the body as a computation of that world, and
handles what it has beyond `e`, here by `runState`; the `with` comes to
`("done", 60)`. The body's evidence is found in its world
([giving the evidence](effects.md#giving-the-evidence)): an effect the
world adds is the body's own, even where `e` has it too, and `print`
is `e`'s `stdio`, through the `io` in scope, with nothing written. A
world `\w -> stdio <+> w` makes a `with` whose holder sends what its
body prints elsewhere, to a file or a buffer.

`with` and `as` are [keywords](definitions.md#keywords): neither is a
name.
