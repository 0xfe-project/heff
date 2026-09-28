export const tryAbandon = (marker) => (thunk) => (ok) => (abandoned) => {
  depth = 0;
  try {
    return ok(thunk());
  } catch (e) {
    if (e instanceof Error && e.message === marker) return abandoned;
    throw e;
  }
};

// Evaluate, telling apart a value, an `abandon`, and running out of stack
// (a definition that recurses without end).
export const tryEval = (marker) => (thunk) => (ok) => (abandoned) => (diverged) => {
  depth = 0;
  try {
    return ok(thunk());
  } catch (e) {
    if (e instanceof Error && e.message === marker) return abandoned;
    if (e instanceof RangeError) return diverged;
    throw e;
  }
};

export const bigToNumber = (b) => Number(b);
export const floorToBig = (x) => BigInt(Math.floor(x));

// How deep evaluation is.  A computation that goes deeper than `deepest`
// is said to diverge (as by running out of stack, a RangeError): deeper
// than a program that ends needs (a list of a hundred thousand takes some
// half a million), and reached long before a stack as deep as heff's
// (bin/heff.mjs) runs out, which would take a while.  (`HEFF_DEPTH`: another
// depth.)  Nothing inside evaluation catches, so what is thrown goes to
// `tryEval` or `tryAbandon`, which start again from 0.
let depth = 0;
const deepest = Number(globalThis.process?.env?.HEFF_DEPTH) || 1000000;
export const deeper = (f) => {
  if (++depth > deepest) throw new RangeError("heff: evaluation too deep");
  const v = f();
  depth--;
  return v;
};
