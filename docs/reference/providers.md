---
title: Providers
---

An effect of the outside world is carried out by its provider: code of
the host beside the effect's module.

| File | Side |
|---|---|
| `M.hf` | the effect: its operations, in heff |
| `M.provider.js` | what the operations do in JavaScript |
| `M.provider.c` | what they do in C |

`lib/browser/dom.provider.js` shows the shape of a JavaScript provider.

## Where each runs

- `lib/std/`'s effects are the system's, with C providers: `std.io`,
  `std.filesystem`, `std.process`, `std.progress`, `std.http`,
  `std.net`, `std.thread`, `std.reactor`, `std.terminal`.
- `std.reference` and `std.time` have both a C and a JavaScript
  provider.
- `lib/browser/`'s are the browser's, JavaScript only: `browser.dom`,
  `browser.http`, and `browser.js`, which has only a type.
- `gpu` has a WebGPU provider in JavaScript. Where the browser has no
  WebGPU, `open` gives `Left`.

A JavaScript provider that only runs in a page says so:
`export const $host = "page"`.

A backend refuses what it cannot run, at the import:

- a C program importing a module of `lib/browser/` is E0351;
- a page importing an effect with only a C provider, or a module that
  imports one, is E0352. A module that imports only pure modules, such
  as `std.path`, is fine.

An effect of a program's own with no provider for the backend is an
error where it is used: it "is not provided here".

## How values cross

### Strings

A provider gives and takes strings only through the runtime's helpers:

| | Text | Bytes |
|---|---|---|
| JavaScript | `$toJs`, `$fromJs` | `$toBytes`, `$fromBytes` |
| C | `heff_str_text` | `heff_str_bytes`, `heff_cstr` |

The C helpers are in `lib/c/runtime.c`, under "What providers share".

- Content and paths are bytes as they are: a file's content, a
  directory's names, what a command wrote.
- Names are text: arguments, the environment, the working directory, a
  command and its arguments, and the DOM's and JSON's strings. What is
  not UTF-8 becomes U+FFFD.
- A path with a NUL in it fails the operation.

### Bytes

In JavaScript, `Bytes` are a `Uint8Array`, never changed once given.
In C, a provider makes them with `heff_bytes_in` (copying `n` bytes) and
reads them with `heff_bytes_data`.

### JSString

`browser.js`'s `JSString` is JavaScript's own UTF-16 text. `toJS` makes
one from a `String`, and `fromJS` turns one back. A `JSString` field
reaches a provider as a plain JavaScript string, with no helper.

An ES module (`--backend=esm`) gives and takes values as they are, so a
function for JavaScript to call should take and give `JSString`s.

### Numbers and runes

| Type | JavaScript | C |
|---|---|---|
| `Int`, `Int64`, `UInt64` | BigInt | a word, boxed past 62 bits |
| other integers, `Rune` | number | a word (`INT_OF`, `MK_INT`) |
| `Float32` | number | a word of its bits (`heff_f32_of`, `heff_f32`) |
| `Float64` | number | boxed (`FLT_OF`, `heff_flt`) |

In C, a record's or tag's `Float64`, `Int64` and `UInt64` fields may be
stored flat. A provider reads a record's field with `heff_rget_v`, and a
tag's with `heff_tget_v`, never directly.

## Operations described in C

```haskell
import ffi as Ffi

threadsInC : Ffi.Described
threadsInC =
  Ffi.describe
    []
    [Ffi.operation "Cores" (Ffi.fun "th_cores" [] (Ffi.gives Ffi.int64))]
```

An operation that is one C call need not be written in the provider: a
definition of type `Ffi.Described` in the effect's module describes it,
and the compiler writes the glue.

`Ffi.describe headers operations` lists the headers to include and the
operations. Each operation names its tag and a C function: the
function's name, its arguments (the operation's fields but the last),
and its result.

| Argument type | heff type |
|---|---|
| `Ffi.int`, `Ffi.int64`, `Ffi.integer "mode_t"` | `Int` |
| `Ffi.double` | `Float64` |
| `Ffi.bool` | `Bool` |
| `Ffi.string`, `Ffi.text` | `String`, as `const char *` |
| `Ffi.pointer "FILE" "fclose"` | a host type; `fclose` lets it go |
| `Ffi.sharedPointer T drop hold` | a host type that can cross places |

