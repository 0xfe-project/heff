#!/usr/bin/env node
// The command line: `promote` writes each watch's and doc test's value
// under it, `test` runs the tests (lib/check.hf) and shrinks what fails,
// `fmt` lays files out in place (with `--check`, only names them),
// `version` says which heff runs, and the usage, the completions and an
// unknown command say what they should, on the right stream, with the
// right exit status.
import { spawnSync } from "node:child_process";
import { chmodSync, copyFileSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { heffBin, root } from "./heff.mjs";

const work = mkdtempSync(join(tmpdir(), "heff-cli-"));
const heff = (...args) => spawnSync(heffBin, [...args], { cwd: work, encoding: "utf8" });

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}`); } };

// promote: an expectation written where there was none, a wrong one
// replaced, a right one kept; then `test` holds
writeFileSync(join(work, "p.hf"), [
  "-- >>> 1 + 2", "",
  "-- >>> (Cons 1 Nil : List Int)", "-- wrong", "",
  "-- >>> not True", "-- False", "",
].join("\n"));
const promoted = heff("promote", "p.hf");
const text = readFileSync(join(work, "p.hf"), "utf8");
expect("promote", promoted.status === 0 &&
  text.includes("-- >>> 1 + 2\n-- 3\n") && text.includes("-- >>> (Cons 1 Nil : List Int)\n-- [1]\n") &&
  text.includes("-- >>> not True\n-- False\n") && !text.includes("wrong"), { promoted, text });
const tested = heff("test", "p.hf");
expect("test after promote", tested.status === 0 && tested.stdout.includes("3/3 expectations hold"), tested);

// doc tests: one that does not hold named by its line and its definition;
// promoted, each expectation in the doc comment, the rest of it kept
writeFileSync(join(work, "d.hf"), [
  "-- | a number, twice",
  "-- | >>> twice 2",
  "-- | >>> twice 3",
  "-- | 5",
  "-- |",
  "-- | the doc goes on",
  "twice : Int -> Int",
  "twice x = x + x",
  "",
].join("\n"));
const dFailed = heff("test", "d.hf");
expect("a doc test that does not hold", dFailed.status === 1 &&
  dFailed.stdout.includes("FAIL d.hf:3 (twice): twice 3\n") && dFailed.stdout.includes("0/1 expectations hold"), dFailed);
const dPromoted = heff("promote", "d.hf");
const dText = readFileSync(join(work, "d.hf"), "utf8");
expect("promote, doc tests", dPromoted.status === 0 && dText === [
  "-- | a number, twice",
  "-- | >>> twice 2",
  "-- | 4",
  "-- | >>> twice 3",
  "-- | 6",
  "-- |",
  "-- | the doc goes on",
  "twice : Int -> Int",
  "twice x = x + x",
  "",
].join("\n"), { dPromoted, dText });
const dTested = heff("test", "d.hf");
expect("test after promote, doc tests", dTested.status === 0 && dTested.stdout.includes("2/2 expectations hold"), dTested);

// a diagnostic's code is written as `check` and the language server
// write it, by `test` too: a doc test of no definition warned of, W0151
writeFileSync(join(work, "w.hf"), ["-- | >>> 1 + 1", "-- | 2", ""].join("\n"));
const wTested = heff("test", "w.hf"), wChecked = heff("check", "w.hf");
expect("test, a warning's code", wTested.status === 0 &&
  (wTested.stdout + wTested.stderr).includes("warning[W0151]: a doc test is of the definition") &&
  wChecked.stderr.includes("warning[W0151]: a doc test is of the definition"), { wTested, wChecked });

// test: the definitions of type `Test` run too.  A property that fails on
// purpose is shrunk to its smallest counterexample, says the seed that
// runs it again, and fails the run; the same seed runs the same runs,
// another seed others, to the same counterexample
writeFileSync(join(work, "props.hf"), [
  "import check", "import co", "import gen as G", "",
  "small : Test",
  "small = group \"small\"",
  "  [ unit \"two\" (equal showInt (1 + 1) 2)",
  "  , unit \"three\" (equal showInt (1 + 1) 3)",
  "  , property \"all below 50\" (do",
  "      xs <- forAll (showList showInt) (G.list 0 20 (G.int 0 99))",
  "      assert \"all below 50\" (List.all (\\x -> x < 50) xs))",
  "  , property \"reversed twice\" (do",
  "      xs <- forAll (showList showInt) (G.list 0 20 (G.int 0 99))",
  "      label (if List.null xs then \"empty\" else \"not empty\")",
  "      equal (showList showInt) (List.reverse (List.reverse xs)) xs)",
  "  ]", "",
].join("\n"));
const props = heff("test", "props.hf");
const runsOf = (r) => (r.stdout.match(/all below 50  \(failed after (\d+) runs held/) ?? [])[1];
expect("test runs the tests", props.status === 1 &&
  props.stdout.includes("  ok    small / two\n") &&
  props.stdout.includes("  FAIL  small / three\n        expected 3\n        but got  2\n") &&
  /  FAIL  small \/ all below 50  \(failed after \d+ runs held; shrunk (once|\d+ times); again: --seed 2026\)\n        \[50\]\n        all below 50\n/.test(props.stdout) &&
  /  ok    small \/ reversed twice  \(100 runs, \d+% empty, \d+% not empty\)\n/.test(props.stdout) &&
  props.stdout.includes("FAIL  props.hf  0/0, tests 2/4\n") &&
  props.stdout.endsWith("0/0 expectations hold, 2/4 tests hold\n"), props);
const seeded = heff("test", "--seed", "99", "props.hf");
expect("test --seed", seeded.status === 1 && seeded.stdout.includes("again: --seed 99)\n        [50]\n") &&
  runsOf(seeded) !== undefined && runsOf(seeded) !== runsOf(props), { seeded, props });
expect("test --seed, again", heff("test", "props.hf", "-s", "99").stdout === seeded.stdout, seeded);

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
// fmt: a definition under `-- fmt: skip` is as it is written, for
// `--check` too, and left so; what is around it is laid out
const keptText = "-- fmt: skip\nm : List Int\nm = [ 1, 0,\n      0, 1 ]\n";
writeFileSync(join(work, "kept.hf"), keptText);
writeFileSync(join(work, "around.hf"), keptText + "n  =  1\n");
const keptCheck = heff("fmt", "--check", "kept.hf", "around.hf");
expect("fmt --check of a definition kept", keptCheck.status === 1 && keptCheck.stdout === "around.hf\n", keptCheck);
const keptLaid = heff("fmt", "kept.hf", "around.hf");
expect("fmt of a definition kept", keptLaid.status === 0 && readFileSync(join(work, "kept.hf"), "utf8") === keptText &&
  readFileSync(join(work, "around.hf"), "utf8") === keptText + "n = 1\n", keptLaid);
// fmt: a chain of operators breaks at its loosest, as the fixities of what
// the file imports say
writeFileSync(join(work, "ops.hf"), [
  "infixl <+", "infixl <* above <+", "",
  "(<+) : Int -> Int -> Int", "(<+) a b = a + b", "",
  "(<*) : Int -> Int -> Int", "(<*) a b = a * b", "",
].join("\n"));
writeFileSync(join(work, "uses.hf"), [
  "import ops", "",
  "long : Int",
  "long = aaaaaaaaaaaaaaaaaaaa <+ bbbbbbbbbbbbbbbbbbbbbbbbb <* cccccccccccccccccccccc <+ dddddddddddddddd", "",
].join("\n"));
const cut = heff("fmt", "uses.hf");
expect("fmt by imported fixities", cut.status === 0 && readFileSync(join(work, "uses.hf"), "utf8") === [
  "import ops", "",
  "long : Int",
  "long =",
  "  aaaaaaaaaaaaaaaaaaaa",
  "  <+ bbbbbbbbbbbbbbbbbbbbbbbbb <* cccccccccccccccccccccc",
  "  <+ dddddddddddddddd", "",
].join("\n"), { cut, text: readFileSync(join(work, "uses.hf"), "utf8") });

// the usage: asked for, on stdout; for what is no command, on stderr,
// failing; a command's own help, asked for; its words wrong, what is wrong
// and how it is called, on stderr
const commands = ["check", "run", "test", "promote", "compile", "explain", "type-at", "fmt", "init", "get", "build", "install", "publish", "lsp", "version", "help", "completions"];
for (const args of [[], ["help"], ["--help"]]) {
  const r = heff(...args);
  expect(`usage (${args.join(" ") || "no arguments"})`, r.status === 0 && r.stdout.startsWith("usage: heff COMMAND ...\n") &&
    commands.every((c) => r.stdout.includes(`\n  ${c}`)) && !r.stdout.includes("dump-core"), r);
}
const noSuch = heff("no-such-command");
expect("refused: no-such-command", noSuch.status === 1 && noSuch.stdout === "" &&
  noSuch.stderr.startsWith("heff: no command no-such-command\nusage: heff COMMAND ..."), noSuch);
for (const [args, says] of [
  [["fmt"], "heff fmt: expected FILE...\nusage: heff fmt FILE... [--check]\n"],
  [["check"], "heff check: expected FILE\nusage: heff check FILE\n"],
  [["fmt", "--chek", "x.hf"], "heff fmt: no flag --chek; --check?\nusage: heff fmt FILE... [--check]\n"],
  [["compile", "a.hf", "a.mjs"], "heff compile: --backend=MODE is wanted\n"],
  [["compile", "a.hf", "a.mjs", "--backend=wasm"], "heff compile: --backend is esm, page, node or c, not wasm\n"],
  [["compile", "a.hf", "--backend=node"], "heff compile: expected OUT\n"],
  [["compile", "a.hf", "a.mjs", "--backend=esm", "--name", "x"], "heff compile: an ES module runs nothing: no --name with --backend=esm"],
  [["init", "a", "b"], "heff init: too many words: b\n"],
  [["build", "x"], "heff build: too many words: x\nusage: heff build\n"],
]) {
  const r = heff(...args);
  expect(`refused: ${args.join(" ")}`, r.status === 1 && r.stdout === "" && r.stderr.startsWith(says), r);
}
for (const args of [["help", "compile"], ["compile", "--help"], ["compile", "a.hf", "-h"]]) {
  const r = heff(...args);
  expect(`help: ${args.join(" ")}`, r.status === 0 &&
    r.stdout.startsWith("usage: heff compile FILE OUT [--name NAME] --backend=MODE\n") &&
    r.stdout.includes("\n  -b, --backend=MODE") && r.stdout.includes("(esm, page, node, c)"), r);
}

// version: the heff running, as the package.hf it came with says (the
// repository's, or one beside its bin/); with none there, or one that
// does not read, not known, and where it looked said
const version = /heff = "([^"]*)"/.exec(readFileSync(join(root, "package.hf"), "utf8"))[1];
const versionSaid = heff("version");
expect("version", versionSaid.status === 0 && versionSaid.stdout === `heff ${version}\n`, versionSaid);
mkdirSync(join(work, "lone", "bin"), { recursive: true });
const lone = join(work, "lone", "bin", basename(heffBin));
copyFileSync(heffBin, lone);
chmodSync(lone, 0o755);
const lonePkg = join(realpathSync(join(work, "lone")), "package.hf");
const noPkg = spawnSync(lone, ["version"], { cwd: work, encoding: "utf8" });
expect("version, no package.hf", noPkg.status === 1 && noPkg.stdout === "" &&
  noPkg.stderr === `heff version: not known: there is no ${lonePkg}\n`, noPkg);
writeFileSync(join(work, "lone", "package.hf"), "package = 1\n");
const badPkg = spawnSync(lone, ["version"], { cwd: work, encoding: "utf8" });
expect("version, a package.hf that does not read", badPkg.status === 1 && badPkg.stdout === "" &&
  badPkg.stderr.startsWith(`heff version: not known: ${lonePkg}:1:11: `), badPkg);

// what a word completes to: the commands, a command's flags, a flag's
// values, its files
for (const [words, out] of [
  [["co"], "compile\tFILE compiled, by MODE: esm (an ES module), page (a script running the app NAME in a page), node (a Node.js program running NAME), c (a C program running NAME, for `zig cc`)\ncompletions\ta completion script for SHELL\n"],
  [["compile", "a.hf", "--b"], "--backend\twhat it is compiled to\n"],
  [["compile", "a.hf", "-b", ""], "esm\npage\nnode\nc\n"],
  [["compile", "a.hf", "--backend=n"], "--backend=node\n"],
  [["fmt", "--check", ""], ":files .hf\n"],
  [["init", ""], ":dirs\n"],
  [["completions", ""], "fish\nbash\nzsh\n"],
  [["run", "main.hf", "--"], ""],
]) {
  const r = heff("complete", ...words);
  expect(`complete ${JSON.stringify(words)}`, r.status === 0 && r.stdout === out, r);
}

// the scripts, run by their shells, with `heff` on the path: a line typed
// so far, and what the shell offers
const bin = join(work, "bin");
mkdirSync(bin);
writeFileSync(join(bin, "heff"), `#!/bin/sh\nexec ${JSON.stringify(heffBin)} "$@"\n`);
chmodSync(join(bin, "heff"), 0o755);
const env = { ...process.env, PATH: `${bin}:${process.env.PATH}` };
writeFileSync(join(work, "comp.bash"), heff("completions", "bash").stdout);
// (bash splits `--backend=n` at `=`: its words as bash makes them)
const bashOffers = (line, words) => spawnSync("bash", ["-c",
  `source comp.bash; COMP_LINE=${JSON.stringify(line)}; COMP_POINT=\${#COMP_LINE}; COMP_WORDS=(${words.map((w) => JSON.stringify(w)).join(" ")}); COMP_CWORD=${words.length - 1}; _heff; printf '%s\\n' "\${COMPREPLY[@]}"`],
  { cwd: work, env, encoding: "utf8" });
