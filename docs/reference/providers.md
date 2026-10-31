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

Below the providers are the primitives, such as
`primitive intAdd : Int -> Int -> Int`, which the runtime has; only the
library declares them.

## Where each runs

- `lib/std/`'s effects are the system's, with C providers: `std.io`,
  `std.filesystem`, `std.process`, `std.progress`, `std.http`,
  `std.net`, `std.reactor`, `std.signal`, `std.terminal`.
- `std.crypto`'s signatures are C's alone, OpenSSL's libcrypto (3.0 or
  later) linked into the program by pkg-config's `libcrypto`: a program
  importing it is built where libcrypto's headers and pkg-config are.
- `std.reference` and `std.time` have both a C and a JavaScript
  provider.
- `lib/browser/`'s are the browser's, JavaScript only: `browser.dom`,
  `browser.http`, `browser.gpu` (WebGPU's; where the browser has no
  WebGPU, `open` gives `Left`), and `browser.js`, which has only a type.

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

In C, a slice of 64 bytes or more, and at least a quarter of its string,
shares the string's bytes instead of copying them. A shorter slice is a
copy, so a small slice never keeps a large string alive.

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
import ffi as FFI

filesInC : FFI.Described
filesInC =
  FFI.describe
    ["<stdio.h>"]
    [FFI.operation "Remove" (FFI.fun "remove" [FFI.string] FFI.doneOrErrno)]
```

An operation that is one C call need not be written in the provider: a
definition of type `FFI.Described` in the effect's module describes it,
and the compiler writes the glue.

`FFI.describe headers operations` lists the headers to include and the
operations. Each operation names its tag and a C function: the
function's name, its arguments (the operation's fields but the last),
and its result.

| Argument type | heff type |
|---|---|
| `FFI.int`, `FFI.int64`, `FFI.integer "mode_t"` | `Int` |
| `FFI.double` | `Float64` |
| `FFI.bool` | `Bool` |
| `FFI.string`, `FFI.text` | `String`, as `const char *` |
| `FFI.bytes` | `Bytes`, as two arguments: `const unsigned char *` and `size_t` |
| `FFI.pointer "FILE" "fclose"` | a host type; `fclose` lets it go |

| Result | heff type |
|---|---|
| `FFI.gives t` | the value; `FFI.void` gives `MkUnit` |
| `FFI.givesOrErrno t` | `Either Errno a`: `-1` or `NULL` with `errno` is `Left` |
| `FFI.doneOrErrno` | `Either Errno Unit` |
| `FFI.ownedString` | a `char *` from `malloc`, copied and freed |
| `FFI.bytes` | `Bytes` from a `const unsigned char *`, copied |
| `FFI.ownedBytes` | `Bytes` from an `unsigned char *` from `malloc`, copied and freed |

Bytes cross with their length, so a NUL among them is a byte like any
other. As a result, the function takes one more argument after the
described ones, a `size_t *`, and puts the length there:

```haskell
FFI.fun "cc_blit" [FFI.int64, FFI.bytes] (FFI.gives FFI.void)
-- void cc_blit(int64_t y, const unsigned char *p, size_t n);

FFI.fun "zz_pack" [FFI.bytes, FFI.int] (FFI.givesOrErrno FFI.ownedBytes)
-- unsigned char *zz_pack(const unsigned char *p, size_t n, int level, size_t *len);
```

A bytes result is `NULL` only on failure, as a string's is. Return a
pointer even for no bytes, with the length 0.

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

## Libraries and frameworks

A module whose C uses a library that is not the runtime's says so with a
definition of type `FFI.Links`. Every program that imports the module is
built with it, by `heff build`, `heff run` and `heff test`:

```haskell
import ffi as FFI

unibilium : FFI.Links
unibilium = { pkgConfig = ["unibilium"] }
```

| Field | What `zig cc` is given |
|---|---|
| `pkgConfig` | `pkg-config --cflags` and `--libs` of these packages |
| `libs` | `-lNAME` for each name |
| `frameworks` | `-framework NAME` for each name, on macOS; nothing elsewhere |

Each field defaults to `[]`. The provider, or the module's description,
then includes the library's own header and calls its functions directly:
the C compiler checks each call against the header, and the library is
linked, so no `dlopen` or `dlsym` table is needed.

```haskell
cocoa : FFI.Links
cocoa = { libs = ["objc"], frameworks = ["AppKit", "CoreGraphics"] }
```

- The fields hold names, not flags: a name starting with `-` is an
  error.
- A flag several modules ask for is given once.
- When neither pkg-config nor the C compiler knows where a library is,
  the program's `build.hf` says where to look, with `includeDirs` and
  `libDirs` (see [Projects](projects.md#programs)).
- Programs are built for the machine they are built on, so pkg-config
  gives that machine's flags. A provider that uses a framework on macOS
  and something else elsewhere keeps the macOS part under
  `#ifdef __APPLE__`.
