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

-- | >>> show (UInt8.add mask 255)
-- | "239"
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

## Arithmetic

```haskell
-- | >>> ([7 / 2, -7 / 2, 7 % -2] : List Int)
-- | [3, -4, -1]
-- | >>> ([7 / 0, 7 % 0] : List Int)
-- | [0, 7]
divisions : List Int
divisions = [7 / 2, -7 / 2]
```

- An `Int` divides rounding down; its remainder has the divisor's sign.
- A fixed-width integer wraps around as the machine does, divides
  rounding toward zero, and shifts by the amount modulo its width.
- Division by zero is 0, and the remainder is the dividend, so
  `(x / y) * y + x % y` is always `x`. To fail instead, use
  `exception`'s `divOrThrow` and `modOrThrow`.
- Each `Float32` operation is rounded to a `Float32`.
- A float divides with `/.`, by IEEE 754: by zero, an infinity.

## Fixed-width modules

```haskell
import int32 as Int32
import int8 as Int8

narrow : Int32 -> Int8
narrow x = Int8.fromInt (Int32.toInt x)
```

Each fixed-width integer type has a module, imported by a name since
they share their names: `add`, `sub`, `mul`, `div`, `rem`, `eq`, `lt`,
`le`, `minBound`, `maxBound`, `fromInt` and `toInt`. `float32` has the
arithmetic, `eq`, `lt`, `le`, `isNaN`, `fromInt`, `fromFloat`,
`toFloat`, `floor` and `sqrt`.

Importing a module also brings in its instances, so the prelude's
operators work at its type: `x * c + d` at `Int64`.

An `Int`'s bits are the prelude's `and`, `or`, `xor`, `complement`,
`shiftL` and `shiftR`, with `import int as Int`. They act as two's
complement with the sign extended forever; shifting by a negative count
shifts the other way.

How each backend stores numbers is in
[Providers](../providers.md#numbers-and-runes).
