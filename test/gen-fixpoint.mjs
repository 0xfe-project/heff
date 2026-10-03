#!/usr/bin/env node
// The generations agree: generation 0 (output/heff-selfcheck.mjs, the
// reference's compilation of compiler/main.hf, kept by self-check) is
// given its own sources -- in the order the reference's loader visits
// them -- and compiles itself with its own backend.  The program it
// writes must be generation 0 again, byte for byte: the compiler is a
// fixed point of itself.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const bin = join(root, "output", "heff-selfcheck.mjs");

if (!existsSync(bin)) { console.log("FAIL gen-fixpoint: no output/heff-selfcheck.mjs (run test/self-check.mjs first)"); process.exit(1); }

// generation 1: the compiler, compiled by itself -- it loads its own
// import closure (compiler/load.hf), no file list given
const out = join(root, "output", "heff-gen1.mjs");
const r = spawnSync("node", ["--stack-size=60000", bin, "node", "main", out, "compiler/main.hf"], { cwd: root, encoding: "utf8", maxBuffer: 1e9 });
if (r.status !== 0) { console.log(`FAIL gen-fixpoint: generation 0 refused its own source\n${r.stdout}${r.stderr}`.split("\n").slice(0, 12).join("\n")); process.exit(1); }

const g0 = readFileSync(bin, "utf8");
const g1 = readFileSync(out, "utf8");
if (g0 !== g1) {
  const a = g0.split("\n"), b = g1.split("\n");
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if (a[i] !== b[i]) { console.log(`FAIL gen-fixpoint: first difference at line ${i + 1}\n  generation 0: ${(a[i] || "").slice(0, 120)}\n  generation 1: ${(b[i] || "").slice(0, 120)}`); break; }
  }
  process.exit(1);
}

console.log(`gen-fixpoint: the compiler compiles itself to itself, ${g1.length} bytes, byte-identical`);
