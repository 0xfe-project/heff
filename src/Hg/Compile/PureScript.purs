-- | Compiling Hippogriff to PureScript source.
-- |
-- | The phase distinction is what makes a *typed* translation possible: a
-- | small type is never computed from a value, so every type argument can be
-- | erased and every `(A : Type) -> ...` becomes a PureScript `forall`.
-- |
-- | | Hippogriff                                   | PureScript                         |
-- | | -------------------------------------------- | ---------------------------------- |
-- | | `def List (A : Type) : Type := sum ... end`  | `data List a = List'nil | ...`     |
-- | | `def Env : Type := Bwd Val`                  | `type Env = Bwd Val`               |
-- | | `(A : Type) -> A -> A`                       | `forall a. a -> a`                 |
-- | | a `sig` with small, independent fields       | a record type                      |
-- | | `struct`, `.l`, `match`, `'tag`              | record, `.l`, `case`, constructor  |
-- | | `abandon`                                    | `unsafeCrashWith "abandon"`        |
-- |
-- | The translation is type-directed -- which data type a `'nil` belongs to,
-- | and which arguments are types, is read off the types -- so it re-checks
-- | the elaborated core with the same NbE the elaborator uses.
-- |
-- | Not yet translated (reported as errors, never silently approximated):
-- | theories used as values (`M.t`, record types with type fields),
-- | specialization, sums defined inside a `struct`, and anything whose type
-- | mentions a value.  The generated code is call-by-value.
module Hg.Compile.PureScript
  ( compileModule
  , CompileResult
  , psValueName
  , psTypeName
  ) where

import Prelude

import Data.Array as A
import Data.Either (Either(..))
import Data.Foldable (foldl)
import Data.Lazy (defer, force)
import Data.List (List(..), (:))
import Data.List as L
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Traversable (traverse)
import Data.Tuple (Tuple(..), fst, snd)
import Hg.Support.Common (Name, dictToList, dictKeyIndex)
import Hg.Core.Evaluation (eval)
import Hg.Core.Globals (Globals, GlobalEntry, globalsList, lookupGlobal)
import Hg.Core.Params (Case(..), Universe(..), Variants(..))
import Hg.Core.Readback (readbTy)
import Hg.Core.Syntax as S
import Hg.Elab.Prims (prims)
import Hg.Core.Value as V

type CompileResult = { source :: String, errors :: Array String, skipped :: Array String }

-- Names
--------------------------------------------------------------------------------

reserved :: Array String
reserved =
  [ "ado", "as", "case", "class", "data", "derive", "do", "else", "false", "forall", "foreign"
  , "hiding", "import", "if", "in", "infix", "infixl", "infixr", "instance", "let", "module"
  , "newtype", "of", "then", "true", "type", "where", "unit"
  ]

primTypes :: Array String
primTypes =
  [ "Int", "String", "Boolean", "Number", "Char", "Array", "Record", "Function", "Type"
  , "Row", "Partial", "Symbol", "Constraint"
  ]

isIdentChar :: Char -> Boolean
isIdentChar c = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '\''

-- | `Bwd/elemAt` -> `bwd'elemAt` style: `/` and `-` become `'` and `_`
mangle :: String -> String
mangle x = SCU.fromCharArray (map f (SCU.toCharArray x))
  where
  f '/' = '\''
  f '-' = '_'
  f c = if isIdentChar c then c else '_'

lowerFirst :: String -> String
lowerFirst s = S.toLower (S.take 1 s) <> S.drop 1 s

upperFirst :: String -> String
upperFirst s = S.toUpper (S.take 1 s) <> S.drop 1 s

-- | a Hippogriff value name as a PureScript value name (lowercase first)
psValueName :: String -> String
psValueName x =
  let
    m = mangle x
    lower = S.take 1 m == S.toLower (S.take 1 m) && S.take 1 m /= "_" && S.take 1 m /= "'"
    base = if lower then m else "hg'" <> m
  in
    if A.elem base reserved then base <> "'" else base

-- | a Hippogriff type name as a PureScript type name (uppercase first)
psTypeName :: String -> String
psTypeName x =
  let base = upperFirst (mangle x)
  in if A.elem base primTypes then base <> "'" else base

-- | a constructor, from the PureScript name of its type and the tag
constructorName :: String -> String -> String
constructorName psTy tag = psTy <> "'" <> mangle tag

-- | global keys that are hashes (`#...`) are named by their display name
type Names = { value :: Name -> String, typ :: Name -> String }

-- Types
--------------------------------------------------------------------------------

-- | what a de Bruijn index stands for, when translating a type
data TEntry = TVar String | TSelf String | TValue

type TCtx = List TEntry

type E a = Either String a

isSmallU :: S.Ty -> Boolean
isSmallU (S.U SmallU) = true
isSmallU (S.IsTy t) = isSmallU t
isSmallU _ = false

freshVar :: TCtx -> String -> String
freshVar ctx hint =
  let
    base0 = lowerFirst (mangle hint)
    base = if base0 == "" || base0 == "_" || A.elem base0 reserved then "a" else base0
    used = L.mapMaybe (case _ of
      TVar v -> Just v
      _ -> Nothing) ctx
    go i = let v = if i == 0 then base else base <> show i in if L.elem v used then go (i + 1) else v
  in
    go 0

