// What differs between an expectation and a value, with jsdiff: by words
// (with the spaces between them), long stretches alike cut to their ends.
import { diffWordsWithSpace } from "diff";

const context = 20;
const red = (s) => `\x1b[31m${s}\x1b[0m`;
const green = (s) => `\x1b[32m${s}\x1b[0m`;

// a stretch alike: kept whole if short, else its ends (only the end after
// the start, only the start before the end)
const cut = (s, first, last) => {
  if (s.length <= 2 * context + 1) return s;
  if (first) return "…" + s.slice(s.length - context);
  if (last) return s.slice(0, context) + "…";
  return s.slice(0, context) + "…" + s.slice(s.length - context);
};

export const mismatchImpl = (color) => (expected) => (got) => {
  const parts = diffWordsWithSpace(expected, got);
  let e = "", g = "", d = "";
  parts.forEach((p, i) => {
    if (p.added) { g += color ? green(p.value) : p.value; d += (color ? green : (s) => s)(`{+${p.value}+}`); }
    else if (p.removed) { e += color ? red(p.value) : p.value; d += (color ? red : (s) => s)(`[-${p.value}-]`); }
    else { const s = cut(p.value, i === 0, i === parts.length - 1); e += s; g += s; d += s; }
  });
  return { expected: e, got: g, diff: d };
};

export const useColorImpl = () => Boolean(process.stdout.isTTY) && !process.env.NO_COLOR;
