#!/usr/bin/env node
// The compiler, bootstrapped: a compiler there is (stage 0) compiles
// app/heff/main.hf to a first stage, and the first stage compiles the
// same source to C, output/heff.d/ (a file a module) -- made by the
// backend of the source, whatever stage 0's was -- which `zig cc` builds
// (each file apart, and again only those changed) into output/heff,
// `heff`.  Stage 0 is the heff built before (output/heff), as Go builds
// with a Go there is; where there is none, or it cannot compile the
// source, the seed: boot/heff.tar.xz, heff compiled by itself to C,
// unpacked to output/heff-seed.d/ and built by zig for this machine.
// (test/gen-fixpoint.mjs holds that heff compiles itself to the same C;
// `node scripts/boot.mjs` renews the seed.)
//
//   node scripts/build.mjs [--seed]     (--seed: from the seed, always)
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdirSync, readdirSync, rmSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.platform === "win32" ? ".exe" : "";
const seed = join(root, "boot", "heff.tar.xz");
const out = join(root, "output", "heff" + exe);
const parts = join(root, "output", "heff.d");
const fromSeed = process.argv.includes("--seed");

// (nothing to do where the compiler is newer than every source of it and
// than the seed)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [seed, join(root, "scripts", "build.mjs"),
  ...["app/heff", "src/heff", "src/heff/fmt", "lib", "lib/std", "lib/c"].flatMap((d) =>
    readdirSync(join(root, d)).filter((f) => /\.(hf|js|c)$/.test(f)).map((f) => join(root, d, f)))];
if (!fromSeed && mtime(out) >= 0 && sources.every((s) => mtime(s) <= mtime(out))) {
  console.log(`heff build: ${out} is up to date`);
  process.exit(0);
}

const run = (cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, stdio: ["ignore", "ignore", "inherit"] });
  if (r.error?.code === "ENOENT" && cmd === "zig") {
    console.error("heff build: zig is not on the PATH (https://ziglang.org/download/)");
    process.exit(1);
  }
  return r.status === 0;
};
const step = (what, cmd, args) => {
  if (!run(cmd, args)) { console.error(`heff build: ${what} failed`); process.exit(1); }
};

// (heff's source, compiled by `compiler` to C as parts in `dir`)
const toC = (compiler, dir) =>
  run(compiler, ["compile", "app/heff/main.hf", dir + "/", "--name", "main", "--backend=c"]);
// (the parts in `dir`, built by zig into `to`: for any machine of this
// kind, not only this one -- zig's default is this machine's processor,
// whose instructions another may not have)
const cc = (dir, to) => {
  const files = readdirSync(dir).filter((f) => f.endsWith(".c")).map((f) => join(dir, f));
  step("zig cc", "zig", ["cc", "-mcpu=baseline", "-O2", "-w", ...files, "-o", to, "-lm"]);
};

mkdirSync(join(root, "output"), { recursive: true });
const stage0 = join(root, "output", "heff-stage0" + exe);
const stage1 = join(root, "output", "heff-stage1" + exe);
const stage1C = join(root, "output", "heff-stage1.d");
let first = false;
if (!fromSeed && mtime(out) >= 0) {
  // (the heff there is, set apart: the build writes over it)
  copyFileSync(out, stage0);
  first = toC(stage0, stage1C);
  if (!first) console.error("heff build: the heff there is cannot compile the source; from the seed");
}
if (!first) {
  // (the seed, built for this machine; `tar` with xz: libarchive's, on
  // macOS and Windows, GNU's with xz, on Linux)
  const seedC = join(root, "output", "heff-seed.d");
  const seedExe = join(root, "output", "heff-seed" + exe);
  rmSync(seedC, { recursive: true, force: true });
  mkdirSync(seedC, { recursive: true });
  step("unpacking the seed", "tar", ["-xJf", seed, "-C", seedC]);
  cc(seedC, seedExe);
  step("the seed compiling app/heff/main.hf", seedExe,
    ["compile", "app/heff/main.hf", stage1C + "/", "--name", "main", "--backend=c"]);
}
cc(stage1C, stage1);
if (!toC(stage1, parts)) { console.error("heff build: the first stage could not compile itself"); process.exit(1); }
cc(parts, out);
console.log(`heff build: ${out}, the compiler compiled by itself${first ? "" : " (from the seed)"}`);
