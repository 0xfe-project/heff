#!/usr/bin/env node
// The language server (`heff lsp`), spoken to as an editor would: diagnostics
// (of the editor's text, and of an import's unsaved text, of a file as deep
// as the command line goes), files changed on disk, hover and go to
// definition (across modules, past non-ASCII text), completion, an outline,
// references and renaming across the project, formatting, shutdown.
import { spawn } from "node:child_process";
import { readFileSync, unlinkSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const uri = (f) => pathToFileURL(join(root, f)).href;
const disk = (f) => readFileSync(join(root, f), "utf8");

// (as vscode-languageclient starts it, with `--stdio`)
// (with a bounded heap: what it keeps must fit, on any machine)
const server = spawn("node", [join(root, "output", "heff.mjs"), "lsp", "--stdio"], { cwd: root, stdio: ["pipe", "pipe", "inherit"], env: { ...process.env, HEFF_HEAP_MB: "1024" } });
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
    caps.referencesProvider && caps.renameProvider.prepareProvider && caps.documentSymbolProvider && caps.workspaceSymbolProvider, caps);
  notify("initialized", {});
  // (asked to be told of the files changed on disk)
  const registration = await note((m) => m.method === "client/registerCapability", "client/registerCapability");
  expect("watching files", registration.params.registrations[0].method === "workspace/didChangeWatchedFiles" && registration.params.registrations[0].registerOptions.watchers[0].globPattern === "**/*.hf", registration);
  write({ id: registration.id, result: null });

  // a file as deep as the elaborator must go deep for: the server's stack is
  // the command line's
  const deep = "import prelude\n\nxs : List Int\nxs = [" + Array.from({ length: 20000 }, (_, i) => i).join(", ") + "]\n";
  open("test/surface/lsp-deep.hf", deep);
  ds = await diagnosticsOf(uri("test/surface/lsp-deep.hf"));
  expect("a deep file", ds.length === 0, ds);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/lsp-deep.hf") } });

  // a type error, in the editor's text (not the file's)
  const bad = 'import a\n\ntwice : Bool -> Bool\ntwice b = not (not "x")\n';
  open("test/modules/src/b.hf", bad);
  ds = await diagnosticsOf(uri("test/modules/src/b.hf"));
  const at = pos(bad, '"x"');
  expect("a type error's diagnostic", ds.length === 1 && ds[0].code === "E0311" && ds[0].severity === 1 &&
    ds[0].range.start.line === at.line && ds[0].range.start.character === at.character && ds[0].range.end.character === at.character + 3 &&
    ds[0].message.includes("expected type Bool, but got type String"), ds);

  // fixed, with a change (full sync, debounced), and non-ASCII before a name
  const good = "import a\n\ntwice : Bool -> Bool\ntwice b = {- 😀 λ -} not (not b)\n";
  notify("textDocument/didChange", { textDocument: { uri: uri("test/modules/src/b.hf"), version: 2 }, contentChanges: [{ text: good }] });
  ds = await diagnosticsOf(uri("test/modules/src/b.hf"));
  expect("no diagnostics once fixed", ds.length === 0, ds);

  const hover = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "not", 1) });
  expect("hover past non-ASCII text", hover.result && hover.result.contents.value.includes("not : Bool -> Bool") && hover.result.contents.value.includes("(global)"), hover.result);
  const local = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "b)") });
  expect("hover on a local", local.result && local.result.contents.value.includes("b : Bool") && local.result.contents.value.includes("(local)"), local.result);
  const nothing = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: { line: 1, character: 0 } });
  expect("hover on nothing", nothing.result === null, nothing.result);

  // a global of an imported module, not open: its file on disk
  const def = await request("textDocument/definition", { textDocument: { uri: uri("test/modules/src/b.hf") }, position: pos(good, "not", 1) });
  const notAt = pos(disk("test/modules/src/a.hf"), "not");
  expect("definition in another module", def.result && def.result.uri === uri("test/modules/src/a.hf") &&
    def.result.range.start.line === notAt.line && def.result.range.start.character === notAt.character && def.result.range.end.character === notAt.character + 3, def.result);

  // a global of a module imported as `A`: `A.not`
  const qualified = disk("test/modules/qualified.hf");
  open("test/modules/qualified.hf", qualified);
  await diagnosticsOf(uri("test/modules/qualified.hf"));
  const qDef = await request("textDocument/definition", { textDocument: { uri: uri("test/modules/qualified.hf") }, position: pos(qualified, "not") });
  expect("definition of a qualified name", qDef.result && qDef.result.uri === uri("test/modules/src/a.hf") && qDef.result.range.start.line === notAt.line, qDef.result);
  const qHover = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/qualified.hf") }, position: pos(qualified, "not") });
  expect("hover on a qualified name", qHover.result && qHover.result.contents.value.includes("A.not : A.Bool -> A.Bool"), qHover.result);

  // completion: the names in scope that start as the one being typed; after
  // `A.`, the definitions of the module imported as `A`
  const typing = "import prelude\n\nx : Int\nx = len";
  open("test/surface/lsp-complete.hf", typing);
  await diagnosticsOf(uri("test/surface/lsp-complete.hf"));
  const comp = await request("textDocument/completion", { textDocument: { uri: uri("test/surface/lsp-complete.hf") }, position: { line: 3, character: 7 } });
  const lengthItem = comp.result && comp.result.find((i) => i.label === "length");
  expect("completion of a prelude name", lengthItem && lengthItem.detail.includes("List a -> Int") && !comp.result.some((i) => i.label === "map"), comp.result && comp.result.map((i) => i.label));
  const dotted = "import a as A\n\nx : A.Bool\nx = A.";
  open("test/modules/lsp-dot.hf", dotted);
  await diagnosticsOf(uri("test/modules/lsp-dot.hf"));
  const dot = await request("textDocument/completion", { textDocument: { uri: uri("test/modules/lsp-dot.hf") }, position: { line: 3, character: 6 } });
  expect("completion after `A.`", dot.result && dot.result.some((i) => i.label === "not") && dot.result.every((i) => !i.label.includes(".")), dot.result && dot.result.map((i) => i.label));

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

  const withLib = "import prelude\n\nx : Bool\nx = True\n";
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
  expect("an import's unsaved text", dependent.some((d) => d.message.includes("no such variable not")), dependent);
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
  const withOps = "import prelude\n\nx : Int\nx = 1 + 2 * 3\n";
  open("test/surface/lsp-ops.hf", withOps);
  ds = await diagnosticsOf(uri("test/surface/lsp-ops.hf"));
  expect("operators of the prelude", ds.length === 0, ds);
  const plusHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-ops.hf") }, position: pos(withOps, "+") });
  expect("hover on an operator", plusHover.result && plusHover.result.contents.value.includes("(+) : Int -> Int -> Int"), plusHover.result);
  const plusDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-ops.hf") }, position: pos(withOps, "+") });
  const plusAt = pos(disk("lib/prelude.hf"), "(+) :");
  expect("definition of an operator", plusDef.result && plusDef.result.uri === uri("lib/prelude.hf") && plusDef.result.range.start.line === plusAt.line, plusDef.result);

  // a field, set in a record and projected: where its theory declares it
  const co = disk("lib/co.hf");
  open("lib/co.hf", co);
  await diagnosticsOf(uri("lib/co.hf"));
  const hmapAt = pos(co, "hmap :");
  for (const [what, at] of [["set in a record", pos(co, "hmap = ")], ["projected", pos(co, "hmap nt x")]]) {
    const d = await request("textDocument/definition", { textDocument: { uri: uri("lib/co.hf") }, position: at });
    expect("definition of a field " + what, d.result && d.result.range.start.line === hmapAt.line && d.result.range.start.character === hmapAt.character, d.result);
  }

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
  const wsym = await request("workspace/symbol", { query: "twic" });
  expect("the project's symbols", wsym.result && wsym.result.some((s) => s.name === "twice" && s.location.uri === uri("test/surface/basics.hf")), wsym.result);

  // a recursive call is the global it calls
  const recDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "map f ys") });
  expect("definition of a recursive call", recDef.result && recDef.result.range.start.line === pos(basics, "map :").line, recDef.result);

  // references: the signature, each equation, each use, a watch; across modules
  const where = (r) => `${r.uri.slice(r.uri.lastIndexOf("/") + 1)}:${r.range.start.line}:${r.range.start.character}-${r.range.end.character}`;
  const refs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "not (not b)"), context: { includeDeclaration: true } });
  const notLines = refs.result && refs.result.filter((r) => r.uri === uri("test/surface/basics.hf")).map((r) => r.range.start.line);
  expect("references in a file", notLines && [pos(basics, "not :"), pos(basics, "not True"), pos(basics, "not False"), pos(basics, "not (not"), pos(basics, ">>> not")].every((p) => notLines.includes(p.line)) && notLines.length === 6, refs.result);
  const usesOnly = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "not (not b)"), context: { includeDeclaration: false } });
  expect("references without the definition", usesOnly.result && usesOnly.result.length === 3, usesOnly.result);
  const a = disk("test/modules/src/a.hf");
  open("test/modules/src/a.hf", a);
  await diagnosticsOf(uri("test/modules/src/a.hf"));
  const across = await request("textDocument/references", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not :"), context: { includeDeclaration: true } });
  const acrossAt = across.result ? across.result.map(where) : [];
  const qNot = pos(qualified, "A.not");
  expect("references across modules", acrossAt.includes(`b.hf:${pos(good, "not", 1).line}:${pos(good, "not", 1).character}-${pos(good, "not", 1).character + 3}`) &&
    acrossAt.includes(`qualified.hf:${qNot.line}:${qNot.character + 2}-${qNot.character + 5}`) && acrossAt.filter((x) => x.startsWith("a.hf")).length === 3, acrossAt);

  // renaming: every occurrence, in every file; refused for a local, a
  // primitive, a name of another kind, and a name already there
  const prep = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not True") });
  expect("prepare a rename", prep.result && prep.result.placeholder === "not" && prep.result.range.start.line === pos(a, "not True").line, prep);
  const prepLocal = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "ys)") });
  expect("prepare a rename of a local", prepLocal.result && prepLocal.result.placeholder === "ys", prepLocal);
  const prepPrim = await request("textDocument/prepareRename", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "intAdd") });
  expect("no rename of a primitive", prepPrim.error && prepPrim.error.message.includes("primitive"), prepPrim);
  const ren = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not :"), newName: "neg" });
  const edits = ren.result && ren.result.changes;
  expect("a rename", edits && edits[uri("test/modules/src/a.hf")].length === 3 && edits[uri("test/modules/src/b.hf")].length === 2 &&
    edits[uri("test/modules/qualified.hf")].length === 2 && Object.values(edits).flat().every((e) => e.newText === "neg" && e.range.end.character - e.range.start.character === 3), edits);
  const wrongKind = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not :"), newName: "Not" });
  expect("no rename to another kind of name", wrongKind.error, wrongKind);
  // (`b` defines `twice`, which would hide the imported `not` renamed to it)
  const captured = await request("textDocument/rename", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not :"), newName: "twice" });
  expect("no rename to a name already there", captured.error && captured.error.message.includes("twice"), captured);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/modules/src/a.hf") } });

  // a local: its binder and its uses, in its equation only (`f` is bound by
  // each equation of `map`); renamed there, unless the new name would be
  // captured, or capture a name used there
  const lineOf = (r) => r.range.start.line;
  const eq2 = pos(basics, "map f (Cons").line;
  const fRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "f y"), context: { includeDeclaration: true } });
  expect("references of a local", fRefs.result && fRefs.result.length === 3 && fRefs.result.every((r) => lineOf(r) === eq2), fRefs.result);
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
  const whereSrc = disk("test/surface/where.hf");
  open("test/surface/where.hf", whereSrc);
  await diagnosticsOf(uri("test/surface/where.hf"));
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
  const amb = disk("test/surface/amb.hf");
  open("test/surface/amb.hf", amb);
  await diagnosticsOf(uri("test/surface/amb.hf"));
  const evRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/amb.hf") }, position: pos(amb, "ev (Flip"), context: { includeDeclaration: true } });
  const evLines = evRefs.result && evRefs.result.map(lineOf);
  expect("references of an instance argument", evLines && evLines.length === 2 &&
    evLines.includes(pos(amb, "{{ev : pick <: e}} -> Co e Bool").line) && evLines.includes(pos(amb, "ev (Flip").line), evRefs.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/amb.hf") } });

  // a lambda's annotated binder, `\(r : R) -> ...`: its uses are its
  const ann = disk("test/surface/binder-ann.hf");
  open("test/surface/binder-ann.hf", ann);
  await diagnosticsOf(uri("test/surface/binder-ann.hf"));
  const rRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/binder-ann.hf") }, position: pos(ann, "r.a + r.b"), context: { includeDeclaration: true } });
  const annLine = pos(ann, "r.a + r.b").line;
  expect("references of an annotated binder", rRefs.result && rRefs.result.length === 3 && rRefs.result.every((r) => lineOf(r) === annLine), rRefs.result);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/surface/binder-ann.hf") } });

  // a file changed on disk, not in the editor: told of, the project is
  // elaborated again
  open("test/modules/src/a.hf", a);
  await diagnosticsOf(uri("test/modules/src/a.hf"));
  const refsOfNot = async () => (await request("textDocument/references", { textDocument: { uri: uri("test/modules/src/a.hf") }, position: pos(a, "not :"), context: { includeDeclaration: false } })).result || [];
  const watched = "test/modules/lsp-watched.hf";
  expect("before a file is made", !(await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  writeFileSync(join(root, watched), "import a\n\ny : Bool\ny = not True\n");
  notify("workspace/didChangeWatchedFiles", { changes: [{ uri: uri(watched), type: 1 }] });
  expect("a file made on disk", (await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  unlinkSync(join(root, watched));
  notify("workspace/didChangeWatchedFiles", { changes: [{ uri: uri(watched), type: 3 }] });
  expect("a file deleted on disk", !(await refsOfNot()).some((r) => r.uri === uri(watched)), "");
  notify("textDocument/didClose", { textDocument: { uri: uri("test/modules/src/a.hf") } });

  // a theory's field, used by the fields after it and projected
  const tRefs = await request("textDocument/references", { textDocument: { uri: uri("test/surface/basics.hf") }, position: pos(basics, "t -> t -> t"), context: { includeDeclaration: true } });
  const tLines = tRefs.result ? tRefs.result.map(lineOf) : [];
  expect("references of a field", tLines.includes(pos(basics, "t : Type").line) && tLines.filter((l) => l === pos(basics, "m : t").line).length === 3 && tLines.includes(pos(basics, "r.t").line), tRefs.result);

  // formatting: one edit, the whole text; what does not parse is left, with a warning
  const messy = "data Bool = True | False\n\nnot :  Bool -> Bool\nnot   True =    False -- c\nnot False = True\n";
  open("test/surface/lsp-fmt.hf", messy);
  await diagnosticsOf(uri("test/surface/lsp-fmt.hf"));
  const fmt = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-fmt.hf") }, options: { tabSize: 2, insertSpaces: true } });
  expect("formatting", Array.isArray(fmt.result) && fmt.result.length === 1 &&
    fmt.result[0].newText === "data Bool = True | False\n\nnot : Bool -> Bool\nnot True = False -- c\nnot False = True\n", fmt.result);
  open("test/surface/lsp-bad.hf", "x : Bool\nx = (\n");
  await diagnosticsOf(uri("test/surface/lsp-bad.hf"));
  const refused = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-bad.hf") }, options: { tabSize: 2, insertSpaces: true } });
  const shown = await note((m) => m.method === "window/showMessage", "window/showMessage");
  expect("formatting refused", Array.isArray(refused.result) && refused.result.length === 0 && shown.params.type === 2 && shown.params.message.includes("does not parse"), { refused, shown });

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
