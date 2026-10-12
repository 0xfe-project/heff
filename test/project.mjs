#!/usr/bin/env node
// Projects (src/heff/build/project.hf, src/heff/build/load.hf): `heff init` makes one
// that builds, its watches and tests holding; a project's modules are
// under src/, a file outside it (a test) may import the project's
// modules and helpers beside it, what is in src/ may not import those; a
// module found twice, a package.hf that does not read and one for
// another heff are errors.
// Packages: given by directories, fetched by `heff get` (git, MVS,
// heff.sum), published by `heff publish`.
import { spawnSync } from "node:child_process";
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { run } from "./pool.mjs";

const work = mkdtempSync(join(tmpdir(), "heff-project-"));
// (packages are fetched into the test's own cache)
process.env.HEFF_MODCACHE = join(work, "mod");
const heff = (...args) => run(heffBin, [...args], { cwd: work });
const write = (path, text) => { mkdirSync(dirname(join(work, path)), { recursive: true }); writeFileSync(join(work, path), text); };
const version = /heff = "([^"]*)"/.exec(readFileSync(join(root, "package.hf"), "utf8"))[1];

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

// (heff itself installed -- looked at last -- begun now, the rest done
// meanwhile: it builds heff, which takes longest.  It takes one slot of
// the tests' budget, as a process does, though zig builds heff's parts as
// many at once as there are cores: it compiles heff to C first, on one,
// and a slot for each core would be held idle meanwhile -- or, all of
// them wanted, wait for those held long by others)
const { HEFF_LIB: _heffLib, ...installEnv } = process.env;
const heffHome = join(work, "heff-home");
const selfInstalling = run(heffBin, ["install"], { cwd: root, env: { ...installEnv, HEFF_HOME: heffHome } });

// init: package.hf (for this heff), .gitignore, a module
// (src/hello/greeting.hf) whose watch holds, the program hello using it
// (app/hello/), built, and a test of it (test/greeting.hf) that holds;
// again, nothing is written
const made = await heff("init", "hello");
const pkg = readFileSync(join(work, "hello", "package.hf"), "utf8");
const inited = ["package.hf", "src/hello/greeting.hf", "app/hello/main.hf", "app/hello/build.hf", "test/greeting.hf"];
expect("init", made.status === 0 && pkg.includes(`heff = "${version}"`) && pkg.includes(`module = "hello"`) &&
  readFileSync(join(work, "hello", ".gitignore"), "utf8") === ".heff/\n" && inited.every((f) => existsSync(join(work, "hello", f))), made);
const laid = await heff("fmt", "--check", ...inited.map((f) => `hello/${f}`));
expect("init's files are laid out", laid.status === 0, laid);
const tested = await heff("test", "hello/src/hello/greeting.hf");
expect("init's module", tested.status === 0 && tested.stdout.includes("1/1 expectations hold"), tested);
const initTests = await run(heffBin, ["test"], { cwd: join(work, "hello") });
expect("init's test", initTests.status === 0 && initTests.stdout.includes("1/1 tests hold"), initTests);
const initBuilt = await run(heffBin, ["build"], { cwd: join(work, "hello") });
expect("init's program, built", initBuilt.status === 0 && initBuilt.stdout.includes("wrote .heff/bin/hello") &&
  (await run(join(work, "hello", ".heff", "bin", "hello"), [])).stdout === "Hello, world!\n", initBuilt);
// (with no file: the project's test/, run where it is; a file in a
// directory starting with `.` or in a project of its own passed over;
// nothing outside test/, and no test/, nothing to test)
write("hello/test/watch.hf", "import hello.greeting\n\n-- >>> greeting \"watch\"\n-- \"Hello, watch!\"\n");
write("hello/test/.hidden/no.hf", "x : Int\nx = \n");
write("hello/test/vendor/other/package.hf", "import manifest\n\npackage : Package\npackage = { module = \"other\", heff = \"0.0.1\", require = [], replace = [] }\n");
write("hello/test/vendor/other/no.hf", "x : Int\nx = \n");
write("hello/elsewhere/no.hf", "x : Int\nx = \n");
const inHello = await run(heffBin, ["test"], { cwd: join(work, "hello") });
expect("test with no file: the project's test/", inHello.status === 0 && inHello.stdout.includes("test/greeting.hf") &&
  inHello.stdout.includes("test/watch.hf") && !inHello.stdout.includes("src/hello/greeting.hf") && !inHello.stdout.includes("no.hf") &&
  inHello.stdout.includes("1/1 expectations hold, 1/1 tests hold"), inHello);
rmSync(join(work, "hello", "test"), { recursive: true });
rmSync(join(work, "hello", "elsewhere"), { recursive: true });
const noTests = await run(heffBin, ["test"], { cwd: join(work, "hello") });
expect("test with no file and no test/", noTests.status === 1 && noTests.stderr.includes("no test/ here"), noTests);
const checked = await heff("check", "hello/package.hf");
expect("package.hf is heff, of type Package", checked.status === 0 && checked.stdout.includes("package : Package"), checked);
const again = await heff("init", "hello");
expect("init over a project", again.status === 1 && again.stderr.includes("already; nothing written"), again);
const badName = await heff("init", "hello-world");
expect("init of a name that is no name", badName.status === 1 && badName.stderr.includes("cannot name a package"), badName);

