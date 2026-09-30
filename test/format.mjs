#!/usr/bin/env node
// The formatter (src/Heff/Format.purs): every .hf file of the repository is
// formatted (the formatter checks itself that the result means the same and
// keeps the comments) and formatting it again changes nothing; then cases
// of comments, blank lines, blocks and watches, some with the exact result.
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { format } from "../output/Heff.Format/index.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

// `format` lays out through the formatter written in heff: have the CLI
// compile it if it is stale, then load it, as `bin/heff.mjs` does
execFileSync("node", [ join(root, "bin", "heff.mjs"), "fmt", "--check", join(root, "lib", "doc.hf") ]);
globalThis.$heffFormatUnchecked = (await import(pathToFileURL(join(root, "output", "heff-fmt.mjs")))).default.formatUnchecked;
let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}:\n${got}`); } };
const run = (src) => {
  const r = format(src);
  return r.constructor.name === "Right" ? { ok: true, out: r.value0 } : { ok: false, why: r.value0 };
};
// formatted, and formatting it again changes nothing
const formats = (what, src, want) => {
  const r = run(src);
  if (!r.ok) return expect(what, false, "refused: " + r.why);
  const again = run(r.out);
  expect(what + " (again)", again.ok && again.out === r.out, again.ok ? again.out : "refused: " + again.why);
  if (want !== undefined) expect(what, r.out === want, r.out);
};

const files = execFileSync("git", ["ls-files", "*.hf"], { cwd: root, encoding: "utf8" }).split("\n").filter(Boolean);
for (const f of files) formats(f, readFileSync(join(root, f), "utf8"));

formats("spacing", "data Bool = True | False\nnot :: Bool -> Bool\nnot   True =    False\nnot False = True\n",
  "data Bool = True | False\nnot :: Bool -> Bool\nnot True = False\nnot False = True\n");

formats("blank lines, at most one", "data Bool = True | False\n\n\n\nx :: Bool\n\nx = True\n",
  "data Bool = True | False\n\nx :: Bool\n\nx = True\n");

formats("comments", [
  "-- a header",
  "",
  "import prelude",
  "",
  "-- | the doc of f",
  "f :: Bool -> Bool -- after the type",
  "f b = case b of -- after of",
  "  -- before an alternative",
  "  True -> False",
  "",
  "  False ->",
  "    -- before a body",
  "    True",
  "",
  "{- a block",
  "   comment -}",
  "g :: Bool",
  "g = f {- inline -} True",
  "-- at the end",
  "",
].join("\n"), [
  "-- a header",
  "",
  "import prelude",
  "",
  "-- | the doc of f",
  "f :: Bool -> Bool -- after the type",
  "f b = case b of -- after of",
  "  -- before an alternative",
  "  True -> False",
  "",
  "  False ->",
  "    -- before a body",
  "    True",
  "",
  "{- a block",
  "   comment -}",
  "g :: Bool",
  "g = f {- inline -} True",
  "-- at the end",
  "",
].join("\n"));

formats("a watch and its expectation, as written", [
  "import prelude",
  "",
  "test not/spec",
  "-- >>> not   True",
  "-- False",
  "",
  "-- >>> not False",
  "-- True",
  "",
].join("\n"), [
  "import prelude",
  "",
  "test not/spec",
  "-- >>> not   True",
  "-- False",
  "",
  "-- >>> not False",
  "-- True",
  "",
].join("\n"));

formats("a case in a record, past its commas", [
  "theory Monoid where",
  "  t :: Type",
  "  m :: t -> t -> t",
  "data Bool = True | False",
  "orM :: Monoid ~ { t = Bool }",
  "orM = { t = Bool, m = \\x y -> case x of",
  "    True -> True",
  "    False -> y }",
  "",
].join("\n"), [
  "theory Monoid where",
  "  t :: Type",
  "  m :: t -> t -> t",
  "data Bool = True | False",
  "orM :: Monoid ~ { t = Bool }",
  "orM =",
  "  { t = Bool",
  "  , m = \\x y -> case x of",
  "      True -> True",
  "      False -> y",
  "  }",
  "",
].join("\n"));

formats("a case in `then`, and `else` after it", [
  "data Bool = True | False",
  "f :: Bool -> Bool -> Bool",
  "f a b = if a then case b of",
  "    True -> False",
  "    False -> True",
  "  else b",
  "",
].join("\n"));

formats("do, nested blocks", [
  "import co",
  "data Unit = MkUnit",
  "k :: {h :: HFunctor} -> Co h Unit",
  "k = do",
  "  x <- val MkUnit",
  "",
  "  y <- do",
  "    z <- val x",
  "    val z",
  "  val y",
  "",
].join("\n"));

// (`else if`: a chain, at one depth)
formats("else if", [
  "data Bool = True | False",
  "pick :: Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then b else if b then c else if c then a else False",
  "",
].join("\n"), [
  "data Bool = True | False",
  "pick :: Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then b else if b then c else if c then a else False",
  "",
].join("\n"));
formats("else if, broken", [
  "data Bool = True | False",
  "pick :: Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then (if b then c else a) else if b then (if c then a else b) else if c then (if a then b else c) else False",
  "",
].join("\n"), [
  "data Bool = True | False",
  "pick :: Bool -> Bool -> Bool -> Bool",
  "pick a b c =",
  "  if a",
  "    then (if b then c else a)",
  "    else if b",
  "    then (if c then a else b)",
  "    else if c",
  "    then (if a then b else c)",
  "    else False",
  "",
].join("\n"));

// (its items two columns in from the line, not from the parenthesis)
formats("a block, the last argument", [
  "import co",
  "data Unit = MkUnit",
  "k :: {h :: HFunctor} -> Co h Unit",
  "k = do",
  "  x <- val MkUnit",
  "  bind (val x) (\\y ->",
  "     do",
  "       z <- val y",
  "       val z)",
  "",
].join("\n"), [
  "import co",
  "data Unit = MkUnit",
  "k :: {h :: HFunctor} -> Co h Unit",
  "k = do",
  "  x <- val MkUnit",
  "  bind (val x) (\\y -> do",
  "    z <- val y",
  "    val z)",
  "",
].join("\n"));

formats("a long signature: the arrows under `::`", [
  "import co",
  "handle2 :: {h :: HFunctor} -> {a b :: Type} -> h.f (Co h) a -> (a -> Co h b) -> Co h b",
  "handle2 p k = op p k",
  "",
].join("\n"), [
  "import co",
  "handle2 :: {h :: HFunctor}",
  "        -> {a b :: Type}",
  "        -> h.f (Co h) a",
  "        -> (a -> Co h b)",
  "        -> Co h b",
  "handle2 p k = op p k",
  "",
].join("\n"));

formats("operators and their fixities", [
  "infixl   +  -",
  "infixl * above + -",
  "(+) :: Int -> Int -> Int",
  "(+) = plus",
  "x :: Int",
  "x = 1+2 * (3 -  4)",
  "long :: Int",
  "long = aaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbb * cccccccccccccccccccccc - dddddddddddddddd",
  "",
].join("\n"), [
  "infixl + -",
  "infixl * above + -",
  "(+) :: Int -> Int -> Int",
  "(+) = plus",
  "x :: Int",
  "x = 1 + 2 * (3 - 4)",
  "long :: Int",
  "long =",
  "  aaaaaaaaaaaaaaaaaaaa",
  "  + bbbbbbbbbbbbbbbbbbbbbbbbb",
  "  * cccccccccccccccccccccc",
  "  - dddddddddddddddd",
  "",
].join("\n"));

const bad = run("x :: Bool\nx = (\n");
expect("what does not parse is left", !bad.ok && bad.why.includes("does not parse"), JSON.stringify(bad));

console.log(`format: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
