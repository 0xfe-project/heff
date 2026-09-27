# heff for VS Code

Highlighting for `.hg` files, and a client for heff's language server
(`hg lsp`): diagnostics as you type, the type of a name on hover, go to
definition (of globals, across modules and `lib/`), and formatting.

The server is the repository's `bin/hg.mjs`, so build heff first:

```sh
npm install && npx spago build     # at the repository's root
```

## Running it from the repository

```sh
cd editors/vscode
npm install
code .          # then F5 ("Run the heff extension"): a window on examples/
```

The extension finds the server at `../../bin/hg.mjs`, relative to itself.

## Installing it

```sh
cd editors/vscode
npm install
npx vsce package          # heff-0.1.0.vsix
code --install-extension heff-0.1.0.vsix
```

An installed extension is not in the repository any more: set
`heff.serverPath` to the absolute path of `bin/hg.mjs` (and `heff.nodePath`
if `node` is not on the `PATH`).

## Settings

| setting | |
| --- | --- |
| `heff.serverPath` | heff's `bin/hg.mjs`, run as `node <path> lsp`; empty: `../../bin/hg.mjs` from the extension |
| `heff.nodePath` | the Node.js that runs it (`node`) |
