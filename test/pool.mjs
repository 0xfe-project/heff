// The differentials' runner: each invocation a child process, as many
// at once as there are cores.  The port's cache (.heff/*.hc) is shared
// among them -- concurrent writers of one module write the same bytes,
// and a torn file does not decode, so it is elaborated over.
import { spawn } from "node:child_process";
import { availableParallelism } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

export const root = join(dirname(fileURLToPath(import.meta.url)), "..");
export const bin = join(root, "output", "heff-selfcheck.mjs");

// | a process run to its end: `{ status, signal, stdout, stderr }`, as
// | spawnSync answers (a timeout kills it, `signal` then set)
export const run = (cmd, args, { cwd = root, timeout = 600000 } = {}) =>
  new Promise((resolve) => {
    const p = spawn(cmd, args, { cwd });
    let stdout = "", stderr = "";
    p.stdout.setEncoding("utf8");
    p.stderr.setEncoding("utf8");
    p.stdout.on("data", (d) => { stdout += d; });
    p.stderr.on("data", (d) => { stderr += d; });
    const timer = setTimeout(() => p.kill("SIGKILL"), timeout);
    p.on("close", (status, signal) => { clearTimeout(timer); resolve({ status, signal, stdout, stderr }); });
  });

// | the reference compiler, and the self-hosted one (on a worker's big
// | stack: its interpreter has no stack chunking)
export const heff = (args, opts) => run("node", ["--stack-size=60000", join(root, "bin", "heff.mjs"), ...args], opts);
export const port = (args, opts) => run("node", [join(root, "test", "run-worker.mjs"), bin, ...args], opts);

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
