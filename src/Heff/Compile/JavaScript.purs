-- | The IR (`Heff.Lower`) to JavaScript: an ES module.  Emitting is all this
-- | does -- what is erased, inlined, dropped or called at once was decided
-- | before.
-- |
-- |   tag      T a b    ~>  { $: "T", a: [a, b] }
-- |   record   { l = e } ~> { l: e }
-- |   match             ~>  a `switch` on `$`
-- |   a definition of n parameters  ~>  a function of n parameters (and,
-- |            where it is passed on, `name$c`, its curried form)
-- |   a definition that is not a function  ~>  a constant, or (if computing
-- |            it may fail) computed once, when first used
-- |
-- | Arguments are evaluated before the call (call-by-value); the
-- | interpreter is lazy.
module Heff.Compile.JavaScript
  ( compileJs
  , compileProgram
  , exportedNames
  , hostOf
  , Provider
  , Output
  ) where

import Prelude

import Data.Array as A
import Data.Foldable (foldl)
import Data.Map (Map)
import Data.Map as Map
import Data.Maybe (Maybe(..), fromMaybe)
import Data.Set (Set)
import Data.Set as Set
import Data.String as S
import Data.String.CodeUnits as SCU
import Data.Tuple (Tuple(..))
import Heff.Core.Globals (Globals, globalsList)
import Heff.Effect (Effect(..), leaves, moduleOfKey, nameOfKey)
import Heff.Elab.Prims (prims)
import Heff.Lower (lower)
import Heff.Lower.IR (Def, Expr(..), baseOf, globalsIn, isValue)
import Heff.Support.BigInt as BI
import Heff.Support.Common (quoteString, spellOperator)

type Output = { source :: String, errors :: Array String }

-- | a module whose default export has the given definitions by the given
-- | names (all of them, by their keys, if none is given)
compileJs :: Globals -> Array { name :: String, key :: String } -> Output
compileJs g exports0 =
  let
    exports = if A.null exports0 then map (\(Tuple k _) -> { name: k, key: k }) (globalsList g) else exports0
    l = lower g (map _.key exports)
    m = emitDefs l.defs (map _.key exports)
    present = A.filter (\o -> Map.member o.key m.globals) exports
    getter o = "  get " <> propName o.name <> "() { return " <> ref m.globals o.key <> "; },"
  in
    { source: S.joinWith "\n" ([ "// Generated from Hippogriff; do not edit." ] <> m.lines <> [ "", "export default {" ] <> map getter present <> [ "};", "" ])
    , errors: l.errors
    }

-- | the world's side of an effect: a provider module's name and text
-- | (`M.provider.js` beside `M.hf`; lib/dom.provider.js says the shape)
type Provider = { module :: String, text :: String }

-- | the factories a provider exports, in the order it declares them
-- | (a `$`-named export is the provider speaking about itself, `$host`)
exportedNames :: String -> Array String
exportedNames text = A.filter (\n -> S.take 1 n /= "$") (A.mapMaybe name (S.split (S.Pattern "\n") text))
  where
  name l = S.stripPrefix (S.Pattern "export const ") l <#> \rest -> fromMaybe rest (S.indexOf (S.Pattern " ") rest <#> \i -> S.take i rest)

