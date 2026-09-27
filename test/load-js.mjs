// Load one compiled JavaScript module and print the named values, as the
// interpreter prints them, as JSON: { name: { value } | { threw } }.
import { pathToFileURL } from "node:url";
const [file, ...names] = process.argv.slice(2);
const g = (await import(pathToFileURL(file))).default;
export const show = (v) => {
  if (typeof v === "function") return v.$erased ? "#erased" : "#fun";
  if (typeof v === "bigint") return v.toString();
  if (typeof v === "number") return String(v);
  if (typeof v === "string") return JSON.stringify(v);
  if (v && typeof v === "object") {
    if ("$" in v && Array.isArray(v.a)) return "'" + v.$ + v.a.map((x) => " " + show(x)).join("");
    return "[" + Object.keys(v).map((k) => `${k} := ${show(v[k])}`).join(", ") + "]";
  }
  return String(v);
};
const out = {};
for (const n of names) {
  try { out[n] = { value: show(g[n]) }; } catch (e) { out[n] = { threw: String(e && e.message || e) }; }
}
process.stdout.write(JSON.stringify(out));
