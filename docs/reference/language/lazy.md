---
title: Lazy values
---

```haskell
-- | the first element, or what the default comes to: the default is
-- | computed only for an empty list
-- | >>> firstOr ~(List.length (List.range 0 1000000)) [5, 6]
-- | 5
firstOr : ~Int -> List Int -> Int
firstOr d [] = d
firstOr d (x :: _) = x
```

heff computes by value: an argument is computed before the call. `~`
puts that off.

## `~A` and `~e`

- `~A` is the type `Lazy A`: an `A` not computed yet.
- `~e` delays `e`: a `Lazy` of `e`'s type, computed only where it is
  forced.

Which of the two `~e` is, its `e` says: where `e` is a type, `~e` is
that type's `Lazy`; otherwise `e` is delayed. Types and values share
their syntax, so both are written the same way:

```haskell
-- | >>> force (later Int 5)
-- | 5
later : (a : Type) -> a -> ~a
later a x = ~x
```

`Lazy` takes a `Type`, so there is no `Lazy Type`: a type cannot be
delayed.

A `Lazy` can be anywhere a type can: a field of a data type or a
record, a function's argument or result.

```haskell
data Cell = MkCell ~Int String
```

## Spacing

`~` is written close to what it delays, as `-` is to what it negates
([Numbers](numbers.md#negative-literals)): `~` right before a name, a
number, a string, a rune, `(` or `[`, and not right after an operand,
is this `~`. So:

| Written | Means |
|---|---|
| `~x`, `~(f x)`, `~[1, 2]`, `~~x` | delayed |
| `f ~x` | `f` applied to `~x` |
| `a ~ b`, `a~b`, `~ x` | the operator `~`, if one is defined |

`~` reaches as far as an argument does: `~f x` is `(~f) x`; delay a
call as `~(f x)`.

## Forcing

`force : {a : Type} -> Lazy a -> a` computes the value. And where an
`a` is wanted and a `Lazy a` is given, it is forced without writing
`force`: the prelude has a coercion, `Coe (Lazy a) a`
([Coercions](arguments.md#coercions)).

```haskell
square : Int -> Int
square n = n * n

-- | >>> squareOf ~7
-- | 49
squareOf : ~Int -> Int
squareOf l = square l
```

An operator is a theory's ([Operators](operators.md)), whose instance
is looked for at the type given, so `l * 2` looks for one of `~Int`:
write `force l * 2`.

A `Lazy` is computed the first time it is forced, and its value is
kept: forced again, it is not computed again. A `Lazy` handed to many
places, or forced in a loop, is computed once.

What is never forced is never computed: `~abandon` is a value like any
other until something forces it.

## Only `~e` delays

A delay is never put in for you. Where a `~Int` is wanted, an `Int` is
an error, as is a call that gives one:

```haskell
seven : ~Int
seven = ~(3 + 4)     -- not `seven = 3 + 4`
```

A `Lazy`'s tag is the language's own: a `Lazy` is made by `~e`, and
taken apart by forcing it.
