// The terminal's side of lib/std/progress.hf: an effect provider (the
// shape of one is said in lib/std/dom.provider.js).  As
// progress.provider.c draws them: the newest lines every 80 ms at most,
// the first once lines have been given for 200 ms, the cursor left where
// they start; what the program writes clears them first
// (`globalThis.$heffStatus`, which io.provider.js calls), and they come
// back below it.  A command run under a task is given a pipe for its
// own progress, `ZIG_PROGRESS` (zig's protocol: progress.provider.c says
// it), and the newest tree it sends drawn by the program's function.
// The lines, a task's name, and a command and its arguments are text
// (`$toJs`, `$fromJs`); what the command writes is bytes (`$fromBytes`).
import { isatty as $progressIsatty } from "node:tty";
import { spawn as $progressSpawn } from "node:child_process";
export const $host = "node";
export const terminal = () => {
  const st = { lines: [], drawn: 0, dirty: false, since: 0, timer: null, over: false };
  const tty = (() => {
    const term = globalThis.process.env.TERM;
    try { return $progressIsatty(2) && term !== "dumb"; } catch { return false; }
  })();
  const err = (s) => globalThis.process.stderr.write(s);
  // (a line, as much of it as fits in `width` columns: a character a
  // column)
  const cut = (s, width) => Array.from(s).slice(0, Math.max(0, width)).join("");
  const draw = () => {
    const cols = globalThis.process.stderr.columns || Number(globalThis.process.env.COLUMNS) || 80;
    const rows = globalThis.process.stderr.rows || Number(globalThis.process.env.LINES) || 25;
    const lines = st.lines.slice(0, Math.max(0, rows - 2));
    let s = "\x1b[?2026h\x1b[J";
    for (const l of lines) s += cut(l, cols - 1) + "\n";
    if (lines.length) s += "\r" + "\x1bM".repeat(lines.length);
    err(s + "\x1b[?2026l");
    st.drawn = lines.length;
    st.dirty = false;
  };
  const clear = () => {
    if (st.drawn === 0) return;
    err("\x1b[J");
    st.drawn = 0;
    st.dirty = st.lines.length > 0;
  };
  const tick = () => {
    st.timer = null;
    if (st.over || !st.lines.length) return;
    if (st.dirty && Date.now() - st.since >= 200) draw();
    st.timer = setTimeout(tick, 80);
    st.timer.unref?.();
  };
  const status = {
    hold: () => clear(),
    end: () => { clear(); st.over = true; st.lines = []; if (st.timer) clearTimeout(st.timer); st.timer = null; },
  };
  const show = (lines) => {
    if (!tty || st.over) return;
    if (lines.length === st.lines.length && lines.every((l, i) => l === st.lines[i])) return;
    st.lines = lines;
    st.dirty = lines.length > 0;
    if (!lines.length) {
      clear();
      if (st.timer) clearTimeout(st.timer);
      st.timer = null;
      return;
    }
    // (200 ms waited once, from the first lines, as progress.provider.c
    // waits)
    if (!st.since) st.since = Date.now();
    globalThis.$heffStatus = status;
    if (!st.timer) {
      st.timer = setTimeout(tick, 80);
      st.timer.unref?.();
    }
  };
  // (a list of strings, as text)
  const array = (l) => { const xs = []; for (; l.$ === "Cons"; l = l.a[1]) xs.push($toJs(l.a[0])); return xs; };
  // (a packet of zig's, the tasks of its tree, as a list of `MkKid`s)
  const kids = (b) => {
    const n = b[0];
    let l = { $: "Nil", a: [] };
    for (let i = n - 1; i >= 0; i--) {
      const s = 1 + i * 128;
      const total = b.readUInt32LE(s + 4);
      const name = $fromJs(b.subarray(s + 8, s + 128).toString("utf8").replace(/\0[\s\S]*$/, ""));
      const kid = { $: "MkKid", a: [name, BigInt(b.readUInt32LE(s)), BigInt(total === 0xffffffff ? 0 : total), BigInt(b[1 + n * 128 + i])] };
      l = { $: "Cons", a: [kid, l] };
    }
    return l;
  };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "IsTerminal": return k(a[0]({ $: tty ? "True" : "False", a: [] }));
      case "ShowLines": show(array(a[0])); return k(a[1]);
      case "ShowRun": {
        const lines = a[2];
        const progress = tty && !st.over && globalThis.process.platform !== "win32";
        const env = { ...globalThis.process.env };
        if (progress) env.ZIG_PROGRESS = "3";
        const child = $progressSpawn($toJs(a[0]), array(a[1]), { env, stdio: progress ? ["inherit", "pipe", "pipe", "pipe"] : ["inherit", "pipe", "pipe"] });
        const out = [], errs = [];
        let pending = Buffer.alloc(0), done = false;
        child.stdout.on("data", (d) => out.push(d));
        child.stderr.on("data", (d) => errs.push(d));
        if (progress) child.stdio[3].on("data", (d) => {
          pending = Buffer.concat([pending, d]);
          let newest = null;
          while (pending.length >= 1 && pending.length >= 1 + pending[0] * 129) {
            const len = 1 + pending[0] * 129;
            newest = pending.subarray(0, len);
            pending = pending.subarray(len);
          }
          if (newest) show(array($tr(lines(kids(newest)))));
        });
        const finish = (status) => {
          if (done) return;
          done = true;
          k(a[3]({ status: BigInt(status), out: $fromBytes(Buffer.concat(out)), err: $fromBytes(Buffer.concat(errs)) }));
        };
        child.on("error", () => finish(-1));
        child.on("close", (code) => finish(code === null ? -1 : code));
        return;
      }
      default: throw new Error("heff: not an operation of terminal: " + (p && p.$));
    }
  };
};
