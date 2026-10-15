# The language

What heff's programs are made of, beyond its Haskell-like core:
modules (and what they expose), records, implicit arguments, operators,
numbers, runes and strings, and effects.  How a project is laid out is in
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

A tag has no type of its own either: it is checked at the type it is
wanted at.  At a function type it is the function that applies it to
the arguments the type takes -- `MkBox` at `Int -> Box` is `\x -> MkBox
x`, `MkP 1` at `Int -> P` is `\y -> MkP 1 y` -- so `List.map MkBox [1,
2]` needs no lambda written; whether the tag is the last codomain's, and
with how many fields, is said as of the tag applied by hand.

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

A module may say what an importer sees of it by a header, first, before
its imports: `module (x, T, U(..), (<+>))` lists some of its own
definitions, each once (E0155), over as many lines as it takes.  With
no header, a module exposes all it defines.  A name it does not list is
its own: an importer neither names it, as `x` or `M.x` (E0353, saying
which module keeps it), nor finds it as an instance (an instance is
found by the names in scope).  A data type listed bare, `T`, is seen
without its tags: an importer has the type, and what the module exposes
to make and take apart its values, but writes none of its tags, to
build a `T` or to match one (E0353); `T(..)` exposes its tags too.  An
alias (`type`), a record type or a theory is seen whole -- a field is
the type's, no name of the module's -- so `(..)` is only of a data type
(E0155).  An operator is listed in parentheses.

```haskell
module (Counter, counter, bump, count)

data Counter = MkCounter Int   -- `MkCounter` is the module's alone

counter : Counter
counter = MkCounter 0

bump : Counter -> Counter
bump (MkCounter n) = MkCounter (n + 1)

count : Counter -> Int
count (MkCounter n) = n
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
may make an error of a search, never another answer.  A type wanted
that is not known whole waits until it is, and is searched for as soon
as it is, so that what the instance found says (its type members: a
`for x in xs` over a `List Int` finds `Iterate (List Int)`, whose `Elem`
is `Int`) is known to what follows; one never known is E0340.

An instance's type may take implicit and instance arguments before what
it is an instance of: `instance instanceEqualityList : {a : Type} ->
{{eq : Equality a}} -> Equality (List a)` is an instance of each
`Equality (List a)` there is an `Equality a` of.  A search unifies what
it is an instance of with the type wanted, which solves its implicit
arguments, and is decided on that alone -- `Equality (List a)`'s and
`Equality (List Int)`'s are two for `Equality (List Int)` -- then finds
its instance arguments as it found it, each a search of its own, whose
error says the instances that wanted it (`no instance for Equality
Color in scope, wanted by instanceEqualityList for Equality (List
Color)`); one whose type what the instance is an instance of leaves
undetermined is E0340.
Searches go 32 deep at most (E0349), as an instance that wants one of a
type no smaller than its own may want them without end.  An explicit
argument, which no search gives, is refused (E0348), and so is an
implicit one not in what the instance is an instance of, which the
unification could not solve (E0350).

The prelude's `Equality` is a type's equality, `equal x y` and `x ==
y`: its instances are `Int`'s (`instanceEqualityInt`), `Float64`'s,
`String`'s, `Bytes`', `Rune`'s, `Bool`'s, `Unit`'s and `Ordering`'s,
those of a `List`, a `Maybe`, a `Pair` and an `Either` of types that
have one (`instanceEqualityList`, `instanceEqualityMaybe`,
`instanceEqualityPair`, `instanceEqualityEither`), and each fixed-width
type's module has its own (`int8`'s `instanceEqualityInt8`) -- an
instance named `instance`, then its theory, then its type: a name no
one need call, as a search finds it.  The prelude's other operators are
theories' too ([operators](#operators)).  A search tries only the
instances whose type could be the one wanted: `x == y` at `Int` tries
`instanceEqualityInt`, not `instanceEqualityList`, whose `Equality
(List a)` no `Equality Int` is.

```haskell
member : {a : Type} -> {{eq : Equality a}} -> a -> List a -> Bool
member x Nil = False
member x (Cons y ys) = equal x y || member x ys

