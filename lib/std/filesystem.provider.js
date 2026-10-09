// The disk's side of lib/std/filesystem.hf: an effect provider (the
// shape of one is said in lib/std/dom.provider.js).  A path is given to
// Node.js as its bytes, a Buffer (`$toBytes`), and a file's text and a
// directory's names are taken as theirs (`$fromBytes`).  (Node.js's
// `cp` takes no Buffer for a path; `cpSync` does.)
import { readFile as $readFile, writeFile as $writeFile, mkdir as $mkdir, readdir as $readdir, rm as $fsRm, cpSync as $fsCpSync, rename as $fsRename, lstatSync as $fsLstatSync, readdirSync as $fsReaddirSync, chmodSync as $fsChmodSync } from "node:fs";
import { sep as $fsSep } from "node:path";
// (something is at `p`: a file, a directory, a link)
const $fsThere = (p) => { try { $fsLstatSync(p); return true; } catch { return false; } };
// (every file and directory under `p`, a Buffer, its mode's write bits
// off, or on: a directory is made writable before what it holds,
// read-only after)
const $fsChmodAll = (p, writable) => {
  const st = $fsLstatSync(p);
  if (st.isSymbolicLink()) return;
  const mode = writable ? st.mode | 0o200 : st.mode & ~0o222;
  if (st.isDirectory()) {
    if (writable) $fsChmodSync(p, mode);
    for (const n of $fsReaddirSync(p, { encoding: "buffer" })) $fsChmodAll(Buffer.concat([p, Buffer.from($fsSep), n]), writable);
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
    const path = $toBytes(a[0]);
    // (a path with a NUL in it is none: the operation fails, as Node.js
    // would throw)
    const two = p.$ === "Copy" || p.$ === "Move" || p.$ === "Replace";
    if (path.includes(0) || (two && $toBytes(a[1]).includes(0)))
      return k(a[a.length - 1](p.$ === "ReadFile" || p.$ === "ReadDir" ? nothing : bool(false)));
    switch (p.$) {
      case "ReadFile": $readFile(path, (e, b) => k(a[1](e ? nothing : just($fromBytes(b))))); return;
      case "WriteFile": $writeFile(path, $toBytes(a[1]), (e) => k(a[2](bool(!e)))); return;
      case "MkDir": $mkdir(path, { recursive: true }, (e) => k(a[1](bool(!e)))); return;
      case "ReadDir": $readdir(path, { encoding: "buffer" }, (e, ns) => k(a[1](e ? nothing : just(list(ns.map($fromBytes).sort()))))); return;
      case "Remove": {
        try { $fsChmodAll(path, true); } catch (e) { if (e.code !== "ENOENT") return k(a[1](bool(false))); }
        $fsRm(path, { recursive: true, force: true }, (e) => k(a[1](bool(!e)))); return;
      }
      case "ReadOnly": {
        let ok = true;
        try { $fsChmodAll(path, false); } catch (e) { ok = false; }
        return k(a[1](bool(ok)));
      }
      case "Copy": {
        const to = $toBytes(a[1]);
        if ($fsThere(to)) return k(a[2](bool(false)));
        let ok = true;
        try { $fsCpSync(path, to, { recursive: true, dereference: true, errorOnExist: true, force: false }); } catch (e) { ok = false; }
        return k(a[2](bool(ok)));
      }
      case "Move": {
        const to = $toBytes(a[1]);
        if ($fsThere(to)) return k(a[2](bool(false)));
        $fsRename(path, to, (e) => k(a[2](bool(!e)))); return;
      }
      // (rename(2), which puts a file in place of another at once; on
      // Windows, Node's is MoveFileEx's, replacing too)
      case "Replace": $fsRename(path, $toBytes(a[1]), (e) => k(a[2](bool(!e)))); return;
      default: throw new Error("heff: not an operation of fs: " + (p && p.$));
    }
  };
};
