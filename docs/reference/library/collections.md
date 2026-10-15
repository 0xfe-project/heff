---
title: Collections
---

## Arrays

```haskell
import array as Array

-- | the primes below `n`, by a sieve
-- | >>> primes 20
-- | [2, 3, 5, 7, 11, 13, 17, 19]
primes : Int -> List Int
primes n = List.filter (\i -> Array.index sieve i == Just 1) (List.range 2 (n - 1))
  where
  cross : Int -> Int -> Array Int -> Array Int
  cross p j xs = if j >= n then xs else cross p (j + p) (Array.set xs j 0)

  sift : Int -> Array Int -> Array Int
  sift p xs = if p * p >= n then xs else sift (p + 1) (cross p (p * p) xs)

  sieve : Array Int
  sieve = sift 2 (Array.replicate n 1)
```

`array`, imported as `Array`, has arrays: a fixed number of elements,
each reached at once by its index.

An array is a value and never changes: `Array.set xs i x` is another
array. But when nothing else holds `xs`, `set` writes in place, so an
array passed from one step of a loop to the next, as above, is updated
in place. An array still used elsewhere is copied once, at that `set`.

| Function | What it does |
|---|---|
| `length`, `index` | its length; `Maybe` the element at an index |
| `set`, `modify`, `swap`, `fill`, `reverse` | change elements (in place when possible) |
| `generate n f`, `replicate n x`, `fromList`, `empty` | make one |
| `toList`, `map`, `foldl`, `foldr` | read it |
| `slice`, `append` | parts; `slice` clamps, and counts negative indexes from the end |
| `sortBy`, `sort` | sort, stably |

On the C backend, an array of a number type is kept flat, as a C array:
`Array Float64` is `double`s, `Array UInt8` bytes. In JavaScript, an
array of numbers is a `Float64Array`.

## Maps and sets

```haskell
import std.map as Map

-- | how many times each word appears
-- | >>> Map.toList (counts ["a", "b", "a"])
-- | [MkPair "a" 2, MkPair "b" 1]
counts : List String -> Map.Map String Int
counts ws = List.foldl (\m w -> Map.update m w 1 (\new old -> new + old)) Map.empty ws
```

`std.map` and `std.set` are ordered by the keys' `Order`. The map comes
first: `Map.insert m k v`, `Map.lookup m k`, `Map.member m k`,
`Map.delete m k`.

- `Map.update m k v f` sets `k` to `v`, or to `f v old` if it is there,
  in one walk.
- `Map.alter` changes or removes what a key is bound to, in one walk.
- Also: `empty`, `singleton`, `fromList`, `toList`, `keys`, `values`,
  `size`, `mapValues`, `union`, `findMin`.

`std.set` has `empty`, `insert`, `member`, `delete`, `toList`,
`fromList`, `size` and `union`.

## Hash maps

```haskell
import std.hashmap as HashMap

-- | >>> HashMap.lookup (seen ["x", "y"]) "y"
-- | Just (True)
seen : List String -> HashMap.HashMap String Bool
seen ws = HashMap.fromList (List.map (\w -> MkPair w True) ws)
```

`std.hashmap`, imported as `HashMap`, is a hash array mapped trie, as
Clojure's: a million keys are 4 levels deep. Its keys need a `Hash` and
an `Equality`. The map comes first, as in `std.map`.

- `empty`, `insert`, `update`, `alter`, `lookup`, `delete`, `size`,
  `toList`, `fromList`, `foldl`.
- A node nothing else holds is changed in place.
- `toList` and `foldl` go in the trie's order, by hash, the same on
  every backend.
- `Hash` has instances for `String`, `Int`, `Rune`, `Bool`, and `Pair`,
  `List` and `Maybe` of types that have one.
