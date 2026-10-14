// The world's scopes, the scope module's: an effect provider.  A token
// is `Held a`, `a` an object holding the scope's release (a computation
// of the world: a function of its continuation); `Leave` disarms it.
// Where the collector finds one armed -- its continuation dropped, by a
// handler that did not go on -- its release is run then, not at once as
// the C provider's is (a `FinalizationRegistry`; none where there is
// none).
export const scope = () => {
  const armory = typeof FinalizationRegistry === "function"
    ? new FinalizationRegistry((release) => release((_) => undefined))
    : null;
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Enter": {
        const armed = { release: a[0] };
        if (armory) armory.register(armed, a[0], armed);
        return k(a[1]({ $: "Held", a: [armed] }));
      }
      case "Leave": {
        const t = a[0];
        if (t.$ === "Held") {
          if (armory) armory.unregister(t.a[0]);
          t.a[0].release = null;
        }
        return k(a[1]);
      }
      default: throw new Error("heff: not an operation of scope: " + (p && p.$));
    }
  };
};
