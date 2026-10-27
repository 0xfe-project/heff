// Custodians, in JavaScript: each an object of this provider's, which
// the program holds itself, let go of whenever the collector comes to
// it -- what it was given dropped then, not run.  A parent holds those
// made under it, so they live as long as it does; each knows its
// parent.  A thing in a custodian's custody holds it, and the custodian
// the thing weakly: it is ended at the custodian's shutdown by what its
// provider put on it to end it (`Symbol.for("heff:end")`), where it has
// that.  What a custodian was given is run in the order given, one
// after another, each perhaps finishing later (an operation of another
// effect that answers later), the shutdown answered once all have.  A
// failure of a custodian's shuts it down, with those under it, and goes
// to the one it is under: one made to contain its failures says it, on
// the console, and it goes no further; past the root, it is thrown, a
// failure of the runtime's.
export const custodian = () => {
  const unit = { $: "MkUnit", a: [] };
  const why = (e) => (e instanceof Error ? e.message : String(e));
  const ender = Symbol.for("heff:end");
  // (a thing ended, where it is not already: why it failed, or null)
  const ended = (t) => {
    if (!t || t.$heffEnded || typeof t[ender] !== "function") return null;
    t.$heffEnded = true;
    try { t[ender](); return null; } catch (e) { return why(e); }
  };
  // (what `c` and those under it were given, taken out, each shut, their
  // things ended: why the first that failed did, at `fail.why`)
  const taken = (c, out, fail) => {
    c.shut = true;
    for (const r of c.things) {
      const w = ended(r.deref());
      if (w !== null && fail && fail.why === null) fail.why = w;
    }
    for (const s of c.stops) out.push(s);
    c.stops = [];
    for (const k of c.children) taken(k, out, fail);
    return out;
  };
  // (each run in turn, then `done` given why those that failed did)
  const ranAll = (stops, i, whys, done) => {
    if (i >= stops.length) return done(whys);
    let on = false;
    const next = () => {
      if (on) return unit;
      on = true;
      return ranAll(stops, i + 1, whys, done);
    };
    try { return $tr($tr(stops[i](unit))((v) => next())); }
    catch (e) {
      if (on) throw e;
      whys.push($fromJs(why(e)));
      return next();
    }
  };
  // (a failure of `c`'s: `c` shut down, with those under it, what they
  // were given run, and the failure passed up, to where it stops)
  const failed = (c, w) => {
    for (;;) {
      const stops = taken(c, [], { why: null });
      ranAll(stops, 0, [], (whys) => unit);
      if (c.contains) {
        console.error("heff: " + w + " (its custodian shut down)");
        return;
      }
      if (!c.parent) throw new Error("heff: " + w);
      c = c.parent;
    }
  };
  // (a failure of the custodian's a `Maybe Custodian` names, or the root's)
  const failedIn = (at, w) => {
    if (at.$ === "Just") return failed(at.a[0], w);
    throw new Error("heff: " + w);
  };
  // (a release kept for a `with`: its cell, which the ticket holds and
  // the collector's callback is given, the ticket not; dropped by the
  // `with`, emptied.  A ticket the collector comes to with its cell not
  // emptied -- its `with` left by a throw -- has its release run then, a
  // turn of the event loop's of its own; one that fails there throws,
  // uncaught, a failure of the runtime's)
  const tickets = typeof FinalizationRegistry !== "undefined"
    ? new FinalizationRegistry((cell) => {
        if (!cell.release) return;
        const r = cell.release;
        cell.release = null;
        ranAll([r], 0, [], (whys) => {
          if (whys.length > 0) {
            const w = "a release, run after its `with` was left: " + $toJs(whys[0]);
            if (cell.owner) failed(cell.owner, w);
            else throw new Error("heff: " + w);
          }
          return unit;
        });
      })
    : null;
  const list = (xs) => {
    let l = { $: "Nil", a: [] };
    for (let i = xs.length - 1; i >= 0; i--) l = { $: "Cons", a: [xs[i], l] };
    return l;
  };
  return (p, k) => {
    // (an operation given no computation, taken out of its `Asked`)
    if (p.$ === "Asked") p = p.a[0];
    const a = p.a;
    switch (p.$) {
      case "Made": {
        const parent = a[0].$ === "Just" ? a[0].a[0] : null;
        const c = {
          children: [], stops: [], things: [], parent,
          contains: a[1].$ === "True", shut: parent ? parent.shut : false,
        };
        if (parent) parent.children.push(c);
        return k(a[2](c));
      }
      case "Managed": {
        const c = a[0];
        if (c.shut) return k(a[2]({ $: "False", a: [] }));
        c.stops.push(a[1]);
        return k(a[2]({ $: "True", a: [] }));
      }
      case "ShutDown": {
        const fail = { why: null };
        const stops = taken(a[0], [], fail);
        if (fail.why !== null) failed(a[0], fail.why);
        return ranAll(stops, 0, [], (whys) => k(a[1](list(whys))));
      }
      case "Own": {
        if (a[0].$ === "Just") {
          const c = a[0].a[0], t = $tr(a[1]((v) => v)).a[0];
          if (t !== null && typeof t === "object") {
            t.$heffOwner = c;
            c.things.push(typeof WeakRef !== "undefined" ? new WeakRef(t) : { deref: () => t });
            if (c.shut) {
              const w = ended(t);
              if (w !== null) failed(c, w);
            }
          }
        }
        return k(a[2]);
      }
      case "Failed": {
        failedIn(a[0], $toJs(a[1]));
        return k(a[2]);
      }
      case "Kept": {
        const cell = { release: a[1], owner: a[0].$ === "Just" ? a[0].a[0] : null };
        const t = { cell };
        if (tickets) tickets.register(t, cell);
        return k(a[2](t));
      }
      case "Dropped": { a[0].cell.release = null; return k(a[1]); }
      default: throw new Error("heff: not an operation of custodian: " + (p && p.$));
    }
  };
};
