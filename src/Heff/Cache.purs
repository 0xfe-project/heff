-- | Separate compilation, as a cache the compiler manages: a module whose
-- | making is unchanged since it last elaborated clean is thawed from
-- | `ROOT/.heff/` -- its definitions' core syntax evaluated back into
-- | globals -- instead of elaborated again.  There is no interface file:
-- | what is kept is the elaborated module itself, and `heff check` remains
-- | the way to look at one.
-- |
-- | What a module is made of is tracked at the definitions it uses, not at
-- | whole imports (the `~` of the plan: an importer depends on a module as
-- | the specialization it actually took of it).  A definition's hash
-- | covers its core syntax and, transitively, the definitions it mentions;
-- | a module records the hash of each outside definition its own mention.
-- | So a change to a dependency re-elaborates only what took that path:
-- | touching a body no importer uses, or only comments, invalidates no
-- | one.  What an importer's *parse* takes from a direct import -- which
-- | names are defined, which operators have which fixity -- is one hash
-- | per import (`nsHash`), so a new name appearing is seen (it may turn a
-- | tag into a reference, or two imports into a clash).
-- |
-- | A module is thawed only under the same compiler, the same source, the
-- | same namespaces and the same needed definitions.  Whatever does not
-- | read, parse or match is treated as absent and elaborated over.
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
import Data.Set as Set
import Data.String as S
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Control.MonadPlus (guard)
import Effect (Effect)
import Effect.Exception (try)
import Heff.Core.Globals (lookupGlobal)
import Heff.Core.Rename (globalsInEl, globalsInTy)
import Heff.Core.Serial (decodeEl, decodeTy, encodeEl, encodeTy)
import Heff.Driver (Checked, Source, Thawed, elabNext)
import Heff.Load (Paths, loadProgramWith)
import Heff.Parse.Fixity (Assoc(..), Fixities)
import Node.Encoding (Encoding(..))
import Node.FS.Sync (readTextFile, rename, writeTextFile)

foreign import sha256Hex :: String -> String
foreign import mtimeMs :: String -> Effect Number
foreign import ensureDir :: String -> Effect Unit

version :: String
version = "heff-cache/2"

-- | what identifies the compiler build: its bundle's mtime (`lib` is
-- | `ROOT/lib`, the bundle `ROOT/output/Main/index.js`)
stampFor :: String -> Effect String
stampFor lib = do
  let root = S.joinWith "/" (A.dropEnd 1 (S.split (S.Pattern "/") lib))
  m <- mtimeMs (root <> "/output/Main/index.js")
  pure (show m)

-- | each definition's hash: its core syntax, and the hashes of the
-- | definitions it mentions -- so a hash covers a definition's making,
-- | transitively.  (A primitive declared again is the same one: the first
-- | hash stands.)
defHashesInto :: Map String String -> Checked -> Map String String
defHashesInto hs c = A.foldl one hs c.own
  where
  one m o
    | Map.member o.key m = m
    | otherwise = case lookupGlobal c.top.globals o.key of
        Nothing -> m
        Just e ->
          let refs = A.sort (A.nub (globalsInTy e.tySyntax <> globalsInEl e.syntax))
          in Map.insert o.key
            (sha256Hex
              ( stringify (encodeTy e.tySyntax) <> "\n" <> stringify (encodeEl e.syntax) <> "\n"
                  <> S.joinWith "," (map (\k -> k <> ":" <> fromMaybe "?" (Map.lookup k m)) refs)
              ))
            m

-- | what a module's parse takes from a direct import: the names it
-- | defines, and the fixities it declares
nsHashOf :: Checked -> String
nsHashOf c = sha256Hex (S.joinWith "," (map _.name c.own) <> "|" <> fixSig c.fixities)
  where
  fixSig f =
    S.joinWith ";" (map (\g -> g.id <> ":" <> show (assocInt g.assoc) <> ":" <> S.joinWith " " g.ops) f.groups)
      <> "|"
      <> S.joinWith ";" (map (\e -> e.tighter <> ">" <> e.looser) f.above)

assocInt :: Assoc -> Int
assocInt = case _ of
  AssocLeft -> 0
  AssocRight -> 1
  AssocNone -> 2

-- | the modules a file imports directly, in scope by name or by alias
directOf :: Source -> Array String
directOf s = A.nub (s.opens <> map _.module s.aliases)

