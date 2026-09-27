-- | A port of `Hippogriff.Core.Conversion`.  There is no type-directed
-- | conversion: every element of a small type is `Opaque` already, and
-- | `Opaque` is equal to anything.
module Hg.Core.Conversion
  ( DefEqCheckError(..)
  , DefEqM
  , defEqTy
  , defEqEl
  , defEqBare
  , prettyDefEqError
  ) where

import Prelude

import Data.Array as A
import Data.Either (Either(..))
import Data.Lazy (defer, force)
import Data.List ((:))
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Traversable (sequence_)
import Hg.Support.Common (dictMap, panic)
import Hg.Core.Params (CtxShape)
import Hg.Core.Print (prtBareR, prtElR, prtTyR)
import Hg.Core.Value as V
import Hg.Support.Pretty (Doc, (<+>))
import Hg.Support.Pretty as P

data DefEqCheckError
  = UnequalTys CtxShape V.Ty V.Ty (Maybe Doc)
  | UnequalEls CtxShape V.El V.El (Maybe Doc)
  | UnequalNeus CtxShape V.BareNeutral V.BareNeutral (Maybe Doc)

type DefEqM = Either DefEqCheckError Unit

prettyDefEqError :: (String -> String) -> DefEqCheckError -> Doc
prettyDefEqError rn = case _ of
  UnequalTys cs a a' r -> P.text "types" <+> prtTyR rn cs a <+> P.text "and" <+> prtTyR rn cs a' <+> P.text "are not equal" <+> because r
  UnequalEls cs a a' r -> P.text "elements" <+> prtElR rn cs a <+> P.text "and" <+> prtElR rn cs a' <+> P.text "are not equal" <+> because r
  UnequalNeus cs a a' r -> P.text "neutrals" <+> prtBareR rn cs a <+> P.text "and" <+> prtBareR rn cs a' <+> P.text "are not equal" <+> because r
  where
  because = case _ of
    Nothing -> mempty
    Just d -> P.text "because" <+> d

ok :: DefEqM
ok = Right unit

unless' :: Boolean -> DefEqM -> DefEqM
unless' b m = if b then ok else m

defEqCloTy :: CtxShape -> V.Ty -> V.Clo V.Ty -> V.Clo V.Ty -> DefEqM
defEqCloTy cs a c0 c1 =
  let
    n = fromMaybe "x" (case V.cloName c0 of
      Just x -> Just x
      Nothing -> V.cloName c1)
    cs' = { len: cs.len + 1, names: n : cs.names }
    v = V.local cs.len a
  in
    defEqTy cs' (V.nom (V.appClo c0 (pure v))) (V.nom (V.appClo c1 (pure v)))

defEqCloEl :: CtxShape -> V.Ty -> V.Clo V.El -> V.Clo V.El -> DefEqM
defEqCloEl cs a c0 c1 =
  let
    n = fromMaybe "x" (case V.cloName c0 of
      Just x -> Just x
      Nothing -> V.cloName c1)
    cs' = { len: cs.len + 1, names: n : cs.names }
    v = V.local cs.len a
  in
    defEqEl cs' (V.nom (V.appClo c0 (pure v))) (V.nom (V.appClo c1 (pure v)))

defEqTy :: CtxShape -> V.Ty -> V.Ty -> DefEqM
defEqTy cs a a' = case a, a' of
  V.U u, V.U u' | u == u' -> ok
  V.Decode n, V.Decode n' -> defEqBare cs (V.toBare n) (V.toBare n')
  V.Function f, V.Function f' -> do
    unless' (f.level == f'.level) $
      Left (UnequalTys cs a a' (Just (P.text "different function levels:" <+> P.text (show f.level) <+> P.text "and" <+> P.text (show f'.level))))
    defEqTy cs f.dom f'.dom
    defEqCloTy cs f.dom f.cod f'.cod
  V.PrimInt, V.PrimInt -> ok
  V.PrimString, V.PrimString -> ok
  _, _ -> Left (UnequalTys cs a a' Nothing)

defEqHead :: CtxShape -> V.Head -> V.Head -> DefEqM
defEqHead cs h h' =
  if h == h' then ok
  else Left (UnequalNeus cs { head: h, spine: V.Id } { head: h', spine: V.Id } Nothing)

defEqBare :: CtxShape -> V.BareNeutral -> V.BareNeutral -> DefEqM
defEqBare cs n n' = case n.spine, n'.spine of
  V.Id, V.Id -> defEqHead cs n.head n'.head
  V.App sq v, V.App sq' v' -> do
    defEqBare cs (n { spine = sq }) (n' { spine = sq' })
    defEqEl cs v v'
  V.Proj sq x, V.Proj sq' x' -> do
    defEqBare cs (n { spine = sq }) (n' { spine = sq' })
    unless' (x == x') (Left (UnequalNeus cs n n' Nothing))
  V.Specialize sq x v, V.Specialize sq' x' v' -> do
    defEqBare cs (n { spine = sq }) (n' { spine = sq' })
    unless' (x == x') (Left (UnequalNeus cs n n' Nothing))
    defEqEl cs v v'
  _, _ -> Left (UnequalNeus cs n n' Nothing)

-- | eta: a neutral of record type is its fields, of function type a lambda
canon :: V.El -> V.El
canon v = case v of
  V.Neu n -> case V.behavior (force n.ty) of
    V.LikeRecord _ -> case force n.expansion of
      V.IntoCons fields -> V.Cons (dictMap V.Nom fields)
      V.NotApplicable -> panic "neutral of record type was never expanded"
    V.LikeFunction _ -> V.Lam n.ty (V.Clo "x" V.LNil (\w -> V.app v (defer \_ -> V.localsAt w 0)))
    _ -> v
  _ -> v

defEqEl :: CtxShape -> V.El -> V.El -> DefEqM
defEqEl _ V.Opaque _ = ok
defEqEl _ _ V.Opaque = ok
defEqEl cs v v' = case canon v, canon v' of
  V.Neu n, V.Neu n' -> defEqBare cs (V.toBare n) (V.toBare n')
  V.Neu _, _ -> Left (UnequalEls cs v v' Nothing)
  V.Code a, V.Code a' -> defEqTy cs a a'
  V.Code _, _ -> Left (UnequalEls cs v v' Nothing)
  V.Lam a c, V.Lam _ c' -> defEqCloEl cs (V.lamDom (force a)) c c'
  V.Lam _ _, _ -> Left (UnequalEls cs v v' Nothing)
  V.Cons d, V.Cons d' -> sequence_ (A.zipWith (\x y -> defEqEl cs (V.nom x) (V.nom y)) d.values d'.values)
  V.Cons _, _ -> Left (UnequalEls cs v v' Nothing)
  V.Opaque, _ -> panic "should have already taken care of opaque"
