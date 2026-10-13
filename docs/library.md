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
  `state`, `exn` (with `exnDiv` and `exnMod`, a division that throws by
  zero), `parallel` and `green` ([green threads](#green-threads));
- `array`, `map` and `set` (a map's `insertWith` and `alter` change
  what a key is bound to in one walk down the tree, where a `lookup`
  and an `insert` take two: `Map.insertWith compareString (\new old ->
  new + old) word 1 counts` counts a word);
- `check` and `gen`, the tests and their generators
  ([tests.md](tests.md#unit-tests-and-properties));
- `manifest`, the types of `package.hf` and `build.hf`
  ([projects.md](projects.md));
- the fixed-width numbers' `int8` to `int64`, `uint8` to `uint64` and
  `float32`, `int` with an `Int`'s bits, and `rune` with a `Rune`'s
  operations ([numbers](language.md#numbers),
  [runes and strings](language.md#runes-and-strings));
- `bytes`, the operations of `Bytes` ([bytes](language.md#bytes)),
  which a binary, `b[...]`, writes and takes apart
  ([binaries](language.md#binaries));
- `bits`, the operations of `Bits`, a bitstring of any length
  ([bits](language.md#bits)): `length` (in bits), `index` (a bit, as a
  `Bool`), `toList` and `fromList`, `toBytes` (`Just` the bytes, of a
  length that is a multiple of 8) and `ofBytes`, `ofInt` (the low `n`
  bits of an integer), `slice` (from bit `i` up to `j`, clamped),
  `append`, `concat`, `empty` and `eq`.

## The batteries

The batteries ship with it, in two halves, by where a program runs.
What is above, in `lib/`, is pure, the same on every backend.

`lib/std/` is the system's, for the C backend (`--backend=c`):

- `std.io` (the standard streams), `std.filesystem`, `std.process`,
  `std.http` (an HTTP client), `std.net` (TCP sockets), `std.progress`
  (what a program is doing, drawn on a terminal), `std.thread` (threads
  truly at once: a pool, spreads, and `Par` handled by them --
  [threads.md](threads.md)), `std.reactor` (outside operations begun
  now and finished later, for [green threads](#green-threads)):
  effects of the system, C's alone;
- `std.http.server`, an HTTP/1.1 server written over `std.net`, C's
  as `std.net` is;
- `std.ref` (the world's cells) and `std.time`: effects needed on both
  sides, one module each with a provider in C and one in JavaScript,
  which a page's script has too;
- `std.handle` (the world's things, which one effect hands out and
  another takes), `std.path`, `std.cli` (command lines), `std.doc`
  (documents laid out) and `std.math` (a float's functions: `sin`,
  `atan2`, `exp`, `log`, `pow`, `round`, `pi`, `e` and the like): pure,
  for any backend.

`lib/browser/` is the browser's, for JavaScript (`--backend=page`, and
`esm`):

- `browser.dom` (web pages);
- `browser.js` (JavaScript's strings, `JSString`).

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
`browser.dom`'s `fetchText`, the browser's `fetch`).  Import it by a
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
name, `import std.net as Net`.  A `Net.Connection` (a `Handle
TcpStream`) is made by `Net.connect host port`, or accepted by
`Net.accept` from a `Net.Listener` (a `Handle TcpListener`), which
`Net.listen address port backlog` makes -- the address `""` for all of
the host's, the port 0 for one the system picks, which `Net.local`
says.  A host or an address is a name the system resolves
(getaddrinfo), to IPv4 and IPv6 addresses, each tried in turn, or one
written as it is (`127.0.0.1`, `::1`).  `Net.read c n` is up to `n`
bytes (a mebibyte at most) of what has come, `Bytes`, waiting for one,
and none at the end, where the other side has closed or shut down its
side; `Net.write c b` writes them all; `Net.shutdown c` ends this
side's writing; `Net.peer c` and `Net.local s` are where each side is,
an `Address`, its `host` numeric and its `port`; `Net.close s` closes a
connection or a listener and lets its handle go (an operation given it
after that fails the program, as one given a handle that is no
socket).  What may fail gives `Either NetError a`: the system's `code`
(errno, or getaddrinfo's where a name was not resolved), its `name`,
the same on every system (`ECONNREFUSED`, `EADDRINUSE`, `EAI_NONAME`),
and its `message`, as the system says it (`Net.showError`).  A read at
the end is no error, but no bytes.

```haskell
import bytes as Bytes
import std.net as Net

-- (what was read, written back, and `again`; at the end, closed)
echoed : {e : HFunctor}
       -> {{nt : Net.net <: e}}
       -> (Unit -> Co e Unit)
       -> Net.Connection
       -> Either Net.NetError Bytes
       -> Co e Unit
echoed again c (Left err) = Net.close c
echoed again c (Right b) =
  if Bytes.length b == 0 then Net.close c
  else bind (Net.write c b) (\w -> again MkUnit)

-- (what the other side writes, written back to it until it ends)
echo : {e : HFunctor} -> {{nt : Net.net <: e}} -> Net.Connection -> Co e Unit
echo c = bind (Net.read c 65536) (echoed (\u -> echo c) c)
```

An operation waits until it is done, the whole program with it --
unless it is a green thread's, scheduled by `Reactor.scheduled`
([green threads](#green-threads)): then the other threads go on while
it waits, the heff side as it is.  Windows has no sockets yet: an
operation there fails the program.

`std.http.server` is an HTTP/1.1 server, a library in heff over
`std.net`: import it by a name, `import std.http.server as Server`.
`Server.serve listener handle` accepts the listener's connections, one
at a time, and answers each request of each by `handle :
Server.Request -> Co e Http.Response` -- std.http's `Response`, its
status, headers and body.  A `Server.Request` is the server's, not
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
`Server.serveConnection` serves one connection accepted already; and
`Server.plain status text` is a response of text.

```haskell
import std.http as Http
import std.http.server as Server
import std.net as Net

hello : {e : HFunctor} -> Server.Request -> Co e Http.Response
hello r = pure (Server.plain 200 ("hello, " ^ r.target ^ "\n"))

-- (`hello` served on the port, until a connection fails to be accepted:
-- why, or why it could not listen)
helloAt : {e : HFunctor} -> {{nt : Net.net <: e}} -> Int -> Co e Net.NetError
helloAt port = do
  l <- Net.listen "127.0.0.1" port 64
  either (\err -> pure err) (\ln -> Server.serve ln hello) l
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
  r <- Reactor.scheduled world (rx := \p -> InR (InR p)) (do
    l <- Net.listen "127.0.0.1" 8080 128
    either (\err -> pure err) (\ln -> Server.serveConcurrently ln hello) l)
  ...
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
a thread whose `Net.read`, `Net.accept`, `Net.connect` or `Net.write`
would wait, or whose `Http.send` (`Http.get`, `Http.getText`) is
under way, is pending while the other threads go on, and goes on with its answer once it is done --
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
std.net's and std.http's are, others' wait where they are, every thread
with them.  It is one reactor for each thread of the system: green
threads in a thread of std.thread's are that thread's own
([threads.md](threads.md#green-threads-in-threads)).

## Examples

`examples/` has apps for the browser: `node examples/build.mjs` builds
each to `.heff/examples/NAME/`, where its `index.html` opens.
`examples/testbridge/` is a project of another kind, built by `heff
build` there: a Linux program, through C, that makes a bridge and a
veth pair by rtnetlink, its socket's calls a provider of its own.
