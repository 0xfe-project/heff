-- | Conversion that may solve metavariables: the rules of
-- | `Heff.Core.Conversion`, plus pattern unification with pruning, run in the
-- | elaboration monad so that the metacontext is that of the definition
-- | being elaborated.
-- |
-- | The phase distinction keeps this small: an implicit argument always has
-- | a large type (small values are statically irrelevant, so there is nothing
-- | to infer about them), so a metavariable stands for a type, a type
-- | constructor, or a module.
module Heff.Elab.Unify
  ( unifyTy
  , blockedTy
  , unifyEl
  , force
  , forceT
  , freshMeta
  , retryPostponed
  , settle
  , zonkEl
  , zonkTy
  , unsolvedMetas
  , runDelayed
  , solveHole
  , isFlexTy
  ) where

import Prelude

import Control.Monad.Except (ExceptT, catchError, runExceptT, throwError)
import Control.Monad.State (State, gets, modify_)
import Control.Monad.Trans.Class (lift)
import Data.Array as A
import Data.Either (Either(..))
import Data.Foldable (for_)
import Data.Lazy (force) as L
import Data.List (List(..), (:))
import Data.Map (Map)
import Data.Map as Map
import Data.Set as Set
import Data.Maybe (Maybe(..), fromMaybe, isNothing, maybe)
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Heff.Parse.Notation (Span)
import Heff.Support.Common (Name, dictMap, dictTraverse)
import Heff.Core.Conversion (DefEqCheckError(..))
import Heff.Core.Evaluation (eval, evalTy)
import Heff.Core.Globals (Globals)
import Heff.Core.Params (Case(..), CtxShape, Universe(..), Variants(..))
import Heff.Core.Readback (readbTy)
import Heff.Core.Print (prettyEl, prettyTy)
import Data.Lazy (defer)
import Heff.Core.Syntax as S
import Heff.Core.Value as V
import Heff.Elab.Environment (Delayed(..), E, ElabSt, tryPure)
import Heff.Support.Pretty as P

type U a = ExceptT DefEqCheckError (State ElabSt) a

liftU :: forall a. U a -> E (Either DefEqCheckError a)
liftU u = lift (runExceptT u)

force :: V.El -> E V.El
force v = do
  mc <- gets _.metas
  pure (V.forceEl mc v)

forceT :: V.Ty -> E V.Ty
forceT t = do
  mc <- gets _.metas
  pure (V.forceTy mc t)

forceU :: V.El -> U V.El
forceU v = do
  mc <- gets _.metas
  pure (V.forceEl mc v)

forceTU :: V.Ty -> U V.Ty
forceTU t = do
  mc <- gets _.metas
  pure (V.forceTy mc t)

-- Creating metavariables
--------------------------------------------------------------------------------

-- | A metavariable of type `ty`, applied to every local in scope (so its
-- | solution may mention them).  `len` locals are in scope, with values
-- | `locals`.
freshMeta :: Int -> V.Locals -> V.Ty -> Name -> Span -> E { stx :: S.El, val :: V.El }
freshMeta len locals ty name sp = do
  m <- gets _.nextMeta
  modify_ \s -> s
    { nextMeta = m + 1
    , metas = Map.insert m { ty, sol: Nothing, solStx: Nothing, arity: len, name, span: sp } s.metas
    }
  let
    idxs = A.reverse (A.range 0 (len - 1)) # (\xs -> if len == 0 then [] else xs)
    spine = A.foldl (\sp' i -> V.App sp' (V.localsAt locals i)) V.Id idxs
  mc <- gets _.metas
  modify_ \st -> st { trace = { span: sp, kind: "meta", text: defer \_ -> "?" <> show m <> " (" <> name <> ") : " <> showTy mc len ty } : st.trace }
  pure { stx: S.MetaApp m idxs (readbTy len ty), val: V.metaEl m spine ty }

-- | a type, as the metacontext `mc` has it
showTy :: V.MetaCtx -> Int -> V.Ty -> String
showTy mc len ty = P.render (prettyTy (namesFor len) (zonkTy mc (readbTy len ty)))

namesFor :: Int -> List Name
namesFor n = if n <= 0 then Nil else ("x" <> show (n - 1)) : namesFor (n - 1)

-- | a solution, for tracing: the body, over one binder per local
showSol :: V.MetaCtx -> S.El -> String
showSol mc stx = P.render (prettyEl Nil (zonkEl mc stx))

