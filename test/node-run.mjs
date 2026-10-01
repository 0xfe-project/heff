#!/usr/bin/env node
// Programs compiled for Node.js (`heff node`: stdio, proc, fs, time, net) and run: what they
// write, their exit status, loops a million rounds deep, and a long loop
// of operations done at once.
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const work = join(root, "ref", "node-run");
mkdirSync(work, { recursive: true });

let failed = 0;
const check = (what, got, want) => {
  if (got !== want) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
};
const build = (file, name, out) => {
  const r = spawnSync("node", [join(root, "bin", "heff.mjs"), "node", file, name, out], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL heff node ${file}\n${r.stdout}${r.stderr}`); return false; }
  return true;
};

const echo = join(work, "echo.mjs"), copied = join(work, "copied.txt");
if (build("test/node/echo.hf", "echo", echo)) {
  const r = spawnSync("node", [echo, copied, "two words"], { input: "from stdin", encoding: "utf8", env: { ...process.env, GREETING: "hi" } });
  check("echo: stdout", r.stdout, `${copied}\ntwo words\nhi\ncopied: from stdin\nmissing: not read\n`);
  check("echo: stderr", r.stderr, "to stderr\n");
  check("echo: status", r.status, 3);
  check("echo: the file", readFileSync(copied, "utf8"), "from stdin");
}

// time and the network: done later, the rest going on then
const timed = join(work, "timed.mjs");
if (build("test/node/timed.hf", "timed", timed)) {
  const r = spawnSync("node", [timed], { encoding: "utf8" });
  check("sleep and fetch", r.stdout + r.stderr, "fetched\n");
}

// a computation of an effect Node has no handler for is refused, and why
{
  const r = spawnSync("node", [join(root, "bin", "heff.mjs"), "node", "examples/counter/counter.hf", join(work, "counter.mjs")], { cwd: root, encoding: "utf8" });
  check("an app is not a program", r.status !== 0 && r.stderr.includes("counter has the effect dom, which is not handled here"), true);
}

// loops, compiled as loops: a million rounds, and the stack does not overflow
const deep = join(work, "deep.mjs");
if (build("test/node/deep.hf", "deep", deep)) {
  const r = spawnSync("node", [deep], { encoding: "utf8" });
  check("deep loops", r.stdout + r.stderr, "1000000\n1000000\n1000000\n500000\n1000000\n6,4,2\n1,2,3\n");
}

// (each operation goes on at once: the stack must not overflow)
const loop = join(work, "loop.hf"), loopJs = join(work, "loop.mjs");
writeFileSync(loop, [
  "import co", "import prelude", "import proc", "import stdio", "",
  "eff : HFunctor",
  "eff = stdio <+> proc", "",
  "loop : Int -> Int -> Co eff Unit",
  "loop n acc = if n == 0 then lift (inl stdio proc) (print (showInt acc)) else do",
  "  v <- lift (inr stdio proc) (getEnv \"X\")",
  "  loop (n - 1) (acc + 1)", "",
  "main : Co eff Unit",
  "main = loop 100000 0", "",
].join("\n"));
if (build(loop, "main", loopJs)) {
  const r = spawnSync("node", [loopJs], { encoding: "utf8" });
  check("a long loop", r.stdout + r.stderr, "100000\n");
}
console.log(`node: ${failed ? "FAILED" : "programs run"}`);
process.exit(failed ? 1 : 0);
