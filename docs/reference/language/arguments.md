---
title: Implicit and instance arguments
---

## Implicit arguments

```haskell
-- | >>> identity 3
-- | 3
identity : {a : Type} -> a -> a
identity x = x
```

An argument in braces, `{a : Type}`, is implicit: heff infers it where
the function is used. To give it yourself, name it: `identity (a := Int) 3`.
Named arguments go in the order of the function's type: of
`{a : Type} -> {b : Type} -> ...`, `(a := ...)` is written before
`(b := ...)`, and either may be left out.

### Defaults

```haskell
greet : {loud : Bool := False} -> String -> String
greet name = if loud then name ^ "!" else name

-- | >>> calm
-- | "hi"
calm : String
calm = greet "hi"

-- | >>> shout
-- | "hi!"
shout : String
shout = greet (loud := True) "hi"
```

An implicit argument with `:= d` is `d` where it is not given. It is not
inferred. A default may only use definitions, so it means the same
everywhere.

### Type constructors

```haskell
twice : {m : Type -> Type} -> {{mo : Monad m}} -> m Int -> m Int
twice c = mo.bind c (\x -> mo.bind c (\y -> mo.pure (x + y)))

-- | >>> twice (Just 2 : Maybe Int)
-- | Just (4)
twiceJust : Maybe Int
twiceJust = twice (Just 2 : Maybe Int)
```

Matching `m Int` with `Maybe Int` has two answers, `Maybe` and
`\_ -> Maybe Int`. When nothing else decides, heff takes the first, as
Lean does: `m` is `Maybe`. It does this only where the other side is a
data type, record or theory applied to enough arguments. To mean the
other answer, give it: `(m := \_ -> Maybe Int)`.

## Instance arguments

```haskell
member : {a : Type} -> {{eq : Equality a}} -> a -> List a -> Bool
member x [] = False
member x (y :: ys) = equal x y || member x ys

-- | >>> found
-- | True
found : Bool
found = member 2 [1, 2] && member 2.5 [0.5, 2.5]
```

An argument in double braces, `{{eq : Equality a}}`, is an instance
argument: heff finds it among the instances in scope. To give it
yourself, name it: `member (eq := myEquality) x xs`.

### Declaring an instance

```haskell
data Color = Red | Green

instance instanceDebugColor : Debug Color
instanceDebugColor = { debug = written }
  where
  written : Color -> String
  written Red = "Red"
  written Green = "Green"

-- | >>> debug ([Red, Green] : List Color)
-- | "[Red, Green]"
colors : List Color
colors = [Red, Green]
```

`instance x : T` declares the definition under it an instance. Nobody
calls an instance by its name, so it is long on purpose:
`instance`, the theory, then the type. An
instance argument in scope, `{{eq : Equality a}}`, is one too.

An instance is in scope:

- in the definitions after it;
- in the modules that import its module, by name or by alias;
- not in the modules that import those.

The prelude's instances are in scope everywhere.

### Finding one

