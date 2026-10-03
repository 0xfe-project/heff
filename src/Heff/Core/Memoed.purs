-- | A port of `Hippogriff.Core.Memoed`: syntax paired with its (lazy) value,
-- | so that elaboration never has to evaluate a term it just built.
module Heff.Core.Memoed where

import Heff.Support.BigInt (BigInt)
import Prelude

import Data.Lazy (Lazy, defer, force)
import Data.Tuple (Tuple)
import Heff.Support.Common (Dict, Name, dictMap)
import Heff.Core.Evaluation (EvalEnv, compile, compileRecordType, compileTy)
import Heff.Core.Params (Case(..), Level, Plicity(..), Universe, Variants, variantsMap)
import Heff.Core.Readback (CtxLen, readbEl, readbTy)
import Heff.Core.Syntax as S
import Heff.Core.Value as V

type El = { stx :: S.El, val :: Lazy (V.Ev V.El) }
type Ty = { stx :: S.Ty, val :: Lazy (V.Ev V.Ty) }

valN :: El -> V.El
valN m = V.nom (force m.val)

tyN :: Ty -> V.Ty
tyN m = V.nom (force m.val)

levelOf :: Ty -> Level
levelOf m = V.levelOfEv (force m.val)

local :: Int -> Lazy V.El -> El
local i v = { stx: S.Local i, val: map V.Nom v }

global :: Name -> V.El -> El
global x v = { stx: S.Global x, val: pure (V.Nom v) }

code :: Ty -> El
code g = { stx: S.Code g.stx, val: defer \_ -> V.emap V.Code (force g.val) }

-- | an application to an implicit argument (the binder's name kept, for
-- | printing)
iapp :: Name -> El -> El -> El
iapp x g0 g1 = (app g0 g1) { stx = S.App g0.stx (S.Implicit x g1.stx) }

app :: El -> El -> El
app g0 g1 = { stx: S.App g0.stx g1.stx, val: defer \_ -> V.ebind (\f -> V.app f (defer \_ -> valN g1)) (force g0.val) }

lam :: Case -> EvalEnv -> Ty -> S.Abs El -> El
lam c e gdom = case _ of
  S.Abs x gbody ->
    { stx: S.Lam gdom.stx (S.Abs x gbody.stx)
    , val: defer \_ -> V.epure c (V.Lam (defer \_ -> tyN gdom) (V.Clo x e.local (compile e.global c gbody.stx)))
    }
  S.AbsConst gbody ->
    { stx: S.Lam gdom.stx (S.AbsConst gbody.stx)
    , val: defer \_ -> V.epure c (V.Lam (defer \_ -> tyN gdom) (V.CloConst (force gbody.val)))
    }

cons :: Case -> Dict El -> El
cons c d = { stx: S.Cons (dictMap _.stx d), val: defer \_ -> V.epure c (V.Cons (dictMap (\m -> force m.val) d)) }

proj :: El -> Name -> El
proj g x = { stx: S.Proj g.stx x, val: defer \_ -> V.ebind (\v -> V.proj v x) (force g.val) }

tag :: Name -> Array El -> El
tag x gs = { stx: S.Tag x (map _.stx gs), val: pure (V.Nom V.Opaque) }

match :: El -> Dict (Tuple (Array Name) S.El) -> El
match g cases = { stx: S.Match g.stx cases, val: pure (V.Nom V.Opaque) }

specialize :: El -> Name -> El -> El
specialize g x g' = { stx: S.Specialize g.stx x g'.stx, val: defer \_ -> V.Nom (V.specialize (valN g) x (valN g')) }

rec :: El -> El
rec m = { stx: S.Rec m.stx, val: pure (V.Nom V.Opaque) }

abandon :: El
abandon = { stx: S.Abandon, val: pure (V.Nom V.Opaque) }

litInt :: BigInt -> El
litInt i = { stx: S.LitInt i, val: pure (V.Nom V.Opaque) }

litFloat :: Number -> El
litFloat x = { stx: S.LitFloat x, val: pure (V.Nom V.Opaque) }

litString :: String -> El
litString s = { stx: S.LitString s, val: pure (V.Nom V.Opaque) }

is :: El -> El
is g = { stx: S.Is g.stx, val: defer \_ -> V.Become (valN g) }

univ :: Universe -> Ty
univ u = { stx: S.U u, val: pure (V.Nom (V.U u)) }

decode :: El -> Ty
decode g = { stx: S.Decode g.stx, val: defer \_ -> V.decodeEv (force g.val) }

function :: EvalEnv -> Ty -> S.Abs Ty -> Ty
function e dom cod = functionI Expl e dom cod

-- | a function type whose argument may be implicit or an instance
functionI :: Plicity -> EvalEnv -> Ty -> S.Abs Ty -> Ty
functionI implicit e dom cod =
  let
    r = case cod of
      S.Abs x body -> { stx: S.Abs x body.stx, clo: V.Clo x e.local (compileTy e.global N body.stx), body }
      S.AbsConst body -> { stx: S.AbsConst body.stx, clo: V.CloConst (force body.val), body }
  in
    { stx: S.Function { level: levelOf r.body, dom: dom.stx, cod: r.stx, implicit }
    , val: defer \_ -> V.Nom (V.Function { level: levelOf r.body, dom: tyN dom, cod: r.clo, implicit })
    }

record :: EvalEnv -> S.RecordType -> Ty
record e rt = { stx: S.Record rt, val: defer \_ -> V.Describe (V.Record (compileRecordType e.global rt e.local)) }

sum :: Variants Ty -> Ty
sum st = { stx: S.Sum (variantsMap _.stx st), val: defer \_ -> V.Describe (V.Sum (variantsMap (\m -> defer \_ -> tyN m) st)) }

isTy :: Ty -> Ty
isTy m = { stx: S.IsTy m.stx, val: defer \_ -> V.Become (tyN m) }

primInt :: Ty
primInt = { stx: S.PrimInt, val: pure (V.Nom V.PrimInt) }

primFloat :: Ty
primFloat = { stx: S.PrimFloat, val: pure (V.Nom V.PrimFloat) }

primArray :: Ty -> Ty
primArray a = { stx: S.PrimArray a.stx, val: defer \_ -> V.Nom (V.PrimArray (tyN a)) }

primString :: Ty
primString = { stx: S.PrimString, val: pure (V.Nom V.PrimString) }

fromVEl :: Case -> CtxLen -> V.El -> El
fromVEl c n v = { stx: readbEl n v, val: pure (V.epure c v) }

fromVTy :: Case -> CtxLen -> V.Ty -> Ty
fromVTy c n v = { stx: readbTy n v, val: pure (V.epure c v) }
