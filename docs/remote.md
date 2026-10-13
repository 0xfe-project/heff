# A client and its server

One project, two programs of two backends: a page (`Page`, JavaScript)
and its server (`C`), the server's API between them an effect both
import.  The page's handler of the effect sends each operation to the
server and goes on with its answer; the server's does it for real; a
test's does both in one program, with no network.  `examples/notes/`
is such a project; this is how it is put together.

## The layout

```
package.hf
src/notes/api.hf        -- the API: its effect, its types, its wire
app/server/build.hf     -- { backend = C, run = "main" }
app/server/main.hf      -- the API's real handler, served over HTTP
app/client/build.hf     -- { backend = Page, run = "main" }
app/client/main.hf      -- the page, its calls sent to the server
app/client/index.html   -- loads client.js
test/api.hf             -- the API's calls through the wire, no network
```

Each program's `build.hf` says its backend, and `heff build` builds
both: `.heff/bin/server`, through C, and `.heff/bin/client.js`, the
page's script ([projects](projects.md#programs)).  What `src/` has is
imported by both, so it imports what both backends have: `co`,
`remote`, `wire`, the pure library -- not `std.net`, not `browser.dom`
(E0351, E0352).

## The API

The API is an effect ([effects](language.md#effects)), whose
operations are what the server offers, each taking a value and giving
one back:

```haskell
import co
import remote as Remote
import wire as Wire

type Note = { id : Int, text : String, done : Bool }

data NotesF (m : Type -> Type) x
  = Add String (Note -> x)
  | Toggle Int (Bool -> x)
  | Every (List Note -> x)

notes : HFunctor
notes = ...

addNote : {e : HFunctor} -> {{api : notes <: e}} -> String -> Co e Note
addNote t = op (api (Add t (\n -> n))) val
```

Each value that crosses has a `Wire` ([the wire](library.md#the-wire)):
the library's types have theirs, and a type of the program's own is
written as one of them, by `Wire.via` -- a record as pairs, a type of
more constructors as `Either`s:

```haskell
instance instanceWireNote : Wire.Wire Note
instanceWireNote =
  Wire.via
    (\(n : Note) -> MkPair n.id (MkPair n.text n.done))
    (\p -> { id = fst p, text = fst (snd p), done = snd (snd p) })
```

The API on the wire is said once, in the same module: each operation's
index, its name and its types (`Remote.operation`, its types those of
the `Operation`; a constructor of no argument takes a `Unit`); what
each constructor called is (`Remote.call`, given its argument and its
continuation); and how each is served (`Remote.serve`, given its
constructor).  `Remote.api` puts them together with the API's name:

```haskell
addOp : Remote.Operation String Note
addOp = Remote.operation 0 "add"

toggleOp : Remote.Operation Int Bool
toggleOp = Remote.operation 1 "toggle"

everyOp : Remote.Operation Unit (List Note)
everyOp = Remote.operation 2 "every"

called : {m : Type -> Type} -> {x : Type} -> NotesF m x -> Remote.Call x
called (Add t k) = Remote.call addOp t k
called (Toggle i k) = Remote.call toggleOp i k
called (Every k) = Remote.call everyOp MkUnit k

served : {m : Type -> Type} -> List (Remote.Served (NotesF m Bytes))
served =
  [ Remote.serve addOp (\t k -> Add t k)
  , Remote.serve toggleOp (\i k -> Toggle i k)
  , Remote.serve everyOp (\u k -> Every k)
  ]

notesApi : Remote.Api notes
notesApi = Remote.api notes "notes" called served
```

Each operation's `Wire`s are found where the API is written, once, and
both programs use them through `notesApi`.  Its fingerprint is a hash
(FNV-1a, 32 bits) of its name and of each operation's index, name and
types, as their `Wire`s' shapes say them (`Remote.signatures`): a page
and a server of different builds of the API -- an operation added, its
type changed -- are told apart at the first call, which the server
refuses.

## The server

The server's handler is the effect's real one, as `through` takes one:
each operation, done in the server's world (the notes in a cell of
`std.ref`).  `std.remote` (`import std.remote as RemoteHttp`) answers a
`POST` of a call's frame with the answer's frame, `RemoteHttp.routed
"/api" api handler other` the requests of `/api` so, any other by
`other` (the page and its script); over std.http.server, each
connection a green thread of its own:

```haskell
kept : {e : HFunctor}
     -> {{cell : ref (List Note) <: e}}
     -> Ref (List Note)
     -> {a : Type}
     -> NotesF (Co e) a
     -> Co e a
kept r (Add t k) = bind (changed r (added t)) (\n -> pure (k n))
...

  err <- Server.serveConcurrently l (RemoteHttp.routed "/api" notesApi (kept r) page)
```

## The client

The page's handler is `Remote.remotely`: each operation sent by a
transport, the rest of the world done as it is.  `browser.remote`
(`import browser.remote as RemoteHttp`) has one by the browser's
`fetch` (`browser.http`), each call a `POST` of `/api`:

```haskell
page : HFunctor
page = dom <+> Http.http

main : Co page Unit
main = do
  r <-
    attempt
      Remote.RemoteError
      page
      (Remote.remotely
         notes
         (exn Remote.RemoteError <+> page)
         (failed := \p -> InL p)
         notesApi
         (RemoteHttp.posted (web := \p -> InR (InR p)) "/api")
         (program
            (api := \p -> InL p)
            (failed := \p -> InR (InL p))
            (ui := \p -> InR (InR (InL p)))))
  pure MkUnit
```

The program asks for the notes (`{{api : notes <: e}}`) as for any
effect, and is the same whoever handles them.  A C program is a client
the same way, by `std.remote`'s `posted`, std.http's.

## When no answer comes

A call that comes to no answer throws: `exn RemoteError`, in the
world under the API's, so the client's world has it.  Why is one of
`Unreachable` (the transport's: no connection, a status that is no
200), `Mismatched` (the server's API is of another build, or its wire
of another version), `Refused` (the server could not read the call:
no such operation, its argument) and `Malformed` (the answer could not
be read); `Remote.showRemoteError` says it.  `catch` hears it where a
program would go on -- the page says why, and is as it was:

```haskell
guarded : {e : HFunctor}
        -> {{failed : exn Remote.RemoteError <: e}}
        -> {{ui : dom <: e}}
        -> Node
        -> Co e Unit
        -> Co e Unit
guarded status c =
  catch
    c
    ((\err -> text status (Remote.showRemoteError err)) :
       Remote.RemoteError -> Co e Unit)
```

and `attempt` (lib/exn.hf) handles the rest at the top, `Left` what no
`catch` heard.  An event's run (browser.dom's `on`) is its own: a
throw in it that no `catch` of its own hears abandons that run, not
the page.

## A test, with no network

`Remote.locally api handler` is the client's handler with a server in
the same program: each call written, answered by `Remote.respond` of
the real handler, and read back, the bytes as they would go.  A test
of the API runs by the interpreter, the notes kept in a `state`:

```haskell
-- >>> ran (session (api := \p -> InL p))
-- "1 [x] milk, 2 [ ] bread; True False"
```

(examples/notes/test/api.hf.)  `Remote.loopback api handler` is the
transport of it, for `remotely`.

## The frame

What goes each way is a frame, a header and what it carries:

```haskell
b["hw", 1::8, kind::8, fingerprint::32, index::16, n::32, payload::binary-size(n)]
```

-- the wire's version (1; a frame of another is refused), what the
frame is (0 a call, 1 its answer, 2 a failure), the API's fingerprint,
the operation's index (of a failure, why: 1 another API, 2 no such
operation, 3 its argument not read, 4 no frame), and the payload: the
argument, the answer, or what the server says of the failure, a
`String`.  Over HTTP, its `Content-Type` is
`application/x-heff-wire`.

A transport is a record, `{ exchange : Bytes -> Co e (Either
RemoteError Bytes) }`: a frame sent, the frame that answers it.  HTTP's
`POST` is the one there is; another -- a WebSocket's, a pipe's -- is
another record of the same type.

## Running it

```sh
cd examples/notes
heff test               # the API through the wire, no network
heff build              # .heff/bin/server and .heff/bin/client.js
.heff/bin/server 8080   # from the project's root: then open http://127.0.0.1:8080/
```
