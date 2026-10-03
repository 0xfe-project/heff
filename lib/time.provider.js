// Time's side of lib/time.hf: an effect provider (the shape of one is
// said in lib/dom.provider.js).
export const time = () => (p, k) => {
  if (p.$ === "Sleep") { setTimeout(() => k(p.a[1]), Number(p.a[0])); return; }
  throw new Error("heff: not an operation of time: " + (p && p.$));
};