// a project's module imports another, by its name under src/
write("hello/src/hello/words.hf", "word : String\nword = \"hi\"\n");
write("hello/src/hello/greet.hf", "import hello.words\n\ngreet : String\ngreet = word ^ \"!\"\n");
const greet = await heff("check", "hello/src/hello/greet.hf");
expect("a module of the project", greet.status === 0 && greet.stdout.includes("greet : String"), greet);

// a test imports the project's modules, and a helper beside it, named
// from the root; a module of src/ cannot import the helper
write("hello/test/helper.hf", "shout : String -> String\nshout s = s ^ \"!!\"\n");
write("hello/test/use.hf", "import hello.greet\nimport test.helper\n\nloud : String\nloud = shout greet\n");
const use = await heff("check", "hello/test/use.hf");
expect("a test, with a helper", use.status === 0 && use.stdout.includes("loud : String"), use);
write("hello/src/hello/reach.hf", "import test.helper\n\nx : String -> String\nx = shout\n");
const reach = await heff("check", "hello/src/hello/reach.hf");
expect("src/ does not import what is outside it", reach.status === 1 &&
  reach.stderr.includes("no module test.helper (looked for hello/src/test/helper.hf, "), reach);

// a module whose import is written in another case than its file (or a
// directory on its path) is named: an error where the file system finds
// it all the same; two names that differ only in case, where it does not
write("hello/src/hello/Shout.hf", "up : String\nup = \"HI\"\n");
write("hello/src/hello/useshout.hf", "import hello.shout\n\nu : String\nu = up\n");
const caseBlind = existsSync(join(work, "hello/src/hello/shout.hf"));
const shout = await heff("check", "hello/src/hello/useshout.hf");
expect("a module found by a case-different name", shout.status === 1 && (caseBlind
  ? shout.stderr.includes("importing hello.shout: hello/src/hello/shout.hf is named Shout.hf on the disk: a file system that tells case apart finds no shout.hf")
  : shout.stderr.includes("no module hello.shout")), shout);
write("hello/src/hello/useshout.hf", "import Hello.Shout\n\nu : String\nu = up\n");
const shoutDir = await heff("check", "hello/src/hello/useshout.hf");
expect("a directory found by a case-different name", shoutDir.status === 1 && (caseBlind
  ? shoutDir.stderr.includes("hello/src/Hello is named hello on the disk")
  : shoutDir.stderr.includes("no module Hello.Shout")), shoutDir);
write("hello/src/hello/useshout.hf", "import hello.Shout\n\nu : String\nu = up\n");
const shoutOk = await heff("check", "hello/src/hello/useshout.hf");
expect("a module named as on the disk", shoutOk.status === 0 && shoutOk.stdout.includes("u : String"), shoutOk);
if (!caseBlind) {
  write("hello/src/hello/shout.hf", "x : Int\nx = 1\n");
  const twoCases = await heff("check", "hello/src/hello/useshout.hf");
  expect("two names that differ only in case", twoCases.status === 1 &&
    twoCases.stderr.includes("hello/src/hello holds Shout.hf, shout.hf: names that differ only in case"), twoCases);
  rmSync(join(work, "hello/src/hello/shout.hf"));
}
rmSync(join(work, "hello/src/hello/Shout.hf")); rmSync(join(work, "hello/src/hello/useshout.hf"));

// a module of the project with a library module's name
write("hello/src/prelude.hf", "x : Int\nx = 1\n");
const clash = await heff("check", "hello/src/hello/words.hf");
expect("a module twice", clash.status === 1 && clash.stderr.includes("module prelude is more than one file: hello/src/prelude.hf and "), clash);
rmSync(join(work, "hello/src/prelude.hf"));
// ... loaded from neither, the file to rename said: what is used of it,
// `List.length`, is no error of its own -- in a project, and in a
// program of no project, beside its `main.hf`
write("hello/src/list.hf", "x : Int\nx = 1\n");
write("hello/src/hello/len.hf", "n : Int\nn = List.length [1, 2]\n");
const listClash = await heff("check", "hello/src/hello/len.hf");
expect("a module of the library's name, in a project", listClash.status === 1 &&
  listClash.stderr.includes("module list is more than one file: hello/src/list.hf and ") &&
  listClash.stderr.includes(", so it is loaded from neither: rename hello/src/list.hf, and the imports of it, so that the name is one file's") &&
  !listClash.stderr.includes("error["), listClash);
rmSync(join(work, "hello/src/list.hf")); rmSync(join(work, "hello/src/hello/len.hf"));
write("loose/list.hf", "x : Int\nx = 1\n");
write("loose/main.hf", "n : Int\nn = List.length [1, 2]\n");
const looseClash = await heff("check", "loose/main.hf");
expect("a module of the library's name, beside a program", looseClash.status === 1 &&
  looseClash.stderr.includes("module list is more than one file: loose/list.hf and ") &&
  looseClash.stderr.includes("rename loose/list.hf") && !looseClash.stderr.includes("error["), looseClash);

// package.hf that does not read; one for another heff
write("bad/package.hf", "package =\n  { module = \"bad\"\n  , heff = \"0.1\"\n  , require = []\n  , replace = []\n  }\n");
write("bad/src/bad/m.hf", "x : Int\nx = 1\n");
const bad = await heff("check", "bad/src/bad/m.hf");
expect("a package.hf that does not read", bad.status === 1 &&
  bad.stderr.includes("bad/package.hf:3:12: `0.1` is no version: write `a.b.c`, three numbers"), bad);
