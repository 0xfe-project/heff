#!/usr/bin/env node
// The queries through both compilers: `explain` (the elaboration trace,
// every event's kind, place and text) and `type-at` (what the name at an
// offset resolved to, and its type) must say byte for byte what the
// reference says -- the trace and the elaborator's record of names are
// what the language server is made of.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { root, bin, heff as heffP, port as portP, pool } from "./pool.mjs";

if (!existsSync(bin)) { console.log("FAIL query-port: no output/heff-selfcheck.mjs (run test/self-check.mjs first)"); process.exit(1); }

const filesIn = (dirs) => dirs.flatMap((d) =>
  spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" }).stdout.split("\n").filter(Boolean));

// (the reference is handed absolute paths; both say the repo-relative one)
const normalize = (f, s) => s.replaceAll(resolve(root, f), f);

const firstDiff = (a, b) => {
  const as = a.split("\n"), bs = b.split("\n");
  for (let i = 0; i < Math.max(as.length, bs.length); i++)
    if (as[i] !== bs[i]) return `  line ${i + 1}\n  reference: ${(as[i] || "").slice(0, 140)}\n  port:      ${(bs[i] || "").slice(0, 140)}`;
  return "";
};

const compare = async (what, f, args) => {
  const [r, p] = await Promise.all([heffP([args[0], resolve(root, f), ...args.slice(1)]), portP([args[0], f, ...args.slice(1)])]);
  const rerr = normalize(f, r.stderr);
  if (p.status === r.status && p.stdout === r.stdout && p.stderr === rerr) return [];
  return [`FAIL query-port: ${what} (reference exit ${r.status}, port exit ${p.status})\n${firstDiff(r.stdout + "\n--stderr--\n" + rerr, p.stdout + "\n--stderr--\n" + p.stderr)}`];
};

// explain: every file's trace
const explainFiles = filesIn(["lib", "fmt", "test/nbe", "test/surface", "test/errors"]);

// type-at: in each file, the starts of some names (every one of a stride,
// eight at most), a place between them, and an offset that is no number
const offsetsIn = (f) => {
  const src = readFileSync(resolve(root, f), "utf8");
  const starts = [];
  for (const m of src.matchAll(/(?<![A-Za-z0-9_'])[A-Za-z_][A-Za-z0-9_']*/g)) if (!/^--/.test(src.slice(src.lastIndexOf("\n", m.index) + 1, m.index).trimStart())) starts.push(m.index);
  const stride = Math.max(1, Math.floor(starts.length / 8));
  const picked = starts.filter((_, i) => i % stride === 0).slice(0, 8).map((o) => String(o + 1));
  return [...picked, "0", "nope"];
};
const typeAtFiles = filesIn(["lib", "test/surface", "test/nbe"]);
const typeAtCases = typeAtFiles.flatMap((f) => offsetsIn(f).map((o) => ({ f, o })));

const jobs = [
  ...explainFiles.map((f) => () => compare(`explain ${f}`, f, ["explain"])),
  ...typeAtCases.map(({ f, o }) => () => compare(`type-at ${f} ${o}`, f, ["type-at", o])),
];
const results = (await pool(jobs, (j) => j())).flat();
for (const l of results) console.log(l);
console.log(`query port: ${explainFiles.length} traces and ${typeAtCases.length} names ${results.length ? "NOT all " : ""}said as the reference says them`);
process.exit(results.length ? 1 : 0);
