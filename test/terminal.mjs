#!/usr/bin/env node
// heff with no words, on a terminal: its commands' app (lib/std/terminal.hf,
// lib/std/cli/interactive.hf), driven by keys typed at a pseudo-terminal
// of 100 columns by 30 rows (test/pty.c, built here by zig cc): the list
// drawn on the alternate screen, filtered, a command's form filled in
// and its command line run once the app has put the terminal back as it
// was; Esc and `q` leaving it, with nothing run; Ctrl-C ending heff with
// the terminal put back.  On a terminal that is `dumb`, or not on one,
// heff says its usage, as before.  (The app's states, key by key, are
// test/programs/terminal.hf's, held to every backend.)
//
//   node test/terminal.mjs
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { heffBin, root } from "./heff.mjs";
import { run, slot } from "./pool.mjs";

let checks = 0, failed = 0;
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: ${JSON.stringify(got)}`); } };

if (process.platform === "win32") {
  console.log("terminal: no pseudo-terminal on Windows: passed over");
  process.exit(0);
}

const work = mkdtempSync(join(tmpdir(), "heff-terminal-"));
const cc = spawnSync("zig", ["version"]).status === 0 ? ["zig", "cc"] : ["cc"];
const pty = join(work, "pty");
const built = await run(cc[0], [...cc.slice(1), "-O1", "-w", join(root, "test", "pty.c"), "-o", pty]);
if (built.status !== 0) {
  console.log(`FAIL terminal: test/pty.c not built: ${built.stderr}`);
  process.exit(1);
}
writeFileSync(join(work, "a.hf"), "x : Int\nx = 1\n");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (ok, ms) => { const t = Date.now(); while (!ok()) { if (Date.now() - t > ms) return false; await sleep(20); } return true; };
const frames = (raw) => raw.split("\x1b[?2026l").length - 1;

// (heff with `args` on the terminal, `TERM` as given, each of `keys`
// typed once what it drew before has been drawn: all it drew, as text,
// and its status)
const onTerminal = async (args, keys, term = "xterm-256color") => {
  const free = await slot();
  const p = spawn(pty, ["100", "30", heffBin, ...args], { cwd: work, env: { ...process.env, TERM: term }, stdio: ["pipe", "pipe", "pipe"] });
  let raw = "", status = null;
  p.stdout.setEncoding("utf8");
  p.stdout.on("data", (d) => { raw += d; });
  p.stdin.on("error", () => {});
  const closed = new Promise((r) => p.on("close", (s) => { status = s; r(); }));
  for (const k of keys) {
    const before = frames(raw);
    if (!(await until(() => status !== null || frames(raw) > 0, 20000))) break;
    p.stdin.write(k);
    await until(() => status !== null || frames(raw) > before + 1 || (before === 0 && frames(raw) > 1), 300);
  }
  if (!(await until(() => status !== null, 20000))) p.kill("SIGKILL");
  await closed;
  free();
  return { raw, status };
};

// (the screen the last frame drew: its rows, their escapes left out)
const lastFrame = (raw) => {
  const at = raw.lastIndexOf("\x1b[?2026h");
  const end = raw.indexOf("\x1b[?2026l", at);
  if (at < 0 || end < 0) return [];
  return raw.slice(at, end).replace(/\x1b\[[?0-9;]*[A-Za-z]/g, "").split("\n").map((l) => l.replace(/\r/g, "").trimEnd());
};
// (what was written after the app put the terminal back)
const after = (raw) => raw.slice(raw.lastIndexOf("\x1b[?1049l") + "\x1b[?1049l".length).replace(/\r/g, "");
const restored = (raw) => raw.startsWith("\x1b[?1049h") && raw.lastIndexOf("\x1b[?1049l") > raw.lastIndexOf("\x1b[?2026l");

// the list, then each step as it is drawn: kept for the frames between
const list = await onTerminal([], ["q"]);
const shownList = (() => { const at = list.raw.indexOf("\x1b[?2026l"); return lastFrame(list.raw.slice(0, at + 8)); })();
expect("the list of the commands", list.status === 0 && restored(list.raw) &&
  shownList[0] === "heff: its commands" && shownList.some((l) => /^> check +elaborate FILE/.test(l)) &&
  shownList.some((l) => /^  dev +the project built/.test(l)) && !shownList.some((l) => l.includes("dump-core")) &&
  shownList[29].startsWith("heff check FILE"), { shownList, status: list.status });
expect("q: nothing run", after(list.raw) === "", after(list.raw));

// filtered, opened, filled in -- a field typed in, a switch turned on --
// and run: the command line said, then run as if it had been given
const fmt = await onTerminal([], ["fm", "\r", "a.hf", "\t", " ", "\r"]);
const fmtShown = lastFrame(fmt.raw);
expect("a form filled in", fmtShown[0] === "heff fmt" &&
  fmtShown.some((l) => /^  FILE\.\.\. +a\.hf$/.test(l)) && fmtShown.some((l) => /^> -c, --check +off  on$/.test(l)) &&
  fmtShown.includes("$ heff fmt a.hf --check") && fmtShown[29].startsWith("Enter: run it"), fmtShown);
expect("its command line run", fmt.status === 0 && restored(fmt.raw) && after(fmt.raw) === "$ heff fmt a.hf --check\n", { after: after(fmt.raw), status: fmt.status });

// a form something is wrong with: said, and Enter runs nothing; Esc
// back to the list, the filter kept; Esc again clears it, and once more
// leaves
const wrong = await onTerminal([], ["comp", "\r", "\r", "\x1b", "\x1b", "\x1b"]);
const frameTexts = wrong.raw.split("\x1b[?2026l").slice(0, -1).map((f) => lastFrame(f + "\x1b[?2026l"));
expect("what is wrong yet, said", frameTexts.some((f) => f[0] === "heff compile" && f[29]?.startsWith("not yet: expected FILE")), frameTexts.map((f) => f[29]));
expect("Esc: back, then the filter cleared, then out", wrong.status === 0 && restored(wrong.raw) && after(wrong.raw) === "" &&
  frameTexts.at(-1)?.[1]?.startsWith("type to filter") && frameTexts.at(-2)?.[1] === "filter: comp", frameTexts.slice(-3).map((f) => f.slice(0, 4)));

// a command of no words: Enter twice runs it
const version = await onTerminal([], ["version", "\r", "\r"]);
expect("a command of no words, run", version.status === 0 && /^\$ heff version\nheff \d+\.\d+\.\d+\n$/.test(after(version.raw)), after(version.raw));

// Ctrl-C: SIGINT, the terminal put back, heff ended by it
const interrupted = await onTerminal([], ["j", "\x03"]);
expect("Ctrl-C", interrupted.status === 130 && restored(interrupted.raw), { status: interrupted.status, end: interrupted.raw.slice(-60) });

// TERM=dumb: no app, the usage
const dumb = await onTerminal([], [], "dumb");
expect("a dumb terminal: the usage", dumb.status === 0 && !dumb.raw.includes("\x1b[?1049h") && dumb.raw.startsWith("usage: heff COMMAND ..."), dumb.raw.slice(0, 200));

console.log(failed ? `terminal: ${failed}/${checks} checks FAILED` : `terminal: ${checks} checks`);
process.exit(failed ? 1 : 0);
