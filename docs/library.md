# The library

`lib/` goes with the compiler, its version the compiler's; `HEFF_LIB`
says where it is, else it is the `lib/` the compiler came with: the
repository's, a release's, or the heff home's (`heff install` puts it
there).  Its modules alone declare primitives --
`primitive intAdd : Int -> Int -> Int`, a function the runtime has --
and each primitive once: one a module of a program declares, or one
declared again, is an error.

heff's own repository is the library's source.  Its `package.hf` names
the package `heff`, which the compiler keeps for itself (see
[projects](projects.md#heffs-own-package)): where heff is from, that
project's `lib/` is the library, whatever `HEFF_LIB` or the installed
heff says.  So an installed heff, or one built in another checkout,
checks the repository's `lib/float32.hf` as the library's, its
primitives its own and declared once.  Any other project's `lib/` is no
library, and a project elsewhere that names itself `heff` is an error
(E0347), its `lib/` no library either.

## Modules

Every module has `builtin`, `prelude` and `list` (as `List`) without
writing them ([how names are found](language.md#modules)).  The others
are imported by their names:

- `co` ([effects](language.md#effects)), and effects made with it:
  `state`, `exn` (with `exnDiv` and `exnMod`, a division that throws by
  zero) and `parallel`;
- `array`, `map` and `set`;
- `check` and `gen`, the tests and their generators
  ([tests.md](tests.md#unit-tests-and-properties));
- `manifest`, the types of `package.hf` and `build.hf`
  ([projects.md](projects.md));
- the fixed-width numbers' `int8` to `int64`, `uint8` to `uint64` and
  `float32`, `int` with an `Int`'s bits, and `rune` with a `Rune`'s
  operations ([numbers](language.md#numbers),
  [runes and strings](language.md#runes-and-strings));
- `bytes`, the operations of `Bytes` ([bytes](language.md#bytes)).

## The batteries

The batteries ship with it, in two halves, by where a program runs.
What is above, in `lib/`, is pure, the same on every backend.

`lib/std/` is the system's, for the C backend (`--backend=c`):

- `std.io` (the standard streams), `std.filesystem`, `std.process`,
  `std.http` (an HTTP client), `std.progress` (what a program is doing,
  drawn on a terminal), `std.thread` (threads truly at once: a pool,
  spreads, and `Par` handled by them -- [threads.md](threads.md)):
  effects of the system, C's alone;
- `std.ref` (the world's cells) and `std.time`: effects needed on both
  sides, one module each with a provider in C and one in JavaScript,
  which a page's script has too;
- `std.handle` (the world's things, which one effect hands out and
  another takes), `std.path`, `std.cli` (command lines), `std.doc`
  (documents laid out) and `std.math` (a float's functions: `sin`,
  `atan2`, `exp`, `log`, `pow`, `round`, `pi`, `e` and the like): pure,
  for any backend.

`lib/browser/` is the browser's, for JavaScript (`--backend=page`, and
`esm`):

- `browser.dom` (web pages);
- `browser.js` (JavaScript's strings, `JSString`).

A program imports only its own half: a C program that imports a module
of `lib/browser/` is refused at the import (E0351), and so is a page's
script that imports an effect of `lib/std/` with no provider there
(E0352), each naming the module and the backend -- before `main`'s
effect is looked at.  (`std.io`, `std.filesystem`, `std.process` and
`std.progress` have providers for Node.js too, kept for heff itself
and its tests, `--backend=node`; no other module of `lib/std/` has
one.)

An effect's JavaScript side is in `M.provider.js` beside `M.hf`, its C
side in `M.provider.c`: see [providers.md](providers.md).

`std.http` is an HTTP client, on the C backend alone (a page fetches by
`browser.dom`'s `fetchText`, the browser's `fetch`).  Import it by a name, `import std.http as Http`: a
`Request` -- `{ url = u }`, or with its `method`, `headers` (a name and
a value each), `body` (`Bytes`), `timeout` (milliseconds in all, 0 for
none) and `follow` (redirects, followed unless `False`) -- is sent by
`Http.send`, and `Http.get u` and `Http.getText u` are a `GET`.  What
comes back is `Either HttpError Response`: a `Response`, its `status`,
`headers` and `body` (`Bytes`; `Http.text` reads it as text,
`Http.header` finds a header), whatever its status, a 404 or a 500 too;
an `HttpError` where no response came -- the connection refused, the
name not found, TLS, the time up -- with curl's `code` and its
`message` (`Http.showError` writes it as curl's tool does).

```haskell
import std.http as Http

-- (its status; 0 where no response came)
statusOf : Either Http.HttpError Http.Response -> Int
statusOf (Left err) = 0
statusOf (Right res) = res.status

upload : {e : HFunctor} -> {{web : Http.http <: e}} -> Bytes -> Co e Int
upload b = do
  r <- Http.send { url = "http://localhost:8080/up", method = "POST", body = b, timeout = 5000 }
  pure (statusOf r)
```

It is libcurl, loaded into the process at the first request (no
`-lcurl`), each thread a handle of its own whose connections are kept
for the next request; where there is no libcurl (a static musl program,
Windows), a `curl` spawned for each, given what libcurl is, so that
either answers the same.  A body is sent with a `POST`, and with any
method where it is not empty, with no `Content-Type` but the request's
own.

## Examples

`examples/` has apps for the browser: `node examples/build.mjs` builds
each to `.heff/examples/NAME/`, where its `index.html` opens.
`examples/testbridge/` is a project of another kind, built by `heff
build` there: a Linux program, through C, that makes a bridge and a
veth pair by rtnetlink, its socket's calls a provider of its own.
