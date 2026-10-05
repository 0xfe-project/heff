// The process's side of lib/process.hf: an effect provider (the shape
// of one is said in lib/dom.provider.js).
// (`Exit` does not go on: the process ends when what is pending, output
// being written, is done)
export const $host = "node";
export const process = () => {
  const nothing = { $: "Nothing", a: [] };
  const just = (x) => ({ $: "Just", a: [x] });
  const list = (xs) => xs.reduceRight((rest, x) => ({ $: "Cons", a: [x, rest] }), { $: "Nil", a: [] });
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Args": return k(a[0](list(globalThis.process.argv.slice(2))));
      case "GetEnv": { const v = globalThis.process.env[a[0]]; return k(a[1](v === undefined ? nothing : just(v))); }
      case "ProgramPath": return k(a[0](decodeURIComponent(new URL(import.meta.url).pathname)));
      case "Exit": globalThis.process.exitCode = Number(a[0]); return;
      default: throw new Error("heff: not an operation of process: " + (p && p.$));
    }
  };
};
