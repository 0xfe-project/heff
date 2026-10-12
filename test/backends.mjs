#!/usr/bin/env node
// The backends held to the interpreter and to one another.
//
//   node test/backends.mjs [N] [SEED]
//
// Values: the programs of test/programs and N generated ones (test/gen.mjs,
// from SEED) compiled by each backend -- JavaScript (`--backend=esm`, its
// definitions shown by test/load-js.mjs) and C (`--backend=c` without
// `--name`: the program shows the definitions it is asked for), built by
// `zig cc` (`cc` where there is no zig) -- and each definition the
// interpreter has a value of (`heff dump-eval`) compared.
//
// Programs: those of test/node, compiled to a program by each backend
// (`--backend=node`, run by Node.js; `--backend=c --name`), run: what they
// write, their exit status, loops a million rounds deep, a long loop of
// operations done at once, time and the network; and, by C alone,
// processes forked, and threads truly at once.
// (Each program apart from the others, as many at once as there are cores.)
import { spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { setSeed, genProgram } from "./gen.mjs";
import { heffBin, root } from "./heff.mjs";
import { pool, run } from "./pool.mjs";

const [nArg = "100", seedArg = "7"] = process.argv.slice(2);
setSeed(seedArg);
const work = join(root, ".heff", "ref", `backends-${seedArg}`);
rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });

// (the C compiler: zig's, else the system's; HEFF_SANITIZE=1: built with
// AddressSanitizer and UBSan, so that a reference count wrong shows, a use
// after free, where it is)
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const ccFlags = process.env.HEFF_SANITIZE ? ["-O0", "-g", "-fsanitize=address,undefined"] : ["-O1"];
const buildC = (c, bin, flags = []) => run(cc[0], [...cc.slice(1), ...ccFlags, ...flags, "-w", c, "-o", bin, "-lm", "-lpthread"]);
// (heff run with `args`, a run that does not end in two minutes a
// failure, not a wait)
const heff = (args, opts = {}) => run(heffBin, args, { timeout: 120000, ...opts });

// Each backend: compiled, its definitions `wanted` shown as JSON,
// { name: { value } | { threw } }; a failure is { error }
const backends = {
  js: async (file, base, wanted) => {
    const out = `${base}.mjs`;
    const r = await heff(["compile", file, out, "--backend=esm"]);
    if (r.status !== 0) return { compile: r.stderr || String(r.error) };
    return { load: await run(process.execPath, [join(root, "test", "load-js.mjs"), out, ...wanted], { timeout: 30000 }) };
  },
  c: async (file, base, wanted) => {
    const r = await heff(["compile", file, `${base}.c`, "--backend=c"]);
    if (r.status !== 0) return { compile: r.stderr || String(r.error) };
    const b = await buildC(`${base}.c`, base);
    if (b.status !== 0) return { compile: b.stderr || String(b.error) };
    return { load: await run(base, wanted, { timeout: 30000 }) };
  },
};

// (what fails is said in the order of the checks, whichever ends first)
let failed = 0;
const said = (msgs) => { for (const m of msgs) { failed++; console.log(m); } };

// Values
// ---------------------------------------------------------------------

const cases = [];
for (const f of readdirSync(join(root, "test", "programs")).filter((f) => f.endsWith(".hf")).sort())
  cases.push({ name: f.replace(".hf", ""), src: readFileSync(join(root, "test", "programs", f), "utf8") });
for (let i = 0; i < Number(nArg); i++) cases.push({ name: `gen${i}`, src: genProgram() });