tyPS :: Names -> TCtx -> S.Ty -> E String
tyPS nm ctx = case _ of
  S.U _ -> Left "a universe is not a runtime type"
  S.Decode e -> elTyPS nm ctx e
  S.IsTy t -> tyPS nm ctx t
  S.PrimInt -> pure "HgBI.BigInt"
  S.PrimString -> pure "String"
  S.Function f
    | isSmallU f.dom -> case f.cod of
        S.Abs x b -> do
          let v = freshVar ctx x
          body <- tyPS nm (TVar v : ctx) b
          pure ("(forall " <> v <> ". " <> body <> ")")
        S.AbsConst b -> tyPS nm ctx b
    | otherwise -> do
        d <- tyPS nm ctx f.dom
        c <- case f.cod of
          S.Abs _ b -> tyPS nm (TValue : ctx) b
          S.AbsConst b -> tyPS nm ctx b
        pure ("(" <> d <> " -> " <> c <> ")")
  S.Record r -> do
    let
      go c fs = case A.uncons fs of
        Nothing -> pure []
        Just { head: Tuple l t, tail } -> do
          when (isSmallU t) (Left ("record field `" <> l <> "` is a type (theories are not compiled yet)"))
          tt <- tyPS nm c t
          rest <- go (TValue : c) tail
          pure (A.cons (psValueName l <> " :: " <> tt) rest)
    fields <- go ctx (dictToList r.fieldTypes)
    pure ("{ " <> S.joinWith ", " fields <> " }")
  S.Sum _ -> Left "an anonymous `sum` (only a named sum becomes a data type)"

-- | an element of `Type`, used as a type
elTyPS :: Names -> TCtx -> S.El -> E String
elTyPS nm ctx = case _ of
  S.Local i -> case L.index ctx i of
    Just (TVar v) -> pure v
    Just (TSelf n) -> pure n
    _ -> Left "a type that depends on a value"
  S.Global x -> pure (nm.typ x)
  S.Code t -> tyPS nm ctx t
  S.Is t -> elTyPS nm ctx t
  e@(S.App _ _) -> do
    let Tuple h args = spine e
    hd <- elTyPS nm ctx h
    as <- traverse (elTyPS nm ctx) args
    pure ("(" <> S.joinWith " " (A.cons hd as) <> ")")
  S.Proj _ l -> Left ("a type projected from a module (`." <> l <> "`); theories are not compiled yet")
  _ -> Left "a type computed in a way the translation does not handle"
  where
  spine = go []
    where
    go acc (S.App f a) = go (A.cons a acc) f
    go acc f = Tuple f acc

-- Elements
--------------------------------------------------------------------------------

-- | a local: its PureScript name (Nothing for an erased type variable), its
-- | type, and the value standing for it during type computations
type Local = { name :: Maybe String, ty :: V.Ty }

type VCtx =
  { locals :: List Local -- ^ innermost first
  , env :: V.Locals
  , len :: Int
  , globals :: Globals
  , names :: Names
  , used :: List String
  }

extend :: VCtx -> Maybe String -> V.Ty -> VCtx
extend c n ty =
  { locals: { name: n, ty } : c.locals
  , env: V.snoc c.env (V.local c.len ty)
  , len: c.len + 1
  , globals: c.globals
  , names: c.names
  , used: case n of
      Just x -> x : c.used
      Nothing -> c.used
  }

freshLocal :: VCtx -> String -> String
freshLocal c hint =
  let
    base0 = lowerFirst (mangle hint)
    base1 = if base0 == "" || base0 == "_" then "x" else base0
    base = if A.elem base1 reserved then base1 <> "'" else base1
    go i = let v = if i == 0 then base else base <> show i in if L.elem v c.used then go (i + 1) else v
  in
    go 0

evalIn :: VCtx -> S.El -> V.El
evalIn c t = V.nom (eval { local: c.env, global: c.globals } N t)

-- | a parameter whose argument is a type (or a type constructor) is erased
isTypeDom :: V.Ty -> Boolean
isTypeDom = classifiesTypes 1000000

-- | When `av` (a module passed at the theory type `dom`) has type fields that
-- | are type-level lambdas, the PureScript type of `av` with those fields
-- | abstract: its other type fields are their static values, and its value
-- | fields are typed through the theory, so they mention the phantom types.
abstractArg :: VCtx -> V.El -> V.Ty -> Maybe String
abstractArg c av dom =
  let
    paths = typeFieldPaths c.len "" av dom
    named = A.mapMaybe (\p -> Tuple p <$> typeLambdaName c.names (projPath av p)) paths
    te0 = { vars: Map.empty, used: Nil, names: c.names, len: c.len }
  in
    if A.null named then Nothing
    else
      let
        -- every type field: the phantom name, or the static type
        fieldTy p = case map snd (A.find (\(Tuple q _) -> q == p) named) of
          Just n -> Right n
          Nothing -> tyV te0 (V.decodeN (projPath av p))
        x = V.local c.len dom
      in
        case traverse (\p -> Tuple p <$> fieldTy p) paths of
          Left _ -> Nothing
          Right fields ->
            let te = te0 { vars = Map.insert c.len (TheoryVar (Map.fromFoldable fields)) Map.empty, len = c.len + 1 }
            in case sigOf te x dom of
              Right t -> Just t
              Left _ -> Nothing

