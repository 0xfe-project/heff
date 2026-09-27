-- | Integers of any size, for Hippogriff's `Int` (JavaScript's BigInt).
module Hg.Support.BigInt
  ( BigInt
  , fromInt
  , fromDigits
  , fromString
  , toString
  , toIntClamped
  , add
  , sub
  , mul
  , neg
  , divFloor
  , modFloor
  ) where

import Prelude

import Data.Maybe (Maybe(..))

foreign import data BigInt :: Type

foreign import fromInt :: Int -> BigInt
-- | digits, possibly after a `-` (as the lexer reads them)
foreign import fromDigits :: String -> BigInt
foreign import fromStringImpl :: Maybe BigInt -> (BigInt -> Maybe BigInt) -> String -> Maybe BigInt
foreign import toString :: BigInt -> String
foreign import toIntClamped :: BigInt -> Int
foreign import add :: BigInt -> BigInt -> BigInt
foreign import sub :: BigInt -> BigInt -> BigInt
foreign import mul :: BigInt -> BigInt -> BigInt
foreign import neg :: BigInt -> BigInt
foreign import divFloorImpl :: Maybe BigInt -> (BigInt -> Maybe BigInt) -> BigInt -> BigInt -> Maybe BigInt
foreign import modFloorImpl :: Maybe BigInt -> (BigInt -> Maybe BigInt) -> BigInt -> BigInt -> Maybe BigInt
foreign import eqImpl :: BigInt -> BigInt -> Boolean
foreign import compareImpl :: Ordering -> Ordering -> Ordering -> BigInt -> BigInt -> Ordering

fromString :: String -> Maybe BigInt
fromString = fromStringImpl Nothing Just

-- | floor division (as Haskell's `div`); by zero, nothing
divFloor :: BigInt -> BigInt -> Maybe BigInt
divFloor = divFloorImpl Nothing Just

-- | the remainder of `divFloor`: the sign of the divisor
modFloor :: BigInt -> BigInt -> Maybe BigInt
modFloor = modFloorImpl Nothing Just

instance Eq BigInt where
  eq = eqImpl

instance Ord BigInt where
  compare = compareImpl LT EQ GT

instance Show BigInt where
  show = toString
