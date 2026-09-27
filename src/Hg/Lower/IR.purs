-- | The untyped intermediate language between the core and the backends:
-- | types are gone, and what is left computes.  Every bound variable has a
-- | name of its own in a whole program (`Hg.Lower.Erase` makes them so, and
-- | every pass that copies code renames what it copies), so substitution
-- | never captures.
-- |
-- |   core ─[Erase]→ IR ─[Simplify]→ IR ─[Shake]→ IR ─[Arity]→ IR ─[emit]→ JavaScript
module Hg.Lower.IR
  ( Expr(..)
  , Arm
  , Def
  , Program
  , Supply
  , fresh
  , baseOf
  , freshen
  , subst
  , occurrences
  , size
  , globalsIn
  , isValue
  , spine
  ) where

import Prelude

import Data.Array as A
import Data.Foldable (foldl, sum)
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as S
import Data.Tuple (Tuple(..))
import Hg.Support.BigInt (BigInt)

data Expr
  = Var String
  | Global String
  | Lam (Array String) Expr
  | App Expr (Array Expr)
  | Let String Expr Expr
  | Record (Array (Tuple String Expr))
  | Proj Expr String
  | Tag String (Array Expr)
  | Match Expr (Array Arm)
  | Prim String -- ^ a primitive, as a (curried) function
  | PrimCall String (Array Expr) -- ^ a primitive, given all its arguments
  | Call String (Array Expr) -- ^ a global of that arity, given all its arguments at once (`Hg.Lower.Arity`)
  | Int BigInt
  | Str String
  | Abandon

-- | `vars` bind the tag's arguments, in order
type Arm = { tag :: String, vars :: Array String, body :: Expr }

-- | a definition: its key, the name it is known by, and its body
type Def = { key :: String, name :: String, body :: Expr }

type Program = Array Def

-- | where fresh names come from (a counter)
type Supply = Int

fresh :: String -> Supply -> Tuple String Supply
fresh base n = Tuple (base <> "$" <> show n) (n + 1)

-- | the base of a variable's name (what it was called in the source)
baseOf :: String -> String
baseOf x = case S.indexOf (S.Pattern "$") x of
  Just i -> S.take i x
  Nothing -> x

-- | a copy of an expression with every binder renamed afresh
freshen :: Supply -> Expr -> Tuple Expr Supply
freshen = go Map.empty
  where
  go :: Map String String -> Supply -> Expr -> Tuple Expr Supply
  go m n = case _ of
    Var x -> Tuple (Var (fromMaybe x (Map.lookup x m))) n
    Lam xs b ->
      let
        r = binders m n xs
        Tuple b' n' = go r.m r.n b
      in
        Tuple (Lam r.xs b') n'
    Let x a b ->
      let
        Tuple a' n1 = go m n a
        Tuple x' n2 = fresh (baseOf x) n1
        Tuple b' n3 = go (Map.insert x x' m) n2 b
      in
        Tuple (Let x' a' b') n3
    Match s arms ->
      let
        Tuple s' n1 = go m n s
        step (Tuple acc k) arm =
          let
            r = binders m k arm.vars
            Tuple b' k' = go r.m r.n arm.body
          in
            Tuple (A.snoc acc { tag: arm.tag, vars: r.xs, body: b' }) k'
        Tuple arms' n2 = foldl step (Tuple [] n1) arms
      in
        Tuple (Match s' arms') n2
    e -> mapChildren (go m) n e
  binders m n xs = foldl
    ( \acc x ->
        let Tuple x' k = fresh (baseOf x) acc.n
        in { m: Map.insert x x' acc.m, n: k, xs: A.snoc acc.xs x' }
    )
    { m, n, xs: [] }
    xs

-- | the same shape, each child transformed (threading the supply); binders
-- | are left as they are
mapChildren :: (Supply -> Expr -> Tuple Expr Supply) -> Supply -> Expr -> Tuple Expr Supply
mapChildren f n = case _ of
  Lam xs b -> let Tuple b' k = f n b in Tuple (Lam xs b') k
  App g as ->
    let
      Tuple g' k = f n g
      Tuple as' k' = many k as
    in
      Tuple (App g' as') k'
  Let x a b ->
    let
      Tuple a' k = f n a
      Tuple b' k' = f k b
    in
      Tuple (Let x a' b') k'
  Record fs ->
    let Tuple es k = many n (map (\(Tuple _ e) -> e) fs)
    in Tuple (Record (A.zipWith (\(Tuple l _) e -> Tuple l e) fs es)) k
  Proj e l -> let Tuple e' k = f n e in Tuple (Proj e' l) k
  Tag t as -> let Tuple as' k = many n as in Tuple (Tag t as') k
  Match s arms ->
    let
      Tuple s' k = f n s
      Tuple bs k' = many k (map _.body arms)
    in
      Tuple (Match s' (A.zipWith (\a b -> a { body = b }) arms bs)) k'
  PrimCall p as -> let Tuple as' k = many n as in Tuple (PrimCall p as') k
  Call g as -> let Tuple as' k = many n as in Tuple (Call g as') k
  e -> Tuple e n
  where
  many k es = foldl (\(Tuple acc k') e -> let Tuple e' k'' = f k' e in Tuple (A.snoc acc e') k'') (Tuple [] k) es

