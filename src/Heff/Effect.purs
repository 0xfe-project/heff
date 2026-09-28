-- | The effect of a computation (`Co h a`, lib/co.hf) as a runtime sees it:
-- | effects summed (`<+>`), and the library's that the runtimes have a
-- | handler for, `dom`, `io` and `state s`.
module Heff.Effect
  ( Effect(..)
  , leaves
  ) where

import Prelude

data Effect = Leaf String | Plus Effect Effect

leaves :: Effect -> Array String
leaves = case _ of
  Leaf x -> [ x ]
  Plus a b -> leaves a <> leaves b