found : Bool
-- instanceEqualityInt, then instanceEqualityFloat64
found = member 2 [1, 2] && member 2.5 [0.5]

same : List (Maybe Int) -> List (Maybe Int) -> Bool
-- instanceEqualityList, given instanceEqualityMaybe, given instanceEqualityInt
same xs ys = equal xs ys
```

An implicit argument that is a function of types, as a theory over a
type constructor wants, is inferred from its application too.  `m Int`
met with `Maybe Int` is outside what unification solves -- `m` could be
`Maybe` or `\_ -> Maybe Int` -- so when the definition is done and
nothing else has decided it, it is solved to the first order, as Lean
does: `m Int` and `Maybe Int`'s last arguments are matched, so `m` is
`Maybe`.  This is done only where the other side is a name whose
definition is, under its arguments, a sum or a record -- however it is
written, `data`, `theory`, `type` or an equation `Box a = { item : a }`;
not an alias to another application, which computes further --
applied to as many arguments at least, and `m`'s type is that of what is left:
not with a local's application (`f Int`), nor with `Int`, which has no
argument for `m`'s.  A program that wants `\_ -> Maybe Int` gives it, `(m := ...)`.

```haskell
twice : {m : Type -> Type} -> {{mo : Monad m}} -> m Int -> m Int
twice c = mo.bind c (\x -> mo.bind c (\y -> mo.pure (x + y)))

r : Maybe Int
r = twice (Just 2 : Maybe Int)    -- m is Maybe: Monad Maybe's instance
```

The prelude has two theories of a value as a string, as Rust has
`Debug` and `Display` and Python `repr` and `str`.  `Show` is a value
as the source writes it, `show x`: a string quoted (`show "hi"` is
`"\"hi\""`, its `"`, `\` and line feeds escaped), a rune quoted
(`show 'a'` is `"'a'"`), bytes a binary (`show b["hi"]` is
`"b[\"hi\"]"`), a list `[1, 2]`, a `Maybe` `Just (1)` or `Nothing`.  It is
what a test shows a value as, and compares two by (`check`'s
`assertEqual`, [tests.md](tests.md)), so two values that differ are not
written the same.  `ToString` is a value as text, `toString x`, for a
reader: a string itself (`toString "hi"` is `"hi"`), a rune its
one-rune string (`toString 'a'` is `"a"`), bytes the UTF-8 text they
are (`toString b["hi"]` is `"hi"`, what is not UTF-8 U+FFFD).

`Show`'s instances are `Int`'s (`instanceShowInt`), `Float64`'s,
`String`'s, `Rune`'s (`instanceShowRune`), `Bytes`'
(`instanceShowBytes`), `Bool`'s, `Unit`'s and `Ordering`'s, with no
import, and those of a `List`, a `Maybe`, a `Pair` and an `Either` of
types that have one (`instanceShowList`, `instanceShowMaybe`,
`instanceShowPair`, `instanceShowEither`); `Int`'s and `Float64`'s are
the prelude's `showInt` and `showFloat`, functions of their own.
`ToString`'s are `String`'s (`instanceToStringString`), `Rune`'s,
`Bytes`', and `Int`'s, `Float64`'s, `Bool`'s, `Unit`'s and
`Ordering`'s, the same text as `show`'s -- but no list's, `Maybe`'s,
pair's or `Either`'s, which have no one text.  Each fixed-width type's
module has both (`int8`'s `instanceShowInt8` and
`instanceToStringInt8`, each the number as a literal writes it), found
where the module is imported.  A type of a program's own has one where
the program declares it:

```haskell
data Color = Red | Green

instance instanceShowColor : Show Color
instanceShowColor = { show = written }
  where
    written : Color -> String
    written Red = "Red"
    written Green = "Green"

shown : String
-- "[Red, Green]": instanceShowList, given instanceShowColor
shown = show ([Red, Green] : List Color)
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
the right, so `f $ g $ h x` is `f (g (h x))` and `show $ a + b * c`
shows the sum; it is inlined, a call of `f` itself.  `$` is the bottom:
nothing is declared below it, or below any operator declared `below`
alone, and two such, side by side, take parentheses.

```haskell
infixl .+. below +

