-- | From the core to what a backend emits: the stages of `Hg.Lower.*`, in
-- | order.
-- |
-- |   core ─[Erase]→ IR ─[Shake]→ ─[Simplify]→ ─[Shake]→ ─[Arity]→ IR
module Hg.Lower
  ( lower
  ) where

import Prelude

import Hg.Core.Globals (Globals)
import Hg.Lower.Arity (arities, saturate)
import Hg.Lower.Erase (eraseProgram)
import Hg.Lower.IR (Def)
import Hg.Lower.Shake (shake)
import Hg.Lower.Simplify (simplifyProgram)

-- | what is reached from `roots`, lowered; what cannot be, said by key
lower :: Globals -> Array String -> { defs :: Array Def, errors :: Array String }
lower g roots =
  let
    e = eraseProgram g
    s = simplifyProgram e.supply (shake roots e.defs)
    kept = shake roots s.defs
    ar = arities kept
  in
    { defs: map (saturate ar) kept, errors: e.errors }
