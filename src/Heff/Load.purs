-- | Finding a program's files.  A module is a file, named by its path from
-- | the project's root: `parse.surface` is `ROOT/parse/surface.hf`.  The root
-- | is the nearest directory above the file given whose `package.json` has a
-- | `"heff"` field, else the file's own directory.  A module not in the project is looked for in
-- | the standard library (`lib/`), then among the installed packages
-- | (`node_modules/`, npm's): a package `foo` holds `foo.bar` at
-- | `node_modules/foo/bar.hf`, and its own files import one another by
-- | those same names.
module Heff.Load
  ( Paths
  , pathsFor
  , loadProgram
  , loadProgramWith
  ) where

import Prelude

import Control.Alt ((<|>))
import Data.Array as A
import Data.Maybe (Maybe(..), fromMaybe, isNothing)
import Data.String as S
import Effect (Effect)
import Effect.Ref as Ref
import Heff.Driver (Source)
import Heff.Parse.Import (importPath, readImports)
import Node.Encoding (Encoding(..))
import Node.FS.Sync (exists, readTextFile)

type Paths = { root :: String, lib :: String }

dirname :: String -> String
dirname p = case S.lastIndexOf (S.Pattern "/") p of
  Just 0 -> "/"
  Just i -> S.take i p
  Nothing -> "."

-- | the root of the project a file is in: the nearest directory above
-- | it whose `package.json` has a "heff" field (npm's, see the README)
pathsFor :: String -> String -> Effect Paths
pathsFor lib file = do
  let start = dirname file
  let
    up d = do
      found <- isRoot d
      if found then pure d
      else if d == "/" || d == "." || d == "" then pure start
      else up (dirname d)
  root <- up start
  pure { root, lib }

isRoot :: String -> Effect Boolean
isRoot d = do
  pkg <- exists (d <> "/package.json")
  if not pkg then pure false
  else do
    txt <- readTextFile UTF8 (d <> "/package.json")
    pure (S.contains (S.Pattern "\"heff\"") txt)

-- | the module a file of the project is; one of the standard library is
-- | the library's (`lib/co.hf` is `co`), wherever the project's root is
moduleOf :: Paths -> String -> String
moduleOf ps file =
  let
    rel = fromMaybe file (S.stripPrefix (S.Pattern (ps.lib <> "/")) file <|> S.stripPrefix (S.Pattern (ps.root <> "/node_modules/")) file <|> S.stripPrefix (S.Pattern (ps.root <> "/")) file)
    base = fromMaybe rel (S.stripSuffix (S.Pattern ".hf") rel)
  in
    S.replaceAll (S.Pattern "/") (S.Replacement ".") base

-- | a file and every module it imports, directly or not, each after what it
-- | imports
loadProgram :: Paths -> String -> Effect { sources :: Array Source, errors :: Array String }
loadProgram = loadProgramWith (readTextFile UTF8)

-- | the same, reading each file with the function given (an editor's
-- | unsaved text, say)
loadProgramWith :: (String -> Effect String) -> Paths -> String -> Effect { sources :: Array Source, errors :: Array String }
loadProgramWith read ps file = do
  done <- Ref.new ([] :: Array Source)
  errors <- Ref.new ([] :: Array String)
  let
    err e = Ref.modify_ (_ <> [ e ]) errors
    visit stack m path = do
      src <- read path
      let
        r = readImports src
        deps = A.nub (map _.name r.imports)
        opens = A.nub (map _.name (A.filter (\i -> isNothing i.alias) r.imports))
        aliases = A.mapMaybe (\i -> i.alias <#> \a -> { module: i.name, alias: a }) r.imports
      A.foldM (\_ d -> dep (A.snoc stack m) path d) unit deps
      Ref.modify_ (_ <> [ { path, module: m, source: src, text: r.text, deps, opens, aliases } ]) done
    dep stack from m = do
      seen <- Ref.read done
      if A.elem m stack then err (from <> ": importing " <> m <> " is a cycle: " <> S.joinWith " -> " (A.snoc stack m))
      else if A.any (\s -> s.module == m) seen then pure unit
      else do
        let inRoot = ps.root <> "/" <> importPath m
        let inLib = ps.lib <> "/" <> importPath m
        let inPkg = ps.root <> "/node_modules/" <> importPath m
        a <- exists inRoot
        b <- exists inLib
        c <- exists inPkg
        if a then visit stack m inRoot
        else if b then visit stack m inLib
        else if c then visit stack m inPkg
        else err (from <> ": no module " <> m <> " (looked for " <> inRoot <> ", " <> inLib <> " and " <> inPkg <> ")")
  visit [] (moduleOf ps file) file
  { sources: _, errors: _ } <$> Ref.read done <*> Ref.read errors
