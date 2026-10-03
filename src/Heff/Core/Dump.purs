-- | Core syntax, printed as the heff source that constructs it over
-- | `compiler/syntax.hf`'s formers -- character for character what
-- | `compiler/print.hf` prints.  The NbE differential (`test/nbe-port.mjs`)
-- | embeds the dump as input and compares the normal forms as these
-- | strings.
module Heff.Core.Dump (dumpEl, dumpTy, dumpDef) where

import Prelude

import Data.Array as A
import Data.Lazy (defer)
import Data.Maybe (Maybe(..))
import Data.String as S
import Data.Tuple (Tuple(..))
import Heff.Core.Evaluation (eval)
import Heff.Core.Globals (GlobalEntry, Globals)
import Heff.Core.Params (Case(..), Level(..), Plicity(..), Universe(..), Variants(..))
import Heff.Core.Readback (readbEv, readbTy)
import Heff.Core.Syntax as Sx
import Heff.Core.Value as V
import Heff.Support.BigInt as BI
import Heff.Support.Common (Dict, Name, dictToList, quoteString)

foreign import showNumberImpl :: Number -> String

showI :: Int -> String
showI n = if n < 0 then "(0 - " <> show (-n) <> ")" else show n

showLevel :: Level -> String
showLevel = case _ of
  Small -> "Small"
  Large -> "Large"
  Top -> "TopL"

showUniverse :: Universe -> String
showUniverse = case _ of
  SmallU -> "SmallU"
  LargeU -> "LargeU"

showBool :: Boolean -> String
showBool b = if b then "True" else "False"

showPlicityB :: Plicity -> String
showPlicityB = case _ of
  Expl -> "False"
  Impl -> "True"
  Inst -> "Inst"

listOf :: Array String -> String
listOf xs = "[" <> S.joinWith ", " xs <> "]"

arrOf :: forall a. (a -> String) -> Array a -> String
arrOf f xs = "(A.fromList " <> listOf (map f xs) <> ")"

dictOf :: forall a. (a -> String) -> Dict a -> String
dictOf f d = "(D.fromList " <> listOf (map (\(Tuple k v) -> "(MkPair " <> quoteString k <> " " <> f v <> ")") (dictToList d)) <> ")"

absOf :: forall a. (a -> String) -> Sx.Abs a -> String
absOf f = case _ of
  Sx.Abs x b -> "(MkAbs " <> quoteString x <> " " <> f b <> ")"
  Sx.AbsConst b -> "(AbsConst " <> f b <> ")"

armOf :: Tuple (Array Name) Sx.El -> String
armOf (Tuple xs b) = "(MkPair " <> arrOf quoteString xs <> " " <> dumpEl b <> ")"

dumpEl :: Sx.El -> String
dumpEl = case _ of
  Sx.Local i -> "(SLocal " <> showI i <> ")"
  Sx.Global x -> "(SGlobal " <> quoteString x <> ")"
  Sx.Code a -> "(SCode " <> dumpTy a <> ")"
  Sx.App f a -> "(SApp " <> dumpEl f <> " " <> dumpEl a <> ")"
  Sx.Lam a b -> "(SLam " <> dumpTy a <> " " <> absOf dumpEl b <> ")"
  Sx.Cons d -> "(SCons " <> dictOf dumpEl d <> ")"
  Sx.Proj t x -> "(SProj " <> dumpEl t <> " " <> quoteString x <> ")"
  Sx.Tag x ts -> "(STag " <> quoteString x <> " " <> arrOf dumpEl ts <> ")"
  Sx.Match t arms -> "(SMatch " <> dumpEl t <> " " <> dictOf armOf arms <> ")"
  Sx.Specialize t x t' -> "(SSpecialize " <> dumpEl t <> " " <> quoteString x <> " " <> dumpEl t' <> ")"
  Sx.Is t -> "(SIs " <> dumpEl t <> ")"
  Sx.Opaque -> "SOpaque"
  Sx.Abandon -> "SAbandon"
  Sx.Rec t -> "(SRec " <> dumpEl t <> ")"
  Sx.Implicit x t -> "(SImplicit " <> quoteString x <> " " <> dumpEl t <> ")"
  Sx.Prim x -> "(SPrim " <> quoteString x <> ")"
  Sx.MetaApp m idxs a -> "(SMetaApp " <> showI m <> " " <> arrOf showI idxs <> " " <> dumpTy a <> ")"
  Sx.LitInt n -> "(SLitInt " <> (let s = BI.toString n in if S.take 1 s == "-" then "(0 - " <> S.drop 1 s <> ")" else s) <> ")"
  Sx.LitFloat x -> "(SLitFloat " <> showNumberImpl x <> ")"
  Sx.LitString s -> "(SLitString " <> quoteString s <> ")"

dumpTy :: Sx.Ty -> String
dumpTy = case _ of
  Sx.U u -> "(SU " <> showUniverse u <> ")"
  Sx.Decode t -> "(SDecode " <> dumpEl t <> ")"
  -- (the port prints the flag as a boolean still; `Inst` is spike-only)
  Sx.Function ft -> "(SFunction " <> showLevel ft.level <> " " <> dumpTy ft.dom <> " " <> absOf dumpTy ft.cod <> " " <> showPlicityB ft.implicit <> ")"
  Sx.Record rt -> "(SRecord " <> showLevel rt.level <> " " <> dictOf dumpTy rt.fieldTypes <> ")"
  Sx.Sum (Variants d) -> "(SSum " <> dictOf (arrOf dumpTy) d <> ")"
  Sx.IsTy t -> "(SIsTy " <> dumpTy t <> ")"
  Sx.PrimInt -> "SPrimInt"
  Sx.PrimFloat -> "SPrimFloat"
  Sx.PrimString -> "SPrimString"
  Sx.PrimArray a -> "(SPrimArray " <> dumpTy a <> ")"

-- | is a stored body a primitive's (no definition to unfold)?
isPrimBody :: Sx.El -> Boolean
isPrimBody = case _ of
  Sx.Prim _ -> true
  _ -> false

-- | one definition, as the differential reads it: its key, whether it is
-- | a primitive, its type and body as constructor source, and the normal
-- | forms the PureScript side computes -- the type evaluated and read
-- | back, the body evaluated at case D with an opaque self and read back
dumpDef :: Globals -> Name -> GlobalEntry -> String
dumpDef g key e =
  let
    self = V.reflect (V.Global key) (pure V.Id) (pure e.type_) Nothing
    nf = readbEv 0 (eval { local: V.LSnoc V.LNil (defer \_ -> self), global: g } D e.syntax)
  in
    S.joinWith "\n"
      [ "=def " <> key <> " " <> (if isPrimBody e.syntax then "prim" else "plain")
      , "=ty " <> dumpTy e.tySyntax
      , "=body " <> dumpEl e.syntax
      , "=normTy " <> dumpTy (readbTy 0 e.type_)
      , "=normEl " <> dumpEl nf
      ]