const funsOut = (v) => v.replace(/\w+ = #fun(, )?/g, "").replace(/, \}/g, " }");
const erasedOut = (v) => v.replace(/\w+ = #erased(, )?/g, "").replace(/, \}/g, " }").replace(/ #erased/g, "");

let compared = 0;
const values = pool(cases, async (c) => {
  const msgs = [], fail = (msg) => msgs.push(msg);
  const file = join(work, `${c.name}.hf`);
  writeFileSync(file, c.src);
  const ev = await heff(["dump-eval", file]);
  // (a value the interpreter met a bug of the compiler in agrees with
  // nothing: said, and the program not compiled)
  const bugs = (ev.stdout ?? "").split("\n").filter((l) => /^\S+ = <compiler bug: /.test(l));
  if (bugs.length) { fail(`COMPILER BUG ${c.name}:\n${bugs.join("\n")}`); return msgs; }
  if (ev.status !== 0) { fail(`NOT EVALUATED ${c.name}:\n${ev.stderr}${ev.error || ""}`); return msgs; }
  // `name = value`, a value going on over the lines that do not start so;
  // an operator's name without its parentheses, as the module exports it
  const interp = new Map();
  for (const block of ev.stdout.split(/\n(?=\S+ = )/)) {
    const m = /^(\S+) = ([\s\S]*)$/.exec(block.trim());
    if (m) interp.set(m[1].replace(/^\((.*)\)$/, "$1"), m[2].replace(/\s*\n\s*/g, " "));
  }
  // (what the interpreter found to diverge is not read: compiled, with its
  // loops as loops, it may run for ever)
  const wanted = [...interp.keys()].filter((n) => interp.get(n) !== "<diverged>");
  for (const [be, compile] of Object.entries(backends)) {
    const r = await compile(file, join(work, `${c.name}-${be}`), wanted);
    if (r.compile !== undefined) { fail(`NOT COMPILED ${c.name} (${be}):\n${r.compile}`); continue; }
    if (r.load.status !== 0 || r.load.error) { fail(`LOAD FAILED ${c.name} (${be}): ${(r.load.stderr || String(r.load.error ?? r.load.signal)).split("\n").slice(0, 6).join("\n")}`); continue; }
    const got = JSON.parse(r.load.stdout);
    for (let [name, want] of interp) {
      if (want === "#erased" || want === "<diverged>") continue;
      const g = got[name];
      const at = `${c.name}.${name} (${be})`;
      // (all are call-by-value: what abandons in one abandons in the others)
      if (want === "<abandoned>") {
        compared++;
        if (!(g && g.threw && g.threw.includes("heff:abandon"))) fail(`DIFF ${at}: compiled ${g && (g.value ?? g.threw)}, interpreted <abandoned>`);
        continue;
      }
      if (g && g.threw) { fail(`DIFF ${at}: compiled threw ${g.threw}, interpreted ${want}`); continue; }
      // (what is a type is not in the compiled code at all; a function shows
      // nothing but that it is one; a type family, erased, is not there)
      want = erasedOut(want);
      if (want === "#fun" && (!g || g.value === "#fun" || g.value === "undefined")) continue;
      want = funsOut(want);
      compared++;
      if (!g || funsOut(g.value) !== want) fail(`DIFF ${at}: compiled ${g && g.value}, interpreted ${want}`);
    }
  }
  return msgs;
});

// Programs
// ---------------------------------------------------------------------

// (a program, compiled by each backend: how each is run; its files named
// by its file and its name; `cflags`, the C compiler's more)
const programs = {
  js: async (file, name) => {
    const out = join(work, `${basename(file, ".hf")}-${name}.mjs`);
    const r = await heff(["compile", file, out, "--name", name, "--backend=node"], { cwd: root });
    return r.status !== 0 ? { error: r.stderr } : { cmd: process.execPath, pre: [out] };
  },
  c: async (file, name, cflags = []) => {
    const c = join(work, `${basename(file, ".hf")}-${name}.c`), bin = join(work, `${basename(file, ".hf")}-${name}-c`);
    const r = await heff(["compile", file, c, "--name", name, "--backend=c"], { cwd: root });
    if (r.status !== 0) return { error: r.stderr };
    const b = await buildC(c, bin, cflags);
    return b.status !== 0 ? { error: b.stderr } : { cmd: bin, pre: [] };
  },
};

// (the program `name` of `file` run by each backend in turn with `args`,
// what it wrote and its status held to `want`; what fails, said)
const ran = async (what, file, name, args, want, { cflags, ...opts } = {}) => {
  const msgs = [];
  for (const [be, build] of Object.entries(programs)) {
    const p = await build(file, name, cflags);
    if (p.error !== undefined) { msgs.push(`FAIL ${what} (${be}): not compiled\n${p.error}`); continue; }
    const r = await run(p.cmd, [...p.pre, ...args], { timeout: 60000, ...opts });
    const got = { out: r.stdout, err: r.stderr, status: r.status };
    for (const k of Object.keys(want))
      if (got[k] !== want[k]) msgs.push(`FAIL ${what} (${be}): ${k} ${JSON.stringify(got[k])}, wanted ${JSON.stringify(want[k])}`);
  }
  return msgs;
};
const checks = [];

// (each backend's world: stdin, the environment, arguments, a file)
checks.push((async () => {
  const copied = join(work, "copied.txt");
  const msgs = await ran("echo", "test/node/echo.hf", "main", [copied, "two words"],
    { out: `${copied}\ntwo words\nhi\ncopied: from stdin\nmissing: not read\n`, err: "to stderr\n", status: 3 },
    { cwd: root, input: "from stdin", env: { ...process.env, GREETING: "hi" } });
  if (readFileSync(copied, "utf8") !== "from stdin") msgs.push("FAIL echo: the file");
  return msgs;
})());

// (a string is bytes, to the system and back, the same on each backend:
// what is not UTF-8 -- an overlong, a surrogate, beyond U+10FFFF, cut
// short, a byte of no rune -- read and written as it is, by stdin,
// stdout and a file named not in ASCII; made text, as TextDecoder
// makes it; the arguments and the environment, which Node.js has as
// text alone, text on each, given by a shell as bytes)
checks.push((async () => {
  const msgs = [];
  const input = Buffer.from([0x61, 0xc0, 0xaf, 0xe0, 0x80, 0x80, 0xed, 0xa0, 0x80, 0xf4, 0x90, 0x80, 0x80,
    0xf0, 0x9f, 0x98, 0x20, 0xff, 0x80, 0x00, 0x0d, 0x0a, 0xef, 0xbb, 0xbf, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80, 0xe2, 0x82]);
  const hex = (b) => Buffer.from(b).toString("hex");
  const text = (b) => hex(Buffer.from(new TextDecoder("utf-8", { ignoreBOM: true }).decode(b)));
  const arg = Buffer.from([0x78, 0xed, 0xa0, 0x80, 0xc3, 0xa9, 0xff]), env = Buffer.from([0xe2, 0x82, 0x79]);
  const octal = (b) => [...b].map((c) => "\\" + c.toString(8).padStart(3, "0")).join("");
  const want = input.toString("latin1") + "\n"
    + `${input.length} bytes, ${[...new TextDecoder("utf-8", { ignoreBOM: true }).decode(input)].length} runes, UTF-8: no\n`
    + `as text: ${text(input)}\n`
    + "file: same\n"
    + `names: ${hex(Buffer.from("bytes-é😀.bin"))}\n`
    + "a NUL in a path, written: no\n"
    + "a NUL in a path, read: no\n"
    + `arguments: ${text(arg)}\n`
    + `environment: ${text(env)}\n`;
  for (const [be, build] of Object.entries(programs)) {
    const p = await build("test/node/bytes.hf", "main");
    if (p.error !== undefined) { msgs.push(`FAIL bytes (${be}): not compiled\n${p.error}`); continue; }
    const dir = join(work, `bytes-${be}`);
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir);
    const r = await run("sh", ["-c", `HEFF_BYTES="$(printf '${octal(env)}')" exec "$@" "$(printf '${octal(arg)}')"`, "sh", p.cmd, ...p.pre, dir],
      { timeout: 60000, input, encoding: "latin1" });
    if (r.stdout !== want || r.status !== 0)
      msgs.push(`FAIL bytes (${be}): status ${r.status}, out ${JSON.stringify(r.stdout)}, wanted ${JSON.stringify(want)}\n${r.stderr}`);
    else if (!readFileSync(join(dir, "bytes-é😀.bin")).equals(input)) msgs.push(`FAIL bytes (${be}): the file`);
  }
  return msgs;
})());

// (`heff run`: compiled for Node.js, then run in heff's place, on its streams)
checks.push((async () => {
  const r = await heff(["run", "test/node/echo.hf", join(work, "run.txt"), "x"], { cwd: root, input: "in", env: { ...process.env, GREETING: "hi" } });
  return r.status !== 3 || r.stderr !== "to stderr\n" ? [`FAIL heff run: status ${r.status}, stderr ${JSON.stringify(r.stderr)}`] : [];
})());

// time and the network: done later, the rest going on then
checks.push(ran("sleep and fetch", "test/node/timed.hf", "timed", [], { out: "fetched\n", status: 0 }));

// the network, by C alone (Node.js's fetch gives a page of 404 as it
// is): a server of the test's own and `data:` URLs fetched by libcurl in
// the process and by the curl spawned (`-DHEFF_NET_SPAWN`), the same
// text from each -- a page, a failure's message -- and each as wanted
// (where it is curl's version's to say, the start of it).  Run again
// with no curl on the PATH, libcurl's the same still: it is in the
// process, not spawned.
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  const big = "0123456789abcdef".repeat(16384);
  const server = createServer((q, r) => {
    const say = (status, body, headers = {}) => { r.writeHead(status, headers); r.end(body); };
    switch (q.url) {
      case "/item": return say(200, "item 3\n".repeat(4));
      case "/nope": return say(404, "not here");
      case "/bad": return say(400, "bad");
      case "/empty": return say(200);
      case "/moved": return say(302, "moving", { Location: "/ping" });
      case "/loop": return say(302, "", { Location: "/loop" });
      case "/ping": return say(200, "pong");
      case "/big": return say(200, big, { "Content-Length": big.length });
      case "/chunked": r.write("one,"); r.write("two,"); return r.end("three");
      case "/agent": return say(200, q.headers["user-agent"] ?? "none");
      case "/short": r.writeHead(200, { "Content-Length": 100 }); r.write("cut"); return setTimeout(() => r.socket.destroy(), 50);
      default: return say(500);
    }
  });
  const listening = (s) => new Promise((ok) => s.listen(0, "127.0.0.1", () => ok(s.address().port)));
  const port = await listening(server);
  const shut = createServer(), refused = await listening(shut);
  shut.close();
  const at = `http://127.0.0.1:${port}`, failed = "TypeError: fetch failed";
  const cases = [
    [`${at}/item`, "28 item 3\nitem 3\nitem 3\nitem 3\n"],
    [`${at}/nope`, `73 ${failed}: curl: (22) The requested URL returned error: 404`],
    [`${at}/bad`, `73 ${failed}: curl: (22) The requested URL returned error: 400`],
    [`http://127.0.0.1:${refused}/`, new RegExp(`^\\d+ ${failed}: curl: \\(7\\) Failed to connect to 127\\.0\\.0\\.1 port ${refused}`)],
    ["data:,hi%20there", "8 hi there"],
    ["data:text/plain;base64,aGkgdGhlcmU=", "8 hi there"],
    ["data:no-comma", `23 ${failed}`],
    [`http://localhost:${port}/ping`, "4 pong"],
    [`${at}/empty`, "0 "],
    [`${at}/moved`, "4 pong"],
    [`${at}/loop`, new RegExp(`^\\d+ ${failed}: curl: \\(47\\) `)],
    [`${at}/chunked`, "13 one,two,three"],
    [`${at}/agent`, /^\d+ curl\/\S+$/],
    [`${at}/short`, new RegExp(`^\\d+ ${failed}: curl: \\(18\\) `)],
    [`127.0.0.1:${port}/ping`, "4 pong"],
    ["nope://x/", new RegExp(`^\\d+ ${failed}: curl: \\(1\\) `)],
    ["http://", new RegExp(`^\\d+ ${failed}: curl: \\(3\\) `)],
    [`${at}/big`, `${big.length} ${big}`],
  ];
  const urls = cases.map(([u]) => u);
  // (what the program wrote, a fetch's lines each, by `cases`: a page may
  // have lines of its own)
  const want = cases.map(([, w]) => w);
  const split = (out) => {
    const got = [];
    let rest = out;
    for (const w of want) {
      const n = typeof w === "string" ? w.length : rest.indexOf("\n");
      got.push(rest.slice(0, n));
      rest = rest.slice(n + 1);
    }
    return got;
  };
  const curt = (s) => JSON.stringify(s.length > 120 ? `${s.slice(0, 120)}...` : s);
  const outs = {};
  const noCurl = { ...process.env, PATH: join(work, "no-such-dir") };
  for (const [built, cflags, runs] of [
    ["libcurl", [], [["libcurl", process.env], ["libcurl, no curl on the PATH", noCurl]]],
    ["spawned", ["-DHEFF_NET_SPAWN"], [["spawned", process.env]]],
  ]) {
    const p = await programs.c("test/node/fetched.hf", "main", cflags);
    if (p.error !== undefined) { msgs.push(`FAIL the network, ${built} (c): not compiled\n${p.error}`); continue; }
    for (const [how, env] of runs) {
      const r = await run(p.cmd, urls, { timeout: 60000, env });
      if (r.status !== 0 || r.stderr) { msgs.push(`FAIL the network, ${how} (c): status ${r.status}\n${r.stderr}`); continue; }
      const got = split(r.stdout);
      cases.forEach(([u, w], i) => {
        if (typeof w === "string" ? got[i] !== w : !w.test(got[i]))
          msgs.push(`FAIL the network, ${how} (c): ${u} gave ${curt(got[i])}, wanted ${curt(String(w))}`);
      });
      outs[how] = r.stdout.replace(/ after \d+ ms/g, " after N ms");
    }
  }
  for (const how of Object.keys(outs).slice(1))
    if (outs[how] !== outs.libcurl) msgs.push(`FAIL the network, ${how} (c): not as libcurl's\n${curt(outs[how])}\n${curt(outs.libcurl)}`);
  server.close();
  return msgs;
})());