-- | replace variables (names are unique, so nothing is captured; what is
-- | put in more than once must be freshened by the caller)
subst :: Map String Expr -> Expr -> Expr
subst m e0 = if Map.isEmpty m then e0 else go e0
  where
  go = case _ of
    Var x -> fromMaybe (Var x) (Map.lookup x m)
    Lam xs b -> Lam xs (go b)
    App f as -> App (go f) (map go as)
    Let x a b -> Let x (go a) (go b)
    Record fs -> Record (map (map go) fs)
    Proj e l -> Proj (go e) l
    Tag t as -> Tag t (map go as)
    Match s arms -> Match (go s) (map (\a -> a { body = go a.body }) arms)
    PrimCall p as -> PrimCall p (map go as)
    Call g as -> Call g (map go as)
    e -> e

-- | how often a variable occurs, and whether any occurrence is under a
-- | lambda (where it may be evaluated any number of times)
occurrences :: String -> Expr -> { count :: Int, underLam :: Boolean }
occurrences x = go false
  where
  none = { count: 0, underLam: false }
  both a b = { count: a.count + b.count, underLam: a.underLam || b.underLam }
  all lam es = foldl (\acc e -> both acc (go lam e)) none es
  go lam = case _ of
    Var y -> if y == x then { count: 1, underLam: lam } else none
    Lam _ b -> go true b
    App f as -> all lam (A.cons f as)
    Let _ a b -> both (go lam a) (go lam b)
    Record fs -> all lam (map (\(Tuple _ e) -> e) fs)
    Proj e _ -> go lam e
    Tag _ as -> all lam as
    Match s arms -> both (go lam s) (all lam (map _.body arms))
    PrimCall _ as -> all lam as
    Call _ as -> all lam as
    _ -> none

size :: Expr -> Int
size = case _ of
  Lam _ b -> 1 + size b
  App f as -> 1 + size f + sum (map size as)
  Let _ a b -> 1 + size a + size b
  Record fs -> 1 + sum (map (\(Tuple _ e) -> size e) fs)
  Proj e _ -> 1 + size e
  Tag _ as -> 1 + sum (map size as)
  Match s arms -> 1 + size s + sum (map (\a -> size a.body) arms)
  PrimCall _ as -> 1 + sum (map size as)
  Call _ as -> 1 + sum (map size as)
  _ -> 1

globalsIn :: Expr -> Array String
globalsIn = A.nub <<< go
  where
  go = case _ of
    Global g -> [ g ]
    Lam _ b -> go b
    App f as -> go f <> A.concatMap go as
    Let _ a b -> go a <> go b
    Record fs -> A.concatMap (\(Tuple _ e) -> go e) fs
    Proj e _ -> go e
    Tag _ as -> A.concatMap go as
    Match s arms -> go s <> A.concatMap (\a -> go a.body) arms
    PrimCall _ as -> A.concatMap go as
    Call g as -> [ g ] <> A.concatMap go as
    _ -> []

-- | evaluating it does nothing but build a value: it can be dropped, moved,
-- | or duplicated (when it is small)
isValue :: Expr -> Boolean
isValue = case _ of
  Var _ -> true
  Global _ -> true
  Lam _ _ -> true
  Record fs -> A.all (\(Tuple _ e) -> isValue e) fs
  Tag _ as -> A.all isValue as
  Prim _ -> true
  Int _ -> true
  Str _ -> true
  _ -> false

-- | an application's head and all its arguments, the nested ones flattened
spine :: Expr -> Tuple Expr (Array Expr)
spine = case _ of
  App f as -> let Tuple h bs = spine f in Tuple h (bs <> as)
  e -> Tuple e []
