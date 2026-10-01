-- | Computing at compile time what is known then: β-reduction, the
-- | definitions that are not recursive inlined (when that makes the code no
-- | bigger than a little), a field of a known record, the arm of a match on
-- | a known tag.
-- |
-- | Hippogriff's programs are pure, but for `abandon` and divergence, and
-- | they are call-by-value: what is computed is computed, in order, even if
-- | no one uses it.  So only what cannot fail (`pure`: a value, of globals
-- | that cannot) is dropped where no one uses it, or moved to where it is
-- | used; what could fail stays where it was.
module Heff.Lower.Simplify
  ( simplifyProgram
  ) where

import Prelude

import Control.Monad.State (State, get, gets, modify_, put, runState)
import Data.Array as A
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..))
import Data.Set (Set)
import Data.Set as Set
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Heff.Lower.IR (Def, Expr(..), Supply, freshen, globalsIn, isValue, occurrences, size, subst)

type St = { supply :: Supply, fuel :: Int, defs :: Map String Expr, safe :: Set String, total :: Map String Int }

type M a = State St a

-- | how much bigger than what it replaces an inlined definition may make
-- | the code
budget :: Int
budget = 24

-- | each definition, in order, simplified; what is inlined is taken
-- | simplified already
simplifyProgram :: Supply -> Array Def -> { defs :: Array Def, supply :: Supply }
simplifyProgram n0 defs =
  let
    sg = safeGlobals defs
    Tuple out st = runState (traverse one defs) { supply: n0, fuel: 0, defs: Map.empty, safe: sg.safe, total: sg.total }
  in
    { defs: out, supply: st.supply }
  where
  one d = do
    modify_ _ { fuel = 4000 }
    -- (twice: what the first pass exposes, the second reduces)
    b <- simp d.body >>= simp
    -- a definition calling itself is never inlined
    unless (A.elem d.key (globalsIn b)) (modify_ \s -> s { defs = Map.insert d.key b s.defs })
    pure d { body = b }

copy :: Expr -> M Expr
copy e = do
  s <- get
  let Tuple e' n = freshen s.supply e
  put s { supply = n }
  pure e'

-- | a definition's body, to inline (a copy), if it may be
known :: String -> M (Maybe Expr)
known g = do
  s <- get
  case Map.lookup g s.defs of
    Just b | s.fuel > 0 -> do
      put s { fuel = s.fuel - 1 }
      Just <$> copy b
    _ -> pure Nothing

-- | try an inlining: keep it if it is small enough
attempt :: Expr -> M Expr -> M Expr
attempt orig m = do
  saved <- get
  e <- m
  if size e <= size orig + budget then pure e
  else do
    -- (the names drawn stay drawn: no name is ever given out twice.  And
    -- the fuel burnt stays burnt: a speculation that did not pay is not
    -- free to try again inside every caller -- refunding it made the
    -- simplifier exponential on a file of record-tied definitions)
    st <- get
    put saved { supply = st.supply, fuel = st.fuel }
    pure orig

