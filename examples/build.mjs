#!/usr/bin/env node
// Each example is a project (examples/NAME: package.json with a "heff"
// field, NAME.hf, index.html);
// this writes its app, NAME.js, with `heff app`, after which index.html opens
// as it is.  (The apps are built, not kept: test/app-run.mjs runs them.)
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
let failed = 0;
for (const name of readdirSync(here).sort()) {
  const dir = join(here, name), src = join(dir, `${name}.hf`), app = join(dir, `${name}.js`);
  if (!existsSync(join(dir, "package.json")) || !existsSync(src)) continue;
  const r = spawnSync("node", [join(root, "bin", "heff.mjs"), "app", src, name, app], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL examples/${name}: ${r.stdout}${r.stderr}`); continue; }
  console.log(`examples/${name}/${name}.js`);
}
process.exit(failed ? 1 : 0);
