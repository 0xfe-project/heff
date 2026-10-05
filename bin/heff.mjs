#!/usr/bin/env node
// heff: the compiler, written in heff (compiler/), compiled by itself --
// output/heff.mjs, as `npm run build` makes it, else the seed it is made
// from (boot/heff.mjs).  It runs in a thread whose stack is deep enough
// for the elaborator and the interpreter to go as deep as a file or a
// program does; the language server speaks on stdin, given to it.
//
// The standard library is the one beside bin/, told to the compiler by
// HEFF_LIB.
import { Worker } from "node:worker_threads";
import { existsSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(realpathSync(fileURLToPath(import.meta.url))), "..");
const built = join(root, "output", "heff.mjs");
const compiler = existsSync(built) ? built : join(root, "boot", "heff.mjs");

const args = process.argv.slice(2);
process.env.HEFF_LIB ??= join(root, "lib");
// (a worker's stdout is never a terminal: whether ours is, said)
if (process.stdout.isTTY) process.env.HEFF_TTY = "1";
const lsp = args[0] === "lsp";
const worker = new Worker(compiler, { argv: args, resourceLimits: { stackSizeMb: 512 }, stdin: lsp });
// (the client's messages, to the server's thread; when it ends, heff does)
if (lsp) process.stdin.pipe(worker.stdin);
worker.on("error", (e) => { console.error(e); process.exitCode = 1; });
worker.on("exit", (code) => { if (lsp) process.exit(code); else if (code !== 0) process.exitCode = code; });