-- | the globals that cannot fail when used: functions, and values of such
-- | globals (each definition after those it uses; one using itself is not).
-- | `total`: the functions whose application cannot fail either -- their
-- | body is pure under the lambdas (erased statics, record builders such
-- | as co.hf's `plus`) -- with their arity
safeGlobals :: Array Def -> { safe :: Set String, total :: Map String Int }
safeGlobals = A.foldl one { safe: Set.empty, total: Map.empty }
  where
  one acc d = case flat d.body of
    Tuple xs b | not (A.null xs) ->
      let acc' = acc { safe = Set.insert d.key acc.safe }
      in
        if pureIn acc.safe acc.total b && not (A.elem d.key (globalsIn b)) then acc' { total = Map.insert d.key (A.length xs) acc.total }
        else acc'
    Tuple _ b
      | pureIn acc.safe acc.total b && not (A.elem d.key (globalsIn b)) -> acc { safe = Set.insert d.key acc.safe }
    _ -> acc
  flat = case _ of
    Lam xs b -> case flat b of
      Tuple ys b' -> Tuple (xs <> ys) b'
    b -> Tuple [] b

-- | what cannot fail: evaluating it gives a value, at once.  A projection
-- | is of a record, always there; an application of a `total` function up
-- | to its arity builds a closure or its pure body
pureIn :: Set String -> Map String Int -> Expr -> Boolean
pureIn safe total = go
  where
  go = case _ of
    Var _ -> true
    Global g -> Set.member g safe
    Lam _ _ -> true
    Record fs -> A.all (\(Tuple _ e) -> go e) fs
    Tag _ as -> A.all go as
    Proj e _ -> go e
    App (Global g) as -> case Map.lookup g total of
      Just ar -> A.length as <= ar && A.all go as
      Nothing -> false
    Call g as -> case Map.lookup g total of
      Just ar -> A.length as <= ar && A.all go as
      Nothing -> false
    Prim _ -> true
    Int _ -> true
    Float _ -> true
    Str _ -> true
    _ -> false

isPure :: Expr -> M Boolean
isPure e = gets \s -> pureIn s.safe s.total e

atomic :: Expr -> Boolean
atomic = case _ of
  Var _ -> true
  Global _ -> true
  Int _ -> true
  Float _ -> true
  Str _ -> true
  Prim _ -> true
  _ -> false

simp :: Expr -> M Expr
simp = case _ of
  Lam xs b -> lam xs <$> simp b
  App f as -> do
    f' <- simp f
    as' <- traverse simp as
    applyTo f' as'
  Let x a b -> do
    a' <- simp a
    letIn x a' b
  Record fs -> Record <$> traverse (traverse simp) fs
  Proj e l -> simp e >>= project l
  Tag t as -> Tag t <$> traverse simp as
  Match s arms -> do
    s' <- simp s
    arms' <- traverse (\a -> (a { body = _ }) <$> simp a.body) arms
    match s' arms'
  PrimCall p as -> PrimCall p <$> traverse simp as
  e -> pure e

-- | (lambdas in lambdas are one lambda)
lam :: Array String -> Expr -> Expr
lam xs = case _ of
  Lam ys b -> Lam (xs <> ys) b
  b -> Lam xs b

-- | an application of simplified parts
applyTo :: Expr -> Array Expr -> M Expr
applyTo f as
  | A.null as = pure f
  | otherwise = case f of
      App g bs -> applyTo g (bs <> as)
      Lam xs b ->
        let
          k = min (A.length xs) (A.length as)
          binds = A.zip (A.take k xs) (A.take k as)
          inner = if A.length xs > k then Lam (A.drop k xs) b else b
        in
          A.foldr (\(Tuple x a) m -> m >>= \body -> bind' x a body) (pure inner) binds
            >>= simp
            >>= \r -> applyTo r (A.drop k as)
      Let x a b -> Let x a <$> applyTo b as
      -- (the arguments go into every arm: only when that copies nothing)
      Match s arms | A.all atomic as -> Match s <$> traverse (\a -> (a { body = _ }) <$> applyTo a.body as) arms
      Global g -> do
        mb <- known g
        case mb of
          Just b -> attempt (App f as) (applyTo b as)
          Nothing -> pure (App f as)
      -- (a field of a known record, applied: judged by what the application
      -- comes to, not by the size of the field)
      Proj (Global g) l -> do
        mb <- known g
        case mb of
          Just b -> attempt (App f as) (project l b >>= \h -> applyTo h as)
          Nothing -> pure (App f as)
      _ -> pure (App f as)
  where
  -- (not simplified yet: `simp` does it once all are bound)
  bind' x a body = pure (Let x a body)

-- | a binding, its value simplified: what could fail is computed where it
-- | was, used or not
letIn :: String -> Expr -> Expr -> M Expr
letIn x a b = do
  p <- isPure a
  let o = occurrences x b
  if not p then Let x a <$> simp b
  else if o.count == 0 then simp b
  else if atomic a || o.count == 1 then simp (subst (Map.singleton x a) b)
  else if isValue a && size a <= 12 then substCopies x a b >>= simp
  else Let x a <$> simp b

-- | `x` replaced by `a`, a copy of it for each occurrence
substCopies :: String -> Expr -> Expr -> M Expr
substCopies x a = go
  where
  go = case _ of
    Var y | y == x -> copy a
    Lam xs b -> Lam xs <$> go b
    App f as -> App <$> go f <*> traverse go as
    Let y e b -> Let y <$> go e <*> go b
    Record fs -> Record <$> traverse (traverse go) fs
    Proj e l -> (\e' -> Proj e' l) <$> go e
    Tag t as -> Tag t <$> traverse go as
    Match s arms -> Match <$> go s <*> traverse (\arm -> (arm { body = _ }) <$> go arm.body) arms
    PrimCall p as -> PrimCall p <$> traverse go as
    Call g as -> Call g <$> traverse go as
    e -> pure e

project :: String -> Expr -> M Expr
project l e = case e of
  -- (the other fields are computed too: dropped only if they cannot fail)
  Record fs -> case A.find (\(Tuple k _) -> k == l) fs of
    Just (Tuple _ v) -> do
      others <- A.all identity <$> traverse (\(Tuple k f) -> if k == l then pure true else isPure f) fs
      pure (if others then v else Proj e l)
    Nothing -> pure (Proj e l)
  Let x a b -> Let x a <$> project l b
  Global g -> do
    mb <- known g
    case mb of
      Just b -> attempt (Proj e l) (project l b)
      Nothing -> pure (Proj e l)
  _ -> pure (Proj e l)

match :: Expr -> Array { tag :: String, vars :: Array String, body :: Expr } -> M Expr
match s arms = case s of
  Tag t as -> case A.find (\a -> a.tag == t) arms of
    Just arm -> A.foldr (\(Tuple x a) m -> m >>= \body -> pure (Let x a body)) (pure arm.body) (A.zip arm.vars as) >>= simp
    Nothing -> pure (Match s arms)
  Let x a b -> Let x a <$> match b arms
  Global g -> do
    mb <- known g
    case mb of
      Just b@(Tag _ _) -> match b arms
      _ -> pure (Match s arms)
  _ -> pure (Match s arms)