write("old/package.hf", "package =\n  { module = \"old\"\n  , heff = \"0.99.0\"\n  , require = []\n  , replace = []\n  }\n");
write("old/src/old/m.hf", "x : Int\nx = 1\n");
const old = await heff("check", "old/src/old/m.hf");
expect("a project for another heff", old.status === 1 &&
  old.stderr.includes(`package.hf: the project is for heff 0.99.0 (the same 0.x), and this heff is ${version}`), old);

// packages: a required package given by a directory (`replace`), its
// modules imported by the name the project requires it by, its own
// imports by its own package.hf
// (a list field of a manifest: left out where it is empty, its default)
const listField = (x, items) => items.length ? `, ${x} = [${items.join(", ")}]` : "";
const manifestOf = (path, requires = [], heff = version) => {
  const req = requires.map(([name, p]) => `{ name = "${name}", path = "${p}", version = "1.0.0" }`);
  const rep = requires.filter(([, , dir]) => dir).map(([name, , dir]) => `{ name = "${name}", dir = "${dir}" }`);
  return `package = { module = "${path}", heff = "${heff}"${listField("require", req)}${listField("replace", rep)} }\n`;
};
write("json/package.hf", manifestOf("x.org/json"));
write("json/src/json/util.hf", "quoted : String -> String\nquoted s = \"<\" ^ s ^ \">\"\n");
write("json/src/json/encode.hf", "import json.util\n\nencode : String -> String\nencode s = quoted s\n");
write("jsony/package.hf", manifestOf("y.org/json"));
write("jsony/src/json/encode.hf", "encode : String -> Int\nencode s = 1\n");

write("app/package.hf", manifestOf("x.org/app", [["json", "x.org/json", "../json"]]));
write("app/src/app/main.hf", "import json.encode\n\nout : String\nout = encode \"hi\"\n\n-- >>> out\n-- \"<hi>\"\n");
const app = await heff("test", "app/src/app/main.hf");
expect("a package from a directory", app.status === 0 && app.stdout.includes("1/1 expectations hold"), app);

write("renamed/package.hf", manifestOf("x.org/renamed", [["js", "x.org/json", "../json"]]));
write("renamed/src/renamed/main.hf", "import js.encode\n\nout : String\nout = encode \"hi\"\n");
const renamed = await heff("check", "renamed/src/renamed/main.hf");
expect("a package imported by another name", renamed.status === 0 && renamed.stdout.includes("out : String"), renamed);

write("both/package.hf", manifestOf("x.org/both", [["json", "x.org/json", "../json"], ["json2", "y.org/json", "../jsony"]]));
write("both/src/both/main.hf", "import json.encode as X\nimport json2.encode as Y\n\nout : String\nout = X.encode \"hi\"\n\nn : Int\nn = Y.encode \"hi\"\n");
const both = await heff("check", "both/src/both/main.hf");
expect("two packages of one name, required by two", both.status === 0 && both.stdout.includes("out : String") && both.stdout.includes("n : Int"), both);

write("clash/package.hf", manifestOf("x.org/clash", [["json", "x.org/json", "../json"]]));
write("clash/src/json/encode.hf", "x : Int\nx = 1\n");
write("clash/src/clash/main.hf", "import json.encode\n\ny : Int\ny = 1\n");
const pclash = await heff("check", "clash/src/clash/main.hf");
expect("a package and the project's module of one name", pclash.status === 1 &&
  pclash.stderr.includes("module json.encode is more than one file: clash/src/json/encode.hf and json/src/json/encode.hf"), pclash);

write("unfetched/package.hf", manifestOf("x.org/unfetched", [["json", "x.org/json"]]));
write("unfetched/src/unfetched/main.hf", "x : Int\nx = 1\n");
const unfetched = await heff("check", "unfetched/src/unfetched/main.hf");
expect("a package not fetched", unfetched.status === 1 &&
  unfetched.stderr.includes("x.org/unfetched requires x.org/json 1.0.0, which is not fetched: run `heff get`"), unfetched);

write("wrong/package.hf", manifestOf("x.org/wrong", [["json", "x.org/json", "../jsony"]]));
write("wrong/src/wrong/main.hf", "x : Int\nx = 1\n");
const wrong = await heff("check", "wrong/src/wrong/main.hf");
expect("a directory of another package", wrong.status === 1 &&
  wrong.stderr.includes("jsony/package.hf is the package y.org/json, not x.org/json"), wrong);

// heff's own package, `heff`, is the compiler's: a project elsewhere
// naming itself so is refused (and its lib/ is no library), and none
// requires it or replaces it, nor gets it, nor is made by that name
write("fakeheff/package.hf", manifestOf("heff"));
write("fakeheff/src/x/m.hf", "x : Int\nx = 1\n");
write("fakeheff/lib/sin.hf", "primitive floatSin : Float64 -> Float64\n");
const claimed = await heff("check", "fakeheff/src/x/m.hf");
expect("a project naming itself heff", claimed.status === 1 &&
  claimed.stderr.includes("fakeheff/package.hf:1:22: error[E0347]: the package `heff` is heff's own, and this is not the checkout heff is built in or was installed from"), claimed);
const claimedLib = await heff("check", "fakeheff/lib/sin.hf");
expect("a project naming itself heff: its lib/", claimedLib.status === 1 &&
  claimedLib.stderr.includes("error[E0347]") && claimedLib.stderr.includes("error[E0335]"), claimedLib);
