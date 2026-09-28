module Main where

import Prelude

import Data.Array as A
import Data.Either (Either(..))
import Data.Int as Int
import Data.Lazy (force)
import Data.Maybe (Maybe(..), fromMaybe, isJust)
import Data.String as S
import Data.Traversable (for_)
import Effect (Effect)
import Effect.Console (log)
import Heff.Compile.JavaScript (compileApp, compileJs, compileNode)
import Heff.Driver (Checked, definitions, elabProgram, errorsIn, explain, evaluate, findRunnable, promote, renderFileDiags, testFile, typeAt, watches)
import Heff.Elab.Environment (enableTraceSink, takeTraceSink)
import Heff.Format (format)
import Heff.Load (loadProgram, pathsFor)
import Heff.Lsp as Lsp
import Heff.Support.Diff (mismatch, useColor)
import Node.Encoding (Encoding(..))
import Node.FS.Sync (readTextFile, realpath, writeTextFile)
import Node.Process (argv, exit', stderr, stdout)
import Node.Stream (writeString)

-- | the commands: how each is called, what it does, and what its words
-- | complete to (`files`: the files' suffix; `flags`)
commands :: Array { name :: String, args :: String, doc :: String, files :: String, flags :: Array { flag :: String, doc :: String } }
commands =
  [ { name: "check", args: "FILE", doc: "elaborate FILE and what it imports; print its definitions' types", files: ".hf", flags: [] }
  , { name: "eval", args: "FILE", doc: "print what each definition of FILE evaluates to", files: ".hf", flags: [] }
  , { name: "run", args: "FILE", doc: "print what each watch (`-- >>> e`) of FILE evaluates to", files: ".hf", flags: [] }
  , { name: "test", args: "FILE...", doc: "check the watches' expectations (the `-- ...` lines under them)", files: ".hf", flags: [] }
  , { name: "promote", args: "FILE", doc: "write each watch's value under it as its expectation", files: ".hf", flags: [] }
  , { name: "compile-js", args: "FILE OUT.js", doc: "an ES module of FILE and what it imports", files: ".hf", flags: [] }
  , { name: "app", args: "FILE [NAME] OUT.js", doc: "a script running the app NAME (lib/dom.hf) in a page", files: ".hf", flags: [] }
  , { name: "node", args: "FILE [NAME] OUT.mjs", doc: "a Node.js program running NAME (lib/io.hf)", files: ".hf", flags: [] }
  , { name: "explain", args: "FILE", doc: "the elaboration trace", files: ".hf", flags: [] }
  , { name: "type-at", args: "FILE OFFSET", doc: "what the name at OFFSET resolved to, and its type", files: ".hf", flags: [] }
  , { name: "fmt", args: "[--check] FILE...", doc: "format FILEs in place (--check: only say which are not formatted)", files: ".hf", flags: [ { flag: "check", doc: "only say which are not formatted" } ] }
  , { name: "lsp", args: "", doc: "a language server, on stdin/stdout", files: "", flags: [] }
  , { name: "completions", args: "fish", doc: "a completion script for the shell", files: "", flags: [] }
  ]

usage :: String
usage = S.joinWith "\n" ([ "usage: heff COMMAND FILE ...", "" ] <> map line commands)
  where
  line c = let head = c.name <> (if c.args == "" then "" else " " <> c.args) in "  " <> head <> S.joinWith "" (A.replicate (max 1 (24 - S.length head)) " ") <> c.doc

-- | fish's completions of heff: `heff completions fish > ~/.config/fish/completions/hg.fish`
fishCompletions :: String
fishCompletions = S.joinWith "\n" ([ "# heff: written by `heff completions fish`", "complete -c heff -e", "complete -c heff -f" ] <> A.concatMap one commands) <> "\n"
  where
  quote x = "'" <> S.replaceAll (S.Pattern "'") (S.Replacement "\\'") x <> "'"
  one c =
    [ "complete -c heff -n __fish_use_subcommand -a " <> c.name <> " -d " <> quote c.doc ]
      <> (if c.files == "" then [] else [ "complete -c heff -n '__fish_seen_subcommand_from " <> c.name <> "' -a '(__fish_complete_suffix " <> c.files <> ")'" ])
      <> map (\f -> "complete -c heff -n '__fish_seen_subcommand_from " <> c.name <> "' -l " <> f.flag <> " -d " <> quote f.doc) c.flags
      <> (if c.name == "completions" then [ "complete -c heff -n '__fish_seen_subcommand_from completions' -a fish" ] else [])

err :: String -> Effect Unit
err s = void (writeString stderr UTF8 s)

-- | the standard library, next to `bin/` (the script's own, not that of a
-- | link to it: `npm link` makes one)
libDir :: Effect String
libDir = do
  args <- argv
  script <- realpath (fromMaybe "bin/heff.mjs" (A.index args 1))
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
    [ "eval", f ] -> do
      l <- load f
      for_ (evaluate l.file) \d -> log (d.name <> " = " <> d.value)
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
      for_ r.errors \e -> err ("heff: not compiled: " <> e <> "\n")
      when (not l.ok || not (A.null r.errors)) (exit' 1)
    [ "app", f, out ] -> app f Nothing out
    [ "app", f, n, out ] -> app f (Just n) out
    [ "node", f, out ] -> program f Nothing out
    [ "node", f, n, out ] -> program f (Just n) out
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
    [ "completions", "fish" ] -> void (writeString stdout UTF8 fishCompletions)
    [] -> log usage
    [ "help" ] -> log usage
    -- (on stderr: what reads the output, a shell's completion, say, does not
    -- take it for an answer)
    _ -> err (usage <> "\n") *> exit' 1
  where
  app f n out = run [ "dom", "state" ] compileApp f n out "load it in a page with <script src=\"...\"> (it imports nothing: it opens from file:// too)"
  program f n out = run [ "io", "state" ] compileNode f n out ("run it with `node " <> out <> " ARGS...`")
  -- (a computation of the effects `handled`, compiled with their handlers)
  run handled compile f n out how = do
    l <- load f
    case findRunnable handled l.file n of
      Left why -> err ("heff: " <> f <> ": " <> why <> "\n") *> exit' 1
      Right r -> do
        let c = compile l.file.top.globals r.key r.effect
        for_ c.errors \e -> err ("heff: not compiled: " <> e <> "\n")
        when (not (A.null c.errors)) (exit' 1)
        writeTextFile UTF8 out c.source
        log ("wrote " <> out <> ": " <> how)
  -- | 1 if the file is not formatted (with `--check`) or cannot be
  fmtOne checkOnly f = do
    src <- readTextFile UTF8 f
    case format src of
      Left why -> err ("heff: " <> f <> " not formatted: " <> why <> "\n") $> 1
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
