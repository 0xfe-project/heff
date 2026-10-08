// The disk's side of lib/std/filesystem.hf: an effect provider (the
// shape of one is said in lib/std/dom.provider.js).
import { readFile as $readFile, writeFile as $writeFile, mkdir as $mkdir, readdir as $readdir, rm as $fsRm, cp as $fsCp, rename as $fsRename, lstatSync as $fsLstatSync, readdirSync as $fsReaddirSync, chmodSync as $fsChmodSync } from "node:fs";
import { join as $fsJoin } from "node:path";
// (something is at `p`: a file, a directory, a link)
const $fsThere = (p) => { try { $fsLstatSync(p); return true; } catch { return false; } };
// (every file and directory under `p`, its mode's write bits off, or on:
// a directory is made writable before what it holds, read-only after)
const $fsChmodAll = (p, writable) => {
  const st = $fsLstatSync(p);
  if (st.isSymbolicLink()) return;
  const mode = writable ? st.mode | 0o200 : st.mode & ~0o222;
  if (st.isDirectory()) {
    if (writable) $fsChmodSync(p, mode);
    for (const n of $fsReaddirSync(p)) $fsChmodAll($fsJoin(p, n), writable);
    if (!writable) $fsChmodSync(p, mode);
  } else $fsChmodSync(p, mode);
};
export const $host = "node";
export const fs = () => {
  const nothing = { $: "Nothing", a: [] };
  const just = (x) => ({ $: "Just", a: [x] });
  const bool = (b) => ({ $: b ? "True" : "False", a: [] });
  const list = (xs) => xs.reduceRight((rest, x) => ({ $: "Cons", a: [x, rest] }), { $: "Nil", a: [] });
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "ReadFile": $readFile(a[0], "utf8", (e, t) => k(a[1](e ? nothing : just(t)))); return;
      case "WriteFile": $writeFile(a[0], a[1], (e) => k(a[2](bool(!e)))); return;
      case "MkDir": $mkdir(a[0], { recursive: true }, (e) => k(a[1](bool(!e)))); return;
      case "ReadDir": $readdir(a[0], (e, ns) => k(a[1](e ? nothing : just(list(ns.sort()))))); return;
      case "Remove": {
        try { $fsChmodAll(a[0], true); } catch (e) { if (e.code !== "ENOENT") return k(a[1](bool(false))); }
        $fsRm(a[0], { recursive: true, force: true }, (e) => k(a[1](bool(!e)))); return;
      }
      case "ReadOnly": {
        let ok = true;
        try { $fsChmodAll(a[0], false); } catch (e) { ok = false; }
        return k(a[1](bool(ok)));
      }
      case "Copy": {
        if ($fsThere(a[1])) return k(a[2](bool(false)));
        $fsCp(a[0], a[1], { recursive: true, dereference: true, errorOnExist: true, force: false }, (e) => k(a[2](bool(!e)))); return;
      }
      case "Move": {
        if ($fsThere(a[1])) return k(a[2](bool(false)));
        $fsRename(a[0], a[1], (e) => k(a[2](bool(!e)))); return;
      }
      default: throw new Error("heff: not an operation of fs: " + (p && p.$));
    }
  };
};