write("reqheff/package.hf", manifestOf("x.org/reqheff", [["h", "heff"]]));
write("reqheff/src/reqheff/m.hf", "x : Int\nx = 1\n");
const reqHeff = await heff("check", "reqheff/src/reqheff/m.hf");
expect("a project requiring heff", reqHeff.status === 1 &&
  reqHeff.stderr.includes("reqheff/package.hf:1:") && reqHeff.stderr.includes(": error[E0347]: the package `heff` is heff's own, which comes with heff: it is not required"), reqHeff);
write("repheff/package.hf", manifestOf("x.org/repheff", [["h", "heff", "../fakeheff"]]));
write("repheff/src/repheff/m.hf", "x : Int\nx = 1\n");
const repHeff = await heff("check", "repheff/src/repheff/m.hf");
expect("a project replacing heff", repHeff.status === 1 &&
  repHeff.stderr.includes("error[E0347]: `h` is the package `heff`, heff's own, which comes with heff: it is not replaced"), repHeff);
write("getheff/package.hf", manifestOf("x.org/getheff"));
const getHeff = await run(heffBin, ["get", "heff"], { cwd: join(work, "getheff") });
expect("get heff", getHeff.status === 1 && getHeff.stderr.includes("heff get: error[E0347]: the package `heff` is heff's own") &&
  readFileSync(join(work, "getheff", "package.hf"), "utf8") === manifestOf("x.org/getheff"), getHeff);
const initHeff = await heff("init", "heff");
expect("init heff", initHeff.status === 1 && initHeff.stderr.includes("`heff` is heff's own package") && !existsSync(join(work, "heff")), initHeff);

write("two/package.hf", manifestOf("x.org/two"));
write("two/src/a/a.hf", "x : Int\nx = 1\n");
write("two/src/b/b.hf", "x : Int\nx = 1\n");
write("usetwo/package.hf", manifestOf("x.org/usetwo", [["two", "x.org/two", "../two"]]));
write("usetwo/src/usetwo/main.hf", "x : Int\nx = 1\n");
const usetwo = await heff("check", "usetwo/src/usetwo/main.hf");
expect("a package whose src/ holds two directories", usetwo.status === 1 &&
  usetwo.stderr.includes("two/src holds a, b: a package's src/ holds one directory, named as the package"), usetwo);

write("loop/package.hf", manifestOf("x.org/loop", [["back", "x.org/back", "../back"]]));
write("loop/src/loop/main.hf", "x : Int\nx = 1\n");
write("back/package.hf", manifestOf("x.org/back", [["loop", "x.org/loop"]]));
write("back/src/back/b.hf", "x : Int\nx = 1\n");
const loop = await heff("check", "loop/src/loop/main.hf");
expect("packages that require one another", loop.status === 1 &&
  loop.stderr.includes("packages require one another: x.org/loop -> x.org/back -> x.org/loop"), loop);

write("olddep/package.hf", manifestOf("x.org/olddep", [], "0.99.0"));
write("olddep/src/olddep/m.hf", "x : Int\nx = 1\n");
write("useold/package.hf", manifestOf("x.org/useold", [["olddep", "x.org/olddep", "../olddep"]]));
write("useold/src/useold/main.hf", "x : Int\nx = 1\n");
const useold = await heff("check", "useold/src/useold/main.hf");
expect("a package for another heff", useold.status === 1 &&
  useold.stderr.includes(`the package x.org/olddep is for heff 0.99.0 (the same 0.x), and this heff is ${version}`), useold);

// publish: checked, tagged vVERSION, the tag pushed; refused for a
// version that is no a.b.c, a path whose /vN is not the major, what is
// not committed, a watch that does not hold, a tag already there
const git = (dir, ...args) => spawnSync("git", args, { cwd: join(work, dir), encoding: "utf8" });
for (const [k, v] of Object.entries({ GIT_AUTHOR_NAME: "t", GIT_AUTHOR_EMAIL: "t@t", GIT_COMMITTER_NAME: "t", GIT_COMMITTER_EMAIL: "t@t" })) process.env[k] = v;
spawnSync("git", ["init", "-q", "--bare", join(work, "origin.git")]);
write("pub/package.hf", "import manifest\n\npackage : Package\n" + manifestOf("x.org/pub"));
write("pub/src/pub/m.hf", "x : Int\nx = 1\n\n-- >>> x + 1\n-- 2\n");
write("pub/.gitignore", ".heff/\n");
git("pub", "init", "-q"); git("pub", "add", "-A"); git("pub", "commit", "-q", "-m", "one");
git("pub", "remote", "add", "origin", join(work, "origin.git"));
const publish = (...args) => run(heffBin, ["publish", ...args], { cwd: join(work, "pub") });
write("untyped/package.hf", manifestOf("x.org/untyped"));
write("untyped/src/untyped/m.hf", "x : Int\nx = 1\n");
const untyped = await run(heffBin, ["publish", "1.0.0"], { cwd: join(work, "untyped") });
expect("publish, a package.hf of no type", untyped.status === 1 && untyped.stderr.includes("package.hf is no Package (lib/manifest.hf)"), untyped);
const noVersion = await publish("1.0");
expect("publish, no a.b.c", noVersion.status === 1 && noVersion.stderr.includes("`1.0` is no version: a.b.c"), noVersion);
const major = await publish("2.0.0");
expect("publish, a major without its /vN", major.status === 1 && major.stderr.includes("version 2.0.0 is fetched from a path ending in /v2"), major);
const published = await publish("1.0.0");
expect("publish", published.status === 0 && published.stdout.includes("heff publish: x.org/pub 1.0.0, tag v1.0.0") &&
  git("pub", "ls-remote", "--tags", "origin").stdout.includes("refs/tags/v1.0.0"), published);
