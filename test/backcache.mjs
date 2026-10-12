#!/usr/bin/env node
// The backend's cache (.heff/bN/<module>.hb): a program compiled to C
// (as parts), or to JavaScript, through it is the program compiled
// without it, byte for byte -- cold, warm, after an edit, and after a
// corrupt file; a warm run rewrites none of its files, whichever backend
// ran before; a definition that is inlined into another, or a copy made
// for it, changed, makes that other be made again: the program runs as
// the edit says; and a global newly named as a definition's local is,
// not mentioned by it, leaves its JavaScript as a cold run makes it.
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { run } from "./pool.mjs";

if (!existsSync(heffBin)) { console.log(`FAIL backcache: no ${heffBin} (run node scripts/build.mjs first)`); process.exit(1); }

const dir = mkdtempSync(join(tmpdir(), "heff-backcache-"));
mkdirSync(join(dir, "lib"), { recursive: true });
const ls = spawnSync("bash", ["-c", "ls lib/*.hf"], { cwd: root, encoding: "utf8" });
for (const f of ls.stdout.split("\n").filter(Boolean)) copyFileSync(join(root, f), join(dir, "lib", basename(f)));

// `step`, small: inlined where it is called; `loop`, calling itself: a
// copy of it made for `step`, `step` inlined in the copy
const a = (n) => `step : Int -> Int
step x = x + ${n}

loop : (Int -> Int) -> Int -> Int -> Int
loop f n acc = if n <= 0 then acc else loop f (n - 1) (f acc)
`;
writeFileSync(join(dir, "a.hf"), a(1));
writeFileSync(join(dir, "main.hf"), `import a
import co
import std.io

program : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
program = print (showInt (loop step 3 (step 10)))

main : Co stdio Unit
main = program (io := \\p -> p)
`);

const heff = (...args) => run(heffBin, args, { cwd: dir });
const back = () => join(dir, ".heff", readdirSync(join(dir, ".heff")).find((d) => /^b\d+$/.test(d)) ?? "b?");
const hbs = () => existsSync(back()) ? readdirSync(back()).filter((f) => f.endsWith(".hb")).sort() : [];
// (the program compiled to parts in `out`, through the cache: its files)
const compile = async (out) => {
  rmSync(join(dir, out), { recursive: true, force: true });
  const r = await heff("compile", "main.hf", out + "/", "--name", "main", "--backend=c");
  if (r.status !== 0) return { error: r.stderr.slice(0, 400) };
  const files = {};
  for (const f of readdirSync(join(dir, out)).sort()) files[f] = readFileSync(join(dir, out, f), "utf8");
  return { files };
};
const same = (x, y) => !x.error && !y.error && JSON.stringify(x.files) === JSON.stringify(y.files);
const cold = async (out) => { for (const f of hbs()) rmSync(join(back(), f)); return compile(out); };
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const runs = async (out) => {
  const exe = join(dir, out, "prog");
  const b = await run(cc[0], [...cc.slice(1), "-O0", "-w", ...readdirSync(join(dir, out)).filter((f) => f.endsWith(".c")).map((f) => join(dir, out, f)), "-o", exe, "-lm", "-lpthread"]);
  if (b.status !== 0) return `(not built: ${b.stderr.slice(0, 200)})`;
  return (await run(exe, [])).stdout.trim();
};

let failed = 0;
const want = (cond, what) => { if (!cond) { failed++; console.log(`FAIL backcache: ${what}`); } };

// cold: compiled, kept
const c1 = await compile("c1");
want(!c1.error, `cold compile failed\n${c1.error}`);
want(hbs().includes("a.hb") && hbs().includes("main.hb"), `a cold run keeps a and main\n${hbs().join(" ")}`);

// warm: the same C, no file of the cache written again
const times = Object.fromEntries(hbs().map((f) => [f, statSync(join(back(), f)).mtimeMs]));
const w1 = await compile("w1");
want(same(c1, w1), "a warm run makes the C a cold run made");
want(hbs().every((f) => statSync(join(back(), f)).mtimeMs === times[f]), "a warm run leaves the cache's files alone");
want((await runs("w1")) === "14", `the program runs\n${await runs("w1")}`);

