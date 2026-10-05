#!/usr/bin/env node
// The formatter written in heff (fmt/) against the PureScript reference:
// the artifact `heff fmt` itself runs (`output/heff-fmt.mjs`) must
// lay out every `.hf` file of the repository exactly as `Heff.Format`'s
// does -- the same text, or the same refusal.
import { spawnSync } from "node:child_process";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
// the CLI compiles output/heff-fmt.mjs when it is stale (bin/reference.mjs);
// have it do so, and hold that artifact -- the one `heff fmt` runs -- to
// the reference
const c = spawnSync("node", [join(root, "bin", "reference.mjs"), "fmt", "--check", join(root, "lib", "doc.hf")], { cwd: root, encoding: "utf8" });
if (c.status !== 0) { console.log("fmt port: heff fmt could not run\n" + c.stdout + c.stderr); process.exit(1); }

const lay = (await import(pathToFileURL(join(root, "output", "heff-fmt.mjs")))).default;
const { formatUnchecked } = await import(pathToFileURL(join(root, "output", "Heff.Format", "index.js")));

// the project's files, as package.json's `"heff"` names them: the source
// directories and the scratch ones (ref/'s probes are laid out too), and
// the root's own `.hf` files
const files = [];
const walk = (d) => {
  for (const e of readdirSync(d)) {
    const p = join(d, e);
    if (statSync(p).isDirectory()) walk(p);
    else if (e.endsWith(".hf")) files.push(p);
  }
};
const heffConfig = JSON.parse(readFileSync(join(root, "package.json"), "utf8")).heff ?? {};
for (const e of readdirSync(root)) if (e.endsWith(".hf")) files.push(join(root, e));
for (const d of [...(heffConfig.sources ?? []), ...(heffConfig.scratch ?? [])]) walk(join(root, d));

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
