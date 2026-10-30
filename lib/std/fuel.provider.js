// Fuel's side of std.fuel, in JavaScript: `limited n c` opens a limit in
// the context of the computation that performed it (the runtime's
// `$fuelCaller`: its count and the limits open in it), under what is left
// of the one around it, and runs `c` there, computed here.  The limit is
// the computation's: an operation `c` performs goes on, at once or later,
// in that context (`$fuelResume`), so a tick that runs out -- here, after
// a sleep, or in a turn of the program's after another's (boc's) -- throws
// `heff:fuel` to the limits open there (`$fuelCaught`), the innermost
// first: `Nothing` where it was of this limit's own, else handed on, as
// a failure is.
export const fuel = () => (p, k) => {
  const unit = { $: "MkUnit", a: [] };
  switch (p.$) {
    case "Limited": {
      const t = $tr(p.a[0]((v) => v));
      const n = t.a[0] < 0n ? 0 : Number(t.a[0]), c = $carriedFn(t.a[1]), g = t.a[2];
      const caller = $fuelCaller, here = $fuelCtx;
      $fuelTo(caller);
      const outer = caller.stack.length > 0 ? Math.max($fuel, 0) : Infinity;
      const bounded = n >= outer, given = bounded ? outer : n;
      let open = true;
      // (the limit closed, in the caller's context: what it used taken
      // from the one around it; whether it ran out of its own)
      const unlimit = (out) => {
        if (!open) return false;
        open = false;
        const left = Math.max($fuel, 0);
        caller.stack.pop();
        $fuelOn = caller.stack.length;
        $fuel = $fuelOn === 0 ? Infinity : out && bounded ? 0 : outer - (given - left);
        return out && !bounded;
      };
      caller.stack.push((e) => {
        if (unlimit(e instanceof Error && e.message === "heff:fuel")) return k(g({ $: "Nothing", a: [] }));
        throw e;
      });
      $fuel = given;
      $fuelOn = caller.stack.length;
      try {
        const comp = $tr(c(unit));
        return $tr(comp((v) => { unlimit(false); return k(g({ $: "Just", a: [v] })); }));
      } catch (e) {
        return $fuelCaught(e);
      } finally {
        $fuelTo(here);
      }
    }
    default: throw new Error("heff: not an operation of fuel: " + (p && p.$));
  }
};
