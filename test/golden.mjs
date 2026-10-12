#!/usr/bin/env node
// What heff says of the repository's files, held to what it said: for
// each file of the library and the tests, `check` (its interface and its
// diagnostics), `explain` (the elaboration's trace), `eval` (each
// definition's value, `dump-eval`), `type-at` (at some names) and, of a
// file of test/errors/ whose first comment names a backend
// (`--backend=B`), `compile` for it -- against the text
// kept under test/golden/, byte for byte.  A change of them is a change
// of heff, to be looked at -- and kept with
// `node test/golden.mjs --update`.
// (The compiler's own files are held by the fixpoint instead: their
// words change with every change of the compiler.)
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { run, pool } from "./pool.mjs";
import { heffBin, root } from "./heff.mjs";

// (`node test/golden.mjs [--update] [KIND...]`: only the kinds named,
// check, explain, eval, type-at, compile; all by default)
const update = process.argv.includes("--update");
const only = process.argv.slice(2).filter((a) => !a.startsWith("--"));
const heff = (args) => run(heffBin, args);

const filesIn = (dirs) => dirs.flatMap((d) =>
  spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" }).stdout.split("\n").filter(Boolean));

const said = (r) => `${r.stdout}--stderr--\n${r.stderr}--exit ${r.status}\n`;

// (the lexer's keywords, `Int` and `Type` among them: no names)
const keywords = new Set(
  readFileSync(join(root, "src", "heff", "syntax", "fmt", "lex.hf"), "utf8")
    .match(/^keywords =\n((?:  .*\n)+)/m)[1].match(/"[^"]+"/g).map((k) => k.slice(1, -1)));

// type-at: the starts of some names of the file (every one of a stride,
// eight at most; not in a comment, a string or an import), the file's
// start, and an offset that is no number
const offsetsIn = (f) => {
  // (a string's words are no names, nor a rune literal's: blanked, its
  // length kept; read as its bytes, a character each, as an offset is
  // of bytes -- a rune literal's rune is one byte, or a first one and
  // those that go on)
  const src = readFileSync(join(root, f), "latin1")
    .replace(/"(?:[^"\\\n]|\\.)*"/g, (s) => " ".repeat(s.length))
    .replace(/(?<![A-Za-z0-9_'])'(?:[^'\\\n\x80-\xff]|[\xc0-\xff][\x80-\xbf]+|\\u\{[0-9A-Fa-f]+\}|\\.)'/g, (s) => " ".repeat(s.length));
  const starts = [];
  for (const m of src.matchAll(/(?<![A-Za-z0-9_'])[A-Za-z_][A-Za-z0-9_']*/g)) {
    const before = src.slice(src.lastIndexOf("\n", m.index) + 1, m.index).trimStart();
    if (!keywords.has(m[0]) && !/^--/.test(before) && !/^import\b/.test(before)) starts.push(m.index);
  }
  const stride = Math.max(1, Math.floor(starts.length / 8));
  return [...starts.filter((_, i) => i % stride === 0).slice(0, 8).map((o) => String(o + 1)), "0", "nope"];
};

// compile: the backend a file's first comment names, what it makes
// thrown away (what is held is what heff says)
const backendOf = (f) => /^--.*--backend=(\w+)/m.exec(readFileSync(join(root, f), "utf8").split(/\n(?!--)/)[0])?.[1];
const thrown = join(root, ".heff", "ref", "golden");
mkdirSync(thrown, { recursive: true });

const kinds = [
  { kind: "check", files: filesIn(["lib", "lib/std", "lib/browser", "test/nbe", "test/surface", "test/errors", "test/modules", "test/modules/src"]),
    say: async (f) => said(await heff(["check", f])) },
  { kind: "explain", files: filesIn(["lib", "lib/std", "lib/browser", "test/nbe", "test/surface", "test/errors"]),
    say: async (f) => said(await heff(["explain", f])) },
  { kind: "eval", files: filesIn(["lib", "lib/std", "lib/browser", "test/nbe"]),
    say: async (f) => said(await heff(["dump-eval", f])) },
  { kind: "type-at", files: filesIn(["lib", "lib/std", "lib/browser", "test/surface", "test/nbe"]),
    say: async (f) => {
      // (one run for all of them, a line each)
      const offsets = offsetsIn(f);
      const r = await heff(["type-at", f, ...offsets]);
      const lines = r.stdout.split("\n");
      const answers = offsets.map((o, i) => `@${o}\n${lines[i] ?? ""}\n`).join("");
      return said({ ...r, stdout: answers + lines.slice(offsets.length).join("\n") });
    } },
  { kind: "compile", files: filesIn(["test/errors"]).filter((f) => backendOf(f)),
    say: async (f) => said(await heff(["compile", f, join(thrown, f.replaceAll("/", "_") + ".out"), `--backend=${backendOf(f)}`])) },
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
