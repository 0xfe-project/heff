#!/usr/bin/env node
// The `.hg` programs: the library, the examples and the tests elaborate and
// their expectations hold; the module rules reject what they should.
// (The apps are run by test/app-run.mjs.)
import { spawnSync } from "node:child_process";
import { readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const hg = (...args) => spawnSync("node", [join(root, "bin", "hg.mjs"), ...args], { cwd: root, encoding: "utf8" });
const hgIn = (dir) => readdirSync(join(root, dir)).filter((f) => f.endsWith(".hg")).map((f) => join(dir, f));
let failed = 0;

const examples = readdirSync(join(root, "examples"), { withFileTypes: true }).filter((d) => d.isDirectory()).flatMap((d) => hgIn(join("examples", d.name)));
const files = [...hgIn("lib"), ...examples, ...hgIn("test/surface"), "test/modules/app/main.hg"];
const t = hg("test", ...files);
console.log(t.stdout.trim().split("\n").pop());
if (t.status !== 0) { failed++; console.log(t.stdout + t.stderr); }

// what must not elaborate, and the error it must give
const rejected = [
  ["test/modules/transitive.hg", "no such variable not in scope"],
  ["test/modules/clash.hg", "`Bool` is defined by more than one module imported: a, c"],
];
for (const [f, msg] of rejected) {
  const r = hg("check", f);
  if (r.status === 0 || !r.stderr.includes(msg)) { failed++; console.log(`FAIL ${f}: expected an error "${msg}"\n${r.stderr}`); }
}
console.log(`modules: ${rejected.length} rejected as they should${failed ? " (with failures)" : ""}`);

process.exit(failed ? 1 : 0);
