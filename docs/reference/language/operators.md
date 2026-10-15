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

The prelude declares:

```
infixr ||
infixr && above ||
infix == /= < <= > >= above &&
infixl + - above == /= < <= > >=
infixl * / % /. above + -
infixr ++ above == /= < <= > >=
infixr ^ above == /= < <= > >=
infixr :: above == /= < <= > >= below + - * / % ++ ^
infixl >>= below ||
infixr $ below
```

So arithmetic binds tighter than comparisons, which bind tighter than
`&&` and `||`. `::` binds looser than arithmetic, `++` and `^`:
`x + 1 :: xs` is `(x + 1) :: xs`. `++` and `^` are not related to `+`,
so `a + b ^ c` needs parentheses.

`f $ x` is `f x`, grouping to the right: `f $ g $ h x` is `f (g (h x))`.

## Operators are theories

Each arithmetic and comparison operator is an operation of a theory, as
in Lean:

| Operator | Theory | Operation |
|---|---|---|
| `+` | `Addition` | `add` |
| `-` | `Subtraction` | `sub` |
| `*` | `Multiplication` | `mul` |
| `/` | `Division` | `div` |
| `%` | `Remainder` | `mod` |
| `/.` | `FloatDivision` | `divide` |
| `==` `/=` | `Equality` | `equal` |
| `<` `<=` `>` `>=` | `Order` | `lt` `le` |
| `-x` | `Negation` | `neg` |

So `n + 1` finds the instance for `n`'s type, and a function can work
for any type that has one:

```haskell
-- | >>> twice 21
-- | 42
twice : {a : Type} -> {{add : Addition a}} -> a -> a
twice x = x + x
```

Integer types have all of these but `/.`. Float types have no `/` and
`%`; their division is `/.`.

`Order` also has `compare`, `min` and `max`. `Bitwise` has `and`, `or`,
`xor`, `complement`, `shiftL` and `shiftR`. `IsZero` has `isZero`.

The theories are named by whole words, so short names like `Add` are left
for your own programs.

When the type is known, the operation is compiled directly: `n + 1` at
`Int` is `intAdd n 1`.
