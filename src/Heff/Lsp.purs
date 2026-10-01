-- | A language server (the Language Server Protocol, over stdio) for `.hf`
-- | files: diagnostics, hover (`typeAt`), go to definition
-- | (`definitionAt`), formatting (`Heff.Format`).
-- |
-- | A document is elaborated with what it imports, from its text in the
-- | editor; an import open in the editor is read from there too, else from
-- | disk.
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
import Data.Traversable (for_)
import Data.Tuple (Tuple(..))
import Effect (Effect)
import Effect.Exception (message, try)
import Effect.Ref (Ref)
import Effect.Ref as Ref
import Foreign.Object as FO
import Heff.Cache (loadCached)
import Heff.Driver (Checked, completionsAt, definitionAt, typeAt)
import Heff.Format (format)
import Heff.Load (pathsFor)
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

-- | the documents open, by path; the last elaboration of each (of what
-- | text); the checks waiting for the typing to pause
type St =
  { docs :: Map String { uri :: String, text :: String }
  , checked :: Map String { text :: String, program :: Array Checked }
  , timers :: Map String Timer
  , shutdown :: Boolean
  }

-- | serve on stdin/stdout, with the standard library at `lib`
serve :: String -> Effect Unit
serve lib0 = do
  st <- Ref.new { docs: Map.empty, checked: Map.empty, timers: Map.empty, shutdown: false }
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

  notification m p = case m of
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
