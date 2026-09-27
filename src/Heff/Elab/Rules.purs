-- | A port of `Hippogriff.Elaborator.Judgments` and
-- | `Hippogriff.Elaborator.Rules.*`, one section per reference module.
-- | Elaboration either succeeds or fails with the one diagnostic that made it
-- | give up, so `IO` + `GiveUp` becomes `Either Diag`.
module Heff.Elab.Rules where

import Heff.Support.BigInt (BigInt)
import Heff.Print.Surface (Implicits, printTy)
import Prelude

import Data.Array as A
import Data.Either (Either(..))
import Data.Lazy (defer, force)
import Data.List ((:))
import Data.Foldable (for_)
import Data.Maybe (Maybe(..), isJust)
import Data.String as S
import Data.Traversable (sequence, traverse)
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Dict, Name, dictContains, dictFromList, dictLength, dictLookup, dictToList)
import Heff.Core.Conversion (prettyDefEqError)
import Heff.Elab.Unify (forceT, freshMeta, isFlexTy, runDelayed, solveHole, unifyEl, unifyTy, zonkTy)
import Heff.Core.Readback (readbTy)
import Control.Monad.State (gets, modify_)
import Data.Maybe (fromMaybe)
import Heff.Core.Globals (lookupGlobalKey)
import Heff.Core.Memoed as M
import Heff.Core.Params (Case(..), Level(..), Universe(..), Variants(..), decodesInto, leq, maxLevel)
import Heff.Core.Print (prtElR, prtTyR)
import Heff.Core.Rename (displayName, renameTy)
import Heff.Core.Syntax as S
import Heff.Core.Value as V
import Heff.Elab.Environment as Env
import Heff.Elab.Environment (BindingMode(..), Ctx(..), E, record, traceE, ElabEnv, ElabFor(..), appElabFor, caseOf, createNominative, evalEnv, failWith, failWithNote, multiBind, projElabFor, shape, undelay, withBound)
import Heff.Parse.Notation (Span)
import Heff.Support.Pretty (Doc, (<+>))
import Heff.Support.Pretty as P

-- Judgments
--------------------------------------------------------------------------------

newtype Typ = Typ (ElabEnv -> E M.Ty)

newtype SmallTyp = SmallTyp (ElabEnv -> E M.Ty)

newtype Syn = Syn (ElabEnv -> E (Tuple M.El V.Ty))

newtype Chk = Chk (ElabEnv -> V.Ty -> E M.El)

runTyp :: Typ -> ElabEnv -> E M.Ty
runTyp (Typ f) = f

runSmall :: SmallTyp -> ElabEnv -> E M.Ty
runSmall (SmallTyp f) = f

runSyn :: Syn -> ElabEnv -> E (Tuple M.El V.Ty)
runSyn (Syn f) = f

runChk :: Chk -> ElabEnv -> V.Ty -> E M.El
runChk (Chk f) = f

txt :: String -> Doc
txt = P.text

-- | a type printed in the scope of the environment
prtIn :: ElabEnv -> V.Ty -> Doc
prtIn e t = txt (printTy surfaceIm 0 e.scope.names (renameTy (displayName e.globals) (readbTy e.scope.len t)))

-- | messages leave implicit arguments out
surfaceIm :: Implicits
surfaceIm = { names: \_ _ -> [], hide: true }

-- | a type in the syntax of the messages
prtSyn :: ElabEnv -> S.Ty -> Doc
prtSyn e t = txt (printTy surfaceIm 0 e.scope.names t)

-- | a memoized type printed by its syntax (as `Readback` does for `Memoed`)
prtInM :: ElabEnv -> M.Ty -> Doc
prtInM e m = prtSyn e (renameTy (displayName e.globals) m.stx)

-- Function
--------------------------------------------------------------------------------

data Binder = Anonymous Typ | Named Name Typ | NamedImplicit Span Name Typ

