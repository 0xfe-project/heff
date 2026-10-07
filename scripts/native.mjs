#!/usr/bin/env node
// The compiler as a program of its own: app/heff/main.hf compiled to C by
// output/heff.mjs (`npm run build` first), then by `zig cc` for each
// TARGET (a zig target, as `zig targets` names them; the host's without
// one) to output/native/TARGET/heff (heff.exe for Windows).  zig builds
// every target from any host.
//
//   node scripts/native.mjs [TARGET...]
import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(root, "output", "native");
mkdirSync(out, { recursive: true });

const run = (cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, stdio: "inherit" });
  if (r.status !== 0) {
    console.error(`heff native: ${cmd} ${args.join(" ")} failed`);
    process.exit(1);
  }
};

const c = join(out, "heff.c");
run("node", [join(root, "output", "heff.mjs"), "compile", "app/heff/main.hf", c, "--name", "main", "--backend=c"]);

const targets = process.argv.slice(2);
for (const t of targets.length ? targets : ["native"]) {
  const dir = join(out, t);
  mkdirSync(dir, { recursive: true });
  const exe = join(dir, t.includes("windows") ? "heff.exe" : "heff");
  // (stripped; Linux's linked statically by musl, so it runs anywhere)
  run("zig", ["cc", ...(t === "native" ? [] : ["-target", t]), "-O2", "-s", "-w", c, "-o", exe, "-lm"]);
  console.log(`heff native: ${exe}`);
}
