#!/usr/bin/env node
// The backends held to the interpreter and to one another.
//
//   node test/backends.mjs [N] [SEED]
//
// Values: the programs of test/programs and N generated ones (test/gen.mjs,
// from SEED) compiled by each backend -- JavaScript (`--backend=esm`, its
// definitions shown by test/load-js.mjs) and C (`--backend=c` without
// `--name`: the program shows the definitions it is asked for), built by
// `zig cc` (`cc` where there is no zig) -- and each definition the
// interpreter has a value of (`heff dump-eval`) compared.
//
// Programs: those of test/node, compiled to a program by each backend
// (`--backend=node`, run by Node.js; `--backend=c --name`), run: what they
// write, their exit status, loops a million rounds deep, a long loop of
// operations done at once, time and the network.
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { setSeed, genProgram } from "./gen.mjs";
import { heff, root } from "./heff.mjs";

const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, "ref", `backends-${seedArg}`);
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });

// (the C compiler: zig's, else the system's; HEFF_SANITIZE=1: built with
// AddressSanitizer and UBSan, so that a reference count wrong shows, a use
// after free, where it is)
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const ccFlags = process.env.HEFF_SANITIZE ? ["-O0", "-g", "-fsanitize=address,undefined"] : ["-O1"];
const buildC = (c, bin) =>
  spawnSync(cc[0], [...cc.slice(1), ...ccFlags, "-w", c, "-o", bin, "-lm", "-lpthread"], { encoding: "utf8" });

// Each backend: compiled, its definitions `wanted` shown as JSON,
// { name: { value } | { threw } }; a failure is { error }
const backends = {
  js: (file, base, wanted) => {
    const out = `${base}.mjs`;
    const r = heff(["compile", file, out, "--backend=esm"]);
    if (r.status !== 0) return { compile: r.stderr || String(r.error) };
    return { load: spawnSync(process.execPath, [join(root, "test", "load-js.mjs"), out, ...wanted], { encoding: "utf8", timeout: 30000 }) };
  },
  c: (file, base, wanted) => {
    const r = heff(["compile", file, `${base}.c`, "--backend=c"]);
    if (r.status !== 0) return { compile: r.stderr || String(r.error) };
    const b = buildC(`${base}.c`, base);
    if (b.status !== 0) return { compile: b.stderr || String(b.error) };
    return { load: spawnSync(base, wanted, { encoding: "utf8", timeout: 30000 }) };
  },
};

let failed = 0;
const fail = (msg) => { failed++; console.log(msg); };

// Values
// ---------------------------------------------------------------------

const cases = [];
for (const f of readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort())
  cases.push({ name: f.replace(".hf", ""), src: readFileSync(join(root, "test", "programs", f), "utf8") });
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

