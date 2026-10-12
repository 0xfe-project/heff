# Developing heff

## The source

heff is written in heff.  The package's modules (`src/heff/`) are the
compiler as a library, in a directory each by what they are for:

- `syntax/` reads a file and lays it out again (`fmt/`, the formatter's);
- `elab/` elaborates it;
- `compile/` erases, lowers and emits JavaScript and C;
- `build/` loads a project, keeps the cache and does each command's
  work (`heff.build.commands`);
- `lsp/` is the language server and how diagnostics and test reports
  are shown;
- `util/` holds what is of no one of them.

The program `heff` is `app/heff/main.hf`, which reads the command line
and gives the commands the world they run in.  The library it ships
with is `lib/` ([library.md](library.md)).

## Building

With [zig](https://ziglang.org/download/) and GNU make (3.81, macOS's,
or newer): building and bootstrapping need nothing else.  The tests,
the benchmarks and the editors' targets need Node.js too.  `make`
lists every target and the variables they take; each runs a step of
`build.zig` or a script of `test/`, `bench/` or `editors/`, and its
line in the `Makefile` says which.

```sh
make build         # .heff/out/heff
make install       # ~/.heff/bin/heff, with its library
```

The build (`zig build`, which runs `scripts/bootstrap.zig`) compiles
the program to C with the heff built before (`.heff/out/heff`), and
`zig cc` builds it into `.heff/out/heff`.  Where there is no heff yet,
or only one of another seed, it starts from the seed,
`boot/heff.tar.xz`: heff compiled by itself to C, which zig unpacks
and builds for whatever machine it is on.  `make build-seed` (`zig
build seed`) always starts from the seed.  zig keeps each C part's
object in its cache, `.zig-cache/`, and builds again only the parts
that changed, as many at once as there are cores; it knows a part by
its path and its time, so a part written anew (as in a fresh checkout)
is built again.  `.zig-cache/` only grows: remove it when it is too
big.

`make install` installs into `~/.heff` whatever `HEFF_HOME` says (to
install elsewhere, `HEFF_HOME=DIR .heff/out/heff install`); `make
install-all` installs the VS Code extension too (`make vsix`, then
`code --install-extension`) and builds bat's cache again.

`make boot` (`zig build boot`) replaces the seed with the current
compiler's C, once it compiles itself to the same C (`make
test-fixpoint`, `zig build fixpoint`).  The seed is a tar of the C
files by name, of no owner and of time 0, compressed by `xz` (which it
needs), so the same C makes the same seed.  This is needed only when a
language change means the old seed can no longer compile the source.

`make native TARGETS="x86_64-linux-musl aarch64-macos"` (`zig build
native -Dtargets=...`) builds heff for other systems, as a release
does, into `.heff/out/native/TARGET/`.

`make clean` removes what the builds and the tests made under `.heff/`,
keeping the caches (`vN/`, `bN/`, the parts built under ASan in
`ref/native-asan.o/`, and zig's, `.zig-cache/`); it never touches the
seed.

## Testing

The tests run on the heff built (`make build` first), or on `HEFF`:

```sh
make test                  # every group, all at once, then the fixpoint
make test GROUPS="compiler lsp"
make test-lsp              # one group: test-compiler, test-lsp,
                           # test-backends, test-asan
make test-fixpoint         # heff compiles itself to the same C (by zig)
make check                 # build-seed, test, fmt-check: all a change is held to
```

`make test` is `node test/run.mjs [GROUP...]`, and the groups are
`compiler`, `lsp`, `backends` (each backend held to the interpreter)
and `asan` (heff under AddressSanitizer, by clang; `make test-asan JOBS=4 TIMEOUT=120` sets
`HEFF_ASAN_JOBS` and `HEFF_ASAN_TIMEOUT`).  The scripts run at once,
and what they run shares one budget, as make's jobserver does: as many
processes at once as there are cores, or `HEFF_TEST_JOBS` (a script
run by itself has its own pools).  So there is no need for `make -j`,
and the targets run one after another even with it.  With no group
given, `make test` checks the fixpoint after them, as `make
test-fixpoint` does, which needs no Node.js.

`make sanitize` (`HEFF_SANITIZE=1 node test/backends.mjs`) builds the
backends' C programs under AddressSanitizer, and `make tsan`
(`HEFF_TSAN=1`) runs the threads test under ThreadSanitizer too
([threads.md](threads.md)).  Among the tests, `test/golden/` holds the
output of `check`, `explain`, `eval` and `type-at` on the library and
test files; after an intended change, update it with `make golden`
(`node test/golden.mjs --update`).

`make bench` (`node bench/run.mjs`, `BENCH="--runs 5 pure"` for its
arguments) times the benchmarks of `bench/`.

## Formatting

`make fmt` lays out the repository's `.hf` files as `heff fmt` does,
and `make fmt-check` names those that are not (the tests' files are
left as they are, as `.pre-commit-config.yaml` leaves them).

`make aliases` (`heff run scripts/aliases.hf [--check] [FILE...]`)
gives each `import m as X` the alias its table says, a word that names
the module (`Driver`, not `Dr`), and every `X.y` of the file with it;
`--check` only says what it would change.  A new module's alias goes in
the table.

## What is made

All that is made is under `.heff/`: the compiler built (`out/`), the
cache of compiled modules (`vN/`), the backends' cache (`bN/`), the
programs `heff build` makes (`bin/`, their C in `build/`), what `heff
run` runs (`run/`), the example apps (`examples/`), and the tests' work
directories (`ref/`); and zig's cache, beside it, `.zig-cache/`.

The backends' cache keeps, for each module (`bN/M.hb`), what the
backends made of its definitions: each definition simplified, by the
elaboration cache's hash of it (which covers what it uses, so an edit
to a definition that is inlined elsewhere makes those again); the
copies the simplifier made of them, by what each was made of; and each
definition's C and its JavaScript, by a hash of what it was made from.
A program compiled through it is the program compiled without it, byte
for byte (`test/backcache.mjs`).  Its files are of the compiler that wrote them:
another compiler (a heff built anew) makes them again, so nothing is to
be bumped when the backend changes.  `bN/compiler` is a copy of that
compiler, by which it knows itself without hashing itself each time.

## The editors

`editors/vscode` is a VS Code extension for the language server
([its README](../editors/vscode/README.md) says how to run it from the
repository); `make vsix` packages it.  `editors/bat` is its grammar for
bat, made from the VS Code grammar ([its README](../editors/bat/README.md)):
`make bat` makes it again, and `make bat-check` fails where it is not
made from the grammar as it is.
