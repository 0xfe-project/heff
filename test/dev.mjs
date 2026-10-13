#!/usr/bin/env node
// `heff dev` (src/heff/build/dev.hf): a project of a C program that runs
// until it is ended (a server's shape) and a page's program, built and
// run -- the program's output heff's -- and the page served, with what
// reloads it; a source changed, built again and the program run anew; a
// change that does not build said and waited past, the program kept; and
// heff dev ended by SIGTERM, what it started ended with it.
//
//   node test/dev.mjs
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { heffBin } from "./heff.mjs";
import { run, slot } from "./pool.mjs";

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

const work = mkdtempSync(join(tmpdir(), "heff-dev-"));
await run(heffBin, ["init", "proj"], { cwd: work });
const proj = join(work, "proj");
writeFileSync(join(proj, "app", "proj", "main.hf"), [
  "import co", "import proj.greeting", "import std.io", "import std.time as Time", "",
  "world : HFunctor", "world = stdio <+> Time.time", "",
  "forever : {e : HFunctor} -> {{clock : Time.time <: e}} -> Co e Unit",
  "forever = do", "  Time.sleep 1000", "  forever", "",
  "program : {e : HFunctor} -> {{io : stdio <: e}} -> {{clock : Time.time <: e}} -> Co e Unit",
  "program = do", "  printErr (greeting \"server\")", "  forever", "",
  "main : Co world Unit", "main = program (io := \\p -> InL p) (clock := \\p -> InR p)", "",
].join("\n"));
mkdirSync(join(proj, "app", "page"));
writeFileSync(join(proj, "app", "page", "build.hf"), "import manifest\n\nbuild : Build\nbuild = { backend = Page, run = \"main\" }\n");
writeFileSync(join(proj, "app", "page", "main.hf"), [
  "import co", "import browser.dom", "import proj.greeting", "",
  "program : {{page : dom <: dom}} -> Co dom Unit",
  "program = bind (select \"#page\") (\\p -> found p (\\r -> bind (textIn r \"p\" (greeting \"page\")) (\\x -> pure MkUnit)))", "",
  "main : Co dom Unit", "main = program (page := here dom)", "",
].join("\n"));
writeFileSync(join(proj, "app", "page", "style.css"), "p { color: teal; }\n");

// (a port no one listens at, for the pages)
const port = await new Promise((r) => { const s = createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => r(p)); }); });

const free = await slot();
const dev = spawn(heffBin, ["dev", "--port", String(port)], { cwd: proj, stdio: ["ignore", "pipe", "pipe"] });
let said = "";
dev.stdout.on("data", (d) => { said += d; });
dev.stderr.on("data", (d) => { said += d; });
const ended = new Promise((r) => dev.on("close", (status, signal) => r({ status, signal })));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (re, ms = 60000) => { const t = Date.now(); while (!re.test(said)) { if (Date.now() - t > ms) return false; await sleep(50); } return true; };
const get = async (path) => {
  try { const r = await fetch(`http://127.0.0.1:${port}${path}`, { redirect: "manual" }); return { status: r.status, type: r.headers.get("content-type"), text: await r.text() }; }
  catch (e) { return { status: 0, text: String(e) }; }
};

try {
  // built, the program run, the pages served
  expect("built, and run", await until(/built \(1\); running proj\n/) && await until(/Hello, server!\n/) && await until(/the pages at http:\/\/127\.0\.0\.1:\d+\//), said);
  const index = await get("/"), page = await get("/page/"), js = await get("/page/page.js"), css = await get("/page/style.css");
  expect("the pages' index", index.status === 200 && index.text.includes("<a href=\"/page/\">page</a>"), index);
  expect("a page made for the program", page.status === 200 && page.text.includes("<div id=\"page\"></div>") &&
    page.text.includes("<script src=\"page.js\"></script>") && page.text.includes("fetch(\"/.heff/built\""), page);
  expect("its script, as built", js.status === 200 && js.type.startsWith("text/javascript") &&
    js.text === readFileSync(join(proj, ".heff", "bin", "page.js"), "utf8"), { status: js.status, type: js.type });
  expect("a file of the program's", css.status === 200 && css.type.startsWith("text/css") && css.text === "p { color: teal; }\n", css);
  expect("built, said to the page", (await get("/.heff/built")).text === "1", await get("/.heff/built"));
  expect("a name without its slash", (await get("/page")).status === 301, await get("/page"));
  expect("no page of no program", (await get("/nothing/")).status === 404, await get("/nothing/"));
  expect("nothing outside the program's", (await get("/page/..%2f..%2fpackage.hf")).status === 404 && (await get("/page/../package.hf")).status !== 200, null);

  // a change: built again, the program run anew
  const g = join(proj, "src", "proj", "greeting.hf"), text = readFileSync(g, "utf8");
  writeFileSync(g, text.replace("\"Hello, \"", "\"Howdy, \""));
  expect("a change, built and run again", await until(/built \(2\); running proj\n/) && await until(/Howdy, server!\n/), said);
  expect("built again, said to the page", (await get("/.heff/built")).text === "2", null);

  // a change that does not build: said, waited past, the program kept
  writeFileSync(g, text.replace("\"Hello, \"", "\"Hello, \" ^ 1"));
  expect("a change that does not build", await until(/error\[E0311\][\s\S]*heff dev: not built; built again at the next change\n/), said);
  expect("the program kept", !/proj ended/.test(said) && (await get("/.heff/built")).text === "2", said);
  writeFileSync(g, text);
  expect("mended, built again", await until(/built \(3\); running proj\n/), said);
} finally {
  // ended by SIGTERM: what it started ended too (else its streams would
  // be held open by them, and it would not be seen to end)
  dev.kill("SIGTERM");
  const end = await Promise.race([ended, sleep(10000).then(() => null)]);
  expect("ended, with what it started", end !== null && end.signal === "SIGTERM", end);
  if (end === null) dev.kill("SIGKILL");
  free();
}

console.log(failed ? `dev: ${failed}/${checks} checks FAILED` : `dev: ${checks} checks`);
process.exit(failed ? 1 : 0);
