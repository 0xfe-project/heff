---
title: Modules
---

```haskell
module counter (Counter, counter, bump, count)

data Counter = MkCounter Int

counter : Counter
counter = MkCounter 0

bump : Counter -> Counter
bump (MkCounter n) = MkCounter (n + 1)

count : Counter -> Int
count (MkCounter n) = n
```

## Files and names

A module is one file. Its name is its path, with `.` for `/` and without
`.hf`:

| File | Module |
|---|---|
| `src/parse/surface.hf` | `parse.surface` |
| `test/helpers.hf` | `test.helpers` |
| `app/hello/main.hf` | `app.hello.main` |

Under `src/`, the path starts after `src/`. A file elsewhere in the
project, such as a test or a program, is named by its path from the
project's root: `lib/` is no different.

`import` looks for a module in the project's `src/`, then in the
[library](../../api/index.md) that comes with heff, of heff's own
version: the `lib/` beside heff, or where `HEFF_LIB` says. A file outside
`src/` can also import files of the project by their paths from the
root. A module found in two places is an error, and neither
file is loaded.

## The module claim

Every file starts with its module claim, `module` and its name, before
its imports. A missing module claim is E0156, and one with another name
is E0157. `package.hf` and a program's `build.hf` are the exception: they are read as data, not
loaded as modules, and have no module claim.

## Exports

```haskell
module parse.surface (parse, Token, Tree(..), (<+>))
```

The list after the name says what an importer sees:

- A name not in the list belongs to the module. An importer does not
  see it, as a name or as an instance.
- A data type listed as `T` is seen without its tags: an importer can
  use the type, but cannot build or match its values with the tags
  (E0353). `T(..)` exposes the tags too.
- A type alias, a record type and a theory are always seen whole. `(..)`
  is only for data types (E0155).
- An operator is written in parentheses.
- Each name is listed once (E0155).

A module claim without a list, `module parse.surface`, exposes
everything.

## Imports

```haskell
import std.io
import rune as Rune
```

`import m` brings in every name `m` exposes. `import m as M` brings them
in qualified only: `M.x`. `as`, as `import` is, is a
[keyword](definitions.md#keywords).

Every module also has, without writing them:

- `builtin` and `prelude`;
- `list`, as `List`: `List.map`, `List.length`.

Writing one of these again (`import prelude`) is an error, but
`import prelude as P` is allowed. Importing the same module twice is an
error.

### Which name is meant

A name is looked up in this order:

1. the module's own definitions;
2. the imports it writes;
3. the imports it has without writing them.

Two written imports that both have a name are an error only where the
name is used unqualified.

In `M.x`, an alias `M` comes before a type of the same name, so a module
can be imported as `Rune` while `Rune` alone still names the type:

```haskell
import rune as Rune

code : Rune -> Int
code r = Rune.toInt r
```

A local name `M` hides an alias `M`: where `M` is a variable, `M.x` is
its field.

## Defining a name once

A module defines each name once. A second signature, `data`, `type`,
`theory` or `primitive` of a name is E0153, which says where the first
one is. A function's equations come right under its signature, with no
other definition between them.

Tags are not definitions: two data types may have tags of the same name.

## Errors

| Code | Meaning |
|---|---|
| E0153 | a name defined twice |
| E0155 | an export list names something twice, or `(..)` on what is not a data type |
| E0156 | the module claim is missing |
| E0157 | the module claim names another module |
| E0353 | a tag the module does not export |
