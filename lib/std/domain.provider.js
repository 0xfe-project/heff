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
// fails in main's ends the program.  The domain whose turn it is is
// `running`, so an operation knows whose it is.  (Not so after another
// effect's operation finished later, a sleep: what comes after it runs
// outside the turns, and is counted the domain's whose turn was last.)
// Domains not joined when main ends take their turns on.  `cas`
// compares by identity.  An operation of a type of its own holds it as
// a function given what to do with it (`Some`): `open` takes it.
export const domain = () => {
  const open = (s) => $tr(s((v) => v));
  const num = (h) => Number(h.a[0]);
  const handle = (tag, i) => ({ $: tag, a: [BigInt(i)] });
  const bool = (b) => ({ $: b ? "True" : "False", a: [] });
  const unit = { $: "MkUnit", a: [] };
  // (by identity, as C compares words: a constructor without fields is
  // itself wherever it is built)
  const same = (x, y) =>
    x === y || (typeof x === "object" && typeof y === "object" && x !== null && y !== null
      && x.$ === y.$ && x.a !== undefined && x.a.length === 0 && y.a !== undefined && y.a.length === 0);
  const cells = [], mutexes = [], conditions = [];
  // (each domain: whether it ended, its answer or why it failed, and
  // the turns of those waiting for it; main is no domain of these)
  const domains = [];
  const main = { joiners: [] };
  let running = main;
  const ready = [];
  let taking = false;
  // (a turn of `d`'s: what fails in it its joiners', or the program's)
  const take = (d, f) => {
    running = d;
    if (d === main) { f(); return; }
    try { f(); }
    catch (e) { ended(d, { failed: e }); }
  };
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
        const child = { joiners: [] };
        domains.push(child);
        ready.push([d, () => k(sp.a[1](handle("MkDomain", domains.length - 1)))]);
        ready.push([child, () => $tr(c((v) => { ended(child, { value: v }); return unit; }))]);
        return turns();
      }
      case "Join": {
        const j = open(a[0]);
        const target = domains[num(j.a[0])];
        const go = (v) => k(j.a[1](v));
        if (target.ended) return queue(d, answered(target, go));
        target.joiners.push([d, answered(target, go)]);
        return turns();
      }
      case "Cores": return on(a[0](1n));
      case "NewAtomic": {
        const m = open(a[0]);
        cells.push(m.a[0]);
        return on(m.a[1](handle("MkAtomic", cells.length - 1)));
      }
      case "GetAtomic": { const r = open(a[0]); return on(r.a[1](cells[num(r.a[0])])); }
      case "SetAtomic": { const w = open(a[0]); cells[num(w.a[0])] = w.a[1]; return on(w.a[2]); }
      case "CasAtomic": {
        const c = open(a[0]);
        const i = num(c.a[0]);
        const ok = same(cells[i], c.a[1]);
        if (ok) cells[i] = c.a[2];
        return on(c.a[3](bool(ok)));
      }
      case "FetchAdd": {
        const i = num(a[0]);
        const old = cells[i];
        cells[i] = old + a[1];
        return on(a[2](old));
      }
      case "NewMutex": {
        mutexes.push({ held: false, waiters: [] });
        return on(a[0](handle("MkMutex", mutexes.length - 1)));
      }
      case "Lock": {
        taken(mutexes[num(a[0])], d, () => k(a[1]));
        return turns();
      }
      case "Unlock": { letGo(mutexes[num(a[0])]); return on(a[1]); }
      case "NewCondition": {
        conditions.push({ waiters: [] });
        return on(a[0](handle("MkCondition", conditions.length - 1)));
      }
      case "Wait": {
        const m = mutexes[num(a[1])];
        letGo(m);
        conditions[num(a[0])].waiters.push({ mutex: m, domain: d, turn: () => k(a[2]) });
        return turns();
      }
      case "Signal": { woken(conditions[num(a[0])]); return on(a[1]); }
      case "Broadcast": {
        const c = conditions[num(a[0])];
        while (c.waiters.length > 0) woken(c);
        return on(a[1]);
      }
      default: throw new Error("heff: not an operation of domain: " + (p && p.$));
    }
  };
};
