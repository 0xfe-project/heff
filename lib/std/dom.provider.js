// The page's side of lib/std/dom.hf: the world as an effect provider.
//
// A module M.hf whose effect the world handles has its provider beside
// it, M.provider.js: for each effect definition X of the module, one
// `export const X = (world) => (p, k) => ...` -- a factory, making a
// fresh algebra each time a program is run, given the world: what holds
// the things every provider of the program hands out (std.handle's
// `Handle`, lib/std/handle.hf says how), so that one effect's provider
// takes what another's handed out.  An operation arrives as `p`, a
// constructor `{ $: "Tag", a: [...] }` (a heff Int is a BigInt, as an
// Int64 and a UInt64 are, any other number -- a Float, a Float32, an
// Int8 to Int32, a UInt8 to UInt32 -- a number of its type's range; a
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
// (a `Node` is a handle, `MkHandle i`: the element is the world's `i`th,
// numbered as it is made, or first found -- by this provider or another)
export const $host = "page";
export const dom = (world) => {
  const node = (h) => world.get(h.a[0]);
  const handle = (el) => ({ $: "MkHandle", a: [world.hold(el)] });
  const found = (el) => el ? { $: "Just", a: [handle(el)] } : { $: "Nothing", a: [] };
  const done = () => {};
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Create": return k(a[1](handle(document.createElement(a[0]))));
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
