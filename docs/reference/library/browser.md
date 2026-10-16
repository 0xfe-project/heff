---
title: Browser and GPU
---

These are a page's effects, in JavaScript.

## browser.dom

```haskell
import co
import browser.dom

greeting : String -> String
greeting name = if name == "" then "Hello, whoever you are." else "Hello, " ^ name ^ "!"

-- | the paragraph greets whatever is typed into the input
greeter : {e : HFunctor} -> {{page : dom <: e}} -> Co e Unit
greeter = do
  with withDocument as doc do
    out <- xpath doc "//p[@id='greeting']"
    name <- select doc "#name"
    found out (\o -> do
      text o (greeting "")
      found name (\n -> on n "input" (\v -> text o (greeting v))))
```

`withDocument body` gives the body the page's document, at the top of
the program. Then:

| Function | What it does |
|---|---|
| `select doc "#out"` | `Maybe` the first element a CSS selector finds |
| `xpath doc "//p"` | `Maybe` the first element an XPath finds |
| `create doc "li"` | a new element |
| `textIn doc parent "p" "hi"` | a new element with text, appended to `parent` |
| `text`, `attr`, `value` | set an element's text, an attribute; read an input's value |
| `append`, `clear` | add a child; remove all children |
| `on el "input" handler` | run `handler` with the value at each event |
| `same a b` | whether two elements are the same one |
| `found m body` | run `body` with the element, if there is one |
| `fetchText url` | fetch a URL's text |

An `Element` is the page's own, passed by value, even to another
effect's operations. The page owns its elements, so there is nothing to
let go of.

## browser.http

`browser.http` is the browser's `fetch`, with `Bytes` bodies, in the same
shapes as `std.http`: `send`, `get` and `text`. `browser.remote` is an
API's client over it; see [Network](network.md#an-api-between-programs).

## browser.js

`browser.js` has `JSString`, JavaScript's UTF-16 text: see
[Providers](../providers.md#jsstring).

## gpu

`gpu`, imported as `Gpu`, is a GPU. Its provider is WebGPU's, so it runs
in pages, and kernels are WGSL. A browser without WebGPU throws a
`GpuError` at `withDevice`.

| Function | What it does |
|---|---|
| `withDevice body` | the device, held for the body; at the top of the program |
| `withBuffer d n body` | a buffer of `n` bytes on the device, held for the body |
| `writeBuffer`, `readBuffer` | write and read a buffer |
| `load` | a kernel's pipeline, from its code and entry point |
| `dispatch1` … `dispatch3` | run a kernel over workgroups, its buffers bound in order from `@binding(0)` |

Each answer is an `Either GpuError a`, given when the GPU is done.

`gpu.canvas`'s `present d canvas px w h` shows a buffer as RGBA pixels on
a `<canvas>` that `browser.dom` found.

### Kernels in heff

A kernel is written in heff with `gpu.kernel`'s combinators, not as WGSL
text:

- an expression is an `Exp a`, `a` a `Float32`, `Int32`, `UInt32` or
  `Bool`, built with the prelude's operators and `lit`, `lessThan`,
  `both`, `either`, `negation`, `convert`;
- a buffer is an `Input a` or `Output a`, read with `load`, written with
  `store`;
- a body is a `Co stmt Unit`, written with `do`, using `define`,
  `store`, `when`, `ifElse`, `forRange` and `globalId`;
- `kernel1` to `kernel3` make a kernel of a name, a workgroup size and a
  body.

A kernel has no functions, recursion or allocation: heff functions in a
body run as the kernel is built, and leave straight code. `gpu.wgsl`'s
`source3 k` prints the WGSL. `gpu.run`'s `run3 d k n x y z` runs a kernel
over `n` invocations, with typed buffers from `withTyped`.

`examples/pages/` has `vecadd`, adding two vectors on the GPU, and
`pixels`, drawing a gradient.
