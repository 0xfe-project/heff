---
title: A web service
---

So far the todos live in one program, on one terminal. In this chapter
they go on the web: a service keeps the list, answers HTTP requests
about it, and sends each change, as it happens, to whoever listens.
Write it as a second program of the project, `app/web/main.hf`, built by
`app/web/build.hf`:

```haskell
import manifest

build : Build
build = { backend = C, run = "main" }
```

Sockets are the C backend's alone, so the service is built through C.
The program starts with its imports:

```haskell
module app.web.main

import boc as BOC
import bytes as Bytes
import co
import exception
import hello.todo
import json as JSON
import std.http as HTTP
import std.http.bocserver as BocServer
import std.http.sse as SSE
import std.io
import std.net as Net
import std.ordmap as OrdMap
import std.process
import std.signal as Signal
import std.time as Time
import resource
```

## What it keeps

The server, std.http.bocserver, runs on boc's behaviours. A cown holds
a value; a behaviour on a cown is given what it holds, and gives back
what it is to hold after it. The service keeps two cowns: the todos,
and the event streams open, its hub.

```haskell
-- | the event streams open, by their numbers
type Hub = BOC.Cown (OrdMap.OrdMap Int (BocServer.Sender Bytes))

-- | what the service keeps: the todos, and the streams open
type Board = { todos : BOC.Cown (List Todo), hub : Hub }
```

## The list

```haskell
-- (an answer, whole)
replied : {e : HFunctor}
        -> {f : HFunctor}
        -> {{bc : BOC.boc <: f}}
        -> BocServer.Responder e Empty
        -> HTTP.Response
        -> Co f Unit
replied respond res = respond (Whole res)

-- (a todo, as JSON)
todoJson : Todo -> JSON.Json
todoJson t =
  JSON.obj
    [ JSON.kv "title" (JSON.jstr t.title)
    , JSON.kv "done" (JBool (not (isOpen t)))
    ]

-- (the todos a `status` asks for: all of them where it asks for none)
asked : Maybe String -> List Todo -> List Todo
asked (Just "open") ts = List.keepIf isOpen ts
asked (Just "done") ts = List.keepIf (\t -> not (isOpen t)) ts
asked _ ts = ts

-- (whether an `Accept` asks for text)
textAsked : Maybe String -> Bool
textAsked (Just "text/plain") = True
textAsked _ = False

-- (the todos, as text, a title a line; or as JSON)
listing : Bool -> List Todo -> HTTP.Response
listing True ts =
  HTTP.plain 200 (joinWith "" (List.map (\(t : Todo) -> t.title ^ "\n") ts))
listing False ts = HTTP.json 200 (JArray (List.map todoJson ts))

-- | `GET /todos`: the todos, those of a `status` where one is asked for
listed : {e : HFunctor}
       -> {{bc : BOC.boc <: e}}
       -> Board
       -> HTTP.ServerRequest
       -> BocServer.Responder e Empty
       -> Co (exception Empty <+> e) Unit
listed board r respond = do
  with ts := board.todos do
    replied
        respond
        (listing
           (textAsked (HTTP.requestHeader "Accept" r))
           (asked (HTTP.query "status" r) ts))
```

The server hands each request to a handler, with a responder,
`respond`, to answer it by. `listed` is one such handler: an
`HTTP.ServerRequest` and a responder in, a computation in the server's
world `e` out, which may throw an `Empty`, so nothing. Its answer is a
response, whole (`Whole`): text (`HTTP.plain`), or JSON (`HTTP.json`).

`HTTP.query "status" r` is the value the request's query gives
`status`, if it gives one: `/todos?status=open` asks for the open todos.
`HTTP.requestHeader "Accept" r` is the request's `Accept` header, its
name's case not minded: a client that asks for `text/plain` is given
the titles, a line each.

`with ts := board.todos do` is a behaviour on the todos: once it runs,
`ts` is the list, and it answers from there. Behaviours on one cown run
one after another, in the order they were spawned; the others run at
once, on as many threads as the machine has cores. A responder may be
called from any behaviour, so `replied` runs in any world `f` that has
boc's.

## A stream of events

```haskell
-- (a stream kept in the hub)
joined : {f : HFunctor}
       -> {{bc : BOC.boc <: f}}
       -> Hub
       -> BocServer.Sender Bytes
       -> Co f Unit
joined hub out = do
  with h := hub do
    h := OrdMap.insert h (BocServer.senderNumber out) out

-- (a stream gone, taken out of the hub)
left : {f : HFunctor}
     -> {{bc : BOC.boc <: f}}
     -> Hub
     -> BocServer.Sender Bytes
     -> Co f Unit
left hub out = do
  with h := hub do
    h := OrdMap.delete h (BocServer.senderNumber out)

-- | `GET /events`: a stream of events, kept in the hub until it is gone
events : {e : HFunctor}
       -> {{bc : BOC.boc <: e}}
       -> Board
       -> BocServer.Responder e Empty
       -> Co (exception Empty <+> e) Unit
events board respond =
  respond
    (Streamed
       { status = 200, headers = SSE.headers }
       { opened = joined board.hub, gone = left board.hub })
```

