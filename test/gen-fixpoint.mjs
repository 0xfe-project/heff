#!/usr/bin/env node
// The compiler is its own fixed point: heff (built by scripts/build.mjs
// from output/heff.c) compiles its own source -- loading its import
// closure itself -- to output/heff-gen.c, which must be output/heff.c,
// byte for byte.
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { heff, heffBin, root } from "./heff.mjs";

const c = join(root, "output", "heff.c");
const out = join(root, "output", "heff-gen.c");

if (!existsSync(heffBin) || !existsSync(c)) { console.log("FAIL gen-fixpoint: no output/heff and output/heff.c (run node scripts/build.mjs first)"); process.exit(1); }

const r = heff(["compile", "app/heff/main.hf", out, "--name", "main", "--backend=c"], { cwd: root });
if (r.status !== 0) { console.log(`FAIL gen-fixpoint: the compiler refused its own source (status ${r.status}, signal ${r.signal}${r.error ? `, ${r.error}` : ""})\n${r.stdout}${r.stderr}`.split("\n").slice(0, 12).join("\n")); process.exit(1); }

const g0 = readFileSync(c, "utf8");
const g1 = readFileSync(out, "utf8");
if (g0 !== g1) {
  const a = g0.split("\n"), b = g1.split("\n");
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if (a[i] !== b[i]) { console.log(`FAIL gen-fixpoint: first difference at line ${i + 1}\n  built:     ${(a[i] || "").slice(0, 120)}\n  its child: ${(b[i] || "").slice(0, 120)}`); break; }
  }
  process.exit(1);
}

console.log(`gen-fixpoint: the compiler compiles itself to itself, ${g1.length} bytes of C, byte-identical`);
