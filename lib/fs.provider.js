// The disk's side of lib/fs.hf: an effect provider (the shape of one
// is said in lib/dom.provider.js).
import { readFile as $readFile, writeFile as $writeFile, mkdir as $mkdir } from "node:fs";
export const $host = "node";
export const fs = () => {
  const nothing = { $: "Nothing", a: [] };
  const just = (x) => ({ $: "Just", a: [x] });
  const bool = (b) => ({ $: b ? "True" : "False", a: [] });
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "ReadFile": $readFile(a[0], "utf8", (e, t) => k(a[1](e ? nothing : just(t)))); return;
      case "WriteFile": $writeFile(a[0], a[1], (e) => k(a[2](bool(!e)))); return;
      case "MkDir": $mkdir(a[0], { recursive: true }, (e) => k(a[1](bool(!e)))); return;
      default: throw new Error("heff: not an operation of fs: " + (p && p.$));
    }
  };
};
