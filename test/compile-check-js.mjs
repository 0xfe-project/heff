#!/usr/bin/env node
// End-to-end test of the Hippogriff -> JavaScript compiler.
//
//   node test/compile-check-js.mjs [N] [SEED]
//
// Compiles the programs of test/programs and N generated ones to JavaScript,
// loads each module (in a child process, with a timeout) and compares every
// definition's value with the interpreter's (`heff eval`).  Both are call-by-value, so they agree
// exactly: the same value, or both abandon.  (What the interpreter finds
// to diverge is not read.)
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { setSeed, genProgram } from "./gen.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, "ref", `compile-check-js-${seedArg}`);
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });

const cases = [];
for (const f of readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort())
  cases.push({ name: f.replace(".hf", ""), src: readFileSync(join(root, "test", "programs", f), "utf8") });
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

// (a generated program needs no depth: one that goes deep diverges, and is
// said to sooner; and heff that does not finish is a failure, not a wait)
const heff = (args, depth) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { encoding: "utf8", timeout: 120000, env: depth ? { ...process.env, HEFF_DEPTH: String(depth) } : process.env });

let agree = 0, differ = 0, compileErrors = 0, compared = 0, timeouts = 0;
for (const c of cases) {
  const file = join(work, `${c.name}.hf`), out = join(work, `${c.name}.mjs`);
  writeFileSync(file, c.src);
  const r = heff(["compile-js", file, out]);
  const ev = heff(["eval", file], c.name.startsWith("gen") ? 100000 : 0);
  if (r.status !== 0 || ev.status !== 0) { compileErrors++; console.log(`NOT COMPILED ${c.name}:\n${r.stderr}${ev.stderr}${r.error || ""}${ev.error || ""}`); continue; }
  // `name = value`, a value going on over the lines that do not start so;
  // an operator's name without its parentheses, as the module exports it
  const interp = new Map();
  for (const block of ev.stdout.split(/\n(?=\S+ = )/)) {
    const m = /^(\S+) = ([\s\S]*)$/.exec(block.trim());
    if (m) interp.set(m[1].replace(/^\((.*)\)$/, "$1"), m[2].replace(/\s*\n\s*/g, " "));
  }
  // (what the interpreter found to diverge is not read: compiled, with its
  // loops as loops, it may run for ever)
  const wanted = [...interp.keys()].filter((n) => interp.get(n) !== "<diverged>");
  const load = spawnSync("node", [join(root, "test", "load-js.mjs"), out, ...wanted], { encoding: "utf8", timeout: 10000 });
  if (load.status !== 0 || load.error) { timeouts++; console.log(`LOAD FAILED ${c.name}: ${(load.stderr || String(load.error)).split("\n").slice(0, 6).join("\n")}`); continue; }
  const got = JSON.parse(load.stdout);
  let ok = true;
  for (let [name, want] of interp) {
    if (want === "#erased" || want === "<diverged>") continue;
    const g = got[name];
    // (both are call-by-value: what abandons in one abandons in the other)
    if (want === "<abandoned>") {
      compared++;
      if (!(g && g.threw && g.threw.includes("heff:abandon"))) { ok = false; console.log(`DIFF ${c.name}.${name}: compiled ${g && (g.value ?? g.threw)}, interpreted <abandoned>`); }
      continue;
    }
    if (g && g.threw) { ok = false; console.log(`DIFF ${c.name}.${name}: compiled threw ${g.threw}, interpreted ${want}`); continue; }
    // (what is a type is not in the compiled code at all)
    want = erasedOut(want);
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
  return v.replace(/\w+ = #fun(, )?/g, "").replace(/, \}/g, " }");
}
function erasedOut(v) {
  return v.replace(/\w+ = #erased(, )?/g, "").replace(/, \}/g, " }").replace(/ #erased/g, "");
}
console.log(JSON.stringify({ cases: cases.length, modulesAgree: agree, modulesDiffer: differ, compileErrors, timeouts, valuesCompared: compared }));
process.exit(differ || compileErrors || timeouts ? 1 : 0);
