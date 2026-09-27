#!/usr/bin/env node
// Each example is a project (examples/NAME: hg.json, NAME.hg, index.html);
// this writes its app, NAME.js, with `hg app`, so that index.html opens as
// it is.  `--check`: write nothing, and fail if an app is not what `hg app`
// makes of its source now.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, readdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const check = process.argv.includes("--check");
const tmp = mkdtempSync(join(tmpdir(), "heff-examples-"));
let failed = 0;
for (const name of readdirSync(here).sort()) {
  const dir = join(here, name), src = join(dir, `${name}.hg`), app = join(dir, `${name}.js`);
  if (!existsSync(join(dir, "hg.json")) || !existsSync(src)) continue;
  const out = check ? join(tmp, `${name}.js`) : app;
  const r = spawnSync("node", [join(root, "bin", "hg.mjs"), "app", src, name, out], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL examples/${name}: ${r.stdout}${r.stderr}`); continue; }
  if (check && (!existsSync(app) || readFileSync(app, "utf8") !== readFileSync(out, "utf8"))) {
    failed++;
    console.log(`FAIL examples/${name}/${name}.js is not what examples/${name}/${name}.hg compiles to: npm run examples`);
  } else if (!check) console.log(`examples/${name}/${name}.js`);
}
rmSync(tmp, { recursive: true, force: true });
if (check) console.log(`examples: ${failed ? "FAILED" : "their apps are up to date"}`);
process.exit(failed ? 1 : 0);
