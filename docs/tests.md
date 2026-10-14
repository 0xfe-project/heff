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

A test is a top-level definition of type `Test` (check's);
`heff test` runs a file's tests after its doc tests and watches.  A
unit test checks; a property draws values from generators
(`lib/generator.hf`, imported `as Generator`) and checks them, a hundred runs:

```haskell
import check
import co
import generator as Generator

lists : Test
lists = group "lists"
  [ unit "append" (assertEqual ([1] ++ [2]) [1, 2])
  , property "reversed twice" (do
      xs <- forAll (Generator.list 0 20 (Generator.int 0 99))
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
(`test/check/map.hf` holds `lib/std/map.hf` to a list of pairs).

## Integration tests

A test that needs the world outside the program -- commands run, files
read and written -- is an integration test: its body is given the
process (std.process) and the filesystem (std.filesystem) besides
`check`.

```haskell
import check
import co
import std.filesystem
import std.process

tool : Test
tool = group "tool"
  [ integration "writes its output where it is run" (do
      r <- run { command = "sh", args = ["-c", "echo hi > out.txt"] }
      out <- readFile "out.txt"
      assertEqual out (Just "hi\n"))
  , within 5000 (integration "answers quickly" (do
      r <- run { command = "true" }
      assert "it ended well" (succeeded r)))
  ]
```

`heff test` cannot run such a test in itself, as it runs the others: it
builds the file's tests into a program through C (in `.heff/test/`, by
zig, as `heff run` builds), and runs each integration test the file has
in it, a process of its own.  Each is run in a new, empty directory of
its own (where the system keeps temporary ones), its working directory,
removed after it; and each has a time limit, a minute unless `within MS`
says otherwise, at which its process is killed and the test fails.
Integration tests run at once, those of all the files given in one
pool, as many as `--jobs N` says, else `HEFF_TEST_JOBS`, else half the
machine's cores (at least one), so that a run of the tests does not
take all of it.  Their lines come after those of the file's other
tests, and a file that has some is reported after the files that have
none, in their order.  Each has, in its environment, `HEFF`,
the path of the heff that runs it, and `HEFF_TEST_ROOT`, the project's
root: a test of heff runs the heff tested, not one on the PATH, on the
project's files (`run { command = heff, args = ..., dir = root }`).

## Subtests

An integration test may make tests of its own as it runs, as Go's
`t.Run` does: `subtest NAME body`.  Each is said on a line of its own,
under its test's name (`files / a.txt`), holds or fails by itself and
keeps its own golden texts; subtests may have subtests.  So the tests
can be what is there when they run, a subtest a file of a directory,
with no list of them written.

```haskell
-- (a subtest each file, its text held to its golden one)
each : {e : HFunctor}
     -> {{chk : check <: e}}
     -> {{disk : filesystem <: e}}
     -> List String
     -> Co e Unit
each Nil = pure MkUnit
each (Cons n rest) = do
  subtest n (do
    text <- readFile ("inputs/" ^ n)
    golden n (fromMaybe "" text))
  each rest

inputs : Test
inputs = integration "files" (do
  names <- readDirectory "inputs"
  each (List.sort (fromMaybe Nil names)))
```

Each subtest runs in a process of its own, as many at once as the
integration tests: the test's program, asked for a subtest by its
names, does the test again from its start, doing only the subtests on
the way to the one asked for and passing over the others; the test
itself, run, names its subtests and does none.  So a test must do the
same each time up to its subtests and name them alike (a directory's
names sorted, as above), each name once and with no `/`; what it does
after a subtest is done as if the subtest held.  A subtest is an
integration test's: a unit test that makes one fails.

## Selecting tests

`heff test --run REGEXP` runs only the tests whose names the pattern
matches, as Go's `go test -run` does: the pattern is cut at each `/`,
a level each, and a test is run when each of its names -- its groups',
then its own -- is matched (anywhere in it) by the level of its depth.
A test whose names are fewer than the levels is matched by those it
has, and one deeper than the pattern by its groups.  `--run '^lsp/hover'`
runs the tests of the groups named `lsp...` whose names have `hover`;
`--run /hover` the tests named `...hover...` of any group, and every
test of no group.  Subtests are selected so too, a level each, as
their tests name them: their test runs to name them, and those the
pattern does not match are not run.  A file none of whose tests is
selected (a test run only on the way to subtests the pattern does not
match counts as none) is passed
over, its watches and doc tests too, and not listed; where no test of
any file is, `heff test` says `no tests match 'PATTERN'` and fails.

The pattern is a regular expression, of a small part of RE2's syntax
(the `regex` module): a byte for itself, `.`, a class `[a-z0-9_]` or
`[^...]`, `\` before a byte that is not a letter or a digit, `*`, `+` and
`?` after one of those, `^` first and `$` last, and `|` between
alternatives.  What it does not read -- groups, counts `{n}`, `\d` -- is an
error, not a literal, so that a pattern means the same once more is read.

## Golden texts

A unit test or an integration test may hold a text to one kept in a
file, its golden text: `golden NAME text`.  The file is `NAME` in the
directory beside the test's file, named as it with `.golden` for `.hf`
(`parser.golden/NAME`, of `parser.hf`); a name has letters, digits,
`-`, `_` and `.`, not first, and may have directories, between `/`s
(`eval/lib/list.hf.txt`).  `heff test` fails the test where the text is
not the one kept (and says the first line where they differ) or none is
kept;
`heff test --update` writes the text there instead, and says so.  A
test that fails otherwise is not held to its golden texts, nor are they
written.

```haskell
pretty : Test
pretty = unit "a module, laid out" (golden "module.txt" (layout example))
```

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
the errors say is held by heff's golden tests, `heff test
test/golden.hf`; that each file of the repository checks or has its
errors as it says, by its surface tests, `heff test test/surface.hf`.)

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
