#!/usr/bin/env node
// The compiler, bootstrapped from its seed: boot/heff.mjs (the compiler,
// compiled by itself, kept) compiles compiler/main.hf to a first stage,
// and the first stage compiles the same source to output/heff.mjs, the
// one bin/heff.mjs runs -- made by the backend of the source, whatever
// the seed's was.  (test/gen-fixpoint.mjs holds that the next generation
// is this one; `npm run boot` keeps it as the seed.)
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const seed = join(root, "boot", "heff.mjs");
const stage1 = join(root, "output", "heff-stage1.mjs");
const out = join(root, "output", "heff.mjs");

// (nothing to do where the compiler is newer than every source of it and
// than the seed)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [seed, join(root, "bin", "build.mjs"),
  ...["compiler", "lib", "fmt"].flatMap((d) => readdirSync(join(root, d)).filter((f) => f.endsWith(".hf") || f.endsWith(".js")).map((f) => join(root, d, f)))];
if (mtime(out) >= 0 && sources.every((s) => mtime(s) <= mtime(out))) {
  console.log(`heff build: ${out} is up to date`);
  process.exit(0);
}

const step = (what, compiler, to) => {
  const r = spawnSync(process.execPath, [join(root, "test", "run-worker.mjs"), compiler, "node", "compiler/main.hf", "main", to],
    { cwd: root, stdio: ["ignore", "ignore", "inherit"], env: { ...process.env, HEFF_LIB: join(root, "lib") } });
  if (r.status !== 0) { console.error(`heff build: ${what} failed`); process.exit(r.status ?? 1); }
};
mkdirSync(join(root, "output"), { recursive: true });
step("the first stage (the seed compiling compiler/main.hf)", seed, stage1);
step("the compiler (the first stage compiling itself)", stage1, out);
console.log(`heff build: ${out}, the compiler compiled by itself`);