// a module named `main` (main.hf, in no project), compiled to C as
// parts: its part, main.c, is not the program's entry, which is a part of
// its own
checks.push((async () => {
  const alone = mkdtempSync(join(tmpdir(), "heff-main-"));
  copyFileSync(join(root, "test", "node", "main.hf"), join(alone, "main.hf"));
  const dir = join(alone, "parts"), bin = join(work, "main-parts-c");
  const r = await heff(["compile", "main.hf", "parts/", "--name", "main", "--backend=c"], { cwd: alone });
  const parts = r.status === 0 ? readdirSync(dir).filter((f) => f.endsWith(".c")) : [];
  const b = r.status === 0 ? await run(cc[0], [...cc.slice(1), ...ccFlags, "-w", ...parts.map((f) => join(dir, f)), "-o", bin, "-lm", "-lpthread"]) : r;
  const out = b.status === 0 ? await run(bin, [], { timeout: 60000 }) : b;
  rmSync(alone, { recursive: true, force: true });
  return parts.includes("main.c") && out.status === 0 && out.stdout === "main, a module of its own: 3 2 1\n" ? []
    : [`FAIL a module named main, as parts: ${parts.join(" ")}\n${out.stdout ?? ""}${out.stderr ?? ""}`];
})());

// a computation of an effect the backend has no provider for is refused,
// and why
for (const be of ["node", "c"]) checks.push((async () => {
  const r = await heff(["compile", "examples/counter/counter.hf", join(work, `counter.${be}`), "--name", "counter", `--backend=${be}`], { cwd: root });
  return r.status !== 0 && r.stderr.includes("counter has the effect std.dom.dom, which is not provided here") ? []
    : [`FAIL an app is not a program (${be}): ${r.status} ${r.stderr}`];
})());

