-- | Core syntax as JSON, for the cache (`Heff.Cache`): every node a tagged
-- | array.  `decode` is the inverse of `encode`; what does not decode --
-- | another compiler's cache, a torn write -- is `Nothing`, and the cache
-- | treats it as absent.
module Heff.Core.Serial
  ( encodeEl
  , decodeEl
  , encodeTy
  , decodeTy
  ) where

import Prelude

import Data.Argonaut.Core (Json, fromArray, fromBoolean, fromNumber, fromString, toArray, toBoolean, toNumber, toString)
import Data.Array as A
import Data.Int as Int
import Data.Maybe (Maybe(..))
import Data.Number as Number
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Heff.Support.BigInt as BigInt
import Heff.Support.Common (Dict, Name, dictFromList, dictToList)
import Heff.Core.Params (Level(..), Universe(..), Variants(..))
import Heff.Core.Syntax as S

tag :: String -> Array Json -> Json
tag t xs = fromArray (A.cons (fromString t) xs)

int :: Int -> Json
int = fromNumber <<< Int.toNumber

asInt :: Json -> Maybe Int
asInt j = toNumber j >>= Int.fromNumber

-- | the tag and the rest
node :: Json -> Maybe (Tuple String (Array Json))
node j = do
  a <- toArray j
  { head, tail } <- A.uncons a
  t <- toString head
  pure (Tuple t tail)

at :: Array Json -> Int -> Maybe Json
at = A.index

encodeAbs :: forall a. (a -> Json) -> S.Abs a -> Json
encodeAbs f = case _ of
  S.Abs x e -> tag "v" [ fromString x, f e ]
  S.AbsConst e -> tag "_" [ f e ]

decodeAbs :: forall a. (Json -> Maybe a) -> Json -> Maybe (S.Abs a)
decodeAbs f j = node j >>= case _ of
  Tuple "v" [ x, e ] -> S.Abs <$> toString x <*> f e
  Tuple "_" [ e ] -> S.AbsConst <$> f e
  _ -> Nothing

encodeDict :: forall a. (a -> Json) -> Dict a -> Json
encodeDict f d = fromArray (map (\(Tuple x v) -> fromArray [ fromString x, f v ]) (dictToList d))

decodeDict :: forall a. (Json -> Maybe a) -> Json -> Maybe (Dict a)
decodeDict f j = toArray j >>= traverse one <#> dictFromList
  where
  one p = toArray p >>= case _ of
    [ x, v ] -> Tuple <$> toString x <*> f v
    _ -> Nothing

encodeLevel :: Level -> Json
encodeLevel = int <<< case _ of
  Small -> 0
  Large -> 1
  Top -> 2

decodeLevel :: Json -> Maybe Level
decodeLevel j = asInt j >>= case _ of
  0 -> Just Small
  1 -> Just Large
  2 -> Just Top
  _ -> Nothing

encodeEl :: S.El -> Json
encodeEl = go
  where
  go = case _ of
    S.Local i -> tag "loc" [ int i ]
    S.Global x -> tag "glo" [ fromString x ]
    S.Code t -> tag "cod" [ encodeTy t ]
    S.App f a -> tag "app" [ go f, go a ]
    S.Lam t b -> tag "lam" [ encodeTy t, encodeAbs go b ]
    S.Cons d -> tag "con" [ encodeDict go d ]
    S.Proj e l -> tag "prj" [ go e, fromString l ]
    S.Tag t as -> tag "tag" [ fromString t, fromArray (map go as) ]
    S.Match e cs -> tag "mat" [ go e, encodeDict (\(Tuple xs b) -> fromArray [ fromArray (map fromString xs), go b ]) cs ]
    S.Specialize e l v -> tag "spe" [ go e, fromString l, go v ]
    S.Is e -> tag "is" [ go e ]
    S.Opaque -> tag "opq" []
    S.Abandon -> tag "abn" []
    S.Rec e -> tag "rec" [ go e ]
    S.Implicit x e -> tag "imp" [ fromString x, go e ]
    S.Prim x -> tag "prm" [ fromString x ]
    -- (zonked away before a definition is stored; decoding refuses it)
    S.MetaApp _ _ _ -> tag "meta" []
    S.LitInt b -> tag "int" [ fromString (BigInt.toString b) ]
    S.LitFloat n -> tag "flt" [ fromString (show n) ]
    S.LitString s -> tag "str" [ fromString s ]

