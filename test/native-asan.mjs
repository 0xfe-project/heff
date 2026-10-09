#!/usr/bin/env node
// heff compiled through C, checked by AddressSanitizer: .heff/out/heff.d/
// (scripts/build.mjs) built by the system's clang with
// `-fsanitize=address` (zig's has no ASan runtime) and no free lists
// (`-DSMALL_MAX=0`, so that a use after the last reference is a use after
// free), then made to check each program of test/programs in .heff/ref/native-asan,
// where there is no cache but its own (one written by the Node.js
// compiler would spare it the elaborating); and a program of them
// compiled to C by it, built under ASan the same way, and run.
// (What each part took to build, and each program to check, said as it
// ends, the slowest again at the end, so that a log shows what held it
// up; a program checked that has not ended in a minute is killed and
// has failed: under ASan a check takes seconds at most, so one that has
// not ended will not.)
//
//   node test/native-asan.mjs
//   HEFF_ASAN_JOBS=N       N at once, building or checking
//   HEFF_ASAN_TIMEOUT=S    each check given S seconds (60)
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync } from "node:fs";
import { availableParallelism } from "node:os";
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
// (under CI, four at once at most: a part takes clang up to some 750MB,
// and a container's memory may be less than its cores' share)
const jobs = Number(process.env.HEFF_ASAN_JOBS) || (process.env.CI ? Math.min(availableParallelism(), 4) : availableParallelism());
const flags = ["-O1", "-g", "-w", "-fsanitize=address", "-fno-omit-frame-pointer", "-DSMALL_MAX=0"];
const kept = join(root, ".heff", "ref", "native-asan.o");
mkdirSync(kept, { recursive: true });
const clang = spawnSync("clang", ["--version"], { encoding: "utf8" }).stdout ?? "";
const madeOf = createHash("sha256").update(clang).update(flags.join(" ")).update(readFileSync(join(parts, "heff.h")));
// (seconds since `t`, and the slowest of `[name, seconds]`s)
const since = (t) => (performance.now() - t) / 1000;
const slowest = (took) => took.toSorted((a, b) => b[1] - a[1]).slice(0, 5).map(([n, s]) => `${n} ${s.toFixed(1)}s`).join(", ");
// (with the bits of Linux's ASLR, where they are said: an ASan of a clang
// before 18 may fault at its start where there are more than 28, and say
// it did, over and over, never ending)
const aslr = existsSync("/proc/sys/vm/mmap_rnd_bits") ? `, vm.mmap_rnd_bits ${readFileSync("/proc/sys/vm/mmap_rnd_bits", "utf8").trim()}` : "";
console.log(`native-asan: ${clang.split("\n")[0]}${aslr}, ${jobs} at once`);
const built = [], t0 = performance.now();
const objects = await pool(c, async (f) => {
  const o = join(kept, `${basename(f, ".c")}-${madeOf.copy().update(readFileSync(f)).digest("hex").slice(0, 16)}.o`);
  if (existsSync(o)) return o;
  const t = performance.now();
  const r = await run("clang", [...flags, "-c", f, "-o", o + ".part"]);
  if (r.status !== 0) { console.log(`building it failed, at ${basename(f)} (${r.signal ?? `status ${r.status}`}, ${since(t).toFixed(1)}s):\n${r.stdout}${r.stderr}${r.error || ""}`); process.exit(1); }
  renameSync(o + ".part", o);
  built.push([basename(f), since(t)]);
  console.log(`native-asan: built ${basename(f)} in ${since(t).toFixed(1)}s`);
  return o;
}, jobs);
for (const n of readdirSync(kept)) if (!objects.includes(join(kept, n))) rmSync(join(kept, n));
const t1 = performance.now();
step("building it", "clang", [...flags, ...objects, "-o", heff, "-lm", "-lpthread"]);
console.log(`native-asan: ${built.length} of ${c.length} parts built anew in ${since(t0).toFixed(1)}s, linked in ${since(t1).toFixed(1)}s${built.length ? `; the slowest: ${slowest(built)}` : ""}`);

// (what it leaves at its end is no fault: a program's last objects are
// the system's to take back)
const env = { ...process.env, ASAN_OPTIONS: "detect_leaks=0" };
const limit = Number(process.env.HEFF_ASAN_TIMEOUT) || 60;
const files = readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort();
const checked = [], t2 = performance.now();
let failed = 0;
await pool(files, async (f) => {
  const t = performance.now();
  // (of what it says, the first MB: what ASan says of a fault is less)
  const r = await run(heff, ["check", join("test", "programs", f)], { cwd: work, env, timeout: limit * 1000, keep: 1 << 20 });
  const s = since(t);
  checked.push([f, s]);
  if (r.status === 0) { console.log(`native-asan: checked ${f} in ${s.toFixed(1)}s`); return; }
  failed++;
  const why = r.error ? `${r.error}` : r.signal === "SIGKILL" && s >= limit ? `not ended in ${limit}s, killed` : `${r.signal ? `killed by ${r.signal}` : `status ${r.status}`}, ${s.toFixed(1)}s`;
  console.log(`FAILED ${f} (${why}):\n${r.stderr.split("\n").slice(0, 40).join("\n")}`);
}, jobs);
console.log(`native-asan: ${files.length - failed} of ${files.length} checked in ${since(t2).toFixed(1)}s; the slowest: ${slowest(checked)}`);

// (a program compiled to C by it, built under ASan as it is, each of its
// definitions shown: test/programs/jsstring.hf, whose JavaScript strings
// (std.js) are strings in C, held in lists, records and constructors and
// let go of -- each dropped as a string is, never as a constructor's,
// which none is)
for (const f of ["jsstring.hf"]) {
  const t = performance.now(), c = join(work, basename(f, ".hf") + ".c"), bin = join(work, basename(f, ".hf"));
  const names = [...readFileSync(join(root, "test", "programs", f), "utf8").matchAll(/^([a-z]\w*) :/gm)].map((m) => m[1]);
  const r = await run(heff, ["compile", join("test", "programs", f), c, "--backend=c"], { cwd: work, env });
  const b = r.status === 0 ? await run("clang", [...flags, c, "-o", bin, "-lm", "-lpthread"]) : r;
  const out = b.status === 0 ? await run(bin, names, { env, timeout: limit * 1000, keep: 1 << 20 }) : b;
  if (out.status === 0) { console.log(`native-asan: ${f} compiled to C, run in ${since(t).toFixed(1)}s`); continue; }
  failed++;
  console.log(`FAILED ${f} compiled to C (${out.signal ?? `status ${out.status}`}):\n${(out.stderr || "").split("\n").slice(0, 40).join("\n")}`);
}
process.exit(failed ? 1 : 0);
