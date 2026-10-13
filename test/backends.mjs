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
// Programs: those of test/node, compiled to C (`--backend=c --name`),
// built and run, held to what they are to do (the interpreter runs no
// provider): what they write, their exit status, loops a million rounds
// deep, a long loop of operations done at once, and time; an HTTP
// client, TCP sockets and an HTTP server over them, an API over HTTP
// (std.remote), green threads waiting on them apart, processes forked,
// and threads truly at once.
// (Each program apart from the others, as many at once as there are cores.)
import { spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import { Agent, createServer, request } from "node:http";
import { connect } from "node:net";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
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
    // (the browser's modules are JavaScript's: a C program refuses them)
    if (be === "c" && /^import browser\./m.test(c.src)) continue;
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

// (the program `name` of `file`, compiled to C and built: what runs it;
// its files named by its file and its name; `cflags`, the C compiler's
// more)
const program = async (file, name, cflags = [], tag = "") => {
  const c = join(work, `${basename(file, ".hf")}-${name}${tag}.c`), bin = join(work, `${basename(file, ".hf")}-${name}${tag}-c`);
  const r = await heff(["compile", file, c, "--name", name, "--backend=c"], { cwd: root });
  if (r.status !== 0) return { error: r.stderr };
  const b = await buildC(c, bin, cflags);
  return b.status !== 0 ? { error: b.stderr } : { cmd: bin };
};

// (the program `name` of `file` run with `args`, what it wrote and its
// status held to `want`; what fails, said)
const ran = async (what, file, name, args, want, { cflags, tag, ...opts } = {}) => {
  const p = await program(file, name, cflags, tag);
  if (p.error !== undefined) return [`FAIL ${what}: not compiled\n${p.error}`];
  const r = await run(p.cmd, args, { timeout: 60000, ...opts });
  const got = { out: r.stdout, err: r.stderr, status: r.status };
  return Object.keys(want).filter((k) => got[k] !== want[k])
    .map((k) => `FAIL ${what}: ${k} ${JSON.stringify(got[k])}, wanted ${JSON.stringify(want[k])}`);
};
const checks = [];

// (the world: stdin, the environment, arguments, a file)
checks.push((async () => {
  const copied = join(work, "copied.txt");
  const msgs = await ran("echo", "test/node/echo.hf", "main", [copied, "two words"],
    { out: `${copied}\ntwo words\nhi\ncopied: from stdin\nmissing: not read\n`, err: "to stderr\n", status: 3 },
    { cwd: root, input: "from stdin", env: { ...process.env, GREETING: "hi" } });
  if (readFileSync(copied, "utf8") !== "from stdin") msgs.push("FAIL echo: the file");
  return msgs;
})());

// (a string is bytes, to the system and back: what is not UTF-8 -- an
// overlong, a surrogate, beyond U+10FFFF, cut short, a byte of no rune
// -- read and written as it is, by stdin, stdout and a file named not in
// ASCII; made text, as TextDecoder makes it; the arguments and the
// environment text, given by a shell as bytes)
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
  const p = await program("test/node/bytes.hf", "main");
  if (p.error !== undefined) return [`FAIL bytes: not compiled\n${p.error}`];
  const dir = join(work, "bytes-c");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir);
  const r = await run("sh", ["-c", `HEFF_BYTES="$(printf '${octal(env)}')" exec "$@" "$(printf '${octal(arg)}')"`, "sh", p.cmd, dir],
    { timeout: 60000, input, encoding: "latin1" });
  if (r.stdout !== want || r.status !== 0)
    msgs.push(`FAIL bytes: status ${r.status}, out ${JSON.stringify(r.stdout)}, wanted ${JSON.stringify(want)}\n${r.stderr}`);
  else if (!readFileSync(join(dir, "bytes-é😀.bin")).equals(input)) msgs.push("FAIL bytes: the file");
  return msgs;
})());

// (`heff run`: compiled to C and built in .heff/run/, then run in heff's
// place, on its streams)
checks.push((async () => {
  const r = await heff(["run", "test/node/echo.hf", join(work, "run.txt"), "x"], { cwd: root, input: "in", env: { ...process.env, GREETING: "hi" } });
  return r.status !== 3 || r.stderr !== "to stderr\n" ? [`FAIL heff run: status ${r.status}, stderr ${JSON.stringify(r.stderr)}`] : [];
})());

// (commands run to their end, std.process's `run`: each option of an
// `Invocation` and each way a command ends -- the commands sh's, so not
// on Windows; given an input of its own, which the commands run with
// none must not see)
if (process.platform !== "win32") checks.push(ran("commands", "test/node/commands.hf", "main", [], {
  out: [
    'plain: exited 0, out "out\\n", err "err\\n"',
    'status: exited 3, out "", err ""',
    'directory: exited 0, out "/\\n", err ""',
    'directory not there: not run, out "", err ""',
    'environment: exited 0, out "new path\\n", err ""',
    'environment as it is: exited 0, out "old\\n", err ""',
    'input: exited 0, out "fed\\n", err ""',
    'no input: exited 0, out "", err ""',
    'input, much: exited 0, out "200000\\n", err ""',
    'input, not read: exited 0, out "", err ""',
    'a signal: signal 9, out "", err ""',
    'a signal, another: signal 15, out "", err ""',
    'a loop: timed out, out "", err ""',
    'a sleep: timed out, out "before\\n", err ""',
    'within its time: exited 0, out "quick\\n", err ""',
    'kept: exited 0, out "0123", err "abcd"',
    'written forever: timed out, out "y\\ny\\ny\\n", err ""',
    'not there: not run, out "", err ""',
    "",
  ].join("\n"),
  err: "", status: 0,
}, { input: "the test's own\n", env: { ...process.env, GREETING: "old" } }));