-- | >>> show $ 1 .+. 2 + 3
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
`instanceAdditionInt`'s, `x * c` at `Int64` `int64`'s
`instanceMultiplicationInt64`'s, in one file, and a function of any type that has one asks for it.  A
type has those it has: an integer type every one but `/.`, a float no
`/` or `%` -- a float's division, IEEE's (by 0, an infinity), is `/.`,
which binds as `*` does.  `Order`'s `compare`, `min` and `max` are the
prelude's too (an `Order` of `Int`, `Float64`, `String`, `Rune`,
`Bool`, `Unit` and `Ordering`, and of a `List`, a `Maybe` and a `Pair`
of types that have one, lexically: what a map's keys and `List.sort`
ask for), and `Bitwise`', an integer type's bits: `and`, `or`,
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
step x = x * 6364136223846793005 + 1442695040888963407   -- with `import int64 as Int64`
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
`y` is -- for a program that would hear of a zero divisor,
`exception`'s `divOrThrow` and `modOrThrow` throw `MkDivByZero` instead
(of a type with an `IsZero`).

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
import uint8 as UInt8

-- | >>> show (UInt8.add mask 255)
-- | "239"
mask : UInt8
mask = 240
```

Each fixed-width integer type has a module, imported by a name
(`import int32 as Int32`, as each has the same names): `add`, `sub`,
`mul`, `div`, `rem`, `eq`, `lt`, `le`, `minBound` and `maxBound`, and `fromInt` and `toInt`, by way of which one type's value
becomes another's (`Int8.fromInt (Int32.toInt x)`).  `float32`'s has the
arithmetic, `eq`, `lt`, `le`, `isNaN`, `fromInt`,
`fromFloat`, `toFloat`, `floor` and `sqrt`.  None of them has a name
the prelude has for every type -- `neg`, `compare`, `min`, `max`, the
bits' -- so that a module opened is in the way of none of them at
another type: those are its instances'.  An `Int`'s bits are the
prelude's `and`, `or`, `xor`, `complement`, `shiftL` and `shiftR`, by
`int`'s instance, `instanceBitwiseInt` (`import int as Int`), as two's
complement has them with the sign extended for ever; a shift right rounds down,
and one by a negative count is one the other way (`shiftL x -n` is
`shiftR x n`), as a signed fixed-width type's is.  A fixed-width type's
instances are its module's, found wherever it is imported, by a name
or not: the prelude's operators at its type, `x * c + d` at `Int64`
beside `n + 1` at `Int`, `-x`, `min x y` and `and x y`; `float32`'s has
all but `Division`, `Remainder`, `Bitwise` and `IsZero`, and
`FloatDivision` too, its `/.`; and each its `Show` and its `ToString`,
the number as a literal writes it (`show (7 : Int8)` is `"7"`).  How each number is kept by each
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
argument: `fromByte (UInt8.fromInt n)` of an `Int`), or a string's own
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

On the C backend a slice of 64 bytes or more, and at least a quarter
of the string it is cut from, shares its bytes, as Go's does (a view,
taken at once: a string walked by its tail, `strSlice s 1 (strLength
s)`, is not copied at each step, until what is left is a small part of
it); a shorter one is a copy, so that a small slice never holds a
large string.  `Bytes` are sliced so too.

```haskell
import rune as Rune

