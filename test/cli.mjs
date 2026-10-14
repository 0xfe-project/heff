#!/usr/bin/env node
// The command line: `promote` writes each watch's and doc test's value
// under it, `test` runs the tests and shrinks what fails,
// `fmt` lays files out in place (with `--check`, only names them),
// `version` says which heff runs, and the usage, the completions and an
// unknown command say what they should, on the right stream, with the
// right exit status.
import { spawnSync } from "node:child_process";
import { chmodSync, copyFileSync, cpSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { run } from "./pool.mjs";

const work = mkdtempSync(join(tmpdir(), "heff-cli-"));
const heff = (...args) => run(heffBin, [...args], { cwd: work });

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}`); } };

// promote: an expectation written where there was none, a wrong one
// replaced, a right one kept; then `test` holds
writeFileSync(join(work, "p.hf"), [
  "-- >>> 1 + 2", "",
  "-- >>> (Cons 1 Nil : List Int)", "-- wrong", "",
  "-- >>> not True", "-- False", "",
].join("\n"));
const promoted = await heff("promote", "p.hf");
const text = readFileSync(join(work, "p.hf"), "utf8");
expect("promote", promoted.status === 0 &&
  text.includes("-- >>> 1 + 2\n-- 3\n") && text.includes("-- >>> (Cons 1 Nil : List Int)\n-- [1]\n") &&
  text.includes("-- >>> not True\n-- False\n") && !text.includes("wrong"), { promoted, text });
const tested = await heff("test", "p.hf");
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
const dFailed = await heff("test", "d.hf");
expect("a doc test that does not hold", dFailed.status === 1 &&
  dFailed.stdout.includes("FAIL d.hf:3 (twice): twice 3\n") && dFailed.stdout.includes("0/1 expectations hold"), dFailed);
const dPromoted = await heff("promote", "d.hf");
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
const dTested = await heff("test", "d.hf");
expect("test after promote, doc tests", dTested.status === 0 && dTested.stdout.includes("2/2 expectations hold"), dTested);

// a diagnostic's code is written as `check` and the language server
// write it, by `test` too: a doc test of no definition warned of, W0151
writeFileSync(join(work, "w.hf"), ["-- | >>> 1 + 1", "-- | 2", ""].join("\n"));
const wTested = await heff("test", "w.hf"), wChecked = await heff("check", "w.hf");
expect("test, a warning's code", wTested.status === 0 &&
  (wTested.stdout + wTested.stderr).includes("warning[W0151]: a doc test is of the definition") &&
  wChecked.stderr.includes("warning[W0151]: a doc test is of the definition"), { wTested, wChecked });

// test: the definitions of type `Test` run too.  A property that fails on
// purpose is shrunk to its smallest counterexample, says the seed that
// runs it again, and fails the run; the same seed runs the same runs,
// another seed others, to the same counterexample
writeFileSync(join(work, "props.hf"), [
  "import check", "import co", "import generator as Generator", "",
  "small : Test",
  "small = group \"small\"",
  "  [ unit \"two\" (assertEqual (1 + 1) 2)",
  "  , unit \"three\" (assertEqual (1 + 1) 3)",
  "  , property \"all below 50\" (do",
  "      xs <- forAll (Generator.list 0 20 (Generator.int 0 99))",
  "      assert \"all below 50\" (List.all (\\x -> x < 50) xs))",
  "  , property \"reversed twice\" (do",
  "      xs <- forAll (Generator.list 0 20 (Generator.int 0 99))",
  "      label (if List.null xs then \"empty\" else \"not empty\")",
  "      assertEqual (List.reverse (List.reverse xs)) xs)",
  "  ]", "",
].join("\n"));
const props = await heff("test", "props.hf");
const runsOf = (r) => (r.stdout.match(/all below 50  \(failed after (\d+) runs held/) ?? [])[1];
expect("test runs the tests", props.status === 1 &&
  props.stdout.includes("  ok    small / two\n") &&
  props.stdout.includes("  FAIL  small / three\n        expected 3\n        but got  2\n") &&
  /  FAIL  small \/ all below 50  \(failed after \d+ runs held; shrunk (once|\d+ times); again: --seed 2026\)\n        \[50\]\n        all below 50\n/.test(props.stdout) &&
  /  ok    small \/ reversed twice  \(100 runs, \d+% empty, \d+% not empty\)\n/.test(props.stdout) &&
  props.stdout.includes("FAIL  props.hf  0/0, tests 2/4\n") &&
  props.stdout.endsWith("0/0 expectations hold, 2/4 tests hold, 1/1 files check as they should\n"), props);
const seeded = await heff("test", "--seed", "99", "props.hf");
expect("test --seed", seeded.status === 1 && seeded.stdout.includes("again: --seed 99)\n        [50]\n") &&
  runsOf(seeded) !== undefined && runsOf(seeded) !== runsOf(props), { seeded, props });
expect("test --seed, again", (await heff("test", "props.hf", "-s", "99")).stdout === seeded.stdout, seeded);

// test: an integration test runs outside, a program of its own in a
// directory of its own, killed at its time limit; `--run` selects tests
// by their names, a level each between `/`s, as Go's `-run`; `--update`
// writes the golden texts the tests keep, which are then held to them
writeFileSync(join(work, "outside.hf"), [
  "import check", "import co", "import std.filesystem", "import std.process", "",
  "slow : Test",
  "slow = within 300 (integration \"sleeps\" (do",
  "  r <- run { command = \"sleep\", args = [\"5\"] }",
  "  pure MkUnit))", "",
  "named : Test",
  "named = group \"a\"",
  "  [ unit \"x\" (assertEqual 1 1)",
  "  , integration \"y\" (do",
  "      here <- workingDir",
  "      names <- readDirectory here",
  "      assertEqual names (Just Nil))",
  "  , group \"lsp\" [unit \"hover\" (golden \"hover.txt\" \"a hover\\n\")]",
  "  ]", "",
].join("\n"));
const outside = await heff("test", "outside.hf");
expect("test, integration tests", outside.status === 1 &&
  outside.stdout.includes("  ok    a / x\n") && outside.stdout.includes("  ok    a / y\n") &&
  outside.stdout.includes("  FAIL  sleeps\n        took longer than its time limit, 300 ms: killed\n") &&
  outside.stdout.includes("  FAIL  a / lsp / hover\n        golden outside.golden/hover.txt\n        not kept yet: `heff test --update` writes it\n") &&
  outside.stdout.endsWith("1/1 files check as they should\n") && outside.stdout.includes("tests 2/4\n"), outside);
const selected = await heff("test", "--run", "^a/y$", "outside.hf");
expect("test --run", selected.status === 0 && selected.stdout === "  ok    a / y\nok    outside.hf  0/0, tests 1/1\n0/0 expectations hold, 1/1 tests hold, 1/1 files check as they should\n", selected);
const nested = await heff("test", "-r", "a/lsp", "-j", "1", "outside.hf");
expect("test --run, a level each", nested.status === 1 && nested.stdout.includes("  FAIL  a / lsp / hover\n") && nested.stdout.includes("tests 0/1\n"), nested);
const unread = await heff("test", "--run", "a(", "outside.hf");
expect("test --run, a pattern not read", unread.status === 2 && unread.stderr === "heff test: --run a(: groups, `(...)`, are not read\n", unread);
const updated = await heff("test", "--update", "--run", "a/lsp", "outside.hf");
expect("test --update", updated.status === 0 && updated.stdout.startsWith("wrote outside.golden/hover.txt\n") &&
  readFileSync(join(work, "outside.golden", "hover.txt"), "utf8") === "a hover\n", updated);
expect("test, a golden text held", (await heff("test", "--run", "a/lsp", "outside.hf")).status === 0, null);
writeFileSync(join(work, "outside.golden", "hover.txt"), "another hover\n");
const differs = await heff("test", "--run", "a/lsp", "outside.hf");
expect("test, a golden text that differs", differs.status === 1 &&
  differs.stdout.includes("        golden outside.golden/hover.txt\n        expected \"another hover\\n\"\n        but got  \"a hover\\n\"\n"), differs);

// test: a file that does not check fails the run, though every
// expectation counted holds: it is named in a list above the last line,
// a line each, where its first error is -- one with a type error, its
// watch evaluated still, and one that does not parse
writeFileSync(join(work, "mistyped.hf"), ["-- >>> 1 + 1", "-- 2", "", "oops : Int", "oops = True", ""].join("\n"));
writeFileSync(join(work, "unparsed.hf"), ["x : Int", "x = (1 +", ""].join("\n"));
const mistyped = await heff("test", "p.hf", "mistyped.hf");
expect("test, a file that does not check", mistyped.status === 1 &&
  mistyped.stdout.includes("FAIL  mistyped.hf  1/1\n") &&
  mistyped.stdout.endsWith("files that do not check as they should:\n" +
    "  mistyped.hf:5:8: error[E0314]: expected type Int, but got the tag True\n" +
    "4/4 expectations hold, 1/2 files check as they should\n"), mistyped);
const unchecked = await heff("test", "p.hf", "mistyped.hf", "unparsed.hf");
expect("test, files that do not check", unchecked.status === 1 &&
  unchecked.stdout.includes("FAIL  unparsed.hf  0/0\n") &&
  unchecked.stdout.endsWith("files that do not check as they should:\n" +
    "  mistyped.hf:5:8: error[E0314]: expected type Int, but got the tag True\n" +
    "  unparsed.hf:2:9: error[E0150]: expected an expression, not the end of the declaration\n" +
    "4/4 expectations hold, 1/3 files check as they should\n"), unchecked);

// test: a file whose first line is `-- test: expect errors` (a test of
// the errors said) passes by having errors, not said; it fails if it
// checks; the line anywhere else is a comment like another
writeFileSync(join(work, "wants.hf"), ["-- test: expect errors", "oops : Int", "oops = True", ""].join("\n"));
writeFileSync(join(work, "checks.hf"), ["-- test: expect errors", "fine : Int", "fine = 1", ""].join("\n"));
writeFileSync(join(work, "below.hf"), ["", "-- test: expect errors", "oops : Int", "oops = True", ""].join("\n"));
const wanted = await heff("test", "p.hf", "wants.hf");
expect("test, a file that expects errors and has them", wanted.status === 0 &&
  wanted.stdout.includes("ok    wants.hf  errors, as expected\n") &&
  !(wanted.stdout + wanted.stderr).includes("E0314") &&
  wanted.stdout.endsWith("\n3/3 expectations hold, 2/2 files check as they should (1 with expected errors)\n"), wanted);
const unwanted = await heff("test", "wants.hf", "checks.hf");
expect("test, a file that expects errors but checks", unwanted.status === 1 &&
  unwanted.stdout.includes("FAIL  checks.hf  checks, but expects errors\n") &&
  unwanted.stdout.endsWith("files that do not check as they should:\n" +
    "  checks.hf: checks, but its first line expects errors\n" +
    "0/0 expectations hold, 1/2 files check as they should (1 with expected errors)\n"), unwanted);
const below = await heff("test", "below.hf");
expect("test, `-- test: expect errors` not the first line", below.status === 1 &&
  below.stdout.endsWith("  below.hf:4:8: error[E0314]: expected type Int, but got the tag True\n" +
    "0/0 expectations hold, 0/1 files check as they should\n"), below);

// fmt: --check names what is not laid out (and fails), leaves it; without
// it, lays it out; a file that does not parse is refused, untouched
writeFileSync(join(work, "messy.hf"), "f :  Int -> Int\nf x   =  x + 1\n");
writeFileSync(join(work, "clean.hf"), "f : Int\nf = 1\n");
writeFileSync(join(work, "broken.hf"), "f : Int ->\n");
const checked = await heff("fmt", "--check", "messy.hf", "clean.hf");
expect("fmt --check", checked.status === 1 && checked.stdout === "messy.hf\n" &&
  readFileSync(join(work, "messy.hf"), "utf8") === "f :  Int -> Int\nf x   =  x + 1\n", checked);
const laid = await heff("fmt", "messy.hf", "clean.hf");
expect("fmt", laid.status === 0 && readFileSync(join(work, "messy.hf"), "utf8") === "f : Int -> Int\nf x = x + 1\n" &&
  readFileSync(join(work, "clean.hf"), "utf8") === "f : Int\nf = 1\n", laid);
const refused = await heff("fmt", "broken.hf");
expect("fmt of what does not parse", refused.status === 1 && refused.stderr.includes("broken.hf not formatted: the file does not parse") &&
  readFileSync(join(work, "broken.hf"), "utf8") === "f : Int ->\n", refused);
// fmt: a definition under `-- fmt: skip` is as it is written, for
// `--check` too, and left so; what is around it is laid out
const keptText = "-- fmt: skip\nm : List Int\nm = [ 1, 0,\n      0, 1 ]\n";
writeFileSync(join(work, "kept.hf"), keptText);
writeFileSync(join(work, "around.hf"), keptText + "n  =  1\n");
const keptCheck = await heff("fmt", "--check", "kept.hf", "around.hf");
expect("fmt --check of a definition kept", keptCheck.status === 1 && keptCheck.stdout === "around.hf\n", keptCheck);
const keptLaid = await heff("fmt", "kept.hf", "around.hf");
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
const cut = await heff("fmt", "uses.hf");
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
// (with no words, not on a terminal: the usage, as `help` says it --
// on one, its commands' app)
const commands = ["check", "run", "test", "promote", "compile", "explain", "type-at", "fmt", "init", "get", "build", "install", "publish", "dev", "lsp", "version", "help", "completions"];
for (const args of [[], ["help"], ["--help"]]) {
  const r = await heff(...args);
  expect(`usage (${args.join(" ") || "no arguments"})`, r.status === 0 && r.stdout.startsWith("usage: heff COMMAND ...\n") &&
    commands.every((c) => r.stdout.includes(`\n  ${c}`)) && !r.stdout.includes("dump-core") && !r.stdout.includes("dev-pages"), r);
}
const noSuch = await heff("no-such-command");
expect("refused: no-such-command", noSuch.status === 1 && noSuch.stdout === "" &&
  noSuch.stderr.startsWith("heff: no command no-such-command\nusage: heff COMMAND ..."), noSuch);
for (const [args, says] of [
  [["fmt"], "heff fmt: expected FILE...\nusage: heff fmt FILE... [--check]\n"],
  [["check"], "heff check: expected FILE\nusage: heff check FILE\n"],
  [["fmt", "--chek", "x.hf"], "heff fmt: no flag --chek; --check?\nusage: heff fmt FILE... [--check]\n"],
  [["compile", "a.hf", "a.mjs"], "heff compile: --backend=MODE is wanted\n"],
  [["compile", "a.hf", "a.mjs", "--backend=wasm"], "heff compile: --backend is esm, page or c, not wasm\n"],
  [["compile", "a.hf", "a.mjs", "--backend=node"], "heff compile: --backend is esm, page or c, not node\n"],
  [["compile", "a.hf", "--backend=c"], "heff compile: expected OUT\n"],
  [["compile", "a.hf", "a.mjs", "--backend=esm", "--name", "x"], "heff compile: an ES module runs nothing: no --name with --backend=esm"],
  [["init", "a", "b"], "heff init: too many words: b\n"],
  [["build", "x"], "heff build: too many words: x\nusage: heff build\n"],
]) {
  const r = await heff(...args);
  expect(`refused: ${args.join(" ")}`, r.status === 1 && r.stdout === "" && r.stderr.startsWith(says), r);
}
for (const args of [["help", "compile"], ["compile", "--help"], ["compile", "a.hf", "-h"]]) {
  const r = await heff(...args);
  expect(`help: ${args.join(" ")}`, r.status === 0 &&
    r.stdout.startsWith("usage: heff compile FILE OUT [--name NAME] --backend=MODE\n") &&
    r.stdout.includes("\n  -b, --backend=MODE") && r.stdout.includes("(esm, page, c)"), r);
}

// version: the heff running, as the package.hf it came with says (the
// repository's, or one beside its bin/); with none there, or one that
// does not read, not known, and where it looked said
const version = /heff = "([^"]*)"/.exec(readFileSync(join(root, "package.hf"), "utf8"))[1];
const versionSaid = await heff("version");
expect("version", versionSaid.status === 0 && versionSaid.stdout === `heff ${version}\n`, versionSaid);
mkdirSync(join(work, "lone", "bin"), { recursive: true });
const lone = join(work, "lone", "bin", basename(heffBin));
copyFileSync(heffBin, lone);
chmodSync(lone, 0o755);
const lonePkg = join(realpathSync(join(work, "lone")), "package.hf");
const noPkg = await run(lone, ["version"], { cwd: work });
expect("version, no package.hf", noPkg.status === 1 && noPkg.stdout === "" &&
  noPkg.stderr === `heff version: not known: there is no ${lonePkg}\n`, noPkg);
writeFileSync(join(work, "lone", "package.hf"), "package = 1\n");
const badPkg = await run(lone, ["version"], { cwd: work });
expect("version, a package.hf that does not read", badPkg.status === 1 && badPkg.stdout === "" &&
  badPkg.stderr.startsWith(`heff version: not known: ${lonePkg}:1:11: `), badPkg);

// what a word completes to: the commands, a command's flags, a flag's
// values, its files
for (const [words, out] of [
  [["co"], "compile\tFILE compiled, by MODE: esm (an ES module), page (a script running the app NAME in a page), c (a C program running NAME, for `zig cc`)\ncompletions\ta completion script for SHELL\n"],
  [["compile", "a.hf", "--b"], "--backend\twhat it is compiled to\n"],
  [["compile", "a.hf", "-b", ""], "esm\npage\nc\n"],
  [["compile", "a.hf", "--backend=p"], "--backend=page\n"],
  [["fmt", "--check", ""], ":files .hf\n"],
  [["init", ""], ":dirs\n"],
  [["completions", ""], "fish\nbash\nzsh\n"],
  [["dev", "--po"], "--port\twhere the pages are served: http://127.0.0.1:PORT/ (8000)\n"],
  [["de"], "dev\tthe project built as `heff build` builds it, its C programs (the APPs, else all) run and its pages served, then built and run again at each change of its files\n"],
  [["run", "main.hf", "--"], ""],
]) {
  const r = await heff("complete", ...words);
  expect(`complete ${JSON.stringify(words)}`, r.status === 0 && r.stdout === out, r);
}

// the scripts, run by their shells, with `heff` on the path: a line typed
// so far, and what the shell offers
const bin = join(work, "bin");
mkdirSync(bin);
writeFileSync(join(bin, "heff"), `#!/bin/sh\nexec ${JSON.stringify(heffBin)} "$@"\n`);
chmodSync(join(bin, "heff"), 0o755);
const env = { ...process.env, PATH: `${bin}:${process.env.PATH}` };
writeFileSync(join(work, "comp.bash"), (await heff("completions", "bash")).stdout);
// (bash splits `--backend=n` at `=`: its words as bash makes them)
const bashOffers = (line, words) => run("bash", ["-c",
  `source comp.bash; COMP_LINE=${JSON.stringify(line)}; COMP_POINT=\${#COMP_LINE}; COMP_WORDS=(${words.map((w) => JSON.stringify(w)).join(" ")}); COMP_CWORD=${words.length - 1}; _heff; printf '%s\\n' "\${COMPREPLY[@]}"`],
  { cwd: work, env });
