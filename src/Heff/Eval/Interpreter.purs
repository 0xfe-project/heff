-- | A port of `Hippogriff.Backend.Interpreter`: an untyped evaluator for
-- | elaborated syntax.  Types evaluate to `Erased`.
-- |
-- | Evaluation is by value, as the compiled code runs: an argument, a
-- | constructor's field and a record's field are evaluated before they are
-- | passed or put, in order, used or not.  A global is evaluated when first
-- | used.
module Heff.Eval.Interpreter
  ( Val(..)
  , EvalResult(..)
  , evalGlobals
  , evalGlobalsWhere
  , prettyVal
  , prettyResult
  , showResult
  , evalIn
  , GlobalEnv
  ) where

import Heff.Support.BigInt as BI
import Heff.Support.BigInt (BigInt)
import Prelude

import Control.Lazy (fix)
import Data.Array as A
import Data.Either (Either)
import Data.Lazy (Lazy, defer, force)
import Data.List (List(Nil), (:))
import Data.List as L
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), maybe)
import Data.Number as Number
import Data.Tuple (Tuple(..))
import Effect.Exception.Unsafe (unsafeThrow)
import Heff.Support.Common (Dict, Name, dictElemAt, dictLookup, dictMap, dictToList, panic)
import Heff.Core.Globals (Globals, globalsList)
import Heff.Core.Syntax as S
import Data.Char (fromCharCode, toCharCode)
import Data.String.CodeUnits as Str
import Data.String as Text
import Heff.Support.Pretty (Doc, (<+>))
import Heff.Support.Pretty as P

data Val
  = Lam (Lazy Val -> Val)
  | Cons (Dict (Lazy Val))
  | Tag Name (Array (Lazy Val))
  | LitInt BigInt
  | LitFloat Number
  | LitString String
  | ArrayV (Array Val)
  | Erased

prettyVal :: Val -> Doc
prettyVal = case _ of
  Lam _ -> P.text "#fun"
  Cons fs -> P.list (map (\(Tuple x v) -> P.text x <+> P.text ":=" <+> prettyVal (force v)) (dictToList fs))
  Tag x vs -> P.text "'" <> P.text x <> A.foldMap (\v -> P.text " " <> prettyVal (force v)) vs
  LitInt i -> P.text (show i)
  -- (`2.0`, not `2`: as a literal writes it)
  LitFloat x -> P.text (show x)
  LitString s -> P.text (show s)
  ArrayV xs -> P.text ("[" <> A.intercalate ", " (map (P.render <<< prettyVal) xs) <> "]")
  Erased -> P.text "#erased"

app :: Val -> Lazy Val -> Val
app (Lam f) v = f v
app Erased _ = Erased
app _ _ = panic "expected a lambda value"

proj :: Val -> Name -> Val
proj (Cons fs) x = force (dictElemAt fs x)
proj Erased _ = Erased
proj _ _ = panic "expected a cons value"

type LocalEnv = List (Lazy Val)

type GlobalEnv = Map Name (Lazy Val)

type Env = { local :: LocalEnv, global :: GlobalEnv }

-- | an argument, or a field: evaluated now
arg :: Env -> S.El -> Lazy Val
arg e t = let v = evalIn e t in defer \_ -> v

bindMulti :: Env -> Array (Lazy Val) -> Env
bindMulti e vs = e { local = A.foldl (\acc v -> v : acc) e.local vs }

-- | `abandon` escapes as a JavaScript exception, like `throw Abandon`
abandonMarker :: String
abandonMarker = "heff:abandon"

match :: Env -> Val -> Dict (Tuple (Array Name) S.El) -> Val
match e (Tag x vs) cases = case dictLookup cases x of
  Just (Tuple _ body) -> evalIn (bindMulti e vs) body
  Nothing -> panic "tag not found"
match _ _ _ = panic "expected a tag value"

evalIn :: Env -> S.El -> Val
evalIn e = case _ of
  S.Local i -> case L.index e.local i of
    Just v -> force v
    Nothing -> panic "no such local"
  S.Global x -> case Map.lookup x e.global of
    Just v -> force v
    Nothing -> panic ("no global " <> x)
  S.Lam _ (S.Abs _ body) -> Lam (\v -> evalIn (e { local = v : e.local }) body)
  S.Lam _ (S.AbsConst body) -> let v = defer \_ -> evalIn e body in Lam (\_ -> force v)
  S.App t1 t2 -> let f = evalIn e t1 in app f (arg e t2)
  S.Cons fs -> Cons (dictMap (arg e) fs)
  S.Proj t x -> proj (evalIn e t) x
  S.Tag x ts -> Tag x (map (arg e) ts)
  S.Match scrut cases -> match e (evalIn e scrut) cases
  S.Code _ -> Erased
  S.Opaque -> panic "opaque should never be elaborated, it only exists for readback"
  S.Is t -> evalIn e t
  S.Abandon -> unsafeThrow abandonMarker
  S.Rec t -> evalIn e t
  S.Prim x -> prim x
  S.MetaApp _ _ _ -> panic "an implicit argument was never inferred"
  S.LitInt i -> LitInt i
  S.LitFloat x -> LitFloat x
  S.LitString s -> LitString s
  S.Implicit _ t -> evalIn e t
  S.Specialize _ _ _ -> Erased

