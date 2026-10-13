#!/usr/bin/env node
// The backend's cache (.heff/bN/<module>.hb): a program compiled to C
// (as parts), or to a page's script, through it is the program compiled
// without it, byte for byte -- cold, warm, after an edit, and after a
// corrupt file; a warm run of a program rewrites none of its files; C
// after a page's script is the C made before; a definition that is inlined into another, or a copy made
// for it, changed, makes that other be made again: the program runs as
// the edit says; and a global newly named as a definition's local is,
// not mentioned by it, leaves its JavaScript as a cold run makes it.
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import vm from "node:vm";
import { heffBin, root } from "./heff.mjs";
import { run } from "./pool.mjs";

if (!existsSync(heffBin)) { console.log(`FAIL backcache: no ${heffBin} (run make build first)`); process.exit(1); }

const dir = mkdtempSync(join(tmpdir(), "heff-backcache-"));
mkdirSync(join(dir, "lib"), { recursive: true });
const ls = spawnSync("bash", ["-c", "ls lib/*.hf"], { cwd: root, encoding: "utf8" });
for (const f of ls.stdout.split("\n").filter(Boolean)) copyFileSync(join(root, f), join(dir, "lib", basename(f)));

// `step`, small: inlined where it is called; `loop`, calling itself: a
// copy of it made for `step`, `step` inlined in the copy
const a = (n) => `step : Int -> Int
step x = x + ${n}

loop : (Int -> Int) -> Int -> Int -> Int
loop f n acc = if n <= 0 then acc else loop f (n - 1) (f acc)
`;
writeFileSync(join(dir, "a.hf"), a(1));
writeFileSync(join(dir, "main.hf"), `import a
import co
import std.io

program : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
program = print (show (loop step 3 (step 10)))

main : Co stdio Unit
main = program (io := \\p -> p)
`);
// (the program for a page: what it shows written in its `#out`)
const pageOf = (imports, defs, shown) => `import co
import browser.dom
${imports}
${defs}
program : {{page : dom <: dom}} -> Co dom Unit
program = do
  out <- select "#out"
  found out (\\o -> text o (show (${shown})))

main : Co dom Unit
main = program (page := here dom)
`;
writeFileSync(join(dir, "page.hf"), pageOf("import a", "", "loop step 3 (step 10)"));

const heff = (...args) => run(heffBin, args, { cwd: dir });
const back = () => join(dir, ".heff", readdirSync(join(dir, ".heff")).find((d) => /^b\d+$/.test(d)) ?? "b?");
const hbs = () => existsSync(back()) ? readdirSync(back()).filter((f) => f.endsWith(".hb")).sort() : [];
// (the program compiled to parts in `out`, through the cache: its files)
const compile = async (out) => {
  rmSync(join(dir, out), { recursive: true, force: true });
  const r = await heff("compile", "main.hf", out + "/", "--name", "main", "--backend=c");
  if (r.status !== 0) return { error: r.stderr.slice(0, 400) };
  const files = {};
  for (const f of readdirSync(join(dir, out)).sort()) files[f] = readFileSync(join(dir, out, f), "utf8");
  return { files };
};
const same = (x, y) => !x.error && !y.error && JSON.stringify(x.files) === JSON.stringify(y.files);
const cold = async (out) => { for (const f of hbs()) rmSync(join(back(), f)); return compile(out); };
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const runs = async (out) => {
  const exe = join(dir, out, "prog");
  const b = await run(cc[0], [...cc.slice(1), "-O0", "-w", ...readdirSync(join(dir, out)).filter((f) => f.endsWith(".c")).map((f) => join(dir, out, f)), "-o", exe, "-lm", "-lpthread"]);
  if (b.status !== 0) return `(not built: ${b.stderr.slice(0, 200)})`;
  return (await run(exe, [])).stdout.trim();
};

let failed = 0;
const want = (cond, what) => { if (!cond) { failed++; console.log(`FAIL backcache: ${what}`); } };

