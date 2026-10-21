---
title: Projects
---

## Layout

```
hello/
├── package.hf          the package
├── src/
│   └── hello/
│       └── *.hf        its modules
├── app/
│   └── NAME/
│       ├── main.hf     a program
│       └── build.hf    how the program is built
├── test/
│   └── *.hf            tests
├── bench/
│   └── *.hf            benchmarks
└── .heff/              what heff makes: builds, caches
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
- Versions are `a.b.c`, each a tag `va.b.c`. When several versions of
  a package are required, heff picks as Go's minimal version selection
  does.
- `heff get PATH[@VERSION]` adds a requirement and fetches it. With no
  VERSION it takes the newest tag of the path's major.
- Where no tag is a version, `heff get PATH` takes the default branch's
  latest commit, at a pseudo-version: `0.0.0-TIME-COMMIT`, the commit's
  time (UTC, `YYYYMMDDhhmmss`) and the first 12 hex digits of its hash,
  as `0.0.0-20261017093000-3fa9c1e2b4d7`. A path ending in `/vN` gets
  `N.0.0-TIME-COMMIT`. `heff get PATH@BRANCH` and `heff get PATH@COMMIT`
  (7 or more hex digits of its hash) take that commit the same way.
- A pseudo-version is older than every release of its major, so
  `0.0.0-...` is older than `0.0.1`; between pseudo-versions the later
  commit is newer. Once the package is tagged, `heff get PATH` takes the
  tag. `heff.sum` records a pseudo-version's tree as a tag's, and
  fetching it checks out that very commit.
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

### What a program can import

A program imports only what its backend has:

| Modules | Backends |
|---|---|
| `lib/`'s own, such as `co`, `json`, `resource` | every backend |
| `std.*` | `C`; but `std.reference`, `std.time`, `std.path`, `std.map`, `std.set`, `std.hashmap`, `std.hashtable`, `std.cli`, `std.cli.interactive`, `std.terminal.widget`, `std.doc` and `std.math` work on every backend |
| `browser.*` | `Page` |

A C program importing a `browser.*` module is E0351, and a page
importing an effect only C has is E0352.
