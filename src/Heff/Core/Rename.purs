-- | Renaming the globals inside syntax: how the keys of other modules'
-- | definitions are shown by the names they have in a file.
module Heff.Core.Rename
  ( renameEl
  , renameTy
  , globalsInEl
  , globalsInTy
  , selfAsLocal
  , describeKey
  , displayName
  ) where

import Prelude

import Data.Array as A
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as Str
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Name, dictMap, dictValues)
import Heff.Core.Globals (Globals)
import Heff.Core.Params (Variants(..))
import Heff.Core.Syntax as S


renameAbs :: forall a. (a -> a) -> S.Abs a -> S.Abs a
renameAbs f = case _ of
  S.Abs x t -> S.Abs x (f t)
  S.AbsConst t -> S.AbsConst (f t)

renameEl :: (Name -> Name) -> S.El -> S.El
renameEl r = go
  where
  go = case _ of
    S.Global x -> S.Global (r x)
    S.Code t -> S.Code (renameTy r t)
    S.App f a -> S.App (go f) (go a)
    S.Lam d b -> S.Lam (renameTy r d) (renameAbs go b)
    S.Cons d -> S.Cons (dictMap go d)
    S.Proj e l -> S.Proj (go e) l
    S.Tag t as -> S.Tag t (map go as)
    S.Match e cs -> S.Match (go e) (dictMap (\(Tuple xs b) -> Tuple xs (go b)) cs)
    S.Specialize e l v -> S.Specialize (go e) l (go v)
    S.Is e -> S.Is (go e)
    S.Rec e -> S.Rec (go e)
    S.Implicit x e -> S.Implicit x (go e)
    S.MetaApp m xs t -> S.MetaApp m xs (renameTy r t)
    other -> other

renameTy :: (Name -> Name) -> S.Ty -> S.Ty
renameTy r = go
  where
  go = case _ of
    S.Decode e -> S.Decode (renameEl r e)
    S.Function f -> S.Function { level: f.level, dom: go f.dom, cod: renameAbs go f.cod, implicit: f.implicit }
    S.Record rt -> S.Record { level: rt.level, fieldTypes: dictMap go rt.fieldTypes }
    S.Sum (Variants vs) -> S.Sum (Variants (dictMap (map go) vs))
    S.IsTy t -> S.IsTy (go t)
    S.PrimArray t -> S.PrimArray (go t)
    other -> other


-- | the globals a term mentions, in order of first mention, each once
globalsInEl :: S.El -> Array Name
globalsInEl = A.nub <<< go
  where
  abs f = case _ of
    S.Abs _ t -> f t
    S.AbsConst t -> f t
  go = case _ of
    S.Global x -> [ x ]
    S.Code t -> globalsInTy t
    S.App f a -> go f <> go a
    S.Lam d b -> globalsInTy d <> abs go b
    S.Cons d -> A.concatMap go (dictValues d)
    S.Proj e _ -> go e
    S.Tag _ as -> A.concatMap go as
    S.Match e cs -> go e <> A.concatMap (\(Tuple _ b) -> go b) (dictValues cs)
    S.Specialize e _ v -> go e <> go v
    S.Is e -> go e
    S.Rec e -> go e
    S.Implicit _ e -> go e
    S.MetaApp _ _ t -> globalsInTy t
    _ -> []

globalsInTy :: S.Ty -> Array Name
globalsInTy = A.nub <<< go
  where
  abs f = case _ of
    S.Abs _ t -> f t
    S.AbsConst t -> f t
  go = case _ of
    S.Decode e -> globalsInEl e
    S.Function f -> go f.dom <> abs go f.cod
    S.Record rt -> A.concatMap go (dictValues rt.fieldTypes)
    S.Sum (Variants vs) -> A.concatMap (A.concatMap go) (dictValues vs)
    S.IsTy t -> go t
    S.PrimArray t -> go t
    _ -> []

-- | A definition's body, the global it is being stored as (`k`) put back
-- | as what it is in the body: local 0, the definition itself.  While it
-- | is elaborated the definition is in scope as the global it will be (so
-- | an implicit argument's solution may mention it, closed); its stored
-- | body has it where its other locals are.
selfAsLocal :: Name -> S.El -> S.El
selfAsLocal k = el 0
  where
  under :: forall a. (Int -> a -> a) -> Int -> S.Abs a -> S.Abs a
  under f d = case _ of
    S.Abs x t -> S.Abs x (f (d + 1) t)
    S.AbsConst t -> S.AbsConst (f d t)
  el d = case _ of
    S.Global x | x == k -> S.Local d
    S.Code t -> S.Code (ty d t)
    S.App f a -> S.App (el d f) (el d a)
    S.Lam dom b -> S.Lam (ty d dom) (under el d b)
    S.Cons c -> S.Cons (dictMap (el d) c)
    S.Proj e l -> S.Proj (el d e) l
    S.Tag t as -> S.Tag t (map (el d) as)
    S.Match e cs -> S.Match (el d e) (dictMap (\(Tuple xs b) -> Tuple xs (el (d + A.length xs) b)) cs)
    S.Specialize e l v -> S.Specialize (el d e) l (el d v)
    S.Is e -> S.Is (el d e)
    S.Rec e -> S.Rec (el d e)
    S.Implicit x e -> S.Implicit x (el d e)
    S.MetaApp m xs t -> S.MetaApp m xs (ty d t)
    other -> other
  ty d = case _ of
    S.Decode e -> S.Decode (el d e)
    S.Function f -> S.Function { level: f.level, dom: ty d f.dom, cod: under ty d f.cod, implicit: f.implicit }
    -- (a record type is a telescope: each field's type sees the ones before)
    S.Record rt -> S.Record { level: rt.level, fieldTypes: rt.fieldTypes { values = A.mapWithIndex (\i t -> ty (d + i) t) rt.fieldTypes.values } }
    S.Sum (Variants vs) -> S.Sum (Variants (dictMap (map (ty d)) vs))
    S.IsTy t -> S.IsTy (ty d t)
    S.PrimArray t -> S.PrimArray (ty d t)
    other -> other

-- | a global key as the name it is known by where it is shown: a module's
-- | definition (`module.name`) by its alias, if it has one
displayName :: Globals -> Name -> Name
displayName g x =
  fromMaybe (describeKey x) (A.head (A.mapMaybe (\(Tuple n k) -> if k == x then Just n else Nothing) (Map.toUnfoldable g.aliases)))

-- | a synthesized key (no surface spelling: `m#` is the module as a value,
-- | `m#T` its theory, `Heff.Driver.moduleStruct`) said in words
describeKey :: Name -> Name
describeKey x = case Str.stripSuffix (Str.Pattern "#T") x of
  Just m -> "the theory of " <> m
  Nothing -> case Str.stripSuffix (Str.Pattern "#") x of
    Just m -> "the module " <> m
    Nothing -> x
