// jstext.hf's world: an effect provider.  A `JSString` field is a JavaScript
// string, given and taken as it is: no helper between.
export const jstext = (world) => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Units": return k(a[1](BigInt(a[0].length)));
    case "Echo": return k(a[1](a[0]));
    default: throw new Error("heff: not an operation of jstext: " + (p && p.$));
  }
};
