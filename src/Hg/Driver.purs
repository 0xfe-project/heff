-- | A program, from text to what the tools report:
-- |
-- |   text -[Hg.Parse]-> notation -[Hg.Elab]-> core -[Hg.Eval | Hg.Compile]-> values, code
-- |
-- | A program is files; a file is elaborated on top of the definitions of
-- | the files it imports (`Hg.Load` finds them, in order).
module Hg.Driver
  ( Syntax(..)
  , syntaxOf
  , Source
  , Checked
  , elabProgram
  , errorsIn
  , renderFileDiags
  , definitions
  , WatchInfo
  , watches
  , TestReport
  , testFile
  , promote
  , findApp
  , explain
  , typeAt
  , definitionAt
  , goldenReport
  , goldenReportWith
  , refReport
  , checkReference
  ) where

import Prelude

import Data.Array as A
import Data.Lazy (force)
import Data.List (List(..), (:))
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe, isJust, maybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple (Tuple(..))
import Hg.Core.Globals (Globals, addAlias, addGlobalEntry, emptyGlobals, globalsList, lookupGlobal)
import Hg.Core.Print (prettyEl, prtTy)
import Hg.Core.Readback (readbTy)
import Hg.Core.Rename (renameTy)
import Hg.Core.Syntax (El(..), Ty(..))
import Hg.Elab.Environment (Info)
import Hg.Elab.Top (TopResult, top, topModule, topWith)
import Hg.Eval.Interpreter (EvalResult, evalGlobals, evalGlobalsWhere, prettyResult, showResult)
import Hg.Parse.Config (lexConfig, readConfig)
import Hg.Parse.FNotation (Diag, Ntn, Severity(..))
import Hg.Parse.FNotation as F
import Hg.Parse.Fixity (Fixities, emptyFixities, union)
import Hg.Parse.Surface (readProgram, readProgramWith)
import Hg.Print.Surface (printTy)
import Hg.Support.Diagnostic (lineCol, renderDiag, renderDiags)
import Hg.Support.Pretty ((<+>))
import Hg.Support.Pretty as P

-- | the two syntaxes: ours (`.hg`, `Hg.Parse.Surface`) and the reference's
-- | (`.hpgf`, fnotation; for comparing with it)
data Syntax = Surface | Reference

derive instance eqSyntax :: Eq Syntax

syntaxOf :: String -> Syntax
syntaxOf path = if S.contains (S.Pattern ".hpgf") (S.drop (S.length path - 5) path) then Reference else Surface

-- | a file as the loader found it: `module` is its name (`parse.surface`
-- | for `parse/surface.hg`), `text` its source with the import lines blanked
-- | out, `deps` the modules it imports
type Source = { path :: String, module :: String, source :: String, text :: String, deps :: Array String }

-- | a file, elaborated: `top.globals` has everything it needs, `own` its
-- | own definitions (by name, and the key each is stored under,
-- | `module.name`), `fixities` the fixities it declares
type Checked = { path :: String, module :: String, source :: String, text :: String, ntns :: Array Ntn, top :: TopResult, diags :: Array Diag, own :: Array { name :: String, key :: String }, deps :: Array String, fixities :: Fixities }

-- | read a file into notation; a capitalised name is a definition if
-- | `known` says so; `fixities`: those of what it imports
readSource :: Syntax -> (String -> Boolean) -> Fixities -> String -> { ntns :: Array Ntn, diags :: Array Diag, fixities :: Fixities }
readSource Reference _ _ src =
  let
    l = F.lex lexConfig src
    r = F.parse readConfig l.tokens
  in
    { ntns: r.ntns, diags: l.diags <> r.diags, fixities: emptyFixities }
readSource Surface known fixities src = readProgramWith { known, fixities, lenient: false } src

