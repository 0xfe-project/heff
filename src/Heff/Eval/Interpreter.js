export const tryAbandon = (marker) => (thunk) => (ok) => (abandoned) => {
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