-- | the one host a provider's world lives in (`export const $host =
-- | "page"`), if it says so; without it, any
hostOf :: String -> Maybe String
hostOf text = A.head (A.mapMaybe host (S.split (S.Pattern "\n") text))
  where
  host l = S.stripPrefix (S.Pattern "export const $host = \"") l <#> \rest -> fromMaybe rest (S.indexOf (S.Pattern "\"") rest <#> \i -> S.take i rest)

-- | one file that runs a computation (`key`) of the effect `eff`: the
-- | program, the runtime, and each world's algebra from its provider --
-- | it imports nothing but what the providers do (`node:` builtins,
-- | hoisted), so a page loads it from file:// too
compileProgram :: Array Provider -> Globals -> String -> Effect -> Output
compileProgram available g key eff =
  let
    l = lower g [ key ]
    m = emitDefs l.defs [ key ]
    provs = A.mapMaybe (\md -> A.find (\p -> p.module == md) available) (A.nub (map moduleOfKey (leaves eff)))
    imports = A.nub (A.concatMap (\p -> A.filter isImport (S.split (S.Pattern "\n") p.text)) provs)
  in
    { source: S.joinWith "\n" ([ "// Generated from Hippogriff; do not edit." ] <> imports <> m.lines <> [ "", runtimeFor provs eff, "", "$run(" <> ref m.globals key <> ", " <> handlerOf provs eff <> ");", "" ])
    , errors: l.errors
    }

isImport :: String -> Boolean
isImport l = S.take 7 l == "import "

-- (each effect's handler, an operation of a sum going to its side's; a
-- leaf's is its provider's factory, called for a fresh algebra)
handlerOf :: Array Provider -> Effect -> String
handlerOf provs = case _ of
  Leaf k -> "$p" <> show (fromMaybe 0 (A.findIndex (\p -> p.module == moduleOfKey k) provs)) <> "." <> nameOfKey k <> "()"
  Plus a b -> "$sum(" <> handlerOf provs a <> ", " <> handlerOf provs b <> ")"

-- Globals
--------------------------------------------------------------------------------

-- | how a definition is reached: its name, how many parameters it takes at
-- | once, whether it is computed when first used
type GInfo = { js :: String, arity :: Int, lazy :: Boolean }

-- | a global as a value
ref :: Map String GInfo -> String -> String
ref gs k = case Map.lookup k gs of
  Just i
    | i.arity >= 2 -> i.js <> "$c"
    | i.lazy -> i.js <> "()"
    | otherwise -> i.js
  Nothing -> "undefined"

reserved :: Array String
reserved =
  [ "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else"
  , "enum", "export", "extends", "false", "finally", "for", "function", "if", "import", "in", "instanceof"
  , "new", "null", "return", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void"
  , "while", "with", "yield", "let", "static", "implements", "interface", "package", "private", "protected"
  , "public", "await", "arguments", "eval", "undefined", "NaN", "Infinity", "document", "window", "BigInt"
  , "String", "Number", "Object", "Error", "Math", "JSON", "setTimeout", "fetch"
  , "process", "console", "globalThis", "Promise", "Array", "Map", "Set", "Date", "Symbol", "RegExp"
  , "Buffer", "setImmediate", "queueMicrotask", "structuredClone", "URL", "TextEncoder", "TextDecoder"
  ]

safe :: String -> String
safe x0 =
  let
    -- (an operator is spelt out: `+` and `-` are not both `_`)
    x = spellOperator x0
    s = SCU.fromCharArray (map (\c -> if isWordChar c then c else '_') (SCU.toCharArray x))
    s' = if s == "" then "x" else s
  in
    if A.elem s' reserved || isDigit (SCU.take 1 s') then "_" <> s' else s'
  where
  isWordChar c = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
  isDigit d = d >= "0" && d <= "9" && d /= ""

isIdent :: String -> Boolean
isIdent x = x /= "" && safe x == x

propName :: String -> String
propName x = if isIdent x then x else quoteString x

-- | the short name of a key (`co.bind` is `bind`)
shortName :: String -> String
shortName k = fromMaybe k (A.last (S.split (S.Pattern ".") k))

emitDefs :: Array Def -> Array String -> { lines :: Array String, globals :: Map String GInfo }
emitDefs defs roots =
  let
    -- names: the short one, unless two definitions share it
    shorts = map (\d -> safe (shortName d.key)) defs
    jsName d = let s = safe (shortName d.key) in if A.length (A.filter (_ == s) shorts) > 1 then safe d.key else s
    -- (a definition is computed at once when that cannot fail)
    info = foldl
      ( \acc d ->
          let
            arity = case d.body of
              Lam xs _ -> A.length xs
              _ -> 0
            lazy = arity == 0 && not (isValue d.body && A.all (\k -> k /= d.key && maybe' (\i -> not i.lazy) (Map.lookup k acc)) (globalsIn d.body))
          in
            Map.insert d.key { js: jsName d, arity, lazy } acc
      )
      Map.empty
      defs
    -- where a function of several parameters is used as a value, it needs
    -- its curried form
    curried = Set.fromFoldable (A.concatMap (\d -> valueRefs d.body) defs <> roots)
    globalNames = Set.fromFoldable (map (\d -> jsName d) defs)
    code = A.concatMap (\d -> defLines info curried globalNames d) defs
    -- (a primitive's helper may use the runtime too: `$abandon`, `$T`)
    helpers = primHelpers (S.joinWith "\n" code)
    used = S.joinWith "\n" (helpers <> code)
  in
    { lines: runtime used <> helpers <> code, globals: info }
  where
  maybe' f = case _ of
    Just x -> f x
    Nothing -> false

-- | the globals an expression uses as values (not called at once)
valueRefs :: Expr -> Array String
valueRefs = case _ of
  Global g -> [ g ]
  Lam _ b -> valueRefs b
  App f as -> valueRefs f <> A.concatMap valueRefs as
  Let _ a b -> valueRefs a <> valueRefs b
  Record fs -> A.concatMap (\(Tuple _ e) -> valueRefs e) fs
  Proj e _ -> valueRefs e
  Tag _ as -> A.concatMap valueRefs as
  Match s arms -> valueRefs s <> A.concatMap (\a -> valueRefs a.body) arms
  PrimCall _ as -> A.concatMap valueRefs as
  Call _ as -> A.concatMap valueRefs as
  _ -> []

defLines :: Map String GInfo -> Set String -> Set String -> Def -> Array String
defLines gs curried globalNames d = case Map.lookup d.key gs of
  Nothing -> []
  Just i ->
    let env0 = { gs, names: Map.empty, used: globalNames, self: Nothing }
    in
      case d.body of
        Lam xs b ->
          let
            r = bindAll env0 xs
            params = S.joinWith ", " r.js
            wrapper =
              if i.arity >= 2 && Set.member d.key curried then [ "const " <> i.js <> "$c = " <> S.joinWith "" (map (\p -> "(" <> p <> ") => ") r.js) <> i.js <> "(" <> params <> ");" ]
              else []
            loops = tailSelf d.key (A.length xs) b
            trmc = consSelf d.key (A.length xs) b
            sapp = appendSelf d.key (A.length xs) b
            -- (a call of itself last: a loop; its parameters `x$`, each
            -- round's own `const`, which what it closes over keeps)
            vars = map (_ <> "$") r.js
            env' = r.env { self = Just { key: d.key, params: vars, trmc, sapp } }
            looped =
              [ "", "const " <> i.js <> " = (" <> S.joinWith ", " vars <> ") => {" ]
                <> (if trmc then [ "  let $root, $hole = null, $at = 0;" ] else [])
                <> (if sapp then [ "  let $s = \"\";" ] else [])
                <> [ "  for (;;) {", "    const " <> S.joinWith ", " (A.zipWith (\x v -> x <> " = " <> v) r.js vars) <> ";" ]
                <> stmts env' 2 b
                <> [ "  }", "};" ]
          in
            (if loops then looped else [ "", "const " <> i.js <> " = (" <> params <> ") => " <> body r.env 0 b <> ";" ]) <> wrapper
        b
          | i.lazy -> [ "", "const " <> i.js <> " = $lazy(() => " <> body env0 0 b <> ");" ]
          | otherwise -> [ "", "const " <> i.js <> " = " <> (expr env0 0 b).code <> ";" ]

-- Expressions
--------------------------------------------------------------------------------

-- | the names in scope: the JavaScript name of each variable, and every
-- | name taken; and, in a function that is a loop, the function (where a
-- | call of it is last, it goes round again)
type Env = { gs :: Map String GInfo, names :: Map String String, used :: Set String, self :: Maybe Self }

-- | a function that is a loop: its parameters (`x$`), whether a
-- | constructor with a call of it last is built in place, its hole filled by
-- | the next round (`$root`, `$hole`, `$at`), and whether a string append
-- | ends in a call of it, gathered left to right (`$s`)
type Self = { key :: String, params :: Array String, trmc :: Boolean, sapp :: Boolean }

-- | whether the function `key` calls itself last somewhere: directly, as a
-- | constructor's last argument, or right of a string append
tailSelf :: String -> Int -> Expr -> Boolean
tailSelf key n = case _ of
  Let _ _ b -> tailSelf key n b
  Match _ arms -> A.any (tailSelf key n <<< _.body) arms
  Call k as -> k == key && A.length as == n
  e -> consSelf key n e || appendSelf key n e

-- | whether it does as a constructor's last argument
consSelf :: String -> Int -> Expr -> Boolean
consSelf key n = case _ of
  Let _ _ b -> consSelf key n b
  Match _ arms -> A.any (consSelf key n <<< _.body) arms
  Tag _ as | Just (Call k bs) <- A.last as -> k == key && A.length bs == n
  _ -> false

-- | whether it does at the end of a chain of string appends,
-- | `a ++ b ++ f xs`
appendSelf :: String -> Int -> Expr -> Boolean
appendSelf key n = case _ of
  Let _ _ b -> appendSelf key n b
  Match _ arms -> A.any (appendSelf key n <<< _.body) arms
  PrimCall "strAppend" [ _, r ] -> appendEnd key n r
  _ -> false

appendEnd :: String -> Int -> Expr -> Boolean
appendEnd key n = case _ of
  Call k as -> k == key && A.length as == n
  PrimCall "strAppend" [ _, r ] -> appendEnd key n r
  _ -> false

bindName :: Env -> String -> { js :: String, env :: Env }
bindName env x =
  let
    base = safe (baseOf x)
    go i = let v = if i == 0 then base else base <> show i in if Set.member v env.used then go (i + 1) else v
    js = go 0
  in
    { js, env: env { names = Map.insert x js env.names, used = Set.insert js env.used } }

bindAll :: Env -> Array String -> { js :: Array String, env :: Env }
bindAll env xs = foldl (\acc x -> let r = bindName acc.env x in { js: A.snoc acc.js r.js, env: r.env }) { js: [], env } xs

indent :: Int -> String
indent n = S.joinWith "" (A.replicate n "  ")

-- | precedences: 0 an arrow or a conditional, 1 a binary operation, 2 a
-- | unary one, 3 anything that can be called or projected
type Code = { code :: String, prec :: Int }

atLeast :: Int -> Code -> String
atLeast p c = if c.prec < p then "(" <> c.code <> ")" else c.code

-- | a function's body: an expression, or a block if it binds or matches
body :: Env -> Int -> Expr -> String
body env ind e = case e of
  Let _ _ _ -> block env ind e
  Match _ _ -> block env ind e
  _ ->
    let c = (expr env ind e).code
    in if SCU.take 1 c == "{" then "(" <> c <> ")" else c

block :: Env -> Int -> Expr -> String
block env ind e = "{\n" <> S.joinWith "\n" (stmts env (ind + 1) e) <> "\n" <> indent ind <> "}"

stmts :: Env -> Int -> Expr -> Array String
stmts env ind = case _ of
  Let x a b ->
    let
      a' = expr env ind a
      r = bindName env x
    in
      A.cons (indent ind <> "const " <> r.js <> " = " <> a'.code <> ";") (stmts r.env ind b)
  Match s arms ->
    let
      scrut = case s of
        Var x | Just js <- Map.lookup x env.names -> { js, env, pre: [] }
        _ -> let r = bindName env "s" in { js: r.js, env: r.env, pre: [ indent ind <> "const " <> r.js <> " = " <> (expr env ind s).code <> ";" ] }
      arm a =
        let
          r = bindAll scrut.env a.vars
          decls = A.mapWithIndex (\i v -> indent (ind + 2) <> "const " <> v <> " = " <> scrut.js <> ".a[" <> show i <> "];") r.js
          inner = stmts r.env (ind + 2) a.body
        in
          [ indent (ind + 1) <> "case " <> quoteString a.tag <> ": {" ] <> decls <> inner <> [ indent (ind + 1) <> "}" ]
    in
      if A.null arms then scrut.pre <> [ indent ind <> "return $nomatch(" <> scrut.js <> ");" ]
      else
        scrut.pre
          <> [ indent ind <> "switch (" <> scrut.js <> ".$) {" ]
          <> A.concatMap arm arms
          <> [ indent ind <> "}", indent ind <> "return $nomatch(" <> scrut.js <> ");" ]
  e -> last env ind e

-- | what is last in a function: returned, or, in a loop, the next round
last :: Env -> Int -> Expr -> Array String
last env ind e = case env.self, e of
  Just s, Call k as | k == s.key && A.length as == A.length s.params -> again s as
  -- `a ++ ... ++ f xs`: the left is gathered into `$s`, and the call goes
  -- round; what returns gives `$s` its prefix back
  Just s, PrimCall "strAppend" [ a, r ] | s.sapp && appendEnd s.key (A.length s.params) r ->
    [ indent ind <> "$s += " <> (expr env ind a).code <> ";" ] <> last env ind r
  Just s, Tag t as
    | s.trmc
    , Just { init, last: Call k bs } <- A.unsnoc as
    , k == s.key && A.length bs == A.length s.params ->
        let
          r = bindName env "cell"
          args = map (\a -> (expr env ind a).code) init <> [ "undefined" ]
        in
          [ indent ind <> "const " <> r.js <> " = { $: " <> quoteString t <> ", a: [" <> S.joinWith ", " args <> "] };"
          , indent ind <> "if ($hole) $hole.a[$at] = " <> r.js <> "; else $root = " <> r.js <> ";"
          , indent ind <> "$hole = " <> r.js <> "; $at = " <> show (A.length init) <> ";"
          ]
            <> again s bs
  Just s, _ | s.trmc ->
    let r = bindName env "r"
    in
      [ indent ind <> "const " <> r.js <> " = " <> (expr env ind e).code <> ";"
      , indent ind <> "if ($hole) { $hole.a[$at] = " <> r.js <> "; return $root; }"
      , indent ind <> "return " <> r.js <> ";"
      ]
  Just s, _ | s.sapp -> [ indent ind <> "return $s + " <> atLeast 2 (expr env ind e) <> ";" ]
  _, _ -> [ indent ind <> "return " <> (expr env ind e).code <> ";" ]
  where
  -- (the arguments are of this round's `const`s: assigned one by one)
  again s as =
    [ indent ind <> S.joinWith " " (A.zipWith (\p a -> p <> " = " <> (expr env ind a).code <> ";") s.params as) <> " continue;" ]

expr :: Env -> Int -> Expr -> Code
expr env0 ind = case _ of
  Var x -> p3 (fromMaybe (safe x) (Map.lookup x env.names))
  Global g -> p3 (ref env.gs g)
  Lam xs b ->
    let r = bindAll env xs
    in { code: S.joinWith "" (map (\p -> "(" <> p <> ") => ") r.js) <> body r.env ind b, prec: 0 }
  -- (a thunk forced: its value kept on it, so that forcing it again
  -- does not compute it again -- the language is pure, `f MkUnit` is the
  -- same value every time, and what abandons is not kept)
  App f [ Tag "MkUnit" [] ] -> p3 ("$force(" <> (expr env ind f).code <> ")")
  App f as -> p3 (atLeast 3 (expr env ind f) <> S.joinWith "" (map (\a -> "(" <> (expr env ind a).code <> ")") as))
  Call g [ Tag "MkUnit" [] ] -> p3 ("$force(" <> maybe'' g (Map.lookup g env.gs) <> ")")
  Call g as -> p3 (maybe'' g (Map.lookup g env.gs) <> "(" <> S.joinWith ", " (map (\a -> (expr env ind a).code) as) <> ")")
  Proj e l -> p3 (atLeast 3 (expr env ind e) <> (if isIdent l then "." <> l else "[" <> quoteString l <> "]"))
  Record fs -> p3 ("{ " <> S.joinWith ", " (map (\(Tuple l e) -> propName l <> ": " <> (expr env ind e).code) fs) <> " }")
  Tag t as -> p3 ("{ $: " <> quoteString t <> ", a: [" <> S.joinWith ", " (map (\a -> (expr env ind a).code) as) <> "] }")
  Int i ->
    let s = BI.toString i
    in if SCU.take 1 s == "-" then { code: s <> "n", prec: 2 } else p3 (s <> "n")
  -- (`2.0` is `2`, a number, not a BigInt)
  Float x ->
    let s = show x
    in if SCU.take 1 s == "-" then { code: s, prec: 2 } else p3 s
  Str s -> p3 (quoteString s)
  Abandon -> p3 "$abandon()"
  Prim p -> p3 ("$" <> p <> "$c")
  PrimCall p as -> primCall p (map (expr env ind) as)
  e@(Let _ _ _) -> p3 ("(() => " <> block env ind e <> ")()")
  e@(Match _ _) -> p3 ("(() => " <> block env ind e <> ")()")
  where
  -- (what is inside an expression is not last in the function)
  env = env0 { self = Nothing }
  p3 code = { code, prec: 3 }
  maybe'' g = case _ of
    Just i -> i.js
    Nothing -> safe g

-- | a primitive given all its arguments: an operator where JavaScript has
-- | one, else its function
primCall :: String -> Array Code -> Code
primCall p as = case p, as of
  "intAdd", [ a, b ] -> bin "+" a b
  "intSub", [ a, b ] -> bin "-" a b
  "intMul", [ a, b ] -> bin "*" a b
  "strAppend", [ a, b ] -> bin "+" a b
  "intEq", [ a, b ] -> test "===" a b
  "strEq", [ a, b ] -> test "===" a b
  "intLt", [ a, b ] -> test "<" a b
  "strLt", [ a, b ] -> test "<" a b
  "intLe", [ a, b ] -> test "<=" a b
  "floatAdd", [ a, b ] -> bin "+" a b
  "floatSub", [ a, b ] -> bin "-" a b
  "floatMul", [ a, b ] -> bin "*" a b
  "floatDiv", [ a, b ] -> bin "/" a b
  "floatEq", [ a, b ] -> test "===" a b
  "floatLt", [ a, b ] -> test "<" a b
  "floatLe", [ a, b ] -> test "<=" a b
  "intShow", [ a ] -> { code: atLeast 3 a <> ".toString()", prec: 3 }
  "strLength", [ a ] -> { code: "BigInt(" <> atLeast 3 a <> ".length)", prec: 3 }
  _, _ -> { code: "$" <> p <> "(" <> S.joinWith ", " (map _.code as) <> ")", prec: 3 }
  where
  bin op a b = { code: atLeast 1 a <> " " <> op <> " " <> atLeast 2 b, prec: 1 }
  test op a b = { code: atLeast 2 a <> " " <> op <> " " <> atLeast 2 b <> " ? $T : $F", prec: 0 }

-- Runtime
--------------------------------------------------------------------------------

-- | what the code uses of the runtime
runtime :: String -> Array String
runtime code = A.catMaybes
  [ when' "$T" "const $T = { $: \"True\", a: [] }, $F = { $: \"False\", a: [] };"
  , when' "$abandon" "const $abandon = () => { throw new Error(\"heff:abandon\"); };"
  , when' "$nomatch" "const $nomatch = (s) => { throw new Error(\"heff: no case for '\" + (s && s.$) + \"'\"); };"
  , when' "$force" "const $force = (f) => { if (f.$m !== undefined) return f.$m; const v = f({ $: \"MkUnit\", a: [] }); f.$m = v; return v; };"
  , when' "$lazy" "const $lazy = (f) => { let state = 0, v; return () => { if (state === 2) return v; if (state === 1) throw new Error(\"heff: defined by itself\"); state = 1; try { v = f(); state = 2; return v; } finally { if (state === 1) state = 0; } }; };"
  ]
  where
  when' k line = if S.contains (S.Pattern k) code then Just line else Nothing

-- | the primitives the code uses: `$p(a, b)`, and `$p$c` (curried)
primHelpers :: String -> Array String
primHelpers code = A.concatMap one prims
  where
  one (Tuple p i) =
    let
      u = uncurryJs i.js
      direct = if S.contains (S.Pattern ("$" <> p <> "(")) code || S.contains (S.Pattern ("$" <> p <> "$c")) code
        then [ "const $" <> p <> " = (" <> S.joinWith ", " u.params <> ") => " <> u.body <> ";" ] else []
      curried = if S.contains (S.Pattern ("$" <> p <> "$c")) code
        then [ "const $" <> p <> "$c = " <> S.joinWith "" (map (\x -> "(" <> x <> ") => ") u.params) <> "$" <> p <> "(" <> S.joinWith ", " u.params <> ");" ] else []
    in
      direct <> curried

-- | `(a) => (b) => body` as its parameters and body
uncurryJs :: String -> { params :: Array String, body :: String }
uncurryJs = go []
  where
  go ps s = case S.stripPrefix (S.Pattern "(") s of
    Just rest -> case S.indexOf (S.Pattern ") => ") rest of
      Just i | isIdent (S.take i rest) -> go (A.snoc ps (S.take i rest)) (S.drop (i + 5) rest)
      _ -> { params: ps, body: s }
    Nothing -> { params: ps, body: s }

-- | The runtime: a computation run by a handler whose (raw) monad is the
-- | continuation monad, `(a -> ()) -> ()`.  An operation may finish later
-- | (`Sleep`, `Fetch`, reading a file), and the rest of the computation --
-- | its continuation -- runs then: JavaScript's asynchrony, as a handler,
-- | and the computation is the same whatever handles it.  There is one
-- | monad for the whole program; each effect contributes an algebra.
-- | `perform` does an operation of the computation's effect: `$sum` of two
-- | algebras gives each operation (`InL`, `InR`) to its side's, and each
-- | world's algebra -- a provider's text, bundled whole, its factories
-- | gathered (`$p0`, `$p1`, ...) -- knows its own operations.
-- | (An operation done at once goes on at once, the stack deeper each time:
-- | every so often it goes on after the stack is let go of, lest a long
-- | loop of them overflow it.)
runtimeFor :: Array Provider -> Effect -> String
runtimeFor provs eff = S.joinWith "\n" ([ run ] <> (if hasSum eff then [ sum ] else []) <> A.mapWithIndex bundle provs)
  where
  hasSum = case _ of
    Plus _ _ -> true
    Leaf _ -> false
  -- the provider's own module, in a scope of its own: its import lines
  -- are hoisted to the file's top, its exported factories handed out
  bundle i p = S.joinWith "\n"
    ( [ "// " <> p.module <> ".provider.js"
      , "const $p" <> show i <> " = (() => {"
      ]
        <> map unexport (A.filter (not <<< isImport) (S.split (S.Pattern "\n") p.text))
        <> [ "return { " <> S.joinWith ", " (exportedNames p.text) <> " };"
          , "})();"
          ]
    )
  unexport l = fromMaybe l (S.stripPrefix (S.Pattern "export const ") l <#> ("const " <> _))
  run = S.joinWith "\n"
    [ "const $run = (main, perform) => {"
    , "  const done = () => {};"
    , "  const later = typeof setImmediate === \"function\" ? setImmediate : (f) => setTimeout(f, 0);"
    , "  let depth = 0;"
    , "  const now = (k) => (x) => { if (depth > 1000) { depth = 0; later(() => k(x)); return; } depth++; k(x); };"
    , "  // the handler: its monad's return and bind, and its algebra"
    , "  const hd = {"
    , "    ret: (x) => (k) => k(x),"
    , "    bind: (m) => (f) => (k) => m((x) => f(x)(k)),"
    , "    malg: (p) => (k) => perform(p, now(k)),"
    , "  };"
    , "  return main(hd)(hd.ret)(done);"
    , "};"
    ]
  sum = "const $sum = (l, r) => (p, k) => p.$ === \"InL\" ? l(p.a[0], k) : r(p.a[0], k);"
