-- | A port of `Hippogriff.Core.Globals`: an insertion-ordered map of names to
-- | elaborated definitions.
module Heff.Core.Globals where

import Prelude

import Data.Array as A
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), maybe)
import Data.Set (Set)
import Data.Set as Set
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Name, panic)
import Heff.Core.Syntax as S
import Heff.Core.Value as V

-- | `syntax` is the definition at case D, with the definition itself bound
-- | as local 0 (the reference implementation's `FixMe`); `tySyntax` its
-- | type, closed.  The two are the definition as a cache keeps it: `value`
-- | and `type_` are their evaluations (`Heff.Elab.Top.entryFor`)
type GlobalEntry = { syntax :: S.El, tySyntax :: S.Ty, value :: V.El, type_ :: V.Ty }

-- | `aliases` maps a name to the key it stands for.  A module's definitions
-- | are keyed `module.name`: a file reaches its own, and those of the
-- | modules it imports, through aliases (`Heff.Driver`).  `broken`: the
-- | keys of definitions that did not elaborate, not even their types (a
-- | use of one is not an error of its own)
type Globals = { order :: Array Name, entries :: Map Name GlobalEntry, aliases :: Map Name Name, broken :: Set Name, instances :: Array Name }

emptyGlobals :: Globals
emptyGlobals = { order: [], entries: Map.empty, aliases: Map.empty, broken: Set.empty, instances: [] }

-- | spike: mark a definition as an instance -- a candidate for instance
-- | search (`instance x : T`)
markInstance :: Name -> Globals -> Globals
markInstance k g = g { instances = A.snoc (A.filter (_ /= k) g.instances) k }

addBroken :: Name -> Globals -> Globals
addBroken k g = g { broken = Set.insert k g.broken }

-- | is a name (or the key it stands for) that of a definition that did not
-- | elaborate?
isBroken :: Globals -> Name -> Boolean
isBroken g x = Set.member x g.broken || maybe false (\k -> Set.member k g.broken) (Map.lookup x g.aliases)

addGlobalEntry :: Name -> GlobalEntry -> Globals -> Globals
-- | (a redefinition moves the name to the end, as the reference
-- | implementation's ordered map does with `>|`)
addGlobalEntry x e g = g
  { order = A.snoc (A.filter (_ /= x) g.order) x
  , entries = Map.insert x e g.entries
  }

addAlias :: Name -> Name -> Globals -> Globals
addAlias name key g = g { aliases = Map.insert name key g.aliases }

-- | resolve a name to its key and entry
lookupGlobalKey :: Globals -> Name -> Maybe (Tuple Name GlobalEntry)
lookupGlobalKey g x = case Map.lookup x g.entries of
  Just e -> Just (Tuple x e)
  Nothing -> do
    k <- Map.lookup x g.aliases
    e <- Map.lookup k g.entries
    pure (Tuple k e)

lookupGlobal :: Globals -> Name -> Maybe GlobalEntry
lookupGlobal g x = map (\(Tuple _ e) -> e) (lookupGlobalKey g x)

elemAtGlobal :: Globals -> Name -> GlobalEntry
elemAtGlobal g x = case Map.lookup x g.entries of
  Just e -> e
  _ -> panic ("no global " <> x)

globalsList :: Globals -> Array (Tuple Name GlobalEntry)
globalsList g = A.mapMaybe (\x -> Tuple x <$> Map.lookup x g.entries) g.order
