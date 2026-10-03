#!/usr/bin/env node
// `check` through both compilers: for every repo file, the interface
// the self-hosted compiler prints and the diagnostics it reports must
// say byte for byte what the reference says (the paths in stderr
// normalized -- the reference is handed absolute ones).  This is the
// message differential: a definition both compilers store as an error
// hole slips through the dump differential, and lands here.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, mkdtempSync, mkdirSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve, basename } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const bin = join(root, "output", "heff-selfcheck.mjs");
const heff = (...args) => spawnSync("node", ["--stack-size=60000", join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8", maxBuffer: 1e9 });
const portIn = (cwd, ...args) => spawnSync("node", [join(root, "test", "run-worker.mjs"), bin, ...args], { cwd, encoding: "utf8", maxBuffer: 1e9 });

if (!existsSync(bin)) { console.log("FAIL check-port: no output/heff-selfcheck.mjs (run test/self-check.mjs first)"); process.exit(1); }

// a nested root's files run from a scratch copy whose lib/ holds the
// library (the port's loader resolves from the working directory)
const mroot = mkdtempSync(join(tmpdir(), "heff-checkport-"));
mkdirSync(join(mroot, "lib"), { recursive: true });
for (const d of ["lib", "test/modules"]) {
  const ls = spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" });
  for (const f of ls.stdout.split("\n").filter(Boolean))
    copyFileSync(join(root, f), join(mroot, d === "lib" ? "lib" : ".", basename(f)));
}
const port = (f) =>
  f.startsWith("test/modules/") ? portIn(mroot, "check", basename(f)) : portIn(root, "check", f);

const dirs = ["lib", "fmt", "compiler", "test/nbe", "test/surface", "test/errors", "test/modules"];
const files = [];
for (const d of dirs) {
  const ls = spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" });
  files.push(...ls.stdout.split("\n").filter(Boolean));
}

// (the paths the two print differ by how they were invoked: both go to
// the port's spelling -- the repo-relative path, or, for a nested
// root's file run from the scratch copy, the bare name)
const canon = (f) => (f.startsWith("test/modules/") ? basename(f) : f);
const normalize = (f, s) => s.replaceAll(resolve(root, f), canon(f));

let failed = 0;
let compared = 0;

for (const f of files) {
  const r = heff("check", resolve(root, f));
  const p = port(f);
  compared++;
  const rerr2 = normalize(f, r.stderr);
  const perr = p.stderr;
  if (p.status !== r.status || p.stdout !== r.stdout || perr !== rerr2) {
    failed++;
    console.log(`FAIL check-port: ${f} (reference exit ${r.status}, port exit ${p.status})`);
    const a = (r.stdout + "\n--stderr--\n" + rerr2).split("\n");
    const b = (p.stdout + "\n--stderr--\n" + perr).split("\n");
    for (let i = 0; i < Math.max(a.length, b.length); i++) {
      if (a[i] !== b[i]) { console.log(`  line ${i + 1}\n  reference: ${(a[i] || "").slice(0, 140)}\n  port:      ${(b[i] || "").slice(0, 140)}`); break; }
    }
  }
}

console.log(`check port: ${compared} files' interfaces and diagnostics ${failed ? "NOT " : ""}printed as the reference does`);
process.exit(failed ? 1 : 0);
