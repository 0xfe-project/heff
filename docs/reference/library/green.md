---
title: Green threads
sidebar:
  badge:
    text: unstable
    variant: caution
---

> [!CAUTION]
> Unstable: concurrency in heff is not settled. What this page describes
> may change, or go, in any release.

```haskell
import co
import green

ponger : {e : HFunctor} -> {{ev : green <: e}} -> Int -> Int -> Co e Unit
ponger a b = do
  x <- recv a
  send b (x + 1)
  ponger a b

pinged : {e : HFunctor} -> {{ev : green <: e}} -> Co e Int
pinged = do
  a <- newChan
  b <- newChan
  spawn (ponger a b)
  send a 0
  recv b

-- | main and one thread: 0 sent there, and `Just 1` back
pingedHere : {e : HFunctor} -> Co e (Maybe Int)
pingedHere = scheduled e (pinged (ev := inl green e))
```

`green` is green threads: many threads in one OS thread, taking turns.
It is an effect, and its scheduler is a handler written in heff, the
same on every backend.

| Operation | What it does |
|---|---|
| `spawn c` | run `c` as a thread of its own |
| `yield` | give the next thread its turn |
| `newChan` | a new channel |
| `send ch x` | put `x` on the channel; never waits |
| `recv ch` | take the first value on the channel, waiting for one |

A thread gives up its turn at `yield`, at a `recv` with nothing to take,
and when it ends. Turns go round robin.

`scheduled e c` runs `c` and the threads it spawns, in `Co e`. It gives
`Just` the main thread's answer, or `Nothing` if every thread waits on a
channel. The other threads are dropped when the main one ends.

## Waiting apart: std.reactor

With `scheduled`, an operation of the outside world that waits, such as
a socket read, makes every thread wait. In a C program, `std.reactor`'s
scheduler lets the others go on:

```haskell
import std.reactor as Reactor

served : {e : HFunctor} -> {{rx : Reactor.reactor <: e}} -> Co (green <+> e) Unit -> Co e (Maybe Unit)
served c = Reactor.scheduled e c
```

`Reactor.scheduled e c` needs `Reactor.reactor` in the world. While a
thread waits for one of these, the others run:

- `std.net`'s `read`, `write`, `accept` and `withConnection`;
- `std.filesystem`'s reads and writes of a pipe, a terminal or a socket;
- `std.process`'s `wait`, and reads and writes of a command's pipes;
- `std.http`'s requests;
- `std.place`'s `recvPlace` and `joinPlace`, and `std.time`'s sleep.

When no thread is ready, the scheduler waits for the first operation to
finish, in one `poll`. A regular file is read and written at once, the
program waiting.

A server can accept each connection in a thread of its own: see
`Server.serveConcurrently` in [Network](network.md#stdhttpserver).

Each OS thread has its own reactor, so green threads inside a
[thread or place](../threads.md) are that thread's own.
