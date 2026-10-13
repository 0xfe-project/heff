// Load one compiled JavaScript module and print the named values as
// `heff dump-eval` shows them, as JSON: { name: { value } | { threw } }.
import { pathToFileURL } from "node:url";
const [file, ...names] = process.argv.slice(2);
const g = (await import(pathToFileURL(file))).default;
// (a list, `Cons a (Cons b Nil)`: its items, as `[a, b]` writes them;
// not one, null)
const isTag = (v, t, n) => v && typeof v === "object" && v.$ === t && Array.isArray(v.a) && v.a.length === n;
const items = (v) => {
  const xs = [];
  for (; isTag(v, "Cons", 2); v = v.a[1]) xs.push(v.a[0]);
  return isTag(v, "Nil", 0) ? xs : null;
};
// (the rune at `i` of a string, bytes each a code unit, and how many
// bytes it takes: what is not UTF-8 U+FFFD, of the longest start of a
// rune's bytes there, or of a byte that starts none -- as TextDecoder
// and lib/c/runtime.c's heff_u8_decode read it)
const runeAt = (s, i) => {
  const b = s.charCodeAt(i);
  if (b < 0x80) return [b, 1];
  let m, c, lo = 0x80, hi = 0xbf;
  if (b >= 0xc2 && b <= 0xdf) { m = 1; c = b & 0x1f; }
  else if (b >= 0xe0 && b <= 0xef) { m = 2; c = b & 0x0f; if (b === 0xe0) lo = 0xa0; else if (b === 0xed) hi = 0x9f; }
  else if (b >= 0xf0 && b <= 0xf4) { m = 3; c = b & 0x07; if (b === 0xf0) lo = 0x90; else if (b === 0xf4) hi = 0x8f; }
  else return [0xfffd, 1];
  for (let k = 1; k <= m; k++) {
    const d = i + k < s.length ? s.charCodeAt(i + k) : -1;
    if (d < lo || d > hi) return [0xfffd, k];
    lo = 0x80; hi = 0xbf; c = c * 64 + (d & 0x3f);
  }
  return [c, m + 1];
};
// (a string, as JSON writes its text; each byte of what is not UTF-8
// in it `\xff`, as a string literal writes it)
const showString = (s) => {
  let out = "", text = "";
  for (let i = 0; i < s.length;) {
    const [c, w] = runeAt(s, i);
    if (c !== 0xfffd || s.slice(i, i + w) === "\xef\xbf\xbd") text += String.fromCodePoint(c);
    else {
      out += JSON.stringify(text).slice(1, -1);
      text = "";
      for (let k = 0; k < w; k++) out += "\\x" + s.charCodeAt(i + k).toString(16).padStart(2, "0");
    }
    i += w;
  }
  return "\"" + out + JSON.stringify(text).slice(1, -1) + "\"";
};
// (bytes, a `Uint8Array`, as the prelude's `showBytes` writes them, a
// binary: `b[...]`, each run of printable ASCII a string, `"` and `\`
// escaped, any other byte its number)
const bytePieces = (b) => {
  const pieces = [];
  let run = "";
  for (const c of b) {
    if (c >= 0x20 && c < 0x7f) run += c === 0x22 ? "\\\"" : c === 0x5c ? "\\\\" : String.fromCharCode(c);
    else { if (run) pieces.push("\"" + run + "\""); run = ""; pieces.push(String(c)); }
  }
  if (run) pieces.push("\"" + run + "\"");
  return pieces;
};
const showBytes = (b) => "b[" + bytePieces(b).join(", ") + "]";
// (bits, a `Uint8Array` whose `bits` is their length, as the prelude's
// `showBits` writes them: their whole bytes as bytes are, then the bits
// left, `v::k`, or, none left, `b[]::bits`)
const showBits = (b) => {
  const k = b.bits % 8, whole = (b.bits - k) / 8;
  const pieces = bytePieces(b.subarray(0, whole));
  pieces.push(k ? `${b[whole] >> (8 - k)}::${k}` : "b[]::bits");
  return "b[" + pieces.join(", ") + "]";
};
// (a list as `[a, b]` writes it; a tag with arguments is parenthesized
// where it is an argument)
export const show = (v, arg = false) => {
  if (typeof v === "function") return v.$erased ? "#erased" : "#fun";
  // (a negative number, an argument: `Just (-7)`)
  if ((typeof v === "bigint" || typeof v === "number") && arg && String(v).startsWith("-")) return "(" + show(v) + ")";
  if (typeof v === "bigint") return v.toString();
  // (a float as a literal writes it: `2.0`, not `2`)
  if (typeof v === "number") { const s = String(v); return isNaN(s + ".0") ? s : s + ".0"; }
  if (typeof v === "string") return showString(v);
  if (v instanceof Uint8Array) return v.bits === undefined ? showBytes(v) : showBits(v);
  if (Array.isArray(v)) return "[" + v.map((x) => show(x)).join(", ") + "]";
  if (v && typeof v === "object") {
    if ("$" in v && Array.isArray(v.a)) {
      const xs = items(v);
      if (xs) return "[" + xs.map((x) => show(x)).join(", ") + "]";
      const tag = v.$;
      if (!v.a.length) return tag;
      const s = tag + v.a.map((x) => " " + show(x, true)).join("");
      return arg ? `(${s})` : s;
    }
    return "{ " + Object.keys(v).map((k) => `${k} = ${show(v[k])}`).join(", ") + " }";
  }
  return String(v);
};
const out = {};
for (const n of names) {
  try { out[n] = { value: show(g[n]) }; } catch (e) { out[n] = { threw: String(e && e.message || e) }; }
}
process.stdout.write(JSON.stringify(out));
