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
(`lib/gen.hf`, imported `as Gen`) and checks them, a hundred runs:

```haskell
import check
import co
import gen as Gen

lists : Test
lists = group "lists"
  [ unit "append" (assertEqual ([1] ++ [2]) [1, 2])
  , property "reversed twice" (do
      xs <- forAll (Gen.list 0 20 (Gen.int 0 99))
      label (if List.null xs then "empty" else "not empty")
      assertEqual (List.reverse (List.reverse xs)) xs)
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
of trivial values shows.  A value is shown, for a counterexample
(`forAll`) or for what differs, and compared by `assertEqual`, as its
type's `Show` writes it ([language.md](language.md#implicit-arguments)):
as the source writes it, a string quoted and a rune too, as Rust's
`Debug` writes a value -- not its text, `ToString`'s, its `Display`, by
which `"1"` and `1` would be shown the same.  The prelude has one of
numbers, strings and the like, and of lists, `Maybe`s, pairs and
`Either`s of them; a type of the test's own is given one by an
`instance`, as `test/check/map.hf` gives its commands.  A generator is
given by hand.  `agree` runs a program of commands on a system and on a
model of it and compares what they observe; drawn by a property, the
program shrinks to a short one that tells them apart
(`test/check/map.hf` holds `lib/map.hf` to a list of pairs).

## Files that expect errors

A file written to be wrong -- a test of what heff says of an error,
as those of `test/errors/` are -- says so on its first line:

```haskell
-- test: expect errors
-- a type defined as itself

a : Type
a = a
```

`heff test` then holds it to having errors, and says nothing of them:
it passes if it has some, and fails if it checks.  The line counts only
as the file's first; anywhere else it is a comment like another.  (What
the errors say is held by `test/golden.mjs`, against `test/golden/`.)

## The report

`heff test` says a line for each file, `ok` or `FAIL`, with how many of
its expectations hold (and of its tests, if it has some), or `errors, as
expected` for a file that expects them; each expectation or test that
fails is shown above its file's line, and each error of a file that does
not check is said on the standard error.  Then, if some files do not
check as they should, it lists them, a line each, where each first went
wrong -- its first error, as `path:line:col: error[E…]: what it says`,
a module it imports that is not there, or that it checks though it
expects errors.  The last line adds it up:

```
ok    test/errors/rec.hf  errors, as expected
FAIL  test/surface/oops.hf  0/0
files that do not check as they should:
  test/surface/oops.hf:5:8: error[E0314]: expected type Int, but got the tag True
485/485 expectations hold, 24/24 tests hold, 194/195 files check as they should (50 with expected errors)
```

It exits 1 if an expectation does not hold, a test fails, or a file does
not check as it should.
