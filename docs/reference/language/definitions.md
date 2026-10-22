---
title: Definitions
---

```haskell
-- | the first element, or `d` for an empty list
-- | >>> firstOr 0 [5, 6]
-- | 5
firstOr : Int -> List Int -> Int
firstOr d [] = d
firstOr d (x :: _) = x
```

## Signatures and equations

A definition is a signature, `name : Type`, then its equations right
under it. Each equation matches its arguments against patterns, tried
from the top; the first that matches is used.

A pattern is one of:

- a name, which matches anything and binds it;
- `_`, which matches anything;
- a tag applied to patterns, `Just x`, `MkPair a b`;
- a tuple of patterns, `(a, b)`, `(a, b, c)`, or the unit, `()`
  ([Unit and pairs](data.md#unit-and-pairs));
- a list: `[]` or `x :: xs` as usual, `[x, y]` for a list of that length;
- a number, string or rune literal;
- a binary, `b[...]` ([Binaries](binaries.md)).

Patterns nest: `Node x (c :: cs)`.

### More names, one type

A signature may name more than one definition: `x, y, z : T` is
`x : T`, `y : T` and `z : T`. Each name still needs its own equations,
after the signature, each name's together, in any order:

```haskell
-- | the smaller and the larger of two
smaller, larger : Int -> Int -> Int
smaller a b = if a < b then a else b
larger a b = if a < b then b else a
```

A name with no equations there has a type but no definition, said at
that name; a name written twice in one signature is an error. The doc
comment above the signature is each name's, as `heff doc` and a hover
show. `heff fmt` keeps the signatures as they are written: it neither
joins separate ones nor splits one of more names.

The same works in a `where` and a `mutual` block, for a theory's
fields (`first, second : a -> a`) and for a record type's
(`{ x, y : Int }`, each with the type, and the default if one is
given, of the field after it).

## Keywords

These words are keywords, and nothing is named by one:
`data`, `type`, `theory`, `primitive`, `instance`, `mutual`, `import`,
`as`, `case`, `of`, `if`, `then`, `else`, `do`, `where`, `let`, `in`,
`mut`, `while`, `for`, `break`, `continue`, `return`, `try`, `catch`,
`with`, `test`, `abandon`, `infixl`, `infixr`, `infix`; and the
built-in types' names, `Type`, `Int`, `String`, `Rune`, `Array` and
the sized numbers (`Int8` to `UInt64`, `Float32`, `Float64`).

## Expressions

```haskell
-- | >>> sign (-2)
-- | "negative"
sign : Int -> String
sign n = case compare n 0 of
  LT -> "negative"
  EQ -> "zero"
  GT -> "positive"

-- | >>> area 3
-- | 9
area : Int -> Int
area n = let side = n in side * side

-- | >>> quadruple 3
-- | 12
quadruple : Int -> Int
quadruple n = double (double n)
  where
  double : Int -> Int
  double k = k * 2

-- | >>> List.map (\x -> x + 1) [1, 2]
-- | [2, 3]
increments : List Int
increments = List.map (\x -> x + 1) [1, 2]
```

- `case e of` matches `e` against patterns, one arm a line.
- `if c then a else b` is `a` where `c` is `True`.
- `let x = e in body` names `e` in `body`. A `let` does not generalise:
  what it binds has one type for every use. `let p = e in body` takes
  `e` apart by the pattern `p`: `let (a, b) = pair in a + b`.
- `where` under an equation holds definitions only that equation sees.
  They can use the equation's arguments.
- `\x -> e` is a function; `\(x : T) -> e` gives its argument a type;
  `\(a, b) -> e` takes its argument apart.
- A lambda whose type is not known where it is written waits until the
  rest of the definition says it. Where nothing ever does, it is taken
  as an explicit function, `a -> b`:
  `List.foldl (\acc f -> f acc) 1 (List.map (\j -> \v -> v + j) [0, 1])`
  checks. If that does not fit either (it was to take an implicit
  argument, or its result's type depends on its argument), the error is
  the one without the guess: give the lambda's argument a type.

`&&` and `||` evaluate their right side only when needed.

### Patterns that bind

A pattern that binds without a `case`, in `let p = e`, `\p -> e`, and
a `do` block's `p <- e` and `with e as p`, must match every value of
its type: names, `_`, tuples and units of them, and a tag that is its
type's only one, such as `MkPair a b`. A pattern that can fail, `Just
x` or a literal, is non-exhaustive (E0317), as a `case` that leaves a
value out is. Write a `case`, or in a `do` block `let p = e else s`
([do](do.md#statements)), which says what happens when it does not
match.

A tag cannot fail where the matches around it have ruled out the
others. In an arm of a `case` on a local (or of an equation, or in the
statements after a `let p = e else s`), that local, a name bound to it,
a field of it and an argument of the tag matched can only have the
tags the arm leaves them, and a `case` on one needs no case for a tag
ruled out. A name bound again, a `let mut` variable set, or a lambda's
parameter is another value, which no match has looked at.

```haskell
first : List Int -> Int
first Nil = 0
first xs = let Cons h t = xs in h
```

### Arms never taken

An arm no value can take is warned of. It is a warning, not an error:
the definition is accepted as it is.

- An arm of a `case` whose tag the matches around have ruled out for
  the same value, by the same rule as above, is W0358. So is a `_`
  whose every tag they have ruled out. The warning says where the arm
  (or the match) that ruled it out is.
- An arm whose every value the arms above it match (its tag again, a
  `_` after arms of every tag, `Cons a (Cons b c)` after `_`), and an
  equation whose every argument the equations above it match, are
  W0359.

```haskell
size : List Int -> Int
size xs = case xs of
  Nil -> 0
  Cons h t -> case xs of
    Nil -> 1        -- W0358: ruled out by the arm `Cons h t`
    Cons a b -> 2

pick : Int -> Int
pick 0 = 0
pick n = n
pick 1 = 1          -- W0359: never used
```

Only arms the source writes in a `case` are warned of, and equations:
not those of an `if`, of `&&` and `||`, of a `do` block's
`let p = e else s`, nor the ones the compiler adds; nor an arm inside
one already warned of. An equation of names after equations of every
tag is not warned of, nor such an arm of a `case` of nested patterns:
which tags the type has is not looked at there.

| Code | Meaning |
|---|---|
| E0317 | a match, or a pattern that binds, leaves a value out |
| W0358 | an arm the matches around rule out |
| W0359 | an arm, or an equation, the ones above match every value of |

## Order

A definition sees the definitions above it, and itself. A name defined
further down is `no such variable`. This is on purpose: a module reads
from top to bottom.

Definitions that call each other go together under `mutual`:

```haskell
mutual
  even : Int -> Bool
  even 0 = True
  even n = odd (n - 1)

  odd : Int -> Bool
  odd 0 = False
  odd n = even (n - 1)
```

Inside the block, every signature is in scope in every body. After the
block, the names are the module's like any other.

The block is built as one record of its definitions, so:

- every definition in it needs a signature;
- every signature is a function type, or an alias of one
  (`ones : List Int` is refused).
