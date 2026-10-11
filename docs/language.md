# The language

What heff's programs are made of, beyond its Haskell-like core:
modules, records, implicit arguments, operators, numbers, runes and
strings, and effects.  How a project is laid out is in
[projects.md](projects.md), what the library has in
[library.md](library.md), doc tests and tests in [tests.md](tests.md).

## Modules

A module is a file under a project's `src/`, named by its path there:
`src/parse/surface.hf` is `parse.surface`.  Modules are looked up there
and in `lib/` (`std.io` is `lib/std/io.hf`); a file outside `src/` (a
test) may also import files beside it, by their paths from the root.

Every module imports `builtin` and `prelude` without writing them, and
`list` as `List`, the operations of lists (`List.map`, `List.length`)
-- but those they are made of.  A name is the module's own
definition's, else an import's it writes, else one of those it has
without writing them, a primitive (the prelude's `intAdd`) as any other
name; two imports written that have it are an error only where it is
used unqualified.  An import a module has already is an error --
`import prelude`, or one written twice -- but `import prelude as P` is
another.

`import m as M` imports `m` qualified only: its definitions are `M.x`.
At `M.x` the alias comes first, before a type or a constructor of the
same name -- a type has no fields -- so a module may be imported under
a built-in type's name, and `Rune` anywhere else is still the type.  A
local name `M` (a binder, a theory's parameter) that the alias's `M.x`
would also be is an error where `M.x` is written (E0152): rename one of
them.

```haskell
import rune as Rune

code : Rune -> Int
code r = Rune.toInt r   -- the module's `toInt`; `Rune` in the signature, the type
```

## Records

A record type names its fields, and a record gives each, in the type's
order.  A field may have a default, `x : A = d`: a record checked
against the type may leave the field out, and is given `d`.  A default
names definitions and the fields before it -- a record that leaves it
out is given it of its own fields, each computed once (`let`s around the
record) -- and nothing else where the type is, not its parameters, so
that it means the same wherever a record leaves it out; a theory's
fields may have defaults too.

```haskell
type Opts = { name : String, verbose : Bool = False, label : String = name }

quiet : Opts
quiet = { name = "quiet" }   -- { name = "quiet", verbose = False, label = "quiet" }
```

A record type's values are run-time data, and a type is none: a record
with a field that is a type, or a function of types, or a theory's
record, is a theory, declared as one -- `theory Packed where`, a field a
line -- and `type Packed = { t : Type, size : Int }` is refused, the
field named.

```haskell
theory Packed where
  t : Type
  size : Int

packed : Packed
packed = { t = Int, size = 8 }
```

## Implicit arguments

An implicit argument, `{x : A} -> B`, is inferred where the function is
applied, unless it is given, `f (x := e)`; an instance argument,
`{{x : A}} -> B`, is found among the instances in scope.  An
implicit argument may have a default, `{x : A := d} -> B`: where it is
not given, it is `d`, as it is -- not inferred, so that its type may be
small.  The default names definitions only, so that it means the same
wherever the function is applied.

```haskell
greet : {loud : Bool := False} -> String -> String
greet name = if loud then name ^ "!" else name

calm : String
calm = greet "hi"                 -- "hi"; greet (loud := True) "hi" is "hi!"
```

An instance is an instance binder in scope, or a definition declared
one, `instance x : T` before its equations: it is in scope in the
definitions after it, and in the modules that import its module -- by
its name or by an alias, the prelude in every module -- but not in
those that import them.  Of the instances in scope, exactly one must be
of the type an instance argument wants: none is an error (E0341), and
so is more than one (E0342), whatever their values, so that an import
may make an error of a search, never another answer.

An instance's type may take implicit and instance arguments before what
it is an instance of: `instance eqList : {a : Type} -> {{eq : Eq a}} ->
Eq (List a)` is an instance of each `Eq (List a)` there is an `Eq a`
of.  A search unifies what it is an instance of with the type wanted,
which solves its implicit arguments, and is decided on that alone --
`Eq (List a)`'s and `Eq (List Int)`'s are two for `Eq (List Int)` --
then finds its instance arguments as it found it, each a search of its
own, whose error says the instances that wanted it (`no instance for Eq
Color in scope, wanted by eqList for Eq (List Color)`); one whose type
what the instance is an instance of leaves undetermined is E0340.
Searches go 32 deep at most (E0349), as an instance that wants one of a
type no smaller than its own may want them without end.  An explicit
argument, which no search gives, is refused (E0348), and so is an
implicit one not in what the instance is an instance of, which the
unification could not solve (E0350).

The prelude's `Eq` is a type's equality, `equal x y`: its instances are
`Int`'s (`eqInt`), `Float64`'s, `String`'s, `Rune`'s, `Bool`'s,
`Unit`'s and `Ordering`'s, those of a `List`, a `Maybe`, a `Pair` and an
`Either` of types that have one (`eqList`, `eqMaybe`, `eqPair`,
`eqEither`), and each fixed-width type's module has its own (`int8`'s
`eqInt8`).

```haskell
member : {a : Type} -> {{eq : Eq a}} -> a -> List a -> Bool
member x Nil = False
member x (Cons y ys) = equal x y || member x ys

found : Bool
found = member 2 [1, 2] && member 2.5 [0.5]   -- eqInt found, then eqFloat

same : List (Maybe Int) -> List (Maybe Int) -> Bool
same xs ys = equal xs ys   -- eqList found, given eqMaybe, given eqInt
```

A theory declared again is another type: a module's own `Eq` and its
instances are none of the prelude's.

## Operators

An operator is a name of symbols, defined as `(op) a b = ...`, and how
it binds is declared relatively: `infixl`, `infixr` or `infix`, the
operators of a group, then `above` the groups it binds tighter than and
`below` those it binds looser than, each named by any of its operators.
Binding tighter goes along a chain of them; two operators no chain
relates are refused side by side, parentheses saying which is first
(the prelude's `^` and `+`, `a + b ^ c`).  `below` alone is below all,
any module's operators too.  The prelude's `$` is: `f $ x` is `f x`, to
the right, so `f $ g $ h x` is `f (g (h x))` and `showInt $ a + b * c`
shows the sum; it is inlined, a call of `f` itself.  `$` is the bottom:
nothing is declared below it, or below any operator declared `below`
alone, and two such, side by side, take parentheses.

```haskell
infixl .+. below +

-- | >>> showInt $ 1 .+. 2 + 3
-- | "15"
(.+.) : Int -> Int -> Int
(.+.) a b = a * 10 + b
```

## Numbers

`Int` is an integer of any size, `Float64` IEEE 754's double.  Built in beside them are the fixed-width
numbers: `Int8`, `Int16`, `Int32` and `Int64` (two's complement),
`UInt8` to `UInt64`, and `Float32` (IEEE 754's single).  An integer of
one wraps around, as the machine's does, divides rounding toward zero,
and shifts by the amount modulo its bits; each operation of a `Float32`
is rounded to one.

A number literal is of the type it is used at: an integer literal of any
number type, a fractional one (`2.5`) of a float type.  An integer
literal may be written in hexadecimal, `0x605`, or in binary,
`0b1010`, and any number's digits may be set apart by `_`
(`1_000_000`); the formatter keeps it as it is written.  A literal may
be negative: `-` right before a digit is its sign, unless it is right
after an operand (a name, a literal, a closing bracket, with no space
between).  So `a - 1` and `a-1` subtract, while `f -1` and `a -1` are
applied to -1; `-128` as an `Int8` is its least, and a pattern may be
negative too (and `-x` negates, below).  Where the type is not known
yet, the literal waits for it, and unification finds it from how the
literal is used (`id 5` wanted as an `Int32` is one).  It must be one
of the type's values, however late the type is found: `300` as a
`UInt8` is an error, as `1.5` as an `Int32` or `5` as a `String` is.
What nothing decides is an `Int`, or a `Float64`, when the definition
is settled, before its instance arguments are last looked
for (an integer literal is a `Float64` where it shares its type with a
fractional one, `[1, 2.5]`).  A `let` does not generalise: what it binds
has one type, for every use.  A literal pattern is of the type of what
it matches, and compared by that type's equality (an `Int`'s is the
prelude's `intEq`, a fixed-width type's builtin's, `int32Eq` and the
others).

```haskell
import uint8 as U8

-- | >>> U8.show (U8.add mask 255)
-- | "239"
mask : UInt8
mask = 240
```

Each fixed-width integer type has a module, imported by a name
(`import int32 as I32`), whose operations are named as the prelude's:
`add`, `div`, `rem`, `compare`, `show`, the bits' (`and`, `or`, `xor`,
`complement`, `shiftL`, `shiftR`), and `fromInt` and `toInt`, by way of
which one type's value becomes another's (`I8.fromInt (I32.toInt x)`).
`float32`'s has the arithmetic, `compare` and `show`, `fromInt`,
`fromFloat`, `toFloat`, `floor` and `sqrt`.  An `Int`'s bits are
`int`'s (`import int as I`: `I.and`, `I.or`, `I.xor`, `I.complement`,
`I.shiftL`, `I.shiftR`), as two's complement has them with the sign
extended for ever; a shift right rounds down, and one by a negative
count abandons.  A fixed-width type's operators, `+` to `>=`, are its
module's too, for a module that imports it as it is: the prelude's,
which it has without writing it, give way to them.  How each number is
kept by each backend, and given to a provider, is in
[providers.md](providers.md#numbers-and-runes).

`-` where it would be a number's sign, right before a name or a `(`
instead of a digit, negates what it is before: a name with its fields
(`-p.x`) or what is in parentheses (`-(f y)`), an argument's worth.  It
is the `neg` in scope applied to it, as `a - b` is the `(-)` in scope:
the prelude's `neg` negates an `Int`, and a fixed-width type's module,
imported as it is, has its own, which `-x` then is.  So `f -x` is
`f (-x)`, as `f -1` is `f (-1)`, while `a - x` and `a-x` subtract;
`-f x` is `(-f) x`, and `- x`, a space after the `-`, is no negation.
A `Float64` has no `neg`, so no `-x` either (`negFloat x`), and a
pattern's `-` is only a number's sign.

## Runes and strings

A `Rune` is a code point of Unicode, a 32-bit value, as Go's `rune` is.
A rune literal is a `'` where a token starts (in a name, as `x'`, a `'`
is a prime), one character, and a `'`: `'a'`, `'é'`, `'😀'`; or an
escape, as a string has them: `'\n'`, `'\t'`, `'\r'`, a code point in
hexadecimal, `'\u{1F600}'`, and `\` before any other character that
character, `'\''`, `'\\'`.  It is a Unicode scalar value, from U+0000
to U+10FFFF but the surrogates (`'\u{D800}'` is an error, as it is in
a string, where `"\u{1F600}"` is 😀 and `"\u{41}"` is `"A"`), and a
`Rune` whatever it is checked at: an integer literal is no rune, nor a
rune literal an integer.  A rune literal is a pattern too, compared by
code point.

A `String` is bytes, as Go's is: UTF-8 as a literal writes it (`"é"`
is two bytes), though any bytes may be in one -- what a file, a stream
or a command gave, `fromByte 255`, or a string's own escape `"\xff"`, a
byte of two hexadecimal digits.  Its length (`strLength`), an index
(`byteAt`, a byte from 0 to 255, -1 out of the string), a slice and its
order (`strLt`: the bytes', which of UTF-8 is the code points') are of
bytes, the same on every backend.  The prelude takes a string apart
into its runes, `runes : String -> List Rune`, and puts them together
again, `ofRunes`; `runeAt s i` is the rune whose bytes start at `i`
and how many they are (Go's `utf8.DecodeRuneInString`).  What is not
UTF-8 is read as U+FFFD, one for each longest start of a rune's bytes,
or a byte that starts none (as `TextDecoder` reads it), and shown a
byte at a time, `"a\xff"`, as it is written back.

```haskell
import rune as R

-- | >>> initials "héllo"
-- | "Hé"
initials : String -> String
initials s = ofRunes (List.map R.toUpper (List.take 2 (runes s)))
```

The module `rune` (`import rune as R`) has their comparisons (`eq`,
`compare`, and `==` to `>=` for a module that imports it as it is),
`toInt` and `fromInt` (which wraps an integer around to 32 bits, as Go's
`rune(n)` does: a rune may be no scalar value, `isValid` says, and is
U+FFFD, `replacement`, in a string made of it), `toString`, `show`
(`"'a'"`), a rune in a string -- `width` (its bytes), `count` (a
string's runes), `foldl` (each rune with where it starts),
`validString` and `toValid` (what is not UTF-8 made U+FFFD) -- and
ASCII's classes and cases: `isDigit`, `isAlpha`, `isSpace`, `isUpper`,
`toUpper` and the like (`isAlpha 'é'` is `False`).

JavaScript's own text, UTF-16, is another type, `std.js`'s `JSString`:
see [providers.md](providers.md#jsstring).

## Effects

`lib/co.hf` implements effects after Yang and Wu (POPL 2026).  A program
names the effects it needs as instance arguments, in any world that has
them -- it can do nothing else -- and is given them where it runs:

```haskell
import co
import std.filesystem
import std.io

world : HFunctor
world = stdio <+> fs

program : {e : HFunctor} -> {{io : stdio <: e}} -> {{disk : fs <: e}} -> Co e Unit
program = do
  ok <- writeFile "hello.txt" "hello"
  print (if ok then "written" else "not written")

main : Co world Unit
main = program (io := \p -> InL p) (disk := \p -> InR p)
```

A handler says what its effect's operations do, in a monad of its
own; `through` handles one effect into `Co` of the others.  Pure work
runs where it is reached, by value: an argument where the computation
is built (`pure (fib n)` computes `fib n` there), a step after a `bind`
as the computation is handled.  Only what comes after an operation is
left to the handler's monad, so a handler into `Co e` that puts a part
elsewhere -- `lib/parallel.hf`'s `Par`, a side in a process of its own
-- finds its pure work done already, unless it comes after `later`:
`parIn (later (\u -> fib n)) (later (\u -> fib m))`.

An effect the world has is carried out by a provider, its JavaScript
and C sides beside its module: see [providers.md](providers.md).
