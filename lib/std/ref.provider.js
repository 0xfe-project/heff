// The world's cells, lib/std/ref.hf's: an effect provider (the shape of
// one is said in lib/std/dom.provider.js).
export const ref = () => {
  const cells = [];
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "NewRef": cells.push(a[0]); return k(a[1]({ $: "MkRef", a: [BigInt(cells.length - 1)] }));
      case "ReadRef": return k(a[1](cells[Number(a[0].a[0])]));
      case "WriteRef": cells[Number(a[0].a[0])] = a[1]; return k(a[2]);
      default: throw new Error("heff: not an operation of ref: " + (p && p.$));
    }
  };
};
