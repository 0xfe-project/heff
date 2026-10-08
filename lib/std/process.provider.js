// The process's side of lib/std/process.hf: an effect provider (the
// shape of one is said in lib/std/dom.provider.js).
import { parentPort as $port } from "node:worker_threads";
import { spawnSync as $spawnSync } from "node:child_process";
import { fileURLToPath as $processFileURLToPath } from "node:url";
// (`Exit` does not go on: the process ends when what is pending, output
// being written, is done; nor does `Exec`: the first thread runs the
// command when the program's thread has ended)
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
      case "ProgramPath": return k(a[0]($processFileURLToPath(import.meta.url)));
      case "WorkingDir": return k(a[0](globalThis.process.cwd()));
      case "Platform": return k(a[0](globalThis.process.platform));
      case "Exit": globalThis.$heffStatus?.end(); globalThis.process.exitCode = Number(a[0]); return;
      case "Run": {
        const as = [];
        for (let l = a[1]; l.$ === "Cons"; l = l.a[1]) as.push(l.a[0]);
        const r = $spawnSync(a[0], as, { encoding: "utf8" });
        const status = r.error || r.status === null ? -1 : r.status;
        return k(a[2]({ status: BigInt(status), out: r.stdout || "", err: r.stderr || "" }));
      }
      case "Exec": {
        globalThis.$heffStatus?.end();
        const as = [];
        for (let l = a[1]; l.$ === "Cons"; l = l.a[1]) as.push(l.a[0]);
        $port.postMessage({ exec: [a[0], as] });
        return;
      }
      default: throw new Error("heff: not an operation of process: " + (p && p.$));
    }
  };
};
