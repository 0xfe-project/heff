-- | Finding a program's files.  A module is a file, named by its path from
-- | the project's root: `parse.surface` is `ROOT/parse/surface.hg`.  The root
-- | is the nearest directory above the file given with an `hg.json`, else
-- | the file's own directory.  A module not in the project is looked for in
-- | the standard library (`lib/`).
module Hg.Load
  ( Paths
  , pathsFor
  , loadProgram
  ) where

import Prelude

import Data.Array as A
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as S
import Effect (Effect)
import Effect.Ref as Ref
import Hg.Driver (Source, Syntax(..), syntaxOf)
import Hg.Parse.Import (importPath, readImports)
import Node.Encoding (Encoding(..))
import Node.FS.Sync (exists, readTextFile)

type Paths = { root :: String, lib :: String }

dirname :: String -> String
dirname p = case S.lastIndexOf (S.Pattern "/") p of
  Just 0 -> "/"
  Just i -> S.take i p
  Nothing -> "."

-- | the root of the project a file is in
pathsFor :: String -> String -> Effect Paths
pathsFor lib file = do
  let start = dirname file
  let
    up d = do
      found <- exists (d <> "/hg.json")
      if found then pure d
      else if d == "/" || d == "." || d == "" then pure start
      else up (dirname d)
  root <- up start
  pure { root, lib }

-- | the module a file of the project is
moduleOf :: Paths -> String -> String
moduleOf ps file =
  let
    rel = fromMaybe file (S.stripPrefix (S.Pattern (ps.root <> "/")) file)
    base = fromMaybe rel (S.stripSuffix (S.Pattern ".hg") rel)
  in
    S.replaceAll (S.Pattern "/") (S.Replacement ".") (fromMaybe base (S.stripSuffix (S.Pattern ".hpgf") base))

-- | a file and every module it imports, directly or not, each after what it
-- | imports
loadProgram :: Paths -> String -> Effect { sources :: Array Source, errors :: Array String }
loadProgram ps file = do
  done <- Ref.new ([] :: Array Source)
  errors <- Ref.new ([] :: Array String)
  let
    err e = Ref.modify_ (_ <> [ e ]) errors
    visit stack m path = do
      src <- readTextFile UTF8 path
      let
        r = if syntaxOf path == Reference then { imports: [], text: src } else readImports src
        deps = map _.name r.imports
      A.foldM (\_ d -> dep (A.snoc stack m) path d) unit deps
      Ref.modify_ (_ <> [ { path, module: m, source: src, text: r.text, deps } ]) done
    dep stack from m = do
      seen <- Ref.read done
      if A.elem m stack then err (from <> ": importing " <> m <> " is a cycle: " <> S.joinWith " -> " (A.snoc stack m))
      else if A.any (\s -> s.module == m) seen then pure unit
      else do
        let inRoot = ps.root <> "/" <> importPath m
        let inLib = ps.lib <> "/" <> importPath m
        a <- exists inRoot
        b <- exists inLib
        if a then visit stack m inRoot
        else if b then visit stack m inLib
        else err (from <> ": no module " <> m <> " (looked for " <> inRoot <> " and " <> inLib <> ")")
  visit [] (moduleOf ps file) file
  { sources: _, errors: _ } <$> Ref.read done <*> Ref.read errors
