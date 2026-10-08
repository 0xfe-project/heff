#!/usr/bin/env node
// Projects (src/heff/project.hf, src/heff/load.hf): `heff init` makes one
// whose watches hold; a project's modules are under src/, a file outside
// it (a test) may import the project's modules and helpers beside it,
// what is in src/ may not import those; a module found twice, a
// package.hf that does not read and one for another heff are errors.
// Packages: given by directories, fetched by `heff get` (git, MVS,
// heff.sum), published by `heff publish`.
import { spawnSync } from "node:child_process";
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { heffBin } from "./heff.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const work = mkdtempSync(join(tmpdir(), "heff-project-"));
// (packages are fetched into the test's own cache)
process.env.HEFF_MODCACHE = join(work, "mod");
const heff = (...args) => spawnSync(heffBin, [...args], { cwd: work, encoding: "utf8" });
const write = (path, text) => { mkdirSync(dirname(join(work, path)), { recursive: true }); writeFileSync(join(work, path), text); };
const version = /heff = "([^"]*)"/.exec(readFileSync(join(root, "package.hf"), "utf8"))[1];

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

// init: package.hf (for this heff), .gitignore, src/hello/main.hf, whose
// watch holds; again, nothing is written
const made = heff("init", "hello");
const pkg = readFileSync(join(work, "hello", "package.hf"), "utf8");
expect("init", made.status === 0 && pkg.includes(`heff = "${version}"`) && pkg.includes(`module = "hello"`) &&
  readFileSync(join(work, "hello", ".gitignore"), "utf8") === ".heff/\n", made);
const laid = heff("fmt", "--check", "hello/package.hf", "hello/src/hello/main.hf");
expect("init's files are laid out", laid.status === 0, laid);
const tested = heff("test", "hello/src/hello/main.hf");
expect("init's main.hf", tested.status === 0 && tested.stdout.includes("1/1 expectations hold"), tested);
// (with no file: the project's test/, run where it is; a file in a
// directory starting with `.` or in a project of its own passed over;
// nothing outside test/, and no test/, nothing to test)
write("hello/test/greeting.hf", "import hello.main\nimport prelude\n\n-- >>> greeting \"test\"\n-- \"Hello, test!\"\n");
write("hello/test/.hidden/no.hf", "x : Int\nx = \n");
write("hello/test/vendor/other/package.hf", "import manifest\n\npackage : Package\npackage = { module = \"other\", heff = \"0.0.1\", require = [], replace = [] }\n");
write("hello/test/vendor/other/no.hf", "x : Int\nx = \n");
write("hello/elsewhere/no.hf", "x : Int\nx = \n");
const inHello = spawnSync(heffBin, ["test"], { cwd: join(work, "hello"), encoding: "utf8" });
expect("test with no file: the project's test/", inHello.status === 0 && inHello.stdout.includes("test/greeting.hf") &&
  !inHello.stdout.includes("src/hello/main.hf") && !inHello.stdout.includes("no.hf") && inHello.stdout.includes("1/1 expectations hold"), inHello);
rmSync(join(work, "hello", "test"), { recursive: true });
rmSync(join(work, "hello", "elsewhere"), { recursive: true });
const noTests = spawnSync(heffBin, ["test"], { cwd: join(work, "hello"), encoding: "utf8" });
expect("test with no file and no test/", noTests.status === 1 && noTests.stderr.includes("no test/ here"), noTests);
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

// a module whose import is written in another case than its file (or a
// directory on its path) is named: an error where the file system finds
// it all the same; two names that differ only in case, where it does not
write("hello/src/hello/Shout.hf", "import prelude\n\nup : String\nup = \"HI\"\n");
write("hello/src/hello/useshout.hf", "import hello.shout\nimport prelude\n\nu : String\nu = up\n");
const caseBlind = existsSync(join(work, "hello/src/hello/shout.hf"));
const shout = heff("check", "hello/src/hello/useshout.hf");
expect("a module found by a case-different name", shout.status === 1 && (caseBlind
  ? shout.stderr.includes("importing hello.shout: hello/src/hello/shout.hf is named Shout.hf on the disk: a file system that tells case apart finds no shout.hf")
  : shout.stderr.includes("no module hello.shout")), shout);
