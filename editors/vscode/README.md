# heff for VS Code

Highlighting for `.hf` files, and a client for heff's language server
(`heff lsp`): diagnostics as you type, hover, go to definition,
completion, formatting, an outline, and references, renaming and symbols
across the project.

The server is a heff executable, run as `heff lsp --stdio`: the one
`heff.serverPath` names, else the `heff` on the `PATH`, else the heff home's
(`HEFF_HOME/bin/heff`, else `~/.heff/bin/heff`).  A release's `bin/heff` is
one; so is the repository's `.heff/out/heff`, made by the build, and the
heff home's, which it installs:

```sh
make build                 # at the repository's root
.heff/out/heff install     # then: ~/.heff/bin/heff, with its lib/
```

## Running it from the repository

```sh
cd editors/vscode
npm install
code .          # then F5 ("Run the heff extension"): a window on examples/
```

Run so, the extension finds the server at `../../.heff/out/heff`, relative to
itself, before looking anywhere else; after changing the compiler, rebuild it.

## Installing it

A release has it built (`heff-X.Y.Z.vsix`), or:

```sh
cd editors/vscode
npm install
npm run package           # heff-X.Y.Z.vsix
code --install-extension heff-*.vsix
```

## Settings

| setting | |
| --- | --- |
| `heff.serverPath` | the heff executable, run as `<path> lsp --stdio`, a relative path from the workspace's folder; empty: the `heff` on the `PATH`, else `HEFF_HOME/bin/heff`, else `~/.heff/bin/heff` |