| Result | heff type |
|---|---|
| `Ffi.gives t` | the value; `Ffi.void` gives `MkUnit` |
| `Ffi.givesOrErrno t` | `Either Errno a`: `-1` or `NULL` with `errno` is `Left` |
| `Ffi.doneOrErrno` | `Either Errno Unit` |
| `Ffi.ownedString` | a `char *` from `malloc`, copied and freed |

- The compiler checks the description against the effect: a wrong tag,
  a wrong number of fields, or a field that cannot cross is an error.
- The C compiler checks each function against its header. A function
  that is a macro cannot be checked: describe a wrapper.
- A string argument with a NUL in it fails the operation with `EINVAL`.
- A module whose operations are all described needs no `M.provider.c`.

A hand-written provider can pass the operations it does not do to the
glue, `heff_described_M`:

```c
static V heff_described_threads(V p, V k);

static V heff_provider_threads(V p, V k) {
  switch (heff_tag_of(p)) {
  case TAG_Pool: ...
  default:
    return heff_described_threads(p, k);
  }
}
```

## Host types

A thing of the host the program holds, such as an open file, a socket
or a page's element, has a host type: a `data` with no tags,
`data OpenFile`. No program can make one or take one apart; only
providers can.

- In C, `heff_foreign(p, let_go)` wraps a pointer, and
  `heff_foreign_ptr(v)` reads it back. `let_go(p)` runs as soon as the
  program drops its last reference. A provider given another provider's
  thing should check it is its own before trusting the pointer.
- `heff_foreign_shared(p, let_go, share)` makes a thing that can be sent
  to another place: `share(p)` holds it once more. Any other foreign
  value cannot cross places.
- In JavaScript, a foreign value is the host's object itself. A thing
  the host does not collect on its own, such as a GPU buffer, should be
  let go of by a `FinalizationRegistry`.

## Scoped operations

A thing that must be let go of, such as an open file, is handed out by a
`with` of the effect's own, such as `std.filesystem`'s `withFile`:

- The body gets the thing as a `With s a`, with a region `s` that keeps
  it from escaping. See [a thing held by a `with`](library/with.md).
- The effect's operations on it take the `With s a`, and give the thing
  to the provider by `inside`.
- The module exports its `with` and those operations, never the bare
  acquire.

The provider makes sure the thing is let go of:

1. Acquiring gives a foreign value whose `let_go` releases it, for when
   the program drops it early, such as after a throw.
2. When the body ends, the `with` releases it by an operation of the
   effect, such as `CloseFile`, and marks it released. An operation
   given it after that fails as the host does, `EBADF`.

## Operations that wait

In C, an operation that must wait, such as a read from a socket that is
not ready, is done in steps by the runtime's reactor, so green threads
go on meanwhile. The provider's operation is a `HeffWait`, the first
member of a struct of its own:

| Member | Meaning |
|---|---|
| `fd`, `events` | what it waits for (`POLLIN`, `POLLOUT`); `fd` -1 for none |
| `until` | a deadline in `heff_now_ms` milliseconds, or 0 |
| `step(w)` | do what can be done now: `HEFF_STEP_DONE`, or `HEFF_STEP_WAIT` |
| `answer(w)` | its answer, or why waiting failed (`w->err`) |
| `cancel(w)` | undo it |

The provider takes the first step itself. If it is not done, it returns
`heff_outside(w, f, k)`: outside `std.reactor`, this waits there; inside,
the wait stays pending while other threads run, and `f` of its answer
goes to `k` when it is done.

- A provider whose waits are done by something else, such as libcurl or
  a child process, is a `HeffSource` (`heff_reactor_source`).
- A provider that closes a descriptor others may wait on calls
  `heff_reactor_closing(fd)` first.
- What has no descriptor, such as a name lookup, runs in a system thread
  that writes to a socket pair when done.

See `lib/c/runtime.c`, "Outside operations done later", and
`lib/std/net.provider.c` for an example.