write("hello/src/hello/useshout.hf", "import Hello.Shout\nimport prelude\n\nu : String\nu = up\n");
const shoutDir = heff("check", "hello/src/hello/useshout.hf");
expect("a directory found by a case-different name", shoutDir.status === 1 && (caseBlind
  ? shoutDir.stderr.includes("hello/src/Hello is named hello on the disk")
  : shoutDir.stderr.includes("no module Hello.Shout")), shoutDir);
write("hello/src/hello/useshout.hf", "import hello.Shout\nimport prelude\n\nu : String\nu = up\n");
const shoutOk = heff("check", "hello/src/hello/useshout.hf");
expect("a module named as on the disk", shoutOk.status === 0 && shoutOk.stdout.includes("u : String"), shoutOk);
if (!caseBlind) {
  write("hello/src/hello/shout.hf", "x : Int\nx = 1\n");
  const twoCases = heff("check", "hello/src/hello/useshout.hf");
  expect("two names that differ only in case", twoCases.status === 1 &&
    twoCases.stderr.includes("hello/src/hello holds Shout.hf, shout.hf: names that differ only in case"), twoCases);
  rmSync(join(work, "hello/src/hello/shout.hf"));
}
rmSync(join(work, "hello/src/hello/Shout.hf")); rmSync(join(work, "hello/src/hello/useshout.hf"));

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

// packages: a required package given by a directory (`replace`), its
// modules imported by the name the project requires it by, its own
// imports by its own package.hf
const manifestOf = (path, requires = [], heff = version) => {
  const req = requires.map(([name, p]) => `{ name = "${name}", path = "${p}", version = "1.0.0" }`);
  const rep = requires.filter(([, , dir]) => dir).map(([name, , dir]) => `{ name = "${name}", dir = "${dir}" }`);
  return `package = { module = "${path}", heff = "${heff}", require = [${req.join(", ")}], replace = [${rep.join(", ")}] }\n`;
};
write("json/package.hf", manifestOf("x.org/json"));
write("json/src/json/util.hf", "import prelude\n\nquoted : String -> String\nquoted s = \"<\" ++ s ++ \">\"\n");
write("json/src/json/encode.hf", "import json.util\nimport prelude\n\nencode : String -> String\nencode s = quoted s\n");
write("jsony/package.hf", manifestOf("y.org/json"));
write("jsony/src/json/encode.hf", "import prelude\n\nencode : String -> Int\nencode s = 1\n");

write("app/package.hf", manifestOf("x.org/app", [["json", "x.org/json", "../json"]]));
write("app/src/app/main.hf", "import json.encode\nimport prelude\n\nout : String\nout = encode \"hi\"\n\n-- >>> out\n-- \"<hi>\"\n");
const app = heff("test", "app/src/app/main.hf");
expect("a package from a directory", app.status === 0 && app.stdout.includes("1/1 expectations hold"), app);

write("renamed/package.hf", manifestOf("x.org/renamed", [["js", "x.org/json", "../json"]]));
write("renamed/src/renamed/main.hf", "import js.encode\nimport prelude\n\nout : String\nout = encode \"hi\"\n");
const renamed = heff("check", "renamed/src/renamed/main.hf");
expect("a package imported by another name", renamed.status === 0 && renamed.stdout.includes("out : String"), renamed);

write("both/package.hf", manifestOf("x.org/both", [["json", "x.org/json", "../json"], ["json2", "y.org/json", "../jsony"]]));
write("both/src/both/main.hf", "import json.encode as X\nimport json2.encode as Y\nimport prelude\n\nout : String\nout = X.encode \"hi\"\n\nn : Int\nn = Y.encode \"hi\"\n");
const both = heff("check", "both/src/both/main.hf");
expect("two packages of one name, required by two", both.status === 0 && both.stdout.includes("out : String") && both.stdout.includes("n : Int"), both);

