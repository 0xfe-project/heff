-- | Renaming the globals inside syntax: how the keys of other modules'
-- | definitions are shown by the names they have in a file.
module Heff.Core.Rename
  ( renameEl
  , renameTy
  , displayName
  ) where

import Prelude

import Data.Array as A
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Name, dictMap)
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
    other -> other


-- | a global key as the name it is known by where it is shown: a module's
-- | definition (`module.name`) by its alias, if it has one
displayName :: Globals -> Name -> Name
displayName g x =
  fromMaybe x (A.head (A.mapMaybe (\(Tuple n k) -> if k == x then Just n else Nothing) (Map.toUnfoldable g.aliases)))
