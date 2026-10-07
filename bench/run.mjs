#!/usr/bin/env node
// The benchmarks: each program here compiled for Node.js and run, and the
// compiler compiling itself (its cache warm, then cold), each timed; the
// best of `--runs N` (3) is said, with its peak memory.
//
//   node bench/run.mjs [--runs N] [NAME...]
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readdirSync, renameSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const heff = join(root, "output", "heff.mjs");
const work = join(root, "ref", "bench");
mkdirSync(work, { recursive: true });

const argv = process.argv.slice(2);
let runs = 3;
const only = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--runs") runs = Number(argv[++i]);
  else only.push(argv[i]);
}
const wanted = (name) => only.length === 0 || only.includes(name);

// (a run, timed by the system: its wall-clock seconds and peak resident
// memory, in MB)
const timed = (args, opts = {}) => {
  const r = spawnSync("/usr/bin/time", ["-l", ...args], { cwd: root, encoding: "utf8", maxBuffer: 1 << 26, ...opts });
  const real = Number((r.stderr.match(/([\d.]+) real/) || [])[1]);
  const rss = Number((r.stderr.match(/(\d+)\s+maximum resident set size/) || [])[1]) / (1 << 20);
  return { ok: r.status === 0, real, rss, out: r.stdout, err: r.stderr };
};

const best = (name, once) => {
  let b = null;
  for (let i = 0; i < runs; i++) {
    const r = once();
    if (!r.ok) { console.log(`${name.padEnd(16)} FAILED\n${r.err}`); return; }
    if (b === null || r.real < b.real) b = r;
  }
  console.log(`${name.padEnd(16)} ${b.real.toFixed(2).padStart(7)} s ${b.rss.toFixed(0).padStart(7)} MB`);
};

for (const f of readdirSync(join(root, "bench")).filter((f) => f.endsWith(".hf")).sort()) {
  const name = basename(f, ".hf");
  if (!wanted(name)) continue;
  const out = join(work, name + ".mjs");
  const c = spawnSync("node", [heff, "compile", join("bench", f), out, "--name", "main", "--backend=node"], { cwd: root, encoding: "utf8" });
  if (c.status !== 0) { console.log(`${name.padEnd(16)} did not compile\n${c.stdout}${c.stderr}`); continue; }
  best(name, () => timed(["node", out]));
}

// the compiler compiling itself: with the cache as it is, then with none
const self = ["node", heff, "compile", "src/heff/main.hf", join(work, "self.mjs"), "--name", "main", "--backend=node"];
if (wanted("self")) best("self (warm)", () => timed(self));
if (wanted("self-cold")) {
  const cache = join(root, ".heff");
  const saved = mkdtempSync(join(tmpdir(), "heff-cache-"));
  const had = existsSync(cache);
  if (had) renameSync(cache, join(saved, ".heff"));
  try {
    best("self (cold)", () => { rmSync(cache, { recursive: true, force: true }); return timed(self); });
  } finally {
    rmSync(cache, { recursive: true, force: true });
    if (had) renameSync(join(saved, ".heff"), cache);
    rmSync(saved, { recursive: true, force: true });
  }
}
