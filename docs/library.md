# The library

`lib/` goes with the compiler, its version the compiler's; `HEFF_LIB`
says where it is, else it is the `lib/` the compiler came with: the
repository's, a release's, or the heff home's (`heff install` puts it
there).  Its modules alone declare primitives --
`primitive intAdd : Int -> Int -> Int`, a function the runtime has --
and each primitive once: one a module of a program declares, or one
declared again, is an error.

heff's own repository is the library's source.  Its `package.hf` names
the package `heff`, which the compiler keeps for itself (see
[projects](projects.md#heffs-own-package)): where heff is from, that
project's `lib/` is the library, whatever `HEFF_LIB` or the installed
heff says.  So an installed heff, or one built in another checkout,
checks the repository's `lib/float32.hf` as the library's, its
primitives its own and declared once.  Any other project's `lib/` is no
library, and a project elsewhere that names itself `heff` is an error
(E0347), its `lib/` no library either.

## Modules

Every module has `builtin`, `prelude` and `list` (as `List`) without
writing them ([how names are found](language.md#modules)).  The others
are imported by their names:

- `co` ([effects](language.md#effects)), and effects made with it:
  `state`, `exception` (`throw` and `recover`, which a `do` block's
  `try` is ([`do`](language.md#do)); `divOrThrow` and `modOrThrow`, a division
  that throws by zero; and `attempt`, a handler of it: what was thrown
  and heard by no `recover`, `Left`), `parallel` and `green` ([green
  threads](#green-threads));
- `array` ([arrays](#arrays)), `std.map` and `std.set`, by the keys'
  `Order`, found as an instance argument is, the map first:
  `Map.insert m k v`, `Map.lookup m k`, `Map.member m k`, `Map.delete
  m k` (a map's `update` and `alter` change what a key is bound to
  in one walk down the tree, where a `lookup` and an `insert` take two:
  `Map.update counts word 1 (\new old -> new + old)` counts a
  word);
- `std.hashmap` (as `HashMap`), maps by the keys' hashes, a hash array
  mapped trie as Clojure's and unordered-containers' are: each level 5
  bits of the hash, a node's children in an array as long as how many
  there are, found by a bitmap of which there are -- 4 levels for a
  million keys, a key compared once, at the end of its walk.  A node
  nothing else holds is changed in place, so `HashMap.update counts
  word 1 (\new old -> new + old)` builds nothing for a word counted
  already.  Its keys' type has a `Hash` (`theory Hash (a : Type) where
  hash : a -> Int`: `String`'s, `Int`'s, `Rune`'s, `Bool`'s, and a
  `Pair`'s, a `List`'s and a `Maybe`'s of types that have one --
  `instanceHashString` and the others) and an `Equality`, each found
  as an instance argument is.  The map comes first, as a `std.map`'s
  does.  It has `empty`, `insert`, `update`,
  `alter`, `lookup`, `delete`, `size` (each key counted), `toList`,
  `fromList` and `foldl`; the order of `toList` and `foldl` is the
  trie's, by the hashes, not the keys', but the same on every backend.
  A string's hash is FxHash's step (`rotl(h, 5) ^ w`, times its
  constant) over its bytes 8 at a time, then murmur3's 64-bit finish,
  which FxHash lacks, so that the low bits a trie looks at first are
  mixed; an integer's is that finish of its low 64 bits;
- `check` and `gen`, the tests and their generators
  ([tests.md](tests.md#unit-tests-and-properties)), and `bench`, the
  benchmarks ([benchmarks.md](benchmarks.md));
- `manifest`, the types of `package.hf` and `build.hf`
  ([projects.md](projects.md));
- `ffi`, C's types as values, by which an effect's module says which
  of its operations C functions do, the compiler making that side of
  the provider ([providers.md](providers.md#operations-described-in-c));
- the fixed-width numbers' `int8` to `int64`, `uint8` to `uint64` and
  `float32`, `int` with an `Int`'s bits, and `rune` with a `Rune`'s
  operations ([numbers](language.md#numbers),
  [runes and strings](language.md#runes-and-strings));
- `bytes`, the operations of `Bytes` ([bytes](language.md#bytes)),
  which a binary, `b[...]`, writes and takes apart
  ([binaries](language.md#binaries));
- `wire` ([the wire](#the-wire)), values as bytes and back, the same
  bytes on every backend, and `remote`, an effect's operations sent
  over it to another program ([a client and its server](remote.md));
- `json` ([JSON](#json)), JSON's values, read from their text and
  written back, in heff, with no provider;
- `with` ([a thing held by a `with`](#a-thing-held-by-a-with)), the
  one `With s a` every effect's scoped resources are had by, and
  `scoped`, the bracket they are made of;
- `bits`, the operations of `Bits`, a bitstring of any length
  ([bits](language.md#bits)): `length` (in bits), `index` (a bit, as a
  `Bool`), `toList` and `fromList`, `toBytes` (`Just` the bytes, of a
  length that is a multiple of 8) and `ofBytes`, `ofInt` (the low `n`
  bits of an integer), `slice` (from bit `i` up to `j`, clamped),
  `append`, `concat`, `empty` and `eq`.

## Arrays

`array` (as `Array`) is arrays: a fixed number of elements, each
reached at once by its index.  An array is a value, never changed: `set
xs i x` is another array, with `x` at `i`, and `xs` is as it was.  So
`set` writes in place where nothing else holds `xs` -- its reference
count is 1, the array given to `set` its last use -- and copies it, all
of it, where something does.  An array threaded through a loop, each
round's array given to the next, is so changed in place, as a mutable
one is, and a sieve is a sieve:

```haskell
cross : Int -> Int -> Int -> Array UInt8 -> Array UInt8
cross n p j xs =
  if j >= n then xs else cross n p (j + p) (Array.set xs j 0)
```

An array read after it is set, or kept elsewhere, is copied at that
`set`, once: the copy is the array's, written in place from then on.
`modify`, `swap`, `fill` and `reverse` are `set`s, in place as it is.
Besides them, `length`, `index` (`Maybe` an element), `generate`,
`replicate` (`n` elements, each `x`), `fromList`, `toList`, `empty`,
`map`, `slice` (from `i` up to `j`, clamped, a negative one from the
end, as a string's), `append`, `foldl`, `foldr` and `sortBy` (stable,
as a list's).

An array of a number's type is kept flat by the C backend, as C's array
of it, not as a boxed value for each element: `Array Float64` is
`double`s, `Array Int64` `int64_t`s, `Array UInt8` bytes, and so on for
each fixed-width type.  It is flat where it is built knowing that the
element is a number -- `replicate` of a number's literal, or of a
number the compiler keeps unboxed there -- and an array of `Float64`s
always, however it is built.  Another built where its type is not known
(`generate` of a function, `fromList`, `map`) holds boxed values.  A read
where the type is known reads the number as it is kept; one where it is
not boxes it; a `set` of a number writes it as the array keeps it.  The
JavaScript backend keeps an array of JavaScript's numbers as a
`Float64Array`.

## The batteries

The batteries ship with it, in two halves, by where a program runs.
What is above, in `lib/`, is pure, the same on every backend.

`lib/std/` is the system's, for the C backend (`--backend=c`):

- `std.io` (the standard streams), `std.filesystem` (and
  `std.filesystem.held`, files held by a `with`), `std.process`,
  `std.http` (an HTTP client), `std.net` (TCP sockets), `std.progress`
  (what a program is doing, drawn on a terminal), `std.thread` (threads
  truly at once: a pool, spreads, and `Parallel` handled by them --
  [threads.md](threads.md)), `std.reactor` (outside operations begun
  now and finished later, for [green threads](#green-threads)),
  `std.terminal` (keys typed, a screen drawn: [the terminal](#the-terminal)):
  effects of the system, C's alone;
- `std.http.server`, an HTTP/1.1 server written over `std.net`, C's
  as `std.net` is, and `std.remote`, an API's server over it and its
  client by `std.http` ([a client and its server](remote.md));
- `std.reference` (the world's cells) and `std.time`: effects needed on both
  sides, one module each with a provider in C and one in JavaScript,
  which a page's script has too;
- `std.handle` (the world's things, which one effect hands out and
  another takes), `std.path`, `std.cli` (command lines) and
  `std.cli.interactive` (a program's commands as an app, to learn them
  by), `std.terminal.widget` (keys, screens and widgets, as values),
  `std.doc` (documents laid out) and `std.math` (a float's functions:
  `sin`, `atan2`, `exp`, `log`, `pow`, `round`, `pi`, `e` and the
  like): pure, for any backend.

`lib/browser/` is the browser's, for JavaScript (`--backend=page`, and
`esm`):

- `browser.dom` (web pages);
- `browser.http` (`fetch`, an HTTP client of `Bytes` bodies, as
  `std.http`'s shapes are) and `browser.remote`, an API's client by it
  ([a client and its server](remote.md));
- `browser.js` (JavaScript's strings, `JSString`).

`gpu` (lib/gpu.hf) is a GPU as an effect, `import gpu as Gpu`: a
device opened (`Gpu.open`), buffers of bytes on it made, written, read
and let go of (`newBuffer`, `writeBuffer`, `readBuffer`, `freeBuffer`),
a kernel loaded from its code and its entry point (`load`), and run
over a grid of workgroups with buffers bound, the first at
`@binding(0)` (`dispatch`), each answered when the GPU is done, an
`Either GpuError a`.  Its provider is WebGPU's, a page's, so a kernel
is WGSL; the example page vecadd (`examples/pages/`) adds two vectors
of Float32s by one.

A kernel is written in heff, by `gpu.kernel`'s combinators (`import
gpu.kernel as Kernel`), not quoted: an expression of the kernel is an
`Exp a`, `a` a `Float32`, an `Int32`, a `UInt32` or a `Bool` (the
theory `Scalar`), built by the prelude's operators at `Exp a` -- `x +
y`, `x * y`, `-x`, `x /. y` of floats, `x / y` and `x % y` of integers
(instances asking `Numeric`, `Signed`, `Integral`) -- and `lit`,
`lessThan` and the other comparisons, `both`, `either`, `negation`,
`convert`; a buffer is an `Input a` (read) or an `Output a` (read and
written), `load`ed at an index and asked its `length`, and `store`
writes an `Output` alone.  A statement is an operation of the effect
`stmt` (`define`, `store`, `when`, `ifElse`, `forRange`, `globalId`), so
that a body is a `Co stmt Unit` written with `do`; `kernel1` to
`kernel3` make a `Kernel1 a` to `Kernel3 a b c` of an entry point's
name, a workgroup's size and the body over its buffers.  The kernel
language has no functions, recursion or allocation: a heff function or
recursion in a body runs as the kernel is built (staging) and leaves
its straight code.  The types hold what WGSL would refuse only through
the combinators -- the constructors under them are named `Unsafe...`,
as heff has no module privacy.  `gpu.wgsl` (`import gpu.wgsl as Wgsl`)
prints a kernel as WGSL (`source3 k`), its body by a handler of `stmt`
whose state is a fresh-name counter and the lines; `gpu.run` (`import
gpu.run as Run`) runs one: a `Typed a` is a buffer of `count` `a`s
(`newTyped`, `writeTyped`, `readTyped`, `freeTyped`), and `run3 d k n
x y z` loads `k` and dispatches it over `n` invocations, as many
workgroups as they take.  The vecadd page's `vecAdd` is one, and its
module's `do` runs the steps on the GPU, each an `Either GpuError`, by
a `bind` of its own that stops at the first error; test/gpu/ pins each
statement's WGSL.

A program imports only its own half: a C program that imports a module
of `lib/browser/` is refused at the import (E0351), and so is a page's
script that imports an effect of `lib/std/` with no provider there, or
a module of `lib/std/` that imports one, however far down
(`std.http.server`, by `std.http` and `std.net`) (E0352), each naming
the module and the backend -- and, for one that imports it, which
effect it reaches -- before `main`'s effect is looked at.

An effect's JavaScript side is in `M.provider.js` beside `M.hf`, its C
side in `M.provider.c`: see [providers.md](providers.md).

`std.http` is an HTTP client, on the C backend alone (a page fetches by
`browser.http`'s `send`, or `browser.dom`'s `fetchText`, the browser's
`fetch`).  Import it by a
name, `import std.http as Http`: a `Request` -- `{ url = u }`, or with its `method`, `headers` (a name and
a value each), `body` (`Bytes`), `timeout` (milliseconds in all, 0 for
none) and `follow` (redirects, followed unless `False`) -- is sent by
`Http.send`, and `Http.get u` and `Http.getText u` are a `GET`.  What
comes back is `Either HttpError Response`: a `Response`, its `status`,
`headers` and `body` (`Bytes`; `Http.text` reads it as text,
`Http.header` finds a header), whatever its status, a 404 or a 500 too;
an `HttpError` where no response came -- the connection refused, the
name not found, TLS, the time up -- with curl's `code` and its
`message` (`Http.showError` writes it as curl's tool does).
`Http.getText u` is the body as text where the status is 2xx, and an
error where it is not, code 22, as `curl --fail` has it: `curl: (22)
The requested URL returned error: 404`.

```haskell
import std.http as Http

-- (its status; 0 where no response came)
statusOf : Either Http.HttpError Http.Response -> Int
statusOf (Left err) = 0
statusOf (Right res) = res.status

upload : {e : HFunctor} -> {{web : Http.http <: e}} -> Bytes -> Co e Int
upload b = do
  r <- Http.send { url = "http://localhost:8080/up", method = "POST", body = b, timeout = 5000 }
  pure (statusOf r)
```

It is libcurl, loaded into the process at the first request (no
`-lcurl`), each thread a handle of its own whose connections are kept
for the next request; where there is no libcurl (a static musl program,
Windows), a `curl` spawned for each, given what libcurl is, so that
either answers the same.  A green thread's request, scheduled by
`Reactor.scheduled`, is a transfer of libcurl's multi interface,
pending while the other threads go on, its answer the same (a `curl`
spawned waits, every thread with it).  A body is sent with a `POST`, and with any
method where it is not empty, with no `Content-Type` but the request's
own.

`std.net` is TCP's sockets, on the C backend alone.  Import it by a
name, `import std.net as Net`.  A connection and a listener are the
host's own, had by a `with` ([a thing held by a
`with`](#a-thing-held-by-a-with)).  `Net.withConnection address body`
connects to the `Address` -- its `host` a name the system resolves
(getaddrinfo), to IPv4 and IPv6 addresses, each tried in turn, or one
written as it is (`127.0.0.1`, `::1`); its `port` -- and runs `body`
with the connection, a `With s Net.TcpStream` in a region `s` of its
own, shut down and closed when `body` ends: what the body came to, or
why the connection was not made.  `Net.withListener address backlog
body` has `body` a `With s Net.TcpListener` listening there -- the
host `""` for all of this host's, the port 0 for one the system picks,
which `Net.local l` says -- closed when `body` ends; and `Net.accept l
body` has `body` the next connection made to it, a `With s2
Net.TcpStream` of its own, closed when that body ends.  `Net.read c n`
is up to `n` bytes (a mebibyte at most) of what has come, `Bytes`,
waiting for one, and none at the end, where the other side has closed
or shut down its side; `Net.write c b` writes them all; `Net.shutdown
c` ends this side's writing; `Net.peer c` and `Net.thisSide c` are
where each side is, an `Address`, its `host` numeric and its `port`.
An operation given a connection whose `with` has ended -- by another
thread, say -- is `EBADF`.  What may fail gives `Either NetError a`:
the system's `code` (errno, or getaddrinfo's where a name was not
resolved), its `name`, the same on every system (`ECONNREFUSED`,
`EADDRINUSE`, `EAI_NONAME`), and its `message`, as the system says it
(`Net.showError`).  A read at the end is no error, but no bytes.

A server of many connections at once accepts each in a green thread of
its own ([green threads](#green-threads)), its `with` inside that
thread: the body of one `accept` spawns the thread accepting the next,
then serves its connection, which is closed as the body ends.  So one
thread is always waiting to accept, no table of connections is kept,
and the threads talk over channels.

```haskell
import bytes as Bytes
import green
import std.net as Net
import with

-- (what was read, written back, and `again`; at the end, done: the
-- connection is closed as its `with` ends)
echoed : {e : HFunctor}
       -> {s : Type}
       -> {{nt : Net.net <: e}}
       -> (Unit -> Co e Unit)
       -> With s Net.TcpStream
       -> Either Net.NetError Bytes
       -> Co e Unit
echoed again c (Left err) = pure MkUnit
echoed again c (Right b) =
  if Bytes.length b == 0 then pure MkUnit
  else bind (Net.write c b) (\w -> again MkUnit)

-- (what the other side writes, written back to it until it ends)
echo : {e : HFunctor} -> {s : Type} -> {{nt : Net.net <: e}} -> With s Net.TcpStream -> Co e Unit
echo c = bind (Net.read c 65536) (echoed (\u -> echo c) c)

-- (the next connection accepted: a thread spawned to accept the one
-- after, and this one echoed here, had by this thread's `with`; until
-- none is accepted)
echoServer : {e : HFunctor}
           -> {s : Type}
           -> {{gr : green <: e}}
           -> {{nt : Net.net <: e}}
           -> With s Net.TcpListener
           -> Co e Unit
echoServer l = do
  r <- Net.accept l (\c -> do
    spawn (echoServer l)
    echo c)
  pure MkUnit

-- (every connection to the port echoed at once, under `Reactor.scheduled`;
-- or why the port could not be listened on)
echoAt : {e : HFunctor} -> {{gr : green <: e}} -> {{nt : Net.net <: e}} -> Int -> Co e (Either Net.NetError Unit)
echoAt port = Net.withListener { host = "127.0.0.1", port = port } 128 (\l -> echoServer l)
```

An operation waits until it is done, the whole program with it --
unless it is a green thread's, scheduled by `Reactor.scheduled`
([green threads](#green-threads)): then the other threads go on while
it waits, the heff side as it is.  Windows has no sockets yet: an
operation there fails the program.

`std.http.server` is an HTTP/1.1 server, a library in heff over
`std.net`: import it by a name, `import std.http.server as Server`.
`Server.serve listener handle` accepts the listener's connections (a
`With s Net.TcpListener`, `Net.withListener`'s), one at a time, and
answers each request of each by `handle : Server.Request -> Co e
Http.Response` -- std.http's `Response`, its status, headers and body.  A `Server.Request` is the server's, not
std.http's (which is a client's, its URL, its time and redirects): its
`method`, its `target` as the request line has it (`/items?id=3`), its
`version`, its `headers` (`Server.header` finds one), its `body`
(`Bytes`) and its `peer`.  `Server.serveUntil last listener handle`
stops after the request `last` says is the last, answered, `Right
MkUnit`.  Where a connection fails to be accepted -- the program out of
descriptors, `EMFILE` -- both stop there and give back the `NetError`
(`serve`'s result is `Co e Net.NetError`, `serveUntil`'s `Co e (Either
Net.NetError Unit)`), the listener left open: whether to wait and serve
again is the caller's to say, not the library's.
`Server.serveConnection` serves one connection had by a `with`
(`Net.accept`'s body), closed by it after; and `Server.plain status
text` is a response of text.

```haskell
import std.http as Http
import std.http.server as Server
import std.net as Net

hello : {e : HFunctor} -> Server.Request -> Co e Http.Response
hello r = pure (Server.plain 200 ("hello, " ^ r.target ^ "\n"))

-- (`hello` served on the port, until a connection fails to be accepted:
-- why; or why it could not listen)
helloAt : {e : HFunctor} -> {{nt : Net.net <: e}} -> Int -> Co e (Either Net.NetError Net.NetError)
helloAt port =
  Net.withListener { host = "127.0.0.1", port = port } 64 (\l -> Server.serve l hello)
```

Each request's head is read -- its line and headers, 64 KiB at most --
and its body, of the length its `Content-Length` says (64 MiB at most);
a client that waits for `100 Continue` is sent one.  The response is
written with its length (`Content-Length`, which the server writes
itself, as `Connection`), and no body for a `HEAD`, a 1xx, a 204 or a
304.  The connection is kept for the next request -- requests sent one
after another without waiting are answered in turn -- unless the
request is HTTP/1.0 without `Connection: keep-alive`, either side says
`Connection: close`, or the request is the last.  A request whose body
comes in chunks (`Transfer-Encoding`) is answered `501`, one that is no
HTTP/1.x's `400` or `505`, one too long `431` or `413`, and the
connection is closed: after the response the server shuts its side
down and reads what the client still sends, a mebibyte of it at most,
and lets it go, so that a client still sending its body reads the
response before the connection is closed, not a reset.

`serve` serves one connection at a time: a client that keeps its
connection keeps the others waiting.  `Server.serveConcurrently
listener handle` serves each by a green thread of its own
([green threads](#green-threads); its world has `green`), many at once:
each connection is had by `Net.accept`'s body in its thread, which
spawns the thread accepting the next, so that one is always waiting;
run by `Reactor.scheduled`, each connection waits on its socket apart,
the others served meanwhile, and `handle` may spawn threads of its own.
It stops where a connection fails to be accepted, as `serve` does, the
connections being served going on.  (std.thread's threads join where
they spread: a thread of the system for each connection, joined only
when the server ends, would be one kept for each connection served.)

```haskell
-- (`hello` served on the port, every connection at once)
main : Co world Unit
main = do
  r <- Reactor.scheduled world (rx := \p -> InR (InR p))
    (Net.withListener { host = "127.0.0.1", port = 8080 } 128 (\l ->
      Server.serveConcurrently l hello))
  ...
```

`std.filesystem` is files.  Whole ones: `readFile` (a file's text,
`Maybe String`), `writeFile`, `readDirectory`, `makeDirectory` (a
directory and those it is in: whether a directory is there -- a file
there is none), `remove`, `copy`, `move`, `replace`, and `readOnly` and
`writable` (a file, or a directory and all it holds, made read-only, and
writable by its owner again), each answering whether it was done.

What a path is: `pathStatus path`, links followed, and `linkStatus path`, a
link itself, are `Either FileError Status`, a `Status` its `kind`
(`Regular`, `Directory`, `Link` or `Other`), its `size`, when it was
`modified` (nanoseconds since the epoch, an `Int`, as precise as the
system keeps it) and its `mode` (its permission bits, `0x1ed` for
`rwxr-xr-x`); `kindAt path` is its kind, `Nothing` where nothing is,
and `exists`, `isFile` and `isDirectory` say so.  `setMode path mode` is
chmod, and `makeExecutable path` `chmod +x` (an execute bit where there
is a read bit); `realPath path` is the path with its links resolved
(macOS's temporary directory is under one, `/var` to `/private/var`).
`temporaryDirectory prefix` makes a directory of the program's own where
the system keeps temporary ones (`TMPDIR`, else `/tmp`), named the
prefix and six letters, and made only where nothing is, so that
programs run at once never share one: its path; `temporaryFile prefix` a
file so, open, `Pair` its path and it.  Both are the program's to
remove.  Paths are put together and taken apart by `std.path`, which
is pure.

An open file is a `File`, a handle (`Handle OpenFile`, std.handle).
`openFile path { access = Read }` opens one for `Read`, `Write` (from
its start), `Append` (each write at its end) or `ReadWrite`; the other
fields of `OpenOptions` are as the access has them by default: whether
the file is made where there is none (`create`, for writing and
appending), with what `mode` (`0x1a4`, less the umask), whether it is
emptied (`truncate`, for writing), and whether it must be new
(`exclusive`, `False`).  `readChunk h n` is up to `n` bytes, `Bytes`
(16 MiB at most), none at its end; `readLine h` the next line, `Maybe
String`, `Nothing` at the end, without its `\n` or `\r\n`, and the last
one too where no newline ends it -- a string, which holds any bytes, as
a file's are, UTF-8 or not; `writeBytes h b` and `writeString h s`
write them all; `seek h from n` (`FromStart`, `FromCurrent`, `FromEnd`)
and `tell h` are where it is read and written next, from its start;
`handleStatus h` is its `Status`; and `closeFile h` closes it and lets its
handle go (an operation given it after that fails the program, as
std.net's do).  A file reads ahead, a buffer at a time, and gives back
what it read ahead before it is written, so that a file opened
`ReadWrite` is written where the program has read up to; what is
written is not held back, each write the system's.  What may fail gives
`Either FileError a`: the system's `code`, errno, its `name`, the same
on every system (`ENOENT`, `EACCES`, `EISDIR`), and its `message`
(`showFileError`).

`foldLines h acc step` folds `step` over the lines of an open file,
from where it is to its end, one line held at a time, and `copyHandle
from to` writes what is left of one to another, a chunk at a time.
`stdin`, `stdout` and `stderr` are the standard streams as open files,
the same handle each time: a program reads its input a line at a time,
in as little memory as a line takes.  (std.io's `readStdin` reads it
all, to its end: read it one way or the other, not both, as neither
sees what the other read ahead.)  What std.io wrote is written out
before a write of `stdout` or `stderr`; a standard stream's handle
closed is let go of, and the stream left open.

```haskell
import std.filesystem

-- (a line's words, added to the count)
countWords : {e : HFunctor} -> Int -> String -> Co e Int
countWords n line = pure (n + List.length (words line))

-- (the words of the standard input, read a line at a time)
wordsIn : {e : HFunctor} -> {{disk : filesystem <: e}} -> Co e (Either FileError Int)
wordsIn = bind stdin (\i -> foldLines i 0 countWords)
```

A file may instead be held by a `with`, closed however the program is
done with it ([a thing held by a `with`](#a-thing-held-by-a-with)):
std.filesystem.held's `withFile path { access = Read } body` opens it
and runs `body` with it, a `With s OpenFile`, read by `readLineOf f` and
written by `writeStringTo f s`; the body ended, it closes it, and is
what the body came to, `Either FileError a` (`Left` where the file did
not open or close).  `withFile path o (\f -> pure f)` does not
type-check (a function that holds it can be: an operation on it after
the body is `EBADF`).  Should the body be left otherwise -- a throw heard
by a `catch` outside it, a handler between that drops the continuation
-- the file is closed where the program drops its last reference to it,
at once (the count of it, in C, comes to naught before the `catch` arm
runs).  What uses it needs only `filesystem`: no effect of scopes, nor
`exception`.

```haskell
firstLine : {e : HFunctor} -> {{disk : filesystem <: e}} -> String
          -> Co e (Either FileError (Either FileError (Maybe String)))
firstLine path = withFile path { access = Read } (\f -> readLineOf f)
```

A regular file is read and written there and then, the whole program
waiting for the disk -- a green thread's too: poll says such a file is
always ready, so only a thread of the system could wait for it apart,
and a disk's wait is short beside a socket's.  A pipe, a terminal or a
socket -- the standard streams, most often -- waits, under
`Reactor.scheduled`, in the reactor, as std.net's sockets do, the
other green threads going on; its descriptor is left as it is, never
made non-blocking (a standard stream's is its parent's too): a read is
done once poll says there is something to read, a write a few hundred
bytes at a time (PIPE_BUF) once it says there is room.  Opening a FIFO
waits for its other end, the whole program with it.  On Windows, links
are not seen, a mode is only whether the owner may write, and every
operation waits where it is.

`std.process` runs a command to its end (`run`), in the program's place
(`exec`), or beside it, started and waited for later (`start`).  `run`
takes an `Invocation`, a record whose fields but `command` have
defaults, so that a call gives only what it changes:

```haskell
r <- run { command = "git", args = ["rev-parse", "HEAD"], dir = repo }
t <- run { command = "heff", args = ["test"], env = [MkPair "HEFF_TTY" "0"],
           input = Given "...", timeout = 120000, keep = 65536 }
```

- `args` (none), its arguments;
- `dir` (`""`, the program's), the directory it runs in;
- `env` (none), environment variables set for it over the program's
  own, which it has too (the environment is never cleared);
- `input` (`NoInput`), its standard input: `Given` a string, written
  to it through a pipe and then closed; `Piped`, a pipe the program
  writes to as it goes (a started command's, below); `Inherited`, the
  program's own; `NoInput` is no input at all -- the null device
  (`/dev/null`, `NUL`), not the program's standard input nor a closed
  descriptor, which the command's next open would take;
- `output` and `errors` (`Collected`), its output and its error stream:
  `Collected`, read as they come and kept for its `Ran`; `Piped`, a
  pipe the program reads as it goes (below); `Inherited`, the program's
  own, written there as they come;
- `timeout` (0, as long as it takes), milliseconds, after which it is
  killed (SIGKILL; on Windows, TerminateProcess) and not waited for
  further, not even for what its own children still write;
- `keep` (0, all of it), how many bytes of each of its output and its
  error stream are kept: the first, the rest read and let go, so a
  command that writes forever until its time limit is not held whole;
- `group` (`False`), started in a process group of its own, so that
  killing it -- its time limit, `kill`, `terminate`, the program's end
  -- kills what it started too (not on Windows: there only it).

Its `Ran` is what it wrote, `out` and `err` (those `Collected`), and
how it ended, `ended`: `Exited` with its status, `Signalled` with the
signal's number (never on Windows), `TimedOut`, or `NotRun` -- not
found, not a program, its directory not there, or a NUL in its name or
an argument.  `succeeded r` is whether it exited with 0.

`start` takes the same `Invocation` and goes on beside the command:
`Just` a `Subprocess` (a handle, std.handle's), or `Nothing` where it
could not be run (where `run` says `NotRun`).  `wait p` waits for it to
end, its `Ran`; `waitAny ps` waits for whichever of several ends
first, `Just` it and its `Ran` (`Nothing` for none), the others running
still -- a pool starts the next as soon as one ends; `tryWait p` is
`Just` its `Ran` if it has ended, without waiting; `kill p` kills it
(SIGKILL; on Windows, TerminateProcess, and it exited with 1), not
waiting; `terminate p` ends it -- SIGTERM, then SIGKILL two seconds
later (on Windows, at once) -- and waits for it, its `Ran`.  Once its
`Ran` is given, its handle is let go of: an operation given it after
that fails the program.

```haskell
-- (two commands at once: the first to end said, then the other)
race : {e : HFunctor}
     -> {{io : stdio <: e}}
     -> {{sys : process <: e}}
     -> Subprocess
     -> Subprocess
     -> Co e Unit
race a b = do
  first <- waitAny [a, b]
  case first of
    Just (MkPair p r) -> do
      print ((if sameHandle p a then "a: " else "b: ") ^ r.out)
      rest <- wait (if sameHandle p a then b else a)
      print ("then: " ^ rest.out)
    Nothing -> pure MkUnit
```

What a started command writes is read, its input written and its time
limit kept, while the program waits on it -- by `wait`, `waitAny`,
`tryWait`, `terminate` -- and not between: one that writes more than a
pipe holds (64 KiB, most often) waits until then, and one past its time
limit is killed at the next.  A wait blocks where it is, every green
thread with it, as `run` does.  It runs in the program's process group
(Ctrl-C ends both); what a program started and has not waited for is
terminated at the program's end, and where a signal (SIGINT, SIGTERM,
SIGHUP) ends it.  (`heff dev` runs a project's programs so, their
streams `Inherited`.)  How many there are to run at once, a pool's
size, is std.thread's `cores`, the machine's cores.

A started command is spoken to where its streams are `Piped`: as an
editor speaks to a language server, writing a request and reading the
answers as they come.  `writeInput p b` writes the `Bytes` to its
input, all of them, waiting while its pipe is full (what it writes is
read meanwhile, so that it never waits on the program while the program
waits on it): `False` where they could not be written, the input not
`Piped`, closed, or no more read.  `closeInput p` closes it, and the
command reads its end (`wait` closes it too; `waitAny` does not).
`readOutput p portion ms` is of what it has written to its output and
the program has not yet read, a `Portion`, waited for up to `ms`
milliseconds (0: as long as it takes): `Available`, whatever there is,
waiting for some; `Exactly n`, so many bytes; `Through d`, all up to
and with the first `d` (`"\n"`, a line; `"\r\n\r\n"`, the headers of
HTTP and of LSP).  It is `Received`: `Got` the bytes; `Timeout`, with
what there is left for the next read; `Closed`, the stream at its end,
or not `Piped`.  At the end a read takes what is left, fewer bytes than
asked or no `d`, and the next is `Closed`.  `readErrors` is the same of
its error stream, `Piped`; left `Collected` or `Inherited`, the error
stream is as `run` has it.  What a `Piped` stream has written and the
program has not read is its `Ran`'s.  Reads are in bytes, as the
framing counts them: `Bytes.asString` reads them as text where they
are.

```haskell
-- (a message framed as the language server protocol frames one)
framed : String -> Bytes
framed body =
  Bytes.append
    (Bytes.fromString ("Content-Length: " ^ show (length (Bytes.fromString body)) ^ "\r\n\r\n"))
    (Bytes.fromString body)

-- (the message the headers frame, read: as many bytes as they say)
bodyOf : {e : HFunctor} -> {{sys : process <: e}} -> Subprocess -> Received -> Co e Received
bodyOf p (Got headers) = case contentLength (Bytes.asString headers) of
  Just n -> readOutput p (Exactly n) 5000
  Nothing -> pure Closed
bodyOf p r = pure r

-- (a request to the server, and the next message from it)
asked : {e : HFunctor} -> {{sys : process <: e}} -> Subprocess -> String -> Co e Received
asked server request = do
  _ <- writeInput server (framed request)
  headers <- readOutput server (Through "\r\n\r\n") 5000
  bodyOf server headers
```

(`contentLength` parses the headers; test/node/commands.hf has one.)

## A thing held by a `with`

A host's thing that must be let go of -- an open file, a socket, a
canvas -- is had by a `with`: `with`'s `scoped acquire release body`
acquires it, runs `body` with it, a `With s r` in a region `s` of its
own, and releases it when `body` ends, `Either err a`: what the body
came to, or why the thing was not acquired (the body not run) or not
released (the body's answer lost to it).  The body is polymorphic in
`s` and what it comes to is of a type bound outside it, so a `With s r`
cannot leave the body (E0311), as Haskell's `ST` keeps its references:
one `With` for every effect, so a `with` of one effect hands its thing
to an operation of another by value -- a canvas found by the DOM, drawn
on by the GPU -- and neither keeps it.  Should the body be left
otherwise -- a throw heard outside it, a handler between that drops the
continuation -- the thing is let go of where the program drops its last
reference to it, as its provider says ([scoped
operations](providers.md#scoped-operations)); `scoped` adds nothing for
that, and what uses it needs no effect but the thing's own.

`With` is exposed without its tag: no program makes one, and `scoped`
is the only `with`.  A library wraps `scoped` around its own operations
acquiring and releasing the thing and exposes that `withX` alone, never
the bare acquire: a bare host value given `scoped` is a use after its
close.  Its operations on the thing take the `With s a` and give the
thing to the provider's operation by `with`'s `inside`, which takes it
out for that and nothing else -- a program has no use for the bare
thing, as the operations take the `With`.

```haskell
withFile : {e : HFunctor} -> {a : Type} -> {{ev : filesystem <: e}}
         -> String -> OpenOptions
         -> ({s : Type} -> With s OpenFile -> Co e a)
         -> Co e (Either FileError a)
withFile path o body = scoped (opened path o) closed body   -- `opened`, `closed`: the module's own

readLineOf : {e : HFunctor} -> {s : Type} -> {{ev : filesystem <: e}}
           -> With s OpenFile -> Co e (Either FileError (Maybe String))
readLineOf f = op (ev (ReadLineHeld (inside f) (\r -> r))) val
```

## The terminal

`std.terminal` (`import std.terminal as Terminal`, its effect
`Terminal.console`) is the terminal a program runs on, C's alone:
`interactive` (its standard input and output are a terminal, `TERM` not
`dumb`; never yet on Windows), `begin` (the terminal made raw -- each key
read as it is typed, not shown -- and the alternate screen shown, which
keeps what was written before apart), `finish` (both as they were),
`measure` (its columns and rows, a `Widget.Size`), `readInput` (what is
typed next, waited for: `Just ""` where its size changed or the program
went on after Ctrl-Z, to draw again; `Nothing` at the end of the input)
and `draw` (text written as it is).  The terminal is put back as it was
however the program ends -- at its end, at a failure, and at a signal
that ends it (Ctrl-C is SIGINT still) -- and while Ctrl-Z has stopped
it.

`Terminal.interact app` runs an app there, as Elm runs one: begun, the
app drawn, each key stepped and the app drawn again until it is done,
then finished -- `Maybe` what it came to (`Nothing`: no terminal, or the
input ended).  The app and all it draws are `std.terminal.widget`'s
(`import std.terminal.widget as Widget`), pure:

- `Key`, what is typed: `Char` a rune, `Enter`, `Escape`, `Tab`,
  `BackTab`, `Backspace`, `Delete`, the arrows (`ArrowUp` ...),
  `Home`, `End`, `PageUp`, `PageDown`, `Control` a letter (Ctrl-A), and
  a `Sequence` not known; `decode` reads a terminal's input as keys,
  its escape sequences xterm's and VT100's;
- a `Frame` is `rows`, each a list of `Span`s (a text and its `Style`:
  `Plain`, `Bold`, `Dim`, `Inverse`, `Underline`), and where the
  `cursor` is shown; `paint size frame` is what draws it, in place of
  what was, as one update; `width`, `cut`, `pad` and `wrap` measure and
  lay text out by columns (an East Asian wide rune two);
- the widgets, each a value and what a key makes of it: a `Menu` (the
  item picked among a list's, `menuKey`, `menuRows` scrolled to it), a
  `Field` (text typed, `fieldKey`: runes at the cursor, the arrows,
  Home and End, a shell line's Ctrl-A, -E, -U, -K, -W), a `Choice`
  (one of some values, `choiceKey`: left and right, space, round) and a
  `status` line;
- `App s r` is a state at the `start`, a `step` (a key and a state: to
  `Continue` with, or `Done` with an `r`) and a `view` of a state on a
  terminal of a size; `replay app keys` and `shownAfter app size keys`
  run one on keys given, for its tests.

`std.cli.interactive` (`import std.cli.interactive as Interactive`) is
such an app of a program's commands, as std.cli has them:
`Interactive.app "prog" (Interactive.entries commands)` lists them,
filtered by what is typed, and a command picked is a form of its
positionals and flags -- each with what it takes, its values and
whether it is wanted -- with the command line it makes, and what is
wrong with it yet (`Cli.problemOf`); it comes to the command line's
words, for `Cli.readLine` to run.  `heff` with no words is it
([commands.md](commands.md#the-commands-app)).

```haskell
import co
import std.cli as Cli
import std.cli.interactive as Interactive
import std.terminal as Terminal

-- (the program's commands, picked and filled in on the terminal: the
-- words of the command line made, or none)
picked : {e : HFunctor}
       -> {{con : Terminal.console <: e}}
       -> {r : Type}
       -> List (Cli.Command r)
       -> Co e (Maybe (Maybe (List String)))
picked cs = Terminal.interact (Interactive.app "prog" (Interactive.entries cs))
```

## Green threads

`green` is green threads, an effect and a scheduler that is a handler
in the language, the same on every backend: many threads in one thread
of the system, taking turns at `yield`, at a `recv` with nothing to
take, and where they end, round robin.  `spawn c` makes `c` a thread
of its own, `yield` gives the next thread its turn, `newChan` is a
channel of Ints, `send ch x` puts `x` on it (never waiting) and `recv
ch` takes the first there, waiting for one.  `scheduled e c` runs `c`
and the threads it spawns in `Co e` (`c : Co (green <+> e) a`): main's
answer, `Just`, or `Nothing` where every thread waits on a channel; the
other threads are let go of when main is done.

```haskell
import co
import green

ponger : {e : HFunctor} -> {{ev : green <: e}} -> Int -> Int -> Co e Unit
ponger a b = do
  x <- recv a
  send b (x + 1)
  ponger a b

-- (main and a thread of its own: 1, sent there and back)
there : {e : HFunctor} -> Co e (Maybe Int)
there = scheduled e (do
  a <- newChan (ev := \p -> InL p)
  b <- newChan (ev := \p -> InL p)
  spawn (ev := \p -> InL p) (ponger (ev := \p -> InL p) a b)
  send (ev := \p -> InL p) a 0
  recv (ev := \p -> InL p) b)
```

A thread's operations of the rest of the world -- `e`'s, which the
scheduler does not handle -- are done as its `Outside` says
(`scheduledWith e o c`).  `scheduled`'s, `blocking e`, does each where
it is started, every thread waiting with it: a pure world's, or a
page's, whose providers do not wait.

On the C backend, `std.reactor` (`import std.reactor as Reactor`, its
effect `Reactor.reactor` in the world) has them wait apart:
`Reactor.scheduled e c` is `scheduledWith e Reactor.outside c`, where
a thread whose `Net.read`, `Net.accept`, `Net.withConnection` or
`Net.write` would wait, whose read or write of a pipe, a terminal or a socket
(std.filesystem's `readLine`, `readChunk`, `writeBytes`, `writeString`)
would, or whose `Http.send` (`Http.get`, `Http.getText`) is under way, is pending while the other threads go on, and goes on with its answer once it is done --
from the thread's own view the call returns its result, as it always
did.  When no thread is ready, the scheduler waits for the first
pending one to be done (the system's `poll`, of every socket waited
on); while threads are ready, every 64 turns it puts among them those
done meanwhile.  A name that is no address (`localhost`, a host's) is
resolved by a thread of the system of its own, the others going on.
When main is done, what the other threads have pending is let go of:
a connection being made closed, a request dropped.

```haskell
import co
import green
import std.io
import std.net as Net
import std.process
import std.reactor as Reactor

world : HFunctor
world = stdio <+> (Net.net <+> (Reactor.reactor <+> process))

-- (`c` and its threads, each one's sockets waited on apart)
main : Co world Unit
main = do
  r <- Reactor.scheduled world (rx := \p -> InR (InR (InL p))) program
  ...
```

How it is done -- an operation begun, pending, and finished later -- is
the providers' ([providers.md](providers.md#outside-operations-done-later)):
std.net's, std.http's and std.filesystem's of a pipe, a terminal or a
socket are, others' wait where they are, every thread with them.  It is one reactor for each thread of the system: green
threads in a thread of std.thread's are that thread's own
([threads.md](threads.md#green-threads-in-threads)).

## The wire

`wire` (`import wire as Wire`) is values as bytes and bytes as values
again: one theory, `Wire`, for both ways --

```haskell
theory Wire (a : Type) where
  shape : String
  encode : a -> Bytes
  decode : Bytes -> Int -> Maybe (Pair a Int)
```

-- `encode` a value's bytes, `decode` a value read from the byte `at`
and where it ends, `shape` a name of how the type is written
(`"List (Pair String Int)"`), of which lib/remote.hf makes an API's
fingerprint.  `Wire.encode x`, `Wire.decode b at`, `Wire.decodeAll b`
(the value all the bytes are, `Nothing` where they are none, or more)
and `Wire.shapeOf T` find the instance; the readers' `succeed`,
`failure`, `andThen` and `converted` put one of a type of one's own
together, `fixed n read` reads `n` bytes by a binary's pattern,
`natural` and `readNatural` write and read a length.

The same value is the same bytes on every backend, the contract
between a C program and a page's script (test/programs/wire.hf holds
both to the interpreter):

| Type | Bytes |
| --- | --- |
| `Int` | zigzag (0, -1, 1, -2 as 0, 1, 2, 3), then LEB128: of any size |
| `Int8` .. `Int64`, `UInt8` .. `UInt64` | its bits, big-endian, two's complement |
| `Float64`, `Float32` | IEEE's 64 or 32 bits, big-endian, a NaN the quiet one |
| `Rune` | its code point, 32 bits |
| `Bool` | a byte, 0 or 1 |
| `Unit` | none |
| `String`, `Bytes` | its length (LEB128), then its bytes, as they are |
| `Bits` | its length in bits, then its bits, the last byte's left over 0 |
| `List`, `Array` | its length, then each element |
| `Maybe` | a byte, 0 `Nothing` or 1 `Just`, then the value |
| `Either` | a byte, 0 `Left` or 1 `Right`, then the value |
| `Pair` | the first, then the second |
| `Map`, `HashMap` | its size, then each key and value, in its `toList`'s order |

A map's instance wants its keys' `Order` (`Int`'s, `String`'s, a
fixed-width type's, ...), a hash map's their `Hash` and `Equality`.  A type of one's own
is written as another, by `via`: a record as pairs, a type of more
constructors as `Either`s, its shape theirs.

```haskell
import wire as Wire

type Note = { id : Int, text : String, done : Bool }

instance instanceWireNote : Wire.Wire Note
instanceWireNote =
  Wire.via
    (\(n : Note) -> MkPair n.id (MkPair n.text n.done))
    (\p -> { id = fst p, text = fst (snd p), done = snd (snd p) })
```

An instance is in scope where its module is imported, not further: a
module that uses one of `wire`'s imports `wire` itself.

## JSON

`json` (`import json as Json`) is JSON (RFC 8259) in heff, with no
provider, the same on every backend; the language server reads each
message by it.  A value is

```haskell
data Json
  = JNull | JBool Bool | JInt Int | JFloat Float64 | JString String
  | JArray (List Json) | JObject (List (Pair String Json))
```

-- an object's fields a list, in their order, a key twice kept twice.
`Json.parse s` is `Right` the value the text is, white space around it,
or `Left (Expected i what)`, a `ParseError`: the byte at which the text
is not JSON and what was expected there (`Json.describe` says it, `at
byte 3: expected a value`).  It reads as it goes, a value inside
another kept on a list, not the stack, so a text however deep is read.
A number without a fraction or an exponent is a `JInt`, of any size as
an `Int` is (`123456789012345678901234567890` is one); one with either
is a `JFloat`, rounded correctly where its digits are fewer than 16 and
its exponent within 22 of 0 (as a C library's `strtod`'s fast path is),
within an ulp or so elsewhere, and beyond a float's range an infinity
or 0.  A string's `\u` escapes are runes, a surrogate pair one, a
surrogate not in a pair U+FFFD; a control character in a string, a
leading zero, a `+` or a trailing comma is no JSON.

`Json.stringify j` is the text, as JavaScript's `JSON.stringify` writes
it: no space, a string's `"`, `\` and control characters escaped, what
is not UTF-8 in it U+FFFD.  A value's `Show` is as the source writes
it, `JArray [JInt 1]`, its `ToString` its text.  To read into one:
`Json.field k j` (a field of an object), `Json.at path j` (the value at
a path of fields), and `Json.str`, `Json.int` (a float's rounded down),
`Json.bool` and `Json.arr` at a path, each `Nothing` where it is not
there or is of another kind; `Json.obj`, `Json.kv`, `Json.num` and
`Json.jstr` make one.

```haskell
import json as Json

uri : String -> Maybe String
uri body =
  either (\e -> Nothing) (\j -> Json.str ["params", "textDocument", "uri"] j)
    (Json.parse body)
```

## Examples

Each directory of `examples/` is a project ([projects](projects.md)):
`heff build` there builds it, and `heff dev` serves its pages.
`examples/collatz/` is a first program, through C, to start from: an
effect of its own, a logger, handled two ways -- printed by the program,
kept by its tests -- the statements of [`do`](language.md#do) (`let
mut`, `while`, `for`, `return`), and [tests](tests.md), unit tests and a
property, run by `heff test` there.  `examples/pages/` is four pages,
each a program of its own (`app/NAME/`, its `index.html` beside it): a
counter, a clock, a greeting written into the page, and a sum of vectors
on the GPU; `heff dev` there serves each at `/NAME/`.  `examples/notes/`
is a page and its server, one project ([a client and its
server](remote.md)).  The apps' tests (`heff test test/apps.hf`) run
them all.  `examples/testbridge/` is a project of another kind: a Linux
program, through C, that makes a bridge and a veth pair by rtnetlink,
its socket's calls a provider of its own.
