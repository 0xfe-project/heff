---
title: Wire and JSON
---

## wire

```haskell
import wire as Wire

type Note = { id : Int, text : String, done : Bool }

instance wireNote : Wire.Wire Note
wireNote =
  Wire.via
    (\(n : Note) -> MkPair n.id (MkPair n.text n.done))
    (\p -> { id = fst p, text = fst (snd p), done = snd (snd p) })

-- | >>> Wire.encode ([1, -1] : List Int)
-- | b[2, 2, 1]
twoInts : Bytes
twoInts = Wire.encode ([1, -1] : List Int)
```

`wire`, imported as `Wire`, turns values into bytes and back. The same
value is the same bytes on every backend, so a C program and a page can
talk.

```haskell
theory Wire (a : Type) where
  shape : String
  encode : a -> Bytes
  decode : Bytes -> Int -> Maybe (Pair a Int)
```

- `Wire.encode x` gives the bytes.
- `Wire.decode b at` reads a value from byte `at`, and where it ends.
- `Wire.decodeAll b` is the value all of `b` is, or `Nothing`.
- `Wire.shapeOf T` is how the type is written, `"List (Pair String Int)"`.
- `Wire.via to from` writes a type of your own as another type, as above.

| Type | Bytes |
|---|---|
| `Int` | zigzag, then LEB128: any size |
| `Int8` … `UInt64` | its bits, big-endian |
| `Float64`, `Float32` | IEEE 754, big-endian |
| `Rune` | its code point, 32 bits |
| `Bool` | a byte, 0 or 1 |
| `Unit` | nothing |
| `String`, `Bytes` | its length, then its bytes |
| `Bits` | its length in bits, then its bits |
| `List`, `Array` | its length, then each element |
| `Maybe`, `Either` | a byte, 0 or 1, then the value |
| `Pair` | the first, then the second |
| `Map`, `HashMap` | its size, then each key and value |

An instance is in scope only where its module is imported: a module
using one of `wire`'s instances imports `wire` itself.

## json

```haskell
import json as Json

-- | >>> uri "{\"params\": {\"textDocument\": {\"uri\": \"file:///a.hf\"}}}"
-- | Just ("file:///a.hf")
uri : String -> Maybe String
uri body =
  either (\e -> Nothing) (\j -> Json.str ["params", "textDocument", "uri"] j) (Json.parse body)
```

`json`, imported as `Json`, is JSON (RFC 8259), written in heff.

```haskell
data Json
  = JNull
  | JBool Bool
  | JInt Int
  | JFloat Float64
  | JString String
  | JArray (List Json)
  | JObject (List (Pair String Json))
```

- `Json.parse s` is `Right` the value, or `Left` a `ParseError`: the byte
  where it went wrong and what was expected. `Json.describe` prints it.
- `Json.stringify j` writes it as JavaScript's `JSON.stringify` does.
- `Json.field k j` and `Json.at path j` find values; `Json.str`,
  `Json.int`, `Json.bool` and `Json.arr` read one at a path. Each is
  `Nothing` where it is missing or of another kind.
- `Json.obj`, `Json.kv`, `Json.num` and `Json.jstr` build one.

Details:

- A number without a fraction or exponent is a `JInt`, of any size.
- An object keeps its fields in order, and a key given twice twice.
- Parsing does not use the stack, so any depth is fine.
