#!/usr/bin/env node
// The tests, by group: each GROUP given (all without one), each of its
// tests a script of test/, on heff as test/heff.mjs says (`HEFF`, else
// .heff/out/heff, built by scripts/build.mjs).  CI runs the groups apart,
// one job each.
// (The scripts run all at once -- each makes what it makes in a
// directory of its own, the cache shared as test/pool.mjs says -- what
// each says kept, and shown whole when it is done.)
//
//   node test/run.mjs [GROUP...]
import { spawn } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));

export const groups = {
  // what heff says of programs: elaborating, formatting, its cache, its
  // command line, projects, what it draws on a terminal, and the texts
  // it said before
  compiler: [["surface.mjs"], ["format.mjs"], ["cache.mjs"], ["backcache.mjs"], ["cli.mjs"], ["project.mjs"], ["progress.mjs"], ["golden.mjs"]],
  lsp: [["lsp.mjs"]],
  // programs compiled by each backend and run, held to the interpreter
  backends: [["backends.mjs", "60", "1"], ["app-run.mjs"]],
  // heff compiles itself to itself
  fixpoint: [["gen-fixpoint.mjs"]],
  // heff through C, under AddressSanitizer
  asan: [["native-asan.mjs"]],
};

const wanted = process.argv.slice(2);
for (const g of wanted) if (!groups[g]) { console.error(`test: no group ${g} (${Object.keys(groups).join(", ")})`); process.exit(2); }
const failed = [];
await Promise.all((wanted.length ? wanted : Object.keys(groups)).flatMap((g) =>
  groups[g].map(([script, ...args]) => new Promise((done) => {
    const name = [script, ...args].join(" "), t0 = Date.now();
    const p = spawn(process.execPath, [join(here, script), ...args], { stdio: ["ignore", "pipe", "pipe"] });
    let said = "";
    p.stdout.on("data", (d) => { said += d; });
    p.stderr.on("data", (d) => { said += d; });
    p.on("close", (status) => {
      if (status !== 0) failed.push(name);
      process.stdout.write(`== ${g}: ${name} (${((Date.now() - t0) / 1000).toFixed(1)}s${status !== 0 ? ", FAILED" : ""})\n${said}`);
      done();
    });
  }))));
if (failed.length) console.log(`test: FAILED ${failed.join(", ")}`);
process.exit(failed.length ? 1 : 0);