`Streamed` answers with a head, a status and its headers, then a body
sent a piece at a time. Its `opened` is given the stream's sender once
the head is on its way: `joined` keeps it in the hub, by its number
(`senderNumber`), and `h := ...` sets what the hub holds after the
behaviour. Its `gone` is called once the client has gone, and
`left` takes it out. `SSE.headers` makes the stream one of server-sent
events, as a browser's `EventSource` reads them. While the service has
nothing to say, the server sends the stream a comment now and then,
so that it is not taken for a dead one.

## Changing the list

```haskell
-- (an event sent on each stream)
told : {f : HFunctor}
     -> {{bc : BOC.boc <: f}}
     -> Bytes
     -> List (Pair Int (BocServer.Sender Bytes))
     -> Co f Unit
told b [] = pure ()
told b (MkPair n out :: rest) = do
  BocServer.send out b
  told b rest

-- (an `added` event for each title, on each stream)
announced : {f : HFunctor}
          -> {{bc : BOC.boc <: f}}
          -> List (Pair Int (BocServer.Sender Bytes))
          -> List String
          -> Co f Unit
announced streams [] = pure ()
announced streams (t :: ts) = do
  told (SSE.event [Event "added", Data t]) streams
  announced streams ts

-- (todos of these titles added, each told to every stream: how many,
-- and how many there are now)
added : {e : HFunctor}
      -> {f : HFunctor}
      -> {{bc : BOC.boc <: f}}
      -> Board
      -> List String
      -> BocServer.Responder e Empty
      -> Co f Unit
added board titles respond = do
  with ts := board.todos
       h := board.hub do
    ts := ts ++ List.map todo titles
    announced (OrdMap.toList h) titles
    replied
        respond
        (HTTP.json
           201
           (JSON.obj
              [ JSON.kv "added" (JSON.num (List.length titles))
              , JSON.kv "todos" (JSON.num (List.length ts))
              ]))

-- | `POST /todos`: the body a new todo
posted : {e : HFunctor}
       -> {{bc : BOC.boc <: e}}
       -> Board
       -> String
       -> BocServer.Responder e Empty
       -> Co (exception Empty <+> e) Unit
posted board "" respond = replied respond (HTTP.plain 400 "a title wanted\n")
posted board t respond = added board [t] respond
```

`SSE.event` writes an event as a stream sends it, its type (`Event`)
and its data (`Data`). `BocServer.send` queues it on the stream's
connection, for the server to write after what was sent before; the
behaviour sending it never waits for the write.

`added` binds two cowns in one `with`: it is one behaviour on both,
which takes them all at once. So the todos are added and the streams
told in one step, and every stream hears of the changes in the order
the list took them. After `ts := ...`, `ts` is the new list.

```haskell
-- (the `n`th todo, counted from 1)
todoAt : Int -> List Todo -> Maybe Todo
todoAt n [] = Nothing
todoAt 1 (t :: ts) = Just t
todoAt n (t :: ts) = todoAt (n - 1) ts

-- (the todos, the `n`th finished)
finishedAt : Int -> List Todo -> List Todo
finishedAt n [] = []
finishedAt 1 (t :: ts) = finish t :: ts
finishedAt n (t :: ts) = t :: finishedAt (n - 1) ts

-- | `POST /todos/N/done`: the `N`th todo finished, told to every stream
done : {e : HFunctor}
     -> {{bc : BOC.boc <: e}}
     -> Board
     -> Int
     -> BocServer.Responder e Empty
     -> Co (exception Empty <+> e) Unit
done board n respond = do
  with ts := board.todos
       h := board.hub do
    case todoAt n ts of
      Just t -> do
        ts := finishedAt n ts
        told (SSE.event [Event "done", Data t.title]) (OrdMap.toList h)
        replied respond (HTTP.plain 200 ("done: " ^ t.title ^ "\n"))
      Nothing -> replied respond (HTTP.plain 404 "no such todo\n")
```

Only the cowns a behaviour sets are written back: where there is no
such todo, `done` sets neither, and the list stays as it was.

## Answering later

A handler need not answer before it ends. `POST /import?from=URL` asks
another list, at `URL`, for its todos, through std.http's client, and
answers once they have come:

```haskell
-- (the todos at `url`, asked for as text)
fetched : {e : HFunctor}
        -> {{web : HTTP.http <: e}}
        -> String
        -> Co e (Either HTTP.HttpError HTTP.Response)
fetched url =
  HTTP.send
    { url = url, headers = [MkPair "Accept" "text/plain"], timeout = 5000 }

-- (the titles a response gives, a line each; or why it gives none)
titlesOf : Either HTTP.HttpError HTTP.Response -> Either String (List String)
titlesOf (Left err) = Left (show err)
titlesOf (Right res) =
  if res.status == 200
  then Right (List.keepIf (\l -> not (l == "")) (lines (HTTP.text res)))
  else Left ("answered " ^ debug res.status)

-- | `POST /import?from=URL`: the todos of the list at `URL`, added once
-- | they have come
imported : {e : HFunctor}
         -> {{bc : BOC.boc <: e}}
         -> {{web : HTTP.http <: e}}
         -> Board
         -> Maybe String
         -> BocServer.Responder e Empty
         -> Co (exception Empty <+> e) Unit
imported board (Just url) respond = do
  with got := BOC.await (fetched url) do
    case titlesOf got of
      Right titles -> added board titles respond
      Left why -> replied respond (HTTP.plain 502 (why ^ "\n"))
imported board Nothing respond =
  replied respond (HTTP.plain 400 "from=URL wanted\n")
```

A behaviour never waits on the world: what it would wait for, it
awaits. `with got := BOC.await (fetched url) do` waits for nothing, and
the handler ends. The request, an act, runs outside every behaviour;
once its response has come, the body is a behaviour of its own, `got`
the response or why none came. That behaviour spawns `added`'s on the
todos and the hub, which answers. Meanwhile the connection waits, read
no further, and no thread and no cown is held for it.

The act is of the `with`'s world, the handler's: it can ask through
std.http because the server's world has `HTTP.http`, which the
handler's type says (`{{web : HTTP.http <: e}}`). The other list is
asked for `text/plain`, so a service like this one answers with its
titles.

## Routes

```haskell
-- (a request answered, by its method and its path's segments)
routed : {e : HFunctor}
       -> {{bc : BOC.boc <: e}}
       -> {{web : HTTP.http <: e}}
       -> Board
       -> String
       -> List String
       -> HTTP.ServerRequest
       -> BocServer.Responder e Empty
       -> Co (exception Empty <+> e) Unit
routed board "GET" ["todos"] r respond = listed board r respond
routed board "POST" ["todos"] r respond =
  posted board (trim (Bytes.asString r.body)) respond
routed board "POST" ["todos", i, "done"] r respond =
  done board (fromMaybe 0 (parseInt i)) respond
routed board "POST" ["import"] r respond =
  imported board (HTTP.query "from" r) respond
routed board "GET" ["events"] r respond = events board respond
routed board m ["todos"] r respond =
  replied respond (HTTP.notAllowed ["GET", "POST"])
routed board m ["todos", i, "done"] r respond =
  replied respond (HTTP.notAllowed ["POST"])
routed board m ["import"] r respond = replied respond (HTTP.notAllowed ["POST"])
routed board m ["events"] r respond = replied respond (HTTP.notAllowed ["GET"])
routed board m path r respond =
  replied respond (HTTP.plain 404 "no such page\n")

-- | each request, in a behaviour on its connection's cown
handled : {e : HFunctor}
        -> {{bc : BOC.boc <: e}}
        -> {{web : HTTP.http <: e}}
        -> Board
        -> HTTP.ServerRequest
        -> BocServer.Responder e Empty
        -> Co (exception Empty <+> e) Unit
handled board r respond = routed board r.method (HTTP.segments r) r respond
```

`HTTP.segments r` is the request's path as a list, `/todos/2/done`
`["todos", "2", "done"]`, so a route is an equation on the method and
the segments. The first equation that matches answers. A path asked
for by a method it has not is answered by `HTTP.notAllowed`, `405`,
with the methods it has; any other path, `404`.

## Serving, and stopping