const twice = await publish("1.0.0");
expect("publish, a version already tagged", twice.status === 1 && twice.stderr.includes("v1.0.0 is tagged already"), twice);
write("pub/src/pub/m.hf", "x : Int\nx = 1\n\n-- >>> x + 1\n-- 3\n");
const dirty = await publish("1.0.1");
expect("publish, not committed", dirty.status === 1 && dirty.stderr.includes("commit first"), dirty);
git("pub", "commit", "-q", "-am", "wrong");
const failing = await publish("1.0.1");
expect("publish, a watch that does not hold", failing.status === 1 && failing.stderr.includes("1 of 1 expectations do not hold") &&
  !git("pub", "tag").stdout.includes("v1.0.1"), failing);

// get: every version reached fetched from git (`https://PATH`, here a
// directory by git's `insteadOf`), the newest of each package selected,
// heff.sum written; a tag moved since is an error, as is a cache that is
// not what heff.sum says
process.env.GIT_CONFIG_COUNT = "1";
process.env.GIT_CONFIG_KEY_0 = `url.file://${join(work, "remotes")}/.insteadOf`;
process.env.GIT_CONFIG_VALUE_0 = "https://";
const remote = (path, files, tag) => {
  const dir = join("remotes", path);
  for (const [f, text] of Object.entries(files)) write(join(dir, f), text);
  if (!existsSync(join(work, dir, ".git"))) git(dir, "init", "-q");
  git(dir, "add", "-A"); git(dir, "commit", "-q", "-m", tag); git(dir, "tag", tag);
};
const typed = (path, requires = []) => "import manifest\n\npackage : Package\n" +
  `package = { module = "${path}", heff = "${version}"${listField("require", requires.map(([n, p, v]) => `{ name = "${n}", path = "${p}", version = "${v}" }`))} }\n`;
const encodeAs = (l, r) => `encode : String -> String\nencode s = "${l}" ^ s ^ "${r}"\n`;
remote("x.org/lib", { "package.hf": typed("x.org/lib"), "src/lib/encode.hf": encodeAs("<", ">") }, "v1.0.0");
remote("x.org/lib", { "src/lib/encode.hf": encodeAs("[", "]") }, "v1.1.0");
remote("x.org/lib", { "package.hf": typed("x.org/lib/v2"), "src/lib/encode.hf": encodeAs("{", "}") }, "v2.0.0");
remote("x.org/mid", { "package.hf": typed("x.org/mid", [["lib", "x.org/lib", "1.1.0"]]),
  "src/mid/m.hf": "import lib.encode\n\ntwice : String -> String\ntwice s = encode (encode s)\n" }, "v1.0.0");

write("user/package.hf", typed("x.org/user", [["lib", "x.org/lib", "1.0.0"], ["mid", "x.org/mid", "1.0.0"]]));
write("user/src/user/main.hf", "import lib.encode\nimport mid.m\n\n-- >>> encode \"hi\"\n-- \"[hi]\"\n\n-- >>> twice \"hi\"\n-- \"[[hi]]\"\n");
const inUser = (...args) => run(heffBin, [...args], { cwd: join(work, "user") });
const notYet = await inUser("test", "src/user/main.hf");
expect("get: not fetched yet", notYet.status === 1 && notYet.stderr.includes("x.org/user requires x.org/lib 1.0.0, which is not fetched: run `heff get`"), notYet);
const got = await inUser("get");
const sum = existsSync(join(work, "user", "heff.sum")) ? readFileSync(join(work, "user", "heff.sum"), "utf8") : "";
expect("get", got.status === 0 && got.stdout.includes("heff get: 2 packages, heff.sum written") &&
  /^x\.org\/lib 1\.0\.0 tree:[0-9a-f]+\nx\.org\/lib 1\.1\.0 tree:[0-9a-f]+\nx\.org\/mid 1\.0\.0 tree:[0-9a-f]+\n$/.test(sum), [got, sum]);
const cached = join(work, "mod", "x.org", "lib@1.1.0");
expect("get: the cache, read-only, without .git", existsSync(join(cached, "src/lib/encode.hf")) && !existsSync(join(cached, ".git")) &&
  (statSync(join(cached, "src/lib/encode.hf")).mode & 0o222) === 0, cached);
const selectedV = await inUser("test", "src/user/main.hf");
expect("get: the newest version reached is the one in the program", selectedV.status === 0 && selectedV.stdout.includes("2/2 expectations hold"), selectedV);
const again2 = await inUser("get");
expect("get, again: nothing fetched", again2.status === 0 && !again2.stdout.includes("fetched"), again2);

const clashV2 = await inUser("get", "x.org/lib/v2");
expect("get PATH: a name required already", clashV2.status === 1 && clashV2.stderr.includes("`lib` is required already: add x.org/lib/v2 to package.hf by another name"), clashV2);
write("fresh/package.hf", "-- mine\n" + typed("x.org/fresh"));
write("fresh/src/fresh/main.hf", "import lib.encode\n\n-- >>> encode \"hi\"\n-- \"[hi]\"\n");
const inFresh = (...args) => run(heffBin, [...args], { cwd: join(work, "fresh") });
const getNewest = await inFresh("get", "x.org/lib");
const freshPkg = readFileSync(join(work, "fresh", "package.hf"), "utf8");
expect("get PATH: its newest version of the path's major, by its own name", getNewest.status === 0 &&
  getNewest.stdout.includes("package.hf requires x.org/lib 1.1.0") && freshPkg.startsWith("-- mine\n") &&
  freshPkg.includes(`{ name = "lib", path = "x.org/lib", version = "1.1.0" }`) && (await inFresh("test", "src/fresh/main.hf")).status === 0, [getNewest, freshPkg]);
