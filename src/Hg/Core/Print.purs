-- | Core terms in messages: read back (from values, in a context), renamed
-- | for display, and printed in heff's syntax (`Hg.Print.Surface`), their
-- | implicit arguments left out.
module Hg.Core.Print
  ( prettyEl
  , prettyTy
  , prtElR
  , prtTyR
  , prtBareR
  ) where

import Prelude

import Data.List (List)
import Hg.Core.Params (CtxShape)
import Hg.Core.Readback (readbBare, readbEl, readbTy)
import Hg.Core.Rename (renameEl, renameTy)
import Hg.Core.Syntax as S
import Hg.Core.Value as V
import Hg.Print.Surface (Implicits, printEl, printTy)
import Hg.Support.Pretty (Doc)
import Hg.Support.Pretty as P

hidden :: Implicits
hidden = { names: \_ _ -> [], hide: true }

-- | a term with the names of its free variables (innermost first)
prettyEl :: List String -> S.El -> Doc
prettyEl ns e = P.text (printEl hidden 0 ns e)

prettyTy :: List String -> S.Ty -> Doc
prettyTy ns t = P.text (printTy hidden 0 ns t)

prtElR :: (String -> String) -> CtxShape -> V.El -> Doc
prtElR r cs v = prettyEl cs.names (renameEl r (readbEl cs.len v))

prtTyR :: (String -> String) -> CtxShape -> V.Ty -> Doc
prtTyR r cs v = prettyTy cs.names (renameTy r (readbTy cs.len v))

prtBareR :: (String -> String) -> CtxShape -> V.BareNeutral -> Doc
prtBareR r cs v = prettyEl cs.names (renameEl r (readbBare cs.len v))
