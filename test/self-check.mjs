#!/usr/bin/env node
// The compiler compiles itself: compiler/main.hf is compiled to a Node
// program by the reference, then run over its own sources -- the
// standard library it uses, the formatter's lexer, and every module of
// compiler/ -- and every definition it stores is held byte-identical to
// what the reference stores for the same files.  The list is in
// dependency order; a compiler/ module missing from it fails the test.
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const heff = (...args) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8", maxBuffer: 1e9 });

const files = [
  "lib/prelude.hf", "lib/array.hf", "lib/map.hf", "lib/co.hf",
  "lib/stdio.hf", "lib/process.hf", "lib/fs.hf",
  "fmt/lex.hf",
  "compiler/dict.hf", "compiler/params.hf", "compiler/syntax.hf",
  "compiler/value.hf", "compiler/globals.hf", "compiler/eval.hf",
  "compiler/readback.hf", "compiler/conversion.hf", "compiler/notation.hf",
  "compiler/environment.hf", "compiler/unify.hf", "compiler/meta.hf",
  "compiler/memoed.hf", "compiler/print.hf", "compiler/top.hf",
  "compiler/rules.hf", "compiler/fixity.hf", "compiler/import.hf",
  "compiler/surface.hf", "compiler/clauses.hf", "compiler/toplevel.hf",
  "compiler/prims.hf", "compiler/ir.hf", "compiler/shake.hf",
  "compiler/arity.hf", "compiler/erase.hf", "compiler/simplify.hf",
  "compiler/effect.hf", "compiler/lower.hf", "compiler/emit.hf",
  "compiler/driver.hf", "compiler/load.hf", "compiler/main.hf",
];

let failed = 0;

// every module of compiler/ is in the list
for (const f of readdirSync(join(root, "compiler")).filter((f) => f.endsWith(".hf"))) {
  if (!files.includes("compiler/" + f)) { failed++; console.log(`FAIL self-check: compiler/${f} is not in the list`); }
}

// the self-hosted compiler, compiled by the reference; the artifact is
// kept (output/, like the formatter's), the next generation's seed
mkdirSync(join(root, "output"), { recursive: true });
const bin = join(root, "output", "heff-selfcheck.mjs");
const b = heff("node", "compiler/main.hf", "main", bin);
if (b.status !== 0) { console.log(`FAIL self-check: could not compile compiler/main.hf\n${b.stdout}${b.stderr}`); process.exit(1); }

// run over its own sources
const run = spawnSync("node", ["--stack-size=60000", bin, ...files], { cwd: root, encoding: "utf8", maxBuffer: 1e9 });
if (run.status !== 0) { failed++; console.log(`FAIL self-check: the compiler refused its own source\n${run.stderr.split("\n").slice(0, 12).join("\n")}`); }

// the reference, on the same files
let ps = "";
for (const f of files) {
  const r = heff("dump-nbe", resolve(root, f));
  if (r.status !== 0) { failed++; console.log(`FAIL self-check: reference dump of ${f}\n${r.stderr.split("\n").slice(0, 6).join("\n")}`); break; }
  ps += r.stdout.split("\n").filter((l) => /^=(def|ty|body) /.test(l)).map((l) => l + "\n").join("");
}

// module values: a program that passes a module as a value (`import m as
// M`, `M` the struct) elaborates byte-identically too -- the port
// synthesizes `m#`/`m#T` as the reference does.  (Its own corpus: the
// selfcheck mode names modules by path, so the files sit in a scratch
// root whose lib/ holds prelude.)
import { mkdtempSync, copyFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
const mroot = mkdtempSync(join(tmpdir(), "heff-modtest-"));
mkdirSync(join(mroot, "lib"), { recursive: true });
copyFileSync(join(root, "lib", "prelude.hf"), join(mroot, "lib", "prelude.hf"));
for (const f of ["plain.hf", "value-use.hf"]) copyFileSync(join(root, "test", "modules", f), join(mroot, f));
const mfiles = ["lib/prelude.hf", "plain.hf", "value-use.hf"];
const mrun = spawnSync("node", ["--stack-size=60000", bin, ...mfiles], { cwd: mroot, encoding: "utf8", maxBuffer: 1e9 });
let mref = "";
for (const f of ["lib/prelude.hf", "test/modules/plain.hf", "test/modules/value-use.hf"]) {
  const r = heff("dump-nbe", resolve(root, f));
  mref += r.stdout.split("\n").filter((l) => /^=(def|ty|body) /.test(l)).map((l) => l + "\n").join("");
}
if (mrun.status !== 0 || mrun.stdout !== mref) { failed++; console.log(`FAIL self-check: module values differ (a program passing a module as a value)\n${mrun.stderr.split("\n").slice(0, 6).join("\n")}`); }
rmSync(mroot, { recursive: true, force: true });

const self = run.stdout;
const defs = (self.match(/^=def /gm) || []).length;
if (!failed && ps !== self) {
  failed++;
  const a = ps.split("\n"), b2 = self.split("\n");
  for (let i = 0; i < Math.max(a.length, b2.length); i++) {
    if (a[i] !== b2[i]) { console.log(`FAIL self-check: first difference at line ${i + 1}\n  reference: ${(a[i] || "").slice(0, 120)}\n  port:      ${(b2[i] || "").slice(0, 120)}`); break; }
  }
}

console.log(`self-check: ${defs} definitions of its own source, ${failed ? "NOT " : ""}byte-identical to the reference`);
process.exit(failed ? 1 : 0);
