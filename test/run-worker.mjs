#!/usr/bin/env node
// Run a compiled heff program on a big stack: a worker thread's stack
// can be grown past the process's (`--stack-size` beyond the OS limit
// segfaults instead).  The port's interpreter has no stack chunking
// (the reference's `deeper`), so a deep watch needs the room.
import { Worker } from "node:worker_threads";
import { resolve } from "node:path";

const [script, ...argv] = process.argv.slice(2);
const w = new Worker(resolve(script), { argv, resourceLimits: { stackSizeMb: 2048 } });
w.on("exit", (code) => process.exit(code));
w.on("error", (e) => { console.error(e.message); process.exit(1); });