expect("get PATH: package.hf laid out", (await inFresh("fmt", "--check", "package.hf")).status === 0, freshPkg);
const getOld = await inFresh("get", "x.org/lib@1.0.0");
expect("get PATH@VERSION", getOld.status === 0 && readFileSync(join(work, "fresh", "package.hf"), "utf8").includes(`version = "1.0.0"`), getOld);
const getNone = await inFresh("get", "x.org/lib@1.7.0");
expect("get PATH@VERSION, no such tag", getNone.status === 1 && getNone.stderr.includes("cannot fetch x.org/lib 1.7.0 (tag v1.7.0 of https://x.org/lib)"), getNone);

write("user/heff.sum", sum.replace(/(x\.org\/mid 1\.0\.0 tree:)[0-9a-f]+/, "$10000"));
const tampered = await inUser("check", "src/user/main.hf");
expect("a cache that is not what heff.sum says", tampered.status === 1 && tampered.stderr.includes("and heff.sum says tree:0000"), tampered);
write("user/heff.sum", sum);
git("remotes/x.org/lib", "checkout", "-q", "v1.0.0");
remote("x.org/lib", { "src/lib/encode.hf": encodeAs("(", ")") }, "moved");
git("remotes/x.org/lib", "tag", "-f", "v1.0.0");
const unlock = (p) => { if (!existsSync(p)) return; chmodSync(p, 0o755); if (statSync(p).isDirectory()) for (const n of readdirSync(p)) unlock(join(p, n)); };
unlock(join(work, "mod")); rmSync(join(work, "mod"), { recursive: true, force: true });
const moved = await inUser("get");
expect("get: a tag moved", moved.status === 1 && moved.stderr.includes("x.org/lib 1.0.0 is tree:") && moved.stderr.includes("its tag has been moved"), moved);

// build: the whole project, every module of src/ checked and every
// program's build.hf read, then, if nothing is wrong, every program of
// app/ built to .heff/bin/; install: the programs built into the heff
// home's bin/ (HEFF_HOME), and said if it is not on the PATH
write("tool/package.hf", manifestOf("tool"));
write("tool/src/tool/greet.hf", "greeting : String\ngreeting = \"hi from tool\"\n");
const program = (says) => "import co\nimport std.io\nimport tool.greet\n\nprogram : {{io : stdio <: stdio}} -> Co stdio Unit\nprogram = print " + says + "\n\nmain : Co stdio Unit\nmain = program (io := here stdio)\n";
const buildOf = (backend, ships = []) =>
  `import manifest\n\nbuild : Build\nbuild = { backend = ${backend}, run = "main"${listField("ships", ships.map((n) => `"${n}"`))} }\n`;
for (const [x, says] of [["tool", "greeting"], ["again", "(greeting ^ \", again\")"]]) {
  write(`tool/app/${x}/build.hf`, buildOf("C"));
  write(`tool/app/${x}/main.hf`, program(says));
}
const inTool = (...args) => run(heffBin, args, { cwd: join(work, "tool") });
const built = await inTool("build");
const toolBin = join(work, "tool", ".heff", "bin", "tool"), againBin = join(work, "tool", ".heff", "bin", "again");
expect("build: the modules checked, every program built", built.status === 0 && built.stdout.includes("heff build: 1 module checked") &&
  built.stdout.includes("wrote .heff/bin/tool") && built.stdout.includes("wrote .heff/bin/again") &&
  (await run(toolBin, [])).stdout === "hi from tool\n" &&
  (await run(againBin, [])).stdout === "hi from tool, again\n", built);
write("tool/src/tool/broken.hf", "x : Int\nx = True\n");
rmSync(join(work, "tool", ".heff", "bin"), { recursive: true });
const broken = await inTool("build");
expect("build: a module that does not check, nothing built", broken.status === 1 && broken.stderr.includes("src/tool/broken.hf") &&
  !existsSync(toolBin) && !existsSync(againBin), broken);
rmSync(join(work, "tool", "src", "tool", "broken.hf"));
write("tool/app/unread/build.hf", buildOf("Rust"));
write("tool/app/unread/main.hf", program("greeting"));
const unread = await inTool("build");
expect("build: a build.hf that cannot be read, nothing built", unread.status === 1 && unread.stderr.includes("app/unread/build.hf") &&
  unread.stderr.includes("no backend Rust") && unread.stderr.includes("nothing built") && !existsSync(toolBin) && !existsSync(againBin), unread);
const home = join(work, "home"), homeBin = join(home, "bin");
const install = (path) => run(heffBin, ["install", "tool"], { cwd: work, env: { ...process.env, HEFF_HOME: home, PATH: path } });
const unreadInstall = await install(process.env.PATH);
expect("install: a build.hf that cannot be read, nothing installed", unreadInstall.status === 1 && unreadInstall.stderr.includes("app/unread/build.hf") &&
  !existsSync(join(homeBin, "tool")) && !existsSync(join(homeBin, "again")), unreadInstall);
