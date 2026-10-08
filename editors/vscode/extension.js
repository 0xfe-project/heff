// The client: runs `<heff.serverPath> lsp` for `.hf` files.
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

// (not there before `npm install` in editors/vscode: said, not thrown, or
// the extension fails to load with nothing on the screen)
let languageclient;
try { languageclient = require("vscode-languageclient/node"); } catch (_) { languageclient = null; }

let client;

// the server: the setting, else the repository's (in development, this
// extension is its editors/vscode), else `heff` on the PATH
function serverPath(context) {
  const configured = vscode.workspace.getConfiguration("heff").get("serverPath");
  if (configured) return configured;
  const repository = path.join(context.extensionPath, "..", "..", ".heff", "out", process.platform === "win32" ? "heff.exe" : "heff");
  return fs.existsSync(repository) ? repository : "heff";
}

async function start(context) {
  if (!languageclient) {
    vscode.window.showErrorMessage(`heff: vscode-languageclient is not installed; run \`npm install\` in ${context.extensionPath}`);
    return;
  }
  const { LanguageClient } = languageclient;
  // (no `transport`: with one, the client adds `--stdio` to the arguments)
  const run = { command: serverPath(context), args: ["lsp"] };
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
