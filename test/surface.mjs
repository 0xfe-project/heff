#!/usr/bin/env node
// The `.hf` programs: the library, the examples and the tests elaborate and
// their expectations hold; one that does not is reported.
// (The apps are run by test/app-run.mjs; what `heff check` says of each
// file of the library and the tests -- among them the errors of
// test/errors/ and the module rules' of test/modules/ -- is held by
// test/golden.mjs, byte for byte.)
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { availableParallelism, tmpdir } from "node:os";
import { join } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { pool, run } from "./pool.mjs";

const heff = (...args) => spawnSync(heffBin, [...args], { cwd: root, encoding: "utf8" });
const hfIn = (dir) => readdirSync(join(root, dir)).filter((f) => f.endsWith(".hf")).map((f) => join(dir, f));
let failed = 0;

const examples = readdirSync(join(root, "examples"), { withFileTypes: true }).filter((d) => d.isDirectory()).flatMap((d) => hfIn(join("examples", d.name)));
const files = [...hfIn("lib"), ...hfIn("lib/std"), ...hfIn("src/heff"), ...hfIn("src/heff/fmt"), "app/heff/main.hf", ...examples, ...hfIn("test/surface"), ...hfIn("test/check"), "test/modules/app/main.hf", "test/modules/qualified.hf", "test/modules/struct-use.hf", "test/modules/value-use.hf"];
// (the files dealt out to as many `heff test` runs as there are cores, all
// at once; what they say added up)
const n = Math.min(availableParallelism(), files.length);
const runs = await pool(Array.from({ length: n }, (_, i) => files.filter((_, k) => k % n === i)), (some) => run(heffBin, ["test", ...some]));
const held = [0, 0, 0, 0];
for (const t of runs) {
  const m = /(\d+)\/(\d+) expectations hold(?:, (\d+)\/(\d+) tests hold)?\n$/.exec(t.stdout) ?? [];
  for (let i = 0; i < 4; i++) held[i] += Number(m[i + 1] ?? 0);
  if (t.status !== 0 || !m.length) { failed++; console.log(t.stdout + t.stderr); }
}
console.log(`${held[0]}/${held[1]} expectations hold, ${held[2]}/${held[3]} tests hold`);

// an expectation that does not hold: what differs is shown, cut to it
mkdirSync(join(root, ".heff", "ref"), { recursive: true });
const wrong = join(root, ".heff", "ref", "wrong-expectation.hf");
writeFileSync(wrong, `-- >>> "${"x".repeat(60)} on click"\n-- "${"x".repeat(60)} onn click"\n`);
const w = heff("test", wrong);
const report = w.stdout.slice(w.stdout.indexOf("  expected"));
if (w.status === 0 || !report.includes("[-onn-]{+on+}") || !report.includes("…") || report.includes("x".repeat(60))) {
  failed++;
  console.log(`FAIL the report of an expectation that does not hold:\n${report}`);
}

// the prelude, imported by every module without being written -- but by
// those it is made of, found from the modules it imports: a library whose
// prelude imports `core`, which then cannot import the prelude (in a
// directory of its own: its cache is not the repository's)
{
  const dir = mkdtempSync(join(tmpdir(), "heff-implicit-"));
  const lib = join(dir, "lib");
  mkdirSync(lib, { recursive: true });
  writeFileSync(join(lib, "builtin.hf"), readFileSync(join(root, "lib", "builtin.hf"), "utf8"));
  writeFileSync(join(lib, "core.hf"), "yes : Bool\nyes = True\n");
  writeFileSync(join(lib, "prelude.hf"), "import core\n\nno : Bool\nno = False\n\nboth : List Bool\nboth = [yes, no]\n");
  writeFileSync(join(lib, "other.hf"), "noes : List Bool\nnoes = [no, no]\n");
  writeFileSync(join(lib, "list.hf"), "none : List Bool\nnone = []\n");
  writeFileSync(join(dir, "use.hf"), "import other\n\n-- >>> both\n-- Cons True (Cons False Nil)\n\n-- >>> noes\n-- Cons False (Cons False Nil)\n\n-- >>> List.none\n-- Nil\n");
  const inLib = (...args) => spawnSync(heffBin, args, { cwd: dir, encoding: "utf8", env: { ...process.env, HEFF_LIB: lib } });
  const used = inLib("test", "use.hf");
  const core = inLib("check", join(lib, "core.hf"));
  const prelude = inLib("check", join(lib, "prelude.hf"));
  if (used.status !== 0 || core.status !== 0 || core.stdout !== "yes : Bool\n" || prelude.status !== 0) {
    failed++;
    console.log(`FAIL the prelude without an import, and what it is made of:\n${[used, core, prelude].map((r) => r.stdout + r.stderr).join("\n")}`);
  }
  // (`core` is what the prelude is made of: the prelude's names are not in
  // its scope)
  writeFileSync(join(lib, "core.hf"), "yes : Bool\nyes = no\n");
  const coreUses = inLib("check", join(lib, "core.hf"));
  if (coreUses.status === 0 || !coreUses.stderr.includes("no such variable no in scope")) {
    failed++;
    console.log(`FAIL a module the prelude is made of has not the prelude:\n${coreUses.stdout}${coreUses.stderr}`);
  }
  rmSync(dir, { recursive: true, force: true });
}

process.exit(failed ? 1 : 0);
