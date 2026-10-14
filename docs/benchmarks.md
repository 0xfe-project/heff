# Benchmarks

`heff bench` runs benchmarks as Go's `go test -bench` does (on the files
given, else the project's `bench/`), and says them in Go's lines, so
that two runs are compared as Go's are (`benchstat old.txt new.txt`).

```sh
heff bench [--run REGEXP] [--time D] [--count N] [--json] [FILE...]
```

## A benchmark

A benchmark is a top-level definition of type `Bench` (bench's), found
by its type as a test is: one another of the file's benchmarks uses (in
a group) runs under that one, by the name its groups give it.  Its body
is given a count, `n`, and does the work measured `n` times:

```haskell
import bench as Bench

fib : Int -> Int
fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

fibs : Bench.Bench
fibs = Bench.group "fib"
  [ Bench.bench "10" (\n -> Bench.times n (\i -> fib (Bench.keep 10)))
  , Bench.bench "20" (\n -> Bench.times n (\i -> fib (Bench.keep 20)))
  ]
```

```
pkg: bench/fib.hf
BenchmarkFib/10	  665864	       160.6 ns/op	       0 B/op	       0 allocs/op
BenchmarkFib/20	    5528	     22466 ns/op	       0 B/op	       0 allocs/op
ok    bench/fib.hf
```

`times n f` is Go's `for i := 0; i < b.N; i++`: `f 0` to `f (n - 1)`,
each result kept.  What a body does before its `\n ->` is done once,
when the benchmark is made, not measured.  A body is in check's effect,
so it may check what it computed (`assert`, `failure`) and skip (`skip
WHY`), as a unit test does.

## Keeping the work

The compiler drops work whose result is never used, and computes once
what is made of what it knows: `times` keeps each result, and `keep`
keeps a value from the compiler -- it is the value, but no pass, nor the
C compiler after them, knows what it is, nor drops it.  So `fib 20` in a
benchmark is computed once (its argument known), `fib (keep 20)` each
time.  `keep` costs nothing (an empty instruction the C compiler must
keep, as Rust's `black_box`):

```
BenchmarkShow/kept     	 3443834	        33.10 ns/op	      39 B/op	       1 allocs/op
BenchmarkShow/dropped  	183560967	         0.5893 ns/op	       0 B/op	       0 allocs/op
BenchmarkShow/constant 	100000000	         1.048 ns/op	       0 B/op	       0 allocs/op
```

The first is `show (i * 7919)` by `times`; the second the same work, its
results dropped, not kept; the third `show 123456789` by `times`, its
string made once, as it is compiled (`show (keep 123456789)` makes it
each time).

## How a benchmark is run

`heff bench` builds a file's benchmarks into a program through C, at
`heff install`'s level (zig's `-O2`, not `heff run`'s `-O1`), in
`.heff/bench/F/` (F the file's path, its separators `_`), the runtime
built to count the objects the program makes and their bytes (which it
does not otherwise).  Each benchmark is run in a program of its own,
one at a time, as Go runs one: once with a count of 1, then again, each
count predicted from the run before (the bench time over what the last
took per op, a fifth more, but at most a hundred times the last count,
at least one more, at most a billion), until a run takes the bench time
-- a second, or what `--time` says: a duration (`500ms`, `1.5s`, `2m`;
`ns`, `us`, `ms`, `s`, `m`, `h`), or a count (`100x`), run once of that
count.  The last run is said: its count, and per op its nanoseconds (by
the monotonic clock), the bytes it made, and how many objects.
`--count N` runs each benchmark N times, a line each.

`--run REGEXP` selects benchmarks by their names, a level each between
`/`s, as `heff test --run` selects tests
([tests.md](tests.md#selecting-tests)).

## The report

A line each benchmark, Go's: `Benchmark` and its names (its groups',
then its own) between `/`s, each's spaces `_`, the first a capital,
padded to the longest of the file's; its count; and per op its
nanoseconds (to about five figures), bytes and objects, a tab between
each.  Before a file's, `pkg: FILE`; after them, `ok` or `FAIL` and the
file.  A benchmark that fails says `--- FAIL: NAME` and why under it --
a check of its body that failed, or how its program ended (`it
abandoned`) -- and one skipped `--- SKIP: NAME` and why.  A file that
does not check, or does not build, says `FAIL` and its first error.
`--json` says each run as a line of JSON instead: its file, its name as
Go's and its names, its count, nanoseconds, objects and bytes, and those
per op (or why it failed, or was skipped).

`heff bench` exits 1 if a benchmark failed or a file did not check, and
where no benchmark was run (`no benchmarks match 'PATTERN'`).

`make bench` is another thing: whole programs of `bench/` (their
`main`s) timed, and the compiler checking and compiling itself.
