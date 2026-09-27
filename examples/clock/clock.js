// Generated from Hippogriff; do not edit.

const textIn = (parent, tag, str, hd, k) => hd.bind(hd.malg({ $: "Create", a: [tag, (n) => n] }))((x) => hd.bind(hd.malg({ $: "Text", a: [x, str, { $: "MkUnit", a: [] }] }))((x1) => hd.bind(hd.malg({ $: "Append", a: [parent, x, { $: "MkUnit", a: [] }] }))((x2) => k(x))));

const tick = (label, n, hd, k) => hd.bind(hd.malg({ $: "Sleep", a: [100n, { $: "MkUnit", a: [] }] }))((x) => {
  const str = n.toString() + " ticks";
  return hd.bind(hd.malg({ $: "Text", a: [label, str, { $: "MkUnit", a: [] }] }))((x1) => tick(label, n + 1n, hd, k));
});

const clock = (hd, k) => hd.bind(hd.malg({ $: "Root", a: [(n) => n] }))((x) => textIn(x, "p", "0 ticks", hd, (x1) => tick(x1, 1n, hd, k)));
const clock$c = (hd) => (k) => clock(hd, k);

const $runDom = (main, rootEl) => {
  const nodes = [rootEl];
  let state;
  const node = (n) => nodes[Number(n)];
  const found = (el) => { if (!el) return { $: "Nothing", a: [] }; let i = nodes.indexOf(el); if (i < 0) { nodes.push(el); i = nodes.length - 1; } return { $: "Just", a: [BigInt(i)] }; };
  const done = () => {};
  const perform = (p, k) => {
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
      default: throw new Error("heff: not a dom operation: " + (p && p.$));
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
