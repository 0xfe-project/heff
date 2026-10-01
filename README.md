# heff

heff is a language and its toolchain: Hippogriff (Lynch & Staton,
arXiv:2608.19728), a typed language with a phase distinction -- types are
computed in the static phase, never from a runtime value -- with a
Haskell-like syntax, modules, doctests, effects and handlers, a formatter,
a language server, and a compiler to JavaScript -- for web pages and for
Node.js.

```haskell
import co

data List a = Nil | Cons a (List a)

map : {a b : Type} -> (a -> b) -> List a -> List b   -- {…}: implicit
map f Nil = Nil
map f (Cons y ys) = Cons (f y) (map f ys)

theory Monoid where
  t : Type
  m : t -> t -> t

foreign import intAdd : Int -> Int -> Int              -- a primitive

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
heff check FILE                # FILE's interface: its types in full, its values' signatures
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

Every command elaborates the file given and what it imports.  An imported
module is thawed from the cache the compiler keeps at the project root
(`.heff/`, safe to delete, worth ignoring in git) instead of elaborated
again, when nothing it is made of has changed: its source, the compiler,
each definition it mentions from other modules (hashed transitively, so a
change to a body no one mentions, or to comments, invalidates no one), and
what its parse takes from each direct import (the names defined, the
fixities declared).

## The language

`:` annotates, everywhere; `x :: xs` is `Cons x xs` (in patterns too), right, below the arithmetic and `++`; a capitalised name is a constructor unless it
names a definition or a binder; `f (a := T)` gives an implicit argument by
name; `case`/`of`, `if`, `do`, `\x -> e`, records `{ x = 1 }` and record
types `{ x : Int }`, lists `[1, 2, 3]` (`Cons 1 (Cons 2 (Cons 3 Nil))`,
in patterns too).  `\(x : T) -> e` annotates a lambda's binder: the
lambda then says its own type, where an argument of `map` would
otherwise be checked before the type is known (a projection `x.field`
there needs it -- the error says so).  A function is defined by equations, each a case; a
call of itself is recursion.  A pattern may be a literal, `fib 0 = 0`,
compared by the equality in scope (`intEq`, `strEq`, `floatEq`); one of
the equations must then match every other value.  Functions that call one
another are fields of one record.

A `-- >>> e` line is a watch, and the `--` lines under it are its
expectation: a doctest, checked by `heff test`.  `test name` names the
watches after it.

Operators are declared, and bind relatively -- no numbers, only which
binds tighter than which:

```haskell
infixl + - above ==
infixl * / above + -
infixr ++ above ==

(+) : Int -> Int -> Int
(+) = intAdd
```

Operators of unrelated groups do not go together without parentheses:
`a + b ++ c` is refused.

`a && b` and `a || b` are `if`: the right side is computed only if it is
needed (`False && e` is `False`, whatever `e`), and a call of itself
there is a tail call.  `(&&)` written alone is still the prelude's
function, for `foldl (&&) True`.

Evaluation is call-by-value: arguments, and the fields of constructors and
records, are computed before they are passed, used or not; a definition
that is not a function is computed when first used.  `Int` is an integer
of any size; division is floor division, and by zero it abandons.  `Float`
is a 64-bit floating-point number (`1.5`, `6.02e23`), its operators the
integers' with a `.`: `+.`, `*.`, `<.`.  `Array a` is an array, each
element reached at once by its index, and never changed (`set` makes
another): `lib/array.hf`, imported as `import array as A`.

`lib/prelude.hf` has `Bool`, `Maybe`, `Either`, pairs and lists, the
arithmetic, and the functions on lists (`map`, `foldl`, `foldr`,
`filter`, `take`, `zip`, `range`, `sortBy`, …) and on text (`split`,
`lines`, `words`, `trim`, `indexOf`, `parseInt`, …).  There are no type
classes: what needs an equality or an order is given one (`elemBy`,
`sortBy`, `compareInt`).  `lib/map.hf` and `lib/set.hf` are maps and sets
by an order of their keys (balanced trees), to be imported by a name:
`import map as M`, then `M.insert compareString "k" 1 m`.

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
its effect.  `do` is `bind`.  `h <+> k` is two effects as one, and
`h <: e` is the evidence that `h`'s operations are among `e`'s -- which
world an operation belongs to is nowhere in its type, so the evidence is
the program saying it; a computation polymorphic in its effect takes the
evidence it needs (`{e : HFunctor} -> (stdio <: e) -> ...`).  `through`
handles the left effect of a sum and no more: the rest is a computation
again, for whoever handles it.

- `lib/dom.hf`: a web page (create, text, attr, append, on, select,
  xpath, sleep, fetch), with `lib/state.hf`: an app is a
  `Co (dom <+> (state s)) Unit`, and `heff app` writes it as a script for
  a page.  Where it goes on the page is the app's to find, by `select` or
  `xpath`.
- `lib/stdio.hf`, `lib/proc.hf`, `lib/fs.hf`, `lib/time.hf`,
  `lib/net.hf`: a program's worlds, one effect each -- the standard
  streams, the process (arguments, environment, exit), files, time, the
  network.  A program sums the ones it lives in, `stdio <+> fs`, and
  lifts each computation in by its evidence (`lift`, `inl`, `inr`:
  values of `<:`, lib/co.hf); `heff node` writes it as a Node.js
  program -- one monad, an algebra per effect.

The page's handler and Node's are written in JavaScript; asynchrony is
the handler's, and the program is plain `do`.  The same computation runs
under a handler written in heff (`test/surface/dom-trace.hf`).

`examples/counter`, `examples/clock` and `examples/greet` are apps:
`npm run examples` builds them, then open their `index.html`.

## Developing

`npm test` runs the tests.

The formatter is written in heff itself (`fmt/`): `heff fmt` and the
language server compile it -- with heff -- when it is missing or stale
(`output/heff-fmt.mjs`), and lay files out through it.  The layout in
`Heff.Format` stays as the reference: `test/fmt-port.mjs` holds the two
byte for byte the same over every `.hf` file of the repository.