-- Unification
--------------------------------------------------------------------------------

unifyTy :: Globals -> CtxShape -> V.Ty -> V.Ty -> E (Either DefEqCheckError Unit)
unifyTy g cs a b = liftU (uTy g cs a b)

unifyEl :: Globals -> CtxShape -> V.El -> V.El -> E (Either DefEqCheckError Unit)
unifyEl g cs a b = liftU (uEl g cs a b)

flexOf :: V.El -> Maybe (Tuple Int V.Spine)
flexOf = case _ of
  V.Neu n -> case n.head of
    V.Meta m -> Just (Tuple m n.spine)
    _ -> Nothing
  _ -> Nothing

-- | a type that is a metavariable, as an element (so it can be solved)
flexTy :: V.Ty -> Maybe (Tuple Int V.Spine)
flexTy = case _ of
  V.Decode n -> case n.head of
    V.Meta m -> Just (Tuple m n.spine)
    _ -> Nothing
  _ -> Nothing

uTy :: Globals -> CtxShape -> V.Ty -> V.Ty -> U Unit
uTy g cs a0 b0 = do
  a <- forceTU a0
  b <- forceTU b0
  -- (an error hole is equal to anything: before a metavariable is solved
  -- with it)
  if V.isErrorTy a || V.isErrorTy b then poisonTy cs.len a *> poisonTy cs.len b else case flexTy a, flexTy b of
    Just (Tuple m sp), _ -> solve g cs m sp (V.code b) (V.code a)
    _, Just (Tuple m sp) -> solve g cs m sp (V.code a) (V.code b)
    _, _ -> case a, b of
      V.U u, V.U u' | u == u' -> pure unit
      V.Decode n, V.Decode n' -> uBare g cs (V.toBare n) (V.toBare n')
      V.Function f, V.Function f' -> do
        when (f.level /= f'.level || f.implicit /= f'.implicit) (throwError (UnequalTys cs a b Nothing))
        uTy g cs f.dom f'.dom
        let
          x = V.local cs.len f.dom
          cs' = { len: cs.len + 1, names: fromMaybe "x" (V.cloName f.cod) : cs.names }
        uTy g cs' (V.nom (V.appClo f.cod (pure x))) (V.nom (V.appClo f'.cod (pure x)))
      V.PrimInt, V.PrimInt -> pure unit
      V.PrimFloat, V.PrimFloat -> pure unit
      V.PrimString, V.PrimString -> pure unit
      V.PrimArray a', V.PrimArray b' -> uTy g cs a' b'
      _, _ -> throwError (UnequalTys cs a b Nothing)

uBare :: Globals -> CtxShape -> V.BareNeutral -> V.BareNeutral -> U Unit
uBare g cs n n' = if n.head == V.Error || n'.head == V.Error then pure unit else case n.spine, n'.spine of
  V.Id, V.Id -> if n.head == n'.head then pure unit else throwError (UnequalNeus cs n n' Nothing)
  V.App sq v, V.App sq' v' -> do
    uBare g cs (n { spine = sq }) (n' { spine = sq' })
    uEl g cs v v'
  V.Proj sq x, V.Proj sq' x' -> do
    uBare g cs (n { spine = sq }) (n' { spine = sq' })
    when (x /= x') (throwError (UnequalNeus cs n n' Nothing))
  V.Specialize sq x v, V.Specialize sq' x' v' -> do
    uBare g cs (n { spine = sq }) (n' { spine = sq' })
    when (x /= x') (throwError (UnequalNeus cs n n' Nothing))
    uEl g cs v v'
  _, _ -> throwError (UnequalNeus cs n n' Nothing)

canon :: V.El -> V.El
canon v = case v of
  V.Neu n -> case n.head of
    V.Meta _ -> v
    _ -> case V.behavior (L.force n.ty) of
      V.LikeRecord _ -> case L.force n.expansion of
        V.IntoCons fields -> V.Cons (dictMap V.Nom fields)
        V.NotApplicable -> v
      V.LikeFunction _ -> V.Lam n.ty (V.Clo "x" V.LNil (\w -> V.app v (pure (V.localsAt w 0))))
      _ -> v
  _ -> v

uEl :: Globals -> CtxShape -> V.El -> V.El -> U Unit
uEl g cs a0 b0 = do
  a <- forceU a0
  b <- forceU b0
  if V.isError a || V.isError b then poisonEl cs.len a *> poisonEl cs.len b else case a, b of
    V.Opaque, _ -> pure unit
    _, V.Opaque -> pure unit
    _, _ -> case flexOf a, flexOf b of
      Just (Tuple m sp), _ -> solve g cs m sp b a
      _, Just (Tuple m sp) -> solve g cs m sp a b
      _, _ -> case canon a, canon b of
        V.Neu n, V.Neu n' -> uBare g cs (V.toBare n) (V.toBare n')
        V.Code t, V.Code t' -> uTy g cs t t'
        V.Lam d c, V.Lam _ c' -> do
          let
            x = V.local cs.len (V.lamDom (L.force d))
            cs' = { len: cs.len + 1, names: fromMaybe "x" (V.cloName c) : cs.names }
          uEl g cs' (V.nom (V.appClo c (pure x))) (V.nom (V.appClo c' (pure x)))
        V.Cons d, V.Cons d' -> for_ (A.zip d.values d'.values) \(Tuple x y) -> uEl g cs (V.nom x) (V.nom y)
        _, _ -> throwError (UnequalEls cs a b Nothing)

