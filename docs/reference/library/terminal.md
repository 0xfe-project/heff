---
title: Terminal
---

```haskell
import co
import std.terminal as Terminal
import std.terminal.widget as Widget

-- | a number, up and down by the arrows, until `q`
counter : Widget.App Int Int
counter =
  { start = 0
  , step = \k n -> case k of
      ArrowUp -> Continue (n + 1)
      ArrowDown -> Continue (n - 1)
      Char 'q' -> Done n
      _ -> Continue n
  , view = \size n ->
      { rows = [[Widget.bold (show n)], Widget.status size.columns "↑↓  q quits"]
      , cursor = Nothing
      }
  }

-- | >>> Widget.shownAfter counter { columns = 12, rows = 2 } [ArrowUp, ArrowUp]
-- | ["2", "↑↓  q quits "]
counted : {e : HFunctor} -> {{con : Terminal.console <: e}} -> Co e (Maybe Int)
counted = Terminal.interact counter
```

## Apps

`std.terminal.widget`, imported as `Widget`, is pure: an app and what it
draws are values.

An `App s r` has:

- `start`, the state at the start;
- `step`, which takes a key and the state, and gives `Continue` a new
  state or `Done` a result;
- `view`, which draws a state on a terminal of a given `Size`.

`Terminal.interact app` runs it on the terminal: it draws, reads a key,
steps, and draws again, until the app is `Done`. It gives `Just` the
result, or `Nothing` if there is no terminal or the input ended.

To test an app without a terminal, `Widget.replay app keys` gives where
it ends, and `Widget.shownAfter app size keys` the rows it shows.

## Keys

A `Key` is `Char` a rune, `Enter`, `Escape`, `Tab`, `BackTab`,
`Backspace`, `Delete`, `ArrowUp`, `ArrowDown`, `ArrowLeft`,
`ArrowRight`, `Home`, `End`, `PageUp`, `PageDown`, `Control` a letter,
or an unknown `Sequence`. Write them without `Widget.`.

## Drawing

- A `Frame` is `rows`, each a list of `Span`s, and where the `cursor`
  is, if shown.
- A `Span` is text with a `Style`: `Plain`, `Bold`, `Dim`, `Inverse` or
  `Underline`. `Widget.plain`, `bold`, `dim` and `inverse` make one.
- `Widget.status width text` is a status line.
- `width`, `cut`, `pad` and `wrap` measure and lay out text by columns;
  an East Asian wide rune is two.

## Widgets

Each widget is a value, and a function of a key:

| Widget | Key function | What it is |
|---|---|---|
| `Menu` | `menuKey`, `menuRows` | an item picked from a list |
| `Field` | `fieldKey` | text typed, with a shell's editing keys |
| `Choice` | `choiceKey` | one of some values |

`std.cli.interactive` is an app of a program's `std.cli` commands:
`Interactive.app "prog" (Interactive.entries commands)`. `heff` with no
words runs it.

## The terminal itself

`std.terminal`'s effect, `Terminal.console`, has `interactive` (whether
the input and output are a terminal), `begin` and `finish` (raw mode and
the alternate screen), `measure`, `readInput` and `draw`. The terminal
is put back however the program ends.

`std.progress` draws what a program is doing below its output, as
`heff build` does.
