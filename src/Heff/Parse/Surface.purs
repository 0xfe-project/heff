-- | The Haskell-like syntax, read into the notation (`Ntn`) the elaborator
-- | takes.
-- |
-- |   data List a = Nil | Cons a (List a)
-- |   map :: {a b :: Type} -> (a -> b) -> List a -> List b
-- |   map f Nil = Nil
-- |   map f (Cons y ys) = Cons (f y) (map f ys)
-- |
-- | Layout is Haskell's, simplified: a declaration starts in column 0; after
-- | `where`, `of` and `do` the next token sets the column of a block, whose
-- | items start in that column; anything less indented ends it.  Inside
-- | brackets there is no layout.
-- |
-- | A capitalised name is a constructor (a tag) unless it names something in
-- | scope: a definition of the file or of what it imports, or a binder.
-- |
-- | Operators (`a + b`, which is `(+) a b`) bind as the fixity declarations
-- | of the file and of what it imports say (`Heff.Parse.Fixity`); `(+)` names
-- | one, in a signature, an equation or an expression.
module Heff.Parse.Surface
  ( readProgram
  , readProgramWith
  , Options
  , Tok
  , Comment
  , lex
  ) where

import Prelude

import Control.Monad.Except (ExceptT, runExceptT, throwError)
import Control.Monad.State (State, evalState, gets, modify_)
import Data.Array as A
import Data.Array.NonEmpty as NEA
import Data.Foldable (for_)
import Data.Either (Either(..))
import Data.List (List(..), (:))
import Data.Maybe (Maybe(..), fromMaybe, maybe)
import Data.Number as Number
import Data.String as S
import Heff.Parse.Notation (Diag, Ntn(..), Severity(..), Span, getGroup, group, ntnSpan)
import Heff.Parse.Fixity (Assoc(..), Fixities, Relation(..), bindsTighter, emptyFixities, groupOf, relate, reserved, union)
import Heff.Support.BigInt as BI
import Heff.Support.Pretty as P

type Tok = { kind :: String, text :: String, start :: Int, end :: Int, line :: Int, col :: Int, first :: Boolean }

type Comment = { start :: Int, end :: Int, text :: String }

foreign import lexImpl :: String -> { tokens :: Array Tok, errors :: Array { start :: Int, end :: Int, msg :: String }, comments :: Array Comment }