rmSync(join(work, "tool", "app", "unread"), { recursive: true });
const installed = await install(process.env.PATH);
expect("install: the programs, in the heff home's bin/", installed.status === 0 && installed.stdout.includes(`installed ${join(homeBin, "tool")}`) &&
  installed.stdout.includes(`installed ${join(homeBin, "again")}`) &&
  installed.stdout.includes(`${homeBin} is not on the PATH`) &&
  (await run(join(homeBin, "tool"), [])).stdout === "hi from tool\n", installed);
const onPath = await install(`${homeBin}:${process.env.PATH}`);
expect("install: its bin/ on the PATH", onPath.status === 0 && !onPath.stdout.includes("not on the PATH"), onPath);

// install: what a program ships (its build.hf's `ships`), files and
// directories of the project's root, put in the heff home beside bin/
// (modes kept), in place of what was there: nothing of that left, nor of
// the copy made beside it; one the project does not have, nothing built
write("tool/share/greeting.txt", "hello\n");
write("tool/share/old.txt", "old\n");
write("tool/share/run.sh", "#!/bin/sh\necho run\n");
chmodSync(join(work, "tool", "share", "run.sh"), 0o755);
write("tool/NOTICE", "notice 1\n");
write("tool/app/tool/build.hf", buildOf("C", ["share", "NOTICE"]));
write("tool/app/again/build.hf", buildOf("C", ["share"]));
const shipped = await install(process.env.PATH);
expect("install: what a program ships, beside bin/", shipped.status === 0 &&
  shipped.stdout.includes(`installed ${join(home, "share")}\n`) && shipped.stdout.includes(`installed ${join(home, "NOTICE")}\n`) &&
  shipped.stdout.split(`installed ${join(home, "share")}\n`).length === 2 &&
  readFileSync(join(home, "share", "greeting.txt"), "utf8") === "hello\n" && readFileSync(join(home, "NOTICE"), "utf8") === "notice 1\n" &&
  (statSync(join(home, "share", "run.sh")).mode & 0o111) !== 0, shipped);
rmSync(join(work, "tool", "share", "old.txt"));
write("tool/share/greeting.txt", "hello again\n");
write("tool/NOTICE", "notice 2\n");
const reshipped = await install(process.env.PATH);
expect("install again: what is shipped, in place of what was there", reshipped.status === 0 &&
  readFileSync(join(home, "share", "greeting.txt"), "utf8") === "hello again\n" && !existsSync(join(home, "share", "old.txt")) &&
  readFileSync(join(home, "NOTICE"), "utf8") === "notice 2\n" && readdirSync(home).sort().join(",") === "NOTICE,bin,share", [reshipped, readdirSync(home)]);
write("tool/app/tool/build.hf", buildOf("C", ["share", "nowhere"]));
rmSync(homeBin, { recursive: true });
const unshipped = await install(process.env.PATH);
expect("install: a program shipping what the project does not have, nothing installed", unshipped.status === 1 &&
  unshipped.stderr.includes(`${join("tool", "app", "tool", "build.hf")}: it ships \`nowhere\`, which the project does not have`) &&
  unshipped.stderr.includes("nothing built") && !existsSync(homeBin), unshipped);
write("lib-only/package.hf", manifestOf("libonly"));
write("lib-only/src/libonly/m.hf", "x : Int\nx = 1\n");
const nothing = await run(heffBin, ["install", "lib-only"], { cwd: work, env: { ...process.env, HEFF_HOME: home } });
expect("install: no programs", nothing.status === 1 && nothing.stderr.includes("no programs"), nothing);

// install again, the program run over and over meanwhile by another
// process: each run is of the program as it was or as it is, never of one
// half written (it is put in place by a rename, at once); an install that
// fails leaves it as it was, and nothing beside it in bin/
// (that other process takes no slot of the tests' budget: it runs until
// the install is done, which, where there is one slot, would wait for it)
await heff("init", "busy");
const busyHome = join(work, "busy-home"), busyBin = join(busyHome, "bin", "busy");
const installBusy = () => run(heffBin, ["install", "busy"], { cwd: work, env: { ...process.env, HEFF_HOME: busyHome } });
const busyFirst = await installBusy();
const busyMain = readFileSync(join(work, "busy", "app", "busy", "main.hf"), "utf8");
write("busy/app/busy/main.hf", busyMain.replace("greeting \"world\"", "greeting \"again\""));
const busyStop = join(work, "busy-stop"), busyReady = join(work, "busy-ready");
const runsBusy = run(process.execPath, ["-e", [
  "const { spawnSync } = require('node:child_process'), { existsSync, writeFileSync } = require('node:fs');",
  "const [bin, stop, ready] = process.argv.slice(1); let runs = 0; const bad = [];",
  "while (!existsSync(stop)) {",
  "  const r = spawnSync(bin, [], { encoding: 'utf8' }); runs++;",
  "  if (r.error || r.status !== 0 || !/^Hello, (world|again)!\\n$/.test(r.stdout)) bad.push(r.error ? r.error.code : `${r.status} ${r.signal} ${JSON.stringify(r.stdout)}`);",
  "  if (runs === 1) writeFileSync(ready, '');",
  "}",
  "console.log(JSON.stringify({ runs, failed: bad.length, bad: bad.slice(0, 5) }));",
].join("\n"), busyBin, busyStop, busyReady], { slot: false });
for (let i = 0; i < 2000 && !existsSync(busyReady); i++) await new Promise((r) => setTimeout(r, 5));
const busyAgain = await installBusy();
writeFileSync(busyStop, "");
const busyRuns = await runsBusy;
const busyRan = (() => { try { return JSON.parse(busyRuns.stdout); } catch { return null; } })();
expect("install again, the program running meanwhile: never run half written", busyFirst.status === 0 && busyAgain.status === 0 &&
  busyRan !== null && busyRan.runs > 0 && busyRan.failed === 0 &&
  (await run(busyBin, [])).stdout === "Hello, again!\n" && readdirSync(join(busyHome, "bin")).join(",") === "busy",
  [busyFirst, busyAgain, busyRuns, readdirSync(join(busyHome, "bin"))]);
