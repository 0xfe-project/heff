# heff

heff is Hippogriff (Lynch & Staton, arXiv:2608.19728), ported from the reference
Haskell implementation to PureScript and checked against it by differential
testing, with a text-based toolchain: files, modules, a checker, doctests,
an interpreter, and compilers to JavaScript and PureScript.

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
seen by the files that import the module declaring them.  `lib/prelude.hg`
has the arithmetic, comparisons, `&&`, `||`, `++` and `$`, with `Maybe`,
`Either`, lists and their functions.

## The stages

```
text ─[Hg.Parse]→ notation ─[Hg.Elab]→ core ─┬─[Hg.Eval]→ values
                                             ├─[Hg.Lower]→ IR ─[Hg.Compile.JavaScript]→ JavaScript
                                             └─[Hg.Compile.PureScript]→ PureScript
```

| path | stage |
| --- | --- |
| `src/Hg/Parse` | text → notation: `Surface` (`.hg`), `FNotation` and `Config` (the reference's `.hpgf`), `Import` (a file's header) |
| `src/Hg/Elab` | notation → core: `Top` (declarations), `Clauses` (equations → `match`), `Rules`, `Unify` (implicit arguments), `Environment`, `Prims` |
| `src/Hg/Core` | the core: syntax, values (normalisation by evaluation), conversion, readback, the reference's printer |
| `src/Hg/Print` | core → `.hg` text (`Surface`, `Resugar`: `match`es back to equations) |
| `src/Hg/Eval` | the interpreter (lazy, as the reference's) |
| `src/Hg/Lower` | core → an untyped IR, and computing on it: `Erase` (types, with the parameters that take them and the arguments given for them, read off the types with the elaborator's NbE), `Shake` (only what is reached), `Simplify` (β, inlining what is not recursive, known fields and tags), `Arity` (a definition of n parameters called with n arguments at once) |
| `src/Hg/Compile` | `JavaScript` (the IR emitted as an ES module; a DOM runtime for apps), `PureScript` (typed, from the core) |
| `src/Hg/Support` | pretty printing, diagnostics, big integers |
| `src/Hg/Format.purs` | the formatter: `.hg` read again keeping every token and comment, laid out anew; refused if the result would not read as the same notation with the same comments |
| `src/Hg/Lsp.purs` | the language server: diagnostics, hover, go to definition, formatting |
| `src/Hg/Load.purs`, `src/Hg/Driver.purs`, `src/Main.purs` | finding a program's files; running the stages over them; the CLI |

## Modules

A module is a file, named by its path from the project's root:
`parse/surface.hg` is `parse.surface`.  The root is the nearest directory
above the file given that has an `hg.json`, else the file's own directory.
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

hg check FILE              # elaborate FILE and what it imports; its definitions' types
hg run FILE                # what each watch evaluates to
hg test FILE...            # do the expectations hold?
hg promote FILE            # write each watch's value under it
hg compile-js FILE OUT.js  # an ES module; its default export has FILE's definitions
hg app FILE [NAME] OUT.js  # a script running an app (lib/dom.hg) in a page
hg compile FILE MOD OUT    # a PureScript module
hg explain FILE            # the elaboration trace: metavariables, unifications, delays
hg type-at FILE OFFSET     # what the name there resolved to, and its type
hg golden [--tools] FILE   # the reference's golden report (.hpgf)
hg ref FILE                # what the reference executable prints (.hpgf)
hg fmt [--check] FILE...   # format in place (--check: list what is not formatted)
hg lsp                     # a language server on stdio (editors/vscode is a client)

npm test
```

(`hg` is `node bin/hg.mjs`, or `npm link` it.)

The differential fuzzer (`npm run fuzz`) needs the reference
implementation: build it with GHC ≥ 9.12 and put the path of the
`hippogriff` executable in `ref/REF_EXE`.

## Compiling

`hg compile-js` and `hg app` compile, not transliterate: what is a type is
gone, and what is known at compile time is computed then.  `examples/clock.hg`

```haskell
tick :: Int -> Int -> Co (dom Int) Unit
tick label n = do
  sleep 100
  text label (strAppend (intShow n) " ticks")
  tick label (intAdd n 1)
```

becomes (the handler `hd` is the page's, given at run time; `Co`'s `bind`,
the operations and the effect's `hmap` are computed away)

```js
const tick = (label, n, hd, k) => hd.bind(hd.malg({ $: "Sleep", a: [100n, { $: "MkUnit", a: [] }] }))((x) => {
  const str = n.toString() + " ticks";
  return hd.bind(hd.malg({ $: "Text", a: [label, str, { $: "MkUnit", a: [] }] }))((x1) => tick(label, n + 1n, hd, k));
});
```

## Tests

- `test/golden.sh`: the reference's golden tests, byte for byte.
- `test/surface.mjs`: the library, examples and `test/surface`, `test/modules`
  elaborate and their expectations hold; the module rules reject what they
  should.
- `test/app-run.mjs`: the example apps, compiled and run on a pretend page.
- `test/compile-check.mjs`, `test/compile-check-js.mjs`: the golden corpus,
  `test/programs` and generated programs, compiled and run; every value
  computed must be the interpreter's.
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
- Effects (`lib/co.hg`), after Yang and Wu, "Handling Higher-Order
  Effectful Operations with Judgemental Monadic Laws" (POPL 2026): an
  effect's signature is a higher-order functor (`HFunctor`), a handler is a
  raw monad with an algebra (`Handler h`, no laws asked), and a computation
  `Co h a` is the paper's realizability model -- for any handler, a
  continuation into its monad -- so `val`/`bind` satisfy the monad laws by
  beta and eta.  Hippogriff's Π takes the level of its codomain
  (impredicative), which is what makes `Co h a` a small type.
  `test/surface/exc.hg`: exceptions with `catch`, handled by `Maybe`.
- Apps: `lib/dom.hg` is the DOM as an effect (create, text, attr, append,
  on, get/put the app's state, sleep, fetch).  A computation with it is an
  app: `hg app` writes it, with all it uses, as a script for a page (into
  `<div id="hg-app">`), whose JavaScript runtime is a handler of the effect
  (its monad: the continuation monad `(a -> ()) -> ()`, so asynchrony is a
  handler).  The same app runs under a handler written in Hippogriff for
  tests (`test/surface/dom-trace.hg`).  See `examples/`.
- In the tools (not `hg golden` or `hg ref`, which stay the reference): a
  call of the definition being defined is checked as `rec (...)`, so
  recursion needs no `rec`.
- The compiled code is call-by-value; the reference interpreter is lazy.
  What the compiler finds unused is not computed, so compiled code may
  abandon less often than call-by-value would.
- `Int` is an integer of any size (JavaScript's BigInt; the reference has a
  64-bit `Int`, and no arithmetic).  Division is floor division, as
  Haskell's `div`/`mod`; by zero it abandons.
