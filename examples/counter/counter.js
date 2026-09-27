// Generated from Hippogriff; do not edit.

const textIn = (parent, tag, str, hd, k) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Create", a: [tag, (n) => n] }] }))((x) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Text", a: [x, str, { $: "MkUnit", a: [] }] }] }))((x1) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Append", a: [parent, x, { $: "MkUnit", a: [] }] }] }))((x2) => k(x))));

const increment = (label, hd, k) => hd.bind(hd.malg({ $: "InR", a: [{ $: "Get", a: [(st) => st] }] }))((x) => {
  const st = x + 1n;
  return hd.bind(hd.malg({ $: "InR", a: [{ $: "Put", a: [st, { $: "MkUnit", a: [] }] }] }))((x1) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Text", a: [label, "count: " + (x + 1n).toString(), { $: "MkUnit", a: [] }] }] }))((x2) => k(x2)));
});

const counter = (hd, k) => hd.bind(hd.malg({ $: "InR", a: [{ $: "Put", a: [0n, { $: "MkUnit", a: [] }] }] }))((x) => hd.bind(hd.malg({ $: "InL", a: [{ $: "Root", a: [(n) => n] }] }))((x1) => textIn(x1, "p", "count: " + 0n.toString(), hd, (x2) => textIn(x1, "button", "+1", hd, (x3) => hd.bind(hd.malg({ $: "InL", a: [{ $: "On", a: [x3, "click", (v) => increment(x2, hd, hd.ret), { $: "MkUnit", a: [] }] }] }))((x4) => k(x4))))));
const counter$c = (hd) => (k) => counter(hd, k);

const $runDom = (main, rootEl) => {
  const nodes = [rootEl];
  let state;
  const node = (n) => nodes[Number(n)];
  const found = (el) => { if (!el) return { $: "Nothing", a: [] }; let i = nodes.indexOf(el); if (i < 0) { nodes.push(el); i = nodes.length - 1; } return { $: "Just", a: [BigInt(i)] }; };
  const done = () => {};
  const perform = (p0, k) => {
    // (the effects' sum: either's operation, whose names are distinct)
    const p = p0.$ === "InL" || p0.$ === "InR" ? p0.a[0] : p0;
    const a = p.a;
    switch (p.$) {
      case "Create": nodes.push(document.createElement(a[0])); return k(a[1](BigInt(nodes.length - 1)));
      case "Text": node(a[0]).textContent = a[1]; return k(a[2]);
      case "Attr": node(a[0]).setAttribute(a[1], a[2]); return k(a[3]);
      case "Append": node(a[0]).appendChild(node(a[1])); return k(a[2]);
      case "Clear": node(a[0]).replaceChildren(); return k(a[1]);
      case "Value": return k(a[1](String(node(a[0]).value ?? "")));
      case "On": { const h = a[2]; node(a[0]).addEventListener(a[1], (ev) => h(String(ev && ev.target && ev.target.value !== undefined ? ev.target.value : ""))(done)); return k(a[3]); }
      case "Root": return k(a[0](0n));
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

$runDom(counter$c, document.getElementById("hg-app") || document.body);
