-- | From the core to what a backend emits: the stages of `Heff.Lower.*`, in
-- | order.
-- |
-- |   core ─[Erase]→ IR ─[Shake]→ ─[Simplify]→ ─[Shake]→ ─[Arity]→ IR
module Heff.Lower
  ( lower
  ) where

import Prelude

import Heff.Core.Globals (Globals)
import Heff.Lower.Arity (arities, saturate)
import Heff.Lower.Erase (eraseProgram)
import Heff.Lower.IR (Def)
import Heff.Lower.Shake (shake)
import Heff.Lower.Simplify (simplifyProgram)

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
