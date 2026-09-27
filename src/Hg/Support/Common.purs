module Hg.Support.Common
  ( panic
  , Dict
  , dictFromList
  , dictToList
  , dictLookup
  , dictElemAt
  , dictContains
  , dictUpdate
  , dictKeyIndex
  , dictLength
  , dictMap
  , dictValues
  , dictKeys
  , dictAt
  , dictTraverse
  , Name
  , quoteString
  , operatorName
  , spellOperator
  ) where

import Prelude

import Data.Array as A
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..), fst, snd)
import Effect.Exception.Unsafe (unsafeThrow)

type Name = String

panic :: forall a. String -> a
panic msg = unsafeThrow ("panicked because " <> msg)

-- | an ordered dictionary: the order is the order of declaration, which for a
-- | record type is the order of its telescope
type Dict a = { keys :: Array Name, byName :: Map Name Int, values :: Array a }

dictFromList :: forall a. Array (Tuple Name a) -> Dict a
dictFromList pairs =
  { keys: map fst pairs
  , byName: Map.fromFoldable (A.mapWithIndex (\i p -> Tuple (fst p) i) pairs)
  , values: map snd pairs
  }

dictToList :: forall a. Dict a -> Array (Tuple Name a)
dictToList d = A.zip d.keys d.values

dictLookup :: forall a. Dict a -> Name -> Maybe a
dictLookup d x = Map.lookup x d.byName >>= A.index d.values

dictElemAt :: forall a. Dict a -> Name -> a
dictElemAt d x = case dictLookup d x of
  Just v -> v
  Nothing -> panic ("should only unwrap a Just (field " <> x <> ")")

dictAt :: forall a. Dict a -> Int -> a
dictAt d i = case A.index d.values i of
  Just v -> v
  Nothing -> panic "dictionary index out of range"

dictContains :: forall a. Dict a -> Name -> Boolean
dictContains d x = Map.member x d.byName

dictUpdate :: forall a. Dict a -> Name -> (a -> a) -> Dict a
dictUpdate d x f =
  let i = dictKeyIndex d x
  in d { values = fromMaybe d.values (A.modifyAt i f d.values) }

dictKeyIndex :: forall a. Dict a -> Name -> Int
dictKeyIndex d x = case Map.lookup x d.byName of
  Just i -> i
  Nothing -> panic ("no key " <> x)

dictLength :: forall a. Dict a -> Int
dictLength d = A.length d.values

dictMap :: forall a b. (a -> b) -> Dict a -> Dict b
dictMap f d = { keys: d.keys, byName: d.byName, values: map f d.values }

dictTraverse :: forall m a b. Applicative m => (a -> m b) -> Dict a -> m (Dict b)
dictTraverse f d = (\vs -> { keys: d.keys, byName: d.byName, values: vs }) <$> traverse f d.values

dictValues :: forall a. Dict a -> Array a
dictValues d = d.values

dictKeys :: forall a. Dict a -> Array Name
dictKeys d = d.keys

-- | a string literal, as JavaScript (and Hippogriff) write it
quoteString :: String -> String
quoteString s = "\"" <> S.joinWith "" (map esc (SCU.toCharArray s)) <> "\""
  where
  esc = case _ of
    '"' -> "\\\""
    '\\' -> "\\\\"
    '\n' -> "\\n"
    '\r' -> "\\r"
    '\t' -> "\\t"
    c -> SCU.singleton c

-- | a name, or a key (`module.name`), with its name spelt out if it is an
-- | operator: `lib.prelude.+` is `lib.prelude.op_plus`
spellOperator :: String -> String
spellOperator k = case S.lastIndexOf (S.Pattern ".") k of
  Just i | Just w <- operatorName (S.drop (i + 1) k) -> S.take (i + 1) k <> w
  _ -> fromMaybe k (operatorName k)

-- | an operator's name as a word, for the languages compiled to: `+` is
-- | `op_plus`, `<=` `op_lt_eq`; nothing for a name that is not an operator
operatorName :: String -> Maybe String
operatorName x =
  let words = map word (SCU.toCharArray x)
  in if x /= "" && A.all (_ /= Nothing) words then Just ("op_" <> S.joinWith "_" (A.catMaybes words)) else Nothing
  where
  word = case _ of
    '+' -> Just "plus"
    '-' -> Just "minus"
    '*' -> Just "times"
    '/' -> Just "slash"
    '%' -> Just "percent"
    '=' -> Just "eq"
    '<' -> Just "lt"
    '>' -> Just "gt"
    '&' -> Just "amp"
    '|' -> Just "bar"
    '!' -> Just "bang"
    '^' -> Just "caret"
    '?' -> Just "q"
    '@' -> Just "at"
    '#' -> Just "hash"
    '$' -> Just "dollar"
    '~' -> Just "tilde"
    ':' -> Just "colon"
    '.' -> Just "dot"
    '\\' -> Just "backslash"
    _ -> Nothing