write("busy/app/busy/main.hf", busyMain.replace("greeting \"world\"", "greeting 1"));
const busyBroken = await installBusy();
expect("install that fails: the program installed before kept", busyBroken.status === 1 &&
  (await run(busyBin, [])).stdout === "Hello, again!\n" && readdirSync(join(busyHome, "bin")).join(",") === "busy",
  [busyBroken, readdirSync(join(busyHome, "bin"))]);

// heff itself, installed: it ships its lib/ and its package.hf, so the
// heff in the heff home finds its library and its version beside bin/,
// as a release's does (no HEFF_LIB to find it by)
const { HEFF_LIB: _lib, ...ownEnv } = process.env;
const selfInstalled = await selfInstalling;
const installedHeff = (...args) => run(join(heffHome, "bin", "heff"), args, { cwd: work, env: ownEnv });
write("uses-prelude.hf", "xs : List Int\nxs = List.map (\\x -> x + 1) [1, 2]\n");
const selfChecked = await installedHeff("check", "uses-prelude.hf");
const selfInit = await installedHeff("init", "fromhome");
const selfVersion = await installedHeff("version");
// (the repository installed from: heff's own, its lib/ the library's
// source to the heff installed, as to the editor's)
const selfOwn = await installedHeff("check", join(root, "lib", "float32.hf"));
expect("install heff: the heff in the heff home, with its library and its version", selfInstalled.status === 0 &&
  selfInstalled.stdout.includes(`installed ${join(heffHome, "lib")}\n`) && selfInstalled.stdout.includes(`installed ${join(heffHome, "package.hf")}\n`) &&
  selfChecked.status === 0 && selfChecked.stdout.includes("xs : List Int") &&
  selfInit.status === 0 && readFileSync(join(work, "fromhome", "package.hf"), "utf8").includes(`heff = "${version}"`) &&
  selfVersion.status === 0 && selfVersion.stdout === `heff ${version}\n`, [selfInstalled, selfChecked, selfInit, selfVersion]);
expect("install heff: the heff home says the repository is heff's own", existsSync(join(heffHome, "source")) &&
  selfInstalled.stdout.includes(`installed ${join(heffHome, "source")}\n`) &&
  selfOwn.status === 0 && !/E03(3[56]|47)/.test(selfOwn.stderr), [existsSync(join(heffHome, "source")) && readFileSync(join(heffHome, "source"), "utf8"), selfOwn]);

// a manifest's fields with a default in lib/manifest.hf (`require`,
// `replace`, a build.hf's `ships`: empty lists) left out, or written: heff
// reading package.hf and build.hf (without elaborating them, `heff
// build`) and elaborating them (`heff check`, against lib/manifest.hf)
// agree on each, and on a field without one left out, or the fields out
// of order -- saying the same fields, and which may be left out
const packageFields = "the fields are module, heff, require, replace, in this order (require and replace may be left out)";
const buildFields = "the fields are backend, run, ships, in this order (ships may be left out)";
const agreeing = [
  ["package.hf", "package : Package\npackage", `{ module = "agree", heff = "${version}" }`, null],
  ["package.hf", "package : Package\npackage", `{ module = "agree", heff = "${version}", require = [], replace = [] }`, null],
  ["package.hf", "package : Package\npackage", `{ module = "agree", heff = "${version}", replace = [] }`, null],
  ["package.hf", "package : Package\npackage", `{ module = "agree" }`, packageFields],
  ["package.hf", "package : Package\npackage", `{ module = "agree", heff = "${version}", replace = [], require = [] }`, packageFields],
  ["app/agree/build.hf", "build : Build\nbuild", `{ backend = Page, run = "main" }`, null],
  ["app/agree/build.hf", "build : Build\nbuild", `{ backend = Page, run = "main", ships = [] }`, null],
  ["app/agree/build.hf", "build : Build\nbuild", `{ backend = Page }`, buildFields],
  ["app/agree/build.hf", "build : Build\nbuild", `{ backend = Page, ships = [], run = "main" }`, buildFields],
];
const inAgree = (...args) => run(heffBin, args, { cwd: join(work, "agree") });
write("agree/src/agree/m.hf", "x : Int\nx = 1\n");
write("agree/app/agree/main.hf", "import co\nimport browser.dom\n\nmain : Co dom Unit\nmain = pure MkUnit\n");
for (const [file, head, literal, fields] of agreeing) {
  write("agree/package.hf", manifestOf("agree"));
  write("agree/app/agree/build.hf", buildOf("Page"));
  write(`agree/${file}`, `import manifest\n\n${head} = ${literal}\n`);
  const read = await inAgree("build"), checked = await inAgree("check", file);
  expect(`${file}, ${literal}: read and elaborated alike`, fields === null
    ? read.status === 0 && checked.status === 0
    : read.status === 1 && checked.status === 1 && read.stderr.includes(fields) && checked.stderr.includes(fields), [read, checked]);
}

console.log(`project: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
