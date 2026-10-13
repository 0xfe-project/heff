// What heff's machine code is made of (#69): the __text of a heff built
// (.heff/out/heff by default) by module, by kind of definition --
// specialised copies, workers, wrappers -- the runtime's (heff.h's)
// static functions copied into each part, and what the copies the same
// but for their names come to.  Each function's size is its distance to
// the next symbol in the binary's __text (nm, otool: macOS); each is the
// module's whose part of C (.heff/out/heff.d/ by default) defines it.
//
//   node scripts/size.mjs [--top N] [--json] [BINARY [C-DIRECTORY]]

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const at = args.indexOf(name);
  if (at < 0) return fallback;
  const [value] = args.splice(at, 2).slice(1);
  return value;
};
const json = args.includes("--json") && args.splice(args.indexOf("--json"), 1);
const top = Number(option("--top", "20"));
const binary = args[0] ?? ".heff/out/heff";
const cDirectory = args[1] ?? ".heff/out/heff.d";

const run = (command, ...rest) => execFileSync(command, rest, { encoding: "utf8", maxBuffer: 1 << 30 });

// (the __text section: where it begins and ends)
const loads = run("otool", "-l", binary).split("\n");
const textAt = loads.findIndex((line) => /sectname __text$/.test(line.trim()));
const field = (name) => BigInt(loads.slice(textAt, textAt + 6).find((l) => l.trim().startsWith(name)).trim().split(/\s+/)[1]);
const textStart = field("addr");
const textEnd = textStart + field("size");

// (each symbol in __text, its size the distance to the next)
const symbols = run("nm", "-n", "-m", binary)
  .split("\n")
  .filter((line) => line.includes("(__TEXT,__text)"))
  .map((line) => {
    const words = line.trim().split(/\s+/);
    return { at: BigInt("0x" + words[0]), name: words[words.length - 1].replace(/^_/, "") };
  });
symbols.forEach((s, k) => (s.size = Number((k + 1 < symbols.length ? symbols[k + 1].at : textEnd) - s.at)));
const textSize = Number(textEnd - textStart);

