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
  threads mode, nothing otherwise): its pages, the stack of what is
  given back, the count of calls before a bounce, the room for a jump's
  arguments, and where a failure is caught (`heff_catch`).
- **A page is the thread's that made it** (its owner): objects of up
  to 128 bytes are made in pages of 64 KB, each of one size, as in one
  thread, and each thread has pages of its own (`heff_heap`).  What the
  owner gives back goes on the page's list, nothing atomic -- whatever
  was not shared.  What another thread gives back (a shared object, its
  last holder another thread) goes on the page's second list,
  `remote`, by a compare-and-swap, nothing else of the page touched.
  The owner takes that list over whole, by one exchange, where it finds
  no room in the page it makes in (`heff_page_alloc`, the slow path),
  before it looks to another page; and looks through its other pages
  for what other threads gave them only when something was given so
  since it last looked.  A thread that ends (`heff_thread_end`) gives
  back to the system its pages that hold nothing, and leaves the others,
  abandoned, to the next thread that finds no room in a page of their
  size: it adopts them, their owner from then on.
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
- **`std.reference`'s cells** are locked: a cell holds a shared value,
  a read is one more count of it, a write shares the new value and
  swaps it in under the lock.  A foreign value (a host's thing, a file
  open) shared between threads is let go of once, by the last to drop
  it; the thing itself is its provider's to guard.

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
- `heff_thread_end()` -- as a thread ends: its pages that hold
  nothing, and what else the runtime kept for it, given back to the
  system; its pages that still hold something left for another thread
  to adopt.
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
cells (locked, a value in them shared).  A task of a pool is pure: it
has no effects to run.

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
  leaks its pages: none adopts them, and what is given back to them
  stays on their `remote`.
