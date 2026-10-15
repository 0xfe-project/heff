---
title: Runes and strings
---

```haskell
import rune as Rune

-- | >>> initials "héllo"
-- | "Hé"
initials : String -> String
initials s = ofRunes (List.map Rune.toUpper (List.take 2 (runes s)))
```

## Runes

A `Rune` is a Unicode code point, as Go's `rune` is.

- A rune literal is one character in single quotes: `'a'`, `'é'`, `'😀'`.
- Escapes are a string's: `'\n'`, `'\t'`, `'\r'`, `'\u{1F600}'`, and `\`
  before any other character is that character, `'\''`, `'\\'`.
- It must be a Unicode scalar value: `'\u{D800}'`, a surrogate, is an
  error.
- A `'` inside a name is a prime: `x'`.
- A rune literal can be a pattern.
- A rune is not an integer: `'a'` is no `Int`, and `97` is no `Rune`.

The `rune` module (`import rune as Rune`) has:

- comparisons: `eq`, `compare`;
- `toInt`, and `fromInt`, which wraps to 32 bits as Go's `rune(n)`
  does; `isValid` says whether a rune is a scalar value;
- `width` (its bytes in UTF-8), `count` (a string's runes), `foldl`,
  `validString`, and `toValid`, which turns what is not UTF-8 into
  U+FFFD;
- ASCII's classes and cases: `isDigit`, `isAlpha`, `isSpace`,
  `isUpper`, `toUpper` and more. `isAlpha 'é'` is `False`.

`show 'a'` is `"'a'"`; `toString 'a'` is `"a"`.

## Strings

A `String` is bytes, as Go's is. A literal is UTF-8 (`"é"` is two
bytes), but a string may hold any bytes, such as what a file gave, or
`"\xff"`.

- `strLength`, `byteAt`, `strSlice` and `strLt` work on bytes. `byteAt`
  gives -1 outside the string.
- `runes` splits a string into its runes, and `ofRunes` joins them.
- `runeAt s i` is the rune starting at byte `i`, and how many bytes it
  takes.
- What is not UTF-8 reads as U+FFFD, one for each longest broken start.
- `^` joins strings; `joinWith`, `split` and `lines` are in the prelude.

On the C backend, a slice of 64 bytes or more, and at least a quarter of
its string, shares the string's bytes instead of copying them. A shorter
slice is a copy, so a small slice never keeps a large string alive.

JavaScript's own UTF-16 text is another type, `JSString`: see
[Providers](../providers.md#jsstring).