write("clash/package.hf", manifestOf("x.org/clash", [["json", "x.org/json", "../json"]]));
write("clash/src/json/encode.hf", "x : Int\nx = 1\n");
write("clash/src/clash/main.hf", "import json.encode\n\ny : Int\ny = 1\n");
const pclash = heff("check", "clash/src/clash/main.hf");
expect("a package and the project's module of one name", pclash.status === 1 &&
  pclash.stderr.includes("module json.encode is more than one file: clash/src/json/encode.hf and json/src/json/encode.hf"), pclash);

write("unfetched/package.hf", manifestOf("x.org/unfetched", [["json", "x.org/json"]]));
write("unfetched/src/unfetched/main.hf", "x : Int\nx = 1\n");
const unfetched = heff("check", "unfetched/src/unfetched/main.hf");
expect("a package not fetched", unfetched.status === 1 &&
  unfetched.stderr.includes("x.org/unfetched requires x.org/json 1.0.0, which is not fetched: run `heff get`"), unfetched);

write("wrong/package.hf", manifestOf("x.org/wrong", [["json", "x.org/json", "../jsony"]]));
write("wrong/src/wrong/main.hf", "x : Int\nx = 1\n");
const wrong = heff("check", "wrong/src/wrong/main.hf");
expect("a directory of another package", wrong.status === 1 &&
  wrong.stderr.includes("jsony/package.hf is the package y.org/json, not x.org/json"), wrong);

write("two/package.hf", manifestOf("x.org/two"));
write("two/src/a/a.hf", "x : Int\nx = 1\n");
write("two/src/b/b.hf", "x : Int\nx = 1\n");
write("usetwo/package.hf", manifestOf("x.org/usetwo", [["two", "x.org/two", "../two"]]));
write("usetwo/src/usetwo/main.hf", "x : Int\nx = 1\n");
const usetwo = heff("check", "usetwo/src/usetwo/main.hf");
expect("a package whose src/ holds two directories", usetwo.status === 1 &&
  usetwo.stderr.includes("two/src holds a, b: a package's src/ holds one directory, named as the package"), usetwo);

write("loop/package.hf", manifestOf("x.org/loop", [["back", "x.org/back", "../back"]]));
write("loop/src/loop/main.hf", "x : Int\nx = 1\n");
write("back/package.hf", manifestOf("x.org/back", [["loop", "x.org/loop"]]));
write("back/src/back/b.hf", "x : Int\nx = 1\n");
const loop = heff("check", "loop/src/loop/main.hf");
expect("packages that require one another", loop.status === 1 &&
  loop.stderr.includes("packages require one another: x.org/loop -> x.org/back -> x.org/loop"), loop);

write("olddep/package.hf", manifestOf("x.org/olddep", [], "0.99.0"));
write("olddep/src/olddep/m.hf", "x : Int\nx = 1\n");
write("useold/package.hf", manifestOf("x.org/useold", [["olddep", "x.org/olddep", "../olddep"]]));
write("useold/src/useold/main.hf", "x : Int\nx = 1\n");
const useold = heff("check", "useold/src/useold/main.hf");
expect("a package for another heff", useold.status === 1 &&
  useold.stderr.includes(`the package x.org/olddep is for heff 0.99.0 (the same 0.x), and this heff is ${version}`), useold);

