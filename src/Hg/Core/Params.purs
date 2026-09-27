module Hg.Core.Params where

import Prelude

import Data.List (List)
import Hg.Support.Common (Dict, Name, dictMap)

-- Levels and universes
--------------------------------------------------------------------------------

data Level = Small | Large | Top

derive instance eqLevel :: Eq Level

instance showLevel :: Show Level where
  show = case _ of
    Small -> "Small"
    Large -> "Large"
    Top -> "Top"

leq :: Level -> Level -> Boolean
leq l1 l2 = case l1, l2 of
  Small, _ -> true
  Large, Small -> false
  Large, _ -> true
  Top, Top -> true
  Top, _ -> false

maxLevel :: Level -> Level -> Level
maxLevel l1 l2
  | leq l1 l2 = l2
  | otherwise = l1

-- | The reference implementation indexes elements and types by a phantom
-- | `Case`; PureScript has no GADTs, so the case travels as a value instead.
-- | `N` (nominative) is an ordinary element, `D` (descriptive) is a
-- | description that gives a top-level name its behavior.
data Case = N | D

derive instance eqCase :: Eq Case

data Universe = SmallU | LargeU

derive instance eqUniverse :: Eq Universe

instance showUniverse :: Show Universe where
  show = case _ of
    SmallU -> "SmallU"
    LargeU -> "LargeU"

codesInto :: Universe -> Level
codesInto = case _ of
  SmallU -> Large
  LargeU -> Top

decodesInto :: Universe -> Level
decodesInto = case _ of
  SmallU -> Small
  LargeU -> Large

-- Context shape
--------------------------------------------------------------------------------

-- | `names` is backwards: its head is the innermost binder
type CtxShape = { len :: Int, names :: List Name }

-- Variants for sum type
--------------------------------------------------------------------------------

newtype Variants a = Variants (Dict (Array a))

variantsMap :: forall a b. (a -> b) -> Variants a -> Variants b
variantsMap f (Variants d) = Variants (dictMap (map f) d)