// time: done later, the rest going on then
checks.push(ran("sleep", "test/node/timed.hf", "timed", [], { out: "slept\n", status: 0 }));

// the HTTP client, by C alone (std.http): a server of the test's own,
// sent requests by libcurl in the process, by the curl spawned
// (`-DHEFF_HTTP_SPAWN`) and by libcurl's multi interface (a green
// thread of std.reactor's), the same from each -- a response, whatever its
// status; why none came, curl's code and what it says -- and each as
// wanted (where it is curl's version's to say, the start of it).  Run
// again with no curl on the PATH, libcurl's the same still: it is in
// the process, not spawned.
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  const big = Buffer.from(Array.from({ length: 1 << 18 }, (_, i) => (i * 7) % 251));
  const sum = (b) => b.reduce((s, c) => s + c, 0);
  const every = Buffer.from(Array.from({ length: 512 }, (_, i) => i % 256));
  const server = createServer((q, r) => {
    const say = (status, body, headers = {}) => { r.writeHead(status, headers); r.end(body); };
    switch (q.url) {
      case "/item": return say(200, "item 3\n".repeat(4));
      // (what it was sent, said back: its method, a header of the
      // test's, its type, how long its body, whether it waits to go on;
      // and the body)
      case "/echo": {
        const parts = [];
        q.on("data", (c) => parts.push(c));
        return q.on("end", () => {
          const body = Buffer.concat(parts);
          say(200, body, {
            "X-Method": q.method, "X-Heff": q.headers["x-heff"] ?? "none", "X-Type": q.headers["content-type"] ?? "none",
            "X-Length": String(body.length), "X-Expect": q.headers.expect ?? "none",
          });
        });
      }
      case "/nope": return say(404, "not here");
      case "/fails": return say(500, "broken");
      case "/moved": return say(302, "moving", { Location: "/item" });
      case "/loop": return say(302, "", { Location: "/loop" });
      case "/slow": return setTimeout(() => say(200, "late"), 500);
      case "/big": return say(200, big, { "Content-Length": big.length });
      case "/chunked": r.write("one,"); r.write("two,"); return r.end("three");
      case "/short": r.writeHead(200, { "Content-Length": 100 }); r.write("cut"); return setTimeout(() => r.socket.destroy(), 50);
      default: return say(500);
    }
  });
  const listening = (s) => new Promise((ok) => s.listen(0, "127.0.0.1", () => ok(s.address().port)));
  const port = await listening(server);
  const shut = createServer(), refused = await listening(shut);
  shut.close();
  const at = `http://127.0.0.1:${port}`;
  const item = 'b["item 3", 10, "item 3", 10, "item 3", 10, "item 3", 10]';
  const echoed = (method, heff, type, length, body) =>
    `200 [X-Method: ${method}, X-Heff: ${heff}, X-Type: ${type}, X-Length: ${length}, X-Expect: none] ${body}`;
  const want = [
    `get: 200 [] ${item}`,
    `post: ${echoed("POST", "one two", "application/octet-stream", 512, `512 bytes, sum ${sum(every)}`)}`,
    `post, big: ${echoed("POST", "none", "none", 512 << 12, `${512 << 12} bytes, sum ${sum(every) << 12}`)}`,
    `post, nothing: ${echoed("POST", "none", "none", 0, 'b[]')}`,
    `put: ${echoed("PUT", "none", "none", 3, 'b["put"]')}`,
    `delete: ${echoed("DELETE", "", "none", 0, 'b[]')}`,
    `head: ${echoed("HEAD", "none", "none", 0, 'b[]')}`,
    '404: 404 [] b["not here"]',
    '500: 500 [] b["broken"]',
    new RegExp(`^refused: curl: \\(7\\) Failed to connect to 127\\.0\\.0\\.1 port ${refused}`),
    `followed: 200 [] ${item}`,
    'not followed: 302 [Location: /item] b["moving"]',
    /^redirects for ever: curl: \(47\) Maximum \(50\) redirects followed$/,
    'in time: 200 [] b["late"]',
    /^timed out: curl: \(28\) /,
    `big: 200 [] ${big.length} bytes, sum ${sum(big)}`,
    'chunked: 200 [] b["one,two,three"]',
    /^cut short: curl: \(18\) /,
    /^no scheme: curl: \(1\) /,
    /^no host: curl: \(3\) /,
    `getText: ${"item 3\n".repeat(4)}`,
    "getText, refused: code 7",
    "getText, 404: curl: (22) The requested URL returned error: 404",
    "get: GET",
  ];
  // (the lines, by `want`: a text may have lines of its own)
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
  const curt = (s) => JSON.stringify(s.length > 160 ? `${s.slice(0, 160)}...` : s);
  const outs = {};
  const noCurl = { ...process.env, PATH: join(work, "no-such-dir") };
  for (const [built, cflags, runs] of [
    ["libcurl", [], [["libcurl", process.env], ["libcurl, no curl on the PATH", noCurl]]],
    ["spawned", ["-DHEFF_HTTP_SPAWN"], [["spawned", process.env]]],
  ]) {
    const p = await program("test/node/fetched.hf", "main", cflags);
    if (p.error !== undefined) { msgs.push(`FAIL http, ${built} (c): not compiled\n${p.error}`); continue; }
    for (const [how, env] of runs) {
      const r = await run(p.cmd, [at, `http://127.0.0.1:${refused}/`], { timeout: 60000, env });
      if (r.status !== 0 || r.stderr) { msgs.push(`FAIL http, ${how} (c): status ${r.status}\n${r.stderr}`); continue; }
      const got = split(r.stdout);
      want.forEach((w, i) => {
        if (typeof w === "string" ? got[i] !== w : !w.test(got[i]))
          msgs.push(`FAIL http, ${how} (c): ${curt(got[i])}, wanted ${curt(String(w))}`);
      });
      outs[how] = r.stdout.replace(/ after \d+ (ms|milliseconds)/g, " after N $1");
    }
  }
  // (the same, from a green thread scheduled by std.reactor: libcurl's
  // multi interface)
  {
    const p = await program("test/node/fetched.hf", "multi");
    if (p.error !== undefined) msgs.push(`FAIL http, multi (c): not compiled\n${p.error}`);
    else {
      const r = await run(p.cmd, [at, `http://127.0.0.1:${refused}/`], { timeout: 60000 });
      if (r.status !== 0 || r.stderr) msgs.push(`FAIL http, multi (c): status ${r.status}\n${r.stderr}`);
      else outs["libcurl's multi, a green thread"] = r.stdout.replace(/ after \d+ (ms|milliseconds)/g, " after N $1");
    }
  }
  for (const how of Object.keys(outs).slice(1))
    if (outs[how] !== outs.libcurl) msgs.push(`FAIL http, ${how} (c): not as libcurl's\n${curt(outs[how])}\n${curt(outs.libcurl)}`);
  server.close();
  return msgs;
})());

