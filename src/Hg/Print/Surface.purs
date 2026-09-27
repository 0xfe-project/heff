-- | Core syntax printed in the Haskell-like syntax (`Hg.Parse.Surface`
-- | reads it back).  Blocks (`case`) are laid out: their alternatives go
-- | on lines of their own, indented past the line they start on.
module Hg.Print.Surface
  ( Implicits
  , printEl
  , printTy
  , printDef
  , tagText
  ) where

import Prelude

import Data.Array as A
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..), fromMaybe, isJust)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple (Tuple(..))
import Hg.Support.Common (Name, dictToList, operatorName, quoteString)
import Hg.Core.Params (Universe(..), Variants(..))
import Hg.Core.Syntax as Syn
import Hg.Print.Resugar (equationsWith)

-- | which arguments of an application are implicit (by the name of their
-- | binder), from its head; and whether to leave them out or write them as
-- | `(x := a)`
type Implicits = { names :: List Name -> Syn.El -> Array (Maybe Name), hide :: Boolean }

type Ctx = { im :: Implicits, indent :: Int }

-- | precedences: 0 anything, 1 an operand of `->`, `~` or an operator, 2 an
-- | argument
paren :: Boolean -> String -> String
paren b s = if b then "(" <> s <> ")" else s

spaces :: Int -> String
spaces n = S.joinWith "" (A.replicate n " ")

-- | a tag: a capitalised one is written as a constructor
tagText :: Name -> String
tagText t = case SCU.charAt 0 t of
  Just c | c >= 'A' && c <= 'Z' -> t
  _ -> "'" <> t

-- | a local by its index; one shadowed by an inner one of the same name is
-- | primed
local :: List Name -> Int -> String
local xs i = case L.index xs i of
  Just x ->
    let n = L.length (L.filter (_ == x) (L.take i xs))
    in x <> S.joinWith "" (A.replicate n "'")
  Nothing -> "?" <> show i

printEl :: Implicits -> Int -> List Name -> Syn.El -> String
printEl im = el { im, indent: 0 }

printTy :: Implicits -> Int -> List Name -> Syn.Ty -> String
printTy im = ty { im, indent: 0 }

