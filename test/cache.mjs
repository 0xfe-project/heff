#!/usr/bin/env node
// The compiler's cache (.heff/vN/<module>.hc): a second run
// thaws what the first elaborated (the dependency's cache file is not
// rewritten), corruption heals over, a new name in a dependency
// re-elaborates the importer, and a comment-only change does not --
// the invalidation is at the definitions used, not at whole imports --
// and an instance gained or lost does, for the searches it may answer,
// as an instance's instance arguments changed do.
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, readdirSync, writeFileSync, mkdtempSync, mkdirSync, copyFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { run } from "./pool.mjs";

if (!existsSync(heffBin)) { console.log(`FAIL cache: no ${heffBin} (run make build first)`); process.exit(1); }

const dir = mkdtempSync(join(tmpdir(), "heff-cache-"));
mkdirSync(join(dir, "lib"), { recursive: true });
const ls = spawnSync("bash", ["-c", "ls lib/*.hf"], { cwd: root, encoding: "utf8" });
for (const f of ls.stdout.split("\n").filter(Boolean)) copyFileSync(join(root, f), join(dir, "lib", basename(f)));

writeFileSync(join(dir, "a.hf"), "nine : Int\nnine = 9\n\nunused : Int\nunused = 1\n");
writeFileSync(join(dir, "b.hf"), "import a\n\nten : Int\nten = nine + 1\n");
writeFileSync(join(dir, "c.hf"), "import b\n\n-- >>> ten\n-- 10\n");

const heff = (...args) => run(heffBin, args, { cwd: dir });
// (the cache files of this heff's version: the directory vN under .heff/,
// among what else is kept there -- `.heff/run/`, `.heff/bin/`, ...)
const hc = (m) => join(dir, ".heff", readdirSync(join(dir, ".heff")).find((d) => /^v\d+$/.test(d)) ?? "v?", m + ".hc");
const mtime = (m) => statSync(hc(m)).mtimeMs;

let failed = 0;
const want = (cond, what) => { if (!cond) { failed++; console.log(`FAIL cache: ${what}`); } };

// cold: elaborated, frozen
const cold = await heff("check", "c.hf");
want(cold.status === 0, `cold check failed\n${cold.stderr.slice(0, 200)}`);
want(existsSync(hc("a")) && existsSync(hc("prelude")) && existsSync(hc("b")), "cold run froze a, prelude and b");

// warm: same answer, the dependencies' files untouched (thawed, not
// re-frozen)
const tA = mtime("a"), tP = mtime("prelude"), tB0 = mtime("b");
const warm = await heff("check", "c.hf");
want(warm.status === 0 && warm.stdout === cold.stdout && warm.stderr === cold.stderr, "warm check says what cold said");
want(mtime("a") === tA && mtime("prelude") === tP && mtime("b") === tB0, "warm run left the dependencies' cache files alone");

// corruption heals over
writeFileSync(hc("a"), "garbage");
const healed = await heff("check", "c.hf");
want(healed.status === 0 && healed.stdout === cold.stdout, "a corrupt cache file is elaborated over");
want(readFileSync(hc("a"), "utf8") !== "garbage", "and frozen again");

// each field a cache file is believed by, changed, is elaborated over:
// the file is frozen again as it was.  (Its tokens: a length-prefixed
// string `3:abc` whole, or a word.  `b` declares no fixities: version,
// module, source hash, no groups, no edges, how many imports -- those it
// has without writing them too -- each and its namespace's hash, how
// many outside definitions it needs, each and its hash, ...)
const tokens = (t) => {
  const out = [];
  for (let i = 0; i < t.length; ) {
    const m = /^(\d+):/.exec(t.slice(i, i + 12));
    let end = m ? i + m[0].length + Number(m[1]) : t.indexOf(" ", i);
    if (end < 0) end = t.length;
    out.push(t.slice(i, end));
    i = end + 1;
  }
  return out;
};
const altered = (tok) => tok.slice(0, -1) + (tok.endsWith("x") ? "y" : "x");
const alteredAt = (k) => (ts) => ts.map((t, i) => (i === k ? altered(t) : t));
const frozenB = readFileSync(hc("b"), "utf8");
const toksB = tokens(frozenB);
const imports = Number(toksB[5]), needs = 6 + 2 * imports;
want(toksB.join(" ") === frozenB && toksB[3] === "0" && toksB[4] === "0" && toksB.slice(6, needs).includes("1:a") && Number(toksB[needs]) > 0,
  `b's cache file reads as expected\n${frozenB.slice(0, 160)}`);
