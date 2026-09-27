-- | The part of Haskell's `prettyprinter` that hippogriff and fnotation use,
-- | with the same layout algorithm (`layoutPretty`, width 80, ribbon 1.0), so
-- | that output matches the reference implementation's golden files.
module Hg.Support.Pretty
  ( Doc
  , text
  , pretty
  , emptyDoc
  , line
  , line'
  , softline
  , hardline
  , nest
  , group
  , flatAlt
  , align
  , hang
  , indent
  , vsep
  , hsep
  , vcat
  , cat
  , sep
  , concatWith
  , encloseSep
  , list
  , enclose
  , spaces
  , appendSpace
  , (<+>)
  , render
  , renderWidth
  , isEmpty
  ) where

import Prelude

import Data.Array as A
import Data.Foldable (foldr)
import Data.Lazy (Lazy, defer, force)
import Data.List (List(..), (:))
import Data.Maybe (Maybe(..))
import Data.String as S
import Data.String.CodeUnits as SCU

data Doc
  = Fail
  | Empty
  | Text String
  | Line
  | FlatAlt Doc Doc
  | Cat Doc Doc
  | Nest Int Doc
  | Union Doc Doc
  | Column (Int -> Doc)
  | Nesting (Int -> Doc)

instance semigroupDoc :: Semigroup Doc where
  append Empty d = d
  append d Empty = d
  append a b = Cat a b

instance monoidDoc :: Monoid Doc where
  mempty = Empty

isEmpty :: Doc -> Boolean
isEmpty = case _ of
  Empty -> true
  _ -> false

emptyDoc :: Doc
emptyDoc = Empty

-- | text without newlines
text :: String -> Doc
text "" = Empty
text s = Text s

-- | like `pretty` on `Text`: newlines become hard line breaks
pretty :: String -> Doc
pretty s = concatWith (\x y -> x <> hardline <> y) (map text (S.split (S.Pattern "\n") s))

hardline :: Doc
hardline = Line

line :: Doc
line = FlatAlt Line (Text " ")

line' :: Doc
line' = FlatAlt Line Empty

softline :: Doc
softline = Union (Text " ") Line

nest :: Int -> Doc -> Doc
nest 0 d = d
nest i d = Nest i d

flatAlt :: Doc -> Doc -> Doc
flatAlt = FlatAlt

group :: Doc -> Doc
group d = case d of
  Union _ _ -> d
  FlatAlt a b -> case changes b of
    Flattened b' -> Union b' a
    AlreadyFlat -> Union b a
    NeverFlat -> a
  _ -> case changes d of
    Flattened d' -> Union d' d
    AlreadyFlat -> d
    NeverFlat -> d

data Flat = Flattened Doc | AlreadyFlat | NeverFlat

-- | `changesUponFlattening` from prettyprinter
changes :: Doc -> Flat
changes = case _ of
  FlatAlt _ y -> Flattened (flatten y)
  Line -> NeverFlat
  Union x _ -> Flattened x
  Nest i x -> mapF (Nest i) (changes x)
  Cat x y -> case changes x, changes y of
    NeverFlat, _ -> NeverFlat
    _, NeverFlat -> NeverFlat
    Flattened x', Flattened y' -> Flattened (Cat x' y')
    Flattened x', AlreadyFlat -> Flattened (Cat x' y)
    AlreadyFlat, Flattened y' -> Flattened (Cat x y')
    AlreadyFlat, AlreadyFlat -> AlreadyFlat
  Column f -> Flattened (Column (flatten <<< f))
  Nesting f -> Flattened (Nesting (flatten <<< f))
  Fail -> NeverFlat
  _ -> AlreadyFlat
  where
  mapF f = case _ of
    Flattened x -> Flattened (f x)
    other -> other

flatten :: Doc -> Doc
flatten = case _ of
  FlatAlt _ y -> flatten y
  Cat x y -> Cat (flatten x) (flatten y)
  Nest i x -> Nest i (flatten x)
  Line -> Fail
  Union x _ -> flatten x
  Column f -> Column (flatten <<< f)
  Nesting f -> Nesting (flatten <<< f)
  other -> other

