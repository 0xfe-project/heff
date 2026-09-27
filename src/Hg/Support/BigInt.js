// Integers of any size: JavaScript's BigInt.
export const fromInt = (i) => BigInt(i);
export const fromDigits = (s) => BigInt(s);
export const fromStringImpl = (nothing) => (just) => (s) => {
  if (!/^-?[0-9]+$/.test(s)) return nothing;
  return just(BigInt(s));
};
export const toString = (a) => a.toString();
// clamped to 32 bits (for string offsets and the like)
export const toIntClamped = (a) => (a > 2147483647n ? 2147483647 : a < -2147483648n ? -2147483648 : Number(a));
export const add = (a) => (b) => a + b;
export const sub = (a) => (b) => a - b;
export const mul = (a) => (b) => a * b;
export const neg = (a) => -a;
// floor division, as Haskell's div and mod: the remainder has the sign of
// the divisor; by zero, nothing
export const divFloorImpl = (nothing) => (just) => (a) => (b) => {
  if (b === 0n) return nothing;
  const q = a / b, r = a % b;
  return just(r !== 0n && (r < 0n) !== (b < 0n) ? q - 1n : q);
};
export const modFloorImpl = (nothing) => (just) => (a) => (b) => {
  if (b === 0n) return nothing;
  const r = a % b;
  return just(r !== 0n && (r < 0n) !== (b < 0n) ? r + b : r);
};
export const eqImpl = (a) => (b) => a === b;
export const compareImpl = (lt) => (eq) => (gt) => (a) => (b) => (a < b ? lt : a > b ? gt : eq);