const mismatches = [
  ["its version", alteredAt(0)],
  ["its module", alteredAt(1)],
  ["its source's hash", alteredAt(2)],
  ["an import's namespace hash", alteredAt(7)],
  ["an import left out", (ts) => [...ts.slice(0, 5), String(imports - 1), ...ts.slice(8)]],
  ["a needed definition's hash", alteredAt(needs + 2)],
];
for (const [what, change] of mismatches) {
  writeFileSync(hc("b"), change(toksB).join(" "));
  const r = await heff("check", "c.hf");
  want(r.status === 0 && r.stdout === cold.stdout, `a cache file with ${what} changed: checked as before`);
  want(readFileSync(hc("b"), "utf8") === frozenB, `a cache file with ${what} changed is elaborated over`);
}

// a comment-only change deep down: the importer between still thaws
// (the entry file is always fresh; `b` is the one to watch)
const tB1 = mtime("b");
writeFileSync(join(dir, "a.hf"), "-- a comment\n\nnine : Int\nnine = 9\n\nunused : Int\nunused = 1\n");
const c1 = await heff("test", "c.hf");
want(c1.status === 0 && /1\/1 expectations hold/.test(c1.stdout), "the watch still holds after a comment-only change");
want(mtime("b") === tB1, "a comment-only change in a dependency invalidates no importer");

// a new name in the dependency re-elaborates the importer (its parse
// may read differently)
writeFileSync(join(dir, "a.hf"), "-- a comment\n\nnine : Int\nnine = 9\n\nunused : Int\nunused = 1\n\nmore : Int\nmore = 2\n");
const c2 = await heff("check", "c.hf");
want(c2.status === 0, "check after a new name in the dependency");
want(mtime("b") > tB1, "a new name in a dependency re-elaborates the importer");

// a changed definition the importer uses: the answer changes with it
writeFileSync(join(dir, "a.hf"), "nine : Int\nnine = 90\n\nunused : Int\nunused = 1\n");
const c3 = await heff("test", "c.hf");
want(c3.status === 1 && /91/.test(c3.stdout), `a changed definition reaches the importer\n${c3.stdout.slice(0, 120)}`);

// a record type's default changed: a record that leaves the field out,
// in an importer that names only a function of the type, is given the
// new one (the importer holds the default it was given)
const opts = (size) => `type Opts = { name : String, size : Int = ${size} }\n\nsizeOf : Opts -> Int\nsizeOf o = o.size\n`;
writeFileSync(join(dir, "d.hf"), opts(1));
writeFileSync(join(dir, "e.hf"), "import d\n\nsized : Int\nsized = sizeOf { name = \"e\" }\n");
writeFileSync(join(dir, "f.hf"), "import e\n\n-- >>> sized\n-- 1\n");
const d1 = await heff("test", "f.hf");
want(d1.status === 0 && /1\/1 expectations hold/.test(d1.stdout), `a default given\n${d1.stdout.slice(0, 120)}`);
writeFileSync(join(dir, "d.hf"), opts(2));
const d2 = await heff("test", "f.hf");
want(d2.status === 1 && /\b2\b/.test(d2.stdout), `a changed default reaches the importer\n${d2.stdout.slice(0, 120)}`);

