-- | Calls with all their arguments at once: a definition that is a
-- | function of `n` parameters is called directly (`Call`) wherever it is
-- | given `n` arguments or more; only where it is passed on, or given fewer,
-- | is it the curried function.  The same for primitives.
module Hg.Lower.Arity
  ( arities
  , saturate
  ) where

import Prelude

import Data.Array as A
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..))
import Data.Tuple (Tuple(..))
import Hg.Elab.Prims (prims)
import Hg.Lower.IR (Def, Expr(..), spine)

-- | the definitions that are functions, and of how many parameters
arities :: Array Def -> Map String Int
arities defs = Map.fromFoldable (A.mapMaybe (\d -> case d.body of
  Lam xs _ -> Just (Tuple d.key (A.length xs))
  _ -> Nothing) defs)

saturate :: Map String Int -> Def -> Def
saturate ar d = d { body = go d.body }
  where
  primArity p = (\(Tuple _ i) -> i.arity) <$> A.find (\(Tuple k _) -> k == p) prims
  go = case _ of
    e@(App _ _) ->
      let
        Tuple h as0 = spine e
        as = map go as0
        call n mk
          | A.length as >= n = rest (mk (A.take n as)) (A.drop n as)
          | otherwise = App (go h) as
      in
        case h of
          Global g | Just n <- Map.lookup g ar -> call n (Call g)
          Prim p | Just n <- primArity p -> call n (PrimCall p)
          _ -> App (go h) as
    Lam xs b -> Lam xs (go b)
    Let x a b -> Let x (go a) (go b)
    Record fs -> Record (map (map go) fs)
    Proj e l -> Proj (go e) l
    Tag t as -> Tag t (map go as)
    Match s arms -> Match (go s) (map (\a -> a { body = go a.body }) arms)
    PrimCall p as -> PrimCall p (map go as)
    Call g as -> Call g (map go as)
    e -> e
  rest f as = if A.null as then f else App f as
