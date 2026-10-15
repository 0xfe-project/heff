---
title: Projects
---

## Layout

```
hello/
  package.hf          the package
  src/hello/*.hf      its modules
  app/NAME/main.hf    a program
  app/NAME/build.hf   how the program is built
  test/*.hf           tests
  bench/*.hf          benchmarks
  .heff/              what heff makes: builds, caches
```

`heff init NAME` makes a project with a module, a program and a test.

## package.hf

```haskell
import manifest

package : Package
package =
  { module = "github.com/someone/hello"
  , heff = "0.4.0"
  , require =
      [{ name = "json", path = "github.com/someone/json", version = "1.2.0" }]
  , replace = [{ name = "json", dir = "../json" }]
  }
```

`package.hf` defines `package`, of type `Package` (`lib/manifest.hf`).
It is read as data, never run.

| Field | Meaning |
|---|---|
| `module` | where the package lives; its last part is the package's name |
| `heff` | the heff version it is for |
| `require` | the packages it uses: the name it imports them by, where they live, and the version |
| `replace` | packages taken from a directory instead, while you work on them |

`require` and `replace` default to `[]`.

### Names

A package's name is the first part of its modules' names: `hello` for
`hello.greeting`. It starts with a lowercase letter, then lowercase
letters, digits and `_`.

- It is never `test`, which names a project's tests.
- It is never `heff`: that package is the compiler and its library,
  which come with heff, and no project requires, replaces or gets it
  (E0347).
- `heff init DIR` names the project after DIR, and refuses a name that
  does not fit, suggesting one that does.

### Packages required

- `heff get` fetches each required package by git, from its path, into
  `~/.cache/heff/mod`, and records each version's git tree in
  `heff.sum`.
- Versions are `a.b.c`. When several versions of a package are
  required, heff picks as Go's minimal version selection does.
- `heff get PATH[@VERSION]` adds a requirement and fetches it.
- A replaced package is read from its directory. Its own `package.hf`
  must have the `module` its `require` names.

## Programs

```haskell
import manifest

build : Build
build = { backend = C, run = "main" }
```

A program is `app/NAME/main.hf`, with `app/NAME/build.hf` beside it
defining `build`, of type `Build`:

| Field | Meaning |
|---|---|
| `backend` | `C` for a native program, `Page` for a web page's script |
| `run` | the definition run, usually `main` |
| `ships` | files and directories of the project that `heff install` puts beside the program; default `[]` |

`heff build` builds every program into `.heff/bin/`. `heff install`
builds them to run fastest, into `~/.heff/bin/`, with what they ship.

A project can have programs of both backends, such as a server through
`C` and a page it serves, sharing an effect from `src/`: see
`examples/notes/`.
