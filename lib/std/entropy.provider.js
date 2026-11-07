// The system's randomness, std.entropy's side: an effect provider.  The
// bytes are `crypto.getRandomValues`'s, 65536 at most a call, so asked
// for so many at a time.
export const entropy = () => (p, k) => {
  if (p.$ === "Gather") {
    const b = new Uint8Array(Number(p.a[0]));
    for (let i = 0; i < b.length; i += 65536) crypto.getRandomValues(b.subarray(i, i + 65536));
    return k(p.a[1](b));
  }
  throw new Error("heff: not an operation of entropy: " + (p && p.$));
};