align :: Doc -> Doc
align d = Column \k -> Nesting \i -> nest (k - i) d

hang :: Int -> Doc -> Doc
hang i d = align (nest i d)

indent :: Int -> Doc -> Doc
indent i d = hang i (spaces i <> d)

spaces :: Int -> Doc
spaces n = if n <= 0 then Empty else Text (SCU.fromCharArray (A.replicate n ' '))

appendSpace :: Doc -> Doc -> Doc
appendSpace a b = a <> Text " " <> b

infixr 6 appendSpace as <+>

concatWith :: (Doc -> Doc -> Doc) -> Array Doc -> Doc
concatWith f ds = case A.unsnoc ds of
  Nothing -> Empty
  Just { init, last } -> foldr f last init

vsep :: Array Doc -> Doc
vsep = concatWith (\x y -> x <> line <> y)

hsep :: Array Doc -> Doc
hsep = concatWith (<+>)

vcat :: Array Doc -> Doc
vcat = concatWith (\x y -> x <> line' <> y)

cat :: Array Doc -> Doc
cat = group <<< vcat

sep :: Array Doc -> Doc
sep = group <<< vsep

encloseSep :: Doc -> Doc -> Doc -> Array Doc -> Doc
encloseSep l r s ds = case ds of
  [] -> l <> r
  [ d ] -> l <> d <> r
  _ -> cat (A.zipWith (<>) (A.cons l (A.replicate (A.length ds - 1) s)) ds) <> r

list :: Array Doc -> Doc
list = group <<< encloseSep (flatAlt (text "[ ") (text "[")) (flatAlt (text " ]") (text "]")) (text ", ")

enclose :: Doc -> Doc -> Doc -> Doc
enclose l r d = l <> d <> r

-- ===========================================================================
-- layout
-- ===========================================================================

-- | (`SFail`: a flattened hard line break, which never fits)
data SDoc = SEmpty | SFail | SText String (Lazy SDoc) | SLine Int (Lazy SDoc)

render :: Doc -> String
render = renderWidth 80

renderWidth :: Int -> Doc -> String
renderWidth w d = display (best w 0 0 ((Tuple3 0 d) : Nil))

data Tuple3 = Tuple3 Int Doc

-- `nl` is the nesting of the line being laid out, `k` the current column
best :: Int -> Int -> Int -> List Tuple3 -> SDoc
best w nl k = case _ of
  Nil -> SEmpty
  Tuple3 i d : ds -> case d of
    Fail -> SFail
    Empty -> best w nl k ds
    Text s -> SText s (defer \_ -> best w nl (k + S.length s) ds)
    Line -> SLine i (defer \_ -> best w i i ds)
    FlatAlt x _ -> best w nl k (Tuple3 i x : ds)
    Cat x y -> best w nl k (Tuple3 i x : Tuple3 i y : ds)
    Nest j x -> best w nl k (Tuple3 (i + j) x : ds)
    Union x y ->
      let
        l = best w nl k (Tuple3 i x : ds)
      in
        if fits w nl k l then l else best w nl k (Tuple3 i y : ds)
    Column f -> best w nl k (Tuple3 i (f k) : ds)
    Nesting f -> best w nl k (Tuple3 i (f i) : ds)

-- only the first line of the rest is inspected, as in `layoutPretty`
fits :: Int -> Int -> Int -> SDoc -> Boolean
fits w nl k s0 = go (min (w - k) (w - (k - nl))) s0
  where
  go rem s
    | rem < 0 = false
    | otherwise = case s of
        SEmpty -> true
        SFail -> false
        SLine _ _ -> true
        SText t rest -> go (rem - S.length t) (force rest)

display :: SDoc -> String
display s0 = go s0 ""
  where
  go s acc = case s of
    SEmpty -> acc
    SFail -> acc
    SText t rest -> go (force rest) (acc <> t)
    SLine i rest -> go (force rest) (acc <> "\n" <> SCU.fromCharArray (A.replicate i ' '))
