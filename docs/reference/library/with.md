---
title: Resources
---

```haskell
import co
import exception
import std.filesystem

firstLine : {e : HFunctor}
          -> {{disk : filesystem <: e}}
          -> {{fx : exception FileError <: e}}
          -> String
          -> Co e (Either FileError (Maybe String))
firstLine path = withFile path { access = Read } (\f -> readLine f)
```

A thing of the host that must be let go of, such as an open file, a
socket or a process, is held by a `with`: `withFile`, `withProcess`,
`Net.withConnection`, and so on. Each one:

1. acquires the thing;
2. runs the body with it, as a `With s a`;
3. releases it when the body ends;
4. gives back what the body gave.

## The thing cannot escape

The body is polymorphic in its region `s`, and its result's type is
fixed outside it. So a `With s a` cannot leave the body:
`withFile path o (\f -> pure f)` is a type error (E0311), as Haskell's
`ST` keeps its references in.

There is one `With` for every effect, so a thing found by one effect
can be given to another, inside the same `with`s: a `<canvas>` found
by `browser.dom` is drawn on by `gpu`.

## Failures

- A thing that cannot be acquired is thrown, over the `exception`
  effect, and the body does not run. So a `with` asks for
  `exception err` besides its own effect, and `try` can catch it.
- A thing that cannot be released is thrown after the body ends.
- If the body is left another way, such as by a throw caught outside
  it, the thing is let go of when the program drops its last reference
  to it.

`held` is the `with` of a thing always there, such as a standard stream
or the page's document. It throws nothing.

## Writing one

```haskell
withFile : {e : HFunctor} -> {a : Type} -> {{ev : filesystem <: e}}
         -> {{ex : exception FileError <: e}}
         -> String -> OpenOptions
         -> ({s : Type} -> With s OpenFile -> Co e a)
         -> Co e a
withFile path o body = scoped (opened path o) closed body

readLine : {e : HFunctor} -> {s : Type} -> {{ev : filesystem <: e}}
         -> With s OpenFile -> Co e (Either FileError (Maybe String))
readLine f = op (ev (ReadLine (inside f) (\r -> r))) val
```

`with`'s `scoped acquire release body` and `held acquire release body`
are the only ways to make a `With`.

- A module exports its `withX`, never its bare acquire.
- Its operations take the `With s a`, and pass the thing to the
  provider with `inside`.
- What lives as long as the program, such as a GPU device, is held by a
  `with` at the top of `main`.

How a provider lets the thing go is in
[Providers](../providers.md#scoped-operations).
