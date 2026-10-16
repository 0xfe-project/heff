---
title: Definitions
---

```haskell
-- | the first element, or `d` for an empty list
-- | >>> firstOr 0 [5, 6]
-- | 5
firstOr : Int -> List Int -> Int
firstOr d [] = d
firstOr d (x :: _) = x
```

## Signatures and equations

A definition is a signature, `name : Type`, then its equations right
under it. Each equation matches its arguments against patterns, tried
from the top; the first that matches is used.

A pattern is one of:

- a name, which matches anything and binds it;
- `_`, which matches anything;
- a tag applied to patterns, `Just x`, `MkPair a b`;
- a list: `[]` or `x :: xs` as usual, `[x, y]` for a list of that length;
- a number, string or rune literal;
- a binary, `b[...]` ([Binaries](binaries.md)).

Patterns nest: `Node x (c :: cs)`.

## Keywords

These words are keywords, and nothing is named by one:
`data`, `type`, `theory`, `primitive`, `instance`, `mutual`, `import`,
`as`, `case`, `of`, `if`, `then`, `else`, `do`, `where`, `let`, `in`,
`mut`, `while`, `for`, `break`, `continue`, `return`, `try`, `catch`,
`with`, `test`, `abandon`, `infixl`, `infixr`, `infix`; and the
built-in types' names, `Type`, `Int`, `String`, `Rune`, `Array` and
the sized numbers (`Int8` to `UInt64`, `Float32`, `Float64`).

## Expressions

```haskell
-- | >>> sign (-2)
-- | "negative"
sign : Int -> String
sign n = case compare n 0 of
  LT -> "negative"
  EQ -> "zero"
  GT -> "positive"

-- | >>> area 3
-- | 9
area : Int -> Int
area n = let side = n in side * side

-- | >>> quadruple 3
-- | 12
quadruple : Int -> Int
quadruple n = double (double n)
  where
  double : Int -> Int
  double k = k * 2

-- | >>> List.map (\x -> x + 1) [1, 2]
-- | [2, 3]
increments : List Int
increments = List.map (\x -> x + 1) [1, 2]
```

- `case e of` matches `e` against patterns, one arm a line.
- `if c then a else b` is `a` where `c` is `True`.
- `let x = e in body` names `e` in `body`. A `let` does not generalise:
  what it binds has one type for every use.
- `where` under an equation holds definitions only that equation sees.
  They can use the equation's arguments.
- `\x -> e` is a function; `\(x : T) -> e` gives its argument a type.

`&&` and `||` evaluate their right side only when needed.

## Order

A definition sees the definitions above it, and itself. A name defined
further down is `no such variable`. This is on purpose: a module reads
from top to bottom.

Definitions that call each other go together under `mutual`:

```haskell
mutual
  even : Int -> Bool
  even 0 = True
  even n = odd (n - 1)

  odd : Int -> Bool
  odd 0 = False
  odd n = even (n - 1)
```

Inside the block, every signature is in scope in every body. After the
block, the names are the module's like any other.

The block is built as one record of its definitions, so:

- every definition in it needs a signature;
- every signature is a function type, or an alias of one
  (`ones : List Int` is refused).
