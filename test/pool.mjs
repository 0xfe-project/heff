// Child processes, as many at once as there are cores: the goldens' heff
// runs, the parts of heff built under ASan.  The cache (.heff/vN/*.hc) is
// shared among heff runs --
// concurrent writers of one module write the same bytes, and a torn
// file does not decode, so it is elaborated over.
import { spawn } from "node:child_process";
import { availableParallelism } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

export const root = join(dirname(fileURLToPath(import.meta.url)), "..");

// | a process run to its end: `{ status, signal, stdout, stderr }`, as
// | spawnSync answers (a timeout kills it, `signal` then set; one that
// | cannot be run, `error` set)
export const run = (cmd, args, { cwd = root, timeout = 600000 } = {}) =>
  new Promise((resolve) => {
    const p = spawn(cmd, args, { cwd });
    let stdout = "", stderr = "";
    p.stdout.setEncoding("utf8");
    p.stderr.setEncoding("utf8");
    p.stdout.on("data", (d) => { stdout += d; });
    p.stderr.on("data", (d) => { stderr += d; });
    const timer = setTimeout(() => p.kill("SIGKILL"), timeout);
    p.on("error", (error) => { clearTimeout(timer); resolve({ status: null, signal: null, stdout, stderr, error }); });
    p.on("close", (status, signal) => { clearTimeout(timer); resolve({ status, signal, stdout, stderr }); });
  });

// | `fn` over `items`, `n` at once; the answers in the items' order
export const pool = async (items, fn, n = availableParallelism()) => {
  const out = new Array(items.length);
  let next = 0;
  const worker = async () => {
    while (next < items.length) {
      const k = next++;
      out[k] = await fn(items[k], k);
    }
  };
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, worker));
  return out;
};
