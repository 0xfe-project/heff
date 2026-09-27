// Generated from Hippogriff; do not edit.

const textIn = (parent, tag, str, hd, k) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Create", a: [tag, (n) => n] }] }))((x) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Text", a: [x, str, { $: "MkUnit", a: [] }] }] }))((x1) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Append", a: [parent, x, { $: "MkUnit", a: [] }] }] }))((x2) => k(x))));

const tick = (label, n, hd, k) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Sleep", a: [100n, { $: "MkUnit", a: [] }] }] }))((x) => {
  const str = n.toString() + " ticks";
  return hd.bind(hd.malg({ $: "InL", a: [{ $: "Text", a: [label, str, { $: "MkUnit", a: [] }] }] }))((x1) => tick(label, n + 1n, hd, k));
});

const clock = (hd, k) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Root", a: [(n) => n] }] }))((x) => textIn(x, "p", "0 ticks", hd, (x1) => tick(x1, 1n, hd, k)));
const clock$c = (hd) => (k) => clock(hd, k);

const $runDom = (main, rootEl) => {
  const nodes = [rootEl];
  let state;
  // (a `Node` is `MkNode i`: the element is nodes[i])
  const node = (n) => nodes[Number(n.a[0])];
  const mk = (i) => ({ $: "MkNode", a: [BigInt(i)] });
  const found = (el) => { if (!el) return { $: "Nothing", a: [] }; let i = nodes.indexOf(el); if (i < 0) { nodes.push(el); i = nodes.length - 1; } return { $: "Just", a: [mk(i)] }; };
  const done = () => {};
  const perform = (p0, k) => {
    // (the effects' sum: either's operation, whose names are distinct)
    const p = p0.$ === "InL" || p0.$ === "InR" ? p0.a[0] : p0;
    const a = p.a;
    switch (p.$) {
      case "Create": nodes.push(document.createElement(a[0])); return k(a[1](mk(nodes.length - 1)));
      case "Text": node(a[0]).textContent = a[1]; return k(a[2]);
      case "Attr": node(a[0]).setAttribute(a[1], a[2]); return k(a[3]);
      case "Append": node(a[0]).appendChild(node(a[1])); return k(a[2]);
      case "Clear": node(a[0]).replaceChildren(); return k(a[1]);
      case "Value": return k(a[1](String(node(a[0]).value ?? "")));
      case "On": { const h = a[2]; node(a[0]).addEventListener(a[1], (ev) => h(String(ev && ev.target && ev.target.value !== undefined ? ev.target.value : ""))(done)); return k(a[3]); }
      case "Root": return k(a[0](mk(0)));
      case "Select": return k(a[1](found(document.querySelector(a[0]))));
      case "XPath": return k(a[1](found(document.evaluate(a[0], document, null, 9, null).singleNodeValue)));
      case "Get": return k(a[0](state));
      case "Put": state = a[0]; return k(a[1]);
      case "Sleep": setTimeout(() => k(a[1]), Number(a[0])); return;
      case "Fetch": fetch(a[0]).then((r) => r.text(), (e) => String(e)).then((t) => k(a[1](t))); return;
      default: throw new Error("heff: not an operation of the app's effect: " + (p && p.$));
    }
  };
  // the handler: its monad's return and bind, and its algebra
  const hd = {
    ret: (x) => (k) => k(x),
    bind: (m) => (f) => (k) => m((x) => f(x)(k)),
    malg: (p) => (k) => perform(p, k),
  };
  return main(hd)(hd.ret)(done);
};

$runDom(clock$c, document.getElementById("hg-app") || document.body);