// an edit to `step`, inlined into main and into the copy of `loop`: the
// C as a cold run makes it, and the program as the edit says
writeFileSync(join(dir, "a.hf"), a(2));
const e1 = await compile("e1");
want(!e1.error && !same(c1, e1), `an edit to an inlined definition changes the C\n${e1.error ?? ""}`);
want((await runs("e1")) === "18", `an edit to an inlined definition reaches what inlines it\n${await runs("e1")}`);
const e2 = await cold("e2");
want(same(e1, e2), "after an edit, the C is what a cold run makes");

// back as it was: the C as it was
writeFileSync(join(dir, "a.hf"), a(1));
const b1 = await compile("b1");
want(same(c1, b1), "an edit undone: the C as it was");

// a corrupt file of the cache: compiled over, the same C
for (const f of hbs()) writeFileSync(join(back(), f), readFileSync(join(back(), f), "utf8").slice(0, 100));
const h1 = await compile("h1");
want(same(c1, h1), "corrupt files of the cache are compiled over");
const h2 = await compile("h2");
want(same(c1, h2), "and kept again");

// JavaScript, through the same files
const compileJs = async (out) => {
  const r = await heff("compile", "main.hf", out, "--name", "main", "--backend=node");
  if (r.status !== 0) return { error: r.stderr.slice(0, 400) };
  return { files: { js: readFileSync(join(dir, out), "utf8") } };
};
const coldJs = async (out) => { for (const f of hbs()) rmSync(join(back(), f)); return compileJs(out); };
const runsJs = async (out) => (await run(process.execPath, [join(dir, out)])).stdout.trim();
const j1 = await coldJs("j1.mjs");
want(!j1.error, `cold JavaScript compile failed\n${j1.error}`);
want((await runsJs("j1.mjs")) === "14", `the JavaScript runs\n${await runsJs("j1.mjs")}`);
const c2 = await compile("c2");
want(same(c1, c2), "C after JavaScript: the C a cold run made");
const times2 = Object.fromEntries(hbs().map((f) => [f, statSync(join(back(), f)).mtimeMs]));
const j2 = await compileJs("j2.mjs");
want(same(j1, j2), "a warm run makes the JavaScript a cold run made");
const c3 = await compile("c3");
want(same(c1, c3), "and the C");
want(hbs().every((f) => statSync(join(back(), f)).mtimeMs === times2[f]), "warm runs of both leave the cache's files alone");
writeFileSync(join(dir, "a.hf"), a(2));
const je = await compileJs("je.mjs");
want(!je.error && !same(j1, je), `an edit to an inlined definition changes the JavaScript\n${je.error ?? ""}`);
want((await runsJs("je.mjs")) === "18", `an edit reaches the JavaScript\n${await runsJs("je.mjs")}`);
want(same(je, await coldJs("je2.mjs")), "after an edit, the JavaScript is what a cold run makes");
writeFileSync(join(dir, "a.hf"), a(1));
for (const f of hbs()) writeFileSync(join(back(), f), readFileSync(join(back(), f), "utf8").slice(0, 100));
want(same(j1, await compileJs("jh.mjs")), "corrupt files of the cache are compiled over, the JavaScript too");

// a global named as `count`'s local is, newly: `count` unchanged, its
// local as a cold run names it
const counting = (withX) => `import co
import std.io
${withX ? "\nx : Int\nx = 5\n" : ""}
count : Int -> Int -> Int
count x acc = if x <= 0 then acc else count (x - 1) (acc + x * x)

program : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
program = print (showInt (count 3 ${withX ? "x" : "0"}))

main : Co stdio Unit
main = program (io := \\p -> p)
`;
writeFileSync(join(dir, "main.hf"), counting(false));
const k1 = await compileJs("k1.mjs");
want(!k1.error && (await runsJs("k1.mjs")) === "14", `count runs\n${k1.error ?? await runsJs("k1.mjs")}`);
writeFileSync(join(dir, "main.hf"), counting(true));
const k2 = await compileJs("k2.mjs");
want(!k2.error && (await runsJs("k2.mjs")) === "19", `count runs past a global x\n${k2.error ?? await runsJs("k2.mjs")}`);
want(same(k2, await coldJs("k3.mjs")), "a global newly met: the JavaScript a cold run makes");

rmSync(dir, { recursive: true, force: true });
console.log(`backcache: ${failed ? "NOT " : ""}the same C and JavaScript through the cache, kept and made again where what it inlines changed`);
process.exit(failed ? 1 : 0);
