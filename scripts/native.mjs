#!/usr/bin/env node
// heff for other systems: .heff/out/heff.d/ (scripts/build.mjs, first) built
// by `zig cc` for each TARGET (a zig target, as `zig targets` names them)
// to .heff/out/native/TARGET/heff (heff.exe for Windows).  zig builds every
// target from any host.
//
//   node scripts/native.mjs TARGET...
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const parts = join(root, ".heff", "out", "heff.d");
if (!existsSync(parts)) { console.error("heff native: no .heff/out/heff.d (run node scripts/build.mjs first)"); process.exit(1); }
const files = readdirSync(parts).filter((f) => f.endsWith(".c")).map((f) => join(parts, f));

for (const t of process.argv.slice(2)) {
  const dir = join(root, ".heff", "out", "native", t);
  mkdirSync(dir, { recursive: true });
  const exe = join(dir, t.includes("windows") ? "heff.exe" : "heff");
  // (stripped; Linux's linked statically by musl, so it runs anywhere)
  const r = spawnSync("zig", ["cc", "-target", t, "-O2", "-s", "-w", ...files, "-o", exe, "-lm"], { cwd: root, stdio: "inherit" });
  if (r.status !== 0) { console.error(`heff native: ${t} failed`); process.exit(1); }
  console.log(`heff native: ${exe}`);
}
