#!/usr/bin/env node
// The command line: `promote` writes each watch's value under it, `fmt`
// lays files out in place (with `--check`, only names them), and the
// usage, the completions and an unknown command say what they should, on
// the right stream, with the right exit status.
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const work = mkdtempSync(join(tmpdir(), "heff-cli-"));
const heff = (...args) => spawnSync("node", [join(root, "bin", "heff.mjs"), ...args], { cwd: work, encoding: "utf8" });

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}`); } };

// promote: an expectation written where there was none, a wrong one
// replaced, a right one kept; then `test` holds
writeFileSync(join(work, "p.hf"), [
  "import prelude", "",
  "-- >>> 1 + 2", "",
  "-- >>> (Cons 1 Nil : List Int)", "-- wrong", "",
  "-- >>> not True", "-- False", "",
].join("\n"));
const promoted = heff("promote", "p.hf");
const text = readFileSync(join(work, "p.hf"), "utf8");
expect("promote", promoted.status === 0 &&
  text.includes("-- >>> 1 + 2\n-- 3\n") && text.includes("-- >>> (Cons 1 Nil : List Int)\n-- Cons 1 Nil\n") &&
  text.includes("-- >>> not True\n-- False\n") && !text.includes("wrong"), { promoted, text });
const tested = heff("test", "p.hf");
expect("test after promote", tested.status === 0 && tested.stdout.includes("3/3 expectations hold"), tested);

// fmt: --check names what is not laid out (and fails), leaves it; without
// it, lays it out; a file that does not parse is refused, untouched
writeFileSync(join(work, "messy.hf"), "f :  Int -> Int\nf x   =  x + 1\n");
writeFileSync(join(work, "clean.hf"), "f : Int\nf = 1\n");
writeFileSync(join(work, "broken.hf"), "f : Int ->\n");
const checked = heff("fmt", "--check", "messy.hf", "clean.hf");
expect("fmt --check", checked.status === 1 && checked.stdout === "messy.hf\n" &&
  readFileSync(join(work, "messy.hf"), "utf8") === "f :  Int -> Int\nf x   =  x + 1\n", checked);
const laid = heff("fmt", "messy.hf", "clean.hf");
expect("fmt", laid.status === 0 && readFileSync(join(work, "messy.hf"), "utf8") === "f : Int -> Int\nf x = x + 1\n" &&
  readFileSync(join(work, "clean.hf"), "utf8") === "f : Int\nf = 1\n", laid);
const refused = heff("fmt", "broken.hf");
expect("fmt of what does not parse", refused.status === 1 && refused.stderr.includes("broken.hf not formatted: the file does not parse") &&
  readFileSync(join(work, "broken.hf"), "utf8") === "f : Int ->\n", refused);

// the usage: asked for, on stdout; for what is no command (or a command
// without its files), on stderr, failing
const commands = ["check", "eval", "run", "test", "promote", "compile-js", "app", "node", "explain", "type-at", "fmt", "lsp", "completions"];
for (const args of [[], ["help"]]) {
  const r = heff(...args);
  expect(`usage (${args.join(" ") || "no arguments"})`, r.status === 0 && r.stdout.startsWith("usage: heff COMMAND FILE ...") &&
    commands.every((c) => r.stdout.includes(`\n  ${c}`)), r);
}
for (const args of [["no-such-command"], ["test"], ["check"]]) {
  const r = heff(...args);
  expect(`refused: ${args.join(" ")}`, r.status === 1 && r.stdout === "" && r.stderr.startsWith("usage: heff COMMAND FILE ..."), r);
}

// fish completions: every command, and file completion where it takes files
const fish = heff("completions", "fish");
expect("completions fish", fish.status === 0 && fish.stdout.startsWith("# heff: written by `heff completions fish`\n") &&
  commands.every((c) => fish.stdout.includes(`-a ${c} -d `)) &&
  fish.stdout.includes("complete -c heff -n '__fish_seen_subcommand_from check' -a '(__fish_complete_suffix .hf)'"), fish.stdout);

// type-at: nothing there, at an offset that is no name or no number
for (const o of ["0", "nope"]) {
  const r = heff("type-at", "clean.hf", o);
  expect(`type-at ${o}`, r.status === 0 && r.stdout === "nothing there\n", r);
}

console.log(`cli: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
