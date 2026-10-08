#!/usr/bin/env node
// The tests, by group: each GROUP given (all without one) run in turn,
// each of its tests a script of test/, on heff as test/heff.mjs says
// (`HEFF`, else .heff/out/heff, built by scripts/build.mjs).  CI runs the
// groups apart, one job each.
//
//   node test/run.mjs [GROUP...]
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));

export const groups = {
  // what heff says of programs: elaborating, formatting, its cache, its
  // command line, projects, and the texts it said before
  compiler: [["surface.mjs"], ["format.mjs"], ["cache.mjs"], ["cli.mjs"], ["project.mjs"], ["golden.mjs"]],
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
let failed = 0;
for (const g of wanted.length ? wanted : Object.keys(groups))
  for (const [script, ...args] of groups[g]) {
    console.log(`== ${g}: ${script} ${args.join(" ")}`);
    const r = spawnSync(process.execPath, [join(here, script), ...args], { stdio: "inherit" });
    if (r.status !== 0) failed++;
  }
process.exit(failed ? 1 : 0);
