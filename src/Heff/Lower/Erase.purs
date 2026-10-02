-- | Core to the untyped IR: what is a type is erased -- not replaced by a
-- | placeholder, but gone, with the parameter that took it and the argument
-- | given for it.  Which is which is read off the types (Hippogriff's phase
-- | distinction: a small type is never computed from a value), so the core
-- | is checked again, with the elaborator's NbE.
-- |
-- | A term is a type when its type classifies types: a universe (`Type`,
-- | `Theory`), or a function into one.  So `(a :: Type) -> ...` loses its
-- | parameter, `Monoid` and `List` are no definitions at all, and a record's
-- | type fields are not in it.
module Heff.Lower.Erase
  ( eraseProgram
  , isType
  ) where

import Prelude

import Control.Monad.State (StateT, get, put, runStateT)
import Control.Monad.Trans.Class (lift)
import Data.Array as A
import Data.Either (Either(..))
import Data.Lazy (Lazy, defer, force)
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Traversable (traverse)
import Heff.Core.Readback (readbTy)
import Heff.Core.Rename (mentionsInnermost)
import Data.Tuple (Tuple(..))
import Heff.Core.Evaluation (eval, evalTy)
import Heff.Core.Globals (Globals, GlobalEntry, globalsList, lookupGlobal)
import Heff.Core.Params (Case(..), Variants(..))
import Heff.Core.Syntax as S
import Heff.Core.Value as V
import Heff.Lower.IR (Def, Expr(..), Supply, fresh)
import Heff.Support.Common (Dict, Name, dictToList, dictAt, dictKeyIndex)
import Heff.Elab.Environment (tryPure)

type M a = StateT Supply (Either String) a

-- | a local: what it is in the IR (nothing, if it was a type), its type
type Local = { ir :: Maybe Expr, ty :: V.Ty }

type Ctx = { locals :: List Local, env :: V.Locals, len :: Int, globals :: Globals }

failWith :: forall a. String -> M a
failWith = lift <<< Left

-- | the computation, or `Nothing` if it fails (the supply advances only
-- | when it succeeds)
try :: forall a. M a -> M (Maybe a)
try m = do
  n <- get
  case runStateT m n of
    Left _ -> pure Nothing
    Right (Tuple a n') -> put n' *> pure (Just a)

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
          fty <- projTyIn c.len ty v l
          if isType c.len fty then pure Nothing
          else Just <<< Tuple l <$> chkD c fe fty (V.nom (V.proj v l))
      )
      (dictToList d)
    pure (Record fields)
  _ -> chk c t ty

-- | The type of field `l`: through the value's fields where they are in
-- | hand; else -- the value is opaque here, computed through a match --
-- | each field before it a local of its own, if the type depends on
-- | none of them (as the elaborator's `fieldTyApart` reads it).
projTyIn :: Int -> V.Ty -> V.El -> Name -> M V.Ty
projTyIn len t v l = case V.projTyMaybe t v l of
  Just ty -> pure ty
  Nothing -> case V.behavior t of
    V.LikeRecord rt -> case fieldTyApart len rt l of
      Just ty -> pure ty
      Nothing -> failWith ("the type of `." <> l <> "` depends on fields not in hand here")
    _ -> failWith "projecting from something that is not of a record type"

fieldTyApart :: Int -> V.RecordType -> Name -> Maybe V.Ty
fieldTyApart len rt x =
  let
    i = dictKeyIndex rt.fieldTypes x
    fieldAt j vs = (dictAt rt.fieldTypes j) (V.LChunkSnoc rt.capture vs)
    before = A.foldl (\vs j -> let f = fieldAt j vs in A.snoc vs (fromMaybe (V.local (len + j) f.ty) f.value)) [] (if i == 0 then [] else A.range 0 (i - 1))
    ty = (fieldAt i before).ty
  in
    if mentionsInnermost i (readbTy (len + i) ty) then Nothing else Just ty

matchVariants :: V.Ty -> M (Dict (Array (Lazy V.Ty)))
matchVariants sty = case V.behavior sty of
  V.LikeSum (Variants vs) -> pure vs
  _ -> failWith "matching on something that is not of a sum type"

-- (an alternative's context: its pattern's variables bound, the type
-- arguments erased)
armBind :: Ctx -> Dict (Array (Lazy V.Ty)) -> Name -> Array Name -> M { c :: Ctx, vars :: Array String }
armBind c vs tag xs =
  A.foldM
    ( \acc (Tuple x aty) ->
        if isType acc.c.len aty then pure acc { c = extend acc.c Nothing aty }
        else do
          v <- freshName x
          pure { c: extend acc.c (Just (Var v)) aty, vars: A.snoc acc.vars v }
    )
    { c, vars: [] }
    (A.zip xs argTys)
  where
  argTys = case A.find (\(Tuple k _) -> k == tag) (dictToList vs) of
    Just (Tuple _ ts) -> map force ts
    Nothing -> []

arm :: Ctx -> Dict (Array (Lazy V.Ty)) -> V.Ty -> Tuple Name (Tuple (Array Name) S.El) -> M { tag :: Name, vars :: Array String, body :: Expr }
arm c vs ty (Tuple tag (Tuple xs body)) = do
  r <- armBind c vs tag xs
  b <- chk r.c body ty
  pure { tag, vars: r.vars, body: b }

-- (the type of a match nothing expects: the first alternative's that can
-- say it -- `a && b` desugars to such a match)
armsTy :: Ctx -> Dict (Array (Lazy V.Ty)) -> Array (Tuple Name (Tuple (Array Name) S.El)) -> M V.Ty
armsTy c vs alts = case A.uncons alts of
  Nothing -> failWith "this needs an expected type"
  Just { head: Tuple tag (Tuple xs body), tail } -> do
    r <- armBind c vs tag xs
    m <- try (syn r.c body)
    case m of
      Just (Tuple _ ty) -> pure ty
      Nothing -> armsTy c vs tail

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
    fty <- projTyIn c.len ety (evalIn c e) l
    pure (Tuple (Proj e' l) fty)
  -- a lambda says its type
  t@(S.Lam ty _) -> do
    let fty = V.nom (evalTy { local: c.env, global: c.globals } N ty)
    t' <- chk c t fty
    pure (Tuple t' fty)
  S.Is t -> syn c t
  S.Rec t -> syn c t
  S.Implicit _ t -> syn c t
  S.LitInt i -> pure (Tuple (Int i) V.PrimInt)
  S.LitFloat x -> pure (Tuple (Float x) V.PrimFloat)
  S.LitString s -> pure (Tuple (Str s) V.PrimString)
  -- a match where a value is expected and no type given (`a && b`
  -- desugars to one): its type is the first alternative's that says it
  S.Match scrut cases -> do
    Tuple s' sty <- syn c scrut
    vs <- matchVariants sty
    ty <- armsTy c vs (dictToList cases)
    arms <- traverse (arm c vs ty) (dictToList cases)
    pure (Tuple (Match s' arms) ty)
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
    vs <- matchVariants sty
    arms <- traverse (arm c vs ty) (dictToList cases)
    pure (Match s' arms)
  S.Abandon -> pure Abandon
  S.Opaque -> failWith "an opaque term"
  S.Code _ -> failWith "a type in a value position"
  _ -> do
    Tuple t' _ <- syn c t
    pure t'