-- | the interpreter's side of `Heff.Elab.Prims`: answers are the tags 'true/'false
prim :: Name -> Val
prim = case _ of
  "intAdd" -> int2 \a b -> LitInt (BI.add a b)
  "intSub" -> int2 \a b -> LitInt (BI.sub a b)
  "intMul" -> int2 \a b -> LitInt (BI.mul a b)
  -- (by zero, `abandon`)
  "intDiv" -> int2 \a b -> orAbandon (BI.divFloor a b)
  "intMod" -> int2 \a b -> orAbandon (BI.modFloor a b)
  "intEq" -> int2 \a b -> bool (a == b)
  "intLt" -> int2 \a b -> bool (a < b)
  "intLe" -> int2 \a b -> bool (a <= b)
  "intShow" -> Lam \a -> LitString (BI.toString (int a))
  "floatAdd" -> float2 \a b -> LitFloat (a + b)
  "floatSub" -> float2 \a b -> LitFloat (a - b)
  "floatMul" -> float2 \a b -> LitFloat (a * b)
  "floatDiv" -> float2 \a b -> LitFloat (a / b)
  "floatEq" -> float2 \a b -> bool (a == b)
  "floatLt" -> float2 \a b -> bool (a < b)
  "floatLe" -> float2 \a b -> bool (a <= b)
  "floatShow" -> Lam \a -> LitString (show (float a))
  "floatFromInt" -> Lam \a -> LitFloat (bigToNumber (int a))
  "floatSqrt" -> Lam \a -> LitFloat (Number.sqrt (float a))
  -- (of what is not a number, or not finite, `abandon`)
  "floatFloor" -> Lam \a -> let x = float a in if Number.isFinite x then LitInt (floorToBig x) else unsafeThrow abandonMarker
  -- (an array's element type is given first, erased)
  "arrayLength" -> ty \_ -> Lam \a -> LitInt (BI.fromInt (A.length (arr a)))
  "arrayIndex" -> ty \_ -> Lam \a -> Lam \i -> orAbandon' (at (arr a) (int i) >>= A.index (arr a))
  "arraySet" -> ty \_ -> Lam \a -> Lam \i -> Lam \x -> orAbandon' (at (arr a) (int i) >>= \k -> ArrayV <$> A.updateAt k (force x) (arr a))
  "arrayGenerate" -> ty \_ -> Lam \n -> Lam \f ->
    let k = BI.toIntClamped (int n)
    in ArrayV (if k <= 0 then [] else map (\j -> app (force f) (pure (LitInt (BI.fromInt j)))) (A.range 0 (k - 1)))
  "arrayFromList" -> ty \_ -> Lam \l -> ArrayV (fromList (force l) [])
  "arrayToList" -> ty \_ -> Lam \a -> A.foldr (\x rest -> Tag "Cons" [ pure x, pure rest ]) (Tag "Nil" []) (arr a)
  "strAppend" -> str2 \a b -> LitString (a <> b)
  "strLength" -> Lam \a -> LitInt (BI.fromInt (Str.length (str a)))
  "strSlice" -> Lam \a -> Lam \i -> Lam \j -> LitString (Str.slice (small i) (small j) (str a))
  "strEq" -> str2 \a b -> bool (a == b)
  "strLt" -> str2 \a b -> bool (a < b)
  "strCharCode" -> Lam \a -> Lam \i ->
    let k = int i
    in LitInt (BI.fromInt (if k < BI.fromInt 0 || k >= BI.fromInt (Str.length (str a)) then -1 else maybe (-1) toCharCode (Str.charAt (BI.toIntClamped k) (str a))))
  "strFromCharCode" -> Lam \c ->
    let k = int c
    in LitString (if k < BI.fromInt 0 || k > BI.fromInt 65535 then "" else maybe "" Str.singleton (fromCharCode (BI.toIntClamped k)))
  x -> panic ("no primitive " <> x)
  where
  int v = case force v of
    LitInt i -> i
    _ -> panic "a primitive expected an integer"
  str v = case force v of
    LitString t -> t
    _ -> panic "a primitive expected a string"
  float v = case force v of
    LitFloat x -> x
    _ -> panic "a primitive expected a float"
  float2 f = Lam \a -> Lam \b -> f (float a) (float b)
  arr v = case force v of
    ArrayV xs -> xs
    _ -> panic "a primitive expected an array"
  ty k = Lam \_ -> k unit
  -- (an index in range)
  at xs i = if i >= BI.fromInt 0 && i < BI.fromInt (A.length xs) then Just (BI.toIntClamped i) else Nothing
  orAbandon' = case _ of
    Just v -> v
    Nothing -> unsafeThrow abandonMarker
  -- (a list's cells, `Cons`/`Nil`, as the prelude has them)
  fromList v acc = case v of
    Tag "Cons" [ x, rest ] -> fromList (force rest) (A.snoc acc (force x))
    _ -> acc
  int2 f = Lam \a -> Lam \b -> f (int a) (int b)
  small v = BI.toIntClamped (int v)
  -- (a `case`: an argument would be thrown before the call)
  orAbandon = case _ of
    Just n -> LitInt n
    Nothing -> unsafeThrow abandonMarker
  str2 f = Lam \a -> Lam \b -> f (str a) (str b)
  bool b = Tag (if b then "True" else "False") []

data EvalResult = Succeeded Val | Abandoned | Diverged

-- | a value as the tools show it: unlike the reference's printing, a tag
-- | with arguments is parenthesized where it is an argument, so what is
-- | shown reads back as the same value
showVal :: Val -> Doc
showVal = go false
  where
  go inArg = case _ of
    Tag x vs | not (A.null vs) ->
      let d = P.text (tag x) <> A.foldMap (\v -> P.text " " <> go true (force v)) vs
      in if inArg then P.text "(" <> d <> P.text ")" else d
    Tag x _ -> P.text (tag x)
    Cons fs -> P.text ("{ " <> A.intercalate ", " (map (\(Tuple x v) -> x <> " = " <> P.render (go false (force v))) (dictToList fs)) <> " }")
    ArrayV xs -> P.text ("[" <> A.intercalate ", " (map (P.render <<< go false) xs) <> "]")
    -- (a negative number, an argument: `Just (-7)`)
    v | inArg && Text.take 1 (P.render (prettyVal v)) == "-" -> P.text ("(" <> P.render (prettyVal v) <> ")")
    v -> prettyVal v

-- | a tag as the surface syntax writes it: a capitalised one bare
tag :: Name -> String
tag x = case Str.charAt 0 x of
  Just c | c >= 'A' && c <= 'Z' -> x
  _ -> "'" <> x

showResult :: EvalResult -> Doc
showResult = case _ of
  Succeeded v -> showVal v
  r -> prettyResult r

prettyResult :: EvalResult -> Doc
prettyResult = case _ of
  Succeeded v -> prettyVal v
  Abandoned -> P.text "<abandoned>"
  Diverged -> P.text "<diverged>"

foreign import bigToNumber :: BigInt -> Number
foreign import floorToBig :: Number -> BigInt
foreign import tryAbandon :: forall a. String -> (Unit -> a) -> (a -> Either Unit a) -> Either Unit a -> Either Unit a
foreign import tryEval :: forall a r. String -> (Unit -> a) -> (a -> r) -> r -> r -> r

evalFix :: GlobalEnv -> S.El -> Lazy Val
evalFix g t = fix \self -> defer \_ -> evalIn { local: self : Nil, global: g } t

-- | everything a printed result will show, forced now so that an `abandon` in
-- | it is caught here (the reference instead crashes while printing)
deepForce :: Val -> Val
deepForce v = case v of
  Cons fs -> const v (map (deepForce <<< force) fs.values)
  Tag _ vs -> const v (map (deepForce <<< force) vs)
  _ -> v

evalGlobals :: Globals -> Array (Tuple Name EvalResult)
evalGlobals = evalGlobalsWhere (const true)

-- | every global is evaluated (lazily), but only those `wanted` are forced
-- | and reported
evalGlobalsWhere :: (Name -> Boolean) -> Globals -> Array (Tuple Name EvalResult)
evalGlobalsWhere wanted gs = go Map.empty (globalsList gs) []
  where
  go ge es acc = case A.uncons es of
    Nothing -> acc
    Just { head: Tuple x entry, tail } ->
      let
        lv = evalFix ge entry.syntax
        ge' = Map.insert x lv ge
      in
        if not (wanted x) then go ge' tail acc
        else
          let r = tryEval abandonMarker (\_ -> deepForce (force lv)) Succeeded Abandoned Diverged
          in go ge' tail (A.snoc acc (Tuple x r))
