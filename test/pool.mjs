// Child processes, as many at once as there are cores: the goldens' heff
// runs, the backends' programs, the parts of heff built under ASan.  The
// cache (.heff/vN/*.hc) is shared among heff runs --
// concurrent writers of one module write the same bytes, and a torn
// file does not decode, so it is elaborated over.
// Under test/run.mjs, which runs the scripts all at once, each process
// takes a slot of its budget first (`HEFF_TEST_JOBSERVER`, where it is
// served) and gives it back when it ends: so many at once among all the
// scripts, whatever each one's pool, on a machine of two cores as on one
// of fourteen.  A slot is held by a connection to test/run.mjs, and given
// back when it is closed -- by a script that crashes too -- and a process
// is given its time once it has its slot, not while it waits for one.
// (What only looks around -- `git`, `ls`, a `--version` -- is run as it
// was, with none.)  Run by itself, a script waits for none, its pools as
// many at once as there are cores.
import { spawn } from "node:child_process";
import { connect, createServer } from "node:net";
import { availableParallelism } from "node:os";
import { root } from "./heff.mjs";

// | a slot of the budget, taken -- once one is free -- and answered with
// | what gives it back; at once, and nothing given back, where there is
// | no budget.  (A slot goes to the script that holds the fewest, then to
// | the one that asked first: a script that runs one process after
// | another is not kept waiting behind a pool's.)
export const slot = () => new Promise((taken) => {
  const at = /^(.*):(\d+)$/.exec(process.env.HEFF_TEST_JOBSERVER ?? "");
  if (!at) return taken(() => {});
  const s = connect({ host: at[1], port: Number(at[2]) });
  let held = false;
  s.on("connect", () => s.write(`${process.pid}\n`));
  s.once("data", () => { held = true; s.unref(); taken(() => s.destroy()); });
  // (a budget gone, or never reached: run all the same)
  s.on("error", () => {});
  s.on("close", () => { if (!held) taken(() => {}); });
});

// | the budget of `n` slots served, for the scripts run under it: its
// | address, for `HEFF_TEST_JOBSERVER`, and what stops it.  (A connection
// | asks for a slot by writing its script's pid and a newline, and has it
// | when a newline is written back; it is free again when the connection
// | is closed, and one closed before has asked for nothing.)
export const serve = (n) => new Promise((listening) => {
  let free = n;
  const held = new Map(), waiting = [];
  const holds = (pid) => held.get(pid) ?? 0;
  const give = () => {
    while (free > 0 && waiting.length) {
      const w = waiting.reduce((a, b) => (holds(b.pid) < holds(a.pid) ? b : a));
      waiting.splice(waiting.indexOf(w), 1);
      free--;
      held.set(w.pid, holds(w.pid) + 1);
      w.given = true;
      w.s.write("\n");
    }
  };
  const server = createServer((s) => {
    let said = "", w = null;
    s.setEncoding("utf8");
    s.on("data", (d) => {
      if (w) return;
      said += d;
      const m = /^(\d+)\n/.exec(said);
      if (!m) return;
      w = { s, pid: m[1], given: false };
      waiting.push(w);
      give();
    });
    s.on("error", () => {});
    s.on("close", () => {
      if (!w) return;
      if (w.given) { free++; held.set(w.pid, holds(w.pid) - 1); }
      else waiting.splice(waiting.indexOf(w), 1);
      give();
    });
  });
  server.listen(0, "127.0.0.1", () => {
    const { address, port } = server.address();
    listening({ address: `${address}:${port}`, close: () => server.close() });
  });
});

// | a process run to its end: `{ status, signal, stdout, stderr }`, as
// | spawnSync answers (a timeout kills it, `signal` then set; one that
// | cannot be run, `error` set); its standard input `input`, then closed
// | (or, `null`, /dev/null: not a socket, which `script` cannot have); of
// | each of its outputs what comes before `keep` characters, the rest
// | let go (a process that says the same thing over and over, endlessly,
// | is not held whole); its outputs read as `encoding` says, UTF-8 (or
// | "latin1": a byte each character, as they are); begun once it has a
// | slot of the budget, its `timeout` counted from then (but at once,
// | `slot: false`, one another process of the script waits on: where
// | there is one slot, they would wait on each other)
export const run = (cmd, args, { cwd = root, timeout = 600000, env = process.env, input, keep = Infinity, encoding = "utf8", slot: budgeted = true } = {}) =>
  (budgeted ? slot() : Promise.resolve(() => {})).then((free) => new Promise((resolve) => {
    const p = spawn(cmd, args, { cwd, env, stdio: [input === null ? "ignore" : "pipe", "pipe", "pipe"] });
    p.stdin?.on("error", () => {});
    p.stdin?.end(input);
    let stdout = "", stderr = "";
    p.stdout.setEncoding(encoding);
    p.stderr.setEncoding(encoding);
    p.stdout.on("data", (d) => { if (stdout.length < keep) stdout += d; });
    p.stderr.on("data", (d) => { if (stderr.length < keep) stderr += d; });
    const timer = setTimeout(() => p.kill("SIGKILL"), timeout);
    p.on("error", (error) => { clearTimeout(timer); free(); resolve({ status: null, signal: null, stdout, stderr, error }); });
    p.on("close", (status, signal) => { clearTimeout(timer); free(); resolve({ status, signal, stdout, stderr }); });
  }));

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
