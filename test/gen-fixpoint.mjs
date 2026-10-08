#!/usr/bin/env node
// The compiler is its own fixed point: heff (built by scripts/build.mjs
// from output/heff.d/: heff.h, and a C file a module) compiles its own
// source -- loading its import closure itself -- to output/heff-gen.d/,
// which must be output/heff.d/, file for file, byte for byte.
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { heff, heffBin, root } from "./heff.mjs";

const built = join(root, "output", "heff.d");
const gen = join(root, "output", "heff-gen.d");

if (!existsSync(heffBin) || !existsSync(built)) { console.log("FAIL gen-fixpoint: no output/heff and output/heff.d (run node scripts/build.mjs first)"); process.exit(1); }

const r = heff(["compile", "app/heff/main.hf", gen + "/", "--name", "main", "--backend=c"], { cwd: root });
if (r.status !== 0) { console.log(`FAIL gen-fixpoint: the compiler refused its own source (status ${r.status}, signal ${r.signal}${r.error ? `, ${r.error}` : ""})\n${r.stdout}${r.stderr}`.split("\n").slice(0, 12).join("\n")); process.exit(1); }

const files = (d) => readdirSync(d).filter((f) => f.endsWith(".c") || f.endsWith(".h")).sort();
const a = files(built), b = files(gen);
if (a.join() !== b.join()) { console.log(`FAIL gen-fixpoint: other files: ${a.filter((f) => !b.includes(f)).concat(b.filter((f) => !a.includes(f))).join(", ")}`); process.exit(1); }
let bytes = 0;
for (const f of a) {
  const g0 = readFileSync(join(built, f), "utf8"), g1 = readFileSync(join(gen, f), "utf8");
  bytes += g0.length;
  if (g0 !== g1) {
    const x = g0.split("\n"), y = g1.split("\n");
    for (let i = 0; i < Math.max(x.length, y.length); i++)
      if (x[i] !== y[i]) { console.log(`FAIL gen-fixpoint: ${f}, first difference at line ${i + 1}\n  built:     ${(x[i] || "").slice(0, 120)}\n  its child: ${(y[i] || "").slice(0, 120)}`); break; }
    process.exit(1);
  }
}
console.log(`gen-fixpoint: the compiler compiles itself to itself, ${a.length} files, ${bytes} bytes of C, byte-identical`);
