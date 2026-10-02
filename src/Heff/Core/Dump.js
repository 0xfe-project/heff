// a float, as heff's own `floatShow` prints one
export const showNumberImpl = (n) => { const s = String(n); return isNaN(s + ".0") ? s : s + ".0"; };
