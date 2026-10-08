#!/usr/bin/env node
// Progress (lib/std/progress.hf): on a terminal -- the standard error a
// terminal, `TERM` not `dumb` -- `heff build` draws what it is doing
// below what it writes, zig's own progress among it (`zig cc`, through
// `ZIG_PROGRESS`), and clears it: what is left on the screen is what it
// writes, as it writes it without one.  Elsewhere -- a pipe, a file,
// `TERM=dumb` -- it writes not a byte more.  A terminal is a
// pseudo-terminal `script` makes (BSD's, or util-linux's), what is drawn
// on it played back on a screen kept here.
//
//   node test/progress.mjs
import { spawnSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { heffBin } from "./heff.mjs";

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

if (process.platform === "win32" || spawnSync("which", ["script"]).status !== 0) {
  console.log("progress: no `script` here to make a terminal: passed over");
  process.exit(0);
}

const work = mkdtempSync(join(tmpdir(), "heff-progress-"));
const plain = (cwd, ...args) => spawnSync(heffBin, args, { cwd, encoding: "utf8", env: { ...process.env, TERM: "xterm-256color" } });

// (heff run on a terminal, `TERM` as given: all it put there, as bytes
// read back as UTF-8, and its status)
const onTerminal = (cwd, term, ...args) => {
  const env = { ...process.env, TERM: term };
  const q = (s) => `'${s.replace(/'/g, `'\\''`)}'`;
  const r = process.platform === "darwin"
    ? spawnSync("script", ["-q", "/dev/null", heffBin, ...args], { cwd, env, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] })
    : spawnSync("script", ["-q", "-e", "-c", [heffBin, ...args].map(q).join(" "), "/dev/null"], { cwd, env, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] });
  return { raw: r.stdout, status: r.status };
};

// (what a terminal shows once it has been given `raw`: its lines, as
// far as the cursor went -- a character a column; `\r`, `\n`, `\b`;
// `ESC M` up a line; `ESC [ J` cleared below the cursor, `ESC [ K` the
// rest of its line; other sequences (colours, `?2026`) shown as nothing)
const screen = (raw) => {
  const rows = [[]];
  let r = 0, c = 0;
  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i];
    if (ch === "\x1b") {
      const m = /^\[([?0-9;]*)([A-Za-z])/.exec(raw.slice(i + 1, i + 32));
      if (m) {
        i += m[0].length;
        if (m[2] === "J") { rows[r] = (rows[r] ?? []).slice(0, c); rows.length = r + 1; }
        else if (m[2] === "K") rows[r] = (rows[r] ?? []).slice(0, c);
      } else if (raw[i + 1] === "M") { i++; if (r > 0) r--; }
      else if (raw[i + 1] === "(") i += 2;
      else i++;
      continue;
    }
    if (ch === "\r") c = 0;
    else if (ch === "\n") { r++; rows[r] ??= []; }
    else if (ch === "\b") c = Math.max(0, c - 1);
    else if (ch >= " ") { (rows[r] ??= [])[c++] = ch; }
  }
  return rows.map((l) => Array.from(l, (x) => x ?? " ").join("").trimEnd()).filter((l) => l !== "" && l !== "^D");
};

// a project whose program is big enough to take its time: a module of
// a few hundred definitions, checked, compiled to C and built by zig --
// long enough to be drawn (after 200 ms), and zig's own tasks with it
spawnSync(heffBin, ["init", "hello"], { cwd: work });
const hello = join(work, "hello");
const n = 600;
const defs = Array.from({ length: n }, (_, i) =>
  `f${i} : Int -> Int\nf${i} x = if x > ${i} then x * ${i % 7 + 2} + ${i} else f${i} (x + 1)\n`);
writeFileSync(join(hello, "src", "hello", "big.hf"),
  `import prelude\n\n${defs.join("\n")}\ntotal : Int\ntotal = ${Array.from({ length: n }, (_, i) => `f${i} 1`).join(" + ")}\n`);
writeFileSync(join(hello, "app", "hello", "main.hf"), [
  "import co", "import hello.big", "import hello.greeting", "import prelude", "import std.io", "",
  "program : {{io : stdio <: stdio}} -> Co stdio Unit", "program = print (greeting (showInt total))", "",
  "main : Co stdio Unit", "main = program (io := here stdio)", "",
].join("\n"));

// built on a terminal: drawn as it goes -- the modules checked, the
// program compiled, zig's tasks under `zig cc` -- then cleared, the
// screen left with what heff wrote; the program built
const said = ["heff build: 2 modules checked", "wrote .heff/bin/hello"];
const drawn = onTerminal(hello, "xterm-256color", "build");
const shown = screen(drawn.raw);
expect("build on a terminal", drawn.status === 0 && shown.join("\n") === said.join("\n"), { shown, raw: drawn.raw.slice(-2000) });
expect("build on a terminal: its progress drawn", drawn.raw.includes("\x1b[J") &&
  /\[\d+\/2\] checking modules|building programs/.test(drawn.raw) && /compiling to C|zig cc/.test(drawn.raw), drawn.raw.slice(0, 2000));
expect("build on a terminal: zig's tasks under `zig cc`", /zig cc\r?\n {6}[├└]─ /.test(drawn.raw), drawn.raw.slice(-3000));
expect("build on a terminal: the program", spawnSync(join(hello, ".heff", "bin", "hello"), { encoding: "utf8" }).stdout.startsWith("Hello, "), null);

// not on a terminal: not a byte more than it says
const piped = plain(hello, "build");
expect("build into pipes", piped.status === 0 && piped.stdout === said.map((l) => l + "\n").join("") && piped.stderr === "", piped);

// TERM=dumb: a terminal, nothing drawn
const dumb = onTerminal(hello, "dumb", "build");
expect("build on a dumb terminal", dumb.status === 0 && !dumb.raw.includes("\x1b") && screen(dumb.raw).join("\n") === said.join("\n"), dumb.raw);

// installed, and tested, on a terminal: the screen left with what each
// says without one
const lines = (r) => (r.stdout + r.stderr).split("\n").map((l) => l.trimEnd()).filter((l) => l !== "");
process.env.HEFF_HOME = join(work, "home");
for (const args of [["install"], ["test"]]) {
  const t = onTerminal(hello, "xterm-256color", ...args), p = plain(hello, ...args);
  expect(`${args[0]} on a terminal`, t.status === 0 && p.status === 0 && screen(t.raw).join("\n") === lines(p).join("\n"), { shown: screen(t.raw), plain: lines(p) });
}
delete process.env.HEFF_HOME;

// what is written while it is drawn is not garbled: a module with an
// error, said as it is found, the progress cleared before it and after
// it (`exit 1` ends it), the screen what it says without a terminal
writeFileSync(join(hello, "src", "hello", "wrong.hf"), "import prelude\n\nwrong : Int\nwrong = \"no\"\n");
rmSync(join(hello, ".heff"), { recursive: true, force: true });
const failing = onTerminal(hello, "xterm-256color", "build");
const failingPlain = plain(hello, "build");
expect("an error, on a terminal", failing.status === 1 && failingPlain.status === 1 &&
  screen(failing.raw).join("\n") === lines(failingPlain).join("\n"),
  { shown: screen(failing.raw), plain: failingPlain.stdout + failingPlain.stderr });

rmSync(work, { recursive: true, force: true });
console.log(`progress: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