-- | `:` annotates (`::` is an operator: the prelude's cons)
isAnnText :: String -> Boolean
isAnnText x = x == ":"

-- | the tokens, and apart from them the comments (`--` and `{- -}`)
lex :: String -> { tokens :: Array Tok, errors :: Array { start :: Int, end :: Int, msg :: String }, comments :: Array Comment }
lex = lexImpl

-- | `start`: the token that starts the current item (it is at the start of
-- | its line, yet it does not end anything)
-- | `binders`: the names each signature binds (in its equations' patterns,
-- | a capitalised one is that variable, not a constructor), and which of them
-- | are implicit (in scope in the equations, unwritten); `patVars`: those
-- | of the equation being read
-- | `fix`: the fixities in scope; `lenient`: an operator with none, or two
-- | unrelated, bind as one left-associative group (for reading a file on its
-- | own, without what it imports: the formatter's)
type St = { toks :: Array Tok, i :: Int, top :: Int, stack :: Array Int, scope :: Array String, known :: String -> Boolean, start :: Int, binders :: Array { f :: String, names :: Array String, implicits :: Array String }, patVars :: Array String, fix :: Fixities, lenient :: Boolean }

type P a = ExceptT Diag (State St) a

-- | the program, as declarations the elaborator takes; `known` says which
-- | capitalised names are imported definitions
readProgram :: (String -> Boolean) -> String -> { ntns :: Array Ntn, diags :: Array Diag }
readProgram known src = let r = readProgramWith { known, fixities: emptyFixities, lenient: false } src in { ntns: r.ntns, diags: r.diags }

-- | `fixities`: those of what the file imports; the result's, the file's own
type Options = { known :: String -> Boolean, fixities :: Fixities, lenient :: Boolean }

-- | `broken`: the names of the declarations that do not parse (a signature
-- | with an equation that does not is a declaration still, of `abandon`:
-- | its type is known to what uses it)
readProgramWith :: Options -> String -> { ntns :: Array Ntn, diags :: Array Diag, fixities :: Fixities, broken :: Array String }
readProgramWith o src =
  let
    l = lexImpl src
    lexDiags = map (\e -> diag { start: e.start, end: e.end } e.msg) l.errors
    declared = prescan l.tokens
    own = fixityDecls o.fixities l.tokens
    st0 = { toks: l.tokens, i: 0, top: 0, stack: [ 0 ], scope: [], known: \x -> o.known x || A.elem x declared, start: 0, binders: [], patVars: [], fix: union o.fixities own.fixities, lenient: o.lenient }
    r = evalState (program []) st0
  in
    { ntns: assemble r.broken r.items, diags: lexDiags <> own.diags <> r.diags <> assembleDiags r.broken r.items, fixities: own.fixities, broken: r.broken }

-- | the fixity declarations of a file, `infixl + - above ==`, read before
-- | the rest (an operator may be used before its declaration); `imported`:
-- | those in scope already
fixityDecls :: Fixities -> Array Tok -> { fixities :: Fixities, diags :: Array Diag }
fixityDecls imported ts =
  let
    decls = A.catMaybes (A.mapWithIndex one ts)
    groups = map (\d -> { id: fromMaybe "" (A.head (map _.text d.ops)), assoc: d.assoc, ops: map _.text d.ops }) (A.filter (\d -> not (A.null d.ops)) decls)
    scope = union imported { groups, above: [] }
    -- (an operator's group, among those of the file and those imported)
    ref t = groupOf scope t.text
    edges = A.concatMap (\d -> case A.head d.ops of
      Just op -> A.mapMaybe (\r -> ref r <#> \g -> { tighter: op.text, looser: g.id }) d.above
        <> A.mapMaybe (\r -> ref r <#> \g -> { tighter: g.id, looser: op.text }) d.below
      Nothing -> []) decls
    fixities = { groups, above: edges }
    all = union imported fixities
    diags = A.concatMap (\d ->
      map (\t -> diag (spanOf t) ("`" <> t.text <> "` is syntax, not an operator")) (A.filter (\t -> A.elem t.text reserved) d.ops)
        <> map (\t -> diag (spanOf t) ("`" <> t.text <> "` has no fixity to be above or below: declare it first")) (A.filter (\t -> ref t == Nothing) (d.above <> d.below))
        <> (if A.null d.ops then [ diag d.span "a fixity declaration names its operators: `infixl + -`" ] else [])
        <> d.errors
        <> case A.head d.ops of
          Just g ->
            map (\t -> diag (spanOf t) ("the fixity of `" <> t.text <> "` is declared twice")) (A.filter (\t -> A.any (\h -> h.id /= g.text && A.elem t.text h.ops) all.groups) d.ops)
              <> (if bindsTighter all g.text g.text then [ diag d.span ("these fixities go round: `" <> g.text <> "` ends up above itself") ] else [])
          Nothing -> []) decls
  in
    { fixities, diags }
  where
  at k = A.index ts k
  one k t
    | t.first && t.col == 0 && t.kind == "kw" && A.elem t.text [ "infixl", "infixr", "infix" ] =
        let
          assoc = case t.text of
            "infixl" -> AssocLeft
            "infixr" -> AssocRight
            _ -> AssocNone
          -- the rest of the declaration: up to the next one
          rest = A.takeWhile (\u -> not (u.kind == "eof" || (u.first && u.col == 0))) (A.drop (k + 1) ts)
          ops = A.takeWhile (\u -> u.kind == "sym") rest
          after = A.drop (A.length ops) rest
          -- `above ...`, `below ...`, in any order
          go acc xs = case A.uncons xs of
            Nothing -> acc
            Just { head: w, tail } | w.kind == "var" && (w.text == "above" || w.text == "below") ->
              let
                these = A.takeWhile (\u -> u.kind == "sym") tail
                acc' = if w.text == "above" then acc { above = acc.above <> these } else acc { below = acc.below <> these }
              in
                go acc' (A.drop (A.length these) tail)
            Just { head: u } -> acc { errors = A.snoc acc.errors (diag (spanOf u) ("expected `above` or `below` and operators, not `" <> u.text <> "`")) }
          r = go { above: [], below: [], errors: [] } after
          end = fromMaybe t.end (A.last rest <#> _.end)
        in
          Just { assoc, ops, above: r.above, below: r.below, errors: r.errors, span: { start: t.start, end } }
    | otherwise = const Nothing (at k)

diag :: Span -> String -> Diag
diag span msg = { code: 150, severity: SError, msg: P.text msg, span, note: Nothing }

-- | the names a file defines (so that a capitalised one is not a tag)
prescan :: Array Tok -> Array String
prescan ts = A.catMaybes (A.mapWithIndex one ts)
  where
  at k = A.index ts k
  one k t
    | t.first && t.col == 0 && t.kind == "kw" && A.elem t.text [ "data", "type", "theory" ] = at (k + 1) <#> _.text
    | t.first && t.col == 0 && (t.kind == "var" || t.kind == "con") && (maybe false isAnnText (at (k + 1) <#> _.text)) = Just t.text
    | otherwise = Nothing

-- Declarations
--------------------------------------------------------------------------------

-- | what a top-level declaration is, before signatures meet equations
data Item
  = Sig String Span Ntn Span -- ^ the name, where it is written, the type, where the signature is
  | Eqn String Ntn Span -- ^ `f p ... := e`, as the equations of `Heff.Elab.Clauses`
  | Decl Ntn

program :: Array Item -> State St { items :: Array Item, diags :: Array Diag, broken :: Array String }
program acc = go acc [] []
  where
  go items diags broken = do
    modify_ \s -> s { scope = [], stack = [ 0 ], start = s.i, top = s.i }
    t <- gets cur
    if t.kind == "eof" then pure { items, diags, broken }
    else do
      r <- runExceptT decl
      case r of
        Right d -> do
          -- a declaration ends where the next begins
          t' <- gets cur
          if t'.kind == "eof" then go (items <> d) diags broken
          else do
            skipDecl
            go (items <> d) (A.snoc diags (diag (spanOf t') ("unexpected `" <> t'.text <> "`"))) broken
        Left d -> do
          x <- gets declared
          skipDecl
          go items (A.snoc diags d) (broken <> A.fromFoldable x)
  -- the name the declaration starting at `top` defines, as far as it says
  declared s = case A.index s.toks s.top, A.index s.toks (s.top + 1) of
    Just t, Just u | t.kind == "kw" && A.elem t.text [ "data", "type", "theory" ] -> Just u.text
    Just t, Just u | t.text == "(" && u.kind == "sym" -> Just u.text
    Just t, _ | t.kind == "var" || t.kind == "con" -> Just t.text
    _, _ -> Nothing

-- | on to the next declaration (the first token of this one, at least)
skipDecl :: State St Unit
skipDecl = do
  modify_ \s -> s { i = max s.i (s.top + 1), stack = [ 0 ], scope = [] }
  t <- gets cur
  unless (t.kind == "eof") (modify_ (\s -> s { i = s.i + 1 }) *> skipDecl)

decl :: P (Array Item)
decl = lazyP \_ -> do
  t <- peek
  case t.kind, t.text of
    "kw", "data" -> A.singleton <<< Decl <$> dataDecl
    "kw", "type" -> A.singleton <<< Decl <$> typeDecl
    "kw", "theory" -> A.singleton <<< Decl <$> theoryDecl
    "kw", "foreign" -> A.singleton <<< Decl <$> foreignDecl
    -- (read before the rest, by `fixityDecls`)
    "kw", _ | A.elem t.text [ "infixl", "infixr", "infix" ] -> do
      let
        skip = do
          u <- peek
          unless (u.kind == "eof" || (u.first && u.col == 0)) (next *> skip)
      _ <- next
      skip
      pure []
    "kw", "test" -> do
      _ <- next
      n <- expectKind "var" "a test's name"
      pure [ Decl (MDecl [] "test" (Ident n.text (spanOf n)) (spanTo t n)) ]
    "watch", _ -> do
      _ <- next
      e <- expr
      end <- prevEnd
      pure [ Decl (MDecl [] ">" e { start: t.start, end }) ]
    _, _ -> do
      name <- declName
      case name of
        Nothing -> throwError (diag (spanOf t) ("a declaration cannot start with `" <> t.text <> "`"))
        Just f -> do
          t2 <- peekAt f.len
          if isAnnText t2.text then do
            modify_ \s -> s { i = s.i + f.len + 1 }
            ty <- expr
            end <- prevEnd
            modify_ \s -> s { binders = A.snoc s.binders { f: f.name, names: piBinders ty, implicits: piImplicits ty } }
            pure [ Sig f.name f.span ty { start: t.start, end } ]
          else equation f

-- | what a signature or an equation defines: `f`, `C`, or `(+)` (`len`: its
-- | tokens, `span`: the name's)
declName :: P (Maybe { name :: String, len :: Int, span :: Span })
declName = do
  t <- peek
  t2 <- peekAt 1
  t3 <- peekAt 2
  pure
    if t.kind == "var" || t.kind == "con" then Just { name: t.text, len: 1, span: spanOf t }
    else if t.text == "(" && isOperator t2 && t3.text == ")" then Just { name: t2.text, len: 3, span: spanOf t2 }
    else Nothing

-- | `f p ... = e`
equation :: { name :: String, len :: Int, span :: Span } -> P (Array Item)
equation f = lazyP \_ -> do
  start <- gets \s -> (cur s).start
  modify_ \s -> s { i = s.i + f.len }
  sig <- gets \s -> A.filter (\b -> b.f == f.name) s.binders
  modify_ \s -> s { patVars = A.concatMap _.names sig }
  pats <- many patAtom
  modify_ \s -> s { patVars = [] }
  _ <- expectSym "="
  -- (an implicit parameter is not written, but bound all the same, by the
  -- name the signature gives it)
  withScope (A.concatMap patVars pats <> A.concatMap _.implicits sig) do
    rhs <- expr
    let sp = { start, end: (ntnSpan rhs).end }
    pure [ Eqn f.name (Infix (group (NEA.cons' (Ident f.name f.span) pats)) (Keyword ":=" sp) rhs) sp ]

-- | a symbol that is an operator (not syntax)
isOperator :: Tok -> Boolean
isOperator t = t.kind == "sym" && not (A.elem t.text reserved)

-- | `data T a (n :: Nat) = C x y | D`, and `data T :: U 1 = …` (its
-- | universe, given)
dataDecl :: P Ntn
dataDecl = lazyP \_ -> do
  kw <- next
  name <- expectKind "con" "the name of a type"
  params <- many dataParam
  let head = group (NEA.cons' (Ident name.text (spanOf name)) params)
  withScope (A.concatMap binderNames params) do
    t0 <- peek
    universe <-
      if isAnnText t0.text && t0.kind == "sym" then next *> app
      else pure (Keyword "Type" (spanOf name))
    t <- peek
    alts <-
      if t.text == "=" then do
        _ <- next
        sepBy1 "|" alternative
      else pure []
    -- (a type is a term: its name cannot also be a constructor's)
    for_ alts \alt -> case NEA.head (getGroup alt) of
      NTag c csp | c == name.text -> throwError (diag csp ("`" <> c <> "` names the type; its constructor needs another name, e.g. `Mk" <> c <> "`"))
      _ -> pure unit
    end <- prevEnd
    let sp = { start: kw.start, end }
    pure (MDecl [] "def" (Infix (Infix head (Keyword ":" sp) universe) (Keyword ":=" sp) (NBlock "sum" Nothing alts sp)) sp)
  where
  alternative = do
    c <- next
    case c.kind of
      "con" -> do
        args <- many atom
        pure (group (NEA.cons' (NTag c.text (spanOf c)) args))
      "tag" -> do
        args <- many atom
        pure (group (NEA.cons' (NTag c.text (spanOf c)) args))
      _ -> throwError (diag (spanOf c) "a constructor starts with a capital")

-- | a parameter of a type: `a` (a type), `(x :: T)` or `{x :: T}`
dataParam :: P Ntn
dataParam = lazyP \_ -> do
  t <- peek
  b <- atBoundary
  if b then throwError (diag (spanOf t) "no parameter here")
  else if t.kind == "con" then throwError (diag (spanOf t) ("a type's parameter is lower case, `" <> S.toLower t.text <> "`, or given its type: `(" <> t.text <> " :: Type)`"))
  else if t.kind == "var" then do
    _ <- next
    pure (Infix (Ident t.text (spanOf t)) (Keyword ":" (spanOf t)) (Keyword "Type" (spanOf t)))
  else if t.text == "(" || t.text == "{" then do
    -- (a type's parameter in parentheses is a binder, `->` after it or not)
    p0 <- atom
    p <- if t.text == "(" then asBinder p0 else pure p0
    case p of
      Infix (Ident _ _) (Keyword ":" _) _ -> pure p
      NBlock "sig" Nothing [ f@(Infix (Ident _ _) (Keyword ":" _) _) ] sp -> pure (NBrace f sp)
      _ -> throwError (diag (ntnSpan p) "a parameter is `a`, `(x : T)` or `{x : T}`")
  else throwError (diag (spanOf t) "no parameter here")

-- | the names a (function) type binds: `(x :: A) -> …`, `{x :: A} -> …`
piBinders :: Ntn -> Array String
piBinders = case _ of
  Infix b (Keyword "->" _) rest -> binderNames b <> piBinders rest
  _ -> []

-- | the names of the implicit parameters, `{x :: A} -> …`
piImplicits :: Ntn -> Array String
piImplicits = case _ of
  Infix b@(NBrace _ _) (Keyword "->" _) rest -> binderNames b <> piImplicits rest
  Infix _ (Keyword "->" _) rest -> piImplicits rest
  _ -> []

binderNames :: Ntn -> Array String
binderNames = case _ of
  Infix (Ident x _) (Keyword ":" _) _ -> [ x ]
  NBrace n _ -> binderNames n
  _ -> []

-- | `type T a = …`
typeDecl :: P Ntn
typeDecl = lazyP \_ -> do
  kw <- next
  name <- expectKind "con" "the name of a type"
  params <- many dataParam
  _ <- expectSym "="
  withScope (A.concatMap binderNames params) do
    body <- expr
    end <- prevEnd
    let
      sp = { start: kw.start, end }
      head = group (NEA.cons' (Ident name.text (spanOf name)) params)
    pure (MDecl [] "def" (Infix (Infix head (Keyword ":" sp) (Keyword "Type" sp)) (Keyword ":=" sp) body) sp)

-- | `theory T a where` and its fields, `x :: A`, each in scope in the next
theoryDecl :: P Ntn
theoryDecl = lazyP \_ -> do
  kw <- next
  name <- expectKind "con" "the name of a theory"
  params <- many dataParam
  _ <- expectKw "where"
  withScope (A.concatMap binderNames params) do
    fields <- block field
    end <- prevEnd
    let
      sp = { start: kw.start, end }
      head = group (NEA.cons' (Ident name.text (spanOf name)) params)
    pure (MDecl [] "theory" (Infix head (Keyword ":=" sp) (NBlock "sig" Nothing fields sp)) sp)
  where
  field = do
    x <- next
    unless (x.kind == "var" || x.kind == "con") $ throwError (diag (spanOf x) "a field is `name :: Type`")
    _ <- expectAnn
    ty <- expr
    modify_ \s -> s { scope = A.cons x.text s.scope }
    pure (Infix (Ident x.text (spanOf x)) (Keyword ":" (spanOf x)) ty)

-- | `foreign import name :: T`, a primitive
foreignDecl :: P Ntn
foreignDecl = lazyP \_ -> do
  kw <- next
  _ <- expectKw "import"
  x <- expectKind "var" "the primitive's name"
  _ <- expectAnn
  ty <- expr
  end <- prevEnd
  let sp = { start: kw.start, end }
  pure (MDecl [] "primitive" (Infix (Ident x.text (spanOf x)) (Keyword ":" sp) ty) sp)

-- | signatures meet their equations: `def f : T where <equations> end`
assemble :: Array String -> Array Item -> Array Ntn
assemble broken items = A.mapMaybe one items
  where
  one = case _ of
    Decl n -> Just n
    Sig f fsp ty sp ->
      let
        eqs = A.mapMaybe (case _ of
          Eqn g e esp | g == f -> Just { e, span: esp }
          _ -> Nothing) items
        -- (the declaration is its signature and its equations)
        whole = sp { end = A.foldl (\e q -> max e q.span.end) sp.end eqs }
      in
        -- (an equation that does not parse: `f = abandon`, not what the
        -- others match without it)
        if A.elem f broken then Just (def f fsp ty sp [ Infix (Ident f fsp) (Keyword ":=" sp) (Keyword "abandon" sp) ])
        else if A.null eqs then Nothing
        else Just (def f fsp ty whole (map _.e eqs))
    Eqn _ _ _ -> Nothing
  def f fsp ty sp eqs = MDecl [] "def" (Infix (Ident f fsp) (Keyword ":" sp) (Juxt ty (NBlock "where" Nothing eqs sp))) sp

assembleDiags :: Array String -> Array Item -> Array Diag
assembleDiags broken items = A.mapMaybe one items
  where
  sigs = A.mapMaybe (case _ of
    Sig f _ _ _ -> Just f
    _ -> Nothing) items
  eqNames = A.mapMaybe (case _ of
    Eqn f _ _ -> Just f
    _ -> Nothing) items
  one = case _ of
    Sig f _ _ sp | not (A.elem f eqNames) && not (A.elem f broken) -> Just (diag sp ("`" <> f <> "` has a type but no definition"))
    Eqn f _ sp | not (A.elem f sigs) && not (A.elem f broken) -> Just (diag sp ("`" <> f <> "` needs a type: `" <> f <> " : …`"))
    _ -> Nothing

-- Expressions
--------------------------------------------------------------------------------

expr :: P Ntn
expr = lazyP \_ -> do
  t <- peek
  e <- case t.kind, t.text of
    "sym", "\\" -> lambda
    "kw", "case" -> caseExpr
    "kw", "if" -> ifExpr
    "kw", "do" -> doExpr
    "kw", "let" -> throwError (diag (spanOf t) "`let` is not supported yet")
    _, _ -> opExpr 0
  -- `e :: T`, an annotation
  t' <- peek
  b <- atBoundary
  if isAnnText t'.text && t'.kind == "sym" && not b then do
    _ <- next
    ty <- expr
    pure (Infix e (Keyword ":" (spanOf t')) ty)
  else pure e

-- | the syntax's operators, loosest first: `->` (right), `~` (right); those
-- | declared bind tighter than both
operators :: Array { sym :: String, prec :: Int, right :: Boolean }
operators =
  [ { sym: "->", prec: 1, right: true }
  , { sym: "~", prec: 2, right: true }
  ]

opExpr :: Int -> P Ntn
opExpr minPrec = do
  lhs <- userOps
  loop lhs
  where
  loop lhs = do
    t <- peek
    b <- atBoundary
    case A.find (\o -> o.sym == t.text) operators of
      Just o | t.kind == "sym" && o.prec >= minPrec && not b -> do
        _ <- next
        rhs <- if o.prec == 1 then arrowRhs else opExpr (if o.right then o.prec else o.prec + 1)
        loop (binop o.sym (spanOf t) lhs rhs)
      _ -> pure lhs
  -- the codomain of `->` may be a lambda, a case, ... as any expression
  arrowRhs = do
    t <- peek
    if t.text == "\\" || (t.kind == "kw" && A.elem t.text [ "case", "if", "do" ]) then expr else opExpr 1

-- | `x -> y`, with `(a b :: T) -> B` for `(a :: T) -> (b :: T) -> B`; and
-- | `T ~ { f = v }`, a specialisation
binop :: String -> Span -> Ntn -> Ntn -> Ntn
binop "->" sp lhs0 rhs = case lhs of
  Infix g (Keyword ":" ksp) ty | Just xs <- names g, A.length xs > 1 ->
    A.foldr (\x b -> Infix (Infix x (Keyword ":" ksp) ty) (Keyword "->" sp) b) rhs xs
  NBrace (Infix g (Keyword ":" ksp) ty) bsp | Just xs <- names g, A.length xs > 1 ->
    A.foldr (\x b -> Infix (NBrace (Infix x (Keyword ":" ksp) ty) bsp) (Keyword "->" sp) b) rhs xs
  _ -> Infix lhs (Keyword "->" sp) rhs
  where
  -- `{a :: T} -> B`: braces with one declaration, left of an arrow, bind
  -- an implicit argument (a record type there is given a name instead)
  lhs = case lhs0 of
    NBlock "sig" Nothing [ f ] bsp -> NBrace f bsp
    _ -> lhs0
  names g = let xs = NEA.toArray (getGroup g) in if A.all isIdent xs then Just xs else Nothing
  isIdent = case _ of
    Ident _ _ -> true
    _ -> false
binop "~" sp lhs rhs = case rhs of
  NBlock "struct" Nothing fields _ ->
    A.foldl (\acc f -> case f of
      Infix (Ident x xsp) k v -> Infix acc (Keyword "~" sp) (NTuple [ Infix (NField x xsp) k v ] (ntnSpan f))
      _ -> acc) lhs fields
  _ -> Infix lhs (Keyword "~" sp) rhs
binop o sp lhs rhs = Infix lhs (Keyword o sp) rhs

-- | `a + b * c`, as the fixities say: `(+) a ((*) b c)`.  An operand is an
-- | application, or, last, a lambda, `case`, `if` or `do`
userOps :: P Ntn
userOps = lazyP \_ -> do
  e0 <- app
  rest <- operands
  if A.null rest then pure e0 else resolve e0 rest
  where
  operands = do
    t <- peek
    b <- atBoundary
    if isOperator t && not b then do
      _ <- next
      u <- peek
      if u.text == "\\" || (u.kind == "kw" && A.elem u.text [ "case", "if", "do" ]) then do
        e <- expr
        pure [ { op: t, e } ]
      else do
        e <- app
        more <- operands
        pure (A.cons { op: t, e } more)
    else pure []

-- | operators and operands into a tree (the shunting yard), refusing
-- | operators that do not go together
resolve :: Ntn -> Array { op :: Tok, e :: Ntn } -> P Ntn
resolve e0 rest = do
  fix <- gets _.fix
  lenient <- gets _.lenient
  let
    -- `&&` and `||` are `if`: the right side is computed only if it is
    -- needed (`False && e` is `False`, whatever `e`), and a call of
    -- itself there is a tail call
    apply op l r = case op.text of
      "&&" -> shortcut op l r (NTag "False" (spanOf op))
      "||" -> shortcut op l (NTag "True" (spanOf op)) r
      _ -> Juxt (Juxt (Ident op.text (spanOf op)) l) r
    -- (the sides are annotated with the `Bool` in scope: the left, so that
    -- a constructor alone can be a scrutinee; the whole, so that it
    -- synthesises where an application would, as an `if` condition does)
    shortcut op c a b =
      let
        sp = { start: (ntnSpan c).start, end: max (ntnSpan a).end (ntnSpan b).end }
        osp = spanOf op
        bool n = Infix n (Keyword ":" osp) (Ident "Bool" osp)
      in bool (NBlock "match" (Just (bool c)) [ Infix (NTag "True" osp) (Keyword "=>" osp) a, Infix (NTag "False" osp) (Keyword "=>" osp) b ] sp)
    reduce st = case A.unsnoc st.ops, A.unsnoc st.out of
      Just o, Just r | Just l <- A.unsnoc r.init -> st { ops = o.init, out = A.snoc l.init (apply o.last l.last r.last) }
      _, _ -> st
    -- `before` is on the stack, `op` comes: does `before` reduce first?
    first before op = case relate fix before.text op.text of
      Tighter -> pure true
      Looser -> pure false
      Same AssocLeft -> pure true
      Same AssocRight -> pure false
      Same AssocNone -> throwError (diag (spanOf op) (if before.text == op.text then "`" <> op.text <> "` does not associate: add parentheses" else "`" <> before.text <> "` and `" <> op.text <> "` do not associate: add parentheses"))
      _ | lenient -> pure true
      Unrelated -> throwError (diag (spanOf op) ("`" <> before.text <> "` and `" <> op.text <> "` are not related: add parentheses, or declare which binds tighter (`above`, `below`)"))
      Undeclared x -> throwError (diag (spanOf (if x == op.text then op else before)) ("`" <> x <> "` has no fixity: declare it (`infixl " <> x <> "`), or import the module that does"))
    push st r = do
      done <- case A.last st.ops of
        Just before -> first before r.op
        Nothing -> pure false
      if done then push (reduce st) r else pure st { ops = A.snoc st.ops r.op, out = A.snoc st.out r.e }
  -- (an operator alone still needs a fixity)
  for_ rest \r -> unless lenient $ when (groupOf fix r.op.text == Nothing) $
    throwError (diag (spanOf r.op) ("`" <> r.op.text <> "` has no fixity: declare it (`infixl " <> r.op.text <> "`), or import the module that does"))
  st <- A.foldM push { ops: [], out: [ e0 ] } rest
  let
    final = go st
    go s = if A.null s.ops then s else go (reduce s)
  pure (fromMaybe e0 (A.head final.out))

-- | application: a head and its arguments
app :: P Ntn
app = lazyP \_ -> do
  h <- atom
  args <- many atom
  pure (A.foldl Juxt h args)

-- | the arguments of an application, with fields: `x`, `C`, `3`, `"s"`,
-- | `(e)`, `{…}`, `p.x`
atom :: P Ntn
atom = lazyP \_ -> do
  t <- peek
  b <- atBoundary
  when (b || not (startsAtom t)) $ throwError (diag (spanOf t) ("expected an expression, not " <> shown t))
  base <- case t.kind of
    "var" -> next *> pure (Ident t.text (spanOf t))
    "con" -> do
      _ <- next
      t2 <- peek
      local <- gets \s -> A.elem t.text s.scope
      qualified <- gets \s -> t2.kind == "field" && s.known (t.text <> "." <> t2.text)
      -- (`M.x`, a definition of the module imported as `M`, unless `M` is
      -- a variable here)
      if qualified && not local then do
        _ <- next
        pure (Ident (t.text <> "." <> t2.text) (spanTo t t2))
      else do
        inScope <- gets \s -> local || s.known t.text
        pure (if inScope then Ident t.text (spanOf t) else NTag t.text (spanOf t))
    "tag" -> next *> pure (NTag t.text (spanOf t))
    "int" -> next *> pure (NInt (BI.fromDigits t.text) (spanOf t))
    "float" -> next *> pure (NFloat (fromMaybe 0.0 (Number.fromString t.text)) (spanOf t))
    "str" -> next *> pure (NString t.text (spanOf t))
    "kw" | A.elem t.text [ "Type", "Int", "Float", "String", "Array", "abandon" ] -> next *> pure (Keyword t.text (spanOf t))
    -- (`data { A | B }`: a sum type with no name, where a type goes)
    "kw" | t.text == "data" -> anonymousSum
    "punct" | t.text == "(" -> parens
    "punct" | t.text == "{" -> braces
    "punct" | t.text == "[" -> listOf expr
    _ -> throwError (diag (spanOf t) ("expected an expression, not " <> shown t))
  fields base
  where
  fields n = do
    t <- peek
    if t.kind == "field" then next *> fields (Juxt n (NField t.text (spanOf t))) else pure n

startsAtom :: Tok -> Boolean
startsAtom t = A.elem t.kind [ "var", "con", "tag", "int", "float", "str" ]
  || (t.kind == "kw" && A.elem t.text [ "Type", "Int", "Float", "String", "Array", "data", "abandon" ])
  || (t.kind == "punct" && (t.text == "(" || t.text == "{" || t.text == "["))

-- | `[a, b, c]`, `Cons a (Cons b (Cons c Nil))` (whichever type has those
-- | tags), and `[]`, `Nil`: of expressions, or of patterns
listOf :: P Ntn -> P Ntn
listOf item = lazyP \_ -> do
  o <- next
  inBrackets do
    t <- peek
    xs <- if t.text == "]" then pure [] else sepBy1 "," item
    c <- expectPunct "]"
    let sp = spanTo o c
    pure (A.foldr (\x rest -> group (NEA.cons' (NTag "Cons" sp) [ x, rest ])) (NTag "Nil" sp) xs)

-- | `( e )`, `( e :: T )`, `(x := e)` (a named argument), `(+)` (an
-- | operator, named)
parens :: P Ntn
parens = lazyP \_ -> do
  o <- next
  inBrackets do
    t <- peek
    t2 <- peekAt 1
    if isOperator t && t2.text == ")" then do
      _ <- next
      _ <- next
      pure (Ident t.text (spanOf t))
    -- (`(x := e)`: the implicit argument named `x`, capitalised or not)
    else if (t.kind == "var" || t.kind == "con") && t2.text == ":=" then do
      _ <- next
      _ <- next
      e <- expr
      c <- expectPunct ")"
      pure (NBrace (Infix (Ident t.text (spanOf t)) (Keyword ":=" (spanOf t2)) e) (spanTo o c))
    else do
      e <- expr
      t' <- peek
      -- `(a, b)`: a pair (HOTT)
      if t'.text == "," then do
        rest <- many1Sep
        c <- expectPunct ")"
        pure (NTuple (A.cons e rest) (spanTo o c))
      else do
        _ <- expectPunct ")"
        -- (`(A :: T) -> B` binds `A`; `(C :: T)` alone annotates `C`)
        after <- peek
        if after.kind == "sym" && after.text == "->" then asBinder e else pure e

-- | `, e, e ...` (after the first of a tuple's components)
many1Sep :: P (Array Ntn)
many1Sep = lazyP \_ -> do
  t <- peek
  if t.text == "," then do
    _ <- next
    e <- expr
    rest <- many1Sep
    pure (A.cons e rest)
  else pure []

-- | `x :: T` in a binder's parentheses names its variable (in scope after)
binderOrExpr :: P Ntn
binderOrExpr = lazyP \_ -> expr >>= asBinder

-- | `A :: T` as a binder: what is left of `::` is names, capitalised or not
-- | (in scope after)
asBinder :: Ntn -> P Ntn
asBinder e0 = do
  let
    e = case e0 of
      Infix g k@(Keyword ":" _) t | A.all isName (NEA.toArray (getGroup g)) -> Infix (asNames g) k t
      _ -> e0
  case e of
    Infix g (Keyword ":" _) _ -> modify_ \s -> s { scope = identsOf g <> s.scope }
    _ -> pure unit
  pure e
  where
  -- (a capitalised name, not a tag: `'x` is written one longer than `x`)
  isName = case _ of
    Ident _ _ -> true
    NTag x sp -> sp.end - sp.start == S.length x
    _ -> false
  asNames g = group (map (case _ of
    NTag x sp -> Ident x sp
    n -> n) (getGroup g))
  identsOf g = A.mapMaybe (case _ of
    Ident x _ -> Just x
    NTag x _ -> Just x
    _ -> Nothing) (NEA.toArray (getGroup g))

-- | `{ x :: A, … }` (a record type), `{ x = e, … }` (a record), `{}`, and
-- | `{x :: A}` (an implicit binder)
braces :: P Ntn
braces = lazyP \_ -> do
  o <- next
  inBrackets do
    t <- peek
    t2 <- peekAt 1
    if t.text == "}" then do
      c <- next
      pure (NBlock "struct" Nothing [] (spanTo o c))
    else if t2.text == "=" then do
      fs <- sepBy1 "," setting
      c <- expectPunct "}"
      pure (NBlock "struct" Nothing fs (spanTo o c))
    else do
      -- a record type (or, left of `->`, an implicit binder)
      fs <- sepBy1 "," declaration
      c <- expectPunct "}"
      pure (NBlock "sig" Nothing fs (spanTo o c))
  where
  setting = lazyP \_ -> do
    x <- next
    _ <- expectSym "="
    e <- expr
    pure (Infix (Ident x.text (spanOf x)) (Keyword ":=" (spanOf x)) e)
  declaration = lazyP \_ -> do
    e <- binderOrExpr
    case e of
      Infix _ (Keyword ":" _) _ -> pure e
      _ -> throwError (diag (ntnSpan e) "a field is `name :: Type`")

-- | `data { A x | B }`, a sum type without a name (`data`, where a type goes)
anonymousSum :: P Ntn
anonymousSum = lazyP \_ -> do
  kw <- next
  _ <- expectPunct "{"
  inBrackets do
    alts <- sepBy1 "|" do
      c <- next
      args <- many atom
      pure (group (NEA.cons' (NTag c.text (spanOf c)) args))
    c <- expectPunct "}"
    pure (NBlock "sum" Nothing alts (spanTo kw c))

-- | `\x {y} _ -> e`
lambda :: P Ntn
lambda = lazyP \_ -> do
  _ <- next
  bs <- many1 binder
  _ <- expectSym "->"
  withScope (A.concatMap bound bs) do
    body <- expr
    pure (A.foldr (\b e -> Infix b (Keyword "=>" (ntnSpan b)) e) body bs)
  where
  -- (a binder is a name, capitalised or not -- a lambda has no
  -- patterns -- or `(x :: T)`, the name annotated)
  binder = do
    t <- peek
    case t.kind, t.text of
      "var", _ -> next *> pure (Ident t.text (spanOf t))
      "con", _ -> next *> pure (Ident t.text (spanOf t))
      "punct", "{" -> do
        _ <- next
        x <- peek
        when (x.kind /= "var" && x.kind /= "con") $ throwError (diag (spanOf x) "expected an implicit argument's name")
        _ <- next
        c <- expectPunct "}"
        pure (NBrace (Ident x.text (spanOf x)) (spanTo t c))
      "punct", "(" -> do
        _ <- next
        inBrackets do
          x <- peek
          when (x.kind /= "var" && x.kind /= "con") $ throwError (diag (spanOf x) "expected a binder's name: `\\(x : T) -> e`")
          _ <- next
          cc <- expectAnn
          ty <- expr
          _ <- expectPunct ")"
          pure (Infix (Ident x.text (spanOf x)) (Keyword ":" (spanOf cc)) ty)
      _, _ -> throwError (diag (spanOf t) "a lambda binds names: `\\x y -> e`, `\\(x : T) -> e`")
  bound = case _ of
    Ident x _ -> [ x ]
    NBrace (Ident x _) _ -> [ x ]
    Infix (Ident x _) (Keyword ":" _) _ -> [ x ]
    _ -> []

-- | `case e of` and its alternatives, `C x y -> e`
caseExpr :: P Ntn
caseExpr = lazyP \_ -> do
  kw <- next
  scrut <- expr
  _ <- expectKw "of"
  alts <- block alternative
  end <- prevEnd
  let sp = { start: kw.start, end }
  pure (NBlock "match" (Just scrut) alts sp)
  where
  alternative = do
    p <- pattern
    arrow <- expectSym "->"
    withScope (patVars p) do
      e <- expr
      pure (Infix p (Keyword "=>" (spanOf arrow)) e)

-- | `if c then a else b`, a `case` on `True` and `False`
ifExpr :: P Ntn
ifExpr = lazyP \_ -> do
  kw <- next
  c <- expr
  _ <- expectKw "then"
  a <- expr
  _ <- expectKw "else"
  b <- expr
  end <- prevEnd
  let sp = { start: kw.start, end }
  pure (NBlock "match" (Just c) [ Infix (NTag "True" sp) (Keyword "=>" sp) a, Infix (NTag "False" sp) (Keyword "=>" sp) b ] sp)

-- | `do` and its statements, `x <- e` or `e`
doExpr :: P Ntn
doExpr = lazyP \_ -> do
  kw <- next
  stmts <- block statement
  end <- prevEnd
  let sp = { start: kw.start, end }
  pure (NBlock "do" Nothing stmts sp)
  where
  statement = do
    t <- peek
    t2 <- peekAt 1
    if t.kind == "var" && t2.text == "<-" then do
      _ <- next
      arrow <- next
      e <- expr
      -- (the rest of the block sees it)
      modify_ \s -> s { scope = A.cons t.text s.scope }
      pure (Infix (Ident t.text (spanOf t)) (Keyword "<-" (spanOf arrow)) e)
    else expr

-- Patterns
--------------------------------------------------------------------------------

-- | a pattern: `C p …`, or an atomic one
pattern :: P Ntn
pattern = lazyP \_ -> do
  p <- patHead
  consTail p
  where
  patHead = do
    t <- peek
    if t.kind == "con" || t.kind == "tag" then do
      _ <- next
      args <- many patAtom
      pure (group (NEA.cons' (NTag t.text (spanOf t)) args))
    else patAtom
  -- `x :: xs` is the `Cons` in scope (as a literal is the equality in
  -- scope), and binds right: `x :: y :: rest`
  consTail l = do
    t <- peek
    b <- atBoundary
    if t.kind == "sym" && t.text == "::" && not b then do
      _ <- next
      r <- pattern
      pure (group (NEA.cons' (NTag "Cons" (spanOf t)) [ l, r ]))
    else pure l

-- | `x`, `_`, `C` (with no arguments), `(p)`
patAtom :: P Ntn
patAtom = lazyP \_ -> do
  t <- peek
  b <- atBoundary
  when b $ throwError (diag (spanOf t) "expected a pattern")
  case t.kind, t.text of
    "var", _ -> next *> pure (Ident t.text (spanOf t))
    "con", _ -> do
      _ <- next
      -- (a capitalised parameter of the signature is a variable)
      isVar <- gets \s -> A.elem t.text s.patVars
      pure (if isVar then Ident t.text (spanOf t) else NTag t.text (spanOf t))
    "tag", _ -> next *> pure (NTag t.text (spanOf t))
    -- (a literal: matched by the equality in scope, `Heff.Elab.Clauses`)
    "int", _ -> next *> pure (NInt (BI.fromDigits t.text) (spanOf t))
    "float", _ -> next *> pure (NFloat (fromMaybe 0.0 (Number.fromString t.text)) (spanOf t))
    "str", _ -> next *> pure (NString t.text (spanOf t))
    "punct", "[" -> listOf pattern
    "punct", "(" -> do
      _ <- next
      inBrackets do
        p <- pattern
        _ <- expectPunct ")"
        pure p
    _, _ -> throwError (diag (spanOf t) ("expected a pattern, not " <> shown t))

patVars :: Ntn -> Array String
patVars n = case NEA.uncons (getGroup n) of
  { head: Ident x _, tail: [] } | x /= "_" -> [ x ]
  { head: NTag _ _, tail } -> A.concatMap patVars tail
  _ -> []

-- Layout and tokens
--------------------------------------------------------------------------------

-- | (the parsers refer to one another)
lazyP :: forall a. (Unit -> P a) -> P a
lazyP f = pure unit >>= f

cur :: St -> Tok
cur s = tokAt s s.i

-- | the token at an index, as the declaration being read sees it: a line
-- | starting at the left margin begins the next declaration, so it is the
-- | end of this one -- in brackets too, where lines end nothing else.  (The
-- | end is where the last token before it ends.)
tokAt :: St -> Int -> Tok
tokAt s j = case A.index s.toks j of
  Just t | j > s.top && t.first && t.col == 0 && t.kind /= "eof" -> endAt
  Just t | t.kind /= "eof" -> t
  _ -> endAt
  where
  e = maybe 0 _.end (A.index s.toks (j - 1))
  endAt = eofTok { start = e, end = e }

-- | a token, as an error says what it found
shown :: Tok -> String
shown t = if t.kind == "eof" then "the end of the declaration" else "`" <> t.text <> "`"

eofTok :: Tok
eofTok = { kind: "eof", text: "", start: 0, end: 0, line: 0, col: 0, first: true }

peek :: P Tok
peek = gets cur

peekAt :: Int -> P Tok
peekAt k = gets \s -> tokAt s (s.i + k)

-- | where the last token read ends (a closing parenthesis, which the span of
-- | what is in it leaves out, included)
prevEnd :: P Int
prevEnd = (_.end) <$> peekAt (-1)

next :: P Tok
next = do
  t <- peek
  modify_ \s -> s { i = s.i + 1 }
  pure t

-- | the token ends what is being read: it is the end, or starts a line not
-- | indented past the current block
atBoundary :: P Boolean
atBoundary = gets \s ->
  let t = cur s
  in t.kind == "eof" || (t.first && t.col <= fromMaybe 0 (A.last s.stack) && s.i /= s.start)

-- | the items of a block: the first sets the column
block :: forall a. P a -> P (Array a)
block item = do
  t <- peek
  when (t.kind == "eof") $ throwError (diag (spanOf t) "a block is empty")
  outer <- gets \s -> fromMaybe 0 (A.last s.stack)
  when (t.first && t.col <= outer) $ throwError (diag (spanOf t) "a block must be indented past what it is in")
  modify_ \s -> s { stack = A.snoc s.stack t.col }
  saved <- gets _.scope
  let
    go acc = do
      modify_ \s -> s { start = s.i }
      x <- item
      t' <- peek
      if t'.kind /= "eof" && t'.first && t'.col == t.col then go (A.snoc acc x) else pure (A.snoc acc x)
  xs <- go []
  modify_ \s -> s { stack = fromMaybe s.stack (A.init s.stack), scope = saved }
  pure xs

-- | inside brackets, lines do not end anything
inBrackets :: forall a. P a -> P a
inBrackets p = do
  modify_ \s -> s { stack = A.snoc s.stack (-1) }
  x <- p
  modify_ \s -> s { stack = fromMaybe s.stack (A.init s.stack) }
  pure x

withScope :: forall a. Array String -> P a -> P a
withScope xs p = do
  saved <- gets _.scope
  modify_ \s -> s { scope = xs <> s.scope }
  x <- p
  modify_ \s -> s { scope = saved }
  pure x

-- | as many as there are (a failure that reads nothing ends them)
-- | (`many` and `sepBy1` gather newest first, into a list: an array
-- | copied per item is a copy live per pending continuation -- memory
-- | quadratic in the count, for a list of thousands)
many :: forall a. P a -> P (Array a)
many p = go Nil
  where
  go acc = do
    b <- atBoundary
    t <- peek
    if b || not (startsItem t) then pure (A.reverse (A.fromFoldable acc))
    else do
      x <- p
      go (x : acc)
  startsItem t = startsAtom t || t.kind == "var" || (t.kind == "punct" && t.text == "(")

many1 :: forall a. P a -> P (Array a)
many1 p = do
  x <- p
  xs <- many p
  pure (A.cons x xs)

sepBy1 :: forall a. String -> P a -> P (Array a)
sepBy1 sep p = do
  x <- p
  let
    go acc = do
      t <- peek
      if t.text == sep then next *> p >>= \y -> go (y : acc) else pure (A.reverse (A.fromFoldable acc))
  go (x : Nil)

expectAnn :: P Tok
expectAnn = do
  t <- peek
  if t.kind == "sym" && isAnnText t.text then next else throwError (diag (spanOf t) "expected `:`")

expectSym :: String -> P Tok
expectSym x = do
  t <- peek
  if t.kind == "sym" && t.text == x then next else throwError (diag (spanOf t) ("expected `" <> x <> "`, not " <> shown t))

expectPunct :: String -> P Tok
expectPunct x = do
  t <- peek
  if t.kind == "punct" && t.text == x then next else throwError (diag (spanOf t) ("expected `" <> x <> "`, not " <> shown t))

expectKw :: String -> P Tok
expectKw x = do
  t <- peek
  if t.kind == "kw" && t.text == x then next else throwError (diag (spanOf t) ("expected `" <> x <> "`, not " <> shown t))

expectKind :: String -> String -> P Tok
expectKind k what = do
  t <- peek
  if t.kind == k then next else throwError (diag (spanOf t) ("expected " <> what <> ", not " <> shown t))

spanOf :: Tok -> Span
spanOf t = { start: t.start, end: t.end }

spanTo :: forall r s. { start :: Int | r } -> { end :: Int | s } -> Span
spanTo a b = { start: a.start, end: b.end }
