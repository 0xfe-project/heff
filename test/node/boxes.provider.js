// test/node/boxes.hf's world: an effect provider (the shape of one is
// said in lib/std/dom.provider.js).  A box is an object of its own, `{ n
// }`, held by the world (lib/std/handle.hf) and counted when the world
// lets it go -- whichever provider asked it to.
export const boxes = (world) => {
  let freed = 0;
  const handle = (box) => ({ $: "MkHandle", a: [world.hold(box, () => { freed++; })] });
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "NewBox": return k(a[1](handle({ n: a[0] })));
      case "Peek": return k(a[1](world.get(a[0].a[0]).n));
      case "Freed": return k(a[0](BigInt(freed)));
      default: throw new Error("heff: not an operation of boxes: " + (p && p.$));
    }
  };
};
