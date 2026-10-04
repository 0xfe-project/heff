#!/usr/bin/env node
// The rest of the CLI through both compilers: `compile-js` and
// `app`/`node` write the same bytes, `eval` prints the same values,
// `promote` writes the same expectations back, `fmt` lays out (and
// refuses) as the reference does, and `help`, `completions fish` and a
// word that is no command say the same.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, writeFileSync, mkdtempSync, mkdirSync, copyFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve, basename } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const bin = join(root, "output", "heff-selfcheck.mjs");
const heff = (...args) => spawnSync("node", ["--stack-size=60000", join(root, "bin", "heff.mjs"), ...args], { cwd: root, encoding: "utf8", maxBuffer: 1e9, timeout: 600000 });
const portIn = (cwd, ...args) => spawnSync("node", [join(root, "test", "run-worker.mjs"), bin, ...args], { cwd, encoding: "utf8", maxBuffer: 1e9, timeout: 600000 });

if (!existsSync(bin)) { console.log("FAIL cli-port: no output/heff-selfcheck.mjs (run test/self-check.mjs first)"); process.exit(1); }

const tmp = mkdtempSync(join(tmpdir(), "heff-cliport-"));
let failed = 0;
let compared = 0;

const fail = (what, extra) => { failed++; console.log(`FAIL cli-port: ${what}\n${extra}`.trim()); };
const firstDiff = (a, b) => {
  const as = a.split("\n"), bs = b.split("\n");
  for (let i = 0; i < Math.max(as.length, bs.length); i++)
    if (as[i] !== bs[i]) return `  line ${i + 1}\n  reference: ${(as[i] || "").slice(0, 140)}\n  port:      ${(bs[i] || "").slice(0, 140)}`;
  return "";
};

// compile-js: the module's bytes
for (const f of ["fmt/layout.hf", "lib/doc.hf", "compiler/arity.hf", "test/nbe/data.hf"]) {
  if (!existsSync(join(root, f))) continue;
  compared++;
  const rout = join(tmp, "r.js"), pout = join(tmp, "p.js");
  const r = heff("compile-js", resolve(root, f), rout);
  const p = portIn(root, "compile-js", f, pout);
  if (p.status !== r.status) { fail(`compile-js ${f} exit (reference ${r.status}, port ${p.status})`, p.stderr.split("\n").slice(0, 3).join("\n")); continue; }
  const a = readFileSync(rout, "utf8"), b = readFileSync(pout, "utf8");
  if (a !== b) fail(`compile-js ${f}`, firstDiff(a, b));
}

// node and app: the program's bytes (the artifact only -- the load
// paths differ, so stderr is not compared)
for (const [mode, f] of [["node", "test/node/echo.hf"], ["node", "compiler/main.hf"], ["app", "test/apps/aliased.hf"], ["app", "test/apps/select.hf"]]) {
  if (!existsSync(join(root, f))) continue;
  compared++;
  const rout = join(tmp, "r.mjs"), pout = join(tmp, "p.mjs");
  const r = heff(mode, resolve(root, f), rout);
  const p = portIn(root, mode, pout, f);
  if (p.status !== r.status) { fail(`${mode} ${f} exit (reference ${r.status}, port ${p.status})`, p.stderr.split("\n").slice(0, 3).join("\n")); continue; }
  if (r.status !== 0) continue;
  const a = readFileSync(rout, "utf8"), b = readFileSync(pout, "utf8");
  if (a !== b) fail(`${mode} ${f}`, firstDiff(a, b));
  // and the one stdout line says the same thing
  const rl = r.stdout.replaceAll(rout, "OUT"), pl = p.stdout.replaceAll(pout, "OUT");
  if (rl !== pl) fail(`${mode} ${f} stdout`, firstDiff(rl, pl));
}

// eval: every definition's value
{
  const dirs = ["lib", "test/nbe"];
  for (const d of dirs) {
    const ls = spawnSync("bash", ["-c", `ls ${d}/*.hf`], { cwd: root, encoding: "utf8" });
    for (const f of ls.stdout.split("\n").filter(Boolean)) {
      compared++;
      const r = heff("eval", resolve(root, f));
      const p = portIn(root, "eval", f);
      if (p.status !== r.status || p.stdout !== r.stdout)
        fail(`eval ${f} (reference exit ${r.status}, port exit ${p.status})`, firstDiff(r.stdout, p.stdout) || p.stderr.split("\n").slice(0, 3).join("\n"));
    }
  }
}

