#!/usr/bin/env node
// The example apps, compiled (`heff compile --backend=page`) and run on a pretend page: a DOM of
// plain objects, events fired by hand, and time that passes when told to.  Each app is loaded
// as `<script src>` loads it, a classic script (not a module) in the page's window.
import { mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import vm from "node:vm";
import { heff, root } from "./heff.mjs";

const work = join(root, ".heff", "ref", "app-run");
mkdirSync(work, { recursive: true });

class Node {
  constructor(tag) { this.tag = tag; this.children = []; this.textContent = ""; this.listeners = {}; this.attrs = {}; }
  appendChild(c) { this.children.push(c); }
  setAttribute(a, v) { this.attrs[a] = v; }
  replaceChildren() { this.children = []; }
  addEventListener(e, h) { (this.listeners[e] ??= []).push(h); }
  fire(e) { for (const h of this.listeners[e] ?? []) h({ target: this }); }
  get text() { return this.textContent + this.children.map((c) => c.text).join(""); }
  // (the elements under this one, this one first)
  *all() { yield this; for (const c of this.children) yield* c.all(); }
}
// what the pretend page finds: `#id` (querySelector), `//tag[@id='id']` (evaluate)
const byId = (page, id, tag) => [...page.all()].find((n) => n.attrs.id === id && (!tag || n.tag === tag)) ?? null;
const pretendDocument = (page) => ({
  body: page, getElementById: (id) => byId(page, id), createElement: (t) => new Node(t),
  querySelector: (q) => { const m = /^#([\w-]+)$/.exec(q); return m ? byId(page, m[1]) : null; },
  evaluate: (q) => { const m = /^\/\/(\w+)\[@id='([\w-]+)'\]$/.exec(q); return { singleNodeValue: m ? byId(page, m[2], m[1]) : null }; },
});
let timers = [];
const tick = () => { const ts = timers; timers = []; for (const f of ts) f(); };
// The page's window: its global scope, which a classic script's top-level definitions would
// be part of, with a browser's text codecs.  `top` is the window's own and cannot be declared again (as a browser's
// `window.top`); `name`, `status`, `self` and `parent` are the window's too.
const pageWindow = (page) => {
  const win = vm.createContext({ document: pretendDocument(page), setTimeout: (f, ms) => { timers.push(f); }, TextDecoder, TextEncoder });
  vm.runInContext(`Object.defineProperty(globalThis, "top", { get: () => globalThis, configurable: false });
Object.assign(globalThis, { self: globalThis, parent: globalThis, name: "", status: "" });`, win);
  return win;
};

let failed = 0;
const check = (what, got, want) => {
  if (got !== want) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
};
// (an app, loaded into a window: whether it loads)
const load = (win, out) => {
  try { vm.runInContext(readFileSync(out, "utf8"), win, { filename: out }); return true; }
  catch (e) { failed++; console.log(`FAIL loading ${out}: ${e}`); return false; }
};
async function run(file, name, fill = () => {}) {
  const out = join(work, `${name}.js`);
  const r = await heff(["compile", file, out, "--name", name, "--backend=page"]);
  if (r.status !== 0) { failed++; console.log(`FAIL heff compile --backend=page ${file}\n${r.stdout}${r.stderr}`); return null; }
  const page = new Node("body");
  fill(page);
  return load(pageWindow(page), out) ? page : null;
}

// (an element with an id, to be found: `#counter`, `#clock`)
const withBox = (id) => (page) => { const d = new Node("div"); d.setAttribute("id", id); page.appendChild(d); };
const counter = await run("examples/counter/counter.hf", "counter", withBox("counter"));
if (counter) {
  const [label, plus, minus] = counter.children[0].children;
  check("counter at first", label.text, "count: 0");
  plus.fire("click"); plus.fire("click"); plus.fire("click");
  check("counter after 3 clicks", label.text, "count: 3");
  minus.fire("click");
  check("counter after -1", label.text, "count: 2");
}
const clock = await run("examples/clock/clock.hf", "clock", withBox("clock"));
if (clock) {
  const [label] = clock.children[0].children;
  check("clock at first", label.text, "0 ticks");
  for (let i = 0; i < 5; i++) tick();
  check("clock after 5 ticks", label.text, "5 ticks");
}
// elements already on the page, found by a CSS selector and an XPath
const withParagraphs = (page) => {
  for (const id of ["out", "log"]) { const p = new Node("p"); p.setAttribute("id", id); page.appendChild(p); }
};
const finder = await run("test/apps/select.hf", "finder", withParagraphs);
if (finder) {
  const [out, log] = finder.children;
  check("found by css", out.text, "found by css");
  check("found by xpath", log.text, "found by xpath");
  check("nothing else written", finder.children.length, 2);
}
// an app whose effect is a definition, `page = dom <+> (state Int)`
const withOut = (page) => { const p = new Node("p"); p.setAttribute("id", "out"); page.appendChild(p); };
const aliased = await run("test/apps/aliased.hf", "aliased", withOut);
if (aliased) check("an effect by its definition", aliased.children[0].text, "42");
// std.time beside the page's world, its provider the page's too
const slept = await run("test/apps/slept.hf", "slept", withOut);
if (slept) {
  check("std.time, before the time has passed", slept.children[0].text, "sleeping");
  tick();
  check("std.time, after", slept.children[0].text, "slept");
}
// a world of the test's own beside the page's, each acting on what the
// other's provider handed out: the world holds them for both (std.handle)
const painted = await run("test/apps/paint.hf", "painted", withOut);
if (painted) {
  const [out] = painted.children;
  check("painted by another world", out.attrs.class, "red");
  check("its stamp, put on the page", out.children.map((c) => `${c.tag}: ${c.text}`).join(), "mark: stamped");
  check("found again, the same handle", out.attrs["data-same"], "yes");
}
// a `JSString` (browser.js) field of an operation: a JavaScript string
// to the provider, given and taken through no helper (its length its
// text's UTF-16 code units, not its bytes)
const texts = await run("test/apps/jstext.hf", "texts", withOut);
if (texts) check("JSString given as it is", texts.children[0].children.map((c) => c.text).join("\n"),
  "5 bytes, 5 code units: plain\n7 bytes, 4 code units: é😀a\n3 bytes, 3 code units: x\u{FFFD}y");
// an app whose definitions have the names of the window's own
const names = await run("test/apps/names.hf", "names", withOut);
if (names) check("the window's names, the app's", names.children[0].text, "top name status self parent");
// the page's own input and paragraph (examples/greet/index.html)
const greetPage = (page) => {
  const input = new Node("input"); input.setAttribute("id", "name"); input.value = "";
  const p = new Node("p"); p.setAttribute("id", "greeting");
  page.appendChild(input); page.appendChild(p);
};
const greet = await run("examples/greet/greet.hf", "greet", greetPage);
if (greet) {
  const [input, p] = greet.children;
  check("greet at first", p.text, "Hello, whoever you are.");
  input.value = "Ada"; input.fire("input");
  check("greet after typing", p.text, "Hello, Ada!");
}
// two apps on one page, each keeping what it defines to itself
const shared = new Node("body");
withBox("counter")(shared);
withBox("clock")(shared);
const win = pageWindow(shared);
if (counter && clock && load(win, join(work, "counter.js")) && load(win, join(work, "clock.js"))) {
  const [label, plus] = shared.children[0].children;
  const [ticks] = shared.children[1].children;
  plus.fire("click");
  tick();
  check("two apps: the counter", label.text, "count: 1");
  check("two apps: the clock", ticks.text, "1 ticks");
}
console.log(`apps: ${failed ? "FAILED" : "counter, clock, finder, aliased, slept, painted, texts, names and greet run, and counter and clock on one page"}`);
process.exit(failed ? 1 : 0);
