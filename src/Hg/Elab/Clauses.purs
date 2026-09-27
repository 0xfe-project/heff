-- | extension: definitions by equations, as in Haskell or Agda --
-- |
-- |     def plus : Nat -> Nat -> Nat where
-- |       plus 'zero n := n
-- |       plus ('succ m) n := 'succ (plus m n)
-- |     end
-- |
-- | Each equation is one case of an induction.  They are compiled, as
-- | notation, into the lambdas and `match`es Hippogriff has, so what is
-- | elaborated is exactly what one would write by hand;
-- | whether every case is covered is then the `match`'s business.
module Hg.Elab.Clauses
  ( stripWhere
  , desugar
  ) where

import Prelude

import Data.Array as A
import Data.Array.NonEmpty as NEA
import Data.Either (Either(..))
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Hg.Parse.FNotation (Ntn(..), Span, getGroup, group, ntnSpan)
import Hg.Support.Common (Name)

-- | `T where ... end`, the type of a definition by equations: the type and
-- | the equations (the block ends up as an argument of the type's last part)
stripWhere :: Ntn -> Maybe (Tuple Ntn (Array Ntn))
stripWhere = case _ of
  Juxt f (NBlock "where" Nothing eqs _) -> Just (Tuple f eqs)
  Infix l op r -> stripWhere r <#> \(Tuple r' eqs) -> Tuple (Infix l op r') eqs
  _ -> Nothing

data Pat = PVar Name | PTag Name (Array Pat) Span

type Row = { pats :: Array Pat, rhs :: Ntn }

type Err = { span :: Span, msg :: String }

pat :: Ntn -> Either Err Pat
pat n = case NEA.uncons (getGroup n) of
  { head: NTuple [ p ] _, tail: [] } -> pat p
  { head: Ident x _, tail: [] } -> Right (PVar x)
  { head: NTag t sp, tail: ps } -> PTag t <$> traverse pat ps <@> sp
  _ -> Left { span: ntnSpan n, msg: "a pattern is a variable, or a tag applied to patterns" }

-- | `f p ... := e`
equation :: Name -> Ntn -> Either Err Row
equation f n = case n of
  Infix lhs (Keyword ":=" _) rhs -> case NEA.uncons (getGroup lhs) of
    { head: Ident g _, tail: ps }
      | g == f -> ps # traverse pat <#> \pats -> { pats, rhs }
    _ -> Left { span: ntnSpan lhs, msg: "an equation of `" <> f <> "` starts with `" <> f <> "`" }
  _ -> Left { span: ntnSpan n, msg: "an equation has the form `" <> f <> " <patterns> := <body>`" }

-- | the body of `f` from its equations: `params` are the parameters named in
-- | the head of the definition, which the first equations' patterns match
desugar :: Name -> Array Name -> Array Ntn -> Span -> Either Err Ntn
desugar f params eqNs sp = do
  rows <- traverse (equation f) eqNs
  arity <- case A.head rows of
    Nothing -> Left { span: sp, msg: "a definition by equations needs an equation" }
    Just r -> Right (A.length r.pats)
  for_' rows \r -> when (A.length r.pats /= arity) $ Left
    { span: ntnSpan r.rhs, msg: "every equation of `" <> f <> "` has " <> show arity <> " pattern(s)" }
  when (arity < A.length params) $ Left
    { span: sp, msg: "the equations of `" <> f <> "` match fewer arguments than it names" }
  let
    used = A.concatMap (\r -> A.concatMap patVars r.pats <> identsOf r.rhs) rows <> params <> [ f ]
    -- the name of a column: the parameter's, else the one its equations
    -- use, else a fresh one
    colName i
      | Just p <- A.index params i = p
      | otherwise = case A.nub (A.mapMaybe (\r -> A.index r.pats i >>= varOf) rows) of
          [ x ] | x /= "_" -> x
          _ -> fresh used ("x" <> show (i + 1))
    vars = map colName (upTo arity)
    lambdas = A.drop (A.length params) vars
  body <- compile used sp vars rows
  pure (A.foldr (\x b -> Infix (Ident x sp) (Keyword "=>" sp) b) body lambdas)

for_' :: forall a. Array a -> (a -> Either Err Unit) -> Either Err Unit
for_' xs k = void (traverse k xs)

varOf :: Pat -> Maybe Name
varOf = case _ of
  PVar x -> Just x
  _ -> Nothing

patVars :: Pat -> Array Name
patVars = case _ of
  PVar x -> [ x ]
  PTag _ ps _ -> A.concatMap patVars ps

identsOf :: Ntn -> Array Name
identsOf = case _ of
  Ident x _ -> [ x ]
  Juxt a b -> identsOf a <> identsOf b
  Infix a _ b -> identsOf a <> identsOf b
  NBlock _ h ns _ -> fromMaybe [] (identsOf <$> h) <> A.concatMap identsOf ns
  MDecl _ _ n _ -> identsOf n
  NTuple ns _ -> A.concatMap identsOf ns
  NBrace n _ -> identsOf n
  _ -> []

fresh :: Array Name -> Name -> Name
fresh used x = if A.elem x used then fresh used (x <> "'") else x

-- | the match tree for `rows` over the variables `vars`, column by column
compile :: Array Name -> Span -> Array Name -> Array Row -> Either Err Ntn
compile used sp vars rows = case A.head rows of
  Nothing -> Right (Keyword "abandon" sp) -- unreachable: a missing tag is the match's error
  Just r -> case A.findIndex isTag r.pats of
    -- the first equation matches whatever is left: it is the answer
    Nothing -> Right (A.foldl (\e (Tuple v p) -> bindVar v p e) r.rhs (A.zip vars r.pats))
    Just c -> do
      let
        v = fromMaybe "_" (A.index vars c)
        tags = A.nubByEq (\a b -> a.tag == b.tag) (A.mapMaybe (\r' -> A.index r'.pats c >>= tagOf) rows)
        -- rows whose column `c` is a variable go on in every other case
        rest = A.filter (\r' -> not (maybe' isTag (A.index r'.pats c))) rows
      arms <- traverse (arm c v) tags
      dflt <-
        if A.null rest then pure []
        else do
          b <- compile used sp (dropAt c vars) (map (\r' -> r' { pats = dropAt c r'.pats, rhs = bindVar v (at c r') r'.rhs }) rest)
          pure [ Infix (Ident "_" sp) (Keyword "=>" sp) b ]
      pure (NBlock "match" (Just (Ident v sp)) (arms <> dflt) sp)
  where
  isTag = case _ of
    PTag _ _ _ -> true
    _ -> false
  tagOf = case _ of
    PTag t ps s -> Just { tag: t, arity: A.length ps, pats: ps, span: s }
    _ -> Nothing
  at c r' = fromMaybe (PVar "_") (A.index r'.pats c)
  arm c v t = do
    let
      -- the fields' names: those the first equation of the tag uses
      sub i = case A.index t.pats i >>= varOf of
        Just x | x /= "_" && not (A.elem x vars) -> x
        _ -> fresh (used <> vars) (v <> show (i + 1))
      ys = map sub (upTo t.arity)
      specialize r' = case A.index r'.pats c of
        Just (PTag t' ps _)
          | t' == t.tag -> Just r' { pats = spliceAt c ps r'.pats }
          | otherwise -> Nothing
        Just p -> Just r' { pats = spliceAt c (map (const (PVar "_")) ys) r'.pats, rhs = bindVar v p r'.rhs }
        Nothing -> Nothing
    when (A.any (\r' -> case A.index r'.pats c of
        Just (PTag t' ps _) -> t' == t.tag && A.length ps /= t.arity
        _ -> false) rows) $ Left { span: t.span, msg: "'" <> t.tag <> " is used with different numbers of arguments" }
    body <- compile used sp (spliceAt c ys vars) (A.mapMaybe specialize rows)
    pure (Infix (group (NEA.cons' (NTag t.tag t.span) (map (\y -> Ident y sp) ys))) (Keyword "=>" sp) body)

-- | 0, 1, ..., n - 1 (`A.range 0 (-1)` counts down)
upTo :: Int -> Array Int
upTo n = if n <= 0 then [] else A.range 0 (n - 1)

maybe' :: forall a. (a -> Boolean) -> Maybe a -> Boolean
maybe' p = case _ of
  Just a -> p a
  Nothing -> false

dropAt :: forall a. Int -> Array a -> Array a
dropAt i xs = fromMaybe xs (A.deleteAt i xs)

-- | replace the element at `i` by `ys`
spliceAt :: forall a. Int -> Array a -> Array a -> Array a
spliceAt i ys xs = A.take i xs <> ys <> A.drop (i + 1) xs

-- | a variable pattern `p` matched against the variable `v`: `p` means `v`
bindVar :: Name -> Pat -> Ntn -> Ntn
bindVar v p e = case p of
  PVar x | x /= "_" && x /= v -> rename x v e
  _ -> e

-- | `x` becomes `v`, except where something binds `x` again
rename :: Name -> Name -> Ntn -> Ntn
rename x v = go
  where
  go n = case n of
    Ident y sp | y == x -> Ident v sp
    Infix l k@(Keyword "=>" _) r
      | A.elem x (identsOf l) -> n
      | otherwise -> Infix l k (go r)
    Infix l k r -> Infix (go l) k (go r)
    Juxt a b -> Juxt (go a) (go b)
    NBlock b h ns sp -> NBlock b (go <$> h) (map go ns) sp
    NTuple ns sp -> NTuple (map go ns) sp
    NBrace a sp -> NBrace (go a) sp
    _ -> n
