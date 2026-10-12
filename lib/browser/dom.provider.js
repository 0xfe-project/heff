// The page's side of lib/browser/dom.hf: the world as an effect provider.
//
// A module M.hf whose effect the world handles has its provider beside
// it, M.provider.js: for each effect definition X of the module, one
// `export const X = (world) => (p, k) => ...` -- a factory, making a
// fresh algebra each time a program is run, given the world: what holds
// the things every provider of the program hands out (std.handle's
// `Handle`, lib/std/handle.hf says how), so that one effect's provider
// takes what another's handed out.  An operation arrives as `p`, a
// constructor `{ $: "Tag", a: [...] }` (a heff Int is a BigInt, as an
// Int64 and a UInt64 are, any other number -- a Float64, a Float32, an
// Int8 to Int32, a UInt8 to UInt32 -- a number of its type's range; a
// Rune a number, its code point; a String a string -- see below -- a
// constructor `{ $, a }` again); `k` is its continuation, called with the
// operation's answer -- which may be what a function of the operation's
// gave back, as it is: `k(a[1](x))`.  A computation an operation carries
// (`On`'s handler, here) has been handled already: it is
// `x => done => ...`, run by applying it and
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
// A String crosses to the host and back only through the runtime's
// helpers, never as it is: it is bytes, kept as a JavaScript string
// each of whose code units is one of its bytes (UTF-8, `"é"` two), which
// no host API reads as its text.  Which one is by what the String is to
// the host:
//   `$toJs(s)`      text given to a host of text: the DOM, `console`,
//                   JSON, a command's name and arguments, the
//                   environment;
//   `$fromJs(t)`    text it gives: an element's value, an event's, the
//                   arguments of the program, the environment's, the
//                   working directory, an error's message -- and a
//                   string the provider writes itself (a literal that is
//                   not ASCII must be given through it);
//   `$toBytes(s)`   bytes given to the host, a Buffer (a Uint8Array on a
//                   page): what is written to a file or a stream, a path;
//   `$fromBytes(b)` bytes it gives (a Buffer, a Uint8Array or an
//                   ArrayBuffer): what is read from a file, a stream, a
//                   command, what is fetched, a directory's names.
// The runtime has each in the program a provider names it in.  A field
// of type `JSString` (browser.js) is a JavaScript string, given as it is.
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
      case "Create": return k(a[1](handle(document.createElement($toJs(a[0])))));
      case "Text": node(a[0]).textContent = $toJs(a[1]); return k(a[2]);
      case "Attr": node(a[0]).setAttribute($toJs(a[1]), $toJs(a[2])); return k(a[3]);
      case "Append": node(a[0]).appendChild(node(a[1])); return k(a[2]);
      case "Clear": node(a[0]).replaceChildren(); return k(a[1]);
      case "Value": return k(a[1]($fromJs(String(node(a[0]).value ?? ""))));
      case "On": { const h = a[2]; node(a[0]).addEventListener($toJs(a[1]), (ev) => $tr($tr(h($fromJs(String(ev && ev.target && ev.target.value !== undefined ? ev.target.value : ""))))(done))); return k(a[3]); }
      case "Select": return k(a[1](found(document.querySelector($toJs(a[0])))));
      case "XPath": return k(a[1](found(document.evaluate($toJs(a[0]), document, null, 9, null).singleNodeValue)));
      case "Sleep": setTimeout(() => k(a[1]), Number(a[0])); return;
      case "Fetch": fetch($toJs(a[0])).then((r) => r.arrayBuffer().then($fromBytes), (e) => $fromJs(String(e))).then((t) => k(a[1](t))); return;
      default: throw new Error("heff: not an operation of dom: " + (p && p.$));
    }
  };
};
