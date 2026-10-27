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
- A `with` throws what it could not acquire: a `try` around it catches
  that. A failure to release is the runtime's (below).

### What a with holds, and custodians

What a `with` like `withProcess` holds lives as long as the program can
reach it. When its body ends, the `with` lets go of its hold:

- where it held the thing alone, the thing is ended there;
- where the body gave it to another, a thread spawned in the body
  given the `With s r` by value, it is ended once the last holder
  lets go of it. The `with` does not wait.

A thing that fails to end, now or later, is a failure of the runtime:
it is printed and the program ends, as with an uncaught failure. It is
not thrown to the `with`'s caller.

So a thread can use a command or a connection after the `with` that
made it has ended. On the C backend "lets go" is the runtime's count of
references, so a thing is ended as soon as its last holder is done
with it. JavaScript keeps no such count; a `with` there lets go of its
thing to the collector. A GPU device or buffer is the exception: like a
file, it is released as its block is left.

A `with` that releases by a computation (resource's `held`: a port of
a standard stream, the page's document) has the other policy: it
releases as its block is left, whoever holds the thing still. Its
release is kept by the program's top level while the block runs, so
such a `with` asks for `{{cu : custodian <: e}}`, and a `main` that uses
one has `custodian` in its world. Where a throw heard outside the block
leaves it, or a handler drops the continuation, the release still runs:

- on the C backend, at the next operation of the domain that started
  the `with`;
- in JavaScript, when the collector reaches what held it.

A release that fails then is a failure of the runtime.

`withStdin`, `withStdout` and `withStderr` give the body a port of its
own, a copy of the stream's descriptor, and close it as the block is
left. std.io's printing and the stream itself go on as before. A
`withDocument` lets go of the page's document as its block is left,
but that closes nothing. The document is the page's, and the program
still has it wherever it passed it on: in a handler set on an element
and run later, or to another effect by value.

For more, there are custodians, as in Racket (the `custodian` module).
A custodian is a runtime object that the program holds as a value:

- `Custodian.manage c stop` gives it something to run at its shutdown;
- `Custodian.under c` makes a custodian that `c` holds and shuts down
  with it;
- `Custodian.shutdown c` runs everything it was given, once and in
  order, then does the same for the custodians under it. It comes to
  the reasons of those that failed.

A custodian manages resources, not threads. A thread that took a
thing from a custodian that was then shut down finds the thing ended at
its next operation on it (a socket's `EBADF`), and handles that itself.

What a custodian is given is higher-order. It is a computation of the
world it was given in, with the handlers between there and the
program's top level already applied, so it runs after their scope may
have ended. An `attempt` there turns a throw into `abandon`, and the
shutdown says that in its list.

A custodian the program no longer reaches is let go of by the runtime,
and what it was given is dropped, not run. On the C backend that is at
once. In a page it is whenever JavaScript's collector comes to it.

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
done once they are, no thread of it left running after it; with
std.domain's `s := Domain.scope`, the body run here and every domain
spawned in `s` waited for at its end
([a scope of domains](#a-scope-of-domains)).

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

### A scope of domains

```haskell
import std.domain as Domain

-- | `n` rounds of `step`, fewer where the scope is cancelled
rounds : {e w : HFunctor}
       -> {s : Type}
       -> {{dm : Domain.domain <: e}}
       -> Domain.Scope s w
       -> Int
       -> Co e Unit
       -> Co e Unit
rounds sc 0 step = pure ()
rounds sc n step = do
  stop <- Domain.cancelled sc
  if stop then pure ()
  else do
    step
    rounds sc (n - 1) step

both : {e : HFunctor}
     -> {{dm : Domain.domain <: e}}
     -> Co e Unit
     -> Co e Unit
     -> Co e Unit
both one other = do
  with s := Domain.scope do
    _ <- Domain.spawn s (rounds s 1000 one)
    _ <- Domain.spawn s (rounds s 1000 other)
    pure ()
```

`with s := Domain.scope do` is structured concurrency, as Rust's
`std::thread::scope`: its body runs here, and `Domain.spawn s c` spawns
`c` on a domain of its own, one of the scope's. At the body's end the
`with` waits for every domain spawned in `s`; the body is free to do
other work before. `Domain.join` gives a domain's answer.

- A domain is spawned in a scope, and only there: there is no `spawn`
  of any world. The body runs in the block's world with the scope's own
  effect, `Domain.scoped e <+> e`, and `Domain.spawn s c` is that
  effect's. `c` is a computation of `e`, the world around the `with`,
  which has no `scoped`: a domain cannot spawn in its own scope, so
  none is spawned after the body ends, and none outlives the `with`. A
  domain that wants domains of its own opens a scope of its own.
- A domain of the scope that fails fails the scope: once all of them
  have ended, the `with` fails as the first to fail did (a
  `Domain.tried` around it catches that). Where the body fails, its
  domains are waited for too, and the `with` fails as the body did.
- Cancelling is cooperative. The scope carries a cancel mark, set by
  `Domain.cancel s`, or when one of its domains fails, or its body does.
  A domain looks at it where it chooses, `Domain.cancelled s`, and
  winds up as it chooses: here `rounds` stops between steps. The
  `with` still waits for it, and a scope that is only cancelled comes
  to its body's value.
- No domain is stopped from outside. Stopped at an arbitrary operation,
  it would leave a `Domain.lock` it holds locked, and the release of a
  resource it holds (a `with`'s, a `held`'s) would not run, its
  continuation dropped: that is why cancelling a thread of the system
  from outside is unsafe. A domain waiting in a call of the system, or
  in a loop with no operation, stops only where it looks at the mark;
  a pure loop that never ends is std.fuel's to stop
  ([fuel](../commands.md#fuel)).
- `s` cannot leave the body: its type, `Domain.Scope s e`, is of the
  `with`'s region. A body left by a throw handled outside the `with`
  (an `exception`'s) does not wait: its domains run on.

`with` and `as` are [keywords](definitions.md#keywords): neither is a
name.
