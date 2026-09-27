-- | Core to the untyped IR: what is a type is erased -- not replaced by a
-- | placeholder, but gone, with the parameter that took it and the argument
-- | given for it.  Which is which is read off the types (Hippogriff's phase
-- | distinction: a small type is never computed from a value), so the core
-- | is checked again, with the elaborator's NbE, as `Hg.Compile.PureScript`
-- | does.
-- |
-- | A term is a type when its type classifies types: a universe (`Type`,
-- | `Theory`), or a function into one.  So `(a :: Type) -> ...` loses its
-- | parameter, `Monoid` and `List` are no definitions at all, and a record's
-- | type fields are not in it.
module Hg.Lower.Erase
  ( eraseProgram
  , isType
  ) where

import Prelude

import Control.Monad.State (StateT, get, put, runStateT)
import Control.Monad.Trans.Class (lift)
import Data.Array as A
import Data.Either (Either(..))
import Data.Lazy (defer, force)
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..))
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..))
import Hg.Core.Evaluation (eval, evalTy)
import Hg.Core.Globals (Globals, GlobalEntry, globalsList, lookupGlobal)
import Hg.Core.Params (Case(..), Variants(..))
import Hg.Core.Syntax as S
import Hg.Core.Value as V
import Hg.Lower.IR (Def, Expr(..), Supply, fresh)
import Hg.Support.Common (Name, dictToList)
import Hg.Elab.Environment (tryPure)

type M a = StateT Supply (Either String) a

-- | a local: what it is in the IR (nothing, if it was a type), its type
type Local = { ir :: Maybe Expr, ty :: V.Ty }

type Ctx = { locals :: List Local, env :: V.Locals, len :: Int, globals :: Globals }

failWith :: forall a. String -> M a
failWith = lift <<< Left

freshName :: String -> M String
freshName x = do
  n <- get
  let Tuple v n' = fresh (if x == "" || x == "_" then "x" else x) n
  put n'
  pure v

extend :: Ctx -> Maybe Expr -> V.Ty -> Ctx
extend c ir ty = c { locals = { ir, ty } : c.locals, env = V.snoc c.env (V.local c.len ty), len = c.len + 1 }

evalIn :: Ctx -> S.El -> V.El
evalIn c t = V.nom (eval { local: c.env, global: c.globals } N t)

-- | does a type classify types (possibly type families): a universe, or a
-- | function into one
isType :: Int -> V.Ty -> Boolean
isType lvl t = case V.behavior t of
  V.LikeU _ -> true
  V.LikeFunction ft -> isType (lvl + 1) (V.nom (V.appClo ft.cod (pure (V.local lvl ft.dom))))
  _ -> false

-- | a program's definitions, in order; the types among them are gone.  What
-- | cannot be lowered is said, by key.
eraseProgram :: Globals -> { defs :: Array Def, errors :: Array String, supply :: Supply }
eraseProgram g = A.foldl one { defs: [], errors: [], supply: 0 } (globalsList g)
  where
  one acc (Tuple key e)
    | isType 0 e.type_ = acc
    | otherwise = case tryPure (\_ -> runStateT (definition g key e) acc.supply) of
        Right (Right (Tuple body n)) -> acc { defs = A.snoc acc.defs { key, name: key, body }, supply = n }
        Right (Left err) -> acc { errors = A.snoc acc.errors (key <> ": " <> err) }
        Left err -> acc { errors = A.snoc acc.errors (key <> ": internal error: " <> err) }

-- | (the definition itself is local 0)
definition :: Globals -> Name -> GlobalEntry -> M Expr
definition g key e =
  chkD { locals: { ir: Just (Global key), ty: e.type_ } : Nil, env: V.snoc V.LNil e.value, len: 1, globals: g } e.syntax e.type_ e.value

-- | a definition's own syntax, whose value `v` is known: under its lambdas
-- | and through its record's fields the value is followed along, so that a
-- | field defining a type (a sum, generative) is never evaluated again
chkD :: Ctx -> S.El -> V.Ty -> V.El -> M Expr
chkD c t ty v = case t of
  S.Lam _ abs | V.LikeFunction ft <- V.behavior ty -> do
    let
      x = V.local c.len ft.dom
      cod = V.nom (V.appClo ft.cod (pure x))
      vBody = V.nom (V.app v (pure x))
      Tuple hint body = case abs of
        S.Abs n b -> Tuple n b
        S.AbsConst b -> Tuple "_" b
      bound = case abs of
        S.Abs _ _ -> true
        S.AbsConst _ -> false
    if isType c.len ft.dom then chkD (if bound then extend c Nothing ft.dom else c) body cod vBody
    else do
      n <- freshName hint
      b <- chkD (if bound then extend c (Just (Var n)) ft.dom else c) body cod vBody
      pure (Lam [ n ] b)
  S.Cons d | V.LikeRecord _ <- V.behavior ty -> do
    fields <- A.catMaybes <$> traverse
      ( \(Tuple l fe) -> do
          let fty = V.projTy ty v l
          if isType c.len fty then pure Nothing
          else Just <<< Tuple l <$> chkD c fe fty (V.nom (V.proj v l))
      )
      (dictToList d)
    pure (Record fields)
  _ -> chk c t ty

