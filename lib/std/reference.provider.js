// The world's cells, lib/std/reference.hf's: an effect provider (the shape of
// one is said in lib/browser/dom.provider.js).
export const reference = () => {
  const cells = [];
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "NewReference": cells.push(a[0]); return k(a[1]({ $: "MkReference", a: [BigInt(cells.length - 1)] }));
      case "ReadReference": return k(a[1](cells[Number(a[0].a[0])]));
      case "WriteReference": cells[Number(a[0].a[0])] = a[1]; return k(a[2]);
      default: throw new Error("heff: not an operation of reference: " + (p && p.$));
    }
  };
};