// one world handled at home (a counter: state, peeled off in the
// language), the rest handed to the backend's providers: the evidence as
// instance implicits, `main` asks for it, its body finds it, `run` gives it
checks.push(ran("evidence found in scope", "test/node/evidence.hf", "run", ["one", "two"], { out: "args: one two\nbye\n", status: 0 }));
checks.push(ran("state peeled in-language", "test/node/counted.hf", "main", [], { out: "0: one\n1: two\n2: three\ncounted 3\n", status: 0 }));

// two worlds of the test's own, one's provider acting on the boxes the
// other's hands out, the world holding them for both (std.handle): held
// again, the same number; let go of, freed by their maker's provider
checks.push(ran("handles shared", "test/node/tally.hf", "main", [],
  { out: "bumped: 42\nheld again: the same; another: not the same\nkept: 5050\nlet go of: 51\n", status: 0 }));

// a `JSString` (std.js) field of an operation: a JavaScript string to the
// JavaScript provider, given and taken through no helper (its length its
// text's UTF-16 code units, not its bytes), and to the C one a string of
// UTF-8, valid, which says the same
checks.push(ran("JSString given as it is", "test/node/jstext.hf", "main", [],
  { out: "5 bytes, 5 code units: plain\n7 bytes, 4 code units: é😀a\n3 bytes, 3 code units: x\u{FFFD}y\n", status: 0 }));

