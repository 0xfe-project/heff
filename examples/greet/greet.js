// Generated from Hippogriff; do not edit.
const $T = { $: "True", a: [] }, $F = { $: "False", a: [] };
const $nomatch = (s) => { throw new Error("heff: no case for '" + (s && s.$) + "'"); };

const greet = (hd, k) => hd.bind(hd.malg({ $: "XPath", a: ["//p[@id='greeting']", (n) => n] }))((x) => hd.bind(hd.malg({ $: "Select", a: ["#name", (n) => n] }))((x1) => {
  switch (x.$) {
    case "Nothing": {
      return k({ $: "MkUnit", a: [] });
    }
    case "Just": {
      const n = x.a[0];
      return hd.bind(hd.malg({ $: "Text", a: [n, (() => {
        const s = "" === "" ? $T : $F;
        switch (s.$) {
          case "True": {
            return "Hello, whoever you are.";
          }
          case "False": {
            return "Hello, " + ("" + "!");
          }
        }
        return $nomatch(s);
      })(), { $: "MkUnit", a: [] }] }))((x2) => {
        switch (x1.$) {
          case "Nothing": {
            return k({ $: "MkUnit", a: [] });
          }
          case "Just": {
            const n1 = x1.a[0];
            return hd.bind(hd.malg({ $: "On", a: [n1, "input", (v) => {
              const k2 = hd.ret;
              return hd.bind(hd.malg({ $: "Text", a: [n, (() => {
                const s = v === "" ? $T : $F;
                switch (s.$) {
                  case "True": {
                    return "Hello, whoever you are.";
                  }
                  case "False": {
                    return "Hello, " + (v + "!");
                  }
                }
                return $nomatch(s);
              })(), { $: "MkUnit", a: [] }] }))((x3) => k2(x3));
            }, { $: "MkUnit", a: [] }] }))((x3) => k(x3));
          }
        }
        return $nomatch(x1);
      });
    }
  }
  return $nomatch(x);
}));
const greet$c = (hd) => (k) => greet(hd, k);

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

$runDom(greet$c, document.getElementById("hg-app") || document.body);
