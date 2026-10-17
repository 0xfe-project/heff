# Changelog

## Unreleased

### Language

- Lazy values: `~A` is the type `Lazy A`, `~e` delays `e` until it is forced (#107).
- A theory's fields are fixed by named application, `Th (t := Bool)`, replacing `Th ~ { t = Bool }`. `~` is now an ordinary operator symbol.
- `T @ { x = v, y = w }` specialises a theory again, decided at parse time. Named arguments name arguments only.
- Instance search: a specialised goal is searched by its theory and parameters, then the found instance's fields are unified with the goal's (E0354). A field given by an instance argument not found yet waits for the search instead of failing with E0311.
- Tuples: `()` is the unit and `(a, b)` a pair, `(a, b, c)` nesting right; each says its type (`Unit`, `Pair`), so `pure ()` needs no annotation. Where a type is expected, `()` is `Unit` and `(A, B)` is `Pair A B`, and types print so. The same forms are patterns, and patterns now bind in `p <- e`, `with e as p` and `\p -> e` as well as `let p = e`; a pattern there that can fail is non-exhaustive (E0317).
- E0317 says `non-exhaustive pattern: no case provided for ...`.
- `let p = e else s` in a `do` block: `p` may fail to match, and `s` runs where it does; `s` must leave the block, by `return`, `break`, `continue` or `abandon` (E0203 otherwise).
- `if let` is removed: write a `case`, or `let p = e else s`.
- A signature may name more than one definition, `x, y, z : T`, at the top level, in `where` and `mutual`, and for a theory's or a record type's fields (`{ x, y : Int }`).

### Standard library

- `Order` carries its type's `Equality`, the field `eq`: every `Order` instance gives one. `ord.eq.equal`, or `(==) (eq := ord.eq)`, compares by it; `==` still finds only an `Equality` in scope. `orderByCompare` takes the `Equality` as an instance argument.

### Backends and performance

- Loop accumulators stay unboxed: a `Float64` total in a `do` loop is boxed once at exit. `doloops_float_do` 7.40 G to 0.42 G instructions.
- `Array.set` in a loop that uses the array linearly checks uniqueness once on entry, then writes in place, so the C compiler vectorises the loop (#102).
- A loop reading and setting arrays at an index checks the bounds once on entry. `saxpy` 0.48 s to 0.30 s (#102).
- `generate` of a lambda giving one fixed-width number builds the array flat, and in-range reads of it are unchecked. `Int64` sum 0.40 s to 0.13 s (#102).
- New numeric array benchmarks: sums, dot product, `saxpy` and `Array.map` over ten million elements (#102).
- In threads mode, a shared object whose count is back to one is reused in place again, as an owned one is. A fold over a shared 16M-node tree: 0.54 s to 0.39 s.
- A program using threads built as parts (`heff build`, `heff compile DIR/`) no longer crashes: every C file now sees threads mode, not only the one with `main`.

### Docs

- New language reference pages for lazy values and for theories.

## 0.5.0

Released 2026-10-16.

### Language

- `do` blocks have imperative statements: `let mut` and `:=`, `while`, `for` over anything `Iterate`, `break`, `continue`, `return`, and `if` without `else` (#86).
- `do` also has `if let`, and `try ... catch` over the exception effect (#100). `try` takes only exceptions, and its `catch` arm is required.
- `do` has a `with e as x do` statement: the rest of the block is the body of a scoped resource (#106). `with` and `as` are keywords.
- `while True` and `if True` work: a bare tag as a condition is read as the built-in `Bool`.
- `mutual` blocks define mutually recursive functions, each with its signature. The formatter lays them out, and the elaborator checks them with the errors worded as the text has the names.
- Every file begins with `module a.b`, optionally listing what it exposes, as in `module (x, T, U(..))`. A missing or wrong name is E0156, another name for the claim is E0157, hiding a name or tag is E0353 (#101).
- `Coe from to`: where a type mismatches and an instance of `Coe` between the two is in scope, the coercion is inserted. `String` is taken as a `Path` this way (#105).
- `Monad` is a theory in the prelude, with `traverse`, `foldM`, `when`, `unless` and `replicateM` written once. `>>=` is an operator.
- A local hides an alias.

### Standard library

- `std.path`: `Path` is its own opaque type. `parse` and `render` take the style (`os`, `native` or `onWindows`). `std.filesystem` and `std.process` take and give `Path` (#105).
- Host things (files, processes, connections, listeners, GPU devices and buffers, DOM elements) are foreign values held by a `with` (#99).
- The `resource` module holds `With s a` and `scoped acquire release body`, one scoped resource for every effect. A resource that cannot be acquired or released is thrown over the exception effect, never returned as an `Either`.
- `std.filesystem`: `withFile`, `withStdin`, `withStdout` and `withStderr` hold a file or a standard stream for a body.
- `std.process`: `withProcess` holds a subprocess, which is ended (SIGTERM, then SIGKILL after two seconds) when the body ends. `run` takes an `Invocation` and gives a `Ran`. Waits and pipe reads are pending in the reactor, so they block no other green thread. `workingDir` and `setWorkingDir` are new.
- `std.net`: `withConnection`, `withListener` and `accept` hold sockets. `std.http.server` has `serveConcurrently`, serving each connection in its own green thread.
- `std.reactor` waits may have a deadline, so a `sleep` lets the other green threads run (#97).
- `browser.dom`: the document comes from `withDocument`, and elements are host values passed by value. `browser.gpu`'s `dispatch1` to `dispatch3` take buffers one by one.
- Acronym aliases are written in capitals.
- New `ffi` module: C types as values (`CType`, `CResult`, `Described`). A provider can be described in C and the compiler writes its glue (#99, #97).
- `onWindows` says whether the program is built for Windows.

### Compiler and tools

- `heff test --watch` reruns tests whenever a tested file changes. `heff test` with no file also runs the project's doc tests.
- `heff bench`: a `Bench` definition found by type, built at `-O2`, run in its own program with Go-style counts and lines (or JSON). Options `--run`, `--time`, `--count`. Objects made are counted when built with `HEFF_COUNT_ALLOCS`.
- `heff doc` writes a Markdown API page for each module from its module comment and doc comments, with signatures laid out as `heff fmt` does them. `--out DIR` writes the pages and an index.
- `heff init` takes the package name from the working directory on every system, and refuses a directory that cannot name a package, saying the rule.
- LSP: quick fix for E0156 and E0157 writes `module <name>`. The VS Code extension writes the header of an empty `.hf` file it opens (#101). Types shown on hover are the solved ones. `mutual` is highlighted as a keyword.
- Loading a file by a relative path from below the project root finds the root above the working directory.
- Pending instance arguments are searched as soon as their goals are known, so what an instance fixes (a type member) is known to the code after it.
- The runtime's fault message no longer uses `std.debug.print`, which cut about 1.1 s from every program's zig build (ReleaseSafe 1.4 s to 0.4 s).
- A SIGTERM arriving while a command starts no longer leaves the command running (`heff dev` shutdown).
- A Nix flake builds heff from the seed; the README installs by `make`.

### Backends and performance

- Parts of the C runtime are rewritten in Zig and linked into every C program: big integer arithmetic, bit helpers, UTF-8 reading and writing, hashes and float formatting. Floats are printed by Ryu, with the same text as before.
- Strings: slices of 64 bytes or more, and at least a quarter of their root, are views of it, so taking a tail again and again no longer copies.
- Threads mode allocates in pages owned by the creating thread. `queens` runs 10-15% faster at 4 or more threads, and a make-here, free-there loop no longer grows (#66).
- Mutually recursive cycles no longer drop parameter pads on each round. `matmul` 4.6 s to 3.2 s.
- `simplify` moves the arms of a match on a match into the ways out choosing them, with no copying. `edit_distance` 450M allocations to 30k, 2.55 s to 1.71 s.
- An unboxed worker is made for a loop that rebuilds its tag argument at each call. `doloops_pure` array loops 0.44 G to 0.24 G instructions.
- Reading an `Int` from a lent array is lent, so a `do` loop over an array no longer dups and drops each element.

### Docs

- The documentation is a site: a tutorial from a first program to tests, and a reference split into the language and the library. The library reference is the `heff doc` API pages.
- One site per release, with a version picker. `dev` is main, each release tag has its own `X.Y.Z`, and the latest is at the root. The release workflow publishes it.
- New pages: concurrency (unstable), host types and providers, lists, tags, records and theories.
- Examples: a first CLI program, a terminal todo app on a zipper, and the browser pages as one project built by `heff build` and served by `heff dev`.
