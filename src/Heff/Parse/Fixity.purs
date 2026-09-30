-- | Operators and how they bind, declared relatively: no numbers, only which
-- | binds tighter than which.
-- |
-- |   infixl + - above ==
-- |   infixl * / above + -
-- |
-- | The operators of one declaration are a group, of one associativity;
-- | `above` and `below` say the group binds tighter (or looser) than the
-- | groups of the operators given, and binding tighter is transitive.  Two
-- | operators of groups neither of which binds tighter than the other do not
-- | go together without parentheses: `a + b ++ c` is refused, unless `+` and
-- | `++` are related.
-- |
-- | A group is named by its first operator: an operator is in one group at
-- | most, among those a file declares and those of what it imports.
module Heff.Parse.Fixity
  ( Assoc(..)
  , Group
  , Fixities
  , Relation(..)
  , emptyFixities
  , union
  , groupOf
  , relate
  , bindsTighter
  , reserved
  ) where

import Prelude

import Data.Array as A
import Data.Maybe (Maybe(..))

data Assoc = AssocLeft | AssocRight | AssocNone

derive instance eqAssoc :: Eq Assoc

type Group = { id :: String, assoc :: Assoc, ops :: Array String }

-- | the groups, and which binds tighter than which (`tighter`, `looser`:
-- | groups' ids)
type Fixities = { groups :: Array Group, above :: Array { tighter :: String, looser :: String } }

emptyFixities :: Fixities
emptyFixities = { groups: [], above: [] }

-- | (the same group from two places is one)
union :: Fixities -> Fixities -> Fixities
union a b =
  { groups: A.nubByEq (\g h -> g.id == h.id) (a.groups <> b.groups)
  , above: A.nubByEq (\p q -> p.tighter == q.tighter && p.looser == q.looser) (a.above <> b.above)
  }

groupOf :: Fixities -> String -> Maybe Group
groupOf f op = A.find (\g -> A.elem op g.ops) f.groups

-- | how an operator is to the one after it, `a op b op' c`
data Relation
  = Tighter -- ^ `op` binds tighter: `(a op b) op' c`
  | Looser -- ^ `a op (b op' c)`
  | Same Assoc -- ^ one group: its associativity says
  | Unrelated
  | Undeclared String -- ^ the operator has no fixity

relate :: Fixities -> String -> String -> Relation
relate f op op' = case groupOf f op, groupOf f op' of
  Nothing, _ -> Undeclared op
  _, Nothing -> Undeclared op'
  Just g, Just g'
    | g.id == g'.id -> Same g.assoc
    | bindsTighter f g.id g'.id -> Tighter
    | bindsTighter f g'.id g.id -> Looser
    | otherwise -> Unrelated

-- | a group binds tighter than another (by their ids), through a chain of
-- | `above`
bindsTighter :: Fixities -> String -> String -> Boolean
bindsTighter f from to = go [ from ] []
  where
  go frontier seen = case A.uncons frontier of
    Nothing -> false
    Just { head, tail }
      | A.elem head seen -> go tail seen
      | otherwise ->
          let next = map _.looser (A.filter (\p -> p.tighter == head) f.above)
          in A.elem to next || go (tail <> next) (A.cons head seen)

-- | symbols that are syntax, never operators
reserved :: Array String
reserved = [ "->", "~", "::", ":", "=", "|", "<-", "\\", ":=", "=>", "." ]
