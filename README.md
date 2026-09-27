# heff

heff is Hippogriff (Lynch & Staton, arXiv:2608.19728), ported from the reference
Haskell implementation to PureScript and checked against it by differential
testing, with a text-based toolchain: files, modules, a checker, doctests,
an interpreter, and a compiler to JavaScript.

Extracted from `hippogriff-lab`, where the same language lived in a
content-addressed codebase with a card-based IDE and a HOTT₁ theorem prover.
The lab keeps those; this is the language on its own.

```haskell
import co

data List a = Nil | Cons a (List a)

map :: {a b :: Type} -> (a -> b) -> List a -> List b   -- {…}: implicit
map f Nil = Nil
map f (Cons y ys) = Cons (f y) (map f ys)              -- no `rec` needed

theory Monoid where
  t :: Type
  m :: t -> t -> t

foreign import intAdd :: Int -> Int -> Int              -- a primitive

-- >>> map not (Cons False Nil)
-- Cons True Nil
```

`::` annotates, everywhere; a capitalised name is a constructor unless it
names a definition or a binder; `f (a := T)` gives an implicit argument by
name; `case`/`of`, `if`, `do`, `\x -> e`, records `{ x = 1 }` and record
types `{ x :: Int }`.  A `-- >>> e` line is a watch, and the `--` lines
under it are its expectation (a doctest); `test name` names the watches
after it.  The reference's syntax (fnotation, `.hpgf`) is still read, for
comparing with it.

Operators are declared, and bind relatively: no numbers, only which binds
tighter than which.

```haskell
infixl + - above ==
infixl * / above + -
infixr ++ above ==

(+) :: Int -> Int -> Int
(+) = intAdd
```

An operator's declaration makes a group, of one associativity; `above` and
`below` relate it to the groups of other operators, transitively.  Two
operators of unrelated groups do not go together without parentheses: with
the prelude's, `a + b ++ c` is refused.  `a + b` is `(+) a b`; fixities are
seen by the files that import the module declaring them.  `lib/prelude.hf`
has the arithmetic, comparisons, `&&`, `||`, `++` and `$`, with `Maybe`,
`Either`, lists and their functions.

## The stages

```
text ─[Hg.Parse]→ notation ─[Hg.Elab]→ core ─┬─[Hg.Eval]→ values
                                             └─[Hg.Lower]→ IR ─[Hg.Compile.JavaScript]→ JavaScript
```

