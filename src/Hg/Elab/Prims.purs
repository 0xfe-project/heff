-- | The primitives: what `primitive name : T` may name.  Each has an
-- | implementation for the interpreter (on its values) and one for the
-- | compiler (PureScript source).  A primitive answering a question returns
-- | a sum with the tags 'true and 'false, whatever it is called.
module Hg.Elab.Prims
  ( primNames
  , isPrim
  , PrimImpl
  , prims
  ) where

import Prelude

import Data.Array as A
import Data.Tuple (Tuple(..))

-- | `arity` arguments; `ps` is a PureScript expression in which `TRUE` and
-- | `FALSE` stand for the constructors of the answer type
-- | `js` is the same as a JavaScript expression (`$T`/`$F` the answers).
-- | `Int` is an integer of any size (`Hg.Support.BigInt`; BigInt in JavaScript);
-- | division is floor division, and by zero it abandons
type PrimImpl = { arity :: Int, ps :: String, js :: String }

prims :: Array (Tuple String PrimImpl)
prims =
  [ Tuple "intAdd" { arity: 2, ps: "\\a b -> HgBI.add a b", js: "(a) => (b) => a + b" }
  , Tuple "intSub" { arity: 2, ps: "\\a b -> HgBI.sub a b", js: "(a) => (b) => a - b" }
  , Tuple "intMul" { arity: 2, ps: "\\a b -> HgBI.mul a b", js: "(a) => (b) => a * b" }
  , Tuple "intDiv" { arity: 2, ps: "\\a b -> HgM.fromMaybe' (\\_ -> HgRT.unsafeCrashWith \"abandon\") (HgBI.divFloor a b)", js: "(a) => (b) => { if (b === 0n) return $abandon(); const q = a / b, r = a % b; return r !== 0n && (r < 0n) !== (b < 0n) ? q - 1n : q; }" }
  , Tuple "intMod" { arity: 2, ps: "\\a b -> HgM.fromMaybe' (\\_ -> HgRT.unsafeCrashWith \"abandon\") (HgBI.modFloor a b)", js: "(a) => (b) => { if (b === 0n) return $abandon(); const r = a % b; return r !== 0n && (r < 0n) !== (b < 0n) ? r + b : r; }" }
  , Tuple "intEq" { arity: 2, ps: "\\a b -> if HgP.eq a b then TRUE else FALSE", js: "(a) => (b) => a === b ? $T : $F" }
  , Tuple "intLt" { arity: 2, ps: "\\a b -> if HgO.lessThan a b then TRUE else FALSE", js: "(a) => (b) => a < b ? $T : $F" }
  , Tuple "intLe" { arity: 2, ps: "\\a b -> if HgO.lessThanOrEq a b then TRUE else FALSE", js: "(a) => (b) => a <= b ? $T : $F" }
  , Tuple "intShow" { arity: 1, ps: "\\a -> HgBI.toString a", js: "(a) => a.toString()" }
  , Tuple "strAppend" { arity: 2, ps: "\\a b -> HgP.append a b", js: "(a) => (b) => a + b" }
  , Tuple "strLength" { arity: 1, ps: "\\a -> HgBI.fromInt (HgS.length a)", js: "(a) => BigInt(a.length)" }
  , Tuple "strSlice" { arity: 3, ps: "\\a i j -> HgS.slice (HgBI.toIntClamped i) (HgBI.toIntClamped j) a", js: "(a) => (i) => (j) => a.slice(Number(i), Number(j))" }
  , Tuple "strEq" { arity: 2, ps: "\\a b -> if HgP.eq a b then TRUE else FALSE", js: "(a) => (b) => a === b ? $T : $F" }
  , Tuple "strLt" { arity: 2, ps: "\\a b -> if HgO.lessThan a b then TRUE else FALSE", js: "(a) => (b) => a < b ? $T : $F" }
  , Tuple "strCharCode" { arity: 2, ps: "\\a i -> if HgO.lessThan i (HgBI.fromInt 0) HgP.|| HgO.greaterThanOrEq i (HgBI.fromInt (HgS.length a)) then HgBI.fromInt (HgP.negate 1) else HgBI.fromInt (HgM.maybe (HgP.negate 1) HgC.toCharCode (HgS.charAt (HgBI.toIntClamped i) a))", js: "(a) => (i) => i >= 0n && i < BigInt(a.length) ? BigInt(a.charCodeAt(Number(i))) : -1n" }
  , Tuple "strFromCharCode" { arity: 1, ps: "\\c -> if HgO.lessThan c (HgBI.fromInt 0) HgP.|| HgO.greaterThan c (HgBI.fromInt 65535) then \"\" else HgM.maybe \"\" HgS.singleton (HgC.fromCharCode (HgBI.toIntClamped c))", js: "(c) => c >= 0n && c <= 65535n ? String.fromCharCode(Number(c)) : \"\"" }
  ]

primNames :: Array String
primNames = map (\(Tuple n _) -> n) prims

isPrim :: String -> Boolean
isPrim n = A.elem n primNames
