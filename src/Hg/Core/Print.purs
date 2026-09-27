-- | A port of `Hippogriff.Core.Print`: core syntax back to notation.
module Hg.Core.Print
  ( Names
  , toNotationEl
  , toNotationTy
  , prettyEl
  , prettyElWith
  , Implicits
  , noImplicits
  , toNotationElWith
  , prettyTy
  , prtEl
  , prtTy
  , prtBare
  , prtElR
  , prtTyR
  , prtBareR
  ) where

import Prelude

import Data.Array as A
import Data.Array.NonEmpty as NEA
import Data.Foldable (foldl)
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Tuple (Tuple(..))
import Hg.Parse.FNotation (Ntn(..), group)
import Hg.Support.Common (dictToList, panic)
import Hg.Core.Params (CtxShape, Universe(..), Variants(..))
import Hg.Core.Readback (readbBare, readbEl, readbTy)
import Hg.Core.Rename (renameEl, renameTy)
import Hg.Core.Syntax as S
import Hg.Core.Value as V
import Hg.Parse.Config (prtNtn)
import Hg.Support.Pretty (Doc)

-- | innermost name first
type Names = List String

sp :: { start :: Int, end :: Int }
sp = { start: 0, end: 0 }

bid :: Names -> Int -> Ntn
bid xs0 i0 = go xs0 i0 Nil
  where
  go (x : _) 0 prev =
    let
      nx = L.length (L.filter (_ == x) prev)
      disamb = if nx > 0 then "^" <> show nx else ""
    in
      Ident (x <> disamb) sp
  go (x : xs') n prev = go xs' (n - 1) (x : prev)
  go Nil _ _ = panic ("name " <> show i0 <> " not bound")

grp :: Ntn -> Array Ntn -> Ntn
grp h ts = group (NEA.cons' h ts)

-- | which arguments of an application are implicit, by its head (the tools
-- | know from types; the plain printer does not), and whether to leave them
-- | out or show them as `{a}`
type Implicits = { of :: Names -> S.El -> Array Boolean, hide :: Boolean }

noImplicits :: Implicits
noImplicits = { of: \_ _ -> [], hide: false }

toNotationEl :: Names -> S.El -> Ntn
toNotationEl xs = toNotationElWith noImplicits xs

toNotationElWith :: Implicits -> Names -> S.El -> Ntn
toNotationElWith im xs0 el0 = go xs0 el0
  where
  go xs = case _ of
    t@(S.App _ _) ->
      let
        sp' = spine t []
        flags = im.of xs sp'.head
        args = A.catMaybes (A.mapWithIndex (\i a ->
          if fromMaybe false (A.index flags i) then (if im.hide then Nothing else Just (NBrace (go xs a) sp))
          else Just (go xs a)) sp'.args)
      in
        A.foldl Juxt (go xs sp'.head) args
    S.Lam (S.Function ft) (S.Abs x t) | ft.implicit -> Infix (NBrace (Ident x sp) sp) (Keyword "=>" sp) (go (x : xs) t)
    e -> toNotationElOne (\ys e' -> go ys e') xs e
  spine (S.App f a) acc = spine f (A.cons a acc)
  spine h acc = { head: h, args: acc }

toNotationElOne :: (Names -> S.El -> Ntn) -> Names -> S.El -> Ntn
toNotationElOne toNotationEl xs = case _ of
  S.Local i -> bid xs i
  S.Global x -> Ident x sp
  S.Code ty -> toNotationTy xs ty
  S.App f t -> Juxt (toNotationEl xs f) (toNotationEl xs t)
  S.Lam _ (S.Abs x t) -> Infix (Ident x sp) (Keyword "=>" sp) (toNotationEl (x : xs) t)
  S.Lam _ (S.AbsConst t) -> Infix (Ident "_" sp) (Keyword "=>" sp) (toNotationEl xs t)
  S.Proj t f -> Juxt (toNotationEl xs t) (NField f sp)
  S.Cons d -> NTuple (map (\(Tuple y t) -> Infix (Ident y sp) (Keyword ":" sp) (toNotationEl xs t)) (dictToList d)) sp
  S.Tag x ts -> grp (NTag x sp) (map (toNotationEl xs) ts)
  S.Match t cases -> NBlock "match" (Just (toNotationEl xs t)) (map (doCase toNotationEl xs) (dictToList cases)) sp
  S.Specialize t x t' ->
    Infix (toNotationEl xs t) (Keyword "&" sp)
      (NTuple [ Infix (NField x sp) (Keyword ":=" sp) (toNotationEl xs t') ] sp)
  S.Opaque -> Ident "<opaque>" sp
  S.Abandon -> Keyword "abandon" sp
  -- (printed as the reference implementation prints it: without the `rec`)
  S.Rec t -> toNotationEl xs t
  S.MetaApp m idxs _ -> grp (Ident ("?" <> show m) sp) (map (bid xs) idxs)
  S.LitInt i -> NInt i sp
  S.LitString s -> NString s sp
  S.Implicit _ t -> toNotationEl xs t
  S.Prim x -> Keyword ("primitive " <> x) sp
  S.Is t -> toNotationEl xs t

doCase :: (Names -> S.El -> Ntn) -> Names -> Tuple String (Tuple (Array String) S.El) -> Ntn
doCase toNotationEl xs (Tuple x (Tuple xs' body)) =
  Infix
    (grp (NTag x sp) (map (\x' -> Ident x' sp) xs'))
    (Keyword "=>" sp)
    (toNotationEl (foldl (flip (:)) xs xs') body)

sigFields :: Names -> Array (Tuple String S.Ty) -> Array Ntn
sigFields xs fs = case A.uncons fs of
  Nothing -> []
  Just { head: Tuple y a, tail } -> A.cons (nbinding y (toNotationTy xs a)) (sigFields (y : xs) tail)

nbinding :: String -> Ntn -> Ntn
nbinding x n = Infix (Ident x sp) (Keyword ":" sp) n

toNotationTy :: Names -> S.Ty -> Ntn
toNotationTy xs = case _ of
  S.U SmallU -> Keyword "Type" sp
  S.U LargeU -> Keyword "Theory" sp
  S.Decode t -> toNotationEl xs t
  S.Function f -> case f.cod of
    S.Abs x b | f.implicit -> Infix (NBrace (nbinding x (toNotationTy xs f.dom)) sp) (Keyword "->" sp) (toNotationTy (x : xs) b)
    S.Abs x b -> Infix (nbinding x (toNotationTy xs f.dom)) (Keyword "->" sp) (toNotationTy (x : xs) b)
    S.AbsConst b -> Infix (toNotationTy xs f.dom) (Keyword "->" sp) (toNotationTy xs b)
  S.Record r -> NBlock "sig" Nothing (sigFields xs (dictToList r.fieldTypes)) sp
  S.Sum (Variants vs) ->
    NBlock "sum" Nothing (map (\(Tuple x ts) -> grp (NTag x sp) (map (toNotationTy xs) ts)) (dictToList vs)) sp
  S.IsTy a -> toNotationTy xs a
  S.PrimInt -> Keyword "Int" sp
  S.PrimString -> Keyword "String" sp

prettyEl :: Names -> S.El -> Doc
prettyEl xs t = prtNtn (toNotationEl xs t)

prettyElWith :: Implicits -> Names -> S.El -> Doc
prettyElWith im xs t = prtNtn (toNotationElWith im xs t)

prettyTy :: Names -> S.Ty -> Doc
prettyTy xs t = prtNtn (toNotationTy xs t)

prtEl :: CtxShape -> V.El -> Doc
prtEl cs v = prettyEl cs.names (readbEl cs.len v)

prtTy :: CtxShape -> V.Ty -> Doc
prtTy cs v = prettyTy cs.names (readbTy cs.len v)

prtBare :: CtxShape -> V.BareNeutral -> Doc
prtBare cs v = prettyEl cs.names (readbBare cs.len v)

-- | the same, with global keys shown through a renaming
prtElR :: (String -> String) -> CtxShape -> V.El -> Doc
prtElR r cs v = prettyEl cs.names (renameEl r (readbEl cs.len v))

prtTyR :: (String -> String) -> CtxShape -> V.Ty -> Doc
prtTyR r cs v = prettyTy cs.names (renameTy r (readbTy cs.len v))

prtBareR :: (String -> String) -> CtxShape -> V.BareNeutral -> Doc
prtBareR r cs v = prettyEl cs.names (renameEl r (readbBare cs.len v))