// publish: checked, tagged vVERSION, the tag pushed; refused for a
// version that is no a.b.c, a path whose /vN is not the major, what is
// not committed, a watch that does not hold, a tag already there
const git = (dir, ...args) => spawnSync("git", args, { cwd: join(work, dir), encoding: "utf8" });
for (const [k, v] of Object.entries({ GIT_AUTHOR_NAME: "t", GIT_AUTHOR_EMAIL: "t@t", GIT_COMMITTER_NAME: "t", GIT_COMMITTER_EMAIL: "t@t" })) process.env[k] = v;
spawnSync("git", ["init", "-q", "--bare", join(work, "origin.git")]);
write("pub/package.hf", "import manifest\n\npackage : Package\n" + manifestOf("x.org/pub"));
write("pub/src/pub/m.hf", "import prelude\n\nx : Int\nx = 1\n\n-- >>> x + 1\n-- 2\n");
write("pub/.gitignore", ".heff/\n");
git("pub", "init", "-q"); git("pub", "add", "-A"); git("pub", "commit", "-q", "-m", "one");
git("pub", "remote", "add", "origin", join(work, "origin.git"));
const publish = (...args) => spawnSync(heffBin, ["publish", ...args], { cwd: join(work, "pub"), encoding: "utf8" });
write("untyped/package.hf", manifestOf("x.org/untyped"));
write("untyped/src/untyped/m.hf", "x : Int\nx = 1\n");
const untyped = spawnSync(heffBin, ["publish", "1.0.0"], { cwd: join(work, "untyped"), encoding: "utf8" });
expect("publish, a package.hf of no type", untyped.status === 1 && untyped.stderr.includes("package.hf is no Package (lib/manifest.hf)"), untyped);
const noVersion = publish("1.0");
expect("publish, no a.b.c", noVersion.status === 1 && noVersion.stderr.includes("`1.0` is no version: a.b.c"), noVersion);
const major = publish("2.0.0");
expect("publish, a major without its /vN", major.status === 1 && major.stderr.includes("version 2.0.0 is fetched from a path ending in /v2"), major);
const published = publish("1.0.0");
expect("publish", published.status === 0 && published.stdout.includes("heff publish: x.org/pub 1.0.0, tag v1.0.0") &&
  git("pub", "ls-remote", "--tags", "origin").stdout.includes("refs/tags/v1.0.0"), published);
const twice = publish("1.0.0");
expect("publish, a version already tagged", twice.status === 1 && twice.stderr.includes("v1.0.0 is tagged already"), twice);
write("pub/src/pub/m.hf", "import prelude\n\nx : Int\nx = 1\n\n-- >>> x + 1\n-- 3\n");
const dirty = publish("1.0.1");
expect("publish, not committed", dirty.status === 1 && dirty.stderr.includes("commit first"), dirty);
git("pub", "commit", "-q", "-am", "wrong");
const failing = publish("1.0.1");
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
  `package = { module = "${path}", heff = "${version}", require = [${requires.map(([n, p, v]) => `{ name = "${n}", path = "${p}", version = "${v}" }`).join(", ")}], replace = [] }\n`;
const encodeAs = (l, r) => `import prelude\n\nencode : String -> String\nencode s = "${l}" ++ s ++ "${r}"\n`;
remote("x.org/lib", { "package.hf": typed("x.org/lib"), "src/lib/encode.hf": encodeAs("<", ">") }, "v1.0.0");
remote("x.org/lib", { "src/lib/encode.hf": encodeAs("[", "]") }, "v1.1.0");
remote("x.org/lib", { "package.hf": typed("x.org/lib/v2"), "src/lib/encode.hf": encodeAs("{", "}") }, "v2.0.0");
remote("x.org/mid", { "package.hf": typed("x.org/mid", [["lib", "x.org/lib", "1.1.0"]]),
  "src/mid/m.hf": "import lib.encode\nimport prelude\n\ntwice : String -> String\ntwice s = encode (encode s)\n" }, "v1.0.0");

write("user/package.hf", typed("x.org/user", [["lib", "x.org/lib", "1.0.0"], ["mid", "x.org/mid", "1.0.0"]]));
write("user/src/user/main.hf", "import lib.encode\nimport mid.m\nimport prelude\n\n-- >>> encode \"hi\"\n-- \"[hi]\"\n\n-- >>> twice \"hi\"\n-- \"[[hi]]\"\n");
const inUser = (...args) => spawnSync(heffBin, [...args], { cwd: join(work, "user"), encoding: "utf8" });
const notYet = inUser("test", "src/user/main.hf");
expect("get: not fetched yet", notYet.status === 1 && notYet.stderr.includes("x.org/user requires x.org/lib 1.0.0, which is not fetched: run `heff get`"), notYet);
const got = inUser("get");
const sum = existsSync(join(work, "user", "heff.sum")) ? readFileSync(join(work, "user", "heff.sum"), "utf8") : "";
expect("get", got.status === 0 && got.stdout.includes("heff get: 2 packages, heff.sum written") &&
  /^x\.org\/lib 1\.0\.0 tree:[0-9a-f]+\nx\.org\/lib 1\.1\.0 tree:[0-9a-f]+\nx\.org\/mid 1\.0\.0 tree:[0-9a-f]+\n$/.test(sum), [got, sum]);
