-- | Only what is reached from the definitions asked for is kept.
module Hg.Lower.Shake
  ( shake
  ) where

import Prelude

import Data.Array as A
import Data.Map as Map
import Data.Maybe (Maybe(..), maybe)
import Data.Set as Set
import Data.Tuple (Tuple(..))
import Hg.Lower.IR (Def, globalsIn)

shake :: Array String -> Array Def -> Array Def
shake roots defs = A.filter (\d -> Set.member d.key reached) defs
  where
  bodies = Map.fromFoldable (map (\d -> Tuple d.key d.body) defs)
  go seen ks = case A.uncons ks of
    Nothing -> seen
    Just { head, tail }
      | Set.member head seen -> go seen tail
      | otherwise -> go (Set.insert head seen) (tail <> maybe [] globalsIn (Map.lookup head bodies))
  reached = go Set.empty roots
