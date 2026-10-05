// The terminal's side of src/heff/report.hf: an effect provider (the
// shape of one is said in lib/dom.provider.js).  What differs is
// jsdiff's, by words (with the spaces between them), long stretches
// alike cut to their ends.
import { diffWordsWithSpace as $diffWordsWithSpace } from "diff";
export const $host = "node";
export const report = () => {
  const context = 20;
  const red = (s) => `\x1b[31m${s}\x1b[0m`;
  const green = (s) => `\x1b[32m${s}\x1b[0m`;
  // a stretch alike: kept whole if short, else its ends (only the end
  // after the start, only the start before the end)
  const cut = (s, first, last) => {
    if (s.length <= 2 * context + 1) return s;
    if (first) return "…" + s.slice(s.length - context);
    if (last) return s.slice(0, context) + "…";
    return s.slice(0, context) + "…" + s.slice(s.length - context);
  };
  const mismatch = (color, expected, got) => {
    const parts = $diffWordsWithSpace(expected, got);
    let e = "", g = "", d = "";
    parts.forEach((p, i) => {
      if (p.added) { g += color ? green(p.value) : p.value; d += (color ? green : (s) => s)(`{+${p.value}+}`); }
      else if (p.removed) { e += color ? red(p.value) : p.value; d += (color ? red : (s) => s)(`[-${p.value}-]`); }
      else { const s = cut(p.value, i === 0, i === parts.length - 1); e += s; g += s; d += s; }
    });
    return { e, g, d };
  };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Differ": { const m = mismatch(a[0].$ === "True", a[1], a[2]); return k(a[3](m.e)(m.g)(m.d)); }
      // (in the thread a Node.js program runs in, stdout is not the
      // terminal: the main thread says whether it is, HEFF_TTY)
      case "UseColor": return k(a[0]({ $: (Boolean(process.stdout.isTTY) || process.env.HEFF_TTY === "1") && !process.env.NO_COLOR ? "True" : "False", a: [] }));
      default: throw new Error("heff: not an operation of report: " + (p && p.$));
    }
  };
};
