---
title: The library
---

The library, `lib/`, comes with the compiler and has its version.
`HEFF_LIB` says where it is; otherwise it is the `lib/` beside heff.

It has three parts:

| Part | For | Modules |
|---|---|---|
| `lib/` | every backend; pure | `co`, `state`, `exception`, `parallel`, `green`, `array`, `bytes`, `bits`, `rune`, numbers, `wire`, `json`, `with`, `check`, `generator`, `bench`, `manifest`, `ffi` |
| `lib/std/` | C programs | `std.io`, `std.filesystem`, `std.process`, `std.net`, `std.http`, `std.http.server`, `std.thread`, `std.place`, `std.reactor`, `std.terminal`, `std.progress`, `std.remote` |
| `lib/browser/` | pages | `browser.dom`, `browser.http`, `browser.remote`, `browser.js` |

Some of `lib/std/` is pure, or has a JavaScript side too, and works on
every backend: `std.reference`, `std.time`, `std.path`, `std.map`,
`std.set`, `std.hashmap`, `std.cli`, `std.cli.interactive`,
`std.terminal.widget`, `std.doc` and `std.math`.

A program can only import what its backend has: a C program importing
`lib/browser/` is E0351, and a page importing a C-only effect is E0352.

Only the library declares primitives, such as
`primitive intAdd : Int -> Int -> Int`.

An effect of the outside world, such as `std.io`'s `stdio`, is carried
out by a provider: JavaScript and C code beside its module. See
[Providers](../providers.md).

## Modules by topic

| Topic | Modules | Page |
|---|---|---|
| effects | `co`, `state`, `exception`, `parallel` | [Effects](../language/effects.md) |
| collections | `array`, `std.map`, `std.set`, `std.hashmap` | [Collections](collections.md) |
| numbers and text | `int8`…`uint64`, `float32`, `int`, `rune`, `bytes`, `bits`, `std.math` | [Numbers](../language/numbers.md), [Runes and strings](../language/text.md), [Binaries](../language/binaries.md) |
| resources | `resource` | [Resources](resource.md) |
| the system | `std.io`, `std.filesystem`, `std.process` | [Files and processes](system.md) |
| the network | `std.net`, `std.http`, `std.http.server` | [Network](network.md) |
| concurrency (unstable) | `green`, `std.reactor`, `std.thread`, `std.place` | [Concurrency](concurrency.md) |
| the terminal | `std.terminal`, `std.terminal.widget`, `std.cli.interactive` | [Terminal](terminal.md) |
| data formats | `wire`, `json` | [Wire and JSON](formats.md) |
| the browser | `browser.dom`, `browser.http`, `gpu` | [Browser and GPU](browser.md) |
| tests | `check`, `generator`, `bench` | [Tests](../tests.md), [Benchmarks](../benchmarks.md) |
| projects | `manifest` | [Projects](../projects.md) |
| providers | `ffi` | [Providers](../providers.md) |

## Examples

Each directory of `examples/` is a project:

- `examples/collatz/`: a first C program, with an effect of its own,
  `do` statements, unit tests and a property.
- `examples/todos/`: a terminal app; a cursor effect handled on a
  zipper.
- `examples/pages/`: four web pages: a counter, a clock, a greeting,
  and a sum of vectors on the GPU. `heff dev` serves each at `/NAME/`.
- `examples/notes/`: a page and its server, in one project.
- `examples/testbridge/`: a Linux program that makes network devices by
  rtnetlink, with a provider of its own.

`heff test test/apps.hf` tests them all.
