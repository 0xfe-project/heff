// Time's side of std.time: an effect provider.
export const time = () => (p, k) => {
  if (p.$ === "Sleep") { setTimeout(() => k(p.a[1]), Number(p.a[0])); return; }
  throw new Error("heff: not an operation of time: " + (p && p.$));
};
