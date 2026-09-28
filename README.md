# heff

heff is a language and its toolchain: Hippogriff (Lynch & Staton,
arXiv:2608.19728), a dependently typed language with a phase distinction,
with a Haskell-like syntax, modules, doctests, effects and handlers, a
formatter, a language server, and a compiler to JavaScript -- for web
pages and for Node.js.

```haskell
import co

data List a = Nil | Cons a (List a)

map :: {a b :: Type} -> (a -> b) -> List a -> List b   -- {…}: implicit
map f Nil = Nil
map f (Cons y ys) = Cons (f y) (map f ys)

theory Monoid where
  t :: Type
  m :: t -> t -> t

foreign import intAdd :: Int -> Int -> Int              -- a primitive

-- >>> map not (Cons False Nil)
-- Cons True Nil
```

## Installing

```sh
npm install
npx spago build
npm link          # puts `heff` on the PATH
```

For VS Code, `editors/vscode` is an extension: highlighting, and the
language server (diagnostics, hover, go to definition, formatting).

## Commands

```sh
heff check FILE                # the types of FILE's definitions
heff eval FILE                 # what each definition evaluates to
heff run FILE                  # what each watch evaluates to
heff test FILE...              # do the watches' expectations hold?
heff promote FILE              # write each watch's value under it
heff fmt [--check] FILE...     # format in place
heff compile-js FILE OUT.js    # an ES module of FILE's definitions
heff app FILE [NAME] OUT.js    # a script running an app in a web page
heff node FILE [NAME] OUT.mjs  # a Node.js program
heff lsp                       # the language server, on stdio
heff completions fish          # > ~/.config/fish/completions/heff.fish
```

## The language

`::` annotates, everywhere; a capitalised name is a constructor unless it
names a definition or a binder; `f (a := T)` gives an implicit argument by
name; `case`/`of`, `if`, `do`, `\x -> e`, records `{ x = 1 }` and record
types `{ x :: Int }`.  A function is defined by equations, each a case; a
call of itself is recursion.

A `-- >>> e` line is a watch, and the `--` lines under it are its
expectation: a doctest, checked by `heff test`.  `test name` names the
watches after it.

Operators are declared, and bind relatively -- no numbers, only which
binds tighter than which:

```haskell
infixl + - above ==
infixl * / above + -
infixr ++ above ==

(+) :: Int -> Int -> Int
(+) = intAdd
```

Operators of unrelated groups do not go together without parentheses:
`a + b ++ c` is refused.

Evaluation is call-by-value: arguments, and the fields of constructors and
records, are computed before they are passed, used or not; a definition
that is not a function is computed when first used.  `Int` is an integer
of any size; division is floor division, and by zero it abandons.

## Modules

A module is a file, named by its path from the project's root:
`parse/surface.hf` is `parse.surface`.  The root is the nearest directory
above the file with a `project.hf` (a module like any other: the
project's `name`), else the file's own directory.  What is not in the
project is looked for in the standard library, `lib/`.

- Imports come first in a file, one per line: `import dom`.
- `import dom as D` brings `dom`'s definitions in as `D.sleep`, `D.Node`,
  … only.  (A constructor is not qualified: `True` is found by its type.)
- A file sees its own definitions and those of the modules it imports --
  not what those import.
- A name defined by two modules imported by name is an error: import one
  of them `as`.  A file's own definition shadows an imported one.
- Everything a module defines is exported.

A `theory` is a first-class module (a record type), and an instance of it
is a record.

## Effects

`lib/co.hf` has computations with effects, after Yang and Wu, "Handling
Higher-Order Effectful Operations with Judgemental Monadic Laws" (POPL
2026): an effect is a higher-order functor, a handler is a monad with an
algebra (`Handler h`), and a computation `Co h a` runs under any handler of
its effect.  `do` is `bind`.  `h <+> k` is two effects as one.

- `lib/dom.hf`: a web page (create, text, attr, append, on, select,
  xpath, sleep, fetch), with `lib/state.hf`: an app is a
  `Co (dom <+> (state s)) Unit`, and `heff app` writes it as a script for
  a page.  Where it goes on the page is the app's to find, by `select` or
  `xpath`.
- `lib/io.hf`: a program's world (standard output and input, arguments,
  environment, files, time, the network, exit): a program is a
  `Co io Unit`, and `heff node` writes it as a Node.js program.

The page's handler and Node's are written in JavaScript; asynchrony is
the handler's, and the program is plain `do`.  The same computation runs
under a handler written in heff (`test/surface/dom-trace.hf`).

`examples/counter`, `examples/clock` and `examples/greet` are apps:
`npm run examples` builds them, then open their `index.html`.

## Developing

`npm test` runs the tests.
