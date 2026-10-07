#!/usr/bin/env node
// The seed renewed: heff (built by scripts/build.mjs) compiles itself to
// JavaScript, output/heff.raw.mjs; that compiles itself once more, and
// where that is itself, byte for byte, it is kept -- compressed -- as
// boot/heff.mjs.gz, the seed the next build starts from.  Wanted when the
// language changes so that the old seed cannot compile the new source;
// otherwise the old seed does.
import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const heff = join(root, "output", process.platform === "win32" ? "heff.exe" : "heff");
const raw = join(root, "output", "heff.raw.mjs");
const again = join(root, "output", "heff-again.mjs");
const run = (cmd, args) => spawnSync(cmd, args, { cwd: root, stdio: ["ignore", "ignore", "inherit"] });
const self = ["compile", "app/heff/main.hf", null, "--name", "main", "--backend=node"];

if (run(process.execPath, [join(root, "scripts", "build.mjs")]).status !== 0) process.exit(1);
if (run(heff, self.map((a) => a ?? raw)).status !== 0) process.exit(1);
if (run(process.execPath, [raw, ...self.map((a) => a ?? again)]).status !== 0) process.exit(1);
if (readFileSync(raw, "utf8") !== readFileSync(again, "utf8")) {
  console.error("heff boot: the compiler in JavaScript does not compile itself to itself; the seed is left as it was");
  process.exit(1);
}
writeFileSync(join(root, "boot", "heff.mjs.gz"), gzipSync(readFileSync(raw), { level: 9 }));
console.log("heff boot: boot/heff.mjs.gz is the compiler, compiled by itself");
