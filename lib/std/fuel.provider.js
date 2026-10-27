// Fuel's side of std.fuel, in JavaScript: `limited n c` sets a limit on
// the runtime's count (`$fuel`, and how many limits are set, `$fuelOn`),
// under what is left of the one around it, and runs `c`, computed here,
// a running out (`heff:fuel`, thrown by a tick) caught: `Nothing` where
// it was of the limit's own, else handed on, as a failure is.  What `c`
// answers later, after an operation that finished later, is given on
// as it comes, the limit set again for it -- the count is the one
// domain's.
export const fuel = () => (p, k) => {
  const unit = { $: "MkUnit", a: [] };
  switch (p.$) {
    case "Limited": {
      const t = $tr(p.a[0]((v) => v));
      const n = t.a[0] < 0n ? 0 : Number(t.a[0]), c = t.a[1], g = t.a[2];
      const outer = $fuelOn > 0 ? Math.max($fuel, 0) : Infinity;
      const bounded = n >= outer, given = bounded ? outer : n;
      $fuel = given;
      $fuelOn++;
      let open = true;
      const unlimit = (out) => {
        if (!open) return false;
        open = false;
        const left = Math.max($fuel, 0);
        $fuelOn--;
        $fuel = $fuelOn === 0 ? Infinity : out && bounded ? 0 : outer - (given - left);
        return out && !bounded;
      };
      try {
        const comp = $tr(c(unit));
        return $tr(comp((v) => { unlimit(false); return k(g({ $: "Just", a: [v] })); }));
      } catch (e) {
        const out = e instanceof Error && e.message === "heff:fuel";
        if (open && unlimit(out)) return k(g({ $: "Nothing", a: [] }));
        throw e;
      }
    }
    default: throw new Error("heff: not an operation of fuel: " + (p && p.$));
  }
};
