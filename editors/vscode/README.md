# heff for VS Code

Highlighting for `.hf` files, and a client for heff's language server
(`heff lsp`): diagnostics as you type, hover, go to definition,
completion, formatting, an outline, and references, renaming and symbols
across the project.

The server is the repository's `.heff/out/heff`, made by the build; after
changing the compiler, rebuild it:

```sh
node scripts/build.mjs     # at the repository's root
```

## Running it from the repository

```sh
cd editors/vscode
npm install
code .          # then F5 ("Run the heff extension"): a window on examples/
```

The extension finds the server at `../../.heff/out/heff`, relative to itself.

## Installing it

```sh
cd editors/vscode
npm install
npx vsce package          # heff-0.1.0.vsix
code --install-extension heff-0.1.0.vsix
```

An installed extension is not in the repository any more: it runs the `heff`
on the `PATH` (a release's `bin/heff`, or `.heff/out/heff`), or set
`heff.serverPath` to the absolute path of one.

## Settings

| setting | |
| --- | --- |
| `heff.serverPath` | the heff executable, run as `<path> lsp`; empty: `../../.heff/out/heff` from the extension, else the `heff` on the `PATH` |
