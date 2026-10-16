---
title: Bytes, bits and binaries
---

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
```

## Bytes

`Bytes` are bytes that are not text: a request body, what a socket
read, a file's content. They never change once made.

What is done with them is in [`bytes`](../../api/bytes.md), imported as
`import bytes as Bytes`; `==` works without importing it.

`toString` of bytes is their UTF-8 text, with U+FFFD for what is not
UTF-8. `show` writes them as a binary.

## Bits

`Bits` are a bitstring of any length. `Bytes` are the case of whole
bytes, kept apart because it is the common one.

What is done with them is in [`bits`](../../api/bits.md), imported as
`import bits as Bits`.

`toString` of bits is each bit, `0` or `1`: `toString b[5::3]` is
`"101"`.

## Binaries

A binary, `b[...]`, writes `Bytes` or `Bits` as segments, as Erlang's
`<<...>>` does:

```haskell
import bytes as Bytes

request : String -> Bytes
request path = b["GET ", Bytes.fromString path::binary, "\r\n"]

header : Int -> Int -> Bytes
header tag len = b[tag::8, len::16-big]
```

- `b[` must be written together; `b [x]` is `b` applied to a list.
- Each segment is a value, then optionally `::` and a spec.
- The first `::` ends the value, so a cons in a segment needs
  parentheses: `(x :: xs)`.

### Specs

A spec is these parts, joined by `-`, each at most once:

| Part | Values | Default |
|---|---|---|
| type | `integer`, `binary` (or `bytes`), `bits` (or `bitstring`), `float`, `utf8`, `utf16`, `utf32` | a string is `binary`, a float literal `float`, a rune literal `utf8`, anything else `integer` |
| size | a number, or `size(n)` with `n` a number or a name | an integer 8, a float 64, bytes and bits all of them |
| unit | `unit(n)`, 1 to 256: what the size counts, in bits | 1, or 8 for bytes |
| endianness | `big`, `little` | `big` |
| sign | `unsigned`, `signed` (for reading) | `unsigned` |

- An integer is 1 to 64 bits. A float is 64 or 32.
- A code point, `utf8` and the others, is as long as its encoding, and
  is written from an `Int` or a `Rune` and read as a `Rune`.
- `x::size(2)-unit(8)` is a 16-bit integer; `x::binary-size(4)` is 4
  bytes.
- A string segment is its UTF-8 bytes, and takes no spec but `binary`.

So `b[258::16, 258::16-little, -1::32]` is
`b[1, 2, 2, 1, 255, 255, 255, 255]`.

When writing:

- an integer is written as its low bits, but must fit its size as a
  signed or unsigned number: `b[-1]` is `b[255]`, and `b[256]` is
  refused;
- `x::binary-size(n)` writes the first `n` bytes of `x`;
- a 32-bit float is the `Float64` rounded.

### Bytes or Bits

A binary is `Bits` when one of its segments is `bits`, or when its sizes
do not add up to whole bytes, or might not. Otherwise it is `Bytes`.
`b[5::3]` is `Bits`; `b[1::4, 2::4]` is `Bytes`. To write `Bits` of
whole bytes, end with an empty `bits` segment: `b[1, 2, b[]::bits]`.

### Patterns

```haskell
-- | >>> List.map method [b["GET /"], b[1, 255, 255], Bytes.fromString "PUT"]
-- | ["get /", "-1", "other"]
method : Bytes -> String
method b["GET ", path::binary] = "get " ^ toString path
method b[1, n::16-little-signed] = show n
method _ = "other"

-- | >>> field b[3::4, 5::3, 1::2]
-- | Just (5)
field : Bits -> Maybe Int
field b[n::4, x::size(n), _::bits] = Just x
field _ = Nothing
```

A binary pattern takes a value apart by the same segments, left to
right:

- A segment is a name, `_`, or a literal that must be there.
- A size can be a name bound by an earlier segment of the same
  pattern, so a length can be read and then used.
- Only the last segment may have no size: it takes all that is left.
- A code point matches only if it is well-formed.
- A value that does not match goes on to the next equation.

## How they are shown

`show` writes bytes as a binary: runs of printable ASCII as strings,
other bytes as numbers. `show b["GET \xff\r\n"]` is
`"b[\"GET \", 255, 13, 10]"`. Bits are written the same, then the bits
left, `b["GET", 5::3]`.
