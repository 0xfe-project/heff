// Domains' side of std.domain, in JavaScript: one domain, as one core
// runs threads.  Each operation's answer is known at once or later, and
// what comes after it -- its continuation -- is put at the back of the
// turns waiting (`ready`), so the others ready go first: the domains
// take turns at their operations of this effect.  A domain waiting on
// a mutex, a condition or another's end is kept there, its turn put
// back when it can go on.  The turns are taken in a loop (`turns`),
// not by calls in calls, so the stack does not grow with them.
//
// A domain is started with the computation it runs (the world's: given
// what to do with its answer), its first turn queued.  What fails in a
// turn of a domain's is kept for its joiners, who fail as it did; what
// fails in main's ends the program -- unless the domain is in a `tried`
// (`catchers`, one for each `tried` not yet done, the innermost last):
// the failure, in whichever turn of the domain's it comes, is given to
// the innermost `tried`, a `Left`, and the domain goes on after it.  The domain whose turn it is is
// `running`, so an operation knows whose it is.  (Not so after another
// effect's operation finished later, a sleep: what comes after it runs
// outside the turns, and is counted the domain's whose turn was last.)
// Domains not joined when main ends take their turns on.  `cas`
// compares by identity.  An operation of a type of its own holds it as
// a function given what to do with it (`Some`): `open` takes it.
//
// A domain, a cell, a mutex, a condition and a queue are each an object
// of this provider's, which the program holds itself (as a value of no
// constructor): the host frees it with the program's last reference,
// nothing here keeping it.  A queue holds what was put and not yet
// taken, and the turns of the domains taking while it was empty, each
// given the next value put; one domain, so nothing is moved or shared.
export const domain = () => {
  const open = (s) => $tr(s((v) => v));
  const bool = (b) => ({ $: b ? "True" : "False", a: [] });
  const unit = { $: "MkUnit", a: [] };
  // (by identity, as C compares words: a constructor without fields is
  // itself wherever it is built)
  const same = (x, y) =>
    x === y || (typeof x === "object" && typeof y === "object" && x !== null && y !== null
      && x.$ === y.$ && x.a !== undefined && x.a.length === 0 && y.a !== undefined && y.a.length === 0);
  // (a domain: whether it ended, its answer or why it failed, and the
  // turns of those waiting for it; main is one, held by no handle)
  const main = { joiners: [], catchers: [] };
  let running = main;
  const ready = [];
  let taking = false;
  // (a turn of `d`'s: what fails in it its innermost `tried`'s, else
  // its joiners', or the program's)
  const take = (d, f) => {
    running = d;
    if (d === main && d.catchers.length === 0) { f(); return; }
    try { f(); }
    catch (e) {
      const caught = d.catchers.pop();
      if (caught) caught(e);
      else if (d === main) throw e;
      else ended(d, { failed: e });
    }
  };
  // (why a failure, as a string: `heff:abandon` for an abandon)
  const why = (e) => (e instanceof Error ? e.message : String(e));
  const turns = () => {
    if (taking) return;
    taking = true;
    try {
      while (ready.length > 0) {
        const [d, f] = ready.shift();
        take(d, f);
      }
    } finally { taking = false; }
  };
  const queue = (d, f) => { ready.push([d, f]); turns(); };
  // (`d` ended: its joiners' turns queued)
  const ended = (d, how) => {
    if (d.ended) return;
    d.ended = how;
    const js = d.joiners;
    d.joiners = [];
    for (const [w, f] of js) ready.push([w, f]);
  };
  // (a joiner's turn: the answer given on, or the failure thrown again)
  const answered = (d, go) => () => {
    if (d.ended.failed !== undefined) throw d.ended.failed;
    go(d.ended.value);
  };
  // (the mutex taken by `d` for the turn `f`, now or when it is let go of)
  const taken = (m, d, f) => {
    if (!m.held) { m.held = true; ready.push([d, f]); }
    else m.waiters.push([d, f]);
  };
  const letGo = (m) => {
    if (!m.held) throw new Error("heff: domain: a mutex unlocked by a domain not holding it");
    const next = m.waiters.shift();
    if (next) ready.push(next);
    else m.held = false;
  };
  // (a waiter on a condition woken: it takes its mutex again first)
  const woken = (c) => {
    const w = c.waiters.shift();
    if (w) taken(w.mutex, w.domain, w.turn);
  };
  return (p, k) => {
    const a = p.a;
    const d = running;
    const on = (x) => queue(d, () => k(x));
    switch (p.$) {
      case "Spawn": {
        const sp = open(a[0]);
        const c = sp.a[0];
        const child = { joiners: [], catchers: [] };
        ready.push([d, () => k(sp.a[1](child))]);
        ready.push([child, () => $tr(c((v) => { ended(child, { value: v }); return unit; }))]);
        return turns();
      }
      case "Join": {
        const j = open(a[0]);
        const target = j.a[0];
        const go = (v) => k(j.a[1](v));
        if (target.ended) return queue(d, answered(target, go));
        target.joiners.push([d, answered(target, go)]);
        return turns();
      }
      // (`c` run in a turn of this domain's, its failure caught by the
      // catcher pushed for it, or its answer given on, the catcher
      // popped)
      case "Tried": {
        const t = open(a[0]);
        const c = t.a[0];
        d.catchers.push((e) => queue(d, () => k(t.a[1]({ $: "Left", a: [why(e)] }))));
        ready.push([d, () => $tr(c((v) => { d.catchers.pop(); return k(t.a[1]({ $: "Right", a: [v] })); }))]);
        return turns();
      }
      case "Cores": return on(a[0](1n));
      // (a cell: what it holds, `v`)
      case "NewAtomic": { const m = open(a[0]); return on(m.a[1]({ v: m.a[0] })); }
      case "GetAtomic": { const r = open(a[0]); return on(r.a[1](r.a[0].v)); }
      case "SetAtomic": { const w = open(a[0]); w.a[0].v = w.a[1]; return on(w.a[2]); }
      case "CasAtomic": {
        const c = open(a[0]);
        const cell = c.a[0];
        const ok = same(cell.v, c.a[1]);
        if (ok) cell.v = c.a[2];
        return on(c.a[3](bool(ok)));
      }
      case "FetchAdd": {
        const cell = a[0];
        const old = cell.v;
        cell.v = old + a[1];
        return on(a[2](old));
      }
      case "NewMutex": return on(a[0]({ held: false, waiters: [] }));
      case "Lock": {
        taken(a[0], d, () => k(a[1]));
        return turns();
      }
      case "Unlock": { letGo(a[0]); return on(a[1]); }
      case "NewCondition": return on(a[0]({ waiters: [] }));
      case "Wait": {
        const m = a[1];
        letGo(m);
        a[0].waiters.push({ mutex: m, domain: d, turn: () => k(a[2]) });
        return turns();
      }
      case "Signal": { woken(a[0]); return on(a[1]); }
      case "Broadcast": {
        const c = a[0];
        while (c.waiters.length > 0) woken(c);
        return on(a[1]);
      }
      case "NewQueue": { const m = open(a[0]); return on(m.a[0]({ items: [], takers: [] })); }
      case "PutQueue": {
        const pt = open(a[0]);
        const q = pt.a[0], v = pt.a[1];
        const taker = q.takers.shift();
        if (taker) ready.push([taker.domain, () => taker.go(v)]);
        else q.items.push(v);
        return on(pt.a[2]);
      }
      case "TakeQueue": {
        const tk = open(a[0]);
        const q = tk.a[0];
        if (q.items.length > 0) return on(tk.a[1](q.items.shift()));
        q.takers.push({ domain: d, go: (v) => k(tk.a[1](v)) });
        return turns();
      }
      default: throw new Error("heff: not an operation of domain: " + (p && p.$));
    }
  };
};
