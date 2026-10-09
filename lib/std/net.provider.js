// The network's side of lib/std/net.hf: an effect provider (the shape
// of one is said in lib/std/dom.provider.js).
export const net = () => (p, k) => {
  if (p.$ === "Fetch") { fetch($toJs(p.a[0])).then((r) => r.arrayBuffer().then($fromBytes), (e) => $fromJs(String(e))).then((t) => k(p.a[1](t))); return; }
  throw new Error("heff: not an operation of net: " + (p && p.$));
};