// processes, by C alone (test/node/fork.provider.c forks; Node.js cannot
// give a continuation to another process): the rest of the program run
// by each of them, each on its own copy, their strings gathered in
// order, and lib/parallel.hf's `Par` each side a process -- what was
// written before a spread written once
if (process.platform !== "win32") checks.push((async () => {
  const p = await programs.c("test/node/fork.hf", "main");
  if (p.error !== undefined) return [`FAIL processes (c): not compiled\n${p.error}`];
  const r = await run(p.cmd, [], { timeout: 60000 });
  const want = "said before the spread, once\nsquares: 0 1 4 9\nnested: 0.0,0.1,0.2 1.0,1.1,1.2\n"
    + "each its own: 0 15 30; the parent's: 15\nfib 15, each Par forked: 610\n";
  return r.status === 0 && r.stdout === want ? [] : [`FAIL processes (c): status ${r.status}\n${r.stdout}${r.stderr}`];
})());

// threads truly at once, by C alone (test/node/threads.provider.c says
// `#define HEFF_THREADS 1`: the runtime built for threads,
// docs/threads.md): the rest of the program run by each thread, each on
// its own copy, their answers gathered in order; a definition every
// thread asks for, computed once; tasks taken by threads as they end
// one; a static thunk forced by threads at once; functions whose C
// workers take their numbers unboxed (#39) called in each;
// lib/parallel.hf's `Par` each side a thread; and a thread abandoning,
// the program abandoning when it is joined.  HEFF_TSAN=1: built with
// ThreadSanitizer too, by the system's clang, and run again, what it says
// a failure.
const threadsWant = "said before the spread, once\nsquares: 0 1 4 9\nnested: 3 33\neach its own: 0 15 30\n"
  + "a definition, shared: 333833500 333833501 333833502 333833503\n"
  + "flat fields, shared and copied: 2528 2529 2530\nworkers: 82810 92940 102988\n"
  + "primes below 100000, by 4: 9592\n"
  + "forced at once: 6765 6765 6765 6765\nfib 20, each Par a thread: 6765\n";
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  for (const [name, args, want] of [
    ["main", [], { out: threadsWant, status: 0 }],
    ["failing", [], { out: "", err: "heff:abandon\n", status: 1 }],
    ["cpu", ["queens", "8", "4"], { out: "queens: answer 92\n", status: 0 }],
  ]) {
    const p = await programs.c("test/node/threads.hf", name);
    if (p.error !== undefined) { msgs.push(`FAIL threads ${name} (c): not compiled\n${p.error}`); continue; }
    const bins = [p.cmd];
    if (process.env.HEFF_TSAN) {
      const tsan = `${p.cmd}-tsan`;
      const b = await run("clang", ["-O1", "-g", "-fsanitize=thread", "-w", p.cmd.replace(/-c$/, ".c"), "-o", tsan, "-lm", "-lpthread"]);
      if (b.status !== 0) msgs.push(`FAIL threads ${name} (tsan): not built\n${b.stderr}${b.error ?? ""}`);
      else bins.push(tsan);
    }
    for (const bin of bins) {
      const r = await run(bin, args, { timeout: 60000, env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0" } });
      const got = { out: r.stdout, err: r.stderr, status: r.status };
      for (const k of Object.keys(want))
        if (got[k] !== want[k]) msgs.push(`FAIL threads ${name} (${basename(bin)}): ${k} ${JSON.stringify(got[k])}, wanted ${JSON.stringify(want[k])}`);
      if (!("err" in want) && r.stderr) msgs.push(`FAIL threads ${name} (${basename(bin)}): said ${r.stderr.split("\n").slice(0, 30).join("\n")}`);
    }
  }
  return msgs;
})());

