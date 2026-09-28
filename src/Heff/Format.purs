-- | The formatter of the Haskell-like syntax (`.hf`).  The text is read
-- | again, into a concrete syntax that keeps every token (the notation the
-- | elaborator takes drops parentheses and sugar), each comment attached to
-- | a token, and laid out anew with `Heff.Support.Pretty`.
-- |
-- | Formatting never changes what a file means: the result is read with
-- | `Heff.Parse.Surface` and must give the same notation (spans aside), the
-- | same diagnostics and the same comments, in order; otherwise the file is
-- | left as it is, with the reason.
-- |
-- | Blank lines between declarations (and between the items of a block) are
-- | kept, at most one; parentheses are kept where they were; a watch
-- | (`-- >>> e`) is kept as it was written, since its expectation is read by
-- | lines.
module Heff.Format
  ( format
  , formatUnchecked
  ) where

import Prelude

import Control.Monad.Except (ExceptT, runExceptT, throwError)
import Control.Monad.State (State, evalState, gets, modify_)
import Data.Array as A
import Data.Either (Either(..))
import Data.Foldable (fold, foldMap)
import Data.FoldableWithIndex (foldMapWithIndex)
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe, maybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple (Tuple(..))
import Heff.Parse.Notation (Ntn(..))
import Heff.Parse.Import (readImports)
import Heff.Parse.Fixity (emptyFixities, reserved)
import Heff.Parse.Surface (lex, readProgramWith)
import Heff.Support.BigInt as BI
import Heff.Support.Pretty (Doc, (<+>))
import Heff.Support.Pretty as P

-- | a file formatted, or why it was left as it is
format :: String -> Either String String
format src = do
  out <- formatUnchecked src
  check src out
  pure out

-- | laid out anew, what it means not checked
formatUnchecked :: String -> Either String String
formatUnchecked src = do
  let
    hdr = readImports src
    l = lex hdr.text
  unless (A.null l.errors) $ Left "the file does not lex"
  let
    imports = map (\im -> { kind: "import", text: "", start: im.span.start, end: im.span.end, col: 0, first: true }) hdr.imports
    lexed = A.sortWith _.start (imports <> map (\t -> { kind: t.kind, text: t.text, start: t.start, end: t.end, col: t.col, first: t.first }) l.tokens)
    at = attach src lexed l.comments
  decls <- evalState (runExceptT program) { toks: at.toks, i: 0, stack: [ 0 ], start: 0 }
  let
    eof = fromMaybe (dummy (-1)) (A.last at.toks)
  pure (render (topLevel at.env decls eof))
  where
  dummy ix = { kind: "eof", text: "", raw: "", start: 0, end: 0, col: 0, first: true, ix, blank: false }

-- | the formatted text means what the text did
check :: String -> String -> Either String Unit
check src out = do
  let
    read s =
      let
        h = readImports s
        -- (without what it imports, whose fixities are not known: operators
        -- bind the same way in both, whatever way)
        r = readProgramWith { known: const false, fixities: emptyFixities, lenient: true } h.text
      in
        { imports: map _.name h.imports, ntns: map ntnKey r.ntns, diags: map (\d -> P.render d.msg) r.diags }
    a = read src
    b = read out
    comments s = map (\c -> rtrim c.text) (lex (readImports s).text).comments
  unless (a.imports == b.imports) $ Left "formatting would change the imports"
  unless (a.ntns == b.ntns && a.diags == b.diags) $ Left "formatting would change what the file means (a bug of the formatter)"
  unless (comments src == comments out) $ Left "formatting would lose or move a comment across code (a bug of the formatter)"

ntnKey :: Ntn -> String
ntnKey = case _ of
  Juxt a b -> "(" <> ntnKey a <> " " <> ntnKey b <> ")"
  Infix a b c -> "{" <> ntnKey a <> " " <> ntnKey b <> " " <> ntnKey c <> "}"
  NBlock s m xs _ -> "[" <> s <> " " <> maybe "" ntnKey m <> " | " <> S.joinWith "; " (map ntnKey xs) <> "]"
  MDecl ms s n _ -> "<" <> S.joinWith "," ms <> " " <> s <> " " <> ntnKey n <> ">"
  NTuple xs _ -> "(," <> S.joinWith ", " (map ntnKey xs) <> ")"
  NBrace n _ -> "{b " <> ntnKey n <> "}"
  Ident x _ -> "i:" <> x
  Keyword x _ -> "k:" <> x
  NField x _ -> "f:" <> x
  NTag x _ -> "t:" <> x
  NInt i _ -> "n:" <> BI.toString i
  NFloat x _ -> "f:" <> show x
  NString s _ -> "s:" <> show s
  NError _ -> "error"

-- Tokens and comments
--------------------------------------------------------------------------------

