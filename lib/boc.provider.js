// boc's side, in JavaScript: one thread, the behaviours run in turns.
// A cown is an object of this provider's, which the program holds
// itself: what it holds, the behaviours queued at it, the first the one
// holding it, those waiting for it to be written (`once`), and what the
// last behaviour done on it knew of the others.  Which behaviour comes
// before which is the paper's causality (Cheeseman et al., "When
// Behaviours Have to Happen", CONCUR 2026, Definition 7), as the C
// runtime keeps it (see lib/boc.provider.c): a behaviour waits for the
// latest its spawner knew of on each of its cowns, not done -- what the
// spawner knew passing along a body, captured by a spawn for the body
// it spawns, and left at each cown by the behaviour done on it for the
// next (`co`) -- and is queued at its cowns only once those are done;
// behaviours with no such path between them are in no order, whichever
// was spawned first.  Those released by one behaviour's end are queued
// in an order drawn at random, as in C.  A spawn whose context is not
// known -- made where no behaviour's or act's turn is being taken while
// a body or an act is under way, so after a wait of another provider's,
// whose rest runs as no one's -- comes after every behaviour spawned
// before it on its cowns, as the clock has it (the sound choice, where
// the causal past is not to be had), and so does what its body spawns.
// What is to be done -- a behaviour ready to run,
// what comes after an operation of this effect's -- is a turn, put at
// the back of those waiting (`ready`), each with whose it is: a
// behaviour's, an act's, a failure's being said, or the program's.  The turns are taken in a loop (`turns`), not by calls in
// calls, so the stack does not grow with them; and what fails in a turn
// is its owner's.
//
// A behaviour runs: its condition checked of what its cowns hold, a
// tuple as they are (`MkPair`), and its body given its turn -- the
// behaviour -- from which it takes what they hold (`Taken`), and to
// which it gives what they are to hold (`Given`), written back once the
// body has come to its end, the next let through at each cown.  A body
// whose rest comes later (an operation of another effect's that answers
// later) holds its cowns until then, the others going on meanwhile.
// Its condition not holding, a behaviour lets go of its cowns and waits
// on those it reads, queued again by the first body to write one.  A
// body that abandons is done with that, its cowns as they were (written
// only after the body), and that is said on the console, the program to
// end by 1 (`process.exitCode`), the others going on.  A body that fails
// -- panics, or fails in the runtime -- ends the program at once: said,
// and the process exited by 1, no other turn taken (`fatal`).
//
// The program has one pool, made by its first operation of boc's, which
// counts the behaviours queued and running.  Every `when` spawns in it.
// The program ends as JavaScript's does, once nothing is left to run or
// waited for: those still waiting on a condition are dropped with it.
//
// An act awaited is a turn of its own, owned by the act -- not by any
// behaviour -- counted its pool's until it has come to its end: the act,
// then its rest, which spawns the behaviour after it.  An act that
// waits (a sleep) holds no cown meanwhile; its rest, coming later, is
// the program's turn.  One that abandons, in its own turn, is given to
// the pool's `failed`, as a body is; one that fails ends the program.  Two raced are two such turns, counted
// as one: the first to come to its value is told so (`Won`), and the
// pool counts the race done once that one's rest is; the other is not
// given up -- nothing here undoes a wait -- but runs on, told it was not
// first, its rest doing nothing.
export const boc = () => {
  const unit = { $: "MkUnit", a: [] };
  const open = (s) => $tr(s((v) => v));
  const why = (e) => (e instanceof Error ? e.message : String(e));
  // (`e`, where it is a failure -- not an abandon, nor fuel run out --
  // the program's end at once: said, and the process exited by 1; where
  // there is no process, a page's, thrown on, past every turn)
  const fatal = (e) => {
    const w = why(e);
    if (w === "heff:abandon" || w === "heff:fuel") return;
    if (typeof process !== "undefined" && typeof process.exit === "function") {
      console.error(w.startsWith("heff: ") ? w : "heff: " + w);
      process.exit(1);
    }
    throw e;
  };
  const isCown = (v) => v !== null && typeof v === "object" && v.$heffCown === true;
  // (the cowns `cs` is, a cown or a tuple of them, in the order of the
  // tuple)
  const leaves = (cs, out) => {
    if (isCown(cs)) { out.push(cs); return out; }
    if (cs !== null && typeof cs === "object" && cs.$ === "MkUnit") return out;
    if (cs === null || typeof cs !== "object" || cs.$ !== "MkPair")
      throw new Error("boc: a behaviour on what is not a cown, nor a tuple of them");
    leaves(cs.a[0], out);
    leaves(cs.a[1], out);
    return out;
  };
  // (where the first cown of `cowns` that is there twice is, counted
  // from 1, both times; null where none is)
  const twice = (cowns) => {
    for (let j = 1; j < cowns.length; j++)
      for (let i = 0; i < j; i++) if (cowns[i] === cowns[j]) return [i + 1, j + 1];
    return null;
  };
  const nth = (n) =>
    n + (n % 100 >= 11 && n % 100 <= 13 ? "th" : ["th", "st", "nd", "rd"][n % 10] || "th");
  // (what they hold, a tuple as they are)
  const contents = (cs) =>
    isCown(cs) ? cs.v
    : cs.$ === "MkUnit" ? cs
    : { $: "MkPair", a: [contents(cs.a[0]), contents(cs.a[1])] };
  // (whether the `i`th cown's bit is set: all of them where the bits are
  // -1)
  const bit = (bits, i) => bits < 0n || (i < 62 && ((bits >> BigInt(i)) & 1n) === 1n);
  // (`v` taken apart as the cowns are, each written back where its bit
  // says)
  const written = (cs, v, ws, at) => {
    if (isCown(cs)) { if (bit(ws, at.i++)) cs.v = v; return; }
    if (cs.$ === "MkUnit") return;
    written(cs.a[0], v.a[0], ws, at);
    written(cs.a[1], v.a[1], ws, at);
  };
  // (a queue, first in first out, taken from at its front by an index:
  // no array shifted along)
  const queue = () => ({ items: [], at: 0 });
  const push = (q, v) => { q.items.push(v); };
  const size = (q) => q.items.length - q.at;
  const front = (q) => q.items[q.at];
  const shift = (q) => {
    const v = q.items[q.at++];
    if (q.at > 1024 && q.at * 2 > q.items.length) { q.items = q.items.slice(q.at); q.at = 0; }
    return v;
  };
  const ready = queue();
  // Causality: what a context knows (`ctx`) is a base, a Map from a
  // cown to the behaviours known on it, shared and never written, and
  // a list of the behaviours it has spawned since, the newest first,
  // cells shared by all that captured the list then.  The latest known
  // on a cown is the newest of the list on it (which waited for every
  // earlier one on it the context knew of), else the base's.  A list
  // longer than twice the base (and 16) is laid into the base, those
  // done dropped.  `GLOBAL` is the context of a spawn whose causal past
  // is not known: every behaviour live on a cown (`c.live`), and so
  // for the body of what it spawns.
  const GLOBAL = { global: true };
  const ctxOf = (base, list, nlist) => ({ base, list, nlist });
  const emptyCtx = () => ctxOf(null, null, 0);
  const live = (b) => !b.finished;
  // (the latest a context knows of on the cown, not done)
  const knownOn = (k, c) => {
    if (k === GLOBAL) return [...c.live];
    for (let l = k.list; l !== null; l = l.tail)
      if (l.b.cowns.includes(c)) return live(l.b) ? [l.b] : [];
    if (k.base === null) return [];
    const bs = k.base.get(c);
    return bs === undefined ? [] : bs.filter(live);
  };
  // (what a context knows gathered into `into`, a Map: of its list, the
  // newest on each cown, which stands for the earlier ones on it and
  // the base's at it; of its base, the entries at the other cowns)
  const gather = (into, k) => {
    const covered = new Set();
    for (let l = k.list; l !== null; l = l.tail)
      for (const c of l.b.cowns) {
        if (covered.has(c)) continue;
        covered.add(c);
        if (live(l.b)) into.set(c, (into.get(c) || []).concat(l.b));
      }
    if (k.base === null) return;
    for (const [c, bs] of k.base) {
      if (covered.has(c)) continue;
      const alive = bs.filter(live);
      if (alive.length) into.set(c, (into.get(c) || []).concat(alive));
    }
  };
  // (... each behaviour once at a cown)
  const made = (into) => {
    for (const [c, bs] of into) into.set(c, [...new Set(bs)]);
    return into.size ? into : null;
  };
  // (what the contexts know, together, laid flat: GLOBAL where any is)
  const flattened = (ks) => {
    if (ks.some((k) => k === GLOBAL)) return GLOBAL;
    const into = new Map();
    for (const k of ks) gather(into, k);
    return ctxOf(made(into), null, 0);
  };
  // (`b`, spawned by the context, known to it: first of its list)
  const added = (k, b) => {
    if (k === GLOBAL) return k;
    k.list = { b, tail: k.list };
    k.nlist++;
    if (k.nlist > 16 && k.nlist > 2 * (k.base === null ? 0 : k.base.size)) {
      const into = new Map();
      gather(into, k);
      k.base = made(into);
      k.list = null;
      k.nlist = 0;
    }
    return k;
  };
  // (the context of what runs now: a behaviour's, an act's, the
  // program's -- or GLOBAL, where a body or an act is under way and no
  // turn is being taken, so whose rest this is cannot be known)
  let inflight = 0;
  let programCtx = emptyCtx();
  const ctxHere = () => {
    if (owner !== null && owner.ctx !== undefined) return owner.ctx;
    return inflight > 0 ? GLOBAL : programCtx;
  };
  // (`b` to wait for `i`, not done, once)
  const need = (b, i, seen) => {
    if (i === b || seen.has(i) || !live(i)) return;
    seen.add(i);
    i.dependents.push(b);
    b.pending++;
  };
  // (`b`, spawned by the context `k`: those it is to come after -- the
  // latest `k` knows of on each of its cowns, and `also` where given --
  // waited for, what `k` knows captured for its body, `k` told of it;
  // queued now where none is to be waited for)
  const spawned = (b, k, also) => {
    const seen = new Set();
    if (also !== undefined) need(b, also, seen);
    for (const c of b.cowns) for (const i of knownOn(k, c)) need(b, i, seen);
    b.ctx = k === GLOBAL ? GLOBAL : ctxOf(k.base, k.list, 0);
    for (const c of b.cowns) c.live.add(b);
    added(k, b);
    resolve(b);
  };
  const resolve = (b) => {
    if (--b.pending === 0) enqueue(b);
  };
  // (`b` done: those waiting for it released, in an order drawn at
  // random -- they are in no order with one another)
  const done = (b) => {
    b.finished = true;
    for (const c of b.cowns) c.live.delete(b);
    const ds = b.dependents;
    b.dependents = null;
    for (let i = ds.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      const t = ds[i]; ds[i] = ds[j]; ds[j] = t;
    }
    for (const d of ds) resolve(d);
  };
  let taking = false;
  // (whose the turn taken is: a behaviour, a `failed` run, or a pool --
  // its computation's -- each with its pool and what it does with a
  // failure; null, the program's)
  let owner = null;
  const take = (t) => {
    const was = owner;
    owner = t[0];
    try { t[1](); }
    catch (e) {
      fatal(e);
      if (t[0] === null) throw e;
      t[0].fail(e);
    } finally { owner = was; }
  };
  const turns = () => {
    if (taking) return unit;
    taking = true;
    try { while (size(ready) > 0) take(shift(ready)); } finally { taking = false; }
    return unit;
  };
  // (what comes after an operation, a turn of its owner's)
  const on = (k, x) => {
    push(ready, [owner, () => k(x)]);
    return turns();
  };
  // (the program's pool, opened by its first operation of boc's: one
  // that abandons said, as the program's own abandon is, and the program
  // ending by 1, the others going on)
  let program;
  const failedProgram = (w) => {
    console.error(w === "heff:abandon" ? w : "heff: " + w);
    if (typeof process !== "undefined") process.exitCode = 1;
    return (k) => k(unit);
  };
  const here = () => {
    if (owner !== null) return owner.pool;
    if (program === undefined) {
      program = { live: 0, failed: failedProgram, parked: new Set(), program: true };
      program.pool = program;
    }
    return program;
  };
  // (the behaviour queued at its cowns, every one it was to come after
  // done)
  const enqueue = (b) => {
    b.waits = 0;
    for (const c of b.cowns) {
      push(c.queue, b);
      if (size(c.queue) > 1) b.waits++;
    }
    if (b.waits === 0) push(ready, [b, () => run(b)]);
  };
  // (done: what the body knew left at each of its cowns for the next to
  // hold it, and the next let through at each)
  const release = (b) => {
    for (const c of b.cowns) {
      c.co = b.ctx;
      shift(c.queue);
      const next = front(c.queue);
      if (next !== undefined && --next.waits === 0) push(ready, [next, () => run(next)]);
    }
    done(b);
  };
  // (a behaviour, its body `body` on the cowns `cs`)
  const behaviour = (pool, cs, cowns, body, cond, ws, rs) => {
    const b = {
      pool, cs, cowns, body, cond, ws, rs,
      waits: 0, taken: undefined, given: undefined, finished: false,
      pending: 1, dependents: [], ctx: undefined, running: false,
    };
    pool.live++;
    b.fail = (e) => {
      if (b.finished) throw e;
      failedBody(b, e);
    };
    return b;
  };
  // (its condition not holding: waiting on the cowns it reads, as one
  // of its own, to be queued again)
  const park = (b) => {
    const w = { armed: true, b: behaviour(b.pool, b.cs, b.cowns, b.body, b.cond, b.ws, b.rs) };
    b.cowns.forEach((c, i) => { if (bit(b.rs, i)) c.waiting.push(w); });
    b.pool.parked.add(w);
  };
  // (those waiting on the cowns it wrote spawned again by it, as its
  // last spawn: after it, and after all it knew of on their cowns)
  const woken = (b) => {
    b.cowns.forEach((c, i) => {
      if (!bit(b.ws, i) || c.waiting.length === 0) return;
      const ws = c.waiting;
      c.waiting = [];
      for (const w of ws) {
        if (!w.armed) continue;
        w.armed = false;
        w.b.pool.parked.delete(w);
        spawned(w.b, b.ctx, b);
      }
    });
  };
  const fewer = (pool) => {
    pool.live--;
  };
  // (a body abandoned: `failed why` run, a turn of its own while the
  // behaviour holds its cowns, what that fails at the pool's, the first
  // kept; then the behaviour is done)
  const failedBody = (b, e) => {
    b.finished = true;
    const pool = b.pool;
    let on = false;
    const after = () => {
      if (on) return unit;
      on = true;
      if (b.running) { b.running = false; inflight--; }
      release(b);
      fewer(pool);
      return unit;
    };
    const f = {
      pool,
      fail: (e2) => {
        if (on) throw e2;
        if (pool.why === undefined) pool.why = e2;
        after();
      },
    };
    push(ready, [f, () => $tr($tr(pool.failed(why(e)))((u) => after()))]);
  };
  // (an act abandoned: `failed why` run, a turn of its own, counted its
  // pool's, what that fails at the pool's)
  const failedAct = (pool, e) => {
    pool.live++;
    let over = false;
    const after = () => {
      if (over) return unit;
      over = true;
      fewer(pool);
      return unit;
    };
    const f = {
      pool,
      fail: (e2) => {
        if (over) throw e2;
        if (pool.why === undefined) pool.why = e2;
        after();
      },
    };
    push(ready, [f, () => $tr($tr(pool.failed(why(e)))((u) => after()))]);
  };
  // (an act, `c`, run as a turn of its own, which is its turn `tn` too:
  // knowing what the context awaiting it knew (`ctx`), and of a race its
  // `race` and which arm it is (`me`); `settle` told whether it came to
  // its end, or failed, once)
  const act = (pool, c, ctx, race, settle) => {
    let over = false;
    inflight++;
    const o = {
      pool,
      ctx: ctx === GLOBAL ? GLOBAL : ctxOf(ctx.base, ctx.list, 0),
      race,
      me: {},
      fail: (e) => {
        if (over) throw e;
        over = true;
        inflight--;
        failedAct(pool, e);
        settle(false);
      },
    };
    push(ready, [o, () => $tr($tr(c(o))((u) => {
      if (!over) { over = true; inflight--; settle(true); }
      return unit;
    }))]);
    return o;
  };
  // (the behaviour run, its turn: its body's context what its spawn
  // knew and what the last on each of its cowns knew, together)
  const run = (b) => {
    b.ctx = flattened([b.ctx, ...b.cowns.map((c) => c.co)]);
    const v = contents(b.cs);
    if (b.cond.$ === "Just" && $tr(b.cond.a[0](v)).$ !== "True") {
      park(b);
      release(b);
      fewer(b.pool);
      return;
    }
    b.taken = v;
    b.running = true;
    inflight++;
    const ended = (u) => {
      if (b.finished) return unit;
      if (b.given === undefined) throw new Error("boc: a body ended without a value");
      b.running = false;
      inflight--;
      written(b.cs, b.given, b.ws, { i: 0 });
      b.given = undefined;
      woken(b);
      release(b);
      fewer(b.pool);
      return turns();
    };
    $tr($tr(b.body(b))(ended));
  };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "NewCown": {
        const m = open(a[0]);
        return on(k, m.a[1]({ $heffCown: true, v: m.a[0], queue: queue(), waiting: [], live: new Set(), co: emptyCtx() }));
      }
      case "When": {
        const bh = open(a[0]);
        const pool = here();
        const cs = bh.a[0];
        const cowns = leaves(cs, []);
        const at = twice(cowns);
        // (the same cown twice: a failure, said as a body's is, the
        // behaviour never queued, the spawner going on)
        if (at) {
          failedAct(pool, new Error("boc: the same cown twice in one `with`, its "
            + nth(at[0]) + " and " + nth(at[1]) + " cowns: the behaviour not spawned"));
          return on(k, a[2]);
        }
        spawned(behaviour(pool, cs, cowns, $carriedFn(a[1]), bh.a[1], bh.a[2], bh.a[3]), ctxHere(), undefined);
        return on(k, a[2]);
      }
      case "Taken": {
        const tk = open(a[0]);
        const tn = tk.a[0];
        const v = tn.taken;
        if (v === undefined) throw new Error("boc: what the cowns hold taken where no body runs, or twice");
        tn.taken = undefined;
        return on(k, tk.a[1](v));
      }
      case "Given": {
        const gv = open(a[0]);
        gv.a[0].given = gv.a[1];
        return on(k, gv.a[2]);
      }
      case "Awaits": {
        const pool = here();
        pool.live++;
        act(pool, $carriedFn(a[0]), ctxHere(), null, (ended) => fewer(pool));
        return on(k, a[1]);
      }
      case "Races": {
        const pool = here();
        const race = { won: false, winner: null, left: 2 };
        const ctx = ctxHere();
        pool.live++;
        const arm = (c) => {
          const o = act(pool, c, ctx, race, (ended) => {
            race.left--;
            if (race.winner === o.me || (race.left === 0 && race.winner === null)) fewer(pool);
          });
        };
        arm($carriedFn(a[0]));
        arm($carriedFn(a[1]));
        return on(k, a[2]);
      }
      case "Won": {
        const tn = a[0];
        const first = tn.race.winner === null;
        if (first) tn.race.winner = tn.me;
        return on(k, a[1]({ $: first ? "True" : "False", a: [] }));
      }
      // (the rest of the act whose turn is `tn`, run as the act's: what
      // it spawns comes after what the act knew)
      case "After": {
        const tn = a[0];
        const c = $carried(a[1]);
        push(ready, [tn, () => $tr($tr(c)((u) => on(k, a[2])))]);
        return turns();
      }
      // (one thread: behaviours run in its turns)
      case "Workers": return on(k, a[0](1n));
      default: throw new Error("heff: not an operation of boc: " + (p && p.$));
    }
  };
};