// (`n` ports nothing listens on, as the system picked them: each held
// until all are picked, so that they are not the same)
const freePorts = async (n) => {
  const held = [];
  for (let i = 0; i < n; i++) {
    const s = createServer();
    await new Promise((ok) => s.listen(0, "127.0.0.1", ok));
    held.push(s);
  }
  const ports = held.map((s) => s.address().port);
  await Promise.all(held.map((s) => new Promise((ok) => s.close(ok))));
  return ports;
};

// files open, read and written a piece at a time (std.filesystem), by C
// alone: round trips, a large file in chunks and copied, lines (`\r\n`, none at the
// end, one longer than a buffer), seek and tell, what the system says of
// a file, a directory and a link, modes, temporary files, the standard
// output as a file between what std.io writes, and what fails, as values
// (ENOENT, EISDIR, EACCES -- not for root, whom no mode stops -- EEXIST,
// EBADF); and a file used after it is closed, failing the program
const filesWant = (denied) => [
  "written: ok ok ok",
  'read back: Just ("hello, world\\n")',
  'appended: Just ("hello, world\\nagain\\n")',
  "in chunks of 4096: 3000004 bytes, sum 157500426, 733 chunks",
  "in chunks of 1000000: 3000004 bytes, sum 157500426, 4 chunks",
  "copied: 3000004 bytes, the same True",
  'lines: ["one", "two", "", "three", "four"]',
  "long lines: [200000, 3]",
  "no lines: []",
  "sought to 7, read world, now at 12",
  'from the end 20: Just ("line"); back 12: Just ("second line")',
  'written after a read: Just ("heXYo, world\\nsecond line\\n")',
  "stat: regular of 25 bytes; directory",
  "a link: link, to a regular, resolved: target.txt",
  "an open file's: regular of 25 bytes",
  "modified: untouched the same True, written not earlier True, since the epoch True",
  "what a path is: True True False False nothing",
  "modes: 644, executable 755",
  "read-only 555, writable again 700, written in True",
  "temporary directories: True, empty Just ([]), each new True, named True",
  "a temporary file: for a while, there True",
  "removed: True",
  `failed: ENOENT EISDIR EISDIR ${denied} EEXIST`,
  "failed too: EBADF ENOENT ENOENT ENOENT, a directory made where a file is False",
  "said so: ENOENT: No such file or directory",
  "std.io before",
  "the standard output, a file",
  "std.io after, the same handle True",
].join("\n") + "\n";
if (process.platform !== "win32") checks.push((async () => {
  const dir = join(work, "files");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir);
  writeFileSync(join(dir, "target.txt"), "target\n");
  symlinkSync("target.txt", join(dir, "link"));
  const denied = process.getuid?.() === 0 ? "ok" : "EACCES";
  const msgs = await ran("files", "test/node/files.hf", "main", [dir], { out: filesWant(denied), err: "", status: 0 });
  msgs.push(...await ran("files, one used after it is closed", "test/node/files.hf", "closed", [dir],
    { out: "closed\n", err: "heff: the world holds nothing numbered 0\n", status: 1 },
    { env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0" } }));
  return msgs;
})());

// (`cmd` run, `fed(child, out)` told of what it has written so far, each
// time it writes: `{ status, out, err }` once it ends)
const streamed = (cmd, args, fed) => new Promise((resolve) => {
  const child = spawn(cmd, args, { stdio: ["pipe", "pipe", "pipe"] });
  let out = "", err = "";
  const timer = setTimeout(() => child.kill("SIGKILL"), 120000);
  child.stdin.on("error", () => {});
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (d) => { out += d; fed(child, out); });
  child.stderr.on("data", (d) => { err += d; });
  child.on("close", (status) => { clearTimeout(timer); resolve({ status, out, err }); });
  fed(child, out);
});

// (how much of the memory a process has is in RAM, in KiB: by /proc where
// there is one, else ps; 0 where neither says)
const residentKiB = (pid) => {
  try { return Number(/VmRSS:\s*(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "utf8"))[1]); } catch {}
  const r = spawnSync("ps", ["-o", "rss=", "-p", String(pid)]);
  return r.status === 0 ? Number(String(r.stdout).trim()) || 0 : 0;
};

