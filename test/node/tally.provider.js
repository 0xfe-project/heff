// test/node/tally.hf's world: an effect provider (the shape of one is
// said in lib/browser/dom.provider.js).  It acts on the boxes boxes.hf's
// provider hands out, by the world's numbers for them
// (lib/std/handle.hf): a box is that provider's `{ n }`.
export const tally = (world) => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Bump": world.get(a[0].a[0]).n += 1n; return k(a[1]);
    case "Again": return k(a[1]({ $: "MkHandle", a: [world.hold(world.get(a[0].a[0]))] }));
    case "LetGo": world.release(a[0].a[0]); return k(a[1]);
    default: throw new Error("heff: not an operation of tally: " + (p && p.$));
  }
};
