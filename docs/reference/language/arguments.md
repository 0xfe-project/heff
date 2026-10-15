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

instance showColor : Show Color
showColor = { show = written }
  where
  written : Color -> String
  written Red = "Red"
  written Green = "Green"

-- | >>> show ([Red, Green] : List Color)
-- | "[Red, Green]"
colors : List Color
colors = [Red, Green]
```

`instance x : T` declares the definition under it an instance. An
instance argument in scope, `{{eq : Equality a}}`, is one too.

An instance is in scope:

- in the definitions after it;
- in the modules that import its module, by name or by alias;
- not in the modules that import those.

The prelude's instances are in scope everywhere.

### Finding one

Exactly one instance in scope must fit the type wanted. None is E0341;
more than one is E0342, whatever their values. So an import can turn a
search into an error, but never into a different answer.

An instance may take implicit and instance arguments of its own:

```haskell
data Pair3 a = MkPair3 a a

instance equalityPair3 : {a : Type} -> {{eq : Equality a}} -> Equality (Pair3 a)
equalityPair3 = { equal = \p q -> case p of
  MkPair3 a b -> case q of
    MkPair3 c d -> equal a c && equal b d }

-- | >>> same
-- | True
same : Bool
same = equal (MkPair3 1 2 : Pair3 Int) (MkPair3 1 2)
```

A search matches what the instance is for, `Equality (Pair3 a)`,
against the type wanted, which solves `a`. Then it finds the instance's
own instance arguments, each by a search of its own. When one of those
fails, the error says which instance wanted it.

Rules:

- If the type wanted is not fully known yet, the search waits until it
  is. One never known is E0340.
- Searches nest at most 32 deep (E0349).
- An instance cannot take an explicit argument (E0348).
- Each of its implicit arguments must appear in what it is for (E0350).

## Equality, Show and ToString

The prelude has three theories most types have:

| Theory | Use | `"hi"` | `'a'` |
|---|---|---|---|
| `Equality` | `==`, `/=`, `equal` | | |
| `Show` | `show x`: the value as source code | `"\"hi\""` | `"'a'"` |
| `ToString` | `toString x`: the value as text for a reader | `"hi"` | `"a"` |

`Show` is what tests use to print values, so two different values never
look the same. `ToString` is Rust's `Display`; `Show` is its `Debug`.

They have instances for `Int`, `Float64`, `String`, `Rune`, `Bytes`,
`Bool`, `Unit` and `Ordering`. `Equality` and `Show` also have them for
`List`, `Maybe`, `Pair` and `Either` of types that have one. `ToString`
does not, as a list has no one text. Each fixed-width number's module
has its own.

## Errors

| Code | Meaning |
|---|---|
| E0340 | the type an instance is wanted at is never known |
| E0341 | no instance fits |
| E0342 | more than one instance fits |
| E0348 | an instance takes an explicit argument |
| E0349 | a search nests more than 32 deep |
| E0350 | an instance's implicit argument is not in what it is for |
