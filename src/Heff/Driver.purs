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
  , completionsAt
  , explain
  , typeAt
  , definitionAt
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
import Heff.Core.Globals (Globals, addAlias, addGlobalEntry, emptyGlobals, lookupGlobal)
import Heff.Core.Readback (readbTy)
import Heff.Core.Rename (describeKey, renameEl, renameTy)
import Heff.Core.Syntax (Abs(..), El(..), Ty(..))
import Heff.Core.Syntax as Syn
import Heff.Effect (Effect(..), leaves)
import Heff.Elab.Environment (Info)
import Heff.Core.Params (Level(..), Universe(..), Variants(..), maxLevel)
import Heff.Core.Value as V
import Heff.Elab.Top (ThawedDef, TopResult, entryFor, topModule, topThawed, topWith)
import Heff.Eval.Interpreter (evalGlobalsWhere, showResult)
import Heff.Parse.Notation (Diag, Ntn, Severity(..))
import Heff.Parse.Notation as F
import Heff.Parse.Fixity (Assoc(..), Fixities, emptyFixities, union)
import Heff.Support.Common (Dict, dictFromList, dictMap, dictToList, operatorName)
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
readSource :: (String -> Boolean) -> Fixities -> String -> { ntns :: Array Ntn, diags :: Array Diag, fixities :: Fixities }
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
      -- (each module's definitions, and the module itself, a struct of them)
      env = A.foldl (\g c -> withEntry (c.module <> "#") c (withEntry (c.module <> "#T") c (A.foldl (\g' o -> withEntry o.key c g') g c.own))) emptyGlobals needed
      withEntry k c g = maybe g (\e -> addGlobalEntry k e g) (lookupGlobal c.top.globals k)
      imported = A.concatMap (\c -> map (\o -> { name: o.name, key: o.key, module: c.module }) c.own) direct
      -- a name two modules imported define, each its own way, is in scope
      -- as neither
      clashes = A.nubByEq (\a b -> a.name == b.name) (A.filter (\o -> A.any (\p -> p.name == o.name && p.key /= o.key) imported) imported)
      clash o =
        { code: 330, severity: SError, span: { start: 0, end: 0 }, note: Nothing
        , msg: P.text ("`" <> o.name <> "` is defined by more than one module imported: " <> S.joinWith ", " (A.nub (map _.module (A.filter (\p -> p.name == o.name) imported))))
        }
      g1 = A.foldl (\g o -> if o.key == o.name || A.any (\c -> c.name == o.name) clashes then g else addAlias o.name o.key g) env imported
      -- (`import m as M`: `m`'s definitions as `M.x`, and `M` the module
      -- itself, a value)
      g0 = A.foldl (\g q -> case A.find (\c -> c.module == q.module) done of
          Just c -> A.foldl (\g' o -> addAlias (q.alias <> "." <> o.name) o.key g') (addAlias q.alias (q.module <> "#") g) c.own
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
          t = topModule key g0 r.ntns
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

-- | `h`, an effect (`Heff.Effect`): a sum, a leaf -- by what it is, a
-- | global's key, not its name -- or a definition that is one of those
effectOf :: Globals -> El -> Maybe Effect
effectOf g e = case e of
  Is e' -> effectOf g e'
  App (App (Global p) a) b | p == "co.plus" || p == "co.<+>" -> Plus <$> effectOf g a <*> effectOf g b
  Global "dom.dom" -> Just (Leaf "dom")
  Global "stdio.stdio" -> Just (Leaf "stdio")
  Global "fs.fs" -> Just (Leaf "fs")
  Global "proc.proc" -> Just (Leaf "proc")
  Global "time.time" -> Just (Leaf "time")
  Global "net.net" -> Just (Leaf "net")
  App (Global "state.state") _ -> Just (Leaf "state")
  Global k -> lookupGlobal g k >>= \entry -> effectOf g entry.syntax
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

-- | a definition to run where there are handlers for the effects
-- | `handled`: the one named, else the first of a file's that fits
findRunnable :: Array String -> Checked -> Maybe String -> Either String { key :: String, effect :: Effect }
findRunnable handled c = case _ of
  Just x -> case A.find (\o -> o.name == x) c.own of
    Nothing -> Left ("no definition " <> x)
    Just o -> case effectOf' o.key of
      Nothing -> Left (x <> " is not a computation, `Co h a`, of the effects " <> known)
      Just eff -> fits { name: x, key: o.key, effect: eff }
  Nothing -> case A.find (\r -> A.null (missing r.effect)) computations, A.head computations of
    Just r, _ -> Right { key: r.key, effect: r.effect }
    -- (the first there is, said why it does not do)
    Nothing, Just r -> fits r
    Nothing, Nothing -> Left ("no computation, `Co h a`, of the effects " <> S.joinWith ", " handled)
  where
  known = "dom, stdio, fs, proc, time, net, state s and their sums"
  computations = A.mapMaybe (\o -> effectOf' o.key <#> \eff -> { name: o.name, key: o.key, effect: eff }) c.own
  missing eff = A.nub (A.filter (\l -> not (A.elem l handled)) (leaves eff))
  fits r = case missing r.effect of
    [] -> Right { key: r.key, effect: r.effect }
    ms -> Left (r.name <> " has the effect " <> S.joinWith ", " ms <> ", which is not handled here (only " <> S.joinWith ", " handled <> ")")
  effectOf' k = lookupGlobal c.top.globals k >>= \e -> computationEffect c.top.globals (readbTy 0 e.type_)

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
  typed = SCU.fromCharArray (A.reverse (A.takeWhile isNameChar (A.reverse (SCU.toCharArray (SCU.take offset c.source)))))
  isNameChar ch = ch == '_' || ch == '\'' || ch == '.' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
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
  pure { path: d.c.path, source: d.c.source, span: nameIn d.c.source i.name d.span }
  where
  defines k c = do
    j <- A.findIndex (\o -> o.key == k) c.own
    o <- A.index c.own j
    Tuple _ sp <- A.index c.top.spans j
    pure { c, name: o.name, span: sp }
  -- the first occurrence of the name, as a word, in the declaration
  nameIn src x sp =
    let
      word p = not (isNameChar (SCU.charAt p src))
      go from = case SCU.indexOf' (S.Pattern x) from src of
        Just p | p + SCU.length x <= sp.end ->
          if (p == 0 || word (p - 1)) && word (p + SCU.length x) then { start: p, end: p + SCU.length x } else go (p + 1)
        _ -> sp
    in
      go sp.start
  isNameChar = case _ of
    Just ch -> ch == '_' || ch == '\'' || ch == '/' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
    Nothing -> false