// an instance gained or lost by a module imported, no name new and no
// definition changed: the importer's searches are made again -- one
// that found one instance finds two, then none
const eqs = (loose, exact) => [
  "theory Eq (a : Type) where", "  equal : a -> a -> Bool", "",
  "member : {a : Type} -> {{eq : Eq a}} -> a -> List a -> Bool",
  "member x Nil = False", "member x (Cons y ys) = eq.equal x y || member x ys", "",
  `${exact ? "instance " : ""}eqInt : Eq Int`, "eqInt = { equal = intEq }", "",
  `${loose ? "instance " : ""}eqLoose : Eq Int`, "eqLoose = { equal = \\a b -> True }", "",
].join("\n");
writeFileSync(join(dir, "g.hf"), eqs(false, true));
writeFileSync(join(dir, "h.hf"), "import g\n\nfound : Bool\nfound = member 1 [2]\n");
writeFileSync(join(dir, "i.hf"), "import h\n\n-- >>> found\n-- False\n");
const g1 = await heff("test", "i.hf");
want(g1.status === 0 && /1\/1 expectations hold/.test(g1.stdout), `an imported instance found\n${g1.stdout.slice(0, 200)}${g1.stderr.slice(0, 200)}`);
want(existsSync(hc("h")), "the importer frozen");
writeFileSync(join(dir, "g.hf"), eqs(true, true));
const g2 = await heff("check", "i.hf");
want(g2.status === 1 && g2.stderr.includes("E0342") && g2.stderr.includes("h.hf"), `an instance gained makes the importer's search ambiguous\n${g2.stderr.slice(0, 300)}`);
writeFileSync(join(dir, "g.hf"), eqs(false, false));
const g3 = await heff("check", "i.hf");
want(g3.status === 1 && g3.stderr.includes("E0341") && g3.stderr.includes("h.hf"), `an instance lost leaves the importer's search none\n${g3.stderr.slice(0, 300)}`);
writeFileSync(join(dir, "g.hf"), eqs(false, true));
const g4 = await heff("test", "i.hf");
want(g4.status === 0 && /1\/1 expectations hold/.test(g4.stdout), `the instance back, found again\n${g4.stdout.slice(0, 200)}${g4.stderr.slice(0, 200)}`);

// an instance's instance argument changed to one of another theory, of
// which there is none for the importer's search: its search is made
// again, and its error says what the instance wants; changed back, found
const sames = (wants) => [
  "theory Same (a : Type) where", "  same : a -> a -> Bool", "",
  "theory Like (a : Type) where", "  like : a -> a -> Bool", "",
  "isSame : {a : Type} -> {{s : Same a}} -> a -> a -> Bool", "isSame x y = s.same x y", "",
  "instance sameInt : Same Int", "sameInt = { same = intEq }", "",
  `instance sameList : {a : Type} -> {{s : ${wants} a}} -> Same (List a)`, "sameList = { same = \\xs ys -> True }", "",
].join("\n");
writeFileSync(join(dir, "j.hf"), sames("Same"));
writeFileSync(join(dir, "k.hf"), "import j\n\nfound : List Int -> Bool\nfound xs = isSame xs xs\n");
writeFileSync(join(dir, "l.hf"), "import k\n\n-- >>> found [1]\n-- True\n");
const j1 = await heff("test", "l.hf");
want(j1.status === 0 && /1\/1 expectations hold/.test(j1.stdout), `an instance found by finding another\n${j1.stdout.slice(0, 200)}${j1.stderr.slice(0, 200)}`);
want(existsSync(hc("k")), "the importer frozen");
writeFileSync(join(dir, "j.hf"), sames("Like"));
const j2 = await heff("check", "l.hf");
want(j2.status === 1 && j2.stderr.includes("E0341") && j2.stderr.includes("wanted by sameList") && j2.stderr.includes("k.hf"), `an instance that wants another now leaves the importer's search none\n${j2.stderr.slice(0, 300)}`);
writeFileSync(join(dir, "j.hf"), sames("Same"));
const j3 = await heff("test", "l.hf");
want(j3.status === 0 && /1\/1 expectations hold/.test(j3.stdout), `the instance's argument back, found again\n${j3.stdout.slice(0, 200)}${j3.stderr.slice(0, 200)}`);

console.log(`cache: ${failed ? "NOT " : ""}thawed, healed and invalidated at the definitions used`);
process.exit(failed ? 1 : 0);