// loops, compiled as loops: a million rounds, and the stack does not
// overflow -- in C, a stack of 8 MiB, which a million calls not made
// loops would (a call of itself put in a tag, src/heff/compile/trmc.hf;
// functions calling one another last, round a cycle, workers among them:
// src/heff/compile/worker.hf)
checks.push(ran("deep loops", "test/node/deep.hf", "deep", [], { out: "1000000\n1000000\n1000000\n500000\n1000000\n6,4,2\n1,2,3\n"
  + "4079499309867987239\n17319587194678911062\n", status: 0 },
  { cflags: ["-DHEFF_MAIN_STACK=8388608"] }));

// (a recursion deeper than the stack, in C: said, what was written before
// it written, not a signal's silent end -- not where a sanitizer handles
// faults itself)
if (!process.env.HEFF_SANITIZE) checks.push((async () => {
  const deeper = join(work, "deeper.hf");
  writeFileSync(deeper, [
    "import co", "import std.io", "",
    "sumTo : Int -> Int",
    "sumTo n = if n == 0 then 0 else n + sumTo (n - 1)", "",
    "main : Co stdio Unit",
    "main = do",
    "  print (ev := here stdio) \"before\"",
    "  print (ev := here stdio) (showInt (sumTo 100000000))", "",
  ].join("\n"));
  const p = await programs.c(deeper, "main", ["-DHEFF_MAIN_STACK=8388608"]);
  if (p.error !== undefined) return [`FAIL a stack overflow (c): not compiled\n${p.error}`];
  const r = await run(p.cmd, [], { timeout: 60000 });
  const want = { out: "before\n", err: "heff: the stack overflowed\n", status: 1 };
  const got = { out: r.stdout, err: r.stderr, status: r.status };
  return Object.keys(want).filter((k) => got[k] !== want[k])
    .map((k) => `FAIL a stack overflow (c): ${k} ${JSON.stringify(got[k])}, wanted ${JSON.stringify(want[k])}`);
})());

