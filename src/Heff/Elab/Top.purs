-- | A port of `Hippogriff.Frontend.Parser`: notation to elaboration
-- | judgments, and the top-level declaration loop.
module Heff.Elab.Top
  ( Judgment(..)
  , elabDecl
  , entryFor
  , topWith
  , topModule
  , topThawed
  , ThawedDef
  , TopResult
  , Watch
  ) where

import Prelude

import Data.Array as A
import Data.Array.NonEmpty as NEA
import Data.Either (Either(..))
import Data.Foldable (foldM, foldl, for_)
import Data.Lazy (Lazy, defer, force)
import Control.Lazy (fix)
import Data.Maybe (Maybe(..), fromMaybe, isJust, maybe)
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Heff.Parse.Notation (Diag, Ntn(..), Severity(..), Span, getGroup, ntnHead, ntnSpan)
import Heff.Support.Common (Name, dictFromList)
import Heff.Core.Evaluation (eval, evalTy)
import Heff.Core.Readback (readbTy)
import Heff.Elab.Clauses (desugar, stripWhere)
import Heff.Elab.Prims (isPrim, primNames)
import Data.String as Str
import Heff.Core.Syntax as Syn
import Heff.Elab.Unify (settle, unsolvedMetas, zonkEl, zonkTy)
import Control.Monad.State (gets, modify_)
import Heff.Core.Globals (GlobalEntry, Globals, addAlias, addBroken, addGlobalEntry, lookupGlobal)
import Heff.Core.Memoed as M
import Heff.Core.Params (Case(..), CtxShape, Universe(..), Variants(..))
import Heff.Core.Print (prtElR)
import Heff.Core.Rename (displayName, selfAsLocal)
import Data.Map as Map
import Heff.Core.Value as V
import Heff.Elab.Environment (BindingMode(..), E, ElabFor(..), Info, TraceEvent, emptyElabEnv, emptyElabEnvFor, emptySt, letAt, reportedElsewhere, runE, tryPure)
import Control.Monad.Except (throwError)
import Control.Monad.Error.Class (try)
import Heff.Elab.Rules as R
import Heff.Support.Pretty (Doc, (<+>))
import Heff.Support.Pretty as P

-- Diagnostics (parser codes are numbered from 200)
--------------------------------------------------------------------------------

failP :: forall a. Span -> Int -> Doc -> E a
failP sp code msg = throwError { code: 200 + code, severity: SError, msg, span: sp, note: Nothing }

unexpectedNotation :: forall a. Ntn -> String -> E a
unexpectedNotation n c =
  failP (ntnSpan n) 3 (P.text "unexpected notation for" <+> P.text c <> P.text ":" <+> ntnHead n)

-- Main functionality
--------------------------------------------------------------------------------

binder :: Ntn -> E R.Binder
binder (Infix (Ident x _) (Keyword ":" _) n) = R.Named x <$> typ N n
-- extension: `{x : A} -> B`
binder (NBrace (Infix (Ident x _) (Keyword ":" _) n) sp) = R.NamedImplicit sp x <$> typ N n
binder n = R.Anonymous <$> typ N n

fieldDeclaration :: Ntn -> E R.FieldDeclaration
fieldDeclaration (Infix (Ident x _) (Keyword ":" _) n) = do
  a <- typ N n
  pure { name: x, typ: a }
fieldDeclaration n = failP (ntnSpan n) 1 (P.text "expected a field declaration of the form `x : A`")

fieldSetting :: Case -> Ntn -> E R.FieldSetting
fieldSetting c (Infix (Ident x xsp) (Keyword ":=" _) n) = do
  v <- chk c n
  pure { name: x, chk: v, span: ntnSpan n, nameSpan: xsp }
fieldSetting _ n = failP (ntnSpan n) 2 (P.text "expected a field setting of the form `x = v`")

data Judgment
  = ChkJ R.Chk
  | SynJ R.Syn
  | TypJ R.Typ

