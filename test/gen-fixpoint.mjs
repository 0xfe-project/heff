#!/usr/bin/env node
// The compiler is its own fixed point: output/heff.mjs (built by
// scripts/build.mjs) compiles its own source -- loading its import closure
// itself -- to output/heff-gen.mjs, which must be output/heff.raw.mjs,
// what heff.mjs was minified from, byte for byte.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const bin = join(root, "output", "heff.mjs");
const raw = join(root, "output", "heff.raw.mjs");
const out = join(root, "output", "heff-gen.mjs");

if (!existsSync(bin) || !existsSync(raw)) { console.log("FAIL gen-fixpoint: no output/heff.mjs and output/heff.raw.mjs (run npm run build first)"); process.exit(1); }

const r = spawnSync("node", [bin, "node", "compiler/main.hf", "main", out],
  { cwd: root, encoding: "utf8", maxBuffer: 1e9 });
if (r.status !== 0) { console.log(`FAIL gen-fixpoint: the compiler refused its own source\n${r.stdout}${r.stderr}`.split("\n").slice(0, 12).join("\n")); process.exit(1); }

const g0 = readFileSync(raw, "utf8");
const g1 = readFileSync(out, "utf8");
if (g0 !== g1) {
  const a = g0.split("\n"), b = g1.split("\n");
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if (a[i] !== b[i]) { console.log(`FAIL gen-fixpoint: first difference at line ${i + 1}\n  built:     ${(a[i] || "").slice(0, 120)}\n  its child: ${(b[i] || "").slice(0, 120)}`); break; }
  }
  process.exit(1);
}

console.log(`gen-fixpoint: the compiler compiles itself to itself, ${g1.length} bytes, byte-identical`);
