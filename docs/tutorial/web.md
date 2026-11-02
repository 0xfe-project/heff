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

import array as Array
import boc as BOC
import bytes as Bytes
import co
import exception
import hello.todo
import json as JSON
import std.http as HTTP
import std.http.bocserver as BocServer
import std.http.router as Router
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
what it is to hold after it. The service keeps one: the todos, the
events of their changes not yet told, its news, and the event streams
open, to tell them to.

```haskell
-- | the todos, the events of their changes not yet told, and the event
-- | streams open, by their numbers, to tell them to
type Todos =
  { list : List Todo
  , news : List Bytes
  , streams : OrdMap.OrdMap Int (BocServer.Sender Bytes)
  }

-- | what the service keeps: the todos, in a cown
type Board = { todos : BOC.Cown Todos }
```

## The list

```haskell
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
listed : Todos -> HTTP.ServerRequest -> HTTP.Response
listed ts r =
  listing
    (textAsked (HTTP.requestHeader "Accept" r))
    (asked (HTTP.query "status" r) ts.list)
```

std.http.router hands each request to its route's handler. `listed`
is one: a function, of what the todos hold, `ts`, and the request, to
a response: text (`HTTP.plain`), or JSON (`HTTP.json`). It does
nothing but compute the response, so it can throw nothing; a handler
that has something wrong to say answers it, a `400` or a `404`.

`HTTP.query "status" r` is the value the request's query gives
`status`, if it gives one: `/todos?status=open` asks for the open todos.
`HTTP.requestHeader "Accept" r` is the request's `Accept` header, its
name's case not minded: a client that asks for `text/plain` is given
the titles, a line each.

## A stream of events

```haskell
-- (a stream kept with the todos)
joined : {f : HFunctor}
       -> {{bc : BOC.boc <: f}}
       -> BOC.Cown Todos
       -> BocServer.Sender Bytes
       -> Co f Unit
joined todos out = do
  with ts := todos do
    ts :=
      { ts
      | streams = OrdMap.insert ts.streams (BocServer.senderNumber out) out
      }

-- (a stream gone, taken out)
left : {f : HFunctor}
     -> {{bc : BOC.boc <: f}}
     -> BOC.Cown Todos
     -> BocServer.Sender Bytes
     -> Co f Unit
left todos out = do
  with ts := todos do
    ts :=
      { ts | streams = OrdMap.delete ts.streams (BocServer.senderNumber out) }

-- | `GET /events`: a stream of events, kept with the todos until it is
-- | gone
events : {e : HFunctor}
       -> {{bc : BOC.boc <: e}}
       -> Board
       -> HTTP.ServerRequest
       -> BocServer.Answer e Empty
events board r =
  Streamed
    { status = 200, headers = SSE.headers }
    { opened = joined board.todos, gone = left board.todos }
```

`events` answers with a stream (`Streamed`): a head, a status and its
headers, then a body sent a piece at a time. Its `opened` is given the
stream's sender once the head is on its way: `joined` keeps it with
the todos, by its number (`senderNumber`), and `ts := ...` sets what
the cown holds after the behaviour. Its `gone` is called once the client has
gone, and `left` takes it out. `SSE.headers` makes the stream one of
server-sent events, as a browser's `EventSource` reads them. While the
service has nothing to say, the server sends the stream a comment now
and then, so that it is not taken for a dead one.

## Changing the list

```haskell
-- (the todos, these titles added, each an `added` event)
-- | >>> List.map title (added ["b", "c"] { list = [todo "a"], news = [], streams = OrdMap.empty }).list
-- | ["a", "b", "c"]
added : List String -> Todos -> Todos
added titles ts =
  { ts
  | list = ts.list ++ List.map todo titles
  , news = ts.news ++ List.map (\t -> SSE.event [Event "added", Data t]) titles
  }

-- (how many were added, and how many there are)
addedSaid : Int -> Todos -> HTTP.Response
addedSaid n ts =
  HTTP.json
    201
    (JSON.obj
       [ JSON.kv "added" (JSON.num n)
       , JSON.kv "todos" (JSON.num (List.length ts.list))
       ])

-- (a todo of this title added; none, where the title is empty)
titled : Todos -> String -> Pair Todos HTTP.Response
titled ts "" = MkPair ts (HTTP.plain 400 "a title wanted\n")
titled ts t = MkPair (added [t] ts) (addedSaid 1 (added [t] ts))

-- | `POST /todos`: the body a new todo
posted : Todos -> HTTP.ServerRequest -> Pair Todos HTTP.Response
posted ts r = titled ts (trim (Bytes.asString r.body))
```

