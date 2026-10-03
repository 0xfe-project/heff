-- | A program, from text to what the tools report:
-- |
-- |   text -[Heff.Parse]-> notation -[Heff.Elab]-> core -[Heff.Eval | Heff.Compile]-> values, code
-- |
-- | A program is files; a file is elaborated on top of the definitions of
-- | the files it imports (`Heff.Load` finds them, in order).
module Heff.Driver
  ( Source
  , Checked
  , Thawed
  , elabProgram
  , elabNext
  , errorsIn
  , renderFileDiags
  , interface
  , WatchInfo
  , watches
  , TestReport
  , testFile
  , promote
  , evaluate
  , findRunnable
  , candidateLeaves
  , completionsAt
  , explain
  , typeAt
  , definitionAt
  , Target
  , Occurrence
  , targetAt
  , occurrences
  , renameRefused
  , DocSymbol
  , symbols
  ) where

import Prelude

import Data.Array as A
import Data.Either (Either(..))
import Data.Lazy (force)
import Data.List (List(..))
import Control.Alt ((<|>))
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe, isJust, maybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple (Tuple(..))
import Heff.Core.Globals (Globals, addAlias, addBroken, addGlobalEntry, emptyGlobals, lookupGlobal)
import Data.Set as Set
import Heff.Core.Readback (readbTy)
import Heff.Core.Rename (describeKey, renameEl, renameTy)
import Heff.Core.Syntax (Abs(..), El(..), Ty(..))
import Heff.Core.Syntax as Syn
import Heff.Effect (Effect(..), leaves)
import Heff.Elab.Environment (Info)
import Heff.Core.Params (Level(..), Universe(..), Variants(..), maxLevel)
import Heff.Core.Value as V
import Heff.Locals (locals)
import Heff.Elab.Top (ThawedDef, TopResult, entryFor, topModule, topThawed, topWith)
import Heff.Eval.Interpreter (evalGlobalsWhere, showResult)
import Heff.Parse.Notation (Diag, Ntn(..), Severity(..), getGroup)
import Data.Array.NonEmpty as NEA
import Heff.Parse.Notation as F
import Heff.Parse.Fixity (Assoc(..), Fixities, emptyFixities, union)
import Heff.Support.Common (dictFromList, dictMap, dictToList, operatorName)
import Heff.Parse.Surface (readProgram, readProgramWith)
import Heff.Print.Surface (printSig, printTy)
import Heff.Support.Diagnostic (lineCol, renderDiag)
import Heff.Support.Pretty as P

-- | a file as the loader found it: `module` is its name (`parse.surface`
-- | for `parse/surface.hf`), `text` its source with the import lines blanked
-- | out, `deps` the modules it imports: `opens` those imported to be in
-- | scope by their names, `aliases` those imported as `M` (`import m as M`),
-- | in scope as `M.x`
type Source = { path :: String, module :: String, source :: String, text :: String, deps :: Array String, opens :: Array String, aliases :: Array { module :: String, alias :: String } }

-- | a file, elaborated: `top.globals` has everything it needs, `own` its
-- | own definitions (by name, and the key each is stored under,
-- | `module.name`), `fixities` the fixities it declares
type Checked = { path :: String, module :: String, source :: String, text :: String, ntns :: Array Ntn, top :: TopResult, diags :: Array Diag, own :: Array { name :: String, key :: String }, deps :: Array String, opens :: Array String, aliases :: Array { module :: String, alias :: String }, fixities :: Fixities, cached :: Boolean }

-- | a module as a cache keeps it: its definitions' core syntax and its
-- | fixities -- everything an importer needs, nothing a tool shows
type Thawed = { fixities :: Fixities, defs :: Array ThawedDef }

-- | read a file into notation; a capitalised name is a definition if
-- | `known` says so; `fixities`: those of what it imports
readSource :: (String -> Boolean) -> Fixities -> String -> { ntns :: Array Ntn, diags :: Array Diag, fixities :: Fixities, broken :: Array String }
readSource known fixities src = readProgramWith { known, fixities, lenient: false } src

-- | the files of a program, in the order `Heff.Load` gives (each after what
-- | it imports), elaborated.  A file sees its own definitions and those of
-- | the modules it imports, by name; what those need is there too, by key,
-- | but not in scope
elabProgram :: Array Source -> Array Checked
elabProgram = A.foldl (\done s -> A.snoc done (elabNext done s Nothing)) []

