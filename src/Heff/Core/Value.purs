-- | Values.  A port of `Hippogriff.Core.Value`.
-- |
-- | The reference implementation leans on Haskell's laziness in a few places
-- | (fields marked `~`): the environment, a neutral's type, its record
-- | expansion and its description.  Those are `Lazy` here; everything else is
-- | strict, as it is there under `StrictData`.
module Heff.Core.Value where

import Prelude

import Data.Array as A
import Data.Lazy (Lazy, defer, force)
import Data.Maybe (Maybe(..))
import Data.Map (Map)
import Data.Map as Map
import Heff.Core.Syntax as S
import Data.Tuple (Tuple(..))
import Heff.Support.Common (Dict, Name, dictAt, dictElemAt, dictKeyIndex, dictMap, dictUpdate, panic)
import Heff.Core.Params (Case(..), Level(..), Universe, Variants, codesInto, decodesInto)

-- Value environments
--------------------------------------------------------------------------------

data Locals = LNil | LSnoc Locals (Lazy El) | LChunkSnoc Locals (Array El)

localsAt :: Locals -> Int -> El
localsAt LNil _ = panic "no such index"
localsAt (LSnoc e v) i
  | i == 0 = force v
  | otherwise = localsAt e (i - 1)
localsAt (LChunkSnoc e vs) i =
  let n = A.length vs
  in
    if i < n then case A.index vs (n - i - 1) of
      Just v -> v
      Nothing -> panic "no such index"
    else localsAt e (i - n)

snoc :: Locals -> El -> Locals
snoc vs v = LSnoc vs (pure v)

-- Evaluation results
--------------------------------------------------------------------------------

-- | `Evaluation f c`: at case N it is just `f N` (here `Nom`); at case D it is
-- | a description (`Describe`) or a definition that turned out to be an
-- | ordinary element after all (`Become`).
data Ev a = Nom a | Describe a | Become a

nom :: forall a. Ev a -> a
nom = case _ of
  Nom x -> x
  Become x -> x
  Describe _ -> panic "expected a nominative evaluation"

epure :: forall a. Case -> a -> Ev a
epure N x = Nom x
epure D x = Describe x

emap :: forall a b. (a -> b) -> Ev a -> Ev b
emap f = case _ of
  Nom x -> Nom (f x)
  Describe x -> Describe (f x)
  Become x -> Become (f x)

-- | `f` is used at case N for `Become` (whose payload is nominative)
ebind :: forall a b. (a -> Ev b) -> Ev a -> Ev b
ebind f = case _ of
  Nom x -> f x
  Describe x -> f x
  Become x -> Become (nom (f x))

-- Closures
--------------------------------------------------------------------------------

data Clo a
  = Clo Name Locals (Locals -> Ev a)
  | CloConst (Ev a)

-- | the argument is lazy, as it is in the reference: a closure that ignores
-- | its argument never evaluates it
appClo :: forall a. Clo a -> Lazy El -> Ev a
appClo (Clo _ vs body) v = body (LSnoc vs v)
appClo (CloConst body) _ = body

cloName :: forall a. Clo a -> Maybe Name
cloName (Clo n _ _) = Just n
cloName (CloConst _) = Nothing

-- Neutrals
--------------------------------------------------------------------------------

-- | a local is a de Bruijn *level*
-- | a local is a de Bruijn level; a meta is an implicit argument being
-- | inferred (see `Heff.Elab.Unify`)
data Head = Local Int | Global Name | Meta Int

derive instance eqHead :: Eq Head

data Spine
  = Id
  | App Spine El
  | Proj Spine Name
  | Specialize Spine Name El

data Expansion = IntoCons (Dict El) | NotApplicable

type Neutral =
  { head :: Head
  , spine :: Spine
  , ty :: Lazy Ty
  , expansion :: Lazy Expansion
  , description :: Lazy (Maybe El)
  }

