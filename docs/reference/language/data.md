---
title: Data and records
---

## Data types

```haskell
data Shape = Circle Float64 | Rect Float64 Float64

-- | >>> areaOf (Rect 2.0 3.0)
-- | 6.0
areaOf : Shape -> Float64
areaOf (Circle r) = 3.14159 * r * r
areaOf (Rect w h) = w * h
```

`data` lists a type's tags, each with the types of its fields. A data
type may take parameters: `data Tree a = Node a (List (Tree a))`.

### Tags

A tag takes its type from where it is used, so two data types may have
a tag of the same name:

```haskell
data Light = Red | Amber | Green

data Ink = Red | Black

stop : Light
stop = Red

pen : Ink
pen = Red
```

So something around a tag must say its type: a signature, as above; a
function it is given to, `paint Red` where `paint : Ink -> String`; or
the tag itself, `(Red : Ink)`. With nothing to say it,
`let x = Red`, it is an error.

## Type aliases

```haskell
type Name = String
```

`type` gives a type another name. The two are the same type.

## Records

```haskell
type Opts = { name : String, verbose : Bool = False, label : String = name }

quiet : Opts
quiet = { name = "quiet" }

-- | >>> quiet.label
-- | "quiet"
labelOf : Opts -> String
labelOf o = o.label
```

A record type names its fields. A record gives each field, in the type's
order, and `r.x` reads one.

A field may have a default, `x : A = d`. A record may leave that field
out, and gets `d`. A default may use definitions and the fields before
it, and nothing else, so it means the same wherever it is used: above,
`label` is `name` unless given.

A record type may take parameters: `type Pair2 a = { first : a, second : a }`.

## Copying a record

```haskell
type Point = { x : Float64, y : Float64 }

type Body = { pos : Point, vx : Float64, vy : Float64 }

-- | >>> (move 1.0 { pos = { x = 0.0, y = 0.0 }, vx = 2.0, vy = 3.0 }).pos.y
-- | 3.0
move : Float64 -> Body -> Body
move dt b = { b | pos.x = b.pos.x + dt * b.vx, pos.y = b.pos.y + dt * b.vy }

loud : Opts
loud = { quiet | name = "loud" }
```

`{ r | x = v }` is a new record: `r`'s fields, but `x` is `v`; `r` itself
is unchanged. Several fields can be set, each once, in any order. A
path sets a field of a field: `{ s | pos.x = 1.0 }` is
`{ s | pos = { s.pos | x = 1.0 } }`.

A field not set takes `r`'s value, even if it has a default: `loud.label`
is still `"quiet"`. Defaults apply when a record is built, not when it
is copied.

## Theories

A theory is a large record: its fields may be types as well as values.
It is what an instance is of: `Equality`, `Show`, `Coe` are theories.
It is declared with `theory`, one field a line:

```haskell
theory Packed where
  t : Type
  size : Int

packed : Packed
packed = { t = Int, size = 8 }
```

It is a declaration of its own because a record is a value at run time,
and a type is not. So where a field is a type, a function of types, or
a theory, the record is large, and must be a theory:
`type Packed = { t : Type, size : Int }` is refused.

A theory is built and copied as a record is. Where a field's type
mentions another field, a copy that sets that field sets the one that
depends on it too:

```haskell
theory Zero where
  t : Type
  zero : t

intZero : Zero
intZero = { t = Int, zero = 0 }

floatZero : Zero
floatZero = { intZero | t = Float64, zero = 0.0 }
```

`{ intZero | t = Float64 }` alone is refused: its `zero` would still be
an `Int`.

Each `theory` declaration is a type of its own, as each `data` is: a
module that declares its own `Equality` has a theory unrelated to the
prelude's, and an instance of one is no instance of the other.
