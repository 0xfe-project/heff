-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at https://mozilla.org/MPL/2.0/.
--
-- A port of the `fnotation` Haskell library (MPL-2.0).
-- | A port of the `fnotation` library (lexer, reader, notation trees and their
-- | printer), which is what the reference Hippogriff implementation parses
-- | with.  Names are kept as strings: `a/b/c` is just the string, and the
-- | parts that care about the last segment split it.
module Hg.Parse.FNotation
  ( Kind(..)
  , TokenValue(..)
  , Token
  , Ntn(..)
  , Span
  , Assoc(..)
  , Prec
  , ConfTable
  , confTable
  , confLookup
  , Diag
  , Severity(..)
  , lex
  , parse
  , ntnSpan
  , ntnHead
  , group
  , getGroup
  , prettyNtn
  , lastSeg
  ) where

import Prelude
import Hg.Support.BigInt as BI
import Hg.Support.BigInt (BigInt)

import Control.Monad.ST (ST)
import Control.Monad.Rec.Class (Step(..), tailRecM)
import Control.Monad.ST as ST
import Control.Monad.ST.Ref (STRef)
import Control.Monad.ST.Ref as STRef
import Data.Array as A
import Data.Array.NonEmpty (NonEmptyArray)
import Data.Array.NonEmpty as NEA
import Data.Array.ST as STA
import Data.Char (toCharCode)
import Data.Foldable (foldl)
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple as T
import Hg.Support.Pretty (Doc, (<+>))
import Hg.Support.Pretty as P

type Span = { start :: Int, end :: Int }

data Severity = SError | SWarning | SDebug

derive instance eqSeverity :: Eq Severity

-- | a diagnostic: `code` is already offset into its family (lexer 0, reader
-- | 100, parser 200, elaborator 300)
type Diag = { code :: Int, severity :: Severity, msg :: Doc, span :: Span, note :: Maybe Doc }

-- ===========================================================================
-- kinds, tokens, configuration
-- ===========================================================================

data Kind
  = AIdent
  | AKeyword
  | SIdent
  | SKeyword
  | Decl
  | Modifier
  | Block
  | End
  | Tag
  | Field
  | FieldImmediate -- ^ a field written right against what precedes it: `a.b`
  | Int
  | String
  | LParen
  | RParen
  | LBrack
  | RBrack
  | LCurly
  | RCurly
  | Comma
  | Semicolon
  | Nl
  | Eof
  | Error

derive instance eqKind :: Eq Kind

instance showKind :: Show Kind where
  show = case _ of
    AIdent -> "AIdent"
    AKeyword -> "AKeyword"
    SIdent -> "SIdent"
    SKeyword -> "SKeyword"
    Decl -> "Decl"
    Modifier -> "Modifier"
    Block -> "Block"
    End -> "End"
    Tag -> "Tag"
    Field -> "Field"
    FieldImmediate -> "FieldImmediate"
    Int -> "Int"
    String -> "String"
    LParen -> "LParen"
    RParen -> "RParen"
    LBrack -> "LBrack"
    RBrack -> "RBrack"
    LCurly -> "LCurly"
    RCurly -> "RCurly"
    Comma -> "Comma"
    Semicolon -> "Semicolon"
    Nl -> "Nl"
    Eof -> "Eof"
    Error -> "Error"

data TokenValue = VEmpty | VName String | VInt BigInt | VString String

type Token = { kind :: Kind, value :: TokenValue, span :: Span }

type ConfTable a = Map String a

confTable :: forall a. Array (T.Tuple String a) -> ConfTable a
confTable = Map.fromFoldable

confLookup :: forall a. ConfTable a -> String -> Maybe a
confLookup t x = Map.lookup x t

data Assoc = AssocL | AssocR | AssocNon

derive instance eqAssoc :: Eq Assoc

type Prec = { binding :: Int, assoc :: Assoc }