// the standard input a line at a time (std.filesystem's `stdin`), by C
// alone: the answer to its first line written before the rest of it is
// (the test writes none until it is), then 64 MiB of lines, through a
// pipe, the program holding a few MiB at most meanwhile (not under a
// sanitizer, which keeps what is freed a while)
if (process.platform !== "win32") checks.push((async () => {
  const p = await program("test/node/files.hf", "linesMain");
  if (p.error !== undefined) return [`FAIL stdin a line at a time (c): not compiled\n${p.error}`];
  const line = "x".repeat(63) + "\n", block = line.repeat(16384), blocks = 64;
  let pinged = false, fed = false, kib = 0;
  const feed = async (child) => {
    for (let i = 0; i < blocks; i++) if (!child.stdin.write(block)) await once(child.stdin, "drain");
    kib = residentKiB(child.pid);
    child.stdin.end();
  };
  const r = await streamed(p.cmd, [], (child, out) => {
    if (!pinged) { pinged = true; child.stdin.write("ping\n"); }
    else if (!fed && out === "pong\n") { fed = true; feed(child); }
  });
  const want = `pong\nlines ${1 + blocks * 16384} bytes ${4 + blocks * 16384 * 63}\n`;
  const msgs = [];
  if (r.status !== 0 || r.err || r.out !== want) msgs.push(`FAIL stdin a line at a time (c): status ${r.status}\n${r.out}${r.err}`);
  if (!process.env.HEFF_SANITIZE && kib > 16384) msgs.push(`FAIL stdin a line at a time (c): ${kib} KiB held, ${blocks} MiB in`);
  return msgs;
})());

// green threads (lib/green.hf), scheduled by std.reactor, by C alone: one
// reads the standard input, a pipe, a line at a time, while another
// counts -- the count written before any input comes, as the test writes
// none until it is (a read that held up the whole program would never
// let it be)
if (process.platform !== "win32") checks.push((async () => {
  const p = await program("test/node/files.hf", "greenMain");
  if (p.error !== undefined) return [`FAIL green stdin (c): not compiled\n${p.error}`];
  let written = false;
  const r = await streamed(p.cmd, [], (child, out) => {
    if (!written && out.includes("counted while the reader waits: 1000\n")) {
      written = true;
      child.stdin.end("one\ntwo\nthree\n");
    }
  });
  const want = "counted while the reader waits: 1000\nread: 3 lines\nmain done\n";
  return r.status === 0 && !r.err && r.out === want ? [] : [`FAIL green stdin (c): status ${r.status}\n${r.out}${r.err}`];
})());

// TCP sockets, by C alone (std.net): a connection to a listener of the
// program's own at the port the test picked, each side echoing the
// other, one side shut down, then closed; a port the system picks, on
// IPv4, on IPv6 where the host has it, by a name; and what fails, as
// values -- a port already in use, a connection refused, a port out of
// range, a name not resolved
if (process.platform !== "win32") checks.push((async () => {
  const p = await program("test/node/sockets.hf", "main");
  if (p.error !== undefined) return [`FAIL sockets (c): not compiled\n${p.error}`];
  const [port, refused] = await freePorts(2);
  const r = await run(p.cmd, [String(port), String(refused)], { timeout: 60000 });
  const quoted = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const want = new RegExp("^" + [
    "listening: the port given",
    "peer: 127.0.0.1, the client's port: yes",
    "written: ok",
    "server read: 272 bytes, those sent: yes",
    "client read: 272 bytes, those sent: yes",
    "shut down: ok",
    'server read at the end: b[]',
    'client read after its shutdown: b["still here"]',
    `client read, the server's side closed: b[]`,
    "listening again on the port: EADDRINUSE",
    "refused: ECONNREFUSED",
    "a port out of range: EINVAL",
    "a name not resolved: EAI_...",
    "ipv4, a port picked: yes",
    "ipv4: 127.0.0.1",
  ].map(quoted).join("\n") + "\n(ipv6, a port picked: yes\nipv6: ::1|ipv6: none here)\n"
    + "a name, localhost, a port picked: yes\na name, localhost: (::1|127\\.0\\.0\\.1)\n$");
  return r.status === 0 && !r.stderr && want.test(r.stdout) ? []
    : [`FAIL sockets (c): status ${r.status}\n${r.stdout}${r.stderr}`];
})());

