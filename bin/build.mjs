#!/usr/bin/env node
// The self-hosted compiler, bootstrapped: the reference (bin/reference.mjs)
// compiles compiler/main.hf to generation 0 (output/heff-gen0.mjs), and
// generation 0 compiles the same source to generation 1 (output/heff.mjs),
// the one bin/heff.mjs runs -- the compiler as its own backend makes it.
// (test/gen-fixpoint.mjs holds that the next generation is this one.)
import { spawnSync } from "node:child_process";
import { readdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const gen0 = join(root, "output", "heff-gen0.mjs");
const gen1 = join(root, "output", "heff.mjs");

// (nothing to do where generation 1 is newer than every source of it and
// than the reference that began it)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [join(root, "output", "Main", "index.js"), join(root, "bin", "build.mjs"),
  ...["compiler", "lib", "fmt"].flatMap((d) => readdirSync(join(root, d)).filter((f) => f.endsWith(".hf") || f.endsWith(".js")).map((f) => join(root, d, f)))];
if (mtime(gen1) >= 0 && sources.every((s) => mtime(s) <= mtime(gen1))) {
  console.log(`heff build: ${gen1} is up to date`);
  process.exit(0);
}

const step = (what, cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, stdio: ["ignore", "ignore", "inherit"] });
  if (r.status !== 0) { console.error(`heff build: ${what} failed`); process.exit(r.status ?? 1); }
};

step("generation 0 (the reference compiling compiler/main.hf)",
  process.execPath, [join(root, "bin", "reference.mjs"), "node", "compiler/main.hf", "main", gen0]);
step("generation 1 (generation 0 compiling itself)",
  process.execPath, [join(root, "test", "run-worker.mjs"), gen0, "node", "compiler/main.hf", "main", gen1]);
console.log(`heff build: ${gen1}, the compiler compiled by itself`);
