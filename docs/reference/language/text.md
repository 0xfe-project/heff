---
title: Runes and strings
---

```haskell
-- | >>> greeting
-- | "héllo, \"😀\"\n"
greeting : String
greeting = "h\u{E9}llo, \"" ^ "\u{1F600}\"\n"

-- | >>> List.map vowel ['a', 'é', 'z']
-- | [True, True, False]
vowel : Rune -> Bool
vowel 'a' = True
vowel 'é' = True
vowel _ = False
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

`show 'a'` is `"'a'"`; `toString 'a'` is `"a"`.

## Strings

A `String` is bytes, as Go's is. A literal is UTF-8 (`"é"` is two
bytes), but a string may hold any bytes, such as what a file gave, or
`"\xff"`.

- A string literal is in double quotes, and can be a pattern.
- Its escapes are `\n`, `\t`, `\r`, `\u{...}` (a code point, in UTF-8),
  `\xHH` (one byte), and `\` before any other character, which is that
  character: `\"`, `\\`.

`show "hi"` is `"\"hi\""`; `toString "hi"` is `"hi"`.

## Operations

What is done with them is in the library:

- strings: the [prelude](../../api/prelude.md), such as
  [`strLength`](../../api/prelude.md#strLength),
  [`runes`](../../api/prelude.md#runes),
  [`fromRunes`](../../api/prelude.md#fromRunes) and
  [`split`](../../api/prelude.md#split); `^` joins two, and groups to
  the left: `s ^ a ^ b` is `(s ^ a) ^ b`, so a string that nothing else
  holds is grown in place; [`mapBytes`](../../api/prelude.md#mapBytes),
  [`mapRunes`](../../api/prelude.md#mapRunes) and
  [`foldBytes`](../../api/prelude.md#foldBytes) go through a string
  byte by byte or rune by rune without making a list of them;
- runes: [`rune`](../../api/rune.md), imported as `import rune as Rune`.

JavaScript's own UTF-16 text is another type, `JSString`: see
[Providers](../providers.md#jsstring).