for (const [line, words, want] of [
  ["heff comp", ["heff", "comp"], ["compile", "completions"]],
  ["heff compile a.hf --b", ["heff", "compile", "a.hf", "--b"], ["--backend"]],
  ["heff compile a.hf --backend ", ["heff", "compile", "a.hf", "--backend", ""], ["esm", "page", "c"]],
  ["heff compile a.hf --backend=p", ["heff", "compile", "a.hf", "--backend", "=", "p"], ["page"]],
  ["heff compile a.hf --backend=", ["heff", "compile", "a.hf", "--backend", "="], ["=esm", "=page", "=c"]],
  ["heff check cle", ["heff", "check", "cle"], ["clean.hf"]],
]) {
  const r = await bashOffers(line, words);
  expect(`bash completes ${JSON.stringify(line)}`, r.status === 0 && JSON.stringify(r.stdout.trim().split("\n").filter(Boolean)) === JSON.stringify(want), r);
}
// (fish and zsh, where they are.  fish reads no one's configuration
// (`--no-config`): a config.fish, or its universal variables
// (`fish_user_paths`), put another heff on the PATH before this one --
// ~/.heff/bin's, which is not what is tested, and which `heff install`
// writing it leaves nothing that runs for a while)
const fishThere = spawnSync("fish", ["--version"]).error === undefined;
if (fishThere) {
  writeFileSync(join(work, "comp.fish"), (await heff("completions", "fish")).stdout);
  const fishOffers = (line) => run("fish", ["--no-config", "-c", `source comp.fish; complete -C ${JSON.stringify(line)}`], { cwd: work, env });
  for (const [line, want] of [
    ["heff comp", ["compile", "completions"]],
    ["heff compile a.hf --backend=", ["--backend=esm", "--backend=page", "--backend=c"]],
    ["heff compile a.hf -b ", ["c", "esm", "page"]],
    ["heff check cle", ["clean.hf"]],
  ]) {
    const r = await fishOffers(line);
    // (fish sorts what it offers)
    const got = r.stdout.trim().split("\n").filter(Boolean).map((l) => l.split("\t")[0]).sort();
    expect(`fish completes ${JSON.stringify(line)}`, r.status === 0 && JSON.stringify(got) === JSON.stringify([...want].sort()), r);
  }
}
const zsh = await heff("completions", "zsh");
const zshRead = spawnSync("zsh", ["-n"], { input: zsh.stdout });
expect("completions zsh", zsh.status === 0 && zsh.stdout.startsWith("#compdef heff\n") && zsh.stdout.includes("heff complete") &&
  (zshRead.error !== undefined || zshRead.status === 0), zsh.stdout);
