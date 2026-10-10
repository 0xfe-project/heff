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

With Node.js and [zig](https://ziglang.org/download/):

```sh
node scripts/build.mjs     # .heff/out/heff
.heff/out/heff install     # ~/.heff/bin/heff (HEFF_HOME/bin/heff)
```

The build compiles the program to C with the heff built before
(`.heff/out/heff`), and `zig cc` builds it into `.heff/out/heff`.
Where there is no heff yet, or only one of another seed, it starts from
the seed, `boot/heff.tar.xz`: heff compiled by itself to C, which zig
builds for whatever machine it is on.

`node scripts/boot.mjs` replaces the seed with the current compiler's
C, once it compiles itself to the same C.  This is needed only when a
language change means the old seed can no longer compile the source.

## Testing

`node test/run.mjs [GROUP...]` runs the tests on `.heff/out/heff` (or on
`HEFF`), by group: `compiler`, `lsp`, `backends` (each backend held to
the interpreter), `fixpoint` (heff compiles itself to the same C) and
`asan` (heff under AddressSanitizer).  `HEFF_SANITIZE=1` builds the
backends' C programs under AddressSanitizer, and `HEFF_TSAN=1` runs the
threads test under ThreadSanitizer too ([threads.md](threads.md)).  Among them, `test/golden/` holds
the output of `check`, `explain`, `eval` and `type-at` on the library
and test files; after an intended change, update it with
`node test/golden.mjs --update`.

## What is made

All that is made is under `.heff/`: the compiler built (`out/`), the
cache of compiled modules (`vN/`), the C backend's cache (`bN/`), the
programs `heff build` makes (`bin/`, their C in `build/`), what `heff
run` runs (`run/`), the example apps (`examples/`), and the tests' work
directories (`ref/`).

The C backend's cache keeps, for each module (`bN/M.hb`), what the
backend made of its definitions: each definition simplified, by the
elaboration cache's hash of it (which covers what it uses, so an edit
to a definition that is inlined elsewhere makes those again); the
copies the simplifier made of them, by what each was made of; and each
definition's C, by a hash of what it was made from.  A program compiled
through it is the C compiled without it, byte for byte
(`test/backcache.mjs`).  Its files are of the compiler that wrote them:
another compiler (a heff built anew) makes them again, so nothing is to
be bumped when the backend changes.  `bN/compiler` is a copy of that
compiler, by which it knows itself without hashing itself each time.

## The editors

`editors/vscode` is a VS Code extension for the language server
([its README](../editors/vscode/README.md) says how to run it from the
repository); `editors/bat` is its grammar for bat, made from the VS
Code grammar ([its README](../editors/bat/README.md)).
