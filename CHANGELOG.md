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

- `parseFloat : String -> Maybe Float64` (the prelude's) and float32's `parseFloat32` read a decimal, correctly rounded, by the runtime: zig's `std.fmt.parseFloat` in C, `Number` in JavaScript (and, for a `Float32` where rounding twice could differ, an exact comparison). json's `parse` reads its floats so: 3 million floats of 17 digits 1.20 s to 0.23 s in C, 8.0 s to 1.8 s in JavaScript.
- A float literal at `Float32` is rounded once, to the `Float32` nearest the decimal it writes, not first to a `Float64`: `(1.0000000596046448 : Float32)` is 1.0000001, not 1.0. float32's `fromDecimal` rounds so, as std.math's `fromDecimal` does to a `Float64`.
- `show` of a `Float64` or a `Float32` is a literal heff reads back as the same float, still the fewest digits: an exponent has a point before it and no `+` (`1.0e23`, `5.0e-324`, `1.5e300`, not `1e+23`, `5e-324`, `1.5e+300`), and -0.0 keeps its sign (`-0.0`, not `0.0`). `NaN`, `Infinity` and `-Infinity` are as before. json's `stringify` writes numbers as before, as JavaScript does.
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
- green is threads alone: `spawn`, `yield`, and `newKey`, `park k` and `wake k`, a key's wake-ups kept in order until a thread parks there. Its channels of `Int`s are those keys, their wake-ups carrying the values, as before; std.stm and std.boc park and wake. `together c` schedules `c` joining every thread: main's answer once all have ended, `Nothing` where every one left is parked (`scheduled` still drops them when main ends); std.reactor's `together` likewise.
- std.chan: typed channels for green threads, a channel of any `a` held by a `with` of its own, `with Chan.channel as ch do` (or `with Chan.channels as cs do`, then `Chan.newChan cs`), outside the scheduler, `together` inside the innermost. Its effect, `chan s a`, keeps the values in its handler's own state, so `send`, `recv` and `tryRecv` are typed with no store shape; several element types are several `with`s, `green <+> chan t b <+> chan s a <+> e`. `recv` parks on green while the channel is empty, and a value goes to the receiver waiting longest. The `with`'s region `s` keeps the channel, and every thread using it, inside it; `together` ends it only once they have ended. On C, 1000 messages: ping-pong 0.87 ms (Int channels 0.16 ms), producer and consumer 0.19 ms (0.06 ms).
- std.reactor: a thread's outside operation is begun and ended by first-order operations, `Begin` and `End`, run in the world's own course, not held by a `Start` that held the computation. A stateful handler between the scheduler and the world (std.chan's, `runState`) kept nothing an outside operation wrote: it ran the computation an operation held from its state of the time and dropped what it left. It now sees each operation as any other.
- `Order` carries its type's `Equality`, the field `eq`: every `Order` instance gives one. `ord.eq.equal`, or `(==) (eq := ord.eq)`, compares by it; `==` still finds only an `Equality` in scope. `orderByCompare` takes the `Equality` as an instance argument.
- std.time: two clocks, both in nanoseconds (as `Status.modified`): `now`, the time of day since the epoch, and `monotonic`, for elapsed time. C reads `clock_gettime` (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`), JavaScript `Date.now` and `performance.now`.
- std.filesystem: `readBytes` gives a file's `Bytes`, as `readFile` gives its text, failing the same way.
- std.io: `flush` writes what the output has kept: to a file or a pipe, the output is written a buffer at a time, so a long-running program's `print` showed only at its end. `flush` is a new operation of `stdio` (`Flush x`): a handler of `stdio` written in the language needs a case for it. Bytes already reach the output unchanged by `write (Bytes.asString b)` or std.filesystem's `withStdout` and `writeBytes`; `Bytes.toString` (std.http's `text`) is what turns bytes that are not UTF-8 into U+FFFD.
- Time limits on what waits. std.net: `withConnection`, `read` and `write` take a `timeout` in milliseconds, by name (`Net.read (timeout := 500) c 4096`); where it passes first the operation fails `ETIMEDOUT`, a connection not made closed, a stream still usable. 0, the default, waits as before. The limit is kept with std.reactor's scheduler or without one, by the poll the operation waits in; without one, a host's name is resolved by getaddrinfo, not cut short. std.http: `Request` has `connectTimeout`, how long the connection may take to be made (libcurl's `CONNECTTIMEOUT_MS`, the spawned curl's `--connect-timeout`), apart from `timeout`, the whole transfer's; either is curl's code 28. std.http.server: `serve`, `serveUntil`, `serveConnection` and `serveConcurrently` take an `idle` limit by name, 30 s by default (0: none); a client that sends nothing for that long, between requests or within one, is closed.
- std's errors say what they are of. std.filesystem's `FileError` has the `path` the failing call was given, as the program wrote it -- in a tree (`copy`, `remove`, `readOnly`) the one inside it, of an open file the path it was opened by -- and, where the call took two (`move` and `replace`'s rename), the `other`; `showFileError` writes `path: NAME: message`, or `path -> other: NAME: message`. std.process's `NotRun` carries a `ProcessError`, which has a `path` too: the command, or the `dir` where that was not there (`showProcessError`). std.net's `NetError` has the `address` (`host:port` as given, or an accepted connection's other side), std.http's and browser.http's `HttpError` the `url`, and their `showError` says them. json's `ParseError` is a record of the `offset`, the `line` and the `column` (in characters) and what was `expected`; `describe`, and its `toString`, says `line 2, column 8: expected a value`. heff's own messages say them: a program not run, a test's directory not made, a `zig` or `git` not there.

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
- The continuations a recursive copy is handed by its own group are tags, not closures, where every one that may reach a call is known and goes nowhere else: their free variables the fields, a call of one a call of a function choosing its body by the tag (defunctionalization, within the group alone), a call of itself last a loop. tree_explore 4.4 s to 3.3 s.
- A rune and its width read at the same byte, as `runeAt s i` reads them, are decoded once, by a primitive giving both; an ASCII byte is read where it is, not by a call. In C, str_runes 17.8 G to 9.8 G instructions, graphemes 25.3 G to 23.3 G; in JavaScript, str_runes 7.2 s to 6.0 s. `ofRunes` counts its bytes without encoding each rune twice.

### Compiler and tools

- The language server goes to a local's definition, its binder, and highlights a name's occurrences in the file. References and renaming elaborate only the files whose text has the name, the rest thawed from the cache, and keep what they elaborated until it changes: references of `IR.Expr` in heff's own project take 1.8 s the first time and 0.7 s after an edit, not 10 s each time; a local's references are found in its file alone. A module claim's entry is a reference too, and is renamed.
- `heff get` takes a package no tag is a version of at a pseudo-version, `0.0.0-TIME-COMMIT`, of its default branch's commit, or with `PATH@BRANCH` or `PATH@COMMIT` of that commit. A pseudo-version is older than every release.
- `heff compile --backend=c` without `--name` runs `main`, as `--backend=page` now does too, not each definition shown nor the first computation that fits; a file without the definition is refused, naming it. OUT in a directory that is not there is refused, `heff compile: DIR: no such directory`, for every backend, and a file `compile`, `promote` or `fmt` cannot write is said, not an exit 1 with no word. The tests' C program of each definition is `heff dump-c FILE OUT`.

### Docs
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
