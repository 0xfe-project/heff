#!/usr/bin/env node
// Each example is a project (examples/NAME: package.hf, NAME.hf,
// index.html); this writes its app, NAME.js, with
// `heff compile --backend=page`, to .heff/examples/NAME/, beside a copy of
// its index.html, which opens there as it is.  (The apps are built, not
// kept: test/app-run.mjs runs them.)
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { heffBin } from "../test/heff.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
let failed = 0;
for (const name of readdirSync(here).sort()) {
  const dir = join(here, name), src = join(dir, `${name}.hf`), to = join(root, ".heff", "examples", name);
  if (!existsSync(join(dir, "package.hf")) || !existsSync(src)) continue;
  mkdirSync(to, { recursive: true });
  const r = spawnSync(heffBin, ["compile", src, join(to, `${name}.js`), "--name", name, "--backend=page"], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL examples/${name}: ${r.stdout}${r.stderr}`); continue; }
  copyFileSync(join(dir, "index.html"), join(to, "index.html"));
  console.log(`.heff/examples/${name}/index.html`);
}
process.exit(failed ? 1 : 0);
