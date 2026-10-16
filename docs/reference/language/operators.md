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