const funsOut = (v) => v.replace(/\w+ = #fun(, )?/g, "").replace(/, \}/g, " }");
const erasedOut = (v) => v.replace(/\w+ = #erased(, )?/g, "").replace(/, \}/g, " }").replace(/ #erased/g, "");

let compared = 0;
for (const c of cases) {
  const file = join(work, `${c.name}.hf`);
  writeFileSync(file, c.src);
  const ev = heff(["dump-eval", file]);
  if (ev.status !== 0) { fail(`NOT EVALUATED ${c.name}:\n${ev.stderr}${ev.error || ""}`); continue; }
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
  for (const [be, compile] of Object.entries(backends)) {
    const r = compile(file, join(work, `${c.name}-${be}`), wanted);
    if (r.compile !== undefined) { fail(`NOT COMPILED ${c.name} (${be}):\n${r.compile}`); continue; }
    if (r.load.status !== 0 || r.load.error) { fail(`LOAD FAILED ${c.name} (${be}): ${(r.load.stderr || String(r.load.error)).split("\n").slice(0, 6).join("\n")}`); continue; }
    const got = JSON.parse(r.load.stdout);
    for (let [name, want] of interp) {
      if (want === "#erased" || want === "<diverged>") continue;
      const g = got[name];
      const at = `${c.name}.${name} (${be})`;
      // (all are call-by-value: what abandons in one abandons in the others)
      if (want === "<abandoned>") {
        compared++;
        if (!(g && g.threw && g.threw.includes("heff:abandon"))) fail(`DIFF ${at}: compiled ${g && (g.value ?? g.threw)}, interpreted <abandoned>`);
        continue;
      }
      if (g && g.threw) { fail(`DIFF ${at}: compiled threw ${g.threw}, interpreted ${want}`); continue; }
      // (what is a type is not in the compiled code at all; a function shows
      // nothing but that it is one; a type family, erased, is not there)
      want = erasedOut(want);
      if (want === "#fun" && (!g || g.value === "#fun" || g.value === "undefined")) continue;
      want = funsOut(want);
      compared++;
      if (!g || funsOut(g.value) !== want) fail(`DIFF ${at}: compiled ${g && g.value}, interpreted ${want}`);
    }
  }
}
console.log(`backends: ${cases.length} programs, ${compared} values compared`);

// Programs
// ---------------------------------------------------------------------

// (a program, compiled by each backend: how each is run)
const programs = {
  js: (file, name) => {
    const out = join(work, `${name}.mjs`);
    const r = heff(["compile", file, out, "--name", name, "--backend=node"], { cwd: root });
    return r.status !== 0 ? { error: r.stderr } : { cmd: process.execPath, pre: [out] };
  },
  c: (file, name) => {
    const c = join(work, `${name}.c`), bin = join(work, `${name}-c`);
    const r = heff(["compile", file, c, "--name", name, "--backend=c"], { cwd: root });
    if (r.status !== 0) return { error: r.stderr };
    const b = buildC(c, bin);
    return b.status !== 0 ? { error: b.stderr } : { cmd: bin, pre: [] };
  },
};

// (the program `name` of `file` run by each backend with `args`, what it
// wrote and its status held to `want`)
const ran = (what, file, name, args, want, opts = {}) => {
  for (const [be, build] of Object.entries(programs)) {
    const p = build(file, name);
    if (p.error !== undefined) { fail(`FAIL ${what} (${be}): not compiled\n${p.error}`); continue; }
    const r = spawnSync(p.cmd, [...p.pre, ...args], { encoding: "utf8", timeout: 60000, ...opts });
    const got = { out: r.stdout, err: r.stderr, status: r.status };
    for (const k of Object.keys(want))
      if (got[k] !== want[k]) fail(`FAIL ${what} (${be}): ${k} ${JSON.stringify(got[k])}, wanted ${JSON.stringify(want[k])}`);
  }
};

// (each backend's world: stdin, the environment, arguments, a file)
{
  const copied = join(work, "copied.txt");
  ran("echo", "test/node/echo.hf", "main", [copied, "two words"],
    { out: `${copied}\ntwo words\nhi\ncopied: from stdin\nmissing: not read\n`, err: "to stderr\n", status: 3 },
    { cwd: root, input: "from stdin", env: { ...process.env, GREETING: "hi" } });
  if (readFileSync(copied, "utf8") !== "from stdin") fail("FAIL echo: the file");
}

// (`heff run`: compiled for Node.js, then run in heff's place, on its streams)
{
  const r = heff(["run", "test/node/echo.hf", join(work, "run.txt"), "x"], { cwd: root, input: "in", env: { ...process.env, GREETING: "hi" } });
  if (r.status !== 3 || r.stderr !== "to stderr\n") fail(`FAIL heff run: status ${r.status}, stderr ${JSON.stringify(r.stderr)}`);
}

// time and the network: done later, the rest going on then
ran("sleep and fetch", "test/node/timed.hf", "timed", [], { out: "fetched\n", status: 0 });

// a computation of an effect the backend has no provider for is refused,
// and why
for (const be of ["node", "c"]) {
  const r = heff(["compile", "examples/counter/counter.hf", join(work, `counter.${be}`), "--name", "counter", `--backend=${be}`], { cwd: root });
  if (!(r.status !== 0 && r.stderr.includes("counter has the effect std.dom.dom, which is not provided here")))
    fail(`FAIL an app is not a program (${be}): ${r.status} ${r.stderr}`);
}

// one world handled at home (a counter: state, peeled off in the
// language), the rest handed to the backend's providers: the evidence as
// instance implicits, `main` asks for it, its body finds it, `run` gives it
ran("evidence found in scope", "test/node/evidence.hf", "run", ["one", "two"], { out: "args: one two\nbye\n", status: 0 });
ran("state peeled in-language", "test/node/counted.hf", "main", [], { out: "0: one\n1: two\n2: three\ncounted 3\n", status: 0 });

// loops, compiled as loops: a million rounds, and the stack does not overflow
ran("deep loops", "test/node/deep.hf", "deep", [], { out: "1000000\n1000000\n1000000\n500000\n1000000\n6,4,2\n1,2,3\n", status: 0 });

// (each operation goes on at once: the stack must not overflow)
const loop = join(work, "loop.hf");
writeFileSync(loop, [
  "import co", "import prelude", "import std.process", "import std.io", "",
  "eff : HFunctor",
  "eff = stdio <+> process", "",
  "loop : {{io : stdio <: eff}} -> {{sys : process <: eff}} -> Int -> Int -> Co eff Unit",
  "loop n acc = if n == 0 then print (showInt acc) else do",
  "  v <- getEnv \"X\"",
  "  loop (n - 1) (acc + 1)", "",
  "main : Co eff Unit",
  "main = loop (io := inl stdio process) (sys := inr stdio process) 100000 0", "",
].join("\n"));
ran("a long loop", loop, "main", [], { out: "100000\n", status: 0 });

// a loop of operations a handler of the language handles: each step's
// rest called last, a million times, in a small heap -- the stack let go
// of every so often (a bounce), not as deep as the loop is long
ran("a long loop, handled in the language", "bench/state.hf", "main", [], { out: "1000000\n", status: 0 },
  { env: { ...process.env, HEFF_HEAP_MB: "256" } });

console.log(`backends: ${failed ? `${failed} FAILED` : "all agree"}`);
process.exit(failed ? 1 : 0);
