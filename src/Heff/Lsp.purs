-- | A language server (the Language Server Protocol, over stdio) for `.hf`
-- | files: diagnostics, hover (`typeAt`), go to definition
-- | (`definitionAt`), completion, formatting (`Heff.Format`), an outline
-- | (`symbols`); and, over the whole project, references and renaming
-- | (`occurrences`), and the symbols of every file.
-- |
-- | A document is elaborated with what it imports, from its text in the
-- | editor; an import open in the editor is read from there too, else from
-- | disk.  The project -- every file under its root, with what each
-- | imports -- is elaborated when it is asked about, and kept until a
-- | document changes.
module Heff.Lsp
  ( serve
  ) where

import Prelude

import Data.Argonaut.Core (Json)
import Data.Argonaut.Core as J
import Data.Argonaut.Parser (jsonParser)
import Data.Array as A
import Data.Either (Either(..))
import Data.Int as Int
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe, maybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Traversable (for_, traverse)
import Data.Tuple (Tuple(..))
import Effect (Effect)
import Effect.Exception (message, try)
import Effect.Ref (Ref)
import Effect.Ref as Ref
import Foreign.Object as FO
import Heff.Cache (loadCached)
import Heff.Driver (Checked, Occurrence, Target, completionsAt, definitionAt, elabProgram, errorsIn, occurrences, renameRefused, symbols, targetAt, typeAt)
import Heff.Format (format)
import Heff.Load (Paths, loadProgramWith, pathsFor)
import Heff.Parse.Notation (Diag, Severity(..), Span)
import Heff.Parse.Import (readImports)
import Heff.Support.Diagnostic (lineCol, renderDiagMessage)
import Heff.Support.Pretty as P
import Node.Encoding (Encoding(..))
import Node.FS.Sync (readTextFile)
import Node.Process (exit')

foreign import data Timer :: Type

foreign import listenImpl :: (String -> Effect Unit) -> Effect Unit
foreign import sendImpl :: String -> Effect Unit
foreign import logImpl :: String -> Effect Unit
foreign import setTimeoutImpl :: Int -> Effect Unit -> Effect Timer
foreign import clearTimeoutImpl :: Timer -> Effect Unit
foreign import uriPathImpl :: (String -> Maybe String) -> Maybe String -> String -> Maybe String
foreign import pathUri :: String -> String
foreign import absPath :: String -> String
foreign import projectFilesImpl :: String -> Effect (Array String)

-- | the documents open, by path; the last elaboration of each (of what
-- | text); the checks waiting for the typing to pause; the project last
-- | elaborated, by its root (forgotten when a document changes)
type St =
  { docs :: Map String { uri :: String, text :: String }
  , checked :: Map String { text :: String, program :: Array Checked }
  , timers :: Map String Timer
  , project :: Maybe { root :: String, program :: Array Checked }
  , shutdown :: Boolean
  }

-- | serve on stdin/stdout, with the standard library at `lib`
serve :: String -> Effect Unit
serve lib0 = do
  st <- Ref.new { docs: Map.empty, checked: Map.empty, timers: Map.empty, project: Nothing, shutdown: false }
  listenImpl \body -> case jsonParser body of
    Left e -> logImpl ("not JSON: " <> e)
    Right msg -> handle (absPath lib0) st msg

-- JSON
--------------------------------------------------------------------------------

field :: String -> Json -> Maybe Json
field k j = J.toObject j >>= FO.lookup k

path :: Array String -> Json -> Maybe Json
path ks j = A.foldM (flip field) j ks

str :: Array String -> Json -> Maybe String
str ks j = path ks j >>= J.toString

int :: Array String -> Json -> Maybe Int
int ks j = path ks j >>= J.toNumber >>= Int.fromNumber

obj :: Array (Tuple String Json) -> Json
obj = J.fromObject <<< FO.fromFoldable

num :: Int -> Json
num = J.fromNumber <<< Int.toNumber

send :: Array (Tuple String Json) -> Effect Unit
send m = sendImpl (J.stringify (obj (A.cons (Tuple "jsonrpc" (J.fromString "2.0")) m)))

reply :: Json -> Json -> Effect Unit
reply id result = send [ Tuple "id" id, Tuple "result" result ]

replyError :: Json -> Int -> String -> Effect Unit
replyError id code msg = send [ Tuple "id" id, Tuple "error" (obj [ Tuple "code" (num code), Tuple "message" (J.fromString msg) ]) ]

notify :: String -> Json -> Effect Unit
notify method params = send [ Tuple "method" (J.fromString method), Tuple "params" params ]

-- Positions
--------------------------------------------------------------------------------

-- | an LSP position (a line, and UTF-16 code units into it) as an offset:
-- | a PureScript string is JavaScript's, indexed by code units too
offsetOf :: String -> Int -> Int -> Int
offsetOf src line ch = go 0 0
  where
  len = SCU.length src
  lineEnd i = fromMaybe len (SCU.indexOf' (S.Pattern "\n") i src)
  go i l
    | l >= line = min (i + max 0 ch) (lineEnd i)
    | otherwise = case SCU.indexOf' (S.Pattern "\n") i src of
        Just e -> go (e + 1) (l + 1)
        Nothing -> len

position :: String -> Int -> Json
position src i = let lc = lineCol src (max 0 (min (SCU.length src) i)) in obj [ Tuple "line" (num lc.line), Tuple "character" (num lc.col) ]

range :: String -> Span -> Json
range src sp = obj [ Tuple "start" (position src sp.start), Tuple "end" (position src (max sp.start sp.end)) ]

-- | the offset a `textDocument/...` request is at
offsetIn :: String -> Json -> Int
offsetIn src params = offsetOf src (fromMaybe 0 (int [ "position", "line" ] params)) (fromMaybe 0 (int [ "position", "character" ] params))

-- Messages
--------------------------------------------------------------------------------

handle :: String -> Ref St -> Json -> Effect Unit
handle lib st msg = case str [ "method" ] msg, field "id" msg of
  Just m, Just id -> do
    s <- Ref.read st
    r <- try (if s.shutdown && m /= "shutdown" then replyError id (-32600) "shut down" else request m id params)
    case r of
      Left e -> logImpl (m <> ": " <> message e) *> replyError id (-32603) (message e)
      Right _ -> pure unit
  Just m, Nothing -> do
    r <- try (notification m params)
    case r of
      Left e -> logImpl (m <> ": " <> message e)
      Right _ -> pure unit
  -- (a response: we ask nothing)
  Nothing, _ -> pure unit
  where
  params = fromMaybe J.jsonNull (field "params" msg)

  request m id p = case m of
    "initialize" -> reply id $ obj
      [ Tuple "capabilities" $ obj
          [ Tuple "textDocumentSync" (obj [ Tuple "openClose" J.jsonTrue, Tuple "change" (num 1), Tuple "save" (obj [ Tuple "includeText" J.jsonFalse ]) ])
          , Tuple "hoverProvider" J.jsonTrue
          , Tuple "definitionProvider" J.jsonTrue
          , Tuple "documentFormattingProvider" J.jsonTrue
          -- (after `M.`, the module's definitions)
          , Tuple "completionProvider" (obj [ Tuple "triggerCharacters" (J.fromArray [ J.fromString "." ]) ])
          , Tuple "referencesProvider" J.jsonTrue
          , Tuple "renameProvider" (obj [ Tuple "prepareProvider" J.jsonTrue ])
          , Tuple "documentSymbolProvider" J.jsonTrue
          , Tuple "workspaceSymbolProvider" J.jsonTrue
          ]
      , Tuple "serverInfo" (obj [ Tuple "name" (J.fromString "heff") ])
      ]
    "shutdown" -> Ref.modify_ (_ { shutdown = true }) st *> reply id J.jsonNull
    "textDocument/hover" -> withProgram p \src program ->
      reply id $ maybe J.jsonNull (\t -> obj [ Tuple "contents" (obj [ Tuple "kind" (J.fromString "markdown"), Tuple "value" (J.fromString ("```heff\n" <> t <> "\n```")) ]) ])
        (A.last program >>= \c -> typeAt c (offsetIn src p))
    "textDocument/definition" -> withProgram p \src program -> do
      s <- Ref.read st
      reply id $ maybe J.jsonNull (\d -> obj [ Tuple "uri" (J.fromString (maybe (pathUri d.path) _.uri (Map.lookup d.path s.docs))), Tuple "range" (range d.source d.span) ])
        (definitionAt program (offsetIn src p))
    "textDocument/completion" -> withProgram p \src program ->
      reply id $ J.fromArray $ maybe [] (\c -> map (\i -> obj [ Tuple "label" (J.fromString i.label), Tuple "kind" (num 3), Tuple "detail" (J.fromString i.detail) ]) (completionsAt c (offsetIn src p))) (A.last program)
    "textDocument/documentSymbol" -> withProgram p \src program ->
      reply id $ J.fromArray $ maybe [] (map (docSymbol src) <<< symbols) (A.last program)
    -- (in the projects of the documents open)
    "workspace/symbol" -> do
      s <- Ref.read st
      roots <- traverse (\f -> pathsFor lib f <#> \ps -> { root: ps.root, f }) (A.fromFoldable (Map.keys s.docs))
      programs <- traverse (\r -> project lib st r.f) (A.nubByEq (\a b -> a.root == b.root) roots)
      let query = S.toLower (fromMaybe "" (str [ "query" ] p))
      reply id $ J.fromArray $ A.concatMap (\c ->
        map (\y -> obj [ Tuple "name" (J.fromString y.name), Tuple "kind" (num y.kind), Tuple "containerName" (J.fromString c.module), Tuple "location" (obj [ Tuple "uri" (J.fromString (uriOf s c.path)), Tuple "range" (range c.source y.selection) ]) ])
          (A.filter (\y -> S.contains (S.Pattern query) (S.toLower y.name)) (symbols c)))
        (A.nubByEq (\a b -> a.path == b.path) (A.concat programs))
    "textDocument/references" -> withTarget p \s _ t ->
      let decl = fromMaybe true (path [ "context", "includeDeclaration" ] p >>= J.toBoolean)
      in reply id $ J.fromArray $ map (location s) (A.filter (\o -> decl || not o.defines) t.occurrences)
    "textDocument/prepareRename" -> withTarget p \_ off t ->
      case renameRefused t.target t.name t.name, A.find (\o -> o.path == t.path && o.span.start <= off && off <= o.span.end) t.occurrences of
        Just why, _ -> replyError id (-32803) why
        Nothing, Just o -> reply id (obj [ Tuple "range" (range o.source o.span), Tuple "placeholder" (J.fromString t.name) ])
        Nothing, Nothing -> replyError id (-32803) ("`" <> t.name <> "` is not written here")
    "textDocument/rename" -> withTarget p \s _ t -> do
      let new = fromMaybe "" (str [ "newName" ] p)
      case renameRefused t.target t.name new of
        Just why -> replyError id (-32803) why
        Nothing -> do
          checked <- renameChecked lib st t new
          case checked of
            Just why -> replyError id (-32803) why
            Nothing -> reply id $ obj
              [ Tuple "changes" $ J.fromObject $ FO.fromFoldable $ map (\os -> Tuple (uriOf s os.path) (J.fromArray (map (\o -> obj [ Tuple "range" (range o.source o.span), Tuple "newText" (J.fromString new) ]) os.occurrences))) (byFile t.occurrences)
              ]
    "textDocument/formatting" -> do
      s <- Ref.read st
      case docPath p >>= \f -> Map.lookup f s.docs of
        Nothing -> reply id (J.fromArray [])
        Just d -> case format d.text of
          Left why -> do
            notify "window/showMessage" (obj [ Tuple "type" (num 2), Tuple "message" (J.fromString ("heff: not formatted: " <> why)) ])
            reply id (J.fromArray [])
          Right t
            | t == d.text -> reply id (J.fromArray [])
            | otherwise -> reply id (J.fromArray [ obj [ Tuple "range" (range d.text { start: 0, end: SCU.length d.text }), Tuple "newText" (J.fromString t) ] ])
    _ -> replyError id (-32601) ("no method " <> m)

  notification m p = do
    -- (a document changed: the project is elaborated again when asked about)
    when (S.take 13 m == "textDocument/") (Ref.modify_ (_ { project = Nothing }) st)
    notification' m p

  notification' m p = case m of
    "exit" -> Ref.read st >>= \s -> exit' (if s.shutdown then 0 else 1)
    "textDocument/didOpen" -> for_ (docPath p) \f -> do
      let uri = fromMaybe "" (str [ "textDocument", "uri" ] p)
      Ref.modify_ (\s -> s { docs = Map.insert f { uri, text: fromMaybe "" (str [ "textDocument", "text" ] p) } s.docs }) st
      checkNow f
    "textDocument/didChange" -> for_ (docPath p) \f ->
      -- (full sync: the last change is the whole text)
      for_ (path [ "contentChanges" ] p >>= J.toArray >>= A.last >>= str [ "text" ]) \text -> do
        Ref.modify_ (\s -> s { docs = Map.update (\d -> Just d { text = text }) f s.docs }) st
        s <- Ref.read st
        for_ (Map.lookup f s.timers) clearTimeoutImpl
        t <- setTimeoutImpl 200 (checkNow f)
        Ref.modify_ (\s' -> s' { timers = Map.insert f t s'.timers }) st
    "textDocument/didSave" -> for_ (docPath p) checkNow
    "textDocument/didClose" -> for_ (docPath p) \f -> do
      s <- Ref.read st
      for_ (Map.lookup f s.timers) clearTimeoutImpl
      for_ (Map.lookup f s.docs) \d -> publish d.uri []
      Ref.write (s { docs = Map.delete f s.docs, checked = Map.delete f s.checked, timers = Map.delete f s.timers }) st
    -- (`initialized`, `$/...`, and what we do not know)
    _ -> pure unit

  docPath p = str [ "textDocument", "uri" ] p >>= uriPathImpl Just Nothing

  -- a document, and those open that import it (as last elaborated)
  checkNow f = do
    s <- Ref.read st
    for_ (Map.lookup f s.timers) clearTimeoutImpl
    Ref.modify_ (\s' -> s' { timers = Map.delete f s'.timers }) st
    check lib st f
    let dependents = Map.keys (Map.filterWithKey (\g c -> g /= f && A.any (\c' -> c'.path == f) c.program) s.checked)
    for_ dependents (check lib st)

  -- the document a request is about, elaborated as its text is now
  withProgram p k = do
    s0 <- Ref.read st
    case docPath p of
      Just f | Just d <- Map.lookup f s0.docs -> do
        when (maybe true (\c -> c.text /= d.text) (Map.lookup f s0.checked)) (check lib st f)
        s <- Ref.read st
        case Map.lookup f s.checked of
          Just c | c.text == d.text -> k d.text c.program
          _ -> k d.text []
      _ -> k "" []

  -- what the name at a request's position refers to, in the project of
  -- its document: the name, and every occurrence of it
  withTarget p k = do
    s <- Ref.read st
    case docPath p of
      Just f | Just d <- Map.lookup f s.docs -> do
        program <- project lib st f
        let off = offsetIn d.text p
        case A.find (\c -> c.path == f) program of
          Just c | c.source == d.text, Just t <- targetAt (A.snoc (A.filter (\c' -> c'.path /= f) program) c) off ->
            k s off { path: f, target: t.target, name: t.name, occurrences: occurrences program t.target t.name }
          _ -> reply' J.jsonNull
      _ -> reply' J.jsonNull
    where
    reply' r = case field "id" msg of
      Just id -> reply id r
      Nothing -> pure unit

publish :: String -> Array Json -> Effect Unit
publish uri ds = notify "textDocument/publishDiagnostics" (obj [ Tuple "uri" (J.fromString uri), Tuple "diagnostics" (J.fromArray ds) ])

-- Diagnostics
--------------------------------------------------------------------------------

-- | elaborate a document open, and publish what is wrong with it
check :: String -> Ref St -> String -> Effect Unit
check lib st f = do
  s <- Ref.read st
  for_ (Map.lookup f s.docs) \d -> do
    let read p = maybe (readTextFile UTF8 p) (pure <<< _.text) (Map.lookup p s.docs)
    r <- try do
      ps <- pathsFor lib f
      loadCached read ps f
    case r of
      Left e -> do
        logImpl (f <> ": " <> message e)
        publish d.uri [ diagnostic d.text { code: 0, severity: SError, span: { start: 0, end: 0 }, note: Nothing, msg: P.text (message e) } ]
      Right l -> do
        Ref.modify_ (\s' -> s' { checked = Map.insert f { text: d.text, program: l.program } s'.checked }) st
        publish d.uri (map (diagnostic d.text) (diagsOf d.text l))

-- | a document's own diagnostics; what did not load; the modules it
-- | imports that do not elaborate, at their `import` lines
diagsOf :: String -> { program :: Array Checked, errors :: Array String } -> Array Diag
diagsOf src l = own <> map loadError l.errors <> A.mapMaybe broken (A.dropEnd 1 l.program)
  where
  own = maybe [] _.diags (A.last l.program)
  imports = (readImports src).imports
  at m = maybe { start: 0, end: 0 } _.span (A.find (\i -> i.name == m) imports)
  loadError e =
    let sp = maybe { start: 0, end: 0 } _.span (A.find (\i -> S.contains (S.Pattern (" " <> i.name <> " ")) e) imports)
    in { code: 0, severity: SError, span: sp, note: Nothing, msg: P.text e }
  broken c = case A.filter (\d -> d.severity == SError) c.diags of
    ds | not (A.null ds) && A.any (\i -> i.name == c.module) imports -> Just
      { code: 0, severity: SWarning, span: at c.module, note: Nothing
      , msg: P.text ("module " <> c.module <> " (" <> c.path <> ") does not elaborate: " <> show (A.length ds) <> " error" <> (if A.length ds == 1 then "" else "s"))
      }
    _ -> Nothing

diagnostic :: String -> Diag -> Json
diagnostic src d = obj $
  [ Tuple "range" (range src d.span)
  , Tuple "severity" (num sev)
  , Tuple "source" (J.fromString "heff")
  , Tuple "message" (J.fromString (P.render (renderDiagMessage d)))
  ] <> (if d.code == 0 then [] else [ Tuple "code" (J.fromString code) ])
  where
  sev = case d.severity of
    SError -> 1
    SWarning -> 2
    SDebug -> 4
  letter = case d.severity of
    SError -> "E"
    SWarning -> "W"
    SDebug -> "D"
  code = letter <> SCU.takeRight 4 ("0000" <> show d.code)

-- The project
--------------------------------------------------------------------------------

-- | the project a document is in, every file of it elaborated, an open one
-- | as its text is in the editor (the last one elaborated, if it is still
-- | that project's)
project :: String -> Ref St -> String -> Effect (Array Checked)
project lib st f = do
  ps <- pathsFor lib f
  s <- Ref.read st
  case s.project of
    Just pr | pr.root == ps.root -> pure pr.program
    _ -> do
      program <- elabProject (readOpen s) ps f
      Ref.modify_ (_ { project = Just { root: ps.root, program } }) st
      pure program

readOpen :: St -> String -> Effect String
readOpen s p = maybe (readTextFile UTF8 p) (pure <<< _.text) (Map.lookup p s.docs)

-- | every file of a project (and `f`, if it is not one), with what each
-- | imports, elaborated as one program
elabProject :: (String -> Effect String) -> Paths -> String -> Effect (Array Checked)
elabProject read ps f = do
  files <- projectFilesImpl ps.root
  loads <- traverse (loadProgramWith read ps) (A.nub (A.snoc files f))
  pure (elabProgram (A.nubByEq (\a b -> a.path == b.path) (A.concatMap _.sources loads)))

-- | Why renaming would change what the program means, if it would: with
-- | every occurrence renamed, the project is elaborated again, and the new
-- | name must be written as often as the old one was, each time referring
-- | to what was renamed -- not captured by a name already there -- and no
-- | file may have more errors than it had.
renameChecked :: String -> Ref St -> { path :: String, target :: Target, name :: String, occurrences :: Array Occurrence } -> String -> Effect (Maybe String)
renameChecked lib st t new = do
  s <- Ref.read st
  ps <- pathsFor lib t.path
  before <- project lib st t.path
  let
    texts = Map.fromFoldable (map (\os -> Tuple os.path (renamed os.occurrences)) (byFile t.occurrences))
    renamed os = A.foldr (\o src -> SCU.take o.span.start src <> new <> SCU.drop o.span.end src) (maybe "" _.source (A.head os)) os
  after <- elabProject (\p -> maybe (readOpen s p) pure (Map.lookup p texts)) ps t.path
  let
    target = case t.target.member of
      Just _ -> t.target { member = Just new }
      Nothing -> t.target { key = SCU.take (SCU.length t.target.key - SCU.length t.name) t.target.key <> new }
    errors program = A.foldl (\n c -> n + A.length (errorsIn c)) 0 (A.nubByEq (\a b -> a.path == b.path) program)
    count = A.length (occurrences after target new)
  pure
    if errors after > errors before then Just ("renaming `" <> t.name <> "` to `" <> new <> "` would make " <> show (errors after - errors before) <> " more error(s)")
    else if count /= A.length t.occurrences then Just ("`" <> new <> "` is already a name some of these occurrences would refer to")
    else Nothing

-- | occurrences, by file
byFile :: Array Occurrence -> Array { path :: String, occurrences :: Array Occurrence }
byFile os = map (\p -> { path: p, occurrences: A.filter (\o -> o.path == p) os }) (A.nub (map _.path os))

uriOf :: St -> String -> String
uriOf s p = maybe (pathUri p) _.uri (Map.lookup p s.docs)

location :: St -> Occurrence -> Json
location s o = obj [ Tuple "uri" (J.fromString (uriOf s o.path)), Tuple "range" (range o.source o.span) ]

docSymbol :: String -> { name :: String, kind :: Int, span :: Span, selection :: Span, children :: Array { name :: String, kind :: Int, span :: Span } } -> Json
docSymbol src y = obj
  [ Tuple "name" (J.fromString y.name)
  , Tuple "kind" (num y.kind)
  , Tuple "range" (range src { start: y.span.start, end: max y.span.end y.selection.end })
  , Tuple "selectionRange" (range src y.selection)
  , Tuple "children" (J.fromArray (map (\m -> obj [ Tuple "name" (J.fromString m.name), Tuple "kind" (num m.kind), Tuple "range" (range src m.span), Tuple "selectionRange" (range src m.span) ]) y.children))
  ]
