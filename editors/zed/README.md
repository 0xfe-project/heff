# heff for Zed

Highlighting for `.hf` files, an outline (signatures, `data`, `type`,
`theory`), and a client for heff's language server (`heff lsp`):
diagnostics as you type, hover, go to definition, completion, formatting
(on save too, as Zed formats by the server by default), code actions,
references, renaming and symbols across the project.  Unlike VS Code's, a
new, empty `.hf` file gets no `module <name>` written in it; the quick fix
writes it.

The highlighting is the tree-sitter grammar of
[`editors/tree-sitter-heff`](../tree-sitter-heff/grammar.js), which Zed
reads from git: from this repository at the commit `extension.toml`
names (`[grammars.heff]`'s `rev`).  After changing the grammar, commit
it, then name that commit there.

The server is a heff executable, run as `heff lsp --stdio`: the one
`lsp.heff.binary.path` names, else the `heff` on the `PATH`, else the heff
home's (`HEFF_HOME/bin/heff`, else `~/.heff/bin/heff`).

## Installing it

From the repository: in Zed, `zed: install dev extension`, and choose
`editors/zed`.  Zed builds the extension (with Rust's `wasm32-wasip2`,
which it installs by rustup) and the grammar.

## Settings

In Zed's `settings.json`:

```json
{
  "lsp": {
    "heff": {
      "binary": { "path": "/path/to/heff", "arguments": ["lsp", "--stdio"] }
    }
  }
}
```