-- | a descriptive field of a struct: a fresh type, not a value
isDescription :: S.El -> Boolean
isDescription = case _ of
  S.Code (S.Sum _) -> true
  S.Code (S.Record _) -> true
  S.Lam _ (S.Abs _ b) -> isDescription b
  S.Lam _ (S.AbsConst b) -> isDescription b
  S.Cons d -> A.any isDescription d.values
  _ -> false

-- | Check a definition's own (descriptive) syntax, whose value `v` is known:
-- | under its lambdas and through its struct fields the value is followed
-- | along, so a field that defines a type is never evaluated as syntax.
chkD :: VCtx -> S.El -> V.Ty -> V.El -> E String
chkD c t ty v = case t of
  S.Is t' -> chk c t' ty
  S.Lam _ abs -> case V.behavior ty of
    V.LikeFunction ft -> do
      let
        hint = case abs of
          S.Abs x _ -> x
          S.AbsConst _ -> "_"
        body = case abs of
          S.Abs _ b -> b
          S.AbsConst b -> b
        bound = case abs of
          S.Abs _ _ -> true
          S.AbsConst _ -> false
        x = V.local c.len ft.dom
        vBody = V.nom (V.app v (pure x))
        tyBody = V.nom (V.appClo ft.cod (pure x))
      if isTypeDom ft.dom then
        chkD (if bound then extend c Nothing ft.dom else c) body tyBody vBody
      else do
        let
          n = freshLocal c hint
          c' = if bound then extend c (Just n) ft.dom else c { used = n : c.used }
        b <- chkD c' body tyBody vBody
        pure (paren ("\\" <> n <> " -> " <> b))
    _ -> Left "a function checked against a non-function type"
  S.Cons d -> case V.behavior ty of
    V.LikeRecord _ -> do
      fields <- A.catMaybes <$> traverse
        ( \(Tuple l fe) -> do
            let
              fty = V.projTy ty v l
              fv = V.nom (V.proj v l)
            if isTypeDom fty then pure Nothing
            else Just <<< (\e -> psValueName l <> ": " <> e) <$> chkD c fe fty fv
        )
        (dictToList d)
      pure ("{ " <> S.joinWith ", " fields <> " }")
    _ -> Left "a struct checked against a non-record type"
  _ -> chk c t ty

-- | the PureScript data type a sum-typed value belongs to
dataOf :: Names -> V.Ty -> E String
dataOf nm = case _ of
  V.Decode n -> case n.head of
    V.Global x ->
      let path = A.mapMaybe (case _ of
            SProj l -> Just l
            _ -> Nothing) (spineItems n.spine)
      in pure (if A.null path then nm.typ x else nestedName nm x path)
    _ -> Left "a value of a sum type whose name is a variable"
  _ -> Left "a sum type that is not a named global"

paren :: String -> String
paren s = "(" <> s <> ")"

