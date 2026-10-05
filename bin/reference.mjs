#!/usr/bin/env node
// The reference compiler (PureScript, output/Main): what `bin/heff.mjs`
// hands the commands the self-hosted compiler does not have yet (lsp,
// dump-nbe), what builds the self-hosted one's first
// generation, and what the differential tests hold it to.
//
// It runs in a thread of its own, whose stack is deep enough for the
// interpreter to go as deep as a program does (a list of a million, say),
// and the elaborator as deep as a file does -- the language server too,
// which speaks on stdin (given to its thread) and stdout.
//
// The formatter is written in heff (fmt/): before `fmt` or `lsp`, it is
// compiled -- by heff itself -- when output/heff-fmt.mjs is missing or
// older than its sources or the compiler, then loaded for Heff.Format.
import { Worker, isMainThread } from "node:worker_threads";
import { spawnSync } from "node:child_process";
import { readdirSync, statSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = fileURLToPath(import.meta.url);
const root = join(dirname(here), "..");
const fmtJs = join(root, "output", "heff-fmt.mjs");

const ensureFmt = () => {
  const mtime = (p) => { try { return statSync(p).mtimeMs; } catch { return -1; } };
  const srcs = [
    join(root, "output", "Main", "index.js"),
    ...["prelude", "array", "map", "doc"].map((m) => join(root, "lib", m + ".hf")),
    ...readdirSync(join(root, "fmt")).filter((f) => f.endsWith(".hf")).map((f) => join(root, "fmt", f)),
  ];
  const out = mtime(fmtJs);
  if (out >= 0 && srcs.every((s) => mtime(s) <= out)) return;
  const r = spawnSync(process.execPath, [ here, "compile-js", join(root, "fmt", "layout.hf"), fmtJs ], { stdio: "inherit" });
  if (r.status !== 0) process.exit(r.status ?? 1);
};

const cmd = process.argv[2];
if (!isMainThread) {
  if (cmd === "fmt" || cmd === "lsp") {
    ensureFmt();
    const m = await import(pathToFileURL(fmtJs));
    globalThis.$heffFormatUnchecked = m.default.formatUnchecked;
  }
  const { main } = await import(pathToFileURL(join(root, "output", "Main", "index.js")));
  main();
} else {
  const lsp = cmd === "lsp";
  // (a worker's stdout is never a terminal: whether ours is, said)
  if (process.stdout.isTTY) process.env.HEFF_TTY = "1";
  const worker = new Worker(new URL(import.meta.url), { argv: process.argv.slice(2), resourceLimits: { stackSizeMb: 512 }, stdin: lsp });
  // (the client's messages, to the server's thread; when it ends, heff does)
  if (lsp) process.stdin.pipe(worker.stdin);
  worker.on("error", (e) => { console.error(e); process.exitCode = 1; });
  worker.on("exit", (code) => { if (lsp) process.exit(code); else if (code !== 0) process.exitCode = code; });
}