const cached = join(work, "mod", "x.org", "lib@1.1.0");
expect("get: the cache, read-only, without .git", existsSync(join(cached, "src/lib/encode.hf")) && !existsSync(join(cached, ".git")) &&
  (statSync(join(cached, "src/lib/encode.hf")).mode & 0o222) === 0, cached);
const selectedV = inUser("test", "src/user/main.hf");
expect("get: the newest version reached is the one in the program", selectedV.status === 0 && selectedV.stdout.includes("2/2 expectations hold"), selectedV);
const again2 = inUser("get");
expect("get, again: nothing fetched", again2.status === 0 && !again2.stdout.includes("fetched"), again2);

const clashV2 = inUser("get", "x.org/lib/v2");
expect("get PATH: a name required already", clashV2.status === 1 && clashV2.stderr.includes("`lib` is required already: add x.org/lib/v2 to package.hf by another name"), clashV2);
write("fresh/package.hf", "-- mine\n" + typed("x.org/fresh"));
write("fresh/src/fresh/main.hf", "import lib.encode\nimport prelude\n\n-- >>> encode \"hi\"\n-- \"[hi]\"\n");
const inFresh = (...args) => spawnSync(heffBin, [...args], { cwd: join(work, "fresh"), encoding: "utf8" });
const getNewest = inFresh("get", "x.org/lib");
const freshPkg = readFileSync(join(work, "fresh", "package.hf"), "utf8");
expect("get PATH: its newest version of the path's major, by its own name", getNewest.status === 0 &&
  getNewest.stdout.includes("package.hf requires x.org/lib 1.1.0") && freshPkg.startsWith("-- mine\n") &&
  freshPkg.includes(`{ name = "lib", path = "x.org/lib", version = "1.1.0" }`) && inFresh("test", "src/fresh/main.hf").status === 0, [getNewest, freshPkg]);
expect("get PATH: package.hf laid out", inFresh("fmt", "--check", "package.hf").status === 0, freshPkg);
const getOld = inFresh("get", "x.org/lib@1.0.0");
expect("get PATH@VERSION", getOld.status === 0 && readFileSync(join(work, "fresh", "package.hf"), "utf8").includes(`version = "1.0.0"`), getOld);
const getNone = inFresh("get", "x.org/lib@1.7.0");
expect("get PATH@VERSION, no such tag", getNone.status === 1 && getNone.stderr.includes("cannot fetch x.org/lib 1.7.0 (tag v1.7.0 of https://x.org/lib)"), getNone);

write("user/heff.sum", sum.replace(/(x\.org\/mid 1\.0\.0 tree:)[0-9a-f]+/, "$10000"));
const tampered = inUser("check", "src/user/main.hf");
expect("a cache that is not what heff.sum says", tampered.status === 1 && tampered.stderr.includes("and heff.sum says tree:0000"), tampered);
write("user/heff.sum", sum);
git("remotes/x.org/lib", "checkout", "-q", "v1.0.0");
remote("x.org/lib", { "src/lib/encode.hf": encodeAs("(", ")") }, "moved");
git("remotes/x.org/lib", "tag", "-f", "v1.0.0");
const unlock = (p) => { if (!existsSync(p)) return; chmodSync(p, 0o755); if (statSync(p).isDirectory()) for (const n of readdirSync(p)) unlock(join(p, n)); };
unlock(join(work, "mod")); rmSync(join(work, "mod"), { recursive: true, force: true });
const moved = inUser("get");
expect("get: a tag moved", moved.status === 1 && moved.stderr.includes("x.org/lib 1.0.0 is tree:") && moved.stderr.includes("its tag has been moved"), moved);

