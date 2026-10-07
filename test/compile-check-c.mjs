#!/usr/bin/env node
// End-to-end test of the Hippogriff -> C compiler (#20), as
// test/compile-check-js.mjs is of the JavaScript one.
//
//   node test/compile-check-c.mjs [N] [SEED]
//
// Compiles the programs of test/programs and N generated ones to C
// (`heff compile FILE OUT.c --backend=c`, no `--name`: each definition
// shown, as test/load-js.mjs shows JavaScript's), builds each with `zig
// cc` (`cc` where there is no zig), runs it for the definitions the
// interpreter has values of (`heff dump-eval`), and compares them.
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { setSeed, genProgram } from "./gen.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, "ref", `compile-check-c-${seedArg}`);
// (the C compiler: zig's, else the system's)
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });

const cases = [];
for (const f of readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort())
  cases.push({ name: f.replace(".hf", ""), src: readFileSync(join(root, "test", "programs", f), "utf8") });
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

// (heff that does not finish is a failure, not a wait)
const heff = (args) => spawnSync("node", [join(root, "output", "heff.mjs"), ...args], { encoding: "utf8", timeout: 120000 });

// (what the C backend does not do yet, its value not compared: integers
// beyond 62 bits, big integers being the next stage)
const notYet = new Set(["prims.big"]);

let agree = 0, differ = 0, compileErrors = 0, compared = 0, timeouts = 0;
for (const c of cases) {
  const file = join(work, `${c.name}.hf`), out = join(work, `${c.name}.c`), bin = join(work, c.name);
  writeFileSync(file, c.src);
  const r0 = heff(["compile", file, out, "--backend=c"]);
  const b = r0.status === 0 ? spawnSync(cc[0], [...cc.slice(1), "-O1", "-w", out, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" }) : r0;
  const r = { status: b.status, stderr: (r0.stderr || "") + (b.stderr || ""), error: b.error };
  const ev = heff(["dump-eval", file]);
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
  const load = spawnSync(bin, wanted, { encoding: "utf8", timeout: 30000 });
  if (load.status !== 0 || load.error) { timeouts++; console.log(`LOAD FAILED ${c.name}: ${(load.stderr || String(load.error)).split("\n").slice(0, 6).join("\n")}`); continue; }
  const got = JSON.parse(load.stdout);
  let ok = true;
  for (let [name, want] of interp) {
    if (notYet.has(`${c.name}.${name}`)) continue;
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