-- | the metavariables an error hole met -- in what it was compared with,
-- | wherever they are in it -- not solved by it, and marked
poisonTy :: Int -> V.Ty -> U Unit
poisonTy len t = do
  mc <- gets _.metas
  poisonAll (\_ -> metasTy mc 20 len t)

poisonEl :: Int -> V.El -> U Unit
poisonEl len v = do
  mc <- gets _.metas
  poisonAll (\_ -> metasEl mc 20 len v)

-- (looking into a value computes what it had not yet: what cannot be
-- computed here -- the definition's own global, not stored yet, say -- is
-- not looked into)
poisonAll :: (Unit -> Array Int) -> U Unit
poisonAll ms = case tryPure ms of
  Right found -> modify_ \s -> s { poisoned = Set.union s.poisoned (Set.fromFoldable found) }
  Left _ -> pure unit

-- | the unsolved metavariables in a value, `depth` deep (under binders, a
-- | local put for each; a record type's fields are not looked into)
metasEl :: V.MetaCtx -> Int -> Int -> V.El -> Array Int
metasEl mc depth len v0
  | depth <= 0 = []
  | otherwise = case V.forceEl mc v0 of
      V.Neu n -> (case n.head of
        V.Meta m -> [ m ]
        _ -> []) <> spine n.spine
      V.Code t -> metasTy mc (depth - 1) len t
      V.Lam d c -> metasEl mc (depth - 1) (len + 1) (V.nom (V.appClo c (pure (V.local len (V.lamDom (L.force d))))))
      V.Cons d -> A.concatMap (\x -> metasEl mc (depth - 1) len (V.nom x)) d.values
      V.Opaque -> []
  where
  spine = case _ of
    V.Id -> []
    V.App sp a -> spine sp <> metasEl mc (depth - 1) len a
    V.Proj sp _ -> spine sp
    V.Specialize sp _ a -> spine sp <> metasEl mc (depth - 1) len a

metasTy :: V.MetaCtx -> Int -> Int -> V.Ty -> Array Int
metasTy mc depth len t0
  | depth <= 0 = []
  | otherwise = case V.forceTy mc t0 of
      V.Decode n -> metasEl mc depth len (L.force n.el)
      V.Function f -> metasTy mc (depth - 1) len f.dom <> metasTy mc (depth - 1) (len + 1) (V.nom (V.appClo f.cod (pure (V.local len f.dom))))
      V.PrimArray a -> metasTy mc (depth - 1) len a
      _ -> []

-- | is a type one nothing can be asked of, for an error elsewhere: an
-- | error hole's, or a metavariable an error hole kept from being solved?
blockedTy :: V.Ty -> E Boolean
blockedTy a0 = do
  a <- forceT a0
  ps <- gets _.poisoned
  pure (V.isErrorTy a || maybe false (\(Tuple m _) -> Set.member m ps) (flexTy a))

-- Solving
--------------------------------------------------------------------------------

-- | an argument of a flexible neutral, in a Miller pattern: a variable
-- | (its level), or the definition being elaborated itself -- let-bound
-- | to the global it will be, closed, so it needs no abstraction, and
-- | the lambda at its position goes unused
data PatArg = PVar Int | PSelf

derive instance eqPatArg :: Eq PatArg

-- | the arguments of a flexible neutral, if they are distinct variables
-- | (or the self) -- a Miller pattern
patternArgs :: Maybe Name -> V.MetaCtx -> V.Spine -> Maybe (Array PatArg)
patternArgs selfKey mc sp = do
  items <- go sp
  let
    vars = A.mapMaybe (case _ of
      PVar l -> Just l
      PSelf -> Nothing) items
  if A.length (A.nub vars) == A.length vars then Just items else Nothing
  where
  go = case _ of
    V.Id -> Just []
    V.App s v -> do
      rest <- go s
      case V.forceEl mc v of
        V.Neu { head: V.Local l, spine: V.Id } -> Just (A.snoc rest (PVar l))
        V.Neu { head: V.Global k, spine: V.Id } | Just k == selfKey -> Just (A.snoc rest PSelf)
        _ -> Nothing
    _ -> Nothing

-- | `?m sp := rhs`; `self` is the flexible side, kept for postponing
solve :: Globals -> CtxShape -> Int -> V.Spine -> V.El -> V.El -> U Unit
solve g cs m sp rhs self = do
  mc <- gets _.metas
  sk <- gets _.selfKey
  case patternArgs sk mc sp of
    Nothing -> postponeU cs self rhs
    Just args -> case flexOf rhs of
      -- `?m` met on both sides: with the same pattern it is nothing; with
      -- two DIFFERENT patterns it may depend only on the positions where
      -- the two agree (pattern intersection) -- never an occurs error
      Just (Tuple m' sp') | m' == m -> case patternArgs sk mc sp' of
        Just args' | args' == args -> pure unit
        Just args' | A.length args' == A.length args -> do
          entry <- case Map.lookup m mc of
            Just e -> pure e
            Nothing -> scopeError
          -- (as in pruning: only a closed type survives dropping arguments)
          tyStx <- renameTy g m { dom: entry.arity, cod: 0, ren: Map.empty } entry.ty
          let
            ty = V.nom (evalTy { local: V.LNil, global: g } N tyStx)
            n = A.length args
            kept = A.mapMaybe (\(Tuple i (Tuple a1 a2)) -> if a1 == a2 then Just i else Nothing)
              (A.mapWithIndex Tuple (A.zip args args'))
          new <- gets _.nextMeta
          modify_ \s -> s
            { nextMeta = new + 1
            , metas = Map.insert new { ty, sol: Nothing, solStx: Nothing, arity: A.length kept, name: entry.name, span: entry.span } s.metas
            }
          let
            idxs = map (\i -> n - i - 1) kept
            stx = lams n (S.MetaApp new idxs tyStx)
            sol = V.nom (eval { local: V.LNil, global: g } N stx)
          modify_ \s -> s { metas = Map.update (\e2 -> Just e2 { sol = Just sol, solStx = Just stx }) m s.metas }
          traceSolve m stx
        _ -> postponeU cs self rhs
      _ -> do
        let ren = { dom: cs.len, cod: A.length args, ren: Map.fromFoldable (A.mapMaybe (\(Tuple i a) -> case a of
              PVar l -> Just (Tuple l i)
              PSelf -> Nothing) (A.mapWithIndex Tuple args)) }
        -- (one mentioning an error hole is no solution: the metavariable is
        -- left as it is)
        renamed <- catchError (Just <$> rename g m ren rhs) case _ of
          MentionsError -> pure Nothing
          err -> throwError err
        for_ renamed \body -> do
          let
            k = A.length args
            stx = lams k body
            val = V.nom (eval { local: V.LNil, global: g } N stx)
          modify_ \s -> s { metas = Map.update (\e -> Just e { sol = Just val, solStx = Just stx }) m s.metas }
          -- (a metavariable an error hole met hands that on to those its
          -- solution has: they stand for it now)
          ps <- gets _.poisoned
          when (Set.member m ps) (poisonEl 0 val)
          traceSolve m stx

-- | record a solution in the trace, at the metavariable's span
traceSolve :: Int -> S.El -> U Unit
traceSolve m stx = do
  mc <- gets _.metas
  case Map.lookup m mc of
    Just e -> modify_ \st -> st { trace = { span: e.span, kind: "solve", text: defer \_ -> "?" <> show m <> " (" <> e.name <> ") := " <> showSol mc stx } : st.trace }
    Nothing -> pure unit

lams :: Int -> S.El -> S.El
lams k body = if k <= 0 then body else lams (k - 1) (S.Lam (S.U SmallU) (S.Abs "x" body))

postponeU :: CtxShape -> V.El -> V.El -> U Unit
postponeU cs l r = modify_ \s -> s { postponed = A.snoc s.postponed { shape: cs, left: l, right: r, failed: false } }

-- | a partial renaming: the levels of `dom` that may appear, and what they
-- | become in a context of `cod` levels
type PRen = { dom :: Int, cod :: Int, ren :: Map Int Int }

lift1 :: PRen -> PRen
lift1 r = { dom: r.dom + 1, cod: r.cod + 1, ren: Map.insert r.dom r.cod r.ren }

scopeError :: forall a. U a
scopeError = throwError (UnequalEls { len: 0, names: mempty } V.Opaque V.Opaque (Just (P.text "an implicit argument would mention a variable out of its scope")))

occursError :: forall a. U a
occursError = throwError (UnequalEls { len: 0, names: mempty } V.Opaque V.Opaque (Just (P.text "an implicit argument would have to contain itself")))

-- | read a value back into the solution's context, checking scope and
-- | occurrence, and pruning other metavariables' arguments that are out of
-- | scope
rename :: Globals -> Int -> PRen -> V.El -> U S.El
rename g m r v0 = do
  v <- forceU v0
  case v of
    V.Neu n -> case n.head of
      V.Meta m'
        | m' == m -> occursError
        | otherwise -> renameFlex g m r m' n.spine
      _ -> do
        h <- renameHead r n.head
        renameSpine g m r n.spine h
    V.Code t -> S.Code <$> renameTy g m r t
    V.Lam fty body -> do
      d <- renameTy g m r (L.force fty)
      b <- renameClo g m r (V.lamDom (L.force fty)) body
      pure (S.Lam d b)
    V.Cons d -> S.Cons <$> dictTraverse (renameEv g m r) d
    V.Opaque -> pure S.Opaque

renameEv :: Globals -> Int -> PRen -> V.Ev V.El -> U S.El
renameEv g m r = case _ of
  V.Nom v -> rename g m r v
  V.Describe v -> rename g m r v
  V.Become v -> S.Is <$> rename g m r v

renameHead :: PRen -> V.Head -> U S.El
renameHead r = case _ of
  V.Local l -> case Map.lookup l r.ren of
    Just l' -> pure (S.Local (r.cod - l' - 1))
    Nothing -> scopeError
  V.Global x -> pure (S.Global x)
  V.Meta _ -> scopeError
  V.Error -> throwError MentionsError

renameSpine :: Globals -> Int -> PRen -> V.Spine -> S.El -> U S.El
renameSpine g m r sp h = case sp of
  V.Id -> pure h
  V.App s a -> do
    f <- renameSpine g m r s h
    x <- rename g m r a
    pure (S.App f x)
  V.Proj s l -> (\f -> S.Proj f l) <$> renameSpine g m r s h
  V.Specialize s l a -> do
    f <- renameSpine g m r s h
    x <- rename g m r a
    pure (S.Specialize f l x)

-- | Another metavariable in the solution.  If some of its arguments are
-- | variables out of scope, it is pruned: replaced by a fresh metavariable
-- | that does not take them.
renameFlex :: Globals -> Int -> PRen -> Int -> V.Spine -> U S.El
renameFlex g m r m' sp = do
  mc <- gets _.metas
  sk <- gets _.selfKey
  case patternArgs sk mc sp of
    Just args | A.any (case _ of
        PVar l -> not (Map.member l r.ren)
        -- (a self in the spine is pruned too: the pruned solution can
        -- only apply the new metavariable to variables)
        PSelf -> true) args -> do
      entry <- case Map.lookup m' mc of
        Just e -> pure e
        Nothing -> scopeError
      -- only a metavariable whose type does not mention the pruned
      -- variables can be pruned; here, one whose type is closed (a universe,
      -- `Type -> Type`, a theory such as `HFunctor`): read back into the
      -- empty context, or a scope error
      tyStx <- renameTy g m { dom: entry.arity, cod: 0, ren: Map.empty } entry.ty
      let
        ty = V.nom (evalTy { local: V.LNil, global: g } N tyStx)
        inRen = case _ of
          PVar l -> Map.member l r.ren
          PSelf -> false
        keep = A.filter inRen args
        kept = A.mapMaybe (\(Tuple i a) -> if inRen a then Just i else Nothing) (A.mapWithIndex Tuple args)
        n = A.length args
      new <- gets _.nextMeta
      modify_ \s -> s
        { nextMeta = new + 1
        , metas = Map.insert new { ty, sol: Nothing, solStx: Nothing, arity: A.length keep, name: entry.name, span: entry.span } s.metas
        }
      -- ?m' := λx₁…xₙ. ?new x_kept
      let
        idxs = map (\i -> n - i - 1) kept
        stx = lams n (S.MetaApp new idxs tyStx)
        val = V.nom (eval { local: V.LNil, global: g } N stx)
      modify_ \s -> s { metas = Map.update (\e -> Just e { sol = Just val, solStx = Just stx }) m' s.metas }
      traceSolve m' stx
      rename g m r (V.replay val sp)
    -- every argument in scope: the metavariable applied to their new indices
    Just args -> do
      entry <- case Map.lookup m' mc of
        Just e -> pure e
        Nothing -> scopeError
      ty <- renameTy g m r entry.ty
      idxs <- traverse (case _ of
        PVar l -> case Map.lookup l r.ren of
          Just l' -> pure (r.cod - l' - 1)
          Nothing -> scopeError
        PSelf -> scopeError) args
      pure (S.MetaApp m' idxs ty)
    Nothing -> scopeError

renameClo :: Globals -> Int -> PRen -> V.Ty -> V.Clo V.El -> U (S.Abs S.El)
renameClo g m r dom = case _ of
  V.Clo x l f -> S.Abs x <$> renameEv g m (lift1 r) (f (V.snoc l (V.local r.dom dom)))
  V.CloConst t -> S.AbsConst <$> renameEv g m r t

renameCloTy :: Globals -> Int -> PRen -> V.Ty -> V.Clo V.Ty -> U (S.Abs S.Ty)
renameCloTy g m r dom = case _ of
  V.Clo x l f -> S.Abs x <$> renameEvTy g m (lift1 r) (f (V.snoc l (V.local r.dom dom)))
  V.CloConst t -> S.AbsConst <$> renameEvTy g m r t

renameEvTy :: Globals -> Int -> PRen -> V.Ev V.Ty -> U S.Ty
renameEvTy g m r = case _ of
  V.Nom a -> renameTy g m r a
  V.Describe a -> renameTy g m r a
  V.Become a -> S.IsTy <$> renameTy g m r a

renameTy :: Globals -> Int -> PRen -> V.Ty -> U S.Ty
renameTy g m r t0 = do
  t <- forceTU t0
  case t of
    V.U u -> pure (S.U u)
    V.Decode n -> case n.head of
      V.Meta m'
        | m' == m -> occursError
        | otherwise -> S.Decode <$> renameFlex g m r m' n.spine
      _ -> do
        h <- renameHead r n.head
        S.Decode <$> renameSpine g m r n.spine h
    V.Function f -> do
      d <- renameTy g m r f.dom
      c <- renameCloTy g m r f.dom f.cod
      pure (S.Function { level: f.level, dom: d, cod: c, implicit: f.implicit })
    V.Record rt -> do
      let
        go i ls fs = case A.uncons fs of
          Nothing -> pure []
          Just { head: fc, tail } -> do
            let ty = (fc ls).ty
            x <- renameTy g m (iterateLift i r) ty
            rest <- go (i + 1) (V.snoc ls (V.local (r.dom + i) ty)) tail
            pure (A.cons x rest)
      tys <- go 0 rt.capture rt.fieldTypes.values
      pure (S.Record { level: rt.level, fieldTypes: { keys: rt.fieldTypes.keys, byName: rt.fieldTypes.byName, values: tys } })
    V.Sum (Variants vs) -> S.Sum <<< Variants <$> dictTraverse (traverse (renameTy g m r <<< L.force)) vs
    V.PrimInt -> pure S.PrimInt
    V.PrimFloat -> pure S.PrimFloat
    V.PrimString -> pure S.PrimString
    V.PrimArray a -> S.PrimArray <$> renameTy g m r a
  where
  iterateLift i rr = if i <= 0 then rr else iterateLift (i - 1) (lift1 rr)

-- Postponed problems, and zonking
--------------------------------------------------------------------------------

-- | Let the postponed problems and the delayed checks feed one another --
-- | a retried problem may solve the type a check waits on, and a run check
-- | may pose new problems -- until neither moves; then force what is left,
-- | and retry once more.  The problems still stuck are returned.
settle :: Globals -> E (Array { shape :: CtxShape, left :: V.El, right :: V.El, failed :: Boolean })
settle g = go 0
  where
  go :: Int -> E (Array { shape :: CtxShape, left :: V.El, right :: V.El, failed :: Boolean })
  go fuel = do
    runDelayed false
    _ <- retryPostponed g
    ds <- gets _.delayed
    mc <- gets _.metas
    let ready = A.any (\(Delayed d) -> not (isFlexTy (V.forceTy mc d.ty))) ds
    if ready && fuel <= 50 then go (fuel + 1)
    else do
      runDelayed true
      retryPostponed g

-- | Retry the postponed problems until no more are solved; the ones left
-- | are reported.
retryPostponed :: Globals -> E (Array { shape :: CtxShape, left :: V.El, right :: V.El, failed :: Boolean })
retryPostponed g = go 0
  where
  go :: Int -> E (Array { shape :: CtxShape, left :: V.El, right :: V.El, failed :: Boolean })
  go fuel = do
    ps <- gets _.postponed
    modify_ (_ { postponed = [] })
    -- a problem that FAILS outright is kept, to be reported as stuck: it
    -- holds the contradiction, and must not vanish into an unsolved meta
    for_ ps \p -> liftU (uEl g p.shape p.left p.right) >>= case _ of
      Left _ -> modify_ \s -> s { postponed = A.snoc s.postponed (p { failed = true }) }
      Right _ -> pure unit
    left <- gets _.postponed
    if A.null left || A.length left == A.length ps || fuel > 50 then pure left else go (fuel + 1)

-- | the metavariables that were created and never solved
unsolvedMetas :: E (Array { name :: Name, span :: Span })
unsolvedMetas = do
  mc <- gets _.metas
  pure (A.mapMaybe (\e -> case e.sol of
    Nothing -> Just { name: e.name, span: e.span }
    Just _ -> Nothing) (A.fromFoldable (Map.values mc)))

-- | Substitute the solutions into syntax.  A metavariable applied to
-- | locals becomes its solution's body, those locals put for its binders.
zonkEl :: V.MetaCtx -> S.El -> S.El
zonkEl mc = go
  where
  go t = case t of
    S.MetaApp m idxs ty -> case Map.lookup m mc >>= _.solStx of
      Just sol -> go (instantiate idxs sol)
      Nothing -> S.MetaApp m idxs (zonkTy mc ty)
    S.Code ty -> S.Code (zonkTy mc ty)
    S.App f a -> S.App (go f) (go a)
    S.Lam d b -> S.Lam (zonkTy mc d) (absMap go b)
    S.Cons d -> S.Cons (dictMap go d)
    S.Proj e l -> S.Proj (go e) l
    S.Tag x as -> S.Tag x (map go as)
    S.Match e cs -> S.Match (go e) (dictMap (\(Tuple xs b) -> Tuple xs (go b)) cs)
    S.Specialize e l v -> S.Specialize (go e) l (go v)
    S.Is e -> S.Is (go e)
    S.Rec e -> S.Rec (go e)
    S.Implicit x e -> S.Implicit x (go e)
    other -> other

zonkTy :: V.MetaCtx -> S.Ty -> S.Ty
zonkTy mc = go
  where
  go t = case t of
    S.Decode e -> S.Decode (zonkEl mc e)
    S.Function f -> S.Function { level: f.level, dom: go f.dom, cod: absMap go f.cod, implicit: f.implicit }
    S.Record r -> S.Record { level: r.level, fieldTypes: dictMap go r.fieldTypes }
    S.Sum (Variants vs) -> S.Sum (Variants (dictMap (map go) vs))
    S.IsTy a -> S.IsTy (go a)
    other -> other

absMap :: forall a. (a -> a) -> S.Abs a -> S.Abs a
absMap f = case _ of
  S.Abs x b -> S.Abs x (f b)
  S.AbsConst b -> S.AbsConst (f b)

-- | `sol` is `λx₁…xₖ. body`, closed; the result is `body` with each `xᵢ`
-- | replaced by the local `idxs[i]`
instantiate :: Array Int -> S.El -> S.El
instantiate idxs sol = substEl 0 (strip (A.length idxs) sol)
  where
  k = A.length idxs
  strip n t = if n <= 0 then t else case t of
    S.Lam _ (S.Abs _ b) -> strip (n - 1) b
    S.Lam _ (S.AbsConst b) -> strip (n - 1) b
    other -> other
  -- under `d` binders of the body, index `d + j` is the binder x_{k-j}
  var d i
    | i < d = S.Local i
    | otherwise = case A.index idxs (k - 1 - (i - d)) of
        Just j -> S.Local (j + d)
        Nothing -> S.Local i
  substEl d t = case t of
    S.Local i -> var d i
    S.MetaApp m ix ty -> S.MetaApp m (map (\i -> case var d i of
      S.Local j -> j
      _ -> i) ix) (substTy d ty)
    S.Code ty -> S.Code (substTy d ty)
    S.App f a -> S.App (substEl d f) (substEl d a)
    S.Lam dd b -> S.Lam (substTy d dd) (absUnder d b)
    S.Cons dct -> S.Cons (dictMap (substEl d) dct)
    S.Proj e l -> S.Proj (substEl d e) l
    S.Tag x as -> S.Tag x (map (substEl d) as)
    S.Match e cs -> S.Match (substEl d e) (dictMap (\(Tuple xs b) -> Tuple xs (substEl (d + A.length xs) b)) cs)
    S.Specialize e l v -> S.Specialize (substEl d e) l (substEl d v)
    S.Is e -> S.Is (substEl d e)
    S.Rec e -> S.Rec (substEl d e)
    S.Implicit x e -> S.Implicit x (substEl d e)
    other -> other
  absUnder d = case _ of
    S.Abs x b -> S.Abs x (substEl (d + 1) b)
    S.AbsConst b -> S.AbsConst (substEl d b)
  substTy d t = case t of
    S.Decode e -> S.Decode (substEl d e)
    S.Function f -> S.Function { level: f.level, dom: substTy d f.dom, cod: tyUnder d f.cod, implicit: f.implicit }
    S.Record rt ->
      let tys = A.mapWithIndex (\i ty -> substTy (d + i) ty) rt.fieldTypes.values
      in S.Record { level: rt.level, fieldTypes: rt.fieldTypes { values = tys } }
    S.Sum (Variants vs) -> S.Sum (Variants (dictMap (map (substTy d)) vs))
    S.IsTy a -> S.IsTy (substTy d a)
    other -> other
  tyUnder d = case _ of
    S.Abs x b -> S.Abs x (substTy (d + 1) b)
    S.AbsConst b -> S.AbsConst (substTy d b)

-- Delayed arguments
--------------------------------------------------------------------------------

-- | Run the delayed checks whose types are now known (and those they delay
-- | in turn); with `all`, every one, known or not.
runDelayed :: Boolean -> E Unit
runDelayed all = do
  ds <- gets _.delayed
  mc <- gets _.metas
  let
    ready (Delayed d) = all || not (isFlexTy (V.forceTy mc d.ty))
    now = A.filter ready ds
    later = A.filter (not <<< ready) ds
  modify_ (_ { delayed = later })
  for_ now \(Delayed d) -> d.check
  unless (A.null now) (runDelayed all)

isFlexTy :: V.Ty -> Boolean
isFlexTy t = case flexTy t of
  Just _ -> true
  Nothing -> false

-- | The hole standing for a delayed argument, filled with the argument once
-- | it is checked: its solution is the argument's syntax, closed over the
-- | locals the hole was applied to.  If unification got there first, the two
-- | must agree.
solveHole :: Globals -> CtxShape -> Int -> V.El -> { stx :: S.El, val :: V.El } -> E (Either DefEqCheckError Unit)
solveHole g cs m holeVal arg = do
  mc <- gets _.metas
  case Map.lookup m mc of
    Just e | isNothing e.sol -> do
      let
        stx = lams e.arity arg.stx
        val = V.nom (eval { local: V.LNil, global: g } N stx)
      modify_ \s -> s { metas = Map.update (\x -> Just x { sol = Just val, solStx = Just stx }) m s.metas }
      _ <- lift (runExceptT (traceSolve m stx))
      pure (Right unit)
    _ -> unifyEl g cs holeVal arg.val
