---
title: Theories
---

A theory says what a type has: `Equality` a type's equality, `Show`
how its values are written, `Coe` how one type is taken as another. An
instance is a value of a theory, and an
[instance argument](arguments.md#instance-arguments) asks for one.

A theory's fields may be types, not only values:

```haskell
theory Pointed where
  t : Type
  base : t

intPointed : Pointed
intPointed = { t = Int, base = 0 }

baseOf : (p : Pointed) -> p.t
baseOf p = p.base
```

A record cannot do this. A record is a value at run time, and a type is
not, so a record type's fields are values only:
`type Pointed = { t : Type, base : t }` is refused. Where a field is a
type, a function of types, or a theory, write a theory.

## Declaring one

```haskell
theory Counter where
  T : Type = Int
  start : T
  name : String = "counter"
```

`theory`, its name, `where`, then its fields, one a line. A field's
type may mention the fields before it: `start` is a `T`.

A field may have a default, `x : A = d`, as a record's may. A default
may use definitions and the fields before it, and nothing else.

## Parameters

```haskell
theory Addition (a : Type) where
  add : a -> a -> a

theory Coe (from : Type) (to : Type) where
  coe : from -> to
```

A theory may take parameters, written after its name as a function's
are. The fields may use them.

A parameter is given with the theory's name: `Addition Int` is a theory
of its own, and every value of it adds `Int`s. An instance search looks
for `Addition Int`, so a parameter is what the search matches. A field
is each value's own: two values of `Pointed` have the same type, whatever
their `t`.

`Addition Int` is a `Theory`, not a `Type`. So a `type` cannot name it:
`type AddInt = Addition Int` is refused.

## Building and copying

```haskell
theory Zero where
  t : Type
  zero : t

intZero : Zero
intZero = { t = Int, zero = 0 }

floatZero : Zero
floatZero = { intZero | t = Float64, zero = 0.0 }
```

A value of a theory is built as a record is: each field, in the
theory's order, leaving out those with a default. A parameter is no
field: `{ add = intAdd }` is an `Addition Int`. `x.f` reads a field.

It is copied as a record is too, `{ r | x = v }`. Where a field's type
mentions another field, a copy that sets that field sets the one that
depends on it too: `{ intZero | t = Float64 }` alone is refused, as its
`zero` would still be an `Int`.

## Specialising one

```haskell
theory Th where
  t : Type
  pick : t -> Bool

inst : Th
inst = { t = Bool, pick = \x -> x }

spec : Th @ { t = Bool }
spec = inst

-- | >>> spec.pick yes
-- | True
yes : spec.t
yes = True
```

`Th @ { t = Bool }` is the theory `Th`, but its `t` is `Bool`: a value
of `Th` is one of it if its `t` is `Bool`. So `spec.t` is `Bool`,
wherever `spec` is used, and `yes` is a `Bool`.

The braces set fields, `T @ { x = A, y = B }`, and `@` binds looser than
an application and tighter than `->`. A value of `Th` whose `t` is not
known, such as an argument `m : Th`, is not one. A name the theory has
no field of, or a `@` after something that is not a theory, is E0322.

`@` sets fields only. A parameter is given as an argument, by name if
you like: `Addition (a := Int)` is `Addition Int`.

### Asking for an instance of one

```haskell
firstInt : {c : Type} -> {{it : Iterate c @ { Elem = Int }}} -> c -> Int
firstInt xs = case it.next (it.start xs) of
  Just p -> fst p
  Nothing -> 0

-- | >>> found
-- | 1
found : Int
found = firstInt ([1, 2, 3] : List Int)
```

The parameters decide, the fields are checked. A search for
`Iterate (List Int) @ { Elem = Int }` looks for `Iterate (List Int)`,
finds the prelude's instance for lists, then checks that its `Elem` is
`Int`. Where it is not, the search fails with E0354, which says the
instance found and its field. A field the search wants may be not known
yet: the check is what makes it known.

So the instances of a theory are told apart by its parameters alone. A
theory with no parameter has one instance in scope at most: an instance
declared `Zero @ { t = Int }` and another declared `Zero @ { t = String }`
are both instances of `Zero`, and a search finds the two (E0342). To have
an instance for each type, make the type a parameter,
`theory Zero (t : Type)`.

## Each theory is a type of its own

Each `theory` declaration is a type of its own, as each `data` is: a
module that declares its own `Equality` has a theory unrelated to the
prelude's, and an instance of one is no instance of the other.

## Instances

How an instance is declared, and how heff finds one, is on
[Implicit and instance arguments](arguments.md#declaring-an-instance).
The arithmetic and comparison operators are theories' operations: see
[Operators are theories](operators.md#operators-are-theories).
