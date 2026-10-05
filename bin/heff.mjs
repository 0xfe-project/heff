#!/usr/bin/env node
// heff: the self-hosted compiler (compiler/, built to output/heff.mjs by
// `npm run build`) runs every command it has; the few it does not have
// yet go to the reference (bin/reference.mjs).  Either runs in a thread
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
const referenceOnly = ["lsp", "dump-nbe"];

const args = process.argv.slice(2);
if (referenceOnly.includes(args[0]) || !existsSync(selfHosted)) {
  await import(pathToFileURL(join(root, "bin", "reference.mjs")));
} else {
  process.env.HEFF_LIB ??= join(root, "lib");
  // (a worker's stdout is never a terminal: whether ours is, said)
  if (process.stdout.isTTY) process.env.HEFF_TTY = "1";
  const worker = new Worker(selfHosted, { argv: args, resourceLimits: { stackSizeMb: 512 } });
  worker.on("error", (e) => { console.error(e); process.exitCode = 1; });
  worker.on("exit", (code) => { if (code !== 0) process.exitCode = code; });
}