precLe :: Prec -> Prec -> Maybe Boolean
precLe p p'
  | p.binding < p'.binding = Just true
  | p.binding > p'.binding = Just false
  | otherwise = case p.assoc, p'.assoc of
      AssocL, AssocL -> Just false
      AssocR, AssocR -> Just true
      _, _ -> Nothing

lastSeg :: String -> String
lastSeg x = fromMaybe x (A.last (S.split (S.Pattern "/") x))

-- ===========================================================================
-- notation
-- ===========================================================================

data Ntn
  = Juxt Ntn Ntn
  | Infix Ntn Ntn Ntn
  | NBlock String (Maybe Ntn) (Array Ntn) Span
  | MDecl (Array String) String Ntn Span
  | NTuple (Array Ntn) Span
  | NBrace Ntn Span -- ^ extension: `{ e }`, for implicit arguments
  | Ident String Span
  | Keyword String Span
  | NField String Span
  | NTag String Span
  | NInt BigInt Span
  | NString String Span
  | NError Span

group :: NonEmptyArray Ntn -> Ntn
group xs = foldl Juxt (NEA.head xs) (NEA.tail xs)

-- | the `Group` view: a juxtaposition read as head and arguments
getGroup :: Ntn -> NonEmptyArray Ntn
getGroup = go []
  where
  go acc (Juxt f x) = go (A.cons x acc) f
  go acc f = NEA.cons' f acc

ntnSpan :: Ntn -> Span
ntnSpan n = { start: startPos n, end: endPos n }
  where
  startPos = case _ of
    Juxt f _ -> startPos f
    Infix x _ _ -> startPos x
    NBlock _ _ _ s -> s.start
    MDecl _ _ _ s -> s.start
    NTuple _ s -> s.start
    NBrace _ s -> s.start
    Ident _ s -> s.start
    Keyword _ s -> s.start
    NField _ s -> s.start
    NTag _ s -> s.start
    NInt _ s -> s.start
    NString _ s -> s.start
    NError s -> s.start
  endPos = case _ of
    Juxt _ x -> endPos x
    Infix _ _ y -> endPos y
    NBlock _ _ _ s -> s.end
    MDecl _ _ _ s -> s.end
    NTuple _ s -> s.end
    NBrace _ s -> s.end
    Ident _ s -> s.end
    Keyword _ s -> s.end
    NField _ s -> s.end
    NTag _ s -> s.end
    NInt _ s -> s.end
    NString _ s -> s.end
    NError s -> s.end

ntnHead :: Ntn -> Doc
ntnHead = case _ of
  Juxt _ _ -> P.text "Juxt"
  Infix _ _ _ -> P.text "Infix"
  NBlock x _ _ _ -> P.text "Block" <+> P.text x
  MDecl mods x _ _ -> P.text "MDecl" <+> P.hsep (map P.text (A.snoc mods x))
  Ident x _ -> P.text "Ident" <+> P.text x
  Keyword x _ -> P.text "Keyword" <+> P.text x
  NField x _ -> P.text "Field" <+> P.text x
  NTag x _ -> P.text "Tag" <+> P.text x
  NInt i _ -> P.text "Int" <+> P.text (show i)
  NString i _ -> P.text "Int" <+> P.text i
  NTuple _ _ -> P.text "Tuple"
  NBrace _ _ -> P.text "Brace"
  NError _ -> P.text "Error"

-- ===========================================================================
-- lexer
-- ===========================================================================

isLetter :: Char -> Boolean
isLetter c = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || toCharCode c > 127

isDigit :: Char -> Boolean
isDigit c = c >= '0' && c <= '9'

isAlphaNum :: Char -> Boolean
isAlphaNum c = isLetter c || isDigit c || c == '_' || c == '-'

isAlphaNumStart :: Char -> Boolean
isAlphaNumStart c = isLetter c || c == '_'

