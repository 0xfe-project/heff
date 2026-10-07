#!/usr/bin/env node
// The benchmarks: each program here compiled by each backend (Node.js,
// and C by `zig cc -O2`) and run, the compiler checking itself
// (elaboration only: the code the compiler emits, measured apart from
// what its simplifier does), and compiling itself to C (its cache warm,
// then cold), each timed; the best of `--runs N` (3) is said, with its
// peak memory.  `--heff FILE` measures another compiler (one built
// before, say).
//
//   node bench/run.mjs [--runs N] [--heff FILE] [NAME...]
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readdirSync, renameSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
let heff = join(root, "output", process.platform === "win32" ? "heff.exe" : "heff");
const work = join(root, "ref", "bench");
mkdirSync(work, { recursive: true });

const argv = process.argv.slice(2);
let runs = 3;
const only = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--runs") runs = Number(argv[++i]);
  else if (argv[i] === "--heff") heff = join(process.cwd(), argv[++i]);
  else only.push(argv[i]);
}
const wanted = (name) => only.length === 0 || only.includes(name);

// (a run, timed by the system: its wall-clock seconds and peak resident
// memory, in MB)
const timed = (args, opts = {}) => {
  const r = spawnSync("/usr/bin/time", ["-l", ...args], { cwd: root, encoding: "utf8", maxBuffer: 1 << 26, ...opts });
  const real = Number((r.stderr.match(/([\d.]+) real/) || [])[1]);
  const rss = Number((r.stderr.match(/(\d+)\s+maximum resident set size/) || [])[1]) / (1 << 20);
  const ins = Number((r.stderr.match(/(\d+)\s+instructions retired/) || [])[1]) / 1e9;
  return { ok: r.status === 0, real, rss, ins, out: r.stdout, err: r.stderr };
};

const best = (name, once) => {
  let b = null;
  for (let i = 0; i < runs; i++) {
    const r = once();
    if (!r.ok) { console.log(`${name.padEnd(16)} FAILED\n${r.err}`); return; }
    if (b === null || r.real < b.real) b = r;
  }
  // (instructions, the steadiest: wall-clock time wanders by a tenth)
  console.log(`${name.padEnd(16)} ${b.real.toFixed(2).padStart(7)} s ${b.rss.toFixed(0).padStart(7)} MB ${b.ins.toFixed(2).padStart(8)} G instr`);
};

for (const f of readdirSync(join(root, "bench")).filter((f) => f.endsWith(".hf")).sort()) {
  const name = basename(f, ".hf");
  if (!wanted(name)) continue;
  const out = join(work, name + ".mjs");
  const c = spawnSync(heff, ["compile", join("bench", f), out, "--name", "main", "--backend=node"], { cwd: root, encoding: "utf8" });
  if (c.status !== 0) { console.log(`${name.padEnd(16)} did not compile\n${c.stdout}${c.stderr}`); continue; }
  best(`${name} (node)`, () => timed(["node", out]));
  const cOut = join(work, name + ".c"), bin = join(work, name);
  const cc = spawnSync(heff, ["compile", join("bench", f), cOut, "--name", "main", "--backend=c"], { cwd: root, encoding: "utf8" });
  const z = cc.status === 0 ? spawnSync("zig", ["cc", "-O2", "-w", cOut, "-o", bin, "-lm"], { encoding: "utf8" }) : cc;
  if (z.status !== 0) { console.log(`${name.padEnd(16)} did not compile to C\n${z.stdout}${z.stderr}`); continue; }
  best(`${name} (c)`, () => timed([bin]));
}

// (with no cache: the one there is put aside, and back after)
const coldly = (f) => {
  const cache = join(root, ".heff");
  const saved = mkdtempSync(join(tmpdir(), "heff-cache-"));
  const had = existsSync(cache);
  if (had) renameSync(cache, join(saved, ".heff"));
  try {
    f(() => rmSync(cache, { recursive: true, force: true }));
  } finally {
    rmSync(cache, { recursive: true, force: true });
    if (had) renameSync(join(saved, ".heff"), cache);
    rmSync(saved, { recursive: true, force: true });
  }
};

const check = [heff, "check", "app/heff/main.hf"];
if (wanted("check")) coldly((clear) => best("check (cold)", () => { clear(); return timed(check); }));

// the compiler compiling itself: with the cache as it is, then with none
const self = [heff, "compile", "app/heff/main.hf", join(work, "self.c"), "--name", "main", "--backend=c"];
if (wanted("self")) best("self (warm)", () => timed(self));
if (wanted("self-cold")) coldly((clear) => best("self (cold)", () => { clear(); return timed(self); }));
