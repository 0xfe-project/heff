#!/usr/bin/env node
// Programs compiled for Node.js (`heff node`, lib/io.hg) and run: what they
// write, their exit status, and a long loop of operations done at once.
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
if (build("test/node/echo.hg", "echo", echo)) {
  const r = spawnSync("node", [echo, copied, "two words"], { input: "from stdin", encoding: "utf8", env: { ...process.env, GREETING: "hi" } });
  check("echo: stdout", r.stdout, `${copied}\ntwo words\nhi\ncopied: from stdin\nmissing: not read\n`);
  check("echo: stderr", r.stderr, "to stderr\n");
  check("echo: status", r.status, 3);
  check("echo: the file", readFileSync(copied, "utf8"), "from stdin");
}

// (each operation goes on at once: the stack must not overflow)
const loop = join(work, "loop.hg"), loopJs = join(work, "loop.mjs");
writeFileSync(loop, [
  "import co", "import io", "import prelude", "",
  "loop :: Int -> Int -> Co io Unit",
  "loop n acc = if n == 0 then print (showInt acc) else do",
  "  v <- getEnv \"X\"",
  "  loop (n - 1) (acc + 1)", "",
  "main :: Co io Unit",
  "main = loop 100000 0", "",
].join("\n"));
if (build(loop, "main", loopJs)) {
  const r = spawnSync("node", [loopJs], { encoding: "utf8" });
  check("a long loop", r.stdout + r.stderr, "100000\n");
}
console.log(`node: ${failed ? "FAILED" : "programs run"}`);
process.exit(failed ? 1 : 0);
