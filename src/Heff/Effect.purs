-- | The effect of a computation (`Co h a`, lib/co.hf) as a runtime sees it:
-- | effects summed (`<+>`), and leaves -- each a global's key, `dom.dom`,
-- | whose module may hold the world's side of it, an effect provider
-- | (`dom.provider.js` beside `dom.hf`, see lib/dom.provider.js).
module Heff.Effect
  ( Effect(..)
  , leaves
  , moduleOfKey
  , nameOfKey
  ) where

import Prelude

import Data.Array as A
import Data.Maybe (fromMaybe)
import Data.String as S

data Effect = Leaf String | Plus Effect Effect

leaves :: Effect -> Array String
leaves = case _ of
  Leaf x -> [ x ]
  Plus a b -> leaves a <> leaves b

-- | the module of a leaf's key: `dom.dom` is `dom`'s, `a.b.c` is `a.b`'s
moduleOfKey :: String -> String
moduleOfKey k = S.joinWith "." (A.dropEnd 1 (S.split (S.Pattern ".") k))

-- | ... and the definition's name in it
nameOfKey :: String -> String
nameOfKey k = fromMaybe k (A.last (S.split (S.Pattern ".") k))