for (const [line, words, want] of [
  ["heff comp", ["heff", "comp"], ["compile", "completions"]],
  ["heff compile a.hf --b", ["heff", "compile", "a.hf", "--b"], ["--backend"]],
  ["heff compile a.hf --backend ", ["heff", "compile", "a.hf", "--backend", ""], ["esm", "page", "node", "c"]],
  ["heff compile a.hf --backend=n", ["heff", "compile", "a.hf", "--backend", "=", "n"], ["node"]],
  ["heff compile a.hf --backend=", ["heff", "compile", "a.hf", "--backend", "="], ["=esm", "=page", "=node", "=c"]],
  ["heff check cle", ["heff", "check", "cle"], ["clean.hf"]],
]) {
  const r = bashOffers(line, words);
  expect(`bash completes ${JSON.stringify(line)}`, r.status === 0 && JSON.stringify(r.stdout.trim().split("\n").filter(Boolean)) === JSON.stringify(want), r);
}
// (fish and zsh, where they are.  fish reads no one's configuration
// (`--no-config`): a config.fish, or its universal variables
// (`fish_user_paths`), put another heff on the PATH before this one --
// ~/.heff/bin's, which is not what is tested, and which `heff install`
// writing it leaves nothing that runs for a while)
const fishThere = spawnSync("fish", ["--version"]).error === undefined;
if (fishThere) {
  writeFileSync(join(work, "comp.fish"), heff("completions", "fish").stdout);
  const fishOffers = (line) => spawnSync("fish", ["--no-config", "-c", `source comp.fish; complete -C ${JSON.stringify(line)}`], { cwd: work, env, encoding: "utf8" });
  for (const [line, want] of [
    ["heff comp", ["compile", "completions"]],
    ["heff compile a.hf --backend=", ["--backend=esm", "--backend=page", "--backend=node", "--backend=c"]],
    ["heff compile a.hf -b ", ["c", "esm", "node", "page"]],
    ["heff check cle", ["clean.hf"]],
  ]) {
    const r = fishOffers(line);
    // (fish sorts what it offers)
    const got = r.stdout.trim().split("\n").filter(Boolean).map((l) => l.split("\t")[0]).sort();
    expect(`fish completes ${JSON.stringify(line)}`, r.status === 0 && JSON.stringify(got) === JSON.stringify([...want].sort()), r);
  }
}
const zsh = heff("completions", "zsh");
const zshRead = spawnSync("zsh", ["-n"], { input: zsh.stdout });
expect("completions zsh", zsh.status === 0 && zsh.stdout.startsWith("#compdef heff\n") && zsh.stdout.includes("heff complete") &&
  (zshRead.error !== undefined || zshRead.status === 0), zsh.stdout);
