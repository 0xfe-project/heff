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
A module found at more than one file -- a `list.hf` of the program's
beside the library's -- is an error, and is loaded from neither: what
is used of it is no error of its own, until one file is renamed.

Every module imports `builtin` and `prelude` without writing them, and
`list` as `List`, the operations of lists (`List.map`, `List.length`)
-- but those they are made of.  A name is the module's own
definition's, else an import's it writes, else one of those it has
without writing them, a primitive (the prelude's `intAdd`) as any other
name; two imports written that have it are an error only where it is
used unqualified.  An import a module has already is an error --
`import prelude`, or one written twice -- but `import prelude as P` is
another.

A module defines a name once: a second definition of it is an error
(E0153) that says where the first is -- a second signature, `data`,
`type`, `theory` or `primitive` of the name, or equations of it apart
from its signature, which they are right under, no other definition
between them (a watch may be).  A tag is no definition: two types may
have the same.

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

A record with some fields replaced, `{ r | x = v, ... }`, is of `r`'s
type: each field set is one of the type's, set once and at its type, in
any order, and every other field is `r`'s, `r` computed once.  A field
of a field is set by its path: `{ s | pos.x = 1 }` is
`{ s | pos = { s.pos | x = 1 } }`.  A field not set keeps `r`'s value
though it has a default -- defaults are given where a record is built,
not where one is updated -- and one whose type a field set changes (a
theory's `unit : t`, where `t` is set) is set too.  A theory's record
is updated the same way.

```haskell
move : Float64 -> Body -> Body
move dt b = { b | x = b.x + dt * b.vx, y = b.y + dt * b.vy }

loud : Opts
loud = { quiet | name = "loud" }   -- { name = "loud", verbose = False, label = "quiet" }
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
it is an instance of: `instance equalityList : {a : Type} -> {{eq :
Equality a}} -> Equality (List a)` is an instance of each `Equality
(List a)` there is an `Equality a` of.  A search unifies what it is an
instance of with the type wanted, which solves its implicit arguments,
and is decided on that alone -- `Equality (List a)`'s and `Equality
(List Int)`'s are two for `Equality (List Int)` -- then finds its
instance arguments as it found it, each a search of its own, whose
error says the instances that wanted it (`no instance for Equality
Color in scope, wanted by equalityList for Equality (List Color)`); one
whose type what the instance is an instance of leaves undetermined is
E0340.
Searches go 32 deep at most (E0349), as an instance that wants one of a
type no smaller than its own may want them without end.  An explicit
argument, which no search gives, is refused (E0348), and so is an
implicit one not in what the instance is an instance of, which the
unification could not solve (E0350).

The prelude's `Equality` is a type's equality, `equal x y` and `x ==
y`: its instances are `Int`'s (`equalityInt`), `Float64`'s, `String`'s,
`Bytes`', `Rune`'s, `Bool`'s, `Unit`'s and `Ordering`'s, those of a
`List`, a `Maybe`, a `Pair` and an `Either` of types that have one
(`equalityList`, `equalityMaybe`, `equalityPair`, `equalityEither`),
and each fixed-width type's module has its own (`int8`'s
`equalityInt8`) -- an instance named by its theory, then its type.  The
prelude's other operators are theories' too ([operators](#operators)).
A search tries only the instances whose type could be the one wanted:
`x == y` at `Int` tries `equalityInt`, not `equalityList`, whose
`Equality (List a)` no `Equality Int` is.

```haskell
member : {a : Type} -> {{eq : Equality a}} -> a -> List a -> Bool
member x Nil = False
member x (Cons y ys) = equal x y || member x ys

found : Bool
found = member 2 [1, 2] && member 2.5 [0.5]   -- equalityInt, then equalityFloat64

same : List (Maybe Int) -> List (Maybe Int) -> Bool
same xs ys = equal xs ys   -- equalityList, given equalityMaybe, given equalityInt
```

A theory declared again is another type: a module's own `Equality` and
its instances are none of the prelude's.

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

The prelude's operators are each a theory's operation, as Lean's are:
`+` is `Addition`'s `add`, `-` `Subtraction`'s `sub`, `*`
`Multiplication`'s `mul`, `/` `Division`'s `div`, `%` `Remainder`'s
`mod`, `/.` `FloatDivision`'s `divide`, `==` and `/=` `Equality`'s
`equal`, `<` and `<=` `Order`'s `lt` and `le` (`>` and `>=` the same,
flipped), and `-x` `Negation`'s `neg` -- each an instance argument,
found at the type it is used at: `n + 1` at `Int` is the prelude's
`additionInt`'s, `x * c` at `Int64` `int64`'s `multiplicationInt64`'s,
in one file, and a function of any type that has one asks for it.  A
type has those it has: an integer type every one but `/.`, a float no
`/` or `%` -- a float's division, IEEE's (by 0, an infinity), is `/.`,
which binds as `*` does.  `Order`'s `compare`, `min` and `max` are the
prelude's too, and `Bitwise`', an integer type's bits: `and`, `or`,
`xor`, `complement`, `shiftL` and `shiftR`; `IsZero`'s `isZero` says
whether an integer is its type's zero.  A theory is named by a whole
noun, so that the short names are a program's own to give: `data Op =
Add | Sub | Mul | Div` takes none of the prelude's.  Where the instance
is known, what is compiled is the operation itself: `n + 1` at `Int`
is `intAdd n 1`, as if written so, and `n < 2` a C condition.

```haskell
twice : {a : Type} -> {{add : Addition a}} -> a -> a
twice x = x + x   -- twice 21 is 42, twice 1.25 is 2.5

step : Int64 -> Int64
step x = x * 6364136223846793005 + 1442695040888963407   -- with `import int64 as I64`
```

## Numbers

`Int` is an integer of any size, `Float64` IEEE 754's double.  Built in beside them are the fixed-width
numbers: `Int8`, `Int16`, `Int32` and `Int64` (two's complement),
`UInt8` to `UInt64`, and `Float32` (IEEE 754's single).  An integer of
one wraps around, as the machine's does (the least by -1 is itself),
divides rounding toward zero, and shifts by the amount modulo its bits
(by a negative one, of a signed type, the other way by its size);
each operation of a `Float32` is rounded to one.  An `Int` divides
rounding down, its remainder of the divisor's sign.  An integer's
division is total, as Lean's and Coq's are: by zero it is 0, and its
remainder the dividend, so that `(x / y) * y + x % y` is `x` whatever
`y` is -- for a program that would hear of a zero divisor, `exn`'s
`exnDiv` and `exnMod` throw `MkDivByZero` instead (of a type with an
`IsZero`).

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
(`import int32 as I32`, as each has the same names): `add`, `sub`,
`mul`, `div`, `rem`, `eq`, `lt`, `le`, `show`, `minBound` and
`maxBound`, and `fromInt` and `toInt`, by way of which one type's value
becomes another's (`I8.fromInt (I32.toInt x)`).  `float32`'s has the
arithmetic, `eq`, `lt`, `le`, `isNaN` and `show`, `fromInt`,
`fromFloat`, `toFloat`, `floor` and `sqrt`.  None of them has a name
the prelude has for every type -- `neg`, `compare`, `min`, `max`, the
bits' -- so that a module opened is in the way of none of them at
another type: those are its instances'.  An `Int`'s bits are the
prelude's `and`, `or`, `xor`, `complement`, `shiftL` and `shiftR`, by
`int`'s instance, `bitwiseInt` (`import int as I`), as two's complement
has them with the sign extended for ever; a shift right rounds down,
and one by a negative count is one the other way (`shiftL x -n` is
`shiftR x n`), as a signed fixed-width type's is.  A fixed-width type's
instances are its module's, found wherever it is imported, by a name
or not: the prelude's operators at its type, `x * c + d` at `Int64`
beside `n + 1` at `Int`, `-x`, `min x y` and `and x y`; `float32`'s has
all but `Division`, `Remainder`, `Bitwise` and `IsZero`, and
`FloatDivision` too, its `/.`.  How each number is kept by each
backend, and given to a provider, is in
[providers.md](providers.md#numbers-and-runes).

`-` where it would be a number's sign, right before a name or a `(`
instead of a digit, negates what it is before: a name with its fields
(`-p.x`) or what is in parentheses (`-(f y)`), an argument's worth.  It
is the `neg` in scope applied to it, as `a - b` is the `(-)` in scope:
the prelude's `neg` is `Negation`'s, of any type that has one -- an
`Int`, a float (0.0's sign too: `1.0 /. -(0.0)` is -Infinity), a
fixed-width type -- and a module's own `neg`, where it has one, is what
`-x` then is.  So `f -x` is `f (-x)`, as `f -1` is `f (-1)`, while
`a - x` and `a-x` subtract; `-f x` is `(-f) x`, and `- x`, a space after
the `-`, is no negation.  A pattern's `-` is only a number's sign.

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
or a command gave, `fromByte 255` (a `UInt8`, the byte type, its
argument: `fromByte (U8.fromInt n)` of an `Int`), or a string's own
escape `"\xff"`, a byte of two hexadecimal digits.  Its length
(`strLength`), an index (`byteAt`, a byte from 0 to 255, -1 out of the
string), a slice and its order (`strLt`: the bytes', which of UTF-8 is
the code points') are of bytes, the same on every backend.  The prelude takes a string apart
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

JavaScript's own text, UTF-16, is another type, `browser.js`'s `JSString`:
see [providers.md](providers.md#jsstring).

### Bytes

`Bytes` are bytes that are not text -- a body, what a socket reads, a
file's content -- a type of their own, the prelude's, and no `String`;
never changed once made.  The module `bytes`
(`import bytes as Bytes`) has what is done with them: `length`,
`index` (the byte at `i`, a `UInt8`, if there is one), `slice` (as a
string's, `strSlice`: clamped, a negative index from the end),
`append` and `concat`, `empty`, `fromList` and `toList` (of
`UInt8`s), `fromString` (a string's bytes, as they are) and
`toString` (their text, UTF-8, what is not UTF-8 U+FFFD, as `rune`'s
`toValid` makes a string), `eq` (as `==`, whose instance,
`equalityBytes`, is the prelude's: no import is needed for it), and
`show`, which writes them as `b"..."`: a byte of ASCII that is printed
as it is (`"` and `\` escaped), `\n`, `\r` and `\t`, and any other
byte `\xff`.

So a literal writes them: `b` right before a string, where a token
starts (`b "x"` is a name and a string, and so is `ab"x"`), read as a
string is, its escapes alike -- `b"GET \xff\r\n"`.  A character as it
is written is its UTF-8 bytes, as in a string (`b"é"` is two), `\xff`
a byte, any, and `\u{...}` its code point's UTF-8 bytes too (`b"\u{e9}"`
is `b"\xc3\xa9"`), a Unicode scalar value as a string's is.  What
`show` writes is a literal of the same bytes, and the tools show a
`Bytes` as `show` does.  A literal is a `Bytes` whatever it is checked
at, and a pattern too, compared by bytes.  A C program has it as a
static object, as it has a string literal; a JavaScript one makes a
`Uint8Array` of its bytes.

```haskell
import bytes as Bytes

-- | >>> Bytes.show (greeting "hé")
-- | "b\"hi h\\xc3\\xa9\""
-- | >>> greeting "hé" == b"hi h\u{e9}"
-- | True
greeting : String -> Bytes
greeting name = Bytes.append b"hi " (Bytes.fromString name)

-- | >>> List.map method [b"GET", Bytes.fromString "PUT"]
-- | [Just "get", Nothing]
method : Bytes -> Maybe String
method b"GET" = Just "get"
method b"POST" = Just "post"
method _ = Nothing
```

A C program keeps them as a buffer of bytes, a JavaScript one as a
`Uint8Array` ([providers.md](providers.md#bytes)).

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
