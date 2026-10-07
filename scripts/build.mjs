#!/usr/bin/env node
// The compiler, bootstrapped from its seed: boot/heff.mjs.gz (the
// compiler, compiled by itself to JavaScript, kept compressed), unpacked
// to output/heff-seed.mjs, compiles app/heff/main.hf to a first stage,
// output/heff-stage1.mjs, and the first stage compiles the same source to
// C, output/heff.c -- made by the backend of the source, whatever the
// seed's was -- which `zig cc` builds into output/heff, `heff`.
// (test/gen-fixpoint.mjs holds that heff compiles itself to the same C;
// `node scripts/boot.mjs` renews the seed.)
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const seed = join(root, "boot", "heff.mjs.gz");
const unpacked = join(root, "output", "heff-seed.mjs");
const stage1 = join(root, "output", "heff-stage1.mjs");
const c = join(root, "output", "heff.c");
const out = join(root, "output", process.platform === "win32" ? "heff.exe" : "heff");

// (nothing to do where the compiler is newer than every source of it and
// than the seed)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [seed, join(root, "scripts", "build.mjs"),
  ...["app/heff", "src/heff", "src/heff/fmt", "lib", "lib/std", "lib/c"].flatMap((d) =>
    readdirSync(join(root, d)).filter((f) => /\.(hf|js|c)$/.test(f)).map((f) => join(root, d, f)))];
if (mtime(out) >= 0 && sources.every((s) => mtime(s) <= mtime(out))) {
  console.log(`heff build: ${out} is up to date`);
  process.exit(0);
}

const step = (what, cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, stdio: ["ignore", "ignore", "inherit"] });
  if (r.error?.code === "ENOENT" && cmd === "zig") {
    console.error("heff build: zig is not on the PATH (https://ziglang.org/download/)");
    process.exit(1);
  }
  if (r.status !== 0) { console.error(`heff build: ${what} failed`); process.exit(r.status ?? 1); }
};

mkdirSync(join(root, "output"), { recursive: true });
writeFileSync(unpacked, gunzipSync(readFileSync(seed)));
step("the first stage (the seed compiling app/heff/main.hf)", process.execPath,
  [unpacked, "compile", "app/heff/main.hf", stage1, "--name", "main", "--backend=node"]);
step("the compiler in C (the first stage compiling itself)", process.execPath,
  [stage1, "compile", "app/heff/main.hf", c, "--name", "main", "--backend=c"]);
// (for any machine of this kind, not only this one: zig's default is
// this machine's processor, whose instructions another may not have)
step("zig cc", "zig", ["cc", "-mcpu=baseline", "-O2", "-w", c, "-o", out, "-lm"]);
console.log(`heff build: ${out}, the compiler compiled by itself`);