| path | stage |
| --- | --- |
| `src/Hg/Parse` | text → notation: `Surface` (`.hf`), `FNotation` and `Config` (the reference's `.hpgf`), `Import` (a file's header) |
| `src/Hg/Elab` | notation → core: `Top` (declarations), `Clauses` (equations → `match`), `Rules`, `Unify` (implicit arguments), `Environment`, `Prims` |
| `src/Hg/Core` | the core: syntax, values (normalisation by evaluation), conversion, readback, the reference's printer |
| `src/Hg/Print` | core → `.hf` text (`Surface`, `Resugar`: `match`es back to equations) |
| `src/Hg/Eval` | the interpreter: by value (heff's), or by need (the reference's) |
| `src/Hg/Lower` | core → an untyped IR, and computing on it: `Erase` (types, with the parameters that take them and the arguments given for them, read off the types with the elaborator's NbE), `Shake` (only what is reached), `Simplify` (β, inlining what is not recursive, known fields and tags), `Arity` (a definition of n parameters called with n arguments at once) |
| `src/Hg/Compile` | `JavaScript` (the IR emitted as an ES module; runtimes for a page's apps and for Node.js programs) |
| `src/Hg/Support` | pretty printing, diagnostics, big integers |
| `src/Hg/Format.purs` | the formatter: `.hf` read again keeping every token and comment, laid out anew; refused if the result would not read as the same notation with the same comments |
| `src/Hg/Lsp.purs` | the language server: diagnostics, hover, go to definition, formatting |
| `src/Hg/Load.purs`, `src/Hg/Driver.purs`, `src/Main.purs` | finding a program's files; running the stages over them; the CLI |

## Modules

A module is a file, named by its path from the project's root:
`parse/surface.hf` is `parse.surface`.  The root is the nearest directory
above the file given that has a `project.hf` (a module like any other,
`project`: the project's `name`), else the file's own directory.
A module not in the project is looked for in `lib/` (the standard library:
`prelude`, `co`, `dom`).

```haskell
import co
import dom
```

- Imports come first in a file (after comments), one per line.
- A file sees its own definitions and those of the modules it imports --
  not what those import.  Import what you use.
- A name defined by two modules imported (differently) is an error; a
  file's own definition shadows an imported one.
- Everything a module defines is exported.
- Import cycles are errors.

Inside, a module's definition `x` is stored under `module.x`, and a file
reaches the names in its scope through aliases; messages and types print
the names as the file has them.  Primitives (`foreign import`) are the
runtime's, and the same wherever they are declared.

There are no parametrised modules: a `theory` is already a first-class
module (a record type), and an instance is a record.

## Using it

```sh
npm install
npx spago build

heff check FILE              # elaborate FILE and what it imports; its definitions' types
heff run FILE                # what each watch evaluates to
heff test FILE...            # do the expectations hold?
heff promote FILE            # write each watch's value under it
heff compile-js FILE OUT.js  # an ES module; its default export has FILE's definitions
heff app FILE [NAME] OUT.js  # a script running an app (lib/dom.hf) in a page
heff node FILE [NAME] OUT.mjs  # a Node.js program running NAME (lib/io.hf)
heff explain FILE            # the elaboration trace: metavariables, unifications, delays
heff type-at FILE OFFSET     # what the name there resolved to, and its type
heff golden [--tools] FILE   # the reference's golden report (.hpgf)
heff ref FILE                # what the reference executable prints (.hpgf)
heff fmt [--check] FILE...   # format in place (--check: list what is not formatted)
heff lsp                     # a language server on stdio (editors/vscode is a client)
heff completions fish        # heff completions fish > ~/.config/fish/completions/hg.fish

npm test
```

(`heff` is `node bin/heff.mjs`, or `npm link` it.)

The differential fuzzer (`npm run fuzz`) needs the reference
implementation: build it with GHC ≥ 9.12 and put the path of the
`hippogriff` executable in `ref/REF_EXE`.

## Compiling

`heff compile-js` and `heff app` compile, not transliterate: what is a type is
gone, and what is known at compile time is computed then.  `examples/clock/clock.hf`

```haskell
tick :: Int -> Int -> Co (dom <+> (state Int)) Unit
tick label n = do
  sleep 100
  text label (showInt n ++ " ticks")
  tick label (n + 1)
```

becomes (the handler `hd` is the page's, given at run time; `Co`'s `bind`,
the operations and the effect's `hmap` are computed away)

```js
const tick = (label, n, hd, k) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Sleep", a: [100n, { $: "MkUnit", a: [] }] }] }))((x) => {
  const str = n.toString() + " ticks";
  return hd.bind(hd.malg({ $: "InL", a: [{ $: "Text", a: [label, str, { $: "MkUnit", a: [] }] }] }))((x1) => tick(label, n + 1n, hd, k));
});
```

## Tests

- `test/golden.sh`: the reference's golden tests, byte for byte.
- `test/surface.mjs`: the library, examples and `test/surface`, `test/modules`
  elaborate and their expectations hold; the module rules reject what they
  should.
- `test/app-run.mjs`: the example apps and `test/apps`, compiled and run on
  a pretend page.
- `test/node-run.mjs`: `test/node` compiled for Node.js and run.
- `test/compile-check-js.mjs`: the golden corpus,
  `test/programs` and generated programs, compiled and run; each value must
  be the interpreter's (by value): the same value, or both abandon.
- `test/fuzz.mjs`: generated programs, against the reference executable.

## Deviations from the reference implementation

Each is either an extension (a program the reference rejects) or a bug fix;
everything else is checked byte for byte against the reference.

- `rec e` is statically opaque (■), the paper's semantics.  The reference
  unfolds a direct self-call under `rec` forever at elaboration time.
- The interpreter reports `<diverged>` for a definition that recurses without
  end, and `<abandoned>` for an `abandon` reached while printing, where the
  reference crashes.
- Extensions: `String` and string literals; `#` comments keep their line
  break; a default case `_ => e` in `match`; implicit arguments
  (`{A : Type}`, `f {T}`, `{x} => e`) with pattern unification and pruning.
- More extensions:
  - `primitive name : T` (`foreign import`) names a runtime operation on
    `Int`/`String` (`Hg.Elab.Prims`); one answering a question returns the
    declared sum's `'true`/`'false`.
  - Definitions by equations: each equation is a case of an induction;
    they are compiled to lambdas and `match`es, as the hand-written `match`
    would be.
  - Watches and their expectations (doctests); `test name`.
  - `do` ... : `x <- e` then the rest is `bind e (\x -> rest)`, with
    whichever `bind` is in scope (so any monad written in Hippogriff).
  - Modules (above).
- Effects (`lib/co.hf`), after Yang and Wu, "Handling Higher-Order
  Effectful Operations with Judgemental Monadic Laws" (POPL 2026): an
  effect's signature is a higher-order functor (`HFunctor`), a handler is a
  raw monad with an algebra (`Handler h`, no laws asked), and a computation
  `Co h a` is the paper's realizability model -- for any handler, a
  continuation into its monad -- so `val`/`bind` satisfy the monad laws by
  beta and eta.  Hippogriff's Π takes the level of its codomain
  (impredicative), which is what makes `Co h a` a small type.
  `test/surface/exc.hf`: exceptions with `catch`, handled by `Maybe`.
- Apps: `lib/dom.hf` is the DOM as an effect (create, text, attr, append,
  on, select and xpath to find an element of the page, sleep,
  fetch), and `lib/state.hf` a state (get, put); an app is a computation with the
  two as one, `Co (dom <+> (state s)) Unit` (`<+>`, the sum of two
  effects, is in `lib/co.hf`): `heff app` writes it, with all it uses, as a script for a page (where
  it goes on the page is the app's to find, by `select` or `xpath`), whose JavaScript runtime is a handler of the effect
  (its monad: the continuation monad `(a -> ()) -> ()`, so asynchrony is a
  handler).  The same app runs under a handler written in Hippogriff for
  tests (`test/surface/dom-trace.hf`).
- Programs: `lib/io.hf` is a program's world as an effect (write to the
  standard output or error, read the standard input, the arguments, an
  environment variable, read and write files, exit with a status); a
  program is a computation with it, `Co io Unit`: `heff node` writes it as
  a Node.js program (an ES module), whose runtime is a handler of `io`
  (again the continuation monad: Node's asynchrony is plain `do`).
  `test/node/echo.hf` does each operation once.
- Examples: each of `examples/counter`, `examples/clock`, `examples/greet`
  is a project (`project.hf`, the program, `index.html`): `npm run examples`
  builds the apps, then open `index.html` in a browser, from the disk as
  it is (the app is a plain script, importing nothing).
  `greet` finds the page's own elements, by a CSS selector and an XPath.
- In the tools (not `heff golden` or `heff ref`, which stay the reference): a
  call of the definition being defined is checked as `rec (...)`, so
  recursion needs no `rec`.
- Evaluation is call-by-value (the reference, in Haskell, is lazy):
  arguments, and the fields of constructors and records, are computed
  before they are passed or put, in order, used or not; a global is
  computed when first used.  `heff run`, `heff test` and
  `heff golden --tools` evaluate so, as the compiled code runs; only
  `heff golden` and `heff ref`, which must print what the reference
  prints, evaluate by need.  The compiler drops or moves only what cannot
  fail (a value), so compiled code abandons exactly when the interpreter
  does.
- In JavaScript, a definition that calls itself last is a loop: directly
  (`count (n - 1) (acc + 1)`), or as a constructor's last argument
  (`Cons x (map f xs)`: the cell is made first, its tail filled by the
  next round).  So `foldl`, `map`, `filter` go a million deep
  (`test/node/deep.hf`); `foldr`, which is neither, does not.
- `Int` is an integer of any size (JavaScript's BigInt; the reference has a
  64-bit `Int`, and no arithmetic).  Division is floor division, as
  Haskell's `div`/`mod`; by zero it abandons.
