#!/usr/bin/env node
// A pretend page, for the example apps' tests: a DOM of plain objects,
// events fired by hand, time that passes when told to, and a pretend
// WebGPU device.  An app, compiled for the page (`--backend=page`), is
// loaded as `<script src>` loads it, a classic script in the page's
// window, and used as a person would; what the page shows then is said,
// a line of JSON each, `{ what, got }`, for the test to hold to what it
// wants.  (An app is JavaScript: only a JavaScript engine runs it.)
//
//   node test/page.mjs APP SCRIPT...   counter, clock, finder, aliased,
//                                      slept, clocked, painted, texts,
//                                      names, greet, two (counter's and
//                                      clock's), vecadd, pixels, cowns,
//                                      fuel, fuel-async
//   node test/page.mjs notes URL       the notes' page, of its server at
//                                      URL; it says `lose the server`
//                                      and waits for a line before the
//                                      last call
import { readFileSync } from "node:fs";
import { createInterface } from "node:readline";
import vm from "node:vm";

const said = (what, got) => console.log(JSON.stringify({ what, got: typeof got === "string" ? got : JSON.stringify(got) }));

class Node {
  constructor(tag) { this.tag = tag; this.children = []; this.textContent = ""; this.listeners = {}; this.attrs = {}; }
  appendChild(c) { this.children.push(c); }
  setAttribute(a, v) { this.attrs[a] = v; }
  replaceChildren() { this.children = []; }
  addEventListener(e, h) { (this.listeners[e] ??= []).push(h); }
  fire(e) { for (const h of this.listeners[e] ?? []) h({ target: this }); }
  // (a pretend canvas: its WebGPU context keeps what was configured and
  // the texture the GPU copies into)
  getContext(kind) {
    if (this.tag !== "canvas" || kind !== "webgpu") return null;
    return this.context ??= { configure: (o) => { this.format = o.format; }, texture: { bytes: null }, getCurrentTexture() { return this.texture; } };
  }
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
// (a script loaded into a window: whether it loads)
const load = (win, file, text = readFileSync(file, "utf8")) => {
  try { vm.runInContext(text, win, { filename: file }); return true; }
  catch (e) { said(`loading ${file}`, `threw ${e}`); return false; }
};
// (a page, filled, the app loaded on it)
const loaded = (file, fill = () => {}, more = {}) => {
  const page = new Node("body");
  fill(page);
  return load(pageWindow(page, more), file) ? page : null;
};
// (an element with an id, to be found: `#counter`, `#clock`)
const withBox = (id) => (page) => { const d = new Node("div"); d.setAttribute("id", id); page.appendChild(d); };
const withOut = (page) => { const p = new Node("p"); p.setAttribute("id", "out"); page.appendChild(p); };
const until = async (what, ok, ms = 20000) => {
  for (const t0 = Date.now(); !ok();) {
    if (Date.now() - t0 > ms) { said(what, `not in ${ms} ms`); return false; }
    await new Promise((r) => setTimeout(r, 20));
  }
  return true;
};
// (a device of plain objects: a buffer its bytes, a command its steps,
// done when submitted; its kernel adds the first two buffers bound into
// the third, as Float32s, or, of one buffer, numbers its elements, as
// UInt32s)
const pretendGpu = () => {
  const made = [], ran = [], codes = [];
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
        copyBufferToTexture: ({ buffer }, { texture }, [w, h]) => steps.push(() => { texture.bytes = buffer.bytes.slice(0, w * h * 4); }),
        beginComputePass: () => {
          let pipeline = null, group = null;
          return {
            setPipeline: (p) => { pipeline = p; }, setBindGroup: (i, g) => { if (i === 0) group = g; },
            dispatchWorkgroups: (x, y, z) => steps.push(() => {
              ran.push(`${pipeline.entry} ${x} ${y} ${z}`);
              if (group.buffers.length === 1) {
                const px = new Uint32Array(group.buffers[0].bytes.buffer);
                for (let i = 0; i < px.length; i++) px[i] = i;
                return;
              }
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
  return { made, ran, codes, gpu: { requestAdapter: () => Promise.resolve({ requestDevice: () => Promise.resolve(device) }) } };
};

const [app, ...files] = process.argv.slice(2);
const apps = {
  counter: () => {
    const page = loaded(files[0], withBox("counter"));
    if (!page) return;
    const [label, plus, minus] = page.children[0].children;
    said("at first", label.text);
    plus.fire("click"); plus.fire("click"); plus.fire("click");
    said("after 3 clicks", label.text);
    minus.fire("click");
    said("after -1", label.text);
  },
  clock: () => {
    const page = loaded(files[0], withBox("clock"));
    if (!page) return;
    const [label] = page.children[0].children;
    said("at first", label.text);
    for (let i = 0; i < 5; i++) tick();
    said("after 5 ticks", label.text);
  },
  // elements already on the page, found by a CSS selector and an XPath
  finder: () => {
    const page = loaded(files[0], (p) => {
      for (const id of ["out", "log"]) { const e = new Node("p"); e.setAttribute("id", id); p.appendChild(e); }
    });
    if (!page) return;
    const [out, log] = page.children;
    said("by css", out.text);
    said("by xpath", log.text);
    said("elements", page.children.length);
  },
  aliased: () => { const page = loaded(files[0], withOut); if (page) said("out", page.children[0].text); },
  // (boc's behaviours, in JavaScript's one thread: their turns go on
  // after the stack is let go of, every so often, so time is passed
  // until it has written)
  cowns: () => {
    const page = loaded(files[0], withOut);
    if (!page) return;
    const out = page.children[0];
    for (let i = 0; i < 100000 && out.text === "" && timers.length > 0; i++) tick();
    said("out", out.text);
  },
  // (std.fuel's limits, and boc's turns across them: as `cowns`)
  fuel: () => apps.cowns(),
  "fuel-async": () => apps.cowns(),
  slept: () => {
    const page = loaded(files[0], withOut);
    if (!page) return;
    said("before the time has passed", page.children[0].text);
    tick();
    said("after", page.children[0].text);
  },
  clocked: () => {
    const page = loaded(files[0], withOut, { performance });
    if (!page) return;
    tick();
    said("out", page.children[0].text);
  },
  painted: () => {
    const page = loaded(files[0], withOut);
    if (!page) return;
    const [out] = page.children;
    said("class", out.attrs.class);
    said("its stamp", out.children.map((c) => `${c.tag}: ${c.text}`).join());
    said("found again", out.attrs["data-same"]);
  },
  texts: () => { const page = loaded(files[0], withOut); if (page) said("out", page.children[0].children.map((c) => c.text).join("\n")); },
  names: () => { const page = loaded(files[0], withOut); if (page) said("out", page.children[0].text); },

  greet: () => {
    const page = loaded(files[0], (p) => {
      const input = new Node("input"); input.setAttribute("id", "name"); input.value = "";
      const e = new Node("p"); e.setAttribute("id", "greeting");
      p.appendChild(input); p.appendChild(e);
    });
    if (!page) return;
    const [input, p] = page.children;
    said("at first", p.text);
    input.value = "Ada"; input.fire("input");
    said("after typing", p.text);
  },
  // two apps on one page, each keeping what it defines to itself
  two: () => {
    const page = new Node("body");
    withBox("counter")(page);
    withBox("clock")(page);
    const win = pageWindow(page);
    if (!load(win, files[0]) || !load(win, files[1])) return;
    const [label, plus] = page.children[0].children;
    const [ticks] = page.children[1].children;
    plus.fire("click");
    tick();
    said("the counter", label.text);
    said("the clock", ticks.text);
  },
  // with no `navigator.gpu`, the page says so; given a pretend device,
  // the provider's wiring: buffers made and written, bound in order, the
  // kernel dispatched, the sum read back, the buffers let go of
  vecadd: async () => {
    const plain = loaded(files[0], withBox("vecadd"));
    if (plain) said("no WebGPU", plain.children[0].children[0].text);
    const pretend = pretendGpu();
    const page = loaded(files[0], withBox("vecadd"), { navigator: { gpu: pretend.gpu } });
    if (!page) return;
    const label = () => page.children[0].children[0].text;
    await until("on a pretend GPU", () => { tick(); return label() !== "adding"; });
    said("on a pretend GPU", label());
    said("its dispatch", pretend.ran.join());
    said("its kernel", pretend.codes.join("\n----\n"));
    said("its buffers let go of", pretend.made.map((b) => !!b.destroyed).join());
  },
  // a canvas the DOM made, given to the GPU's canvas effect: with no
  // `navigator.gpu`, the page says so; on a pretend device, the kernel
  // dispatched over the buffer, and the buffer copied into the canvas's
  // texture, the canvas configured for the device
  pixels: async () => {
    const plain = loaded(files[0], withBox("pixels"));
    if (plain) said("no WebGPU", plain.children[0].children[1].text);
    const pretend = pretendGpu();
    const page = loaded(files[0], withBox("pixels"), { navigator: { gpu: pretend.gpu } });
    if (!page) return;
    const [canvas, label] = page.children[0].children;
    await until("drawn", () => { tick(); return label.text !== "drawing"; });
    said("drawn", label.text);
    said("its dispatch", pretend.ran.join());
    said("its kernel", pretend.codes.join("\n----\n"));
    const shown = canvas.context && canvas.context.texture.bytes;
    said("on the canvas", `${canvas.tag} ${canvas.width}x${canvas.height} ${canvas.format}, ${shown ? shown.length : "no"} bytes, pixel 1: ${shown ? shown.subarray(4, 8).join() : "-"}`);
    said("its buffers let go of", pretend.made.map((b) => !!b.destroyed).join());
  },
  // the notes' page, its script the server's, its `fetch` Node's of the
  // server's URLs: each step waits for the page to say what came back
  notes: async () => {
    const base = files[0];
    const stray = [];
    process.on("unhandledRejection", (e) => stray.push(String(e && e.stack || e)));
    const notesPage = async () => {
      const page = new Node("body");
      withBox("notes")(page);
      const script = await fetch(`${base}/client.js`).then((r) => r.text());
      const win = pageWindow(page, { fetch: (u, o) => fetch(new URL(u, base), o), AbortController });
      return load(win, "client.js", script) ? page.children[0] : null;
    };
    // (the server up, once it answers)
    let html = null;
    for (let i = 0; i < 400 && html === null; i++) {
      html = await fetch(`${base}/`).then((r) => r.text(), () => null);
      if (html === null) await new Promise((r) => setTimeout(r, 25));
    }
    if (html === null) { said("the server", "not listening"); return; }
    said("served", /<script src="client.js">/.test(html));
    const box = await notesPage();
    if (!box) return;
    const [input, add, list, status] = box.children;
    const lines = () => list.children.map((li) => li.textContent).join("|");
    await until("at first", () => status.text === "0 notes");
    input.value = "milk"; add.fire("click");
    await until("a note added", () => status.text === "1 notes");
    input.value = "bread"; add.fire("click");
    await until("another", () => status.text === "2 notes");
    said("as the server has them", lines());
    list.children[0].children[0].fire("click");
    await until("a note toggled", () => lines() === "[x] milk|[ ] bread");
    // (another page: the same notes, the server's)
    const other = await notesPage();
    if (other) {
      await until("on another page", () => other.children[3].text === "2 notes");
      said("on another page", other.children[2].children.map((li) => li.textContent).join("|"));
    }
    // (a call of another build of the API: refused, and why)
    const frame = Buffer.from([0x68, 0x77, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
    const answer = Buffer.from(await fetch(`${base}/api`, { method: "POST", body: frame }).then((r) => r.arrayBuffer()));
    said("another build", `${answer.subarray(0, 2).toString("latin1")} ${answer[2]} ${answer[3]} ${answer[9]}`);
    said("why", /^the server's API is notes #[0-9a-f]{8}, the call's #00000000$/.test(answer.subarray(15).toString("latin1")));
    // (the server gone: a call comes to no answer, and the page says so)
    said("lose the server", "");
    await createInterface({ input: process.stdin })[Symbol.asyncIterator]().next();
    input.value = "tea"; add.fire("click");
    await until("a call to no server", () => status.text.startsWith("unreachable: /api: fetch: "));
    said("a call to no server", status.text.startsWith("unreachable: /api: fetch: "));
    said("as they were", lines());
    said("stray rejections", stray.join("\n"));
  },
};
await apps[app]();
process.exit(0);
