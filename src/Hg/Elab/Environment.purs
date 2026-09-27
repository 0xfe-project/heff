-- | A port of `Hippogriff.Elaborator.Environment`.
module Hg.Elab.Environment where

import Prelude

import Data.Either (Either(..))
import Effect (Effect)
import Control.Monad.Except (ExceptT, runExceptT, throwError)
import Control.Monad.State (State, modify_, runState)
import Data.Array as A
import Data.Map as Map
import Data.Lazy (Lazy)
import Data.Tuple (Tuple)
import Data.Foldable (foldr)
import Data.List (List(..), (:))
import Data.Maybe (Maybe(..))
import Hg.Parse.Notation (Diag, Severity(..), Span)
import Hg.Support.Common (Name)
import Hg.Core.Evaluation (EvalEnv)
import Hg.Core.Globals (Globals)
import Hg.Core.Params (Case(..), CtxShape)
import Hg.Core.Value as V
import Hg.Support.Pretty (Doc)

-- Scope
--------------------------------------------------------------------------------

data BindingMode = BDelayedBy Int

data Ctx
  = CEmpty
  | CBind Ctx BindingMode V.Ty
  | CUndelay Ctx

type Scope = { len :: Int, names :: List Name, locals :: V.Locals, ctx :: Ctx }

emptyScope :: Scope
emptyScope = { len: 0, names: Nil, locals: V.LNil, ctx: CEmpty }

undelay :: Scope -> Scope
undelay sd = sd { ctx = CUndelay sd.ctx }

bind :: Name -> V.Ty -> Scope -> Scope
bind = bindAt (BDelayedBy 0)

multiBind :: Array Name -> Array V.Ty -> Scope -> Scope
multiBind xs as sc = go (listOf xs) (listOf as) sc
  where
  listOf :: forall a. Array a -> List a
  listOf = foldrArr (:) Nil
  go (x : xs') (a : as') s = go xs' as' (bind x a s)
  go _ _ s = s

foldrArr :: forall a b. (a -> b -> b) -> b -> Array a -> b
foldrArr = foldr

bindAt :: BindingMode -> Name -> V.Ty -> Scope -> Scope
bindAt bm x a c =
  let v = V.local c.len a
  in letAt bm x v a c

withBound :: forall a. Name -> V.Ty -> Scope -> (V.El -> Scope -> a) -> a
withBound x a c action =
  let
    v = V.local c.len a
    c' = let_ x v a c
  in
    action v c'

let_ :: Name -> V.El -> V.Ty -> Scope -> Scope
let_ = letAt (BDelayedBy 0)

letAt :: BindingMode -> Name -> V.El -> V.Ty -> Scope -> Scope
letAt bm x v a c =
  { len: c.len + 1, names: x : c.names, locals: V.snoc c.locals v, ctx: CBind c.ctx bm a }

shape :: Scope -> CtxShape
shape c = { len: c.len, names: c.names }

-- Elaboration environment
--------------------------------------------------------------------------------

data ElabFor = ForNominative | ForDescriptive V.BareNeutral

caseOf :: ElabFor -> Case
caseOf = case _ of
  ForNominative -> N
  ForDescriptive _ -> D

projElabFor :: ElabFor -> Name -> ElabFor
projElabFor ForNominative _ = ForNominative
projElabFor (ForDescriptive n) x = ForDescriptive (n { spine = V.Proj n.spine x })

appElabFor :: ElabFor -> V.El -> ElabFor
appElabFor ForNominative _ = ForNominative
appElabFor (ForDescriptive n) v = ForDescriptive (n { spine = V.App n.spine v })

createNominative :: ElabFor -> V.Ev V.El -> V.Ty -> V.El
createNominative ForNominative v _ = V.nom v
createNominative (ForDescriptive n) ev a = V.reflect n.head (pure n.spine) (pure a) (Just ev)

type ElabEnv = { scope :: Scope, globals :: Globals, elabFor :: ElabFor }

evalEnv :: ElabEnv -> EvalEnv
evalEnv e = { local: e.scope.locals, global: e.globals }

emptyElabEnv :: Globals -> ElabEnv
emptyElabEnv globals = { elabFor: ForNominative, scope: emptyScope, globals }

emptyElabEnvFor :: Globals -> Name -> ElabEnv
emptyElabEnvFor globals x =
  { elabFor: ForDescriptive { head: V.Global x, spine: V.Id }, scope: emptyScope, globals }

-- Failure
--------------------------------------------------------------------------------

-- | What elaboration learns along the way, for the IDE's queries: what a
-- | name at a span resolved to, and its type (rendered lazily, since most
-- | are never asked for).
type Info = { span :: Span, name :: Name, key :: Maybe Name, ty :: Lazy String }

-- | A step of elaboration, for explaining it: a metavariable created or
-- | solved, a unification, an argument delayed.  The text is rendered on
-- | demand, from the state as it was at the event.
type TraceEvent = { span :: Span, kind :: String, text :: Lazy String }

-- | The state of elaborating one definition: what it learned for the IDE,
-- | its metacontext, and the conversion problems waiting for a metavariable.
-- | It starts empty for every definition and is dropped at its end.
type ElabSt =
  { infos :: Array Info
  , metas :: V.MetaCtx
  , nextMeta :: Int
  , postponed :: Array { shape :: CtxShape, left :: V.El, right :: V.El }
  , delayed :: Array Delayed
  , trace :: Array TraceEvent
  }

-- | an argument whose check waits until the type it is checked at (`ty`)
-- | is known
newtype Delayed = Delayed { ty :: V.Ty, check :: ExceptT Diag (State ElabSt) Unit }

emptySt :: ElabSt
emptySt = { infos: [], metas: Map.empty, nextMeta: 0, postponed: [], delayed: [], trace: [] }



-- | Elaboration gives up with the one diagnostic that made it stop, and
-- | keeps its state (the infos so far) either way.
type E a = ExceptT Diag (State ElabSt) a

runE :: forall a. E a -> ElabSt -> Tuple (Either Diag a) ElabSt
runE m st = runState (runExceptT m) st

traceE :: Span -> String -> Lazy String -> E Unit
traceE span kind text = modify_ (\s -> sinkPushImpl { span, kind, text } (s { trace = A.snoc s.trace { span, kind, text } }))

foreign import enableSinkImpl :: Effect Unit
foreign import sinkPushImpl :: forall a. TraceEvent -> a -> a
foreign import sinkTakeImpl :: Effect (Array TraceEvent)
foreign import tryPureImpl :: forall a r. (a -> r) -> (String -> r) -> (Unit -> a) -> r

-- | keep every trace event also outside the state (see Environment.js)
enableTraceSink :: Effect Unit
enableTraceSink = enableSinkImpl

takeTraceSink :: Effect (Array TraceEvent)
takeTraceSink = sinkTakeImpl

-- | a pure computation, with an internal error (an exception) as a value
tryPure :: forall a. (Unit -> a) -> Either String a
tryPure = tryPureImpl Right Left

record :: Info -> E Unit
record i = modify_ (\s -> s { infos = A.snoc s.infos i })

-- | elaborator codes are numbered from 300
failWith :: forall a. Span -> Int -> Doc -> E a
failWith sp code msg = throwError { code: 300 + code, severity: SError, msg, span: sp, note: Nothing }

failWithNote :: forall a. Span -> Int -> Doc -> Doc -> E a
failWithNote sp code msg note = throwError { code: 300 + code, severity: SError, msg, span: sp, note: Just note }
