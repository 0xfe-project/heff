// paint.hf's world: an effect provider.  The elements it paints
// browser.dom's provider found, and the document it makes its stamps
// with is the one browser.dom's handed out: each the host's own object,
// given as it is from one provider to the other.
export const $host = "page";
export const paint = () => (p, k) => {
  const a = p.a;
  switch (p.$) {
    case "Paint": a[0].setAttribute("class", $toJs(a[1])); return k(a[2]);
    case "Stamp": { const el = a[0].createElement("mark"); el.textContent = $toJs(a[1]); return k(a[2](el)); }
    default: throw new Error("heff: not an operation of paint: " + (p && p.$));
  }
};