-- | the files of a program, in the order `Hg.Load` gives (each after what
-- | it imports), elaborated.  A file sees its own definitions and those of
-- | the modules it imports, by name; what those need is there too, by key,
-- | but not in scope
elabProgram :: Array Source -> Array Checked
elabProgram = A.foldl step []
  where
  step done s =
    let
      direct = A.filter (\c -> A.elem c.module s.deps) done
      needed = A.filter (\c -> A.elem c.module (closure done s.deps)) done
      env = A.foldl (\g c -> A.foldl (\g' o -> maybe g' (\e -> addGlobalEntry o.key e g') (lookupGlobal c.top.globals o.key)) g c.own) emptyGlobals needed
      imported = A.concatMap (\c -> map (\o -> { name: o.name, key: o.key, module: c.module }) c.own) direct
      -- a name two modules imported define, each its own way, is in scope
      -- as neither
      clashes = A.nubByEq (\a b -> a.name == b.name) (A.filter (\o -> A.any (\p -> p.name == o.name && p.key /= o.key) imported) imported)
      clash o =
        { code: 330, severity: SError, span: { start: 0, end: 0 }, note: Nothing
        , msg: P.text ("`" <> o.name <> "` is defined by more than one module imported: " <> S.joinWith ", " (A.nub (map _.module (A.filter (\p -> p.name == o.name) imported))))
        }
      g0 = A.foldl (\g o -> if o.key == o.name || A.any (\c -> c.name == o.name) clashes then g else addAlias o.name o.key g) env imported
      r = readSource (syntaxOf s.path) (\x -> isJust (lookupGlobal g0 x)) (A.foldl (\f c -> union f c.fixities) emptyFixities direct) s.text
      -- (a file in the reference's syntax is a program on its own, as the
      -- reference has it: its definitions keep their names)
      key x = if syntaxOf s.path == Reference then x else s.module <> "." <> x
      t = topModule key g0 r.ntns
    in
      A.snoc done
        { path: s.path, module: s.module, source: s.source, text: s.text, ntns: r.ntns, top: t
        , diags: map clash clashes <> map hint (r.diags <> t.diags)
        , own: A.zipWith (\(Tuple x _) k -> { name: x, key: k }) t.spans t.keys
        , deps: s.deps
        , fixities: r.fixities
        }
  -- the modules reached from some, through their imports
  closure done = go []
    where
    go seen ms = case A.uncons ms of
      Nothing -> seen
      Just { head, tail }
        | A.elem head seen -> go seen tail
        | otherwise -> go (A.snoc seen head) (tail <> maybe [] _.deps (A.find (\c -> c.module == head) done))

-- | the tools add advice on how to fix the common cases
hint :: Diag -> Diag
hint d = case d.code, d.note of
  319, Nothing -> d { note = Just (P.text "if this is a recursive call, guard the whole call: rec (f ...)") }
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

-- | a type, its globals by the names they have in the file (else by key)
showTy :: Globals -> Ty -> String
showTy g = printTy { names: \_ _ -> [], hide: true } 0 Nil <<< renameTy shown
  where
  names = Map.fromFoldable (map (\(Tuple x k) -> Tuple k x) (Map.toUnfoldable g.aliases :: Array (Tuple String String)))
  shown k = fromMaybe k (Map.lookup k names)

-- | a file's own definitions and their types
definitions :: Checked -> Array { name :: String, type :: String }
definitions c = A.mapMaybe (\o -> lookupGlobal c.top.globals o.key <#> \e -> { name: o.name, type: showTy c.top.globals (readbTy 0 e.type_) }) c.own

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

evalWatch :: String -> Globals -> String
evalWatch name g = maybe "" (\(Tuple _ res) -> P.render (showResult res)) (A.head (evalGlobalsWhere (_ == name) g))

-- | the value of an expectation, read as `(e :: T)`
readExpectation :: Globals -> String -> String -> Maybe String
readExpectation g ty e =
  let
    r = readProgram (\x -> isJust (lookupGlobal g x)) ("-- >>> (" <> e <> " :: " <> ty <> ")\n")
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

-- | an app is a definition that takes a `Handler (plus dom (state s))`
-- | (lib/dom.hg):
-- | the first of a file's, or the one named
findApp :: Checked -> Maybe String -> Maybe String
findApp c = case _ of
  Just x -> _.key <$> A.find (\o -> o.name == x) c.own
  Nothing -> _.key <$> A.find (isApp <<< _.key) c.own
  where
  isApp k = case lookupGlobal c.top.globals k <#> \e -> readbTy 0 e.type_ of
    Just (Function ft) -> case ft.dom of
      Decode (App (Global h) (App (App (Global p) (Global d)) (App (Global st) _))) ->
        short h == "Handler" && short p == "plus" && short d == "dom" && short st == "state"
      _ -> false
    _ -> false
  short k = fromMaybe k (A.last (S.split (S.Pattern ".") k))

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
-- | the name at an offset: the innermost (what `do` is read into, `bind`,
-- | spans the whole block, and must not hide the names in it)
infoAt :: Checked -> Int -> Maybe Info
infoAt c off = A.head (A.sortWith (\i -> i.span.end - i.span.start) (A.filter (\i -> i.span.start <= off && off <= i.span.end) c.top.infos))

typeAt :: Checked -> Int -> Maybe String
typeAt c off =
  infoAt c off <#> \i ->
    i.name <> " :: " <> force i.ty <> (case i.key of
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

-- The reference's reports
--------------------------------------------------------------------------------

-- | a file in the reference's syntax, as the reference elaborates it
checkReference :: String -> { ntns :: Array Ntn, globals :: Globals, diags :: Array Diag }
checkReference src =
  let
    r = readSource Reference (const false) emptyFixities src
    t = top r.ntns
  in
    { ntns: r.ntns, globals: t.globals, diags: r.diags <> t.diags }

-- | what the reference executable prints: the report on stdout, the
-- | diagnostics on stderr
refReport :: String -> { out :: String, err :: String }
refReport src =
  let
    c = checkReference src
  in
    { out: P.render $ P.vsep [ P.text "-- elaborated", P.vsep (declsDoc c.globals), P.text "-- evaluated", P.vsep (resultsDoc (evalGlobals c.globals)) ]
    , err: renderDiags src c.diags
    }

-- | what the reference's golden tests print
goldenReport :: String -> String
goldenReport = goldenReportWith (\src -> let c = checkReference src in { globals: c.globals, diags: c.diags })

-- | the same, of what the tools elaborate (`hg golden --tools`)
goldenReportWith :: (String -> { globals :: Globals, diags :: Array Diag }) -> String -> String
goldenReportWith check src =
  let
    c = check src
  in
    P.render $ P.vsep
      [ P.text "-- elaborated"
      , P.vsep (declsDoc c.globals)
      , P.text "-- evaluated"
      , P.vsep (resultsDoc (evalGlobals c.globals))
      , P.text "-- messages"
      , P.pretty (renderDiags src c.diags)
      ]

declsDoc :: Globals -> Array P.Doc
declsDoc gs = A.concatMap
  ( \(Tuple x e) ->
      [ P.text "name:" <+> P.text x
      , P.text "type:" <+> prtTy { len: 0, names: Nil } e.type_
      , P.text "syntax:" <+> prettyEl (x : Nil) e.syntax
      ]
  )
  (globalsList gs)

resultsDoc :: Array (Tuple String EvalResult) -> Array P.Doc
resultsDoc = map (\(Tuple x r) -> P.text x <+> P.text "=" <+> prettyResult r)