// cold: compiled, kept
const c1 = await compile("c1");
want(!c1.error, `cold compile failed\n${c1.error}`);
want(hbs().includes("a.hb") && hbs().includes("main.hb"), `a cold run keeps a and main\n${hbs().join(" ")}`);

// warm: the same C, no file of the cache written again
const times = Object.fromEntries(hbs().map((f) => [f, statSync(join(back(), f)).mtimeMs]));
const w1 = await compile("w1");
want(same(c1, w1), "a warm run makes the C a cold run made");
want(hbs().every((f) => statSync(join(back(), f)).mtimeMs === times[f]), "a warm run leaves the cache's files alone");
want((await runs("w1")) === "14", `the program runs\n${await runs("w1")}`);

// an edit to `step`, inlined into main and into the copy of `loop`: the
// C as a cold run makes it, and the program as the edit says
writeFileSync(join(dir, "a.hf"), a(2));
const e1 = await compile("e1");
want(!e1.error && !same(c1, e1), `an edit to an inlined definition changes the C\n${e1.error ?? ""}`);
want((await runs("e1")) === "18", `an edit to an inlined definition reaches what inlines it\n${await runs("e1")}`);
const e2 = await cold("e2");
want(same(e1, e2), "after an edit, the C is what a cold run makes");

// back as it was: the C as it was
writeFileSync(join(dir, "a.hf"), a(1));
const b1 = await compile("b1");
want(same(c1, b1), "an edit undone: the C as it was");

// a corrupt file of the cache: compiled over, the same C
for (const f of hbs()) writeFileSync(join(back(), f), readFileSync(join(back(), f), "utf8").slice(0, 100));
const h1 = await compile("h1");
want(same(c1, h1), "corrupt files of the cache are compiled over");
const h2 = await compile("h2");
want(same(c1, h2), "and kept again");

// JavaScript, a page's script, through the same files: run on a
// pretend page, what it wrote in `#out`
const compileJs = async (out) => {
  const r = await heff("compile", "page.hf", out, "--name", "main", "--backend=page");
  if (r.status !== 0) return { error: r.stderr.slice(0, 400) };
  return { files: { js: readFileSync(join(dir, out), "utf8") } };
};
const coldJs = async (out) => { for (const f of hbs()) rmSync(join(back(), f)); return compileJs(out); };
const runsJs = (out) => {
  const shown = { textContent: "" };
  try { vm.runInNewContext(readFileSync(join(dir, out), "utf8"), { document: { querySelector: () => shown } }); }
  catch (e) { return `(threw ${e})`; }
  return shown.textContent;
};
const j1 = await coldJs("j1.js");
want(!j1.error, `cold JavaScript compile failed\n${j1.error}`);
want((await runsJs("j1.js")) === "14", `the JavaScript runs\n${await runsJs("j1.js")}`);
const times2 = Object.fromEntries(hbs().map((f) => [f, statSync(join(back(), f)).mtimeMs]));
const j2 = await compileJs("j2.js");
want(same(j1, j2), "a warm run makes the JavaScript a cold run made");
want(hbs().every((f) => statSync(join(back(), f)).mtimeMs === times2[f]), "a warm run of the page's leaves the cache's files alone");
const c2 = await compile("c2");
want(same(c1, c2), "C after JavaScript: the C a cold run made");
const c3 = await compile("c3");
want(same(c1, c3), "and again");
writeFileSync(join(dir, "a.hf"), a(2));
const je = await compileJs("je.js");
want(!je.error && !same(j1, je), `an edit to an inlined definition changes the JavaScript\n${je.error ?? ""}`);
want((await runsJs("je.js")) === "18", `an edit reaches the JavaScript\n${await runsJs("je.js")}`);
want(same(je, await coldJs("je2.js")), "after an edit, the JavaScript is what a cold run makes");
writeFileSync(join(dir, "a.hf"), a(1));
for (const f of hbs()) writeFileSync(join(back(), f), readFileSync(join(back(), f), "utf8").slice(0, 100));
want(same(j1, await compileJs("jh.js")), "corrupt files of the cache are compiled over, the JavaScript too");

