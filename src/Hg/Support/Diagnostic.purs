-- | Rendering diagnostics the way `diagnostician` does.
module Hg.Support.Diagnostic
  ( renderDiag
  , renderDiags
  , lineCol
  , renderDiagMessage
  ) where

import Prelude

import Data.Array as A
import Data.Foldable (foldMap)
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String.CodeUnits as SCU
import Hg.Parse.FNotation (Diag, Severity(..))
import Hg.Support.Pretty (Doc, (<+>))
import Hg.Support.Pretty as P

-- | positions of the line breaks, with -1 in front and the length at the end
lineBreaks :: String -> Array Int
lineBreaks src =
  let
    cs = SCU.toCharArray src
    nls = A.catMaybes (A.mapWithIndex (\i c -> if c == '\n' then Just i else Nothing) cs)
  in
    A.cons (-1) (A.snoc nls (A.length cs))

-- | 0-based line and column
lineCol :: String -> Int -> { line :: Int, col :: Int }
lineCol src i = go 0
  where
  lbs = lineBreaks src
  at k = fromMaybe 0 (A.index lbs k)
  n = A.length lbs
  go l
    | l + 1 >= n - 1 = { line: l, col: i - (at l + 1) }
    | i <= at (l + 1) = { line: l, col: i - (at l + 1) }
    | otherwise = go (l + 1)

numDigits :: Int -> Int
numDigits x = if x < 10 then 1 else 1 + numDigits (x / 10)

repeated :: Int -> Char -> Doc
repeated k c = if k <= 0 then mempty else P.text (SCU.fromCharArray (A.replicate k c))

linesPretty :: String -> { start :: Int, end :: Int } -> Doc
linesPretty src sp =
  let
    lbs = lineBreaks src
    at k = fromMaybe 0 (A.index lbs k)
    ls = (lineCol src sp.start).line
    le = (lineCol src sp.end).line
    -- (the width is taken from the 0-based line numbers, as diagnostician does)
    w = max (numDigits ls) (numDigits le)
    one l =
      let
        lstart = at l + 1
        lend = at (l + 1)
        t = SCU.slice lstart lend src
        s' = max lstart sp.start
        e' = min lend sp.end
        ns = s' - lstart
        nc = max 1 (e' - s')
        ln = P.text (show (l + 1)) <> repeated (w - numDigits (l + 1)) ' '
        gutter = ln <+> P.text "|"
      in
        P.vsep [ gutter <+> P.text t, gutter <+> repeated ns ' ' <> repeated nc '^' ]
  in
    P.vsep (map one (A.range ls le))

prtCode :: Diag -> Doc
prtCode d =
  let
    s = case d.severity of
      SDebug -> { s: "debug", l: "D" }
      SWarning -> { s: "warning", l: "W" }
      SError -> { s: "error", l: "E" }
  in
    P.text s.s <> P.text "[" <> P.text s.l <> repeated (4 - numDigits d.code) '0' <> P.text (show d.code) <> P.text "]"

renderDiag :: String -> Diag -> Doc
renderDiag src d =
  P.vsep
    [ prtCode d <> P.text ": " <> d.msg
    , P.vsep (A.cons (linesPretty src d.span) (case d.note of
        Just n -> [ n ]
        Nothing -> []))
    ]

-- | what a file reporter writes: each diagnostic between hard line breaks
renderDiags :: String -> Array Diag -> String
renderDiags src ds = foldMap (\d -> "\n" <> P.render (renderDiag src d) <> "\n") ds

-- | the message and note of a diagnostic, without the source snippet (an
-- | editor shows the span itself)
renderDiagMessage :: Diag -> Doc
renderDiagMessage d = case d.note of
  Just n -> P.vsep [ d.msg, n ]
  Nothing -> d.msg