- A page another thread gave back to is reused once its owner finds no
  room elsewhere of that size: where one thread makes and others give
  back, the maker holds what they gave back until it next makes that
  size.  A thread that never ends (the program's own) keeps its pages,
  and abandoned pages nothing of their size is made in again stay
  abandoned.
- A frozen value is never freed: a definition's, a static thunk's, and
  what of it was shared besides, though another thread held it too.
- A shared value is counted atomically each time it is used, by every
  thread, its maker too: a value read much after it is handed over
  costs more to read than one never shared.  A shared value updated is
  copied, never updated in place.
- A count is 30 bits in threads mode (31 otherwise): an object held
  more than a billion times at once is not counted right.
- Other providers' tables are their own to lock.  The library's:
  `std.reference`'s cells are locked; `std.progress` locks its own;
  `std.io`, `std.filesystem`, `std.http`, `std.net`, `std.time` and
  `std.process` keep no tables (`exit` in a thread ends the process).
  A foreign value's pointer is the host's thing itself: one let go of
  by one thread while another uses it is the providers' to prevent.
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

## Places

A place (#97, as Racket's) is a thread with a heap of its own that
shares nothing: what a place makes stays on its own pages, counted by
it alone, never atomically, and a value given to another place is
copied -- or moved, where the sender held it alone -- never shared.
Places and shared threads are two uses of one object model, not two
runtimes: a place is a thread as `std.thread` starts one, with the
pages every thread has (#66); what makes it a place is that its
provider marks nothing `SHARED`.  The library's is `std.place`, whose
provider builds the runtime for threads as `std.thread`'s does.

```haskell
import co
import std.io
import std.place

world : HFunctor
world = stdio <+> places

program : {{io : stdio <: world}} -> {{pl : places <: world}} -> Co world Unit
program = do
  p <- spawnPlace (pure (fib 30))        -- run there, from now
  ch <- newPlaceChannel                   -- a PlaceChannel (List Int)
  q <- spawnPlace (sendPlace ch (List.range 1 100000))
  xs <- recvPlace ch                      -- the list, made again here
  n <- joinPlace p                        -- fib 30, copied back
  ...
```

- `spawnPlace c`: `c`, a computation of the world, run in a place of
  its own; the `Place a` it comes to.  `joinPlace p`: its value, waited
  for, made the joiner's; a place is joined once.  A failure in a place
  (`abandon`) is the joiner's at the join, as a thread's is: the place
  ends alone, the program fails when it joins it.
- `newPlaceChannel`: a `PlaceChannel a`; `sendPlace ch v` puts `v` on
  it and goes on at once; `recvPlace ch` takes the next value sent,
  waiting for one.  A channel is typed, and may carry any type -- a
  record, a list, a function, a channel -- one effect for them all.
  What a place is to hold (a channel, a table to look things up in) it
  holds through the computation given to it, copied with it.  A place
  and a channel are the host's own things (foreign values, `Place a`
  and `PlaceChannel a`): a channel crosses to each place that is given
  it, which holds it as its own, and is freed, with what was sent on it
  and not received, when no place holds it; a place dropped before it
  is joined is joined where the program drops its last reference to it,
  what it came to left unread.
- A receive blocks the whole place, unless it is in a group of
  std.reactor's (green threads scheduled by `Reactor.scheduled`): then
  it is pending, a wait of the reactor's, the other green threads of
  the place going on, and the thread that received runs on once
  something is sent.  A join is the same: blocking, or pending until
  the place has ended.

The working directory is each place's own, not the process's: a
place starts in its starter's directory (the main place, in the
process's), `setWorkingDir` moves that place alone, `workingDir` says
where it is, and every relative path a place gives `std.filesystem`
or `std.process` is taken from there -- a command it starts runs
there, its `dir` under there.  The runtime keeps the directory for
each place and joins each relative path to it before the system sees
it; the process's own working directory is never changed, except in a
command started, between its fork and its exec.

### Green threads in a place

The reactor is each thread's own, so each place has one, the main
place included, and a place's scheduler drives it: a place that runs
`Reactor.scheduled` runs green threads whose waits are all pending in
that place's reactor -- a receive on a place channel, a place joined, a
command of std.process waited for or read from, a socket of std.net, a
request of std.http, a sleep of std.time -- the other green threads of
the place going on meanwhile, and the place's OS thread blocked only
when every thread of it waits, in one `poll` of everything they wait
for.  Nothing of one place's reactor is touched by another: a send
from another place wakes the receiver through the channel's pipe, a
place ending wakes its joiner through its own, and the rest is the
place's alone, so there is no lock on the way.  A place with no
scheduler waits where it is, the whole place with it, as a program with
no scheduler does.

What a wait costs: a pending wait is a struct of its provider's, linked
in the place's reactor, and one descriptor in the reactor's `poll` --
a channel's pipe (made when a receive first pends on it, kept for the
channel's life), a place's pipe (made with the place, closed at the
join), a command's pipes and the descriptor that says it exited, a
socket; a sleep has none, only a deadline, which bounds the poll.  A
turn of the reactor is one `poll` and a walk over the pending waits:
a thread that waits costs the others nothing until something comes for
it.

```haskell
-- in a place's scheduler: three threads, each waiting for something
-- else, and the rest going on
spawn (do
  sleep 600
  print "slept")
spawn (do
  n <- recvPlace asks
  print ("received " ^ show n))
spawn (do
  r <- withProcess { command = "sleep", args = ["1.2"] } (\p -> wait p)
  print "the command ended")
print "went on meanwhile"
```

### How a value crosses

What a place is given to run, what it is sent and what it gives back
cross as a message: a flat buffer of words that is no heap's.  The
sender walks the value once and writes it into the buffer -- each
object a header word (its kind, its tag, how many fields) and its
fields, an integer or a tag of no fields its word as it is, an
immortal object (a definition's value, a string written in the
program) its pointer, since it is the program's and counted by none --
and then drops its own value as it would any other.  The receiver reads
the message and makes a value of its own out of it, each object in its
own pages, and frees the message.  So no object is ever held by two
places, none is counted atomically, and no page holds another place's
object.  What a value holds twice -- an object of count more than one
-- is numbered in the message and made once in the receiver too, so
that a copy is no bigger than its original.  A foreign value (a `FILE
*` the program holds) does not cross -- sending one fails -- unless its
provider made it shared, as a channel is: then the thing is held once
more, and the receiver has a foreign value of its own for it.

Copy against move: an object the sender holds alone -- its count 1,
and so every object's above it in the value -- that is the system's,
not a page's (a string, bytes, an array of numbers or a big integer of
more than 128 bytes, which the allocator gives to `malloc`) is moved:
its pointer goes in the message, its bytes are not copied, and the
sender forgets it.  Everything else is copied, because a small object
is in a page of its maker's, and the receiver's objects must be in the
receiver's own pages: moving a list would leave its cells on the
sender's pages, freed later by the receiver onto the pages' remote
lists -- the shared threads' path, which places are there to avoid.
A long string sent is thus free; a long list is made again.

The other way -- the receiver reading the sender's objects where they
are, allocating its own as it goes -- would walk the value once
instead of twice, but the sender's value would have to stay, and stay
unchanged, until the receiver was done with it: either a send waits for
its receive, or the receiver hands back what to drop.  A buffer the
sender fills and forgets keeps the two apart, lets a channel hold any
number of messages, and makes a pending receive a wait on a
descriptor: a channel asked for one keeps a pipe readable while it
holds a message, and the reactor polls it with the rest.

What a channel costs the program: it is held by the world for the
program's life (there is no closing it yet).  A place's `exit` ends
the process, as a thread's does.  Definitions' values are computed
once and frozen, as in threads mode, so a place reading one copies
nothing.  `std.reference`'s cells are still the process's, not the
place's.

### Measured

The same machine and build as below (`zig cc -O2`, each the fastest of
5 runs by `/usr/bin/time`, which resolves to 10 ms).  The places test
program's `cpu WORK N W` is a pool of W places over two channels (a
task's number out, its answer back) against std.thread's `parFor`
(`cpu` of the threads test) and against W processes of a worker
program, each given a W-th of the tasks, started at once and waited
for:

| work | W | threads, parFor | places, a pool over channels | W processes |
|---|---|---|---|---|
| queens 12 | 1 | 190 ms | 200 ms | 200 ms |
| | 2 | 90 ms | 100 ms | 100 ms |
| | 4 | 50 ms | 50 ms | 50 ms |
| | 8 | 20 ms | 20 ms | 30 ms |
| | 14 | 20 ms | 20 ms | 30 ms |
| primes below 5,000,000 | 1 | 900 ms | 910 ms | 930 ms |
| | 4 | 230 ms | 230 ms | 250 ms |
| | 14 | 90 ms | 90 ms | 100 ms |

One process per task (144 processes for queens 12) takes 110 ms where
a pool of 14 places takes 20 ms: a place is started as cheaply as a
thread, isolated as a process is.  On work that crosses little, places
cost nothing over threads: the tasks' numbers and answers are the
message, a few words each.

Made in one place and summed in another (`handOver R W`: R rounds,
each W lists of 100,000 made by W places and sent to the main one,
which sums them, a round at a time; the threads test's `handOver`, the
same lists made by a pool's threads and summed by the caller), the time
of the whole program and its peak memory:

| rounds, W | threads | places |
|---|---|---|
| 100, W=1 | 40 ms, 5 MB | 190 ms, 11 MB |
| 10, W=4 | 20 ms, 25 MB | 50 ms, 33 MB |
| 100, W=4 | 180 ms, 26 MB | 490 ms, 44 MB |
| 400, W=4 | 710 ms, 31 MB | 1960 ms, 46 MB |
| 100, W=8 | 410 ms, 83 MB | 890 ms, 64 MB |

With one thread, std.thread's pool runs in the caller, nothing crossing:
40 ms is making and summing 100 lists.  The place's 190 ms is the same
work and 100 lists crossing: some 1.5 ms a list of 100,000 -- 15 ns a
cell to write, read, make again and free, four times what making the
cell costs.  That is the price of isolation, paid per object that
crosses, and paid by the receiver, one place reading what many sent:
a program that hands over much should hand over little -- answers, not
inputs -- or run on shared threads.  Memory does not grow with the
rounds: a message is freed as it is read, the lists made in a place
are freed in it, and nothing goes on a page's remote list.

## Measured

On a 14-core Apple M-series machine, `test/node/threads.hf`'s
`cpu WORK N W` (threads mode, `zig cc -O2`) against the same work in a
program not built for threads, each the fastest of 7 runs, the machine
busy with other builds -- with pages each thread's own (#66), and in
parentheses before, when threads mode kept no pages:

| work | one thread | W=1 | W=2 | W=4 | W=8 | W=14 |
|---|---|---|---|---|---|---|
| primes below 5,000,000 | 938 ms | 905 ms | 465 ms | 233 ms | 122 ms | 104 ms (9.0x) |
| queens 12 | 197 ms | 206 ms (201) | 107 ms (106) | 56 ms (63) | 31 ms (35) | 24 ms (28) (8.2x) |
| fib 38 | 109 ms | 124 ms | 65 ms | 34 ms | 19 ms | 15 ms (7.3x) |

Primes and fib make almost nothing, and pages moved neither; nor the
one-thread rows, built as before.  Queens makes a list cell at each
step: with pages, each thread's cells near one another, it runs 10 to
15% faster at 4 threads and more, and some 2% slower at one.

Made in one thread and given back in another (`handOver R 4`: each
round, 4 lists of 100,000 made by the tasks' threads and summed and
given back by the caller), the peak memory of the whole program:

| rounds | 10 | 100 | 400 | 2000 |
|---|---|---|---|---|
| threads mode keeping no pages | 97 MB | 924 MB | 3.7 GB | |
| pages each thread's own | 37 MB | 33 MB | 41 MB | 51 MB |

Keeping no pages, each object the system's, a cell given back by the
caller went on the caller's free list and was made again by nothing
but the caller: the lists grew by the cells of 3 lists a round.  With
pages, the cells go back to their pages, which the next round's
threads adopt.

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
`failing`, `failingTask`, `cpu` and `handOver`) and the places test
beside it (places spawned, joined and nested; every kind of value over
a channel; a pool over channels; long strings moved and copied; a place
abandoning; a receive pending in green threads; `cpu`, `handOver`;
`stress`, eight places sending 2000 messages each on one channel,
every one checked as sent) and the test of green threads in a place
(a sleep, a receive, a command and a join, each pending in the place's
reactor while the others go on);
with `HEFF_SANITIZE=1` under
AddressSanitizer (`make sanitize`), and with `HEFF_TSAN=1` again under
ThreadSanitizer (`make tsan`), built by the system's clang.