// (each operation goes on at once: the stack must not overflow)
const loop = join(work, "loop.hf");
writeFileSync(loop, [
  "import co", "import std.process", "import std.io", "",
  "eff : HFunctor",
  "eff = stdio <+> process", "",
  "loop : {{io : stdio <: eff}} -> {{sys : process <: eff}} -> Int -> Int -> Co eff Unit",
  "loop n acc = if n == 0 then print (showInt acc) else do",
  "  v <- getEnv \"X\"",
  "  loop (n - 1) (acc + 1)", "",
  "main : Co eff Unit",
  "main = loop (io := inl stdio process) (sys := inr stdio process) 100000 0", "",
].join("\n"));
checks.push(ran("a long loop", loop, "main", [], { out: "100000\n", status: 0 }));

// a loop of operations a handler of the language handles: each step's
// rest called last, a million times, in a small heap -- the stack let go
// of every so often (a bounce), not as deep as the loop is long
checks.push(ran("a long loop, handled in the language", "bench/state.hf", "main", [], { out: "1000000\n", status: 0 },
  { env: { ...process.env, HEFF_HEAP_MB: "256" } }));

for (const msgs of await values) said(msgs);
console.log(`backends: ${cases.length} programs, ${compared} values compared`);

// `f $ x` inlined, a call of `f`: the prelude's `$` is not in the code
// either backend made of test/programs/dollar.hf (`op_dollar` in
// JavaScript, `prelude_2e_24` in C)
{
  const code = (f) => { try { return readFileSync(join(work, f), "utf8"); } catch { return null; } };
  const js = code("dollar-js.mjs"), c = code("dollar-c.c");
  if (js !== null && js.includes("op_dollar")) said(["FAIL dollar (js): `$` is not inlined"]);
  if (c !== null && /prelude_2e_24\b/.test(c)) said(["FAIL dollar (c): `$` is not inlined"]);
}
for (const msgs of await Promise.all(checks)) said(msgs);
console.log(`backends: ${failed ? `${failed} FAILED` : "all agree"}`);
process.exit(failed ? 1 : 0);
