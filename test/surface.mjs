#!/usr/bin/env node
// The `.hg` programs: the library, the examples and the tests elaborate and
// their expectations hold; the module rules reject what they should.
// (The apps are run by test/app-run.mjs.)
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const heff = (...args) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8" });
const hgIn = (dir) => readdirSync(join(root, dir)).filter((f) => f.endsWith(".hg")).map((f) => join(dir, f));
let failed = 0;

const examples = readdirSync(join(root, "examples"), { withFileTypes: true }).filter((d) => d.isDirectory()).flatMap((d) => hgIn(join("examples", d.name)));
const files = [...hgIn("lib"), ...examples, ...hgIn("test/surface"), "test/modules/app/main.hg"];
const t = heff("test", ...files);
console.log(t.stdout.trim().split("\n").pop());
if (t.status !== 0) { failed++; console.log(t.stdout + t.stderr); }

// what must not elaborate, and the error it must give
const rejected = [
  ["test/modules/transitive.hg", "no such variable not in scope"],
  ["test/modules/clash.hg", "`Bool` is defined by more than one module imported: a, c"],
];
for (const [f, msg] of rejected) {
  const r = heff("check", f);
  if (r.status === 0 || !r.stderr.includes(msg)) { failed++; console.log(`FAIL ${f}: expected an error "${msg}"\n${r.stderr}`); }
}
console.log(`modules: ${rejected.length} rejected as they should${failed ? " (with failures)" : ""}`);

// an expectation that does not hold: what differs is shown, cut to it
mkdirSync(join(root, "ref"), { recursive: true });
const wrong = join(root, "ref", "wrong-expectation.hg");
writeFileSync(wrong, `import prelude\n\n-- >>> "${"x".repeat(60)} on click"\n-- "${"x".repeat(60)} onn click"\n`);
const w = heff("test", wrong);
const report = w.stdout.slice(w.stdout.indexOf("  expected"));
if (w.status === 0 || !report.includes("[-onn-]{+on+}") || !report.includes("…") || report.includes("x".repeat(60))) {
  failed++;
  console.log(`FAIL the report of an expectation that does not hold:\n${report}`);
}

process.exit(failed ? 1 : 0);
