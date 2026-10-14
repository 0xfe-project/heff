#!/usr/bin/env node
// The tests, by group: each GROUP given (all without one), each of its
// tests a script of test/, on heff (`HEFF`, else
// .heff/out/heff, built by `make build`).  CI runs them all but asan in
// one job, on a runner of two cores.  (That heff compiles itself to
// itself is `make test-fixpoint`'s, by zig: `zig build fixpoint`.)
// (The scripts run all at once -- each makes what it makes in a
// directory of its own, the cache shared -- what
// each says kept, and shown whole when it is done.  What they run, they
// run within one budget, as make's jobserver keeps one: so many
// processes at once among them all, as many as there are cores, each
// taking a slot of it; or they would run some
// dozens at once, each the slower for it, and the nearer its timeout.)
//
// (A test `heff ARGS` is heff's own, `heff test`: it runs its integration
// tests as many at once as `--jobs` says, given half the budget's slots,
// held while it runs.)
//
//   node test/run.mjs [GROUP...]
//   HEFF_TEST_JOBS=N       N slots, not as many as there are cores
import { spawn } from "node:child_process";
import { availableParallelism } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { serve, slot } from "./pool.mjs";
import { heffBin } from "./heff.mjs";

const here = dirname(fileURLToPath(import.meta.url));

export const groups = {
  // what heff says of programs: elaborating, formatting, its cache, its
  // command line, projects, what it draws on a terminal, its commands'
  // app, `heff dev`, and the texts it said before
  compiler: [["cli.mjs"], ["project.mjs"], ["progress.mjs"], ["terminal.mjs"], ["dev.mjs"], ["heff", "test", "test/golden.hf", "test/surface.hf", "test/format.hf", "test/cache.hf", "test/backcache.hf"]],
  lsp: [["lsp.mjs"]],
  // programs compiled by each backend and run, held to the interpreter
  backends: [["heff", "test", "test/backends.hf", "test/apps.hf"]],
  // heff through C, under AddressSanitizer
  asan: [["heff", "test", "test/asan.hf"]],
};

const wanted = process.argv.slice(2);
for (const g of wanted) if (!groups[g]) { console.error(`test: no group ${g} (${Object.keys(groups).join(", ")})`); process.exit(2); }
const failed = [];
const slots = Number(process.env.HEFF_TEST_JOBS) || availableParallelism();
const budget = await serve(slots);
process.env.HEFF_TEST_JOBSERVER = budget.address;
const env = { ...process.env };
const share = Math.max(1, Math.floor(slots / 2));
// (the program a test is, and its arguments; and the slots it holds)
const launch = (script, args) => script === "heff"
  ? [heffBin, [...args, "--jobs", String(share)]]
  : [process.execPath, [join(here, script), ...args]];
const held = (script) => Promise.all(Array.from({ length: script === "heff" ? share : 0 }, slot));
await Promise.all((wanted.length ? wanted : Object.keys(groups)).flatMap((g) =>
  groups[g].map(([script, ...args]) => new Promise(async (done) => {
    const name = [script, ...args].join(" "), t0 = Date.now();
    const given = await held(script);
    const p = spawn(...launch(script, args), { env, cwd: join(here, ".."), stdio: ["ignore", "pipe", "pipe"] });
    let said = "";
    p.stdout.on("data", (d) => { said += d; });
    p.stderr.on("data", (d) => { said += d; });
    p.on("close", (status) => {
      for (const back of given) back();
      if (status !== 0) failed.push(name);
      process.stdout.write(`== ${g}: ${name} (${((Date.now() - t0) / 1000).toFixed(1)}s${status !== 0 ? ", FAILED" : ""})\n${said}`);
      done();
    });
  }))));
budget.close();
if (failed.length) console.log(`test: FAILED ${failed.join(", ")}`);
process.exit(failed.length ? 1 : 0);
