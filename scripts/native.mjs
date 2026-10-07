#!/usr/bin/env node
// heff for other systems: output/heff.c (scripts/build.mjs, first) built
// by `zig cc` for each TARGET (a zig target, as `zig targets` names them)
// to output/native/TARGET/heff (heff.exe for Windows).  zig builds every
// target from any host.
//
//   node scripts/native.mjs TARGET...
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const c = join(root, "output", "heff.c");
if (!existsSync(c)) { console.error("heff native: no output/heff.c (run node scripts/build.mjs first)"); process.exit(1); }

for (const t of process.argv.slice(2)) {
  const dir = join(root, "output", "native", t);
  mkdirSync(dir, { recursive: true });
  const exe = join(dir, t.includes("windows") ? "heff.exe" : "heff");
  // (stripped; Linux's linked statically by musl, so it runs anywhere)
  const r = spawnSync("zig", ["cc", "-target", t, "-O2", "-s", "-w", c, "-o", exe, "-lm"], { cwd: root, stdio: "inherit" });
  if (r.status !== 0) { console.error(`heff native: ${t} failed`); process.exit(1); }
  console.log(`heff native: ${exe}`);
}
