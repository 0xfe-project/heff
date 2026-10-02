-- | The locals of a file, as written: each binder, and the names that refer
-- | to it.  Read from the notation -- before the equations are compiled to
-- | matches, which merges the variables of different equations into one --
-- | by the scoping the elaborator follows: a name refers to the nearest
-- | binder of it around it, else to a global.
-- |
-- | What binds: a lambda's parameters and a `case` alternative's (`x => e`,
-- | `Cons y ys => e`, `{x} => e`, `\(x : T) -> e`), an equation's patterns, `x <- e` in a
-- | `do` block (for the statements after it), a named argument of a
-- | function type (`(x :: A) -> B`, `{a :: Type} -> B`, for `B`), a
-- | definition's or a theory's parameters (`data Pair a b`, `theory SET a`,
-- | for what follows them); and a signature's implicit arguments, which its
-- | equations are under.  A
-- | record type's field is in scope for the fields after it: a use of one
-- | is the field's (`fields`), not a local's.
module Heff.Locals
  ( Local
  , FieldUse
  , locals
  ) where

import Prelude

import Data.Array as A
import Data.Array.NonEmpty as NEA
import Data.List (List(..), (:))
import Data.List as L
import Data.Maybe (Maybe(..), maybe)
import Heff.Parse.Notation (Ntn(..), Span, getGroup)

-- | a local: its name, where its binder is, where it is used
type Local = { name :: String, binder :: Span, uses :: Array Span }

-- | a use of a record type's field among the fields after it (`t` of
-- | `m :: t -> t`), and where the field is declared
type FieldUse = { name :: String, field :: Span, use :: Span }

type Binder = { name :: String, span :: Span, field :: Boolean }

type Found = { binders :: Array Binder, uses :: Array { binder :: Binder, use :: Span } }

locals :: Array Ntn -> { locals :: Array Local, fields :: Array FieldUse }
locals ntns =
  let
    found = A.foldl (\acc n -> acc <> declaration n) none ntns
    own = A.filter (not <<< _.field) found.binders
  in
    { locals: map (\b -> { name: b.name, binder: b.span, uses: map _.use (A.filter (\u -> u.binder.span == b.span) found.uses) }) own
    , fields: map (\u -> { name: u.binder.name, field: u.binder.span, use: u.use }) (A.filter (_.binder.field) found.uses)
    }

none :: Found
none = { binders: [], uses: [] }

