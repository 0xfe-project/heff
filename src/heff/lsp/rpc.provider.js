// The client's side of src/heff/lsp/rpc.hf: JSON-RPC over stdio, as the
// Language Server Protocol frames it -- each message a `Content-Length:
// n` header, a blank line, then n bytes of JSON; stdout carries only the
// messages, logs go to stderr -- and what of the host a language server
// asks for (the shape of a provider is said in lib/std/dom.provider.js).
// Its strings are text, a host of text's -- JSON's, and the paths and
// URIs a client names, or that are made of them (`$toJs`, `$fromJs`) --
// but for what it sends and logs, which is written out as it is
// (`$toBytes`).
import { fileURLToPath as $fileURLToPath, pathToFileURL as $pathToFileURL } from "node:url";
import { join as $join, resolve as $resolve } from "node:path";
import { readFileSync as $readFileSync, readdirSync as $readdirSync } from "node:fs";
export const $host = "node";
export const rpc = () => {
  const nil = { $: "Nil", a: [] };
  const list = (xs) => xs.reduceRight((rest, x) => ({ $: "Cons", a: [x, rest] }), nil);
  const bool = (b) => ({ $: b ? "True" : "False", a: [] });
  // (a message's JSON, as src/heff/util/json.hf's `Json`: an integer an Int)
  const json = (v) =>
    v === null ? { $: "JNull", a: [] }
    : typeof v === "boolean" ? { $: "JBool", a: [bool(v)] }
    : typeof v === "number" ? (Number.isInteger(v) ? { $: "JInt", a: [BigInt(v)] } : { $: "JFloat", a: [v] })
    : typeof v === "string" ? { $: "JString", a: [$fromJs(v)] }
    : Array.isArray(v) ? { $: "JArray", a: [list(v.map(json))] }
    : { $: "JObject", a: [list(Object.entries(v).map(([k, x]) => ({ $: "MkPair", a: [$fromJs(k), json(x)] })))] };
  // the messages read and not yet awaited; the await waiting for one
  const queue = [];
  let closed = false, waiting = null, listening = false;
  const deliver = () => {
    if (!waiting) return;
    let ev;
    if (queue.length) {
      const body = queue.shift();
      try { ev = { $: "Message", a: [json(JSON.parse(body))] }; }
      catch (e) { process.stderr.write("heff lsp: not JSON: " + e.message + "\n"); return; }
    } else if (closed) ev = { $: "Closed", a: [] };
    else return;
    const w = waiting;
    waiting = null;
    if (w.timer) clearTimeout(w.timer);
    w.k(w.kont(ev));
  };
  const listen = () => {
    if (listening) return;
    listening = true;
    let buf = Buffer.alloc(0);
    process.stdin.on("data", (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      for (;;) {
        const sep = buf.indexOf("\r\n\r\n");
        if (sep < 0) break;
        const m = /Content-Length:\s*(\d+)/i.exec(buf.subarray(0, sep).toString("ascii"));
        if (!m) { buf = buf.subarray(sep + 4); continue; }
        const end = sep + 4 + Number(m[1]);
        if (buf.length < end) break;
        queue.push(buf.subarray(sep + 4, end).toString("utf8"));
        buf = buf.subarray(end);
      }
      deliver();
    });
    process.stdin.on("end", () => { closed = true; deliver(); });
  };
  // the `.hf` files of the project at `root` (a directory with a
  // package.hf): every directory but another project's, a hidden one,
  // or one the root's .gitignore names
  const isRoot = (d) => {
    try { $readFileSync($join(d, "package.hf"), "utf8"); return true; } catch (_) { return false; }
  };
  const projectFiles = (root) => {
    const files = [];
    let ignored = ["node_modules"];
    const walk = (d) => {
      let entries = [];
      try { entries = $readdirSync(d, { withFileTypes: true }); } catch (_) { return; }
      for (const e of entries) {
        if (e.name.startsWith(".") || ignored.includes(e.name)) continue;
        const p = $join(d, e.name);
        if (e.isDirectory()) { if (!isRoot(p)) walk(p); }
        else if (e.isFile() && e.name.endsWith(".hf")) files.push(p);
      }
    };
    if (!isRoot(root)) return files;
    try {
      ignored = ignored.concat($readFileSync($join(root, ".gitignore"), "utf8").split("\n")
        .map((l) => l.trim().replace(/^\//, "").replace(/\/$/, ""))
        .filter((l) => l && !l.startsWith("#") && !/[*?\[\/!]/.test(l)));
    } catch (_) {}
    walk(root);
    return files;
  };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Await": {
        listen();
        const ms = Number(a[0]);
        waiting = { k, kont: a[1], timer: null };
        if (queue.length || closed) return deliver();
        if (ms >= 0) waiting.timer = setTimeout(() => { const w = waiting; waiting = null; w.k(w.kont({ $: "Timeout", a: [] })); }, ms);
        return;
      }
      case "Send": {
        const body = $toBytes(a[0]);
        process.stdout.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"), body]), () => k(a[1]));
        return;
      }
      case "Log": process.stderr.write(Buffer.concat([Buffer.from("heff lsp: "), $toBytes(a[0]), Buffer.from("\n")]), () => k(a[1])); return;
      case "Now": return k(a[0](BigInt(Date.now())));
      case "ProjectFiles": return k(a[1](list(projectFiles($toJs(a[0])).map($fromJs))));
      case "UriPath": {
        const u = $toJs(a[0]);
        let r = { $: "Nothing", a: [] };
        try { if (u.startsWith("file:")) r = { $: "Just", a: [$fromJs($fileURLToPath(u))] }; } catch (_) {}
        return k(a[1](r));
      }
      case "PathUri": return k(a[1]($fromJs($pathToFileURL($toJs(a[0])).href)));
      case "AbsPath": return k(a[1]($fromJs($resolve($toJs(a[0])))));
      case "Quit": { const code = Number(a[0]); process.stdout.write("", () => process.exit(code)); return; }
      default: throw new Error("heff: not an operation of rpc: " + (p && p.$));
    }
  };
};
