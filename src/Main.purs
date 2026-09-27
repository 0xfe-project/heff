module Main where

import Prelude

import Data.Argonaut.Core as J
import Data.Array as A
import Data.Either (Either(..))
import Data.Int as Int
import Data.Lazy (force)
import Data.Maybe (Maybe(..), fromMaybe, isJust)
import Data.String as S
import Data.Traversable (for_)
import Data.Tuple (Tuple(..))
import Effect (Effect)
import Effect.Console (log)
import Hg.Compile.JavaScript (compileApp, compileJs)
import Hg.Compile.PureScript (compileModule, psValueName)
import Hg.Core.Globals (globalsList)
import Hg.Driver (Checked, definitions, elabProgram, errorsIn, explain, findApp, goldenReport, goldenReportWith, promote, refReport, renderFileDiags, testFile, typeAt, watches)
import Hg.Elab.Environment (enableTraceSink, takeTraceSink)
import Hg.Format (format)
import Hg.Load (loadProgram, pathsFor)
import Hg.Lsp as Lsp
import Hg.Support.Diff (mismatch, useColor)
import Hg.Support.Pretty as P
import Node.Encoding (Encoding(..))
import Node.FS.Sync (readTextFile, writeTextFile)
import Node.Process (argv, exit', stderr, stdout)
import Node.Stream (writeString)

usage :: String
usage = S.joinWith "\n"
  [ "usage: hg COMMAND FILE ..."
  , ""
  , "  check FILE              elaborate FILE and what it imports; print its definitions' types"
  , "  run FILE                print what each watch (`-- >>> e`) of FILE evaluates to"
  , "  test FILE...            check the watches' expectations (the `-- ...` lines under them)"
  , "  promote FILE            write each watch's value under it as its expectation"
  , "  compile-js FILE OUT.js  an ES module of FILE and what it imports"
  , "  app FILE [NAME] OUT.js  a script running the app NAME (lib/dom.hg) in a page"
  , "  compile FILE MOD OUT    a PureScript module"
  , "  explain FILE            the elaboration trace"
  , "  type-at FILE OFFSET     what the name at OFFSET resolved to, and its type"
  , "  golden [--tools] FILE   the reference's golden report (.hpgf)"
  , "  ref FILE                what the reference executable prints (.hpgf)"
  , "  fmt [--check] FILE...   format FILEs in place (--check: only say which are not formatted)"
  , "  lsp                     a language server, on stdin/stdout"
  ]

err :: String -> Effect Unit
err s = void (writeString stderr UTF8 s)

-- | the standard library, next to `bin/`
libDir :: Effect String
libDir = do
  args <- argv
  let script = fromMaybe "bin/hg.mjs" (A.index args 1)
  pure (S.joinWith "/" (A.dropEnd 2 (S.split (S.Pattern "/") script) <> [ "lib" ]))

-- | a file and what it imports, elaborated, the file itself last.  What
-- | does not load, or does not elaborate, is reported on stderr.
load :: String -> Effect { program :: Array Checked, file :: Checked, ok :: Boolean }
load f = do
  lib <- libDir
  ps <- pathsFor lib f
  l <- loadProgram ps f
  for_ l.errors \e -> err (e <> "\n")
  let program = elabProgram l.sources
  for_ program \c -> when (not (A.null c.diags)) (err (renderFileDiags c.path c.source c.diags))
  case A.last program of
    Just file -> pure { program, file, ok: A.null l.errors && A.all (A.null <<< errorsIn) program }
    Nothing -> exit' 1

main :: Effect Unit
main = do
  args <- argv
  case A.drop 2 args of
    [ "check", f ] -> do
      l <- load f
      for_ (definitions l.file) \d -> log (d.name <> " :: " <> d.type)
      when (not l.ok) (exit' 1)
    [ "run", f ] -> do
      l <- load f
      for_ (watches l.file) \w -> log (">>> " <> w.expr <> "\n" <> w.value)
      when (not l.ok) (exit' 1)
    [ "promote", f ] -> do
      l <- load f
      if l.ok then writeTextFile UTF8 f (promote l.file) else exit' 1
    [ "compile-js", f, out ] -> do
      l <- load f
      let r = compileJs l.file.top.globals l.file.own
      writeTextFile UTF8 out r.source
      for_ r.errors \e -> err ("hg: not compiled: " <> e <> "\n")
      when (not l.ok || not (A.null r.errors)) (exit' 1)
    [ "app", f, out ] -> app f Nothing out
    [ "app", f, n, out ] -> app f (Just n) out
    [ "compile", f, modName, outFile ] -> do
      l <- load f
      let
        g = l.file.top.globals
        r = compileModule modName g identity
        names = map (\(Tuple x _) -> J.fromArray [ J.fromString x, J.fromString (psValueName x) ]) (globalsList g)
      writeTextFile UTF8 outFile r.source
      writeTextFile UTF8 (outFile <> ".json") (J.stringify (J.fromArray names))
      -- (what does not elaborate is left out, and said so)
      for_ (r.skipped <> map (\d -> "(not elaborated) " <> P.render d.msg) l.file.diags) \x -> log ("skipped " <> x)
      for_ r.errors \x -> log ("error " <> x)
      when (not (A.null r.errors)) (exit' 1)
    [ "explain", f ] -> do
      -- the trace is kept outside the state too, so an internal error does
      -- not take it along
      enableTraceSink
      l <- load f
      let lines = explain l.file
      for_ lines log
      when (A.any (S.contains (S.Pattern "internal error")) lines) do
        evs <- takeTraceSink
        log "-- the trace up to the internal error:"
        for_ evs \ev -> log (show ev.span.start <> "  " <> ev.kind <> "  " <> force ev.text)
    [ "type-at", f, o ] -> do
      l <- load f
      log (fromMaybe "nothing there" (Int.fromString o >>= typeAt l.file))
    [ "golden", f ] -> do
      src <- readTextFile UTF8 f
      log (goldenReport src)
    [ "golden", "--tools", f ] -> do
      l <- load f
      log (goldenReportWith (\_ -> { globals: l.file.top.globals, diags: l.file.diags }) l.file.source)
    [ "ref", f ] -> do
      src <- readTextFile UTF8 f
      let r = refReport src
      void (writeString stdout UTF8 r.out)
      void (writeString stderr UTF8 r.err)
    -- (clients add flags, `--stdio` say: stdin/stdout is all there is)
    a | A.head a == Just "lsp" && A.all (\x -> S.take 2 x == "--") (A.drop 1 a) -> libDir >>= Lsp.serve
    a | A.head a == Just "fmt" && A.length a > 1 -> do
      let
        checkOnly = A.index a 1 == Just "--check"
        files = A.drop (if checkOnly then 2 else 1) a
      bad <- A.foldM (\n f -> (n + _) <$> fmtOne checkOnly f) 0 files
      when (bad > 0) (exit' 1)
    a | A.head a == Just "test" && A.length a > 1 -> do
      results <- A.foldM (\acc f -> (\r -> acc <> [ r ]) <$> testOne f) [] (A.drop 1 a)
      let
        total = A.foldl (\n r -> n + r.total) 0 results
        failed = A.foldl (\n r -> n + r.failed) 0 results
      log (show (total - failed) <> "/" <> show total <> " expectations hold")
      when (failed > 0 || A.any (not <<< _.ok) results) (exit' 1)
    _ -> log usage *> exit' 1
  where
  app f n out = do
    l <- load f
    case findApp l.file n of
      Nothing -> err ("hg: no app " <> fromMaybe "" n <> " in " <> f <> " (an app takes a `Handler (dom <+> (state s))`, lib/dom.hg)\n") *> exit' 1
      Just k -> do
        let r = compileApp l.file.top.globals k
        for_ r.errors \e -> err ("hg: not compiled: " <> e <> "\n")
        when (not (A.null r.errors)) (exit' 1)
        writeTextFile UTF8 out r.source
        log ("wrote " <> out <> ": load it in a page with <script src=\"...\"> (it imports nothing: it opens from file:// too), after a <div id=\"hg-app\"> (else it uses the body)")
  -- | 1 if the file is not formatted (with `--check`) or cannot be
  fmtOne checkOnly f = do
    src <- readTextFile UTF8 f
    case format src of
      Left why -> err ("hg: " <> f <> " not formatted: " <> why <> "\n") $> 1
      Right out
        | out == src -> pure 0
        | checkOnly -> log f $> 1
        | otherwise -> writeTextFile UTF8 f out $> 0
  testOne f = do
    l <- load f
    let
      ts = testFile l.file
      bad = A.filter (not <<< _.watch.holds) ts
    color <- useColor
    for_ bad \t -> do
      let m = mismatch color (fromMaybe "" t.watch.expected) t.watch.value
      log ("FAIL " <> f <> (case t.test of
        Just n -> " (" <> n <> ")"
        Nothing -> "") <> ": " <> t.watch.expr <> "\n  expected " <> m.expected <> "\n  but got  " <> m.got <> "\n  diff     " <> m.diff)
    log ((if A.null bad && l.ok then "ok    " else "FAIL  ") <> f <> "  " <> show (A.length ts - A.length bad) <> "/" <> show (A.length ts))
    pure { total: A.length ts, failed: A.length bad, ok: l.ok && A.all (isJust <<< _.watch.expected) ts }
