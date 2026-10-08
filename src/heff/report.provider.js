// The terminal's side of src/heff/report.hf: an effect provider (the
// shape of one is said in lib/std/dom.provider.js).
export const $host = "node";
// (colour on `stream`: it is a terminal, or `HEFF_TTY=1` says it is, and
// `NO_COLOR` is not set)
const color = (stream) =>
  ({ $: (Boolean(stream.isTTY) || process.env.HEFF_TTY === "1") && !process.env.NO_COLOR ? "True" : "False", a: [] });
export const report = () => (p, k) => {
  const a = p.a;
  switch (p.$) {
    // (in the thread a Node.js program runs in, stdout is not the
    // terminal: the main thread says whether it is, HEFF_TTY)
    case "UseColor": return k(a[0](color(process.stdout)));
    case "ErrColor": return k(a[0](color(process.stderr)));
    default: throw new Error("heff: not an operation of report: " + (p && p.$));
  }
};
