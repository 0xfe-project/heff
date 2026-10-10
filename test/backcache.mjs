#!/usr/bin/env node
// The backend's cache (.heff/bN/<module>.hb): a program compiled to C
// (as parts) through it is the C compiled without it, byte for byte --
// cold, warm, after an edit, and after a corrupt file; a warm run
// rewrites none of its files; and a definition that is inlined into
// another, or a copy made for it, changed, makes that other be made
// again: the program runs as the edit says.
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { heffBin, root } from "./heff.mjs";

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

const heff = (...args) => spawnSync(heffBin, args, { cwd: dir, encoding: "utf8", maxBuffer: 1e9, timeout: 600000 });
const back = () => join(dir, ".heff", readdirSync(join(dir, ".heff")).find((d) => /^b\d+$/.test(d)) ?? "b?");
const hbs = () => existsSync(back()) ? readdirSync(back()).filter((f) => f.endsWith(".hb")).sort() : [];
// (the program compiled to parts in `out`, through the cache: its files)
const compile = (out) => {
  rmSync(join(dir, out), { recursive: true, force: true });
  const r = heff("compile", "main.hf", out + "/", "--name", "main", "--backend=c");
  if (r.status !== 0) return { error: r.stderr.slice(0, 400) };
  const files = {};
  for (const f of readdirSync(join(dir, out)).sort()) files[f] = readFileSync(join(dir, out, f), "utf8");
  return { files };
};
const same = (x, y) => !x.error && !y.error && JSON.stringify(x.files) === JSON.stringify(y.files);
const cold = (out) => { for (const f of hbs()) rmSync(join(back(), f)); return compile(out); };
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const runs = (out) => {
  const exe = join(dir, out, "prog");
  const b = spawnSync(cc[0], [...cc.slice(1), "-O0", "-w", ...readdirSync(join(dir, out)).filter((f) => f.endsWith(".c")).map((f) => join(dir, out, f)), "-o", exe, "-lm", "-lpthread"], { encoding: "utf8" });
  if (b.status !== 0) return `(not built: ${b.stderr.slice(0, 200)})`;
  return spawnSync(exe, [], { encoding: "utf8" }).stdout.trim();
};

let failed = 0;
const want = (cond, what) => { if (!cond) { failed++; console.log(`FAIL backcache: ${what}`); } };

// cold: compiled, kept
const c1 = compile("c1");
want(!c1.error, `cold compile failed\n${c1.error}`);
want(hbs().includes("a.hb") && hbs().includes("main.hb"), `a cold run keeps a and main\n${hbs().join(" ")}`);

// warm: the same C, no file of the cache written again
const times = Object.fromEntries(hbs().map((f) => [f, statSync(join(back(), f)).mtimeMs]));
const w1 = compile("w1");
want(same(c1, w1), "a warm run makes the C a cold run made");
want(hbs().every((f) => statSync(join(back(), f)).mtimeMs === times[f]), "a warm run leaves the cache's files alone");
want(runs("w1") === "14", `the program runs\n${runs("w1")}`);

// an edit to `step`, inlined into main and into the copy of `loop`: the
// C as a cold run makes it, and the program as the edit says
writeFileSync(join(dir, "a.hf"), a(2));
const e1 = compile("e1");
want(!e1.error && !same(c1, e1), `an edit to an inlined definition changes the C\n${e1.error ?? ""}`);
want(runs("e1") === "18", `an edit to an inlined definition reaches what inlines it\n${runs("e1")}`);
const e2 = cold("e2");
want(same(e1, e2), "after an edit, the C is what a cold run makes");

// back as it was: the C as it was
writeFileSync(join(dir, "a.hf"), a(1));
const b1 = compile("b1");
want(same(c1, b1), "an edit undone: the C as it was");

// a corrupt file of the cache: compiled over, the same C
for (const f of hbs()) writeFileSync(join(back(), f), readFileSync(join(back(), f), "utf8").slice(0, 100));
const h1 = compile("h1");
want(same(c1, h1), "corrupt files of the cache are compiled over");
const h2 = compile("h2");
want(same(c1, h2), "and kept again");

rmSync(dir, { recursive: true, force: true });
console.log(`backcache: ${failed ? "NOT " : ""}the same C through the cache, kept and made again where what it inlines changed`);
process.exit(failed ? 1 : 0);
