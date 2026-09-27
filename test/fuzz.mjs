#!/usr/bin/env node
// Differential testing against the reference Haskell implementation.
//
//   node test/fuzz.mjs REF_EXE [N] [SEED]
//
// Generates N programs -- half type-directed random programs, half mutations
// of the reference test corpus -- runs both implementations on each, and
// compares stdout and stderr byte for byte.  Programs avoid the two places
// where this port deliberately differs (string literals and `#` comments).
import { spawnSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdirSync, readdirSync, cpSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [refExe, nArg = "400", seedArg = "1"] = process.argv.slice(2);
if (!refExe) {
  console.error("usage: node test/fuzz.mjs REF_EXE [N] [SEED]");
  process.exit(2);
}

import { setSeed, pick, chance, range, genProgram } from "./gen.mjs";
setSeed(seedArg);

// --- mutation of the corpus -----------------------------------------------------
const corpusDir = join(root, "test", "golden");
const corpus = readdirSync(corpusDir).filter((f) => f.endsWith(".hpgf")).map((f) => readFileSync(join(corpusDir, f), "utf8"));

function mutate(src) {
  let s = src;
  const n = range(1, 3);
  for (let i = 0; i < n; i++) {
    const words = [...s.matchAll(/[A-Za-z_][A-Za-z0-9_\/-]*|'[A-Za-z_]+|:=|=>|->|[():~\[\]]/g)];
    if (!words.length) break;
    const w = pick(words);
    const at = w.index, len = w[0].length;
    const other = pick(words)[0];
    const op = range(0, 6);
    if (op === 0) s = s.slice(0, at) + s.slice(at + len); // delete a token
    else if (op === 1) s = s.slice(0, at) + other + s.slice(at + len); // replace by another token
    else if (op === 2) s = s.slice(0, at) + other + " " + s.slice(at); // insert a token
    else if (op === 3) { // delete a line
      const lines = s.split("\n"); lines.splice(range(0, lines.length - 1), 1); s = lines.join("\n");
    } else if (op === 4) { // duplicate a line
      const lines = s.split("\n"); const k = range(0, lines.length - 1); lines.splice(k, 0, lines[k]); s = lines.join("\n");
    } else if (op === 5) s = s.slice(0, at) + "rec (" + w[0] + ")" + s.slice(at + len);
    else s = s.slice(0, at) + pick(["Type", "Int", "abandon", "'nil", "x", "(", ")", "end", "sum", "match"]) + s.slice(at + len);
  }
  return s;
}

// --- running -----------------------------------------------------------------
const tmp = join(root, "ref", `fuzz-run-${seedArg}-${process.pid}`);
mkdirSync(tmp, { recursive: true });
// run a snapshot of the build, so that rebuilding meanwhile cannot mix versions
const snap = join(tmp, "snapshot");
cpSync(join(root, "output"), join(snap, "output"), { recursive: true });
mkdirSync(join(snap, "bin"), { recursive: true });
cpSync(join(root, "bin", "hg.mjs"), join(snap, "bin", "hg.mjs"));
writeFileSync(join(snap, "package.json"), JSON.stringify({ type: "module" }));

function run(cmd, args) {
  const r = spawnSync(cmd, args, { encoding: "utf8", timeout: 10000 });
  return { out: r.stdout, err: r.stderr, code: r.status, timedOut: r.error && r.error.code === "ETIMEDOUT" };
}

const N = Number(nArg);
let same = 0, differ = 0, bothCrash = 0, refCrashOnly = 0, oursCrashOnly = 0, timeouts = 0, lazyAbandon = 0, bothDiverge = 0, refLexerBug = 0, refRecLoop = 0;
const failures = [];
for (let i = 0; i < N; i++) {
  const src = i % 2 === 0 ? genProgram() : mutate(pick(corpus));
  const file = join(tmp, `case${i}.hpgf`);
  writeFileSync(file, src);
  const ref = run(refExe, [file]);
  const ours = run("node", [join(snap, "bin", "hg.mjs"), "ref", file]);
  // Both diverging counts as agreeing: the reference loops (Haskell's stack
  // lives on the heap), the port runs out of JavaScript stack.
  const oursDiverged = ours.timedOut || /RangeError: Maximum call stack size exceeded/.test(ours.err || "") || /<diverged>/.test(ours.out || "");
  if (ref.timedOut && oursDiverged) { bothDiverge++; continue; }
  // The reference unfolds a direct self-call under `rec` forever at
  // elaboration time; the port treats `rec e` as statically opaque (the
  // paper's semantics), so it terminates.
  if (ref.timedOut && !ours.timedOut && /\brec\b/.test(src)) { refRecLoop++; continue; }
  if (ref.timedOut || ours.timedOut) { timeouts++; failures.push({ i, why: "timeout on one side only", file }); continue; }
  // The reference interpreter's `try` does not catch an `abandon` that is only
  // reached while printing a lazily evaluated result, so it crashes midway.
  // The port is strict and reports `<abandoned>`.  Compare what comes before.
  if (ref.code !== 0 && /InterpException:\s*Abandon/.test(ref.err)) {
    // stdout is block-buffered, so the reference's partial output stops
    // anywhere before the crash; it must be a prefix of ours up to the results
    const partial = ref.out.split("-- evaluated")[0];
    const errCut = ref.err.split("hippogriff: Uncaught exception")[0];
    if (ours.out.startsWith(partial) && ours.err.startsWith(errCut)) { lazyAbandon++; continue; }
    differ++; failures.push({ i, why: "differs before a lazy-abandon crash", file }); continue;
  }
  // a bug in the reference's lexer: its token buffer has a fixed capacity
  if (ref.code !== 0 && /cannot push because buffer is full/.test(ref.err)) { refLexerBug++; continue; }
  const refOk = ref.code === 0, oursOk = ours.code === 0;
  if (!refOk && !oursOk) { bothCrash++; continue; }
  if (!refOk) { refCrashOnly++; failures.push({ i, why: "reference crashed, port did not", file, detail: ref.err.slice(0, 300) }); continue; }
  if (!oursOk) { oursCrashOnly++; failures.push({ i, why: "port crashed, reference did not", file, detail: ours.err.slice(0, 300) }); continue; }
  if (ref.out === ours.out && ref.err === ours.err) same++;
  else { differ++; failures.push({ i, why: ref.out !== ours.out ? "stdout differs" : "stderr differs", file }); }
}
console.log(JSON.stringify({ N, same, differ, lazyAbandon, bothDiverge, refRecLoop, refLexerBug, bothCrash, refCrashOnly, oursCrashOnly, timeouts }, null, 1));
for (const f of failures.slice(0, 15)) console.log(f.why, f.file, f.detail ? "\n  " + f.detail.replace(/\n/g, "\n  ") : "");
process.exit(failures.length ? 1 : 0);