// green threads (lib/green.hf) doing std.net's operations, scheduled by
// std.reactor, by C alone: a thread blocked on a read lets another run,
// whose write it then reads; fifty clients echoed at once, all holding
// their connections, by a server of a thread a connection; a connection
// refused, a name resolved by a thread of its own and one not, while
// another thread counts; a read ended by the other side's close, one by
// its own socket closed; and main done while a thread waits to accept.
// HEFF_TSAN=1: built with ThreadSanitizer too (a name is resolved by a
// thread of the system), and run again.
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  const p = await program("test/node/green-sockets.hf", "main");
  if (p.error !== undefined) return [`FAIL green sockets (c): not compiled\n${p.error}`];
  const bins = [p.cmd];
  if (process.env.HEFF_TSAN) {
    const tsan = `${p.cmd}-tsan`;
    const b = await run("clang", ["-O1", "-g", "-fsanitize=thread", "-w", p.cmd.replace(/-c$/, ".c"), "-o", tsan, "-lm", "-lpthread"]);
    if (b.status !== 0) msgs.push(`FAIL green sockets (tsan): not built\n${b.stderr}${b.error ?? ""}`);
    else bins.push(tsan);
  }
  const want = [
    "reader: reads",
    "writer: runs while the reader waits",
    "writer: wrote, ok",
    'reader: read b["hello"]',
    "both done: 2",
    "refused: ECONNREFUSED",
    "a name, resolved: ok",
    "a name not resolved: EAI_...",
    "counted meanwhile: 1000",
    'read, the other side closed: b[]',
    "read, its socket closed: EBADF",
    "echoed, all held open at once: 50",
    "closed: 50",
    "main done",
    "abandoned, main's answer: 7",
  ].join("\n") + "\n";
  const [refused] = await freePorts(1);
  for (const bin of bins) {
    const r = await run(bin, [String(refused)], { timeout: 60000 });
    if (r.status !== 0 || r.stderr || r.stdout !== want)
      msgs.push(`FAIL green sockets (${basename(bin)}): status ${r.status}\n${r.stdout}${r.stderr}`);
  }
  return msgs;
})());

// std.http's requests from green threads, by C alone: ten at once, to a
// server that takes half a second over each -- at once as the server
// sees it, more than one of them in hand together (one after another,
// never more than one), which no load on the machine makes untrue, as a
// time taken would -- and one refused while they wait; and ten asked of
// the program's own server, std.net's sockets and libcurl's waited on
// together
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  const delay = 500, n = 10;
  let inHand = 0, most = 0;
  const server = createServer((q, r) => {
    const i = q.url.replace(/^\/item\//, "");
    most = Math.max(most, ++inHand);
    setTimeout(() => { inHand--; r.writeHead(200, { "Content-Type": "text/plain" }); r.end(`item ${i}\n`); }, delay);
  });
  const listening = (s) => new Promise((ok) => s.listen(0, "127.0.0.1", () => ok(s.address().port)));
  const port = await listening(server);
  const [refused] = await freePorts(1);
  const p = await program("test/node/green-http.hf", "main");
  if (p.error !== undefined) msgs.push(`FAIL green http (c): not compiled\n${p.error}`);
  else {
    const r = await run(p.cmd, [`http://127.0.0.1:${port}`, String(n), String(refused)], { timeout: 60000 });
    const want = `refused, while the others wait: code 7\nitems fetched at once: ${n}\nmain done\n`;
    if (r.status !== 0 || r.stderr || r.stdout !== want)
      msgs.push(`FAIL green http (c): status ${r.status}\n${r.stdout}${r.stderr}`);
    else if (most < 2)
      msgs.push(`FAIL green http (c): ${n} requests, never more than ${most} in the server's hands at once`);
  }
  server.close();
  const self = await program("test/node/green-http.hf", "self");
  if (self.error !== undefined) msgs.push(`FAIL green http, self (c): not compiled\n${self.error}`);
  else {
    const r = await run(self.cmd, [], { timeout: 60000 });
    if (r.status !== 0 || r.stderr || r.stdout !== "asked of its own server, at once: 10\nmain done\n")
      msgs.push(`FAIL green http, self (c): status ${r.status}\n${r.stdout}${r.stderr}`);
  }
  return msgs;
})());

// std.http.server's `serveConcurrently`, by C alone: a green thread a
// connection, forty clients holding theirs open at once, each answered
// (a server of one connection at a time never answers the second), then
// asked again on the same connection
if (process.platform !== "win32") checks.push(ran("green server", "test/node/green-served.hf", "main", [],
  { out: "first requests answered, every connection held open: 40\n"
    + "second requests answered, on the same connections: 40\nmain done\n", status: 0 }));

// an API over HTTP, both sides of std.remote, by C alone: its server
// over std.http.server, its client by std.http, green threads of one
// program -- calls answered, the server's count kept between them; a
// client of another build refused; no server there; a GET, no call
if (process.platform !== "win32") checks.push((async () => {
  const [refused] = await freePorts(1);
  return ran("remote", "test/node/remote.hf", "main", [String(refused)],
    { out: "answered: 2 42, shouted, its bytes as they were: True\n"
      + "again, the count kept: 44 84, shouted, its bytes as they were: True\n"
      + "another build: mismatched: the server's API is tally #d783e7b7, the call's #0f45894d\n"
      + "no server: unreachable: curl: (7)\n"
      + "a GET: 405 a call of the API is a POST\n\n"
      + "main done\n", status: 0 });
})());

