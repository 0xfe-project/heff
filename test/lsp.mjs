#!/usr/bin/env node
// The language server (`hg lsp`), spoken to as an editor would: diagnostics
// (of the editor's text, and of an import's unsaved text), hover and go to
// definition (across modules, past non-ASCII text), formatting, shutdown.
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const uri = (f) => pathToFileURL(join(root, f)).href;
const disk = (f) => readFileSync(join(root, f), "utf8");

// (as vscode-languageclient starts it, with `--stdio`)
const server = spawn("node", [join(root, "bin", "hg.mjs"), "lsp", "--stdio"], { cwd: root, stdio: ["pipe", "pipe", "inherit"] });
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

try {
  const init = await request("initialize", { processId: process.pid, rootUri: pathToFileURL(root).href, capabilities: {} });
  const caps = init.result.capabilities;
  expect("capabilities", caps.hoverProvider && caps.definitionProvider && caps.documentFormattingProvider && caps.textDocumentSync.change === 1, caps);
  notify("initialized", {});

  // a type error, in the editor's text (not the file's)
  const bad = 'import a\n\ntwice :: Bool -> Bool\ntwice b = not (not "x")\n';
  open("test/modules/b.hg", bad);
  let ds = await diagnosticsOf(uri("test/modules/b.hg"));
  const at = pos(bad, '"x"');
  expect("a type error's diagnostic", ds.length === 1 && ds[0].code === "E0311" && ds[0].severity === 1 &&
    ds[0].range.start.line === at.line && ds[0].range.start.character === at.character && ds[0].range.end.character === at.character + 3 &&
    ds[0].message.includes("expected type Bool, but got type String"), ds);

  // fixed, with a change (full sync, debounced), and non-ASCII before a name
  const good = "import a\n\ntwice :: Bool -> Bool\ntwice b = {- 😀 λ -} not (not b)\n";
  notify("textDocument/didChange", { textDocument: { uri: uri("test/modules/b.hg"), version: 2 }, contentChanges: [{ text: good }] });
  ds = await diagnosticsOf(uri("test/modules/b.hg"));
  expect("no diagnostics once fixed", ds.length === 0, ds);

  const hover = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/b.hg") }, position: pos(good, "not", 1) });
  expect("hover past non-ASCII text", hover.result && hover.result.contents.value.includes("not :: Bool -> Bool") && hover.result.contents.value.includes("(global)"), hover.result);
  const local = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/b.hg") }, position: pos(good, "b)") });
  expect("hover on a local", local.result && local.result.contents.value.includes("b :: Bool") && local.result.contents.value.includes("(local)"), local.result);
  const nothing = await request("textDocument/hover", { textDocument: { uri: uri("test/modules/b.hg") }, position: { line: 1, character: 0 } });
  expect("hover on nothing", nothing.result === null, nothing.result);

  // a global of an imported module, not open: its file on disk
  const def = await request("textDocument/definition", { textDocument: { uri: uri("test/modules/b.hg") }, position: pos(good, "not", 1) });
  const notAt = pos(disk("test/modules/a.hg"), "not");
  expect("definition in another module", def.result && def.result.uri === uri("test/modules/a.hg") &&
    def.result.range.start.line === notAt.line && def.result.range.start.character === notAt.character && def.result.range.end.character === notAt.character + 3, def.result);

  // a global of the file itself, and the standard library's
  const basics = disk("test/surface/basics.hg");
  open("test/surface/basics.hg", basics);
  ds = await diagnosticsOf(uri("test/surface/basics.hg"));
  expect("basics.hg elaborates", ds.length === 0, ds);
  const own = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hg") }, position: pos(basics, "not (not b)") });
  const ownAt = pos(basics, "not ::");
  expect("definition in the same file", own.result && own.result.uri === uri("test/surface/basics.hg") && own.result.range.start.line === ownAt.line && own.result.range.start.character === 0, own.result);
  const loc = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hg") }, position: pos(basics, "ys)") });
  expect("no definition for a local", loc.result === null, loc.result);

  const withLib = "import prelude\n\nx :: Bool\nx = True\n";
  open("test/surface/lsp-lib.hg", withLib);
  ds = await diagnosticsOf(uri("test/surface/lsp-lib.hg"));
  expect("an unsaved file importing the library", ds.length === 0, ds);
  const libDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-lib.hg") }, position: pos(withLib, "Bool") });
  const boolAt = pos(disk("lib/prelude.hg"), "data Bool");
  expect("definition in lib/", libDef.result && libDef.result.uri === uri("lib/prelude.hg") &&
    libDef.result.range.start.line === boolAt.line && libDef.result.range.start.character === boolAt.character + 5, libDef.result);

  // an import's unsaved text is what the importer sees
  open("test/modules/a.hg", "data Bool = True | False\n");
  ds = await diagnosticsOf(uri("test/modules/a.hg"));
  const dependent = await diagnosticsOf(uri("test/modules/b.hg"));
  expect("an import's unsaved text", dependent.some((d) => d.message.includes("no such variable not")), dependent);
  notify("textDocument/didClose", { textDocument: { uri: uri("test/modules/a.hg") } });

  // in a `do` block, the name hovered, not the `bind` it is read into
  const counter = disk("examples/counter/counter.hg");
  open("examples/counter/counter.hg", counter);
  await diagnosticsOf(uri("examples/counter/counter.hg"));
  const inDo = await request("textDocument/hover", { textDocument: { uri: uri("examples/counter/counter.hg") }, position: pos(counter, "showCount 0") });
  expect("hover in a do block", inDo.result && inDo.result.contents.value.includes("showCount :: Int -> String"), inDo.result);
  const defInDo = await request("textDocument/definition", { textDocument: { uri: uri("examples/counter/counter.hg") }, position: pos(counter, "showCount 0") });
  expect("definition in a do block", defInDo.result && defInDo.result.range.start.line === pos(counter, "showCount ::").line, defInDo.result);

  // a constructor, in an expression and in a pattern: where its type declares it
  const consAt = pos(basics, "Cons a (List a)");
  for (const [what, at] of [["in an expression", pos(basics, "Cons (f y)")], ["in a pattern", pos(basics, "Cons y ys) = Cons")]]) {
    const d = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/basics.hg") }, position: at });
    expect("definition of a constructor " + what, d.result && d.result.range.start.line === consAt.line && d.result.range.start.character === consAt.character, d.result);
  }

  // an operator: `+` of the prelude
  const withOps = "import prelude\n\nx :: Int\nx = 1 + 2 * 3\n";
  open("test/surface/lsp-ops.hg", withOps);
  ds = await diagnosticsOf(uri("test/surface/lsp-ops.hg"));
  expect("operators of the prelude", ds.length === 0, ds);
  const plusHover = await request("textDocument/hover", { textDocument: { uri: uri("test/surface/lsp-ops.hg") }, position: pos(withOps, "+") });
  expect("hover on an operator", plusHover.result && plusHover.result.contents.value.includes("(+) :: Int -> Int -> Int"), plusHover.result);
  const plusDef = await request("textDocument/definition", { textDocument: { uri: uri("test/surface/lsp-ops.hg") }, position: pos(withOps, "+") });
  const plusAt = pos(disk("lib/prelude.hg"), "(+) ::");
  expect("definition of an operator", plusDef.result && plusDef.result.uri === uri("lib/prelude.hg") && plusDef.result.range.start.line === plusAt.line, plusDef.result);

  // formatting: one edit, the whole text; what does not parse is left, with a warning
  const messy = "data Bool = True | False\n\nnot :: Bool -> Bool\nnot   True =    False -- c\nnot False = True\n";
  open("test/surface/lsp-fmt.hg", messy);
  await diagnosticsOf(uri("test/surface/lsp-fmt.hg"));
  const fmt = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-fmt.hg") }, options: { tabSize: 2, insertSpaces: true } });
  expect("formatting", Array.isArray(fmt.result) && fmt.result.length === 1 &&
    fmt.result[0].newText === "data Bool = True | False\n\nnot :: Bool -> Bool\nnot True = False -- c\nnot False = True\n", fmt.result);
  open("test/surface/lsp-bad.hg", "x :: Bool\nx = (\n");
  await diagnosticsOf(uri("test/surface/lsp-bad.hg"));
  const refused = await request("textDocument/formatting", { textDocument: { uri: uri("test/surface/lsp-bad.hg") }, options: { tabSize: 2, insertSpaces: true } });
  const shown = await note((m) => m.method === "window/showMessage", "window/showMessage");
  expect("formatting refused", Array.isArray(refused.result) && refused.result.length === 0 && shown.params.type === 2 && shown.params.message.includes("does not parse"), { refused, shown });

  const unknown = await request("textDocument/codeLens", { textDocument: { uri: uri("test/modules/b.hg") } });
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
