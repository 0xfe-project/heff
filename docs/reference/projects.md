---
title: Projects
---

How a project is laid out: its manifest, the packages it requires, and
its programs.  `heff init` makes one -- a module, a program and a test
-- and the [commands](commands.md) build, test and install it.

## The package

A project is a directory with a `package.hf`: a heff file defining
`package`, of type `Package` (`lib/manifest.hf`), read without being
run.  Its modules are the files under its `src/`, named by their paths
there (see [the language's modules](language.md#modules)).  Compiled
modules are cached in `.heff/`, where all heff makes is kept.

A package required is fetched by git from its path (`heff get`), the
version chosen as Go's minimal version selection does, into
`~/.cache/heff/mod`; `heff.sum` holds the tree of each version.  What a
project requires (`require`) and the packages it takes from directories
instead (`replace`) are none where left out, as their defaults in
`lib/manifest.hf` say.

```haskell
import manifest

package : Package
package =
  { module = "github.com/someone/hello"
  , heff = "0.1.0"
  , require = [{ name = "json", path = "github.com/someone/json", version = "1.2.0" }]
  }
```

A package's name is the first part of its modules' names: a lowercase
letter, then lowercase letters, digits and `_` (a capitalised name is a
tag).  It is never `test`: a project's tests, under its `test/`, are the
modules `test.X`.  The one directory of `src/`, each `name` required or
replaced, and the project `heff init` makes are named so.  `heff init
DIR` names the project as DIR's last part, and `heff init` as the
working directory's; a directory named otherwise (`MyApp`, `my-app`) is
refused, with a name that would do (`heff init myapp`).

## heff's own package

The package `heff` is heff's: the compiler and its library, which come
with heff.  The compiler keeps the name, as Go keeps `std`: no package
requires `heff`, replaces it, or gets it (`heff get heff`), and `heff
init` makes no project of that name -- each an error, E0347.

A project's `package.hf` names `heff` only where heff is from, and there
its `lib/` is the library (see [the library](library.md)):

- the directory the heff running came with -- the checkout it is built
  in (`.heff/out/heff`), or the heff home it is installed in;
- the checkout it was installed from: `heff install` of heff's own
  package writes it to the heff home's `source` (`~/.heff/source`),
  read from the heff home heff came with and from `HEFF_HOME`'s;
- a directory whose `lib/` is the library heff has already (`HEFF_LIB`,
  a release's), which so gains nothing.

Anywhere else, naming `heff` is an error: where heff is from is said by
heff and its heff home, never by the project.  To work on a checkout of
heff, build it (`make build`) or install from it.

## Programs

A program is `app/NAME/main.hf`, with an `app/NAME/build.hf` beside it
defining `build`, of type `Build`: the computation run, what it is
compiled to (`C`, `Page`), and the files and directories of the
project's root it ships (`ships`, none where left out).  `heff build`
builds every program to `.heff/bin/`; `heff install` builds them into
the heff home's `bin/`, and puts what they ship beside it, in place of
what was there.

```haskell
import manifest

build : Build
build = { backend = C, run = "main" }
```

A project may have programs of both: `app/server/` through `C`, and
`app/client/` a `Page` whose script the server serves, the API between
them an effect in a module of `src/` both import -- see
`examples/notes/`.

heff itself is such a program: `app/heff/build.hf` ships `lib` and
`package.hf`, so that `heff install` lays the heff home out as a
release is -- `bin/heff`, and beside it what heff finds there, its
library and its version -- and `source`, the checkout it came from.
