// Time's side of std.time: an effect provider.  The clocks are read in
// nanoseconds, as BigInts: the time of day by `Date.now`, in
// milliseconds; the one that only goes forward by `performance.now`, in
// fractions of them.
export const time = () => (p, k) => {
  if (p.$ === "Sleep") { setTimeout(() => k(p.a[1]), Number(p.a[0])); return; }
  if (p.$ === "Now") return k(p.a[0](BigInt(Date.now()) * 1000000n));
  if (p.$ === "Monotonic") return k(p.a[0](BigInt(Math.floor(performance.now() * 1e6))));
  throw new Error("heff: not an operation of time: " + (p && p.$));
};
