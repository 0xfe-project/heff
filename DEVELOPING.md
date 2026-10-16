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
with is `lib/` (its [API](https://dannypsnl.github.io/heff/api/)).

## Building

With [zig](https://ziglang.org/download/) and GNU make (3.81, macOS's,
or newer): building and bootstrapping need nothing else; the tests are
heff's own, run by `heff test`.  Node.js runs the JavaScript some of
the tests check, the benchmarks and the editors' targets.  `make` lists
every target and the variables they take; each runs a step of
`build.zig`, `heff test` on files of `test/`, or a script of `bench/`
or `editors/`, and its line in the `Makefile` says which.

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
build seed`) always starts from the seed.  zig builds each C part
to an object apart -- and the runtime's part in Zig, `heff.zig`, which
the compiler writes beside the C as it bundles the runtime's C into
`heff.h`, by `zig build-obj` -- as many at once as there are cores,
and links them; each object is kept in `.heff/out/heff.o/`, named by
what it is made of (the zig and the machine it builds for, the flags,
`heff.h` and the part), so a part as it was is not built again, whichever stage
it is of, and one no stage of the build is made of is let go.  (zig's
own cache, `.zig-cache/`, holds `scripts/bootstrap.zig` built.)

With Nix, `nix develop` gives a shell with zig 0.17, make and Node.js,
and `nix build` builds heff from the seed into `result/`, laid out as a
release is (its zig, for `heff build`, after any on the `PATH`).

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

`make release` (`zig build release`) makes a release, into
`.heff/out/release/` anew, by zig alone (no Node.js, no system tar or
zip): heff built for each platform, as `make native` builds it, and
packed as `heff-VERSION-PLATFORM.tar.gz` (`.zip` for Windows), the
version `package.hf`'s, with `SHA256SUMS` of them.

| Platform      | zig target           | Archive                           |
| ------------- | -------------------- | --------------------------------- |
| `linux-arm64` | `aarch64-linux-musl` | `heff-VERSION-linux-arm64.tar.gz` |
| `linux-x64`   | `x86_64-linux-musl`  | `heff-VERSION-linux-x64.tar.gz`   |
| `macos-arm64` | `aarch64-macos`      | `heff-VERSION-macos-arm64.tar.gz` |
| `macos-x64`   | `x86_64-macos`       | `heff-VERSION-macos-x64.tar.gz`   |
| `windows-x64` | `x86_64-windows-gnu` | `heff-VERSION-windows-x64.zip`    |

Linux's are linked statically, by musl.  Each archive is one directory,
`heff-VERSION-PLATFORM/`, laid out as `make install` lays out the heff
home, so the heff in it finds its library and its version beside its
`bin/`: `bin/heff` (`bin/heff.exe`), `lib/` and `package.hf` (what
`app/heff/build.hf` ships), with `LICENSE`, `README.md` and `docs/`.
The same source, by the same zig, makes the same archives, byte for
byte: the entries
sorted by path, of no owner, of time 0 (1980-01-01 in a zip, its
first) and of mode 755 or 644.  A tag `vX.Y.Z` pushed to GitHub runs
`.github/workflows/release.yml`, which checks that the tag is the
version, makes the release and the VS Code extension, tries each
archive where there is a runner for it, and publishes them.

`make clean` removes what the builds and the tests made under `.heff/`,
keeping the caches (`vN/`, `bN/`, the parts zig built in `out/heff.o/`
and `out/native.o/`, and those built under ASan in
`ref/native-asan.o/`); it never touches the seed.

## Testing

The tests run on the heff built (`make build` first), or on `HEFF`:

```sh
make test                  # every test, all at once, then the fixpoint
make test RUN=cli/usage    # the tests --run selects, of every file
make test-lsp              # a part: test-compiler, test-lsp,
                           # test-backends, test-asan
make test-fixpoint         # heff compiles itself to the same C (by zig)
make check                 # build-seed, test, fmt-check: all a change is held to
```

`make test` is `heff test` on every file of `test/` (`test/*.hf`),
and the parts are the language server's (`test/lsp.hf`), the
backends' (each backend held to the interpreter, and the example apps),
heff under AddressSanitizer (by clang; `make test-asan JOBS=4
TIMEOUT=120` sets `HEFF_ASAN_JOBS` and `HEFF_ASAN_TIMEOUT`; `heff test
--run asan/check/wire.hf test/asan.hf` checks one program; where there
is no clang with ASan's runtime, it is skipped), and the compiler's,
every other file.  `heff test` runs the integration tests of all the
files it is given in one pool, as many at once as `HEFF_TEST_JOBS`
says, else half the cores; so there is no need for `make -j`, and the
targets run one after another even with it.  Without `RUN`, `make
test` checks the fixpoint after them, as `make test-fixpoint` does.

The backends' tests are `heff test test/backends.hf`, a subtest a
program and a backend (`--run backends/programs/dollar.hf/c`,
`--run backends/c/threads`); `HEFF_GEN="N SEED"` generates other
programs.  `make sanitize` (`HEFF_SANITIZE=1 heff test test/backends.hf`) builds the
backends' C programs under AddressSanitizer, and `make tsan`
(`HEFF_TSAN=1`) runs the threads test under ThreadSanitizer too
(see [std.thread](https://dannypsnl.github.io/heff/api/std/thread/)).  Among the tests, the golden ones hold the
output of `check`, `explain`, `eval` and `type-at` on the library and
test files (`heff test test/golden.hf`, `--run golden/nbe` for those
of `test/nbe`); after an intended change, update them with `make golden`
(`heff test --update test/golden.hf`, `RUN=golden/nbe` for some).  The
surface tests run `heff test` on each `.hf` file of the library, the
compiler, the examples and the tests' directories, a subtest each,
and hold what heff says of a few libraries of their own (`heff test
test/surface.hf`, `--run surface/lib.std` for the files of `lib/std`).
The format tests format each `.hf` file of the repository, and the
cases of `test/format/`, each held to its golden text (`heff test
test/format.hf`, `--run format/cases/spacing` for one;
`heff test --update --run format/cases test/format.hf` after an
intended change).  The cache tests hold the compiler's cache and the
backend's, each scenario in a directory of its own (`heff test
test/cache.hf test/backcache.hf`, `--run cache/instances` for one).
The command line's tests are `heff test test/cli.hf` (`--run
cli/usage` for the usage's), and projects' and packages' `heff test
test/project.hf` (`--run project/get` for `heff get`'s); the
language server's, `heff test test/lsp.hf`, a session with the server
each (`--run lsp/modules`).
The progress tests draw `heff build` on a pseudo-terminal of the tests'
(a small C program, built by the test) and hold what is left on the
screen to what it says without one (`heff test test/progress.hf`); the
terminal tests type keys at its commands' app there (`heff test
test/terminal.hf`).  The dev tests run `heff dev` on a project, ask for
its pages by curl, and change its sources, and so `heff test --watch`
(`heff test test/dev.hf`).

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
run` runs (`run/`), the tests' work directories (`ref/`), and zig's
own cache of the parts it builds (`zig-cache/`, this checkout's alone,
begun afresh each build); and
zig's cache, beside it, `.zig-cache/`, of the build's own program.

The backends' cache keeps, for each module (`bN/M.hb`), what the
backends made of its definitions: each definition simplified, by the
elaboration cache's hash of it (which covers what it uses, so an edit
to a definition that is inlined elsewhere makes those again); the
copies the simplifier made of them, by what each was made of; and each
definition's C and its JavaScript, by a hash of what it was made from.
A program compiled through it is the program compiled without it, byte
for byte (`heff test test/backcache.hf`).  Its files are of the compiler that wrote them:
another compiler (a heff built anew) makes them again, so nothing is to
be bumped when the backend changes.  `bN/compiler` is a copy of that
compiler, by which it knows itself without hashing itself each time.

## The editors

`editors/vscode` is a VS Code extension for the language server
([its README](editors/vscode/README.md) says how to run it from the
repository); `make vsix` packages it.  `editors/bat` is its grammar for
bat, made from the VS Code grammar ([its README](editors/bat/README.md)):
`make bat` makes it again, and `make bat-check` fails where it is not
made from the grammar as it is.
