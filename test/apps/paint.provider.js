// test/apps/paint.hf's world: an effect provider (the shape of one is
// said in lib/browser/dom.provider.js).  The elements it paints browser.dom's
// provider found, and the stamps it makes browser.dom's puts on the page:
// each is the world's, held for both (lib/std/handle.hf), a handle
// `MkHandle i` the world's `i`th.
export const $host = "page";
export const paint = (world) => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Paint": world.get(a[0].a[0]).setAttribute("class", $toJs(a[1])); return k(a[2]);
    case "Stamp": { const el = document.createElement("mark"); el.textContent = $toJs(a[0]); return k(a[1]({ $: "MkHandle", a: [world.hold(el)] })); }
    default: throw new Error("heff: not an operation of paint: " + (p && p.$));
  }
};
