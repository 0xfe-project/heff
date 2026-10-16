// The client: runs `<heff> lsp --stdio` for `.hf` files.
const fs = require("fs");
const os = require("os");
const path = require("path");
const vscode = require("vscode");

// (not there before `npm install` in editors/vscode: said, not thrown, or
// the extension fails to load with nothing on the screen)
let languageclient;
try { languageclient = require("vscode-languageclient/node"); } catch (_) { languageclient = null; }

let client;

const exe = process.platform === "win32" ? "heff.exe" : "heff";

function runnable(file) {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return fs.statSync(file).isFile();
  } catch (_) {
    return false;
  }
}

// the heff home's bin/, where `heff install` puts programs: HEFF_HOME/bin,
// else ~/.heff/bin
function homeBin() {
  return path.join(process.env.HEFF_HOME || path.join(os.homedir(), ".heff"), "bin");
}

// the setting's path: `~` is the home directory, and a relative path is from
// the workspace's first folder (none without one)
function configuredPath(configured) {
  if (/^~($|[\\/])/.test(configured)) return path.join(os.homedir(), configured.slice(1));
  if (path.isAbsolute(configured)) return configured;
  const folder = (vscode.workspace.workspaceFolders || [])[0];
  return folder ? path.join(folder.uri.fsPath, configured) : null;
}

// the heff to run, else where it was looked for: the setting, else (run from
// the repository, F5) the repository's, else `heff` on the PATH, else the one
// in the heff home's bin/
function findHeff(context) {
  const configured = vscode.workspace.getConfiguration("heff").get("serverPath");
  if (configured) {
    const file = configuredPath(configured);
    if (!file) return { missing: `heff.serverPath is ${configured}, a relative path, and no folder is open to find it from` };
    return runnable(file) ? { heff: file } : { missing: `no heff at ${file} (heff.serverPath)` };
  }
  if (context.extensionMode === vscode.ExtensionMode.Development) {
    const repository = path.join(context.extensionPath, "..", "..", ".heff", "out", exe);
    if (runnable(repository)) return { heff: repository };
  }
  for (const dir of (process.env.PATH || "").split(path.delimiter)) {
    if (dir && runnable(path.join(dir, exe))) return { heff: path.join(dir, exe) };
  }
  const bin = homeBin();
  if (runnable(path.join(bin, exe))) return { heff: path.join(bin, exe) };
  return { missing: `no heff on the PATH or in ${bin}; put a release's bin/heff (or the build's .heff/out/heff) on the PATH, or set heff.serverPath to it` };
}

async function start(context) {
  if (!languageclient) {
    vscode.window.showErrorMessage(`heff: vscode-languageclient is not installed; run \`npm install\` in ${context.extensionPath}`);
    return;
  }
  const { LanguageClient, TransportKind } = languageclient;
  const { heff, missing } = findHeff(context);
  if (!heff) {
    vscode.window.showErrorMessage(`heff: ${missing}`);
    return;
  }
  // (the stdio transport: the client adds `--stdio` to the arguments)
  const run = { command: heff, args: ["lsp"], transport: TransportKind.stdio };
  client = new LanguageClient("heff", "heff language server", { run, debug: run }, {
    documentSelector: [{ scheme: "file", language: "heff" }],
  });
  client.info(`${heff} lsp --stdio`, undefined, false);
  await client.start();
}

// A manifest is read as a literal, not a module: it has no header.
function isManifest(file) {
  return ["package.hf", "build.hf"].includes(path.basename(file));
}

// (a heff file on disk, empty: one just made)
function isNew(doc) {
  return doc.languageId === "heff" && doc.uri.scheme === "file" && !isManifest(doc.uri.fsPath) && doc.getText() === "";
}

// A file just made gets its header, `module <name>`: the server says the
// name (`heff/moduleName`, the one it asks a header for), or none where
// no header can say it; a file not empty by then is left as it is.
async function writeHeader(doc) {
  if (!client || !isNew(doc)) return;
  let name;
  try {
    name = await client.sendRequest("heff/moduleName", { textDocument: { uri: doc.uri.toString() } });
  } catch (_) {
    return;
  }
  if (typeof name !== "string" || !name || doc.isClosed || doc.getText() !== "") return;
  const edit = new vscode.WorkspaceEdit();
  edit.insert(doc.uri, new vscode.Position(0, 0), `module ${name}\n\n`);
  await vscode.workspace.applyEdit(edit);
}

async function activate(context) {
  // (a file made in the editor is opened in it too)
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(writeHeader));
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (!e.affectsConfiguration("heff")) return;
      if (client) await client.stop();
      client = undefined;
      await start(context);
    })
  );
  await start(context);
  for (const doc of vscode.workspace.textDocuments) await writeHeader(doc);
}

async function deactivate() {
  if (client) await client.stop();
}

module.exports = { activate, deactivate };
