-- | The header of a file: the `import m` lines before its first
-- | declaration.  `m` names the file `m.hf` (`a.b` names `a/b.hf`); where it
-- | is looked for is the loader's business (`Heff.Load`).
module Heff.Parse.Import
  ( Import
  , readImports
  , importPath
  ) where

import Prelude

import Data.Array as A
import Data.Maybe (Maybe(..))
import Data.String as S
import Data.String.CodeUnits as SCU
import Heff.Parse.Notation (Span)

type Import = { name :: String, span :: Span }

-- | the imports of a file, and its text with the import lines blanked out
-- | (spaces, so every offset stays where it was)
readImports :: String -> { imports :: Array Import, text :: String }
readImports src = go 0 []
  where
  len = SCU.length src
  lineEnd i = case SCU.indexOf' (S.Pattern "\n") i src of
    Just e -> e
    Nothing -> len
  go i acc
    | i >= len = done acc
    | otherwise =
        let
          e = lineEnd i
          line = SCU.slice i e src
          trimmed = S.trim line
          next = min len (e + 1)
        in
          if trimmed == "" || S.take 2 trimmed == "--" then go next acc
          else case S.stripPrefix (S.Pattern "import ") line of
            Just rest | isName (S.trim rest) -> go next (A.snoc acc { name: S.trim rest, span: { start: i, end: e } })
            _ -> done acc
  done acc = { imports: acc, text: A.foldl blank src acc }
  blank s imp = SCU.take imp.span.start s <> SCU.fromCharArray (A.replicate (imp.span.end - imp.span.start) ' ') <> SCU.drop imp.span.end s
  isName x = x /= "" && A.all (\c -> c == '.' || c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) (SCU.toCharArray x)

-- | the path, relative to a directory searched, of the file an import names
importPath :: String -> String
importPath name = S.replaceAll (S.Pattern ".") (S.Replacement "/") name <> ".hf"
