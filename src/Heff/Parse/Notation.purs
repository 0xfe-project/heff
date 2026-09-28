-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at https://mozilla.org/MPL/2.0/.

-- From the `fnotation` Haskell library (MPL-2.0).
-- | Notation: what the parser (`Heff.Parse.Surface`) reads text into and the
-- | elaborator takes, a tree of juxtapositions, infixes and blocks; and the
-- | diagnostics, with their spans.
module Heff.Parse.Notation
  ( Ntn(..)
  , Span
  , Diag
  , Severity(..)
  , ntnSpan
  , ntnHead
  , group
  , getGroup
  ) where

import Prelude

import Data.Array as A
import Data.Array.NonEmpty (NonEmptyArray)
import Data.Array.NonEmpty as NEA
import Data.Foldable (foldl)
import Data.Maybe (Maybe)
import Heff.Support.BigInt (BigInt)
import Heff.Support.Pretty (Doc, (<+>))
import Heff.Support.Pretty as P

type Span = { start :: Int, end :: Int }

data Severity = SError | SWarning | SDebug

derive instance eqSeverity :: Eq Severity

-- | a diagnostic: `code` is already offset into its family (lexer 0, reader
-- | 100, parser 200, elaborator 300)
type Diag = { code :: Int, severity :: Severity, msg :: Doc, span :: Span, note :: Maybe Doc }

data Ntn
  = Juxt Ntn Ntn
  | Infix Ntn Ntn Ntn
  | NBlock String (Maybe Ntn) (Array Ntn) Span
  | MDecl (Array String) String Ntn Span
  | NTuple (Array Ntn) Span
  | NBrace Ntn Span -- ^ extension: `{ e }`, for implicit arguments
  | Ident String Span
  | Keyword String Span
  | NField String Span
  | NTag String Span
  | NInt BigInt Span
  | NFloat Number Span
  | NString String Span
  | NError Span

group :: NonEmptyArray Ntn -> Ntn
group xs = foldl Juxt (NEA.head xs) (NEA.tail xs)

-- | the `Group` view: a juxtaposition read as head and arguments
getGroup :: Ntn -> NonEmptyArray Ntn
getGroup = go []
  where
  go acc (Juxt f x) = go (A.cons x acc) f
  go acc f = NEA.cons' f acc

ntnSpan :: Ntn -> Span
ntnSpan n = { start: startPos n, end: endPos n }
  where
  startPos = case _ of
    Juxt f _ -> startPos f
    Infix x _ _ -> startPos x
    NBlock _ _ _ s -> s.start
    MDecl _ _ _ s -> s.start
    NTuple _ s -> s.start
    NBrace _ s -> s.start
    Ident _ s -> s.start
    Keyword _ s -> s.start
    NField _ s -> s.start
    NTag _ s -> s.start
    NInt _ s -> s.start
    NFloat _ s -> s.start
    NString _ s -> s.start
    NError s -> s.start
  endPos = case _ of
    Juxt _ x -> endPos x
    Infix _ _ y -> endPos y
    NBlock _ _ _ s -> s.end
    MDecl _ _ _ s -> s.end
    NTuple _ s -> s.end
    NBrace _ s -> s.end
    Ident _ s -> s.end
    Keyword _ s -> s.end
    NField _ s -> s.end
    NTag _ s -> s.end
    NInt _ s -> s.end
    NFloat _ s -> s.end
    NString _ s -> s.end
    NError s -> s.end

ntnHead :: Ntn -> Doc
ntnHead = case _ of
  Juxt _ _ -> P.text "Juxt"
  Infix _ _ _ -> P.text "Infix"
  NBlock x _ _ _ -> P.text "Block" <+> P.text x
  MDecl mods x _ _ -> P.text "MDecl" <+> P.hsep (map P.text (A.snoc mods x))
  Ident x _ -> P.text "Ident" <+> P.text x
  Keyword x _ -> P.text "Keyword" <+> P.text x
  NField x _ -> P.text "Field" <+> P.text x
  NTag x _ -> P.text "Tag" <+> P.text x
  NInt i _ -> P.text "Int" <+> P.text (show i)
  NFloat x _ -> P.text "Float" <+> P.text (show x)
  NString i _ -> P.text "String" <+> P.text i
  NTuple _ _ -> P.text "Tuple"
  NBrace _ _ -> P.text "Brace"
  NError _ -> P.text "Error"
