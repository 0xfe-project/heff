#!/usr/bin/env node
// The formatter written in heff (fmt/) against the PureScript reference:
// `fmt/layout.hf` is compiled to JavaScript, and its `formatUnchecked` must
// lay out every `.hf` file of the repository exactly as `Heff.Format`'s
// does -- the same text, or the same refusal.
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, readdirSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const out = join(mkdtempSync(join(tmpdir(), "heff-fmt-")), "layout.mjs");
const c = spawnSync("node", [join(root, "bin", "heff.mjs"), "compile-js", "fmt/layout.hf", out], { cwd: root, encoding: "utf8" });
if (c.status !== 0) { console.log("fmt port: fmt/layout.hf does not compile\n" + c.stdout + c.stderr); process.exit(1); }

const lay = (await import(pathToFileURL(out))).default;
const { formatUnchecked } = await import(pathToFileURL(join(root, "output", "Heff.Format", "index.js")));

const files = [];
const walk = (d) => {
  for (const e of readdirSync(d)) {
    if (e === "node_modules" || e === "output" || e === ".git") continue;
    const p = join(d, e);
    if (statSync(p).isDirectory()) walk(p);
    else if (e.endsWith(".hf")) files.push(p);
  }
};
walk(root);

const psEither = (v) => v.constructor.name === "Left" ? { left: v.value0 } : { right: v.value0 };
const hfEither = (v) => v.$ === "Left" ? { left: v.a[0] } : { right: v.a[0] };
let same = 0, diff = 0;
for (const f of files) {
  const src = readFileSync(f, "utf8");
  const ps = psEither(formatUnchecked(src));
  const hf = hfEither(lay.formatUnchecked(src));
  if (JSON.stringify(ps) === JSON.stringify(hf)) same++;
  else if (++diff <= 3) console.log(`fmt port differs on ${f.slice(root.length + 1)}:\n  ps: ${JSON.stringify(ps).slice(0, 200)}\n  hf: ${JSON.stringify(hf).slice(0, 200)}`);
}
console.log(`fmt port: ${same}/${files.length} files laid out as the reference does`);
process.exit(diff === 0 ? 0 : 1);
