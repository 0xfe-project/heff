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
the tag itself, `(Red : Ink)`. A tag with nothing to say its type, as
in `let x = Red`, will cause an error.

## Unit and pairs

```haskell
-- | >>> swap (1, "one")
-- | MkPair "one" 1
swap : {a b : Type} -> (a, b) -> (b, a)
swap (x, y) = (y, x)

-- | >>> total (1, 2, 3)
-- | 6
total : (Int, Int, Int) -> Int
total (a, b, c) = a + b + c

done : ()
done = ()
```

`()` is the unit, `MkUnit`, and `(a, b)` is a pair, `MkPair a b`.
`(a, b, c)` nests to the right: `(a, (b, c))`. `(e)` is `e`, grouped.

As a value, `()` is a `Unit` and `(a, b)` a `Pair`, so each says its
type where a tag alone would not: `pure ()` needs nothing around it to
say what it is. Written as a type, `()` is `Unit` and `(A, B)` is
`Pair A B`; so is an argument of a type there, `List (String, Int)`.
Elsewhere, a type given as an argument is written `Pair A B`.

The same forms are patterns, nested as deep as needed: `(x, y)`,
`((a, b), c)`, `()`.

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

A record's fields are values only; where a field is a type, write a
[theory](theories.md).

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
