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
  print ("read " ^ show n ^ " bytes")
```

A `do` block is a computation written as statements:

- `x <- e` runs `e` and names its result `x`;
- a statement `e` runs `e`;
- `let x = e` names `e` in the statements after it;
- the last statement is the block's result.

`x <- e` is `bind e (\x -> ...)`, using whatever `bind` and `pure` are
in scope. No `Monad` instance is needed.

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
- `if let p = e then a else b` runs `a` when `e` matches `p`, with its
  names bound, and `b` otherwise.
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
of its own effect, such as `std.filesystem`'s `withFile`.

## with

```haskell
import exception
import std.filesystem

lineCount : {e : HFunctor}
          -> {{disk : filesystem <: e}}
          -> {{ex : exception FileError <: e}}
          -> String
          -> Co e Int
lineCount path = do
  let mut n = 0
  with withFile path { access = Read } as f do
    let mut more : Bool = True
    while more do
      l <- readLine f
      case l of
        Right (Just _) -> n := n + 1
        _ -> more := False
  return n
```

`with e as x do` runs the statements under it as the body of the
[`with`](../library/resource.md) `e`, such as `withFile path o`, with `x`
the `With s r` it holds. It is `e (\x -> do ...)`, written as a
statement.

- `x` is in scope in the statements under it alone, and cannot leave
  them (E0311).
- The statements belong to the block, as a `try`'s do: they can set
  its variables, `break`, `continue` and `return`. The thing is let go
  of on the way out.
- As the block's last statement, its value is the block's; otherwise
  it is dropped.
- A `with` throws what it could not acquire or release: a `try` around
  it catches that.

`with` and `as` are [keywords](definitions.md#keywords): neither is a
name.