el :: Ctx -> Int -> List Name -> Syn.El -> String
el c p xs = case _ of
  Syn.Local i -> local xs i
  -- (an operator, alone: `(+)`)
  Syn.Global x | isJust (operatorName x) -> "(" <> x <> ")"
  Syn.Global x -> x
  Syn.Code t -> ty c p xs t
  t@(Syn.App _ _) | Just stmts <- doBlock t ->
    -- `bind m (\x -> rest)`, all the way down: a `do` block
    let
      inner = c { indent = c.indent + 2 }
      stmt st = "\n" <> spaces inner.indent <> (case st.bound of
        Just x | x /= "_" -> x <> " <- "
        _ -> "") <> el inner 0 st.names st.e
    in
      paren (p >= 1) ("do" <> S.joinWith "" (map stmt stmts))
  -- `a + b`: an operator applied to two (its fixity unknown here, an
  -- operand that is an operator's is parenthesised)
  t@(Syn.App _ _)
    | { head: Syn.Global x, args: [ l, r ] } <- spine t []
    , isJust (operatorName x)
    , not (isImplicit l || isImplicit r) ->
        paren (p >= 1) (el c 1 xs l <> " " <> x <> " " <> el c 1 xs r)
  t@(Syn.App _ _) ->
    let
      sp = spine t []
      names = c.im.names xs sp.head
      -- an implicit argument: marked so by the elaborator, or (in what
      -- was stored before it did) known from the head's type
      implicitName i a = case a of
        Syn.Implicit n inner -> Just { n, inner }
        _ -> join (A.index names i) <#> \n -> { n, inner: a }
      args = A.catMaybes (A.mapWithIndex (\i a -> case implicitName i a of
        Just _ | c.im.hide -> Nothing
        Just imp -> Just ("(" <> imp.n <> " := " <> el c 0 xs imp.inner <> ")")
        Nothing -> Just (el c 2 xs a)) sp.args)
    in
      if A.null args then el c p xs sp.head
      else paren (p >= 2) (S.joinWith " " (A.cons (el c 2 xs sp.head) args))
  t@(Syn.Lam _ _) ->
    let l = lambdas xs [] t
    in paren (p >= 1) ("\\" <> S.joinWith " " l.binders <> " -> " <> el c 0 l.names l.body)
  Syn.Cons d -> "{ " <> S.joinWith ", " (map (\(Tuple l e) -> l <> " = " <> el c 0 xs e) (dictToList d)) <> " }"
  Syn.Proj e l -> el c 2 xs e <> "." <> l
  Syn.Tag x [] -> tagText x
  Syn.Tag x ts -> paren (p >= 2) (S.joinWith " " (A.cons (tagText x) (map (el c 2 xs) ts)))
  Syn.Match s cases ->
    let
      inner = c { indent = c.indent + 2 }
      alt (Tuple t (Tuple ys body)) =
        let xs' = A.foldl (\acc y -> y : acc) xs ys
        in "\n" <> spaces inner.indent <> S.joinWith " " (A.cons (tagText t) ys) <> " -> " <> el inner 0 xs' body
    in
      paren (p >= 1) ("case " <> el c 0 xs s <> " of" <> S.joinWith "" (map alt (dictToList cases)))
  Syn.Specialize e x v -> paren (p >= 1) (el c 2 xs e <> " ~ { " <> x <> " = " <> el c 0 xs v <> " }")
  Syn.Is e -> el c p xs e
  Syn.Rec e -> el c p xs e
  Syn.Opaque -> "<opaque>"
  Syn.Abandon -> "abandon"
  Syn.MetaApp m _ _ -> "?" <> show m
  Syn.LitInt i -> show i
  Syn.LitString s -> quoteString s
  Syn.Implicit _ e -> el c p xs e
  Syn.Prim x -> "<primitive " <> x <> ">"
  where
  -- the statements of a `do` block (each with the locals it sees)
  doBlock t = case explicitArgs t of
    Just { head: Syn.Global "bind", args: [ m, k ] } ->
      let
        first = case k of
          Syn.Lam _ (Syn.Abs x rest) -> Just { stmt: { bound: Just x, e: m, names: xs }, rest, names: x : xs }
          Syn.Lam _ (Syn.AbsConst rest) -> Just { stmt: { bound: Nothing, e: m, names: xs }, rest, names: xs }
          _ -> Nothing
      in
        first <#> \f -> A.cons f.stmt (fromMaybe [ { bound: Nothing, e: f.rest, names: f.names } ] (doBlockIn f.names f.rest))
    _ -> Nothing
  doBlockIn ys t = el' ys t
  el' ys t = case explicitArgsIn ys t of
    Just { head: Syn.Global "bind", args: [ m, k ] } ->
      case k of
        Syn.Lam _ (Syn.Abs x rest) -> Just (A.cons { bound: Just x, e: m, names: ys } (fromMaybe [ { bound: Nothing, e: rest, names: x : ys } ] (el' (x : ys) rest)))
        Syn.Lam _ (Syn.AbsConst rest) -> Just (A.cons { bound: Nothing, e: m, names: ys } (fromMaybe [ { bound: Nothing, e: rest, names: ys } ] (el' ys rest)))
        _ -> Nothing
    _ -> Nothing
  explicitArgs t = explicitArgsIn xs t
  explicitArgsIn ys t = case t of
    Syn.App _ _ ->
      let
        sp = spine t []
        names = c.im.names ys sp.head
      in
        Just { head: sp.head, args: A.catMaybes (A.mapWithIndex (\i a -> case a of
          Syn.Implicit _ _ -> Nothing
          _ -> if isJust (join (A.index names i)) then Nothing else Just a) sp.args) }
    _ -> Nothing
  spine (Syn.App f a) acc = spine f (A.cons a acc)
  spine h acc = { head: h, args: acc }
  lambdas ys bs = case _ of
    Syn.Lam (Syn.Function ft) (Syn.Abs x b) | ft.implicit -> lambdas (x : ys) (A.snoc bs ("{" <> x <> "}")) b
    Syn.Lam _ (Syn.Abs x b) -> lambdas (x : ys) (A.snoc bs x) b
    Syn.Lam _ (Syn.AbsConst b) -> lambdas ys (A.snoc bs "_") b
    b -> { names: ys, binders: bs, body: b }

ty :: Ctx -> Int -> List Name -> Syn.Ty -> String
ty c p xs = case _ of
  Syn.U SmallU -> "Type"
  Syn.U LargeU -> "Theory"
  Syn.Decode e -> el c p xs e
  Syn.Function f -> paren (p >= 1) (arrow f)
  Syn.Record r ->
    let
      fields = A.foldl (\acc (Tuple l t) -> { names: l : acc.names, out: A.snoc acc.out (l <> " :: " <> ty c 0 acc.names t) })
        { names: xs, out: [] } (dictToList r.fieldTypes)
    in "{ " <> S.joinWith ", " fields.out <> " }"
  Syn.Sum (Variants vs) -> "sum { " <> S.joinWith " | " (map (\(Tuple t ts) -> S.joinWith " " (A.cons (tagText t) (map (ty c 2 xs) ts))) (dictToList vs)) <> " }"
  Syn.IsTy t -> ty c p xs t
  Syn.PrimInt -> "Int"
  Syn.PrimString -> "String"
  where
  arrow f = case f.cod of
    Syn.Abs x b | f.implicit ->
      -- `{a b :: T}`: implicit binders of the same type, together
      let r = implicits [ x ] f.dom b (x : xs)
      in "{" <> S.joinWith " " r.names <> " :: " <> ty c 0 xs f.dom <> "} -> " <> ty c 0 r.ctx r.rest
    Syn.Abs x b -> "(" <> x <> " :: " <> ty c 0 xs f.dom <> ") -> " <> ty c 0 (x : xs) b
    Syn.AbsConst b -> ty c 1 xs f.dom <> " -> " <> ty c 0 xs b
  -- (only binders of a universe: their types cannot mention one another)
  implicits names dom rest ctx = case rest, dom of
    Syn.Function g, Syn.U u | g.implicit, Syn.Abs y b <- g.cod, Syn.U u' <- g.dom, u == u' ->
      implicits (A.snoc names y) dom b (y : ctx)
    _, _ -> { names, rest, ctx }

-- | a stored definition: as `data`, `type`, `theory` or `foreign import`
-- | where it has their shape, else a signature and equations
printDef :: Implicits -> Name -> Syn.Ty -> Syn.El -> String
printDef im name t body =
  case primitive body of
    Just _ -> "foreign import " <> name <> " :: " <> printTy im 0 Nil t <> "\n"
    Nothing -> case typeFormer t body (name : Nil) [] of
      Just tf -> case tf.def of
        Syn.Sum (Variants vs) | not tf.large ->
          "data " <> head tf.params <> (if A.null (dictToList vs) then "" else " = " <> S.joinWith " | " (map (\(Tuple x ts) -> S.joinWith " " (A.cons (tagText x) (map (ty c 2 tf.names) ts))) (dictToList vs))) <> "\n"
        Syn.Record r | tf.large ->
          let
            fields = A.foldl (\acc (Tuple l ft) -> { names: l : acc.names, out: A.snoc acc.out ("  " <> l <> " :: " <> ty c 0 acc.names ft) })
              { names: tf.names, out: [] } (dictToList r.fieldTypes)
          in "theory " <> head tf.params <> " where\n" <> S.joinWith "\n" fields.out <> "\n"
        other | not tf.large -> "type " <> head tf.params <> " = " <> ty c 0 tf.names other <> "\n"
        _ -> general
      Nothing -> general
  where
  c = { im, indent: 0 }
  general =
    name <> " :: " <> printTy im 0 Nil t <> "\n"
      <> case equationsWith { tag: tagText, sep: "=", minRows: 1, body: \ns e -> el { im, indent: 0 } 0 ns e } name (name : Nil) t body of
        Just eqs -> S.joinWith "" (map (_ <> "\n") eqs)
        Nothing -> name <> " = " <> el { im, indent: 0 } 0 (name : Nil) body <> "\n"
  head params = S.joinWith " " (A.cons name params)
  primitive = case _ of
    Syn.Prim x -> Just x
    Syn.Is e -> primitive e
    _ -> Nothing

-- | `(p :: A) -> … -> Type` defined by lambdas and then a type: a `data`, a
-- | `type` or a `theory` (`large`: into `Theory`)
typeFormer :: Syn.Ty -> Syn.El -> List Name -> Array String -> Maybe { params :: Array String, names :: List Name, def :: Syn.Ty, large :: Boolean }
typeFormer t body ns params = case t, body of
  Syn.U u, _ -> case strip body of
    Syn.Code d -> Just { params, names: ns, def: unIs d, large: u == LargeU }
    _ -> Nothing
  Syn.Function f, Syn.Lam _ (Syn.Abs x b) | not f.implicit, Syn.Abs _ cod <- f.cod ->
    -- (a parameter of type `Type` is bare if it is lower case: `data List a`)
    let param = case f.dom of
          Syn.U SmallU | not (upper x) -> x
          d -> "(" <> x <> " :: " <> printTy { names: \_ _ -> [], hide: false } 0 ns d <> ")"
    in typeFormer cod b (x : ns) (A.snoc params param)
  _, Syn.Is b -> typeFormer t b ns params
  _, _ -> Nothing
  where
  strip = case _ of
    Syn.Is e -> strip e
    e -> e
  unIs = case _ of
    Syn.IsTy d -> unIs d
    d -> d

upper :: String -> Boolean
upper x = case SCU.charAt 0 x of
  Just c -> c >= 'A' && c <= 'Z'
  Nothing -> false

isImplicit :: Syn.El -> Boolean
isImplicit = case _ of
  Syn.Implicit _ _ -> true
  _ -> false
