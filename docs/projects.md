# Projects

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

## Programs

A program is `app/NAME/main.hf`, with an `app/NAME/build.hf` beside it
defining `build`, of type `Build`: the computation run, what it is
compiled to (`C`, `Node`, `Page`), and the files and directories of the
project's root it ships (`ships`, none where left out).  `heff build`
builds every program to `.heff/bin/`; `heff install` builds them into
the heff home's `bin/`, and puts what they ship beside it, in place of
what was there.

```haskell
import manifest

build : Build
build = { backend = C, run = "main" }
```

heff itself is such a program: `app/heff/build.hf` ships `lib` and
`package.hf`, so that `heff install` lays the heff home out as a
release is -- `bin/heff`, and beside it what heff finds there, its
library and its version.
