// The page's side of lib/dom.hf: the world as an effect provider.
//
// A module M.hf whose effect the world handles has its provider beside
// it, M.provider.js: for each effect definition X of the module, one
// `export const X = () => (p, k) => ...` -- a factory, making a fresh
// algebra each time a program is run.  An operation arrives as `p`, a
// constructor `{ $: "Tag", a: [...] }` (a heff Int is a BigInt, a
// String a string, a constructor `{ $, a }` again); `k` is its
// continuation, called with the operation's answer -- which may be what
// a function of the operation's gave back, as it is: `k(a[1](x))`.  A
// computation an operation carries (`On`'s handler, here) has been
// handled already: it is `x => done => ...`, run by applying it and
// discarding `done`, each application through `$tr` (what a function of
// the program gives back may be a bounce, the rest of its work).
// An `import` must be of a `node:` builtin only, its names aliased
// `$name`: `heff compile` (page, node) writes one file, hoisting these
// lines to its top, and the factories into one scope.  A world that
// lives in one host only says so: `export const $host = "page"` (the
// page's, `--backend=page`) or `"node"` (Node.js's, `--backend=node`);
// without it, both.  (`$`-named exports are the provider speaking about
// itself, never factories.)
//
// (a `Node` is `MkNode i`: the element is nodes[i], numbered as it is
// made, or first found)
export const $host = "page";
export const dom = () => {
  const nodes = [];
  const node = (n) => nodes[Number(n.a[0])];
  const mk = (i) => ({ $: "MkNode", a: [BigInt(i)] });
  const found = (el) => { if (!el) return { $: "Nothing", a: [] }; let i = nodes.indexOf(el); if (i < 0) { nodes.push(el); i = nodes.length - 1; } return { $: "Just", a: [mk(i)] }; };
  const done = () => {};
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Create": nodes.push(document.createElement(a[0])); return k(a[1](mk(nodes.length - 1)));
      case "Text": node(a[0]).textContent = a[1]; return k(a[2]);
      case "Attr": node(a[0]).setAttribute(a[1], a[2]); return k(a[3]);
      case "Append": node(a[0]).appendChild(node(a[1])); return k(a[2]);
      case "Clear": node(a[0]).replaceChildren(); return k(a[1]);
      case "Value": return k(a[1](String(node(a[0]).value ?? "")));
      case "On": { const h = a[2]; node(a[0]).addEventListener(a[1], (ev) => $tr($tr(h(String(ev && ev.target && ev.target.value !== undefined ? ev.target.value : "")))(done))); return k(a[3]); }
      case "Select": return k(a[1](found(document.querySelector(a[0]))));
      case "XPath": return k(a[1](found(document.evaluate(a[0], document, null, 9, null).singleNodeValue)));
      case "Sleep": setTimeout(() => k(a[1]), Number(a[0])); return;
      case "Fetch": fetch(a[0]).then((r) => r.text(), (e) => String(e)).then((t) => k(a[1](t))); return;
      default: throw new Error("heff: not an operation of dom: " + (p && p.$));
    }
  };
};