syn :: VCtx -> S.El -> E (Tuple String V.Ty)
syn c = case _ of
  S.Local i -> case L.index c.locals i of
    Just { name: Just n, ty } -> pure (Tuple n ty)
    Just { name: Nothing } -> Left "a type variable used as a value"
    Nothing -> Left "unbound local"
  S.Global x -> case lookupGlobal c.globals x of
    Just e -> pure (Tuple (c.names.value x) e.type_)
    Nothing -> Left ("unknown global " <> x)
  S.App f a -> do
    Tuple f' fty <- syn c f
    case V.behavior fty of
      V.LikeFunction ft
        | isTypeDom ft.dom -> pure (Tuple f' (V.nom (V.appClo ft.cod (defer \_ -> evalIn c a))))
        | otherwise -> do
            a' <- chk c a ft.dom
            let
              av = evalIn c a
              resTy = V.nom (V.appClo ft.cod (pure av))
              plain = Tuple (paren (f' <> " " <> paren a')) resTy
            case abstractArg c av ft.dom of
              Nothing -> pure plain
              Just absTy -> do
                -- the module argument is coerced to the form where its
                -- type-lambda fields are phantom types, and the result back
                -- to the form the Hippogriff types say (they β-reduce)
                let resVal = V.nom (V.app (evalIn c f) (pure av))
                resPs <- sigOf { vars: Map.empty, used: Nil, names: c.names, len: c.len } resVal resTy
                pure (Tuple (paren ("HgRTC.unsafeCoerce (" <> f' <> " (HgRTC.unsafeCoerce " <> paren a' <> " :: " <> absTy <> ")) :: " <> resPs)) resTy)
      _ -> Left "applying something that is not a function"
  S.Proj e l -> do
    Tuple e' ety <- syn c e
    pure (Tuple (paren e' <> "." <> psValueName l) (V.projTy ety (evalIn c e) l))
  S.Is t -> syn c t
  S.Rec t -> syn c t
  S.Implicit _ t -> syn c t
  S.Specialize _ _ _ -> Left "specialization (`~ [...]`) is not compiled yet"
  S.LitInt i -> pure (Tuple (paren ("HgBI.fromDigits " <> show (show i))) V.PrimInt)
  S.LitString s -> pure (Tuple (show s) V.PrimString)
  _ -> Left "this needs an expected type"

chk :: VCtx -> S.El -> V.Ty -> E String
chk c t ty = case t of
  S.Is t' -> chk c t' ty
  S.Rec t' -> chk c t' ty
  S.Implicit _ t' -> chk c t' ty
  S.Lam _ abs -> case V.behavior ty of
    V.LikeFunction ft ->
      let
        hint = case abs of
          S.Abs x _ -> x
          S.AbsConst _ -> "_"
        body = case abs of
          S.Abs _ b -> b
          S.AbsConst b -> b
        bound = case abs of
          S.Abs _ _ -> true
          S.AbsConst _ -> false
      in
        if isTypeDom ft.dom then do
          let c' = extend c Nothing ft.dom
          let v = V.local c.len ft.dom
          if bound then chk c' body (V.nom (V.appClo ft.cod (pure v)))
          else chk c body (V.nom (V.appClo ft.cod (pure v)))
        else do
          let
            n = freshLocal c hint
            c' = extend c (Just n) ft.dom
            v = V.local c.len ft.dom
          b <- if bound then chk c' body (V.nom (V.appClo ft.cod (pure v)))
            else chk (c { used = n : c.used }) body (V.nom (V.appClo ft.cod (pure v)))
          pure (paren ("\\" <> n <> " -> " <> b))
    _ -> Left "a function checked against a non-function type"
  S.Prim x -> case A.find (\(Tuple k _) -> k == x) prims of
    Nothing -> Left ("no primitive " <> x)
    Just (Tuple _ p) ->
      -- the answer type, after `arity` arguments: 'true/'false name its tags
      let
        answer = A.foldl (\t i -> case V.behavior t of
          V.LikeFunction ft -> V.nom (V.appClo ft.cod (pure (V.local (c.len + i) ft.dom)))
          _ -> t) ty (A.range 0 (p.arity - 1))
        tags = case V.behavior answer of
          V.LikeSum _ -> case dataOf c.names answer of
            Right d -> { t: constructorName d "True", f: constructorName d "False" }
            Left _ -> { t: "true", f: "false" }
          _ -> { t: "true", f: "false" }
      in pure (paren (S.replaceAll (S.Pattern "TRUE") (S.Replacement tags.t) (S.replaceAll (S.Pattern "FALSE") (S.Replacement tags.f) p.ps)))
  S.Tag x args -> case V.behavior ty of
    V.LikeSum (Variants vs) -> do
      d <- dataOf c.names ty
      ts <- case A.find (\(Tuple k _) -> k == x) (dictToList vs) of
        Just (Tuple _ ts) -> pure (map force ts)
        Nothing -> Left ("no tag " <> x)
      args' <- traverse (\(Tuple a aty) -> chk c a aty) (A.zip args ts)
      pure (paren (S.joinWith " " (A.cons (constructorName d x) (map paren args'))))
    _ -> Left "a tag checked against a non-sum type"
  S.Cons d -> case V.behavior ty of
    V.LikeRecord rt -> do
      let
        go vs fs acc = case A.uncons fs of
          Nothing -> pure acc
          Just { head: Tuple l fe, tail } -> do
            let field = (V.typeForProjection rt l { keys: rt.fieldTypes.keys, byName: rt.fieldTypes.byName, values: vs })
            -- a type field is static only: it has no runtime component
            if isTypeDom field.ty then go (A.snoc vs (evalIn c fe)) tail acc
            else do
              fe' <- chk c fe field.ty
              go (A.snoc vs (evalIn c fe)) tail (A.snoc acc (psValueName l <> ": " <> fe'))
      fields <- go [] (dictToList d) []
      pure ("{ " <> S.joinWith ", " fields <> " }")
    _ -> Left "a struct checked against a non-record type"
  S.Match scrut cases -> do
    Tuple s' sty <- syn c scrut
    d <- dataOf c.names sty
    vs <- case V.behavior sty of
      V.LikeSum (Variants vs) -> pure vs
      _ -> Left "matching on a non-sum"
    arms <- traverse
      ( \(Tuple tag (Tuple xs body)) -> do
          let
            argTys = case A.find (\(Tuple k _) -> k == tag) (dictToList vs) of
              Just (Tuple _ ts) -> map force ts
              Nothing -> []
            bindAll c0 ns = foldl
              ( \(Tuple cc acc) (Tuple x aty) ->
                  let n = if x == "_" then "_" else freshLocal cc x
                  in Tuple (extend cc (Just n) aty) (A.snoc acc n)
              )
              (Tuple c0 [])
              (A.zip ns argTys)
            Tuple c' ns' = bindAll c xs
          b <- chk c' body ty
          pure ("  " <> S.joinWith " " (A.cons (constructorName d tag) ns') <> " -> " <> indentMore b)
      )
      (dictToList cases)
    -- a match on an empty sum has no arms; there is no value to match
    pure (if A.null arms then "(HgRT.unsafeCrashWith \"impossible: a value of an empty type\")"
      else paren ("case " <> s' <> " of\n" <> S.joinWith "\n" arms))
  S.Abandon -> pure "(HgRT.unsafeCrashWith \"abandon\")"
  S.Opaque -> Left "an opaque term"
  S.Code _ -> Left "a type in a value position"
  _ -> do
    Tuple t' _ <- syn c t
    pure t'

indentMore :: String -> String
indentMore s = S.replaceAll (S.Pattern "\n") (S.Replacement "\n    ") s

-- Types from values: the static extent
--------------------------------------------------------------------------------

-- | What a local level stands for when translating a type value: a small type
-- | variable, or a theory-typed variable whose type fields (by path, e.g.
-- | `"t"` or `"plus.t"`) are type variables.
data TV = SmallVar String | TheoryVar (Map String String)

type TEnv = { vars :: Map Int TV, used :: List String, names :: Names, len :: Int }

-- | does a type classify types (possibly type constructors): `Type`,
-- | `Type -> Type`, ...
classifiesTypes :: Int -> V.Ty -> Boolean
classifiesTypes lvl = case _ of
  V.U SmallU -> true
  V.Function ft -> case ft.dom of
    V.U SmallU -> classifiesTypes (lvl + 1) (V.nom (V.appClo ft.cod (pure (V.local lvl ft.dom))))
    _ -> false
  _ -> false

-- | how many type arguments a type-classifying type takes (`Type -> Type` is 1)
typeArity :: Int -> V.Ty -> Int
typeArity lvl = case _ of
  V.Function ft -> 1 + typeArity (lvl + 1) (V.nom (V.appClo ft.cod (pure (V.local lvl ft.dom))))
  _ -> 0

-- | A module's type-constructor field defined as a type-level lambda
-- | (`m := V => a => Nat -> Res a`) has a name, its path, but no PureScript
-- | form: PureScript has no type-level lambdas.  It is represented by a
-- | phantom type of that name, and values are coerced to and from it where a
-- | functor is applied to the module (see `syn`).
typeLambdaName :: Names -> V.El -> Maybe String
typeLambdaName nm v = case v of
  V.Neu n -> case n.head, force n.description of
    V.Global x, Just (V.Lam _ _) ->
      let path = A.mapMaybe (case _ of
            SProj l -> Just l
            _ -> Nothing) (spineItems n.spine)
      in if A.null path then Nothing else Just (nestedName nm x path)
    _, _ -> Nothing
  _ -> Nothing

freshTV :: TEnv -> String -> Tuple String TEnv
freshTV te hint =
  let
    base0 = lowerFirst (mangle hint)
    base = if base0 == "" || base0 == "_" || A.elem base0 reserved then "a" else base0
    go i = let v = if i == 0 then base else base <> show i in if L.elem v te.used then go (i + 1) else v
    v = go 0
  in
    Tuple v (te { used = v : te.used })

-- | the record fields of a theory-typed value `v : ty`, as paths to its type
-- | fields (nested theories included)
typeFieldPaths :: Int -> String -> V.El -> V.Ty -> Array String
typeFieldPaths lvl prefix v ty = case V.behavior ty of
  V.LikeRecord rt -> A.concatMap
    ( \l ->
        let
          fty = V.projTy ty v l
          fv = V.nom (V.proj v l)
          path = if prefix == "" then l else prefix <> "." <> l
        in
          if classifiesTypes lvl fty then [ path ]
          else typeFieldPaths lvl path fv fty
    )
    rt.fieldTypes.keys
  _ -> []

hasTypeFields :: Int -> V.El -> V.Ty -> Boolean
hasTypeFields lvl v ty = not (A.null (typeFieldPaths lvl "" v ty))

-- | a type, as a value, to PureScript
tyV :: TEnv -> V.Ty -> E String
tyV te = case _ of
  V.U _ -> Left "a universe is not a runtime type"
  V.PrimInt -> pure "HgBI.BigInt"
  V.PrimString -> pure "String"
  V.Decode n -> neuTy te n.head n.spine
  V.Function ft
    | classifiesTypes te.len ft.dom -> do
        let
          hint = fromMaybe "a" (V.cloName ft.cod)
          Tuple v te' = freshTV te hint
          te'' = te' { vars = Map.insert te.len (SmallVar v) te'.vars, len = te.len + 1 }
        body <- tyV te'' (V.nom (V.appClo ft.cod (pure (V.local te.len ft.dom))))
        pure ("(forall " <> v <> ". " <> body <> ")")
    | otherwise -> do
        let x = V.local te.len ft.dom
        Tuple te' qs <- bindTheory te x ft.dom
        d <- sigOf (te' { len = te.len + 1 }) x ft.dom
        c <- tyV (te' { len = te.len + 1 }) (V.nom (V.appClo ft.cod (pure x)))
        pure (paren (quantify qs (d <> " -> " <> c)))
  t@(V.Record _) -> sigOf te (V.local te.len t) t
  V.Sum _ -> Left "an anonymous `sum`"

quantify :: Array String -> String -> String
quantify qs body = if A.null qs then body else "forall " <> S.joinWith " " qs <> ". " <> body

-- | when `x : ty` is a theory, its type fields become type variables
bindTheory :: TEnv -> V.El -> V.Ty -> E (Tuple TEnv (Array String))
bindTheory te x ty = case x of
  V.Neu { head: V.Local lvl } | hasTypeFields te.len x ty -> do
    let
      paths = typeFieldPaths te.len "" x ty
      step (Tuple t acc) path =
        let Tuple v t' = freshTV t (fromMaybe path (A.last (S.split (S.Pattern ".") path)))
        in Tuple t' (A.snoc acc (Tuple path v))
      Tuple te' pairs = foldl step (Tuple te []) paths
    pure (Tuple (te' { vars = Map.insert lvl (TheoryVar (Map.fromFoldable pairs)) te'.vars }) (map snd pairs))
  _ -> pure (Tuple te [])

-- | a neutral type: a variable, a projection from a theory variable, or a
-- | named type applied to arguments
neuTy :: TEnv -> V.Head -> V.Spine -> E String
neuTy te head spine0 = do
  let items = spineItems spine0
  case head of
    V.Local lvl -> case Map.lookup lvl te.vars of
      Just (SmallVar v) -> applyTo v items
      Just (TheoryVar fields) -> do
        let
          projs = A.takeWhile isProj items
          path = S.joinWith "." (A.mapMaybe projName projs)
          rest = A.drop (A.length projs) items
        case Map.lookup path fields of
          Just v -> applyTo v rest
          Nothing -> Left ("the type field `" <> path <> "` of a module parameter")
      Nothing -> Left "a type that depends on a value"
    V.Meta _ -> Left "an implicit argument that was never inferred"
    V.Global x ->
      if A.any isProj items then do
        -- a type defined inside a module: `G args .path` is the data type
        -- `G'path`, whose parameters are the type arguments and the type
        -- fields of the module arguments
        let path = A.mapMaybe projName items
        args <- A.concat <$> traverse moduleArgTys (A.filter (not <<< isProj) items)
        let h = nestedName te.names x path
        pure (if A.null args then h else paren (S.joinWith " " (A.cons h args)))
      else applyTo (te.names.typ x) items
  where
  applyTo h items = do
    args <- traverse argTy (A.filter (not <<< isProj) items)
    let as = A.catMaybes args
    pure (if A.null as then h else paren (S.joinWith " " (A.cons h as)))
  -- a type argument is kept; an argument at a small type is statically
  -- irrelevant and erased
  argTy = case _ of
    SApp (V.Code t) -> Just <$> tyV te t
    SApp v@(V.Neu _) -> case v of
      V.Neu n -> case force n.ty of
        V.U SmallU -> Just <$> tyV te (V.decodeN v)
        _ -> pure Nothing
      _ -> pure Nothing
    _ -> pure Nothing
  -- the types an argument contributes to a nested data type: itself if it is
  -- a type, its type fields if it is a module, nothing if it is a value
  moduleArgTys = case _ of
    SApp v -> case v of
      V.Code t -> pure <$> tyV te t
      V.Neu n -> do
        let aty = force n.ty
        if classifiesTypes te.len aty then pure <$> tyV te (V.decodeN v)
        else traverse (\path -> tyV te (V.decodeN (projPath v path))) (typeFieldPaths te.len "" v aty)
      _ -> pure []
    SProj _ -> pure []
  isProj = case _ of
    SProj _ -> true
    _ -> false
  projName = case _ of
    SProj l -> Just l
    _ -> Nothing

data SItem = SApp V.El | SProj String

isSProj :: SItem -> Boolean
isSProj = case _ of
  SProj _ -> true
  _ -> false

projPath :: V.El -> String -> V.El
projPath v path = foldl (\acc l -> V.nom (V.proj acc l)) v (S.split (S.Pattern ".") path)

nestedName :: Names -> Name -> Array String -> String
nestedName nm x path = nm.typ x <> "'" <> S.joinWith "'" (map mangle path)

spineItems :: V.Spine -> Array SItem
spineItems = case _ of
  V.Id -> []
  V.App sp v -> A.snoc (spineItems sp) (SApp v)
  V.Proj sp l -> A.snoc (spineItems sp) (SProj l)
  V.Specialize sp _ _ -> spineItems sp

-- | The PureScript type of a value `v : ty`, from its static extent: a
-- | function's is read off its result for a fresh argument, a module's from
-- | the static values of its type fields.
sigOf :: TEnv -> V.El -> V.Ty -> E String
sigOf te v ty = case V.behavior ty of
  V.LikeRecord rt | hasTypeFields te.len v ty -> do
    fields <- A.catMaybes <$> traverse
      ( \l -> do
          let
            fty = V.projTy ty v l
            fv = V.nom (V.proj v l)
          if classifiesTypes te.len fty then pure Nothing
          else Just <<< (\t -> psValueName l <> " :: " <> t) <$> sigOf te fv fty
      )
      rt.fieldTypes.keys
    pure ("{ " <> S.joinWith ", " fields <> " }")
  V.LikeRecord rt -> do
    fields <- traverse
      ( \l -> (\t -> psValueName l <> " :: " <> t) <$> sigOf te (V.nom (V.proj v l)) (V.projTy ty v l))
      rt.fieldTypes.keys
    pure ("{ " <> S.joinWith ", " fields <> " }")
  V.LikeFunction ft | not (classifiesTypes te.len ft.dom) -> do
    let x = V.local te.len ft.dom
    Tuple te' qs <- bindTheory te x ft.dom
    d <- sigOf (te' { len = te.len + 1 }) x ft.dom
    c <- sigOf (te' { len = te.len + 1 }) (V.nom (V.app v (pure x))) (V.nom (V.appClo ft.cod (pure x)))
    pure (paren (quantify qs (d <> " -> " <> c)))
  V.LikeFunction ft -> do
    let
      hint = fromMaybe "a" (V.cloName ft.cod)
      Tuple tv te' = freshTV te hint
      x = V.local te.len ft.dom
      te'' = te' { vars = Map.insert te.len (SmallVar tv) te'.vars, len = te.len + 1 }
    body <- sigOf te'' (V.nom (V.app v (pure x))) (V.nom (V.appClo ft.cod (pure x)))
    pure ("(forall " <> tv <> ". " <> body <> ")")
  _ -> tyV te ty

-- Declarations
--------------------------------------------------------------------------------

-- | peel the parameter lambdas of a definition, with their function types
params :: S.El -> Tuple (Array (Tuple String S.Ty)) S.El
params = go []
  where
  go acc (S.Lam ty (S.Abs x b)) = go (A.snoc acc (Tuple x ty)) b
  go acc (S.Lam ty (S.AbsConst b)) = go (A.snoc acc (Tuple "_" ty)) b
  go acc b = Tuple acc b

-- | the kind of a global, read off its type
data Kind = TypeCon Int | Theory | Value

kindOf :: V.Ty -> Kind
kindOf = go 0
  where
  go n t = case t of
    V.U SmallU -> TypeCon n
    V.U LargeU -> Theory
    V.Function f -> case f.dom of
      V.U SmallU -> go (n + 1) (V.nom (V.appClo f.cod (pure (V.local n f.dom))))
      _ -> case V.nom (V.appClo f.cod (pure (V.local n f.dom))) of
        V.U SmallU -> Theory
        V.U LargeU -> Theory
        r -> case kindOf r of
          Value -> Value
          _ -> Theory
    _ -> Value

typeDecl :: Names -> Name -> S.El -> E String
typeDecl nm x syntax = do
  let
    Tuple ps body = params syntax
    tname = nm.typ x
    -- the definition itself is local 0, then its parameters
    ctx0 = TSelf tname : Nil
    step (Tuple ctx vars) (Tuple p _) = let v = freshVar ctx p in Tuple (TVar v : ctx) (A.snoc vars v)
    Tuple ctx vars = foldl step (Tuple ctx0 []) ps
    -- (the definition always applies itself to its parameters explicitly, so
    -- the self reference is the bare type name)
    ctxSelf = ctx
    header = S.joinWith " " (A.cons tname vars)
  case body of
    -- a theory whose fields are all ordinary values is just a record type
    S.Code (S.Record rt) -> do
      rhs <- tyPS nm ctxSelf (S.Record rt)
      pure ("type " <> header <> " = " <> rhs)
    S.Code (S.Sum (Variants vs)) -> do
      alts <- traverse
        ( \(Tuple tag ts) -> do
            ts' <- traverse (tyPS nm ctxSelf) ts
            pure (S.joinWith " " (A.cons (constructorName tname tag) (map paren ts')))
        )
        (dictToList vs)
      pure (if A.null alts then "data " <> header else "data " <> header <> "\n  = " <> S.joinWith "\n  | " alts)
    other -> do
      rhs <- elTyPS nm ctx other
      pure ("type " <> header <> " = " <> rhs)

valueDecl :: Globals -> Names -> Name -> GlobalEntry -> E String
valueDecl g nm x entry = do
  let n = nm.value x
  ty <- sigOf { vars: Map.empty, used: Nil, names: nm, len: 0 } entry.value entry.type_
  let
    c0 =
      { locals: { name: Just n, ty: entry.type_ } : Nil
      , env: V.snoc V.LNil entry.value
      , len: 1
      , globals: g
      , names: nm
      , used: n : Nil
      }
  body <- chkD c0 entry.syntax entry.type_ entry.value
  pure (n <> " :: " <> ty <> "\n" <> n <> " = " <> body)

-- | whether (after its parameters) a theory has type fields, possibly nested
theoryHasTypeFields :: V.El -> V.Ty -> Boolean
theoryHasTypeFields code ty = go 0 code ty
  where
  go lvl c t = case t of
    V.Function ft ->
      let x = V.local lvl ft.dom
      in go (lvl + 1) (V.nom (V.app c (pure x))) (V.nom (V.appClo ft.cod (pure x)))
    _ ->
      let rty = V.decodeN c
      in hasTypeFields (lvl + 1) (V.local lvl rty) rty

-- | The data types a module defines inside itself, found by applying it to
-- | fresh parameters and looking at the static values of its type fields.
nestedDecls :: TEnv -> Array String -> V.El -> V.Ty -> E (Array String)
nestedDecls te ps v ty = case V.behavior ty of
  V.LikeFunction ft
    | classifiesTypes te.len ft.dom -> do
        let
          Tuple tv te' = freshTV te (fromMaybe "a" (V.cloName ft.cod))
          x = V.local te.len ft.dom
          te'' = te' { vars = Map.insert te.len (SmallVar tv) te'.vars, len = te.len + 1 }
        nestedDecls te'' (A.snoc ps tv) (V.nom (V.app v (pure x))) (V.nom (V.appClo ft.cod (pure x)))
    | otherwise -> do
        let x = V.local te.len ft.dom
        Tuple te' qs <- bindTheory te x ft.dom
        nestedDecls (te' { len = te.len + 1 }) (ps <> qs) (V.nom (V.app v (pure x))) (V.nom (V.appClo ft.cod (pure x)))
  V.LikeRecord rt -> A.concat <$> traverse
    ( \l -> do
        let
          fty = V.projTy ty v l
          fv = V.nom (V.proj v l)
        case fty, fv of
          -- a type defined here is a neutral on this module's own path; a
          -- field that merely names an existing type has no projections
          V.U SmallU, V.Neu n | A.any isSProj (spineItems n.spine) -> case force n.description of
            Just (V.Code (V.Sum (Variants vs))) -> do
              name <- dataOf te.names (V.decodeN fv)
              alts <- traverse
                ( \(Tuple tag ts) -> do
                    ts' <- traverse (tyV te <<< force) ts
                    pure (S.joinWith " " (A.cons (constructorName name tag) (map paren ts')))
                )
                (dictToList vs)
              let header = "data " <> S.joinWith " " (A.cons name ps)
              pure [ if A.null alts then header else header <> "\n  = " <> S.joinWith "\n  | " alts ]
            _ -> pure []
          _, _ | not (classifiesTypes te.len fty) -> nestedDecls te ps fv fty
          _, _ -> case typeLambdaName te.names fv of
            Just name | A.null ps ->
              let
                arity = typeArity te.len fty
                vars = map (\i -> "t" <> show i) (A.range 1 arity)
              in
                pure
                  [ "-- | the type-level lambda `" <> name <> "`, as a phantom type\n"
                      <> "data " <> name <> " :: " <> S.joinWith " -> " (A.replicate (arity + 1) "Type") <> "\n"
                      <> "data " <> S.joinWith " " (A.cons name vars)
                  ]
            _ -> pure []
    )
    rt.fieldTypes.keys
  _ -> pure []

compileModule :: String -> Globals -> (Name -> Name) -> CompileResult
compileModule modName g display =
  let
    entries = globalsList g
    nm = { value: \x -> psValueName (display x), typ: \x -> psTypeName (display x) }
    one (Tuple x e) = case kindOf e.type_ of
      TypeCon _ -> case typeDecl nm x e.syntax of
        Right d -> { decl: Just d, err: Nothing, skip: Nothing }
        Left err -> { decl: Nothing, err: Just (display x <> ": " <> err), skip: Nothing }
      Theory | theoryHasTypeFields e.value e.type_ ->
        { decl: Nothing, err: Nothing, skip: Just (display x <> ": a theory with type fields (its uses get their types from the static extent)") }
      Theory -> case typeDecl nm x e.syntax of
        Right d -> { decl: Just d, err: Nothing, skip: Nothing }
        Left err -> { decl: Nothing, err: Nothing, skip: Just (display x <> ": " <> err) }
      Value -> case do
          nested <- nestedDecls { vars: Map.empty, used: Nil, names: nm, len: 0 } [] e.value e.type_
          d <- valueDecl g nm x e
          pure (S.joinWith "\n\n" (A.snoc nested d)) of
        Right d -> { decl: Just d, err: Nothing, skip: Nothing }
        Left err -> { decl: Nothing, err: Just (display x <> ": " <> err), skip: Nothing }
    results = map one entries
    header =
      [ "-- | Generated from Hippogriff; do not edit."
      , "module " <> modName <> " where"
      , ""
      , "import Partial.Unsafe as HgRT"
      , "import Unsafe.Coerce as HgRTC"
      , "import Prelude as HgP"
      , "import Data.String.CodeUnits as HgS"
      , "import Data.Char as HgC"
      , "import Data.Maybe as HgM"
      , "import Data.Ord as HgO"
      , "import Hg.Support.BigInt as HgBI"
      , ""
      , ""
      ]
  in
    { source: S.joinWith "\n" header <> S.joinWith "\n\n" (A.mapMaybe _.decl results) <> "\n"
    , errors: A.mapMaybe _.err results
    , skipped: A.mapMaybe _.skip results
    }

