// JSON-RPC over stdio, as the Language Server Protocol frames it: each
// message a `Content-Length: n` header, a blank line, then n bytes of JSON.
// stdout carries only the messages; logs go to stderr.
import { fileURLToPath, pathToFileURL } from "node:url";
import { join, resolve } from "node:path";
import { existsSync, readFileSync, readdirSync } from "node:fs";

export const listenImpl = (onMessage) => () => {
  let buf = Buffer.alloc(0);
  process.stdin.on("data", (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      const sep = buf.indexOf("\r\n\r\n");
      if (sep < 0) return;
      const m = /Content-Length:\s*(\d+)/i.exec(buf.subarray(0, sep).toString("ascii"));
      if (!m) { buf = buf.subarray(sep + 4); continue; }
      const end = sep + 4 + Number(m[1]);
      if (buf.length < end) return;
      const body = buf.subarray(sep + 4, end).toString("utf8");
      buf = buf.subarray(end);
      try { onMessage(body)(); } catch (e) { process.stderr.write("heff lsp: " + (e && e.stack || e) + "\n"); }
    }
  });
  // (the client went away without `exit`)
  process.stdin.on("end", () => process.exit(1));
};

export const sendImpl = (s) => () => {
  const body = Buffer.from(s, "utf8");
  process.stdout.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"), body]));
};

export const logImpl = (s) => () => { process.stderr.write("heff lsp: " + s + "\n"); };

export const setTimeoutImpl = (ms) => (k) => () => setTimeout(() => k(), ms);
export const clearTimeoutImpl = (t) => () => clearTimeout(t);

// a `file:` URI's path, or null
export const uriPathImpl = (just) => (nothing) => (uri) => {
  try { return uri.startsWith("file:") ? just(fileURLToPath(uri)) : nothing; } catch (_) { return nothing; }
};
export const pathUri = (p) => pathToFileURL(p).href;
export const absPath = (p) => resolve(p);

// the `.hf` files of the project at `root`: not in a directory of another
// project (one with its own project.hf), a hidden one, or one the root's
// .gitignore names (`/output`, `node_modules`: a name, not a pattern)
export const projectFilesImpl = (root) => () => {
  let ignored = ["node_modules"];
  try {
    ignored = ignored.concat(readFileSync(join(root, ".gitignore"), "utf8").split("\n")
      .map((l) => l.trim().replace(/^\//, "").replace(/\/$/, ""))
      .filter((l) => l && !l.startsWith("#") && !/[*?\[\/!]/.test(l)));
  } catch (_) {}
  const files = [];
  const walk = (d) => {
    let entries = [];
    try { entries = readdirSync(d, { withFileTypes: true }); } catch (_) { return; }
    for (const e of entries) {
      if (e.name.startsWith(".") || ignored.includes(e.name)) continue;
      const p = join(d, e.name);
      if (e.isDirectory()) { if (!existsSync(join(p, "project.hf"))) walk(p); }
      else if (e.isFile() && e.name.endsWith(".hf")) files.push(p);
    }
  };
  if (existsSync(join(root, "project.hf"))) walk(root);
  return files;
};
