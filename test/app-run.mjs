#!/usr/bin/env node
// The example apps, compiled (`heff compile --backend=page`) and run on a pretend page: a DOM of
// plain objects, events fired by hand, and time that passes when told to.  Each app is loaded
// as `<script src>` loads it, a classic script (not a module) in the page's window.
import { spawn } from "node:child_process";
import { mkdirSync, readFileSync } from "node:fs";
import { connect, createServer } from "node:net";
import { join } from "node:path";
import vm from "node:vm";
import { heff, root } from "./heff.mjs";
import { slot } from "./pool.mjs";

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
const pageWindow = (page, more = {}) => {
  const win = vm.createContext({ document: pretendDocument(page), setTimeout: (f, ms) => { timers.push(f); }, TextDecoder, TextEncoder, ...more });
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
// the page's own input and paragraph
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
// A page and its server, one project (examples/notes): `heff build`
// makes the server, through C, and the page's script; the server runs,
// and the script, on a pretend page whose `fetch` is Node's, asks it
// for the notes (browser.remote, std.remote) -- each call's frame POSTed
// to it, its answer read back.  Each step waits for the page to say
// what came back; the server gone, a call's failure is said on the page.
const notes = join(root, "examples", "notes");
const until = async (what, ok, ms = 20000) => {
  for (const t0 = Date.now(); !ok();) {
    if (Date.now() - t0 > ms) { failed++; console.log(`FAIL ${what}: not in ${ms} ms`); return false; }
    await new Promise((r) => setTimeout(r, 20));
  }
  return true;
};
// A GPU (WebGPU's) on the pretend page: where it
// has no `navigator.gpu`, the page says so; given a pretend device, the
// provider's wiring -- buffers made and written, bound in order, the
// kernel dispatched, the sum copied to a buffer mapped and read back,
// the buffers let go of.  The device runs no WGSL: its kernel adds the
// first two buffers bound into the third, as Float32s; the WGSL it was
// given (gpu.wgsl's, of the example's `vecAdd`) is held to what is
// expected.  (A real GPU is checked by hand, in a browser with WebGPU.)
const vecadd = await run("examples/vecadd/vecadd.hf", "vecadd", withBox("vecadd"));
if (vecadd) check("vecadd, no WebGPU", vecadd.children[0].children[0].text, "gpu: WebGPU is not here");
// (a device of plain objects: a buffer its bytes, a command its steps,
// done when submitted)
const pretendGpu = () => {
  const made = [], said = [], codes = [];
  class Buffer {
    constructor(size, usage) { this.bytes = new Uint8Array(size); this.usage = usage; this.mapped = false; made.push(this); }
    destroy() { this.destroyed = true; }
    mapAsync(mode) { return new Promise((ok) => timers.push(() => { this.mapped = mode === 1; ok(); })); }
    getMappedRange() { if (!this.mapped) throw new Error("not mapped"); return this.bytes.buffer; }
    unmap() { this.mapped = false; }
  }
  const floats = (b) => new Float32Array(b.bytes.buffer, 0, b.bytes.length / 4);
  const device = {
    pushErrorScope() {}, popErrorScope: () => Promise.resolve(null),
    queue: {
      writeBuffer: (b, at, data) => b.bytes.set(data, at),
      submit: (commands) => { for (const c of commands) c(); },
      onSubmittedWorkDone: () => Promise.resolve(),
    },
    createBuffer: ({ size, usage }) => new Buffer(size, usage),
    createShaderModule: ({ code }) => (codes.push(code), { code, getCompilationInfo: () => Promise.resolve({ messages: [] }) }),
    createComputePipelineAsync: ({ compute }) => Promise.resolve({ entry: compute.entryPoint, getBindGroupLayout: (i) => ({ group: i }) }),
    createBindGroup: ({ layout, entries }) => ({ layout, buffers: entries.map((e, i) => e.binding === i ? e.resource.buffer : null) }),
    createCommandEncoder: () => {
      const steps = [];
      return {
        copyBufferToBuffer: (from, at, to, to0, n) => steps.push(() => to.bytes.set(from.bytes.subarray(at, at + n), to0)),
        beginComputePass: () => {
          let pipeline = null, group = null;
          return {
            setPipeline: (p) => { pipeline = p; }, setBindGroup: (i, g) => { if (i === 0) group = g; },
            dispatchWorkgroups: (x, y, z) => steps.push(() => {
              said.push(`${pipeline.entry} ${x} ${y} ${z}`);
              const [a, b, c] = group.buffers.map(floats);
              for (let i = 0; i < c.length; i++) c[i] = a[i] + b[i];
            }),
            end() {},
          };
        },
        finish: () => () => { for (const s of steps) s(); },
      };
    },
  };
  return { made, said, codes, gpu: { requestAdapter: () => Promise.resolve({ requestDevice: () => Promise.resolve(device) }) } };
};
if (vecadd) {
  const pretend = pretendGpu(), page = new Node("body");
  withBox("vecadd")(page);
  if (load(pageWindow(page, { navigator: { gpu: pretend.gpu } }), join(work, "vecadd.js"))) {
    const label = () => page.children[0].children[0].text;
    await until("vecadd, on a pretend GPU", () => { tick(); return label() !== "adding"; });
    check("vecadd, on a pretend GPU", label(), "ok");
    check("its dispatch: the entry point, the workgroups", pretend.said.join(), "add 16 1 1");
    check("its kernel, as WGSL", pretend.codes.join("\n----\n"), [
      "@group(0) @binding(0) var<storage, read> a : array<f32>;",
      "@group(0) @binding(1) var<storage, read> b : array<f32>;",
      "@group(0) @binding(2) var<storage, read_write> c : array<f32>;",
      "@compute @workgroup_size(64)",
      "fn add(@builtin(global_invocation_id) id : vec3<u32>) {",
      "  let v0 = id.x;",
      "  if (v0 < arrayLength(&c)) {",
      "    c[v0] = a[v0] + b[v0];",
      "  }",
      "}",
    ].join("\n"));
    check("its buffers and the staging one, let go of", pretend.made.map((b) => !!b.destroyed).join(), "true,true,true,true");
  }
}
const strayRejections = [];
process.on("unhandledRejection", (e) => strayRejections.push(String(e && e.stack || e)));
// (its API's test first: the calls through the wire, with no network)
const tested = await heff(["test"], { cwd: notes });
if (tested.status !== 0) { failed++; console.log(`FAIL heff test examples/notes\n${tested.stdout}${tested.stderr}`); }
const built = await heff(["build"], { cwd: notes, timeout: 600000 });
if (built.status !== 0) { failed++; console.log(`FAIL heff build examples/notes\n${built.stdout}${built.stderr}`); }
else {
  const port = await new Promise((ok) => { const s = createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => ok(p)); }); });
  const base = `http://127.0.0.1:${port}`;
  const done = await slot();
  const server = spawn(join(notes, ".heff", "bin", "server"), [String(port)], { cwd: notes, stdio: ["ignore", "pipe", "pipe"] });
  let said = "";
  server.stdout.on("data", (d) => { said += d; });
  server.stderr.on("data", (d) => { said += d; });
  const up = async () => {
    for (let i = 0; i < 400; i++) {
      if (await new Promise((ok) => { const c = connect({ host: "127.0.0.1", port }); c.on("connect", () => { c.destroy(); ok(true); }); c.on("error", () => ok(false)); })) return true;
      await new Promise((r) => setTimeout(r, 25));
    }
    return false;
  };
  // (a page of the notes, its script the server's, its `fetch` Node's
  // of the server's URLs)
  const notesPage = async () => {
    const page = new Node("body");
    withBox("notes")(page);
    const script = await fetch(`${base}/client.js`).then((r) => r.text());
    const win = pageWindow(page, { fetch: (u, o) => fetch(new URL(u, base), o), AbortController });
    try { vm.runInContext(script, win, { filename: "client.js" }); } catch (e) { failed++; console.log(`FAIL loading the notes' client: ${e}`); return null; }
    return page.children[0];
  };
  if (!(await up())) { failed++; console.log(`FAIL the notes' server: not listening\n${said}`); }
  else {
    const html = await fetch(`${base}/`).then((r) => r.text());
    check("the notes' page, served", /<script src="client.js">/.test(html), true);
    const box = await notesPage();
    if (box) {
      const [input, add, list, status] = box.children;
      const lines = () => list.children.map((li) => li.textContent).join("|");
      await until("the notes, at first", () => status.text === "0 notes");
      input.value = "milk"; add.fire("click");
      await until("a note added", () => status.text === "1 notes");
      input.value = "bread"; add.fire("click");
      await until("another", () => status.text === "2 notes");
      check("the notes, as the server has them", lines(), "[ ] milk|[ ] bread");
      list.children[0].children[0].fire("click");
      await until("a note toggled", () => lines() === "[x] milk|[ ] bread");
      // (another page: the same notes, the server's)
      const other = await notesPage();
      if (other) {
        await until("the notes, on another page", () => other.children[3].text === "2 notes");
        check("the notes, on another page", other.children[2].children.map((li) => li.textContent).join("|"), "[x] milk|[ ] bread");
      }
      // (a call of another build of the API: refused, and why)
      const frame = Buffer.from([0x68, 0x77, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
      const answer = Buffer.from(await fetch(`${base}/api`, { method: "POST", body: frame }).then((r) => r.arrayBuffer()));
      check("a call of another build: a failure (kind 2), another API (1)", `${answer.subarray(0, 2).toString("latin1")} ${answer[2]} ${answer[3]} ${answer[9]}`, "hw 1 2 1");
      check("why, said", /^the server's API is notes #[0-9a-f]{8}, the call's #00000000$/.test(answer.subarray(15).toString("latin1")), true);
      // (the server gone: a call comes to no answer, and the page says so)
      server.kill();
      await new Promise((r) => server.once("exit", r));
      input.value = "tea"; add.fire("click");
      await until("a call to no server", () => status.text.startsWith("unreachable: fetch: "));
      check("the notes, as they were", lines(), "[x] milk|[ ] bread");
    }
  }
  server.kill();
  done();
  if (strayRejections.length) { failed++; console.log(`FAIL the notes' page: ${strayRejections.join("\n")}`); }
}
console.log(`apps: ${failed ? "FAILED" : "counter, clock, finder, aliased, slept, painted, texts, names and greet run, and counter and clock on one page; vecadd with no WebGPU and on a pretend GPU; the notes' page and server, each call over the wire"}`);
process.exit(failed ? 1 : 0);
