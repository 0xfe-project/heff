// Custodians, in JavaScript: each an object of this provider's, which
// the program holds itself, let go of whenever the collector comes to
// it -- what it was given dropped then, not run.  A parent holds those
// made under it, so they live as long as it does.  What
// a custodian was given is run in the order given, one after another,
// each perhaps finishing later (an operation of another effect that
// answers later), the shutdown answered once all have.
export const custodian = () => {
  const unit = { $: "MkUnit", a: [] };
  const why = (e) => (e instanceof Error ? e.message : String(e));
  // (what `c` and those under it were given, taken out, each shut)
  const taken = (c, out) => {
    c.shut = true;
    for (const s of c.stops) out.push(s);
    c.stops = [];
    for (const k of c.children) taken(k, out);
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
          if (whys.length > 0) throw new Error("heff: a release, run after its `with` was left: " + $toJs(whys[0]));
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
        const c = { children: [], stops: [], shut: parent ? parent.shut : false };
        if (parent) parent.children.push(c);
        return k(a[1](c));
      }
      case "Managed": {
        const c = a[0];
        if (c.shut) return k(a[2]({ $: "False", a: [] }));
        c.stops.push(a[1]);
        return k(a[2]({ $: "True", a: [] }));
      }
      case "ShutDown": {
        const stops = taken(a[0], []);
        return ranAll(stops, 0, [], (whys) => k(a[1](list(whys))));
      }
      // (a thing that failed to end: the program's failure, thrown out)
      case "Failed": throw new Error("heff: " + $toJs(a[0]));
      case "Kept": {
        const cell = { release: a[0] };
        const t = { cell };
        if (tickets) tickets.register(t, cell);
        return k(a[1](t));
      }
      case "Dropped": { a[0].cell.release = null; return k(a[1]); }
      default: throw new Error("heff: not an operation of custodian: " + (p && p.$));
    }
  };
};