-- | a token: `raw` is its text as written, `ix` its place, `blank` whether a
-- | blank line is before it
type T = { kind :: String, text :: String, raw :: String, start :: Int, end :: Int, col :: Int, first :: Boolean, ix :: Int, blank :: Boolean }

-- | a comment on lines of its own, before a token
type Cmt = { text :: String, blank :: Boolean }

-- | `src`: the text; `lead`: the comments on lines of their own before a
-- | token; `inline`: those just before it, on its line; `trail`: the comment
-- | after it, on its line; `noLead`: the token whose leading comments are
-- | already laid out (by the item it starts), `noTrail` the token whose
-- | trailing comment is (by the item it ends)
type Env = { src :: String, lead :: Map Int (Array Cmt), inline :: Map Int (Array String), trail :: Map Int String, noLead :: Int, noTrail :: Int }

-- | tokens and comments together, in order, with the lines each starts and
-- | ends on: a `{- -}` comment with a token after it on its line is inline,
-- | before that token; another comment after a token on its line trails it;
-- | any other leads the next token, on lines of its own
attach
  :: String
  -> Array { kind :: String, text :: String, start :: Int, end :: Int, col :: Int, first :: Boolean }
  -> Array { start :: Int, end :: Int, text :: String }
  -> { toks :: Array T, env :: Env }
attach src toks comments =
  let
    elems0 = A.sortWith (\e -> e.start) (map (\t -> { start: t.start, end: t.end, tok: Just t, text: "" }) toks <> map (\c -> { start: c.start, end: c.end, tok: Nothing, text: c.text }) comments)
    newlines a b = let s = SCU.slice a b src in SCU.length s - SCU.length (S.replaceAll (S.Pattern "\n") (S.Replacement "") s)
    lined = A.foldl (\acc e ->
      let
        startLine = acc.line + newlines acc.offset e.start
        endLine = startLine + newlines e.start e.end
      in
        acc { line = endLine, offset = e.end, out = A.snoc acc.out { start: e.start, end: e.end, tok: e.tok, text: e.text, startLine, endLine } })
      { line: 0, offset: 0, out: [] } elems0
    elems = lined.out
    inlineAt k e = S.take 2 e.text == "{-" && case A.index elems (k + 1) of
      Just n | Just _ <- n.tok -> n.startLine == e.endLine
      _ -> false
    step st (Tuple k e) =
      let
        blank = st.prevEnd >= 0 && e.startLine - st.prevEnd >= 2
        st' = st { prevEnd = e.endLine }
      in
        case e.tok of
          Just t ->
            let
              ix = A.length st.toks
              raw = if t.kind == "import" then S.trim (SCU.slice t.start t.end src) else SCU.slice t.start t.end src
              t' = { kind: t.kind, text: t.text, raw, start: t.start, end: t.end, col: t.col, first: t.first, ix, blank }
            in
              st'
                { toks = A.snoc st.toks t'
                , pending = []
                , inline = []
                , lead = if A.null st.pending then st.lead else Map.insert ix st.pending st.lead
                , inl = if A.null st.inline then st.inl else Map.insert ix st.inline st.inl
                , lastTok = Just { ix, line: e.endLine }
                }
          Nothing
            | inlineAt k e -> st' { inline = A.snoc st.inline e.text }
            | Just lt <- st.lastTok, lt.line == e.startLine && A.null st.pending && A.null st.inline ->
                st' { trail = Map.insertWith (\a b -> a <> " " <> b) lt.ix e.text st.trail, lastTok = Just { ix: lt.ix, line: e.endLine } }
            | otherwise -> st' { pending = A.snoc st.pending { text: e.text, blank } }
    r = A.foldl step { prevEnd: -1, toks: [], pending: [], inline: [], lead: Map.empty, inl: Map.empty, trail: Map.empty, lastTok: Nothing } (A.mapWithIndex Tuple elems)
  in
    { toks: r.toks, env: { src, lead: r.lead, inline: r.inl, trail: r.trail, noLead: -1, noTrail: -1 } }

-- The concrete syntax
--------------------------------------------------------------------------------

data E
  = Atom T
  | Paren T E T
  | Named T T T E T -- ^ `(x := e)`
  | Tup T (Array E) (Array T) T
  | Braces T (Array Fld) (Array T) T
  | Sum T T (Array Con) (Array T) T
  | Field E T
  | App E (Array E)
  | Op E T E
  | Ops E (Array { op :: T, e :: E }) -- ^ declared operators, `a + b * c`
  | Ann E T E
  | Lam T (Array Bnd) T E
  | Case T E T (Array Alt)
  | If T E T E T E
  | Do T (Array Stmt)

data Fld = FSet T T E | FDecl E

data Bnd = BVar T | BImp T T T

type Con = { con :: T, args :: Array E }

type Alt = { pat :: Pat, arrow :: T, body :: E }

