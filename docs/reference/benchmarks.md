---
title: Benchmarks
---

```haskell
import bench as Bench

fib : Int -> Int
fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

fibs : Bench.Bench
fibs =
  Bench.group
    "fib"
    [ Bench.bench "10" (\n -> Bench.times n (\i -> fib (Bench.keep 10)))
    , Bench.bench "20" (\n -> Bench.times n (\i -> fib (Bench.keep 20)))
    ]
```

```
$ heff bench
pkg: bench/fib.hf
BenchmarkFib/10	  665864	       160.6 ns/op	       0 B/op	       0 allocs/op
BenchmarkFib/20	    5528	     22466 ns/op	       0 B/op	       0 allocs/op
ok    bench/fib.hf
```

`heff bench` runs benchmarks as Go's `go test -bench` does, and prints
Go's lines, so `benchstat old.txt new.txt` can compare two runs. Given
no files, it runs the project's `bench/`.

## A benchmark

A benchmark is a top-level definition of type `Bench.Bench`. Its body
gets a count, `n`, and does the work `n` times.

- `Bench.times n f` runs `f 0` to `f (n - 1)` and keeps each result, as
  Go's `for i := 0; i < b.N; i++`.
- Work before the body's `\n ->` runs once, unmeasured.
- `Bench.group` puts benchmarks under a name.
- The body can check what it computed, with `assert` and `failure`, and
  skip, with `skip WHY`, as a unit test can.

## Keeping the work

The compiler drops work whose result is unused, and computes ahead what
it can. So:

- `times` keeps each result, so the work is not dropped;
- `Bench.keep x` is `x`, but hidden from the compiler, so the work is
  not computed ahead. It costs nothing, as Rust's `black_box`.

`fib 20` in a benchmark is computed once, since its argument is known;
`fib (Bench.keep 20)` is computed each time.

## Running

Each benchmark is built through C with `zig -O2`, and runs in its own
program. It runs once, then again with larger counts, until a run takes
the bench time. The last run is printed: its count, and per operation,
its nanoseconds, bytes allocated and objects allocated.

| Flag | Meaning |
|---|---|
| `--time D` | the bench time: a duration (`500ms`, `1.5s`, `2m`), or a count (`100x`); default `1s` |
| `--count N` | run each benchmark N times, a line each |
| `--run REGEXP` | only the benchmarks whose names match, as `heff test --run` |
| `--json` | a line of JSON for each run instead |

A benchmark that fails prints `--- FAIL: NAME` and why; one skipped,
`--- SKIP: NAME`. `heff bench` exits 1 if a benchmark fails, a file does
not check, or no benchmark ran.
