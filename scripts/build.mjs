#!/usr/bin/env node
// The compiler, bootstrapped: a compiler there is (stage 0) compiles
// app/heff/main.hf to a first stage, and the first stage compiles the
// same source to C, .heff/out/heff.d/ (a file a module) -- made by the
// backend of the source, whatever stage 0's was -- which `zig cc` builds
// (each file apart, and again only those changed) into .heff/out/heff,
// `heff`.  Stage 0 is the heff built before (.heff/out/heff), as Go builds
// with a Go there is, if it comes of the seed there is: built from it, or
// with a heff that came of it (.heff/out/heff.seed, the seed's hash).  A
// seed is renewed when the source wants what an older heff has not, so a
// heff of another seed, or of one not known, is not built with.  Where
// there is no heff of the seed, or it cannot make the first stage, the
// seed: boot/heff.tar.xz, heff compiled by itself to C, unpacked to
// .heff/out/heff-seed.d/ and built by zig for this machine.
// (test/gen-fixpoint.mjs holds that heff compiles itself to the same C;
// `node scripts/boot.mjs` renews the seed.)
//
//   node scripts/build.mjs [--seed]     (--seed: from the seed, always)
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const exe = process.platform === "win32" ? ".exe" : "";
const seed = join(root, "boot", "heff.tar.xz");
const out = join(root, ".heff", "out", "heff" + exe);
const parts = join(root, ".heff", "out", "heff.d");
const fromSeed = process.argv.includes("--seed");

// (the seed there is, by its hash, and the seed the heff there is comes
// of, by the hash kept beside it when it was built -- none beside a heff
// built before the hash was kept, whose seed is not known)
const seedOf = join(root, ".heff", "out", "heff.seed");
const seedHash = createHash("sha256").update(readFileSync(seed)).digest("hex");
const heffSeed = (() => { try { return readFileSync(seedOf, "utf8").trim(); } catch { return ""; } })();

// (nothing to do where the compiler comes of the seed there is and is
// newer than every source of it and than the seed)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [seed, join(root, "scripts", "build.mjs"),
  ...["app/heff", "src/heff", "src/heff/fmt", "lib", "lib/std", "lib/c"].flatMap((d) =>
    readdirSync(join(root, d)).filter((f) => /\.(hf|js|c)$/.test(f)).map((f) => join(root, d, f)))];
if (!fromSeed && mtime(out) >= 0 && heffSeed === seedHash && sources.every((s) => mtime(s) <= mtime(out))) {
  console.log(`heff build: ${out} is up to date`);
  process.exit(0);
}

// (the library each stage compiles with: the repository's, wherever the
// stage is -- a heff from before .heff/out/ looks for it beside itself)
const env = { ...process.env, HEFF_LIB: process.env.HEFF_LIB ?? join(root, "lib") };
const run = (cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, env, stdio: ["ignore", "ignore", "inherit"] });
  if (r.error?.code === "ENOENT" && cmd === "zig") {
    console.error("heff build: zig is not on the PATH (https://ziglang.org/download/)");
    process.exit(1);
  }
  return r.status === 0;
};
const fail = (what) => { console.error(`heff build: ${what} failed`); process.exit(1); };

// (heff's source, compiled by `compiler` to C as parts in `dir`)
const toC = (compiler, dir) =>
  run(compiler, ["compile", "app/heff/main.hf", dir + "/", "--name", "main", "--backend=c"]);
// (the parts in `dir`, built by zig into `to`: for any machine of this
// kind, not only this one -- zig's default is this machine's processor,
// whose instructions another may not have)
const cc = (dir, to) => {
  const files = readdirSync(dir).filter((f) => f.endsWith(".c") && !f.startsWith(".")).map((f) => join(dir, f));
  return run("zig", ["cc", "-mcpu=baseline", "-O2", "-w", ...files, "-o", to, "-lm"]);
};

mkdirSync(join(root, ".heff", "out"), { recursive: true });
const stage0 = join(root, ".heff", "out", "heff-stage0" + exe);
const stage1 = join(root, ".heff", "out", "heff-stage1" + exe);
const stage1C = join(root, ".heff", "out", "heff-stage1.d");
let first = false;
if (!fromSeed && mtime(out) >= 0) {
  if (heffSeed !== seedHash)
    console.log(`heff build: the heff there is comes of ${heffSeed ? "another seed" : "a seed not known"}; from the seed`);
  else {
    // (the heff there is, set apart: the build writes over it)
    copyFileSync(out, stage0);
    if (!toC(stage0, stage1C)) console.error("heff build: the heff there is cannot compile the source; from the seed");
    else if (!cc(stage1C, stage1)) console.error("heff build: the heff there is compiles the source to C zig cannot build; from the seed");
    else first = true;
  }
}
if (!first) {
  // (the seed, built for this machine; `tar` with xz: libarchive's, on
  // macOS and Windows, GNU's with xz, on Linux)
  const seedC = join(root, ".heff", "out", "heff-seed.d");
  const seedExe = join(root, ".heff", "out", "heff-seed" + exe);
  rmSync(seedC, { recursive: true, force: true });
  mkdirSync(seedC, { recursive: true });
  if (!run("tar", ["-xJf", seed, "-C", seedC])) fail("unpacking the seed");
  if (!cc(seedC, seedExe)) fail("zig cc");
  if (!toC(seedExe, stage1C)) fail("the seed compiling app/heff/main.hf");
  if (!cc(stage1C, stage1)) fail("zig cc");
}
if (!toC(stage1, parts)) { console.error("heff build: the first stage could not compile itself"); process.exit(1); }
if (!cc(parts, out)) fail("zig cc");
// (the heff built comes of the seed there is: built from it, or with a
// heff of it)
writeFileSync(seedOf, seedHash + "\n");
console.log(`heff build: ${out}, the compiler compiled by itself${first ? "" : " (from the seed)"}`);
