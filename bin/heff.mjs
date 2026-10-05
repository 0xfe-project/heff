#!/usr/bin/env node
// heff: the self-hosted compiler (compiler/, built to output/heff.mjs by
// `npm run build`) runs every command but `dump-nbe`, the reference's
// own side of the differential tests (bin/reference.mjs).  Either runs
// in a thread
// whose stack is deep enough for the elaborator and the interpreter to
// go as deep as a file or a program does.
//
// The standard library is the one beside bin/, told to the compiler by
// HEFF_LIB.
import { Worker } from "node:worker_threads";
import { existsSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(realpathSync(fileURLToPath(import.meta.url))), "..");
const selfHosted = join(root, "output", "heff.mjs");
const referenceOnly = ["dump-nbe"];

const args = process.argv.slice(2);
if (referenceOnly.includes(args[0]) || !existsSync(selfHosted)) {
  await import(pathToFileURL(join(root, "bin", "reference.mjs")));
} else {
  process.env.HEFF_LIB ??= join(root, "lib");
  // (a worker's stdout is never a terminal: whether ours is, said)
  if (process.stdout.isTTY) process.env.HEFF_TTY = "1";
  // (the language server speaks on stdin, given to its thread; when it
  // ends, heff does)
  const lsp = args[0] === "lsp";
  const worker = new Worker(selfHosted, { argv: args, resourceLimits: { stackSizeMb: 512 }, stdin: lsp });
  if (lsp) process.stdin.pipe(worker.stdin);
  worker.on("error", (e) => { console.error(e); process.exitCode = 1; });
  worker.on("exit", (code) => { if (lsp) process.exit(code); else if (code !== 0) process.exitCode = code; });
}