fromSynN :: Case -> R.Syn -> Judgment
fromSynN N j = SynJ j
fromSynN D j = SynJ $ R.Syn \e -> do
  Tuple res a <- R.runSyn j (e { elabFor = ForNominative })
  pure (Tuple (M.is res) a)

fromChkN :: Case -> R.Chk -> Judgment
fromChkN N j = ChkJ j
fromChkN D j = ChkJ $ R.Chk \e a -> do
  res <- R.runChk j (e { elabFor = ForNominative }) a
  pure (M.is res)

fromTypN :: Case -> R.Typ -> Judgment
fromTypN N j = TypJ j
fromTypN D j = TypJ $ R.Typ \e -> do
  res <- R.runTyp j (e { elabFor = ForNominative })
  pure (M.isTy res)

fromTypD :: Case -> Span -> R.Typ -> E Judgment
fromTypD N sp _ = failP sp 4 (P.text "cannot use descriptive syntax in nominal position")
fromTypD D _ j = pure (TypJ j)

variant :: Ntn -> E (Tuple Name (Array R.SmallTyp))
variant n = case NEA.uncons (getGroup n) of
  { head: NTag x _, tail: ts } -> do
    typs <- traverse smalltyp ts
    pure (Tuple x typs)
  _ -> unexpectedNotation n "variant in a sum type"

doBlock :: Span -> Array Ntn -> Maybe Ntn
doBlock sp stmts = case A.unsnoc stmts of
  Nothing -> Nothing
  Just { init, last } -> case last of
    Infix _ (Keyword "<-" _) _ -> Nothing
    _ -> Just (A.foldr step last init)
  where
  bindN e x rest = Juxt (Juxt (Ident "bind" sp) e) (Infix x (Keyword "=>" sp) rest)
  step s rest = case s of
    Infix x (Keyword "<-" _) e -> bindN e x rest
    e -> bindN e (Ident "_" sp) rest

doPat :: Ntn -> E (Tuple Name (Array Name))
doPat n = case NEA.uncons (getGroup n) of
  { head: NTag x _, tail: ns } -> do
    xs <- traverse patName ns
    pure (Tuple x xs)
  -- extension: `_ => e` handles every tag not listed
  { head: Ident "_" _, tail: [] } -> pure (Tuple "_" [])
  { head: Ident x sp, tail: [] } ->
    failP sp 3 (P.text ("`" <> x <> "` would be a new name for the whole value, which a `case` alternative cannot bind: `_` matches every other value, and the value is in hand already"))
  _ -> failP (ntnSpan n) 3 (P.text "a `case` pattern is flat: a constructor or a tag applied to names, or `_` -- an equation of a definition matches nested patterns")
  where
  patName (Ident x _) = pure x
  patName p = failP (ntnSpan p) 3 (P.text "an argument of a `case` pattern is a name: match a nested pattern in an equation of a definition, or bind a name here and `case` on it")

doCase :: Ntn -> E (Tuple Name (Tuple (Array Name) R.Chk))
doCase (Infix pat (Keyword "=>" _) bodyN) = do
  Tuple x xs <- doPat pat
  body <- chk N bodyN
  pure (Tuple x (Tuple xs body))
doCase n = unexpectedNotation n "case in a match statement"

specialization :: Ntn -> E (Tuple Name R.Chk)
specialization (NTuple [ Infix (NField x _) (Keyword ":=" _) n ] _) = do
  c <- chk N n
  pure (Tuple x c)
specialization n = unexpectedNotation n "specialization"