- `heff compile --backend=c FILE OUT` writes the flags beside the C, a
  flag per line: `OUT.link` for one file, `OUT/heff.link` for a
  directory.

## The first thread

A C program runs on a thread of its own, with a large stack; the
process's first thread only waits for it to end. Some toolkits must
have the first thread: AppKit's and UIKit's run loops, and every window
call, belong there. A provider lends it to one:

```c
int heff_first_thread(void (*run)(void *), void (*stop)(void *), void *u);
int heff_is_first_thread(void);
```

`heff_first_thread` queues `run(u)` to run on the first thread, and
says:

| Result | Meaning |
|---|---|
| `0` | queued: `run(u)` runs as soon as the first thread is free |
| `1` | a job is lent already, queued or running: one at a time |
| `2` | the program is ending: nothing is run |

`run` may return, and the first thread waits again for another job or
the program's end; or it may never return, and the thread is the
toolkit's from then on. `heff_is_first_thread()` says whether the
caller is on the first thread.

- No heff code runs on the first thread. The program asks the toolkit
  for something by the toolkit's own way to its thread, such as
  `dispatch_sync_f(dispatch_get_main_queue(), ...)` on macOS; the
  toolkit tells the program something through the provider, such as a
  queue and a condition variable, or a byte on a pipe the reactor waits
  on.
- When the program ends while a job runs there, however it ends (its
  `main` returning, `exit n`, a failure no one caught):
  - with `stop`, the ending thread calls `stop(u)`, which must make
    `run` return, and waits; the first thread then ends the process
    with the program's status, its output flushed and its `atexit`
    handlers run there. Nothing times the wait out: winding the toolkit
    up is the provider's job. `stop` for AppKit sends `[NSApp stop:nil]`
    to the main queue and posts an event, by which the loop notices it;
    for a bare `CFRunLoopRun`, a function sent to the main queue
    (`dispatch_async_f`) that calls `CFRunLoopStop(CFRunLoopGetCurrent())`.
    `run` may return on its own just as `stop` is called, so `stop` must
    not wait for the loop.
  - with `stop` `NULL`, the program's end does not wait for the first
    thread: the ending thread flushes the output and exits with the
    program's status.
- A job queued but not begun when the program ends is not run.
- A stack overflow ends the program at once, without `stop`.
- A program that lends nothing ends exactly as before.
- Linux toolkits (GTK) and Win32's message loop do not need the first
  thread; their providers do not lend it.

## Host types

A thing of the host the program holds, such as an open file, a socket
or a page's element, has a host type: a `data` with no tags,
`data File`. No program can make one or take one apart; only
providers can.

- In C, `heff_foreign(p, let_go)` wraps a pointer, and
  `heff_foreign_ptr(v)` reads it back. `let_go(p)` runs as soon as the
  program drops its last reference. A provider given another provider's
  thing should check it is its own before trusting the pointer.
- In JavaScript, a foreign value is the host's object itself. A thing
  the host does not collect on its own, such as a GPU buffer, should be
  let go of by a `FinalizationRegistry`.

## Scoped operations

A thing that must be let go of, such as an open file, is acquired and
let go of by operations of the effect's own, such as `std.file`'s `open`
and `close`, and held by a `with` that does both around its body, such
as `with f := File.open path do`. See [a thing held by a
`with`](../api/resource.md).

- The provider lets go of the thing once: an operation on it after is
  an error of the effect's (a file's, `EBADF`), never one on a thing
  acquired since, and letting go of it again does nothing.
- A thing the program drops without letting go of it is let go of by
  its finalizer.
- A module whose things must not outlive a body exports its `with`
  alone: the body gets a `With s a`, whose region `s` keeps it from
  escaping, and its operations take the `With s a`, giving the thing to
  the provider by `inside`.

The provider makes sure the thing is let go of:

1. Acquiring gives a foreign value whose `let_go` releases it, for when
   the program drops it early, such as after a throw.
2. When the body ends, the `with` releases it by an operation of the
   effect, such as `CloseFile`, and marks it released. An operation
   given it after that fails as the host does, `EBADF`.

## Operations that wait

In C, an operation that must wait, such as a read from a socket that is
not ready, is done in steps by the runtime's reactor, so the other
acts and behaviours of boc's go on meanwhile, an act awaited being
started in a group of its worker's reactor. The provider's operation is a `HeffWait`, the first
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
