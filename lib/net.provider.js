// The network's side of lib/net.hf: an effect provider (the shape of
// one is said in lib/dom.provider.js).
export const net = () => (p, k) => {
  if (p.$ === "Fetch") { fetch(p.a[0]).then((r) => r.text(), (e) => String(e)).then((t) => k(p.a[1](t))); return; }
  throw new Error("heff: not an operation of net: " + (p && p.$));
};
