-- | A stored definition printed back as equations, when it has their shape:
-- | lambdas, then `match`es on what they bind (and on what those bind).  The
-- | inverse of `Heff.Elab.Clauses`, so an equation-defined function reads
-- | as it was written.
module Heff.Print.Resugar
  ( equationsWith
  , Style
  ) where

import Prelude

import Data.Array as A
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as S
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Name, dictToList)
import Heff.Core.Syntax as Syn
import Heff.Support.Pretty as P

-- | a pattern: a variable (by de Bruijn level) or a tag of patterns
data Pat = PVar Int | PTag Name (Array Pat)

type Row = { pats :: Array Pat, names :: List Name, body :: Syn.El }

-- | how equations are written: a tag in a pattern, what separates the sides,
-- | how many equations it takes to write a definition so (one: any function
-- | is `f x = …`), and a body
type Style = { tag :: Name -> String, sep :: String, minRows :: Int, body :: List Name -> Syn.El -> String }

equationsWith :: Style -> Name -> List Name -> Syn.Ty -> Syn.El -> Maybe (Array String)
equationsWith style f names0 ty0 el =
  let
    lam = lambdas names0 [] ty0 el
    rows = go lam.names lam.pats lam.body
  in
    if A.null lam.pats || A.length rows < style.minRows then Nothing
    else Just (map (\r -> A.intercalate " " (A.cons f (map (atom r.names) r.pats)) <> " " <> style.sep <> " " <> style.body r.names r.body) rows)
  where
  -- the lambdas of the explicit arguments (the type says which are); an
  -- implicit one ends the equations' shape
  lambdas ns ps ty b = case ty, b of
    -- an implicit argument is not written in the equations: it is in scope
    Syn.Function ft, Syn.Lam _ (Syn.Abs x body) | ft.implicit -> case ft.cod of
      Syn.Abs _ cod -> lambdas (x : ns) ps cod body
      Syn.AbsConst cod -> lambdas (x : ns) ps cod body
    Syn.Function ft, Syn.Lam _ abs | not ft.implicit ->
      let
        x = case abs of
          Syn.Abs y _ -> y
          Syn.AbsConst _ -> "_"
        body = case abs of
          Syn.Abs _ e -> e
          Syn.AbsConst e -> e
        bound = case abs of
          Syn.Abs _ _ -> true
          Syn.AbsConst _ -> false
        cod = case ft.cod of
          Syn.Abs _ t -> t
          Syn.AbsConst t -> t
      in
        if bound then lambdas (x : ns) (A.snoc ps (PVar (L.length ns))) cod body
        else { names: ns, pats: [], body: b } -- `_ => e`: not by cases
    _, Syn.Is e | isLam e -> lambdas ns ps ty e
    _, _ -> { names: ns, pats: ps, body: b }

  -- refine the patterns by every `match` on one of their variables
  go :: List Name -> Array Pat -> Syn.El -> Array Row
  go ns ps body = case body of
    -- `is` marks where a description is made; equations do not show it
    Syn.Is e | A.length ps > 0 && isMatch e -> go ns ps e
    Syn.Match (Syn.Local i) arms
      | lvl <- L.length ns - 1 - i
      , A.any (hasVar lvl) ps ->
          A.concatMap
            ( \(Tuple t (Tuple xs b)) ->
                let
                  base = L.length ns
                  sub = A.mapWithIndex (\k _ -> PVar (base + k)) xs
                  ns' = A.foldl (\acc x -> x : acc) ns xs
                in
                  go ns' (map (refine lvl (PTag t sub)) ps) b
            )
            (dictToList arms)
    _ -> [ { pats: ps, names: ns, body } ]

  isLam = case _ of
    Syn.Lam _ _ -> true
    _ -> false

  isMatch = case _ of
    Syn.Match _ _ -> true
    _ -> false

  hasVar l = case _ of
    PVar l' -> l == l'
    PTag _ qs -> A.any (hasVar l) qs

  refine l p = case _ of
    PVar l' | l == l' -> p
    PTag t qs -> PTag t (map (refine l p) qs)
    q -> q

  -- a pattern in argument position
  atom ns = case _ of
    PVar l -> fromMaybe "_" (L.index ns (L.length ns - 1 - l))
    PTag t [] -> style.tag t
    PTag t qs -> "(" <> style.tag t <> " " <> S.joinWith " " (map (atom ns) qs) <> ")"