decodeEl :: Json -> Maybe S.El
decodeEl = go
  where
  go j = node j >>= case _ of
    Tuple "loc" [ i ] -> S.Local <$> asInt i
    Tuple "glo" [ x ] -> S.Global <$> toString x
    Tuple "cod" [ t ] -> S.Code <$> decodeTy t
    Tuple "app" [ f, a ] -> S.App <$> go f <*> go a
    Tuple "lam" [ t, b ] -> S.Lam <$> decodeTy t <*> decodeAbs go b
    Tuple "con" [ d ] -> S.Cons <$> decodeDict go d
    Tuple "prj" [ e, l ] -> S.Proj <$> go e <*> toString l
    Tuple "tag" [ t, as ] -> S.Tag <$> toString t <*> (toArray as >>= traverse go)
    Tuple "mat" [ e, cs ] -> S.Match <$> go e <*> decodeDict alt cs
    Tuple "spe" [ e, l, v ] -> S.Specialize <$> go e <*> toString l <*> go v
    Tuple "is" [ e ] -> S.Is <$> go e
    Tuple "opq" [] -> Just S.Opaque
    Tuple "abn" [] -> Just S.Abandon
    Tuple "rec" [ e ] -> S.Rec <$> go e
    Tuple "imp" [ x, e ] -> S.Implicit <$> toString x <*> go e
    Tuple "prm" [ x ] -> S.Prim <$> toString x
    Tuple "int" [ s ] -> S.LitInt <$> (toString s >>= BigInt.fromString)
    Tuple "flt" [ s ] -> S.LitFloat <$> (toString s >>= Number.fromString)
    Tuple "str" [ s ] -> S.LitString <$> toString s
    _ -> Nothing
  alt j = toArray j >>= case _ of
    [ xs, b ] -> Tuple <$> (toArray xs >>= traverse toString) <*> go b
    _ -> Nothing

encodeTy :: S.Ty -> Json
encodeTy = go
  where
  go = case _ of
    S.U SmallU -> tag "u" [ int 0 ]
    S.U LargeU -> tag "u" [ int 1 ]
    S.Decode e -> tag "de" [ encodeEl e ]
    S.Function f -> tag "fn" [ encodeLevel f.level, go f.dom, encodeAbs go f.cod, fromBoolean f.implicit ]
    S.Record r -> tag "rc" [ encodeLevel r.level, encodeDict go r.fieldTypes ]
    S.Sum (Variants vs) -> tag "sm" [ encodeDict (fromArray <<< map go) vs ]
    S.IsTy t -> tag "is" [ go t ]
    S.PrimInt -> tag "pi" []
    S.PrimFloat -> tag "pf" []
    S.PrimString -> tag "ps" []
    S.PrimArray t -> tag "pa" [ go t ]

decodeTy :: Json -> Maybe S.Ty
decodeTy = go
  where
  go j = node j >>= case _ of
    Tuple "u" [ u ] -> asInt u >>= case _ of
      0 -> Just (S.U SmallU)
      1 -> Just (S.U LargeU)
      _ -> Nothing
    Tuple "de" [ e ] -> S.Decode <$> decodeEl e
    Tuple "fn" [ l, d, c, i ] -> (\level dom cod implicit -> S.Function { level, dom, cod, implicit })
      <$> decodeLevel l <*> go d <*> decodeAbs go c <*> toBoolean i
    Tuple "rc" [ l, fs ] -> (\level fieldTypes -> S.Record { level, fieldTypes }) <$> decodeLevel l <*> decodeDict go fs
    Tuple "sm" [ vs ] -> S.Sum <<< Variants <$> decodeDict (\v -> toArray v >>= traverse go) vs
    Tuple "is" [ t ] -> S.IsTy <$> go t
    Tuple "pi" [] -> Just S.PrimInt
    Tuple "pf" [] -> Just S.PrimFloat
    Tuple "ps" [] -> Just S.PrimString
    Tuple "pa" [ t ] -> S.PrimArray <$> go t
    _ -> Nothing
