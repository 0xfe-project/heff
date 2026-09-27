# heff for VS Code

Highlighting for `.hf` files, and a client for heff's language server
(`heff lsp`): diagnostics as you type, the type of a name on hover, go to
definition (of globals, across modules and `lib/`), and formatting.

The server is the repository's `bin/heff.mjs`, so build heff first:

```sh
npm install && npx spago build     # at the repository's root
```

## Running it from the repository

```sh
cd editors/vscode
npm install
code .          # then F5 ("Run the heff extension"): a window on examples/
```

The extension finds the server at `../../bin/heff.mjs`, relative to itself.

## Installing it

```sh
cd editors/vscode
npm install
npx vsce package          # heff-0.1.0.vsix
code --install-extension heff-0.1.0.vsix
```

An installed extension is not in the repository any more: it runs the `heff`
on the `PATH` (`npm link` at the repository's root puts it there), or set
`heff.serverPath` to the absolute path of `bin/heff.mjs` (and `heff.nodePath`
if `node` is not on the `PATH`).

## Settings

| setting | |
| --- | --- |
| `heff.serverPath` | heff's `bin/heff.mjs`, run as `node <path> lsp`; empty: `../../bin/heff.mjs` from the extension, else the `heff` on the `PATH` |
| `heff.nodePath` | the Node.js that runs it (`node`) |
