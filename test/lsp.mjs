#!/usr/bin/env node
// The language server (`heff lsp`), spoken to as an editor would: diagnostics
// (of the editor's text, and of an import's unsaved text, of a file as deep
// as the command line goes), files changed on disk, hover and go to
// definition (across modules, past non-ASCII text), completion, an outline,
// references and renaming across the project, doc tests (their errors,
// shown in a hover), splitting a variable into cases, formatting, shutdown.
import { spawn } from "node:child_process";
import { readFileSync, unlinkSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { heffBin, root } from "./heff.mjs";
import { slot } from "./pool.mjs";

const uri = (f) => pathToFileURL(join(root, f)).href;
const disk = (f) => readFileSync(join(root, f), "utf8");

// (as vscode-languageclient starts it, with `--stdio`)
// (with a bounded heap: what it keeps must fit, on any machine; told it
// is on a terminal, which it says nothing to in colour all the same)
// (a slot of the tests' budget taken for it, as the pool takes one
// for a process run, and held while it runs -- idle mostly, but deep in
// elaborating at times -- its waits counted only once it has one)
await slot();
const server = spawn(heffBin, ["lsp", "--stdio"], { cwd: root, stdio: ["pipe", "pipe", "inherit"], env: { ...process.env, HEFF_HEAP_MB: "1024", HEFF_TTY: "1" } });
let buf = Buffer.alloc(0), nextId = 1, failed = 0, checks = 0;
const waiting = new Map(), notes = [], noteWaiters = [];
server.stdout.on("data", (chunk) => {
  buf = Buffer.concat([buf, chunk]);
  for (;;) {
    const sep = buf.indexOf("\r\n\r\n");
    if (sep < 0) return;
    const len = Number(/Content-Length: (\d+)/.exec(buf.subarray(0, sep).toString())[1]);
    if (buf.length < sep + 4 + len) return;
    const msg = JSON.parse(buf.subarray(sep + 4, sep + 4 + len).toString("utf8"));
    buf = buf.subarray(sep + 4 + len);
    if (msg.id !== undefined && waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
    else { notes.push(msg); noteWaiters.splice(0).forEach((w) => w()); }
  }
});
const write = (msg) => {
  const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...msg }), "utf8");
  server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  server.stdin.write(body);
};
const request = (method, params) => new Promise((ok) => { const id = nextId++; waiting.set(id, ok); write({ id, method, params }); });
const notify = (method, params) => write({ method, params });
// the next notification that satisfies p (taken off the queue)
const note = (p, what) => new Promise((ok, no) => {
  const timer = setTimeout(() => no(new Error("timed out waiting for " + what)), 10000);
  const look = () => {
    const i = notes.findIndex(p);
    if (i >= 0) { clearTimeout(timer); ok(notes.splice(i, 1)[0]); } else noteWaiters.push(look);
  };
  look();
});
const diagnosticsOf = (u) => note((m) => m.method === "textDocument/publishDiagnostics" && m.params.uri === u, "diagnostics of " + u).then((m) => m.params.diagnostics);
const expect = (what, ok, got) => { checks++; if (!ok) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}`); } };
// the position of the n-th occurrence of s in text
const pos = (text, s, n = 0) => {
  let i = -1;
  for (let k = 0; k <= n; k++) i = text.indexOf(s, i + 1);
  const before = text.slice(0, i).split("\n");
  return { line: before.length - 1, character: before[before.length - 1].length };
};
const open = (f, text) => notify("textDocument/didOpen", { textDocument: { uri: uri(f), languageId: "heff", version: 1, text } });

let ds;
try {
  const init = await request("initialize", { processId: process.pid, rootUri: pathToFileURL(root).href, capabilities: { workspace: { didChangeWatchedFiles: { dynamicRegistration: true } } } });
  const caps = init.result.capabilities;
  expect("capabilities", caps.hoverProvider && caps.definitionProvider && caps.documentFormattingProvider && caps.textDocumentSync.change === 1 &&
    caps.referencesProvider && caps.renameProvider.prepareProvider && caps.documentSymbolProvider && caps.workspaceSymbolProvider &&
    caps.codeActionProvider.codeActionKinds.includes("refactor.rewrite"), caps);
  notify("initialized", {});
  // (asked to be told of the files changed on disk)
  const registration = await note((m) => m.method === "client/registerCapability", "client/registerCapability");
  expect("watching files", registration.params.registrations[0].method === "workspace/didChangeWatchedFiles" && registration.params.registrations[0].registerOptions.watchers[0].globPattern === "**/*.hf", registration);
  write({ id: registration.id, result: null });

  // a file as deep as the elaborator must go deep for: the server's stack is
  // the command line's
  const deep = "xs : List Int\nxs = [" + Array.from({ length: 20000 }, (_, i) => i).join(", ") + "]\n";
  open("test/surface/lsp-deep.hf", deep);
  ds = await diagnosticsOf(uri("test/surface/lsp-deep.hf"));
  expect("a deep file", ds.length === 0, ds);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-deep.hf") } });

  // a position is UTF-16 code units into its line: a rune beyond U+FFFF
  // two of them, before a span and in it; a character no token starts
  // with whole, its message text
  const astral = 's : Int\ns = {- 😀 -} "😀"\n\nt : Int\nt = 1 😀 2\n\nu : Int\nu = {- 😀 -} t\n';
  open("test/surface/lsp-astral.hf", astral);
  ds = await diagnosticsOf(uri("test/surface/lsp-astral.hf"));
  const strAt = pos(astral, '"😀"'), runeAt = pos(astral, "😀", 2);
  const e311 = ds.find((d) => d.code === "E0311"), e150 = ds.find((d) => d.code === "E0150");
  expect("a diagnostic past a rune beyond U+FFFF", e311 && e311.range.start.line === strAt.line && e311.range.start.character === strAt.character &&
    e311.range.end.character === strAt.character + 4, ds);
  expect("a character no token starts with, beyond U+FFFF", e150 && e150.message.includes("unexpected character 😀") &&
    e150.range.start.line === runeAt.line && e150.range.start.character === runeAt.character && e150.range.end.character === runeAt.character + 2, ds);
  const tAt = pos(astral, "-} t");
  const tHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-astral.hf") }, position: { line: tAt.line, character: tAt.character + 3 } });
  expect("hover past a rune beyond U+FFFF", tHover.result && tHover.result.contents.value.includes("t : Int"), tHover.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-astral.hf") } });

  // a name defined twice: at the second, saying where the first is
  const twice = "one : Int\none = 1\n\none : Int\none = 2\n";
  open("test/surface/lsp-twice.hf", twice);
  ds = await diagnosticsOf(uri("test/surface/lsp-twice.hf"));
  const againAt = pos(twice, "one :", 1);
  expect("a name defined twice", ds.length === 1 && ds[0].code === "E0153" && ds[0].severity === 1 &&
    ds[0].range.start.line === againAt.line && ds[0].range.start.character === 0 && ds[0].range.end.character === 3 &&
    ds[0].message.includes("`one` is defined twice") && ds[0].message.includes("lsp-twice.hf:1:1"), ds);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-twice.hf") } });

  // a type error, in the editor's text (not the file's)
  const bad = 'import a\n\ntwice : Bool -> Bool\ntwice b = inv (inv "x")\n';
  open("test/modules/src/b.hf", bad);
  ds = await diagnosticsOf(uri("test/modules/src/b.hf"));
  const at = pos(bad, '"x"');
  expect("a type error's diagnostic", ds.length === 1 && ds[0].code === "E0311" && ds[0].severity === 1 &&
    ds[0].range.start.line === at.line && ds[0].range.start.character === at.character && ds[0].range.end.character === at.character + 3 &&
    ds[0].message.includes("expected type Bool, but got type String") && !ds[0].message.includes("\u001b"), ds);

  // fixed, with a change (full sync, debounced), and non-ASCII before a name
  const good = "import a\n\ntwice : Bool -> Bool\ntwice b = {- 😀 λ -} inv (inv b)\n";
  notify("textDocument/didChange", { textDocument: { uri: uri("test/modules/src/b.hf"), version: 2 }, contentChanges: [{ text: good }] });
  ds = await diagnosticsOf(uri("test/modules/src/b.hf"));
  expect("no diagnostics once fixed", ds.length === 0, ds);

  const hover = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "inv", 1) });
  expect("hover past non-ASCII text", hover.result && hover.result.contents.value.includes("inv : Bool -> Bool") && hover.result.contents.value.includes("(global)"), hover.result);
  const local = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "b)") });
  expect("hover on a local", local.result && local.result.contents.value.includes("b : Bool") && local.result.contents.value.includes("(local)"), local.result);
  const nothing = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: { line: 1, character: 0 } });
  expect("hover on nothing", nothing.result === null, nothing.result);

  // a global of an imported module, not open: its file on disk
  const def = await request("textDocument/definition", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "inv", 1) });
  const notAt = pos(disk("test/modules/src/a.hf"), "inv");
  expect("definition in another module", def.result && def.result.uri === uri("test/modules/src/a.hf") &&
    def.result.range.start.line === notAt.line && def.result.range.start.character === notAt.character && def.result.range.end.character === notAt.character + 3, def.result);

  // a global of a module imported as `A`: `A.inv`
  const qualified = disk("test/modules/qualified.hf");
  open("test/modules/qualified.hf", qualified);
  await diagnosticsOf(uri("test/modules/qualified.hf"));
  const qDef = await request("textDocument/definition", { textDocument: { uri: uri("test/modules/qualified.hf") }, position: pos(qualified, "inv") });
  expect("definition of a qualified name", qDef.result && qDef.result.uri === uri("test/modules/src/a.hf") && qDef.result.range.start.line === notAt.line, qDef.result);
  const qHover = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/qualified.hf") }, position: pos(qualified, "inv") });
  expect("hover on a qualified name", qHover.result && qHover.result.contents.value.includes("A.inv : A.Bool -> A.Bool"), qHover.result);

  // completion: the names in scope that start as the one being typed; after
  // `A.`, the definitions of the module imported as `A`
  const typing = "x : Float64\nx = toFlo";
  open("test/surface/lsp-complete.hf", typing);
  await diagnosticsOf(uri("test/surface/lsp-complete.hf"));
  const comp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-complete.hf") }, position: { line: 1, character: 9 } });
  const lengthItem = comp.result && comp.result.find((i) => i.label === "toFloat");
  expect("completion of a prelude name", lengthItem && lengthItem.detail.includes("Int -> Float64") && !comp.result.some((i) => i.label === "toString"), comp.result && comp.result.map((i) => i.label));
  // `abandon`, a hole while writing a definition (#95)
  const typingHole = "x : Int\nx = aban";
  open("test/surface/lsp-complete-hole.hf", typingHole);
  await diagnosticsOf(uri("test/surface/lsp-complete-hole.hf"));
  const hComp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-complete-hole.hf") }, position: { line: 1, character: 8 } });
  expect("completion of abandon", hComp.result && hComp.result.some((i) => i.label === "abandon"), hComp.result && hComp.result.map((i) => i.label));
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-complete-hole.hf") } });
  // a primitive the library declares (named by no alias: its key is its name)
  const typingPrim = "x : Int\nx = intAd";
  open("test/surface/lsp-complete-prim.hf", typingPrim);
  await diagnosticsOf(uri("test/surface/lsp-complete-prim.hf"));
  const pComp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-complete-prim.hf") }, position: { line: 1, character: 9 } });
  expect("completion of a primitive", pComp.result && pComp.result.some((i) => i.label === "intAdd" && i.detail === "Int -> Int -> Int"), pComp.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-complete-prim.hf") } });
  // the prelude, which no file writes an import of: its names in scope,
  // completed and hovered
  const unwritten = "b : Bool\nb = not True\n\nn : Float64\nn = toFlo";
  open("test/surface/lsp-unwritten.hf", unwritten);
  await diagnosticsOf(uri("test/surface/lsp-unwritten.hf"));
  const uHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-unwritten.hf") }, position: pos(unwritten, "not") });
  expect("hover on a prelude name, not imported", uHover.result && uHover.result.contents.value.includes("not : Bool -> Bool") && uHover.result.contents.value.includes("(global)"), uHover.result);
  const uComp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-unwritten.hf") }, position: { line: 4, character: 9 } });
  expect("completion of a prelude name, not imported", uComp.result && uComp.result.some((i) => i.label === "toFloat" && i.detail.includes("Int -> Float64")), uComp.result && uComp.result.map((i) => i.label));
  const uDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-unwritten.hf") }, position: pos(unwritten, "not") });
  expect("definition of a prelude name, not imported", uDef.result && uDef.result.uri === uri("lib/prelude.hf") && uDef.result.range.start.line === pos(disk("lib/prelude.hf"), "not :").line, uDef.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-unwritten.hf") } });
  // the operations of lists, by the name `List`, written by no import:
  // completed after `List.`, hovered, gone to
  const listed = "xs : List Int\nxs = List.map (\\x -> x + 1) [1]\n\nn : Int\nn = List.len";
  open("test/surface/lsp-list.hf", listed);
  await diagnosticsOf(uri("test/surface/lsp-list.hf"));
  const lComp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-list.hf") }, position: { line: 4, character: 12 } });
  expect("completion after `List.`", lComp.result && lComp.result.some((i) => i.label === "length" && i.detail.includes("List a -> Int")) && lComp.result.every((i) => !i.label.includes(".")), lComp.result && lComp.result.map((i) => i.label));
  const lHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-list.hf") }, position: pos(listed, "map") });
  expect("hover on `List.map`", lHover.result && lHover.result.contents.value.includes("map : {a b : Type} -> (a -> b) -> List a -> List b"), lHover.result);
  const lDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-list.hf") }, position: pos(listed, "map") });
  expect("definition of `List.map`", lDef.result && lDef.result.uri === uri("lib/list.hf") && lDef.result.range.start.line === pos(disk("lib/list.hf"), "map :").line, lDef.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-list.hf") } });
  // a module imported under a built-in type's name: `Rune.toInt` the
  // module's, hovered and gone to
  const runed = "import rune as Rune\n\nx : Int\nx = Rune.toInt (Rune.fromInt 65)\n";
  open("test/surface/lsp-rune.hf", runed);
  const rDiags = await diagnosticsOf(uri("test/surface/lsp-rune.hf"));
  expect("an alias over a type checks", rDiags.length === 0, rDiags);
  const rHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-rune.hf") }, position: pos(runed, "toInt") });
  expect("hover on `Rune.toInt`", rHover.result && rHover.result.contents.value.includes("toInt : Rune -> Int"), rHover.result);
  const rDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-rune.hf") }, position: pos(runed, "toInt") });
  expect("definition of `Rune.toInt`", rDef.result && rDef.result.uri === uri("lib/rune.hf") && rDef.result.range.start.line === pos(disk("lib/rune.hf"), "toInt :").line, rDef.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-rune.hf") } });
  // an instance, `instance x : T`: found where an instance argument is
  // wanted; in the outline from `instance`, hovered and gone to as any
  // definition is
  const inst = "theory Eq (a : Type) where\n  equal : a -> a -> Bool\n\ninstance eqInt : Eq Int\neqInt = { equal = intEq }\n\nmember : {a : Type} -> {{eq : Eq a}} -> a -> List a -> Bool\nmember x Nil = False\nmember x (Cons y ys) = eq.equal x y || member x ys\n\nfound : Bool\nfound = member 1 [1] && member (eq := eqInt) 2 [2]\n";
  open("test/surface/lsp-instance.hf", inst);
  const iDiags = await diagnosticsOf(uri("test/surface/lsp-instance.hf"));
  expect("an instance found", iDiags.length === 0, iDiags);
  const iOutline = await request("textDocument/documentSymbol", { textDocument: { uri: uri("test/surface/lsp-instance.hf") } });
  const iSym = iOutline.result && iOutline.result.find((y) => y.name === "eqInt");
  const iAt = pos(inst, "eqInt :");
  expect("an instance in the outline", iSym && iSym.kind === 12 && iSym.range.start.line === iAt.line && iSym.range.start.character === 0 &&
    iSym.range.end.line === pos(inst, "eqInt =").line && iSym.selectionRange.start.line === iAt.line && iSym.selectionRange.start.character === iAt.character, iSym);
  const iHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-instance.hf") }, position: pos(inst, "eqInt)") });
  expect("hover on an instance", iHover.result && iHover.result.contents.value.includes("eqInt : Eq Int") && iHover.result.contents.value.includes("(global)"), iHover.result);
  const iDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-instance.hf") }, position: pos(inst, "eqInt)") });
  expect("definition of an instance", iDef.result && iDef.result.uri === uri("test/surface/lsp-instance.hf") && iDef.result.range.start.line === iAt.line && iDef.result.range.start.character === iAt.character, iDef.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-instance.hf") } });
  const dotted = "import a as A\n\nx : A.Bool\nx = A.";
  open("test/modules/lsp-dot.hf", dotted);
  await diagnosticsOf(uri("test/modules/lsp-dot.hf"));
  const dot = await request("textDocument/completion", { textDocument: { uri: uri("test/modules/lsp-dot.hf") }, position: { line: 3, character: 6 } });
  expect("completion after `A.`", dot.result && dot.result.some((i) => i.label === "inv") && dot.result.every((i) => !i.label.includes(".")), dot.result && dot.result.map((i) => i.label));

  // a global of the file itself, and the standard library's
  const basics = disk("test/surface/basics.hf");
  open("test/surface/basics.hf", basics);
  ds = await diagnosticsOf(uri("test/surface/basics.hf"));
  expect("basics.hf elaborates", ds.length === 0, ds);
  const own = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "not (not b)") });
  const ownAt = pos(basics, "not :");
  expect("definition in the same file", own.result && own.result.uri === uri("test/surface/basics.hf") && own.result.range.start.line === ownAt.line && own.result.range.start.character === 0, own.result);
  const loc = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "ys)") });
  expect("no definition for a local", loc.result === null, loc.result);

  const withLib = "x : Bool\nx = True\n";
  open("test/surface/lsp-lib.hf", withLib);
  ds = await diagnosticsOf(uri("test/surface/lsp-lib.hf"));
  expect("an unsaved file importing the library", ds.length === 0, ds);
  const libDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-lib.hf") }, position: pos(withLib, "Bool") });
  const boolAt = pos(disk("lib/builtin.hf"), "data Bool");
  expect("definition in lib/", libDef.result && libDef.result.uri === uri("lib/builtin.hf") &&
    libDef.result.range.start.line === boolAt.line && libDef.result.range.start.character === boolAt.character + 5, libDef.result);

  // an import's unsaved text is what the importer sees
  open("test/modules/src/a.hf", "data Bool = True | False\n");
  ds = await diagnosticsOf(uri("test/modules/src/a.hf"));
  const dependent = await diagnosticsOf(uri("test/modules/src/b.hf"));
  expect("an import's unsaved text", dependent.some((d) => d.message.includes("no such variable inv")), dependent);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/modules/src/a.hf") } });

  // in a `do` block, the name hovered, not the `bind` it is read into
  const counter = disk("examples/counter/counter.hf");
  open("examples/counter/counter.hf", counter);
  await diagnosticsOf(uri("examples/counter/counter.hf"));
  const inDo = await request("textDocument/hover", { textDocument: { uri: uri("examples/counter/counter.hf") }, position: pos(counter, "showCount 0") });
  expect("hover in a do block", inDo.result && inDo.result.contents.value.includes("showCount : Int -> String"), inDo.result);
  const defInDo = await request("textDocument/definition", { textDocument: { uri: uri("examples/counter/counter.hf") }, position: pos(counter, "showCount 0") });
  expect("definition in a do block", defInDo.result && defInDo.result.range.start.line === pos(counter, "showCount :").line, defInDo.result);

  // a name bound in a `do` block: nothing is recorded of it, and `bind` is not there
  const onBinder = await request("textDocument/hover", { textDocument: { uri: uri("examples/counter/counter.hf") }, position: pos(counter, "label <-") });
  expect("hover on a do binder is not bind's", onBinder.result === null || !onBinder.result.contents.value.includes("bind :"), onBinder.result);

  // a constructor, in an expression and in a pattern: where its type declares it
  const consAt = pos(basics, "Cons a (List a)");
  for (const [what, at] of [["in an expression", pos(basics, "Cons (f y)")], ["in a pattern", pos(basics, "Cons y ys) = Cons")]]) {
    const d = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hf") }, position: at });
    expect("definition of a constructor " + what, d.result && d.result.range.start.line === consAt.line && d.result.range.start.character === consAt.character, d.result);
  }

  // an operator: `+` of the prelude
  const withOps = "x : Int\nx = 1 + 2 * 3\n";
  open("test/surface/lsp-ops.hf", withOps);
  ds = await diagnosticsOf(uri("test/surface/lsp-ops.hf"));
  expect("operators of the prelude", ds.length === 0, ds);
  const plusHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-ops.hf") }, position: pos(withOps, "+") });
  expect("hover on an operator", plusHover.result && plusHover.result.contents.value.includes("(+) : {a : Type} -> {{add : Addition a}} -> a -> a -> a"), plusHover.result);
  const plusDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-ops.hf") }, position: pos(withOps, "+") });
  const plusAt = pos(disk("lib/prelude.hf"), "(+) :");
  expect("definition of an operator", plusDef.result && plusDef.result.uri === uri("lib/prelude.hf") && plusDef.result.range.start.line === plusAt.line, plusDef.result);

  // a field, set in a record and projected: where its theory declares it
  const co = disk("lib/co.hf");
  open("lib/co.hf", co);
  await diagnosticsOf(uri("lib/co.hf"));
  const hmapAt = pos(co, "hmap :");
  for (const [what, at] of [["set in a record", pos(co, "hmap = ")], ["projected", pos(co, "hmap m n nt x")]]) {
    const d = await request("textDocument/definition", { textDocument: { uri: uri("lib/co.hf") }, position: at });
    expect("definition of a field " + what, d.result && d.result.range.start.line === hmapAt.line && d.result.range.start.character === hmapAt.character, d.result);
  }

  // a record type's defaults: a name in a default, a field set where the
  // others are left out, and a theory's field that has a default
  const defaults = disk("test/surface/record-defaults.hf");
  const du = uri("test/surface/record-defaults.hf");
  open("test/surface/record-defaults.hf", defaults);
  ds = await diagnosticsOf(du);
  expect("a record type's defaults: no diagnostics", ds.length === 0, ds);
  const inDefault = await request("textDocument/hover", { textDocument: { uri: du }, position: pos(defaults, "greeting\n") });
  expect("hover in a default", inDefault.result && inDefault.result.contents.value.includes("greeting : String"), inDefault.result);
  const setField = await request("textDocument/hover", { textDocument: { uri: du }, position: pos(defaults, "name = \"bare\"") });
  expect("hover on a field set, the others left out", setField.result && setField.result.contents.value.includes("name : String"), setField.result);
  const nameDef = await request("textDocument/definition", { textDocument: { uri: du }, position: pos(defaults, "name = \"strings\"") });
  const nameAt = pos(defaults, "name : String = ");
  expect("definition of a theory's field that has a default", nameDef.result && nameDef.result.range.start.line === nameAt.line && nameDef.result.range.start.character === nameAt.character, nameDef.result);
  // (an implicit argument's default: the definition it names)
  const implicits = disk("test/surface/implicit-defaults.hf");
  const iu = uri("test/surface/implicit-defaults.hf");
  open("test/surface/implicit-defaults.hf", implicits);
  ds = await diagnosticsOf(iu);
  expect("an implicit argument's default: no diagnostics", ds.length === 0, ds);
  const inImplicit = await request("textDocument/hover", { textDocument: { uri: iu }, position: pos(implicits, "quietly}") });
  expect("hover in an implicit argument's default", inImplicit.result && inImplicit.result.contents.value.includes("quietly : Opts"), inImplicit.result);
  const runHover = await request("textDocument/hover", { textDocument: { uri: iu }, position: pos(implicits, "run 1") });
  expect("hover on a function with an implicit argument's default", runHover.result && runHover.result.contents.value.includes("{opts : Opts := quietly} -> Int -> Bool"), runHover.result);
  // (a default naming a field before it: that field, a local there)
  const inLo = await request("textDocument/hover", { textDocument: { uri: du }, position: pos(defaults, "lo\n  , width") });
  expect("hover on a field a default names", inLo.result && inLo.result.contents.value.includes("lo : Int") && inLo.result.contents.value.includes("(local)"), inLo.result);

  // a record updated, `{ r | x = v }`: a field it sets, and a field of
  // one, `pos.py`, as fields set are; a field's references and rename
  // take in where an update sets it
  const upd = disk("test/surface/record-update.hf");
  const uu = uri("test/surface/record-update.hf");
  open("test/surface/record-update.hf", upd);
  ds = await diagnosticsOf(uu);
  expect("an update: no diagnostics", ds.length === 0, ds);
  for (const [what, at, says] of [["a field set", pos(upd, "x = b.x"), "x : Float64"], ["a field whose field is set", pos(upd, "pos.py"), "pos : Pos"], ["a field of a field", pos(upd, "py = 20"), "py : Int"]]) {
    const h = await request("textDocument/hover", { textDocument: { uri: uu }, position: at });
    expect("hover on " + what + " in an update", h.result && h.result.contents.value.includes(says), h.result);
  }
  const pyRefs = await request("textDocument/references", { textDocument: { uri: uu }, position: pos(upd, "py = 2 }"), context: { includeDeclaration: true } });
  const pyLines = pyRefs.result ? pyRefs.result.map((r) => r.range.start.line) : [];
  expect("references of a field take in an update's", pyLines.includes(pos(upd, "pos.py = 20").line), pyRefs.result);
  const unitRen = await request("textDocument/rename", { textDocument: { uri: uu }, position: pos(upd, "unit : t"), newName: "one" });
  const unitEdits = unitRen.result && unitRen.result.changes && unitRen.result.changes[uu];
  expect("a rename of a field takes in an update's", unitEdits && unitEdits.some((e) => e.range.start.line === pos(upd, "unit = 1").line && e.newText === "one"), unitRen);
  notify("textDocument/didClose", { textDocument: { uri: uu } });

  // an outline: each definition, a data's tags and a theory's fields under it
  const outline = await request("textDocument/documentSymbol", { textDocument: { uri: uri("test/surface/basics.hf") } });
  const sym = (n) => outline.result && outline.result.find((s) => s.name === n);
  expect("an outline", sym("Bool") && sym("Bool").kind === 10 && sym("Bool").children.map((c) => c.name).join() === "True,False" &&
    sym("Monoid") && sym("Monoid").kind === 11 && sym("Monoid").children.map((c) => c.name).join() === "t,m" &&
    sym("not") && sym("not").kind === 12 && sym("not").selectionRange.start.line === pos(basics, "not :").line, outline.result);
  // (a definition is its signature and its equations; a `data`, to its last `)`)
  const listEnd = pos(basics, "Cons a (List a)");
  expect("an outline's ranges", sym("not").range.end.line === pos(basics, "not False").line &&
    sym("List").range.end.line === listEnd.line && sym("List").range.end.character === listEnd.character + "Cons a (List a)".length, outline.result && [sym("not"), sym("List")]);
  // what does not elaborate is in the outline too, where it is
  const recover = disk("test/errors/recover.hf");
  open("test/errors/recover.hf", recover);
  await diagnosticsOf(uri("test/errors/recover.hf"));
  const rOutline = await request("textDocument/documentSymbol", { textDocument: { uri: uri("test/errors/recover.hf") } });
  expect("an outline of what does not elaborate", rOutline.result && ["Bool", "f", "g", "h", "k", "bad", "m", "not", "n"].join() === rOutline.result.map((y) => y.name).join(), rOutline.result && rOutline.result.map((y) => y.name));
  notify("textDocument/didClose", { textDocument: { uri: uri("test/errors/recover.hf") } });

  // (a document opened or closed has its project elaborated anew when it
  // is next asked of -- the repository's takes seconds -- so the documents
  // asked of are opened first, and what is asked of each project is asked
  // together: the repository's, then test/modules')
  const whereSrc = disk("test/surface/where.hf");
  const amb = disk("test/surface/amb.hf");
  const ann = disk("test/surface/binder-ann.hf");
  for (const [f, text] of [["test/surface/where.hf", whereSrc], ["test/surface/amb.hf", amb], ["test/surface/binder-ann.hf", ann]]) {
    open(f, text);
    await diagnosticsOf(uri(f));
  }
  const wsym = await request("workspace/symbol", { query: "twic" });
  expect("the project's symbols", wsym.result && wsym.result.some((s) => s.name === "twice" && s.location.uri === uri("test/surface/basics.hf")), wsym.result);

  // a recursive call is the global it calls
  const recDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "map f ys") });
  expect("definition of a recursive call", recDef.result && recDef.result.range.start.line === pos(basics, "map :").line, recDef.result);

  // references: the signature, each equation, each use, a watch
  const refs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "not (not b)"), context: { includeDeclaration: true } });
  const notLines = refs.result && refs.result.filter((r) => r.uri === uri("test/surface/basics.hf")).map((r) => r.range.start.line);
  expect("references in a file", notLines && [pos(basics, "not :"), pos(basics, "not True"), pos(basics, "not False"), pos(basics, "not (not"), pos(basics, ">>> not")].every((p) => notLines.includes(p.line)) && notLines.length === 6, refs.result);
  const usesOnly = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "not (not b)"), context: { includeDeclaration: false } });
  expect("references without the definition", usesOnly.result && usesOnly.result.length === 3, usesOnly.result);
  // a watch's expectation: a name written there is a reference, but not a
  // string's text, nor a record's label
  const expecting = 'name : String\nname = "name"\n\ntype R = { name : String, n : Int }\n\ny : R\ny = { name = name, n = 1 }\n\n-- >>> y\n-- { name = "name", n = 1 }\n\n-- >>> y.name\n-- name\n';
  open("test/surface/lsp-expect.hf", expecting);
  await diagnosticsOf(uri("test/surface/lsp-expect.hf"));
  const eRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/lsp-expect.hf") }, position: { line: 0, character: 0 }, context: { includeDeclaration: true } });
  expect("references in a watch's expectation", eRefs.result && eRefs.result.map((r) => r.range.start.line).join() === [0, 1, 6, 12].join(), eRefs.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-expect.hf") } });

  // doc tests: checked after their definition, an error on the doc test's
  // line (a name defined below), a warning on one of no definition's;
  // shown in the definition's hover; a name in an expectation a reference
  // as in a watch's
  const documented = [
    'name : String', 'name = "name"', '', 'type R = { name : String, n : Int }', '',
    '-- | a record of a name', '-- | >>> y', '-- | { name = "name", n = 1 }', '-- | >>> y.name', '-- | name',
    'y : R', 'y = { name = name, n = 1 }', '',
    '-- | >>> twice y.name', '-- | "namename"', '-- | >>> twice x',
    'twice : String -> String', 'twice s = s ^ s', '',
    '-- | >>> twice "stray"', '',
    'x : String', 'x = "x"', '',
  ].join("\n");
  const docU = uri("test/surface/lsp-doctest.hf");
  open("test/surface/lsp-doctest.hf", documented);
  ds = await diagnosticsOf(docU);
  const byLine = [...ds].sort((a, b) => a.range.start.line - b.range.start.line);
  expect("diagnostics on doc tests' lines", byLine.length === 2 && byLine[0].range.start.line === 15 && byLine[0].message.includes("x") &&
    byLine[0].severity === 1 && byLine[1].range.start.line === 19 && byLine[1].message.includes("doc test") &&
    byLine[1].severity === 2 && byLine[1].code === "W0151", ds);
  const docHover = await request("textDocument/hover", { textDocument: { uri: docU }, position: pos(documented, "y\n") });
  expect("hover: the doc comment, its doc tests as heff", docHover.result &&
    docHover.result.contents.value.includes("y : R") &&
    docHover.result.contents.value.includes('a record of a name\n```heff\n>>> y\n{ name = "name", n = 1 }\n>>> y.name\nname\n```'), docHover.result);
  const dRefs = await request("textDocument/references", { textDocument: { uri: docU }, position: { line: 0, character: 0 }, context: { includeDeclaration: true } });
  expect("references in a doc test's expectation", dRefs.result && dRefs.result.filter((r) => r.uri === docU).map((r) => r.range.start.line).join() === [0, 1, 9, 11].join(), dRefs.result);
  notify("textDocument/didClose", { textDocument: { uri: docU } });

  // a local: its binder and its uses, in its equation only (`f` is bound by
  // each equation of `map`); renamed there, unless the new name would be
  // captured, or capture a name used there; a primitive is not renamed
  const lineOf = (r) => r.range.start.line;
  const eq2 = pos(basics, "map f (Cons").line;
  const fRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "f y"), context: { includeDeclaration: true } });
  expect("references of a local", fRefs.result && fRefs.result.length === 3 && fRefs.result.every((r) => lineOf(r) === eq2), fRefs.result);
  const prepLocal = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "ys)") });
  expect("prepare a rename of a local", prepLocal.result && prepLocal.result.placeholder === "ys", prepLocal);
  const prepPrim = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "intAdd") });
  expect("no rename of a primitive", prepPrim.error && prepPrim.error.message.includes("primitive"), prepPrim);
  const yRen = await request("textDocument/rename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "y ys) = Cons"), newName: "z" });
  const yEdits = yRen.result && yRen.result.changes[uri("test/surface/basics.hf")];
  expect("a rename of a local", yEdits && yEdits.length === 2 && yEdits.every((e) => lineOf(e) === eq2 && e.newText === "z"), yRen);
  const yCaught = await request("textDocument/rename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "y ys) = Cons"), newName: "ys" });
  expect("no rename of a local to a name it would be captured by", yCaught.error, yCaught);
  const yCaptures = await request("textDocument/rename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "y ys) = Cons"), newName: "map" });
  expect("no rename of a local to a name used under it", yCaptures.error, yCaptures);

  // a local definition of a `where`: under its parent in the outline; its
  // signature, the head of each of its equations and each use are it,
  // renamed together
  const wOutline = await request("textDocument/documentSymbol", { textDocument: { uri: uri("test/surface/where.hf") } });
  const wSym = (n) => wOutline.result && wOutline.result.find((s) => s.name === n);
  expect("local definitions in the outline", wSym("scaled") && wSym("scaled").children.map((c) => c.name).join() === "times,go" &&
    wSym("nested") && wSym("nested").children.map((c) => c.name).join() === "outer,inner", wOutline.result && [wSym("scaled"), wSym("nested")]);
  const goAt = pos(whereSrc, "go xs");
  const goRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/where.hf") }, position: goAt, context: { includeDeclaration: true } });
  const goLines = goRefs.result && goRefs.result.map(lineOf);
  expect("references of a local definition", goLines && goLines.length === 5 &&
    [goAt, pos(whereSrc, "go : List Int"), pos(whereSrc, "go Nil = Nil"), pos(whereSrc, "go (y :: ys) = times"), pos(whereSrc, "go ys\n")].every((p) => goLines.includes(p.line)), goRefs.result);
  const goRen = await request("textDocument/rename", { textDocument: { uri: uri("test/surface/where.hf") }, position: pos(whereSrc, "go : List Int"), newName: "loop" });
  const goEdits = goRen.result && goRen.result.changes[uri("test/surface/where.hf")];
  expect("a rename of a local definition", goEdits && goEdits.length === 5 && goEdits.every((e) => e.newText === "loop"), goRen);
  // an operator in a pattern is a tag's other name, no local: `::` after
  // `=` is the prelude's, and so is the one in the pattern
  const consRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/where.hf") }, position: pos(whereSrc, ":: go ys"), context: { includeDeclaration: true } });
  expect("an operator in a pattern binds nothing", consRefs.result && consRefs.result.some((r) => r.uri === uri("lib/prelude.hf")), consRefs.result);
  // an instance argument, `{{ev : pick <: e}}`: bound in the equations
  const evRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/amb.hf") }, position: pos(amb, "ev (Flip"), context: { includeDeclaration: true } });
  const evLines = evRefs.result && evRefs.result.map(lineOf);
  expect("references of an instance argument", evLines && evLines.length === 2 &&
    evLines.includes(pos(amb, "{{ev : pick <: e}} -> Co e Bool").line) && evLines.includes(pos(amb, "ev (Flip").line), evRefs.result);

  // a lambda's annotated binder, `\(r : R) -> ...`: its uses are its
  const rRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/binder-ann.hf") }, position: pos(ann, "r.a + r.b"), context: { includeDeclaration: true } });
  const annLine = pos(ann, "r.a + r.b").line;
  expect("references of an annotated binder", rRefs.result && rRefs.result.length === 3 && rRefs.result.every((r) => lineOf(r) === annLine), rRefs.result);

  // a theory's field, used by the fields after it and projected
  const tRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "t -> t -> t"), context: { includeDeclaration: true } });
  const tLines = tRefs.result ? tRefs.result.map(lineOf) : [];
  expect("references of a field", tLines.includes(pos(basics, "t : Type").line) && tLines.filter((l) => l === pos(basics, "m : t").line).length === 3 && tLines.includes(pos(basics, "r.t").line), tRefs.result);
  for (const f of ["test/surface/where.hf", "test/surface/amb.hf", "test/surface/binder-ann.hf"])
    notify("textDocument/didClose", { textDocument: { uri: uri(f) } });

  // references across modules
  const where = (r) => `${r.uri.slice(r.uri.lastIndexOf("/") + 1)}:${r.range.start.line}:${r.range.start.character}-${r.range.end.character}`;
  const a = disk("test/modules/src/a.hf");
  open("test/modules/src/a.hf", a);
  await diagnosticsOf(uri("test/modules/src/a.hf"));
  const across = await request("textDocument/references", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv :"), context: { includeDeclaration: true } });
  const acrossAt = across.result ? across.result.map(where) : [];
  const qNot = pos(qualified, "A.inv");
  expect("references across modules", acrossAt.includes(`b.hf:${pos(good, "inv", 1).line}:${pos(good, "inv", 1).character}-${pos(good, "inv", 1).character + 3}`) &&
    acrossAt.includes(`qualified.hf:${qNot.line}:${qNot.character + 2}-${qNot.character + 5}`) && acrossAt.filter((x) => x.startsWith("a.hf")).length === 3, acrossAt);

  // renaming: every occurrence, in every file; refused for a name of
  // another kind, and a name already there
  const prep = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv True") });
  expect("prepare a rename", prep.result && prep.result.placeholder === "inv" && prep.result.range.start.line === pos(a, "inv True").line, prep);
  const ren = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv :"), newName: "neg" });
  const edits = ren.result && ren.result.changes;
  expect("a rename", edits && edits[uri("test/modules/src/a.hf")].length === 3 && edits[uri("test/modules/src/b.hf")].length === 2 &&
    edits[uri("test/modules/qualified.hf")].length === 2 && Object.values(edits).flat().every((e) => e.newText === "neg" && e.range.end.character - e.range.start.character === 3), edits);
  const wrongKind = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv :"), newName: "Not" });
  expect("no rename to another kind of name", wrongKind.error, wrongKind);
  // (`b` defines `twice`, which would hide the imported `inv` renamed to it)
  const captured = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv :"), newName: "twice" });
  expect("no rename to a name already there", captured.error && captured.error.message.includes("twice"), captured);

  // a file changed on disk, not in the editor: told of, the project is
  // elaborated again
  const refsOfNot = async () => (await request("textDocument/references", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "inv :"), context: { includeDeclaration: false } })).result || [];
  // (in app/, where test/golden.mjs, run at the same time, looks for no file)
  const watched = "test/modules/app/lsp-watched.hf";
  expect("before a file is made", !(await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  writeFileSync(join(root, watched), "import a\n\ny : Bool\ny = inv True\n");
  notify("workspace/didChangeWatchedFiles", { changes: [{ uri: uri(watched), type: 1 }] });
  expect("a file made on disk", (await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  unlinkSync(join(root, watched));
  notify("workspace/didChangeWatchedFiles", { changes: [{ uri: uri(watched), type: 3 }] });
  expect("a file deleted on disk", !(await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  notify("textDocument/didClose", { textDocument: { uri: uri("test/modules/src/a.hf") } });

  // splitting a pattern variable into cases: its equation once for each
  // tag of its type (a list's `Cons` by `::`, as the prelude writes it), or
  // once for a record, with names none in the equation is; the other
  // patterns and the body kept; nothing for a type that is no data, a
  // lambda's binder, or a request for another kind of action
  const splitSrc = [
    "data Shape = Circle Int | Rect Int Int", "", "type Point = { x : Int, y : Int }", "",
    "len : List Int -> Int", "len xs = 0", "",
    "size : Maybe Bool -> Int -> Int", "size m n = n", "",
    "area : Shape -> Int", "area s = 0", "",
    "px : Point -> Int", "px p = 1", "",
    "sum2 : List Int -> Int -> Int", "sum2 xs x = x", "",
    "inner : List Int -> Int", "inner (Cons y ys) = y", "inner _ = 0", "",
    "flag : Bool -> Pair Int Int -> Int", "flag b p = 0", "",
    "wrap : Maybe Int -> List Int", "wrap m =", "  Cons 1 (Cons 2", "    Nil)", "",
    "apply : (Int -> Int) -> Int", "apply f = 0", "",
    "lam : Bool -> Bool", "lam = \\c -> c", "",
  ].join("\n");
  const splitU = uri("test/surface/lsp-split.hf");
  open("test/surface/lsp-split.hf", splitSrc);
  ds = await diagnosticsOf(splitU);
  expect("a file to split in", ds.length === 0, ds);
  const splitAt = async (at, only) => {
    const p = pos(splitSrc, at.text, 0);
    const start = { line: p.line, character: p.character + at.offset };
    const r = await request("textDocument/codeAction", { textDocument: { uri: splitU }, range: { start, end: start }, context: { diagnostics: [], ...(only ? { only } : {}) } });
    return r.result;
  };
  const splitTo = async (what, at, title, expected) => {
    const acts = await splitAt(at);
    const edit = acts && acts.length === 1 && acts[0].edit.changes[splitU];
    const line = pos(splitSrc, at.text).line;
    expect(what, acts && acts.length === 1 && acts[0].title === title && acts[0].kind === "refactor.rewrite" && edit && edit.length === 1 &&
      edit[0].range.start.line === line && edit[0].range.start.character === 0 && edit[0].newText === expected, acts);
    return edit;
  };
  await splitTo("split a list", { text: "len xs", offset: 5 }, "Split `xs` into cases", "len Nil = 0\nlen (x :: xs1) = 0");
  await splitTo("split a Maybe, the other pattern kept", { text: "size m n", offset: 5 }, "Split `m` into cases", "size Nothing n = n\nsize (Just x) n = n");
  await splitTo("split a data type of one's own", { text: "area s", offset: 5 }, "Split `s` into cases", "area (Circle n) = 0\narea (Rect n m) = 0");
  await splitTo("split a record", { text: "px p", offset: 3 }, "Split `p` into cases", "px { x = x, y = y } = 1");
  await splitTo("split with names taken", { text: "sum2 xs", offset: 5 }, "Split `xs` into cases", "sum2 Nil x = x\nsum2 (y :: xs1) x = x");
  await splitTo("split a variable in a pattern", { text: "inner (Cons y ys)", offset: 14 }, "Split `ys` into cases", "inner (Cons y Nil) = y\ninner (Cons y (x :: xs)) = y");
  await splitTo("split a Bool", { text: "flag b", offset: 5 }, "Split `b` into cases", "flag True p = 0\nflag False p = 0");
  await splitTo("split a Pair", { text: "flag b p", offset: 7 }, "Split `p` into cases", "flag b (MkPair x y) = 0");
  const wrapped = await splitTo("split an equation of lines", { text: "wrap m", offset: 5 }, "Split `m` into cases",
    "wrap Nothing =\n  Cons 1 (Cons 2\n    Nil)\nwrap (Just x) =\n  Cons 1 (Cons 2\n    Nil)");
  expect("an equation of lines replaced to its end", wrapped && wrapped[0].range.end.line === pos(splitSrc, "    Nil)").line && wrapped[0].range.end.character === 8, wrapped);
  for (const [what, at] of [["an Int, used", { text: "size m n", offset: 7 }], ["a function", { text: "apply f", offset: 6 }], ["a lambda's binder", { text: "\\c", offset: 1 }], ["no variable", { text: "len : ", offset: 1 }]]) {
    const acts = await splitAt(at);
    expect("no split: " + what, Array.isArray(acts) && acts.length === 0, acts);
  }
  const quick = await splitAt({ text: "len xs", offset: 5 }, ["quickfix"]);
  expect("no split where only another kind is asked for", Array.isArray(quick) && quick.length === 0, quick);
  const refactors = await splitAt({ text: "len xs", offset: 5 }, ["refactor"]);
  expect("a split where refactors are asked for", Array.isArray(refactors) && refactors.length === 1, refactors);
  notify("textDocument/didClose", { textDocument: { uri: splitU } });

  // formatting: one edit, the whole text, a definition under `-- fmt: skip`
  // as written; what does not parse is left, and nothing said of it (no
  // message on each save of a file half written)
  const kept = "-- fmt: skip\nsame  :  Bool -> Bool\nsame   b  =  b\n";
  const messy = "data Bool = True | False\n\nnot :  Bool -> Bool\nnot   True =    False -- c\nnot False = True\n\n" + kept;
  open("test/surface/lsp-fmt.hf", messy);
  await diagnosticsOf(uri("test/surface/lsp-fmt.hf"));
  const fmt = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-fmt.hf") }, options: { tabSize: 2, insertSpaces: true } });
  expect("formatting", Array.isArray(fmt.result) && fmt.result.length === 1 &&
    fmt.result[0].newText === "data Bool = True | False\n\nnot : Bool -> Bool\nnot True = False -- c\nnot False = True\n\n" + kept, fmt.result);
  open("test/surface/lsp-bad.hf", "x : Bool\nx = (\n");
  await diagnosticsOf(uri("test/surface/lsp-bad.hf"));
  const refused = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-bad.hf") }, options: { tabSize: 2, insertSpaces: true } });
  // (what the server says comes in the order it says it: a message sent
  // with the reply would be here by now)
  const shown = notes.filter((m) => m.method === "window/showMessage");
  expect("formatting refused, quietly", Array.isArray(refused.result) && refused.result.length === 0 && shown.length === 0, { refused, shown });

  const unknown = await request("textDocument/codeLens", { textDocument: { uri: uri("test/modules/src/b.hf") } });
  expect("an unknown request", unknown.error && unknown.error.code === -32601, unknown);

  const down = await request("shutdown", null);
  expect("shutdown", down.result === null, down);
  const code = await new Promise((ok) => { server.on("exit", ok); notify("exit", null); });
  expect("exit after shutdown", code === 0, code);
} catch (e) {
  failed++;
  console.log("FAIL " + e.message);
  server.kill();
}

console.log(`lsp: ${checks - failed}/${checks} checks hold`);
process.exit(failed ? 1 : 0);