// promote: the expectations written back (each side its own copy of a
// scratch root whose lib/ holds the library)
{
  const mk = (name) => {
    const d = join(tmp, name);
    mkdirSync(join(d, "lib"), { recursive: true });
    const ls = spawnSync("bash", ["-c", "ls lib/*.hf"], { cwd: root, encoding: "utf8" });
    for (const f of ls.stdout.split("\n").filter(Boolean)) copyFileSync(join(root, f), join(d, "lib", basename(f)));
    return d;
  };
  const rdir = mk("ref"), pdir = mk("port");
  const ls = spawnSync("bash", ["-c", "ls test/nbe/*.hf test/surface/floats.hf test/surface/shortcut.hf"], { cwd: root, encoding: "utf8" });
  for (const f of ls.stdout.split("\n").filter(Boolean)) {
    compared++;
    copyFileSync(join(root, f), join(rdir, basename(f)));
    copyFileSync(join(root, f), join(pdir, basename(f)));
    const r = heff("promote", join(rdir, basename(f)));
    const p = portIn(pdir, "promote", basename(f));
    if (p.status !== r.status) { fail(`promote ${f} exit (reference ${r.status}, port ${p.status})`, p.stderr.split("\n").slice(0, 3).join("\n")); continue; }
    const a = readFileSync(join(rdir, basename(f)), "utf8"), b = readFileSync(join(pdir, basename(f)), "utf8");
    if (a !== b) fail(`promote ${f}`, firstDiff(a, b));
  }
}

// fmt: each side formats its own copy of the same files in place (one
// unformatted, one that does not parse); `--check` says the same names
{
  const cases = {
    "messy.hf": "f :  Int -> Int\nf x   =  x + 1\n",
    "broken.hf": "f : Int ->\n",
    "clean.hf": "f : Int\nf = 1\n",
  };
  const mk = (name) => {
    const d = join(tmp, name);
    mkdirSync(d, { recursive: true });
    for (const [f, t] of Object.entries(cases)) writeFileSync(join(d, f), t);
    return d;
  };
  for (const check of [true, false]) {
    compared++;
    const rdir = mk(`fmt-ref-${check}`), pdir = mk(`fmt-port-${check}`);
    const args = [...(check ? ["--check"] : []), ...Object.keys(cases)];
    const r = spawnSync("node", ["--stack-size=60000", join(root, "bin", "heff.mjs"), "fmt", ...args], { cwd: rdir, encoding: "utf8" });
    const p = portIn(pdir, "fmt", ...args);
    const what = `fmt${check ? " --check" : ""}`;
    if (p.status !== r.status) { fail(`${what} exit (reference ${r.status}, port ${p.status})`, p.stderr.split("\n").slice(0, 3).join("\n")); continue; }
    if (p.stdout !== r.stdout) fail(`${what} stdout`, firstDiff(r.stdout, p.stdout));
    if (p.stderr !== r.stderr) fail(`${what} stderr`, firstDiff(r.stderr, p.stderr));
    for (const f of Object.keys(cases)) {
      const a = readFileSync(join(rdir, f), "utf8"), b = readFileSync(join(pdir, f), "utf8");
      if (a !== b) fail(`${what} ${f}`, firstDiff(a, b));
    }
  }
  // and the repository's own files are formatted, by the port's word too
  compared++;
  const ls = spawnSync("bash", ["-c", "ls lib/*.hf fmt/*.hf compiler/*.hf"], { cwd: root, encoding: "utf8" });
  const own = portIn(root, "fmt", "--check", ...ls.stdout.split("\n").filter(Boolean));
  if (own.status !== 0) fail("fmt --check of lib/, fmt/ and compiler/", own.stdout + own.stderr.split("\n").slice(0, 3).join("\n"));
}

// help, completions, and what is no command: the same words, on the same
// stream, with the same exit
for (const args of [[], ["help"], ["completions", "fish"], ["no-such-command"], ["test"]]) {
  compared++;
  const r = heff(...args);
  const p = portIn(root, ...args);
  const what = args.length ? args.join(" ") : "(no arguments)";
  if (p.status !== r.status) fail(`${what} exit (reference ${r.status}, port ${p.status})`, "");
  if (p.stdout !== r.stdout) fail(`${what} stdout`, firstDiff(r.stdout, p.stdout));
  if (p.stderr !== r.stderr) fail(`${what} stderr`, firstDiff(r.stderr, p.stderr));
}

console.log(`cli port: ${compared} invocations of compile-js, node, app, eval, promote, fmt, help and completions ${failed ? "NOT " : ""}matched the reference`);
process.exit(failed ? 1 : 0);