`posted` is given what the todos hold too, and gives back what they
are to hold after it, beside its response: a `Pair`. `added` adds the
todos, and an `added` event for each, written as `SSE.event` writes an
event, its type (`Event`) and its data (`Data`), to the news, to be
told.

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

-- (the `n`th todo finished, a `done` event; or none there)
-- | >>> (snd (finished 7 { list = [], news = [], streams = OrdMap.empty } Nothing)).status
-- | 404
finished : Int -> Todos -> Maybe Todo -> Pair Todos HTTP.Response
finished n ts (Just t) =
  MkPair
    { ts
    | list = finishedAt n ts.list
    , news = ts.news ++ [SSE.event [Event "done", Data t.title]]
    }
    (HTTP.plain 200 ("done: " ^ t.title ^ "\n"))
finished n ts Nothing = MkPair ts (HTTP.plain 404 "no such todo\n")

-- | `POST /todos/N/done`: the `N`th todo finished
done : Todos -> HTTP.ServerRequest -> Int -> Pair Todos HTTP.Response
done ts r n = finished n ts (todoAt n ts.list)
```

`done` takes `n` too, what the path captures, as the routes below say.
A handler is a function like any other, so it is checked as one, with
no server around it: the lines under `-- | >>>` are tests of `heff
test`, as in [the chapter on tests](tests.md).

```haskell
-- (each event sent on each stream)
told : {f : HFunctor}
     -> {{bc : BOC.boc <: f}}
     -> List Bytes
     -> List (Pair Int (BocServer.Sender Bytes))
     -> Co f Unit
told news streams = do
  for b in news do
    for s in streams do
      BocServer.send (snd s) b

-- (the todos' news told to every stream, and the todos left with none)
settled : {f : HFunctor}
        -> {{bc : BOC.boc <: f}}
        -> Todos
        -> Pair Todos (Co f Unit)
settled ts = MkPair { ts | news = [] } (told ts.news (OrdMap.toList ts.streams))

-- | the todos, as the routes keep them: once a batch has changed them,
-- | their news told
todos : {e : HFunctor} -> {{bc : BOC.boc <: e}} -> Board -> Router.Kept e Todos
todos board = Router.kept board.todos settled
```

`settled` tells the news: given what the todos hold, it gives back the
todos with no news, and what to do, each event sent on each stream
(`BocServer.send`, which queues it on the stream's connection, never
waiting for the write). `todos board` is the todos as the routes keep
them, `Router.kept`: once a batch of requests has changed them, the
router gives `settled` what they hold, in that batch's behaviour, and
writes back what it gives. So each batch's news is told once, in the
order the list took the changes, and the todos are written once a
batch.

## Answering later

A handler need not answer at once. `POST /import?from=URL` asks
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

-- (the titles added to the todos; or why there are none)
importedFrom : {e : HFunctor}
             -> {{bc : BOC.boc <: e}}
             -> Board
             -> Either String (List String)
             -> Co e (BocServer.Answer e Empty)
importedFrom board (Right titles) = do
  with ts := board.todos do
    let after = settled (added titles ts)
    ts := fst after
    snd after
  pure
      (Whole
         (HTTP.json
            201
            (JSON.obj [JSON.kv "added" (JSON.num (List.length titles))])))
importedFrom board (Left why) = pure (Whole (HTTP.plain 502 (why ^ "\n")))

-- | `POST /import?from=URL`: the todos of the list at `URL`, added once
-- | they have come
imported : {e : HFunctor}
         -> {{bc : BOC.boc <: e}}
         -> {{web : HTTP.http <: e}}
         -> Board
         -> HTTP.ServerRequest
         -> Maybe String
         -> Co e (BocServer.Answer e Empty)
imported board r (Just url) = do
  got <- fetched url
  importedFrom board (titlesOf got)
imported board r Nothing = pure (Whole (HTTP.plain 400 "from=URL wanted\n"))
```

`imported` comes to a computation of the server's world, which comes
to the answer. The router runs it as an act it awaits: outside every
behaviour, so that it may wait on the world -- here for the other
list's answer -- no thread and no cown held meanwhile, the connection
read no further. What it fails at is a value, `Left`, and an answer
too, a `502`. Once the titles have come, a behaviour on the todos adds
them and tells their news, `settled`; the client is answered after
that behaviour is spawned, so a request it sends next is taken after
the todos have them.

The act is of the server's world: it can ask through std.http because
that world has `HTTP.http`, which `imported`'s type says (`{{web :
HTTP.http <: e}}`). The other list is asked for `text/plain`, so a
service like this one answers with its titles.

## Waiting for a change

A client that wants to hear of the next todo can ask `GET
/todos?after=3`, a long poll: the todos after the third, answered once
there are some, however long that takes. A route on a state has a
slot, a cown the router puts its requests in, and a waiter on it,
which takes them; for the routes so far, the router's own. This
route's waiter is the service's:

```haskell
-- (whether there is a todo after the `n`th, which `a` waits for)
newer : {e : HFunctor} -> Todos -> Router.Asked e Int -> Bool
newer ts a = List.length ts.list > a.captured

