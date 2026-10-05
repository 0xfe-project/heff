# heff

heff is an implementation of Hippogriff (Lynch & Staton,
arXiv:2608.19728), a typed language with a phase distinction, with a
Haskell-like syntax, modules, doctests, algebraic effects, a formatter, a
language server, and a compiler to JavaScript for web pages and Node.js.

```haskell
import prelude

map : {a b : Type} -> (a -> b) -> List a -> List b
map f Nil = Nil
map f (Cons y ys) = Cons (f y) (map f ys)

-- >>> map not [True, False]
-- Cons False (Cons True Nil)
```

## Installing

```sh
npm install       # also builds heff
npm link          # puts `heff` on the PATH
```

heff is written in heff (`compiler/`).  `boot/heff.mjs` is a compiled
copy of the compiler; the build (`npm run build`) uses it to compile the
compiler into `output/heff.mjs`, which is `heff` (minified; the
compiler's own output is `output/heff.raw.mjs`).

`editors/vscode` is a VS Code extension for the language server.

## Commands

```sh
heff check FILE                # the file's interface, and its errors
heff eval FILE                 # the value of each definition
heff run FILE                  # the value of each watch (`-- >>> e`)
heff test FILE...              # check the watches' expectations
heff promote FILE              # write each watch's value as its expectation
heff fmt [--check] FILE...     # format in place
heff compile-js FILE OUT.js    # an ES module
heff app FILE [NAME] OUT.js    # a script running an app in a web page
heff node FILE [NAME] OUT.mjs  # a Node.js program
heff lsp                       # the language server, on stdio
heff completions fish          # fish completions
```

## Modules

A module is a file, named by its path from the project root:
`parse/surface.hf` is `parse.surface`.  The project root is the nearest
directory with a `package.json` that has a `"heff"` field.  Other modules
are looked up in `lib/`, then in `node_modules/`.  Compiled modules are
cached in `.heff/`.

## Effects

`lib/co.hf` implements effects after Yang and Wu (POPL 2026).  A program
names the effects it needs as instance arguments and is given them where
it runs:

```haskell
import co
import fs
import prelude
import stdio

world : HFunctor
world = stdio <+> fs

program : {{io : stdio <: world}} -> {{disk : fs <: world}} -> Co world Unit
program = do
  ok <- writeFile "hello.txt" "hello"
  print (if ok then "written" else "not written")

main : Co world Unit
main = program (io := \p -> InL p) (disk := \p -> InR p)
```

`lib/` has effects for the standard streams, the process, files, time,
the network and web pages; each one's JavaScript side is in
`M.provider.js` beside `M.hf`.  `examples/` has apps for the browser
(`npm run examples`).

## Developing

`npm test` builds the compiler and runs the tests.  Among them,
`test/golden/` holds the output of `check`, `explain`, `eval` and
`type-at` on the library and test files; after an intended change,
update it with `node test/golden.mjs --update`.

`npm run boot` replaces `boot/heff.mjs` with the current compiler, once it
compiles itself to an identical file.  This is needed only when a
language change means the old copy can no longer compile the source.
