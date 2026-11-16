---
title: Operators
---

## Defining one

```haskell
infixl .+. below +

-- | >>> 1 .+. 2 + 3
-- | 15
(.+.) : Int -> Int -> Int
(.+.) a b = a * 10 + b
```

An operator is a name made of symbols, defined in parentheses. Its
fixity says how it groups and how tightly it binds:

- `infixl`, `infixr` or `infix` (no grouping), then the operators of the
  group;
- `above` the operators it binds tighter than;
- `below` the operators it binds looser than.

Binding is only compared along these declarations. Two operators that no
chain of `above` and `below` relates cannot stand side by side: write
parentheses.

`below` with nothing after it is below every other operator.

## Names in backticks

```haskell
infixl `plus` above + below *

-- | >>> 1 `plus` 2 * 3
-- | 7
-- | >>> (0 `Cons` [1] : List Int)
-- | [0, 1]
-- | >>> [1] `List.append` [2]
-- | [1, 2]
plus : Int -> Int -> Int
plus a b = a + b
```

A name in backticks is applied as an operator: `` a `f` b `` is
`f a b`. Any name will do: a variable, a tag (`` x `Cons` xs ``, in a
pattern too), or a definition reached through an import's alias
(`` xs `List.append` ys ``). Nothing is written inside the backticks but
the name.

Application binds tighter than any of them, as it does than any
operator: `` f x `g` h y `` is `g (f x) (h y)`.

A name's fixity is declared as an operator's, and it can be named in
another declaration's `above` and `below`. A module's fixities of its
names come with it when it is imported, under an alias too: imported as
`M`, its `` `f` `` is written `` `M.f` ``.

A name with no fixity is not related to any operator but the loosest,
nor does it associate:

```haskell
-- | >>> larger 2 3
-- | 4
larger : Int -> Int -> Int
larger a b = id $ (a `max` b) + 1
```

`` id $ a `max` b `` needs no parentheses, but `` a `max` b + 1 `` and
`` a `max` b `max` c `` do.

## The prelude's

The prelude's operators, `+`, `==`, `&&`, `::`, `++`, `^`, `$` and the
rest, and their fixities are on [its page](../../api/prelude.md).

## Operators are theories

Each arithmetic and comparison operator is an operation of a theory, as
in Lean:

| Operator | Theory | Operation |
|---|---|---|
| `+` | [`Addition`](../../api/prelude.md#Addition) | `add` |
| `-` | [`Subtraction`](../../api/prelude.md#Subtraction) | `sub` |
| `*` | [`Multiplication`](../../api/prelude.md#Multiplication) | `mul` |
| `/` | [`Division`](../../api/prelude.md#Division) | `div` |
| `%` | [`Remainder`](../../api/prelude.md#Remainder) | `mod` |
| `/.` | [`FloatDivision`](../../api/prelude.md#FloatDivision) | `divide` |
| `==` `/=` | [`Equality`](../../api/prelude.md#Equality) | `equal` |
| `<` `<=` `>` `>=` | [`Order`](../../api/prelude.md#Order) | `lt` `le` |
| `-x` | [`Negation`](../../api/prelude.md#Negation) | `neg` |

So `n + 1` finds the instance for `n`'s type, and a function can work
for any type that has one:

```haskell
-- | >>> twice 21
-- | 42
twice : {a : Type} -> {{add : Addition a}} -> a -> a
twice x = x + x
```

Which types have each theory, and a theory's other operations, such as
`Order`'s `compare`, are with the theory on the prelude's page, linked
above.

The theories are named by whole words, so short names like `Add` are left
for your own programs.

When the type is known, the operation is compiled directly: `n + 1` at
`Int` is `intAdd n 1`.
