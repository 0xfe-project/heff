# Tests

Three kinds of check, all run by `heff test` (on the files given, else
the project's `test/`): doc tests, watches and tests.  `heff promote
FILE` writes the value of each doc test and watch of a file under it as
its expectation.

## Doc tests

A doc test is an example in a definition's documentation: a line
`-- | >>> e` of the doc comment right above the definition, with the
value expected on the `-- | ...` lines under it.  It belongs to the
definition, and is checked after it, as a watch written under it
would be: the definition and what is above it are in scope.  The
language server shows it in the definition's hover, and `heff test`
names the definition when it does not hold.

```haskell
-- | the list, its elements the other way round
-- | >>> reverse [1, 2, 3]
-- | [3, 2, 1]
-- | >>> reverse (Cons "a" Nil)
-- | ["a"]
-- |
-- | It takes as many steps as there are elements.
reverse : {a : Type} -> List a -> List a
```

The expectation is the `-- | ...` lines up to the next doc test, a
blank line of the doc comment (`-- |`), or its end: it may go on over
more than one line (read as one expression), and the comment goes on
after a blank one.  It is compared by value, so `Cons 3 (Cons 2 Nil)`
holds of `[3, 2]`.  A value is shown as the compiled code has it: a
function as `#fun`, a type as `#erased`, and a value that takes types
first -- erased where it is compiled -- given them: `Nil` of
`{a : Type} -> List a` is `[]`.  A doc test with no expectation is
checked only to elaborate; `heff promote` writes the value of each doc
test and watch under it as its expectation (replacing what was there).
A doc test is in the comment right above a definition -- its
signature, or a `data`, `type`, `theory` or `primitive` -- with nothing
but comments between, no blank line.  One that is not (a blank line
under it, a `test`, a watch or an import, the end of the file) is
checked where it is, as a watch, and warned of: write it as a watch, or
move it into the doc comment right above its definition.

## Watches

A watch, `-- >>> e` with the value expected on the `-- ...` lines under
it, is such a check of no one definition, written where it is: of what
the file defines above it, or what it imports.  A `test NAME` names the
watches after it (up to the next definition) in `heff test`'s report.

## Unit tests and properties

A test is a top-level definition of type `Test` (`lib/check.hf`);
`heff test` runs a file's tests after its doc tests and watches.  A
unit test checks; a property draws values from generators
(`lib/gen.hf`, imported `as G`) and checks them, a hundred runs:

```haskell
import check
import co
import gen as G

lists : Test
lists = group "lists"
  [ unit "append" (equal (showList showInt) ([1] ++ [2]) [1, 2])
  , property "reversed twice" (do
      xs <- forAll (showList showInt) (G.list 0 20 (G.int 0 99))
      label (if List.null xs then "empty" else "not empty")
      equal (showList showInt) (List.reverse (List.reverse xs)) xs)
  ]
```

```
  ok    lists / append
  ok    lists / reversed twice  (100 runs, 17% empty, 83% not empty)
ok    lists.hf  0/0, tests 2/2
```

Randomness and checking are effects: a unit test's type grants it only
`check`, a property also `random`, and the runner gives both.  A
generator makes each value with the smaller ones to try instead
(Hedgehog's rose trees), so a property that fails is shrunk, every draw
of it, with nothing written for it; the runner then says the
counterexample and the seed to run it again (`--seed N`; the default
seed is always the same, so a run of the tests is always the same run).
A label counts the runs it is given, so that a property that holds only
of trivial values shows.  There are no type classes, so a value is
shown, and compared by `equal`, by a function given, and a generator is
given by hand.  `agree` runs a program of commands on a system and on a
model of it and compares what they observe; drawn by a property, the
program shrinks to a short one that tells them apart
(`test/check/map.hf` holds `lib/map.hf` to a list of pairs).
