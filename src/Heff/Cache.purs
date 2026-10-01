-- | Separate compilation, as a cache the compiler manages: a module whose
-- | source, dependencies and compiler are unchanged since it last
-- | elaborated clean is thawed from `ROOT/.heff/` -- its definitions' core
-- | syntax evaluated back into globals -- instead of elaborated again.
-- | There is no interface file: what is kept is the elaborated module
-- | itself, and `heff check` remains the way to look at one.
-- |
-- | A module is valid under a hash of the compiler build, its source, and
-- | its dependencies' hashes, so staleness is transitive by construction.
-- | Whatever does not read, parse or match -- another build's cache, a torn
-- | write -- is treated as absent and elaborated over.
module Heff.Cache
  ( loadCached
  ) where

import Prelude

import Data.Argonaut.Core (Json, fromArray, fromNumber, fromString, stringify, toArray, toNumber, toString)
import Data.Argonaut.Parser (jsonParser)
import Data.Array as A
import Data.Either (hush)
import Data.Foldable (for_)
import Data.Int as Int
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe, maybe)
import Data.String as S
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Control.MonadPlus (guard)
import Effect (Effect)
import Effect.Exception (try)
import Heff.Core.Globals (lookupGlobal)
import Heff.Core.Serial (decodeEl, decodeTy, encodeEl, encodeTy)
import Heff.Driver (Checked, Source, Thawed, elabProgramWith)
import Heff.Load (Paths, loadProgramWith)
import Heff.Parse.Fixity (Assoc(..), Fixities)
import Node.Encoding (Encoding(..))
import Node.FS.Sync (readTextFile, rename, writeTextFile)

foreign import sha256Hex :: String -> String
foreign import mtimeMs :: String -> Effect Number
foreign import ensureDir :: String -> Effect Unit

version :: String
version = "heff-cache/1"

-- | what identifies the compiler build: its bundle's mtime (`lib` is
-- | `ROOT/lib`, the bundle `ROOT/output/Main/index.js`)
stampFor :: String -> Effect String
stampFor lib = do
  let root = S.joinWith "/" (A.dropEnd 1 (S.split (S.Pattern "/") lib))
  m <- mtimeMs (root <> "/output/Main/index.js")
  pure (show m)

-- | each module's hash, over the modules in dependency order: the stamp,
-- | the source (length-prefixed), and the hashes of what it imports
hashes :: String -> Array Source -> Map String String
hashes stamp = A.foldl step Map.empty
  where
  step m s = Map.insert s.module
    (sha256Hex (version <> "\n" <> stamp <> "\n" <> show (S.length s.source) <> ":" <> s.source <> "\n" <> S.joinWith "," (map (\d -> fromMaybe "?" (Map.lookup d m)) s.deps)))
    m

-- | `loadProgram` and `elabProgram`, around the cache.  The file asked for
-- | is always elaborated: its watches, spans and infos are wanted; the
-- | modules it imports are thawed where the cache holds them, and frozen
-- | where it did not and they elaborated clean.
loadCached :: (String -> Effect String) -> Paths -> String -> Effect { program :: Array Checked, errors :: Array String }
loadCached read ps file = do
  l <- loadProgramWith read ps file
  stamp <- stampFor ps.lib
  let
    hs = hashes stamp l.sources
    dir = ps.root <> "/.heff"
    hashOf m = fromMaybe "?" (Map.lookup m hs)
  thaws <- A.foldM (\acc s -> thawOne dir (hashOf s.module) s.module <#> maybe acc (\t -> Map.insert s.module t acc)) Map.empty (A.dropEnd 1 l.sources)
  let program = elabProgramWith (\m -> Map.lookup m thaws) l.sources
  for_ (A.filter (\c -> not c.cached && A.null c.diags) program) \c ->
    for_ (freeze (hashOf c.module) c) \out -> write dir c.module out
  pure { program, errors: l.errors }

thawOne :: String -> String -> String -> Effect (Maybe Thawed)
thawOne dir h m = do
  r <- try (readTextFile UTF8 (dir <> "/" <> m <> ".json"))
  pure (hush r >>= melt h)

-- | written whole, then named: a torn write is never half read
write :: String -> String -> String -> Effect Unit
write dir m out = void $ try do
  ensureDir dir
  let path = dir <> "/" <> m <> ".json"
  writeTextFile UTF8 (path <> ".tmp") out
  rename (path <> ".tmp") path

-- | a clean module as the cache file keeps it
freeze :: String -> Checked -> Maybe String
freeze h c = do
  defs <- traverse one (A.zip c.own c.top.spans)
  pure (stringify (fromArray [ fromString version, fromString h, encodeFixities c.fixities, fromArray defs ]))
  where
  one (Tuple o (Tuple _ sp)) = lookupGlobal c.top.globals o.key <#> \e ->
    fromArray [ fromString o.name, fromString o.key, int sp.start, int sp.end, encodeTy e.tySyntax, encodeEl e.syntax ]

melt :: String -> String -> Maybe Thawed
melt expected s = do
  j <- hush (jsonParser s)
  toArray j >>= case _ of
    [ v, h, fx, ds ] -> do
      guard (toString v == Just version && toString h == Just expected)
      fixities <- decodeFixities fx
      defs <- toArray ds >>= traverse def
      pure { fixities, defs }
    _ -> Nothing
  where
  def j = toArray j >>= case _ of
    [ n, k, s0, e0, t, b ] -> (\name key start end ty body -> { name, key, span: { start, end }, ty, body })
      <$> toString n <*> toString k <*> asInt s0 <*> asInt e0 <*> decodeTy t <*> decodeEl b
    _ -> Nothing

encodeFixities :: Fixities -> Json
encodeFixities f = fromArray
  [ fromArray (map (\g -> fromArray [ fromString g.id, int (assocInt g.assoc), fromArray (map fromString g.ops) ]) f.groups)
  , fromArray (map (\e -> fromArray [ fromString e.tighter, fromString e.looser ]) f.above)
  ]
  where
  assocInt = case _ of
    AssocLeft -> 0
    AssocRight -> 1
    AssocNone -> 2

decodeFixities :: Json -> Maybe Fixities
decodeFixities j = toArray j >>= case _ of
  [ gs, es ] -> { groups: _, above: _ }
    <$> (toArray gs >>= traverse group)
    <*> (toArray es >>= traverse edge)
  _ -> Nothing
  where
  group g = toArray g >>= case _ of
    [ i, a, ops ] -> (\id assoc ops' -> { id, assoc, ops: ops' })
      <$> toString i
      <*> (asInt a >>= assocOf)
      <*> (toArray ops >>= traverse toString)
    _ -> Nothing
  edge e = toArray e >>= case _ of
    [ t, l ] -> { tighter: _, looser: _ } <$> toString t <*> toString l
    _ -> Nothing
  assocOf = case _ of
    0 -> Just AssocLeft
    1 -> Just AssocRight
    2 -> Just AssocNone
    _ -> Nothing

int :: Int -> Json
int = fromNumber <<< Int.toNumber

asInt :: Json -> Maybe Int
asInt j = toNumber j >>= Int.fromNumber