-- (each long poll answered: the todos after the one it has seen)
answered : {e : HFunctor}
         -> {{bc : BOC.boc <: e}}
         -> Todos
         -> Array (Router.Asked e Int)
         -> Co e Unit
answered ts polls = do
  for a in polls do
    a.respond (Whole (listing False (List.drop a.captured ts.list)))

-- | `GET /todos?after=N`: the todos after the `N`th, once there are some
later : {e : HFunctor}
      -> {{bc : BOC.boc <: e}}
      -> Board
      -> Router.Slot e Int
      -> Co e Unit
later board slot = do
  with q := slot
       ts := board.todos
       once Array.any (newer ts) q do
    later board slot
    BOC.task (answered ts (Array.keepIf (newer ts) q))
    q := Array.keepIf (\a -> not (newer ts a)) q
```

`later` is given the route's slot, a `Router.Slot e Int`: the
requests put in it and not yet answered, each a `Router.Asked`, its
request, its responder (`a.respond`), and what its path captured,
`captured`, the `after` it asks for. `with q := slot` and `ts :=
board.todos` under it is one behaviour on the slot and the todos, and
`once Array.any (newer ts) q` its condition: it runs only once some
request waits for fewer todos than there are. Until then it lets go of
both cowns and waits for a behaviour to write one of them -- a request
put in the slot, or the todos changed -- and then checks again. Once
it holds, it waits again first; hands the requests it can answer to
`answered`, a behaviour of its own (`BOC.task`); and keeps the others
in the slot. So were answering them to fail, those alone would be lost,
and the route would wait on. Nothing keeps a list of who waits: the
todos' writes wake the waiter.

## Routes

```haskell
-- | the service's routes, each handler given the board or the todos
instance instanceRouteRegisterBoard : {e : HFunctor}
                                    -> {{bc : BOC.boc <: e}}
                                    -> {{web : HTTP.http <: e}}
                                    -> Router.RouteRegister e Board
instanceRouteRegisterBoard = { routes = registered }
  where
  registered : Board -> Router.Routes e
  registered board = do
    Router.waiting "GET" ("todos" + Router.query "after") (later board)
    Router.get "todos" (Router.on board.todos listed)
    Router.post "todos" (Router.on (todos board) posted)
    Router.post ("todos" / Router.int / "done") (Router.on (todos board) done)
    Router.post ("import" + Router.query "from") (imported board)
    Router.get "events" (events board)

