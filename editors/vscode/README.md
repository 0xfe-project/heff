# heff for VS Code

Highlighting for `.hf` files, and a client for heff's language server
(`heff lsp`): diagnostics as you type, hover, go to definition,
completion, formatting, an outline, and references, renaming and symbols
across the project.

The server is the repository's `output/heff.mjs`, made by the build
(`npm install` runs it); after changing the compiler, rebuild it:

```sh
npm install && npm run build     # at the repository's root
```

## Running it from the repository

```sh
cd editors/vscode
npm install
code .          # then F5 ("Run the heff extension"): a window on examples/
```

The extension finds the server at `../../output/heff.mjs`, relative to itself.

## Installing it

```sh
cd editors/vscode
npm install
npx vsce package          # heff-0.1.0.vsix
code --install-extension heff-0.1.0.vsix
```

An installed extension is not in the repository any more: it runs the `heff`
on the `PATH` (`npm link` at the repository's root puts it there), or set
`heff.serverPath` to the absolute path of `output/heff.mjs` (and `heff.nodePath`
if `node` is not on the `PATH`).

## Settings

| setting | |
| --- | --- |
| `heff.serverPath` | heff's `output/heff.mjs`, run as `node <path> lsp`; empty: `../../output/heff.mjs` from the extension, else the `heff` on the `PATH` |
| `heff.nodePath` | the Node.js that runs it (`node`) |
