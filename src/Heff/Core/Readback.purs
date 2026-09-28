-- | A port of `Hippogriff.Core.Readback`.
module Heff.Core.Readback where

import Prelude

import Data.Array as A
import Data.Maybe (Maybe(..))
import Data.Lazy (force)
import Data.List (List(..), (:))
import Heff.Support.Common (dictMap)
import Heff.Core.Params (Universe(..), variantsMap)
import Heff.Core.Syntax as S
import Heff.Core.Value as V

type CtxLen = Int

readbHead :: CtxLen -> V.Head -> S.El
readbHead n = case _ of
  V.Local i -> S.Local (n - i - 1)
  V.Global x -> S.Global x
  V.Meta m -> S.MetaApp m [] (S.U SmallU)

readbSpine :: CtxLen -> V.Spine -> S.El -> S.El
readbSpine n = case _ of
  V.Id -> \t -> t
  V.App sp v -> \t -> S.App (readbSpine n sp t) (readbEl n v)
  V.Proj sp x -> \t -> S.Proj (readbSpine n sp t) x
  V.Specialize sp x v -> \t -> S.Specialize (readbSpine n sp t) x (readbEl n v)

readbBare :: CtxLen -> V.BareNeutral -> S.El
readbBare n ne = case ne.head of
  -- a metavariable is applied to locals: its syntax lists them (so that
  -- evaluating it again gives the same flexible neutral)
  V.Meta m | Just idxs <- localArgs ne.spine -> S.MetaApp m (map (\l -> n - l - 1) idxs) (S.U SmallU)
  _ -> readbSpine n ne.spine (readbHead n ne.head)
  where
  localArgs = case _ of
    V.Id -> Just []
    V.App sp (V.Neu { head: V.Local l, spine: V.Id }) -> (\xs -> A.snoc xs l) <$> localArgs sp
    _ -> Nothing

-- | an evaluation read back as syntax of the same case
readbEv :: CtxLen -> V.Ev V.El -> S.El
readbEv n = case _ of
  V.Nom v -> readbEl n v
  V.Describe v -> readbEl n v
  V.Become v -> S.Is (readbEl n v)

readbEvTy :: CtxLen -> V.Ev V.Ty -> S.Ty
readbEvTy n = case _ of
  V.Nom a -> readbTy n a
  V.Describe a -> readbTy n a
  V.Become a -> S.IsTy (readbTy n a)

readbCloEl :: CtxLen -> V.Ty -> V.Clo V.El -> S.Abs S.El
readbCloEl n dom = case _ of
  V.Clo x l f -> S.Abs x (readbEv (n + 1) (f (V.snoc l (V.local n dom))))
  V.CloConst t -> S.AbsConst (readbEv n t)

readbCloTy :: CtxLen -> V.Ty -> V.Clo V.Ty -> S.Abs S.Ty
readbCloTy n dom = case _ of
  V.Clo x l f -> S.Abs x (readbEvTy (n + 1) (f (V.snoc l (V.local n dom))))
  V.CloConst t -> S.AbsConst (readbEvTy n t)

readbEl :: CtxLen -> V.El -> S.El
readbEl n = case _ of
  V.Neu ne -> readbBare n (V.toBare ne)
  V.Code a -> S.Code (readbTy n a)
  V.Lam fty body -> S.Lam (readbTy n (force fty)) (readbCloEl n (V.lamDom (force fty)) body)
  V.Cons d -> S.Cons (dictMap (readbEv n) d)
  V.Opaque -> S.Opaque

readbFunctionType :: CtxLen -> V.FunctionType -> S.FunctionType
readbFunctionType n f =
  { level: f.level
  , dom: readbTy n f.dom
  , cod: readbCloTy n f.dom f.cod
  , implicit: f.implicit
  }

readbRecordType :: CtxLen -> V.RecordType -> S.RecordType
readbRecordType n r =
  { level: r.level
  , fieldTypes:
      { keys: r.fieldTypes.keys
      , byName: r.fieldTypes.byName
      , values: A.fromFoldable (go n r.capture (A.toUnfoldable r.fieldTypes.values))
      }
  }
  where
  go i ls fs = case fs of
    Nil -> Nil
    f : rest ->
      let ty = (f ls).ty
      in readbTy i ty : go (i + 1) (V.snoc ls (V.local i ty)) rest

readbTy :: CtxLen -> V.Ty -> S.Ty
readbTy n = case _ of
  V.U u -> S.U u
  V.Decode ne -> S.Decode (readbSpine n ne.spine (readbHead n ne.head))
  V.Function ft -> S.Function (readbFunctionType n ft)
  V.Record rt -> S.Record (readbRecordType n rt)
  V.Sum st -> S.Sum (variantsMap (readbTy n <<< force) st)
  V.PrimInt -> S.PrimInt
  V.PrimFloat -> S.PrimFloat
  V.PrimString -> S.PrimString
  V.PrimArray a -> S.PrimArray (readbTy n a)
