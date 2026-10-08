#!/usr/bin/env node
// heff compiled through C, checked by AddressSanitizer: .heff/out/heff.d/
// (scripts/build.mjs) built by the system's clang with
// `-fsanitize=address` (zig's has no ASan runtime) and no free lists
// (`-DSMALL_MAX=0`, so that a use after the last reference is a use after
// free), then made to check each program of test/programs in .heff/ref/native-asan,
// where there is no cache but its own (one written by the Node.js
// compiler would spare it the elaborating).
//
//   node test/native-asan.mjs
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync } from "node:fs";
import { basename, join } from "node:path";
import { root } from "./heff.mjs";
import { pool, run } from "./pool.mjs";

// (the program beside its library and version, as a release lays it out,
// in a project as the repository is, its modules named as they are there)
const work = join(root, ".heff", "ref", "native-asan");
rmSync(work, { recursive: true, force: true });
mkdirSync(join(work, "bin"), { recursive: true });
cpSync(join(root, "lib"), join(work, "lib"), { recursive: true });
cpSync(join(root, "package.hf"), join(work, "package.hf"));
// (the programs checked there, apart from the repository and its cache)
cpSync(join(root, "test", "programs"), join(work, "test", "programs"), { recursive: true });

const step = (what, cmd, args, opts = {}) => {
  const r = spawnSync(cmd, args, { cwd: root, encoding: "utf8", maxBuffer: 1 << 26, ...opts });
  if (r.status !== 0) {
    console.log(`${what} failed:\n${r.stdout || ""}${r.stderr || ""}${r.error || ""}`);
    process.exit(1);
  }
  return r;
};

const parts = join(root, ".heff", "out", "heff.d"), heff = join(work, "bin", "heff");
const c = readdirSync(parts).filter((f) => f.endsWith(".c")).map((f) => join(parts, f));
// (a part an object, as many at once as there are cores -- clang given
// them all builds them one after another -- then the objects linked; an
// object kept from a run before, in .heff/ref/native-asan.o/, named by
// what it is made of -- the clang, the flags, heff.h and the part -- is
// not made again, and one no part is made of now is let go)
const flags = ["-O1", "-g", "-w", "-fsanitize=address", "-fno-omit-frame-pointer", "-DSMALL_MAX=0"];
const kept = join(root, ".heff", "ref", "native-asan.o");
mkdirSync(kept, { recursive: true });
const madeOf = createHash("sha256").update(spawnSync("clang", ["--version"], { encoding: "utf8" }).stdout ?? "")
  .update(flags.join(" ")).update(readFileSync(join(parts, "heff.h")));
const objects = await pool(c, async (f) => {
  const o = join(kept, `${basename(f, ".c")}-${madeOf.copy().update(readFileSync(f)).digest("hex").slice(0, 16)}.o`);
  if (existsSync(o)) return o;
  const r = await run("clang", [...flags, "-c", f, "-o", o + ".part"]);
  if (r.status !== 0) { console.log(`building it failed, at ${basename(f)}:\n${r.stdout}${r.stderr}${r.error || ""}`); process.exit(1); }
  renameSync(o + ".part", o);
  return o;
});
for (const n of readdirSync(kept)) if (!objects.includes(join(kept, n))) rmSync(join(kept, n));
step("building it", "clang", [...flags, ...objects, "-o", heff, "-lm", "-lpthread"]);

// (what it leaves at its end is no fault: a program's last objects are
// the system's to take back)
const env = { ...process.env, ASAN_OPTIONS: "detect_leaks=0" };
let failed = 0;
const files = readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort();
for (const f of files) {
  const r = spawnSync(heff, ["check", join("test", "programs", f)], { cwd: work, encoding: "utf8", env, maxBuffer: 1 << 26 });
  if (r.status !== 0) {
    failed++;
    console.log(`FAILED ${f}:\n${r.stderr.split("\n").slice(0, 40).join("\n")}`);
  }
}
console.log(`native-asan: ${files.length - failed} of ${files.length} checked`);
process.exit(failed ? 1 : 0);