-- | >>> initials "héllo"
-- | "Hé"
initials : String -> String
initials s = ofRunes (List.map Rune.toUpper (List.take 2 (runes s)))
```

The module `rune` (`import rune as Rune`) has their comparisons (`eq`,
`compare`, and `==` to `>=` for a module that imports it as it is),
`toInt` and `fromInt` (which wraps an integer around to 32 bits, as Go's
`rune(n)` does: a rune may be no scalar value, `isValid` says, and is
U+FFFD, `replacement`, in a string made of it), a rune in a string -- `width` (its bytes), `count` (a string's runes), `foldl` (each rune with where it starts),
`validString` and `toValid` (what is not UTF-8 made U+FFFD) -- and
ASCII's classes and cases: `isDigit`, `isAlpha`, `isSpace`, `isUpper`,
`toUpper` and the like (`isAlpha 'é'` is `False`).  A rune's `show`
and `toString` are the prelude's, with no import: `show 'a'` is
`"'a'"`, as a literal writes it (a control character an escape,
`"'\\n'"`), and `toString 'a'` is `"a"`, its string.

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
`UInt8`s), `fromString` (a string's bytes, as they are), `asString`
(the bytes as a string, as they are, UTF-8 or not: `fromString` undone)
and `eq` (as
`==`, whose instance, `instanceEqualityBytes`, is the prelude's: no
import is needed for it).  Their `show` and `toString` are the
prelude's too: `show` writes them as a binary does
(`instanceShowBytes`, below); `toString` is their text
(`instanceToStringBytes`), UTF-8, what is not UTF-8 U+FFFD, as
`rune`'s `toValid` makes a string.  A binary, `b[...]`, writes them and
takes them apart.

A C program keeps them as a buffer of bytes, a JavaScript one as a
`Uint8Array` ([providers.md](providers.md#bytes)).

### Bits

`Bits` are a bitstring, of any length in bits, never changed once made:
another type of the prelude's, beside `Bytes`, which are the whole
bytes' case kept apart -- the type of most binaries, and the one the
system reads and writes.  The module `bits` (`import bits as Bits`)
has what is done with them: `length` (in bits), `index` (the bit at
`i`, as a `Bool`), `toList` and `fromList` (of `Bool`s), `toBytes`
(`Just` their bytes, where their length is a multiple of 8, else
`Nothing`) and `ofBytes`, `ofInt` (an integer's low `n` bits),
`slice`, `append`, `concat`, `empty` and `eq` (as `==`, whose instance,
`instanceEqualityBits`, is the prelude's).  Their `show` writes them as
a binary does (`instanceShowBits`, below), and their `toString` is
each bit, `0` or `1` (`instanceToStringBits`): `toString b[5::3]` is
`"101"`.  A C program keeps them as a buffer of bytes and how many of
its bits they are, a JavaScript one as a `Uint8Array` whose `bits` is
their length; the bits past them are 0.

### Binaries

A binary, `b[...]`, is `Bytes`, or `Bits`, written as segments, one
after another, as Erlang's and Elixir's `<<...>>` are: `b[1, 2, 255]`,
`b["GET ", path::binary, "\r\n"]`, `b[tag::8, len::16-big,
body::binary]`, `b[flag::1, kind::3, n::12-little]`.  It opens with
`b[`, a `b` where a token starts, right before the `[` (`b [x]` is `b`
applied to a list, and `ab[x]` a name and a list), and each of its
segments is a value, perhaps with a spec after `::`.  The first `::`
outside parentheses ends the value, so cons in one is in parentheses:
`(x :: xs)` (heff's types are written with `:`).  A spec is these,
between `-`s, each at most once:

- what the segment is: `integer`, an `Int`; `binary` (or `bytes`), a
  `Bytes`; `bits` (or `bitstring`), a `Bits`; `float`, a `Float64`,
  IEEE's; or `utf8`, `utf16`, `utf32`, a code point in that encoding,
  written of an `Int` or a `Rune` and read as a `Rune`.  Where nothing
  says, a string is `binary`, a float literal `float`, a rune literal
  `utf8`, and anything else `integer`;
- its size: a number, or `size(n)`, `n` a number or a name, counted in
  its unit: an integer's of 1 to 64 bits (8 where none is said), a
  float's 64 or 32 (64 where none is said), bytes' and bits' all of
  them where none is said (in a pattern, all that are left); a code
  point says none, it is as long as its encoding;
- its unit, `unit(n)`, 1 to 256 bits, what its size counts, as
  Erlang's: an integer's, a float's and bits' 1 where none is said,
  bytes' 8 (and always a multiple of 8) -- `x::size(2)-unit(8)` is an
  integer of 16 bits, `x::binary-size(4)` 4 bytes, and
  `x::binary-size(2)-unit(16)` 4 bytes too.  A unit is said with a
  size;
- an integer's, a float's, UTF-16's and UTF-32's endianness, `big`
  (where none is said) or `little`, and an integer's sign, `unsigned`
  (where none is said) or `signed`, for reading.  Little-endian, an
  integer whose size is no multiple of 8 is Erlang's: its bytes, the
  least significant first, then the bits left, its most significant --
  `b[0x123::12-little]` is `b[0x23, 1::4]`.

A string written is a segment of its bytes, UTF-8, its escapes as a
string's (`b["é"]` is two bytes, `b["\xff"]` one), and takes no spec
but `binary`.  So `b[258::16, 258::16-little, -1::32]` is `b[1, 2, 2,
1, 255, 255, 255, 255]`, and `b['é', 0x1F600::utf8]` is `b[195, 169,
240, 159, 152, 128]`.  Made, an integer is written as its low bits,
two's complement (`b[-1]` is `b[255]`, `b[256]` is not: a number
written must fit, as a signed or an unsigned integer of its size), a
code point that is no Unicode scalar value is abandoned (one written
there is refused), `x::binary-size(n)` writes the first `n` bytes of
`x` and `x::bits-size(n)` the first `n` bits (of fewer, abandoned).
A float of 32 bits is the `Float64` rounded to one, as `float32`'s
`fromFloat` rounds it, and read, made a `Float64` again; a NaN is
written as the quiet one, its sign 0, on every backend.

Which a binary is, `Bytes` or `Bits`, is said where it is elaborated,
of its segments alone, as written: it is `Bits` where one of its
segments is `bits`, or where its segments' sizes, past whole bytes,
add up to no multiple of 8 -- or to what is not known there: a size
that is a name, of a unit that is no multiple of 8 (`x::size(n)`, an
integer of `n` bits), or bits all that are left.  Otherwise it is
`Bytes`.  So `b[5::3]` and `b[x::size(n)]` are `Bits`, and `b[1::4, 2::4]`
and `b[x::size(n)-unit(8)]` are `Bytes`.  Bits whose length is a multiple of 8 are
written with a `bits` segment of nothing, `b[1, 2, b[]::bits]`: a
`bits` segment whose value is a binary written there is that binary
made bits.  A value of the other type is made one by `bits`'s
`ofBytes` and `toBytes`.

A binary in a pattern takes `Bytes`, or `Bits`, apart, by the same
segments and the same rule, read left to right: a segment is a name,
`_`, a number, a string, a float or a rune (one that must be there),
and a size may be a name an earlier segment of the same binary binds --
so a length read is the length of what follows.  Only the last segment
may be bytes or bits of no size, all that are left (bytes, of whole
bytes); else the binary matches what is all read.  A code point is read
where it is well formed: UTF-8 neither overlong, nor a surrogate, nor
past U+10FFFF, nor cut short, UTF-16 a surrogate only of a pair -- else
the binary does not match.  A binary of one segment of bytes,
`b[x::binary]`, matches any bytes, as `x` does.  A value a binary does
not match goes on to the next equation, as one a literal does not, and
a binary is matched where a row matches any value (a variable or `_`)
after it.

```haskell
type Packet = { tag : Int, body : Bytes, rest : Bytes }

-- | >>> maybe b[] (\(p : Packet) -> p.body) (packet b[7, 0, 2, "hi", "!"])
-- | b["hi"]
-- | >>> packet b[7, 0, 9, "hi"]
-- | Nothing
packet : Bytes -> Maybe Packet
packet b[tag::8, len::16-big, body::binary-size(len), rest::binary] =
  Just { tag = tag, body = body, rest = rest }
packet _ = Nothing

-- | >>> List.map method [b["GET /"], b[1, 255, 255], Bytes.fromString "PUT"]
-- | ["get /", "-1", "other"]
method : Bytes -> String
method b["GET ", path::binary] = "get " ^ toString path
method b[1, n::16-little-signed] = show n
method _ = "other"

-- | the runes of UTF-8, an ill-formed byte as U+FFFD
-- | >>> runesOf b["h", 0xff, "é"]
-- | ['h', '\u{FFFD}', 'é']
runesOf : Bytes -> List Rune
runesOf b[c::utf8, rest::binary] = c :: runesOf rest
runesOf b[_::8, rest::binary] = '\u{FFFD}' :: runesOf rest
runesOf _ = []

-- | a header of 4 bits, its length, then that many bits
-- | >>> field b[3::4, 5::3, 1::2]
-- | Just (5)
field : Bits -> Maybe Int
field b[n::4, x::size(n), _::bits] = Just x
field _ = Nothing
```

`show` writes bytes as a binary of them (`instanceShowBytes`, the tools as it
does): each run of printable ASCII, a byte from 32 up to 126, a string
(`"` and `\` escaped), and every other byte its number -- `show b["GET
\xff\r\n"]` is `"b[\"GET \", 255, 13, 10]"`, and `b[]` is none.  Bits
are written the same, their whole bytes as bytes are, then the bits
left, `v::k` (`b["GET", 5::3]`), or, where none are left, `b[]::bits`
(`b[1, 2, b[]::bits]`, and of no bits `b[b[]::bits]`), so that what is
written is `Bits` again (`instanceShowBits`).  What `show` writes is a
binary of the same bytes, or bits.

A binary is made and taken apart where it is elaborated, the same for
every backend.  Of whole bytes, its segments' bytes are put together
(`bytesConcat`, `bytesOfInt`); otherwise its segments are made bits and
put together (`bitsConcat`, `bitsOfInt`, `bitsOfBytes`), and, where
the binary is `Bytes`, made bytes again (`bitsBytes`).  A pattern is a
test, of `Bool`, that reads the value by the prelude's primitives
(`bytesLength`, `bytesIntAt`, `bytesHasAt`; `bitsLength`,
`bitsIntAt`, `bitsSlice`, where it is `Bits` or its segments are not
whole bytes -- a `Bytes` made bits first; a binary written whole
compared by `bytesEq`; a float's bits read by `floatOfBits`, a code
point's by the prelude's `bytesUtf8At` and the like), and then the
names its segments bind, each read again, the equation's rows matched
on with them; offsets and sizes are in bits.

## `do`

A `do` block is a computation written as statements, taken apart
through the `bind` and `pure` in scope, by name (no theory of monads is
asked for): `x <- e` is `bind e (\x -> ...)`, a statement `e` is `bind
e (\_ -> ...)`, `let p = e` binds `p` in the statements after it, and
the last statement, an expression, is the block's value.

Its statements may be imperative too:

```haskell
sumEvens : {e : HFunctor} -> {{io : stdio <: e}} -> List Int -> Co e Int
sumEvens xs = do
  let mut total = 0
  for x in xs do
    if x % 2 /= 0 then continue
    if x > 1000000 then break
    if x < 0 then return (0 - 1)
    total := total + x
  print ("total " ^ show total)
  return total
```

- `let mut x = e` declares a variable of the block (`let mut x : T =
  e`, given its type), and `x := e` sets it.  Another binding of its
  name in the block is refused.  A closure -- a lambda, or a `do`
  block that is not a statement's arm -- holds the value the variable
  has where it is made, and cannot set it.
- `while c do ...` runs its statements while `c` holds; `for p in e
  do ...` runs them for each element of `e`, matched by `p`.  What a
  `for` goes through is `e`'s instance of the prelude's `Iterate`, a
  cursor: `start` gives the first, `next` the element (an `Elem`) and
  the cursor after it, or `Nothing` at the end.  The prelude has a
  list's, a string's (each `Rune`), and a range's, `range a b`, the
  integers from `a` to `b`, both in; `array`
  has an array's (its cursor its index), `std.map` a map's and
  `std.hashmap` a hash map's (each key and its value, a `Pair`).
- `break` leaves the innermost loop, `continue` goes on to its next
  turn, each the last of its statements; `return e` ends the whole
  block with `e` (as `pure e` does last, which it is there).  A
  `while` or `for` last is the block's `MkUnit`.
- An `if` among the statements may have no `else`, and the arms of an
  `if` or a `case` among them are statements too: `x := e`, a jump,
  `return e`, another such `if` or `case`, or a `do` block of
  statements, which are the block's own (its `x := e` sets the block's
  variable).
- `(<- c)` is the value of the action `c` where it is used: it runs
  before the statement it is in, in the order written.  In an arm of an
  `if` or a `case` it runs at the start of that arm, only if the arm is
  taken: where the `if` is a value, it is made an action, the other
  arms `pure` (`print (if b then (<- c) else "x")` is `v <- (if b then c
  else pure "x"); print v`), and so are `&&` and `||`.
- In a lambda in the block, `(<- c)`, `return`, `break` and `continue`
  are refused: its body runs later, not as the block's statement.
- `if let p = e then a else b` is `a` where `e` matches `p`, its
  variables bound there, else `b`: a `case` of `e`, `p` and any other.
  Among the statements it may have no `else`, and its arms are
  statements, as an `if`'s are.

`try` runs its statements over the `exception` effect (the `exception`
module imported, as its names are taken by name, as `bind` is):

```haskell
loadAll : {e : HFunctor} -> {{ex : exception String <: e}} -> {{io : stdio <: e}}
        -> List String -> Co e Int
loadAll names = do
  let mut loaded = 0
  for name in names do
    try
      if name == "" then continue
      load name
      loaded := loaded + 1
    catch (why : String) ->
      print ("not loaded: " ^ why)
  return loaded
```

- A throw of its statements is heard by its `catch` arm, `catch x ->`
  (or `catch (x : T) ->`), which runs in their place.  Which type of
  throws it hears is found from the arm, as `recover`'s handler says it,
  or given, `try (err := T)`.  A `try` has its `catch` arm, under it.
- Its statements and its `catch` arm's are the block's: they set its
  variables, jump and `return`.  A throw abandons what it is thrown in,
  so the `catch` arm sees the variables as they were where the `try`
  began.
- What is let go of however it is left is no part of `try`: a resource
  is held in a `with` of its own effect, its provider's to release
  (std.filesystem's `withFile`, [the batteries](library.md#the-batteries)).

Pure code is written as statements by `run do ...`: `run : Id a -> a`,
of the identity monad (`type Id a = a`), whose `do` is taken apart by
its own `idBind` and `idPure`.

The statements are taken apart into tail calls, through `bind`, never
into a value to be matched later: a loop is a local tail-recursive
function of the block's variables (each a parameter), a jump the call
it leads to -- `continue` the next turn, `break` the function of what
follows the loop, `return e` the block's answer, `pure e` -- and an
`if` whose arms go on to what follows it a function of that too, a join
point (where only one arm goes on, what follows is that arm's own).
Pure code so compiles to plain tail recursion, a loop in `Co` to a loop
that goes round its turns without a handler between them, and neither
grows the stack.

A `try` is a scope, the exception module's `recover`, its `catch` arm
the handler: its statements, one scope further in, give as their answer
what to do once it is left -- at their end the call of what follows it,
a join point; a jump out of `k` scopes, `pure` of what to do once the
next is left, `k` deep -- and once it is left, that is called, a tail
call.  A loop whose turns each go through a `try` leaves each before
the next, so it does not grow the stack either.

## Effects

`lib/co.hf` implements effects after Yang and Wu (POPL 2026).  A program
names the effects it needs as instance arguments, in any world that has
them -- it can do nothing else -- and is given them where it runs:

```haskell
import co
import std.filesystem
import std.io

world : HFunctor
world = stdio <+> filesystem

program : {e : HFunctor} -> {{io : stdio <: e}} -> {{disk : filesystem <: e}} -> Co e Unit
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
elsewhere -- `lib/parallel.hf`'s `Parallel`, a side in a process of its own
-- finds its pure work done already, unless it comes after `later`:
`parallelIn (later (\u -> fib n)) (later (\u -> fib m))`.

An effect the world has is carried out by a provider, its JavaScript
and C sides beside its module: see [providers.md](providers.md).