parseHead :: Case -> Ntn -> E Judgment
parseHead c n = case n of
  -- Variable rule
  Ident x sp -> pure (fromSynN c (R.find sp x))
  -- Type formers
  Infix domN (Keyword "->" _) codN -> do
    dom <- binder domN
    cod <- typ N codN
    pure (fromTypN c (R.functionFormation dom cod))
  NBlock "sig" Nothing fields sp -> do
    fieldDecls <- traverse fieldDeclaration fields
    fromTypD c sp (R.recordFormation fieldDecls)
  NBlock "sum" Nothing variantNs sp -> do
    variants <- traverse variant variantNs
    fromTypD c sp (R.sumFormation (Variants (dictFromList variants)))
  Keyword "Type" _ -> pure (fromTypN c (R.universeFormation SmallU))
  -- Introduction rules (except for tag)
  Infix (Ident x _) (Keyword "=>" _) bodyN -> do
    body <- chk c bodyN
    pure (ChkJ (R.functionIntro (ntnSpan n) x body))
  -- extension: `\(x :: T) -> e` annotates the binder; a body that
  -- synthesises makes the lambda synthesise, and say an argument type
  -- that nothing else has said yet; a body that can only be checked (a
  -- `case`, say) makes the lambda check, where the annotation need only
  -- agree with the argument type expected of it
  Infix (Infix (Ident x _) (Keyword ":" _) tyN) (Keyword "=>" _) bodyN -> do
    dom <- typ N tyN
    bj <- parse N bodyN
    case bj of
      SynJ body -> pure (fromSynN c (R.functionIntroSyn x dom body))
      ChkJ j ->
        let body = if implicitLambda bodyN then j else R.implicitLams j
        in pure (fromChkN c (R.functionIntroAnn (ntnSpan n) x dom body))
      TypJ j -> pure (fromChkN c (R.functionIntroAnn (ntnSpan n) x dom (R.universeIntro (ntnSpan bodyN) j)))
  -- extension: `{x} => e` names an implicit argument
  Infix (NBrace (Ident x _) _) (Keyword "=>" _) bodyN -> do
    body <- chk c bodyN
    pure (ChkJ (R.functionIntroImplicit (ntnSpan n) x body))
  NBlock "struct" Nothing fields sp -> do
    fieldSettings <- traverse (fieldSetting c) fields
    pure (ChkJ (R.recordIntro sp fieldSettings))
  -- Non-suffix elimination rules
  -- extension: `do` ... `end`, with whichever `bind` is in scope:
  -- `x <- e` then the rest is `bind e (x => rest)`, and `e` is `bind e (_ => rest)`
  NBlock "do" Nothing stmts sp -> case doBlock sp stmts of
    Just n' -> parse c n'
    Nothing -> failP sp 12 (P.text "a `do` block ends with an expression")
  NBlock "match" (Just scrutN) casesN sp -> do
    scrut <- syn N scrutN
    cases <- traverse doCase casesN
    let
      tags = A.mapMaybe (case _ of
        Infix p (Keyword "=>" _) _ | NTag x tsp <- NEA.head (getGroup p) -> Just (Tuple x tsp)
        _ -> Nothing) casesN
    pure (fromChkN c (R.sumElim sp scrut tags (dictFromList cases)))
  -- Misc.
  Keyword "abandon" sp -> pure (fromChkN c (R.abandonChk sp))
  Infix n0 (Keyword "~" _) n1 -> do
    Tuple x cc <- specialization n1
    s <- syn N n0
    pure (fromSynN c (R.recordSpec (ntnSpan n) s x cc))
  Infix n0 (Keyword ":" _) n1 -> do
    cc <- chk c n0
    t <- typ N n1
    pure (SynJ (R.annot t cc))
  Keyword "Int" _ -> pure (fromTypN c R.primInt)
  NInt i _ -> pure (fromSynN c (R.litInt i))
  -- extension: strings
  Keyword "Float" _ -> pure (fromTypN c R.primFloat)
  NFloat x _ -> pure (fromSynN c (R.litFloat x))
  Keyword "String" _ -> pure (fromTypN c R.primString)
  NString s _ -> pure (fromSynN c (R.litString s))
  _ -> failP (ntnSpan n) 3 (P.text "unexpected notation")

parse :: Case -> Ntn -> E Judgment
parse c n = case NEA.uncons (getGroup n) of
  { head: NTag x tsp, tail: ns } ->
    (\args -> fromChkN c (R.sumIntro (ntnSpan n) tsp x args)) <$> traverse (chk N) ns
  { head: Keyword "Array" _, tail: [ n0 ] } ->
    (\a -> fromTypN c (R.primArray a)) <$> typ N n0
  { head: Keyword "rec" _, tail: [ n0 ] } ->
    (\b -> fromChkN c (R.recChk (ntnSpan n) b)) <$> chk N n0
  { head: n0, tail: [] } -> parseHead c n0
  { head: n0, tail: ns } -> do
    s0 <- parseHead N n0 >>= intoSyn (ntnSpan n0)
    fromSynN c <$> foldM (elim (ntnSpan n0).start) s0 ns

