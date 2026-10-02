#!/usr/bin/env node
// The cache (`.heff/`, Heff.Cache): a second run thaws what the first
// elaborated (the files are not rewritten), a changed dependency is seen
// through it transitively, and a corrupt cache file is elaborated over.
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(root, "ref", "cache-run");
const heff = (...args) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8" });
let failed = 0;
const fail = (what, extra = "") => { failed++; console.log(`FAIL ${what}\n${extra}`); };

rmSync(dir, { recursive: true, force: true });
mkdirSync(dir, { recursive: true });
writeFileSync(join(dir, "package.json"), '{ "name": "cache-run", "heff": {} }\n');
writeFileSync(join(dir, "c.hf"), "import prelude\n\nn : Int\nn = 3\n");
writeFileSync(join(dir, "b.hf"), "import prelude\nimport c\n\nm : Int\nm = n * 2\n");
writeFileSync(join(dir, "a.hf"), "import prelude\nimport b\n\n-- >>> m + 1\n");
const a = join(dir, "a.hf");
const cache = join(dir, ".heff");
const mtimes = () => Object.fromEntries(readdirSync(cache).map((f) => [f, statSync(join(cache, f)).mtimeMs]));

const r1 = heff("run", a);
if (r1.status !== 0 || !r1.stdout.includes("7")) fail("the first run", r1.stdout + r1.stderr);
const m1 = mtimes();
if (!("b.json" in m1) || !("c.json" in m1) || !("prelude.json" in m1)) fail("the cache written", Object.keys(m1).join(" "));

const r2 = heff("run", a);
if (r2.status !== 0 || r2.stdout !== r1.stdout) fail("the second run differs", r2.stdout + r2.stderr);
const m2 = mtimes();
if (m2["b.json"] !== m1["b.json"] || m2["c.json"] !== m1["c.json"]) fail("a hit rewrote the cache");

// a changed module is seen through the cache, and through what imports it
writeFileSync(join(dir, "c.hf"), "import prelude\n\nn : Int\nn = 4\n");
const r3 = heff("run", a);
if (r3.status !== 0 || !r3.stdout.includes("9")) fail("a change in a dependency", r3.stdout + r3.stderr);

// a corrupt cache file is treated as absent
writeFileSync(join(cache, "b.json"), readFileSync(join(cache, "b.json"), "utf8").slice(0, 20));
const r4 = heff("run", a);
if (r4.status !== 0 || !r4.stdout.includes("9")) fail("a corrupt cache file", r4.stdout + r4.stderr);

// a new name in a direct import is seen (it could be a tag's, a clash):
// the importer re-elaborates
writeFileSync(join(dir, "c.hf"), "import prelude\n\nn : Int\nn = 4\n\nunused : Int\nunused = 99\n");
const m4 = mtimes();
const r7 = heff("run", a);
const m5 = mtimes();
if (r7.status !== 0 || !r7.stdout.includes("9")) fail("a new name in a dependency", r7.stdout + r7.stderr);
if (m5["b.json"] === m4["b.json"]) fail("a new name did not re-elaborate the importer");

// a body no importer mentions may change freely: nothing re-elaborates
writeFileSync(join(dir, "c.hf"), "import prelude\n\nn : Int\nn = 4\n\nunused : Int\nunused = 100\n");
const r8 = heff("run", a);
const m6 = mtimes();
if (r8.status !== 0 || !r8.stdout.includes("9")) fail("an unused body change", r8.stdout + r8.stderr);
if (m6["b.json"] !== m5["b.json"]) fail("an unused body change re-elaborated the importer");

// comments are not part of any definition: the file re-elaborates, its
// importers do not
writeFileSync(join(dir, "c.hf"), "-- a note\nimport prelude\n\nn : Int\nn = 4\n\nunused : Int\nunused = 100\n");
const r9 = heff("run", a);
const m7 = mtimes();
if (r9.status !== 0 || !r9.stdout.includes("9")) fail("a comment-only change", r9.stdout + r9.stderr);
if (m7["c.json"] === m6["c.json"]) fail("a comment-only change was not re-frozen");
if (m7["b.json"] !== m6["b.json"]) fail("a comment-only change re-elaborated the importer");

// an error in a dependency is reported again on every run (never cached)
writeFileSync(join(dir, "c.hf"), "import prelude\n\nn : Int\nn = unbound\n");
const r5 = heff("run", a);
const r6 = heff("run", a);
if (r5.status === 0 || r6.status === 0 || !r6.stderr.includes("unbound")) fail("an error in a dependency, twice", r6.stdout + r6.stderr);

console.log(failed ? "cache: failures" : "cache: thawed on a hit, invalidated by what is mentioned, healed over corruption");
process.exit(failed ? 1 : 0);