-- | the same, around a cache: a module `thaw` has is not read or
-- | elaborated -- its definitions come back through `topThawed` as they
-- | were.  Which modules qualify is the caller's judgment (`Heff.Cache`
-- | hashes what each module is made of).
elabNext :: Array Checked -> Source -> Maybe Thawed -> Checked
elabNext done s thaw =
  let
      direct = A.filter (\c -> A.elem c.module s.opens) done
      needed = A.filter (\c -> A.elem c.module (closure done s.deps)) done
      -- (each module's definitions, and the module itself, a struct of them;
      -- and what did not elaborate in each)
      env = A.foldl (\g c -> withEntry (c.module <> "#") c (withEntry (c.module <> "#T") c (A.foldl (\g' o -> withEntry o.key c g') (g { broken = Set.union g.broken c.top.globals.broken, instances = A.nub (g.instances <> c.top.globals.instances) }) c.own))) emptyGlobals needed
      withEntry k c g = maybe g (\e -> addGlobalEntry k e g) (lookupGlobal c.top.globals k)
      -- (what did not elaborate in a module imported, by its name there)
      brokenIn c = A.mapMaybe (\k -> S.stripPrefix (S.Pattern (c.module <> ".")) k <#> \x -> { name: x, key: k }) (A.fromFoldable c.top.globals.broken)
      imported = A.concatMap (\c -> map (\o -> { name: o.name, key: o.key, module: c.module }) c.own) direct
      -- a name two modules imported define, each its own way, is in scope
      -- as neither
      clashes = A.nubByEq (\a b -> a.name == b.name) (A.filter (\o -> A.any (\p -> p.name == o.name && p.key /= o.key) imported) imported)
      clash o =
        { code: 330, severity: SError, span: { start: 0, end: 0 }, note: Nothing
        , msg: P.text ("`" <> o.name <> "` is defined by more than one module imported: " <> S.joinWith ", " (A.nub (map _.module (A.filter (\p -> p.name == o.name) imported))))
        }
      g1 = A.foldl (\g o -> if o.key == o.name || A.any (\c -> c.name == o.name) clashes then g else addAlias o.name o.key g) env (imported <> A.concatMap (\c -> map (\b -> { name: b.name, key: b.key, module: c.module }) (brokenIn c)) direct)
      -- (`import m as M`: `m`'s definitions as `M.x`, and `M` the module
      -- itself, a value)
      g0 = A.foldl (\g q -> case A.find (\c -> c.module == q.module) done of
          Just c -> A.foldl (\g' o -> addAlias (q.alias <> "." <> o.name) o.key g') (addAlias q.alias (q.module <> "#") g) (map (\o -> { name: o.name, key: o.key }) c.own <> brokenIn c)
          Nothing -> g) g1 s.aliases
      key x = s.module <> "." <> x
      checked t ntns diags fixities cached =
        let
          own = A.zipWith (\(Tuple x _) k -> { name: x, key: k }) t.spans t.keys
        in
          { path: s.path, module: s.module, source: s.source, text: s.text, ntns
          , top: t { globals = moduleStruct s.module own t.globals }
          , diags
          , own
          , deps: s.deps
          , opens: s.opens
          , aliases: s.aliases
          , fixities
          , cached
          }
      -- a module the cache has was elaborated clean, and nothing it is
      -- made of has changed: its text is not even read
      thawed m = checked (topThawed g0 m.defs) [] [] m.fixities true
      fresh _ =
        let
          r = readSource (\x -> isJust (lookupGlobal g0 x)) (A.foldl (\f c -> union f c.fixities) emptyFixities direct) s.text
          -- (what does not parse: a use of it is not an error of its own)
          t = topModule key (A.foldl (\g x -> addBroken (key x) (addAlias x (key x) g)) g0 r.broken) r.ntns
        in
          checked t r.ntns (map clash clashes <> map hint (r.diags <> t.diags)) r.fixities false
    in
      maybe (fresh unit) thawed thaw
  where
  -- the modules reached from some, through their imports
  closure done' = go []
    where
    go seen ms = case A.uncons ms of
      Nothing -> seen
      Just { head, tail }
        | A.elem head seen -> go seen tail
        | otherwise -> go (A.snoc seen head) (tail <> maybe [] _.deps (A.find (\c -> c.module == head) done'))

-- | A file, after its definitions, as a value: a struct of them, under
-- | the module's own name (importers reach it by their alias, `import m
-- | as M`).  Its type is the file's theory -- the definitions' types as a
-- | telescope, a reference to an earlier definition becoming a reference
-- | to its field -- and its fields are the definitions themselves, so the
-- | struct adds no equations: `M.x` and the global are one.
moduleStruct :: String -> Array { name :: String, key :: String } -> Globals -> Globals
moduleStruct mkey own g =
  let
    entries = A.mapMaybe (\o -> lookupGlobal g o.key <#> \e -> { name: o.name, key: o.key, ty: e.tySyntax, level: V.levelOf e.type_ }) own
    level = A.foldl (\l r -> maxLevel l r.level) Small entries
  in
    -- (a file that defines theories has fields no universe holds: it is
    -- its definitions only, not a struct of them)
    if A.null entries || level == Top then g
    else
      let
        idx = Map.fromFoldable (A.mapWithIndex (\i r -> Tuple r.key i) entries)
        fieldTypes = dictFromList (A.mapWithIndex (\i r -> Tuple r.name (closeTy idx i 0 r.ty)) entries)
        -- the file's theory, a definition of its own (`m#`), so that the
        -- struct's type is a name, as every nominal type is
        tkey = mkey <> "#T"
        tEntry = entryFor g tkey false (U (if level == Large then LargeU else SmallU)) (Code (Record { level, fieldTypes }))
        g' = addGlobalEntry tkey tEntry g
        bodyStx = Syn.Cons (dictFromList (map (\r -> Tuple r.name (Global r.key)) entries))
      in
        addGlobalEntry (mkey <> "#") (entryFor g' (mkey <> "#") false (Decode (Global tkey)) bodyStx) g'

-- | a definition's type, moved into the telescope of the fields before
-- | field `i`: a reference to an earlier definition becomes its field, by
-- | index from `depth` binders down; anything else stays the global it was
closeTy :: Map.Map String Int -> Int -> Int -> Ty -> Ty
closeTy idx i depth = case _ of
  Decode e -> Decode (closeEl idx i depth e)
  Function f -> Function { level: f.level, dom: closeTy idx i depth f.dom, cod: closeAbs (closeTy idx i) depth f.cod, implicit: f.implicit }
  Record rt -> Record { level: rt.level, fieldTypes: dictFromList (A.mapWithIndex (\j (Tuple x t) -> Tuple x (closeTy idx i (depth + j) t)) (dictToList rt.fieldTypes)) }
  Sum (Variants vs) -> Sum (Variants (dictMap (map (closeTy idx i depth)) vs))
  IsTy t -> IsTy (closeTy idx i depth t)
  PrimArray t -> PrimArray (closeTy idx i depth t)
  other -> other

closeAbs :: forall a. (Int -> a -> a) -> Int -> Abs a -> Abs a
closeAbs f depth = case _ of
  Abs x t -> Abs x (f (depth + 1) t)
  -- (a constant abstraction does not bind: its body is a step shallower)
  AbsConst t -> AbsConst (f depth t)

closeEl :: Map.Map String Int -> Int -> Int -> El -> El
closeEl idx i depth = case _ of
  Global k -> case Map.lookup k idx of
    Just j | j < i -> Local (depth + (i - 1 - j))
    _ -> Global k
  Code t -> Code (closeTy idx i depth t)
  App f a -> App (closeEl idx i depth f) (closeEl idx i depth a)
  Lam t b -> Lam (closeTy idx i depth t) (closeAbs (closeEl idx i) depth b)
  Syn.Cons d -> Syn.Cons (dictMap (closeEl idx i depth) d)
  Proj e l -> Proj (closeEl idx i depth e) l
  Tag t as -> Tag t (map (closeEl idx i depth) as)
  Match e cs -> Match (closeEl idx i depth e) (dictMap (\(Tuple xs b) -> Tuple xs (closeEl idx i (depth + A.length xs) b)) cs)
  Specialize e l v -> Specialize (closeEl idx i depth e) l (closeEl idx i depth v)
  Is e -> Is (closeEl idx i depth e)
  Rec e -> Rec (closeEl idx i depth e)
  Implicit x e -> Implicit x (closeEl idx i depth e)
  MetaApp m xs t -> MetaApp m xs (closeTy idx i depth t)
  other -> other

-- | the tools add advice on how to fix the common cases
hint :: Diag -> Diag
hint d = case d.code, d.note of
  319, Nothing -> d { note = Just (P.text "if this is a recursive call, guard the whole call: rec (f ...)") }
  204, Nothing -> d { note = Just (P.text "name the type: `type T = ...` at top level, or a type field of the struct it is in (`t = { ... }`, then `m.t` here)") }
  _, _ -> d

errorsIn :: Checked -> Array Diag
errorsIn c = A.filter (\d -> d.severity == SError) c.diags

-- | each diagnostic under `path:line:col`
renderFileDiags :: String -> String -> Array Diag -> String
renderFileDiags path src ds = S.joinWith "" (map one ds)
  where
  one d =
    let lc = lineCol src d.span.start
    in path <> ":" <> show (lc.line + 1) <> ":" <> show (lc.col + 1) <> "\n" <> P.render (renderDiag src d) <> "\n\n"

-- | a global key by the name it has in the file, else the key (a
-- | synthesized one, which no file writes, in words)
localName :: Globals -> String -> String
localName g = \k -> fromMaybe (describeKey k) (Map.lookup k names)
  where
  names = Map.fromFoldable (map (\(Tuple x k) -> Tuple k x) (Map.toUnfoldable g.aliases :: Array (Tuple String String)))

-- | a type, its globals by the names they have in the file (else by key)
showTy :: Globals -> Ty -> String
showTy g = printTy { names: \_ _ -> [], hide: true } 0 Nil <<< renameTy (localName g)

-- | a file's interface: its imports and fixity declarations, its
-- | types in full (`data`, `type`, `theory` -- an importer sees through
-- | them), its values as signatures.  What `heff check` prints.
interface :: Checked -> String
interface c = S.joinWith "\n" (A.filter (_ /= "") [ imports, fixities, defs ])
  where
  imports = S.joinWith ""
    (map (\m -> "import " <> m <> "\n") c.opens
      <> map (\a -> "import " <> a.module <> " as " <> a.alias <> "\n") c.aliases)
  -- `above` and `below` relate a group to groups declared before it, so
  -- each relation is written on the group that came later
  fixities = S.joinWith "" (A.mapWithIndex fixityLine c.fixities.groups)
  fixityLine i g =
    assocKw g.assoc <> " " <> S.joinWith " " g.ops
      <> rel " above " (A.nub (A.mapMaybe (\e -> if e.tighter == g.id && earlier i e.looser then Just e.looser else Nothing) c.fixities.above))
      <> rel " below " (A.nub (A.mapMaybe (\e -> if e.looser == g.id && earlier i e.tighter then Just e.tighter else Nothing) c.fixities.above))
      <> "\n"
  earlier i x = case A.findIndex (\h -> h.id == x) c.fixities.groups of
    Just j -> j < i
    Nothing -> true
  rel kw ops = if A.null ops then "" else kw <> S.joinWith " " ops
  assocKw = case _ of
    AssocLeft -> "infixl"
    AssocRight -> "infixr"
    AssocNone -> "infix"
  -- a definition of more lines than one keeps blank lines around it
  defs = (A.foldl glue { out: "", multi: false, first: true } (A.mapMaybe entry c.own)).out
  glue acc d =
    let m = isJust (S.indexOf (S.Pattern "\n") (SCU.dropRight 1 d))
    in { out: acc.out <> (if not acc.first && (m || acc.multi) then "\n" else "") <> d, multi: m, first: false }
  -- (the type as the signature wrote it -- `Co e Unit`, an alias folded --
  -- not the readback of its evaluation, which would unfold every alias)
  entry o = lookupGlobal c.top.globals o.key <#> \e ->
    printSig im o.name (named o.name) (renameTy ln e.tySyntax) (renameEl ln e.syntax)
  ln = localName c.top.globals
  im = { names: \_ _ -> [], hide: true }

-- | a name as written alone: an operator in parentheses, `(+)`
named :: String -> String
named x = if isJust (operatorName x) then "(" <> x <> ")" else x

-- Watches and tests
--------------------------------------------------------------------------------

-- | a watch `-- >>> e`, with the `-- ...` lines under it, its expectation
-- | (a doctest): `expectFrom`..`expectTo` is where those lines are, or
-- | where they would go
type WatchInfo =
  { from :: Int
  , to :: Int
  , expr :: String
  , type :: String
  , value :: String
  , expected :: Maybe String
  , holds :: Boolean
  , expectFrom :: Int
  , expectTo :: Int
  }

-- | the `-- ...` lines after the line that `end` is on, up to a blank one
expectation :: String -> Int -> { expected :: Maybe String, from :: Int, to :: Int }
expectation src end =
  let
    lineEnd i = fromMaybe (SCU.length src) (SCU.indexOf' (S.Pattern "\n") i src)
    start = min (SCU.length src) (lineEnd end + 1)
    go i acc =
      let
        e = lineEnd i
        line = S.trim (SCU.slice i e src)
      in
        if i < SCU.length src && S.take 2 line == "--" && S.take 6 line /= "-- >>>" && S.trim (S.drop 2 line) /= "" then go (min (SCU.length src) (e + 1)) (A.snoc acc (S.trim (S.drop 2 line)))
        else { lines: acc, stop: i }
    r = go start []
  in
    { expected: if A.null r.lines then Nothing else Just (S.joinWith "\n" r.lines), from: start, to: r.stop }

watches :: Checked -> Array WatchInfo
watches c = A.mapWithIndex info c.top.watches
  where
  src = c.source
  info i w =
    let
      name = "> " <> show i
      value = evalWatch name (addGlobalEntry name w.entry c.top.globals)
      ex = expectation src w.span.end
      ty = showTy c.top.globals (readbTy 0 w.entry.type_)
      -- an expectation is Hippogriff: it holds when it evaluates to the
      -- same, however it is written; what cannot be read (`#fun`) is
      -- compared as text
      holds = case ex.expected of
        Just e -> e == value || readExpectation c.top.globals ty e == Just value
        Nothing -> true
    in
      { from: w.span.start
      , to: w.span.end
      , expr: S.trim (fromMaybe "" (S.stripPrefix (S.Pattern "-- >>>") (S.trim (SCU.slice w.span.start w.span.end src))))
      , type: ty
      , value
      , expected: ex.expected
      , holds
      , expectFrom: ex.from
      , expectTo: ex.to
      }

-- | a file's own definitions, evaluated (by value), as a watch shows them
evaluate :: Checked -> Array { name :: String, value :: String }
evaluate c =
  let
    keys = map _.key c.own
    results = evalGlobalsWhere (\k -> A.elem k keys) c.top.globals
  in
    A.mapMaybe (\o -> A.find (\(Tuple k _) -> k == o.key) results <#> \(Tuple _ r) -> { name: named o.name, value: P.render (showResult r) }) c.own

evalWatch :: String -> Globals -> String
evalWatch name g = maybe "" (\(Tuple _ res) -> P.render (showResult res)) (A.head (evalGlobalsWhere (_ == name) g))

-- | the value of an expectation, read as `(e :: T)`
readExpectation :: Globals -> String -> String -> Maybe String
readExpectation g ty e =
  let
    r = readProgram (\x -> isJust (lookupGlobal g x)) ("-- >>> (" <> e <> " : " <> ty <> ")\n")
    t = topWith g r.ntns
  in
    if not (A.null r.diags && A.null t.diags) then Nothing
    else (A.head t.watches) <#> \w -> evalWatch "> expectation" (addGlobalEntry "> expectation" w.entry g)

-- | a file's tests: the watches with an expectation, each under the name of
-- | the `test` before it (if any)
type TestReport = { test :: Maybe String, watch :: WatchInfo }

testFile :: Checked -> Array TestReport
testFile c = A.mapMaybe one (watches c)
  where
  one w = if isJust w.expected then Just { test: owner w, watch: w } else Nothing
  starts = map (\(Tuple _ sp) -> sp.start) c.top.spans
  owner w = _.name <$> A.last (A.filter (\m -> m.span.start < w.from && not (A.any (\p -> m.span.start < p && p < w.from) starts)) c.top.tests)

-- | write every watch's value under it as its expectation (the "promote" of
-- | an expect test)
promote :: Checked -> String
promote c = A.foldr put c.source (watches c)
  where
  -- from the last watch back, so earlier offsets stay right
  put w s =
    let
      lines = map ("-- " <> _) (S.split (S.Pattern "\n") w.value)
      before = SCU.take w.expectFrom s
      -- the watch may be the last line, with no newline after it
      sep = if before /= "" && SCU.takeRight 1 before /= "\n" then "\n" else ""
    in
      before <> sep <> S.joinWith "" (map (_ <> "\n") lines) <> SCU.drop w.expectTo s

-- | `h`, an effect (`Heff.Effect`): a sum, or a leaf -- a global, by its
-- | key, not its name (a definition that is a sum is seen through; one
-- | that is not a sum is a leaf: whether its module provides it is the
-- | caller's to ask, by the key)
effectOf :: Globals -> El -> Maybe Effect
effectOf g e = case e of
  Is e' -> effectOf g e'
  App (App (Global p) a) b | p == "co.plus" || p == "co.<+>" -> Plus <$> effectOf g a <*> effectOf g b
  App (Global k) _ -> Just (Leaf k)
  Global k -> case lookupGlobal g k >>= \entry -> effectOf g entry.syntax of
    Just eff -> Just eff
    Nothing -> Just (Leaf k)
  _ -> Nothing

-- | a computation's effect, from its type: `Co h a`, which is a function
-- | taking a `Handler h`
computationEffect :: Globals -> Ty -> Maybe Effect
computationEffect g = case _ of
  Function ft -> case ft.dom of
    Decode (App (Global "co.Handler") h) -> effectOf g h
    _ -> Nothing
  Decode (App (App (Global "co.Co") h) _) -> effectOf g h
  _ -> Nothing

-- | a computation's effect, from the type its key is stored with
effectOfKey :: Checked -> String -> Maybe Effect
effectOfKey c k = lookupGlobal c.top.globals k >>= \e -> computationEffect c.top.globals (readbTy 0 e.type_)

-- | the file's computations, `Co h a`, each with its effect
candidates :: Checked -> Array { name :: String, key :: String, effect :: Effect }
candidates c = A.mapMaybe (\o -> effectOfKey c o.key <#> \eff -> { name: o.name, key: o.key, effect: eff }) c.own

-- | every leaf of every candidate's effect, each once: what the caller
-- | asks providers for, before it picks one to run
candidateLeaves :: Checked -> Array String
candidateLeaves c = A.nub (A.concatMap (\r -> leaves r.effect) (candidates c))

-- | a definition to run where the effects `handled` (leaves, by their
-- | keys) have providers: the one named, else the first of a file's
-- | that fits
findRunnable :: Array String -> Checked -> Maybe String -> Either String { key :: String, effect :: Effect }
findRunnable handled c = case _ of
  Just x -> case A.find (\o -> o.name == x) c.own of
    Nothing -> Left ("no definition " <> x)
    Just o -> case effectOfKey c o.key of
      Nothing -> Left (x <> " is not a computation, `Co h a`")
      Just eff -> fits { name: x, key: o.key, effect: eff }
  Nothing -> case A.find (\r -> A.null (missing r.effect)) (candidates c), A.head (candidates c) of
    Just r, _ -> Right { key: r.key, effect: r.effect }
    -- (the first there is, said why it does not do)
    Nothing, Just r -> fits r
    Nothing, Nothing -> Left "no computation, `Co h a`, to run"
  where
  missing eff = A.nub (A.filter (\l -> not (A.elem l handled)) (leaves eff))
  fits r = case missing r.effect of
    [] -> Right { key: r.key, effect: r.effect }
    ms -> Left (r.name <> " has the effect " <> S.joinWith ", " ms <> ", which is not provided here (a module M provides its effect in M.provider.js beside M.hf; `$host` says where it runs)")

-- Semantic queries
--------------------------------------------------------------------------------

-- | the elaboration trace of a file, one line per event: where, what
explain :: Checked -> Array String
explain c =
  map (\ev -> at ev.span <> "  " <> ev.kind <> "  " <> force ev.text) c.top.trace
    <> map (\d -> at d.span <> "  error  " <> P.render (P.text "" <> d.msg)) c.diags
  where
  at sp = let lc = lineCol c.source sp.start in show (lc.line + 1) <> ":" <> show (lc.col + 1)

-- | what the name at an offset resolved to, and its type
-- | the name at an offset: one written there, the innermost.  (What `do`
-- | is read into, `bind`, is no name written: its span is the block's, wider
-- | than the name, and it must not hide what is in the block -- its binders,
-- | say, of which nothing is recorded.)
infoAt :: Checked -> Int -> Maybe Info
infoAt c off = A.head (A.sortWith (\i -> i.span.end - i.span.start) (A.filter (\i -> written i && i.span.start <= off && off <= i.span.end) c.top.infos))
  where
  -- (a tag may be written with its `'`)
  written i = i.span.end - i.span.start <= S.length i.name + 1

-- | What may be written where the name being typed ends, at `offset`: the
-- | names in scope that start as it does, each with its type.  After `M.`,
-- | the definitions of the module imported as `M`, without the `M.`.
completionsAt :: Checked -> Int -> Array { label :: String, detail :: String }
completionsAt c offset =
  A.sortWith _.label (A.mapMaybe one (A.nub names))
  where
  typed = SCU.fromCharArray (A.reverse (A.takeWhile isTypedChar (A.reverse (SCU.toCharArray (SCU.take offset c.source)))))
  isTypedChar ch = ch == '_' || ch == '\'' || ch == '.' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
  -- (`M.fo`: the qualifier `M.`, and `fo`)
  qualifier = case S.lastIndexOf (S.Pattern ".") typed of
    Just i -> S.take (i + 1) typed
    Nothing -> ""
  names = map _.name c.own <> A.fromFoldable (Map.keys c.top.globals.aliases)
  one x = case S.stripPrefix (S.Pattern typed) x of
    Just _ | qualifier /= "" || not (S.contains (S.Pattern ".") x) ->
      let label = fromMaybe x (S.stripPrefix (S.Pattern qualifier) x)
      in Just { label, detail: maybe "" (\e -> showTy c.top.globals (readbTy 0 e.type_)) (lookupGlobal c.top.globals x <|> lookupGlobal c.top.globals (c.module <> "." <> x)) }
    _ -> Nothing

typeAt :: Checked -> Int -> Maybe String
typeAt c off =
  infoAt c off <#> \i ->
    named i.name <> " : " <> force i.ty <> (case i.key of
      Just _ -> "   (global)"
      Nothing -> "   (local)")

-- | where the global the name at an offset resolved to is defined, in a
-- | program (the file last): its file, and the span of its name there.
-- | (A local's binder is not kept by the elaborator: nothing, for one.)
definitionAt :: Array Checked -> Int -> Maybe { path :: String, source :: String, span :: F.Span }
definitionAt program off = do
  c <- A.last program
  i <- infoAt c off
  k <- i.key
  -- (a primitive may be declared by more than one file: this one's first)
  d <- A.head (A.mapMaybe (defines k) (A.reverse program))
  -- (the name looked for is the one at the offset: a tag's is in its type's
  -- declaration)
  let target = if i.member then memberT k i.name else globalT k
  pure { path: d.c.path, source: d.c.source, span: maybe (nameIn d.c.source i.name d.span) _.span (A.find (\s -> s.target == target) (sites d.c)) }
  where
  defines k c = do
    j <- A.findIndex (\o -> o.key == k) c.own
    o <- A.index c.own j
    Tuple _ sp <- A.index c.top.spans j
    pure { c, name: o.name, span: sp }

-- | the first occurrence of a name, as a word, in a span (else the span)
nameIn :: String -> String -> F.Span -> F.Span
nameIn src x sp = go sp.start
  where
  go from = case SCU.indexOf' (S.Pattern x) from src of
    Just p | p + SCU.length x <= sp.end ->
      if wordAt src p x then { start: p, end: p + SCU.length x } else go (p + 1)
    _ -> sp

-- | is `x`, at `p` in `src`, a word: not part of a longer name?
wordAt :: String -> Int -> String -> Boolean
wordAt src p x = (p == 0 || not (isNameChar (SCU.charAt (p - 1) src))) && not (isNameChar (SCU.charAt (p + SCU.length x) src))

isNameChar :: Maybe Char -> Boolean
isNameChar = case _ of
  Just ch -> ch == '_' || ch == '\'' || ch == '/' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
  Nothing -> false

-- References
--------------------------------------------------------------------------------

-- | What a name refers to: a global, by its key; a tag or a field
-- | (`member`) of the type `key` names; or a local (`local`: where its
-- | binder is) of the file `key` is the path of.
type Target = { key :: String, member :: Maybe String, local :: Maybe F.Span }

globalT :: String -> Target
globalT k = { key: k, member: Nothing, local: Nothing }

memberT :: String -> String -> Target
memberT k m = { key: k, member: Just m, local: Nothing }

-- | a name written in a file; the span is the name's alone (`not` of
-- | `A.not`, `t` of `.t`, `x` of `'x`); `defines`: where it is defined
type Occurrence = { path :: String, source :: String, span :: F.Span, defines :: Boolean }

-- | Where a file defines its names, as its notation says: each
-- | declaration's name, in its signature and at the head of each equation
-- | (`not`, `(&&)`, `data Bool`, `theory Monoid`); a `data`'s tags; a
-- | `theory`'s fields.  (A declaration that did not elaborate defines
-- | nothing to refer to.)
sites :: Checked -> Array { target :: Target, name :: String, span :: F.Span }
sites c = A.concatMap decl c.ntns
  where
  keyOf x = _.key <$> A.find (\o -> o.name == x) c.own
  -- (the name alone: a tag's `'` is not part of it)
  site t x sp = { target: t, name: x, span: { start: max sp.start (sp.end - SCU.length x), end: sp.end } }
  global x sps = maybe [] (\k -> map (site (globalT k) x) sps) (keyOf x)
  members x ms = maybe [] (\k -> map (\m -> site (memberT k m.name) m.name m.span) ms) (keyOf x)
  decl = case _ of
    MDecl _ "def" (Infix (Ident f fsp) (Keyword ":" _) (Juxt _ (NBlock "where" Nothing eqs _))) _ ->
      global f (A.cons fsp (A.mapMaybe equationHead eqs))
    MDecl _ "def" (Infix (Infix h (Keyword ":" _) _) (Keyword ":=" _) body) _ | Just x <- headOf h ->
      global x.name [ x.span ] <> members x.name (case body of
        NBlock "sum" _ alts _ -> A.mapMaybe tagOf alts
        _ -> [])
    MDecl _ "theory" (Infix h (Keyword ":=" _) (NBlock "sig" _ fs _)) _ | Just x <- headOf h ->
      global x.name [ x.span ] <> members x.name (A.mapMaybe fieldOf fs)
    MDecl _ "primitive" (Infix (Ident x sp) (Keyword ":" _) _) _ -> global x [ sp ]
    _ -> []
  headOf h = case NEA.head (getGroup h) of
    Ident x sp -> Just { name: x, span: sp }
    _ -> Nothing
  equationHead = case _ of
    Infix l (Keyword ":=" _) _ -> _.span <$> headOf l
    _ -> Nothing
  tagOf alt = case NEA.head (getGroup alt) of
    NTag t sp -> Just { name: t, span: sp }
    _ -> Nothing
  fieldOf = case _ of
    Infix (Ident x sp) (Keyword ":" _) _ -> Just { name: x, span: sp }
    _ -> Nothing
-- | the name at an offset, in a program (the file last), as what it refers
-- | to: a global or a member used there, or one the file defines there; a
-- | local, at its binder or a use (`Heff.Locals`); and the name it is
-- | defined as
targetAt :: Array Checked -> Int -> Maybe { target :: Target, name :: String }
targetAt program off = do
  c <- A.last program
  let
    at sp = sp.start <= off && off <= sp.end
    ls = locals c.ntns
    withName t = case t.member of
      Just m -> Just { target: t, name: m }
      Nothing -> A.findMap (\c' -> _.name <$> A.find (\o -> o.key == t.key) c'.own) program <#> \x -> { target: t, name: x }
    site = A.find (at <<< _.span) (sites c) >>= withName <<< _.target
    -- (a field used by the fields after it: the field's)
    field = A.find (at <<< _.use) ls.fields >>= \f -> A.find (\s -> s.span == f.field) (sites c) >>= withName <<< _.target
    local = A.find (\l -> at l.binder || A.any at l.uses) ls.locals <#> \l -> { target: { key: c.path, member: Nothing, local: Just l.binder }, name: l.name }
  case infoAt c off of
    Just i | Just k <- i.key -> withName (if i.member then memberT k i.name else globalT k)
    _ -> site <|> field <|> local

-- | every occurrence in a program of what a name refers to (`name`, as it
-- | is defined), by file, in order: its definition, its uses -- the
-- | elaborator's record of each -- and, as text, in the files that use it,
-- | in a fixity declaration (an operator) and in a watch's expectation
-- | (a local: its binder and its uses, in its file)
occurrences :: Array Checked -> Target -> String -> Array Occurrence
occurrences program t name = case t.local of
  Just b -> fromMaybe [] do
    c <- A.find (\c -> c.path == t.key) program
    l <- A.find (\l -> l.binder == b) (locals c.ntns).locals
    pure (map (\sp -> { path: c.path, source: c.source, span: sp, defines: sp == b }) (A.sortWith _.start (A.cons l.binder l.uses)))
  Nothing -> A.concatMap inFile (A.nubByEq (\a b -> a.path == b.path) program)
  where
  inFile c =
    let
      ss = A.filter (\s -> s.target == t) (sites c)
      defs = map _.span ss
      -- (a field used by the fields after it)
      fieldUses = map _.use (A.filter (\f -> A.any (\s -> s.span == f.field) ss) (locals c.ntns).fields)
      found = A.mapMaybe use c.top.infos <> defs <> fieldUses
      more = if A.null found then [] else fixities c <> expected c
    in
      map (\sp -> { path: c.path, source: c.source, span: sp, defines: A.any (\d -> d.start == sp.start) defs }) (A.nubByEq (\a b -> a.start == b.start) (A.sortWith _.start (found <> more)))
  len = SCU.length name
  use i
    | i.key == Just t.key && (if i.member then t.member == Just i.name else t.member == Nothing) =
        Just { start: max i.span.start (i.span.end - len), end: i.span.end }
    | otherwise = Nothing
  operator = not (isNameChar (SCU.charAt 0 name))
  -- (`infixl + - above ==`: its words)
  fixities c
    | operator && t.member == Nothing = A.concatMap (fixityLine c.source) (A.filter (\p -> p == 0 || SCU.charAt (p - 1) c.source == Just '\n') (A.range 0 (SCU.length c.source - 1)))
    | otherwise = []
  fixityLine src p =
    let
      e = fromMaybe (SCU.length src) (SCU.indexOf' (S.Pattern "\n") p src)
      line = SCU.slice p e src
      ws = wordsOf p line
    in
      if A.any (\w -> A.elem w.text [ "infixl", "infixr", "infix" ]) (A.take 1 ws) then map _.span (A.filter (\w -> w.text == name) ws) else []
  wordsOf base line = go 0 []
    where
    n = SCU.length line
    go i acc
      | i >= n = acc
      | SCU.charAt i line == Just ' ' = go (i + 1) acc
      | otherwise =
          let j = fromMaybe n (SCU.indexOf' (S.Pattern " ") i line)
          in go j (A.snoc acc { text: SCU.slice i j line, span: { start: base + i, end: base + j } })
  expected c
    | operator = []
    | otherwise = A.concatMap (\w -> let ex = expectation c.source w.span.end in wordsIn c.source ex.from ex.to) c.top.watches
  wordsIn src from to = go from []
    where
    go i acc = case SCU.indexOf' (S.Pattern name) i src of
      Just p | p + len <= to -> go (p + 1) (if wordAt src p name then A.snoc acc { start: p, end: p + len } else acc)
      _ -> acc

-- | why a name cannot be renamed to another, if it cannot: a primitive is
-- | the runtime's; a name stays the kind it is (a variable, a capitalised
-- | name, an operator), and is not qualified
renameRefused :: Target -> String -> String -> Maybe String
renameRefused t old new
  | t.member == Nothing && t.local == Nothing && not (S.contains (S.Pattern ".") t.key) = Just ("`" <> old <> "` is a primitive: its name is the runtime's")
  | nameClass old /= nameClass new = Just ("`" <> new <> "` is not a name of the kind `" <> old <> "` is")
  | otherwise = Nothing

-- | a capitalised name (1), a variable (2), an operator (3), or none (0)
nameClass :: String -> Int
nameClass x = case SCU.charAt 0 x of
  Just ch | ch >= 'A' && ch <= 'Z' -> if all isNameChar' then 1 else 0
  Just ch | ch == '_' || (ch >= 'a' && ch <= 'z') -> if all isNameChar' then 2 else 0
  Just _ | all (\d -> not (isNameChar' d) && not (A.elem d [ ' ', '\n', '(', ')', '[', ']', '{', '}', ',', '"', '`' ])) -> 3
  _ -> 0
  where
  all p = SCU.length (SCU.takeWhile p x) == SCU.length x
  isNameChar' d = d /= '/' && isNameChar (Just d)

-- Symbols
--------------------------------------------------------------------------------

-- | a file's definitions, for an outline: each with where it is, where its
-- | name is, and its tags or fields
type DocSymbol = { name :: String, kind :: Int, span :: F.Span, selection :: F.Span, children :: Array { name :: String, kind :: Int, span :: F.Span } }

-- | (the kinds are the Language Server Protocol's: enum, interface,
-- | function, operator; enum member, field)
symbols :: Checked -> Array DocSymbol
symbols c = A.sortWith _.span.start (A.zipWith one c.own c.top.spans <> map broken c.top.broken)
  where
  -- (one that did not elaborate, not even its type: still where it is)
  broken (Tuple x sp) = { name: x, kind: kindOf x (SCU.slice sp.start sp.end c.source), span: sp, selection: nameIn c.source x sp, children: [] }
  kindOf x text
    | S.take 5 text == "data " = 10
    | S.take 7 text == "theory " = 11
    | not (isNameChar (SCU.charAt 0 x)) = 25
    | otherwise = 12
  ss = sites c
  one o (Tuple _ sp) =
    let
      text = SCU.slice sp.start sp.end c.source
      own = A.filter (\s -> s.span.start >= sp.start && s.span.end <= sp.end) ss
      sel = maybe (nameIn c.source o.name sp) _.span (A.find (\s -> s.target.member == Nothing) own)
      members = A.filter (\s -> isJust s.target.member) own
      isData = S.take 5 text == "data "
    in
      { name: o.name, kind: kindOf o.name text, span: sp, selection: sel
      , children: map (\m -> { name: m.name, kind: if isData then 22 else 8, span: m.span }) members
      }