expandRecord :: RecordType -> Head -> Spine -> Maybe El -> Dict El
expandRecord rt head spine desc =
  { keys: rt.fieldTypes.keys
  , byName: rt.fieldTypes.byName
  , values: go rt.capture (A.zip rt.fieldTypes.keys rt.fieldTypes.values) []
  }
  where
  go vs fs acc = case A.uncons fs of
    Nothing -> acc
    Just { head: Tuple x' f, tail: rest } ->
      let
        field = f vs
        v = case field.value of
          Just v' -> v'
          Nothing -> reflect head (pure (Proj spine x')) (pure field.ty) (map (\d -> proj d x') desc)
      in
        go (snoc vs v) rest (A.snoc acc v)

-- | Make a neutral, or, when the description says the name is really just an
-- | element (`Become`), that element.  The spine is only built in the first
-- | case, so a `Become` never forces the arguments that led to it.
reflect :: Head -> Lazy Spine -> Lazy Ty -> Maybe (Ev El) -> El
reflect head spineL ty edesc = case edesc of
  Just (Describe desc) -> k (Just desc)
  Just (Become v) -> v
  Just (Nom v) -> v
  Nothing -> k Nothing
  where
  k desc =
    let
      spine = force spineL
      expansion = defer \_ -> case behavior (force ty) of
        LikeRecord recordType -> IntoCons (expandRecord recordType head spine desc)
        _ -> NotApplicable
    in
      Neu { head, spine, ty, expansion, description: pure desc }

local :: Int -> Ty -> El
local i a = reflect (Local i) (pure Id) (pure a) Nothing

localL :: Int -> Lazy Ty -> El
localL i a = reflect (Local i) (pure Id) a Nothing

-- | `el` is the element this type is the decoding of, so that encoding it
-- | again gives that element back (`code (decode e) = e`, see `code`)
type DecodedNeutral =
  { head :: Head
  , spine :: Spine
  , universe :: Universe
  , behavior :: Lazy TypeBehavior
  , el :: Lazy El
  }

type BareNeutral = { head :: Head, spine :: Spine }

toBare :: forall r. { head :: Head, spine :: Spine | r } -> BareNeutral
toBare n = { head: n.head, spine: n.spine }

-- Elements
--------------------------------------------------------------------------------

data El
  = Neu Neutral
  | Code Ty
  -- | annotated with its whole function type, as `S.Lam` is (see `lamDom`)
  | Lam (Lazy Ty) (Clo El)
  | Cons (Dict (Ev El))
  | Opaque

app :: El -> Lazy El -> Ev El
app (Lam _ clo) arg = appClo clo arg
app (Neu n) arg =
  Nom (reflect n.head (defer \_ -> App n.spine (force arg)) (defer \_ -> appTy (force n.ty) arg) (map (\d -> app d arg) (force n.description)))
app Opaque _ = Nom Opaque
app _ _ = panic "ill-typed application"

coerceToFields :: El -> Dict (Ev El)
coerceToFields (Cons fields) = fields
coerceToFields (Neu n) = case force n.expansion of
  IntoCons fields -> dictMap Nom fields
  _ -> panic "unexpanded neutral of record type"
coerceToFields _ = panic "ill-typed projection"

proj :: El -> Name -> Ev El
proj Opaque _ = Nom Opaque
proj v x = dictElemAt (coerceToFields v) x

specialize :: El -> Name -> El -> El
specialize (Neu n) x f = case force n.description of
  Just (Code (Record rt)) ->
    Neu n
      { spine = Specialize n.spine x f
      , description = pure (Just (Code (Record (addSpecialization rt x f))))
      }
  _ -> panic "can only specialize record type"
specialize _ _ _ = panic "can only specialize neutral"

-- Metavariables
--------------------------------------------------------------------------------

-- | An implicit argument being inferred.  The metacontext is a value, local
-- | to the elaboration of one definition: it is threaded through that
-- | elaboration, zonked away at its end, and never shared, so definitions
-- | elaborate independently of one another.
-- | `solStx` is the solution as syntax, closed, under one binder per local
-- | the metavariable was applied to (what zonking substitutes)
type MetaEntry = { ty :: Ty, sol :: Maybe El, solStx :: Maybe S.El, arity :: Int, name :: Name, span :: { start :: Int, end :: Int } }

type MetaCtx = Map Int MetaEntry

-- | a metavariable applied to the locals in scope where it was created: a
-- | flexible neutral, of the type it was created at
metaEl :: Int -> Spine -> Ty -> El
metaEl m sp ty = reflect (Meta m) (pure sp) (pure ty) Nothing

replay :: El -> Spine -> El
replay v = case _ of
  Id -> v
  App sp a -> nom (app (replay v sp) (pure a))
  Proj sp l -> nom (proj (replay v sp) l)
  Specialize sp l f -> specialize (replay v sp) l f

-- | a solved metavariable at the head replaced by its solution
forceEl :: MetaCtx -> El -> El
forceEl mc v = case v of
  Neu n -> case n.head of
    Meta m -> case Map.lookup m mc >>= _.sol of
      Just s -> forceEl mc (replay s n.spine)
      Nothing -> v
    _ -> v
  _ -> v

forceTy :: MetaCtx -> Ty -> Ty
forceTy mc t = case t of
  Decode n -> case n.head of
    Meta m -> case Map.lookup m mc >>= _.sol of
      Just s -> forceTy mc (decodeN (forceEl mc (replay s n.spine)))
      Nothing -> t
    _ -> t
  _ -> t

-- Types
--------------------------------------------------------------------------------

type FunctionType = { level :: Level, dom :: Ty, cod :: Clo Ty, implicit :: Boolean }

type Field = { ty :: Ty, value :: Maybe El }

type RecordType =
  { level :: Level
  , capture :: Locals
  , fieldTypes :: Dict (Locals -> Field)
  }

addSpecialization :: RecordType -> Name -> El -> RecordType
addSpecialization rt x v =
  rt { fieldTypes = dictUpdate rt.fieldTypes x (\field -> \vs -> (field vs) { value = Just v }) }

-- | the argument types are lazy: a recursive type mentions itself here
type SumType = Variants (Lazy Ty)

typeForProjection :: RecordType -> Name -> Dict El -> Field
typeForProjection rt x fields =
  let
    i = dictKeyIndex rt.fieldTypes x
    chunk = A.slice 0 i fields.values
    locals = LChunkSnoc rt.capture chunk
  in
    (dictAt rt.fieldTypes i) locals

data Ty
  = U Universe
  | Decode DecodedNeutral
  | Function FunctionType
  | Record RecordType
  | Sum SumType
  | PrimInt
  | PrimString

levelOf :: Ty -> Level
levelOf = case _ of
  U u -> codesInto u
  Decode n -> decodesInto n.universe
  Function ft -> ft.level
  Record rt -> rt.level
  Sum _ -> Small
  PrimInt -> Small
  PrimString -> Small

levelOfEv :: Ev Ty -> Level
levelOfEv = case _ of
  Nom t -> levelOf t
  Describe t -> levelOf t
  Become t -> levelOf t

data TypeBehavior
  = LikeU Universe
  | LikeFunction FunctionType
  | LikeRecord RecordType
  | LikeSum SumType
  | LikeInt
  | LikeString
  | NoRules

behavior :: Ty -> TypeBehavior
behavior = case _ of
  U u -> LikeU u
  Decode n -> force n.behavior
  Function ft -> LikeFunction ft
  Record rt -> LikeRecord rt
  Sum st -> LikeSum st
  PrimInt -> LikeInt
  PrimString -> LikeString

-- | the domain of a lambda's function type annotation; a metavariable
-- | solution's lambdas are annotated with a placeholder universe instead, which
-- | is taken as the domain itself
lamDom :: Ty -> Ty
lamDom t = case behavior t of
  LikeFunction ft -> ft.dom
  _ -> t

-- | decoding an element of case N
decodeN :: El -> Ty
decodeN (Code a) = a
decodeN (Neu n) =
  let
    u = case force n.ty of
      U u' -> u'
      _ -> panic "ill-typed decoding"
    k desc = Decode { head: n.head, spine: n.spine, universe: u, behavior: desc, el: pure (Neu n) }
  in
    case map decodeD (force n.description) of
      Just (Describe desc) -> k (defer \_ -> behavior desc)
      Just (Become ty) -> ty
      Just (Nom ty) -> ty
      Nothing -> k (pure NoRules)
decodeN _ = panic "ill-typed decoding"

-- | Encoding a type as an element of its universe.  A decoded neutral
-- | encodes back to the neutral itself, so values stay normal: there is no
-- | `Code (Decode e)`, and no η rule for it is needed in conversion.
code :: Ty -> El
code = case _ of
  Decode dn -> force dn.el
  t -> Code t

-- | decoding a description
decodeD :: El -> Ev Ty
decodeD (Code a) = Describe a
decodeD _ = panic "ill-typed decoding of a description"

decodeEv :: Ev El -> Ev Ty
decodeEv = case _ of
  Nom x -> Nom (decodeN x)
  Describe x -> decodeD x
  Become x -> Become (decodeN x)

appTy :: Ty -> Lazy El -> Ty
appTy t arg = case behavior t of
  LikeFunction ft -> nom (appClo ft.cod arg)
  _ -> panic "ill-typed application"

projTy :: Ty -> El -> Name -> Ty
projTy t v x = case behavior t of
  LikeRecord rt -> (typeForProjection rt x (dictMap nom (coerceToFields v))).ty
  _ -> panic "ill-typed projection"