data Pat = PCon T (Array Pat) | PAtom T | PParen T Pat T

data Stmt = SBind T T E | SExpr E

type TheoryField = { name :: T, colons :: T, ty :: E }

-- | (a declaration's name: `f`, or `(`, `+`, `)`)
type Nm = Array T

data D
  = DImport T
  | DFixity T (Array T) -- ^ `infixl + - above ==`
  | DSig Nm T E
  | DEqn Nm (Array Pat) T E
  | DData T T (Array E) (Maybe (Tuple T E)) (Maybe { eq :: T, alts :: Array Con, bars :: Array T })
  | DType T T (Array E) T E
  | DTheory T T (Array E) T (Array TheoryField)
  | DForeign T T T T E
  | DTest T T
  | DWatch T E

firstE :: E -> T
firstE = case _ of
  Atom t -> t
  Paren o _ _ -> o
  Named o _ _ _ _ -> o
  Tup o _ _ _ -> o
  Braces o _ _ _ -> o
  Sum kw _ _ _ _ -> kw
  Field e _ -> firstE e
  App h _ -> firstE h
  Op a _ _ -> firstE a
  Ops a _ -> firstE a
  Ann e _ _ -> firstE e
  Lam t _ _ _ -> t
  Case t _ _ _ -> t
  If t _ _ _ _ _ -> t
  Do t _ -> t

lastE :: E -> T
lastE = case _ of
  Atom t -> t
  Paren _ _ c -> c
  Named _ _ _ _ c -> c
  Tup _ _ _ c -> c
  Braces _ _ _ c -> c
  Sum _ _ _ _ c -> c
  Field _ t -> t
  App h args -> maybe (lastE h) lastE (A.last args)
  Op _ _ b -> lastE b
  Ops a rest -> maybe (lastE a) (\r -> lastE r.e) (A.last rest)
  Ann _ _ t -> lastE t
  Lam _ _ _ b -> lastE b
  Case _ _ o alts -> maybe o (\a -> lastE a.body) (A.last alts)
  If _ _ _ _ _ b -> lastE b
  Do t stmts -> maybe t lastStmt (A.last stmts)

lastStmt :: Stmt -> T
lastStmt = case _ of
  SBind _ _ e -> lastE e
  SExpr e -> lastE e

lastCon :: Con -> T
lastCon c = maybe c.con lastE (A.last c.args)

lastD :: D -> T
lastD = case _ of
  DImport t -> t
  DFixity kw ts -> fromMaybe kw (A.last ts)
  DSig _ _ ty -> lastE ty
  DEqn _ _ _ e -> lastE e
  DData _ name params universe alts -> case alts, universe of
    Just a, _ | Just c <- A.last a.alts -> lastCon c
    _, Just (Tuple _ u) -> lastE u
    _, _ -> maybe name lastE (A.last params)
  DType _ _ _ _ e -> lastE e
  DTheory _ _ _ w fields -> maybe w (\f -> lastE f.ty) (A.last fields)
  DForeign _ _ _ _ ty -> lastE ty
  DTest _ x -> x
  DWatch _ e -> lastE e

nmFirst :: Nm -> T
nmFirst n = fromMaybe eofTok (A.head n)

firstP :: Pat -> T
firstP = case _ of
  PCon t _ -> t
  PAtom t -> t
  PParen o _ _ -> o

firstStmt :: Stmt -> T
firstStmt = case _ of
  SBind x _ _ -> x
  SExpr e -> firstE e

firstD :: D -> T
firstD = case _ of
  DImport t -> t
  DFixity t _ -> t
  DSig n _ _ -> nmFirst n
  DEqn n _ _ _ -> nmFirst n
  DData t _ _ _ _ -> t
  DType t _ _ _ _ -> t
  DTheory t _ _ _ _ -> t
  DForeign t _ _ _ _ -> t
  DTest t _ -> t
  DWatch t _ -> t

-- Reading (as `Heff.Parse.Surface` reads, keeping the tokens)
--------------------------------------------------------------------------------

type St = { toks :: Array T, i :: Int, stack :: Array Int, start :: Int }

type P a = ExceptT String (State St) a

program :: P (Array D)
program = go []
  where
  go acc = do
    t <- peek
    if t.kind == "eof" then pure acc
    else do
      modify_ \s -> s { stack = [ 0 ], start = s.i }
      d <- decl
      t' <- peek
      unless (t'.kind == "eof" || (t'.first && t'.col == 0)) $ failAt t'
      go (A.snoc acc d)

decl :: P D
decl = lazyP \_ -> do
  t <- peek
  case t.kind, t.text of
    "import", _ -> DImport <$> next
    "kw", "data" -> dataDecl
    "kw", "type" -> do
      kw <- next
      name <- expectKind "con"
      params <- many atom
      eq <- expectSym "="
      DType kw name params eq <$> expr
    "kw", "theory" -> do
      kw <- next
      name <- expectKind "con"
      params <- many atom
      w <- expectKw "where"
      DTheory kw name params w <$> block do
        x <- next
        cc <- expectSym "::"
        ty <- expr
        pure { name: x, colons: cc, ty }
    "kw", "foreign" -> do
      kw <- next
      imp <- expectKw "import"
      x <- expectKind "var"
      cc <- expectSym "::"
      DForeign kw imp x cc <$> expr
    "kw", "test" -> do
      kw <- next
      DTest kw <$> expectKind "var"
    "watch", _ -> do
      w <- next
      DWatch w <$> expr
    "kw", _ | A.elem t.text [ "infixl", "infixr", "infix" ] -> do
      kw <- next
      let
        rest acc = do
          u <- peek
          if u.kind == "eof" || (u.first && u.col == 0) then pure acc else next >>= \v -> rest (A.snoc acc v)
      DFixity kw <$> rest []
    _, _ -> do
      t2 <- peekAt 1
      t3 <- peekAt 2
      len <-
        if t.kind == "var" || t.kind == "con" then pure 1
        else if t.text == "(" && isOperator t2 && t3.text == ")" then pure 3
        else failAt t
      name <- A.replicate len unit # traverseNext
      u <- peek
      if u.text == "::" then do
        cc <- next
        DSig name cc <$> expr
      else do
        pats <- many patAtom
        eq <- expectSym "="
        DEqn name pats eq <$> expr
  where
  traverseNext = A.foldM (\acc _ -> next <#> A.snoc acc) []

dataDecl :: P D
dataDecl = do
  kw <- next
  name <- expectKind "con"
  params <- many atom
  t0 <- peek
  universe <-
    if t0.text == "::" && t0.kind == "sym" then do
      cc <- next
      u <- app
      pure (Just (Tuple cc u))
    else pure Nothing
  t <- peek
  alts <-
    if t.text == "=" then do
      eq <- next
      r <- sepBy1 "|" con
      pure (Just { eq, alts: r.items, bars: r.seps })
    else pure Nothing
  pure (DData kw name params universe alts)

con :: P Con
con = do
  c <- next
  args <- many atom
  pure { con: c, args }

expr :: P E
expr = lazyP \_ -> do
  t <- peek
  e <- case t.kind, t.text of
    "sym", "\\" -> lambda
    "kw", "case" -> caseExpr
    "kw", "if" -> ifExpr
    "kw", "do" -> doExpr
    _, _ -> opExpr 0
  t' <- peek
  b <- atBoundary
  if t'.text == "::" && t'.kind == "sym" && not b then do
    cc <- next
    Ann e cc <$> expr
  else pure e

operators :: Array { sym :: String, prec :: Int, right :: Boolean }
operators =
  [ { sym: "->", prec: 1, right: true }
  , { sym: "~", prec: 2, right: true }
  , { sym: "*", prec: 3, right: true }
  ]

opExpr :: Int -> P E
opExpr minPrec = do
  lhs <- userOps
  loop lhs
  where
  loop lhs = do
    t <- peek
    b <- atBoundary
    case A.find (\o -> o.sym == t.text) operators of
      Just o | t.kind == "sym" && o.prec >= minPrec && not b -> do
        op <- next
        rhs <- if o.prec == 1 then arrowRhs else opExpr (if o.right then o.prec else o.prec + 1)
        loop (Op lhs op rhs)
      _ -> pure lhs
  arrowRhs = do
    t <- peek
    if t.text == "\\" || (t.kind == "kw" && A.elem t.text [ "case", "if", "do" ]) then expr else opExpr 1

-- | declared operators, as they are written (how they bind is the
-- | parser's business)
userOps :: P E
userOps = lazyP \_ -> do
  e0 <- app
  rest <- operands
  pure (if A.null rest then e0 else Ops e0 rest)
  where
  operands = do
    t <- peek
    b <- atBoundary
    if isOperator t && not b then do
      op <- next
      u <- peek
      if u.text == "\\" || (u.kind == "kw" && A.elem u.text [ "case", "if", "do" ]) then do
        e <- expr
        pure [ { op, e } ]
      else do
        e <- app
        more <- operands
        pure (A.cons { op, e } more)
    else pure []

isOperator :: T -> Boolean
isOperator t = t.kind == "sym" && not (A.elem t.text reserved)

app :: P E
app = lazyP \_ -> do
  h <- atom
  args <- many atom
  pure (if A.null args then h else App h args)

atom :: P E
atom = lazyP \_ -> do
  t <- peek
  b <- atBoundary
  when (b || not (startsAtom t)) $ failAt t
  base <- case t.kind of
    "kw" | t.text == "sum" -> do
      kw <- next
      o <- expectPunct "{"
      inBrackets do
        r <- sepBy1 "|" con
        c <- expectPunct "}"
        pure (Sum kw o r.items r.seps c)
    "punct" | t.text == "(" -> parens
    "punct" | t.text == "{" -> braces
    _ -> Atom <$> next
  fields base
  where
  fields e = do
    t <- peek
    if t.kind == "field" then next >>= \f -> fields (Field e f) else pure e

startsAtom :: T -> Boolean
startsAtom t = A.elem t.kind [ "var", "con", "tag", "int", "float", "str" ]
  || (t.kind == "kw" && A.elem t.text [ "Type", "Int", "Float", "String", "Array", "sum", "abandon" ])
  || (t.kind == "punct" && (t.text == "(" || t.text == "{"))

parens :: P E
parens = lazyP \_ -> do
  o <- next
  inBrackets do
    t <- peek
    t2 <- peekAt 1
    if isOperator t && t2.text == ")" then do
      op <- next
      Paren o (Atom op) <$> next
    else if (t.kind == "var" || t.kind == "con") && t2.text == ":=" then do
      x <- next
      k <- next
      e <- expr
      c <- expectPunct ")"
      pure (Named o x k e c)
    else do
      e <- expr
      t' <- peek
      if t'.text == "," then do
        rest <- commaSep
        c <- expectPunct ")"
        pure (Tup o (A.cons e (map _.e rest)) (map _.comma rest) c)
      else Paren o e <$> expectPunct ")"
  where
  commaSep = do
    t <- peek
    if t.text == "," then do
      comma <- next
      e <- expr
      rest <- commaSep
      pure (A.cons { comma, e } rest)
    else pure []

braces :: P E
braces = lazyP \_ -> do
  o <- next
  inBrackets do
    t <- peek
    t2 <- peekAt 1
    if t.text == "}" then do
      c <- next
      pure (Braces o [] [] c)
    else do
      r <-
        if t2.text == "=" then sepBy1 "," do
          x <- next
          eq <- expectSym "="
          FSet x eq <$> expr
        else sepBy1 "," (FDecl <$> expr)
      c <- expectPunct "}"
      pure (Braces o r.items r.seps c)

lambda :: P E
lambda = lazyP \_ -> do
  bs <- next
  binders <- many1 binder
  arrow <- expectSym "->"
  Lam bs binders arrow <$> expr
  where
  binder = do
    t <- peek
    case t.kind, t.text of
      -- (a binder is a name, capitalised or not)
      "var", _ -> BVar <$> next
      "con", _ -> BVar <$> next
      "punct", "{" -> do
        o <- next
        x <- peek
        when (x.kind /= "var" && x.kind /= "con") (failAt x)
        _ <- next
        c <- expectPunct "}"
        pure (BImp o x c)
      _, _ -> failAt t

caseExpr :: P E
caseExpr = lazyP \_ -> do
  kw <- next
  scrut <- expr
  of_ <- expectKw "of"
  Case kw scrut of_ <$> block do
    p <- pattern
    arrow <- expectSym "->"
    body <- expr
    pure { pat: p, arrow, body }

ifExpr :: P E
ifExpr = lazyP \_ -> do
  kw <- next
  c <- expr
  th <- expectKw "then"
  a <- expr
  el <- expectKw "else"
  If kw c th a el <$> expr

doExpr :: P E
doExpr = lazyP \_ -> do
  kw <- next
  Do kw <$> block do
    t <- peek
    t2 <- peekAt 1
    if t.kind == "var" && t2.text == "<-" then do
      x <- next
      arrow <- next
      SBind x arrow <$> expr
    else SExpr <$> expr

pattern :: P Pat
pattern = lazyP \_ -> do
  t <- peek
  if t.kind == "con" || t.kind == "tag" then do
    c <- next
    PCon c <$> many patAtom
  else patAtom

patAtom :: P Pat
patAtom = lazyP \_ -> do
  t <- peek
  b <- atBoundary
  when b $ failAt t
  case t.kind, t.text of
    "punct", "(" -> do
      o <- next
      inBrackets do
        p <- pattern
        PParen o p <$> expectPunct ")"
    _, _ | A.elem t.kind [ "var", "con", "tag" ] -> PAtom <$> next
    _, _ -> failAt t

-- (layout, as `Heff.Parse.Surface`'s)

lazyP :: forall a. (Unit -> P a) -> P a
lazyP f = pure unit >>= f

eofTok :: T
eofTok = { kind: "eof", text: "", raw: "", start: 0, end: 0, col: 0, first: true, ix: -1, blank: false }

peek :: P T
peek = gets \s -> fromMaybe eofTok (A.index s.toks s.i)

peekAt :: Int -> P T
peekAt k = gets \s -> fromMaybe eofTok (A.index s.toks (s.i + k))

next :: P T
next = do
  t <- peek
  modify_ \s -> s { i = s.i + 1 }
  pure t

failAt :: forall a. T -> P a
failAt t = throwError ("the file does not parse (at `" <> t.raw <> "`, offset " <> show t.start <> ")")

atBoundary :: P Boolean
atBoundary = gets \s ->
  let t = fromMaybe eofTok (A.index s.toks s.i)
  in t.kind == "eof" || (t.first && t.col <= fromMaybe 0 (A.last s.stack) && s.i /= s.start)

block :: forall a. P a -> P (Array a)
block one = do
  t <- peek
  outer <- gets \s -> fromMaybe 0 (A.last s.stack)
  when (t.kind == "eof" || (t.first && t.col <= outer)) $ failAt t
  modify_ \s -> s { stack = A.snoc s.stack t.col }
  let
    go acc = do
      modify_ \s -> s { start = s.i }
      x <- one
      t' <- peek
      if t'.kind /= "eof" && t'.first && t'.col == t.col then go (A.snoc acc x) else pure (A.snoc acc x)
  xs <- go []
  modify_ \s -> s { stack = fromMaybe s.stack (A.init s.stack) }
  pure xs

inBrackets :: forall a. P a -> P a
inBrackets p = do
  modify_ \s -> s { stack = A.snoc s.stack (-1) }
  x <- p
  modify_ \s -> s { stack = fromMaybe s.stack (A.init s.stack) }
  pure x

many :: forall a. P a -> P (Array a)
many p = go []
  where
  go acc = do
    b <- atBoundary
    t <- peek
    if b || not (startsAtom t || t.kind == "var" || (t.kind == "punct" && t.text == "(")) then pure acc
    else p >>= \x -> go (A.snoc acc x)

many1 :: forall a. P a -> P (Array a)
many1 p = do
  x <- p
  xs <- many p
  pure (A.cons x xs)

sepBy1 :: forall a. String -> P a -> P { items :: Array a, seps :: Array T }
sepBy1 sep p = do
  x <- p
  let
    go acc = do
      t <- peek
      if t.text == sep then do
        s <- next
        y <- p
        go { items: A.snoc acc.items y, seps: A.snoc acc.seps s }
      else pure acc
  go { items: [ x ], seps: [] }

expectSym :: String -> P T
expectSym x = do
  t <- peek
  if t.kind == "sym" && t.text == x then next else failAt t

expectPunct :: String -> P T
expectPunct x = do
  t <- peek
  if t.kind == "punct" && t.text == x then next else failAt t

expectKw :: String -> P T
expectKw x = do
  t <- peek
  if t.kind == "kw" && t.text == x then next else failAt t

expectKind :: String -> P T
expectKind k = do
  t <- peek
  if t.kind == k then next else failAt t

-- Laying out
--------------------------------------------------------------------------------

-- (a line to keep blank, and a line break inside a comment: what is left
-- blank otherwise, from a line break forced by a comment, is dropped)
blankMark :: String
blankMark = "\x01"

newlineMark :: String
newlineMark = "\x02"

render :: Doc -> String
render d =
  let
    ls = map rtrim (S.split (S.Pattern "\n") (P.renderWidth 80 d))
    kept = A.mapMaybe (\l -> if S.trim l == blankMark then Just "" else if S.trim l == "" then Nothing else Just l) ls
  in
    S.replaceAll (S.Pattern newlineMark) (S.Replacement "\n") (S.joinWith "\n" kept) <> "\n"

rtrim :: String -> String
rtrim s = SCU.fromCharArray (A.reverse (A.dropWhile (\c -> c == ' ' || c == '\t' || c == '\r') (A.reverse (SCU.toCharArray s))))

comment :: String -> Doc
comment s = P.text (S.replaceAll (S.Pattern "\n") (S.Replacement newlineMark) s)

blankLine :: Doc
blankLine = P.text blankMark <> P.hardline

-- | a token, with its comments: those before it on lines of their own, and
-- | the one after it on its line (after which the line ends)
tok :: Env -> T -> Doc
tok env t = leading <> foldMap (\c -> comment c <> P.text " ") (fromMaybe [] (Map.lookup t.ix env.inline)) <> P.text t.raw <> trailing
  where
  cs = if t.ix == env.noLead then [] else fromMaybe [] (Map.lookup t.ix env.lead)
  leading = if A.null cs then mempty else foldMap (\c -> P.hardline <> comment c.text) cs <> P.hardline
  trailing = if t.ix == env.noTrail then mempty else trailOf env t

-- | an item (a declaration, or an item of a block) from the token `first`
-- | to `last`, at the current column: its leading comments there, the rest
-- | of it indented further, so that no line of it could start another, and
-- | the comment after it (outside what it groups, which it would break)
item :: Env -> T -> T -> (Env -> Doc) -> Doc
item env first last body =
  foldMapWithIndex (\i c -> (if i > 0 && c.blank then blankLine else mempty) <> comment c.text <> P.hardline) cs
    <> (if not (A.null cs) && first.blank then blankLine else mempty)
    <> P.nest 2 (body env { noLead = first.ix, noTrail = last.ix } <> trailOf env last)
  where
  cs = leadOf env first

leadOf :: Env -> T -> Array Cmt
leadOf env t = fromMaybe [] (Map.lookup t.ix env.lead)

-- | a blank line was before the item (before its comments, if any)
blankBefore :: Env -> T -> Boolean
blankBefore env t = maybe t.blank _.blank (A.head (leadOf env t))

-- | items, each on a line of its own, blank lines between kept
items :: forall a. Env -> (a -> T) -> (a -> T) -> (Env -> a -> Doc) -> Array a -> Doc
items env first last doc xs = fold (A.mapWithIndex one xs)
  where
  one i x = P.hardline <> (if i > 0 && blankBefore env (first x) then blankLine else mempty) <> item env (first x) (last x) (\e -> doc e x)

topLevel :: Env -> Array D -> T -> Doc
topLevel env ds eof =
  fold (A.mapWithIndex one ds) <> foldMap (\c -> P.hardline <> (if c.blank then blankLine else mempty) <> comment c.text) (leadOf env eof)
  where
  one i d =
    (if i > 0 then P.hardline <> (if blankBefore env (firstD d) then blankLine else mempty) else mempty)
      <> item env (firstD d) (lastD d) (\e -> pD e d)

pD :: Env -> D -> Doc
pD env = case _ of
  DImport t -> tok env t
  DFixity kw ts -> tok env kw <> foldMap (\t -> P.text " " <> tok env t) ts
  DSig f cc ty -> nm f <+> sig env cc ty
  DEqn f pats eq rhs -> nm f <> foldMap (\p -> P.text " " <> pP env p) pats <+> tok env eq <> rhsOf env rhs
  DData kw name params universe alts ->
    tok env kw <+> tok env name <> params' params
      <> maybe mempty (\(Tuple cc u) -> P.text " " <> tok env cc <+> pE env u) universe
      <> maybe mempty (\a -> P.group (P.line <> tok env a.eq <+> pCon env (headOr a.alts) <> fold (A.zipWith (\bar c -> P.line <> tok env bar <+> pCon env c) a.bars (A.drop 1 a.alts)))) alts
  DType kw name params eq body -> tok env kw <+> tok env name <> params' params <+> tok env eq <> rhsOf env body
  DTheory kw name params w fields ->
    tok env kw <+> tok env name <> params' params <+> tok env w
      <> items env _.name (\f -> lastE f.ty) (\e f -> tok e f.name <+> sig e f.colons f.ty) fields
  DForeign kw imp x cc ty -> tok env kw <+> tok env imp <+> tok env x <+> sig env cc ty
  DTest kw x -> tok env kw <+> tok env x
  -- (as written: its expectation is the lines under it)
  DWatch w e -> tok env w <> comment (SCU.slice w.end (lastE e).end env.src)
  where
  -- (a declaration's parameters are not broken: on one line, however long)
  params' = foldMap (\p -> P.text " " <> P.flatten (pE env p))
  nm = foldMap (tok env)
  headOr xs = fromMaybe { con: eofTok, args: [] } (A.head xs)

-- | `:: A -> B`, the arrows under `::` if it does not fit
sig :: Env -> T -> E -> Doc
sig env cc ty = P.align (tok env cc <+> pE env ty)

trailOf :: Env -> T -> Doc
trailOf env t = maybe mempty (\c -> P.text " " <> comment c <> P.hardline) (Map.lookup t.ix env.trail)

-- | what is right of `=` (or `->`, `<-`): a lambda, `case` or `do` right
-- | there, anything else on the next line if it does not fit
rhsOf :: Env -> E -> Doc
rhsOf env e
  | opensBlock e = P.text " " <> pE env e
  | otherwise = P.group (P.line <> pE env e)

opensBlock :: E -> Boolean
opensBlock = case _ of
  Lam _ _ _ _ -> true
  Case _ _ _ _ -> true
  Do _ _ -> true
  _ -> false

-- | a `do` or `case`, or a lambda to one: its items are lines of their own
hangs :: E -> Boolean
hangs = case _ of
  Lam _ _ _ b -> hangs b
  Case _ _ _ _ -> true
  Do _ _ -> true
  _ -> false

pCon :: Env -> Con -> Doc
pCon env c = tok env c.con <> foldMap (\a -> P.text " " <> pE env a) c.args

pE :: Env -> E -> Doc
pE env = case _ of
  Atom t -> tok env t
  Paren o e c -> tok env o <> P.align (pE env e) <> tok env c
  Named o x k e c -> tok env o <> tok env x <+> tok env k <+> pE env e <> tok env c
  Tup o es commas c ->
    P.group (tok env o <> P.align (pE env (headE es) <> fold (A.zipWith (\comma e -> tok env comma <> P.line <> pE env e) commas (A.drop 1 es))) <> tok env c)
  Braces o [] _ c -> tok env o <> tok env c
  Braces o fs commas c ->
    P.group (P.align (tok env o <+> pF (headF fs) <> fold (A.zipWith (\comma f -> P.line' <> tok env comma <+> pF f) commas (A.drop 1 fs)) <> P.line <> tok env c))
  Sum kw o alts bars c ->
    tok env kw <+> P.group (P.align (tok env o <+> pCon env (headC alts) <> fold (A.zipWith (\bar a -> P.line <> tok env bar <+> pCon env a) bars (A.drop 1 alts)) <+> tok env c))
  Field e f -> pE env e <> tok env f
  -- (a block last, `f x (\y -> do`, stays on the line, and its items are
  -- indented from the line, not from the parenthesis)
  App h args | Just { init, last: Paren o e c } <- A.unsnoc args, hangs e ->
    P.group (pE env h <> P.nest 2 (foldMap (\a -> P.line <> pE env a) init)) <+> tok env o <> pE env e <> tok env c
  App h args | Just { init, last } <- A.unsnoc args, hangs last ->
    P.group (pE env h <> P.nest 2 (foldMap (\a -> P.line <> pE env a) init)) <+> pE env last
  App h args -> P.group (pE env h <> P.nest 2 (foldMap (\a -> P.line <> pE env a) args))
  op@(Op _ _ _) -> case chain op of
    { first, rest } ->
      let
        -- `{x :: A} -> B`: an implicit binder, written close
        operand e = case e of
          Braces o [ FDecl d ] [] c | A.any (\r -> r.op.text == "->") rest -> tok env o <> pE env d <> tok env c
          _ -> pE env e
      in
        P.group (operand first <> foldMap (\r -> P.line <> tok env r.op <+> operand r.e) rest)
  Ops first rest -> P.group (pE env first <> foldMap (\r -> P.line <> tok env r.op <+> pE env r.e) rest)
  Ann e cc ty -> P.group (pE env e <+> tok env cc <> P.nest 2 (P.line <> pE env ty))
  Lam bs binders arrow body ->
    tok env bs <> P.concatWith (<+>) (map pB binders) <+> tok env arrow
      <> (if opensBlock body then P.text " " <> pE env body else P.group (P.nest 2 (P.line <> pE env body)))
  Case kw scrut of_ alts ->
    tok env kw <+> pE env scrut <+> tok env of_
      <> items env (\a -> firstP a.pat) (\a -> lastE a.body) (\e a -> pP e a.pat <+> tok e a.arrow <> rhsOf e a.body) alts
  If kw c th a el b ->
    P.group (tok env kw <+> pE env c <> P.nest 2 (P.line <> tok env th <+> P.nest 2 (pE env a) <> P.line <> tok env el <+> P.nest 2 (pE env b)))
  Do kw stmts -> tok env kw <> items env firstStmt lastStmt pS stmts
  where
  headE es = fromMaybe (Atom eofTok) (A.head es)
  headF fs = fromMaybe (FDecl (Atom eofTok)) (A.head fs)
  headC cs = fromMaybe { con: eofTok, args: [] } (A.head cs)
  pF = case _ of
    -- (a block in a field is indented past the commas)
    FSet x eq e -> P.nest 4 (tok env x <+> tok env eq <> rhsOf env e)
    FDecl e -> P.nest 4 (pE env e)
  pB = case _ of
    BVar t -> tok env t
    BImp o x c -> tok env o <> tok env x <> tok env c
  pS e = case _ of
    SBind x arrow rhs -> tok e x <+> tok e arrow <> rhsOf e rhs
    SExpr rhs -> pE e rhs

-- | `a -> b -> c` (or `~`, `*`) as its operands
chain :: E -> { first :: E, rest :: Array { op :: T, e :: E } }
chain = case _ of
  Op a op b -> case chain b of
    r | sameOp op r -> { first: a, rest: A.cons { op, e: r.first } r.rest }
    _ -> { first: a, rest: [ { op, e: b } ] }
  e -> { first: e, rest: [] }
  where
  sameOp op r = case A.head r.rest of
    Just x -> x.op.text == op.text
    Nothing -> true

pP :: Env -> Pat -> Doc
pP env = case _ of
  PCon c args -> tok env c <> foldMap (\p -> P.text " " <> pP env p) args
  PAtom t -> tok env t
  PParen o p c -> tok env o <> pP env p <> tok env c
