#!/usr/bin/env node
// The example apps, compiled (`hg app`) and run on a pretend page: a DOM of
// plain objects, events fired by hand, and time that passes when told to.
import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const work = join(root, "ref", "app-run");
mkdirSync(work, { recursive: true });

class Node {
  constructor(tag) { this.tag = tag; this.children = []; this.textContent = ""; this.listeners = {}; this.attrs = {}; }
  appendChild(c) { this.children.push(c); }
  setAttribute(a, v) { this.attrs[a] = v; }
  replaceChildren() { this.children = []; }
  addEventListener(e, h) { (this.listeners[e] ??= []).push(h); }
  fire(e) { for (const h of this.listeners[e] ?? []) h({ target: this }); }
  get text() { return this.textContent + this.children.map((c) => c.text).join(""); }
}
let timers = [];
globalThis.setTimeout = (f, ms) => { timers.push(f); };
const tick = () => { const ts = timers; timers = []; for (const f of ts) f(); };

let failed = 0;
const check = (what, got, want) => {
  if (got !== want) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
};
async function run(file, name) {
  const out = join(work, `${name}.mjs`);
  const r = spawnSync("node", [join(root, "bin", "hg.mjs"), "app", file, name, out], { encoding: "utf8" });
  if (r.status !== 0) { failed++; console.log(`FAIL hg app ${file}\n${r.stdout}${r.stderr}`); return null; }
  const page = new Node("body");
  globalThis.document = { body: page, getElementById: () => null, createElement: (t) => new Node(t) };
  await import(pathToFileURL(out) + `?${Date.now()}`);
  return page;
}

const counter = await run("examples/counter.hg", "counter");
if (counter) {
  const [label, button] = counter.children;
  check("counter at first", label.text, "count: 0");
  button.fire("click"); button.fire("click"); button.fire("click");
  check("counter after 3 clicks", label.text, "count: 3");
}
const clock = await run("examples/clock.hg", "clock");
if (clock) {
  const [label] = clock.children;
  check("clock at first", label.text, "0 ticks");
  for (let i = 0; i < 5; i++) tick();
  check("clock after 5 ticks", label.text, "5 ticks");
}
console.log(`apps: ${failed ? "FAILED" : "counter and clock run"}`);
process.exit(failed ? 1 : 0);
