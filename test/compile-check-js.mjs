#!/usr/bin/env node
// End-to-end test of the Hippogriff -> JavaScript compiler.
//
//   node test/compile-check-js.mjs [N] [SEED]
//
// Compiles the golden corpus, the hand-written programs and N generated
// programs to JavaScript, loads each module (in a child process, with a
// timeout) and compares every definition's value with the interpreter's.
// The invariant: whenever the compiled (call-by-value) code computes a
// value, it is exactly the interpreter's.  A value whose computation throws
// (an `abandon` the lazy interpreter never reaches, a loop) is counted apart.
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { setSeed, genProgram } from "./gen.mjs";
import { flat } from "./runtime-inspect.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, "ref", `compile-check-js-${seedArg}`);
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });

const cases = [];
for (const dir of ["golden", "programs"])
  for (const f of readdirSync(join(root, "test", dir)).filter((f) => f.endsWith(".hpgf")).sort())
    cases.push({ name: `${dir}-${f.replace(".hpgf", "")}`, src: readFileSync(join(root, "test", dir, f), "utf8") });
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

const hg = (args) => spawnSync("node", [join(root, "bin", "hg.mjs"), ...args], { encoding: "utf8" });

let agree = 0, differ = 0, compileErrors = 0, compared = 0, failedCBV = 0, timeouts = 0;
for (const c of cases) {
  const file = join(work, `${c.name}.hpgf`), out = join(work, `${c.name}.mjs`);
  writeFileSync(file, c.src);
  const r = hg(["compile-js", file, out]);
  const golden = hg(["golden", "--tools", file]).stdout;
  if (!golden.includes("-- evaluated\n")) continue;
  const interp = new Map();
  // a value's lines after the first are indented, or start with `,`/`]`
  for (const block of golden.split("-- evaluated\n")[1].split("-- messages")[0].split(/\n(?=[^\s,\]])/)) {
    const m = /^(\S+) = ([\s\S]*)$/.exec(block.trim());
    if (m) interp.set(m[1], flat(m[2]));
  }
  if (r.status !== 0 && interp.size === 0) { compileErrors++; continue; }
  const load = spawnSync("node", [join(root, "test", "load-js.mjs"), out, ...interp.keys()], { encoding: "utf8", timeout: 10000 });
  if (load.status !== 0 || load.error) { timeouts++; console.log(`LOAD FAILED ${c.name}: ${(load.stderr || String(load.error)).split("\n").slice(0, 6).join("\n")}`); continue; }
  const got = JSON.parse(load.stdout);
  let ok = true;
  for (let [name, want] of interp) {
    if (want === "#erased" || want === "<abandoned>" || want === "<diverged>") continue;
    // (what is a type is not in the compiled code at all)
    want = erasedOut(want);
    const g = got[name];
    if (g && g.threw) { failedCBV++; continue; }
    // (a function shows nothing but that it is one; a type family, erased,
    // is not there at all)
    if (want === "#fun" && (!g || g.value === "#fun" || g.value === "undefined")) continue;
    want = funsOut(want);
    if (g) g.value = funsOut(g.value);
    compared++;
    if (!g || g.value !== want) { ok = false; console.log(`DIFF ${c.name}.${name}: compiled ${g && g.value}, interpreted ${want}`); }
  }
  if (ok) agree++; else differ++;
}
function funsOut(v) {
  return v.replace(/\w+ := #fun(, )?/g, "").replace(/, \]/g, "]");
}
function erasedOut(v) {
  return v.replace(/\w+ := #erased(, )?/g, "").replace(/, \]/g, "]").replace(/ #erased/g, "");
}
console.log(JSON.stringify({ cases: cases.length, modulesAgree: agree, modulesDiffer: differ, compileErrors, timeouts, valuesCompared: compared, valuesFailedCBV: failedCBV }));
process.exit(differ ? 1 : 0);