isSymbol :: Char -> Boolean
isSymbol c = A.elem c [ '<', '>', '-', '+', '/', '*', '~', ':', '=' ]

nameSegStart :: Char -> Boolean
nameSegStart c = isAlphaNumStart c || isSymbol c || c == '`'

isPrint :: Char -> Boolean
isPrint c = toCharCode c >= 32

lex :: ConfTable Kind -> String -> { tokens :: Array Token, diags :: Array Diag }
lex config src = ST.run do
  out <- STA.new
  diags <- STA.new
  pend <- STRef.new Nothing
  let
    cs = SCU.toCharArray src
    n = A.length cs
    at i = if i < n then fromMaybe '\x0' (A.index cs i) else '\x0'
    slice i j = SCU.fromCharArray (A.slice i j cs)
    -- A token's span starts where the previous token (or skipped whitespace)
    -- ended, as fnotation's `prev` pointer does: characters that were
    -- consumed without emitting anything -- a `.` with no name after it, an
    -- unexpected character -- end up at the front of the next token.
    emit k v s e = do
      p <- STRef.read pend
      _ <- STRef.write Nothing pend
      void (STA.push { kind: k, value: v, span: { start: fromMaybe s p, end: e } } out)
    holdFrom i = do
      p <- STRef.read pend
      when (p == Nothing) (void (STRef.write (Just i) pend))
    skipTo i = STRef.write Nothing pend *> pure i
    reportL code msg s e =
      void (STA.push { code, severity: SError, msg: P.text msg, span: { start: s, end: e }, note: Nothing } diags)
    while p i = if i < n && p (at i) then while p (i + 1) else i

    classifySeg x = case confLookup config x of
      Just k -> k
      Nothing -> if isAlphaNumStart (fromMaybe ' ' (SCU.charAt 0 x)) then AIdent else SIdent

    -- one segment: returns kind, text, end
    nameSeg i
      | isAlphaNumStart (at i) = let j = while isAlphaNum i in { k: classifySeg (slice i j), t: slice i j, j }
      | isSymbol (at i) = let j = while isSymbol i in { k: classifySeg (slice i j), t: slice i j, j }
      | otherwise =
          -- backtick-quoted
          let
            j = while (\c -> c /= '`' && isPrint c) (i + 1)
            j' = if at j == '`' then j + 1 else j
          in
            { k: AIdent, t: slice (i + 1) j, j: j' }

    -- a whole name `a/b/c`; `wanted` is Field or Tag when after `.` or `'`
    name s i wanted = go (nameSeg i) []
      where
      go seg stack =
        if at seg.j == '/' && nameSegStart (at (seg.j + 1)) then do
          when (seg.k /= AIdent) (reportL 3 "used keyword as namespace component" s seg.j)
          go (nameSeg (seg.j + 1)) (A.snoc stack seg.t)
        else do
          let full = S.joinWith "/" (A.snoc stack seg.t)
          case wanted of
            Nothing -> case seg.k of
              AIdent -> emit AIdent (VName full) s seg.j
              SIdent -> emit SIdent (VName full) s seg.j
              k ->
                if A.null stack then emit k (VName full) s seg.j
                else do
                  reportL 4 "referred to keyword as a qualified name" s seg.j
                  emit (if isSymbol (fromMaybe ' ' (SCU.charAt 0 seg.t)) then SIdent else AIdent) (VName full) s seg.j
            Just k' -> do
              when (seg.k /= AIdent) (reportL 5 ("used reserved word as a " <> show k') s seg.j)
              emit k' (VName full) s seg.j
          pure seg.j

    -- the end of the last token emitted, to tell `a.b` from `a .b`
    lastEnd = do
      len <- STA.length out
      if len == 0 then pure (-1) else do
        t <- STA.peek (len - 1) out
        pure (case t of
          Just tok -> tok.span.end
          Nothing -> -1)
    -- one token (or skipped character); returns where to continue, -1 at the end
    step i = do
      let c = at i
      if i >= n then emit Eof VEmpty n n *> pure (-1)
      else if c == '\t' || c == ' ' || c == '\r' then skipTo (i + 1)
      else if c == '(' then emit LParen VEmpty i (i + 1) *> pure (i + 1)
      else if c == ')' then emit RParen VEmpty i (i + 1) *> pure (i + 1)
      else if c == '[' then emit LBrack VEmpty i (i + 1) *> pure (i + 1)
      else if c == ']' then emit RBrack VEmpty i (i + 1) *> pure (i + 1)
      else if c == '{' then emit LCurly VEmpty i (i + 1) *> pure (i + 1)
      else if c == '}' then emit RCurly VEmpty i (i + 1) *> pure (i + 1)
      else if c == ',' then emit Comma VEmpty i (i + 1) *> pure (i + 1)
      else if c == ';' then emit Semicolon VEmpty i (i + 1) *> pure (i + 1)
      else if c == '\n' then emit Nl VEmpty i (i + 1) *> pure (i + 1)
      -- a comment runs to the end of the line; the newline is kept as a token
      else if c == '#' then pure (while (_ /= '\n') i)
      else if c == '"' then do
        let j = while (_ /= '"') (i + 1)
        emit String (VString (slice (i + 1) j)) i (j + 1)
        pure (j + 1)
      else if c == '.' || c == '\'' then do
        prevEnd <- lastEnd
        let
          k =
            if c == '\'' then Tag
            else if prevEnd == i then FieldImmediate
            else Field
        if nameSegStart (at (i + 1)) then name i (i + 1) (Just k) 
        else do
          reportL 2 ("expected a name after " <> (if c == '.' then "period" else "single quote")) i (i + 1)
          holdFrom i
          pure (i + 1)
      else if c == '`' then name i i Nothing 
      else if isDigit c then do
        let j = while isDigit i
        emit Int (VInt (BI.fromDigits (SCU.fromCharArray (A.slice i j cs)))) i j
        pure j
      else if isLetter c || c == '_' || isSymbol c then name i i Nothing 
      else do
        reportL 0 ("Unexpected character '" <> SCU.singleton c <> "'") i (i + 1)
        holdFrom i
        pure (i + 1)
  tailRecM (\i -> if i < 0 then pure (Done unit) else Loop <$> step i) 0
  tokens <- STA.unsafeFreeze out
  ds <- STA.unsafeFreeze diags
  pure { tokens, diags: ds }

-- ===========================================================================
-- parser
-- ===========================================================================

type PS r =
  { pos :: STRef r Int
  , skipNewlines :: STRef r Boolean
  , tokens :: Array Token
  , diags :: STA.STArray r Diag
  , config :: ConfTable Prec
  }

tokAt :: forall r. PS r -> Int -> Token
tokAt st i = case A.index st.tokens i of
  Just t -> t
  Nothing -> { kind: Eof, value: VEmpty, span: { start: 0, end: 0 } }

curTok :: forall r. PS r -> ST r Token
curTok st = tokAt st <$> STRef.read st.pos

cur :: forall r. PS r -> ST r Kind
cur st = _.kind <$> curTok st

curSpan :: forall r. PS r -> ST r Span
curSpan st = _.span <$> curTok st

curName :: forall r. PS r -> ST r String
curName st = curTok st <#> \t -> case t.value of
  VName x -> x
  _ -> ""

report :: forall r. PS r -> Span -> Int -> Severity -> String -> ST r Unit
report st s code sev msg =
  void (STA.push { code: 100 + code, severity: sev, msg: P.text msg, span: s, note: Nothing } st.diags)

advance :: forall r. PS r -> ST r Unit
advance st = do
  pos <- STRef.read st.pos
  let n = A.length st.tokens
  when (pos < n - 1) do
    skip <- STRef.read st.skipNewlines
    let
      next j
        | j < n && (tokAt st j).kind == Nl = next (j + 1)
        | otherwise = j
    void (STRef.write (if skip then next (pos + 1) else pos + 1) st.pos)

locally :: forall r a b. STRef r a -> a -> ST r b -> ST r b
locally ref v action = do
  old <- STRef.read ref
  _ <- STRef.write v ref
  res <- action
  _ <- STRef.write old ref
  pure res

ignoreNewlines :: forall r a. PS r -> ST r a -> ST r a
ignoreNewlines st = locally st.skipNewlines true

reportUnexpected :: forall r. PS r -> Kind -> String -> ST r Unit
reportUnexpected st k c = do
  s <- curSpan st
  report st s 0 SError ("Unexpected token kind " <> show k <> ", expected " <> c)

expect :: forall r. PS r -> Kind -> ST r Unit
expect st k = do
  k' <- cur st
  if k == k' then advance st else reportUnexpected st k' (show k)

argStart :: Kind -> Boolean
-- (a string or an immediate field can begin an expression, but does not
-- continue a juxtaposition: `f "a"` is an error, `f ("a")` is not)
argStart k = A.elem k [ LParen, LBrack, LCurly, AIdent, AKeyword, Field, Tag, Int, Block ]

advanceClose :: forall r. PS r -> Int -> (Span -> Ntn) -> ST r Ntn
advanceClose st m f = do
  s <- curSpan st
  advance st
  pure (f { start: m, end: s.end })

tupleElems :: forall r. PS r -> ST r (Array Ntn)
tupleElems st = cur st >>= case _ of
  RBrack -> pure []
  k | argStart k -> do
    n <- expr st
    cur st >>= case _ of
      RBrack -> pure [ n ]
      Comma -> do
        advance st
        ns <- tupleElems st
        pure (A.cons n ns)
      k' -> do
        reportUnexpected st k' "a token that can follow an element of a tuple, e.g. ',' or ']'"
        pure [ n ]
  k -> do
    reportUnexpected st k "a token that can start an expression"
    pure []

-- | An argument, then any fields written right against it: `a.b.c` is one
-- | argument, while `f .b` (with a space) is `.b` applied as a separate
-- | element of the group.  The reference implementation's fnotation makes
-- | this distinction (its golden output prints `'leaf M.t` unparenthesised).
arg :: forall r. PS r -> ST r Ntn
arg st = do
  a <- arg0 st
  suffixes a
  where
  suffixes a = do
    t <- curTok st
    if t.kind == FieldImmediate then do
      f <- arg0 st
      suffixes (Juxt a f)
    else pure a

arg0 :: forall r. PS r -> ST r Ntn
arg0 st = do
  t <- curTok st
  let m = t.span.start
  case t.kind of
    LParen -> do
      e <- ignoreNewlines st do
        advance st
        expr st
      expect st RParen
      pure e
    LBrack -> do
      ns <- ignoreNewlines st do
        advance st
        tupleElems st
      expect st RBrack
      -- (the span runs to the end of the token *after* `]`, as fnotation's
      -- `close` reads the current token once `]` has been consumed)
      e <- curSpan st
      pure (NTuple ns { start: m, end: e.end })
    LCurly -> do
      e <- ignoreNewlines st do
        advance st
        expr st
      c <- curSpan st
      expect st RCurly
      pure (NBrace e { start: m, end: c.end })
    AIdent -> nameTok Ident
    AKeyword -> nameTok Keyword
    Field -> nameTok NField
    FieldImmediate -> nameTok NField
    Tag -> nameTok NTag
    Int -> advanceClose st m (NInt (case t.value of
      VInt i -> i
      _ -> BI.fromInt 0))
    String -> advanceClose st m (NString (case t.value of
      VString s -> s
      _ -> ""))
    Block -> block st
    k -> do
      reportUnexpected st k "a token that can start an expression"
      advanceClose st m NError
  where
  nameTok f = do
    t <- curTok st
    x <- curName st
    advanceClose st t.span.start (f x)

expr :: forall r. PS r -> ST r Ntn
expr st = arg st >>= go { binding: 0, assoc: AssocNon }
  where
  go p lhs = do
    t <- curTok st
    case t.kind of
      k | k == SIdent || k == SKeyword -> do
        x <- curName st
        let op = if k == SIdent then Ident x t.span else Keyword x t.span
        p' <- case confLookup st.config (lastSeg x) of
          Just p' -> pure p'
          Nothing -> do
            report st t.span 1 SWarning ("Defaulted precedence of " <> x <> " to the same as +")
            pure { binding: 50, assoc: AssocL }
        case precLe p p' of
          Nothing -> do
            report st t.span 2 SError "Incompatible precedences"
            pure lhs
          Just false -> pure lhs
          Just true -> do
            advance st
            rhs <- arg st >>= go p'
            go p (Infix lhs op rhs)
      k | argStart k -> do
        rhs <- arg st
        go p (Juxt lhs rhs)
      _ -> pure lhs

decl :: forall r. PS r -> ST r Ntn
decl st = do
  p <- _.start <$> curSpan st
  go p []
  where
  go p mods = cur st >>= case _ of
    Modifier -> do
      m <- curName st
      advance st
      go p (A.snoc mods m)
    Decl -> do
      x <- curName st
      advance st
      n <- expr st
      pure (MDecl mods x n { start: p, end: (ntnSpan n).end })
    _ -> do
      s <- curSpan st
      report st s 3 SError "expected a declaration after a declaration modifier"
      advanceClose st p NError

stmt :: forall r. PS r -> ST r Ntn
stmt st = cur st >>= case _ of
  Modifier -> decl st
  Decl -> decl st
  _ -> expr st

stmts :: forall r. PS r -> ST r (Array Ntn)
stmts st = tailRecM go []
  where
  go ns = cur st >>= case _ of
    Nl -> advance st *> pure (Loop ns)
    k | k == End || k == Eof -> pure (Done ns)
    _ -> do
      n <- stmt st
      -- a statement must be followed by a newline, `end`, or the end of the
      -- file; anything else is reported, and the block ends here (`block`
      -- then consumes the offending token as its `end`)
      t <- curTok st
      if A.elem t.kind [ Nl, End, Eof ] then pure (Loop (A.snoc ns n))
      else do
        report st t.span 4 SError "expected a newline, end, or eof after a statement"
        pure (Done (A.snoc ns n))

block :: forall r. PS r -> ST r Ntn
block st = do
  t <- curTok st
  x <- curName st
  advance st
  -- the header is a whole expression (`match f x`), as in the version of
  -- fnotation the reference implementation builds against
  h <- cur st >>= case _ of
    k | argStart k -> Just <$> expr st
    _ -> pure Nothing
  ns <- stmts st
  advanceClose st t.span.start (NBlock x h ns)

parse :: ConfTable Prec -> Array Token -> { ntns :: Array Ntn, diags :: Array Diag }
parse config tokens = ST.run do
  pos <- STRef.new 0
  skipNewlines <- STRef.new false
  diags <- STA.new
  let st = { pos, skipNewlines, tokens, diags, config }
  ntns <- stmts st
  ds <- STA.unsafeFreeze diags
  pure { ntns, diags: ds }

-- ===========================================================================
-- printing notation
-- ===========================================================================

data PrevPrec = LeftOf Prec | RightOf Prec | Top | Bot

tighter :: Prec -> PrevPrec -> Boolean
tighter p = case _ of
  Top -> true
  Bot -> false
  LeftOf p'
    | p.binding > p'.binding -> true
    | p.binding < p'.binding -> false
    | otherwise -> p.assoc == AssocL && p'.assoc == AssocL
  RightOf p'
    | p.binding > p'.binding -> true
    | p.binding < p'.binding -> false
    | otherwise -> p.assoc == AssocR && p'.assoc == AssocR

looser :: Prec -> PrevPrec -> Boolean
looser p pp = not (tighter p pp)

precApp :: Prec
precApp = { binding: 100, assoc: AssocL }

par :: Boolean -> Doc -> Doc
par b d = if b then P.text "(" <> d <> P.text ")" else d

isNameChar :: Char -> Boolean
isNameChar c = isLetter c || isDigit c || c == '_' || c == '-'

-- | a name segment, quoted with backticks unless it lexes as an identifier
nameSegDoc :: ConfTable Kind -> String -> Doc
nameSegDoc lconfig x =
  let
    cs = SCU.toCharArray x
    plain = A.all isNameChar cs && (case A.head cs of
      Just c -> isLetter c || c == '_'
      Nothing -> false)
  in
    case confLookup lconfig x of
      Nothing | plain -> P.text x
      Just AIdent | plain -> P.text x
      _ -> P.text ("`" <> x <> "`")

nameDoc :: ConfTable Kind -> String -> Doc
nameDoc lconfig x = P.concatWith (\a b -> a <> P.text "/" <> b) (map (nameSegDoc lconfig) (S.split (S.Pattern "/") x))

opNameDoc :: ConfTable Kind -> String -> Doc
opNameDoc lconfig x =
  let segs = S.split (S.Pattern "/") x
  in case A.unsnoc segs of
    Just { init, last } -> P.concatWith (\a b -> a <> P.text "/" <> b) (A.snoc (map (nameSegDoc lconfig) init) (P.text last))
    Nothing -> P.text x

-- | a projection path `a.b.c` prints without spaces and needs no parentheses
isPath :: Ntn -> Boolean
isPath = case _ of
  Ident _ _ -> true
  Juxt l (NField _ _) -> isPath l
  _ -> false

prettyNtn :: ConfTable Prec -> ConfTable Kind -> Ntn -> Doc
prettyNtn config lconfig = prt Top
  where
  prtTop x = prt Top x
  prt p = case _ of
    n@(Juxt l (NField x _)) | isPath n -> prt (LeftOf precApp) l <> P.text "." <> nameDoc lconfig x
    Juxt n n' ->
      par (looser precApp p) (prt (LeftOf precApp) n <+> prt (RightOf precApp) n')
    Infix l n r ->
      let
        mp' = case n of
          Ident x _ -> confLookup config (lastSeg x)
          Keyword x _ -> confLookup config (lastSeg x)
          _ -> Nothing
        p' = fromMaybe { binding: 50, assoc: AssocL } mp'
      in
        par (looser p' p) (prt (LeftOf p') l <+> prt Bot n <+> prt (RightOf p') r)
    NBlock x hd ss _ ->
      P.vsep
        ( [ P.text x <> (case hd of
              Just h -> P.text " " <> prtTop h
              Nothing -> mempty)
          ]
            <> map (\s -> P.indent 2 (prtTop s)) ss
            <> [ P.text "end" ]
        )
    MDecl ms x n _ -> P.hsep (map P.text (A.snoc ms x)) <+> prtTop n
    Ident x _ -> case p of
      Bot -> opNameDoc lconfig x
      _ -> nameDoc lconfig x
    Keyword x _ -> P.text x
    NField x _ -> P.text "." <> nameDoc lconfig x
    NTag x _ -> P.text "'" <> nameDoc lconfig x
    NInt i _ -> P.text (show i)
    NString x _ -> P.text ("\"" <> x <> "\"")
    NTuple ns _ ->
      P.group
        ( P.text "["
            <> P.nest 2 (P.line' <> P.concatWith (\a b -> a <> P.text "," <> P.line <> b) (map prtTop ns))
            <> P.line'
            <> P.text "]"
        )
    NBrace n _ -> P.text "{" <> prtTop n <> P.text "}"
    NError _ -> P.text "<error>"
