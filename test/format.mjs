#!/usr/bin/env node
// The formatter (src/heff/format.hf): every .hf file of the repository
// (that parses) is formatted (the formatter checks itself that the result
// means the same and keeps the comments) and formatting it again changes
// nothing; then cases of comments, blank lines, blocks, watches and
// definitions kept as written (`-- fmt: skip`), some with the exact result.
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { heffBin, root } from "./heff.mjs";

// the formatter, compiled to a module of its own, its `format` called
// directly: `Right` the text laid out, `Left` why not
const module = join(root, ".heff", "out", "heff-format.mjs");
execFileSync(heffBin, ["compile", "src/heff/format.hf", module, "--backend=esm"], { cwd: root });
const { format } = (await import(pathToFileURL(module))).default;
let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}:\n${got}`); } };
const run = (src) => {
  const r = format(src);
  return r.$ === "Right" ? { ok: true, out: r.a[0] } : { ok: false, why: r.a[0] };
};
// formatted, and formatting it again changes nothing
const formats = (what, src, want) => {
  const r = run(src);
  if (!r.ok) return expect(what, false, "refused: " + r.why);
  const again = run(r.out);
  expect(what + " (again)", again.ok && again.out === r.out, again.ok ? again.out : "refused: " + again.why);
  if (want !== undefined) expect(what, r.out === want, r.out);
};

// (but those that do not parse on purpose, which are left)
const unparsed = ["test/errors/recover.hf"];
const files = execFileSync("git", ["ls-files", "*.hf"], { cwd: root, encoding: "utf8" }).split("\n").filter(Boolean)
  // (the index may still list a file the working copy has removed)
  .filter((f) => existsSync(join(root, f)));
for (const f of files) {
  const src = readFileSync(join(root, f), "utf8");
  if (!unparsed.includes(f)) formats(f, src);
  else { const r = run(src); expect(f + " is left", !r.ok && r.why.includes("does not parse"), JSON.stringify(r)); }
}

formats("spacing", "data Bool = True | False\nnot : Bool -> Bool\nnot   True =    False\nnot False = True\n",
  "data Bool = True | False\nnot : Bool -> Bool\nnot True = False\nnot False = True\n");

formats("blank lines, at most one", "data Bool = True | False\n\n\n\nx : Bool\n\nx = True\n",
  "data Bool = True | False\n\nx : Bool\n\nx = True\n");

formats("comments", [
  "-- a header",
  "",
  "-- | the doc of f",
  "f : Bool -> Bool -- after the type",
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
  "g : Bool",
  "g = f {- inline -} True",
  "-- at the end",
  "",
].join("\n"), [
  "-- a header",
  "",
  "-- | the doc of f",
  "f : Bool -> Bool -- after the type",
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
  "g : Bool",
  "g = f {- inline -} True",
  "-- at the end",
  "",
].join("\n"));

formats("a watch and its expectation, as written", [
  "test not/spec",
  "-- >>> not   True",
  "-- False",
  "",
  "-- >>> not False",
  "-- True",
  "",
].join("\n"), [
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
  "  t : Type",
  "  m : t -> t -> t",
  "data Bool = True | False",
  "orM : Monoid ~ { t = Bool }",
  "orM = { t = Bool, m = \\x y -> case x of",
  "    True -> True",
  "    False -> y }",
  "",
].join("\n"), [
  "theory Monoid where",
  "  t : Type",
  "  m : t -> t -> t",
  "data Bool = True | False",
  "orM : Monoid ~ { t = Bool }",
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
  "f : Bool -> Bool -> Bool",
  "f a b = if a then case b of",
  "    True -> False",
  "    False -> True",
  "  else b",
  "",
].join("\n"));

formats("do, nested blocks", [
  "import co",
  "data Unit = MkUnit",
  "k : {h : HFunctor} -> Co h Unit",
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
  "pick : Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then b else if b then c else if c then a else False",
  "",
].join("\n"), [
  "data Bool = True | False",
  "pick : Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then b else if b then c else if c then a else False",
  "",
].join("\n"));
formats("else if, broken", [
  "data Bool = True | False",
  "pick : Bool -> Bool -> Bool -> Bool",
  "pick a b c = if a then (if b then c else a) else if b then (if c then a else b) else if c then (if a then b else c) else False",
  "",
].join("\n"), [
  "data Bool = True | False",
  "pick : Bool -> Bool -> Bool -> Bool",
  "pick a b c =",
  "  if a then (if b then c else a)",
  "  else if b then (if c then a else b)",
  "  else if c then (if a then b else c)",
  "  else False",
  "",
].join("\n"));

// (its items two columns in from the line, not from the parenthesis)
formats("a block, the last argument", [
  "import co",
  "data Unit = MkUnit",
  "k : {h : HFunctor} -> Co h Unit",
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
  "k : {h : HFunctor} -> Co h Unit",
  "k = do",
  "  x <- val MkUnit",
  "  bind (val x) (\\y -> do",
  "    z <- val y",
  "    val z)",
  "",
].join("\n"));

formats("a long signature: the arrows under `::`", [
  "import co",
  "handle2 : {h : HFunctor} -> {a b : Type} -> h.f (Co h) a -> (a -> Co h b) -> Co h b",
  "handle2 p k = op p k",
  "",
].join("\n"), [
  "import co",
  "handle2 : {h : HFunctor}",
  "        -> {a b : Type}",
  "        -> h.f (Co h) a",
  "        -> (a -> Co h b)",
  "        -> Co h b",
  "handle2 p k = op p k",
  "",
].join("\n"));

formats("operators and their fixities", [
  "infixl   +  -",
  "infixl * above + -",
  "(+) : Int -> Int -> Int",
  "(+) = plus",
  "x : Int",
  "x = 1+2 * (3 -  4)",
  "long : Int",
  "long = aaaaaaaaaaaaaaaaaaaa + bbbbbbbbbbbbbbbbbbbbbbbbb * cccccccccccccccccccccc - dddddddddddddddd",
  "",
].join("\n"), [
  "infixl + -",
  "infixl * above + -",
  "(+) : Int -> Int -> Int",
  "(+) = plus",
  "x : Int",
  "x = 1 + 2 * (3 - 4)",
  "long : Int",
  "long =",
  "  aaaaaaaaaaaaaaaaaaaa",
  "  + bbbbbbbbbbbbbbbbbbbbbbbbb * cccccccccccccccccccccc",
  "  - dddddddddddddddd",
  "",
].join("\n"));

formats("let: on one line while it fits; else `in` under `let`, a chain", [
  "short : Int",
  "short = let  a = 1   in a",
  "long : Int -> Int",
  "long n = let aaaaaaaaaaaaaaaa = n + 1111111111111 + 22222222222222 in let bbbbbbbbbbbb = aaaaaaaaaaaaaaaa * 3333333333 in aaaaaaaaaaaaaaaa + bbbbbbbbbbbb",
  "blocky : Maybe Int -> Int",
  "blocky m =",
  "  let k : Int = case m of",
  "        Just x -> x",
  "        Nothing -> 0",
  "  in k + 1",
  "inDo : Maybe Int",
  "inDo = do",
  "  x <- Just 1",
  "  let  y : Int = x + 1",
  "  let MkPair a b = p",
  "  Just y",
  "",
].join("\n"), [
  "short : Int",
  "short = let a = 1 in a",
  "long : Int -> Int",
  "long n =",
  "  let aaaaaaaaaaaaaaaa = n + 1111111111111 + 22222222222222",
  "  in let bbbbbbbbbbbb = aaaaaaaaaaaaaaaa * 3333333333",
  "  in aaaaaaaaaaaaaaaa + bbbbbbbbbbbb",
  "blocky : Maybe Int -> Int",
  "blocky m =",
  "  let k : Int = case m of",
  "    Just x -> x",
  "    Nothing -> 0",
  "  in k + 1",
  "inDo : Maybe Int",
  "inDo = do",
  "  x <- Just 1",
  "  let y : Int = x + 1",
  "  let MkPair a b = p",
  "  Just y",
  "",
].join("\n"));

formats("where: under its equation, its declarations under it, with their comments", [
  "scaled : Int -> List Int -> List Int",
  "scaled k xs = go xs   -- the loop",
  "  where",
  "      -- multiply",
  "      times : Int -> Int",
  "      times x = k * x",
  "",
  "",
  "      go : List Int -> List Int",
  "      go Nil = Nil",
  "      go (y :: ys) = times y :: go ys -- recursion",
  "classify : Maybe Int -> Int",
  "classify m = case m of",
  "  Just x -> cap x",
  "  Nothing -> 0",
  "  where",
  "    cap : Int -> Int",
  "    cap x = x",
  "nested : Int -> Int",
  "nested n = outer n where",
  "  outer : Int -> Int",
  "  outer m = inner m",
  "     where",
  "       inner : Int -> Int",
  "       inner k = k * n",
  "",
].join("\n"), [
  "scaled : Int -> List Int -> List Int",
  "scaled k xs = go xs -- the loop",
  "  where",
  "    -- multiply",
  "    times : Int -> Int",
  "    times x = k * x",
  "",
  "    go : List Int -> List Int",
  "    go Nil = Nil",
  "    go (y :: ys) = times y :: go ys -- recursion",
  "classify : Maybe Int -> Int",
  "classify m = case m of",
  "  Just x -> cap x",
  "  Nothing -> 0",
  "  where",
  "    cap : Int -> Int",
  "    cap x = x",
  "nested : Int -> Int",
  "nested n = outer n",
  "  where",
  "    outer : Int -> Int",
  "    outer m = inner m",
  "      where",
  "        inner : Int -> Int",
  "        inner k = k * n",
  "",
].join("\n"));

formats("a record type's defaults: `= d` after the field, a long one on the next line", [
  "type Opts = {   name : String,   verbose:Bool=False , tags : List String = [] }",
  "type Long = { aVeryLongFieldName : String = \"a long default value\", another : List String = [\"one\", \"two\"], third : Int }",
  "theory Sig where",
  "  T : Type   =   Int",
  "  long : List String = [\"one\", \"two\", \"three\", \"four\", \"five\", \"six\", \"seven\", \"eight\"]",
  "",
].join("\n"), [
  "type Opts = { name : String, verbose : Bool = False, tags : List String = [] }",
  "type Long =",
  "  { aVeryLongFieldName : String = \"a long default value\"",
  "  , another : List String = [\"one\", \"two\"]",
  "  , third : Int",
  "  }",
  "theory Sig where",
  "  T : Type = Int",
  "  long : List String =",
  "    [\"one\", \"two\", \"three\", \"four\", \"five\", \"six\", \"seven\", \"eight\"]",
  "",
].join("\n"));

// `-- fmt: skip` right above a top-level declaration: the definition (its
// signature and the equations of its name after it) kept byte for byte,
// its comments too, up to the comment after its last token; what is
// around it laid out, the blank lines at its edges at most one
const kept = [
  "-- | the corners: x, y, then r, g, b",
  "-- fmt: skip",
  "corners : List Float",
  "corners =",
  "  [  0.0,  0.8,   1.0, 0.0, 0.0",
  "  , -0.8, -0.8,   0.0, 1.0, 0.0   -- left  ",
  "",
  "  ,  0.8, -0.8,   0.0, 0.0, 1.0",
  "  ]   {- after -}  -- and",
];
const pick = [
  "-- fmt: skip",
  "pick  :  Int -> Int",
  "pick 0  =  10",
  "pick n  =    n",
];
formats("a definition under `-- fmt: skip`, as written", [
  "", "", "",
  ...kept,
  "", "",
  "size  :  Int",
  "size =   512",
  ...pick,
  "next  :  Int",
  "next =   pick 1",
  "",
].join("\n"), [
  ...kept,
  "",
  "size : Int",
  "size = 512",
  ...pick,
  "next : Int",
  "next = pick 1",
  "",
].join("\n"));

// (formatted, it is as it was: what `fmt --check` holds)
const keptSrc = [...kept, "", ...pick, ""].join("\n");
const again = run(keptSrc);
expect("a definition as written is formatted", again.ok && again.out === keptSrc, JSON.stringify(again));

// (one definition: the next, of another name, laid out)
formats("`-- fmt: skip` keeps one definition, not those after it", [
  "-- fmt: skip",
  "red   = 1",
  "green  = 2",
  "-- fmt: skip",
  "blue  = 3",
  "",
].join("\n"), [
  "-- fmt: skip",
  "red   = 1",
  "green = 2",
  "-- fmt: skip",
  "blue  = 3",
  "",
].join("\n"));

// (a blank line under it, in a `where`, after code on its line, or at the
// end: a comment like another, what is under it laid out)
formats("`-- fmt: skip` elsewhere, a comment like another", [
  "-- fmt: skip",
  "",
  "f  =  1",
  "g = h  -- fmt: skip",
  "  where",
  "    -- fmt: skip",
  "    h  =  2",
  "-- fmt: skip",
  "",
].join("\n"), [
  "-- fmt: skip",
  "",
  "f = 1",
  "g = h -- fmt: skip",
  "  where",
  "    -- fmt: skip",
  "    h = 2",
  "-- fmt: skip",
  "",
].join("\n"));

const badKept = run("-- fmt: skip\nx : Bool\nx = (\n");
expect("a definition kept that does not parse is refused", !badKept.ok && badKept.why.includes("does not parse"), JSON.stringify(badKept));

formats("an implicit argument's default: `:=`, the binder written close", [
  "f : {   x : Bool:=False } -> {a b : Type := Int} -> a -> b -> Bool",
  "",
].join("\n"), [
  "f : {x : Bool := False} -> {a b : Type := Int} -> a -> b -> Bool",
  "",
].join("\n"));

console.log(`format: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
