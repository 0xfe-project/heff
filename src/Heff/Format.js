// The formatter written in heff: `bin/heff.mjs` compiles `fmt/layout.hf`
// (with heff itself) and puts its `formatUnchecked` here; `format` then
// lays out through it.  Absent -- a build that has not run -- the
// reference layout in Heff.Format stands in; the two are byte for byte
// the same (test/fmt-port.mjs holds them to it).
export const heffLayoutImpl = (src) => {
  const f = globalThis.$heffFormatUnchecked;
  if (!f) return { available: false, ok: false, value: "" };
  const r = f(src);
  return r.$ === "Left"
    ? { available: true, ok: false, value: r.a[0] }
    : { available: true, ok: true, value: r.a[0] };
};