Exactly one instance in scope must fit the type wanted. None is E0341;
more than one is E0342, whatever their values. So an import can turn a
search into an error, but never into a different answer. Evidence that
an effect is in a world, `h <: e`, that no instance in scope is, is
found by where `h` is in the world instead, as `(x := _)` finds it (see
[giving the evidence](effects.md#giving-the-evidence)): none is E0355,
more than one E0356.

An instance may take implicit and instance arguments of its own:

```haskell
data Duo a = MkDuo a a

instance instanceEqualityDuo : {a : Type} -> {{eq : Equality a}} -> Equality (Duo a)
instanceEqualityDuo = { equal = \p q -> case p of
  MkDuo a b -> case q of
    MkDuo c d -> equal a c && equal b d }

-- | >>> same
-- | True
same : Bool
same = equal (MkDuo 1 2 : Duo Int) (MkDuo 1 2)
```

A search matches what the instance is for, `Equality (Duo a)`,
against the type wanted, which solves `a`. Then it finds the instance's
own instance arguments, each by a search of its own. When one of those
fails, the error says which instance wanted it.

Rules:

- If the type wanted is not fully known yet, the search waits until the
  definition is elaborated. Where it is still not known then, the one
  instance in scope that unifies with it is taken, and solves what was
  unknown, as Agda's instance search does: under `{{st : state Int <: e}}`,
  `get` alone reads an `Int`. None, or more than one, is E0340.
- Searches nest at most 32 deep (E0349).
- An instance cannot take an explicit argument (E0348).
- Each of its implicit arguments must appear in what it is for (E0350).

### An instance for every type

```haskell
instance instanceAdditionGraph : {g : Type} -> {{m : Graph g}} -> Addition g
```

An instance whose type is its theory of a bare variable, `Addition g`,
fits every search of that theory: `Addition Int` fits it, `g` being
`Int`. The prelude's `instanceAdditionInt` fits too, so with both in
scope, `1 + 2` finds more than one instance, and is E0342.

The instance's own instance arguments, `{{m : Graph g}}`, are searched
for only once it is chosen. They do not rule it out: `Int` having no
`Graph` leaves it fitting `Addition Int`, and a search failing there
does not go back to try another instance.

Write a plain function instead, and an instance for each type it is
for:

```haskell
data Adjacency = MkAdjacency (List (Int, Int))

theory Graph (g : Type) where
  edges : g -> List (Int, Int)
  fromEdges : List (Int, Int) -> g

instance instanceGraphAdjacency : Graph Adjacency
instanceGraphAdjacency = { edges = \(MkAdjacency es) -> es, fromEdges = MkAdjacency }

additionOfGraph : {g : Type} -> {{m : Graph g}} -> Addition g
additionOfGraph = { add = \a b -> m.fromEdges (m.edges a ++ m.edges b) }

instance instanceAdditionAdjacency : Addition Adjacency
instanceAdditionAdjacency = additionOfGraph

-- | >>> size (MkAdjacency [(1, 2)] + MkAdjacency [(2, 3)])
-- | 2
size : Adjacency -> Int
size (MkAdjacency es) = List.length es
```

`additionOfGraph` is no instance, so no search finds it, and
`Addition Int` is the prelude's alone. `Addition Adjacency` is
`instanceAdditionAdjacency`, `additionOfGraph` at `Adjacency`. Or give
it where it is wanted, with no instance at all:
`(+) (add := additionOfGraph) x y`.

## Equality, Show and ToString

The prelude has three theories most types have:

| Theory | Use | `"hi"` | `'a'` |
|---|---|---|---|
| [`Equality`](../../api/prelude.md#Equality) | `==`, `/=`, `equal` | | |
| [`Debug`](../../api/prelude.md#Debug) | `debug x`: the value as source code | `"\"hi\""` | `"'a'"` |
| [`Show`](../../api/prelude.md#Show) | `show x`: the value as text for a reader | `"hi"` | `"a"` |

`Debug` is what tests use to print values, so two different values never
look the same. `Show` is Rust's `Display`; `Debug` is its `Debug`.

Which types have instances of them is on the
[prelude's page](../../api/prelude.md); a fixed-width number's
instances are in its module, such as [`int64`](../../api/int64.md).

## Coercions

```haskell
data Name = MkName String

instance instanceCoeStringName : Coe String Name
instanceCoeStringName = { coe = MkName }

greet : Name -> String
greet (MkName s) = "hi " ^ s

-- | >>> hello
-- | "hi ann"
hello : String
hello = greet "ann"
```

Where an expression of one type is given and another is expected, and
the two do not unify, heff looks for an instance of the prelude's
[`Coe`](../../api/prelude.md#Coe) of the two, `Coe String Name`, and
puts its `coe` around the expression: `greet "ann"` is
`greet (coe "ann")`. The instance is found as an instance argument is,
among those in scope, by the same rules. Otherwise nothing changes:
with no instance, the error is the mismatch it was.

Rules:

- Both types must be known whole where they meet. A type still waiting
  for an implicit argument, such as the `List ?a` of `List.reverse Nil`,
  is no coercion's.
- One coercion, never two in a row: with `Coe A B` and `Coe B C`, an `A`
  is no `C`.
- Only an expression whose type is worked out first is coerced: a
  variable, a call, a string literal. A number literal, a constructor
  (`MkName`, `Nil`, `Just x`), a lambda and a record take the type
  expected as their own, so `3` where a `Name` is wanted is still an
  error. Annotate to coerce one: `(3 : Int)`.
- `coe x` says it explicitly.

The prelude has one coercion, `Coe (Lazy a) a`: a delayed value given
where its value is wanted is forced ([Lazy values](lazy.md)).

## Errors

| Code | Meaning |
|---|---|
| E0340 | the type an instance is wanted at is never known, and no one instance in scope says it |
| E0341 | no instance fits |
| E0342 | more than one instance fits |
| E0348 | an instance takes an explicit argument |
| E0349 | a search nests more than 32 deep |
| E0350 | an instance's implicit argument is not in what it is for |