const noShell = heff("completions", "tcsh");
expect("completions of no shell", noShell.status === 1 && noShell.stderr.startsWith("heff completions: a shell is fish, bash or zsh, not tcsh"), noShell);

// a diagnostic: in colour where heff is told the standard error is a
// terminal (`HEFF_TTY=1`), not where `NO_COLOR` is set or it is not one;
// a span over lines marked once, in a margin
writeFileSync(join(work, "diag.hf"), [
  "data C = R | G | B", "",
  "name : C -> String", "name c = case c of", "  R -> \"red\"", "  G -> \"green\"", "",
].join("\n"));
const withEnv = (extra, file = "diag.hf") => {
  const env = { ...process.env, ...extra };
  for (const k of ["HEFF_TTY", "NO_COLOR"]) if (!(k in extra)) delete env[k];
  return spawnSync(heffBin, ["check", file], { cwd: work, encoding: "utf8", env });
};
const esc = "\u001b";
const plain = withEnv({});
expect("a diagnostic, not on a terminal", plain.status === 1 && !plain.stderr.includes(esc) &&
  plain.stderr.includes([
    "error[E0317]: no case provided for tag B",
    "4 |   name c = case c of",
    "  |  __________^",
    "5 | |   R -> \"red\"",
    "6 | |   G -> \"green\"",
    "  | |______________^",
  ].join("\n")), plain);
