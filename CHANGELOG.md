# Changelog

## Unreleased

### Language

- Lazy values: `~A` is the type `Lazy A`, `~e` delays `e` until it is forced (#107).
- A theory's fields are fixed by named application, `Th (t := Bool)`, replacing `Th ~ { t = Bool }`. `~` is now an ordinary operator symbol.
- `T @ { x = v, y = w }` specialises a theory again, decided at parse time. Named arguments name arguments only.
- Instance search: a specialised goal is searched by its theory and parameters, then the found instance's fields are unified with the goal's (E0354). A field given by an instance argument not found yet waits for the search instead of failing with E0311.
- Tuples: `()` is the unit and `(a, b)` a pair, `(a, b, c)` is `((a, b), c)`; each says its type (`Unit`, `Pair`), so `pure ()` needs no annotation. Where a type is expected, `()` is `Unit` and `(A, B)` is `Pair A B`, and types print so. The same forms are patterns, and patterns now bind in `p <- e`, `with e as p` and `\p -> e` as well as `let p = e`; a pattern there that can fail is non-exhaustive (E0317).
- E0317 says `non-exhaustive pattern: no case provided for ...`.
- `let p = e else s` in a `do` block: `p` may fail to match, and `s` runs where it does; `s` must leave the block, by `return`, `break`, `continue` or `abandon` (E0203 otherwise).
- `if let` is removed: write a `case`, or `let p = e else s`.
- In a `do` block, `then` and `else` may start a line under their `if`, as outside a block; an `else` under an `if` is that `if`'s, and in an `else if` chain, the first `if`'s. `heff fmt` lays them out so.
- `if <- c then ...` and `case <- c of`: what an `if` tests or a `case` matches may be the value of an action, as `(<- c)`.
- `(x := _)` finds evidence `h <: e`: where `e` is a sum by `<+>` known at the call, the one place of `h` in it (through the evidence in scope where a part of it is a world of its own). None is E0355, more than one E0356, no `h <: e` or no world known E0357.
- exception's `attempt err c`: the rest of the world, `e`, is implicit, found from where the result is used, as `main`'s type.
- `plus` is gone: effects are summed by `<+>` alone. `<:` binds looser than `<+>`, so `h <: h <+> k` is `h <: (h <+> k)`.
- `xs <- for x in c do body` gathers each turn's value, what its last statement comes to, into a list, in order; a turn left by `continue` gives none, and `break` ends the gathering. A `for` alone runs for its effects, as before.
- A signature may name more than one definition, `x, y, z : T`, at the top level, in `where` and `mutual`, and for a theory's or a record type's fields (`{ x, y : Int }`).
- `^` groups to the left: `s ^ a ^ b` is `(s ^ a) ^ b`, so a string grown a piece at a time is grown in place, each piece appended to it, not joined to the next first. A function ending in `x ^ "," ^ f xs` still appends a piece at a time. str_concat 19.5 G to 16.9 G instructions, base64_sha256 41.2 G to 37.0 G, graphemes 27.8 G to 26.0 G.

### Standard library

- std.map is renamed std.ordmap, and its type `Map` `OrdMap`: the name says what it asks of the keys, an `Order`.
- std.map: the theory `Mapping c` of a map of any kind, with `Key` and `Value` as its fields: `empty`, `insert`, `lookup`, `remove`, `size` and `foldl`, and an instance for std.ordmap, std.hashmap and std.hashtable, so that one definition serves all three and a program picks one by its type.
- check: `within ms t` is now `withinTime ms t`, `within` being co's.
- co: `within h e alg c` runs `c` with what `h` does said within the handler of the rest: `alg` is given that handler and answers in its monad, so no `Co e` stands between `h` and `e` as in `through`. A query handed on through many such layers builds nothing at each (handler_sieve 4.9 s to 0.48 s). The answer is the outer handler's to bind: the same as `through`'s where that handler keeps the monad laws.
- array: `push`, `pop` and `reserve`, and `insertAt` and `removeAt`: pure functions like `set`, changing an array in place where nothing else holds it and copying it first where something does. An array keeps room past its length, doubled when it runs out, so pushes are amortised O(1); `append` grows a unique left array in place. `pop` gives `Maybe (Pair (Array a) a)`. (#115).
- array: `pop` is made of `removeAt`, which now shortens an array nothing else holds in place: a `case` on it where it is called builds neither the `Just` nor the pair. The primitive `arrayPop` is gone.
- std.filesystem: an operation on a path that fails throws why, a `FileError`, over `exception FileError`, instead of giving a `Maybe`, a `Bool` or an `Either`: `readFile` gives the text, `writeFile`, `makeDirectory`, `remove`, `copy`, `move`, `replace`, `readOnly`, `writable`, `setMode` and `makeExecutable` give `Unit`, and `pathStatus`, `linkStatus`, `realPath`, `temporaryDirectory` and `temporaryFile` their value. `readDirectory` gives what the directory holds as paths in it (`dir / name`), sorted. `readLink` gives what a link says, as it was written. `exists`, `isFile`, `isDirectory` and `kindAt` still answer without a throw; an operation on a file open still gives an `Either FileError a`.
- std.stm: software transactional memory on a typed store. The store is one value of a shape the program declares, `(Int, Int, List String)`, and a `TVar s a` is a position in it, std.store's. Transactions (`Co (stm s)`) read, write, `retry`, `orElse` and `check`; `atomically` runs them among green threads, the store the world's state, and `runGreen` runs threads on a store in the language, on every backend. There is no `newTVar`: what grows is a `Map` or a `List` in the shape.
- std.store: positions in a store, shared by std.stm and std.boc. `Position s a` is an `a` in a store of shape `s`, made from its pairs (`whole`, `fstOf`, `sndOf`, `left`, `right`, `first`, `second`, `third`), read with `contents` and written with `replaced`; `both p q` is one position of the pair. `greenWith` runs green threads sharing a state, in the language.
- std.boc: behaviour-oriented concurrency on a typed store. A `Cown s a` is a position; `when (a, b, log) (\(x, y, l) -> ...)`, on a cown or a tuple of them (the theory `Cowns`), spawns a behaviour that, once every earlier behaviour on an overlapping cown is done, is given what its cowns hold and gives back what they hold after. Behaviours are green threads; `runBoc` runs a program and its behaviours on a store, on every backend. Cowns that overlap in one `when` are abandoned at run time.
- `Order` carries its type's `Equality`, the field `eq`: every `Order` instance gives one. `ord.eq.equal`, or `(==) (eq := ord.eq)`, compares by it; `==` still finds only an `Equality` in scope. `orderByCompare` takes the `Equality` as an instance argument.

### Backends and performance

- In `run do`, an `if` or a `case` statement whose arms each reach their end is no join point: each arm gives the variables back to what follows, as a loop does (`MkPair`s). A match's value bound and taken apart at once, every way out building the tag taken apart, written so or by hand, is copied into the arms or, past the simplifier's budget, the names it binds set in each arm (`ESet`), no tag built. A loop whose `if` and the loop called each other last was one `$cycle`, its numbers boxed: 3.7 s to 2.7 s over 300M turns.
- Conditions are a language of their own from the simplifier on (`EBool`: comparisons, `not`, `&&`, `||`, a `Bool` looked at): an `if` is a match on one and builds no `Bool`, and the passes after it take it as it is. The compiler's C 33.61 MB to 33.38 MB.
- In `run do`, a loop is a function of its own that gives its variables back where it ends, what follows it after the call, not a function it calls last: a loop in a loop is a loop of its own, its numbers unboxed, not one function of them all. `matmul` (a `for` nest) 6.7 s to 1.07 s (#113).
- Loop accumulators stay unboxed: a `Float64` total in a `do` loop is boxed once at exit. `doloops_float_do` 7.40 G to 0.42 G instructions.
- `Array.set` in a loop that uses the array linearly checks uniqueness once on entry, then writes in place, so the C compiler vectorises the loop (#102).
- A loop reading and setting arrays at an index checks the bounds once on entry. `saxpy` 0.48 s to 0.30 s (#102).
- `generate` of a lambda giving one fixed-width number builds the array flat, and in-range reads of it are unchecked. `Int64` sum 0.40 s to 0.13 s (#102).
- New numeric array benchmarks: sums, dot product, `saxpy` and `Array.map` over ten million elements (#102).
- In threads mode, a shared object whose count is back to one is reused in place again, as an owned one is. A fold over a shared 16M-node tree: 0.54 s to 0.39 s.
- A stack overflow is an error, not a crash: a C program says `heff: stack overflow` and exits with 2, from any thread heff starts. Where a failure is caught, so is an overflow, as "stack overflow": a std.thread worker hands it to the thread that joins it, and a language server request fails alone. `heff check` on a file nested too deep says so, and the language server reports it on the file and keeps serving. Catching a failure no longer saves the signal mask.
- A program using threads built as parts (`heff build`, `heff compile DIR/`) no longer crashes: every C file now sees threads mode, not only the one with `main`.

### Compiler and tools

- `heff get` takes a package no tag is a version of at a pseudo-version, `0.0.0-TIME-COMMIT`, of its default branch's commit, or with `PATH@BRANCH` or `PATH@COMMIT` of that commit. A pseudo-version is older than every release.

### Docs

- New language reference pages for lazy values and for theories.
- The tutorial's file reading throws, heard in `main` by `attempt`.
- A new example, `examples/fs_tree`: UNIX `tree`, the same output, `-a`, `-d` and `-L`.

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
