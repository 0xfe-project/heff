#!/usr/bin/env node
// End-to-end test of the Hippogriff -> PureScript compiler.
//
//   node test/compile-check.mjs [N] [SEED]
//
// Compiles the golden corpus and N generated programs to PureScript, builds
// them all with purs, loads each module, and compares every definition's
// value with the interpreter's.  The invariant: whenever the compiled
// (call-by-value) module computes a value, it is exactly the interpreter's.
// A module that crashes or diverges while loading is counted separately --
// the interpreter is lazy, so it may well have a value there.
import { spawnSync, execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { setSeed, genProgram } from "./gen.mjs";
import { inspect, flat } from "./runtime-inspect.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, "ref", `compile-check-${seedArg}`);
rmSync(work, { recursive: true, force: true });
mkdirSync(join(work, "src"), { recursive: true });

const cases = [];
for (const f of readdirSync(join(root, "test", "golden")).filter((f) => f.endsWith(".hpgf")).sort()) {
  cases.push({ name: f.replace(".hpgf", ""), src: readFileSync(join(root, "test", "golden", f), "utf8") });
}
// hand-written programs that exercise the translation (modules, effects)
for (const f of readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hpgf")).sort()) {
  cases.push({ name: "prog" + f.replace(".hpgf", "").replace(/[^A-Za-z0-9]/g, ""), src: readFileSync(join(root, "test", "programs", f), "utf8") });
}
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

const hg = (args) => spawnSync("node", [join(root, "bin", "hg.mjs"), ...args], { encoding: "utf8" });
const modName = (name) => "Gen.M" + name.replace(/[^A-Za-z0-9]/g, "");

// 1. compile each case to PureScript; keep those that compile
const compiled = [];
let compileErrors = 0;
for (const c of cases) {
  const file = join(work, `${c.name}.hpgf`);
  writeFileSync(file, c.src);
  const out = join(work, "src", `${c.name}.purs`);
  const r = hg(["compile", file, modName(c.name), out]);
  // compiled as the tools elaborate, so compared with what they evaluate
  const check = hg(["golden", "--tools", file]);
  if (r.status !== 0) {
    rmSync(out, { force: true });
    compileErrors++;
    console.log(`compile error in ${c.name}:\n  ${r.stdout.split("\n").filter((l) => l.startsWith("error ")).join("\n  ")}`);
    continue;
  }
  compiled.push({ ...c, file, manifest: JSON.parse(readFileSync(out + ".json", "utf8")), golden: check.stdout });
}

// 2. build all generated modules with purs
// primitives use the standard library; it is already built in /output
const pkgs = readdirSync(join(root, ".spago", "p")).map((p) => join(root, ".spago", "p", p, "src", "**", "*.purs"));
const purs = spawnSync("npx", ["purs", "compile", join(work, "src", "*.purs"), join(root, "src", "Hg", "Support", "BigInt.purs"), ...pkgs, "-o", join(work, "output")], { encoding: "utf8", cwd: root });
if (purs.status !== 0) {
  console.log("purs rejected generated code:\n" + purs.stderr.split("\n").filter((l) => /Error|src\/|^\s{2}/.test(l)).slice(0, 80).join("\n"));
  process.exit(1);
}

// 3. load each module and compare values
let same = 0, differ = 0, loadFailed = 0, compared = 0, interpFailed = 0;
for (const c of compiled) {
  const interp = new Map();
  if (!c.golden.includes("-- evaluated\n")) { interpFailed++; continue; }
  const ev = c.golden.split("-- evaluated\n")[1].split("-- messages")[0];
  for (const block of ev.split(/\n(?=\S)/)) {
    const m = /^(\S+) = ([\s\S]*)$/.exec(block.trim());
    if (m) interp.set(m[1], flat(m[2]));
  }
  const load = spawnSync("node", [join(root, "test", "load-module.mjs"), join(work, "output", modName(c.name), "index.js"), ...c.manifest.map((m) => m[1])], { encoding: "utf8", timeout: 10000 });
  if (load.status !== 0) { loadFailed++; continue; }
  const mod = JSON.parse(load.stdout);
  let ok = true;
  for (const [hgName, psName] of c.manifest) {
    if (!(psName in mod)) continue;
    const want = interp.get(hgName);
    if (want === undefined || want === "#erased") continue;
    compared++;
    const got = mod[psName];
    if (got !== want) { ok = false; console.log(`DIFF ${c.name}.${hgName}: compiled ${got}, interpreted ${want}`); }
  }
  if (ok) same++; else differ++;
}
console.log(JSON.stringify({ cases: cases.length, compiled: compiled.length, compileErrors, modulesAgree: same, modulesDiffer: differ, loadFailedCBV: loadFailed, interpreterFailed: interpFailed, valuesCompared: compared }));
process.exit(differ || compileErrors ? 1 : 0);