// an HTTP server over them, by C alone (std.http.server): asked by
// std.http's client in the same program, a thread of its own; and by
// the test's own -- node:http, its connection kept for each request,
// bytes written to a socket (requests one after another in one write, a
// head in parts, HTTP/1.0, `Expect: 100-continue`, a chunked body, a
// body refused that the client sends on after the response, no request
// at all), and curl, two requests on one connection -- each
// request said by the server as it answers it, until it is asked for
// `/stop`; and stopped by a connection it cannot accept, with no
// descriptor left
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  const bytesOf = (n) => Buffer.from(Array.from({ length: 256 * n }, (_, i) => i % 256));
  const sum = (b) => b.reduce((s, c) => s + c, 0);
  const big = Buffer.concat(Array.from({ length: 4096 }, () => bytesOf(4)));
  const self = await program("test/node/served.hf", "self");
  if (self.error !== undefined) msgs.push(`FAIL http server, self (c): not compiled\n${self.error}`);
  else {
    const r = await run(self.cmd, [], { timeout: 60000 });
    const want = [
      'get: 200 [- - 11] b["hello, GET", 10]',
      `post: 200 [POST 127.0.0.1 512] 512 bytes, sum ${sum(bytesOf(2))}`,
      `post, big: 200 [POST 127.0.0.1 ${512 << 12}] ${512 << 12} bytes, sum ${sum(bytesOf(2)) << 12}`,
      'put, nothing: 200 [PUT 127.0.0.1 0] b[]',
      'head: 200 [- - 12] b[]',
      '404: 404 [- - 14] b["no such thing", 10]',
      `big: 200 [- - ${big.length}] ${big.length} bytes, sum ${sum(big)}`,
      'closed after: 200 [- - 3] b["bye"]',
      'again: 200 [- - 11] b["hello, GET", 10]',
      'stop: 200 [- - 9] b["stopping", 10]',
    ].join("\n") + "\n";
    if (r.status !== 0 || r.stderr || r.stdout !== want)
      msgs.push(`FAIL http server, self (c): status ${r.status}\n${r.stdout}${r.stderr}`);
  }
  // (no descriptor left for a connection accepted: the server stops, and
  // says why, not trying again for ever)
  const crowded = await program("test/node/served.hf", "crowded");
  if (crowded.error !== undefined) msgs.push(`FAIL http server, crowded (c): not compiled\n${crowded.error}`);
  else {
    const r = await run("sh", ["-c", 'ulimit -n 32 && exec "$0"', crowded.cmd], { timeout: 30000 });
    const want = "connect: EMFILE\nserveUntil: EMFILE\nserve: EMFILE\n";
    if (r.status !== 0 || r.stderr || r.stdout !== want)
      msgs.push(`FAIL http server, crowded (c): status ${r.status}\n${r.stdout}${r.stderr}`);
  }
  const p = await program("test/node/served.hf", "main");
  if (p.error !== undefined) return [...msgs, `FAIL http server (c): not compiled\n${p.error}`];
  const [port] = await freePorts(1);
  const served = run(p.cmd, [String(port)], { timeout: 60000 });
  // (bytes written to a connection, in parts -- each after a pause, or
  // after what it waits for has come -- and all that came back, until the
  // server closed it)
  const raw = (parts) => new Promise((ok) => {
    const s = connect({ host: "127.0.0.1", port });
    let got = "", next = 0;
    const timer = setTimeout(() => { s.destroy(); ok(`${got}(no end)`); }, 10000);
    const send = () => {
      if (next >= parts.length) return;
      const [after, text] = parts[next];
      if (typeof after === "number") { next++; setTimeout(() => { s.write(text); send(); }, after); }
      else if (got.includes(after)) { next++; s.write(text); send(); }
    };
    s.setEncoding("latin1");
    s.on("connect", send);
    s.on("data", (d) => { got += d; send(); });
    s.on("error", (e) => { got += `(${e.code})`; });
    s.on("close", () => { clearTimeout(timer); ok(got); });
  });
  // (the server up: connected to, the connection closed at once)
  const up = async () => {
    for (let i = 0; i < 600; i++) {
      if (await new Promise((ok) => {
        const s = connect({ host: "127.0.0.1", port });
        s.on("connect", () => { s.destroy(); ok(true); });
        s.on("error", () => ok(false));
      })) return true;
      await new Promise((ok) => setTimeout(ok, 50));
    }
    return false;
  };
  if (!(await up())) return [...msgs, `FAIL http server (c): not listening\n${(await served).stderr}`];
  const agent = new Agent({ keepAlive: true, maxSockets: 1 });
  // (a request by node:http, on the connection kept, or `via` another)
  const ask = (method, path, body, headers = {}, via = agent) => new Promise((ok) => {
    const q = request({ host: "127.0.0.1", port, method, path, agent: via, headers }, (r) => {
      const parts = [];
      r.on("data", (c) => parts.push(c));
      r.on("end", () => ok({ status: r.statusCode, headers: r.headers, body: Buffer.concat(parts), reused: q.reusedSocket }));
    });
    q.on("error", (e) => ok({ status: e.code, headers: {}, body: Buffer.alloc(0), reused: false }));
    q.end(body);
  });
  const log = [];
  const check = (what, got, want) => {
    if (got !== want) msgs.push(`FAIL http server, ${what} (c): ${JSON.stringify(got)}, wanted ${JSON.stringify(want)}`);
  };
  const posted = Buffer.from(Array.from({ length: 300000 }, (_, i) => (i * 13) % 256));
  for (const [what, method, path, body, headers, shown, want, said] of [
    ["get", "GET", "/hello", undefined, {}, (r) => `${r.status} ${r.headers["content-length"]} ${r.body} ${r.reused}`,
      "200 11 hello, GET\n false", 'GET /hello HTTP/1.1, b[]'],
    ["post", "POST", "/echo", posted, { "X-Heff": "node" },
      (r) => `${r.status} ${r.headers["x-method"]} ${r.headers["x-version"]} ${r.headers["x-peer"]} ${r.headers["x-heff"]} ${r.body.equals(posted)} ${r.reused}`,
      "200 POST HTTP/1.1 127.0.0.1 node true true", `POST /echo HTTP/1.1, ${posted.length} bytes, sum ${sum(posted)}`],
    ["head", "HEAD", "/hello", undefined, {}, (r) => `${r.status} ${r.headers["content-length"]} ${r.body.length} ${r.reused}`,
      "200 12 0 true", 'HEAD /hello HTTP/1.1, b[]'],
    ["404", "GET", "/nope", undefined, {}, (r) => `${r.status} ${r.body} ${r.reused}`,
      "404 no such thing\n true", 'GET /nope HTTP/1.1, b[]'],
    ["big", "GET", "/big", undefined, {}, (r) => `${r.status} ${r.body.equals(big)} ${r.reused}`,
      "200 true true", 'GET /big HTTP/1.1, b[]'],
  ]) {
    check(what, shown(await ask(method, path, body, headers)), want);
    log.push(said);
  }
  // (the connection kept, closed: the server, one connection at a time,
  // goes on to the next)
  agent.destroy();
  const hello = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 11\r\n";
  for (const [what, parts, want, said] of [
    ["one after another, in one write",
      [[0, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"]],
      `${hello}\r\nhello, GET\n${hello}Connection: close\r\n\r\nhello, GET\n`, ['GET /hello HTTP/1.1, b[]', 'GET /hello HTTP/1.1, b[]']],
    ["a head in parts",
      [[0, "GET /hel"], [50, "lo HTTP/1.1\r\nHost: x\r\nConnection: close\r"], [50, "\n\r\n"]],
      `${hello}Connection: close\r\n\r\nhello, GET\n`, ['GET /hello HTTP/1.1, b[]']],
    ["HTTP/1.0", [[0, "GET /hello HTTP/1.0\r\n\r\n"]],
      `${hello}Connection: close\r\n\r\nhello, GET\n`, ['GET /hello HTTP/1.0, b[]']],
    ["HTTP/1.0, kept alive", [[0, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /hello HTTP/1.0\r\n\r\n"]],
      `${hello}Connection: keep-alive\r\n\r\nhello, GET\n${hello}Connection: close\r\n\r\nhello, GET\n`,
      ['GET /hello HTTP/1.0, b[]', 'GET /hello HTTP/1.0, b[]']],
    ["Expect: 100-continue",
      [[0, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n"], ["100 Continue\r\n\r\n", "hello"]],
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nX-Method: POST\r\nX-Version: HTTP/1.1\r\nX-Peer: 127.0.0.1\r\nX-Heff: none\r\n"
        + "Content-Length: 5\r\nConnection: close\r\n\r\nhello",
      ['POST /echo HTTP/1.1, b["hello"]']],
    ["a chunked body", [[0, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"]],
      "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 16\r\nConnection: close\r\n\r\nNot Implemented\n", []],
    // (refused, the client sending on after the response: drained, not
    // reset, so that it reads the response)
    ["a body too long, sent on",
      [[0, "POST /echo HTTP/1.1\r\nContent-Length: 100000000\r\n\r\n" + "x".repeat(32768)], ["Content Too Large\n", "y".repeat(524288)]],
      "HTTP/1.1 413 Content Too Large\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 18\r\nConnection: close\r\n\r\nContent Too Large\n", []],
    ["a chunked body, sent on",
      [[0, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n8000\r\n" + "x".repeat(32768)], ["Not Implemented\n", "y".repeat(524288)]],
      "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 16\r\nConnection: close\r\n\r\nNot Implemented\n", []],
    ["no request", [[0, "NONSENSE\r\n\r\n"]],
      "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 12\r\nConnection: close\r\n\r\nBad Request\n", []],
  ]) {
    check(what, await raw(parts), want);
    log.push(...said);
  }
  if (spawnSync("curl", ["--version"]).status === 0) {
    const at = `http://127.0.0.1:${port}`;
    const r = await run("curl", ["-sS", "--http1.1", "-o", "/dev/null", "-o", "/dev/null", "-w", "%{http_code} %{num_connects} %{size_download}\n",
      `${at}/hello`, `${at}/big`], { timeout: 30000, slot: false });
    check("curl, two requests on one connection", `${r.stdout}${r.stderr}`, `200 1 11\n200 0 ${big.length}\n`);
    log.push('GET /hello HTTP/1.1, b[]', 'GET /big HTTP/1.1, b[]');
  }
  const stop = await ask("GET", "/stop", undefined, {}, false);
  check("stop", `${stop.status} ${stop.body}`, "200 stopping\n");
  log.push('GET /stop HTTP/1.1, b[]');
  const r = await served;
  if (r.status !== 0 || r.stderr) msgs.push(`FAIL http server (c): status ${r.status}\n${r.stderr}`);
  check("what the server said", r.stdout, log.map((l) => `${l}\n`).join(""));
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

// an app is not a C program: refused at the import of the browser's
// module, and why
checks.push((async () => {
  const why = "examples/counter/counter.hf:7:1\nerror[E0351]: `browser.dom` is the browser's (lib/browser/), for JavaScript: a C program (--backend=c) cannot import it";
  const r = await heff(["compile", "examples/counter/counter.hf", join(work, "counter.c"), "--name", "counter", "--backend=c"], { cwd: root });
  return r.status !== 0 && r.stderr.includes(why) ? [] : [`FAIL an app is not a program: ${r.status} ${r.stderr}`];
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

// processes (test/node/fork.provider.c forks): the rest of the program run
// by each of them, each on its own copy, their strings gathered in
// order, and lib/parallel.hf's `Parallel` each side a process -- what was
// written before a spread written once
if (process.platform !== "win32") checks.push((async () => {
  const p = await program("test/node/fork.hf", "main");
  if (p.error !== undefined) return [`FAIL processes (c): not compiled\n${p.error}`];
  const r = await run(p.cmd, [], { timeout: 60000 });
  const want = "said before the spread, once\nsquares: 0 1 4 9\nnested: 0.0,0.1,0.2 1.0,1.1,1.2\n"
    + "each its own: 0 15 30; the parent's: 15\nfib 15, each Parallel forked: 610\n";
  return r.status === 0 && r.stdout === want ? [] : [`FAIL processes (c): status ${r.status}\n${r.stdout}${r.stderr}`];
})());

// threads truly at once (std.thread, whose provider,
// lib/std/thread.provider.c, says `#define HEFF_THREADS 1`: the runtime
// built for threads, docs/threads.md): the rest of the program run by
// each thread, shared by them (#66), their answers gathered in order; a
// definition every thread asks for, computed once; tasks taken by
// threads as they end one; a static thunk, and a shared one, forced by
// threads at once;
// functions whose C workers take their numbers unboxed (#39) called in
// each; lib/parallel.hf's `Parallel` each side a thread; answers of any type
// (strings, lists, pairs) mapped, gathered and each side of a `Parallel`; and
// a thread abandoning, a spread's or a pool's task, the program
// abandoning when it is joined.  HEFF_TSAN=1: built with ThreadSanitizer
// too, by the system's clang, and run again, what it says a failure.
const threadsWant = "said before the spread, once\nsquares: 0 1 4 9\nnested: 3 33\neach its own: 0 15 30\n"
  + "a definition, shared: 333833500 333833501 333833502 333833503\n"
  + "flat fields, frozen and shared: 2528 2529 2530\nworkers: 82810 92940 102988\n"
  + "primes below 100000, by 4: 9592\n"
  + "forced at once: 6765 6765 6765 6765\nfib 20, each Parallel a thread: 6765\n";
const genericWant = "strings, by 3: a! bb! ccc! dddd! eeeee!\nlists, by 4: [] [1] [1,2] [1,2,3] [1,2,3,4]\n"
  + "a list of the caller's, in each: 0 3 165 15150 15150\n"
  + "a thunk of the caller's, forced at once: 15 15 15 15 15 15 15 15\npairs: one=55 two=89\nof none: []\n"
  + "gathered: ,ab,abab\na Parallel of two types: fib 15 is 610; [1,1,2,3,5,8,13,21]\ncores: at least one\n";
if (process.platform !== "win32") checks.push((async () => {
  const msgs = [];
  for (const [name, args, want] of [
    ["main", [], { out: threadsWant, status: 0 }],
    ["generic", [], { out: genericWant, status: 0 }],
    ["failing", [], { out: "", err: "heff:abandon\n", status: 1 }],
    ["failingTask", [], { out: "said before the pool\n", err: "heff:abandon\n", status: 1 }],
    ["cpu", ["queens", "8", "4"], { out: "queens: answer 92\n", status: 0 }],
  ]) {
    const p = await program("test/node/threads.hf", name);
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
// src/heff/compile/worker.hf, of other arities too: cycles.hf, #78).  No
// call made a jump by the C compiler but those it must make (`musttail`),
// and none of those either (`HEFF_NO_JUMPS`, as where there is no
// `musttail`): a loop all the same
const deepWant = "1000000\n1000000\n1000000\n500000\n1000000\n6,4,2\n1,2,3\n"
  + "4079499309867987239\n17319587194678911062\n0\n1\n";
for (const [what, tag, flags] of [["deep loops", "", []], ["deep loops, no jumps", "-nojumps", ["-DHEFF_NO_JUMPS"]]])
  checks.push(ran(what, "test/node/deep.hf", "deep", [], { out: deepWant, status: 0 },
    { cflags: ["-DHEFF_MAIN_STACK=8388608", "-fno-optimize-sibling-calls", ...flags], tag }));

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
    "  print (ev := here stdio) (show (sumTo 100000000))", "",
  ].join("\n"));
  const p = await program(deeper, "main", ["-DHEFF_MAIN_STACK=8388608"]);
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
  "loop n acc = if n == 0 then print (show acc) else do",
  "  v <- getEnv \"X\"",
  "  loop (n - 1) (acc + 1)", "",
  "main : Co eff Unit",
  "main = loop (io := inl stdio process) (sys := inr stdio process) 100000 0", "",
].join("\n"));
checks.push(ran("a long loop", loop, "main", [], { out: "100000\n", status: 0 }));

// a loop of operations a handler of the language handles: each step's
// rest called last, a million times, in a small heap -- the stack let go
// of every so often (a bounce), not as deep as the loop is long
checks.push(ran("a long loop, handled in the language", "bench/state.hf", "main", [], { out: "1000000\n", status: 0 }));

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
