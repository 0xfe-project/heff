// The client: runs `node <heff.serverPath> lsp` for `.hg` files.
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

// the server's script: the setting, else the repository's (in development,
// this extension is its editors/vscode)
function serverPath(context) {
  const configured = vscode.workspace.getConfiguration("heff").get("serverPath");
  return configured || path.join(context.extensionPath, "..", "..", "bin", "hg.mjs");
}

async function start(context) {
  const config = vscode.workspace.getConfiguration("heff");
  const script = serverPath(context);
  if (!fs.existsSync(script)) {
    vscode.window.showErrorMessage(`heff: no language server at ${script}; set heff.serverPath to heff's bin/hg.mjs`);
    return;
  }
  const run = { command: config.get("nodePath") || "node", args: [script, "lsp"], transport: TransportKind.stdio };
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
