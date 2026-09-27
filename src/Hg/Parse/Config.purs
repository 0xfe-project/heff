-- | A port of `Hippogriff.Frontend.Notation`: the fnotation configuration.
module Hg.Parse.Config where


import Data.Tuple (Tuple(..))
import Hg.Parse.FNotation (Assoc(..), ConfTable, Kind(..), Ntn, Prec, confTable, prettyNtn)
import Hg.Support.Pretty (Doc)

lexConfig :: ConfTable Kind
lexConfig =
  confTable
    [ Tuple "sig" Block
    , Tuple "struct" Block
    , Tuple "sum" Block
    , Tuple "match" Block
    , Tuple "where" Block
    , Tuple "do" Block
    , Tuple "theory" Decl
    , Tuple "def" Decl
    , Tuple "primitive" Decl
    , Tuple ">" Decl
    , Tuple "test" Decl
    , Tuple "type" Decl
    , Tuple "let" Decl
    , Tuple "open" Decl
    , Tuple "import" Decl
    , Tuple "end" End
    , Tuple "Type" AKeyword
    , Tuple "Int" AKeyword
    , Tuple "String" AKeyword
    , Tuple "rec" AKeyword
    , Tuple "abandon" AKeyword
    , Tuple ":=" SKeyword
    , Tuple ":" SKeyword
    , Tuple "->" SKeyword
    , Tuple "=>" SKeyword
    , Tuple "+" SKeyword
    , Tuple "~" SKeyword
    , Tuple "<-" SKeyword
    ]

readConfig :: ConfTable Prec
readConfig =
  confTable
    [ Tuple "<-" { binding: 5, assoc: AssocNon }
    , Tuple ":=" { binding: 10, assoc: AssocNon }
    , Tuple ":" { binding: 20, assoc: AssocNon }
    , Tuple "->" { binding: 30, assoc: AssocR }
    , Tuple "=>" { binding: 30, assoc: AssocR }
    , Tuple "~" { binding: 40, assoc: AssocR }
    , Tuple "+" { binding: 50, assoc: AssocL }
    , Tuple "-" { binding: 50, assoc: AssocL }
    , Tuple "*" { binding: 60, assoc: AssocL }
    , Tuple "/" { binding: 60, assoc: AssocL }
    ]

prtNtn :: Ntn -> Doc
prtNtn = prettyNtn readConfig lexConfig