-- | a type printed in the scope of the environment, with the current
-- | metacontext's solutions substituted in
prtInZ :: ElabEnv -> V.Ty -> E Doc
prtInZ e t = do
  mc <- gets _.metas
  pure (prtSyn e (renameTy (displayName e.globals) (zonkTy mc (readbTy e.scope.len t))))

zonked :: V.MetaCtx -> ElabEnv -> V.Ty -> Doc
zonked mc e t = prtSyn e (renameTy (displayName e.globals) (zonkTy mc (readbTy e.scope.len t)))

isImplicitPi :: V.Ty -> Boolean
isImplicitPi t = case V.behavior t of
  V.LikeFunction ft -> ft.implicit
  _ -> false

-- | Apply `m : ty` to a fresh metavariable for each implicit argument it
-- | takes first.
insertImplicits :: ElabEnv -> Span -> M.El -> V.Ty -> E (Tuple M.El V.Ty)
insertImplicits e sp m ty0 = do
  ty <- forceT ty0
  case V.behavior ty of
    V.LikeFunction ft | ft.implicit -> do
      meta <- freshMeta e.scope.len e.scope.locals ft.dom (fromMaybe "_" (V.cloName ft.cod)) sp
      let arg = { stx: meta.stx, val: pure (V.Nom meta.val) }
      insertImplicits e sp (M.iapp (fromMaybe "_" (V.cloName ft.cod)) m arg) (V.nom (V.appClo ft.cod (pure meta.val)))
    _ -> pure (Tuple m ty)

functionFormation :: Binder -> Typ -> Typ
functionFormation (Anonymous dom) cod = Typ \e -> do
  edom <- runTyp dom e
  ecod <- runTyp cod e
  pure (M.function (evalEnv e) edom (S.AbsConst ecod))
functionFormation (Named x dom) cod = Typ \e -> do
  edom <- runTyp dom e
  ecod <- runTyp cod (e { scope = Env.bind x (M.tyN edom) e.scope })
  pure (M.function (evalEnv e) edom (S.Abs x ecod))
-- extension: an implicit argument, inferred at each use.  Its type must be
-- large: a value of a small type is statically irrelevant, so nothing could
-- ever determine it.
functionFormation (NamedImplicit sp x dom) cod = Typ \e -> do
  edom <- runTyp dom e
  when (leq (M.levelOf edom) Small) $
    failWith sp 24 (txt "an implicit argument must have a large type (a small one could never be inferred: its values are statically irrelevant)")
  ecod <- runTyp cod (e { scope = Env.bind x (M.tyN edom) e.scope })
  pure (M.functionI true (evalEnv e) edom (S.Abs x ecod))

