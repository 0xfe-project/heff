// The client: runs `node <heff.serverPath> lsp` for `.hf` files.
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

// (not there before `npm install` in editors/vscode: said, not thrown, or
// the extension fails to load with nothing on the screen)
let languageclient;
try { languageclient = require("vscode-languageclient/node"); } catch (_) { languageclient = null; }

let client;

// the `heff` on the PATH, through its links (`npm link`, `npm install -g`), when
// it is heff's bin/heff.mjs
function heffOnPath() {
  for (const dir of (process.env.PATH || "").split(path.delimiter)) {
    if (!dir) continue;
    let real;
    try { real = fs.realpathSync(path.join(dir, "heff")); } catch (_) { continue; }
    if (path.basename(real) === "heff.mjs") return real;
  }
  return null;
}

// the server's script: the setting, else the repository's (in development,
// this extension is its editors/vscode), else the `heff` on the PATH
function serverPath(context) {
  const configured = vscode.workspace.getConfiguration("heff").get("serverPath");
  if (configured) return configured;
  const repository = path.join(context.extensionPath, "..", "..", "bin", "heff.mjs");
  if (fs.existsSync(repository)) return repository;
  return heffOnPath() || repository;
}

async function start(context) {
  if (!languageclient) {
    vscode.window.showErrorMessage(`heff: vscode-languageclient is not installed; run \`npm install\` in ${context.extensionPath}`);
    return;
  }
  const { LanguageClient } = languageclient;
  const config = vscode.workspace.getConfiguration("heff");
  const script = serverPath(context);
  if (!fs.existsSync(script)) {
    vscode.window.showErrorMessage(`heff: no language server at ${script}; put \`heff\` on the PATH (\`npm link\`), or set heff.serverPath to its bin/heff.mjs`);
    return;
  }
  // (no `transport`: with one, the client adds `--stdio` to the arguments)
  const run = { command: config.get("nodePath") || "node", args: [script, "lsp"] };
  client = new LanguageClient("heff", "heff language server", { run, debug: run }, {
    documentSelector: [{ scheme: "file", language: "heff" }],
  });
  await client.start();
}

async function activate(context) {
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (!e.affectsConfiguration("heff")) return;
      if (client) await client.stop();
      client = undefined;
      await start(context);
    })
  );
  await start(context);
}

async function deactivate() {
  if (client) await client.stop();
}

module.exports = { activate, deactivate };
