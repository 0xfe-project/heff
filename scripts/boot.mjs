#!/usr/bin/env node
// The seed renewed: the compiler built (scripts/build.mjs) compiles itself
// once more, and where that is itself, byte for byte (what it was
// minified from), it is kept -- minified -- as boot/heff.mjs, the seed
// the next build starts from.  Wanted when the language changes so that
// the old seed cannot compile the new source; otherwise the old seed does.
import { spawnSync } from "node:child_process";
import { copyFileSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(root, "output", "heff.mjs");
const raw = join(root, "output", "heff.raw.mjs");
const again = join(root, "output", "heff-again.mjs");
const run = (args) => spawnSync(process.execPath, args, { cwd: root, stdio: ["ignore", "ignore", "inherit"] });

if (run([join(root, "scripts", "build.mjs")]).status !== 0) process.exit(1);
if (run([out, "node", "compiler/main.hf", "main", again]).status !== 0) process.exit(1);
if (readFileSync(raw, "utf8") !== readFileSync(again, "utf8")) {
  console.error("heff boot: the compiler does not compile itself to itself; the seed is left as it was");
  process.exit(1);
}
copyFileSync(out, join(root, "boot", "heff.mjs"));
console.log("heff boot: boot/heff.mjs is the compiler, compiled by itself");