-- (a request no route takes)
noPage : HTTP.ServerRequest -> HTTP.Response
noPage r = HTTP.plain 404 "no such page\n"
```

std.http.router routes a request by its method and its path. A service
says its routes in a `do` block, one a line: the method (`Router.get`,
`Router.post`), the path, and the handler; or, for a waiter of the
service's own, `Router.waiting` and the method. That is the service's
`RouteRegister`, an instance, so that the router finds the routes by
the board's type. The first route of a path and method that takes a
request answers it, so `later`'s, which takes only a request with an
`after`, comes before `listed`'s.

`Router.on (todos board) done` is `done` on the todos: the router
gives it what they hold. Its requests are put in its route's slot, each
by a behaviour on that cown alone, and its waiter takes them a batch at
a time, by a behaviour on the todos: each request given to `done` in
turn, the todos the one before gave back, `settled` given the last,
and the todos written back once; the answers are then sent by a
behaviour of their own, so the todos are let go of at once. A handler
that only reads the todos, `listed`, is on the cown itself,
`board.todos`, and given what it holds by a behaviour of its own; the
todos are not written. `imported` and `events` are of the request
alone, answered where the request is found.

A path is its segments, joined by `/`. A string is a segment of that
text, and `Router.int` a segment read as an `Int`: `done`'s, after the
request, so `/todos/2/done` is `done ts r 2`. What the handler's type
takes after the request is what the path captures, in order, and a path
whose `Router.int` the handler does not take as an `Int` is said before
the service starts. `+ Router.query "from"` captures the query's `from`
too, read as `imported` takes it, a `Maybe String`: `Nothing` where
there is none. What the handler comes to is how it answers: a response,
a pair of the state and a response, an answer such as a stream, or a
computation that comes to an answer.

The router answers a path it has, asked for by a method it has not,
`405`, its `Allow` the methods it has, `HEAD` too where it has `GET`
(a `HEAD` is answered as the `GET`, its body not sent); any other
request is `noPage`'s. A path may have more than this one: a segment a
pattern matches, the rest of the path, a query's value or a header
asked for, each on [std.http.router](../api/std/http/router.md).

## Serving, and stopping

```haskell
-- | the listener served until SIGTERM or Ctrl-C: then closed, and the
-- | server done once the requests in hand are answered
served : {e : HFunctor}
       -> {s : Type}
       -> {{bc : BOC.boc <: e}}
       -> {{io : stdio <: e}}
       -> {{nt : Net.net <: e}}
       -> {{clock : Time.time <: e}}
       -> {{sg : Signal.signals <: e}}
       -> Router.Router e
       -> With s Net.TcpListener
       -> Co e Unit
served router l = do
  at <- Net.local l
  print
      ("listening on http://127.0.0.1:"
       ^ either (\err -> "?") (\(a : Net.Address) -> debug a.port) at)
  flush
  BocServer.serve Empty l (Router.dispatch router noPage)
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
  todos <-
    BOC.newCown ({ list = [], news = [], streams = OrdMap.empty } : Todos)
  routed <- Router.serving ({ todos = todos } : Board)
  case routed of
    Right router -> do
      r <-
        Net.withListener
          { host = "127.0.0.1", port = portOf argv }
          128
          (served router)
      listenedOr r
    Left conflicts ->
      printErr ("web: " ^ joinWith "; " (List.map show conflicts))
```

`Router.serving` finds the board's routes, and compiles them, once,
into what a request is found its route by, then makes each route's slot
and spawns its waiter; where they cannot be served, two routes of one
method and path, say, it is a `Left`, each conflict said, before any
slot is made or anything is listened on.
`Net.withListener` listens on the port, and holds the listener with a
`with`, a `With s Net.TcpListener` in a region `s` of its own, for its
body, `served`. Where the port cannot be listened on, it is a `Left`,
and `listenedOr` says why.

`BocServer.serve Empty l handler` comes back at once: the server runs
in its behaviours, each connection a cown of its own, each request
handed to the router (`Router.dispatch`) in a behaviour on it. The
router finds the route there, holding no cown: it answers a handler of
the request alone there, and puts any other request in its route's
slot, by a behaviour on that cown alone, so a request to one route
never waits for another's. Nothing throws, so the server's error type
is `Empty`. `served` then awaits a signal: SIGTERM, as `kill` and
service managers send, or Ctrl-C. Once one comes, it closes the
listener, and the server stops: the event streams are ended, and each
connection is closed once the request in hand is answered. The program
ends once nothing of the server is left.

`main`'s world names what the service does: boc's behaviours, the
standard streams, std.http's client, sockets, the time the server keeps
its limits by, signals, and the arguments, for the port.

## Trying it

```sh
$ heff test app/web/main.hf
...
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

and, in another, wait for the fourth todo:

```sh
$ curl 'localhost:8080/todos?after=3'
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
Allow: GET, HEAD, POST
Content-Length: 19

method not allowed
$ curl localhost:8080/nothing
no such page
```

Once there is a fourth todo, the long poll is answered:

```sh
$ curl -d 'ship it' localhost:8080/todos
{"added":1,"todos":4}
```

```
[{"title":"ship it","done":false}]
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

event: added
data: ship it

```

Start a second service, `.heff/bin/web 8081`, and have it copy the
first's open todos (the `?` of the URL in the query written `%3F`):

```sh
$ curl -X POST 'localhost:8081/import?from=http://localhost:8080/todos%3Fstatus=open'
{"added":2}
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
