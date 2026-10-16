---
title: Numbers
---

## Types

| Type | What it is |
|---|---|
| `Int` | an integer of any size |
| `Float64` | an IEEE 754 double |
| `Int8` `Int16` `Int32` `Int64` | fixed-width integers, two's complement |
| `UInt8` `UInt16` `UInt32` `UInt64` | fixed-width unsigned integers |
| `Float32` | an IEEE 754 single |

## Literals

```haskell
import uint8 as UInt8

-- | >>> show mask
-- | "240"
mask : UInt8
mask = 0xf0

million : Int
million = 1_000_000

flags : Int
flags = 0b1010
```

- An integer literal can be of any number type; a fractional one, `2.5`,
  of a float type.
- Hexadecimal `0x605` and binary `0b1010` are allowed, and `_` can
  separate digits.
- The literal must fit its type: `300` as a `UInt8` is an error, and so
  is `1.5` as an `Int32`.
- Where nothing decides the type, an integer literal is an `Int` and a
  fractional one a `Float64`. In `[1, 2.5]`, both are `Float64`.

### Negative literals

A `-` right before a digit is the literal's sign, unless it comes right
after an operand with no space. So:

| Written | Means |
|---|---|
| `a - 1`, `a-1` | subtraction |
| `f -1` | `f` applied to `-1` |
| `-128` as an `Int8` | its least value |

A `-` right before a name or `(` negates it: `-p.x`, `-(f y)`. It is the
`neg` in scope, so it works for any type with a `Negation`. `f -x` is
`f (-x)`; `- x`, with a space, is not a negation.

## Operations

Numbers are added, compared and divided by the prelude's operators,
whose theories each number type has: see [Operators](operators.md).
How each type divides, rounds and wraps around is with its instances:

- `Int` and `Float64`: [`prelude`](../../api/prelude.md), and an
  `Int`'s bits in [`int`](../../api/int.md);
- the fixed-width integers: [`int8`](../../api/int8.md),
  [`int16`](../../api/int16.md), [`int32`](../../api/int32.md),
  [`int64`](../../api/int64.md), [`uint8`](../../api/uint8.md),
  [`uint16`](../../api/uint16.md), [`uint32`](../../api/uint32.md),
  [`uint64`](../../api/uint64.md);
- `Float32`: [`float32`](../../api/float32.md).

A fixed-width type's module is imported by a name, as they share their
names (`import int64 as Int64`), and brings in its instances, so the
prelude's operators work at its type. One fixed-width type goes to
another by way of an `Int`:

```haskell
import int32 as Int32
import int8 as Int8

narrow : Int32 -> Int8
narrow x = Int8.fromInt (Int32.toInt x)
```

How each backend stores numbers is in
[Providers](../providers.md#numbers-and-runes).