-- | `loadProgram` and `elabProgram`, around the cache.  The file asked for
-- | is always elaborated: its watches, spans and infos are wanted; the
-- | modules it imports are thawed where the cache holds them, and frozen
-- | where it did not and they elaborated clean.
loadCached :: (String -> Effect String) -> Paths -> String -> Effect { program :: Array Checked, errors :: Array String }
loadCached read ps file = do
  l <- loadProgramWith read ps file
  stamp <- stampFor ps.lib
  let
    dir = ps.root <> "/.heff"
    final = A.length l.sources - 1
    one acc (Tuple i s) = do
      th <- if i == final then pure Nothing else thawOne dir stamp acc s
      let c = elabNext acc.done s th
      pure { done: A.snoc acc.done c, hashes: defHashesInto acc.hashes c }
  r <- A.foldM one { done: [], hashes: Map.empty } (A.mapWithIndex Tuple l.sources)
  for_ (A.filter (\c -> not c.cached && A.null c.diags) r.done) \c ->
    for_ (freeze stamp r.hashes r.done c) \out -> write dir c.module out
  pure { program: r.done, errors: l.errors }

thawOne :: String -> String -> { done :: Array Checked, hashes :: Map String String } -> Source -> Effect (Maybe Thawed)
thawOne dir stamp acc s = do
  r <- try (readTextFile UTF8 (dir <> "/" <> s.module <> ".json"))
  pure (hush r >>= melt stamp acc s)

-- | written whole, then named: a torn write is never half read
write :: String -> String -> String -> Effect Unit
write dir m out = void $ try do
  ensureDir dir
  let path = dir <> "/" <> m <> ".json"
  writeTextFile UTF8 (path <> ".tmp") out
  rename (path <> ".tmp") path

-- | a clean module as the cache file keeps it: under which compiler and
-- | source it elaborated, what its parse took from each direct import,
-- | which outside definitions it mentions (at which hashes), and its own
-- | definitions' syntax
freeze :: String -> Map String String -> Array Checked -> Checked -> Maybe String
freeze stamp hashes done c = do
  defs <- traverse def (A.zip c.own c.top.spans)
  pure $ stringify $ fromArray
    [ fromString version
    , fromString stamp
    , fromString (sha256Hex c.source)
    , encodeFixities c.fixities
    , fromArray (map (\m -> fromArray [ fromString m, fromString (maybe "?" nsHashOf (A.find (\d -> d.module == m) done)) ]) direct)
    , fromArray (map (\k -> fromArray [ fromString k, fromString (fromMaybe "?" (Map.lookup k hashes)) ]) needs)
    , fromArray defs
    ]
  where
  direct = A.nub (c.opens <> map _.module c.aliases)
  ownSet = Set.fromFoldable (map _.key c.own)
  needs = A.sort $ A.filter (\k -> not (Set.member k ownSet)) $ A.nub $ A.concatMap
    (\o -> maybe [] (\e -> globalsInTy e.tySyntax <> globalsInEl e.syntax) (lookupGlobal c.top.globals o.key))
    c.own
  def (Tuple o (Tuple _ sp)) = lookupGlobal c.top.globals o.key <#> \e ->
    fromArray [ fromString o.name, fromString o.key, int sp.start, int sp.end, encodeTy e.tySyntax, encodeEl e.syntax ]

melt :: String -> { done :: Array Checked, hashes :: Map String String } -> Source -> String -> Maybe Thawed
melt stamp acc s text = do
  j <- hush (jsonParser text)
  toArray j >>= case _ of
    [ v, st, srcH, fx, ns, nd, ds ] -> do
      guard (toString v == Just version && toString st == Just stamp)
      guard (toString srcH == Just (sha256Hex s.source))
      nsPairs <- toArray ns >>= traverse pair
      -- every direct import's namespace as it was (the source is the same,
      -- so the import list is too)
      guard (A.all (\(Tuple m h) -> (nsHashOf <$> A.find (\d -> d.module == m) acc.done) == Just h) nsPairs)
      guard (A.length nsPairs == A.length (directOf s))
      needPairs <- toArray nd >>= traverse pair
      guard (A.all (\(Tuple k h) -> Map.lookup k acc.hashes == Just h) needPairs)
      fixities <- decodeFixities fx
      defs <- toArray ds >>= traverse def
      pure { fixities, defs }
    _ -> Nothing
  where
  pair j = toArray j >>= case _ of
    [ x, h ] -> Tuple <$> toString x <*> toString h
    _ -> Nothing
  def j = toArray j >>= case _ of
    [ n, k, s0, e0, t, b ] -> (\name key start end ty body -> { name, key, span: { start, end }, ty, body })
      <$> toString n <*> toString k <*> asInt s0 <*> asInt e0 <*> decodeTy t <*> decodeEl b
    _ -> Nothing

encodeFixities :: Fixities -> Json
encodeFixities f = fromArray
  [ fromArray (map (\g -> fromArray [ fromString g.id, int (assocInt g.assoc), fromArray (map fromString g.ops) ]) f.groups)
  , fromArray (map (\e -> fromArray [ fromString e.tighter, fromString e.looser ]) f.above)
  ]

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
