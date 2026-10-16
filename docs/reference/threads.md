---
title: Threads and places
sidebar:
  badge:
    text: unstable
    variant: caution
---

> [!CAUTION]
> Unstable: concurrency in heff is not settled. What this page describes
> may change, or go, in any release.

A C program can run heff in several OS threads at once, in two ways:

- **threads** (`std.thread`) share values between them;
- **places** (`std.place`) share nothing, and pass values as messages.

Neither has a JavaScript side: a program that uses them is a C program.

## std.thread

```haskell
import co
import std.io
import std.thread

-- | the primes below `n`
primesBelow : Int -> Int
primesBelow n = List.length (List.filter isPrime (List.range 2 (n - 1)))
  where
  isPrime : Int -> Bool
  isPrime k = List.all (\d -> k % d /= 0) (List.range 2 (k / 2))

counted : {e : HFunctor} -> {{io : stdio <: e}} -> {{th : threads <: e}} -> Co e Unit
counted = do
  n <- cores
  counts <- parFor n 8 (\i -> primesBelow (1000 * (i + 1)))
  print (show counts)
  names <- parMap 4 (\s -> s ^ "!") ["a", "b", "c"]
  print (joinWith " " names)
```

| Operation | What it does |
|---|---|
| `parMap n f xs` | `f` of each of `xs`, by `n` threads, the caller one of them; the answers in order |
| `parFor n count f` | `f 0` to `f (count - 1)`, by `n` threads |
| `cores` | how many threads the machine runs at once |
| `spread n` | run the rest of the program in `n` threads; each gets its index |
| `gather v` | in thread 0, every thread's `v`, in order; the other threads end here |
| `inThreads a b e` | a handler running each side of `parallel`'s `Parallel` in a thread |

- A task may take and give values of any type.
- `f` and the values are shared with the threads, not copied. A large
  value every task reads is in memory once.
- A shared value is never updated in place: updating it copies it.
- If a task fails, the others stop taking more, and the caller fails as
  the first failed task did, in the list's order.
- A spread's threads run the rest of the program, effects and all: what
  one prints, all print.

### What is safe

Safe in threads at once: pure code and the definitions it reads,
`std.io` (each write is whole), and `std.reference`'s cells (locked).

Not safe:

- `exit` in any thread ends the process.
- A provider's own tables are its own to lock. Among the library's,
  `std.progress` locks its own, and the others keep none.
- One thread letting go of a host's thing while another uses it is the
  providers' to prevent.
- Windows is not tested.

## std.place

```haskell
import std.place

-- | the first `n` numbers sent from another place, and their sum
summed : {e : HFunctor} -> {{io : stdio <: e}} -> {{pl : places <: e}} -> Int -> Co e Unit
summed n = do
  ch <- newPlaceChannel
  sender <- spawnPlace (sendPlace ch (List.range 1 n))
  xs <- recvPlace ch
  joinPlace sender
  print (show (List.sum xs))
```

A place, as in Racket, is a thread with a heap of its own.

| Operation | What it does |
|---|---|
| `spawnPlace c` | run the computation `c` in a new place |
| `joinPlace p` | wait for a place's value; a place is joined once |
| `newPlaceChannel` | a new channel, `PlaceChannel a` |
| `sendPlace ch v` | send `v` on the channel, and go on at once |
| `recvPlace ch` | take the next value sent, waiting for one |

- A value sent to another place is copied, so no object is ever held by
  two places. Strings, bytes and arrays larger than 128 bytes that the
  sender alone holds are moved instead, without copying.
- A channel can carry any type, including functions and channels.
- A place that fails makes its joiner fail.
- A place's working directory is its own: it starts in its starter's,
  and `setWorkingDir` moves only that place.
- `recvPlace` and `joinPlace` block the whole place, unless they run in
  a group of green threads (`std.reactor`): then only that green thread
  waits, and the others go on.

Copying costs some 15 ns a list cell. A program that sends a lot should
send answers rather than inputs, or use `std.thread`.

## Writing a provider that runs threads

A provider that starts threads says so, on a line of its own:

```c
#define HEFF_THREADS 1
```

The whole program is then built for threads. A program none of whose
providers says it is built as before, and runs as fast as before.

What such a provider uses, from `lib/c/runtime.c`:

| Function | What it does |
|---|---|
| `heff_share(v)` | mark `v` shared, before handing it to another thread |
| `heff_try(f, x, &out)` | run `f x`, catching a failure: `NULL`, or why it failed |
| `heff_fail_again(why)` | fail, in the joining thread, as the thread did |
| `heff_thread_start(&t, run, arg)` | start a thread with a large stack |
| `heff_thread_end()` | give the thread's memory back as it ends |
| `heff_freeze(v)` | make `v` immortal: never freed or counted |
| `HEFF_TLS` | declare per-thread state |

`lib/std/thread.provider.c` is the library's.
