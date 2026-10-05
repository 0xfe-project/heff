#!/usr/bin/env node
// The compiler's cache (.heff/<module>.hc): a second run
// thaws what the first elaborated (the dependency's cache file is not
// rewritten), corruption heals over, a new name in a dependency
// re-elaborates the importer, and a comment-only change does not --
// the invalidation is at the definitions used, not at whole imports.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, writeFileSync, mkdtempSync, mkdirSync, copyFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, basename } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const bin = join(root, "output", "heff.mjs");
if (!existsSync(bin)) { console.log("FAIL cache: no output/heff.mjs (run npm run build first)"); process.exit(1); }

const dir = mkdtempSync(join(tmpdir(), "heff-cache-"));
mkdirSync(join(dir, "lib"), { recursive: true });
const ls = spawnSync("bash", ["-c", "ls lib/*.hf"], { cwd: root, encoding: "utf8" });
for (const f of ls.stdout.split("\n").filter(Boolean)) copyFileSync(join(root, f), join(dir, "lib", basename(f)));

writeFileSync(join(dir, "a.hf"), "import prelude\n\nnine : Int\nnine = 9\n\nunused : Int\nunused = 1\n");
writeFileSync(join(dir, "b.hf"), "import a\nimport prelude\n\nten : Int\nten = nine + 1\n");
writeFileSync(join(dir, "c.hf"), "import b\nimport prelude\n\n-- >>> ten\n-- 10\n");

const heff = (...args) => spawnSync("node", [bin, ...args], { cwd: dir, encoding: "utf8", maxBuffer: 1e9, timeout: 600000 });
const hc = (m) => join(dir, ".heff", m + ".hc");
const mtime = (m) => statSync(hc(m)).mtimeMs;

let failed = 0;
const want = (cond, what) => { if (!cond) { failed++; console.log(`FAIL cache: ${what}`); } };

// cold: elaborated, frozen
const cold = heff("check", "c.hf");
want(cold.status === 0, `cold check failed\n${cold.stderr.slice(0, 200)}`);
want(existsSync(hc("a")) && existsSync(hc("prelude")) && existsSync(hc("b")), "cold run froze a, prelude and b");

// warm: same answer, the dependencies' files untouched (thawed, not
// re-frozen)
const tA = mtime("a"), tP = mtime("prelude"), tB0 = mtime("b");
const warm = heff("check", "c.hf");
want(warm.status === 0 && warm.stdout === cold.stdout && warm.stderr === cold.stderr, "warm check says what cold said");
want(mtime("a") === tA && mtime("prelude") === tP && mtime("b") === tB0, "warm run left the dependencies' cache files alone");

// corruption heals over
writeFileSync(hc("a"), "garbage");
const healed = heff("check", "c.hf");
want(healed.status === 0 && healed.stdout === cold.stdout, "a corrupt cache file is elaborated over");
want(readFileSync(hc("a"), "utf8") !== "garbage", "and frozen again");

// a comment-only change deep down: the importer between still thaws
// (the entry file is always fresh; `b` is the one to watch)
const tB1 = mtime("b");
writeFileSync(join(dir, "a.hf"), "-- a comment\nimport prelude\n\nnine : Int\nnine = 9\n\nunused : Int\nunused = 1\n");
const c1 = heff("test", "c.hf");
want(c1.status === 0 && /1\/1 expectations hold/.test(c1.stdout), "the watch still holds after a comment-only change");
want(mtime("b") === tB1, "a comment-only change in a dependency invalidates no importer");

// a new name in the dependency re-elaborates the importer (its parse
// may read differently)
writeFileSync(join(dir, "a.hf"), "-- a comment\nimport prelude\n\nnine : Int\nnine = 9\n\nunused : Int\nunused = 1\n\nmore : Int\nmore = 2\n");
const c2 = heff("check", "c.hf");
want(c2.status === 0, "check after a new name in the dependency");
want(mtime("b") > tB1, "a new name in a dependency re-elaborates the importer");

// a changed definition the importer uses: the answer changes with it
writeFileSync(join(dir, "a.hf"), "import prelude\n\nnine : Int\nnine = 90\n\nunused : Int\nunused = 1\n");
const c3 = heff("run", "c.hf");
want(c3.status === 0 && /91/.test(c3.stdout), `a changed definition reaches the importer\n${c3.stdout.slice(0, 120)}`);

console.log(`cache: ${failed ? "NOT " : ""}thawed, healed and invalidated at the definitions used`);
process.exit(failed ? 1 : 0);
