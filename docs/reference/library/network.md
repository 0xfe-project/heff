---
title: Network
---

These are C programs' effects. A page uses `browser.http` instead.

## std.http

```haskell
import co
import std.http as Http

-- | the response's status, or 0 where none came
upload : {e : HFunctor} -> {{web : Http.http <: e}} -> Bytes -> Co e Int
upload b = do
  r <- Http.send
         { url = "http://localhost:8080/up"
         , method = "POST"
         , body = b
         , timeout = 5000
         }
  pure (either (\err -> 0) (\(res : Http.Response) -> res.status) r)
```

`std.http`, imported as `Http`, is an HTTP client.

- `Http.send request` sends a `Request`: `url`, and optionally `method`,
  `headers`, `body` (`Bytes`), `timeout` (milliseconds, 0 for none) and
  `follow` (follow redirects; `True`).
- `Http.get url` and `Http.getText url` are `GET`s. `getText` gives the
  body as text, and an error for a status that is not 2xx, as
  `curl --fail` does.
- The answer is `Either HttpError Response`. A `Response` has `status`,
  `headers` and `body`, whatever the status. An `HttpError` means no
  response came, with curl's `code` and `message`.
- `Http.text` reads a body as text, `Http.header` finds a header, and
  `Http.showError` prints an error as curl does.

It uses libcurl, loaded at the first request. Where there is none, it
runs the `curl` command instead, with the same answers.

## std.net

```haskell
import bytes as Bytes
import std.net as Net
import with

-- | what the other side writes, written back until it closes
echo : {e : HFunctor} -> {s : Type} -> {{nt : Net.net <: e}} -> With s Net.TcpStream -> Co e Unit
echo c = do
  r <- Net.read c 65536
  case r of
    Right b -> if Bytes.length b == 0 then pure MkUnit else do
      _ <- Net.write c b
      echo c
    Left err -> pure MkUnit
```

`std.net`, imported as `Net`, is TCP.

| Function | What it does |
|---|---|
| `Net.withConnection address body` | connect, run `body` with the connection, then close it |
| `Net.withListener address backlog body` | listen, run `body` with the listener, then close it |
| `Net.accept l body` | run `body` with the next connection, then close it |
| `Net.read c n` | up to `n` bytes that have come; none at the end |
| `Net.write c b` | write all of `b` |
| `Net.shutdown c` | end this side's writing |
| `Net.peer c`, `Net.thisSide c`, `Net.local l` | addresses |

- An `Address` is a `host` and a `port`. The host can be a name, which
  is resolved and each address tried, or a literal address. Listening
  on host `""` means every address; port 0 lets the system pick.
- An operation that can fail gives `Either NetError a`: errno as `code`,
  its `name` (`ECONNREFUSED`), and its `message`. A `with` that fails
  throws its `NetError`.
- Every operation waits, and the whole program with it, unless it runs
  in a [green thread](concurrency.md).
- Windows has no sockets yet.

## std.http.server

```haskell
import exception
import std.http.server as Server

hello : {e : HFunctor} -> Server.Request -> Co e Http.Response
hello r = pure (Server.plain 200 ("hello, " ^ r.target ^ "\n"))

-- | `hello` served on the port, until a connection cannot be accepted:
-- | then that is thrown
helloAt : {e : HFunctor}
        -> {{nt : Net.net <: e}}
        -> {{fail : exception Net.NetError <: e}}
        -> Int
        -> Co e Unit
helloAt port =
  Net.withListener { host = "127.0.0.1", port = port } 64 (\l -> Server.serve l hello)
```

`std.http.server`, imported as `Server`, is an HTTP/1.1 server in heff,
over `std.net`.

- `Server.serve listener handle` answers each request by `handle`, one
  connection at a time.
- `Server.serveConcurrently listener handle` serves each connection in
  a green thread of its own, many at once.
- `Server.serveUntil last listener handle` stops after the request
  `last` says is the last.
- `Server.serveConnection` serves one connection.
- `Server.plain status text` is a text response.

A `Server.Request` has `method`, `target` (`/items?id=3`), `version`,
`headers` (`Server.header` finds one), `body` and `peer`. `handle` gives
back an `Http.Response`.

When a connection cannot be accepted, such as when the program is out
of descriptors, they stop and throw its `NetError`, leaving the listener
as it is.

Limits: a request head of 64 KiB, a body of 64 MiB. Connections are kept
alive, unless HTTP/1.0 or `Connection: close` says otherwise. A chunked
request body is answered `501`.

## An API between programs

`remote` sends an effect's operations, as [wire](formats.md) bytes, to
another program: `std.remote` is a C program's side, over HTTP, and
`browser.remote` a page's. `examples/notes/` is a page and a server
sharing one effect this way.