// a global named as `count`'s local is, newly: `count` unchanged, its
// local as a cold run names it
const counting = (withX) => pageOf("", `${withX ? "\nx : Int\nx = 5\n" : ""}
count : Int -> Int -> Int
count x acc = if x <= 0 then acc else count (x - 1) (acc + x * x)
`, `count 3 ${withX ? "x" : "0"}`);
writeFileSync(join(dir, "page.hf"), counting(false));
const k1 = await compileJs("k1.js");
want(!k1.error && (await runsJs("k1.js")) === "14", `count runs\n${k1.error ?? await runsJs("k1.js")}`);
writeFileSync(join(dir, "page.hf"), counting(true));
const k2 = await compileJs("k2.js");
want(!k2.error && (await runsJs("k2.js")) === "19", `count runs past a global x\n${k2.error ?? await runsJs("k2.js")}`);
want(same(k2, await coldJs("k3.js")), "a global newly met: the JavaScript a cold run makes");

// copies settled (heff.compile.simplify, Settling): callers simplified
// again once the copies they call are made, copies not reached taken as
// not made, a loop's copies tied into one -- a clock's `tick` in a page,
// sleeping each round; four handlers stacked, in C
writeFileSync(join(dir, "page.hf"), `import co
import browser.dom

tick : {{page : dom <: dom}} -> Node -> Int -> Co dom Unit
tick label n = do
  sleep 100
  text label (show n ^ " ticks")
  tick label (n + 1)

program : {{page : dom <: dom}} -> Co dom Unit
program = do
  place <- select "#out"
  found place (\\r -> do
    label <- textIn r "p" "0 ticks"
    tick label 1)

main : Co dom Unit
main = program (page := here dom)
`);
const s1 = await coldJs("s1.js");
want(!s1.error, `a ticking page compiles\n${s1.error}`);
want(same(s1, await compileJs("s2.js")), "a ticking page: a warm run makes the JavaScript a cold run made");
writeFileSync(join(dir, "main.hf"), `import co
import std.io
import state
import exception

data AskF (m : Type -> Type) x = Ask (Int -> x)

reader : HFunctor
reader =
  { f = AskF
  , hfmap = \\m g p -> case p of
      Ask k -> Ask (\\v -> g (k v))
  , hmap = \\m n nt p -> case p of
      Ask k -> Ask k
  }

data TellF (m : Type -> Type) x = Tell Int x

writer : HFunctor
writer =
  { f = TellF
  , hfmap = \\m g p -> case p of
      Tell v x -> Tell v (g x)
  , hmap = \\m n nt p -> case p of
      Tell v x -> Tell v x
  }

-- the world the stack handles, nested to the right: globals built by
-- \`plus\`, known to the simplifier as the records they come to
W3 : HFunctor
W3 = plus (state Int) (exception Int)

W2 : HFunctor
W2 = plus writer W3

ask : {e : HFunctor} -> {{rd : reader <: e}} -> Co e Int
ask = op (rd (Ask (\\v -> v))) val

tell : {e : HFunctor} -> {{wr : writer <: e}} -> Int -> Co e Unit
tell v = op (wr (Tell v MkUnit)) val

-- | a round: each operation once, a throw caught some rounds
step : {e : HFunctor}
     -> {{rd : reader <: e}}
     -> {{wr : writer <: e}}
     -> {{st : state Int <: e}}
     -> {{ex : exception Int <: e}}
     -> Int
     -> Co e Int
step i = do
  k <- ask
  s <- get
  let s2 = (s * k + i) % 1000003
  put s2
  if s2 % 10 == 0 then tell s2 else pure MkUnit
  catch
      (if s2 % 13 == 0 then throw s2 else pure 1)
      (\\err -> pure (0 - err % 5))

-- | \`n\` rounds, what they give summed
loop : {e : HFunctor}
     -> {{rd : reader <: e}}
     -> {{wr : writer <: e}}
     -> {{st : state Int <: e}}
     -> {{ex : exception Int <: e}}
     -> Int
     -> Int
     -> Int
     -> Co e Int
loop n i acc =
  if i >= n then pure acc
  else do
    r <- step i
    loop n (i + 1) (acc + r)

-- | ... and a throw not caught, the round it is thrown in: \`n\` rounds
-- | at most
escaping : {e : HFunctor}
         -> {{st : state Int <: e}}
         -> {{ex : exception Int <: e}}
         -> Int
         -> Int
         -> Co e Int
escaping n i =
  if i >= n then get
  else do
    s <- get
    if s % 97 == 0 then throw i else put ((s * 7 + i) % 1000003)
    escaping n (i + 1)

data Out a = MkOut a Int

outValue : {a : Type} -> Out a -> a
outValue (MkOut x _) = x

passingF : (e : HFunctor) -> RawFunctor
passingF e =
  { f = \\a -> Int -> Co e (Out a)
  , fmap = \\g m s ->
        bind (m s) (\\o -> case o of
        MkOut x s2 -> val (MkOut (g x) s2))
  }

-- | a layer that keeps an Int as it handles \`h\`, into \`Co e\` of the
-- | rest: an operation of the rest is an operation again
passing : (h e : HFunctor)
        -> ({a : Type}
            -> h.f (\\x -> Int -> Co e (Out x)) a
            -> Int
            -> Co e (Out a))
        -> Handler (plus h e)
passing h e alg =
  { M = \\a -> Int -> Co e (Out a)
  , ret = \\x s -> val (MkOut x s)
  , bind = \\m k s ->
        bind (m s) (\\o -> case o of
        MkOut x s2 -> k x s2)
  , malg = \\p s -> case p of
      InL t -> alg t s
      InR q ->
        op
          (e.hmap
             (passingF e)
             (coFunctor e)
             (\\c -> bind (c s) (\\o -> val (outValue o)))
             q)
          (\\x -> val (MkOut x s))
  }

data Res a = Failed Int | Ok a

failing : Handler (exception Int)
failing =
  { M = \\a -> Res a
  , ret = \\x -> Ok x
  , bind = \\m k -> case m of
      Failed e -> Failed e
      Ok x -> k x
  , malg = \\p -> case p of
      Throw e -> Failed e
      Catch c h -> case c of
        Failed e -> h e
        Ok x -> Ok x
  }

-- | \`n\` rounds of the four, stacked
stacked : Int -> Res (Out (Out Int))
stacked n =
  let c1 =
    handle
      (through reader W2 (\\t -> case t of
       Ask k -> val (k 7)))
      (loop
         (rd := \\p -> InL p)
         (wr := \\p -> InR (InL p))
         (st := \\p -> InR (InR (InL p)))
         (ex := \\p -> InR (InR (InR p)))
         n
         0
         0)
  in let c2 =
    handle
      (passing writer W3 (\\t s -> case t of
       Tell v x -> val (MkOut x (s + v))))
      c1
      0
  in let c3 =
    handle
      (passing (state Int) (exception Int) (\\t s -> case t of
       Get k -> val (MkOut (k s) s)
       Put st x -> val (MkOut x st)))
      c2
      1
  in handle failing c3


shown : Res (Out (Out Int)) -> String
shown (Failed e) = "failed " ^ show e
shown (Ok (MkOut (MkOut acc told) st)) = show acc ^ " " ^ show told ^ " " ^ show st

program : {e : HFunctor} -> {{io : stdio <: e}} -> Co e Unit
program = print (shown (stacked 300))

main : Co stdio Unit
main = program (io := \\p -> p)
`);
const s3 = await cold("s3");
want(!s3.error, `four handlers stacked compile\n${s3.error}`);
const s4 = await compile("s4");
want(same(s3, s4), "four handlers stacked: a warm run makes the C a cold run made");
want((await runs("s4")) === (await runs("s3")), "and it runs as it did");

rmSync(dir, { recursive: true, force: true });
console.log(`backcache: ${failed ? "NOT " : ""}the same C and JavaScript through the cache, kept and made again where what it inlines changed`);
process.exit(failed ? 1 : 0);
