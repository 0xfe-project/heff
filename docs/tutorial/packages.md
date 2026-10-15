---
title: Packages
---

The zipper of chapter 4 has nothing to do with todos: other programs
could use it too. In this chapter it becomes a package of its own, and
`hello` requires it.

## A package of its own

Next to `hello`, make a directory `zipper` and move the zipper there:

```sh
mkdir -p ../zipper/src/zipper
mv src/hello/zipper.hf ../zipper/src/zipper/list.hf
```

Its first line becomes its new name, `module zipper.list`. Then give the
package a `package.hf`:

```haskell
import manifest

package : Package
package = { module = "github.com/you/zipper", heff = "0.4.0" }
```

`module` is where the package lives, and its last part, `zipper`, is
the first part of its modules' names.

## Requiring it

In `hello`'s `package.hf`:

```haskell
import manifest

package : Package
package =
  { module = "hello"
  , heff = "0.4.0"
  , require =
      [{ name = "zipper", path = "github.com/you/zipper", version = "0.1.0" }]
  , replace = [{ name = "zipper", dir = "../zipper" }]
  }
```

`require` names the package, where it lives and the version `hello`
needs. `replace` takes it from the directory next to `hello` instead,
while you work on both. In `src/hello/cursor.hf` and `app/hello/main.hf`,
`import hello.zipper` becomes `import zipper.list`.

```sh
$ heff build
heff build: 3 modules checked
wrote .heff/bin/hello
```

## Installing it

```sh
$ heff install
installed ~/.heff/bin/hello
$ hello
```

`heff install` builds the program to run its fastest and puts it in
`~/.heff/bin/`, which is on your `PATH` since chapter 1.
