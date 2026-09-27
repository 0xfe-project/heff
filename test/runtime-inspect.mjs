// Print a compiled PureScript value the way the Hippogriff interpreter prints
// values: tags as `'tag args`, records as `[l := v, ...]`, functions as `#fun`.
export function inspect(v, nested = false) {
  if (typeof v === "function") return "#fun";
  if (typeof v === "bigint") return v.toString();
  if (typeof v === "number" || typeof v === "string") return typeof v === "string" ? JSON.stringify(v) : String(v);
  if (v && typeof v === "object") {
    const cname = v.constructor && v.constructor.name;
    if (cname && cname !== "Object") {
      const tag = cname.split("$prime").pop();
      const args = Object.keys(v).filter((k) => /^value\d+$/.test(k)).sort((a, b) => a.slice(5) - b.slice(5)).map((k) => inspect(v[k], true));
      return "'" + tag + args.map((a) => " " + a).join("");
    }
    return "[" + Object.keys(v).map((k) => `${k} := ${inspect(v[k], true)}`).join(", ") + "]";
  }
  return String(v);
}

// the interpreter's `list` layout, flattened
export const flat = (s) => s.replace(/\s*\n\s*/g, " ").replace(/\[ /g, "[").replace(/ \]/g, "]").replace(/ ,/g, ",");