const tty = withEnv({ HEFF_TTY: "1" });
expect("a diagnostic, on a terminal", tty.status === 1 &&
  tty.stderr.includes(`${esc}[1;31merror[E0317]${esc}[0m: no case provided`) &&
  tty.stderr.includes(`${esc}[2m6 |${esc}[0m`) && tty.stderr.includes(`${esc}[1;31m|______________^${esc}[0m`) &&
  tty.stderr.replace(/\u001b\[[0-9;]*m/g, "") === plain.stderr, tty);
const noColor = withEnv({ HEFF_TTY: "1", NO_COLOR: "1" });
expect("a diagnostic, NO_COLOR", noColor.status === 1 && noColor.stderr === plain.stderr, noColor);

// a diagnostic's column is of runes, and so are its marks: one beyond
// U+FFFF is one column, whatever units it takes; a character no token
// starts with is whole
writeFileSync(join(work, "astral.hf"), 's : Int\ns = {- 😀 -} "😀"\n\nt : Int\nt = 1 😀 2\n');
const astral = withEnv({}, "astral.hf");
expect("a diagnostic past a rune beyond U+FFFF", astral.status === 1 &&
  astral.stderr.includes('astral.hf:2:13\nerror[E0311]: expected type Int, but got type String\n2 | s = {- 😀 -} "😀"\n2 |             ^^^\n') &&
  astral.stderr.includes("astral.hf:5:7\nerror[E0150]: unexpected character 😀\n5 | t = 1 😀 2\n5 |       ^\n") &&
  astral.stderr.includes("astral.hf:5:5\nerror[E0302]: tried to apply a value that was not of a function type\n5 | t = 1 😀 2\n5 |     ^^^^^\n") &&
  !astral.stderr.includes("�"), astral);

console.log(`cli: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
