# heff for bat

`heff.sublime-syntax` colours `.hf` files in
[bat](https://github.com/sharkdp/bat) (and Sublime Text).  It is made from
the VS Code grammar, `editors/vscode/syntaxes/heff.tmLanguage.json`, by
`build.mjs`; change that grammar, not this file, then make it again:

```sh
node editors/bat/build.mjs           # at the repository's root
node editors/bat/build.mjs --check   # only: is it made from the grammar as it is?
```

## Installing it

bat reads the syntaxes in its configuration's directory into its cache:

```sh
mkdir -p "$(bat --config-dir)/syntaxes"
ln -sf "$PWD/editors/bat/heff.sublime-syntax" "$(bat --config-dir)/syntaxes/"
bat cache --build
bat --list-languages | grep heff     # heff:hf
```

Linked, the file follows the repository; when it changes, `bat cache --build`
again.
