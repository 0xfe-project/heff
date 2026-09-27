// JSON-RPC over stdio, as the Language Server Protocol frames it: each
// message a `Content-Length: n` header, a blank line, then n bytes of JSON.
// stdout carries only the messages; logs go to stderr.
import { fileURLToPath, pathToFileURL } from "node:url";
import { resolve } from "node:path";

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
      try { onMessage(body)(); } catch (e) { process.stderr.write("hg lsp: " + (e && e.stack || e) + "\n"); }
    }
  });
  // (the client went away without `exit`)
  process.stdin.on("end", () => process.exit(1));
};

export const sendImpl = (s) => () => {
  const body = Buffer.from(s, "utf8");
  process.stdout.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"), body]));
};

export const logImpl = (s) => () => { process.stderr.write("hg lsp: " + s + "\n"); };

export const setTimeoutImpl = (ms) => (k) => () => setTimeout(() => k(), ms);
export const clearTimeoutImpl = (t) => () => clearTimeout(t);

// a `file:` URI's path, or null
export const uriPathImpl = (just) => (nothing) => (uri) => {
  try { return uri.startsWith("file:") ? just(fileURLToPath(uri)) : nothing; } catch (_) { return nothing; }
};
export const pathUri = (p) => pathToFileURL(p).href;
export const absPath = (p) => resolve(p);