const noShell = await heff("completions", "tcsh");
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
  return run(heffBin, ["check", file], { cwd: work, env });
};
const esc = "\u001b";
const plain = await withEnv({});
expect("a diagnostic, not on a terminal", plain.status === 1 && !plain.stderr.includes(esc) &&
  plain.stderr.includes([
    "error[E0317]: no case provided for tag B",
    "4 |   name c = case c of",
    "  |  __________^",
    "5 | |   R -> \"red\"",
    "6 | |   G -> \"green\"",
    "  | |______________^",
  ].join("\n")), plain);
const tty = await withEnv({ HEFF_TTY: "1" });
expect("a diagnostic, on a terminal", tty.status === 1 &&
  tty.stderr.includes(`${esc}[1;31merror[E0317]${esc}[0m: no case provided`) &&
  tty.stderr.includes(`${esc}[2m6 |${esc}[0m`) && tty.stderr.includes(`${esc}[1;31m|______________^${esc}[0m`) &&
  tty.stderr.replace(/\u001b\[[0-9;]*m/g, "") === plain.stderr, tty);
const noColor = await withEnv({ HEFF_TTY: "1", NO_COLOR: "1" });
expect("a diagnostic, NO_COLOR", noColor.status === 1 && noColor.stderr === plain.stderr, noColor);

// a diagnostic's column is of runes, and so are its marks: one beyond
// U+FFFF is one column, whatever units it takes; a character no token
// starts with is whole
writeFileSync(join(work, "astral.hf"), 's : Int\ns = {- 😀 -} "😀"\n\nt : Int\nt = 1 😀 2\n');
const astral = await withEnv({}, "astral.hf");
expect("a diagnostic past a rune beyond U+FFFF", astral.status === 1 &&
  astral.stderr.includes('astral.hf:2:13\nerror[E0311]: expected type Int, but got type String\n2 | s = {- 😀 -} "😀"\n2 |             ^^^\n') &&
  astral.stderr.includes("astral.hf:5:7\nerror[E0150]: unexpected character 😀\n5 | t = 1 😀 2\n5 |       ^\n") &&
  astral.stderr.includes("astral.hf:5:5\nerror[E0302]: tried to apply a value as a function, where its type is not known\n5 | t = 1 😀 2\n5 |     ^^^^^\n") &&
  !astral.stderr.includes("�"), astral);

// heff's own lib/ is the library, whatever library the heff running has
// (`HEFF_LIB` elsewhere, as an installed heff's is): its primitives its
// own, declared once; the lib/ of a project that is not heff is none
{
  const { HEFF_LIB: _lib, ...env } = process.env;
  const noHome = join(work, "no-home");
  mkdirSync(noHome, { recursive: true });
  const checkWith = (bin, more, f) => run(bin, ["check", f], { cwd: work, env: { ...env, HEFF_HOME: noHome, ...more } });
  const elsewhere = join(work, "elsewhere", "lib");
  cpSync(join(root, "lib"), elsewhere, { recursive: true });
  const checkIn = (f) => checkWith(heffBin, { HEFF_LIB: elsewhere }, f);
  const own = await checkIn(join(root, "lib", "float32.hf"));
  expect("heff's own lib/, the library elsewhere", own.status === 0 && !/E033[56]/.test(own.stderr), own);
  // a primitive the library declares at a type the runtime does not have
  // it at is what type checking cannot see: the interpreter, given a
  // value of the wrong shape, says it met a bug of the compiler -- a
  // watch that meets one fails, whatever its expectation, as a test
  // does, and so does `dump-eval`, saying where
  writeFileSync(join(elsewhere, "wrong.hf"), [
    "import check", "",
    "primitive floatSin : Int -> Int", "",
    "-- >>> floatSin 4", "-- <compiler bug: a primitive given what is not a `Float64`>", "",
    "-- >>> floatSin 5", "",
    "root : Int", "root = floatSin 4", "",
    "bugged : Test", "bugged = unit \"met a bug\" (assert \"never\" (floatSin 4 == 2))", "",
  ].join("\n"));
  const inElsewhere = (...args) => run(heffBin, args, { cwd: work, env: { ...env, HEFF_HOME: noHome, HEFF_LIB: elsewhere } });
  const bug = "<compiler bug: a primitive given what is not a `Float64`>";
  const bugTested = await inElsewhere("test", join(elsewhere, "wrong.hf"));
  expect("a compiler bug met, by a watch and a test", bugTested.status === 1 &&
    bugTested.stdout.includes(`but got  ${bug}\n`) && bugTested.stdout.includes(`FAIL  bugged\n        ${bug}\n`) &&
    bugTested.stdout.includes("0/2 expectations hold, 0/1 tests hold"), bugTested);
  const bugEvaluated = await inElsewhere("dump-eval", join(elsewhere, "wrong.hf"));
  expect("a compiler bug met, evaluated", bugEvaluated.status === 1 && bugEvaluated.stdout.includes(`root = ${bug}\n`), bugEvaluated);
  const other = join(work, "other");
  mkdirSync(join(other, "lib"), { recursive: true });
  writeFileSync(join(other, "package.hf"), readFileSync(join(root, "package.hf"), "utf8").replace(/module = "[^"]*"/, 'module = "example.com/other"'));
  writeFileSync(join(other, "lib", "sin.hf"), "primitive floatSin : Float64 -> Float64\n");
  const notHeff = await checkIn(join(other, "lib", "sin.hf"));
  expect("another project's lib/", notHeff.status === 1 && notHeff.stderr.includes("error[E0335]"), notHeff);
  // a heff not built in the repository (installed, another checkout's):
  // the repository is heff's own where its heff home says it was
  // installed from it (`source`), and nowhere else
  const home = join(work, "home");
  const installed = join(home, "bin", basename(heffBin));
  mkdirSync(join(home, "bin"), { recursive: true });
  copyFileSync(heffBin, installed);
  chmodSync(installed, 0o755);
  cpSync(join(root, "lib"), join(home, "lib"), { recursive: true });
  copyFileSync(join(root, "package.hf"), join(home, "package.hf"));
  const unsaid = await checkWith(installed, { HEFF_HOME: home }, join(root, "lib", "float32.hf"));
  expect("heff's own lib/, the heff home not installed from it", unsaid.status === 1 && unsaid.stderr.includes("error[E0347]: the package `heff` is heff's own"), unsaid);
  writeFileSync(join(home, "source"), root + "\n");
  const said = await checkWith(installed, { HEFF_HOME: home }, join(root, "lib", "float32.hf"));
  expect("heff's own lib/, the heff home installed from it", said.status === 0 && !/E03(3[56]|47)/.test(said.stderr), said);
  // (the heff home's own package.hf names heff: its lib/ is the library
  // a heff given it, `HEFF_LIB`, has)
  const ofHome = await checkWith(heffBin, { HEFF_LIB: join(home, "lib") }, join(home, "lib", "float32.hf"));
  expect("the heff home's lib/, the library", ofHome.status === 0 && !/E03(3[56]|47)/.test(ofHome.stderr), ofHome);
}

console.log(`cli: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
