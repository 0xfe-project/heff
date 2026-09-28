-- | Core syntax.  A port of `Hippogriff.Core.Syntax`; the case index is
-- | erased (see `Heff.Core.Params.Case`).
module Heff.Core.Syntax where

import Heff.Support.BigInt (BigInt)
import Heff.Support.Common (Dict, Name)
import Heff.Core.Params (Level, Universe, Variants)
import Data.Tuple (Tuple)

-- | abstractions (syntactic closures)
data Abs a = Abs Name a | AbsConst a

data El
  = Local Int -- ^ de Bruijn index
  | Global Name
  | Code Ty
  | App El El
  | Lam Ty (Abs El)
  | Cons (Dict El)
  | Proj El Name
  | Tag Name (Array El)
  | Match El (Dict (Tuple (Array Name) El))
  | Specialize El Name El
  | Is El
  | Opaque
  | Abandon
  | Rec El -- ^ `rec e`: statically ■ (the paper's semantics), dynamically `e`
  | Implicit Name El -- ^ an argument the elaborator gave for the implicit binder `x`: it is just the argument, the tools only print it so (or leave it out)
  | Prim String -- ^ extension: a primitive, implemented by the runtime (see `Heff.Elab.Prims`)
  | MetaApp Int (Array Int) Ty -- ^ an implicit argument being inferred: a metavariable applied to some locals (by index), at a type; zonked away before a definition is stored
  | LitInt BigInt
  | LitFloat Number
  | LitString String

type FunctionType = { level :: Level, dom :: Ty, cod :: Abs Ty, implicit :: Boolean }

type RecordType = { level :: Level, fieldTypes :: Dict Ty }

type SumType = Variants Ty

data Ty
  = U Universe
  | Decode El
  | Function FunctionType
  | Record RecordType
  | Sum SumType
  | IsTy Ty
  | PrimInt
  | PrimFloat
  | PrimString
  | PrimArray Ty
