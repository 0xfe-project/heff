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
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { availableParallelism } from "node:os";
import { basename, dirname, join } from "node:path";
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
const noZig = () => {
  console.error("heff build: zig is not on the PATH (https://ziglang.org/download/)");
  process.exit(1);
};
const run = (cmd, args) => {
  const r = spawnSync(cmd, args, { cwd: root, env, stdio: ["ignore", "ignore", "inherit"] });
  if (r.error?.code === "ENOENT" && cmd === "zig") noZig();
  return r.status === 0;
};
const fail = (what) => { console.error(`heff build: ${what} failed`); process.exit(1); };
// (a step, and how long it took, said once it is done: where a build's
// time goes, in a CI's log too)
const timed = async (what, step) => {
  const t0 = Date.now(), ok = await step();
  if (ok) console.log(`heff build: ${what()} in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  return ok;
};

// (heff's source, compiled by `compiler` to C as parts in `dir`)
const toC = (compiler, dir) => timed(() => `${basename(compiler)} compiled the source to C`, () =>
  run(compiler, ["compile", "app/heff/main.hf", dir + "/", "--name", "main", "--backend=c"]));

// (the parts in `dir`, built by zig into `to`: for any machine of this
// kind, not only this one -- zig's default is this machine's processor,
// whose instructions another may not have.  A part an object, as many at
// once as there are cores, then the objects linked; an object kept from
// a build before, in .heff/out/heff.o/, named by what it is made of --
// the zig and the machine it builds for, the flags, heff.h and the part
// -- is not made again, and one no stage of this build is made of is let
// go.  zig's own cache knows a C file by its inode and time, not by what
// is in it, so it builds again a part written again, as every part is in
// a CI's checkout or its cache restored.)
const flags = ["-mcpu=baseline", "-O2", "-w"];
const kept = join(root, ".heff", "out", "heff.o");
const used = new Set();
let zigIs = null;
const zig = () => {
  if (zigIs === null) {
    // (its version and target, as `zig env` says them, else all it says)
    const r = spawnSync("zig", ["env"], { encoding: "utf8" });
    if (r.error?.code === "ENOENT") noZig();
    const said = r.stdout ?? "", v = /\.version = "([^"]*)"/.exec(said), t = /\.target = "([^"]*)"/.exec(said);
    zigIs = v && t ? `${v[1]} ${t[1]}` : said;
  }
  return zigIs;
};
const object = (part, o) => new Promise((done) => {
  const p = spawn("zig", ["cc", ...flags, "-c", part, "-o", o + ".part"], { cwd: root, env, stdio: ["ignore", "ignore", "inherit"] });
  p.on("error", () => done(false));
  p.on("close", (status) => {
    if (status === 0) renameSync(o + ".part", o);
    done(status === 0);
  });
});
const cc = (dir, to) => {
  let anew = 0, all = 0;
  return timed(() => `zig built ${basename(to)} (${anew} of its ${all} parts anew)`, async () => {
    mkdirSync(kept, { recursive: true });
    const madeOf = createHash("sha256").update(zig()).update(flags.join(" ")).update(readFileSync(join(dir, "heff.h")));
    const files = readdirSync(dir).filter((f) => f.endsWith(".c") && !f.startsWith(".")).map((f) => join(dir, f));
    const objects = files.map((f) =>
      join(kept, `${basename(f, ".c")}-${madeOf.copy().update(readFileSync(f)).digest("hex").slice(0, 16)}.o`));
    for (const o of objects) used.add(basename(o));
    const make = files.flatMap((f, k) => existsSync(objects[k]) ? [] : [[f, objects[k]]]);
    [anew, all] = [make.length, files.length];
    let next = 0, ok = true;
    await Promise.all(Array.from({ length: Math.min(availableParallelism(), make.length) }, async () => {
      while (ok && next < make.length) {
        const [f, o] = make[next++];
        if (!await object(f, o)) ok = false;
      }
    }));
    return ok && run("zig", ["cc", ...flags, ...objects, "-o", to, "-lm"]);
  });
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
    if (!await toC(stage0, stage1C)) console.error("heff build: the heff there is cannot compile the source; from the seed");
    else if (!await cc(stage1C, stage1)) console.error("heff build: the heff there is compiles the source to C zig cannot build; from the seed");
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
  if (!await cc(seedC, seedExe)) fail("zig cc");
  if (!await toC(seedExe, stage1C)) fail("the seed compiling app/heff/main.hf");
  if (!await cc(stage1C, stage1)) fail("zig cc");
}
if (!await toC(stage1, parts)) { console.error("heff build: the first stage could not compile itself"); process.exit(1); }
if (!await cc(parts, out)) fail("zig cc");
for (const n of readdirSync(kept)) if (!used.has(n)) rmSync(join(kept, n));
// (the heff built comes of the seed there is: built from it, or with a
// heff of it)
writeFileSync(seedOf, seedHash + "\n");
console.log(`heff build: ${out}, the compiler compiled by itself${first ? "" : " (from the seed)"}`);