```haskell
-- | the listener served until SIGTERM or Ctrl-C: then closed, and the
-- | server done once the requests in hand are answered
served : {e : HFunctor}
       -> {s : Type}
       -> {{bc : BOC.boc <: e}}
       -> {{io : stdio <: e}}
       -> {{web : HTTP.http <: e}}
       -> {{nt : Net.net <: e}}
       -> {{clock : Time.time <: e}}
       -> {{sg : Signal.signals <: e}}
       -> Board
       -> With s Net.TcpListener
       -> Co e Unit
served board l = do
  at <- Net.local l
  print
      ("listening on http://127.0.0.1:"
       ^ either (\err -> "?") (\(a : Net.Address) -> debug a.port) at)
  flush
  BocServer.serve Empty l (handled (e := e) board)
  with s := BOC.await (Signal.wait [Terminate, Interrupt]) do
    _ <- Net.closeListener l
    print ("stopping on " ^ show s)

-- (a port not listened on, said)
listenedOr : {e : HFunctor}
           -> {{io : stdio <: e}}
           -> Either Net.NetError Unit
           -> Co e Unit
listenedOr (Left err) = printErr ("web: cannot listen: " ^ show err)
listenedOr (Right u) = pure ()

-- (the port the arguments say, 8080 where they say none)
portOf : List String -> Int
portOf (p :: rest) = fromMaybe 8080 (parseInt p)
portOf [] = 8080

main : Co
       (BOC.boc
        <+> stdio
        <+> HTTP.http
        <+> Net.net
        <+> Time.time
        <+> Signal.signals
        <+> process)
       Unit
main = do
  argv <- args
  todos <- BOC.newCown (Nil : List Todo)
  hub <- BOC.newCown (OrdMap.empty : OrdMap.OrdMap Int (BocServer.Sender Bytes))
  r <-
    Net.withListener
      { host = "127.0.0.1", port = portOf argv }
      128
      (served { todos = todos, hub = hub })
  listenedOr r
```

`Net.withListener` listens on the port, and holds the listener with a
`with`, a `With s Net.TcpListener` in a region `s` of its own, for its
body, `served`. Where the port cannot be listened on, it is a `Left`,
and `listenedOr` says why.

`BocServer.serve Empty l handler` comes back at once: the server runs
in its behaviours, each connection a cown of its own, each request
handed to `handled` in a behaviour on it. `(e := e)` gives the handler
the server's world. `served` then awaits a signal: SIGTERM, as `kill`
and service managers send, or Ctrl-C. Once one comes, it closes the
listener, and the server stops: the event streams are ended, and each
connection is closed once the request in hand is answered. The program
ends once nothing of the server is left.

`main`'s world names what the service does: boc's behaviours, the
standard streams, std.http's client, sockets, the time the server keeps
its limits by, signals, and the arguments, for the port.

## Trying it

```sh
$ heff build
...
wrote .heff/bin/web
$ .heff/bin/web
listening on http://127.0.0.1:8080
```

In a second terminal, listen to the events, `-N` so that curl shows
each as it comes:

```sh
$ curl -N localhost:8080/events
```

In a third, change the list and read it:

```sh
$ curl -d 'write the tutorial' localhost:8080/todos
{"added":1,"todos":1}
$ curl -d 'read the reference' localhost:8080/todos
{"added":1,"todos":2}
$ curl -d 'test the examples' localhost:8080/todos
{"added":1,"todos":3}
$ curl -X POST localhost:8080/todos/2/done
done: read the reference
$ curl -X POST localhost:8080/todos/7/done
no such todo
$ curl localhost:8080/todos
[{"title":"write the tutorial","done":false},{"title":"read the reference","done":true},{"title":"test the examples","done":false}]
$ curl 'localhost:8080/todos?status=open'
[{"title":"write the tutorial","done":false},{"title":"test the examples","done":false}]
$ curl -H 'Accept: text/plain' localhost:8080/todos
write the tutorial
read the reference
test the examples
$ curl -i -X DELETE localhost:8080/todos
HTTP/1.1 405 Method Not Allowed
Content-Type: text/plain; charset=utf-8
Allow: GET, POST
Content-Length: 19

method not allowed
$ curl localhost:8080/nothing
no such page
```

The second terminal has heard each change:

```
event: added
data: write the tutorial

event: added
data: read the reference

event: added
data: test the examples

event: done
data: read the reference

```

Start a second service, `.heff/bin/web 8081`, and have it copy the
first's open todos (the `?` of the URL in the query written `%3F`):

```sh
$ curl -X POST 'localhost:8081/import?from=http://localhost:8080/todos%3Fstatus=open'
{"added":2,"todos":2}
$ curl -X POST 'localhost:8081/import?from=http://localhost:1/'
http://localhost:1/: curl: (7) Failed to connect to localhost port 1 after 0 ms: Couldn't connect to server
$ curl localhost:8081/todos
[{"title":"write the tutorial","done":false},{"title":"test the examples","done":false}]
```

Press Ctrl-C in the first terminal:

```sh
$ .heff/bin/web
listening on http://127.0.0.1:8080
^Cstopping on SIGINT
```

The server ends the event stream, and the curl listening to it ends
too.

The server has more to it than this chapter uses: time limits for slow
clients, how much a stream may have waiting to be written, and
WebSocket (`Upgraded`), each in
[std.http.bocserver](../api/std/http/bocserver.md). For many streams at
once, [std.http.broadcast](../api/std/http/broadcast.md) sends to them
from shards of their own, in parallel.
