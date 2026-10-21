---
title: Tests
---

`heff test` checks three kinds of thing: doc tests, watches and tests.
Given no files, it checks the project's `src/` and `test/`.

## Doc tests

```haskell
-- | the list the other way round
-- | >>> List.reverse [1, 2, 3]
-- | [3, 2, 1]
-- |
-- | It takes as many steps as there are elements.
backwards : List Int -> List Int
backwards xs = List.reverse xs
```

A doc test is a `-- | >>> e` line in the comment right above a
definition. The lines under it, up to a blank `-- |` line or the next
`>>>`, are the value expected, compared by value: `Cons 3 Nil` holds of
`[3]`.

- The definition and everything above it are in scope.
- A function is shown as `#fun`, and a type as `#erased`.
- A doc test with no expected value is only type checked.
- `heff promote FILE` writes each doc test's value as its expected value.
- A doc test that is not right above a definition is checked where it is,
  as a watch, with a warning.
- The language server shows doc tests when you hover the definition.

## Watches

A watch, `-- >>> e` with the expected value on the `-- ...` lines under
it, checks something of no one definition, where it is written.
`test NAME` names the watches after it in the report.

## Unit tests and properties

```haskell
import check
import co
import generator as Generator

lists : Test
lists =
  group
    "lists"
    [ unit "append" (assertEqual ([1] ++ [2] : List Int) [1, 2])
    , property "reversed twice" (do
        xs <- forAll (Generator.list 0 20 (Generator.int 0 99))
        label (if List.null xs then "empty" else "not empty")
        assertEqual (List.reverse (List.reverse xs)) xs)
    ]
```

```
  ok    lists / append
  ok    lists / reversed twice  (100 runs, 17% empty, 83% not empty)
```

A test is a top-level definition of type `Test`, from `import check`.

- `unit NAME body` runs `body` once.
- `property NAME body` runs `body` a hundred times. `forAll` draws a
  value from a generator (`import generator as Generator`).
- `group NAME tests` puts tests under a name.
- `assertEqual`, `assert` and `failure` check; `label` counts the runs it
  is in; `skip WHY` says a test cannot run here, and is counted apart.

A property that fails is shrunk to a small counterexample, which is
printed with the seed to run it again (`--seed N`). The default seed is
fixed, so every run is the same.

Values are compared and printed by their `Show`. A type of your own needs
an instance: see [Implicit and instance arguments](language/arguments.md#declaring-an-instance).

`agree` runs a program of commands on a system and on a model of it, and
compares them. `test/check/ordmap.hf` holds `std.ordmap` to a list of pairs
this way.

## Integration tests

```haskell
import std.filesystem
import std.path
import std.process

tool : Test
tool =
  group
    "tool"
    [ integration "writes its output where it is run" (do
        r <- run { command = "sh", args = ["-c", "echo hi > out.txt"] }
        out <- readFile "out.txt"
        assertEqual out "hi\n")
    , withinTime 5000 (integration "answers quickly" (do
        r <- run { command = "true" }
        assert "it ended well" (succeeded r)))
    ]
```

An integration test can also run commands and use files: it gets
`std.process` and `std.filesystem` besides `check`, and what they throw
(`exception ProcessError`, `exception FileError`). A throw the test does
not hear fails it.

- `heff test` builds the file's tests into a C program, and runs each
  integration test in a process of its own.
- Each runs in a new, empty directory, removed afterwards.
- Each has a time limit, a minute unless `withinTime MS` says otherwise. At
  the limit, it and every process it started are killed.
- They run at once, up to `--jobs N`, else `HEFF_TEST_JOBS`, else half
  the machine's cores.
- Its environment has `HEFF`, the heff running it, and `HEFF_TEST_ROOT`,
  the project's root.

### Subtests

```haskell
-- (a subtest for each file, its text held to its golden text)
each : {e : HFunctor}
     -> {{chk : check <: e}}
     -> {{disk : filesystem <: e}}
     -> {{fx : exception FileError <: e}}
     -> List String
     -> Co e Unit
each [] = pure MkUnit
each (n :: rest) = do
  subtest n (do
    text <- readFile ("inputs/" ^ n)
    golden n text)
  each rest

inputs : Test
inputs =
  integration "files" (do
    paths <- readDirectory "inputs"
    each (List.sort (List.map basename paths)))
```

`subtest NAME body`, as Go's `t.Run`, makes a test while an integration
test runs, so tests can come from what is there, such as the files of a
directory.

Each subtest runs in a process of its own: the test runs again from the
start, doing only the subtests on the way. So a test must do the same
each time up to its subtests, and name them the same way, each name once,
with no `/`.

## Golden texts

```haskell
pretty : Test
pretty = unit "laid out" (golden "list.txt" (show ([1, 2] : List Int)))
```

`golden NAME text` holds `text` to the file `NAME` in the directory
beside the test's file, named like it with `.golden` for `.hf`:
`parser.golden/NAME` for `parser.hf`. `heff test --update` writes the
text there instead.

## Choosing tests

`heff test --run REGEXP` runs only the tests whose names match, as Go's
`-run` does. The pattern is split at each `/`, one part for each level
of groups: `--run '^lsp/hover'` runs the tests with `hover` in their
names, in groups starting with `lsp`.

The pattern is a small part of RE2: a byte, `.`, `[a-z]`, `[^...]`, `\`
before a symbol, `*`, `+`, `?`, `^`, `$` and `|`. Anything else is an
error.

`heff test --watch` tests again at each change of the files tested, or
of `package.hf`, `src/` or `app/`.

## Files that expect errors

```haskell
-- test: expect errors
-- a type defined as itself
```

A file whose first line is `-- test: expect errors` passes when it has
errors, and fails when it checks.

## The report

```
ok    test/errors/rec.hf  errors, as expected
FAIL  test/surface/oops.hf  0/0
files that do not check as they should:
  test/surface/oops.hf:5:8: error[E0314]: expected type Int, but got the tag True
485/485 expectations hold, 24/24 tests hold, 194/195 files check as they should
```

One line for each file, `ok` or `FAIL`, with how many of its
expectations and tests hold. Failures are shown above their file's line,
and files that do not check are listed at the end. `heff test` exits 1
if anything fails.