-- | extension: checked at an implicit function type, anything is under the
-- | implicit lambdas it needs (not only a lambda)
implicitLams :: Chk -> Chk
implicitLams c = Chk \e a0 -> do
  a <- forceT a0
  case V.behavior a of
    V.LikeFunction ft | ft.implicit -> do
      let y = fromMaybe "_" (V.cloName ft.cod)
      ebody <- withBound y ft.dom e.scope \v scope' ->
        runChk (implicitLams c)
          (e { scope = scope', elabFor = appElabFor e.elabFor v })
          (V.nom (V.appClo ft.cod (pure v)))
      pure (M.lam (caseOf e.elabFor) (evalEnv e) (M.fromVTy N e.scope.len a) (S.Abs y ebody))
    _ -> runChk c e a

functionIntro :: Span -> Name -> Chk -> Chk
functionIntro sp x body = Chk \e a0 -> do
  a <- forceT a0
  case V.behavior a of
    -- a lambda checked at an implicit function type: the implicit argument
    -- is bound silently, and the lambda is for the next argument
    V.LikeFunction ft | ft.implicit -> do
      let y = fromMaybe "_" (V.cloName ft.cod)
      ebody <- withBound y ft.dom e.scope \v scope' ->
        runChk (functionIntro sp x body)
          (e { scope = scope', elabFor = appElabFor e.elabFor v })
          (V.nom (V.appClo ft.cod (pure v)))
      pure (M.lam (caseOf e.elabFor) (evalEnv e) (M.fromVTy N e.scope.len a) (S.Abs y ebody))
    V.LikeFunction ft -> do
      ebody <- withBound x ft.dom e.scope \v scope' ->
        runChk body
          (e { scope = scope', elabFor = appElabFor e.elabFor v })
          (V.nom (V.appClo ft.cod (pure v)))
      pure (M.lam (caseOf e.elabFor) (evalEnv e) (M.fromVTy N e.scope.len a) (S.Abs x ebody))
    _ -> failWith sp 1 (txt "tried to check a lambda expression at a non-function type")

-- | `{x} => e`: naming an implicit argument
functionIntroImplicit :: Span -> Name -> Chk -> Chk
functionIntroImplicit sp x body = Chk \e a0 -> do
  a <- forceT a0
  case V.behavior a of
    V.LikeFunction ft | ft.implicit -> do
      ebody <- withBound x ft.dom e.scope \v scope' ->
        runChk body
          (e { scope = scope', elabFor = appElabFor e.elabFor v })
          (V.nom (V.appClo ft.cod (pure v)))
      pure (M.lam (caseOf e.elabFor) (evalEnv e) (M.fromVTy N e.scope.len a) (S.Abs x ebody))
    _ -> failWith sp 25 (txt "an implicit lambda `{x} => ...` checked at a type that takes no implicit argument")

-- | `f {a}`: giving an implicit argument explicitly
functionElimImplicit :: Span -> Syn -> Chk -> Syn
functionElimImplicit sp callee arg = Syn \e -> do
  Tuple ecallee a0 <- runSyn callee e
  a <- forceT a0
  case V.behavior a of
    V.LikeFunction ft | ft.implicit -> do
      earg <- runChk arg e ft.dom
      pure (Tuple (M.iapp (fromMaybe "_" (V.cloName ft.cod)) ecallee earg) (V.nom (V.appClo ft.cod (defer \_ -> M.valN earg))))
    _ -> failWith sp 26 (txt "`{...}` gives an implicit argument, but this takes none here")

-- | extension: `f (x := a)` gives the implicit argument named `x`; the
-- | implicit ones before it are inferred
functionElimNamed :: Span -> Name -> Syn -> Chk -> Syn
functionElimNamed sp x callee arg = Syn \e -> do
  Tuple ecallee a0 <- runSyn callee e
  go e ecallee a0
  where
  go e m ty0 = do
    ty <- forceT ty0
    case V.behavior ty of
      V.LikeFunction ft | ft.implicit && V.cloName ft.cod == Just x -> do
        earg <- runChk arg e ft.dom
        pure (Tuple (M.iapp x m earg) (V.nom (V.appClo ft.cod (defer \_ -> M.valN earg))))
      V.LikeFunction ft | ft.implicit -> do
        meta <- freshMeta e.scope.len e.scope.locals ft.dom (fromMaybe "_" (V.cloName ft.cod)) sp
        go e (M.iapp (fromMaybe "_" (V.cloName ft.cod)) m { stx: meta.stx, val: pure (V.Nom meta.val) }) (V.nom (V.appClo ft.cod (pure meta.val)))
      _ -> failWith sp 29 (txt ("`(" <> x <> " := ...)` names an implicit argument, but there is none named " <> x <> " here"))

functionElim :: Span -> Syn -> Chk -> Syn
functionElim sp callee arg = functionElimC false sp callee arg

-- | Application.  An argument that can only be checked (a tag, a lambda, a
-- | struct, ...) at a type that is still an unsolved implicit waits: a hole
-- | stands for it, and it is checked once the application's type has been
-- | unified with what is expected (see `apply`).
functionElimC :: Boolean -> Span -> Syn -> Chk -> Syn
functionElimC checksOnly sp callee arg = Syn \e -> do
  Tuple ecallee0 a0 <- runSyn callee e
  Tuple ecallee a <- insertImplicits e sp ecallee0 a0
  case V.behavior a of
    V.LikeFunction ft | checksOnly -> do
      dom <- forceT ft.dom
      if not (isFlexTy dom) then plain e ecallee ft
      else do
        hole <- delayChk e sp "argument" arg dom
        pure (Tuple (M.app ecallee hole) (V.nom (V.appClo ft.cod (defer \_ -> M.valN hole))))
    V.LikeFunction ft -> plain e ecallee ft
    _ -> failWith sp 2 (txt "tried to apply a value that was not of a function type")
  where
  plain e ecallee ft = do
      earg <- runChk arg e ft.dom
      pure (Tuple (M.app ecallee earg) (V.nom (V.appClo ft.cod (defer \_ -> M.valN earg))))

-- | check `c` at `ty` later, once `ty` (flexible now) is known: a hole
-- | stands for the result until then
delayChk :: ElabEnv -> Span -> String -> Chk -> V.Ty -> E M.El
delayChk e sp what c ty = do
  hole <- freshMeta e.scope.len e.scope.locals ty what sp
  let
    m = case hole.stx of
      S.MetaApp k _ _ -> k
      _ -> -1
    check = do
      el <- runChk c e ty
      r <- solveHole e.globals (shape e.scope) m hole.val { stx: el.stx, val: M.valN el }
      case r of
        Right _ -> pure unit
        Left err -> conversionFail sp (txt ("this " <> what <> " does not fit how it is used")) (prettyDefEqError (displayName e.globals) err)
  modify_ \st -> st { delayed = A.snoc st.delayed (Env.Delayed { ty, check }) }
  traceE sp "delay" (defer \_ -> what <> " waits for its type ?" <> show m)
  pure { stx: hole.stx, val: pure (V.Nom hole.val) }

-- ModeShift
--------------------------------------------------------------------------------

conversionFail :: forall a. Span -> Doc -> Doc -> E a
conversionFail sp msg note = failWithNote sp 11 msg note

apply :: Span -> Syn -> Chk
apply sp syn = Chk \e a0 -> do
  Tuple m0 a1 <- runSyn syn e
  a <- forceT a0
  -- implicit arguments are inserted, unless an implicit function is wanted
  Tuple m a' <- if isImplicitPi a then pure (Tuple m0 a1) else insertImplicits e sp m0 a1
  r <- unifyTy e.globals (shape e.scope) a a'
  mc <- gets _.metas
  traceE sp "unify" (defer \_ -> P.render (zonked mc e a) <> "  =?=  " <> P.render (zonked mc e a') <> (case r of
    Right _ -> "   ok"
    Left _ -> "   FAILS"))
  case r of
    Right _ -> do
      runDelayed false
      pure m
    Left err -> do
      want <- prtInZ e a
      got <- prtInZ e a'
      let msg = txt "expected type" <+> want <> txt ", but got type" <+> got
      conversionFail sp msg (prettyDefEqError (displayName e.globals) err)

annot :: Typ -> Chk -> Syn
annot typ chk = Syn \e -> do
  a <- runTyp typ (e { elabFor = ForNominative })
  g <- runChk chk e (M.tyN a)
  pure (Tuple g (M.tyN a))

-- NonTerm
--------------------------------------------------------------------------------

recChk :: Span -> Chk -> Chk
recChk sp c = Chk \e a ->
  if leq (V.levelOf a) Small then M.rec <$> runChk c (e { scope = undelay e.scope }) a
  else failWith sp 20 (txt "can only use rec when checking at a small type")

-- | a call `f a ...` of a delayed variable `f` (a recursive call) is
-- | checked as `rec (f a ...)`
autoRec :: Span -> Name -> Chk -> Chk
autoRec sp x c = Chk \e a ->
  case findLocal e x of
    Left Delayed | leq (V.levelOf a) Small -> runChk (recChk sp c) e a
    _ -> runChk c e a

abandonChk :: Span -> Chk
abandonChk sp = Chk \_ a ->
  if leq (V.levelOf a) Small then pure M.abandon
  else failWith sp 21 (txt "can only use abandon when checking at a small type")

-- Prim
--------------------------------------------------------------------------------

primInt :: Typ
primInt = Typ \_ -> pure M.primInt

litInt :: BigInt -> Syn
litInt i = Syn \_ -> pure (Tuple (M.litInt i) V.PrimInt)

primString :: Typ
primString = Typ \_ -> pure M.primString

litString :: String -> Syn
litString s = Syn \_ -> pure (Tuple (M.litString s) V.PrimString)

-- Record
--------------------------------------------------------------------------------

type FieldDeclaration = { name :: Name, typ :: Typ }

recordFormation :: Array FieldDeclaration -> Typ
recordFormation fieldTyps = Typ \e -> do
  let
    go e' fs = case A.uncons fs of
      Nothing -> pure (Tuple Small [])
      Just { head: fd, tail: rest } -> do
        ty <- runTyp fd.typ e'
        Tuple l fieldTys <- go (e' { scope = Env.bind fd.name (M.tyN ty) e'.scope }) rest
        pure (Tuple (maxLevel l (M.levelOf ty)) (A.cons (Tuple fd.name ty.stx) fieldTys))
  Tuple l fields <- go (e { elabFor = ForNominative }) fieldTyps
  pure (M.record (evalEnv e) { level: l, fieldTypes: dictFromList fields })

-- | (`nameSpan`: the field's name, where it is written)
type FieldSetting = { name :: Name, chk :: Chk, span :: Span, nameSpan :: Span }

recordIntro :: Span -> Array FieldSetting -> Chk
recordIntro sp fieldSettings = Chk \e a0 -> do
  a <- forceT a0
  let
    go vs pairs = case A.uncons pairs of
      Nothing -> pure []
      Just { head: Tuple fs (Tuple x fieldC), tail: rest }
        | fs.name == x -> do
            let
              field = fieldC vs
              elabFor' = projElabFor e.elabFor x
            recordName e a fs.name fs.nameSpan field.ty
            m <- runChk fs.chk (e { elabFor = elabFor' }) field.ty
            let v = createNominative elabFor' (force m.val) field.ty
            case field.value of
              Nothing -> pure unit
              Just expected -> unifyEl e.globals (shape e.scope) v expected >>= case _ of
                Right _ -> pure unit
                Left err -> do
                  let msg = txt "expected value" <+> prtElR (displayName e.globals) (shape e.scope) expected <> txt ", but got value" <+> prtElR (displayName e.globals) (shape e.scope) v
                  conversionFail fs.span msg (prettyDefEqError (displayName e.globals) err)
            fields <- go (V.snoc vs v) rest
            pure (A.cons (Tuple x m) fields)
        | otherwise ->
            failWith fs.span 3 (txt "expected record field" <+> txt x <+> txt "got: " <+> txt fs.name)
  case V.behavior a of
    V.LikeRecord rt -> do
      let
        expectedLength = dictLength rt.fieldTypes
        givenLength = A.length fieldSettings
      when (expectedLength /= givenLength) $
        failWith sp 4 (txt "expected" <+> txt (show expectedLength) <+> txt "fields, got: " <+> txt (show givenLength))
      fields <- go rt.capture (A.zip fieldSettings (dictToList rt.fieldTypes))
      pure (M.cons (caseOf e.elabFor) (dictFromList fields))
    _ -> failWith sp 5 (txt "tried to check a record expression at a non-record type")

recordElim :: Span -> Syn -> Name -> Syn
recordElim sp projectee x = Syn \e -> do
  Tuple eprojectee a0 <- runSyn projectee e
  a <- forceT a0
  case V.behavior a of
    V.LikeRecord rt -> do
      when (not (dictContains rt.fieldTypes x)) $
        failWith sp 7 (txt "no such field" <+> txt x <+> txt "in type" <+> prtIn e a)
      let fieldTy = V.projTy a (M.valN eprojectee) x
      recordName e a x sp fieldTy
      pure (Tuple (M.proj eprojectee x) fieldTy)
    _ -> failWith sp 6 (txt "tried to project from a value that was not of a record type")

recordSpec :: Span -> Syn -> Name -> Chk -> Syn
recordSpec sp s x c = Syn \e -> do
  Tuple g a <- runSyn s e
  n <- case M.valN g of
    V.Neu n -> pure n
    _ -> failWith sp 22 (txt "can only specialize neutral")
  rt <- case force n.description of
    Just (V.Code (V.Record rt)) -> pure rt
    _ -> failWith sp 22 (txt "can only specialize type with record behavior")
  f <- if dictContains rt.fieldTypes x then do
      let d = V.expandRecord rt (V.Local e.scope.len) V.Id Nothing
      pure (V.typeForProjection rt x d)
    else failWith sp 22 (txt "no such field" <+> txt x)
  g' <- runChk c e f.ty
  pure (Tuple (M.specialize g x g') a)

-- Sum
--------------------------------------------------------------------------------

sumFormation :: Variants SmallTyp -> Typ
sumFormation (Variants vs) = Typ \e -> do
  vs' <- traverse (traverse (\t -> runSmall t (e { elabFor = ForNominative, scope = undelay e.scope }))) vs.values
  pure (M.sum (Variants (vs { values = vs' })))

-- | `sp`: the tag applied, `tsp`: the tag
sumIntro :: Span -> Span -> Name -> Array Chk -> Chk
sumIntro sp tsp x cs = Chk \e a0 -> do
  a <- forceT a0
  -- extension: at a type not known yet, the tag waits for it (once)
  if isFlexTy a then delayChk e sp "tag" (sumIntroNow sp tsp x cs) a else runChk (sumIntroNow sp tsp x cs) e a

sumIntroNow :: Span -> Span -> Name -> Array Chk -> Chk
sumIntroNow sp tsp x cs = Chk \e a0 -> do
  a <- forceT a0
  case V.behavior a of
    V.LikeSum (Variants vs) -> case dictLookup vs x of
      Just ts -> do
        recordName e a x tsp a
        tagArgs
        where
        tagArgs =
          if A.length ts == A.length cs then do
            args <- sequence (A.zipWith (\c t -> runChk c e (force t)) cs ts)
            pure (M.tag x args)
          else
            failWith sp 16 (txt "wrong number of arguments for tag" <+> txt x <+> txt "in type" <+> prtIn e a)
      Nothing -> failWith sp 15 (txt "no such tag" <+> txt x <+> txt "in type" <+> prtIn e a)
    _ | isFlexTy a -> failWith sp 14 (txt "cannot check tag syntax at a type that is not known: annotate it, as in `(" <> txt ("'" <> x) <> txt " ... : T)`")
    _ -> failWith sp 14 (txt "cannot check tag syntax at non-sum type")

-- | a tag or a field, where it is written, as a name of the type it is of
-- | (a sum's, a record's), defined where that type is (the type named by a
-- | global); `ty` the name's type.  Not one put there by a desugaring
-- | (`if`'s tags), whose span is not the name's
recordName :: ElabEnv -> V.Ty -> Name -> Span -> V.Ty -> E Unit
recordName e a x sp ty = case a of
  V.Decode dn | V.Global k <- dn.head, sp.end - sp.start == S.length x || sp.end - sp.start == S.length x + 1 ->
    record { span: sp, name: x, key: Just k, ty: defer \_ -> P.render (prtIn e ty) }
  _ -> pure unit

-- | `tags`: where the cases' tags are written
sumElim :: Span -> Syn -> Array (Tuple Name Span) -> Dict (Tuple (Array Name) Chk) -> Chk
sumElim sp scrutSyn tags casesChk = Chk \e b -> do
  when (not (leq (V.levelOf b) Small)) $
    failWith sp 23 (txt "cannot eliminate from sum type into large type")
  Tuple scrut a0 <- runSyn scrutSyn e
  a <- forceT a0
  vs <- case V.behavior a of
    V.LikeSum (Variants vs) -> pure vs
    _ -> failWith sp 18 (txt "cannot match against non-sum type" <+> prtIn e a)
  for_ tags \(Tuple x tsp) -> when (isJust (dictLookup vs x)) (recordName e a x tsp a)
  cases <- map dictFromList $ flip traverse (dictToList vs) \(Tuple x ts) ->
    case dictLookup casesChk x of
      Just (Tuple xs bodyChk) ->
        if A.length xs == A.length ts then do
          let e' = e { scope = multiBind xs (map force ts) e.scope }
          body <- runChk bodyChk e' b
          pure (Tuple x (Tuple xs body.stx))
        else failWith sp 16 (txt "wrong number of arguments provided for tag" <+> txt x)
      -- extension: a default case `_ => e` covers the tags not listed; it
      -- binds the tag's arguments to `_`, so it can be checked per tag
      Nothing -> case dictLookup casesChk "_" of
        Just (Tuple _ bodyChk) -> do
          let
            xs = map (const "_") ts
            e' = e { scope = multiBind xs (map force ts) e.scope }
          body <- runChk bodyChk e' b
          pure (Tuple x (Tuple xs body.stx))
        Nothing -> failWith sp 17 (txt "no case provided for tag" <+> txt x)
  pure (M.match scrut cases)

-- Universe
--------------------------------------------------------------------------------

universeFormation :: Universe -> Typ
universeFormation u = Typ \_ -> pure (M.univ u)

universeIntro :: Span -> Typ -> Chk
universeIntro sp typ = Chk \e a0 -> do
  a <- forceT a0
  ty <- runTyp typ e
  case V.behavior a of
    V.LikeU u ->
      if leq (M.levelOf ty) (decodesInto u) then pure (M.code ty)
      else failWith sp 9 (txt "type" <+> prtInM e ty <+> txt "too large for universe" <+> txt (show u))
    _ -> failWith sp 10 (txt "cannot check type" <+> prtInM e ty <+> txt "at non-universe type" <+> prtIn e a)

universeElim :: Universe -> Chk -> Typ
universeElim u chk = Typ \e -> do
  m <- runChk chk e (V.U u)
  pure (M.decode m)

universeElimSyn :: Span -> Syn -> Typ
universeElimSyn sp syn = Typ \e -> do
  Tuple m a <- runSyn syn e
  case a of
    V.U _ -> pure (M.decode m)
    _ -> failWith sp 10 (txt "cannot decode an element of non-universe type")

asSmall :: Span -> Typ -> SmallTyp
asSmall sp typ = SmallTyp \e -> do
  ty <- runTyp typ e
  if leq (M.levelOf ty) Small then pure ty
  else failWith sp 9 (txt "expected small type")

-- Variable
--------------------------------------------------------------------------------

data VarError = NotInScope | Delayed

findLocal :: ElabEnv -> Name -> Either VarError (Tuple M.El V.Ty)
findLocal e x = go e.scope.names e.scope.ctx e.scope.locals 0 0
  where
  go names ctx locals n i = case names, ctx, locals of
    _, CUndelay c', _ -> go names c' locals (n + 1) i
    (x' : xs), CBind c' (BDelayedBy k) a, V.LSnoc vs v
      | x == x' -> if n >= k then Right (Tuple (M.local i v) a) else Left Delayed
      | otherwise -> go xs c' vs n (i + 1)
    _, _, _ -> Left NotInScope

find :: Span -> Name -> Syn
find sp x = Syn \e ->
  case findLocal e x of
    Right res@(Tuple _ ty) -> do
      record { span: sp, name: x, key: Nothing, ty: defer \_ -> P.render (prtIn e ty) }
      pure res
    Left Delayed ->
      failWith sp 19 (txt "variable" <+> txt x <+> txt "is bound in delayed mode and not available for use")
    Left NotInScope -> case lookupGlobalKey e.globals x of
      Just (Tuple key ge) -> do
        record { span: sp, name: x, key: Just key, ty: defer \_ -> P.render (prtIn e ge.type_) }
        pure (Tuple (M.global key ge.value) ge.type_)
      Nothing -> failWith sp 8 (txt "no such variable" <+> txt x <+> txt "in scope")