// (each function the C defines: its part, and its body, its own names
// made one, for the copies the same)
const definition = /^(?:static |inline |__attribute__\(\([^)]*\)\) |HEFF_KNOWN )*[A-Za-z_][\w ]*?[ *](\w+)\(.*\) \{$/;
const defined = new Map();
const bodyOf = new Map();
const parts = readdirSync(cDirectory).filter((f) => f.endsWith(".c"));
for (const file of parts) {
  const lines = readFileSync(join(cDirectory, file), "utf8").split("\n");
  for (let k = 0; k < lines.length; k++) {
    const match = definition.exec(lines[k]);
    if (!match) continue;
    const name = match[1];
    if (file === "heff.h") continue;
    defined.set(name, file);
    let end = k + 1;
    while (end < lines.length && lines[end] !== "}") end++;
    bodyOf.set(name, lines.slice(k, end + 1).join("\n"));
    k = end;
  }
}

// (a part's module: list.3.c is list's)
const moduleOf = (file) => file.replace(/\.c$/, "").replace(/\.\d+$/, "");
const specialised = /_24s\d+x\d+/;
const kindOf = (name) =>
  /_entry$/.test(name) ? "entry (wrapper)"
  : /_lam\d+$/.test(name) ? "lambda"
  : /_24dps$/.test(name) ? "$dps"
  : /_24cpr(_str\d+)?$/.test(name) ? "$cpr"
  : /_24f$/.test(name) ? "$f"
  : "plain";

const modules = new Map();
const kinds = new Map();
const add = (table, key, size) => {
  const row = table.get(key) ?? { size: 0, count: 0 };
  row.size += size;
  row.count += 1;
  table.set(key, row);
};
const sizeOf = new Map();
let spec = { size: 0, count: 0 };
let specBase = { size: 0, count: 0 };
const copiedRuntime = new Map();
const specOf = new Map();
for (const s of symbols) {
  const file = defined.get(s.name);
  if (file) {
    add(modules, moduleOf(file), s.size);
    sizeOf.set(s.name, s.size);
    const kind = kindOf(s.name);
    add(kinds, (specialised.test(s.name) ? "spec " : "") + kind, s.size);
    if (specialised.test(s.name)) {
      spec.size += s.size;
      spec.count += 1;
      if (/_24s\d+x\d+$/.test(s.name)) (specBase.size += s.size), (specBase.count += 1);
      add(specOf, s.name.replace(/_24s\d+x\d+.*/, ""), s.size);
    }
  } else {
    // (heff.h's, and the libc's thunks: what no part defines)
    add(modules, "(runtime)", s.size);
    add(copiedRuntime, s.name, s.size);
  }
}

// (the copies the same but for their names: a function's own name, and
// the names it gives its wrappers and lambdas, made one; then, again
// until none is found anew, the names of the copies found made their
// first's, so that copies calling copies are found)
const own = (name) => /_entry$|_lam\d+$/.test(name);
const firstOf = new Map();
const renamed = (token) => {
  const derived = /^(.*?)(_entry|_lam\d+)?$/.exec(token);
  const first = firstOf.get(derived[1]);
  return first ? first + (derived[2] ?? "") : token;
};
let groups;
for (let found = -1; found !== firstOf.size; ) {
  found = firstOf.size;
  groups = new Map();
  for (const [name, body] of bodyOf) {
    if (!sizeOf.has(name) || own(name)) continue;
    const normal = body
      .split(name)
      .join("SELF")
      .replace(/\bg_\w+/g, renamed);
    const key = createHash("sha256").update(normal).digest("hex");
    const group = groups.get(key) ?? [];
    group.push(name);
    groups.set(key, group);
  }
  for (const group of groups.values()) for (const name of group.slice(1)) firstOf.set(name, group[0]);
}
// (a copy the same as another, with its wrapper and lambdas, would go)
const family = (name) =>
  [name, name + "_entry", ...[...sizeOf.keys()].filter((n) => n.startsWith(name + "_lam"))].reduce((sum, n) => sum + (sizeOf.get(n) ?? 0), 0);
let duplicates = { functions: 0, bytes: 0, groups: 0, spec: 0, specBytes: 0 };
const largest = [];
for (const group of groups.values()) {
  if (group.length < 2) continue;
  duplicates.groups += 1;
  for (const name of group.slice(1)) {
    const bytes = family(name);
    duplicates.functions += 1;
    duplicates.bytes += bytes;
    if (specialised.test(name)) (duplicates.spec += 1), (duplicates.specBytes += bytes);
  }
  largest.push({ names: group.slice(0, 3), copies: group.length, bytes: family(group[0]) * (group.length - 1) });
}
largest.sort((a, b) => b.bytes - a.bytes);

const copied = [...copiedRuntime].map(([name, row]) => ({ name, ...row })).filter((r) => r.count > 1).sort((a, b) => b.size - a.size);
const copiedBytes = copied.reduce((sum, r) => sum + r.size, 0);
const copiedOnce = copied.reduce((sum, r) => sum + r.size / r.count, 0);

const report = {
  binary,
  text: textSize,
  modules: [...modules].map(([name, row]) => ({ name, ...row })).sort((a, b) => b.size - a.size),
  kinds: [...kinds].map(([name, row]) => ({ name, ...row })).sort((a, b) => b.size - a.size),
  spec,
  specBase,
  specialisedFrom: [...specOf].map(([name, row]) => ({ name, ...row })).sort((a, b) => b.size - a.size).slice(0, top),
  duplicates,
  largestDuplicates: largest.slice(0, top),
  runtimeCopies: { bytes: copiedBytes, ifOnce: Math.round(copiedOnce), largest: copied.slice(0, top) },
};
if (json) {
  console.log(JSON.stringify(report, null, 2));
  process.exit(0);
}

// (a name as the source says it: g_list_2emap_24s1x2 is list.map$s1x2)
const shown = (name) => name.replace(/^g_/, "").replace(/_([0-9a-f]{2})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16)));
// (a module's lines of source, where it is found: src/, lib/, app/)
const linesOf = (module) => {
  const path = module.replace(/\./g, "/") + ".hf";
  const file = ["src/", "lib/", ""].map((d) => d + path).find((f) => existsSync(f));
  return file ? readFileSync(file, "utf8").split("\n").length : 0;
};
const kb = (n) => (n / 1024).toFixed(0).padStart(7) + " KB";
const share = (n) => ((100 * n) / textSize).toFixed(1).padStart(5) + "%";
console.log(`${binary}: __text ${kb(textSize)}, ${symbols.length} functions`);
console.log(`\nmodules, the ${top} largest (functions, bytes a line of source):`);
for (const m of report.modules.slice(0, top)) {
  const lines = linesOf(m.name);
  const perLine = lines ? String(Math.round(m.size / lines)).padStart(6) : "      ";
  console.log(`${kb(m.size)} ${share(m.size)} ${String(m.count).padStart(6)} ${perLine}  ${m.name}`);
}
console.log(`\nkinds:`);
for (const k of report.kinds) console.log(`${kb(k.size)} ${share(k.size)} ${String(k.count).padStart(6)}  ${k.name}`);
console.log(`\nspecialised copies: ${kb(spec.size)} ${share(spec.size)} in ${spec.count} functions (${specBase.count} copies themselves, ${kb(specBase.size)})`);
for (const g of report.specialisedFrom.slice(0, 10)) console.log(`${kb(g.size)} ${share(g.size)} ${String(g.count).padStart(6)}  ${shown(g.name)}`);
console.log(`the same but for their names: ${duplicates.functions} functions beyond the first of ${duplicates.groups} groups, ${kb(duplicates.bytes)} ${share(duplicates.bytes)} (${duplicates.spec} of them specialised, ${kb(duplicates.specBytes)})`);
for (const d of report.largestDuplicates.slice(0, 10)) console.log(`${kb(d.bytes)}  ${d.copies}x  ${d.names.map(shown).join("  ")}`);
console.log(`\nthe runtime's static functions, a copy a part: ${kb(copiedBytes)} ${share(copiedBytes)}, once each ${kb(copiedOnce)}`);
for (const r of copied.slice(0, 10)) console.log(`${kb(r.size)} ${String(r.count).padStart(4)}x  ${r.name}`);
