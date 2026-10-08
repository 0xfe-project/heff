#!/usr/bin/env node
// The example apps, compiled (`heff compile --backend=page`) and run on a pretend page: a DOM of
// plain objects, events fired by hand, and time that passes when told to.
import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { heffBin } from "./heff.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
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
globalThis.setTimeout = (f, ms) => { timers.push(f); };
const tick = () => { const ts = timers; timers = []; for (const f of ts) f(); };

let failed = 0;
const check = (what, got, want) => {
  if (got !== want) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
};
async function run(file, name, fill = () => {}) {
  const out = join(work, `${name}.mjs`);
  const r = spawnSync(heffBin, ["compile", file, out, "--name", name, "--backend=page"], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL heff compile --backend=page ${file}\n${r.stdout}${r.stderr}`); return null; }
  const page = new Node("body");
  fill(page);
  globalThis.document = pretendDocument(page);
  await import(pathToFileURL(out) + `?${Date.now()}`);
  return page;
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
console.log(`apps: ${failed ? "FAILED" : "counter, clock, finder, aliased and greet run"}`);
process.exit(failed ? 1 : 0);
