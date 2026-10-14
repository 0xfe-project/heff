# Threads

`lib/parallel.hf`'s `Parallel` says what may run at once; a handler says how
it runs.  In C, a provider may run heff in threads of its own, truly at
once (#36).  The library's is `std.thread` (`lib/std/thread.hf`, its
provider `thread.provider.c`); `test/node/threads.hf` uses all of it.

## std.thread

```haskell
import co
import parallel
import std.io
import std.thread

world : HFunctor
world = stdio <+> threads

program : {{io : stdio <: world}} -> {{th : threads <: world}} -> Co world Unit
program = do
  n <- cores
  counts <- parFor n 1000 (primesInBlock 5000000)
  print (show (List.sum counts))
  names <- parMap 4 (\s -> s ^ "!") ["a", "b", "c"]
  r <- handle (inThreads Int Int world) (fibPar 3 30)
  ...
```

- `parMap n f xs`: `f` of each of `xs`, by `n` threads (the caller one
  of them), each taking the next value as it ends one; the answers, in
  order.  `parFor n count f` is `f 0 .. f (count - 1)`.  What a task
  takes and gives may be of any type -- a string, a list, a record -- and one `threads`
  in the world serves them all: an operation keeps its types to itself.
  One thread, or one value, runs in the caller, nothing shared.
- `cores`: how many threads the machine runs at once.
- `spread n`: the rest of the program given to each of `n` threads,
  shared by them, its index its answer; `gather v`: in thread 0, every
  thread's `v`, in order, every other thread ending there.  Spreads
  nest: a gather joins its own spread's threads.
- `inThreads a b e`: `Parallel` handled into `Co e`, each side a thread (a
  spread into two, joined again); `threaded` is its algebra, for a
  handler of one's own.  As with any handler into `Co e`, a side's pure
  work is done where it is handled, before the spread, unless it comes
  after an operation or after `later`: `parallelIn (later (\u -> fib n))
  ...`.
- A task that fails (`abandon`, say) stops the others taking more, and
  the caller fails as the first that failed, in the list's order, would
  have failed it: the program fails as it would have, one thread doing
  all.  A thread of a spread that fails fails thread 0 at the gather.

