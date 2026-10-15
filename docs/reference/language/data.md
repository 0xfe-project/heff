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

## Updating a record

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

`{ r | x = v }` is `r` with `x` replaced. Several fields can be set,
each once, in any order. A path sets a field of a field:
`{ s | pos.x = 1.0 }` is `{ s | pos = { s.pos | x = 1.0 } }`.

A field not set keeps `r`'s value, even if it has a default: `loud.label`
is still `"quiet"`. Defaults apply when a record is built, not when it
is updated.

## Theories

```haskell
theory Packed where
  t : Type
  size : Int

packed : Packed
packed = { t = Int, size = 8 }
```

A record's values exist at run time; types do not. So a record with a
field that is a type, a function of types, or another theory is a
theory, declared with `theory`, one field a line.
`type Packed = { t : Type, size : Int }` is refused.

A theory is built and updated like a record. A field whose type depends
on a field being set is set too.

A theory declared again is a new type: a module's own `Equality` is not
the prelude's.
