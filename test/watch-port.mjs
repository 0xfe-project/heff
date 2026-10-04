#!/usr/bin/env node
// The watches through both compilers: for every repo file with a watch
// (`-- >>> e`), the self-hosted compiler's `run` -- elaborate the watch,
// evaluate it by value, print it -- must say byte for byte what the
// reference says, and its `test` must count the same expectations.
// This holds the whole chain alike: the loader, the watch elaboration,
// the interpreter, the value printing, and the expectation check
// (which re-reads the expectation at the watch's printed type).
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, mkdtempSync, mkdirSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve, basename } from "node:path";
import { root, bin, heff as heffP, port as portP, pool } from "./pool.mjs";

const heff = (...args) => heffP(args);
const portIn = (cwd, ...args) => portP(args, { cwd });

// the port's loader resolves from the working directory (the root walk
// stays with the host): a file of a nested root, test/modules, runs
// from a scratch copy of that root whose lib/ holds the library
const mroot = mkdtempSync(join(tmpdir(), "heff-watchport-"));
mkdirSync(join(mroot, "lib"), { recursive: true });
for (const d of ["lib", "test/modules"]) {
  const ls = spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" });
  for (const f of ls.stdout.split("\n").filter(Boolean))
    copyFileSync(join(root, f), join(mroot, d === "lib" ? "lib" : ".", basename(f)));
}
const port = (mode, f) =>
  f.startsWith("test/modules/") ? portIn(mroot, mode, basename(f)) : portIn(root, mode, f);

if (!existsSync(bin)) { console.log("FAIL watch-port: no output/heff-selfcheck.mjs (run test/self-check.mjs first)"); process.exit(1); }

// every corpus file with a watch (the abandoning ones too: the port
// threads abandon as a value and prints `<abandoned>` like the
// reference)
const dirs = ["lib", "fmt", "compiler", "test/nbe", "test/surface", "test/modules"];
// (whose watches interpret the elaborator itself: minutes in the port's
// interpreter -- its values agree, but they are no printer differential)
const slow = ["compiler/toplevel.hf", "compiler/driver.hf"];
const files = [];
for (const d of dirs) {
  const ls = spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" });
  for (const f of ls.stdout.split("\n").filter(Boolean)) {
    if (slow.includes(f)) continue;
    const src = readFileSync(join(root, f), "utf8");
    if (/^-- >>> /m.test(src)) files.push(f);
  }
}

// (each file through both compilers, as many files at once as there are
// cores; what is said is said in the files' order)
const one = async (f) => {
  const [r, p] = await Promise.all([heff("run", resolve(root, f)), port("run", f)]);
  // a fixture the reference itself refuses is not this differential's
  if (r.status !== 0) return { compared: false, watches: 0, fails: [] };
  const watches = (r.stdout.match(/^>>> /gm) || []).length;
  if (p.status !== 0 || p.stdout !== r.stdout) {
    const a = r.stdout.split("\n"), b = p.stdout.split("\n");
    for (let i = 0; i < Math.max(a.length, b.length); i++) {
      if (a[i] !== b[i]) return { compared: true, watches, fails: [`FAIL watch-port: ${f} line ${i + 1}\n  reference: ${(a[i] || "").slice(0, 140)}\n  port:      ${(b[i] || "").slice(0, 140)}\n${p.stderr.split("\n").slice(0, 3).join("\n")}`] };
    }
    return { compared: true, watches, fails: [`FAIL watch-port: ${f} (port exit ${p.status})\n${p.stderr.split("\n").slice(0, 3).join("\n")}`] };
  }
  // and `test` agrees on the counts
  const [rt, pt] = await Promise.all([heff("test", resolve(root, f)), port("test", f)]);
  const count = (s) => (s.match(/(\d+)\/(\d+) expectations hold/) || []).slice(1).join("/");
  if (count(pt.stdout) !== count(rt.stdout) || pt.status !== rt.status)
    return { compared: true, watches, fails: [`FAIL watch-port: ${f} test counts differ (reference ${count(rt.stdout)} exit ${rt.status}, port ${count(pt.stdout)} exit ${pt.status})`] };
  return { compared: true, watches, fails: [] };
};

const results = await pool(files, one);
let failed = 0, compared = 0, watchesSeen = 0;
for (const r of results) {
  if (r.compared) compared++;
  watchesSeen += r.watches;
  for (const m of r.fails) { failed++; console.log(m); }
}

console.log(`watch port: ${watchesSeen} watches of ${compared} files ${failed ? "NOT " : ""}printed and checked as the reference does`);
process.exit(failed ? 1 : 0);