syn :: Ctx -> S.El -> M (Tuple Expr V.Ty)
syn c = case _ of
  S.Local i -> case L.index c.locals i of
    Just { ir: Just x, ty } -> pure (Tuple x ty)
    Just { ir: Nothing } -> failWith "a type used as a value"
    Nothing -> failWith "an unbound local"
  S.Global x -> case lookupGlobal c.globals x of
    Just e
      | isType 0 e.type_ -> failWith ("the type " <> x <> " used as a value")
      | otherwise -> pure (Tuple (Global x) e.type_)
    Nothing -> failWith ("no global " <> x)
  S.App f a -> do
    Tuple f' fty <- syn c f
    case V.behavior fty of
      V.LikeFunction ft
        | isType c.len ft.dom -> pure (Tuple f' (V.nom (V.appClo ft.cod (defer \_ -> evalIn c a))))
        | otherwise -> do
            a' <- chk c a ft.dom
            pure (Tuple (App f' [ a' ]) (V.nom (V.appClo ft.cod (defer \_ -> evalIn c a))))
      _ -> failWith "applying something that is not a function"
  S.Proj e l -> do
    Tuple e' ety <- syn c e
    pure (Tuple (Proj e' l) (V.projTy ety (evalIn c e) l))
  -- a lambda says its type
  t@(S.Lam ty _) -> do
    let fty = V.nom (evalTy { local: c.env, global: c.globals } N ty)
    t' <- chk c t fty
    pure (Tuple t' fty)
  S.Is t -> syn c t
  S.Rec t -> syn c t
  S.Implicit _ t -> syn c t
  S.LitInt i -> pure (Tuple (Int i) V.PrimInt)
  S.LitString s -> pure (Tuple (Str s) V.PrimString)
  _ -> failWith "this needs an expected type"

chk :: Ctx -> S.El -> V.Ty -> M Expr
chk c t ty = case t of
  S.Is t' -> chk c t' ty
  S.Rec t' -> chk c t' ty
  S.Implicit _ t' -> chk c t' ty
  S.Lam _ abs -> case V.behavior ty of
    V.LikeFunction ft -> do
      let
        x = V.local c.len ft.dom
        cod = V.nom (V.appClo ft.cod (pure x))
        Tuple hint body = case abs of
          S.Abs n b -> Tuple n b
          S.AbsConst b -> Tuple "_" b
        bound = case abs of
          S.Abs _ _ -> true
          S.AbsConst _ -> false
      if isType c.len ft.dom then chk (if bound then extend c Nothing ft.dom else c) body cod
      else do
        v <- freshName hint
        b <- chk (if bound then extend c (Just (Var v)) ft.dom else c) body cod
        pure (Lam [ v ] b)
    _ -> failWith "a function checked against a non-function type"
  S.Prim x -> pure (Prim x)
  S.Tag x args -> case V.behavior ty of
    V.LikeSum (Variants vs) -> case A.find (\(Tuple k _) -> k == x) (dictToList vs) of
      Just (Tuple _ ts) -> Tag x <<< A.catMaybes <$> traverse (\(Tuple a aty) -> if isType c.len aty then pure Nothing else Just <$> chk c a aty) (A.zip args (map force ts))
      Nothing -> failWith ("no tag " <> x)
    _ -> failWith "a tag checked against a non-sum type"
  S.Cons d -> case V.behavior ty of
    V.LikeRecord rt -> do
      let
        go vs fs acc = case A.uncons fs of
          Nothing -> pure acc
          Just { head: Tuple l fe, tail } -> do
            let field = V.typeForProjection rt l { keys: rt.fieldTypes.keys, byName: rt.fieldTypes.byName, values: vs }
            -- a type field is static only
            if isType c.len field.ty then go (A.snoc vs (evalIn c fe)) tail acc
            else do
              fe' <- chk c fe field.ty
              go (A.snoc vs (evalIn c fe)) tail (A.snoc acc (Tuple l fe'))
      Record <$> go [] (dictToList d) []
    _ -> failWith "a record checked against a non-record type"
  S.Match scrut cases -> do
    Tuple s' sty <- syn c scrut
    vs <- case V.behavior sty of
      V.LikeSum (Variants vs) -> pure vs
      _ -> failWith "matching on something that is not of a sum type"
    arms <- traverse
      ( \(Tuple tag (Tuple xs body)) -> do
          let
            argTys = case A.find (\(Tuple k _) -> k == tag) (dictToList vs) of
              Just (Tuple _ ts) -> map force ts
              Nothing -> []
          r <- A.foldM
            ( \acc (Tuple x aty) ->
                if isType acc.c.len aty then pure acc { c = extend acc.c Nothing aty }
                else do
                  v <- freshName x
                  pure { c: extend acc.c (Just (Var v)) aty, vars: A.snoc acc.vars v }
            )
            { c, vars: [] }
            (A.zip xs argTys)
          b <- chk r.c body ty
          pure { tag, vars: r.vars, body: b }
      )
      (dictToList cases)
    pure (Match s' arms)
  S.Abandon -> pure Abandon
  S.Opaque -> failWith "an opaque term"
  S.Code _ -> failWith "a type in a value position"
  _ -> do
    Tuple t' _ <- syn c t
    pure t'