-- | an application to `n`, from `start` (where the function is) to it
elim :: Int -> R.Syn -> Ntn -> E R.Syn
elim _ s (NField x sp) = pure (R.recordElim sp s x)
-- extension: `f (x := a)` (the new syntax) gives the implicit argument `x`
elim start s (NBrace (Infix (Ident x _) (Keyword ":=" _) n) sp) = R.functionElimNamed { start, end: sp.end } x s <$> chk N n
-- extension: `f {a}` gives an implicit argument
elim start s (NBrace n sp) = R.functionElimImplicit { start, end: sp.end } s <$> chk N n
elim start s n = R.functionElimC (checksOnly n) { start, end: (ntnSpan n).end } s <$> chk N n

-- | notation that can only be checked, never synthesised
checksOnly :: Ntn -> Boolean
checksOnly n = case NEA.head (getGroup n) of
  NTag _ _ -> true
  NBlock "struct" _ _ _ -> true
  NBlock "match" _ _ _ -> true
  NTuple _ _ -> true
  Keyword "abandon" _ -> true
  Infix _ (Keyword "=>" _) _ -> true
  _ -> false

implicitLambda :: Ntn -> Boolean
implicitLambda = case _ of
  Infix (NBrace _ _) (Keyword "=>" _) _ -> true
  _ -> false

intoSyn :: Span -> Judgment -> E R.Syn
intoSyn sp = case _ of
  ChkJ _ -> failP sp 0 (P.text "this notation must be put in a checking position")
  SynJ j -> pure j
  TypJ _ -> failP sp 0 (P.text "a type used as an element must be in checking position")

syn :: Case -> Ntn -> E R.Syn
syn c n = parse c n >>= intoSyn (ntnSpan n)

chk :: Case -> Ntn -> E R.Chk
chk c n = parse c n <#> case _ of
  -- (`{x} => e` binds the implicit argument itself)
  ChkJ j | not (implicitLambda n) -> R.implicitLams j
  ChkJ j -> j
  SynJ j -> R.implicitLams case NEA.head (getGroup n) of
    Ident x _ -> R.autoRec (ntnSpan n) x (R.apply (ntnSpan n) j)
    _ -> R.apply (ntnSpan n) j
  TypJ j -> R.universeIntro (ntnSpan n) j

typ :: Case -> Ntn -> E R.Typ
typ c n = parse c n <#> case _ of
  ChkJ j -> R.universeElim LargeU j
  SynJ j -> R.universeElimSyn (ntnSpan n) j
  TypJ j -> j

smalltyp :: Ntn -> E R.SmallTyp
smalltyp n = R.asSmall (ntnSpan n) <$> typ N n

definition :: Ntn -> E (Tuple Ntn Ntn)
definition (Infix n0 (Keyword ":=" _) n1) = pure (Tuple n0 n1)
definition n = unexpectedNotation n "notation of the form `<head> := <body>`"

annotN :: Ntn -> E (Tuple Ntn Ntn)
annotN (Infix n0 (Keyword ":" _) n1) = pure (Tuple n0 n1)
annotN n = unexpectedNotation n "type-annotated expression, e.g. `<pattern> : <type>`"

ident :: Ntn -> E Name
ident (Ident x _) = pure x
ident n = unexpectedNotation n "identifier"

type ArgBinding = { span :: Span, name :: Name, typ :: R.Typ, implicit :: Boolean }

argBinding :: Ntn -> E ArgBinding
argBinding n@(Infix n0 (Keyword ":" _) n1) = do
  x <- ident n0
  a <- typ N n1
  pure { span: ntnSpan n, name: x, typ: a, implicit: false }
