-- | A port of `Hippogriff.Core.Evaluation`: syntax is compiled, once, into a
-- | function from the local environment to a value.
module Hg.Core.Evaluation where


import Data.Lazy (defer)
import Data.Maybe (Maybe(..))
import Hg.Support.Common (dictMap)
import Data.Array as A
import Hg.Core.Globals (Globals, elemAtGlobal)
import Hg.Core.Params (Case(..), Variants, variantsMap)
import Hg.Core.Syntax as S
import Hg.Core.Value as V

type EvalEnv = { local :: V.Locals, global :: Globals }

eval :: EvalEnv -> Case -> S.El -> V.Ev V.El
eval e c s = compile e.global c s e.local

evalTy :: EvalEnv -> Case -> S.Ty -> V.Ev V.Ty
evalTy e c s = compileTy e.global c s e.local

compileAbs :: forall a b. (a -> V.Locals -> V.Ev b) -> S.Abs a -> V.Locals -> V.Clo b
compileAbs comp (S.Abs x t) =
  let k = comp t
  in \vs -> V.Clo x vs k
compileAbs comp (S.AbsConst t) =
  let k = comp t
  in \vs -> V.CloConst (k vs)

compile :: Globals -> Case -> S.El -> V.Locals -> V.Ev V.El
compile g c = case _ of
  S.Local i -> \vs -> V.Nom (V.localsAt vs i)
  S.Global x -> \_ -> V.Nom (elemAtGlobal g x).value
  S.Code a ->
    let k = compileTy g c a
    in \vs -> V.emap V.code (k vs)
  S.App t0 t1 ->
    let
      k0 = compile g N t0
      k1 = compile g N t1
    in
      \vs -> V.ebind (\f -> V.app f (defer \_ -> V.nom (k1 vs))) (k0 vs)
  S.Lam dom abs ->
    let
      kDom = compileTy g N dom
      kClo = compileAbs (compile g c) abs
    in
      \vs -> V.epure c (V.Lam (defer \_ -> V.nom (kDom vs)) (kClo vs))
  S.Cons fields ->
    let kFields = dictMap (compile g c) fields
    in \vs -> V.epure c (V.Cons (dictMap (\k -> k vs) kFields))
  S.Proj t x ->
    let k = compile g N t
    in \vs -> V.ebind (\v -> V.proj v x) (k vs)
  S.Tag _ _ -> \_ -> V.Nom V.Opaque
  S.Match _ _ -> \_ -> V.Nom V.Opaque
  S.Specialize t x t' ->
    let
      k = compile g N t
      k' = compile g N t'
    in
      \vs -> V.Nom (V.specialize (V.nom (k vs)) x (V.nom (k' vs)))
  S.Opaque -> \_ -> V.Nom V.Opaque
  S.Abandon -> \_ -> V.Nom V.Opaque
  -- guarded recursion: at a small type its value is ignored statically, which
  -- is also what keeps a direct self-call from unfolding forever
  S.Rec _ -> \_ -> V.Nom V.Opaque
  S.MetaApp m idxs ty ->
    let kTy = compileTy g N ty
    in \vs -> V.Nom (V.metaEl m (A.foldl (\sp i -> V.App sp (V.localsAt vs i)) V.Id idxs) (V.nom (kTy vs)))
  S.Is t ->
    let k = compile g N t
    in \vs -> V.Become (V.nom (k vs))
  S.LitInt _ -> \_ -> V.Nom V.Opaque
  S.LitString _ -> \_ -> V.Nom V.Opaque
  S.Implicit _ e -> compile g c e
  S.Prim _ -> \_ -> V.Nom V.Opaque

compileFunctionType :: Globals -> S.FunctionType -> V.Locals -> V.FunctionType
compileFunctionType g ft =
  let
    kDom = compileTy g N ft.dom
    kCod = compileAbs (compileTy g N) ft.cod
  in
    \vs -> { level: ft.level, dom: V.nom (kDom vs), cod: kCod vs, implicit: ft.implicit }

compileRecordType :: Globals -> S.RecordType -> V.Locals -> V.RecordType
compileRecordType g rt =
  let
    kFieldTypes = dictMap (\t -> let k = compileTy g N t in \vs -> { ty: V.nom (k vs), value: Nothing }) rt.fieldTypes
  in
    \vs -> { level: rt.level, capture: vs, fieldTypes: kFieldTypes }

compileSumType :: Globals -> Variants S.Ty -> V.Locals -> V.SumType
compileSumType g st =
  let kVariants = variantsMap (compileTy g N) st
  in \vs -> variantsMap (\k -> defer \_ -> V.nom (k vs)) kVariants

compileTy :: Globals -> Case -> S.Ty -> V.Locals -> V.Ev V.Ty
compileTy g c = case _ of
  S.U u -> \_ -> V.Nom (V.U u)
  S.Decode t ->
    let k = compile g c t
    in \vs -> V.decodeEv (k vs)
  S.Function ft ->
    let k = compileFunctionType g ft
    in \vs -> V.Nom (V.Function (k vs))
  S.Record rt ->
    let k = compileRecordType g rt
    in \vs -> V.Describe (V.Record (k vs))
  S.Sum st ->
    let k = compileSumType g st
    in \vs -> V.Describe (V.Sum (k vs))
  S.IsTy t ->
    let k = compileTy g N t
    in \vs -> V.Become (V.nom (k vs))
  S.PrimInt -> \_ -> V.Nom V.PrimInt
  S.PrimString -> \_ -> V.Nom V.PrimString
