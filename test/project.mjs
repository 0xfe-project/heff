#!/usr/bin/env node
// Projects (compiler/project.hf, compiler/load.hf): `heff init` makes one
// whose watches hold; a project's modules are under src/, a file outside
// it (a test) may import the project's modules and helpers beside it,
// what is in src/ may not import those; a module found twice, a
// package.hf that does not read and one for another heff are errors.
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const work = mkdtempSync(join(tmpdir(), "heff-project-"));
const heff = (...args) => spawnSync("node", [join(root, "output", "heff.mjs"), ...args], { cwd: work, encoding: "utf8" });
const write = (path, text) => { mkdirSync(dirname(join(work, path)), { recursive: true }); writeFileSync(join(work, path), text); };
const version = JSON.parse(readFileSync(join(root, "package.json"), "utf8")).version;

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

// init: package.hf (for this heff), .gitignore, src/hello/main.hf, whose
// watch holds; again, nothing is written
const made = heff("init", "hello");
const pkg = readFileSync(join(work, "hello", "package.hf"), "utf8");
expect("init", made.status === 0 && pkg.includes(`heff = "${version}"`) && pkg.includes(`module = "hello"`) &&
  readFileSync(join(work, "hello", ".gitignore"), "utf8").includes(".heff/"), made);
const tested = heff("test", "hello/src/hello/main.hf");
expect("init's main.hf", tested.status === 0 && tested.stdout.includes("1/1 expectations hold"), tested);
const checked = heff("check", "hello/package.hf");
expect("package.hf is heff, of type Package", checked.status === 0 && checked.stdout.includes("package : Package"), checked);
const again = heff("init", "hello");
expect("init over a project", again.status === 1 && again.stderr.includes("already; nothing written"), again);
const badName = heff("init", "hello-world");
expect("init of a name that is no name", badName.status === 1 && badName.stderr.includes("cannot name a package"), badName);

// a project's module imports another, by its name under src/
write("hello/src/hello/words.hf", "import prelude\n\nword : String\nword = \"hi\"\n");
write("hello/src/hello/greet.hf", "import hello.words\nimport prelude\n\ngreet : String\ngreet = word ++ \"!\"\n");
const greet = heff("check", "hello/src/hello/greet.hf");
expect("a module of the project", greet.status === 0 && greet.stdout.includes("greet : String"), greet);

// a test imports the project's modules, and a helper beside it, named
// from the root; a module of src/ cannot import the helper
write("hello/test/helper.hf", "import prelude\n\nshout : String -> String\nshout s = s ++ \"!!\"\n");
write("hello/test/use.hf", "import hello.greet\nimport prelude\nimport test.helper\n\nloud : String\nloud = shout greet\n");
const use = heff("check", "hello/test/use.hf");
expect("a test, with a helper", use.status === 0 && use.stdout.includes("loud : String"), use);
write("hello/src/hello/reach.hf", "import test.helper\n\nx : String -> String\nx = shout\n");
const reach = heff("check", "hello/src/hello/reach.hf");
expect("src/ does not import what is outside it", reach.status === 1 &&
  reach.stderr.includes("no module test.helper (looked for hello/src/test/helper.hf, "), reach);

// a module of the project with a library module's name
write("hello/src/prelude.hf", "x : Int\nx = 1\n");
const clash = heff("check", "hello/src/hello/words.hf");
expect("a module twice", clash.status === 1 && clash.stderr.includes("module prelude is more than one file: hello/src/prelude.hf and "), clash);

// package.hf that does not read; one for another heff
write("bad/package.hf", "package =\n  { module = \"bad\"\n  , heff = \"0.1\"\n  , require = []\n  , replace = []\n  }\n");
write("bad/src/bad/m.hf", "x : Int\nx = 1\n");
const bad = heff("check", "bad/src/bad/m.hf");
expect("a package.hf that does not read", bad.status === 1 &&
  bad.stderr.includes("bad/package.hf:3:12: `0.1` is no version: write `a.b.c`, three numbers"), bad);
write("old/package.hf", "package =\n  { module = \"old\"\n  , heff = \"0.99.0\"\n  , require = []\n  , replace = []\n  }\n");
write("old/src/old/m.hf", "x : Int\nx = 1\n");
const old = heff("check", "old/src/old/m.hf");
expect("a project for another heff", old.status === 1 &&
  old.stderr.includes(`package.hf: the project is for heff 0.99.0 (the same 0.x), and this heff is ${version}`), old);

console.log(`project: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