-- | a declaration: its signature, then its equations under the signature's
-- | implicit arguments
declaration :: Ntn -> Found
declaration = case _ of
  MDecl _ "def" (Infix (Ident _ _) (Keyword ":" _) (Juxt ty (NBlock "where" Nothing eqs _))) _ ->
    -- (the implicit arguments are bound in the signature, which walks them)
    walk Nil ty <> A.foldl (\acc eq -> acc <> walk (scope (implicits ty) Nil) eq) none eqs
  -- a definition with named arguments (`data Pair a b = ...`, `type T a =
  -- ...`): each in scope for those after it, the type, and the body
  MDecl _ "def" (Infix (Infix h (Keyword ":" _) ty) (Keyword ":=" _) body) _ | Ident _ _ <- NEA.head (getGroup h) ->
    arguments Nil (NEA.tail (getGroup h)) [ ty, body ]
  -- a theory's parameters (`theory Handler (h :: HFunctor)`, `theory SET a`)
  MDecl _ "theory" (Infix h (Keyword ":=" _) body) _ | Ident _ _ <- NEA.head (getGroup h) ->
    arguments Nil (NEA.tail (getGroup h)) [ body ]
  MDecl _ _ n _ -> walk Nil n
  n -> walk Nil n
  where
  arguments env args rest = case A.uncons args of
    Just { head: a, tail } -> case named a of
      -- (an argument's type is in the scope of those before it)
      Just arg -> maybe none (walk env) arg.ty <> { binders: [ arg.binder ], uses: [] } <> arguments (arg.binder : env) tail rest
      Nothing -> walk env a <> arguments env tail rest
    Nothing -> A.foldl (\acc x -> acc <> walk env x) none rest
  named = case _ of
    Ident x sp -> Just { binder: binder x sp, ty: Nothing }
    Infix (Ident x sp) (Keyword ":" _) a -> Just { binder: binder x sp, ty: Just a }
    NBrace (Infix (Ident x sp) (Keyword ":" _) a) _ -> Just { binder: binder x sp, ty: Just a }
    _ -> Nothing
  implicits = case _ of
    Infix (NBrace (Infix (Ident x sp) (Keyword ":" _) _) _) (Keyword "->" _) r -> A.cons (binder x sp) (implicits r)
    _ -> []

binder :: String -> Span -> Binder
binder name span = { name, span, field: false }

-- | binders in scope, the innermost first
scope :: Array Binder -> List Binder -> List Binder
scope bs env = A.foldl (flip (:)) env bs

walk :: List Binder -> Ntn -> Found
walk env n = case n of
  Ident x sp -> case L.find (\b -> b.name == x) env of
    Just b -> { binders: [], uses: [ { binder: b, use: sp } ] }
    Nothing -> none
  -- a lambda with its binder annotated, `\(x : T) -> e`: `T` is outside
  Infix (Infix (Ident x sp) (Keyword ":" _) a) (Keyword "=>" _) r -> walk env a <> under [ binder x sp ] r
  -- a lambda, a `case` alternative
  Infix l (Keyword "=>" _) r -> under (patternBinders l) r
  -- an equation: its head is the definition's name, its arguments patterns
  Infix l (Keyword ":=" _) r | Ident _ _ <- NEA.head (getGroup l) ->
    under (A.concatMap patternBinders (NEA.tail (getGroup l))) r
  -- a named argument of a function type
  Infix (Infix (Ident x sp) (Keyword ":" _) a) (Keyword "->" _) r -> walk env a <> under [ binder x sp ] r
  Infix (NBrace (Infix (Ident x sp) (Keyword ":" _) a) _) (Keyword "->" _) r -> walk env a <> under [ binder x sp ] r
  NBlock "do" h stmts _ -> maybe' h <> doBlock env stmts
  -- (a field's name, set or declared, is no use of anything)
  NBlock "struct" h fs _ -> maybe' h <> A.foldl (\acc f -> acc <> fieldSetting f) none fs
  NBlock "sig" h fs _ -> maybe' h <> sigBlock env fs
  Infix l op r -> walk env l <> walk env op <> walk env r
  Juxt a b -> walk env a <> walk env b
  NBlock _ h ns _ -> maybe' h <> A.foldl (\acc x -> acc <> walk env x) none ns
  MDecl _ _ x _ -> walk env x
  NTuple ns _ -> A.foldl (\acc x -> acc <> walk env x) none ns
  NBrace x _ -> walk env x
  _ -> none
  where
  under bs r = { binders: bs, uses: [] } <> walk (scope bs env) r
  maybe' = case _ of
    Just h -> walk env h
    Nothing -> none
  fieldSetting = case _ of
    Infix (Ident _ _) (Keyword ":=" _) e -> walk env e
    f -> walk env f

-- | `x <- e`: `x` is bound for the statements after it
doBlock :: List Binder -> Array Ntn -> Found
doBlock env stmts = case A.uncons stmts of
  Nothing -> none
  Just { head: Infix (Ident x sp) (Keyword "<-" _) e, tail } ->
    walk env e <> { binders: [ binder x sp ], uses: [] } <> doBlock (binder x sp : env) tail
  Just { head, tail } -> walk env head <> doBlock env tail

-- | a record type's fields: each in scope for those after it
sigBlock :: List Binder -> Array Ntn -> Found
sigBlock env fs = case A.uncons fs of
  Nothing -> none
  Just { head: Infix (Ident x sp) (Keyword ":" _) a, tail } ->
    let b = { name: x, span: sp, field: true }
    in walk env a <> { binders: [ b ], uses: [] } <> sigBlock (b : env) tail
  Just { head, tail } -> walk env head <> sigBlock env tail

-- | the variables a pattern binds (`_` binds nothing; a tag is no variable)
patternBinders :: Ntn -> Array Binder
patternBinders = case _ of
  Ident "_" _ -> []
  Ident x sp -> [ binder x sp ]
  NBrace p _ -> patternBinders p
  NTuple ps _ -> A.concatMap patternBinders ps
  Juxt a b -> patternBinders a <> patternBinders b
  _ -> []
