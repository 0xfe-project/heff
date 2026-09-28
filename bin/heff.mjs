#!/usr/bin/env node
// heff runs in a thread of its own, whose stack is deep enough for the
// interpreter to go as deep as a program does (a list of a million, say);
// the language server, which speaks on stdin and stdout, runs here.
import { Worker, isMainThread } from "node:worker_threads";
import { main } from "../output/Main/index.js";

if (!isMainThread || process.argv[2] === "lsp") main();
else {
  const worker = new Worker(new URL(import.meta.url), { argv: process.argv.slice(2), resourceLimits: { stackSizeMb: 512 } });
  worker.on("error", (e) => { console.error(e); process.exitCode = 1; });
  worker.on("exit", (code) => { if (code !== 0) process.exitCode = code; });
}
