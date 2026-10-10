# Threads

`lib/parallel.hf`'s `Par` says what may run at once; a handler says how
it runs.  In C, a provider may run heff in threads of its own, truly at
once (#36).  `test/node/threads.hf` and its `threads.provider.c` are an
example: a spread of threads, each going on with a copy of the rest of
the program (as `test/node/fork.hf` does with processes), a pool of
workers for `f 0 .. f (n - 1)`, and `Par` each side a thread.

## Turning it on

A provider that starts threads says so, a line of its own:

```c
#define HEFF_THREADS 1
```

The emitter puts that line before the runtime too, so the whole
program is built for threads: `heff build`, `heff install` and
`heff compile` need no flag.  (`-DHEFF_THREADS` does the same by hand.)
A program none of whose providers says it is built as before, and runs
as fast as before: nothing below is in it.

## What the runtime does

Counts are not atomic.  So a mortal object is only ever one thread's,
counted by it alone, and what is shared is immortal, counted by none.

- **Per-run state is per thread** (`HEFF_TLS`, `_Thread_local` in
  threads mode, nothing otherwise): the free lists, the stack of what is
  given back, the count of calls before a bounce, the room for a jump's
  arguments, and where a failure is caught (`heff_catch`).
- **A definition's value is computed once**, by the first thread to ask
  for it, under a lock (`heff_global`); a thread asking meanwhile waits
  for it.  It is then made immortal (`heff_freeze`), it and all it holds,
  so every thread has the same value and none counts it.  A thread
  asking for what it is itself computing is told it is defined by
  itself.  Definitions are in order, none using one after it, so a
  thread waits only for one before the one it computes: no deadlock.
- **A static thunk's value** (an immortal closure of one parameter,
  forced by `MkUnit`, whose `memo` all threads share) is made immortal
  too, and kept by the first thread to have it (`heff_force_shared`);
  another computing it meanwhile has the same value and lets its own
  go.  A big integer written in the program likewise.
- **The world's table** of handles (`heff_hold`, `heff_held`,
  `heff_release`) is locked, and so are `std.ref`'s cells, whose values
  go in and out as copies.

Why a definition's value is computed once and frozen, not computed by
each thread for itself: the language says a definition is one value;
it is computed once, not once per thread (a big table, or one that takes
long); it is in memory once; and frozen, no thread counts it, where a
value shared and counted would be counted by two threads at once.  What
it costs: the value is walked once, when it is made, and is never freed
-- as a definition's value never was.

## What a provider does

What a provider hands another thread, it copies, and it runs it there
under a catch:

- `V heff_copy_for_thread(V v)` -- a copy of `v` for another thread, `v`
  not consumed: what is immortal is shared, every other object is made
  again, and what `v` shares the copy shares.  The provider no longer
  knows how objects are laid out.  To copy two values that share
  objects, copy them as one (a pair, say), so the copy shares them too.
- `const char *heff_try(V f, V x, V *out)` -- `f x`, its failure
  caught: `NULL` and the value at `*out`, or why it failed
  (`"heff:abandon"` where it abandoned).  Copy the reason before the
  thread ends: it may be the thread's own.
- `heff_fail_again(why)` -- in the thread that joins, fails as the
  thread did: the program fails as it would have, one thread doing all.
- `heff_thread_end()` -- as a thread ends: its free lists, and what
  else the runtime kept for it, given back to the system.
- `heff_thread_start(&t, run, arg)` -- a pthread with a large stack
  (`HEFF_THREAD_STACK`, 256 MB reserved): heff's recursion is the C
  stack's.
- `heff_freeze(v)` -- `v` made immortal, to share without copying, for
  good: it is never freed.
- `HEFF_TLS` -- for a provider's own per-thread state (the spreads a
  thread has open, say).

What a thread gives back (its answer) is its own; once the thread is
joined it is the joiner's, and nothing more is needed: no other thread
holds it.

## What is still not safe, or costs

- A failure in a thread not run under `heff_try` ends the process (it
  prints why and exits), as it does in one thread; under `heff_try` it
  is handed back.  A definition being computed when a failure is caught
  is left to be computed again, and a thread waiting for it is woken.
  What was made before the failure is leaked.
- A thread that ends without `heff_thread_end` (by `pthread_exit`, say)
  leaks its free lists.  An object made by one thread and given back by
  another goes to the second's free lists, so where one thread makes
  and another drops, the second's lists grow.
- A frozen value is never freed: a definition's, a static thunk's, and
  what the computing thread held of it besides.
- A copy is as big as what it copies, and takes as long: a large value
  handed to many threads is copied for each (or frozen once, and kept).
  `std.ref` copies a value each time it is read or written.
- Other providers' tables are their own to lock.  The library's:
  `std.ref` and the world's table are locked; `std.progress` locks its
  own; `std.io`, `std.filesystem`, `std.net`, `std.time` and
  `std.process` keep no tables (`exit` in a thread ends the process).
  `heff_held(i)` is a pointer: a thing let go of by one thread while
  another uses it is the providers' to prevent.
- Per-thread state costs: on macOS a `_Thread_local` is reached through
  a call, so a program that allocates much runs some 5% more
  instructions in threads mode, one thread working (queens, below).
- Windows is not tested.

## Measured

On a 14-core Apple M-series machine, `test/node/threads.hf`'s
`cpu WORK N W` (threads mode, `zig cc -O2`) against the same work in a
program not built for threads:

| work | one thread | W=1 | W=2 | W=4 | W=8 | W=14 |
|---|---|---|---|---|---|---|
| primes below 5,000,000 | 989 ms | 1032 ms | 503 ms | 250 ms | 133 ms | 103 ms (9.6x) |
| queens 12 | 199 ms | 215 ms | 117 ms | 68 ms | 41 ms | 30 ms (6.6x) |
| fib 38 | 128 ms | 126 ms | 66 ms | 37 ms | 20 ms | 15 ms (8.5x) |

## Testing

`test/backends.mjs` runs `test/node/threads.hf` (its `main`, `failing`
and `cpu`); with `HEFF_SANITIZE=1` under AddressSanitizer, and with
`HEFF_TSAN=1` again under ThreadSanitizer, built by the system's
clang.
