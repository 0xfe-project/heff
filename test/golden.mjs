#!/usr/bin/env node
// What heff says of the repository's files, held to what it said: for
// each file of the library and the tests, `check` (its interface and its
// diagnostics), `explain` (the elaboration's trace), `eval` (each
// definition's value, `dump-eval`) and `type-at` (at some names), against the text
// kept under test/golden/, byte for byte.  A change of them is a change
// of heff, to be looked at -- and kept with
// `node test/golden.mjs --update`.
// (The compiler's own files are held by the fixpoint instead: their
// words change with every change of the compiler.)
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { root, run, pool } from "./pool.mjs";

// (`node test/golden.mjs [--update] [KIND...]`: only the kinds named,
// check, explain, eval, type-at; all by default)
const update = process.argv.includes("--update");
const only = process.argv.slice(2).filter((a) => !a.startsWith("--"));
const heff = (args) => run("node", [join(root, "output", "heff.mjs"), ...args]);

const filesIn = (dirs) => dirs.flatMap((d) =>
  spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" }).stdout.split("\n").filter(Boolean));

const said = (r) => `${r.stdout}--stderr--\n${r.stderr}--exit ${r.status}\n`;

// (the lexer's keywords, `Int` and `Type` among them: no names)
const keywords = new Set(
  readFileSync(join(root, "src", "heff", "fmt", "lex.hf"), "utf8")
    .match(/^keywords =\n((?:  .*\n)+)/m)[1].match(/"[^"]+"/g).map((k) => k.slice(1, -1)));

// type-at: the starts of some names of the file (every one of a stride,
// eight at most; not in a comment, a string or an import), the file's
// start, and an offset that is no number
const offsetsIn = (f) => {
  // (a string's words are no names: blanked, its length kept)
  const src = readFileSync(join(root, f), "utf8").replace(/"(?:[^"\\\n]|\\.)*"/g, (s) => " ".repeat(s.length));
  const starts = [];
  for (const m of src.matchAll(/(?<![A-Za-z0-9_'])[A-Za-z_][A-Za-z0-9_']*/g)) {
    const before = src.slice(src.lastIndexOf("\n", m.index) + 1, m.index).trimStart();
    if (!keywords.has(m[0]) && !/^--/.test(before) && !/^import\b/.test(before)) starts.push(m.index);
  }
  const stride = Math.max(1, Math.floor(starts.length / 8));
  return [...starts.filter((_, i) => i % stride === 0).slice(0, 8).map((o) => String(o + 1)), "0", "nope"];
};

const kinds = [
  { kind: "check", files: filesIn(["lib", "lib/std", "test/nbe", "test/surface", "test/errors", "test/modules", "test/modules/src"]),
    say: async (f) => said(await heff(["check", f])) },
  { kind: "explain", files: filesIn(["lib", "lib/std", "test/nbe", "test/surface", "test/errors"]),
    say: async (f) => said(await heff(["explain", f])) },
  { kind: "eval", files: filesIn(["lib", "lib/std", "test/nbe"]),
    say: async (f) => said(await heff(["dump-eval", f])) },
  { kind: "type-at", files: filesIn(["lib", "lib/std", "test/surface", "test/nbe"]),
    say: async (f) => {
      // (one run for all of them, a line each)
      const offsets = offsetsIn(f);
      const r = await heff(["type-at", f, ...offsets]);
      const lines = r.stdout.split("\n");
      const answers = offsets.map((o, i) => `@${o}\n${lines[i] ?? ""}\n`).join("");
      return said({ ...r, stdout: answers + lines.slice(offsets.length).join("\n") });
    } },
];

const jobs = kinds.filter((k) => only.length === 0 || only.includes(k.kind)).flatMap((k) => k.files.map((f) => ({ kind: k.kind, f, say: k.say })));
const results = await pool(jobs, async (j) => {
  const got = await j.say(j.f);
  const at = join(root, "test", "golden", j.kind, j.f + ".txt");
  if (update) {
    mkdirSync(dirname(at), { recursive: true });
    writeFileSync(at, got);
    return null;
  }
  const want = existsSync(at) ? readFileSync(at, "utf8") : null;
  if (want === got) return null;
  if (want === null) return `FAIL golden: ${j.kind} ${j.f}: no golden text (run with --update to keep it)`;
  const a = want.split("\n"), b = got.split("\n");
  const i = a.findIndex((l, k) => l !== b[k]);
  return `FAIL golden: ${j.kind} ${j.f}\n  line ${i + 1}\n  kept: ${(a[i] ?? "").slice(0, 140)}\n  now:  ${(b[i] ?? "").slice(0, 140)}`;
});
const failed = results.filter(Boolean);
for (const l of failed) console.log(l);
console.log(update
  ? `golden: ${jobs.length} texts kept`
  : `golden: ${jobs.length - failed.length}/${jobs.length} texts as they were kept`);
process.exit(failed.length ? 1 : 0);
