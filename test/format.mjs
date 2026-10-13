#!/usr/bin/env node
// The formatter (src/heff/syntax/format.hf), as `heff fmt` runs it: every
// .hf file of the repository (that parses) is formatted (the formatter
// checks itself that the result means the same and keeps the comments)
// and formatting it again changes nothing; then cases of comments, blank
// lines, blocks, watches, doc tests and definitions kept as written
// (`-- fmt: skip`), some with the exact result.
// (Each a file of its own, a copy, in a directory of the test's: the
// cases are gathered, then formatted by a few runs of `heff fmt` at once,
// each of many files; then what they made formatted again, the same way.)
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { heff, root } from "./heff.mjs";

const work = join(root, ".heff", "ref", "format");
rmSync(work, { recursive: true, force: true });
let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}:\n${got}`); } };

// the cases, each its text and what is to be held of what `heff fmt` made
// of it, `{ ok, out }` or `{ ok: false, why }`
const cases = [];
const run = (src, then, path) => cases.push({ src, then, path: path ?? join("case", `${cases.length}.hf`) });
// formatted, and formatting it again changes nothing
const formats = (what, src, want, path) => run(src, (r, again) => {
  if (!r.ok) return expect(what, false, "refused: " + r.why);
  expect(what + " (again)", again.ok && again.out === r.out, again.ok ? again.out : "refused: " + again.why);
  if (want !== undefined) expect(what, r.out === want, r.out);
}, path);

// (the files `heff fmt` is given, at `dir`, each case's text at its own
// path -- the repository's files under theirs -- formatted by a few runs
// at once: what each made of its case, or why it would not)
const formatted = async (dir, texts) => {
  const files = texts.map(({ path, text }) => {
    const f = join(dir, path);
    mkdirSync(dirname(f), { recursive: true });
    writeFileSync(f, text);
    return f;
  });
  const runs = 4, said = new Map();
  await Promise.all(Array.from({ length: runs }, async (_, i) => {
    const mine = files.filter((_, j) => j % runs === i);
    if (!mine.length) return;
    const r = await heff(["fmt", ...mine], { cwd: dir });
    // (`heff: FILE not formatted: why`, a refusal; a file not said, formatted)
    for (const m of r.stderr.split(/\n(?=heff: )/)) {
      const at = /^heff: (.*?) not formatted: ([\s\S]*)$/.exec(m);
      if (at) said.set(at[1], at[2]);
    }
    if (r.status !== 0 && ![...mine].some((f) => said.has(f))) for (const f of mine) said.set(f, `heff fmt: ${r.stderr}${r.error ?? ""}`);
  }));
  return files.map((f) => said.has(f) ? { ok: false, why: said.get(f) } : { ok: true, out: readFileSync(f, "utf8") });
};

// (but those that do not parse, or lex, on purpose, which are left)
const unparsed = ["test/errors/instance-where.hf", "test/errors/negation-pattern.hf", "test/errors/recover.hf", "test/errors/rune-literals.hf", "test/errors/string-escapes.hf", "test/errors/bytes-literals.hf"];
const files = execFileSync("git", ["ls-files", "*.hf"], { cwd: root, encoding: "utf8" }).split("\n").filter(Boolean)
  // (the index may still list a file the working copy has removed)
  .filter((f) => existsSync(join(root, f)));
for (const f of files) {
  const src = readFileSync(join(root, f), "utf8");
  if (!unparsed.includes(f)) formats(f, src, undefined, f);
  else run(src, (r) => expect(f + " is left", !r.ok && /does not (parse|lex)/.test(r.why), JSON.stringify(r)), f);
}

formats("spacing", "data Bool = True | False\nnot : Bool -> Bool\nnot   True =    False\nnot False = True\n",
  "data Bool = True | False\nnot : Bool -> Bool\nnot True = False\nnot False = True\n");

formats("alternatives written broken stay broken", "data Tree (a : Type)\n  = Leaf\n  | Node a (Tree a) (Tree a)\n",
  "data Tree (a : Type)\n  = Leaf\n  | Node a (Tree a) (Tree a)\n");
formats("alternatives broken anywhere break each", "data Tree (a : Type) = Leaf\n  | Node a (Tree a) (Tree a)\n",
  "data Tree (a : Type)\n  = Leaf\n  | Node a (Tree a) (Tree a)\n");
formats("alternatives written flat, if they fit", "data Tree (a : Type) = Leaf | Node a (Tree a) (Tree a)\n",
  "data Tree (a : Type) = Leaf | Node a (Tree a) (Tree a)\n");

formats("rune literals, as written", "x : List Rune\nx = ['\\u{41}',   '😀', '\\'',x' ]\n",
  "x : List Rune\nx = ['\\u{41}', '😀', '\\'', x']\n");

formats("string escapes, as written", "x : String\nx =   \"\\u{41}\\u{1F600}\\u\\n\\\"\"\n",
  "x : String\nx = \"\\u{41}\\u{1F600}\\u\\n\\\"\"\n");

formats("bytes literals, as written", "x : List Bytes\nx = [b\"\\u{41}\\xFF\",   b\"é\\n\\\"\",b\"\" ]\n",
  "x : List Bytes\nx = [b\"\\u{41}\\xFF\", b\"é\\n\\\"\", b\"\"]\n");
formats("a bytes literal pattern", "m : Bytes -> Int\nm   b\"GET\" = 1\nm _ = 0\n",
  "m : Bytes -> Int\nm b\"GET\" = 1\nm _ = 0\n");
// (a `b` apart from a string, or in a name, is a name's)
formats("b and a string, apart", "x : Int\nx = f b   \"s\" ab\"t\"\n",
  "x : Int\nx = f b \"s\" ab \"t\"\n");

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

formats("doc tests, in their doc comment: as written, after the `>>>` and a space", [
  "-- | the doc of f",
  "-- | >>>   f   True",
  "-- | False",
  "-- |",
  "-- | more of it",
  "-- | >>> f False",
  "f : Bool -> Bool",
  "f   b = b",
  "",
  "-- >>>f True",
  "-- True",
  "",
].join("\n"), [
  "-- | the doc of f",
  "-- | >>> f   True",
  "-- | False",
  "-- |",
  "-- | more of it",
  "-- | >>> f False",
  "f : Bool -> Bool",
  "f b = b",
  "",
  "-- >>> f True",
  "-- True",
  "",
].join("\n"));

formats("`-- fmt: skip` above a definition's doc tests", [
  "-- | the doc of m",
  "-- fmt: skip",
  "-- | >>> m",
  "-- | [1, 0]",
  "m : List Int",
  "m  =  [ 1,",
  "        0 ]",
  "n  =  1",
  "",
].join("\n"), [
  "-- | the doc of m",
  "-- fmt: skip",
  "-- | >>> m",
  "-- | [1, 0]",
  "m : List Int",
  "m  =  [ 1,",
  "        0 ]",
  "n = 1",
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
  "corners : List Float64",
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
run(keptSrc, (again) => expect("a definition as written is formatted", again.ok && again.out === keptSrc, JSON.stringify(again)));

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

run("-- fmt: skip\nx : Bool\nx = (\n", (badKept) =>
  expect("a definition kept that does not parse is refused", !badKept.ok && badKept.why.includes("does not parse"), JSON.stringify(badKept)));

formats("an implicit argument's default: `:=`, the binder written close", [
  "f : {   x : Bool:=False } -> {a b : Type := Int} -> a -> b -> Bool",
  "",
].join("\n"), [
  "f : {x : Bool := False} -> {a b : Type := Int} -> a -> b -> Bool",
  "",
].join("\n"));

formats("`-x`: written close, the space before it kept, a subtraction's `-` spaced", [
  "f : Int -> Int",
  "f n = n",
  "",
  "g : Int -> Int",
  "g x = f  -x  +  -(f   x)-x",
  "",
].join("\n"), [
  "f : Int -> Int",
  "f n = n",
  "",
  "g : Int -> Int",
  "g x = f -x + -(f x) - x",
  "",
].join("\n"));

// (an instance's signature keeps `instance` before it -- what it means
// holds it there -- its equations as any definition's; under `-- fmt:
// skip`, kept with them)
formats("`instance`: kept before the signature, the equations after it", [
  "-- | the integers' equality",
  "instance   eqInt  :Eq Int",
  "eqInt =  { equal = intEq }",
  "",
  "-- fmt: skip",
  "instance eqFloat  :  Eq Float64",
  "eqFloat  =  { equal = floatEq }",
  "",
].join("\n"), [
  "-- | the integers' equality",
  "instance eqInt : Eq Int",
  "eqInt = { equal = intEq }",
  "",
  "-- fmt: skip",
  "instance eqFloat  :  Eq Float64",
  "eqFloat  =  { equal = floatEq }",
  "",
].join("\n"));