-- extension: `def f {A : Type} ...`
argBinding (NBrace (Infix n0 (Keyword ":" _) n1) sp) = do
  x <- ident n0
  a <- typ N n1
  pure { span: sp, name: x, typ: a, implicit: true }
argBinding n = unexpectedNotation n "argument binding of the form `<name> : <type>`"

unpackArgs :: Ntn -> E (Tuple Name (Array ArgBinding))
unpackArgs n = do
  let g = NEA.uncons (getGroup n)
  x <- ident g.head
  args <- traverse argBinding g.tail
  pure (Tuple x args)

withArgs :: Array ArgBinding -> Tuple R.Typ R.Chk -> Tuple R.Typ R.Chk
withArgs args base = A.foldr go base args
  where
  go ab (Tuple t c)
    | ab.implicit = Tuple (R.functionFormation (R.NamedImplicit ab.span ab.name ab.typ) t) (R.functionIntroImplicit ab.span ab.name c)
    | otherwise = Tuple (R.functionFormation (R.Named ab.name ab.typ) t) (R.functionIntro ab.span ab.name c)

type DeclJ = { name :: Name, ty :: R.Typ, tm :: R.Chk, prim :: Boolean }

theory :: Ntn -> E DeclJ
theory n = do
  Tuple patN bodyN <- definition n
  Tuple name args <- unpackArgs patN
  body <- chk D bodyN
  let Tuple ty tm = withArgs args (Tuple (R.Typ \_ -> pure (M.univ LargeU)) body)
  pure { name, ty, tm, prim: false }

def :: Ntn -> E DeclJ
-- extension: a definition by equations, `def f : T where <equations> end`
def (Infix headN (Keyword ":" _) tyW)
  | Just (Tuple tyN eqs) <- stripWhere tyW = do
      Tuple name args <- unpackArgs headN
      let params = map _.name (A.filter (not <<< _.implicit) args)
      bodyN <- case desugar name params eqs (ntnSpan tyW) of
        Left err -> failP err.span 10 (P.text err.msg)
        Right b -> pure b
      returnTyp <- typ N tyN
      body <- chk D bodyN
      let Tuple ty tm = withArgs args (Tuple returnTyp body)
      pure { name, ty, tm, prim: false }
def n = do
  Tuple headN bodyN <- definition n
  Tuple patN tyN <- annotN headN
  Tuple name args <- unpackArgs patN
  returnTyp <- typ N tyN
  body <- chk D bodyN
  let Tuple ty tm = withArgs args (Tuple returnTyp body)
  pure { name, ty, tm, prim: false }

declJ :: Ntn -> E DeclJ
declJ = case _ of
  MDecl [] "theory" n _ -> theory n
  MDecl [] "def" n _ -> def n
  -- extension: `primitive name : T`, implemented by the runtime
  MDecl [] "primitive" (Infix (Ident x _) (Keyword ":" _) tyN) _ -> do
    ty <- typ N tyN
    pure { name: x, ty, tm: R.Chk \_ _ -> pure { stx: Syn.Prim x, val: pure (V.Nom V.Opaque) }, prim: true }
  n -> unexpectedNotation n "top-level declaration"

