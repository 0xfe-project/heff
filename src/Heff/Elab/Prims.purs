-- | The primitives: what `primitive name : T` may name.  Each has an
-- | implementation for the interpreter (on its values) and one for the
-- | compiler (JavaScript source).  A primitive answering a question returns
-- | a sum with the tags 'true and 'false, whatever it is called.
module Heff.Elab.Prims
  ( primNames
  , isPrim
  , PrimImpl
  , prims
  ) where

import Prelude

import Data.Array as A
import Data.Tuple (Tuple(..))

-- | `arity` arguments; `js` is a JavaScript expression (`$T`/`$F` the
-- | answers).
-- | `Int` is an integer of any size (`Heff.Support.BigInt`; BigInt in JavaScript);
-- | division is floor division, and by zero it abandons
type PrimImpl = { arity :: Int, js :: String }

prims :: Array (Tuple String PrimImpl)
prims =
  [ Tuple "intAdd" { arity: 2, js: "(a) => (b) => a + b" }
  , Tuple "intSub" { arity: 2, js: "(a) => (b) => a - b" }
  , Tuple "intMul" { arity: 2, js: "(a) => (b) => a * b" }
  , Tuple "intDiv" { arity: 2, js: "(a) => (b) => { if (b === 0n) return $abandon(); const q = a / b, r = a % b; return r !== 0n && (r < 0n) !== (b < 0n) ? q - 1n : q; }" }
  , Tuple "intMod" { arity: 2, js: "(a) => (b) => { if (b === 0n) return $abandon(); const r = a % b; return r !== 0n && (r < 0n) !== (b < 0n) ? r + b : r; }" }
  , Tuple "intEq" { arity: 2, js: "(a) => (b) => a === b ? $T : $F" }
  , Tuple "intLt" { arity: 2, js: "(a) => (b) => a < b ? $T : $F" }
  , Tuple "intLe" { arity: 2, js: "(a) => (b) => a <= b ? $T : $F" }
  , Tuple "intShow" { arity: 1, js: "(a) => a.toString()" }
  , Tuple "strAppend" { arity: 2, js: "(a) => (b) => a + b" }
  , Tuple "strLength" { arity: 1, js: "(a) => BigInt(a.length)" }
  , Tuple "strSlice" { arity: 3, js: "(a) => (i) => (j) => a.slice(Number(i), Number(j))" }
  , Tuple "strEq" { arity: 2, js: "(a) => (b) => a === b ? $T : $F" }
  , Tuple "strLt" { arity: 2, js: "(a) => (b) => a < b ? $T : $F" }
  , Tuple "strCharCode" { arity: 2, js: "(a) => (i) => i >= 0n && i < BigInt(a.length) ? BigInt(a.charCodeAt(Number(i))) : -1n" }
  , Tuple "strFromCharCode" { arity: 1, js: "(c) => c >= 0n && c <= 65535n ? String.fromCharCode(Number(c)) : \"\"" }
  ]

primNames :: Array String
primNames = map (\(Tuple n _) -> n) prims

isPrim :: String -> Boolean
isPrim n = A.elem n primNames
