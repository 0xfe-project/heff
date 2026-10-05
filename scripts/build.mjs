#!/usr/bin/env node
// The compiler, bootstrapped from its seed: boot/heff.mjs (the compiler,
// compiled by itself, kept) compiles compiler/main.hf to a first stage,
// and the first stage compiles the same source to output/heff.raw.mjs --
// made by the backend of the source, whatever the seed's was -- which,
// minified (esbuild: white space and syntax, every name kept), is
// output/heff.mjs, `heff`.  (test/gen-fixpoint.mjs holds that the next
// generation is the raw one; `npm run boot` keeps it as the seed.)
import { spawnSync } from "node:child_process";
import { chmodSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { transformSync } from "esbuild";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const seed = join(root, "boot", "heff.mjs");
const stage1 = join(root, "output", "heff-stage1.mjs");
const raw = join(root, "output", "heff.raw.mjs");
const out = join(root, "output", "heff.mjs");

// (nothing to do where the compiler is newer than every source of it and
// than the seed)
const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
const sources = [seed, join(root, "scripts", "build.mjs"),
  ...["compiler", "compiler/fmt", "lib"].flatMap((d) => readdirSync(join(root, d)).filter((f) => f.endsWith(".hf") || f.endsWith(".js")).map((f) => join(root, d, f)))];
if (mtime(out) >= 0 && sources.every((s) => mtime(s) <= mtime(out))) {
  console.log(`heff build: ${out} is up to date`);
  process.exit(0);
}

const step = (what, compiler, to) => {
  const r = spawnSync(process.execPath, [compiler, "compile", "compiler/main.hf", to, "--name", "main", "--backend=node"],
    { cwd: root, stdio: ["ignore", "ignore", "inherit"] });
  if (r.status !== 0) { console.error(`heff build: ${what} failed`); process.exit(r.status ?? 1); }
};
// (the compiler without its layout: what it says and does is the same)
const minify = (code) => transformSync(code,
  { loader: "js", format: "esm", target: "esnext", minifyWhitespace: true, minifySyntax: true, legalComments: "none" }).code;

mkdirSync(join(root, "output"), { recursive: true });
step("the first stage (the seed compiling compiler/main.hf)", seed, stage1);
step("the compiler (the first stage compiling itself)", stage1, raw);
writeFileSync(out, minify(readFileSync(raw, "utf8")));
chmodSync(out, 0o755);
console.log(`heff build: ${out}, the compiler compiled by itself`);