// Line breaks as written (#75): a construct written over several lines
// stays broken though it would fit; written on one line, it stays on one.
const asWritten = (what, broken, flat) => {
  formats(what + ", written broken, stays broken", broken, broken);
  formats(what + ", written on one line, stays on one", flat, flat);
};
asWritten("a record type", [
  "type ElabCtx =",
  "  { type_context : List VType",
  "  , env : List Value",
  "  , metas : List Value",
  "  }",
  "",
].join("\n"), "type ElabCtx = { type_context : List VType, env : List Value }\n");
asWritten("a record", [
  "r : R",
  "r =",
  "  { a = 1",
  "  , b = 2",
  "  }",
  "",
].join("\n"), "r : R\nr = { a = 1, b = 2 }\n");
formats("a record broken anywhere breaks at each comma", [
  "r : R",
  "r = { a = 1,",
  "      b = 2 }",
  "",
].join("\n"), [
  "r : R",
  "r =",
  "  { a = 1",
  "  , b = 2",
  "  }",
  "",
].join("\n"));
asWritten("a record update", [
  "u : R -> R",
  "u s =",
  "  { s",
  "  | a = 1",
  "  , b = 2",
  "  }",
  "",
].join("\n"), "u : R -> R\nu s = { s | a = 1, b = 2 }\n");
asWritten("a list", [
  "xs : List Int",
  "xs =",
  "  [ 1",
  "  , 2",
  "  , 3",
  "  ]",
  "",
].join("\n"), "xs : List Int\nxs = [1, 2, 3]\n");
asWritten("a tuple", [
  "p : Pair Int Int",
  "p =",
  "  ( 1",
  "  , 2",
  "  )",
  "",
].join("\n"), "p : Pair Int Int\np = (1, 2)\n");
asWritten("an anonymous sum", [
  "T : Type",
  "T =",
  "  data { A Int",
  "       | B }",
  "",
].join("\n"), "T : Type\nT = data { A Int | B }\n");
asWritten("an application", [
  "x : Int",
  "x =",
  "  f",
  "    1",
  "    2",
  "",
].join("\n"), "x : Int\nx = f 1 2\n");
asWritten("if, then, else", [
  "f : Bool -> Int",
  "f b =",
  "  if b",
  "    then 1",
  "  else 2",
  "",
].join("\n"), "f : Bool -> Int\nf b = if b then 1 else 2\n");
asWritten("else, on a line of its own", [
  "f : Bool -> Int",
  "f b =",
  "  if b then 1",
  "  else 2",
  "",
].join("\n"), "f : Bool -> Int\nf b = if b then 1 else 2\n");
asWritten("a signature's arrows", [
  "f : Int",
  "  -> Int",
  "  -> Int",
  "f a b = a",
  "",
].join("\n"), "f : Int -> Int -> Int\nf a b = a\n");
asWritten("a chain of operators", [
  "infixl + -",
  "x : Int",
  "x =",
  "  1",
  "  + 2",
  "  - 3",
  "",
].join("\n"), "infixl + -\nx : Int\nx = 1 + 2 - 3\n");
asWritten("a chain cut at its loosest", [
  "infixl + -",
  "infixl * above + -",
  "x : Int",
  "x =",
  "  1 * 2",
  "  + 3 * 4",
  "",
].join("\n"), "infixl + -\ninfixl * above + -\nx : Int\nx = 1 * 2 + 3 * 4\n");
formats("a chain broken after an operator, broken before it", [
  "f : Int ->",
  "  Int",
  "f a = a",
  "",
].join("\n"), [
  "f : Int",
  "  -> Int",
  "f a = a",
  "",
].join("\n"));
asWritten("let, in", [
  "x : Int",
  "x =",
  "  let a = 1",
  "  in a",
  "",
].join("\n"), "x : Int\nx = let a = 1 in a\n");

// each case formatted; what was, formatted again
const first = await formatted(join(work, "first"), cases.map((c) => ({ path: c.path, text: c.src })));
const made = cases.map((c, i) => ({ path: c.path, text: first[i].out, i })).filter((m) => first[m.i].ok);
const second = await formatted(join(work, "again"), made);
const again = new Map(made.map((m, j) => [m.i, second[j]]));
cases.forEach((c, i) => c.then(first[i], again.get(i)));

console.log(`format: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