-- | Elaborate one declaration.  The definition may mention itself, as local 0
-- | in delayed mode; its value is the knot `v = reflect x (eval [v] body)`.
-- | `key` is the name it is stored under (its module's, qualified); a
-- | primitive keeps its own, the runtime's.
-- |
-- | A definition whose type elaborates and whose body does not is a
-- | definition still, `failed`: of its type, opaque to what uses it (as a
-- | primitive is), and abandoning if it is ever computed -- so what uses it
-- | is checked against its type, not refused with it.
elabDecl :: (Name -> Name) -> Globals -> Ntn -> E { name :: Name, key :: Name, prim :: Boolean, entry :: GlobalEntry, failed :: Maybe Diag }
elabDecl key g n = do
  modify_ \st -> st { selfKey = Nothing }
  d <- declJ n
  a <- R.runTyp d.ty (emptyElabEnv g)
  let
    aval = M.tyN a
    k = if d.prim then d.name else key d.name
    -- the definition mentions itself as the global it will be, not as a
    -- local of another name (the two would not convert); pattern
    -- unification knows it by its key, and passes it through -- so a
    -- solution may mention it, and the global is there, as it is now:
    -- not yet defined
    self = V.reflect (V.Global k) (pure V.Id) (pure aval) Nothing
    gSelf = addGlobalEntry k { syntax: Syn.Opaque, tySyntax: a.stx, value: self, type_: aval } g
    tmE = emptyElabEnvFor gSelf k
    tmE' = tmE { scope = letAt (BDelayedBy 1) d.name self aval tmE.scope, self = Just k }
  modify_ \st -> st { selfKey = Just k }
  body <- try do
    t <- R.runChk d.tm tmE' aval
    -- the implicit arguments: every one inferred, and substituted in
    stuck <- settle gSelf
    for_ (A.head stuck) \p -> stuckDiag gSelf (ntnSpan n) p >>= throwError
    unsolved <- unsolvedMetas
    for_ (A.head unsolved) \u -> throwError
      { code: 328, severity: SError, span: u.span, note: Nothing
      , msg: P.text ("cannot infer the implicit argument `" <> u.name <> "`; give it as `{...}`")
      }
    pure t
  modify_ \st -> st { selfKey = Nothing }
  mc <- gets _.metas
  let
    tyStx = zonkTy mc a.stx
  when (d.prim && not (isPrim d.name)) $ throwError
    { code: 329, severity: SError, span: ntnSpan n, note: Nothing
    , msg: P.text ("there is no primitive called `" <> d.name <> "` (the runtime has: " <> Str.joinWith ", " primNames <> ")")
    }
  pure case body of
    -- (as a primitive is: nothing to unfold, a neutral)
    Left failed ->
      { name: d.name, key: k, prim: d.prim, failed: Just failed, entry: entryFor g k true tyStx Syn.Abandon }
    Right t ->
      let
        -- (the global it was mentioned as, while it was not yet one: the
        -- definition itself, local 0)
        bodyStx = selfAsLocal k (zonkEl mc t.stx)
      in
        { name: d.name, key: k, prim: d.prim, failed: Nothing, entry: entryFor g k d.prim tyStx bodyStx }

-- | the entry a definition is stored as, from its core syntax alone: the
-- | type and the body, evaluated.  Elaboration ends here, and a definition
-- | thawed from a cache comes back through here too: the syntax is all
-- | either keeps.
entryFor :: Globals -> Name -> Boolean -> Syn.Ty -> Syn.El -> GlobalEntry
entryFor g k prim tyStx bodyStx =
  let
    aval = V.nom (evalTy { local: V.LNil, global: g } N tyStx)
    vL :: Lazy V.El
    vL = fix \self -> defer \_ ->
      let fixed = eval { local: V.LSnoc V.LNil self, global: g } D bodyStx
      in V.reflect (V.Global k) (pure V.Id) (pure aval) (Just fixed)
    -- a primitive has no definition to unfold: it is a neutral
    value = if prim then V.reflect (V.Global k) (pure V.Id) (pure aval) Nothing else force vL
  in { syntax: bodyStx, tySyntax: tyStx, value, type_: aval }

-- | a constraint unification could not solve, said where the implicit
-- | argument it is about was to be inferred (else where the declaration is)
stuckDiag :: Globals -> Span -> { shape :: CtxShape, left :: V.El, right :: V.El, failed :: Boolean } -> E Diag
stuckDiag g fallback p = do
  mc <- gets _.metas
  let
    shown v = P.render (prtElR (displayName g) p.shape (V.forceEl mc v))
    known = shown p.left <> " = " <> shown p.right
    about = A.head (A.nub (metasIn mc p.left <> metasIn mc p.right)) >>= \m -> Map.lookup m mc
  -- a problem that was retried and found contradictory: not missing
  -- information, a mismatch -- say so, where the implicit was asked for
  if p.failed then pure
    { code: 311, severity: SError
    , span: fromMaybe fallback (map _.span about)
    , note: map (\me -> P.text ("found for `" <> me.name <> "`, it would need " <> known)) about
    , msg: P.text ("the implicit arguments found do not fit together: " <> shown p.left <> " and " <> shown p.right <> " are not equal")
    }
  else pure case about of
    Just me ->
      { code: 327, severity: SError, span: me.span
      , note: Just (P.text ("what is known of it, " <> known <> ", does not say what it is"))
      , msg: P.text ("cannot infer the implicit argument `" <> me.name <> "`: give it, `(" <> me.name <> " := ...)`")
      }
    Nothing ->
      { code: 327, severity: SError, span: fallback, note: Nothing
      , msg: P.text ("cannot infer an implicit argument: " <> known <> " does not say what it is")
      }

-- | the metavariables not yet solved in a value: at its head, or in its
-- | spine's arguments
metasIn :: V.MetaCtx -> V.El -> Array Int
metasIn mc v = case V.forceEl mc v of
  V.Neu n -> (case n.head of
    V.Meta m -> [ m ]
    _ -> []) <> spine n.spine
  _ -> []
  where
  spine = case _ of
    V.Id -> []
    V.App sp e -> spine sp <> metasIn mc e
    V.Proj sp _ -> spine sp
    V.Specialize sp _ e -> spine sp <> metasIn mc e

-- | extension: `> e`, a watch -- an expression to evaluate and show, as in
-- | a playground; it is elaborated like a definition body but never becomes
-- | a global
elabWatch :: Globals -> Ntn -> E GlobalEntry
elabWatch g n = do
  j <- parse N n
  s <- case j of
    SynJ s -> pure s
    _ -> failP (ntnSpan n) 0 (P.text "a watch must say what type it has: write `-- >>> (e : T)`")
  Tuple t ty <- R.runSyn s (emptyElabEnv g)
  stuck <- settle g
  for_ (A.head stuck) \p -> stuckDiag g (ntnSpan n) p >>= throwError
  unsolved <- unsolvedMetas
  for_ (A.head unsolved) \u -> throwError
    { code: 328, severity: SError, span: u.span, note: Nothing
    , msg: P.text ("cannot infer the implicit argument `" <> u.name <> "`; give it as `{...}`")
    }
  mc <- gets _.metas
  let
    syntax = zonkEl mc t.stx
    tySyntax = zonkTy mc (readbTy 0 (V.forceTy mc ty))
    type_ = V.nom (evalTy { local: V.LNil, global: g } N tySyntax)
  pure { syntax, tySyntax, value: V.nom (eval { local: V.LNil, global: g } N syntax), type_ }

type Watch = { span :: Span, entry :: GlobalEntry }

-- | `spans` says where each successfully elaborated declaration is, `keys`
-- | what each is stored under; `broken`: the declarations that did not
-- | elaborate, not even their types (where each is)
type TopResult = { globals :: Globals, diags :: Array Diag, spans :: Array (Tuple Name Span), infos :: Array Info, trace :: Array TraceEvent, watches :: Array Watch, tests :: Array { name :: Name, span :: Span }, keys :: Array Name, broken :: Array (Tuple Name Span) }

-- | elaborate on top of some existing globals (what a file imports)
topWith :: Globals -> Array Ntn -> TopResult
topWith = topModule identity

-- | the same, for a module: each definition `x` is stored under `key x`,
-- | and `x` is an alias of it
topModule :: (Name -> Name) -> Globals -> Array Ntn -> TopResult
topModule key g0 = foldl tryDecl { globals: g0, diags: [], spans: [], infos: [], trace: [], watches: [], tests: [], keys: [], broken: [] }
  where
  -- extension: `test name`, the watches after it (with expectations) are a
  -- test, stored as a definition of its own
  tryDecl acc (MDecl [] "test" (Ident x _) sp) = acc { tests = A.snoc acc.tests { name: x, span: sp } }
  tryDecl acc (MDecl [] "test" n _) = acc { diags = A.snoc acc.diags { code: 211, severity: SError, span: ntnSpan n, note: Nothing, msg: P.text "a test is named: `test <name>`" } }
  tryDecl acc (MDecl [] ">" e sp) = case tryPure (\_ -> runE (elabWatch acc.globals e) st0) of
    Left msg -> acc { diags = A.snoc acc.diags { code: 399, severity: SError, span: sp, note: Nothing, msg: P.text ("internal error while elaborating this: " <> msg) } }
    Right (Tuple (Right entry) st) -> acc
      { watches = A.snoc acc.watches { span: sp, entry }
      , infos = acc.infos <> st.infos
      , trace = acc.trace <> st.trace
      }
    Right (Tuple (Left d) st) -> acc { diags = acc.diags <> reported d, infos = acc.infos <> st.infos, trace = acc.trace <> st.trace }
  -- an internal error in one declaration is reported, not fatal
  tryDecl acc n = case tryPure (\_ -> runE (elabDecl key acc.globals n) st0) of
    Left msg -> acc { diags = A.snoc acc.diags { code: 399, severity: SError, span: ntnSpan n, note: Nothing, msg: P.text ("internal error while elaborating this: " <> msg) } }
    -- (a primitive declared again, by another module, is the same one)
    Right (Tuple (Right d) st) | d.prim && isJust (lookupGlobal acc.globals d.key) -> acc
      { spans = A.snoc acc.spans (Tuple d.name (ntnSpan n))
      , keys = A.snoc acc.keys d.key
      , infos = acc.infos <> st.infos
      , trace = acc.trace <> st.trace
      }
    Right (Tuple (Right d) st) -> acc
      { globals = (if d.key == d.name then identity else addAlias d.name d.key) (addGlobalEntry d.key d.entry acc.globals)
      , diags = acc.diags <> maybe [] reported d.failed
      , spans = A.snoc acc.spans (Tuple d.name (ntnSpan n))
      , keys = A.snoc acc.keys d.key
      , infos = acc.infos <> st.infos
      , trace = acc.trace <> st.trace
      }
    -- (not even its type: what uses it is not checked, and not refused)
    Right (Tuple (Left d) st) -> case declName n of
      Just x -> acc
        { globals = addBroken (key x) (if key x == x then acc.globals else addAlias x (key x) acc.globals)
        , diags = acc.diags <> reported d
        , broken = A.snoc acc.broken (Tuple x (ntnSpan n))
        , infos = acc.infos <> st.infos
        , trace = acc.trace <> st.trace
        }
      Nothing -> acc { diags = acc.diags <> reported d, infos = acc.infos <> st.infos, trace = acc.trace <> st.trace }
  st0 = emptySt
  declName n = case runE (declJ n) st0 of
    Tuple (Right d) _ -> Just d.name
    _ -> Nothing
  -- (a use of a definition that did not elaborate is that definition's
  -- error, reported there)
  reported d = if reportedElsewhere d then [] else [ d ]

-- | a definition as a cache keeps it: its key, where it is, and its core
-- | syntax (type and body)
type ThawedDef = { name :: Name, key :: Name, span :: Span, ty :: Syn.Ty, body :: Syn.El }

-- | a module thawed from a cache: its definitions' syntax evaluated back
-- | into globals -- `topModule` with the elaboration already done
topThawed :: Globals -> Array ThawedDef -> TopResult
topThawed g0 = foldl one { globals: g0, diags: [], spans: [], infos: [], trace: [], watches: [], tests: [], keys: [], broken: [] }
  where
  one acc d =
    let
      keep = acc { spans = A.snoc acc.spans (Tuple d.name d.span), keys = A.snoc acc.keys d.key }
      prim = isForeign d.body
    in
      -- (a primitive declared again, by another module, is the same one)
      if prim && isJust (lookupGlobal acc.globals d.key) then keep
      else keep
        { globals = (if d.key == d.name then identity else addAlias d.name d.key)
            (addGlobalEntry d.key (entryFor acc.globals d.key prim d.ty d.body) acc.globals)
        }
  isForeign = case _ of
    Syn.Prim _ -> true
    Syn.Is e -> isForeign e
    _ -> false
