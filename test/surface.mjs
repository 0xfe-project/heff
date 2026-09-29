#!/usr/bin/env node
// The `.hf` programs: the library, the examples and the tests elaborate and
// their expectations hold; the module rules reject what they should.
// (The apps are run by test/app-run.mjs.)
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const heff = (...args) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8" });
const hfIn = (dir) => readdirSync(join(root, dir)).filter((f) => f.endsWith(".hf")).map((f) => join(dir, f));
let failed = 0;

const examples = readdirSync(join(root, "examples"), { withFileTypes: true }).filter((d) => d.isDirectory()).flatMap((d) => hfIn(join("examples", d.name)));
const files = [...hfIn("lib"), ...hfIn("fmt"), ...examples, ...hfIn("test/surface"), "test/modules/app/main.hf", "test/modules/qualified.hf"];
const t = heff("test", ...files);
console.log(t.stdout.trim().split("\n").pop());
if (t.status !== 0) { failed++; console.log(t.stdout + t.stderr); }

// what must not elaborate, and the error it must give
const rejected = [
  ["test/modules/transitive.hf", "no such variable not in scope"],
  ["test/modules/clash.hf", "`Bool` is defined by more than one module imported: a, c"],
  ["test/errors/large-elim.hf", "cannot eliminate from sum type into large type"],
  ["test/errors/odd-and-even.hf", "no such tag zero in type OddAndEven.odd"],
  ["test/errors/rec.hf", "variable a is bound in delayed mode"],
  ["test/errors/specialize.hf", "neutrals Bool and Unit are not equal"],
  ["test/modules/qualified-only.hf", "no such variable not in scope"],
  ["test/errors/literals.hf", "one of them must match every other value"],
  ["test/errors/implicit.hf", "test/errors/implicit.hf:33:8\nerror[E0327]: cannot infer the implicit argument `h`: give it, `(h := ...)`"],
  ["test/errors/specialize.hf", "neutrals m.t and Bool are not equal"],
];
for (const [f, msg] of rejected) {
  const r = heff("check", f);
  if (r.status === 0 || !r.stderr.includes(msg)) { failed++; console.log(`FAIL ${f}: expected an error "${msg}"\n${r.stderr}`); }
}
console.log(`modules: ${rejected.length} rejected as they should${failed ? " (with failures)" : ""}`);

// an expectation that does not hold: what differs is shown, cut to it
mkdirSync(join(root, "ref"), { recursive: true });
const wrong = join(root, "ref", "wrong-expectation.hf");
writeFileSync(wrong, `import prelude\n\n-- >>> "${"x".repeat(60)} on click"\n-- "${"x".repeat(60)} onn click"\n`);
const w = heff("test", wrong);
const report = w.stdout.slice(w.stdout.indexOf("  expected"));
if (w.status === 0 || !report.includes("[-onn-]{+on+}") || !report.includes("…") || report.includes("x".repeat(60))) {
  failed++;
  console.log(`FAIL the report of an expectation that does not hold:\n${report}`);
}

process.exit(failed ? 1 : 0);