// build: the whole project, every module of src/ checked and every
// program's build.hf read, then, if nothing is wrong, every program of
// app/ built to .heff/bin/; install: the programs built into the heff
// home's bin/ (HEFF_HOME), and said if it is not on the PATH
write("tool/package.hf", manifestOf("tool"));
write("tool/src/tool/greet.hf", "import prelude\n\ngreeting : String\ngreeting = \"hi from tool\"\n");
const program = (says) => "import co\nimport prelude\nimport std.io\nimport tool.greet\n\nprogram : {{io : stdio <: stdio}} -> Co stdio Unit\nprogram = print " + says + "\n\nmain : Co stdio Unit\nmain = program (io := here stdio)\n";
for (const [x, says] of [["tool", "greeting"], ["again", "(greeting ++ \", again\")"]]) {
  write(`tool/app/${x}/build.hf`, "import manifest\n\nbuild : Build\nbuild = { backend = Node, run = \"main\" }\n");
  write(`tool/app/${x}/main.hf`, program(says));
}
const inTool = (...args) => spawnSync(heffBin, args, { cwd: join(work, "tool"), encoding: "utf8" });
const built = inTool("build");
const toolBin = join(work, "tool", ".heff", "bin", "tool"), againBin = join(work, "tool", ".heff", "bin", "again");
expect("build: the modules checked, every program built", built.status === 0 && built.stdout.includes("heff build: 1 module checked") &&
  built.stdout.includes("wrote .heff/bin/tool") && built.stdout.includes("wrote .heff/bin/again") &&
  spawnSync(toolBin, [], { encoding: "utf8" }).stdout === "hi from tool\n" &&
  spawnSync(againBin, [], { encoding: "utf8" }).stdout === "hi from tool, again\n", built);
write("tool/src/tool/broken.hf", "import prelude\n\nx : Int\nx = True\n");
rmSync(join(work, "tool", ".heff", "bin"), { recursive: true });
const broken = inTool("build");
expect("build: a module that does not check, nothing built", broken.status === 1 && broken.stderr.includes("src/tool/broken.hf") &&
  !existsSync(toolBin) && !existsSync(againBin), broken);
rmSync(join(work, "tool", "src", "tool", "broken.hf"));
write("tool/app/unread/build.hf", "import manifest\n\nbuild : Build\nbuild = { backend = Rust, run = \"main\" }\n");
write("tool/app/unread/main.hf", program("greeting"));
const unread = inTool("build");
expect("build: a build.hf that cannot be read, nothing built", unread.status === 1 && unread.stderr.includes("app/unread/build.hf") &&
  unread.stderr.includes("no backend Rust") && unread.stderr.includes("nothing built") && !existsSync(toolBin) && !existsSync(againBin), unread);
const home = join(work, "home"), homeBin = join(home, "bin");
const install = (path) => spawnSync(heffBin, ["install", "tool"], { cwd: work, encoding: "utf8", env: { ...process.env, HEFF_HOME: home, PATH: path } });
const unreadInstall = install(process.env.PATH);
expect("install: a build.hf that cannot be read, nothing installed", unreadInstall.status === 1 && unreadInstall.stderr.includes("app/unread/build.hf") &&
  !existsSync(join(homeBin, "tool")) && !existsSync(join(homeBin, "again")), unreadInstall);
rmSync(join(work, "tool", "app", "unread"), { recursive: true });
const installed = install(process.env.PATH);
expect("install: the programs, in the heff home's bin/", installed.status === 0 && installed.stdout.includes(`installed ${join(homeBin, "tool")}`) &&
  installed.stdout.includes(`installed ${join(homeBin, "again")}`) &&
  installed.stdout.includes(`${homeBin} is not on the PATH`) &&
  spawnSync(join(homeBin, "tool"), [], { encoding: "utf8" }).stdout === "hi from tool\n", installed);
const onPath = install(`${homeBin}:${process.env.PATH}`);
expect("install: its bin/ on the PATH", onPath.status === 0 && !onPath.stdout.includes("not on the PATH"), onPath);
write("lib-only/package.hf", manifestOf("libonly"));
write("lib-only/src/libonly/m.hf", "import prelude\n\nx : Int\nx = 1\n");
const nothing = spawnSync(heffBin, ["install", "lib-only"], { cwd: work, encoding: "utf8", env: { ...process.env, HEFF_HOME: home } });
expect("install: no programs", nothing.status === 1 && nothing.stderr.includes("no programs"), nothing);

console.log(`project: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