There is no JavaScript side: the JavaScript backend is the browser's,
and a program with `threads` in its world is compiled for C
(`--backend=c`; a page's script refuses it, the effect not provided).

### What it costs

- Nothing is copied.  `f` and each value are shared (below): walked
  once, each object of them marked, and then held by each thread that
  takes them by one more count.  An answer is handed back as it is.
  A large value every task reads -- held by `f` -- is in memory once,
  however many threads read it.
- A spread shares the rest of the program with each thread: what was
  made before the spread, and is used after it, is held by them all,
  not copied.
- What is shared is counted atomically by every thread that holds it,
  thread 0 too, from then on, and is never updated in place: an update
  of it copies it, as one counted twice is copied in one thread.  A
  task that builds on what it was given builds its own, and that is
  counted as before.

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

An object is one of three (#66).  A thread's own: counted by it alone,
not atomically -- whatever a thread makes is its own.  Shared
(`SHARED`, a bit of its count beside `IMMORTAL`): counted atomically by
every thread that holds it, and freed by the one that lets it go last.
Or immortal: counted by none, never freed.  What a shared or an
immortal object holds is shared or immortal; a thread's own object may
hold a shared one, not the other way.

- **`heff_dup` and `heff_drop`** test `rc & (IMMORTAL | SHARED)` where
  one thread tests `rc & IMMORTAL`: the same branch, another constant.
  Under it, out of line: immortal, nothing; shared, `__atomic_fetch_add`
  or `__atomic_fetch_sub` on the count, the one that takes it to 0
  freeing it.  So is every other place that counts an object down (a
  match giving up its scrutinee, a field taken from a record, the
  frees' walk).  A count is read atomically too, relaxed -- a plain
  load, to the processor.  Without `HEFF_THREADS`, `SHARED` is 0 and
  the program is what it was.
- **Reuse:** an object is updated in place where its count is 1
  (`heff_unique`), which a shared one's never is: a shared object is
  copied where it is updated.
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
- **A shared or immortal thunk forced** (`heff_force_shared`, a closure
  of one parameter forced by `MkUnit`, whose `memo` other threads may
  read and write): its value is shared, then put in the `memo` by a
  compare-and-swap; a thread that computed it meanwhile and lost takes
  the `memo`'s and drops its own, which is freed.  A shared thunk's
  value is counted as any shared object is.  A static thunk is
  immortal, its `memo` holds the value for good, so the first freezes
  it once it is there: shared to publish, frozen once published, no
  thread counting it after.  A big integer written in the program is
  made once likewise, another thread's made meanwhile freed.
- **The world's table** of handles (`heff_hold`, `heff_held`,
  `heff_release`) is locked, and so are `std.reference`'s cells: a cell
  holds a shared value, a read is one more count of it, a write shares
  the new value and swaps it in under the lock.

Why a definition's value is computed once and frozen, not shared and
counted: the language says a definition is one value; it is computed
once, not once per thread (a big table, or one that takes long); it is
in memory once; and frozen, no thread counts it at all, where a shared
value is counted atomically each time it is used.  What it costs: the
value is walked once, when it is made, and is never freed -- as a
definition's value never was.

## What a provider does

`lib/std/thread.provider.c` is a provider that runs heff in threads;
another may do the same.  What a provider hands another thread, it
shares, and it runs it there under a catch:

- `V heff_share(V v)` -- `v` shared, and given back, not consumed: each
  of its objects marked `SHARED`, the walk stopping at what is shared
  already or immortal.  Then each thread it is handed to holds it by
  one more count (`heff_dup`), and lets it go by `heff_drop`.  Handing
  one value to n threads walks it once; shared again, only what is new
  since is walked.  `v` is this thread's: no other changes it
  meanwhile.
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
- `heff_freeze(v)` -- `v` made immortal, for good: it is never freed,
  and no thread counts it.
- `HEFF_TLS` -- for a provider's own per-thread state (the spreads a
  thread has open, say).

What a thread gives back (its answer) is its own; once the thread is
joined it is the joiner's, and nothing more is needed: no other thread
holds what of it is not shared.

## What is safe, and what is not

Safe, in threads at once: pure code, all of it -- a task, a side of a
`Parallel`, a spread's thread -- and the definitions it reads; `std.io`
(a write is one `fwrite`, whole: C locks the stream); `std.reference`'s
cells (locked, a value in them shared); the world's table of handles
(locked).  A task of a pool is pure: it has no effects to run.

Not safe, or not as one thread would do:

- A thread of a spread runs the rest of the program, effects and all:
  what one prints, all print, in no order but their own; a file written
  by each is written by each.  `exit` in any ends the process.
- A failure in a thread not run under `heff_try` ends the process (it
  prints why and exits), as it does in one thread; under `heff_try` it
  is handed back.  A definition being computed when a failure is caught
  is left to be computed again, and a thread waiting for it is woken.
  What was made before the failure is leaked.
- A thread that ends without `heff_thread_end` (by `pthread_exit`, say)
  leaks its free lists.  An object made by one thread and given back by
  another goes to the second's free lists, so where one thread makes
  and another drops -- a shared value, its last holder another thread
  -- the second's lists grow.  Threads mode keeps no pages: each object
  is the system's, on a list of its size.
- A frozen value is never freed: a definition's, a static thunk's, and
  what of it was shared besides, though another thread held it too.
- A shared value is counted atomically each time it is used, by every
  thread, its maker too: a value read much after it is handed over
  costs more to read than one never shared.  A shared value updated is
  copied, never updated in place.
- A count is 30 bits in threads mode (31 otherwise): an object held
  more than a billion times at once is not counted right.
- Other providers' tables are their own to lock.  The library's:
  `std.reference` and the world's table are locked; `std.progress` locks its
  own; `std.io`, `std.filesystem`, `std.http`, `std.net`, `std.time`
  and `std.process` keep no tables (`exit` in a thread ends the
  process; `std.net`'s sockets are the world's).
  `heff_held(i)` is a pointer: a thing let go of by one thread while
  another uses it is the providers' to prevent.
- Per-thread state costs: on macOS a `_Thread_local` is reached through
  a call, so a program that allocates much runs some 5% more
  instructions in threads mode, one thread working (queens, below).
- Windows is not tested.

## Green threads in threads

std.reactor's reactor, where green threads' outside operations wait
([providers.md](providers.md#outside-operations-done-later)), is each
thread's own (`HEFF_TLS`): a scheduler's operations are pending in the
thread it runs in, polled by it alone, and a thread of a spread may run
a scheduler of its own.  A task of a pool is pure, and waits on
nothing.  A spread in a green thread is not to be done: what a spread
gives each thread, the rest of the program, is there only the rest of
that thread's operation.
A name std.net resolves under a scheduler is resolved by a thread of
the system started for it, which touches nothing of heff's, in a
program built for threads or not.

## Measured

On a 14-core Apple M-series machine, `test/node/threads.hf`'s
`cpu WORK N W` (threads mode, `zig cc -O2`) against the same work in a
program not built for threads (W = 8 and 14 as measured before #66,
the rest after it, the machine busy with other builds; sharing moved
none of them):

| work | one thread | W=1 | W=2 | W=4 | W=8 | W=14 |
|---|---|---|---|---|---|---|
| primes below 5,000,000 | 997 ms | 933 ms | 470 ms | 243 ms | 133 ms | 103 ms (9.6x) |
| queens 12 | 204 ms | 200 ms | 105 ms | 57 ms | 41 ms | 30 ms (6.6x) |
| fib 38 | 126 ms | 120 ms | 62 ms | 35 ms | 20 ms | 15 ms (8.5x) |

One large value handed to W threads, each reading it all (`parFor W W`
of a function holding it), copied for each thread before #66 and
shared since -- the time of the whole program, and its peak memory:

| value | W=1 | W=2, copied | W=2, shared | W=4, copied | W=4, shared |
|---|---|---|---|---|---|
| a list of 10^6 integers | 12 ms, 33 MB | 57 ms, 127 MB | 17 ms, 33 MB | 140 ms, 193 MB | 17 ms, 33 MB |
| 200,000 records of two strings | 25 ms, 37 MB | 63 ms, 134 MB | 29 ms, 37 MB | 137 ms, 201 MB | 29 ms, 37 MB |

A `std.reference` cell holding a list of 1000, read and written back
100,000 times in threads mode: 4.0 s copied each way, 100 ms shared (95
ms in a program not built for threads).

## Testing

`heff test test/backends.hf` runs `test/node/threads.hf` (its `main`, `generic`,
`failing`, `failingTask` and `cpu`); with `HEFF_SANITIZE=1` under
AddressSanitizer (`make sanitize`), and with `HEFF_TSAN=1` again under
ThreadSanitizer (`make tsan`), built by the system's clang.
